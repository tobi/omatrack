//! The status bar: the cursor on the left, sync, jobs and theme on the
//! right.
//!
//! It is its own entity so a cursor move re-renders only this bar (and the
//! other cursor observers), never the workspace or the static traces.

use gpui_kit::component::{
    ActiveTheme as _, Sizable as _, Theme, h_flex, separator::Separator, spinner::Spinner,
    status_bar::StatusBar, tooltip::Tooltip,
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
use omatrack_ui::theme::{ThemeFonts, ThemeStatus};
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

/// Whether deltas at `confidence` read as approximate.
pub fn approximate(confidence: &str) -> bool {
    matches!(confidence, "LOW" | "NONE")
}

/// `Δ`, or `Δ≈` for an approximate delta.
fn delta_mark(approximate: bool) -> &'static str {
    if approximate { "Δ≈" } else { "Δ" }
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
        let Some(fraction) = cursor.readout_fraction() else {
            return vec![item(
                "status-cursor",
                "No cursor".into(),
                div()
                    .text_color(cx.theme().muted_foreground)
                    .child("No cursor"),
            )];
        };
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
            // Under LOW sync confidence every delta is approximate (a
            // 12–15 m turn-in error is typical), and says so.
            let approximate = approximate(comparison.confidence());
            let spoken = if approximate {
                format!("Delta about {label} s")
            } else {
                format!("Delta {label} s")
            };
            items.push(item(
                "status-delta",
                spoken.into(),
                h_flex()
                    .gap_1()
                    .child(delta_mark(approximate))
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
                        .child("Range")
                        .child(delta_mark(approximate))
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
        let confidence = comparison.confidence();
        let text: SharedString = if anchors > 0 {
            format!("{} · {anchors} anchors", comparison.basis())
        } else {
            comparison.basis().to_owned()
        }
        .into();
        let spoken = super::header::sync_summary(comparison.basis(), anchors, confidence);
        Some(
            div()
                .id("status-sync")
                .role(Role::Status)
                .test_support()
                .aria_label(spoken.clone())
                .tooltip(move |window, cx| Tooltip::new(spoken.clone()).build(window, cx))
                .child(
                    h_flex()
                        .gap_1()
                        .child(div().text_color(cx.theme().muted_foreground).child(text))
                        .child(
                            div()
                                .when(approximate(confidence), |this| {
                                    this.text_color(cx.theme().warning)
                                })
                                .child(confidence),
                        ),
                )
                .into_any_element(),
        )
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
        let fonts = ThemeFonts::global(cx).cloned();
        let appearance: SharedString = match &fonts {
            Some(fonts) => format!("{theme_label} · {}", fonts.label()).into(),
            None => theme_label,
        };
        let separator = || Separator::vertical().h_3().into_any_element();
        let mut bar = StatusBar::new().text_xs();
        for (ix, element) in self.render_cursor(cx).into_iter().enumerate() {
            if ix > 0 {
                bar = bar.left(separator());
            }
            bar = bar.left(element);
        }
        let mut right = Vec::new();
        right.extend(self.render_jobs(cx));
        right.extend(self.render_sync(cx));
        for element in right {
            bar = bar.right(element).right(separator());
        }
        bar.right(
            div()
                .id("theme-status")
                .role(Role::Status)
                .test_support()
                .aria_label(appearance.clone())
                .when_some(fonts, |this, fonts| {
                    let description = fonts.description();
                    this.tooltip(move |window, cx| {
                        Tooltip::new(description.clone()).build(window, cx)
                    })
                })
                .text_color(cx.theme().muted_foreground)
                .child(appearance),
        )
    }
}
