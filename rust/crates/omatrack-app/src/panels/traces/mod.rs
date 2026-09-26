//! Traces: the stacked, synchronized channel lanes.
//!
//! This wave owns the panel, its focus, its modal editing state and an
//! honest summary; the trace stack replaces the body.

use gpui_kit::component::{
    ActiveTheme as _, Sizable as _,
    button::{Button, ButtonVariants as _},
    h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    Context, FocusHandle, InteractiveElement as _, IntoElement, ParentElement as _, Render,
    SharedString, StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _,
    Window, div,
};
use omatrack_core::format_lap_time;

use crate::actions::{CancelEdit, SaveEdit};
use crate::keymap::TRACE_EDIT_CONTEXT;
use crate::panels::{PanelKind, analysis_body, simple_panel};
use crate::state::AppState;

/// The modal editors of the trace workspace. They are mutually exclusive;
/// Ctrl+S saves and Escape cancels.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum TraceMode {
    #[default]
    Browse,
    ResizingLanes,
    EditingCorners,
}

impl TraceMode {
    fn label(self) -> &'static str {
        match self {
            Self::Browse => "",
            Self::ResizingLanes => "Resizing lanes",
            Self::EditingCorners => "Editing corners",
        }
    }
}

pub struct TracesPanel {
    app: AppState,
    focus_handle: FocusHandle,
    mode: TraceMode,
    _subscriptions: Vec<Subscription>,
}

impl TracesPanel {
    pub fn new(app: AppState, cx: &mut Context<Self>) -> Self {
        let subscriptions = vec![
            cx.observe(&app.session, |_, _, cx| cx.notify()),
            cx.observe(&app.viewport, |_, _, cx| cx.notify()),
            cx.observe(&app.preferences, |_, _, cx| cx.notify()),
        ];
        Self {
            app,
            focus_handle: cx.focus_handle().tab_stop(true),
            mode: TraceMode::default(),
            _subscriptions: subscriptions,
        }
    }

    pub fn mode(&self) -> TraceMode {
        self.mode
    }

    /// Enter `mode`, or leave it when it is the current one (cancelling).
    pub fn toggle_mode(&mut self, mode: TraceMode, cx: &mut Context<Self>) {
        self.mode = if self.mode == mode {
            TraceMode::Browse
        } else {
            mode
        };
        cx.notify();
    }

    fn save(&mut self, _: &SaveEdit, _: &mut Window, cx: &mut Context<Self>) {
        self.mode = TraceMode::Browse;
        cx.notify();
    }

    fn cancel(&mut self, _: &CancelEdit, _: &mut Window, cx: &mut Context<Self>) {
        self.mode = TraceMode::Browse;
        cx.notify();
    }

    fn render_mode_bar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let label = self.mode.label();
        h_flex()
            .id("trace-mode-bar")
            .test_support()
            .aria_label(SharedString::from(label))
            .w_full()
            .gap_2()
            .px_3()
            .py_1()
            .border_b_1()
            .border_color(cx.theme().border)
            .bg(cx.theme().muted)
            .text_sm()
            .child(div().flex_1().child(label))
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
                    .tooltip_with_action("Save", &SaveEdit, Some(TRACE_EDIT_CONTEXT))
                    .on_click(cx.listener(|this, _, window, cx| this.save(&SaveEdit, window, cx))),
            )
    }
}

simple_panel!(TracesPanel, PanelKind::Traces);

impl Render for TracesPanel {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let editing = self.mode != TraceMode::Browse;
        let axis = match self.app.viewport.read(cx).axis() {
            omatrack_trace::XAxis::Distance => "distance",
            omatrack_trace::XAxis::Time => "time",
        };
        let fit = self
            .app
            .preferences
            .read(cx)
            .config()
            .trace
            .is_fitting_channels();
        let body = analysis_body("traces-summary", &self.app, cx, move |analysis| {
            let primary = analysis.primary();
            let lap = format!(
                "L{} {}",
                primary.lap_id(),
                format_lap_time(primary.lap().time_ms)
            );
            let comparison = match analysis.reference() {
                Some(reference) => {
                    let total = analysis.delta().last().copied();
                    format!(
                        " against L{} {} · Δ {} s",
                        reference.lap_id(),
                        format_lap_time(reference.lap().time_ms),
                        omatrack_ui::format_delta(total, 3, omatrack_ui::DeltaSense::LowerIsBetter)
                            .0
                    )
                }
                None => String::new(),
            };
            format!(
                "{lap}{comparison} · {} corners · by {axis}{}",
                analysis.corners().len(),
                if fit { " · lanes fit" } else { "" }
            )
            .into()
        });
        v_flex()
            .id("traces-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .when(editing, |this| {
                this.key_context(TRACE_EDIT_CONTEXT)
                    .on_action(cx.listener(Self::save))
                    .on_action(cx.listener(Self::cancel))
            })
            .size_full()
            .when(editing, |this| this.child(self.render_mode_bar(cx)))
            .child(div().flex_1().min_h_0().child(body))
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name) and, when it has them, its own actions and key
/// bindings. Called once from [`crate::panels::init`].
pub fn init(cx: &mut gpui_kit::App) {
    crate::panels::register(PanelKind::Traces, cx);
}
