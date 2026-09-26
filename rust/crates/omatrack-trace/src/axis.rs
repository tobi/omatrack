//! The shared x-axis row under the lanes.
//!
//! Tick positions come from [`crate::scale::axis_ticks`] with the same
//! viewport and width the static layer uses for its vertical grid, so labels
//! and grid lines coincide. Labels are ordinary text elements (tabular
//! numbers in the monospace family); their x offset is measured plot
//! geometry, the documented `px` exception.

use gpui_kit::component::ActiveTheme as _;
use gpui_kit::{
    App, IntoElement, ParentElement as _, RenderOnce, SharedString, Styled as _, Window, div,
    prelude::FluentBuilder as _, px,
};

use omatrack_ui::TypeScale as _;

use crate::scale::{Tick, Viewport};

/// One row of tick labels spanning the plot width.
#[derive(IntoElement)]
pub struct TraceAxis {
    ticks: Vec<(f32, SharedString)>,
}

impl TraceAxis {
    /// Place `ticks` for `viewport` across a plot `width` logical pixels wide.
    #[expect(
        clippy::cast_possible_truncation,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    pub fn new(ticks: &[Tick], viewport: &Viewport, width: f32) -> Self {
        let ticks = ticks
            .iter()
            .filter_map(|tick| {
                let x = viewport.x_for_fraction(tick.fraction, 0.0, f64::from(width)) as f32;
                (x >= 0.0 && x < width - 24.0).then(|| (x, SharedString::from(tick.label.clone())))
            })
            .collect();
        Self { ticks }
    }
}

impl RenderOnce for TraceAxis {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        div()
            .relative()
            .size_full()
            .overflow_hidden()
            .text_caption()
            .numeric()
            .text_color(theme.muted_foreground)
            .children(self.ticks.into_iter().map(|(x, label)| {
                div()
                    .absolute()
                    .top_0()
                    .left(px(x))
                    .h_full()
                    .flex()
                    .items_center()
                    .when(x > 0.0, gpui_kit::Styled::pl_1)
                    .child(label)
            }))
    }
}
