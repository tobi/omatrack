//! The status bar: the cursor on the left, sync, jobs and theme on the
//! right.
//!
//! It is its own entity so a cursor move re-renders only this bar (and the
//! other cursor observers), never the workspace or the static traces.

use gpui_kit::component::{
    ActiveTheme as _, Sizable as _, Theme, h_flex, spinner::Spinner, status_bar::StatusBar,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AnyElement, App, Context, InteractiveElement as _, IntoElement, ParentElement as _, Render,
    Role, SharedString, StatefulInteractiveElement as _, Styled as _, Subscription,
    TestSupportExt as _, Window, div,
};
use omatrack_core::format_lap_time;
use omatrack_core::session::Analysis;
use omatrack_trace::scale::value_at_fraction;
use omatrack_ui::theme::ThemeStatus;
use omatrack_ui::{DeltaSense, DeltaText};

use crate::state::AppState;

pub struct StatusView {
    app: AppState,
    _subscriptions: Vec<Subscription>,
}

impl StatusView {
    pub fn new(app: AppState, cx: &mut Context<Self>) -> Self {
        let subscriptions = vec![
            cx.observe(&app.cursor, |_, _, cx| cx.notify()),
            cx.observe(&app.session, |_, _, cx| cx.notify()),
            cx.observe(&app.jobs, |_, _, cx| cx.notify()),
            cx.observe_global::<Theme>(|_, cx| cx.notify()),
            cx.observe_global::<ThemeStatus>(|_, cx| cx.notify()),
        ];
        Self {
            app,
            _subscriptions: subscriptions,
        }
    }
}

/// `1,234`.
fn thousands(value: f64) -> String {
    let digits = format!("{:.0}", value.abs());
    let mut out = String::new();
    for (ix, ch) in digits.chars().enumerate() {
        if ix > 0 && (digits.len() - ix) % 3 == 0 {
            out.push(',');
        }
        out.push(ch);
    }
    if value < 0.0 { format!("-{out}") } else { out }
}

/// `1,234 m · 0:42.310` at a primary lap fraction.
pub fn cursor_text(analysis: &Analysis, fraction: f64) -> SharedString {
    let lap = analysis.primary().unified();
    let distance = value_at_fraction(&lap.distance, fraction);
    let time = value_at_fraction(&lap.time, fraction);
    format!(
        "{} m · {}",
        thousands(distance),
        format_lap_time(time * 1000.0)
    )
    .into()
}

fn lowest(values: &[f64], from: f64, to: f64) -> Option<f64> {
    if values.len() < 2 {
        return None;
    }
    let last = (values.len() - 1) as f64;
    let (a, b) = (
        (from.clamp(0.0, 1.0) * last).floor() as usize,
        (to.clamp(0.0, 1.0) * last).ceil() as usize,
    );
    values[a.min(b)..=b.max(a)]
        .iter()
        .copied()
        .filter(|v| v.is_finite())
        .min_by(f64::total_cmp)
}

fn item(id: &'static str, label: SharedString, content: impl IntoElement) -> AnyElement {
    div()
        .id(id)
        .role(Role::Status)
        .test_support()
        .aria_label(label)
        .child(content)
        .into_any_element()
}

impl StatusView {
    fn render_cursor(&self, cx: &App) -> Vec<AnyElement> {
        let cursor = self.app.cursor.read(cx);
        let session = self.app.session.read(cx);
        let Some(analysis) = session.analysis() else {
            return vec![item(
                "status-cursor",
                "No lap loaded".into(),
                div()
                    .text_color(cx.theme().muted_foreground)
                    .child("No lap loaded"),
            )];
        };
        let fraction = cursor.readout_fraction().unwrap_or(0.0);
        let mono = cx.theme().mono_font_family.clone();
        let text = cursor_text(analysis, fraction);
        let mut items = vec![item(
            "status-cursor",
            text.clone(),
            div().font_family(mono.clone()).child(text),
        )];
        if let Some(comparison) = analysis.comparison() {
            let delta = comparison.time_delta_at(fraction);
            let finite = delta.is_finite().then_some(delta);
            let (label, _) = omatrack_ui::format_delta(finite, 3, DeltaSense::LowerIsBetter);
            items.push(item(
                "status-delta",
                format!("Delta {label} s").into(),
                h_flex()
                    .gap_1()
                    .child("Δ")
                    .child(DeltaText::new(finite).unit("s")),
            ));
            if let Some(selection) = cursor.selection() {
                let range = comparison.time_delta_at(selection.end)
                    - comparison.time_delta_at(selection.start);
                let finite = range.is_finite().then_some(range);
                let primary_min = lowest(
                    &analysis.primary().unified().speed,
                    selection.start,
                    selection.end,
                );
                let reference_min = analysis.reference().and_then(|reference| {
                    lowest(
                        &reference.unified().speed,
                        comparison.compare_fraction_for_primary_fraction(selection.start),
                        comparison.compare_fraction_for_primary_fraction(selection.end),
                    )
                });
                let speeds = format!(
                    "min {} / {} km/h",
                    primary_min.map_or("—".to_string(), |v| format!("{v:.0}")),
                    reference_min.map_or("—".to_string(), |v| format!("{v:.0}")),
                );
                let (label, _) = omatrack_ui::format_delta(finite, 3, DeltaSense::LowerIsBetter);
                items.push(item(
                    "status-selection",
                    format!("Range delta {label} s, {speeds}").into(),
                    h_flex()
                        .gap_1()
                        .child("Range Δ")
                        .child(DeltaText::new(finite).unit("s"))
                        .child(div().text_color(cx.theme().muted_foreground).child(speeds)),
                ));
            }
        }
        items
    }

    fn render_sync(&self, cx: &App) -> Option<AnyElement> {
        let session = self.app.session.read(cx);
        let comparison = session.analysis()?.comparison()?.clone();
        let anchors = comparison.alignment().gps_anchors;
        let text: SharedString = if anchors > 0 {
            format!(
                "{} · {anchors} anchors · {}",
                comparison.basis(),
                comparison.confidence()
            )
        } else {
            format!("{} · {}", comparison.basis(), comparison.confidence())
        }
        .into();
        Some(item(
            "status-sync",
            text.clone(),
            div().text_color(cx.theme().muted_foreground).child(text),
        ))
    }

    fn render_jobs(&self, cx: &App) -> Option<AnyElement> {
        let jobs = self.app.jobs.read(cx);
        let first = jobs.running().first()?;
        let text: SharedString = match (jobs.running().len(), first.progress()) {
            (1, Some((done, total))) if total > 0 => {
                format!("{} {done}/{total}", first.label()).into()
            }
            (1, _) => format!("{}…", first.label()).into(),
            (count, _) => format!("{count} jobs running").into(),
        };
        Some(item(
            "status-jobs",
            text.clone(),
            h_flex().gap_1().child(Spinner::new().xsmall()).child(text),
        ))
    }
}

impl Render for StatusView {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme_label = ThemeStatus::global(cx)
            .map(ThemeStatus::label)
            .unwrap_or_default();
        let mut bar = StatusBar::new().text_xs();
        for element in self.render_cursor(cx) {
            bar = bar.left(element);
        }
        bar.when_some(self.render_sync(cx), |bar, sync| bar.right(sync))
            .when_some(self.render_jobs(cx), |bar, jobs| bar.right(jobs))
            .right(
                div()
                    .id("theme-status")
                    .role(Role::Status)
                    .test_support()
                    .aria_label(theme_label.clone())
                    .text_color(cx.theme().muted_foreground)
                    .child(theme_label),
            )
    }
}
