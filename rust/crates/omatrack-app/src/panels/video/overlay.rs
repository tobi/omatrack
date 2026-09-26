//! What is drawn over the videos: the telemetry HUD (speed, gear, Δ at the
//! cursor, and the gap bar when both GPS fixes are better than 1 m) and the
//! 3-2-1 countdown into the next lap.
//!
//! A separate entity on purpose: it observes `CursorState`, so a playing
//! video re-renders this layer once per frame and nothing else in the panel.

use gpui_kit::component::{ActiveTheme as _, v_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, Context, InteractiveElement as _, IntoElement, ParentElement as _, Render, Role,
    SharedString, StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _,
    Window, div,
};
use omatrack_core::Comparison;
use omatrack_core::alignment::relative_along_track_meters;
use omatrack_core::monotonic::interpolate_fraction;
use omatrack_core::unify::UnifiedLap;
use omatrack_ui::{HudPosition, HudVariant, VideoHud};

use crate::state::AppState;

/// GPS fixes must both be better than this (m) for the gap bar.
const GAP_ACCURACY_M: f64 = 1.0;

/// Readouts of the HUD at one cursor.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct HudReadout {
    pub speed: Option<f64>,
    pub gear: Option<i32>,
    pub delta: Option<f64>,
    pub gap: Option<f64>,
}

impl HudReadout {
    /// Sample the primary lap (and the comparison, when there is one) at
    /// `fraction`.
    pub fn at(primary: &UnifiedLap, comparison: Option<&Comparison>, fraction: f64) -> Self {
        let fraction = fraction.clamp(0.0, 1.0);
        let finite = |value: f64| value.is_finite().then_some(value);
        let speed = (primary.speed.len() >= 2)
            .then(|| interpolate_fraction(&primary.speed, fraction))
            .and_then(finite);
        let gear = (!primary.gear.is_empty())
            .then(|| {
                let ix = (fraction * (primary.gear.len() - 1) as f64).round() as usize;
                primary.gear[ix.min(primary.gear.len() - 1)]
            })
            .filter(|gear| *gear >= -1);
        let (delta, gap) = match comparison {
            Some(comparison) => {
                let reference_fraction = comparison.compare_fraction_for_primary_fraction(fraction);
                (
                    finite(comparison.time_delta_at(fraction)),
                    relative_along_track_meters(
                        primary,
                        fraction,
                        comparison.reference(),
                        reference_fraction,
                        GAP_ACCURACY_M,
                    ),
                )
            }
            None => (None, None),
        };
        Self {
            speed,
            gear,
            delta,
            gap,
        }
    }
}

/// The layer over the video stage.
pub struct VideoOverlay {
    app: AppState,
    _subscriptions: Vec<Subscription>,
}

impl VideoOverlay {
    pub fn new(app: AppState, cx: &mut Context<Self>) -> Self {
        let subscriptions = vec![
            cx.observe(&app.cursor, |_, _, cx| cx.notify()),
            cx.observe(&app.video, |_, _, cx| cx.notify()),
            cx.observe(&app.preferences, |_, _, cx| cx.notify()),
            cx.observe(&app.session, |_, _, cx| cx.notify()),
        ];
        Self {
            app,
            _subscriptions: subscriptions,
        }
    }

    /// The readout the HUD shows now.
    pub fn readout(&self, cx: &App) -> Option<HudReadout> {
        let video = self.app.video.read(cx);
        let timeline = video.timeline(crate::actions::Role::Primary)?;
        let fraction = self.app.cursor.read(cx).fraction().unwrap_or(0.0);
        // The one comparison of the pair, when it is for the lap on screen.
        let comparison = self
            .app
            .session
            .read(cx)
            .analysis()
            .filter(|analysis| {
                let lap = analysis.primary();
                lap.lap_id() == timeline.lap_id()
                    && video
                        .lap(crate::actions::Role::Primary)
                        .is_some_and(|shown| shown.recording().path() == lap.recording().path())
            })
            .and_then(|analysis| analysis.comparison())
            .map(|comparison| comparison.as_ref());
        Some(HudReadout::at(timeline.unified(), comparison, fraction))
    }

    fn render_countdown(&self, count: u8, cx: &App) -> impl IntoElement {
        let video = self.app.video.read(cx);
        let next = video
            .advancing_to()
            .and_then(|id| {
                video
                    .lap(crate::actions::Role::Primary)
                    .and_then(|lap| lap.lap_by_id(id))
            })
            .map(|lap| format!("L{}", lap.source_number.unwrap_or(lap.id)));
        let label: SharedString = match &next {
            Some(next) => format!("{next} in {count}").into(),
            None => format!("Next lap in {count}").into(),
        };
        let theme = cx.theme();
        div()
            .absolute()
            .inset_0()
            .flex()
            .items_center()
            .justify_center()
            .child(
                v_flex()
                    .id("video-countdown")
                    .role(Role::Status)
                    .aria_label(label)
                    .test_support()
                    .items_center()
                    .gap_1()
                    .px_6()
                    .py_3()
                    .rounded(theme.radius_lg)
                    .border_1()
                    .border_color(theme.border)
                    .bg(theme.popover.opacity(0.9))
                    .text_color(theme.popover_foreground)
                    .child(
                        div()
                            .font_family(theme.mono_font_family.clone())
                            .text_3xl()
                            .child(SharedString::from(count.to_string())),
                    )
                    .child(
                        div()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child(match next {
                                Some(next) => {
                                    SharedString::from(format!("Next: {next} · Space cancels"))
                                }
                                None => "Space cancels".into(),
                            }),
                    ),
            )
    }
}

impl Render for VideoOverlay {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let countdown = self.app.video.read(cx).countdown();
        let readout = self.readout(cx);
        let position = self
            .app
            .video
            .read(cx)
            .hud_position(cx)
            .map(|(x, y)| HudPosition::new(x as f32, y as f32))
            .unwrap_or_default();
        let variant = if window.is_fullscreen() {
            HudVariant::Fullscreen
        } else {
            HudVariant::Compact
        };
        let video = self.app.video.clone();
        div()
            .absolute()
            .inset_0()
            .when_some(readout, |this, readout| {
                this.child(
                    VideoHud::new("video-hud")
                        .speed(readout.speed)
                        .gear(readout.gear)
                        .delta(readout.delta)
                        .gap(readout.gap)
                        .position(position)
                        .variant(variant)
                        .on_moved(move |position, _, cx| {
                            let (x, y) = (f64::from(position.x), f64::from(position.y));
                            video.update(cx, |video, cx| video.set_hud_position(x, y, cx));
                        }),
                )
            })
            .when_some(countdown, |this, count| {
                this.child(self.render_countdown(count, cx))
            })
    }
}

/// `L8 · 3/12`: the lap's label and its place among the recording's laps.
pub fn lap_caption(label: &str, lap: Option<&omatrack_core::session::LoadedLap>) -> SharedString {
    let place = lap.and_then(|lap| {
        let ix = lap.laps().iter().position(|l| l.id == lap.lap_id())?;
        Some(format!("{}/{}", ix + 1, lap.laps().len()))
    });
    match place {
        Some(place) => format!("{label} · {place}").into(),
        None => label.to_string().into(),
    }
}
