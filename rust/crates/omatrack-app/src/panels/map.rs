//! Map: the Track Atlas centerline with both laps' GPS traces.
//!
//! This wave shows an honest summary; the track map replaces the body.

use gpui_kit::{
    Context, FocusHandle, InteractiveElement as _, IntoElement, ParentElement as _, Render,
    Styled as _, Subscription, TestSupportExt as _, Window,
};

use crate::panels::{PanelKind, analysis_body, simple_panel};
use crate::state::AppState;

pub struct MapPanel {
    app: AppState,
    focus_handle: FocusHandle,
    _subscriptions: Vec<Subscription>,
}

impl MapPanel {
    pub fn new(app: AppState, cx: &mut Context<Self>) -> Self {
        let subscriptions = vec![cx.observe(&app.session, |_, _, cx| cx.notify())];
        Self {
            app,
            focus_handle: cx.focus_handle(),
            _subscriptions: subscriptions,
        }
    }
}

simple_panel!(MapPanel, PanelKind::Map);

impl Render for MapPanel {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        gpui_kit::div()
            .id("map-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full()
            .child(analysis_body(
                "map-summary",
                &self.app,
                cx,
                |analysis| match analysis.primary().layout() {
                    Some(layout) => format!(
                        "{} · {} · Track Atlas",
                        layout.track_name, layout.layout_name
                    )
                    .into(),
                    None => "No Track Atlas layout matches this lap".into(),
                },
            ))
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name) and, when it has them, its own actions and key
/// bindings. Called once from [`crate::panels::init`].
pub fn init(cx: &mut gpui_kit::App) {
    crate::panels::register(PanelKind::Map, cx);
}
