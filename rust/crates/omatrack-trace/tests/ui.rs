//! UI integration tests for the trace stack: real `TraceStack` in a
//! headless window under `Root`, driven by native pointer events.

use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;

use gpui_kit::component::{Root, Theme, ThemeMode};
use gpui_kit::test::TestWindowExt as _;
use gpui_kit::{
    AnyWindowHandle, AppContext as _, Bounds, Entity, Focusable as _, InputEvent as _, Modifiers,
    MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, Pixels, Point, ScrollDelta,
    ScrollWheelEvent, TestAppContext, TouchPhase, Window, point, px, size,
};
use omatrack_trace::layout::LaneSizing;
use omatrack_trace::{
    CursorState, LaneStyle, LaneStyles, TraceEvent, TraceStack, Viewport, ViewportState, synthetic,
};

struct Fixture {
    window: AnyWindowHandle,
    stack: Entity<TraceStack>,
    viewport: Entity<ViewportState>,
    cursor: Entity<CursorState>,
    events: Rc<RefCell<Vec<TraceEvent>>>,
}

/// Draw without `Window::refresh`, so cached views stay cached (the kit's
/// `render_frame` refreshes, which deliberately bypasses view caching).
fn draw(window: &mut Window, cx: &mut gpui_kit::App) {
    window.draw(cx).clear(cx);
}

fn open(cx: &mut TestAppContext) -> Fixture {
    cx.update(gpui_kit::init);
    cx.update(|cx| cx.set_reduce_motion(true));
    let viewport = cx.new(|_| ViewportState::new());
    let cursor = cx.new(|_| CursorState::new());
    let scene = Arc::new(synthetic::scene());
    let mut stack = None;
    let handle = cx.open_window(size(px(1400.), px(900.)), |window, cx| {
        let view =
            cx.new(|cx| TraceStack::new(scene, viewport.clone(), cursor.clone(), window, cx));
        stack = Some(view.clone());
        Root::new(view, window, cx)
    });
    let stack = stack.unwrap();
    let events = Rc::new(RefCell::new(Vec::new()));
    let sink = events.clone();
    cx.update(|cx| {
        cx.subscribe(&stack, move |_, event: &TraceEvent, _| {
            sink.borrow_mut().push(event.clone())
        })
        .detach();
    });
    let window: AnyWindowHandle = handle.into();
    // First frame measures the plot; the deferred layout lands in the next.
    for _ in 0..3 {
        cx.update_window(window, |_, window, cx| window.render_frame(cx))
            .unwrap();
        cx.run_until_parked();
    }
    Fixture {
        window,
        stack,
        viewport,
        cursor,
        events,
    }
}

fn plot_bounds(cx: &mut TestAppContext, window: AnyWindowHandle) -> Bounds<Pixels> {
    cx.update_window(window, |_, window, _| window.find("trace-plot").bounds())
        .unwrap()
}

fn at(bounds: Bounds<Pixels>, fx: f32, fy: f32) -> Point<Pixels> {
    bounds.origin + point(bounds.size.width * fx, bounds.size.height * fy)
}

fn dispatch(cx: &mut TestAppContext, window: AnyWindowHandle, event: gpui_kit::PlatformInput) {
    cx.update_window(window, |_, window, cx| {
        window.dispatch_event(event, cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(window, |_, window, cx| draw(window, cx))
        .unwrap();
}

#[gpui_kit::test]
fn lanes_render_with_labels_and_stable_ids(cx: &mut TestAppContext) {
    let f = open(cx);
    let layout_lanes = cx.update(|cx| f.stack.read(cx).layout().slots.len());
    // 8 channels, brake shares the throttle lane.
    assert_eq!(layout_lanes, 7);
    cx.update_window(f.window, |_, window, _| {
        let speed = window.find("lane-speed");
        assert!(speed.visible());
        assert!(speed.label().unwrap().starts_with("Speed"));
        let throttle = window.find("lane-throttle");
        assert!(throttle.label().unwrap().starts_with("Throttle / Brake"));
        assert!(window.try_find("lane-brake").is_none());
        // Lanes stack top to bottom in the chrome column.
        assert!(window.find("lane-throttle").bounds().top() >= speed.bounds().bottom() - px(1.));
    })
    .unwrap();
    let stats = cx.update(|cx| f.stack.read(cx).static_stats(cx));
    assert!(stats.geometry_builds >= 8, "{stats:?}");
    assert!(stats.vertices > 10_000, "{stats:?}");
}

#[gpui_kit::test]
fn cursor_moves_leave_the_static_layer_alone(cx: &mut TestAppContext) {
    let f = open(cx);
    let before = cx.update(|cx| f.stack.read(cx).static_stats(cx));
    let label_before = cx
        .update_window(f.window, |_, window, _| {
            window.find("lane-speed").label().unwrap().to_string()
        })
        .unwrap();

    // Programmatic cursor move (video playback path).
    f.cursor
        .update(cx, |cursor, cx| cursor.set_fraction(Some(0.37), cx));
    cx.run_until_parked();
    cx.update_window(f.window, |_, window, cx| draw(window, cx))
        .unwrap();
    let label_after = cx
        .update_window(f.window, |_, window, _| {
            window.find("lane-speed").label().unwrap().to_string()
        })
        .unwrap();
    assert_ne!(label_before, label_after, "readouts follow the cursor");

    // Simulated pointer motion over the plot updates the hover readout.
    let plot = plot_bounds(cx, f.window);
    for fx in [0.2, 0.25, 0.3, 0.6] {
        dispatch(
            cx,
            f.window,
            MouseMoveEvent {
                position: at(plot, fx, 0.4),
                pressed_button: None,
                modifiers: Modifiers::default(),
            }
            .to_platform_input(),
        );
    }
    let hover = cx.update(|cx| f.cursor.read(cx).hover()).unwrap();
    assert!((hover - 0.6).abs() < 0.01, "{hover}");

    let after = cx.update(|cx| f.stack.read(cx).static_stats(cx));
    assert_eq!(
        after.renders, before.renders,
        "static layer re-rendered on cursor motion"
    );
    assert_eq!(after.geometry_builds, before.geometry_builds);
    assert_eq!(
        cx.update(|cx| f.stack.read(cx).static_rebuilds(cx)),
        before.renders
    );
}

#[gpui_kit::test]
fn wheel_zooms_about_the_pointer(cx: &mut TestAppContext) {
    let f = open(cx);
    let plot = plot_bounds(cx, f.window);
    let position = at(plot, 0.25, 0.5);
    cx.update_window(f.window, |_, window, cx| {
        window.scroll("trace-plot", ScrollDelta::Lines(point(0., 1.)), cx);
    })
    .unwrap();
    // The kit helper scrolls at the plot centre; also send one at 25%.
    dispatch(
        cx,
        f.window,
        ScrollWheelEvent {
            position,
            delta: ScrollDelta::Lines(point(0., 1.)),
            modifiers: Modifiers::default(),
            touch_phase: TouchPhase::Moved,
        }
        .to_platform_input(),
    );
    let view = cx.update(|cx| f.viewport.read(cx).viewport());
    assert!((view.span() - 0.64).abs() < 1e-6, "{view:?}");
    // The sample under the pointer stays under the pointer.
    let under = view.start + 0.25 * view.span();
    let previous = Viewport::FULL.zoom_about(0.5, 0.8);
    let expected = previous.start + 0.25 * previous.span();
    assert!((under - expected).abs() < 1e-3, "{under} vs {expected}");
    assert!(
        f.events
            .borrow()
            .iter()
            .any(|e| matches!(e, TraceEvent::ViewportChanged(_)))
    );
    // Horizontal trackpad motion pans.
    dispatch(
        cx,
        f.window,
        ScrollWheelEvent {
            position,
            delta: ScrollDelta::Pixels(point(px(-120.), px(0.))),
            modifiers: Modifiers::default(),
            touch_phase: TouchPhase::Moved,
        }
        .to_platform_input(),
    );
    let panned = cx.update(|cx| f.viewport.read(cx).viewport());
    assert!(panned.start > view.start);
    assert!((panned.span() - view.span()).abs() < 1e-12);
}

#[gpui_kit::test]
fn left_drag_selects_a_range(cx: &mut TestAppContext) {
    let f = open(cx);
    let plot = plot_bounds(cx, f.window);
    let from = at(plot, 0.2, 0.5);
    let to = at(plot, 0.6, 0.5);
    cx.update_window(f.window, |_, window, cx| window.drag(from, to, cx))
        .unwrap();
    cx.run_until_parked();
    let selected = f
        .events
        .borrow()
        .iter()
        .find_map(|e| match e {
            TraceEvent::RangeSelected(selection) => Some(*selection),
            _ => None,
        })
        .expect("RangeSelected");
    assert!((selected.start - 0.2).abs() < 0.01, "{selected:?}");
    assert!((selected.end - 0.6).abs() < 0.01, "{selected:?}");
    let cursor = cx.update(|cx| f.cursor.read(cx).fraction()).unwrap();
    assert!((cursor - 0.6).abs() < 0.01);
    assert_eq!(
        cx.update(|cx| f.cursor.read(cx).selection()),
        Some(selected)
    );
    // The press focused the stack for keyboard actions.
    let focused = cx.update(|cx| {
        let handle = f.stack.read(cx).focus_handle(cx);
        cx.update_window(f.window, |_, window, _| handle.is_focused(window))
            .unwrap()
    });
    assert!(focused);
}

#[gpui_kit::test]
fn double_click_resets_the_viewport(cx: &mut TestAppContext) {
    let f = open(cx);
    f.viewport
        .update(cx, |v, cx| v.set_viewport(Viewport::new(0.3, 0.4), cx));
    cx.run_until_parked();
    cx.update_window(f.window, |_, window, cx| {
        window.double_click("trace-plot", cx)
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(
        cx.update(|cx| f.viewport.read(cx).viewport()),
        Viewport::FULL
    );
}

#[gpui_kit::test]
fn vertical_wheel_scrolls_overflowing_lanes(cx: &mut TestAppContext) {
    let f = open(cx);
    let mut styles = LaneStyles::new();
    for key in [
        "speed", "throttle", "gear", "steering", "rpm", "g_long", "delta",
    ] {
        styles.set(
            key,
            LaneStyle::default_for(key)
                .with_sizing(LaneSizing::default().with_height_percent(40.0)),
        );
    }
    f.stack.update(cx, |stack, cx| {
        stack.set_lane_styles(styles, cx);
        stack.set_fit(false, cx);
    });
    cx.run_until_parked();
    cx.update_window(f.window, |_, window, cx| draw(window, cx))
        .unwrap();
    assert!(cx.update(|cx| f.stack.read(cx).layout().overflows()));
    let view = cx.update(|cx| f.viewport.read(cx).viewport());
    let plot = plot_bounds(cx, f.window);
    dispatch(
        cx,
        f.window,
        ScrollWheelEvent {
            position: at(plot, 0.5, 0.5),
            delta: ScrollDelta::Lines(point(0., -1.)),
            modifiers: Modifiers::default(),
            touch_phase: TouchPhase::Moved,
        }
        .to_platform_input(),
    );
    assert_eq!(cx.update(|cx| f.stack.read(cx).scroll_offset()), 48.0);
    // Scrolling moves lanes, not the viewport.
    assert_eq!(cx.update(|cx| f.viewport.read(cx).viewport()), view);
    // Ctrl+wheel still zooms while overflowing.
    dispatch(
        cx,
        f.window,
        ScrollWheelEvent {
            position: at(plot, 0.5, 0.5),
            delta: ScrollDelta::Lines(point(0., 1.)),
            modifiers: Modifiers {
                control: true,
                ..Modifiers::default()
            },
            touch_phase: TouchPhase::Moved,
        }
        .to_platform_input(),
    );
    assert!(cx.update(|cx| f.viewport.read(cx).viewport()).span() < 1.0);
}

#[gpui_kit::test]
fn theme_change_repaints_without_rebuilding_geometry(cx: &mut TestAppContext) {
    let f = open(cx);
    let before = cx.update(|cx| f.stack.read(cx).static_stats(cx));
    cx.update_window(f.window, |_, window, cx| {
        Theme::change(ThemeMode::Light, Some(window), cx);
        draw(window, cx);
    })
    .unwrap();
    cx.run_until_parked();
    let after = cx.update(|cx| f.stack.read(cx).static_stats(cx));
    assert!(after.renders > before.renders, "the theme change repainted");
    assert_eq!(
        after.geometry_builds, before.geometry_builds,
        "geometry keyed on colour"
    );
}

#[gpui_kit::test]
fn zoom_rebuilds_geometry_once(cx: &mut TestAppContext) {
    let f = open(cx);
    let before = cx.update(|cx| f.stack.read(cx).static_stats(cx));
    f.stack.update(cx, |stack, cx| stack.zoom_in(cx));
    cx.run_until_parked();
    cx.update_window(f.window, |_, window, cx| draw(window, cx))
        .unwrap();
    let after = cx.update(|cx| f.stack.read(cx).static_stats(cx));
    assert_eq!(after.renders, before.renders + 1);
    assert!(after.geometry_builds > before.geometry_builds);
}

#[gpui_kit::test]
fn corner_click_focuses_the_corner(cx: &mut TestAppContext) {
    let f = open(cx);
    cx.update_window(f.window, |_, window, cx| {
        window.click(("corner", 5usize), cx)
    })
    .unwrap();
    cx.run_until_parked();
    assert!(f.events.borrow().contains(&TraceEvent::CornerClicked(5)));
    let view = cx.update(|cx| f.viewport.read(cx).viewport());
    let corner = cx.update(|cx| {
        f.stack
            .read(cx)
            .scene()
            .corners()
            .iter()
            .find(|c| c.id == 5)
            .cloned()
            .unwrap()
    });
    assert_eq!(view, Viewport::focus_on(corner.start, corner.end));
    assert_eq!(cx.update(|cx| f.stack.read(cx).focused_corner()), Some(5));
    assert!(cx.update(|cx| f.cursor.read(cx).focus()).is_some());
}

#[gpui_kit::test]
fn middle_drag_pans(cx: &mut TestAppContext) {
    let f = open(cx);
    f.viewport
        .update(cx, |v, cx| v.set_viewport(Viewport::new(0.4, 0.5), cx));
    cx.run_until_parked();
    let plot = plot_bounds(cx, f.window);
    let start = at(plot, 0.6, 0.5);
    let end = at(plot, 0.3, 0.5);
    dispatch(
        cx,
        f.window,
        MouseDownEvent {
            button: MouseButton::Middle,
            position: start,
            modifiers: Modifiers::default(),
            click_count: 1,
            first_mouse: false,
        }
        .to_platform_input(),
    );
    dispatch(
        cx,
        f.window,
        MouseMoveEvent {
            position: end,
            pressed_button: Some(MouseButton::Middle),
            modifiers: Modifiers::default(),
        }
        .to_platform_input(),
    );
    dispatch(
        cx,
        f.window,
        MouseUpEvent {
            button: MouseButton::Middle,
            position: end,
            modifiers: Modifiers::default(),
            click_count: 1,
        }
        .to_platform_input(),
    );
    let view = cx.update(|cx| f.viewport.read(cx).viewport());
    // The grabbed sample (0.46) followed the pointer to 30% of the plot.
    let under = view.start + 0.3 * view.span();
    assert!((under - 0.46).abs() < 1e-3, "{view:?}");
}

fn press(cx: &mut TestAppContext, f: &Fixture, position: Point<Pixels>) {
    dispatch(
        cx,
        f.window,
        MouseDownEvent {
            button: MouseButton::Left,
            position,
            modifiers: Modifiers::default(),
            click_count: 1,
            first_mouse: false,
        }
        .to_platform_input(),
    );
}

fn drag_to(cx: &mut TestAppContext, f: &Fixture, position: Point<Pixels>) {
    dispatch(
        cx,
        f.window,
        MouseMoveEvent {
            position,
            pressed_button: Some(MouseButton::Left),
            modifiers: Modifiers::default(),
        }
        .to_platform_input(),
    );
}

fn release(cx: &mut TestAppContext, f: &Fixture, position: Point<Pixels>) {
    dispatch(
        cx,
        f.window,
        MouseUpEvent {
            button: MouseButton::Left,
            position,
            modifiers: Modifiers::default(),
            click_count: 1,
        }
        .to_platform_input(),
    );
}

#[gpui_kit::test]
fn corner_edit_drag_stays_off_the_static_layer(cx: &mut TestAppContext) {
    let f = open(cx);
    f.stack
        .update(cx, |stack, cx| stack.set_editing_corners(true, cx));
    // Focus first: GPUI refreshes the whole window on a focus change, which
    // repaints (but does not rebuild) the static layer once.
    let handle = cx.update(|cx| f.stack.read(cx).focus_handle(cx));
    cx.update_window(f.window, |_, window, cx| {
        window.focus(&handle, cx);
        draw(window, cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(f.window, |_, window, cx| draw(window, cx))
        .unwrap();
    let before = cx.update(|cx| f.stack.read(cx).static_stats(cx));
    let corner = cx.update(|cx| f.stack.read(cx).scene().corners()[2].clone());
    let plot = plot_bounds(cx, f.window);
    let edge = plot.origin
        + point(
            plot.size.width * corner.start as f32,
            plot.size.height * 0.5,
        );
    press(cx, &f, edge);
    drag_to(cx, &f, edge + point(px(-30.), px(0.)));
    release(cx, &f, edge + point(px(-30.), px(0.)));
    let edited = f
        .events
        .borrow()
        .iter()
        .rev()
        .find_map(|e| match e {
            TraceEvent::CornerEdited { id, start, end } => Some((*id, *start, *end)),
            _ => None,
        })
        .expect("CornerEdited");
    assert_eq!(edited.0, corner.id);
    assert!(edited.1 < corner.start && (edited.2 - corner.end).abs() < 1e-12);
    let after = cx.update(|cx| f.stack.read(cx).static_stats(cx));
    assert_eq!(
        after.renders, before.renders,
        "corner drags stay on the overlay path"
    );
    assert_eq!(after.geometry_builds, before.geometry_builds);
    // The press did not move the cursor or select: it grabbed the corner.
    assert!(cx.update(|cx| f.cursor.read(cx).selection()).is_none());
}

#[gpui_kit::test]
fn resize_mode_drags_lane_dividers(cx: &mut TestAppContext) {
    let f = open(cx);
    f.stack.update(cx, |stack, cx| stack.set_resizing(true, cx));
    cx.run_until_parked();
    cx.update_window(f.window, |_, window, cx| draw(window, cx))
        .unwrap();
    let (first_bottom, first_height) = cx.update(|cx| {
        let slot = &f.stack.read(cx).layout().slots[0];
        (slot.y + slot.height, slot.height)
    });
    let plot = plot_bounds(cx, f.window);
    let divider = plot.origin + point(plot.size.width * 0.5, px(first_bottom as f32));
    press(cx, &f, divider);
    drag_to(cx, &f, divider + point(px(0.), px(60.)));
    release(cx, &f, divider + point(px(0.), px(60.)));
    let (keys, heights) = f
        .events
        .borrow()
        .iter()
        .rev()
        .find_map(|e| match e {
            TraceEvent::LaneResized { keys, heights } => Some((keys.clone(), heights.clone())),
            _ => None,
        })
        .expect("LaneResized");
    assert_eq!(keys[0].as_ref(), "speed");
    assert!(
        (heights[0] - first_height - 60.0).abs() < 1e-3,
        "{heights:?}"
    );
    let layout_height = cx.update(|cx| f.stack.read(cx).layout().slots[0].height);
    assert!(
        (layout_height - heights[0]).abs() < 1e-9,
        "the draft is shown"
    );
}

#[gpui_kit::test]
fn pinning_moves_a_lane_above_the_scroll_region(cx: &mut TestAppContext) {
    let f = open(cx);
    f.stack
        .update(cx, |stack, cx| stack.toggle_lane_pinned("delta", cx));
    cx.run_until_parked();
    assert!(f.events.borrow().contains(&TraceEvent::LanePinToggled {
        key: "delta".into(),
        pinned: true
    }));
    let (root, pinned, pinned_height) = cx.update(|cx| {
        let layout = f.stack.read(cx).layout();
        (
            layout.slots[0].root,
            layout.slots[0].pinned,
            layout.pinned_height,
        )
    });
    let delta = cx.update(|cx| {
        f.stack
            .read(cx)
            .scene()
            .lanes()
            .iter()
            .position(|l| l.key.as_ref() == "delta")
            .unwrap()
    });
    assert_eq!(root, delta);
    assert!(pinned && pinned_height > 0.0);
    cx.update_window(f.window, |_, window, cx| {
        draw(window, cx);
        let delta = window.find("lane-delta");
        let speed = window.find("lane-speed");
        assert!(delta.bounds().top() < speed.bounds().top());
    })
    .unwrap();
}
