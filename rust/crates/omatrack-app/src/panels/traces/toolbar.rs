//! The chrome of the trace workspace that is not the stack: the trace
//! toolbar, the statistics chip of a range selection, the modal editors'
//! bar, and the corner row above the lanes. (The laps are in the workspace
//! filmstrip, above every panel; playback and the Distance | Time axis are
//! in the control row under the video.)

use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, IconName, Selectable as _, Sizable as _, StyledExt as _,
    button::{Button, ButtonGroup, ButtonVariants as _},
    h_flex,
    menu::DropdownMenu as _,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    Action, Anchor, App, Context, InteractiveElement as _, IntoElement, ParentElement as _, Role,
    SharedString, StatefulInteractiveElement as _, Styled as _, TestSupportExt as _, Window, div,
};
use omatrack_core::session::CornerSource;
use omatrack_library::config::{TraceColorMode, TraceViewMode};
use omatrack_ui::TypeScale as _;

use super::{ToggleLane, TraceMode, TracesPanel, scene_build};
use crate::actions::{
    CancelEdit, ResizeLanes, SaveEdit, ShowChannels, ToggleCornerEdit, ToggleFit,
    ToggleTraceColorMode, ViewConsistency, ViewCorners, ViewEvents, ViewLap, ZoomIn, ZoomOut,
    ZoomReset,
};
use crate::keymap::{TRACE_EDIT_CONTEXT, WORKSPACE_CONTEXT};

/// A view mode's segment label, tooltip and action.
fn view_mode_segment(mode: TraceViewMode) -> (&'static str, &'static str, Box<dyn Action>) {
    match mode {
        TraceViewMode::Lap => ("Lap", "The whole lap", Box::new(ViewLap)),
        TraceViewMode::Corners => (
            "Corners",
            "One corner at a time, with its approach and exit (h / j step)",
            Box::new(ViewCorners),
        ),
        TraceViewMode::Consistency => (
            "Consistency",
            "The session’s timed laps behind the primary, with their spread",
            Box::new(ViewConsistency),
        ),
        TraceViewMode::Events => (
            "Events",
            "Brake points, shifts, lifts and corner notes on the traces",
            Box::new(ViewEvents),
        ),
    }
}

impl TracesPanel {
    /// Why the Consistency view shows no session behind the lap (too few
    /// timed laps, or still loading), floating at the top left of the lanes
    /// clear of the legends: a degraded view says so, never draws nothing
    /// silently.
    pub(super) fn render_consistency_notice(
        &self,
        cx: &mut Context<Self>,
    ) -> Option<impl IntoElement> {
        let notice = self.consistency_notice(cx)?;
        let theme = cx.theme();
        Some(
            div()
                .id("trace-consistency-notice")
                .role(Role::Status)
                .aria_label(notice.clone())
                .test_support()
                .absolute()
                .top_1()
                .left(gpui_kit::rems(omatrack_trace::CHROME_REMS + 0.5))
                .max_w_1_2()
                .px_2()
                .py_0p5()
                .rounded(theme.radius)
                .border_1()
                .border_color(theme.border)
                .bg(theme.popover)
                .text_caption()
                .text_color(theme.muted_foreground)
                .truncate()
                .child(notice),
        )
    }

    /// The trace toolbar at the top of the trace area: zoom out, zoom in
    /// and fit; the view mode (`Lap | Corners | Consistency | Events`);
    /// the Channels menu (lane visibility); the colour mode; and, right
    /// aligned, the less frequent lane tools (fit lanes to the height,
    /// resize, edit corners). Every control dispatches the action of its
    /// key and names that key in its tooltip.
    pub(super) fn render_toolbar(
        &self,
        window: &Window,
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let config = self.app.preferences.read(cx).config();
        let view_mode = self.app.trace_view.read(cx).mode();
        let channel_colours = config.trace.color_mode() == TraceColorMode::Channel;
        let keys = self.focus_handle.clone();

        let fit_tooltip = if view_mode == TraceViewMode::Corners {
            "Fit the corner"
        } else {
            "Whole lap"
        };
        let zoom_keys = keys.clone();
        let zoom = ButtonGroup::new("trace-zoom")
            .small()
            .outline()
            .child(
                Button::new("trace-zoom-out")
                    .icon(self.icons.zoom_out.clone())
                    .accessibility_label("Zoom out")
                    .tooltip_with_action("Zoom out", &ZoomOut, Some(WORKSPACE_CONTEXT)),
            )
            .child(
                Button::new("trace-zoom-in")
                    .icon(self.icons.zoom_in.clone())
                    .accessibility_label("Zoom in")
                    .tooltip_with_action("Zoom in", &ZoomIn, Some(WORKSPACE_CONTEXT)),
            )
            .child(
                Button::new("trace-zoom-fit")
                    .icon(self.icons.fit.clone())
                    .accessibility_label(fit_tooltip)
                    .tooltip_with_action(fit_tooltip, &ZoomReset, Some(WORKSPACE_CONTEXT)),
            )
            .on_click(move |clicked: &Vec<usize>, window, cx| {
                let action: Box<dyn Action> = match clicked.first() {
                    Some(0) => Box::new(ZoomOut),
                    Some(1) => Box::new(ZoomIn),
                    Some(2) => Box::new(ZoomReset),
                    _ => return,
                };
                zoom_keys.dispatch_action(action.as_ref(), window, cx);
            });

        let mode_keys = keys.clone();
        let modes = ButtonGroup::new("trace-view-mode")
            .small()
            .outline()
            .children(TraceViewMode::ALL.map(|mode| {
                let (label, tooltip, action) = view_mode_segment(mode);
                Button::new(SharedString::from(format!(
                    "trace-view-{}",
                    label.to_ascii_lowercase()
                )))
                .label(label)
                .selected(mode == view_mode)
                .toggled(mode == view_mode)
                .tooltip_with_action(
                    tooltip,
                    action.as_ref(),
                    Some(WORKSPACE_CONTEXT),
                )
            }))
            .on_click(move |clicked: &Vec<usize>, window, cx| {
                let Some(mode) = clicked.first().and_then(|ix| TraceViewMode::ALL.get(*ix)) else {
                    return;
                };
                let (_, _, action) = view_mode_segment(*mode);
                mode_keys.dispatch_action(action.as_ref(), window, cx);
            });

        let lanes: Vec<(SharedString, SharedString)> = self
            .scene
            .lanes()
            .iter()
            .map(|lane| (lane.key.clone(), lane.title.clone()))
            .collect();
        let preferences = self.app.preferences.clone();
        let menu_focus = keys.clone();
        let menu_height = window.rem_size() * 24.;
        let channels = Button::new("trace-channels")
            .small()
            .outline()
            .icon(IconName::Menu)
            .label("Channels")
            .accessibility_label("Channels: show or hide lanes")
            .tooltip("Show or hide lanes")
            .dropdown_menu(move |menu, _, cx| {
                let config = preferences.read(cx).config();
                let mut menu = menu
                    .action_context(menu_focus.clone())
                    .max_h(menu_height)
                    .scrollable(true);
                for (key, title) in &lanes {
                    menu = menu.menu_with_check(
                        title.clone(),
                        scene_build::is_lane_visible(config, key),
                        Box::new(ToggleLane { key: key.clone() }),
                    );
                }
                menu.separator()
                    .menu("Channel settings…", Box::new(ShowChannels))
            });

        let colour_keys = keys.clone();
        let colours = Button::new("trace-color-mode")
            .small()
            .outline()
            .icon(IconName::Palette)
            .label("Channel colours")
            .selected(channel_colours)
            .toggled(channel_colours)
            .accessibility_label("Channel colours")
            .tooltip_with_action(
                if channel_colours {
                    "Each channel in its own hue; off shows lap colours"
                } else {
                    "Lap colours; on draws each channel in its own hue"
                },
                &ToggleTraceColorMode,
                Some(WORKSPACE_CONTEXT),
            )
            .on_click(move |_, window, cx| {
                colour_keys.dispatch_action(&ToggleTraceColorMode, window, cx)
            });

        let tools_preferences = self.app.preferences.clone();
        let tools_focus = keys;
        let tools = Button::new("trace-tools")
            .ghost()
            .small()
            .icon(IconName::Ellipsis)
            .accessibility_label("Lane tools")
            .tooltip("Lane tools")
            .dropdown_menu_with_anchor(Anchor::TopRight, move |menu, _, cx| {
                let fit = tools_preferences
                    .read(cx)
                    .config()
                    .trace
                    .is_fitting_channels();
                menu.action_context(tools_focus.clone())
                    .menu_with_check("Fit lanes to the height", fit, Box::new(ToggleFit))
                    .menu("Resize lanes…", Box::new(ResizeLanes))
                    .menu("Edit corners…", Box::new(ToggleCornerEdit))
            });

        h_flex()
            .id("trace-toolbar")
            .role(Role::Toolbar)
            .aria_label("Traces")
            .test_support()
            .w_full()
            .flex_shrink_0()
            .items_center()
            .gap_2()
            .px_2()
            .py_1p5()
            .overflow_hidden()
            .text_label()
            .child(div().flex_shrink_0().child(zoom))
            .child(div().flex_shrink_0().child(modes))
            .child(div().flex_shrink_0().child(channels))
            .child(div().flex_shrink_0().child(colours))
            .child(div().flex_1())
            .child(div().flex_shrink_0().child(tools))
    }

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
            // Atlas corners placed through the reference lap; the Sync
            // menu says why.
            Some(CornerSource::Reference) => "Corners · Track Atlas".into(),
            Some(CornerSource::Unmatched) => "Corners · GPS off the map".into(),
            None => "Corners".into(),
        };
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
                ),
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
                .px_3()
                .child(label),
        )
        .child(div().flex_1().min_w_0().child(content))
}
