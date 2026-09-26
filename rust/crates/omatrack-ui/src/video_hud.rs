//! The telemetry HUD over the video: speed, gear, Δ and the gap bar.
//!
//! [`VideoHud`] fills the video area it is placed in (absolutely, `inset_0`;
//! the owner places it in one video pane, never across two) and draws one
//! slim card inside it at a normalized position of the *available* space,
//! [`HUD_INSET`] in from every edge: `(0, 0)` is the top-left placement,
//! `(1, 1)` the bottom-right, the default [`HudPosition::DEFAULT`] the
//! bottom-left, so a stored position survives window resizes and layout
//! changes (`video.hud_position` in `omatrack.yml`). The card is draggable; the new
//! position is reported once, at drag end, through [`VideoHud::on_moved`]
//! (the owner persists it and passes it back: a controlled value). During
//! the drag the live position is element-local state keyed by the HUD's id.
//!
//! The gap bar shows where the reference car is along the track, ±8 m with
//! the centre as level. The caller supplies a gap only when both GPS fixes
//! are better than 1 m (`alignment::relative_along_track_meters`); `None`
//! draws nothing at all rather than a misleading empty bar.
//!
//! Sizes use rem-based helpers so the HUD follows the application zoom; the
//! card has a fixed rem size per variant so the available space is exact
//! without measuring the card. Colours are theme tokens: the popover
//! surface, `success`/`danger` for gain/loss (always with an explicit sign),
//! `warning` for the reference role.

use std::cell::Cell;
use std::rc::Rc;

use gpui_kit::base::TestSupportExt as _;
use gpui_kit::component::{ActiveTheme as _, StyledExt as _, h_flex, v_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, Bounds, ElementId, InteractiveElement as _, IntoElement, MouseButton, ParentElement as _,
    Pixels, Point, Rems, RenderOnce, Role, SharedString, StatefulInteractiveElement as _,
    Styled as _, Window, canvas, div, relative, rems,
};

use crate::{DeltaText, MISSING_VALUE, Swatch, TypeScale as _};

/// Range of the gap bar either side of level, metres.
pub const GAP_RANGE_M: f64 = 8.0;

/// How far the card keeps from the pane's edges, rem.
pub const HUD_INSET: f32 = 0.5;

/// A normalized placement inside the available space (`0..=1` each way).
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct HudPosition {
    pub x: f32,
    pub y: f32,
}

impl HudPosition {
    /// Bottom-left: clear of the role caption (top-left) and of an inset
    /// video (bottom-right).
    pub const DEFAULT: HudPosition = HudPosition { x: 0.0, y: 1.0 };

    /// A position clamped into `0..=1`; non-finite coordinates fall back to
    /// the default.
    pub fn new(x: f32, y: f32) -> Self {
        let clamp = |v: f32, fallback: f32| {
            if v.is_finite() {
                v.clamp(0.0, 1.0)
            } else {
                fallback
            }
        };
        Self {
            x: clamp(x, Self::DEFAULT.x),
            y: clamp(y, Self::DEFAULT.y),
        }
    }

    /// The normalized position that puts the card's top-left corner at
    /// `origin`, inside the available `track` (the area the top-left corner
    /// can reach).
    pub fn from_origin(origin: Point<Pixels>, track: Bounds<Pixels>) -> Self {
        let span = |extent: Pixels| extent.as_f32().max(1.0);
        Self::new(
            (origin.x - track.origin.x).as_f32() / span(track.size.width),
            (origin.y - track.origin.y).as_f32() / span(track.size.height),
        )
    }

    /// The card's top-left corner for this position inside `track`.
    pub fn origin(&self, track: Bounds<Pixels>) -> Point<Pixels> {
        track.origin
            + gpui_kit::point(
                track.size.width.max(Pixels::ZERO) * self.x,
                track.size.height.max(Pixels::ZERO) * self.y,
            )
    }
}

impl Default for HudPosition {
    fn default() -> Self {
        Self::DEFAULT
    }
}

/// Docked (compact) or fullscreen presentation.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum HudVariant {
    #[default]
    Compact,
    Fullscreen,
}

impl HudVariant {
    /// Card width and height in rem, with and without the gap bar.
    fn card_size(self, gap: bool) -> (Rems, Rems) {
        match (self, gap) {
            (HudVariant::Compact, false) => (rems(16.), rems(2.25)),
            (HudVariant::Compact, true) => (rems(16.), rems(3.5)),
            (HudVariant::Fullscreen, false) => (rems(21.), rems(3.25)),
            (HudVariant::Fullscreen, true) => (rems(21.), rems(4.75)),
        }
    }
}

/// Where the gap marker sits on the bar: `-1` (reference 8 m behind) …
/// `1` (reference 8 m ahead), `0` level.
pub fn gap_position(gap_m: f64) -> f64 {
    if gap_m.is_finite() {
        (gap_m / GAP_RANGE_M).clamp(-1.0, 1.0)
    } else {
        0.0
    }
}

/// `+2.4 m`, `−1.0 m`, `0.0 m` (level within 5 cm).
pub fn format_gap(gap_m: f64) -> SharedString {
    if !gap_m.is_finite() {
        return MISSING_VALUE.into();
    }
    if gap_m.abs() < 0.05 {
        return "0.0 m".into();
    }
    let sign = if gap_m > 0.0 { '+' } else { '−' };
    format!("{sign}{:.1} m", gap_m.abs()).into()
}

/// Gear label: `N` for neutral, `R` for reverse.
pub fn format_gear(gear: Option<i32>) -> SharedString {
    match gear {
        Some(0) => "N".into(),
        Some(g) if g < 0 => "R".into(),
        Some(g) => g.to_string().into(),
        None => MISSING_VALUE.into(),
    }
}

/// Transient drag state, keyed by the HUD's id.
#[derive(Default)]
struct HudDrag {
    /// Pointer offset from the card's top-left corner.
    grab: Option<Point<Pixels>>,
    /// Position while dragging.
    live: Option<HudPosition>,
    /// The available space, measured each frame (no notification).
    track: Rc<Cell<Option<Bounds<Pixels>>>>,
}

type MovedHandler = Rc<dyn Fn(&HudPosition, &mut Window, &mut App)>;

/// The draggable telemetry HUD. See the module docs.
#[derive(IntoElement)]
pub struct VideoHud {
    id: ElementId,
    speed: Option<f64>,
    gear: Option<i32>,
    delta: Option<f64>,
    approximate: bool,
    gap: Option<f64>,
    position: HudPosition,
    variant: HudVariant,
    on_moved: Option<MovedHandler>,
}

impl VideoHud {
    /// `id` names the HUD's video pane (`"video-hud-primary"`); it keys the
    /// drag state and prefixes the card's parts.
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            speed: None,
            gear: None,
            delta: None,
            approximate: false,
            gap: None,
            position: HudPosition::DEFAULT,
            variant: HudVariant::Compact,
            on_moved: None,
        }
    }

    /// Primary speed at the cursor, km/h.
    pub fn speed(mut self, kmh: Option<f64>) -> Self {
        self.speed = kmh.filter(|v| v.is_finite());
        self
    }

    pub fn gear(mut self, gear: Option<i32>) -> Self {
        self.gear = gear;
        self
    }

    /// Cumulative Δt at the cursor, seconds (negative: primary ahead).
    pub fn delta(mut self, seconds: Option<f64>) -> Self {
        self.delta = seconds.filter(|v| v.is_finite());
        self
    }

    /// The Δt is a LOW-confidence estimate: `≈`, two decimals, no gain/loss
    /// colour.
    pub fn approximate(mut self, approximate: bool) -> Self {
        self.approximate = approximate;
        self
    }

    /// Signed along-track metres to the reference car (positive: reference
    /// ahead). Pass `Some` only when both GPS fixes are better than 1 m.
    pub fn gap(mut self, metres: Option<f64>) -> Self {
        self.gap = metres.filter(|v| v.is_finite());
        self
    }

    /// Normalized placement (controlled).
    pub fn position(mut self, position: HudPosition) -> Self {
        self.position = position;
        self
    }

    pub fn variant(mut self, variant: HudVariant) -> Self {
        self.variant = variant;
        self
    }

    /// The position requested by a drag, reported once when it ends.
    pub fn on_moved(
        mut self,
        handler: impl Fn(&HudPosition, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_moved = Some(Rc::new(handler));
        self
    }

    fn part(&self, name: &str) -> ElementId {
        ElementId::Name(format!("{}-{name}", self.id).into())
    }

    fn spoken(&self) -> SharedString {
        let mut text = match self.speed {
            Some(speed) => format!("Speed {speed:.0} km/h"),
            None => "Speed unavailable".into(),
        };
        text.push_str(&format!(", gear {}", format_gear(self.gear)));
        if let Some(delta) = self.delta {
            if self.approximate {
                text.push_str(&format!(", delta approximately {delta:+.2} s"));
            } else {
                text.push_str(&format!(", delta {delta:+.3} s"));
            }
        }
        if let Some(gap) = self.gap {
            text.push_str(&format!(", gap {}", format_gap(gap)));
        }
        text.into()
    }
}

impl RenderOnce for VideoHud {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let drag = window.use_keyed_state(self.part("drag"), cx, |_, _| HudDrag::default());
        let (grabbing, live, track_cell) = {
            let state = drag.read(cx);
            (state.grab.is_some(), state.live, state.track.clone())
        };
        let position = live.unwrap_or(self.position);
        let (card_width, card_height) = self.variant.card_size(self.gap.is_some());
        let fullscreen = self.variant == HudVariant::Fullscreen;
        let spoken = self.spoken();
        let card_id = self.part("card");
        let gap_id = self.part("gap");
        let theme = cx.theme();
        let (surface, surface_fg, border, muted) = (
            theme.popover.opacity(0.9),
            theme.popover_foreground,
            theme.border,
            theme.muted_foreground,
        );

        // One row, `236 km/h │ Gear 6 │ Δ +0.123 s`, baseline-aligned; the
        // values use tabular figures so they do not jitter while the video plays.
        let caption = |text: &'static str| div().text_xs().text_color(muted).child(text);
        let value = |text: SharedString| {
            div()
                .numeric()
                .font_semibold()
                .map(|d| {
                    if fullscreen {
                        d.text_2xl()
                    } else {
                        d.text_lg()
                    }
                })
                .child(text)
        };
        let divider = || {
            div()
                .flex_shrink_0()
                .w_px()
                .map(|d| if fullscreen { d.h_6() } else { d.h_4() })
                .bg(border)
        };
        let speed = h_flex()
            .flex_shrink_0()
            .items_baseline()
            .gap_1()
            .child(value(match self.speed {
                Some(speed) => SharedString::from(format!("{speed:.0}")),
                None => MISSING_VALUE.into(),
            }))
            .child(caption("km/h"));
        let gear = h_flex()
            .flex_shrink_0()
            .items_baseline()
            .gap_1()
            .child(caption("Gear"))
            .child(value(format_gear(self.gear)));
        let delta = h_flex()
            .flex_1()
            .min_w_0()
            .justify_end()
            .items_baseline()
            .gap_1()
            .child(caption("Δ"))
            .child(
                div()
                    .map(|d| if fullscreen { d.text_lg() } else { d.text_sm() })
                    .child(
                        DeltaText::new(self.delta)
                            .decimals(if self.approximate { 2 } else { 3 })
                            .approximate(self.approximate)
                            .unit("s"),
                    ),
            );

        let gap_bar = self.gap.map(|gap| {
            let marker = gap_position(gap);
            // Positive gap: the reference is ahead, time is being lost.
            let color = if gap.abs() < 0.05 {
                surface_fg
            } else if gap > 0.0 {
                theme.danger
            } else {
                theme.success
            };
            let centre = 0.5f32;
            let end = (0.5 + marker * 0.5) as f32;
            h_flex()
                .id(gap_id.clone())
                .role(Role::Meter)
                .aria_label(SharedString::from(format!("Gap {}", format_gap(gap))))
                .test_support()
                .gap_2()
                .child(
                    div()
                        .w_16()
                        .flex_shrink_0()
                        .text_right()
                        .text_xs()
                        .numeric()
                        .text_color(color)
                        .child(format_gap(gap)),
                )
                .child(
                    div()
                        .relative()
                        .flex_1()
                        .h_2()
                        .rounded_sm()
                        .bg(theme.muted)
                        .child(
                            div()
                                .absolute()
                                .top_0()
                                .bottom_0()
                                .left(relative(centre.min(end)))
                                .w(relative((end - centre).abs()))
                                .bg(theme.warning.opacity(0.45)),
                        )
                        .child(
                            div()
                                .absolute()
                                .top_0()
                                .bottom_0()
                                .left(relative(end.clamp(0.0, 0.99)))
                                .w_0p5()
                                .bg(theme.warning),
                        )
                        .child(
                            div()
                                .absolute()
                                .top_0()
                                .bottom_0()
                                .left(relative(centre))
                                .w_px()
                                .bg(surface_fg),
                        ),
                )
        });

        let card = v_flex()
            .id(card_id)
            .role(Role::Group)
            .aria_label(spoken)
            .test_support()
            .absolute()
            .left(relative(position.x))
            .top(relative(position.y))
            .w(card_width)
            .h(card_height)
            .map(|d| if fullscreen { d.px_4() } else { d.px_2p5() })
            .gap_1()
            .justify_center()
            .overflow_hidden()
            .rounded(theme.radius_lg)
            .border_1()
            .border_color(border)
            .bg(surface)
            .text_color(surface_fg)
            .shadow_md()
            .map(|el| {
                if grabbing {
                    el.cursor_grabbing()
                } else {
                    el.cursor_grab()
                }
            })
            .child(
                h_flex()
                    .items_center()
                    .gap_2()
                    // The readouts are the primary lap's, whichever video
                    // the HUD sits on.
                    .child(Swatch::new(theme.primary))
                    .child(speed)
                    .child(divider())
                    .child(gear)
                    .child(divider())
                    .child(delta),
            )
            .children(gap_bar)
            .on_mouse_down(MouseButton::Left, {
                let drag = drag.clone();
                move |event, _, cx| {
                    let Some(track) = drag.read(cx).track.get() else {
                        return;
                    };
                    let grab = event.position - position.origin(track);
                    drag.update(cx, |state, cx| {
                        state.grab = Some(grab);
                        state.live = Some(position);
                        cx.notify();
                    });
                    cx.stop_propagation();
                }
            });

        // The area the card's top-left corner can reach: the pane inset by
        // `HUD_INSET`, minus the card. Measured every frame for the drag
        // arithmetic only.
        let track = div()
            .absolute()
            .top(rems(HUD_INSET))
            .left(rems(HUD_INSET))
            .right(rems(card_width.0 + HUD_INSET))
            .bottom(rems(card_height.0 + HUD_INSET))
            .child(
                canvas(
                    move |bounds, _, _| track_cell.set(Some(bounds)),
                    |_, _, _, _| {},
                )
                .size_full(),
            )
            .child(card);

        let on_moved = self.on_moved;
        let end_drag = {
            let drag = drag.clone();
            move |window: &mut Window, cx: &mut App| {
                let Some(live) = drag.read(cx).live else {
                    return;
                };
                if drag.read(cx).grab.is_none() {
                    return;
                }
                drag.update(cx, |state, cx| {
                    state.grab = None;
                    state.live = None;
                    cx.notify();
                });
                if let Some(handler) = &on_moved {
                    handler(&live, window, cx);
                }
            }
        };
        let end_inside = end_drag.clone();
        div()
            .id(self.id)
            .absolute()
            .inset_0()
            .child(track)
            .on_mouse_move({
                let drag = drag.clone();
                move |event, _, cx| {
                    let (grab, track) = {
                        let state = drag.read(cx);
                        (state.grab, state.track.get())
                    };
                    let (Some(grab), Some(track)) = (grab, track) else {
                        return;
                    };
                    let next = HudPosition::from_origin(event.position - grab, track);
                    drag.update(cx, |state, cx| {
                        if state.live != Some(next) {
                            state.live = Some(next);
                            cx.notify();
                        }
                    });
                }
            })
            .on_mouse_up(MouseButton::Left, move |_, window, cx| {
                end_inside(window, cx)
            })
            .on_mouse_up_out(MouseButton::Left, move |_, window, cx| end_drag(window, cx))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui_kit::{point, px, size};

    #[test]
    fn positions_clamp_and_round_trip_through_the_track() {
        assert_eq!(HudPosition::new(-1.0, 2.0), HudPosition::new(0.0, 1.0));
        assert_eq!(
            HudPosition::new(f32::NAN, 0.25),
            HudPosition::new(0.0, 0.25)
        );
        let track = Bounds::new(point(px(10.), px(20.)), size(px(400.), px(200.)));
        let position = HudPosition::new(0.25, 0.5);
        let origin = position.origin(track);
        assert_eq!(origin, point(px(110.), px(120.)));
        assert_eq!(HudPosition::from_origin(origin, track), position);
        // Past the edges clamps.
        let far = HudPosition::from_origin(point(px(900.), px(-50.)), track);
        assert_eq!(far, HudPosition::new(1.0, 0.0));
    }

    #[test]
    fn gap_reads_signed_and_clamps_to_eight_metres() {
        assert_eq!(gap_position(4.0), 0.5);
        assert_eq!(gap_position(-20.0), -1.0);
        assert_eq!(gap_position(f64::NAN), 0.0);
        assert_eq!(format_gap(2.44).as_ref(), "+2.4 m");
        assert_eq!(format_gap(-1.0).as_ref(), "−1.0 m");
        assert_eq!(format_gap(0.01).as_ref(), "0.0 m");
    }

    #[test]
    fn gears_read_as_drivers_say_them() {
        assert_eq!(format_gear(Some(4)).as_ref(), "4");
        assert_eq!(format_gear(Some(0)).as_ref(), "N");
        assert_eq!(format_gear(Some(-1)).as_ref(), "R");
        assert_eq!(format_gear(None).as_ref(), MISSING_VALUE);
    }
}
