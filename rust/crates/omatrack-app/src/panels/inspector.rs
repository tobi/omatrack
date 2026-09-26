//! Inspector: every channel's primary, reference and delta values at the
//! cursor.
//!
//! This wave shows an honest summary; the readout table replaces the body.

use gpui_kit::{
    Context, FocusHandle, InteractiveElement as _, IntoElement, ParentElement as _, Render,
    Styled as _, Subscription, TestSupportExt as _, Window,
};

use crate::panels::{PanelKind, analysis_body, simple_panel};
use crate::state::AppState;

pub struct InspectorPanel {
    app: AppState,
    focus_handle: FocusHandle,
    _subscriptions: Vec<Subscription>,
}

impl InspectorPanel {
    pub fn new(app: AppState, cx: &mut Context<Self>) -> Self {
        let subscriptions = vec![cx.observe(&app.session, |_, _, cx| cx.notify())];
        Self {
            app,
            focus_handle: cx.focus_handle(),
            _subscriptions: subscriptions,
        }
    }
}

simple_panel!(InspectorPanel, PanelKind::Inspector);

impl Render for InspectorPanel {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        gpui_kit::div()
            .id("inspector-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full()
            .child(analysis_body(
                "inspector-summary",
                &self.app,
                cx,
                |analysis| match analysis.comparison() {
                    Some(comparison) => format!(
                        "Comparing through {} · {} confidence",
                        comparison.basis(),
                        comparison.confidence()
                    )
                    .into(),
                    None => "Primary lap only · select a reference lap to compare".into(),
                },
            ))
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name) and, when it has them, its own actions and key
/// bindings. Called once from [`crate::panels::init`].
pub fn init(cx: &mut gpui_kit::App) {
    crate::panels::register(PanelKind::Inspector, cx);
}
