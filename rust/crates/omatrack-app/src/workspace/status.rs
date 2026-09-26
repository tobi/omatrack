//! The status bar: background jobs and the range readout, what no other
//! surface already says. Idle (no job, no range) it takes no space; the
//! theme and fonts are stated in Preferences > Appearance.
//!
//! It is its own entity so a cursor move re-renders only this bar (and the
//! other cursor observers), never the workspace or the static traces.

use gpui_kit::component::{
    ActiveTheme as _, Sizable as _, Theme, h_flex, separator::Separator, spinner::Spinner,
    status_bar::StatusBar,
};
use gpui_kit::{
    AnyElement, App, Context, InteractiveElement as _, IntoElement, ParentElement as _, Render,
    Role, SharedString, StatefulInteractiveElement as _, Styled as _, Subscription,
    TestSupportExt as _, Window, div,
};
use omatrack_core::alignment::Strategy;
use omatrack_core::format_lap_time;
use omatrack_core::session::Analysis;
use omatrack_trace::scale::value_at_fraction;
use omatrack_ui::TypeScale as _;
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
    if value < 0.0 {
        format!("{}{out}", omatrack_ui::MINUS)
    } else {
        out
    }
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

/// Whether an analysis's deltas are approximate (LOW or no alignment).
/// The one rule every delta surface uses (status bar, header, inspector,
/// traces, corners, HUD, map).
pub fn analysis_approximate(analysis: &Analysis) -> bool {
    analysis
        .comparison()
        .is_some_and(|comparison| approximate(comparison.confidence()))
}

/// Whether the comparison is a share of lap time (`Lap time %`): every
/// station then maps to the same share of the other lap's duration, so the
/// cumulative Δt is a ramp of the lap-time difference, not a station
/// delta, and a corner's Δt mostly measures how long the corner is.
pub fn analysis_time_share(analysis: &Analysis) -> bool {
    analysis
        .comparison()
        .is_some_and(|comparison| comparison.basis() == omatrack_core::alignment::BASIS_LAP_TIME)
}

/// Why GPS alignment was not used although both laps carry GPS: the
/// fixes of the two laps never agreed in position, speed and heading.
pub fn gps_rejection(analysis: &Analysis) -> Option<SharedString> {
    let comparison = analysis.comparison()?;
    let offered = analysis.available_strategies().contains(&Strategy::Gps);
    let alignment = comparison.alignment();
    (offered && comparison.strategy() == Strategy::Gps && alignment.gps_anchors == 0).then(|| {
        match alignment.gps_rejected {
            0 => "GPS rejected: no fix of one lap lies near a fix of the other.".into(),
            1 => {
                "GPS rejected: 1 matched fix failed the position, speed and heading checks.".into()
            }
            n => format!(
                "GPS rejected: {n} matched fixes failed the position, speed and heading checks."
            )
            .into(),
        }
    })
}

/// The caution shown beside approximate deltas (corners notes, focus
/// card): `Alignment LOW (Lap time %): deltas are approximate.`
pub fn alignment_caution(analysis: &Analysis) -> Option<SharedString> {
    let comparison = analysis.comparison()?;
    let confidence = comparison.confidence();
    if !approximate(confidence) {
        return None;
    }
    let basis = comparison.basis();
    let time_share = if basis == omatrack_core::alignment::BASIS_LAP_TIME {
        " a time-share estimate, not a station alignment"
    } else {
        " approximate"
    };
    Some(format!("Alignment {confidence} ({basis}): deltas are{time_share}.").into())
}

/// `Δ`, or `Δ≈` for an approximate delta.
fn delta_mark(approximate: bool) -> &'static str {
    if approximate { "Δ≈" } else { "Δ" }
}

/// A Δt readout: three decimals and gain/loss colour, or two muted decimals
/// when approximate (the `Δ≈` mark precedes it).
fn approx_delta(value: Option<f64>, approximate: bool) -> DeltaText {
    DeltaText::new(value)
        .decimals(if approximate { 2 } else { 3 })
        .muted(approximate)
        .unit("s")
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
    /// The selection's Δt and minimum speeds, while a range is selected:
    /// the one analysis readout no other surface carries.
    fn render_selection(&self, cx: &App) -> Option<AnyElement> {
        let selection = self.app.cursor.read(cx).selection()?;
        let analysis = self.app.session.read(cx).analysis()?;
        let comparison = analysis.comparison()?;
        // Under LOW sync confidence every delta is approximate (a 12–15 m
        // turn-in error is typical), and says so.
        let approximate = approximate(comparison.confidence());
        let range =
            comparison.time_delta_at(selection.end) - comparison.time_delta_at(selection.start);
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
        Some(item(
            "status-selection",
            format!("Range delta {label} s, {speeds}").into(),
            h_flex()
                .gap_1()
                .child("Range")
                .child(delta_mark(approximate))
                .child(approx_delta(finite, approximate))
                .child(div().text_color(cx.theme().muted_foreground).child(speeds)),
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
        // Only what no other surface says: background work and the range
        // readout. The cursor and its Δ live in the traces and the video
        // bar, the sync basis and confidence in the title bar, the theme in
        // Preferences.
        let left: Vec<AnyElement> = self
            .render_jobs(cx)
            .into_iter()
            .chain(self.render_selection(cx))
            .collect();
        if left.is_empty() {
            return div().id("status-idle").into_any_element();
        }
        let mut bar = StatusBar::new().text_label();
        for (ix, element) in left.into_iter().enumerate() {
            if ix > 0 {
                bar = bar.left(Separator::vertical().h_3().into_any_element());
            }
            bar = bar.left(element);
        }
        div()
            .id("status-bar")
            .test_support()
            .child(bar)
            .into_any_element()
    }
}
