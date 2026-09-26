//! The trace toolbar, the modal editors' bar, and the corner row above the
//! lanes. (The laps are in the workspace filmstrip, above every panel.)
//!
//! The toolbar is kit button groups only (x-axis, lane sizing, corner
//! editing, zoom), all the same size and variant. Every control dispatches
//! the same workspace action as its key (from this panel's focus handle, so
//! it runs outside the panel's own update), and shows that key in its
//! tooltip.

use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, IconName, Selectable as _, Sizable as _, StyledExt as _,
    button::{Button, ButtonGroup, ButtonVariants as _},
    h_flex,
    separator::Separator,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    Action, App, ClickEvent, Context, FocusHandle, InteractiveElement as _, IntoElement,
    ParentElement as _, Role, SharedString, StatefulInteractiveElement as _, Styled as _,
    TestSupportExt as _, Window, div,
};
use omatrack_core::session::CornerSource;
use omatrack_trace::XAxis;
use omatrack_ui::TypeScale as _;

use super::{TraceMode, TracesPanel};
use crate::actions::{
    CancelEdit, ResizeLanes, SaveEdit, ToggleCornerEdit, ToggleFit, ToggleXAxis, ZoomIn, ZoomOut,
    ZoomReset,
};
use crate::keymap::{TRACE_EDIT_CONTEXT, WORKSPACE_CONTEXT};

/// A click handler that dispatches `action` from `keys` (this panel).
fn dispatch(
    keys: &FocusHandle,
    action: impl Action,
) -> impl Fn(&ClickEvent, &mut Window, &mut App) + 'static {
    let keys = keys.clone();
    move |_, window, cx| keys.dispatch_action(&action, window, cx)
}

impl TracesPanel {
    pub(super) fn render_toolbar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let has_data = !self.scene.is_empty();
        let fit = self
            .app
            .preferences
            .read(cx)
            .config()
            .trace
            .is_fitting_channels();
        let keys = &self.focus_handle;
        let axis = self.axis;
        let axis_keys = keys.clone();
        let range = self.range.map(|range| SharedString::from(range.summary()));

        h_flex()
            .id("trace-toolbar")
            .role(Role::Toolbar)
            .aria_label("Trace tools")
            .test_support()
            .w_full()
            .flex_shrink_0()
            .gap_1()
            .px_2()
            .py_1()
            .border_b_1()
            .border_color(theme.border)
            .child(
                ButtonGroup::new("trace-axis")
                    .xsmall()
                    .outline()
                    .disabled(!has_data)
                    .child(
                        Button::new("trace-axis-distance")
                            .label("Distance")
                            .selected(axis == XAxis::Distance)
                            .when(axis == XAxis::Distance, |button| button.primary())
                            .tooltip_with_action(
                                "X-axis by distance",
                                &ToggleXAxis,
                                Some(WORKSPACE_CONTEXT),
                            ),
                    )
                    .child(
                        Button::new("trace-axis-time")
                            .label("Time")
                            .selected(axis == XAxis::Time)
                            .when(axis == XAxis::Time, |button| button.primary())
                            .tooltip_with_action(
                                "X-axis by time",
                                &ToggleXAxis,
                                Some(WORKSPACE_CONTEXT),
                            ),
                    )
                    .on_click(move |clicked, window, cx| {
                        let wanted = if clicked.contains(&0) {
                            XAxis::Distance
                        } else {
                            XAxis::Time
                        };
                        if wanted != axis {
                            axis_keys.dispatch_action(&ToggleXAxis, window, cx);
                        }
                    }),
            )
            .child(
                ButtonGroup::new("trace-lanes")
                    .xsmall()
                    .outline()
                    .disabled(!has_data)
                    .child(
                        Button::new("trace-fit")
                            .label("Fit")
                            .selected(fit)
                            .when(fit, |button| button.primary())
                            .tooltip_with_action(
                                "Fit every lane to the workspace height",
                                &ToggleFit,
                                Some(WORKSPACE_CONTEXT),
                            )
                            .on_click(dispatch(keys, ToggleFit)),
                    )
                    .child(
                        Button::new("trace-resize")
                            .label("Resize")
                            .selected(self.mode == TraceMode::ResizingLanes)
                            .when(self.mode == TraceMode::ResizingLanes, |button| {
                                button.primary()
                            })
                            .tooltip_with_action(
                                "Resize lanes",
                                &ResizeLanes,
                                Some(WORKSPACE_CONTEXT),
                            )
                            .on_click(dispatch(keys, ResizeLanes)),
                    ),
            )
            .child(
                ButtonGroup::new("trace-corners")
                    .xsmall()
                    .outline()
                    .disabled(!has_data)
                    .child(
                        Button::new("trace-edit-corners")
                            .label("Edit corners")
                            .selected(self.mode == TraceMode::EditingCorners)
                            .when(self.mode == TraceMode::EditingCorners, |button| {
                                button.primary()
                            })
                            .tooltip_with_action(
                                "Edit corner zones",
                                &ToggleCornerEdit,
                                Some(WORKSPACE_CONTEXT),
                            )
                            .on_click(dispatch(keys, ToggleCornerEdit)),
                    ),
            )
            .child(Separator::vertical().h_4())
            .child(
                ButtonGroup::new("trace-zoom")
                    .xsmall()
                    .outline()
                    .disabled(!has_data)
                    .child(
                        Button::new("trace-zoom-out")
                            .icon(IconName::Minus)
                            .accessibility_label("Zoom out")
                            .tooltip_with_action("Zoom out", &ZoomOut, Some(WORKSPACE_CONTEXT))
                            .on_click(dispatch(keys, ZoomOut)),
                    )
                    .child(
                        Button::new("trace-zoom-reset")
                            .icon(IconName::Maximize)
                            .accessibility_label("Whole lap")
                            .tooltip_with_action("Whole lap", &ZoomReset, Some(WORKSPACE_CONTEXT))
                            .on_click(dispatch(keys, ZoomReset)),
                    )
                    .child(
                        Button::new("trace-zoom-in")
                            .icon(IconName::Plus)
                            .accessibility_label("Zoom in")
                            .tooltip_with_action("Zoom in", &ZoomIn, Some(WORKSPACE_CONTEXT))
                            .on_click(dispatch(keys, ZoomIn)),
                    ),
            )
            .child(div().flex_1().min_w_0())
            .when_some(range, |el, summary| {
                el.child(
                    h_flex()
                        .id("trace-range-stats")
                        .role(Role::Status)
                        .aria_label(SharedString::from(format!("Selection: {summary}")))
                        .test_support()
                        .min_w_0()
                        .gap_1()
                        .pl_2()
                        .rounded(theme.radius)
                        .bg(theme.muted)
                        .child(
                            div()
                                .min_w_0()
                                .truncate()
                                .text_xs()
                                .numeric()
                                .text_color(theme.foreground)
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
            })
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
