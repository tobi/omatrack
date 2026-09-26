//! The shared x-axis row under the lanes: a tick mark at each round
//! distance (or time), labelled in the trace numerals (`0`, `500 m`,
//! `1 km`), with an unlabelled minor mark halfway between.
//!
//! Tick positions come from [`crate::scale::axis_ticks`] with the plot's
//! viewport and width. Labels are ordinary text elements; their x offset is
//! measured plot geometry, the documented `px` exception.

use gpui_kit::component::ActiveTheme as _;
use gpui_kit::{
    App, IntoElement, ParentElement as _, RenderOnce, SharedString, Styled as _, Window, div, px,
};

use omatrack_ui::TypeScale as _;

use crate::scale::{Tick, Viewport};

/// Minimum spacing of labelled axis ticks, logical pixels.
pub const AXIS_TICK_SPACING: f64 = 96.0;
/// Length of a major and a minor tick mark, logical pixels.
const MAJOR_TICK: f32 = 5.0;
const MINOR_TICK: f32 = 3.0;

/// One row of tick marks and labels spanning the plot width.
#[derive(IntoElement)]
pub struct TraceAxis {
    ticks: Vec<(f32, SharedString)>,
    minors: Vec<f32>,
}

impl TraceAxis {
    /// Place `ticks` for `viewport` across a plot `width` logical pixels wide.
    #[expect(
        clippy::cast_possible_truncation,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    pub fn new(ticks: &[Tick], viewport: &Viewport, width: f32) -> Self {
        let x_of = |fraction: f64| viewport.x_for_fraction(fraction, 0.0, f64::from(width)) as f32;
        let placed: Vec<(f32, SharedString)> = ticks
            .iter()
            .filter_map(|tick| {
                let x = x_of(tick.fraction);
                (x >= 0.0 && x < width - 24.0).then(|| (x, SharedString::from(tick.label.clone())))
            })
            .collect();
        let minors = ticks
            .windows(2)
            .map(|pair| x_of(0.5 * (pair[0].fraction + pair[1].fraction)))
            .filter(|x| (0.0..width).contains(x))
            .collect();
        Self {
            ticks: placed,
            minors,
        }
    }
}

impl RenderOnce for TraceAxis {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        let mark = theme.border;
        let minor = |x: f32| {
            div()
                .absolute()
                .top_0()
                .left(px(x.round()))
                .w(px(1.))
                .h(px(MINOR_TICK))
                .bg(mark)
        };
        div()
            .relative()
            .size_full()
            .overflow_hidden()
            .text_caption()
            .trace_numeric(cx)
            .text_color(theme.muted_foreground)
            .children(self.minors.into_iter().map(minor))
            .children(self.ticks.into_iter().map(|(x, label)| {
                div()
                    .absolute()
                    .top_0()
                    .left(px(x.round()))
                    .h_full()
                    .child(
                        div()
                            .absolute()
                            .top_0()
                            .left_0()
                            .w(px(1.))
                            .h(px(MAJOR_TICK))
                            .bg(mark),
                    )
                    .child(
                        div()
                            .absolute()
                            .top(px(MAJOR_TICK + 1.0))
                            .left(px(if x < 2.0 { -x } else { -2.0 }))
                            .whitespace_nowrap()
                            .child(label),
                    )
            }))
    }
}
