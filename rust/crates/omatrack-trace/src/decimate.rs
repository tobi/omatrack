//! Viewport-bounded telemetry paths: a 1:1 port of `src/app/TraceDecimator.cpp`.
//!
//! A NaN point is a pen-up. Samples are selected in time order, never sorted
//! by value. The forward alignment map (`source_fraction`) is evaluated
//! locally in each device column: a nonlinear reference must not use an
//! endpoint-only inverse. At most two extrema per device column plus clipped
//! run endpoints are emitted; a gap costs at most one column. A swinging
//! slope corridor then removes only subpixel detail (at most `2 * 0.05 / dpr`
//! logical pixels of vertical error, i.e. 0.1 physical pixel).
//!
//! No allocation happens after warm-up: the caller owns the output buffer
//! and its capacity is reused.

/// A decimated path vertex in logical pixels. A non-finite `x` is a pen-up.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct PathPoint {
    pub x: f64,
    pub y: f64,
}

impl PathPoint {
    pub const PEN_UP: PathPoint = PathPoint {
        x: f64::NAN,
        y: f64::NAN,
    };

    pub const fn new(x: f64, y: f64) -> Self {
        Self { x, y }
    }

    /// Whether this point is a pen-up marker.
    pub fn is_pen_up(&self) -> bool {
        !self.x.is_finite()
    }
}

/// An axis-aligned rectangle in logical pixels (the plot area of one lane).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct PlotRect {
    pub left: f64,
    pub top: f64,
    pub width: f64,
    pub height: f64,
}

impl PlotRect {
    pub const fn new(left: f64, top: f64, width: f64, height: f64) -> Self {
        Self {
            left,
            top,
            width,
            height,
        }
    }
    pub fn right(&self) -> f64 {
        self.left + self.width
    }
    pub fn bottom(&self) -> f64 {
        self.top + self.height
    }
}

/// The horizontal and vertical mapping of one decimation call.
///
/// Start from [`DecimateParams::default`] (the whole lap onto an empty
/// rectangle, values `0..1`, DPR 1) and set the fields.
#[derive(Clone, Copy, Debug)]
#[non_exhaustive]
pub struct DecimateParams {
    /// Viewport start, in lap fraction.
    pub x_start: f64,
    /// Viewport span, in lap fraction.
    pub x_span: f64,
    pub rect: PlotRect,
    /// Value at the bottom edge of `rect`.
    pub y_min: f64,
    /// Value range covered by `rect`'s height.
    pub y_span: f64,
    /// Device pixel ratio; one device column is `1 / dpr` logical pixels.
    pub dpr: f64,
    /// Viewport-axis clip range. The lap itself is `0..1`; neighbour laps
    /// use `-1..0` and `1..2`.
    pub clip_low: f64,
    pub clip_high: f64,
}

impl Default for DecimateParams {
    fn default() -> Self {
        Self {
            x_start: 0.0,
            x_span: 1.0,
            rect: PlotRect::default(),
            y_min: 0.0,
            y_span: 1.0,
            dpr: 1.0,
            clip_low: 0.0,
            clip_high: 1.0,
        }
    }
}

/// Qt's `QPointF::operator==`: fuzzy per coordinate, exact-ish near zero.
fn fuzzy_equal(a: PathPoint, b: PathPoint) -> bool {
    fn same(p: f64, q: f64) -> bool {
        if p == 0.0 || q == 0.0 {
            (p - q).abs() <= 1e-12
        } else {
            (p - q).abs() * 1e12 <= p.abs().min(q.abs())
        }
    }
    same(a.x, b.x) && same(a.y, b.y)
}

/// Upper bound of points `decimate` can emit for `rect.width * dpr` columns.
pub fn point_capacity(columns: usize) -> usize {
    columns * 5 + 4
}

/// Number of device columns for a plot width.
pub fn device_columns(width: f64, dpr: f64) -> usize {
    ((width * dpr).ceil() as i64).max(2) as usize
}

/// Decimate `series` into `output` (cleared first). See the module docs.
pub fn decimate(
    series: &[f64],
    source_fraction: &dyn Fn(f64) -> f64,
    params: &DecimateParams,
    output: &mut Vec<PathPoint>,
) {
    decimate_columns(series, source_fraction, params, output);
    simplify(params.dpr, output);
}

/// First pass: per-device-column temporal min/max with pen-ups. Exposed so
/// tests can check the column logic independently of simplification.
pub fn decimate_columns(
    series: &[f64],
    source_fraction: &dyn Fn(f64) -> f64,
    params: &DecimateParams,
    output: &mut Vec<PathPoint>,
) {
    output.clear();
    let DecimateParams {
        x_start,
        x_span,
        rect,
        y_min,
        y_span,
        dpr,
        clip_low,
        clip_high,
    } = *params;
    if series.len() < 2
        || rect.width < 2.0
        || rect.height <= 0.0
        || !x_start.is_finite()
        || !x_span.is_finite()
        || x_span <= 0.0
        || !y_min.is_finite()
        || !y_span.is_finite()
        || y_span <= 0.0
    {
        return;
    }
    let last = series.len() - 1;
    let last_f = last as f64;
    let columns = device_columns(rect.width, dpr);
    // Capacity is reused across frames, so nothing allocates after warm-up.
    output.reserve(point_capacity(columns));
    let points = output;

    let value_at = |index: f64| -> f64 {
        let lo = index.floor() as usize;
        let f = index - lo as f64;
        if f < 1e-10 || lo == last {
            return series[lo];
        }
        series[lo] + (series[lo + 1] - series[lo]) * f
    };
    let bottom = rect.bottom();
    let top = rect.top;
    let append = |points: &mut Vec<PathPoint>, x: f64, value: f64| {
        let y = (bottom - (value - y_min) / y_span * rect.height).clamp(top, bottom);
        let point = PathPoint::new(x, y);
        let used = points.len();
        if used > 0 && fuzzy_equal(point, points[used - 1]) {
            return;
        }
        if used >= 2 {
            let p1 = points[used - 1];
            let p2 = points[used - 2];
            let (ax, ay) = (p1.x - p2.x, p1.y - p2.y);
            let (bx, by) = (point.x - p1.x, point.y - p1.y);
            // Only remove numerically collinear points, never filter peaks.
            if ax.is_finite() && ax * bx + ay * by >= 0.0 && (ax * by - ay * bx).abs() < 1e-9 {
                points[used - 1] = point;
                return;
            }
        }
        points.push(point);
    };
    let pen_up = |points: &mut Vec<PathPoint>| {
        if points.last().is_some_and(|p| p.x.is_finite()) {
            points.push(PathPoint::PEN_UP);
        }
    };

    let mut open = false;
    let mut pending_x = 0.0;
    let mut pending_value = 0.0;
    let columns_f = columns as f64;
    // A column's start is the previous column's end: evaluate the forward
    // map once per boundary (it can be a binary search per call).
    let mut boundary = (f64::NAN, f64::NAN);
    let mut map = |at: f64| -> f64 {
        if at.to_bits() == boundary.0.to_bits() {
            return boundary.1;
        }
        let mapped = source_fraction(at);
        boundary = (at, mapped);
        mapped
    };
    for column in 0..columns {
        let start = clip_low.max(x_start + x_span * column as f64 / columns_f);
        let end = clip_high.min(x_start + x_span * (column + 1) as f64 / columns_f);
        if end <= start {
            if open {
                append(points, pending_x, pending_value);
            }
            open = false;
            pen_up(points);
            continue;
        }
        let from = map(start) * last_f;
        let to = map(end) * last_f;
        if !from.is_finite() || !to.is_finite() || from < 0.0 || to > last_f || to < from {
            if open {
                append(points, pending_x, pending_value);
            }
            open = false;
            pen_up(points);
            continue;
        }
        let first_value = value_at(from);
        let last_value = value_at(to);
        let first = from.ceil() as usize;
        let end_index = (last + 1).min(to.ceil() as usize);
        let mut low = first;
        let mut high = first;
        let mut gap = !first_value.is_finite() || !last_value.is_finite();
        if !gap {
            for i in first..end_index {
                let value = series[i];
                if !value.is_finite() {
                    gap = true;
                    break;
                }
                if value < series[low] {
                    low = i;
                }
                if value > series[high] {
                    high = i;
                }
            }
        }
        if gap {
            if open {
                append(points, pending_x, pending_value);
            }
            open = false;
            pen_up(points);
            continue;
        }
        let x0 = rect.left + (start - x_start) / x_span * rect.width;
        let x1 = rect.left + (end - x_start) / x_span * rect.width;
        if !open {
            append(points, x0, first_value);
        }
        open = true;
        // Min/max in temporal order. Samples keep their own x, never the
        // column centre; sparse columns contribute only the actual sample.
        if first < end_index && to > from {
            let selected = [low.min(high), low.max(high)];
            let mut previous = usize::MAX;
            for index in selected {
                if index == previous {
                    continue;
                }
                previous = index;
                append(
                    points,
                    x0 + (index as f64 - from) / (to - from) * (x1 - x0),
                    series[index],
                );
            }
        }
        pending_x = x1;
        pending_value = last_value;
    }
    if open {
        append(points, pending_x, pending_value);
    }
    if points.last().is_some_and(|p| !p.x.is_finite()) {
        points.pop();
    }
}

/// Second pass: subpixel line simplification with a swinging slope corridor.
///
/// O(points), not recursive Douglas-Peucker. The emitted endpoint can differ
/// from the corridor centre by epsilon as well, so the maximum vertical
/// error is `2 * epsilon` = 0.1 physical pixel. This removes invisible
/// joints, not telemetry noise or real spikes, and is independent of stroke
/// width.
pub fn simplify(dpr: f64, points: &mut Vec<PathPoint>) {
    let epsilon = 0.05 / dpr;
    let used = points.len();
    let mut written = 0usize;
    let mut anchor: Option<usize> = None;
    let mut low_slope = f64::NEG_INFINITY;
    let mut high_slope = f64::INFINITY;
    let mut previous = PathPoint::default();
    for i in 0..used {
        let point = points[i];
        if !point.x.is_finite() {
            if let Some(a) = anchor
                && !fuzzy_equal(previous, points[a])
            {
                points[written] = previous;
                written += 1;
            }
            points[written] = point;
            written += 1;
            anchor = None;
            continue;
        }
        match anchor {
            None => {
                anchor = Some(written);
                points[written] = point;
                written += 1;
                low_slope = f64::NEG_INFINITY;
                high_slope = f64::INFINITY;
            }
            Some(mut a) => {
                let mut dx = point.x - points[a].x;
                let mut dy = point.y - points[a].y;
                let lo = if dx > 0.0 {
                    (dy - epsilon) / dx
                } else {
                    high_slope + 1.0
                };
                let hi = if dx > 0.0 {
                    (dy + epsilon) / dx
                } else {
                    low_slope - 1.0
                };
                if lo > high_slope || hi < low_slope {
                    if !fuzzy_equal(previous, points[a]) {
                        a = written;
                        anchor = Some(a);
                        points[written] = previous;
                        written += 1;
                    }
                    dx = point.x - points[a].x;
                    dy = point.y - points[a].y;
                    low_slope = if dx > 0.0 {
                        (dy - epsilon) / dx
                    } else {
                        f64::NEG_INFINITY
                    };
                    high_slope = if dx > 0.0 {
                        (dy + epsilon) / dx
                    } else {
                        f64::INFINITY
                    };
                } else {
                    low_slope = low_slope.max(lo);
                    high_slope = high_slope.min(hi);
                }
            }
        }
        previous = point;
    }
    if let Some(a) = anchor
        && !fuzzy_equal(previous, points[a])
    {
        points[written] = previous;
        written += 1;
    }
    points.truncate(written);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn params(width: f64, dpr: f64) -> DecimateParams {
        DecimateParams {
            x_start: 0.0,
            x_span: 1.0,
            rect: PlotRect::new(0.0, 0.0, width, 100.0),
            y_min: -1.5,
            y_span: 3.0,
            dpr,
            clip_low: 0.0,
            clip_high: 1.0,
        }
    }

    fn identity(f: f64) -> f64 {
        f.clamp(0.0, 1.0)
    }

    fn noisy(n: usize) -> Vec<f64> {
        (0..n)
            .map(|i| {
                let t = i as f64 * 0.013;
                (t * 3.1).sin() + 0.4 * (t * 17.0).sin() + 0.1 * ((i * 7919) % 13) as f64 / 13.0
            })
            .collect()
    }

    #[test]
    fn nan_is_a_pen_up_even_mid_column() {
        // 1000 samples into 10 columns: the NaN sits in the middle of column 5.
        let mut series: Vec<f64> = (0..1000).map(|i| (i as f64 * 0.05).sin()).collect();
        series[555] = f64::NAN;
        let mut out = Vec::new();
        decimate(&series, &identity, &params(10.0, 1.0), &mut out);
        let pen_ups = out.iter().filter(|p| p.is_pen_up()).count();
        assert_eq!(pen_ups, 1, "one gap, one pen-up: {out:?}");
        let gap = out.iter().position(|p| p.is_pen_up()).unwrap();
        // The gap costs at most one column (column width is 1 px here).
        let before = out[gap - 1].x;
        let after = out[gap + 1].x;
        assert!(after - before <= 1.0 + 1e-9, "gap {before}..{after}");
        assert!(before <= 5.0 + 1e-9 && after >= 6.0 - 1e-9);
    }

    #[test]
    fn extrema_survive_and_stay_in_temporal_order() {
        // A spike up then a spike down inside one column.
        let mut series = vec![0.0; 101];
        series[40] = 1.0;
        series[45] = -1.0;
        let mut out = Vec::new();
        decimate_columns(&series, &identity, &params(2.0, 1.0), &mut out);
        let ys: Vec<f64> = out.iter().map(|p| p.y).collect();
        let top = ys
            .iter()
            .position(|&y| (y - 16.666_666_666_666_664).abs() < 1e-6);
        let bottom = ys
            .iter()
            .position(|&y| (y - 83.333_333_333_333_33).abs() < 1e-6);
        assert!(top.is_some() && bottom.is_some(), "{out:?}");
        assert!(top < bottom, "high (t=40) precedes low (t=45)");
        for pair in out.windows(2) {
            if pair[0].x.is_finite() && pair[1].x.is_finite() {
                assert!(pair[1].x >= pair[0].x, "x is monotonic: {pair:?}");
            }
        }
        // After simplification the extrema are still there.
        simplify(1.0, &mut out);
        assert!(
            out.iter()
                .any(|p| (p.y - 16.666_666_666_666_664).abs() < 1e-6)
        );
        assert!(
            out.iter()
                .any(|p| (p.y - 83.333_333_333_333_33).abs() < 1e-6)
        );
    }

    #[test]
    fn simplification_error_is_bounded_by_two_epsilon() {
        for dpr in [1.0, 1.5, 2.0] {
            let series = noisy(20_000);
            let p = params(700.0, dpr);
            let mut raw = Vec::new();
            decimate_columns(&series, &identity, &p, &mut raw);
            let mut simple = raw.clone();
            simplify(dpr, &mut simple);
            assert!(simple.len() <= raw.len());
            let epsilon = 0.05 / dpr;
            // Every raw vertex lies within 2*eps of the simplified polyline.
            let mut j = 0;
            for point in raw.iter().filter(|p| !p.is_pen_up()) {
                while j + 1 < simple.len() && simple[j + 1].x < point.x {
                    j += 1;
                }
                let a = simple[j];
                let b = simple[(j + 1).min(simple.len() - 1)];
                let y = if b.x > a.x {
                    a.y + (b.y - a.y) * (point.x - a.x) / (b.x - a.x)
                } else {
                    // Vertical run at one x: the point must be inside it.
                    point.y.clamp(a.y.min(b.y), a.y.max(b.y))
                };
                let nearest = if (point.x - a.x).abs() < 1e-12 {
                    // Exactly on a kept vertex x: vertical runs are exact.
                    point.y.clamp(a.y.min(y), a.y.max(y))
                } else {
                    y
                };
                assert!(
                    (nearest - point.y).abs() <= 2.0 * epsilon + 1e-9,
                    "dpr {dpr}: raw {point:?} vs simplified {nearest}"
                );
            }
        }
    }

    #[test]
    fn nonlinear_map_matches_brute_force_per_column() {
        let series = noisy(5_000);
        let map = |f: f64| (f * f).clamp(0.0, 1.0);
        let p = params(300.0, 1.0);
        let mut out = Vec::new();
        decimate_columns(&series, &map, &p, &mut out);
        let last = (series.len() - 1) as f64;
        let columns = device_columns(p.rect.width, p.dpr);
        let to_y =
            |v: f64| (p.rect.bottom() - (v - p.y_min) / p.y_span * p.rect.height).clamp(0.0, 100.0);
        for column in 0..columns {
            let s = column as f64 / columns as f64;
            let e = (column + 1) as f64 / columns as f64;
            // Brute force: the samples this column owns, found through the
            // forward map evaluated for this column only.
            let from = map(s) * last;
            let to = map(e) * last;
            let owned: Vec<usize> =
                (from.ceil() as usize..(to.ceil() as usize).min(series.len())).collect();
            let x0 = s * p.rect.width;
            let x1 = e * p.rect.width;
            let near = |y: f64| {
                out.iter()
                    .any(|q| q.x >= x0 - 1e-9 && q.x <= x1 + 1e-9 && (q.y - y).abs() < 1e-6)
            };
            if !owned.is_empty() && to > from {
                let lo = owned
                    .iter()
                    .map(|&i| series[i])
                    .fold(f64::INFINITY, f64::min);
                let hi = owned
                    .iter()
                    .map(|&i| series[i])
                    .fold(f64::NEG_INFINITY, f64::max);
                assert!(near(to_y(lo)), "column {column}: minimum {lo} missing");
                assert!(near(to_y(hi)), "column {column}: maximum {hi} missing");
                // Nothing inside the column is fabricated beyond the extrema.
                for q in out.iter().filter(|q| q.x > x0 + 1e-9 && q.x < x1 - 1e-9) {
                    assert!(
                        q.y >= to_y(hi) - 1e-6 && q.y <= to_y(lo) + 1e-6,
                        "column {column}: {q:?}"
                    );
                }
            }
        }
    }

    #[test]
    fn point_count_is_bounded() {
        for (n, width, dpr) in [(100, 50.0, 1.0), (100_000, 800.0, 2.0), (10, 2560.0, 2.0)] {
            let mut series = noisy(n);
            for i in (0..n).step_by(97) {
                series[i] = f64::NAN;
            }
            let p = params(width, dpr);
            let mut out = Vec::new();
            decimate(&series, &identity, &p, &mut out);
            let columns = device_columns(width, dpr);
            assert!(
                out.len() <= columns * 5 + 4,
                "{} > {}",
                out.len(),
                columns * 5 + 4
            );
        }
    }

    #[test]
    fn no_allocation_after_warm_up() {
        let series = noisy(10_000);
        let p = params(1280.0, 2.0);
        let mut out = Vec::new();
        decimate(&series, &identity, &p, &mut out);
        let capacity = out.capacity();
        let pointer = out.as_ptr();
        for zoom in 1..50 {
            let q = DecimateParams {
                x_start: 0.01 * zoom as f64,
                x_span: 1.0 / zoom as f64,
                ..p
            };
            decimate(&series, &identity, &q, &mut out);
        }
        assert_eq!(out.capacity(), capacity);
        assert_eq!(out.as_ptr(), pointer);
    }

    #[test]
    fn clip_limits_draw_only_inside_their_window() {
        let series = noisy(1000);
        // Viewport -0.5..0.5, neighbour window -1..0 shifted by one lap.
        let p = DecimateParams {
            x_start: -0.5,
            x_span: 1.0,
            clip_low: -1.0,
            clip_high: 0.0,
            ..params(200.0, 1.0)
        };
        let mut out = Vec::new();
        decimate(&series, &|f: f64| (f + 1.0).clamp(0.0, 1.0), &p, &mut out);
        assert!(!out.is_empty());
        assert!(
            out.iter()
                .filter(|q| !q.is_pen_up())
                .all(|q| q.x <= 100.0 + 1e-9)
        );
    }

    #[test]
    fn degenerate_inputs_emit_nothing() {
        let mut out = vec![PathPoint::new(1.0, 1.0)];
        decimate(&[1.0], &identity, &params(100.0, 1.0), &mut out);
        assert!(out.is_empty());
        decimate(&[1.0, 2.0], &identity, &params(1.0, 1.0), &mut out);
        assert!(out.is_empty());
        let p = DecimateParams {
            y_span: 0.0,
            ..params(100.0, 1.0)
        };
        decimate(&[1.0, 2.0], &identity, &p, &mut out);
        assert!(out.is_empty());
    }
}
