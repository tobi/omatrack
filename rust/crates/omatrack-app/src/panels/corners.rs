//! Corners: where the lap gains and loses time, corner by corner.
//!
//! This wave shows an honest summary; the corner table replaces the body.

use gpui_kit::{
    Context, FocusHandle, InteractiveElement as _, IntoElement, ParentElement as _, Render,
    Styled as _, Subscription, TestSupportExt as _, Window,
};
use omatrack_core::session::Analysis;

use crate::panels::{PanelKind, analysis_body, simple_panel};
use crate::state::AppState;

pub struct CornersPanel {
    app: AppState,
    focus_handle: FocusHandle,
    _subscriptions: Vec<Subscription>,
}

impl CornersPanel {
    pub fn new(app: AppState, cx: &mut Context<Self>) -> Self {
        let subscriptions = vec![cx.observe(&app.session, |_, _, cx| cx.notify())];
        Self {
            app,
            focus_handle: cx.focus_handle(),
            _subscriptions: subscriptions,
        }
    }
}

/// `14 corners · most time lost at T5 (+0.231 s)`.
pub fn corners_summary(analysis: &Analysis) -> String {
    let rows = analysis.rows();
    if rows.is_empty() {
        return "No corners for this lap".to_string();
    }
    let count = format!(
        "{} corner{}",
        rows.len(),
        if rows.len() == 1 { "" } else { "s" }
    );
    let worst = rows
        .iter()
        .filter(|row| row.dt.is_finite())
        .max_by(|a, b| a.dt.total_cmp(&b.dt));
    match (analysis.reference(), worst) {
        (Some(_), Some(row)) if row.dt > 0.0 => format!(
            "{count} · most time lost at {} ({})",
            row.zone.name,
            omatrack_ui::format_delta(Some(row.dt), 3, omatrack_ui::DeltaSense::LowerIsBetter).0
        ),
        _ => count,
    }
}

simple_panel!(CornersPanel, PanelKind::Corners);

impl Render for CornersPanel {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        gpui_kit::div()
            .id("corners-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full()
            .child(analysis_body(
                "corners-summary",
                &self.app,
                cx,
                |analysis| corners_summary(analysis).into(),
            ))
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name) and, when it has them, its own actions and key
/// bindings. Called once from [`crate::panels::init`].
pub fn init(cx: &mut gpui_kit::App) {
    crate::panels::register(PanelKind::Corners, cx);
}
