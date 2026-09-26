//! The shared horizontal scale: viewport in lap fraction, zoom/pan, the
//! Distance/Time x-axis and nice tick generation.
//!
//! The horizontal coordinate is always the primary lap fraction. The 50 Hz
//! lap is uniform in time, so fraction is linear in time; Distance mode
//! labels the same axis in metres (ticks land where the lap reaches each
//! nice distance). Every lane, the cursor, the delta and the video therefore
//! share one mapping.

/// Numerical zoom floor, in lap fraction. Sub-sample inspection is allowed.
pub const MIN_SPAN: f64 = 1.0e-7;

/// The visible window of the primary lap, in lap fraction. It may extend past
/// `0..1` (a focused corner near start/finish keeps its place; the renderer
/// fills the overhang with the neighbouring lap).
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct Viewport {
    pub start: f64,
    pub end: f64,
}

impl Default for Viewport {
    fn default() -> Self {
        Self::FULL
    }
}

impl Viewport {
    pub const FULL: Self = Self {
        start: 0.0,
        end: 1.0,
    };

    /// A viewport from `start` to `end`; a non-finite or inverted range
    /// becomes the full lap and a span below [`MIN_SPAN`] is widened about
    /// its centre.
    pub fn new(start: f64, end: f64) -> Self {
        if !start.is_finite() || !end.is_finite() || end <= start {
            return Self::FULL;
        }
        if end - start < MIN_SPAN {
            let centre = (start + end) * 0.5;
            return Self {
                start: centre - MIN_SPAN * 0.5,
                end: centre + MIN_SPAN * 0.5,
            };
        }
        Self { start, end }
    }

    pub fn span(&self) -> f64 {
        self.end - self.start
    }

    #[expect(
        clippy::float_cmp,
        reason = "Exact equality detects unchanged state or the full-view sentinel; epsilon would hide small changes."
    )]
    pub fn is_full(&self) -> bool {
        self.start == 0.0 && self.end == 1.0
    }

    /// Logical x of a lap fraction inside a plot of `width` starting at `left`.
    pub fn x_for_fraction(&self, fraction: f64, left: f64, width: f64) -> f64 {
        left + (fraction - self.start) / self.span() * width
    }

    /// Lap fraction under logical x, clamped to the visible window (as the
    /// Qt `fracForX`).
    pub fn fraction_for_x(&self, x: f64, left: f64, width: f64) -> f64 {
        let width = width.max(1.0);
        self.start + ((x - left) / width).clamp(0.0, 1.0) * self.span()
    }

    /// Zoom by `factor` (<1 zooms in) keeping `anchor` (a lap fraction) under
    /// the pointer. The result stays inside the lap; span is clamped to
    /// `MIN_SPAN..=1`. Port of `TelemetryStore::zoomAt`.
    #[must_use]
    pub fn zoom_about(&self, anchor: f64, factor: f64) -> Self {
        if !anchor.is_finite() || !factor.is_finite() || factor <= 0.0 {
            return *self;
        }
        let span = self.span();
        if span <= 0.0 {
            return *self;
        }
        let new_span = (span * factor).clamp(MIN_SPAN, 1.0);
        let anchor = anchor.clamp(0.0, 1.0);
        let share = (anchor - self.start) / span;
        let mut start = anchor - share * new_span;
        let mut end = start + new_span;
        if start < 0.0 {
            end -= start;
            start = 0.0;
        }
        if end > 1.0 {
            start -= end - 1.0;
            end = 1.0;
            start = start.clamp(0.0, 1.0);
        }
        Self { start, end }
    }

    /// Shift by `delta` lap fraction, clamped inside the lap. The Qt store
    /// scaled its argument by the span a second time; here the argument is
    /// the lap-fraction distance itself, so a grab-pan keeps the grabbed
    /// sample under the pointer.
    #[must_use]
    pub fn pan_by(&self, delta: f64) -> Self {
        if !delta.is_finite() {
            return *self;
        }
        let span = self.span();
        let start = (self.start + delta).clamp(0.0, (1.0 - span).max(0.0));
        Self {
            start,
            end: start + span,
        }
    }

    /// The viewport that places a corner zone in the middle of the left half
    /// of the workspace: the zone takes 30% of the view, its centre sits at
    /// 25%. Deliberately unclamped (see the type docs). Port of
    /// `TelemetryStore::focusCorner`.
    pub fn focus_on(start: f64, end: f64) -> Self {
        const ZONE_SHARE_OF_VIEW: f64 = 0.30;
        const CORNER_CENTRE: f64 = 0.25;
        let zone = (end - start).max(0.002);
        let span = (zone / ZONE_SHARE_OF_VIEW).clamp(0.004, 1.0);
        let mid = (start + end) * 0.5;
        let view_start = mid - CORNER_CENTRE * span;
        Self {
            start: view_start,
            end: view_start + span,
        }
    }

    /// Linear interpolation between two viewports (for animation).
    #[must_use]
    pub fn lerp(&self, to: &Self, t: f64) -> Self {
        Self {
            start: self.start + (to.start - self.start) * t,
            end: self.end + (to.end - self.end) * t,
        }
    }
}

/// What the shared x-axis is labelled in.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum XAxis {
    #[default]
    Distance,
    Time,
}

impl XAxis {
    #[must_use]
    pub fn toggled(self) -> Self {
        match self {
            Self::Distance => Self::Time,
            Self::Time => Self::Distance,
        }
    }
}

/// One axis tick: where it sits (lap fraction) and its label.
#[derive(Clone, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct Tick {
    pub fraction: f64,
    pub value: f64,
    pub label: String,
}

impl Tick {
    pub fn new(fraction: f64, value: f64, label: impl Into<String>) -> Self {
        Self {
            fraction,
            value,
            label: label.into(),
        }
    }
}

/// Value of a sampled lap array at a lap fraction (linear interpolation).
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Clamped lap fractions map to indices in resident sample buffers; interpolation intentionally uses f64."
)]
pub fn value_at_fraction(values: &[f64], fraction: f64) -> f64 {
    if values.is_empty() {
        return f64::NAN;
    }
    if values.len() == 1 {
        return values[0];
    }
    let last = values.len() - 1;
    let position = fraction.clamp(0.0, 1.0) * last as f64;
    let low = (position.floor() as usize).min(last);
    let high = (low + 1).min(last);
    let a = values[low];
    let b = values[high];
    a + (b - a) * (position - low as f64)
}

/// Lap fraction at which a monotonic non-decreasing array first reaches
/// `value` (linear interpolation between samples).
#[expect(
    clippy::cast_precision_loss,
    reason = "Clamped lap fractions map to indices in resident sample buffers; interpolation intentionally uses f64."
)]
pub fn fraction_at_value(monotonic: &[f64], value: f64) -> f64 {
    let n = monotonic.len();
    if n < 2 || !value.is_finite() {
        return f64::NAN;
    }
    let last = (n - 1) as f64;
    let upper = monotonic.partition_point(|&v| v < value);
    if upper == 0 {
        return 0.0;
    }
    if upper >= n {
        return 1.0;
    }
    let (a, b) = (monotonic[upper - 1], monotonic[upper]);
    let t = if b > a { (value - a) / (b - a) } else { 0.0 };
    ((upper - 1) as f64 + t) / last
}

/// The 1-2-5 step at or above `raw`.
pub fn nice_step(raw: f64) -> f64 {
    if !raw.is_finite() || raw <= 0.0 {
        return 1.0;
    }
    let magnitude = 10f64.powf(raw.log10().floor());
    for multiple in [1.0, 2.0, 5.0, 10.0] {
        let step = multiple * magnitude;
        if step >= raw * (1.0 - 1e-9) {
            return step;
        }
    }
    10.0 * magnitude
}

/// Time step at or above `raw` seconds, preferring clock-friendly values
/// (…, 0.5, 1, 2, 5, 10, 15, 30, 60, 120 s).
pub fn nice_time_step(raw: f64) -> f64 {
    if raw <= 1.0 {
        return nice_step(raw);
    }
    for step in [2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0] {
        if step >= raw * (1.0 - 1e-9) {
            return step;
        }
    }
    nice_step(raw)
}

fn group_thousands(value: i64) -> String {
    let digits = value.unsigned_abs().to_string();
    let mut out = String::with_capacity(digits.len() + digits.len() / 3 + 1);
    if value < 0 {
        out.push('-');
    }
    for (i, c) in digits.chars().enumerate() {
        if i > 0 && (digits.len() - i).is_multiple_of(3) {
            out.push(',');
        }
        out.push(c);
    }
    out
}

/// "850 m", "1,250 m", "12.5 m" (decimals only when the step needs them).
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "Axis labels intentionally round floating-point units to integer display precision."
)]
pub fn format_distance(metres: f64, step: f64) -> String {
    if step >= 1.0 {
        format!("{} m", group_thousands(metres.round() as i64))
    } else {
        let decimals = (-step.log10().floor()) as usize;
        format!("{metres:.decimals$} m")
    }
}

/// "0:42", "1:13.6", "0:42.31" (decimals only when the step needs them).
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_sign_loss,
    reason = "Axis labels intentionally round floating-point units to integer display precision."
)]
pub fn format_time(seconds: f64, step: f64) -> String {
    let negative = seconds < 0.0;
    let seconds = seconds.abs();
    let decimals = if step >= 1.0 {
        0
    } else {
        ((-step.log10().floor()) as usize).min(3)
    };
    let scale = 10f64.powi(decimals as i32);
    let total = (seconds * scale).round() / scale;
    let minutes = (total / 60.0).floor();
    let rest = total - minutes * 60.0;
    let width = if decimals == 0 { 2 } else { decimals + 3 };
    format!(
        "{}{}:{:0width$.decimals$}",
        if negative { "-" } else { "" },
        minutes as i64,
        rest
    )
}

/// Ticks for the visible part of the lap, at least `min_spacing` logical pixels apart
/// across a plot `width` wide.
///
/// `values` is the lap's distance (m) or time (s) array; both are monotonic on the
/// normalized lap. Writes into `out` (cleared first).
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
)]
#[expect(
    clippy::neg_cmp_op_on_partial_ord,
    reason = "Negated ordered comparisons deliberately include unordered (NaN) values; preserve that behavior."
)]
pub fn axis_ticks(
    axis: XAxis,
    viewport: &Viewport,
    values: &[f64],
    width: f64,
    min_spacing: f64,
    out: &mut Vec<Tick>,
) {
    out.clear();
    if values.len() < 2 || width <= 0.0 || min_spacing <= 0.0 {
        return;
    }
    let origin = values[0];
    let visible_start = viewport.start.clamp(0.0, 1.0);
    let visible_end = viewport.end.clamp(0.0, 1.0);
    if visible_end <= visible_start {
        return;
    }
    let low = value_at_fraction(values, visible_start) - origin;
    let high = value_at_fraction(values, visible_end) - origin;
    if !(high > low) {
        return;
    }
    // Average units per pixel across the visible window.
    let visible_width = width * (visible_end - visible_start) / viewport.span();
    let raw = (high - low) / visible_width.max(1.0) * min_spacing;
    let step = match axis {
        XAxis::Distance => nice_step(raw),
        XAxis::Time => nice_time_step(raw),
    };
    let first = (low / step).ceil() as i64;
    let last = (high / step).floor() as i64;
    if last - first > 2_000 {
        return;
    }
    for k in first..=last {
        let value = k as f64 * step;
        let fraction = fraction_at_value(values, value + origin);
        if !fraction.is_finite() {
            continue;
        }
        let label = match axis {
            XAxis::Distance => format_distance(value, step),
            XAxis::Time => format_time(value, step),
        };
        out.push(Tick {
            fraction,
            value,
            label,
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zoom_keeps_anchor_and_respects_floor() {
        let v = Viewport::FULL.zoom_about(0.3, 0.5);
        assert!((v.span() - 0.5).abs() < 1e-12);
        // Anchor keeps its share of the view.
        assert!(((0.3 - v.start) / v.span() - 0.3).abs() < 1e-12);
        let mut w = Viewport::FULL;
        for _ in 0..200 {
            w = w.zoom_about(0.61, 0.5);
        }
        assert!((w.span() - MIN_SPAN).abs() < 1e-15);
        assert!(w.start <= 0.61 && w.end >= 0.61);
        let out = w.zoom_about(0.5, 1e9);
        assert_eq!(out, Viewport::FULL);
    }

    #[test]
    fn zoom_stays_inside_the_lap() {
        let v = Viewport::new(0.0, 0.2).zoom_about(0.0, 3.0);
        assert!(v.start >= 0.0 && v.end <= 1.0);
        assert!((v.span() - 0.6).abs() < 1e-12);
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn pan_is_in_lap_fraction_and_clamped() {
        let v = Viewport::new(0.2, 0.4).pan_by(0.1);
        assert!((v.start - 0.3).abs() < 1e-12 && (v.end - 0.5).abs() < 1e-12);
        let v = Viewport::new(0.2, 0.4).pan_by(5.0);
        assert!((v.end - 1.0).abs() < 1e-12);
        let v = Viewport::new(0.2, 0.4).pan_by(-5.0);
        assert_eq!(v.start, 0.0);
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn x_and_fraction_round_trip() {
        let v = Viewport::new(0.25, 0.75);
        let x = v.x_for_fraction(0.5, 100.0, 400.0);
        assert!((x - 300.0).abs() < 1e-9);
        assert!((v.fraction_for_x(x, 100.0, 400.0) - 0.5).abs() < 1e-12);
        assert_eq!(v.fraction_for_x(-50.0, 100.0, 400.0), 0.25);
    }

    #[test]
    fn focus_places_corner_in_left_half() {
        let v = Viewport::focus_on(0.40, 0.46);
        assert!((v.span() - 0.2).abs() < 1e-12);
        let mid = 0.43;
        assert!(((mid - v.start) / v.span() - 0.25).abs() < 1e-12);
        // Near the start the view may run off the lap.
        assert!(Viewport::focus_on(0.0, 0.03).start < 0.0);
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn nice_steps() {
        assert_eq!(nice_step(0.7), 1.0);
        assert_eq!(nice_step(1.3), 2.0);
        assert_eq!(nice_step(3.0), 5.0);
        assert_eq!(nice_step(120.0), 200.0);
        assert_eq!(nice_time_step(11.0), 15.0);
        assert_eq!(nice_time_step(0.03), 0.05);
    }

    #[test]
    fn labels() {
        assert_eq!(format_distance(1250.0, 250.0), "1,250 m");
        assert_eq!(format_distance(850.0, 50.0), "850 m");
        assert_eq!(format_distance(12.5, 0.5), "12.5 m");
        assert_eq!(format_time(42.0, 5.0), "0:42");
        assert_eq!(format_time(73.644, 0.1), "1:13.6");
        assert_eq!(format_time(42.31, 0.01), "0:42.31");
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn distance_ticks_land_on_nice_values() {
        // 4000 m lap, 90 s, non-uniform speed.
        let n = 4501;
        let distance: Vec<f64> = (0..n)
            .map(|i| {
                let t = f64::from(i) / f64::from(n - 1);
                4000.0 * (t + 0.05 * (t * 12.0).sin() / 12.0)
            })
            .collect();
        let mut ticks = Vec::new();
        axis_ticks(
            XAxis::Distance,
            &Viewport::FULL,
            &distance,
            1000.0,
            80.0,
            &mut ticks,
        );
        assert!(ticks.len() >= 5 && ticks.len() <= 13, "{}", ticks.len());
        let step = ticks[1].value - ticks[0].value;
        assert_eq!(nice_step(step), step);
        for tick in &ticks {
            let at = value_at_fraction(&distance, tick.fraction);
            assert!((at - tick.value).abs() < 1e-6, "{tick:?} -> {at}");
        }
        // Zoomed in: finer steps, still inside the viewport.
        let view = Viewport::new(0.5, 0.52);
        axis_ticks(XAxis::Distance, &view, &distance, 1000.0, 80.0, &mut ticks);
        assert!(!ticks.is_empty());
        assert!(
            ticks
                .iter()
                .all(|t| t.fraction >= 0.5 - 1e-9 && t.fraction <= 0.52 + 1e-9)
        );
    }

    #[test]
    fn time_ticks_use_clock_labels() {
        let time: Vec<f64> = (0..4501).map(|i| f64::from(i) * 0.02).collect();
        let mut ticks = Vec::new();
        axis_ticks(XAxis::Time, &Viewport::FULL, &time, 900.0, 80.0, &mut ticks);
        assert_eq!(ticks[0].label, "0:00");
        assert!(ticks.iter().any(|t| t.label == "1:00"));
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn fraction_lookup_handles_flat_runs() {
        let values = [0.0, 1.0, 1.0, 1.0, 2.0];
        assert!((fraction_at_value(&values, 1.0) - 0.25).abs() < 1e-12);
        assert!((fraction_at_value(&values, 1.5) - 0.875).abs() < 1e-12);
        assert_eq!(fraction_at_value(&values, -1.0), 0.0);
        assert_eq!(fraction_at_value(&values, 9.0), 1.0);
    }
}
