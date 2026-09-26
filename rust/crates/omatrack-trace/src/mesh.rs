//! Triangle meshes for decimated paths, independent of GPUI.
//!
//! GPUI rasterizes a `Path` from raw triangles (4x MSAA where the adapter
//! supports it) and composites premultiplied colour, so overlapping
//! translucent triangles would blend twice. The meshes here therefore never
//! overlap:
//!
//! - [`stroke`]: one strip per pen-down run with two shared vertices per
//!   joint, bounded miter joins (limit 2) and no caps; a pen-up starts a new
//!   strip. Width is in logical pixels, independent of zoom.
//! - [`fill_to_baseline`]: non-overlapping trapezoids between the path and a
//!   baseline, split at baseline crossings into an "above" and a "below"
//!   sink (the delta lane colours them as gain and loss).
//! - [`fill_band`]: one quad per column between an envelope's top and
//!   bottom (the session spread band); a gap column breaks the band.
//!
//! Vertices are emitted as plain triangles into a [`TriangleSink`]; the GPUI
//! adapter writes them straight into `Path::vertices` (u32-addressed, so the
//! 65,535-vertex cliff of 16-bit index batches does not exist here).

use crate::decimate::PathPoint;

/// Receives triangles in logical pixels.
pub trait TriangleSink {
    fn triangle(&mut self, a: [f32; 2], b: [f32; 2], c: [f32; 2]);
    /// Reserve room for `count` more triangles.
    fn reserve(&mut self, _count: usize) {}
}

impl TriangleSink for Vec<[[f32; 2]; 3]> {
    fn triangle(&mut self, a: [f32; 2], b: [f32; 2], c: [f32; 2]) {
        self.push([a, b, c]);
    }
    fn reserve(&mut self, count: usize) {
        Vec::reserve(self, count);
    }
}

/// Counts triangles without storing them.
#[derive(Default, Debug)]
pub struct TriangleCounter(pub usize);

impl TriangleSink for TriangleCounter {
    fn triangle(&mut self, _: [f32; 2], _: [f32; 2], _: [f32; 2]) {
        self.0 += 1;
    }
}

#[inline]
fn pt(x: f64, y: f64) -> [f32; 2] {
    [x as f32, y as f32]
}

/// Unit normal of segment a→b, or zero for a degenerate segment.
#[inline]
fn normal(a: PathPoint, b: PathPoint) -> (f64, f64) {
    let (dx, dy) = (b.x - a.x, b.y - a.y);
    let length = (dx * dx + dy * dy).sqrt();
    if length > 1.0e-9 {
        (-dy / length, dx / length)
    } else {
        (0.0, 0.0)
    }
}

/// Join vector at an interior vertex from the adjacent segment normals.
/// Bounded like the Qt builder: never longer than 2 (miter limit 2), and a
/// full reversal falls back to the outgoing normal.
#[inline]
fn join(a: (f64, f64), b: (f64, f64)) -> (f64, f64) {
    let sum = (a.0 + b.0, a.1 + b.1);
    let denominator = 1.0 + a.0 * b.0 + a.1 * b.1;
    if denominator < 1.0e-8 {
        return b;
    }
    let divisor = if denominator >= 0.5 {
        denominator
    } else {
        (2.0 * denominator).sqrt() * 0.5
    };
    (sum.0 / divisor, sum.1 / divisor)
}

/// Stroke every pen-down run of `points` with `width` logical pixels.
pub fn stroke(points: &[PathPoint], width: f64, sink: &mut impl TriangleSink) {
    if width <= 0.0 || !width.is_finite() {
        return;
    }
    let half = width * 0.5;
    sink.reserve(points.len() * 2);
    let mut first = 0;
    for i in 0..=points.len() {
        if i < points.len() && !points[i].is_pen_up() {
            continue;
        }
        stroke_run(&points[first..i], half, sink);
        first = i + 1;
    }
}

fn stroke_run(run: &[PathPoint], half: f64, sink: &mut impl TriangleSink) {
    let count = run.len();
    if count < 2 {
        return;
    }
    let mut previous_normal = normal(run[0], run[1]);
    let mut left = pt(
        run[0].x + previous_normal.0 * half,
        run[0].y + previous_normal.1 * half,
    );
    let mut right = pt(
        run[0].x - previous_normal.0 * half,
        run[0].y - previous_normal.1 * half,
    );
    for i in 1..count {
        let next_normal = if i + 1 < count {
            normal(run[i], run[i + 1])
        } else {
            previous_normal
        };
        let n = if i + 1 < count {
            join(previous_normal, next_normal)
        } else {
            previous_normal
        };
        let p = run[i];
        let next_left = pt(p.x + n.0 * half, p.y + n.1 * half);
        let next_right = pt(p.x - n.0 * half, p.y - n.1 * half);
        sink.triangle(left, right, next_left);
        sink.triangle(right, next_right, next_left);
        left = next_left;
        right = next_right;
        previous_normal = next_normal;
    }
}

/// Fill the area between each pen-down run of `points` and the horizontal
/// line `baseline` with non-overlapping trapezoids. Parts above the
/// baseline (smaller y) go to `above`, parts below to `below`. `points` must
/// be non-decreasing in x within each run (decimator output is).
pub fn fill_to_baseline(
    points: &[PathPoint],
    baseline: f64,
    above: &mut impl TriangleSink,
    below: &mut impl TriangleSink,
) {
    above.reserve(points.len() * 2);
    for pair in points.windows(2) {
        let (a, b) = (pair[0], pair[1]);
        if a.is_pen_up() || b.is_pen_up() {
            continue;
        }
        let da = a.y - baseline;
        let db = b.y - baseline;
        if da * db < 0.0 {
            let crossing =
                PathPoint::new(a.x + (b.x - a.x) * (baseline - a.y) / (b.y - a.y), baseline);
            trapezoid(a, crossing, baseline, above, below);
            trapezoid(crossing, b, baseline, above, below);
        } else {
            trapezoid(a, b, baseline, above, below);
        }
    }
}

#[inline]
fn trapezoid(
    a: PathPoint,
    b: PathPoint,
    baseline: f64,
    above: &mut impl TriangleSink,
    below: &mut impl TriangleSink,
) {
    if b.x <= a.x {
        return;
    }
    let (ay, by) = (a.y - baseline, b.y - baseline);
    if ay == 0.0 && by == 0.0 {
        return;
    }
    if ay + by < 0.0 {
        emit_trapezoid(a, b, baseline, above);
    } else {
        emit_trapezoid(a, b, baseline, below);
    }
}

#[inline]
fn emit_trapezoid(a: PathPoint, b: PathPoint, baseline: f64, sink: &mut impl TriangleSink) {
    let top_a = pt(a.x, a.y);
    let top_b = pt(b.x, b.y);
    let base_a = pt(a.x, baseline);
    let base_b = pt(b.x, baseline);
    // One edge may sit on the baseline (a crossing); skip the empty triangle.
    if a.y != baseline {
        sink.triangle(top_a, top_b, base_a);
    }
    if b.y != baseline {
        sink.triangle(top_b, base_b, base_a);
    }
}

/// One column of a band: the band spans `top..=bottom` (logical pixels,
/// `top <= bottom`) at `x`. A non-finite `top` or `bottom` lifts the band.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BandColumn {
    pub x: f64,
    pub top: f64,
    pub bottom: f64,
}

impl BandColumn {
    pub const GAP: BandColumn = BandColumn {
        x: f64::NAN,
        top: f64::NAN,
        bottom: f64::NAN,
    };

    fn is_gap(&self) -> bool {
        !(self.x.is_finite() && self.top.is_finite() && self.bottom.is_finite())
    }
}

/// Fill between the tops and bottoms of consecutive `columns` (increasing
/// in `x`) with one quad each: the quads share only their vertical edges,
/// so nothing blends twice. A gap column breaks the band.
pub fn fill_band(columns: &[BandColumn], sink: &mut impl TriangleSink) {
    sink.reserve(columns.len() * 2);
    for pair in columns.windows(2) {
        let (a, b) = (pair[0], pair[1]);
        if a.is_gap() || b.is_gap() || b.x <= a.x {
            continue;
        }
        let (at, ab) = (pt(a.x, a.top), pt(a.x, a.bottom));
        let (bt, bb) = (pt(b.x, b.top), pt(b.x, b.bottom));
        if a.bottom > a.top {
            sink.triangle(at, bt, ab);
        }
        if b.bottom > b.top {
            sink.triangle(bt, bb, ab);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn band_quads_cover_the_envelope_once() {
        let columns: Vec<BandColumn> = (0..20)
            .map(|i| {
                if i == 10 {
                    BandColumn::GAP
                } else {
                    let x = i as f64 * 2.0;
                    BandColumn {
                        x,
                        top: 20.0 - (i % 3) as f64,
                        bottom: 30.0 + (i % 4) as f64,
                    }
                }
            })
            .collect();
        let mut triangles: Vec<[[f32; 2]; 3]> = Vec::new();
        fill_band(&columns, &mut triangles);
        // 19 pairs minus the two touching the gap, two triangles each.
        assert_eq!(triangles.len(), 17 * 2);
        let mut x = 0.25;
        while x < 38.0 {
            let mut y = 15.25;
            while y < 36.0 {
                let hits = triangles.iter().filter(|t| inside(t, (x, y))).count();
                assert!(hits <= 1, "({x}, {y}) covered {hits} times");
                y += 0.5;
            }
            x += 0.5;
        }
        // Nothing spans the gap between x = 18 and x = 22.
        assert!(
            triangles
                .iter()
                .all(|t| !(t.iter().any(|v| v[0] < 19.0) && t.iter().any(|v| v[0] > 21.0)))
        );
    }

    fn area(t: &[[f32; 2]; 3]) -> f64 {
        let [a, b, c] = *t;
        ((b[0] - a[0]) as f64 * (c[1] - a[1]) as f64 - (c[0] - a[0]) as f64 * (b[1] - a[1]) as f64)
            .abs()
            * 0.5
    }

    /// Point-in-triangle by barycentric signs, strictly inside.
    fn inside(t: &[[f32; 2]; 3], p: (f64, f64)) -> bool {
        let s = |a: [f32; 2], b: [f32; 2]| {
            (b[0] as f64 - a[0] as f64) * (p.1 - a[1] as f64)
                - (b[1] as f64 - a[1] as f64) * (p.0 - a[0] as f64)
        };
        let d1 = s(t[0], t[1]);
        let d2 = s(t[1], t[2]);
        let d3 = s(t[2], t[0]);
        let eps = 1e-6;
        (d1 > eps && d2 > eps && d3 > eps) || (d1 < -eps && d2 < -eps && d3 < -eps)
    }

    fn zigzag(n: usize) -> Vec<PathPoint> {
        (0..n)
            .map(|i| {
                PathPoint::new(
                    i as f64 * 3.0,
                    50.0 + if i % 2 == 0 { -30.0 } else { 25.0 } * ((i % 5) as f64 / 4.0),
                )
            })
            .collect()
    }

    #[test]
    fn fill_triangles_never_overlap() {
        let points = zigzag(40);
        let mut above: Vec<[[f32; 2]; 3]> = Vec::new();
        let mut below: Vec<[[f32; 2]; 3]> = Vec::new();
        fill_to_baseline(&points, 50.0, &mut above, &mut below);
        assert!(!above.is_empty() && !below.is_empty());
        let all: Vec<_> = above.iter().chain(below.iter()).collect();
        // Sample a dense grid: every point is covered at most once.
        let mut x = 0.25;
        while x < 117.0 {
            let mut y = 0.25;
            while y < 100.0 {
                let hits = all.iter().filter(|t| inside(t, (x, y))).count();
                assert!(hits <= 1, "({x}, {y}) covered {hits} times");
                y += 0.5;
            }
            x += 0.5;
        }
        // Above triangles stay above the baseline and below ones below.
        assert!(above.iter().flatten().all(|p| p[1] <= 50.0 + 1e-4));
        assert!(below.iter().flatten().all(|p| p[1] >= 50.0 - 1e-4));
        // Total area equals the integral of |y - baseline|.
        let expected: f64 = points
            .windows(2)
            .map(|w| {
                let (a, b) = (w[0].y - 50.0, w[1].y - 50.0);
                let dx = w[1].x - w[0].x;
                if a * b < 0.0 {
                    let t = a.abs() / (a.abs() + b.abs());
                    0.5 * dx * (t * a.abs() + (1.0 - t) * b.abs())
                } else {
                    0.5 * dx * (a.abs() + b.abs())
                }
            })
            .sum();
        let actual: f64 = all.iter().map(|t| area(t)).sum();
        assert!((expected - actual).abs() < 1e-2, "{expected} vs {actual}");
    }

    #[test]
    fn stroke_breaks_at_pen_ups() {
        let mut points = zigzag(10);
        points.insert(5, PathPoint::PEN_UP);
        let mut triangles: Vec<[[f32; 2]; 3]> = Vec::new();
        stroke(&points, 1.25, &mut triangles);
        // Runs of 5 and 5 points: 4 + 4 segments, two triangles each.
        assert_eq!(triangles.len(), 16);
        // No triangle bridges the gap between x = 12 and x = 15.
        for t in &triangles {
            let xs: Vec<f32> = t.iter().map(|p| p[0]).collect();
            let min = xs.iter().cloned().fold(f32::INFINITY, f32::min);
            let max = xs.iter().cloned().fold(f32::NEG_INFINITY, f32::max);
            assert!(!(min < 13.0 && max > 14.0), "bridged gap: {t:?}");
        }
        // Isolated single points (a run of one) draw nothing and do not panic.
        let mut single: Vec<[[f32; 2]; 3]> = Vec::new();
        stroke(
            &[
                PathPoint::new(1.0, 1.0),
                PathPoint::PEN_UP,
                PathPoint::new(2.0, 2.0),
            ],
            1.0,
            &mut single,
        );
        assert!(single.is_empty());
    }

    #[test]
    fn stroke_width_is_constant_and_joins_are_bounded() {
        let points = [
            PathPoint::new(0.0, 0.0),
            PathPoint::new(10.0, 0.0),
            PathPoint::new(10.5, -30.0),
            PathPoint::new(11.0, 0.0),
        ];
        let mut triangles: Vec<[[f32; 2]; 3]> = Vec::new();
        stroke(&points, 2.0, &mut triangles);
        // Horizontal first segment: vertices exactly one half-width off.
        assert!((triangles[0][0][1] + 1.0).abs() < 1e-6 || (triangles[0][0][1] - 1.0).abs() < 1e-6);
        // Miter limit 2: no vertex strays more than 2 half-widths from its sample.
        for t in &triangles {
            for v in t {
                let near = points
                    .iter()
                    .map(|p| ((v[0] as f64 - p.x).powi(2) + (v[1] as f64 - p.y).powi(2)).sqrt())
                    .fold(f64::INFINITY, f64::min);
                assert!(near <= 2.0 + 1e-4, "{v:?} is {near} away");
            }
        }
    }

    #[test]
    fn more_than_65535_vertices_are_kept() {
        let points: Vec<PathPoint> = (0..40_000)
            .map(|i| PathPoint::new(i as f64 * 0.1, 10.0 + ((i * 37) % 11) as f64))
            .collect();
        let mut triangles: Vec<[[f32; 2]; 3]> = Vec::new();
        stroke(&points, 1.0, &mut triangles);
        assert_eq!(triangles.len(), 2 * 39_999);
        assert!(triangles.len() * 3 > 65_535);
        // The last segment is present, at the end of the run.
        let last = triangles.last().unwrap();
        assert!(last.iter().any(|v| (v[0] - 3999.9).abs() < 1.0));
    }
}
