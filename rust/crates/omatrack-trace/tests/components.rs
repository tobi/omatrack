//! UI integration tests for the corner ruler, damper strip and track map:
//! real entities in a headless window under `Root`, driven by native pointer
//! events, asserting the events their owners receive.

#![cfg(test)]

use std::cell::RefCell;
use std::f64::consts::TAU;
use std::rc::Rc;
use std::sync::Arc;

use gpui_kit::component::Root;
use gpui_kit::prelude::*;
use gpui_kit::test::TestWindowExt as _;
use gpui_kit::{
    AnyWindowHandle, Bounds, Context, Entity, EventEmitter, InputEvent as _, Modifiers,
    MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, Pixels, PlatformInput, Point,
    ScrollDelta, ScrollWheelEvent, TestAppContext, TouchPhase, Window, div, point, px, size,
};
use omatrack_trace::{
    ComplexBand, CornerBand, CornerRuler, CornerRulerEvent, CursorState, DamperStrip,
    DamperStripData, DamperStripEvent, GeoPoint, GpsTrack, MapCorner, TrackMap, TrackMapData,
    TrackMapEvent, ViewportState,
};

// ---- harness ------------------------------------------------------------------

/// Hosts one component view and records the events it emits.
struct Host<V: 'static> {
    view: Entity<V>,
}

impl<V: Render> Render for Host<V> {
    fn render(&mut self, _: &mut Window, _: &mut Context<'_, Self>) -> impl IntoElement {
        div()
            .size_full()
            .p_4()
            .flex()
            .flex_col()
            .child(self.view.clone())
    }
}

fn open<V, E>(
    cx: &mut TestAppContext,
    window_size: gpui_kit::Size<Pixels>,
    build: impl FnOnce(&mut Window, &mut gpui_kit::App) -> Entity<V>,
) -> (AnyWindowHandle, Entity<V>, Rc<RefCell<Vec<E>>>)
where
    V: Render + EventEmitter<E>,
    E: Clone + 'static,
{
    cx.update(gpui_kit::init);
    cx.update(|cx| cx.set_reduce_motion(true));
    let mut view = None;
    let handle = cx.open_window(window_size, |window, cx| {
        let component = build(window, cx);
        view = Some(component.clone());
        let host = cx.new(|_| Host { view: component });
        Root::new(host, window, cx)
    });
    let view = view.unwrap();
    let events = Rc::new(RefCell::new(Vec::new()));
    let sink = events.clone();
    cx.update(|cx| {
        cx.subscribe(&view, move |_, event: &E, _| {
            sink.borrow_mut().push(event.clone());
        })
        .detach();
    });
    let window: AnyWindowHandle = handle.into();
    for _ in 0..2 {
        cx.update_window(window, |_, window, cx| window.render_frame(cx))
            .unwrap();
        cx.run_until_parked();
    }
    (window, view, events)
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

fn press(cx: &mut TestAppContext, window: AnyWindowHandle, at: Point<Pixels>, clicks: usize) {
    dispatch(
        cx,
        window,
        MouseDownEvent {
            button: MouseButton::Left,
            position: at,
            modifiers: Modifiers::default(),
            click_count: clicks,
            first_mouse: false,
        }
        .to_platform_input(),
    );
}

fn move_to(cx: &mut TestAppContext, window: AnyWindowHandle, at: Point<Pixels>, pressed: bool) {
    dispatch(
        cx,
        window,
        MouseMoveEvent {
            position: at,
            pressed_button: pressed.then_some(MouseButton::Left),
            modifiers: Modifiers::default(),
        }
        .to_platform_input(),
    );
}

fn release(cx: &mut TestAppContext, window: AnyWindowHandle, at: Point<Pixels>) {
    dispatch(
        cx,
        window,
        MouseUpEvent {
            button: MouseButton::Left,
            position: at,
            modifiers: Modifiers::default(),
            click_count: 1,
        }
        .to_platform_input(),
    );
}

fn bounds_of(cx: &mut TestAppContext, window: AnyWindowHandle, id: &'static str) -> Bounds<Pixels> {
    cx.update_window(window, |_, window, _| window.find(id).bounds())
        .unwrap()
}

/// A point at lap fraction `fraction` along `bounds`, `from_bottom` pixels
/// above its bottom edge.
fn at_fraction(bounds: Bounds<Pixels>, fraction: f32, from_bottom: f32) -> Point<Pixels> {
    point(
        bounds.origin.x + bounds.size.width * fraction,
        bounds.bottom() - px(from_bottom),
    )
}

// ---- corner ruler ---------------------------------------------------------------

fn corners() -> Vec<CornerBand> {
    vec![
        CornerBand::new(1, "T1", 0.20, 0.30),
        CornerBand::new(2, "T2", 0.50, 0.60),
        CornerBand::new(3, "T3", 0.62, 0.70),
    ]
}

fn open_ruler(
    cx: &mut TestAppContext,
) -> (
    AnyWindowHandle,
    Entity<CornerRuler>,
    Rc<RefCell<Vec<CornerRulerEvent>>>,
) {
    open(cx, size(px(1032.), px(200.)), |_, cx| {
        let viewport = cx.new(|_| ViewportState::new());
        cx.new(|cx| {
            let mut ruler = CornerRuler::new(viewport, cx);
            ruler.set_corners(corners(), vec![ComplexBand::new("Esses", 0.50, 0.70)], cx);
            ruler
        })
    })
}

#[gpui_kit::test]
fn corner_ruler_click_emits_corner_clicked(cx: &mut TestAppContext) {
    let (window, ruler, events) = open_ruler(cx);
    let bounds = bounds_of(cx, window, "corner-ruler");
    assert_eq!(bounds.size.width, px(1000.));
    let label = cx
        .update_window(window, |_, window, _| {
            window.find("corner-ruler").label().map(str::to_string)
        })
        .unwrap();
    assert_eq!(label.as_deref(), Some("Corners: T1, T2, T3"));

    let t2 = at_fraction(bounds, 0.55, 6.0);
    press(cx, window, t2, 1);
    release(cx, window, t2);
    assert_eq!(*events.borrow(), vec![CornerRulerEvent::CornerClicked(2)]);

    // Focus is controlled by the owner; the ruler announces it.
    ruler.update(cx, |r, cx| r.set_focused_corner(Some(2), cx));
    cx.update_window(window, |_, window, cx| window.render_frame(cx))
        .unwrap();
    let label = cx
        .update_window(window, |_, window, _| {
            window.find("corner-ruler").label().map(str::to_string)
        })
        .unwrap();
    assert_eq!(label.as_deref(), Some("Corners: T1, T2, T3; focused T2"));

    // A press between corners, or a press that drags away, is not a click.
    let gap = at_fraction(bounds, 0.40, 6.0);
    press(cx, window, gap, 1);
    release(cx, window, gap);
    press(cx, window, t2, 1);
    move_to(cx, window, t2 + point(px(30.), px(0.)), true);
    release(cx, window, t2 + point(px(30.), px(0.)));
    assert_eq!(events.borrow().len(), 1, "{:?}", events.borrow());
}

#[gpui_kit::test]
#[expect(
    clippy::float_cmp,
    reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
)]
fn corner_ruler_edit_drag_emits_corner_edited(cx: &mut TestAppContext) {
    let (window, ruler, events) = open_ruler(cx);
    ruler.update(cx, |r, cx| r.set_editing(true, cx));
    cx.update_window(window, |_, window, cx| window.render_frame(cx))
        .unwrap();
    let bounds = bounds_of(cx, window, "corner-ruler");

    // Grab T1's start edge 2 px inside and drag it 50 px left.
    let edge = at_fraction(bounds, 0.20, 6.0) + point(px(2.), px(0.));
    press(cx, window, edge, 1);
    assert!(cx.update(|cx| ruler.read(cx).is_dragging()));
    move_to(cx, window, edge - point(px(50.), px(0.)), true);
    release(cx, window, edge - point(px(50.), px(0.)));

    let events = events.borrow();
    let Some(CornerRulerEvent::CornerEdited { id, start, end }) = events.last().cloned() else {
        panic!("{events:?}");
    };
    assert_eq!(id, 1);
    assert!((start - 0.152).abs() < 1e-3, "{start}");
    assert_eq!(end, 0.30);
    // The displayed zone follows; edits never click.
    let shown = cx.update(|cx| ruler.read(cx).corners()[0].clone());
    assert!((shown.start - start).abs() < 1e-12);
    assert!(
        !events
            .iter()
            .any(|e| matches!(e, CornerRulerEvent::CornerClicked(_)))
    );
    assert!(!cx.update(|cx| ruler.read(cx).is_dragging()));
}

// ---- damper strip ---------------------------------------------------------------

const LAP_SECONDS: f64 = 90.0;

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn damper_series(phase: f64) -> Arc<[f64]> {
    let n = (LAP_SECONDS * 50.0) as usize + 1;
    (0..n)
        .map(|i| {
            let t = i as f64 / 50.0;
            20.0 + 6.0 * (TAU * t / 1.7 + phase).sin() + 2.0 * (TAU * t / 0.35).sin()
        })
        .collect()
}

fn open_damper(
    cx: &mut TestAppContext,
) -> (
    AnyWindowHandle,
    Entity<DamperStrip>,
    Rc<RefCell<Vec<DamperStripEvent>>>,
) {
    open(cx, size(px(1200.), px(160.)), |_, cx| {
        let cursor = cx.new(|_| CursorState::new());
        cursor.update(cx, |c, cx| c.set_fraction(Some(0.5), cx));
        cx.new(|cx| {
            let mut strip = DamperStrip::new(cursor, cx);
            strip.set_data(
                Some(Arc::new(DamperStripData::new(
                    damper_series(0.0),
                    damper_series(0.4),
                    LAP_SECONDS,
                ))),
                cx,
            );
            strip
        })
    })
}

fn last_offset(events: &Rc<RefCell<Vec<DamperStripEvent>>>) -> (f64, f64) {
    match events.borrow().last() {
        Some(DamperStripEvent::OffsetChanged { seconds, fraction }) => (*seconds, *fraction),
        other => panic!("{other:?}"),
    }
}

#[gpui_kit::test]
fn damper_strip_drag_slides_the_reference(cx: &mut TestAppContext) {
    let (window, strip, events) = open_damper(cx);
    let plot = bounds_of(cx, window, "damper-strip-plot");
    let width = plot.size.width;
    let from = plot.center();
    let to = from + point(width * 0.25, px(0.));
    press(cx, window, from, 1);
    move_to(cx, window, from + point(px(20.), px(0.)), true);
    move_to(cx, window, to, true);
    release(cx, window, to);

    // A quarter of the default 6 s window: the reference moves 1.5 s later.
    let (seconds, fraction) = last_offset(&events);
    assert!((seconds - 1.5).abs() < 1e-6, "{seconds}");
    assert!((fraction - 1.5 / LAP_SECONDS).abs() < 1e-9, "{fraction}");
    assert!(events.borrow().len() >= 2, "every drag step reports");
    assert!((cx.update(|cx| strip.read(cx).offset_seconds()) - 1.5).abs() < 1e-6);
    let readout = cx
        .update_window(window, |_, window, _| {
            window.find("damper-offset").label().map(str::to_string)
        })
        .unwrap();
    assert_eq!(readout.as_deref(), Some("Reference offset +1.500 s"));

    // The nudge buttons move one 20 ms sample.
    cx.update_window(window, |_, window, cx| {
        window.click("damper-nudge-later", cx);
    })
    .unwrap();
    assert!((last_offset(&events).0 - 1.52).abs() < 1e-6);
    cx.update_window(window, |_, window, cx| {
        window.click("damper-nudge-earlier", cx);
        window.click("damper-nudge-earlier", cx);
    })
    .unwrap();
    assert!((last_offset(&events).0 - 1.48).abs() < 1e-6);
}

#[gpui_kit::test]
#[expect(
    clippy::float_cmp,
    reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
)]
fn damper_strip_double_click_resets_the_offset(cx: &mut TestAppContext) {
    let (window, strip, events) = open_damper(cx);
    strip.update(cx, |s, cx| s.set_offset_seconds(0.8, cx));
    cx.update_window(window, |_, window, cx| window.render_frame(cx))
        .unwrap();
    assert!((last_offset(&events).0 - 0.8).abs() < 1e-9);

    let plot = bounds_of(cx, window, "damper-strip-plot");
    press(cx, window, plot.center(), 1);
    release(cx, window, plot.center());
    press(cx, window, plot.center(), 2);
    release(cx, window, plot.center());
    assert_eq!(last_offset(&events), (0.0, 0.0));
    assert_eq!(cx.update(|cx| strip.read(cx).offset()), 0.0);
    // A plain click moves nothing.
    assert_eq!(events.borrow().len(), 2, "{:?}", events.borrow());
}

#[gpui_kit::test]
#[expect(
    clippy::float_cmp,
    reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
)]
fn damper_strip_wheel_zooms_between_one_second_and_the_lap(cx: &mut TestAppContext) {
    let (window, strip, _) = open_damper(cx);
    let plot = bounds_of(cx, window, "damper-strip-plot");
    let wheel = |cx: &mut TestAppContext, lines: f32| {
        dispatch(
            cx,
            window,
            ScrollWheelEvent {
                position: plot.center(),
                delta: ScrollDelta::Lines(point(0., lines)),
                modifiers: Modifiers::default(),
                touch_phase: TouchPhase::Moved,
            }
            .to_platform_input(),
        );
    };
    wheel(cx, 1.0);
    assert!((cx.update(|cx| strip.read(cx).window_seconds()) - 4.8).abs() < 1e-9);
    for _ in 0..30 {
        wheel(cx, 1.0);
    }
    assert_eq!(cx.update(|cx| strip.read(cx).window_seconds()), 1.0);
    for _ in 0..60 {
        wheel(cx, -1.0);
    }
    assert_eq!(cx.update(|cx| strip.read(cx).window_seconds()), LAP_SECONDS);
}

// ---- track map ------------------------------------------------------------------

const METERS_PER_DEGREE: f64 = 111_319.490_793_273_57;

/// A 300 m-radius circle, anticlockwise from east.
#[expect(
    clippy::cast_precision_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn circle(samples: usize, radius_m: f64) -> Vec<GeoPoint> {
    let (lon0, lat0): (f64, f64) = (-83.81, 34.15);
    (0..samples)
        .map(|i| {
            let a = TAU * i as f64 / (samples - 1) as f64;
            GeoPoint::new(
                lon0 + radius_m * a.cos() / (METERS_PER_DEGREE * lat0.to_radians().cos()),
                lat0 + radius_m * a.sin() / METERS_PER_DEGREE,
            )
        })
        .collect()
}

fn map_data() -> TrackMapData {
    let lap = circle(4501, 300.0);
    let latitude: Arc<[f64]> = lap.iter().map(|p| p.lat).collect();
    let longitude: Arc<[f64]> = lap.iter().map(|p| p.lon).collect();
    let reference = circle(4401, 302.0);
    let delta: Arc<[f64]> = (0..4501)
        .map(|i| 0.4 * (TAU * f64::from(i) / 4500.0).sin())
        .collect();
    TrackMapData::new()
        .with_centerline(circle(200, 300.0))
        .with_primary(Some(GpsTrack::new(latitude, longitude)))
        .with_reference(Some(GpsTrack::new(
            reference.iter().map(|p| p.lat).collect(),
            reference.iter().map(|p| p.lon).collect(),
        )))
        .with_delta(Some(delta))
        .with_corners(vec![MapCorner::new(1, "T1", circle(9, 300.0)[2])])
}

type MapFixture = (
    AnyWindowHandle,
    Entity<TrackMap>,
    Entity<CursorState>,
    Rc<RefCell<Vec<TrackMapEvent>>>,
);

fn open_map(cx: &mut TestAppContext) -> MapFixture {
    let mut cursor = None;
    let (window, map, events) = open(cx, size(px(640.), px(520.)), |_, cx| {
        let shared = cx.new(|_| CursorState::new());
        cursor = Some(shared.clone());
        cx.new(|cx| {
            let mut map = TrackMap::new(shared, cx);
            map.set_data(Arc::new(map_data()), cx);
            map
        })
    });
    (window, map, cursor.unwrap(), events)
}

#[gpui_kit::test]
fn track_map_renders_and_reports_hover_and_clicks(cx: &mut TestAppContext) {
    let (window, map, cursor, events) = open_map(cx);
    let figure = cx
        .update_window(window, |_, window, _| {
            let figure = window.find("track-map");
            assert!(figure.visible());
            (figure.label().map(str::to_string), figure.bounds())
        })
        .unwrap();
    assert_eq!(figure.0.as_deref(), Some("Track map, 1 corner"));
    assert_eq!(cx.update(|cx| map.read(cx).geometry_builds()), 1);
    let static_renders = cx.update(|cx| map.read(cx).static_renders());
    assert!(static_renders >= 1);

    // The quarter-lap point lands inside the figure, north of the centre.
    let quarter = cx.update(|cx| map.read(cx).primary_position(0.25)).unwrap();
    assert!(figure.1.contains(&quarter));
    assert!(quarter.y < figure.1.center().y, "north is up");

    move_to(cx, window, quarter + point(px(3.), px(-2.)), false);
    let hover = match events.borrow().last() {
        Some(TrackMapEvent::MapHover(Some(fraction))) => *fraction,
        other => panic!("{other:?}"),
    };
    assert!((hover - 0.25).abs() < 0.01, "{hover}");

    press(cx, window, quarter, 1);
    release(cx, window, quarter);
    match events.borrow().last() {
        Some(TrackMapEvent::MapClicked(fraction)) => {
            assert!((fraction - 0.25).abs() < 0.005, "{fraction}");
        }
        other => panic!("{other:?}"),
    }

    // Far from the lap (the empty middle of the circle) there is nothing.
    move_to(cx, window, figure.1.center(), false);
    assert_eq!(events.borrow().last(), Some(&TrackMapEvent::MapHover(None)));

    // Cursor moves repaint the dots, never the geometry.
    for fraction in [0.1, 0.4, 0.8] {
        cursor.update(cx, |c, cx| c.set_fraction(Some(fraction), cx));
        cx.run_until_parked();
        cx.update_window(window, |_, window, cx| window.render_frame(cx))
            .unwrap();
    }
    assert_eq!(cx.update(|cx| map.read(cx).geometry_builds()), 1);

    // Static and overlay are separate passes: an ordinary frame after a
    // cursor move or a hover replays the meshes from GPUI's view cache
    // (`render_frame` above forces a full refresh, `draw` does not).
    let draw = |cx: &mut TestAppContext| {
        cx.update_window(window, |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
    };
    draw(cx);
    let before = cx.update(|cx| map.read(cx).static_renders());
    for fraction in [0.2, 0.5, 0.9] {
        cursor.update(cx, |c, cx| c.set_fraction(Some(fraction), cx));
        cx.run_until_parked();
        draw(cx);
    }
    // (`move_to` would force a refresh too.)
    cx.update_window(window, |_, window, cx| {
        window.dispatch_event(
            MouseMoveEvent {
                position: quarter,
                pressed_button: None,
                modifiers: Modifiers::default(),
            }
            .to_platform_input(),
            cx,
        );
    })
    .unwrap();
    cx.run_until_parked();
    draw(cx);
    assert!(matches!(
        events.borrow().last(),
        Some(TrackMapEvent::MapHover(Some(_)))
    ));
    assert_eq!(
        cx.update(|cx| map.read(cx).static_renders()),
        before,
        "the static layer re-rendered on a cursor or hover frame"
    );
    // New data does re-render it.
    map.update(cx, |m, cx| m.set_data(Arc::new(map_data()), cx));
    draw(cx);
    assert_eq!(cx.update(|cx| map.read(cx).static_renders()), before + 1);
    assert_eq!(cx.update(|cx| map.read(cx).geometry_builds()), 2);
}

#[gpui_kit::test]
fn track_map_without_data_shows_an_empty_state(cx: &mut TestAppContext) {
    let (window, map, _, _) = open_map(cx);
    map.update(cx, |m, cx| m.set_data(Arc::new(TrackMapData::new()), cx));
    cx.update_window(window, |_, window, cx| {
        window.render_frame(cx);
        assert_eq!(
            window.find("track-map").label(),
            Some("Track map: no GPS or track layout")
        );
    })
    .unwrap();
    assert!(cx.update(|cx| map.read(cx).primary_position(0.5)).is_none());
}
