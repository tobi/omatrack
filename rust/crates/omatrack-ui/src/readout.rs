//! Numeric readouts: a value with a muted unit, and a signed delta colored by
//! whether it is a gain or a loss.
//!
//! Both render in the theme's monospace family so digits keep a fixed width
//! and columns of readouts line up while values change.

use gpui_kit::component::{ActiveTheme as _, h_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, IntoElement, ParentElement as _, RenderOnce, SharedString, Styled as _, Window, div,
};

/// Shown for a value that does not exist (no sample, NaN).
pub const MISSING_VALUE: &str = "—";

/// Format `value` with a fixed number of decimals, or the missing marker for
/// `None` and non-finite values.
pub fn format_value(value: Option<f64>, decimals: usize) -> SharedString {
    match value {
        Some(value) if value.is_finite() => format!("{value:.decimals$}").into(),
        _ => MISSING_VALUE.into(),
    }
}

/// Whether a delta is better, worse or neither.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeltaTrend {
    Gain,
    Loss,
    Even,
}

/// Which direction of a delta is a gain.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum DeltaSense {
    /// Time deltas: negative means the primary lap is ahead.
    #[default]
    LowerIsBetter,
    /// Speed deltas: positive means the primary lap is faster.
    HigherIsBetter,
}

/// Format a delta with an explicit sign (`+0.123`, `-0.123`, `±0.000`) and
/// classify it. A value that rounds to zero at `decimals` is even, so the
/// sign and color never disagree with the digits shown.
pub fn format_delta(
    value: Option<f64>,
    decimals: usize,
    sense: DeltaSense,
) -> (SharedString, DeltaTrend) {
    let Some(value) = value.filter(|value| value.is_finite()) else {
        return (MISSING_VALUE.into(), DeltaTrend::Even);
    };
    let magnitude = format!("{:.decimals$}", value.abs());
    let is_zero = magnitude.bytes().all(|b| b == b'0' || b == b'.');
    if is_zero {
        return (format!("±{magnitude}").into(), DeltaTrend::Even);
    }
    let sign = if value > 0. { '+' } else { '-' };
    let better = match sense {
        DeltaSense::LowerIsBetter => value < 0.,
        DeltaSense::HigherIsBetter => value > 0.,
    };
    let trend = if better {
        DeltaTrend::Gain
    } else {
        DeltaTrend::Loss
    };
    (format!("{sign}{magnitude}").into(), trend)
}

/// A value in monospace digits with an optional muted unit.
#[derive(IntoElement)]
pub struct Readout {
    value: SharedString,
    unit: Option<SharedString>,
}

impl Readout {
    /// Preformatted text, for values with their own formatting (lap times).
    pub fn new(value: impl Into<SharedString>) -> Self {
        Self {
            value: value.into(),
            unit: None,
        }
    }

    /// A number with a fixed count of decimals; `None` and NaN read as `—`.
    pub fn number(value: Option<f64>, decimals: usize) -> Self {
        Self::new(format_value(value, decimals))
    }

    /// The unit suffix, drawn muted after the value (`km/h`, `s`, `%`).
    pub fn unit(mut self, unit: impl Into<SharedString>) -> Self {
        self.unit = Some(unit.into());
        self
    }
}

impl RenderOnce for Readout {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        h_flex()
            .items_baseline()
            .gap_0p5()
            .font_family(theme.mono_font_family.clone())
            .whitespace_nowrap()
            .child(self.value)
            .when_some(self.unit, |this, unit| {
                this.child(div().text_color(theme.muted_foreground).child(unit))
            })
    }
}

/// A signed delta in monospace digits, colored by trend: gain uses the
/// theme's `success`, loss its `danger`, even stays muted. The explicit sign
/// carries the same meaning for readers who cannot rely on color.
#[derive(IntoElement)]
pub struct DeltaText {
    value: Option<f64>,
    decimals: usize,
    sense: DeltaSense,
    unit: Option<SharedString>,
}

impl DeltaText {
    /// A delta with three decimals where lower is better (time).
    pub fn new(value: Option<f64>) -> Self {
        Self {
            value,
            decimals: 3,
            sense: DeltaSense::default(),
            unit: None,
        }
    }

    pub fn decimals(mut self, decimals: usize) -> Self {
        self.decimals = decimals;
        self
    }

    pub fn sense(mut self, sense: DeltaSense) -> Self {
        self.sense = sense;
        self
    }

    pub fn unit(mut self, unit: impl Into<SharedString>) -> Self {
        self.unit = Some(unit.into());
        self
    }
}

impl RenderOnce for DeltaText {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        let (text, trend) = format_delta(self.value, self.decimals, self.sense);
        let color = match trend {
            DeltaTrend::Gain => theme.success,
            DeltaTrend::Loss => theme.danger,
            DeltaTrend::Even => theme.muted_foreground,
        };
        h_flex()
            .items_baseline()
            .gap_0p5()
            .font_family(theme.mono_font_family.clone())
            .whitespace_nowrap()
            .child(div().text_color(color).child(text))
            .when_some(self.unit, |this, unit| {
                this.child(div().text_color(theme.muted_foreground).child(unit))
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn values_format_with_fixed_decimals_and_a_missing_marker() {
        assert_eq!(format_value(Some(123.456), 1).as_ref(), "123.5");
        assert_eq!(format_value(Some(-0.25), 2).as_ref(), "-0.25");
        assert_eq!(format_value(None, 1).as_ref(), MISSING_VALUE);
        assert_eq!(format_value(Some(f64::NAN), 1).as_ref(), MISSING_VALUE);
    }

    #[test]
    fn time_deltas_are_gains_when_negative() {
        assert_eq!(
            format_delta(Some(-0.1234), 3, DeltaSense::LowerIsBetter),
            ("-0.123".into(), DeltaTrend::Gain)
        );
        assert_eq!(
            format_delta(Some(0.5), 3, DeltaSense::LowerIsBetter),
            ("+0.500".into(), DeltaTrend::Loss)
        );
    }

    #[test]
    fn speed_deltas_are_gains_when_positive() {
        assert_eq!(
            format_delta(Some(2.26), 1, DeltaSense::HigherIsBetter),
            ("+2.3".into(), DeltaTrend::Gain)
        );
        assert_eq!(
            format_delta(Some(-2.26), 1, DeltaSense::HigherIsBetter),
            ("-2.3".into(), DeltaTrend::Loss)
        );
    }

    #[test]
    fn deltas_that_round_to_zero_are_even() {
        assert_eq!(
            format_delta(Some(-0.0004), 3, DeltaSense::LowerIsBetter),
            ("±0.000".into(), DeltaTrend::Even)
        );
        assert_eq!(
            format_delta(Some(0.0), 0, DeltaSense::LowerIsBetter),
            ("±0".into(), DeltaTrend::Even)
        );
        assert_eq!(
            format_delta(None, 3, DeltaSense::LowerIsBetter),
            (MISSING_VALUE.into(), DeltaTrend::Even)
        );
    }
}
