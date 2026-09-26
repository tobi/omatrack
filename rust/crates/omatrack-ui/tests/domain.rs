//! UI integration tests for the lap strip and the video HUD: real
//! components in a headless window under `Root`, driven by native pointer
//! events.

#![cfg(test)]

use std::sync::Arc;

use gpui_kit::component::Root;
use gpui_kit::prelude::*;
use gpui_kit::test::{TestSupportExt as _, TestWindowExt as _};
use gpui_kit::{
    AnyWindowHandle, Context, Entity, InputEvent as _, Modifiers, MouseButton, MouseDownEvent,
    MouseMoveEvent, MouseUpEvent, PlatformInput, Point, TestAppContext, Window, div, point, px,
    size,
};
use omatrack_ui::{
    HUD_INSET, HudPosition, LapRole, LapSelect, LapStrip, LapStripItem, VideoHud, lap_strip_layout,
    theme,
};

fn init(cx: &mut TestAppContext) {
    cx.update(|cx| {
        gpui_kit::init(cx);
        theme::install(theme::ThemeSource::none(), cx);
    });
}

fn dispatch(cx: &mut TestAppContext, window: AnyWindowHandle, event: PlatformInput) {
    cx.update_window(window, |_, window, cx| {
        window.dispatch_event(event, cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(window, |_, window, cx| window.render_frame(cx))
        .unwrap();
}

fn press(cx: &mut TestAppContext, window: AnyWindowHandle, at: Point<gpui_kit::Pixels>, alt: bool) {
    let modifiers = Modifiers {
        alt,
        ..Modifiers::default()
    };
    dispatch(
        cx,
        window,
        MouseDownEvent {
            button: MouseButton::Left,
            position: at,
            modifiers,
            click_count: 1,
            first_mouse: false,
        }
        .to_platform_input(),
    );
}

fn release(
    cx: &mut TestAppContext,
    window: AnyWindowHandle,
    at: Point<gpui_kit::Pixels>,
    alt: bool,
) {
    let modifiers = Modifiers {
        alt,
        ..Modifiers::default()
    };
    dispatch(
        cx,
        window,
        MouseUpEvent {
            button: MouseButton::Left,
            position: at,
            modifiers,
            click_count: 1,
        }
        .to_platform_input(),
    );
}

fn move_to(cx: &mut TestAppContext, window: AnyWindowHandle, at: Point<gpui_kit::Pixels>) {
    dispatch(
        cx,
        window,
        MouseMoveEvent {
            position: at,
            pressed_button: Some(MouseButton::Left),
            modifiers: Modifiers::default(),
        }
        .to_platform_input(),
    );
}

// ---- lap strip --------------------------------------------------------------

struct Session {
    items: Arc<[LapStripItem]>,
    role: LapRole,
    primary: Option<i32>,
    reference: Option<i32>,
    requests: Vec<LapSelect>,
}

impl Render for Session {
    fn render(&mut self, _: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let this = cx.entity().downgrade();
        div().size_full().p_4().child(
            LapStrip::new("lap-strip", self.items.clone())
                .role(self.role)
                .primary(self.primary)
                .reference(self.reference)
                .primary_playhead(Some(0.4))
                .on_select(move |request, _, cx| {
                    _ = this.update(cx, |this, cx| {
                        this.requests.push(*request);
                        match request.role {
                            LapRole::Primary => this.primary = Some(request.lap_id),
                            LapRole::Reference => this.reference = Some(request.lap_id),
                        }
                        cx.notify();
                    });
                }),
        )
    }
}

fn session_items() -> Arc<[LapStripItem]> {
    Arc::from(vec![
        LapStripItem::new(0, "Out", 95.0),
        LapStripItem::new(1, "L1", 74.2).time("1:14.201"),
        LapStripItem::new(2, "L2", 73.6).time("1:13.644").best(true),
        LapStripItem::new(3, "In", 40.0),
        LapStripItem::new(4, "Pit", 0.0).pit_stop(true),
        LapStripItem::new(5, "Out", 30.0),
        LapStripItem::new(6, "L3", 75.0).time("1:15.020"),
    ])
}

fn open_strip(cx: &mut TestAppContext) -> (AnyWindowHandle, Entity<Session>) {
    open_strip_as(cx, LapRole::Primary)
}

fn open_strip_as(cx: &mut TestAppContext, role: LapRole) -> (AnyWindowHandle, Entity<Session>) {
    init(cx);
    let mut view = None;
    let handle = cx.open_window(size(px(1000.), px(120.)), |window, cx| {
        let session = cx.new(|_| Session {
            items: session_items(),
            role,
            primary: Some(1),
            reference: None,
            requests: Vec::new(),
        });
        view = Some(session.clone());
        Root::new(session, window, cx)
    });
    let window: AnyWindowHandle = handle.into();
    cx.update_window(window, |_, window, cx| window.render_frame(cx))
        .unwrap();
    (window, view.unwrap())
}

fn cell(id: u32) -> (&'static str, u32) {
    ("lap-strip-cell", id)
}

#[gpui_kit::test]
#[expect(
    clippy::cast_possible_truncation,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn lap_strip_cells_follow_the_layout(cx: &mut TestAppContext) {
    let (window, _) = open_strip(cx);
    cx.update_window(window, |_, window, _| {
        let strip = window.find("lap-strip");
        assert_eq!(strip.label(), Some("Laps: 7 laps"));
        let width = strip.bounds().size.width.as_f32();
        let spans = lap_strip_layout(width, &session_items());
        for (id, span) in spans.iter().enumerate() {
            let bounds = window.find(cell(id as u32)).bounds();
            let x = (bounds.origin.x - strip.bounds().origin.x).as_f32();
            assert!((x - span.x).abs() < 0.51, "cell {id}: x {x} vs {}", span.x);
            assert!(
                (bounds.size.width.as_f32() - span.width).abs() < 0.51,
                "cell {id}: width {} vs {}",
                bounds.size.width.as_f32(),
                span.width
            );
        }
        // The pit stop is one fixed cell, the laps share the rest by time.
        assert_eq!(window.find(cell(4)).bounds().size.width, px(36.));
        assert!(
            window.find(cell(1)).bounds().size.width > window.find(cell(5)).bounds().size.width
        );
        // Accessible names carry the role and the best lap.
        assert_eq!(window.find(cell(1)).label(), Some("L1 1:14.201, primary"));
        assert_eq!(window.find(cell(2)).label(), Some("L2 1:13.644, best lap"));
    })
    .unwrap();
}

#[gpui_kit::test]
fn lap_strip_click_selects_primary_and_alt_click_reference(cx: &mut TestAppContext) {
    let (window, session) = open_strip(cx);
    cx.update_window(window, |_, window, cx| window.click(cell(2), cx))
        .unwrap();
    cx.run_until_parked();
    cx.update(|cx| {
        let session = session.read(cx);
        assert_eq!(session.requests, vec![LapSelect::new(2, LapRole::Primary)]);
        assert_eq!(session.primary, Some(2));
    });

    let centre = cx
        .update_window(window, |_, window, _| {
            window.find(cell(6)).bounds().center()
        })
        .unwrap();
    press(cx, window, centre, true);
    release(cx, window, centre, true);
    cx.update(|cx| {
        let session = session.read(cx);
        assert_eq!(session.requests.last(), Some(&LapSelect::compare(6)));
        assert_eq!(session.reference, Some(6));
        assert_eq!(
            session.primary,
            Some(2),
            "alt-click leaves the primary alone"
        );
    });
    cx.update_window(window, |_, window, _| {
        assert_eq!(window.find(cell(6)).label(), Some("L3 1:15.020, reference"));
    })
    .unwrap();
}

fn right_click(cx: &mut TestAppContext, window: AnyWindowHandle, at: Point<gpui_kit::Pixels>) {
    dispatch(
        cx,
        window,
        MouseDownEvent {
            button: MouseButton::Right,
            position: at,
            modifiers: Modifiers::default(),
            click_count: 1,
            first_mouse: false,
        }
        .to_platform_input(),
    );
    dispatch(
        cx,
        window,
        MouseUpEvent {
            button: MouseButton::Right,
            position: at,
            modifiers: Modifiers::default(),
            click_count: 1,
        }
        .to_platform_input(),
    );
}

#[gpui_kit::test]
fn a_reference_strip_clicks_for_its_role_and_right_click_compares(cx: &mut TestAppContext) {
    let (window, session) = open_strip_as(cx, LapRole::Reference);
    // A plain click asks for the strip's own role.
    cx.update_window(window, |_, window, cx| window.click(cell(2), cx))
        .unwrap();
    cx.run_until_parked();
    cx.update(|cx| {
        let session = session.read(cx);
        assert_eq!(
            session.requests,
            vec![LapSelect::new(2, LapRole::Reference)]
        );
        assert!(!session.requests[0].secondary);
    });

    // A right click compares against the lap, whatever the strip's role.
    let centre = cx
        .update_window(window, |_, window, _| {
            window.find(cell(6)).bounds().center()
        })
        .unwrap();
    right_click(cx, window, centre);
    cx.update(|cx| {
        let session = session.read(cx);
        assert_eq!(session.requests.last(), Some(&LapSelect::compare(6)));
        assert!(session.requests.last().unwrap().secondary);
        assert_eq!(session.requests.len(), 2, "one request per right click");
    });
}

// ---- video HUD --------------------------------------------------------------

struct VideoPane {
    gap: Option<f64>,
    position: HudPosition,
    moves: Vec<HudPosition>,
}

impl Render for VideoPane {
    fn render(&mut self, _: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let this = cx.entity().downgrade();
        div().size_full().p_4().child(
            div()
                .id("video-pane")
                .test_support()
                .relative()
                .size_full()
                .child(
                    VideoHud::new("hud")
                        .speed(Some(187.46))
                        .gear(Some(4))
                        .delta(Some(-0.123))
                        .gap(self.gap)
                        .position(self.position)
                        .on_moved(move |position, _, cx| {
                            _ = this.update(cx, |this, cx| {
                                this.moves.push(*position);
                                this.position = *position;
                                cx.notify();
                            });
                        }),
                ),
        )
    }
}

fn open_pane(cx: &mut TestAppContext, gap: Option<f64>) -> (AnyWindowHandle, Entity<VideoPane>) {
    init(cx);
    let mut view = None;
    let handle = cx.open_window(size(px(900.), px(560.)), |window, cx| {
        let pane = cx.new(|_| VideoPane {
            gap,
            position: HudPosition::new(0.0, 0.0),
            moves: Vec::new(),
        });
        view = Some(pane.clone());
        Root::new(pane, window, cx)
    });
    let window: AnyWindowHandle = handle.into();
    for _ in 0..2 {
        cx.update_window(window, |_, window, cx| window.render_frame(cx))
            .unwrap();
    }
    (window, view.unwrap())
}

#[gpui_kit::test]
fn video_hud_hides_the_gap_bar_without_a_gap(cx: &mut TestAppContext) {
    let (window, _) = open_pane(cx, None);
    cx.update_window(window, |_, window, _| {
        let card = window.find("hud-card");
        assert!(card.visible());
        assert_eq!(card.label(), Some("Speed 187 km/h, gear 4, delta -0.123 s"));
        assert!(window.try_find("hud-gap").is_none());
    })
    .unwrap();
}

#[gpui_kit::test]
fn video_hud_shows_a_precise_gap(cx: &mut TestAppContext) {
    let (window, _) = open_pane(cx, Some(2.44));
    cx.update_window(window, |_, window, _| {
        let gap = window.find("hud-gap");
        assert!(gap.visible());
        assert_eq!(gap.label(), Some("Gap +2.4 m"));
        // The bar sits inside the card.
        let card = window.find("hud-card").bounds();
        assert!(card.contains(&gap.bounds().center()));
    })
    .unwrap();
}

#[gpui_kit::test]
fn video_hud_drag_reports_a_normalized_position_at_drag_end(cx: &mut TestAppContext) {
    let (window, pane) = open_pane(cx, None);
    let (card, pane_bounds) = cx
        .update_window(window, |_, window, _| {
            (
                window.find("hud-card").bounds(),
                window.find("video-pane").bounds(),
            )
        })
        .unwrap();
    // Starts in the top-left corner of the pane, `HUD_INSET` in from both
    // edges.
    let inset = cx
        .update_window(window, |_, window, _| {
            gpui_kit::rems(HUD_INSET).to_pixels(window.rem_size())
        })
        .unwrap();
    assert!(inset > px(0.));
    assert!((card.origin.x - pane_bounds.origin.x - inset).abs() < px(0.5));
    assert!((card.origin.y - pane_bounds.origin.y - inset).abs() < px(0.5));

    let track_width = pane_bounds.size.width - card.size.width - inset * 2.;
    let track_height = pane_bounds.size.height - card.size.height - inset * 2.;
    let grab = card.center();
    let target = grab + point(track_width * 0.5, track_height * 0.25);
    press(cx, window, grab, false);
    move_to(cx, window, grab + point(px(40.), px(10.)));
    move_to(cx, window, target);
    // Nothing is reported while dragging, but the card follows.
    assert!(cx.update(|cx| pane.read(cx).moves.is_empty()));
    let live = cx
        .update_window(window, |_, window, _| window.find("hud-card").bounds())
        .unwrap();
    assert!((live.center().x - target.x).abs() < px(1.), "{live:?}");
    release(cx, window, target, false);

    let moves = cx.update(|cx| pane.read(cx).moves.clone());
    assert_eq!(moves.len(), 1, "{moves:?}");
    assert!((moves[0].x - 0.5).abs() < 0.01, "{moves:?}");
    assert!((moves[0].y - 0.25).abs() < 0.01, "{moves:?}");

    // Dragging past the edge clamps to the available space.
    let card = cx
        .update_window(window, |_, window, _| window.find("hud-card").bounds())
        .unwrap();
    let corner = pane_bounds.bottom_right() - point(px(2.), px(2.));
    press(cx, window, card.center(), false);
    move_to(cx, window, corner);
    release(cx, window, corner, false);
    let last = cx.update(|cx| *pane.read(cx).moves.last().unwrap());
    assert_eq!(last, HudPosition::new(1.0, 1.0));
}
