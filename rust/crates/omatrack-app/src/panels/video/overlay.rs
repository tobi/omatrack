//! What is drawn over the videos.
//!
//! - Docked: the slim telemetry card ([`VideoHud`]: speed, gear, Δ at the
//!   cursor, and the gap bar when both GPS fixes are better than 1 m),
//!   placed inside the large video pane.
//! - Fullscreen stage ([`StageOverlay`]): the broadcast telemetry band
//!   ([`TelemetryHud`], port of the Qt `VideoTelemetryHud`), draggable over
//!   the whole stage, and the live delta bar at the top (port of
//!   `VideoDeltaBar`): the Δt readout, its gain/loss bar coloured by
//!   whether the gap is closing right now, and each role's driver and lap
//!   either side of centre.
//! - The 3-2-1 countdown into the next lap ([`countdown`]).
//!
//! The layer is a separate entity on purpose: it observes `CursorState`,
//! so a playing video re-renders this layer once per frame and nothing else
//! in the panel. The band's geometry buffers live here and are reused
//! frame to frame; its per-selection scales are built once
//! ([`TelemetryHudData`]) and kept while the laps stay the same.
//!
//! `video.hud_position` is the one stored HUD placement (normalized in the
//! space the HUD can reach, so it survives resizes); both presentations
//! read it and write it once, at drag end.

use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;

use gpui_kit::component::{ActiveTheme as _, StyledExt as _, h_flex, kbd::Kbd, v_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AnyElement, App, Context, Hsla, InteractiveElement as _, IntoElement, MouseButton,
    ParentElement as _, Render, Role, SharedString, StatefulInteractiveElement as _, Styled as _,
    Subscription, TestSupportExt as _, Window, div, linear_color_stop, linear_gradient, px,
};
use omatrack_core::Comparison;
use omatrack_core::alignment::relative_along_track_meters;
use omatrack_core::monotonic::interpolate_fraction;
use omatrack_core::unify::UnifiedLap;
use omatrack_trace::telemetry_hud::{self, format_live_delta};
use omatrack_trace::{TelemetryHud, TelemetryHudBuffers, TelemetryHudColors, TelemetryHudData};
use omatrack_ui::TypeScale as _;
use omatrack_ui::{HudPosition, LapRole, VideoHud};

use super::stage;
use crate::state::AppState;

/// GPS fixes must both be better than this (m) for the gap bar.
pub const GAP_ACCURACY_M: f64 = 1.0;
/// The delta bar's full scale either side of centre, seconds.
const DELTA_FULL_SCALE: f64 = 1.0;
/// The delta bar's track, px.
const DELTA_BAR_WIDTH: f32 = 260.;
const DELTA_BAR_HEIGHT: f32 = 18.;
/// Approximate height of the delta block (labels, bar, readout), px.
const DELTA_BLOCK_HEIGHT: f32 = 118.;
/// The bar and readout's share of [`DELTA_BLOCK_HEIGHT`], px.
const DELTA_BAR_BLOCK: f32 = 64.;
/// Speed deltas (km/h) at which the rate colour is at full strength.
const RATE_FULL_KMH: f64 = 5.0;

/// Readouts of the HUD at one cursor.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct HudReadout {
    pub speed: Option<f64>,
    pub gear: Option<i32>,
    pub delta: Option<f64>,
    /// Primary minus reference speed at the mapped place, km/h.
    pub speed_delta: Option<f64>,
    pub gap: Option<f64>,
}

impl HudReadout {
    /// Sample the primary lap (and the comparison, when there is one) at
    /// `fraction`.
    pub fn at(primary: &UnifiedLap, comparison: Option<&Comparison>, fraction: f64) -> Self {
        let fraction = fraction.clamp(0.0, 1.0);
        let finite = |value: f64| value.is_finite().then_some(value);
        let speed_at = |lap: &UnifiedLap, fraction: f64| {
            (lap.speed.len() >= 2)
                .then(|| interpolate_fraction(&lap.speed, fraction))
                .and_then(finite)
        };
        let speed = speed_at(primary, fraction);
        let gear = (!primary.gear.is_empty())
            .then(|| {
                let ix = (fraction * (primary.gear.len() - 1) as f64).round() as usize;
                primary.gear[ix.min(primary.gear.len() - 1)]
            })
            .filter(|gear| *gear >= -1);
        let (delta, speed_delta, gap) = match comparison {
            Some(comparison) => {
                let reference_fraction = comparison.compare_fraction_for_primary_fraction(fraction);
                let reference_speed = speed_at(comparison.reference(), reference_fraction);
                (
                    finite(comparison.time_delta_at(fraction)),
                    speed.zip(reference_speed).map(|(p, r)| p - r),
                    relative_along_track_meters(
                        primary,
                        fraction,
                        comparison.reference(),
                        reference_fraction,
                        GAP_ACCURACY_M,
                    ),
                )
            }
            None => (None, None, None),
        };
        Self {
            speed,
            gear,
            delta,
            speed_delta,
            gap,
        }
    }
}

/// How the panel presents the fullscreen stage to this layer.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct StageOverlay {
    /// Height at the stage bottom the band keeps clear of (filmstrip lane
    /// and controls), px.
    pub bottom_inset: f32,
    /// Top of the highest picture, px: the delta block sits in the
    /// headroom above it when there is room.
    pub pictures_top: f32,
    /// The telemetry band is shown (the stage's HUD toggle).
    pub hud: bool,
}

/// A drag of the band in progress (stage px).
#[derive(Clone, Copy)]
struct BandDrag {
    /// Pointer offset from the band's top-left corner.
    grab: (f32, f32),
    /// The band's top-left corner now.
    origin: (f32, f32),
}

/// The layer over the video stage.
pub struct VideoOverlay {
    app: AppState,
    stage: Option<StageOverlay>,
    data: Option<Arc<TelemetryHudData>>,
    buffers: Rc<RefCell<TelemetryHudBuffers>>,
    drag: Option<BandDrag>,
    _subscriptions: Vec<Subscription>,
}

/// What the layer reads: the primary lap on screen and the pair's
/// comparison when it is for that lap.
struct Shown {
    primary: Arc<UnifiedLap>,
    comparison: Option<Arc<Comparison>>,
    fraction: f64,
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
            stage: None,
            data: None,
            buffers: Rc::default(),
            drag: None,
            _subscriptions: subscriptions,
        }
    }

    /// Present the fullscreen stage (`Some`) or the docked card (`None`).
    pub fn set_stage(&mut self, stage: Option<StageOverlay>, cx: &mut Context<Self>) {
        if self.stage != stage {
            if stage.is_none() {
                self.drag = None;
            }
            self.stage = stage;
            cx.notify();
        }
    }

    pub fn stage(&self) -> Option<StageOverlay> {
        self.stage
    }

    fn shown(&self, cx: &App) -> Option<Shown> {
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
            .cloned();
        Some(Shown {
            primary: timeline.unified().clone(),
            comparison,
            fraction,
        })
    }

    /// The readout the HUD shows now.
    pub fn readout(&self, cx: &App) -> Option<HudReadout> {
        let shown = self.shown(cx)?;
        Some(HudReadout::at(
            &shown.primary,
            shown.comparison.as_deref(),
            shown.fraction,
        ))
    }

    /// The band's data for these laps, rebuilt only when the laps change.
    fn hud_data(&mut self, shown: &Shown) -> Arc<TelemetryHudData> {
        match &self.data {
            Some(data) if data.is_for(&shown.primary, shown.comparison.as_ref()) => data.clone(),
            _ => {
                let data = Arc::new(TelemetryHudData::new(
                    shown.primary.clone(),
                    shown.comparison.clone(),
                ));
                self.data = Some(data.clone());
                data
            }
        }
    }

    fn approximate(&self, cx: &App) -> bool {
        self.app
            .session
            .read(cx)
            .analysis()
            .is_some_and(|analysis| crate::workspace::status::analysis_approximate(analysis))
    }

    fn stored_position(&self, cx: &App) -> Option<(f32, f32)> {
        self.app
            .video
            .read(cx)
            .hud_position(cx)
            .map(|(x, y)| (x as f32, y as f32))
    }

    fn render_docked(&mut self, cx: &mut Context<Self>) -> AnyElement {
        let readout = self.readout(cx);
        let approximate = self.approximate(cx);
        let position = self
            .stored_position(cx)
            .map(|(x, y)| HudPosition::new(x, y))
            .unwrap_or_default();
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
                        .approximate(approximate)
                        .gap(readout.gap)
                        .position(position)
                        .on_moved(move |position, _, cx| {
                            let (x, y) = (f64::from(position.x), f64::from(position.y));
                            video.update(cx, |video, cx| video.set_hud_position(x, y, cx));
                        }),
                )
            })
            .into_any_element()
    }

    fn render_stage(
        &mut self,
        stage: StageOverlay,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let viewport = window.viewport_size();
        let stage_size = (viewport.width.as_f32(), viewport.height.as_f32());
        let shown = self.shown(cx);
        let readout = shown.as_ref().map(|shown| {
            HudReadout::at(&shown.primary, shown.comparison.as_deref(), shown.fraction)
        });
        let hud_size = telemetry_hud::hud_size(stage_size.0);
        let origin = match self.drag {
            Some(drag) => drag.origin,
            None => stage::hud_origin(
                stage_size,
                hud_size,
                stage.bottom_inset,
                self.stored_position(cx),
            ),
        };
        let continuous = self.app.video.read(cx).is_continuous(cx);
        let band = match (&shown, stage.hud) {
            (Some(shown), true) => {
                let data = self.hud_data(shown);
                let colors = TelemetryHudColors::from_theme(cx.theme());
                let gap = readout.and_then(|r| r.gap);
                let element = TelemetryHud::new(data, shown.fraction, colors, self.buffers.clone())
                    .marker(telemetry_hud::marker(continuous))
                    .gap(gap);
                Some(
                    div()
                        .id("video-telemetry-hud")
                        .role(Role::Figure)
                        .aria_label("Telemetry overlay")
                        .test_support()
                        .absolute()
                        .left(px(origin.0))
                        .top(px(origin.1))
                        .w(px(hud_size.0))
                        .h(px(hud_size.1))
                        .map(|this| {
                            if self.drag.is_some() {
                                this.cursor_grabbing()
                            } else {
                                this.cursor_grab()
                            }
                        })
                        .child(element)
                        // A marker for tests and assistive tech: the gap
                        // bar is drawn only when both fixes allow it.
                        .when(gap.is_some(), |this| {
                            this.child(
                                div()
                                    .id("video-telemetry-gap")
                                    .test_support()
                                    .absolute()
                                    .size_0(),
                            )
                        })
                        .on_mouse_down(
                            MouseButton::Left,
                            cx.listener(move |this, event: &gpui_kit::MouseDownEvent, _, cx| {
                                let at = (event.position.x.as_f32(), event.position.y.as_f32());
                                this.drag = Some(BandDrag {
                                    grab: (at.0 - origin.0, at.1 - origin.1),
                                    origin,
                                });
                                cx.stop_propagation();
                                cx.notify();
                            }),
                        )
                        .into_any_element(),
                )
            }
            _ => None,
        };
        let delta_bar = self.render_delta_bar(stage, readout, stage_size, cx);
        let bottom_inset = stage.bottom_inset;
        let end_drag = move |this: &mut Self, cx: &mut Context<Self>| {
            let Some(drag) = this.drag.take() else {
                return;
            };
            let (x, y) = stage::hud_position(stage_size, hud_size, bottom_inset, drag.origin);
            this.app.video.update(cx, |video, cx| {
                video.set_hud_position(f64::from(x), f64::from(y), cx)
            });
            cx.notify();
        };
        div()
            .id("video-stage-overlay")
            .absolute()
            .inset_0()
            .children(delta_bar)
            .children(band)
            .on_mouse_move(
                cx.listener(move |this, event: &gpui_kit::MouseMoveEvent, _, cx| {
                    let Some(drag) = this.drag.as_mut() else {
                        return;
                    };
                    let (room_x, room_y) = stage::hud_room(stage_size, hud_size, bottom_inset);
                    let at = (event.position.x.as_f32(), event.position.y.as_f32());
                    let next = (
                        (at.0 - drag.grab.0).clamp(0., room_x),
                        (at.1 - drag.grab.1).clamp(0., room_y),
                    );
                    if next != drag.origin {
                        drag.origin = next;
                        cx.notify();
                    }
                }),
            )
            .on_mouse_up(
                MouseButton::Left,
                cx.listener(move |this, _, _, cx| end_drag(this, cx)),
            )
            .on_mouse_up_out(
                MouseButton::Left,
                cx.listener(move |this, _, _, cx| end_drag(this, cx)),
            )
            .into_any_element()
    }

    /// The live delta at the top of the stage: role labels either side of
    /// centre, the gain/loss bar and the Δt readout (with `≈` and no
    /// gain/loss colour when the alignment is LOW confidence).
    fn render_delta_bar(
        &self,
        stage: StageOverlay,
        readout: Option<HudReadout>,
        stage_size: (f32, f32),
        cx: &App,
    ) -> Option<AnyElement> {
        let session = self.app.session.read(cx);
        let primary = session.slot(crate::actions::Role::Primary)?;
        let reference = session.slot(crate::actions::Role::Reference);
        let comparing = readout.is_some_and(|r| r.delta.is_some());
        let approximate = self.approximate(cx);
        let theme = cx.theme();
        let context_width = ((stage_size.0 - DELTA_BAR_WIDTH - 48.) / 2.).clamp(100., 210.);
        let video = self.app.video.read(cx);
        let label = |role: crate::actions::Role, info: &crate::state::LapInfo, right: bool| {
            let lap_role = match role {
                crate::actions::Role::Primary => LapRole::Primary,
                crate::actions::Role::Reference => LapRole::Reference,
            };
            let color = lap_role.color(theme);
            let mut detail = lap_caption(&info.label, video.lap(role)).to_string();
            if !info.time.is_empty() {
                detail.push_str(" · ");
                detail.push_str(&info.time);
            }
            let name = info
                .driver
                .clone()
                .unwrap_or_else(|| lap_role.label().into());
            v_flex()
                .w(px(context_width))
                .flex_shrink_0()
                .gap_0p5()
                .when(right, |this| this.items_end())
                .when(!right, |this| this.items_start())
                .child(
                    h_flex()
                        .gap_1p5()
                        .max_w_full()
                        .when(right, |this| this.flex_row_reverse())
                        .child(
                            div()
                                .flex_shrink_0()
                                .px_1()
                                .rounded(theme.radius_tokens().sm)
                                .bg(color)
                                .text_color(match lap_role {
                                    LapRole::Primary => theme.primary_foreground,
                                    LapRole::Reference => theme.warning_foreground,
                                })
                                .text_label()
                                .numeric()
                                .font_semibold()
                                .child(lap_role.marker()),
                        )
                        .child(
                            div()
                                .min_w_0()
                                .truncate()
                                .text_body()
                                .font_medium()
                                .text_color(theme.foreground)
                                .child(name),
                        ),
                )
                .child(
                    div()
                        .max_w_full()
                        .truncate()
                        .text_caption()
                        .numeric()
                        .text_color(theme.muted_foreground)
                        .child(SharedString::from(detail)),
                )
        };
        let labels = h_flex()
            .justify_center()
            .items_start()
            .gap_6()
            .child(label(crate::actions::Role::Primary, primary.info(), true))
            .when_some(reference, |this, slot| {
                this.child(label(crate::actions::Role::Reference, slot.info(), false))
            });

        let bar = readout.and_then(|r| r.delta).map(|delta| {
            let gaining = match readout.and_then(|r| r.speed_delta) {
                Some(speed_delta) => speed_delta >= 0.0,
                None => delta <= 0.0,
            };
            let rate = if approximate {
                theme.muted_foreground
            } else {
                let base = if gaining { theme.success } else { theme.danger };
                let strength = readout
                    .and_then(|r| r.speed_delta)
                    .map_or(1.0, |d| (d.abs() / RATE_FULL_KMH).min(1.0));
                mix(
                    theme.muted_foreground,
                    base,
                    (0.55 + 0.45 * strength) as f32,
                )
            };
            let dim = rate.opacity(0.38);
            let share = (delta.abs() / DELTA_FULL_SCALE).min(1.0) as f32;
            let fill_w = DELTA_BAR_WIDTH * 0.5 * share;
            let ahead = delta < 0.0;
            let text: SharedString = if approximate {
                format!("≈{}", format_live_delta(delta)).into()
            } else {
                format_live_delta(delta)
            };
            let spoken: SharedString = if approximate {
                format!("Delta approximately {delta:+.2} seconds").into()
            } else {
                format!("Delta {delta:+.2} seconds").into()
            };
            v_flex()
                .id("video-delta-bar")
                .role(Role::Meter)
                .aria_label(spoken)
                .test_support()
                .items_center()
                .gap_1()
                .child(
                    div()
                        .relative()
                        .w(px(DELTA_BAR_WIDTH))
                        .h(px(DELTA_BAR_HEIGHT))
                        .rounded_full()
                        .overflow_hidden()
                        .border_1()
                        .border_color(theme.border.opacity(0.7))
                        .bg(theme.background.opacity(0.9))
                        .when(fill_w > 0.5, |this| {
                            this.child(
                                div()
                                    .absolute()
                                    .top_0()
                                    .bottom_0()
                                    .left(px(if ahead {
                                        DELTA_BAR_WIDTH * 0.5 - fill_w
                                    } else {
                                        DELTA_BAR_WIDTH * 0.5
                                    }))
                                    .w(px(fill_w))
                                    .bg(if ahead {
                                        linear_gradient(
                                            90.,
                                            linear_color_stop(rate, 0.),
                                            linear_color_stop(dim, 1.),
                                        )
                                    } else {
                                        linear_gradient(
                                            90.,
                                            linear_color_stop(dim, 0.),
                                            linear_color_stop(rate, 1.),
                                        )
                                    }),
                            )
                        })
                        .child(
                            div()
                                .absolute()
                                .top_0()
                                .bottom_0()
                                .left(px(DELTA_BAR_WIDTH * 0.5 - 1.))
                                .w(px(2.))
                                .bg(theme.foreground.opacity(0.7)),
                        ),
                )
                .child(
                    div()
                        .px_2()
                        .rounded(theme.radius)
                        .bg(theme.background.opacity(0.9))
                        .text_display()
                        .numeric()
                        .text_color(rate)
                        .child(text),
                )
        });
        // Centred in the headroom above the pictures when it fits there,
        // else at the top edge over the picture.
        let block = DELTA_BLOCK_HEIGHT - if comparing { 0. } else { DELTA_BAR_BLOCK };
        let top = if stage.pictures_top >= block + 24. {
            (stage.pictures_top - block) / 2.
        } else {
            12.
        };
        Some(
            v_flex()
                .id("video-delta")
                .test_support()
                .absolute()
                .left_0()
                .right_0()
                .top(px(top))
                .items_center()
                .gap_2()
                .child(
                    div()
                        .px_3()
                        .py_1p5()
                        .rounded(theme.radius_lg)
                        .border_1()
                        .border_color(theme.border.opacity(0.7))
                        .bg(theme.background.opacity(0.9))
                        .child(labels),
                )
                .when(comparing, |this| this.children(bar))
                .into_any_element(),
        )
    }
}

/// Linear mix in RGB (never a hue interpolation, which crosses hues).
fn mix(from: Hsla, to: Hsla, t: f32) -> Hsla {
    let (a, b) = (from.to_rgb(), to.to_rgb());
    let lerp = |x: f32, y: f32| x + (y - x) * t;
    gpui_kit::Rgba {
        r: lerp(a.r, b.r),
        g: lerp(a.g, b.g),
        b: lerp(a.b, b.b),
        a: lerp(a.a, b.a),
    }
    .into()
}

/// The 3-2-1 card counting into the next lap, centred over the stage; `None`
/// when no countdown runs.
pub fn countdown(app: &AppState, window: &Window, cx: &App) -> Option<AnyElement> {
    let video = app.video.read(cx);
    let count = video.countdown()?;
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
    let cancel = Kbd::binding_for_action(
        &crate::actions::TogglePlay,
        Some(crate::keymap::WORKSPACE_CONTEXT),
        window,
    );
    let theme = cx.theme();
    Some(
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
                    .min_w(gpui_kit::rems(10.))
                    .px_6()
                    .py_3()
                    .rounded(theme.radius_lg)
                    .border_1()
                    .border_color(theme.border)
                    .bg(theme.popover.opacity(0.92))
                    .text_color(theme.popover_foreground)
                    .shadow_lg()
                    .child(
                        div()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child(match &next {
                                Some(next) => SharedString::from(format!("{next} starts in")),
                                None => "Next lap starts in".into(),
                            }),
                    )
                    .child(
                        div()
                            .numeric()
                            .font_semibold()
                            .text_3xl()
                            .child(SharedString::from(count.to_string())),
                    )
                    .child(
                        h_flex()
                            .gap_1p5()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .children(cancel)
                            .child("Cancel"),
                    ),
            )
            .into_any_element(),
    )
}

impl Render for VideoOverlay {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        match self.stage {
            Some(stage) => self.render_stage(stage, window, cx),
            None => self.render_docked(cx),
        }
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

#[cfg(test)]
mod tests {
    use super::*;
    use omatrack_core::alignment::Strategy;

    /// A straight northbound lap at 180 km/h with GPS fixes of `accuracy` m,
    /// starting `ahead` metres up the road.
    fn gps_lap(ahead: f64, accuracy: f64) -> Arc<UnifiedLap> {
        let n = 501;
        let metres_per_degree = 111_320.0;
        let time: Vec<f64> = (0..n).map(|i| i as f64 * 0.02).collect();
        let distance: Vec<f64> = time.iter().map(|t| t * 50.0).collect();
        Arc::new(UnifiedLap {
            speed: vec![180.0; n],
            gear: vec![5; n],
            gps_lat: distance
                .iter()
                .map(|d| 45.0 + (d + ahead) / metres_per_degree)
                .collect(),
            gps_lon: vec![9.0; n],
            gps_position_accuracy: vec![accuracy; n],
            gps_speed_accuracy: vec![0.2; n],
            distance,
            time,
            ..UnifiedLap::default()
        })
    }

    fn comparison(primary: &Arc<UnifiedLap>, reference: &Arc<UnifiedLap>) -> Comparison {
        Comparison::new(
            primary.clone(),
            reference.clone(),
            Strategy::LapPercentage,
            Vec::new(),
            0.0,
        )
    }

    #[test]
    fn the_gap_bar_needs_both_fixes_better_than_a_metre() {
        let primary = gps_lap(0.0, 0.5);
        let precise = gps_lap(3.0, 0.5);
        let readout = HudReadout::at(&primary, Some(&comparison(&primary, &precise)), 0.5);
        let gap = readout.gap.expect("both fixes are precise");
        assert!((gap - 3.0).abs() < 0.2, "{gap}");
        assert!(readout.delta.is_some());
        assert_eq!(readout.speed_delta, Some(0.0));

        let coarse = gps_lap(3.0, 2.0);
        let readout = HudReadout::at(&primary, Some(&comparison(&primary, &coarse)), 0.5);
        assert_eq!(readout.gap, None, "a 2 m fix hides the gap bar");
        let readout = HudReadout::at(&coarse, Some(&comparison(&coarse, &precise)), 0.5);
        assert_eq!(readout.gap, None, "either lap's fix counts");
        // No reference, no gap, no delta.
        let single = HudReadout::at(&primary, None, 0.5);
        assert_eq!(
            (single.gap, single.delta, single.speed_delta),
            (None, None, None)
        );
        assert_eq!(single.speed, Some(180.0));
    }
}
