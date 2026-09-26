//! The chrome of the trace workspace that is not the stack: the statistics
//! chip of a range selection, the modal editors' bar, and the corner row
//! above the lanes. (The laps are in the workspace filmstrip, above every
//! panel; the axis, fit, sizing, corner editing and zoom controls are in the
//! control row under the video and on their keys.)

use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, IconName, Sizable as _, StyledExt as _,
    button::{Button, ButtonVariants as _},
    h_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, Context, InteractiveElement as _, IntoElement, ParentElement as _, Role, SharedString,
    StatefulInteractiveElement as _, Styled as _, TestSupportExt as _, div,
};
use omatrack_core::session::CornerSource;
use omatrack_ui::TypeScale as _;

use super::{TraceMode, TracesPanel};
use crate::actions::{CancelEdit, SaveEdit};
use crate::keymap::TRACE_EDIT_CONTEXT;

impl TracesPanel {
    /// The statistics of the range selection, floating at the top right of
    /// the lanes while a range is selected, with its clear button.
    pub(super) fn render_range_stats(&self, cx: &mut Context<Self>) -> Option<impl IntoElement> {
        let summary = SharedString::from(self.range?.summary());
        let theme = cx.theme();
        Some(
            h_flex()
                .id("trace-range-stats")
                .role(Role::Status)
                .aria_label(SharedString::from(format!("Selection: {summary}")))
                .test_support()
                .absolute()
                .top_1()
                .right_2()
                .max_w_2_3()
                .min_w_0()
                .gap_1()
                .pl_2()
                .rounded(theme.radius)
                .border_1()
                .border_color(theme.border)
                .bg(theme.popover)
                .shadow_sm()
                .child(
                    div()
                        .min_w_0()
                        .truncate()
                        .text_caption()
                        .numeric()
                        .text_color(theme.popover_foreground)
                        .child(summary),
                )
                .child(
                    Button::new("trace-range-clear")
                        .ghost()
                        .xsmall()
                        .icon(IconName::Close)
                        .accessibility_label("Clear selection")
                        .tooltip("Clear selection")
                        .on_click(cx.listener(|this, _, _, cx| this.clear_selection(cx))),
                ),
        )
    }

    /// The bar of the active editor: what it does, and its commands.
    pub(super) fn render_mode_bar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let label = self.mode.label();
        let (hint, can_save): (SharedString, bool) = match self.mode {
            TraceMode::ResizingLanes => (
                "Drag a divider; lanes borrow from their neighbours. Save keeps the proportions."
                    .into(),
                true,
            ),
            TraceMode::EditingCorners if !self.has_track(cx) => (
                "This recording names no track, so corner zones can’t be saved.".into(),
                false,
            ),
            TraceMode::EditingCorners => (
                "Drag a zone edge or body in the ruler or the lanes.".into(),
                self.can_save_corners(cx),
            ),
            TraceMode::Browse => (SharedString::default(), false),
        };
        let has_override = self
            .analysis()
            .is_some_and(|analysis| analysis.has_corner_override());
        h_flex()
            .id("trace-mode-bar")
            .role(Role::Toolbar)
            .test_support()
            .aria_label(SharedString::from(label))
            .w_full()
            .flex_shrink_0()
            .gap_2()
            .px_2()
            .py_1()
            .border_b_1()
            .border_color(theme.border)
            .bg(theme.muted)
            .text_sm()
            .child(div().font_medium().child(label))
            .child(
                div()
                    .flex_1()
                    .min_w_0()
                    .truncate()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child(hint),
            )
            .when(self.mode == TraceMode::ResizingLanes, |el| {
                el.child(
                    Button::new("trace-reset-heights")
                        .ghost()
                        .small()
                        .label("Reset heights")
                        .on_click(cx.listener(|this, _, _, cx| this.reset_heights(cx))),
                )
            })
            .when(
                self.mode == TraceMode::EditingCorners && has_override,
                |el| {
                    el.child(
                        Button::new("trace-atlas-corners")
                            .ghost()
                            .small()
                            .label("Use Track Atlas zones")
                            .on_click(cx.listener(|this, _, _, cx| this.use_atlas_corners(cx))),
                    )
                },
            )
            .child(
                Button::new("trace-mode-cancel")
                    .ghost()
                    .small()
                    .label("Cancel")
                    .tooltip_with_action("Cancel", &CancelEdit, Some(TRACE_EDIT_CONTEXT))
                    .on_click(
                        cx.listener(|this, _, window, cx| this.cancel(&CancelEdit, window, cx)),
                    ),
            )
            .child(
                Button::new("trace-mode-save")
                    .primary()
                    .small()
                    .label("Save")
                    .disabled(!can_save)
                    .tooltip_with_action("Save", &SaveEdit, Some(TRACE_EDIT_CONTEXT))
                    .on_click(cx.listener(|this, _, window, cx| this.save(&SaveEdit, window, cx))),
            )
    }

    /// Corner zones and complexes on the shared x mapping, labelled with
    /// where the zones come from.
    pub(super) fn render_ruler_row(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let source = self.analysis().map(|analysis| analysis.corner_source());
        let label: SharedString = match source {
            Some(CornerSource::Atlas) => "Corners · Track Atlas".into(),
            Some(CornerSource::User) => "Corners · edited".into(),
            Some(CornerSource::Generated) => "Corners · from braking".into(),
            Some(CornerSource::Reference) => "Corners · via reference".into(),
            Some(CornerSource::Unmatched) => "Corners · GPS off the map".into(),
            None => "Corners".into(),
        };
        // The readout columns' header sits here, at the top of the lanes'
        // chrome, on the same spines as every lane's values.
        let key = self
            .stack
            .as_ref()
            .map(|stack| stack.read(cx).column_key(cx));
        row(
            "trace-ruler-row",
            gpui_kit::component::v_flex()
                .w_full()
                .min_w_0()
                .gap_0p5()
                .child(
                    div()
                        .text_caption()
                        .text_color(theme.muted_foreground)
                        .truncate()
                        .child(label),
                )
                .children(key),
            self.ruler.clone(),
            cx,
        )
    }
}

/// A row on the lanes' alignment spine: a label in the chrome column (as
/// wide as the lane chrome) and the content in the plot column.
fn row(
    id: &'static str,
    label: impl IntoElement,
    content: impl IntoElement,
    cx: &App,
) -> impl IntoElement {
    let theme = cx.theme();
    h_flex()
        .id(id)
        .test_support()
        .w_full()
        .flex_shrink_0()
        .items_stretch()
        .border_b_1()
        .border_color(theme.border)
        .child(
            div()
                .w(gpui_kit::rems(omatrack_trace::CHROME_REMS))
                .flex_shrink_0()
                .flex()
                .items_center()
                .px_2()
                .border_r_1()
                .border_color(theme.border)
                .child(label),
        )
        .child(div().flex_1().min_w_0().child(content))
}
