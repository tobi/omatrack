//! UI integration tests for the Traces panel: the real workspace in a
//! headless window, a synthetic library of two MTJ recordings (written to a
//! temporary folder, so the whole scan → load → analysis → scene pipeline
//! runs), driven by native pointer and keyboard events.
//!
//! The `real_` test runs the same panel on the `AiM` recordings (read-only);
//! run it with
//! `OMATRACK_FIXTURES=~/Documents/Telemetry/26T07_PLM cargo test -p omatrack-app --test traces_panel -- --include-ignored real_`.

#![cfg(test)]

mod common;

use std::path::Path;
use std::time::Duration;

use gpui_kit::test::{TestAppContextExt as _, TestWindowExt as _};
use gpui_kit::{
    AnyWindowHandle, AppContext as _, Bounds, Entity, Focusable as _, InputEvent as _, Modifiers,
    MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, Pixels, Point, ScrollDelta,
    TestAppContext, Window, point, px,
};
use omatrack_app::actions::{ResizeLanes, Role, SelectLap, ToggleTraceColorMode, ZoomReset};
use omatrack_app::panels::TraceMode;
use omatrack_app::panels::traces::{DELTA_KEY, ToggleLane, TracesPanel};
use omatrack_app::state::TraceViewMode;
use omatrack_trace::{EventMarkKind, TraceStack, Viewport};

const RATE: f64 = 50.0;
const DURATION: f64 = 90.0;
const FIRST_LAP_START: f64 = 5.0;
/// Lap boundaries (s): an out fragment, four laps (the second fastest), an
/// in fragment.
const BOUNDARIES: [f64; 7] = [0.0, 5.0, 25.1, 45.0, 65.0, 85.0, 90.0];

/// Two braked corners per 20 s lap; `shift` makes the second driver a
/// little slower everywhere.
fn sample(t: f64, shift: f64) -> [f64; 5] {
    let lap = ((t - FIRST_LAP_START) / 20.0).floor();
    let phase = (t - FIRST_LAP_START) / 20.0 - lap;
    let k = lap.clamp(0.0, 4.0);
    let (mut speed, mut throttle, mut brake, mut steering, mut gear) =
        (200.0 - 2.0 * k + shift, 100.0, 0.0, 0.0, 6.0);
    for (corner_start, corner_end) in [(0.25, 0.40), (0.65, 0.80)] {
        let start = corner_start + 0.004 * k;
        if phase >= start && phase < corner_end {
            let local = (phase - start) / (corner_end - start);
            let depth = 110.0 + k;
            speed -= if local < 0.5 {
                depth * local / 0.5
            } else {
                depth * (1.0 - local) / 0.5
            };
            throttle = if local < 0.6 { 0.0 } else { 100.0 };
            brake = if local < 0.45 {
                80.0 * (1.0 - local / 0.45)
            } else {
                0.0
            };
            steering = 60.0 * (std::f64::consts::PI * local).sin();
            gear = if local < 0.2 { 5.0 } else { 3.0 };
        }
    }
    [speed, throttle, brake, steering, gear]
}

/// One MTJ recording (`*.telemetry.jsonl`), per TELEMETRY.md.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn write_recording(path: &Path, driver: &str, shift: f64) {
    let ns = |seconds: f64| (seconds * 1e9).round() as i64;
    let count = (DURATION * RATE) as usize;
    let mut columns: [Vec<f64>; 5] = Default::default();
    for i in 0..count {
        for (column, value) in columns.iter_mut().zip(sample(i as f64 / RATE, shift)) {
            column.push((value * 1e4).round() / 1e4);
        }
    }
    let laps: Vec<serde_json::Value> = BOUNDARIES
        .windows(2)
        .enumerate()
        .map(|(number, bounds)| {
            let complete = number > 0 && number + 2 < BOUNDARIES.len();
            serde_json::json!([number, ns(bounds[0]), ns(bounds[1]), i32::from(complete)])
        })
        .collect();
    let mut lines = vec![
        serde_json::json!({
            "mtj": 1, "q": 20_000_000, "dur": ns(DURATION), "drv": driver,
            "ven": "Test Circuit", "utc": 1_788_000_000_000_000_000i64, "tz": "UTC",
        })
        .to_string(),
        serde_json::Value::Array(laps).to_string(),
    ];
    for ((name, unit), values) in [
        ("Speed", "km/h"),
        ("Throttle Pos", "%"),
        ("Brake Pressure F", "bar"),
        ("Steering Angle", "deg"),
        ("Gear", ""),
    ]
    .into_iter()
    .zip(columns)
    {
        lines.push(serde_json::json!({"n": name, "hz": RATE, "u": unit, "v": values}).to_string());
    }
    std::fs::write(path, lines.join("\n") + "\n").expect("fixture written");
}

struct Fixture {
    sandbox: common::Sandbox,
    test: common::TestApp,
    window: AnyWindowHandle,
    traces: Entity<TracesPanel>,
}

impl Fixture {
    fn stack(&self, cx: &mut TestAppContext) -> Entity<TraceStack> {
        cx.update(|cx| self.traces.read(cx).stack().cloned())
            .expect("the stack exists after the first frame")
    }

    fn plot(&self, cx: &mut TestAppContext) -> Bounds<Pixels> {
        cx.update_window(self.window, |_, window, _| {
            window.find("trace-plot").bounds()
        })
        .unwrap()
    }

    fn config(&self, cx: &mut TestAppContext) -> omatrack_library::Config {
        let preferences = self.test.app.preferences.clone();
        cx.update(|cx| preferences.update(cx, omatrack_app::state::Preferences::flush));
        self.sandbox.read_config()
    }
}

/// Scan `folder`, then select `primary` and `reference` (file name part,
/// the best lap of each) through `SelectLap`, and wait for the scene.
async fn open_pair(
    cx: &mut TestAppContext,
    sandbox: common::Sandbox,
    folder: &Path,
    primary: &str,
    reference: &str,
) -> Fixture {
    sandbox.write_config(&format!(
        "locations:\n  - type: folder\n    name: Fixtures\n    target: {}\n",
        folder.display()
    ));
    let test = common::start(cx, sandbox.options());
    let window: AnyWindowHandle = test.window.into();
    let library = test.app.library.clone();
    cx.update(|cx| library.update(cx, omatrack_app::state::Library::rescan));
    cx.run_until_parked();
    cx.wait_for(window, Duration::from_secs(600), |_, cx| {
        let library = library.read(cx);
        library.has_scanned() && !library.is_scanning()
    })
    .await;
    let (primary, reference) = cx.update(|cx| {
        let snapshot = library.read(cx).snapshot().clone();
        let find = |name: &str| {
            let node = snapshot
                .sessions()
                .find(|node| node.file_name().contains(name))
                .cloned()
                .unwrap_or_else(|| panic!("{name} is in the library"));
            let lap = node.best_lap_id.expect("a best lap");
            (node.id, lap)
        };
        (find(primary), find(reference))
    });
    cx.update_window(window, |_, window, cx| {
        for ((session, lap), role) in [(primary, Role::Primary), (reference, Role::Reference)] {
            window.dispatch_action(
                Box::new(SelectLap {
                    session: session.into(),
                    lap,
                    role,
                }),
                cx,
            );
        }
    })
    .unwrap();
    cx.run_until_parked();
    let traces = cx.update(|cx| test.workspace.read(cx).panels().traces.clone());
    let session = test.app.session.clone();
    let panel = traces.clone();
    cx.wait_for(window, Duration::from_secs(600), move |_, cx| {
        let analysis = session.read(cx).analysis().cloned();
        let scene = panel.read(cx).scene().clone();
        analysis.is_some_and(|analysis| analysis.reference().is_some())
            && scene.lane(DELTA_KEY).is_some()
    })
    .await;
    // First frames: the plot measures itself, the layout follows.
    for _ in 0..3 {
        cx.update_window(window, |_, window, cx| window.render_frame(cx))
            .unwrap();
        cx.run_until_parked();
    }
    Fixture {
        sandbox,
        test,
        window,
        traces,
    }
}

async fn synthetic_pair(cx: &mut TestAppContext) -> Fixture {
    let sandbox = common::Sandbox::new();
    let folder = sandbox.dir.path().join("library").join("2026-09-02");
    std::fs::create_dir_all(&folder).unwrap();
    write_recording(&folder.join("Run1.telemetry.jsonl"), "Ada", 0.0);
    write_recording(&folder.join("Run2.telemetry.jsonl"), "Grace", -3.0);
    // A named track: corner edits are stored per track.
    std::fs::write(folder.join("TRACK.yml"), "track: {name: Test Circuit}\n").unwrap();
    let root = sandbox.dir.path().join("library");
    open_pair(cx, sandbox, &root, "Run1", "Run2").await
}

/// Draw without `Window::refresh`, so cached views stay cached (the kit's
/// `render_frame` refreshes, which deliberately bypasses view caching).
fn draw(window: &mut Window, cx: &mut gpui_kit::App) {
    window.draw(cx).clear(cx);
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

fn at(bounds: Bounds<Pixels>, fx: f32, fy: f32) -> Point<Pixels> {
    bounds.origin + point(bounds.size.width * fx, bounds.size.height * fy)
}

fn mouse(
    cx: &mut TestAppContext,
    window: AnyWindowHandle,
    position: Point<Pixels>,
    down: Option<bool>,
) {
    let event = match down {
        Some(true) => MouseDownEvent {
            button: MouseButton::Left,
            position,
            modifiers: Modifiers::default(),
            click_count: 1,
            first_mouse: false,
        }
        .to_platform_input(),
        Some(false) => MouseUpEvent {
            button: MouseButton::Left,
            position,
            modifiers: Modifiers::default(),
            click_count: 1,
        }
        .to_platform_input(),
        None => MouseMoveEvent {
            position,
            pressed_button: None,
            modifiers: Modifiers::default(),
        }
        .to_platform_input(),
    };
    dispatch(cx, window, event);
}

#[gpui_kit::test]
async fn the_scene_follows_the_analysis(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    let (keys, delta_len, corners) = cx.update(|cx| {
        let scene = f.traces.read(cx).scene().clone();
        let keys: Vec<String> = scene.lanes().iter().map(|l| l.key.to_string()).collect();
        let delta = scene.lane(DELTA_KEY).unwrap().primary.len();
        (keys, delta, scene.corners().len())
    });
    assert_eq!(
        &keys[..6],
        &[DELTA_KEY, "speed", "throttle", "brake", "gear", "steering"]
    );
    assert!(delta_len > 100);
    assert!(corners >= 2, "two braked corners per lap: {corners}");
    cx.update_window(f.window, |_, window, _| {
        // The gap lane is pinned; brake has its own lane under throttle;
        // steering is opt in.
        assert!(
            window
                .find("lane-delta")
                .label()
                .unwrap()
                .starts_with("Gap to R")
        );
        assert!(
            window
                .find("lane-brake")
                .label()
                .unwrap()
                .starts_with("Brake")
        );
        assert!(window.try_find("lane-steering").is_none());
        assert!(
            window
                .find("corner-ruler")
                .label()
                .unwrap()
                .starts_with("Corners: ")
        );
        // The trace toolbar heads the lanes; the axis rides in the video's
        // control row.
        assert!(window.find("trace-toolbar").visible());
        assert!(window.find("trace-axis-distance").visible());
        assert!(window.find("trace-tools").visible());
    })
    .unwrap();
    let stack = f.stack(cx);
    let pinned = cx.update(|cx| {
        let layout = stack.read(cx).layout().clone();
        layout.slots.iter().filter(|slot| slot.pinned).count()
    });
    assert_eq!(pinned, 1, "the Δ lane is pinned");
}

#[gpui_kit::test]
async fn moving_the_cursor_never_rebuilds_trace_geometry(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    let stack = f.stack(cx);
    // Focus first: GPUI refreshes the whole window on a focus change.
    let handle = cx.update(|cx| stack.read(cx).focus_handle(cx));
    cx.update_window(f.window, |_, window, cx| {
        window.focus(&handle, cx);
        draw(window, cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(f.window, |_, window, cx| draw(window, cx))
        .unwrap();
    let before = cx.update(|cx| stack.read(cx).static_stats(cx));
    // Idle frames replay the cached static layer.
    for _ in 0..2 {
        cx.update_window(f.window, |_, window, cx| draw(window, cx))
            .unwrap();
        cx.run_until_parked();
    }
    assert_eq!(
        cx.update(|cx| stack.read(cx).static_rebuilds(cx)),
        before.renders
    );
    let plot = f.plot(cx);
    for fx in [0.2, 0.35, 0.5] {
        let position = at(plot, fx, 0.6);
        mouse(cx, f.window, position, None);
        mouse(cx, f.window, position, Some(true));
        mouse(cx, f.window, position, Some(false));
    }
    let cursor = cx.update(|cx| f.test.app.cursor.read(cx).fraction());
    assert!(cursor.is_some_and(|c| (c - 0.5).abs() < 0.02), "{cursor:?}");
    let after = cx.update(|cx| stack.read(cx).static_stats(cx));
    assert_eq!(
        after.geometry_builds, before.geometry_builds,
        "a cursor move never rebuilds trace geometry"
    );
    // Inside the dock the static layer is re-rendered (from its geometry
    // cache) on cursor frames: gpui-component's TabPanel renders every
    // panel through `AnyView::cached`, and a cache miss of that ancestor
    // (the stack's cursor notify dirties it) makes GPUI render every nested
    // cached view fresh. See the packet report; the standalone stack keeps
    // `static_rebuilds()` unchanged (omatrack-trace tests/ui.rs).
    assert!(after.renders > before.renders);
}

#[gpui_kit::test]
async fn the_wheel_zooms_the_shared_viewport(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    cx.update_window(f.window, |_, window, cx| {
        window.scroll("trace-plot", ScrollDelta::Lines(point(0., 1.)), cx);
    })
    .unwrap();
    cx.run_until_parked();
    let view = cx.update(|cx| f.test.app.viewport.read(cx).viewport());
    assert!(view.span() < 1.0 && view != Viewport::FULL, "{view:?}");
    // Whole lap (the tools menu, the palette, ctrl-0) is ZoomReset.
    cx.update_window(f.window, |_, window, cx| {
        window.dispatch_action(Box::new(ZoomReset), cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(
        cx.update(|cx| f.test.app.viewport.read(cx).viewport()),
        Viewport::FULL
    );
}

#[gpui_kit::test]
async fn a_drag_selects_a_range_and_shows_its_statistics(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    cx.update_window(f.window, |_, window, _| {
        assert!(window.try_find("trace-range-stats").is_none());
    })
    .unwrap();
    let plot = f.plot(cx);
    let (from, to) = (at(plot, 0.2, 0.6), at(plot, 0.6, 0.6));
    cx.update_window(f.window, |_, window, cx| window.drag(from, to, cx))
        .unwrap();
    cx.run_until_parked();
    let stats = cx
        .update(|cx| f.traces.read(cx).range_stats().copied())
        .expect("a selection has statistics");
    assert!((stats.selection.start - 0.2).abs() < 0.02, "{stats:?}");
    assert!((stats.selection.end - 0.6).abs() < 0.02, "{stats:?}");
    assert!(stats.dt.is_some() && stats.primary_speed.is_some() && stats.reference_speed.is_some());
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        let chip = window.find("trace-range-stats");
        assert!(chip.visible());
        let label = chip.label().unwrap().to_string();
        assert!(label.contains("Δt") && label.contains("km/h"), "{label}");
        // Clearing the selection removes the chip.
        window.click("trace-range-clear", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find("trace-range-stats").is_none());
    })
    .unwrap();
    assert!(
        cx.update(|cx| f.test.app.cursor.read(cx).selection())
            .is_none()
    );
}

#[gpui_kit::test]
async fn h_and_j_focus_corners_in_the_left_half(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    let corners = cx.update(|cx| f.traces.read(cx).scene().corners().to_vec());
    let press = |cx: &mut TestAppContext, key: &str| {
        cx.update_window(f.window, |_, window, cx| window.press(key, cx))
            .unwrap();
        // Let the 140 ms focus motion land.
        cx.executor().advance_clock(Duration::from_millis(400));
        cx.run_until_parked();
    };
    let check = |cx: &mut TestAppContext, ix: usize| {
        let corner = &corners[ix];
        let view = cx.update(|cx| f.test.app.viewport.read(cx).viewport());
        let left_half = view.start + view.span() * 0.5;
        assert!(
            corner.start >= view.start - 1e-9 && corner.end <= left_half + 1e-9,
            "{} in {view:?}",
            corner.label
        );
        let (ruler, stack) = cx.update(|cx| {
            let panel = f.traces.read(cx);
            (
                panel.ruler().read(cx).focused_corner(),
                panel.stack().unwrap().read(cx).focused_corner(),
            )
        });
        assert_eq!(ruler, Some(corner.id), "the ruler marks the focused corner");
        assert_eq!(stack, Some(corner.id), "the lanes mark the focused corner");
        let cursor = cx.update(|cx| f.test.app.cursor.read(cx).fraction());
        assert_eq!(
            cursor,
            Some(corner.start),
            "the readouts describe the focused corner"
        );
    };
    let before = cx.update(|cx| f.test.app.cursor.read(cx).fraction());
    press(cx, "j");
    check(cx, 0);
    press(cx, "j");
    check(cx, 1);
    press(cx, "h");
    check(cx, 0);
    // The notes live in Where the time goes (its card follows the focus),
    // never in a card over the plot.
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find("trace-corner-notes").is_none());
    })
    .unwrap();
    // Escape returns to the viewport from before the focus.
    press(cx, "escape");
    assert_eq!(
        cx.update(|cx| f.test.app.viewport.read(cx).viewport()),
        Viewport::FULL
    );
    assert_eq!(
        cx.update(|cx| f.test.app.cursor.read(cx).fraction()),
        before,
        "escape restores the cursor with the viewport"
    );
    assert_eq!(
        cx.update(|cx| f.traces.read(cx).ruler().read(cx).focused_corner()),
        None
    );
}

/// Enter resize mode (the tools menu's `Resize lanes…`) and drag the
/// first divider down.
#[expect(
    clippy::cast_possible_truncation,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn drag_first_divider(cx: &mut TestAppContext, f: &Fixture) {
    cx.update_window(f.window, |_, window, cx| {
        window.dispatch_action(Box::new(ResizeLanes), cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(
        cx.update(|cx| f.traces.read(cx).mode()),
        TraceMode::ResizingLanes
    );
    cx.update_window(f.window, |_, window, cx| window.render_frame(cx))
        .unwrap();
    let stack = f.stack(cx);
    let bottom = cx.update(|cx| {
        let slot = &stack.read(cx).layout().slots[1];
        slot.y + slot.height
    });
    let plot = f.plot(cx);
    let divider = plot.origin + point(plot.size.width * 0.5, px(bottom as f32));
    cx.update_window(f.window, |_, window, cx| {
        window.drag(divider, divider + point(px(0.), px(40.)), cx);
    })
    .unwrap();
    cx.run_until_parked();
    let draft = cx.update(|cx| f.traces.read(cx).resize_draft().cloned());
    assert!(
        draft.is_some_and(|draft| !draft.is_empty()),
        "the drag drafted weights"
    );
}

#[gpui_kit::test]
async fn saving_a_resize_writes_weights_and_fit(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    drag_first_divider(cx, &f);
    // Nothing is written while the draft is open.
    assert!(f.config(cx).channels.values().all(|c| c.weight.is_none()));
    cx.update_window(f.window, |_, window, cx| window.press("ctrl-s", cx))
        .unwrap();
    cx.run_until_parked();
    assert_eq!(cx.update(|cx| f.traces.read(cx).mode()), TraceMode::Browse);
    let config = f.config(cx);
    let speed = config.channels.get("speed").and_then(|c| c.weight);
    assert!(speed.is_some_and(|w| w > 0.0), "{speed:?}");
    assert_eq!(config.trace.fit_channels, Some(true));
}

#[gpui_kit::test]
async fn reset_heights_previews_and_saves_equal_weights(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    drag_first_divider(cx, &f);
    let stack = f.stack(cx);
    let dragged = cx.update(|cx| stack.read(cx).layout().slots[1].height);
    cx.update_window(f.window, |_, window, cx| {
        window.click("trace-reset-heights", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let reset = cx.update(|cx| stack.read(cx).layout().slots[1].height);
    assert!((reset - dragged).abs() > 1.0, "the preview drops the drag");
    cx.update_window(f.window, |_, window, cx| {
        window.click("trace-mode-save", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let config = f.config(cx);
    for key in ["speed", "throttle", "brake", "gear"] {
        assert_eq!(
            config.channels.get(key).and_then(|c| c.weight),
            Some(1.0),
            "{key}"
        );
    }
}

#[gpui_kit::test]
async fn cancelling_a_resize_writes_nothing(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    drag_first_divider(cx, &f);
    // Escape cancels the editor (it does not reach the workspace's Escape).
    cx.update_window(f.window, |_, window, cx| window.press("escape", cx))
        .unwrap();
    cx.run_until_parked();
    assert_eq!(cx.update(|cx| f.traces.read(cx).mode()), TraceMode::Browse);
    assert!(cx.update(|cx| f.traces.read(cx).resize_draft().is_none()));
    let config = f.config(cx);
    assert!(config.channels.values().all(|c| c.weight.is_none()));
    assert_eq!(config.trace.fit_channels, None);
}

#[gpui_kit::test]
async fn single_keys_do_not_leave_the_resize_editor(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    drag_first_divider(cx, &f);
    // `a` (corner editing) and `t` (x-axis) are workspace keys; the resize
    // editor stays open while they run, and only Escape or Save leave it.
    let axis = cx.update(|cx| f.test.app.viewport.read(cx).axis());
    cx.update_window(f.window, |_, window, cx| window.press("t", cx))
        .unwrap();
    cx.run_until_parked();
    assert_ne!(cx.update(|cx| f.test.app.viewport.read(cx).axis()), axis);
    assert_eq!(
        cx.update(|cx| f.traces.read(cx).mode()),
        TraceMode::ResizingLanes
    );
}

#[gpui_kit::test]
async fn lanes_hide_and_show_through_the_lane_command(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    let stack = f.stack(cx);
    let lanes = |cx: &mut TestAppContext| cx.update(|cx| stack.read(cx).layout().slots.len());
    let before = lanes(cx);
    cx.update_window(f.window, |_, window, cx| {
        window.dispatch_action(
            Box::new(ToggleLane {
                key: "steering".into(),
            }),
            cx,
        );
    })
    .unwrap();
    cx.run_until_parked();
    // Steering is off by default: the command shows it.
    assert_eq!(lanes(cx), before + 1);
    assert_eq!(
        f.config(cx)
            .channels
            .get("steering")
            .and_then(|c| c.visible),
        Some(true)
    );
    let title = cx.update(|cx| {
        cx.global::<omatrack_app::commands::CommandRegistry>()
            .get("lane-steering")
            .map(|spec| spec.title().to_string())
    });
    assert_eq!(title.as_deref(), Some("Hide Steering lane"));
}

#[gpui_kit::test]
#[expect(
    clippy::cast_possible_truncation,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
#[expect(
    clippy::manual_midpoint,
    reason = "Keep the test oracle's bounded floating-point operation order explicit."
)]
async fn clicking_a_ruler_corner_focuses_it(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    let corner = cx.update(|cx| f.traces.read(cx).scene().corners()[1].clone());
    let ruler = cx
        .update_window(f.window, |_, window, _| {
            window.find("corner-ruler").bounds()
        })
        .unwrap();
    let x = ruler.origin.x + ruler.size.width * ((corner.start + corner.end) / 2.0) as f32;
    let position = point(x, ruler.bottom() - px(4.));
    mouse(cx, f.window, position, None);
    mouse(cx, f.window, position, Some(true));
    mouse(cx, f.window, position, Some(false));
    cx.executor().advance_clock(Duration::from_millis(400));
    cx.run_until_parked();
    let focused = cx.update(|cx| f.traces.read(cx).focused_corner().cloned());
    assert!(focused.is_some(), "the corner is focused");
    let workspace = cx.update(|cx| f.test.workspace.read(cx).focused_corner());
    assert_eq!(workspace, Some(1), "through the workspace's FocusCorner");
}

#[gpui_kit::test]
#[expect(
    clippy::cast_possible_truncation,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
async fn corner_edits_save_to_the_track_and_rebuild_the_analysis(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    let corner = cx.update(|cx| f.traces.read(cx).scene().corners()[0].clone());
    cx.update_window(f.window, |_, window, cx| window.press("a", cx))
        .unwrap();
    cx.run_until_parked();
    assert_eq!(
        cx.update(|cx| f.traces.read(cx).mode()),
        TraceMode::EditingCorners
    );
    // Drag the first zone's entry edge 30 px earlier in the ruler.
    let ruler = cx
        .update_window(f.window, |_, window, cx| {
            window.render_frame(cx);
            window.find("corner-ruler").bounds()
        })
        .unwrap();
    let edge = point(
        ruler.origin.x + ruler.size.width * corner.start as f32,
        ruler.origin.y + ruler.size.height * 0.5,
    );
    cx.update_window(f.window, |_, window, cx| {
        window.drag(edge, edge - point(px(30.), px(0.)), cx);
    })
    .unwrap();
    cx.run_until_parked();
    let edited = cx
        .update(|cx| {
            f.traces
                .read(cx)
                .corner_draft()
                .filter(|draft| draft.is_edited())
                .map(|draft| draft.bands()[0].clone())
        })
        .expect("the drag edited the draft");
    assert!(
        edited.start < corner.start - 1e-4,
        "{edited:?} vs {corner:?}"
    );
    assert!((edited.end - corner.end).abs() < 1e-12);
    // The lanes show the dragged zone too.
    let stack = f.stack(cx);
    let lanes = cx.update(|cx| stack.read(cx).is_editing_corners());
    assert!(lanes);
    // Nothing is stored before Save.
    assert!(f.config(cx).tracks.values().all(|t| t.corners.is_empty()));
    cx.update_window(f.window, |_, window, cx| window.press("ctrl-s", cx))
        .unwrap();
    cx.run_until_parked();
    assert_eq!(cx.update(|cx| f.traces.read(cx).mode()), TraceMode::Browse);
    let session = f.test.app.session.clone();
    let panel = f.traces.clone();
    let start = edited.start;
    cx.wait_for(f.window, Duration::from_secs(60), move |_, cx| {
        let analysis = session.read(cx).analysis().cloned();
        analysis.is_some_and(|a| a.has_corner_override())
            && panel
                .read(cx)
                .scene()
                .corners()
                .first()
                .is_some_and(|c| (c.start - start).abs() < 1e-9)
    })
    .await;
    let stored = f.config(cx);
    let zones: Vec<_> = stored
        .tracks
        .values()
        .flat_map(|track| track.corners.iter())
        .collect();
    assert!(!zones.is_empty(), "tracks.<track>.corners is written");
    assert!(
        zones
            .iter()
            .any(|z| z.start.is_some_and(|s| (s - start).abs() < 1e-9))
    );
}

#[gpui_kit::test]
async fn the_lane_menu_pins_a_lane_from_the_keyboard(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    let stack = f.stack(cx);
    let pinned = |cx: &mut TestAppContext| {
        cx.update(|cx| {
            let layout = stack.read(cx).layout().clone();
            layout.slots.iter().filter(|slot| slot.pinned).count()
        })
    };
    assert_eq!(pinned(cx), 1);
    // A secondary click on a scroll lane (below the pinned Δ lane).
    let plot = f.plot(cx);
    let position = at(plot, 0.5, 0.8);
    cx.update_window(f.window, |_, window, cx| {
        window.dispatch_event(
            MouseDownEvent {
                button: MouseButton::Right,
                position,
                modifiers: Modifiers::default(),
                click_count: 1,
                first_mouse: false,
            }
            .to_platform_input(),
            cx,
        );
        window.dispatch_event(
            MouseUpEvent {
                button: MouseButton::Right,
                position,
                modifiers: Modifiers::default(),
                click_count: 1,
            }
            .to_platform_input(),
            cx,
        );
    })
    .unwrap();
    cx.run_until_parked();
    assert!(cx.update(|cx| f.traces.read(cx).is_lane_menu_open()));
    // Down selects "Pin lane", Enter runs it and closes the menu.
    for key in ["down", "enter"] {
        cx.update_window(f.window, |_, window, cx| window.press(key, cx))
            .unwrap();
        cx.run_until_parked();
    }
    assert!(!cx.update(|cx| f.traces.read(cx).is_lane_menu_open()));
    assert_eq!(pinned(cx), 2, "the lane joined the pinned region");
    // Escape closes a reopened menu without running anything.
    cx.update_window(f.window, |_, window, cx| {
        window.dispatch_event(
            MouseDownEvent {
                button: MouseButton::Right,
                position,
                modifiers: Modifiers::default(),
                click_count: 1,
                first_mouse: false,
            }
            .to_platform_input(),
            cx,
        );
    })
    .unwrap();
    cx.run_until_parked();
    assert!(cx.update(|cx| f.traces.read(cx).is_lane_menu_open()));
    cx.update_window(f.window, |_, window, cx| window.press("escape", cx))
        .unwrap();
    cx.run_until_parked();
    assert!(!cx.update(|cx| f.traces.read(cx).is_lane_menu_open()));
    assert_eq!(pinned(cx), 2);
}

fn fixtures() -> String {
    std::env::var("OMATRACK_FIXTURES")
        .expect("set OMATRACK_FIXTURES to the 26T07_PLM folder of AiM MP4 recordings")
}

#[gpui_kit::test]
#[ignore = "requires private telemetry/video fixtures; set OMATRACK_FIXTURES"]
async fn real_run4_against_run1_fills_the_lanes(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let folder = std::path::PathBuf::from(fixtures());
    let f = open_pair(cx, sandbox, &folder, "Run4", "Run1").await;
    let stack = f.stack(cx);
    let (lanes, delta_finite, visible) = cx.update(|cx| {
        let scene = f.traces.read(cx).scene().clone();
        let delta = scene.lane(DELTA_KEY).expect("a Δ lane");
        let finite = delta.primary.iter().filter(|v| v.is_finite()).count();
        let visible = stack.read(cx).layout().slots.len();
        (scene.lanes().len(), finite, visible)
    });
    assert!(lanes >= 5, "{lanes} lanes");
    assert!(visible >= 5, "{visible} visible lanes");
    assert!(delta_finite > 1000, "the Δ lane has data: {delta_finite}");
    cx.update_window(f.window, |_, window, _| {
        assert!(window.find("lane-delta").visible());
        assert!(window.find("lane-speed").visible());
    })
    .unwrap();
}

#[gpui_kit::test]
async fn a_time_share_pair_keeps_a_readable_delta_lane_in_the_trace_card(cx: &mut TestAppContext) {
    // The synthetic pair carries no GPS: the map is a share of lap time.
    let f = synthetic_pair(cx).await;
    let time_share = cx.update(|cx| f.traces.read(cx).scene().time_share_delta());
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        // The basis is said by the Sync selector, not a lane subtitle.
        assert!(window.try_find("delta-time-share").is_none());
        let delta = window.find("lane-delta").bounds();
        if time_share {
            let speed = window.find("lane-speed").bounds();
            assert!(
                delta.size.height < speed.size.height,
                "a slim Δ lane: {delta:?} vs {speed:?}"
            );
        }
        let floor = omatrack_trace::layout::GAP_LANE_MIN_HEIGHT as f32;
        assert!(
            delta.size.height.as_f32() >= floor - 1.0,
            "the Δ lane keeps 1.5 lanes: {delta:?}"
        );
        // One bordered card holds the ruler row above the lanes.
        let card = window.find("trace-card").bounds();
        let ruler = window.find("trace-ruler-row").bounds();
        assert!(card.contains(&ruler.origin) && card.contains(&delta.origin));
        assert!(ruler.bottom() <= delta.top() + px(1.));
    })
    .unwrap();
}

#[gpui_kit::test]
async fn the_colour_mode_toggles_persist_and_repaint_the_lanes(cx: &mut TestAppContext) {
    use omatrack_library::config::TraceColorMode;
    use omatrack_trace::ColorMode;

    let f = synthetic_pair(cx).await;
    let stack = f.stack(cx);
    assert_eq!(cx.update(|cx| stack.read(cx).color_mode()), ColorMode::Lap);
    let before = cx.update(|cx| stack.read(cx).static_stats(cx));
    cx.update_window(f.window, |_, window, cx| {
        window.dispatch_action(Box::new(ToggleTraceColorMode), cx)
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(f.window, |_, window, cx| window.render_frame(cx))
        .unwrap();
    assert_eq!(f.config(cx).trace.color_mode, Some(TraceColorMode::Channel));
    assert_eq!(
        cx.update(|cx| stack.read(cx).color_mode()),
        ColorMode::Channel
    );
    let after = cx.update(|cx| stack.read(cx).static_stats(cx));
    assert_eq!(
        after.geometry_builds, before.geometry_builds,
        "a colour change repaints, never rebuilds geometry"
    );
    cx.update_window(f.window, |_, window, cx| {
        window.dispatch_action(Box::new(ToggleTraceColorMode), cx)
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(f.config(cx).trace.color_mode, Some(TraceColorMode::Lap));
    assert_eq!(cx.update(|cx| stack.read(cx).color_mode()), ColorMode::Lap);
}

#[gpui_kit::test]
#[expect(
    clippy::too_many_lines,
    reason = "Exercise the complete workflow in order, keeping its setup and state assertions together."
)]
async fn one_control_row_under_the_video_drives_the_traces(cx: &mut TestAppContext) {
    use omatrack_app::panels::PanelKind;
    use omatrack_trace::XAxis;

    let f = synthetic_pair(cx).await;
    // The centre carries no title bars: the video and the traces are the
    // only panels of their groups.
    assert!(!PanelKind::Video.has_title_bar() && !PanelKind::Traces.has_title_bar());
    assert!(PanelKind::Library.has_title_bar());
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        // The row sits under the pictures and above the traces' toolbar.
        let bar = window.find("video-bar").bounds();
        assert!(bar.top() >= window.find("video-empty").bounds().bottom() - px(1.));
        let toolbar = window.find("trace-toolbar").bounds();
        assert!(bar.bottom() <= toolbar.top() + px(1.));
        assert!(toolbar.bottom() <= window.find("corner-ruler").bounds().top() + px(1.));
        // Zoom, FIT, sizing and corner editing have one home: the toolbar.
        for id in ["trace-zoom-in", "trace-tools"] {
            let control = window.find(id).bounds();
            assert!(!bar.intersects(&control), "{id} is not in the video row");
            assert!(control.top() >= toolbar.top() && control.bottom() <= toolbar.bottom());
        }
        for id in [
            "video-play",
            "video-slow-motion",
            "video-per-lap",
            "trace-axis-distance",
            "trace-axis-time",
            "video-mute",
            "video-enter-fullscreen",
        ] {
            let control = window.find(id).bounds();
            assert!(
                control.top() >= bar.top() && control.bottom() <= bar.bottom(),
                "{id}"
            );
        }
    })
    .unwrap();

    // Distance | Time is the traces' axis (`t`).
    let axis = |cx: &mut TestAppContext| cx.update(|cx| f.test.app.viewport.read(cx).axis());
    let start = axis(cx);
    let other = if start == XAxis::Distance {
        "trace-axis-time"
    } else {
        "trace-axis-distance"
    };
    cx.update_window(f.window, |_, window, cx| window.click(other, cx))
        .unwrap();
    cx.run_until_parked();
    assert_ne!(axis(cx), start);
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        window.click(other, cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_ne!(axis(cx), start, "the selected segment stays selected");

    // The cursor in a corner: the row names it and the ruler's chip marks
    // it, without rebuilding trace geometry.
    let stack = f.stack(cx);
    let before = cx.update(|cx| stack.read(cx).static_stats(cx));
    let corner = cx.update(|cx| f.traces.read(cx).scene().corners()[1].clone());
    let middle = (corner.start + corner.end) * 0.5;
    f.test
        .app
        .cursor
        .update(cx, |cursor, cx| cursor.set_fraction(Some(middle), cx));
    cx.run_until_parked();
    let ruler = cx.update(|cx| f.traces.read(cx).ruler().clone());
    assert_eq!(
        cx.update(|cx| ruler.read(cx).cursor_corner()),
        Some(corner.id)
    );
    assert_eq!(
        cx.update(|cx| ruler.read(cx).chip_corner()),
        Some(corner.id)
    );
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        let place = window
            .find("video-cursor-place")
            .label()
            .unwrap()
            .to_string();
        assert!(place.starts_with("Cursor at "), "{place}");
        assert!(
            place.ends_with(corner.label.as_ref()),
            "{place} / {}",
            corner.label
        );
    })
    .unwrap();
    let after = cx.update(|cx| stack.read(cx).static_stats(cx));
    assert_eq!(after.geometry_builds, before.geometry_builds);

    // Between corners the chip goes and the row names the straight.
    let between = cx.update(|cx| {
        let corners = f.traces.read(cx).scene().corners().to_vec();
        (corners[0].end + corners[1].start) * 0.5
    });
    f.test
        .app
        .cursor
        .update(cx, |cursor, cx| cursor.set_fraction(Some(between), cx));
    cx.run_until_parked();
    assert_eq!(cx.update(|cx| ruler.read(cx).chip_corner()), None);
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        let place = window
            .find("video-cursor-place")
            .label()
            .unwrap()
            .to_string();
        assert!(place.contains("straight after"), "{place}");
    })
    .unwrap();
}

/// Switch the trace view mode and let the pipeline settle.
async fn set_view_mode(cx: &mut TestAppContext, f: &Fixture, mode: TraceViewMode) {
    let trace_view = f.test.app.trace_view.clone();
    cx.update(|cx| trace_view.update(cx, |view, cx| view.set_mode(mode, cx)));
    cx.run_until_parked();
    let panel = f.traces.clone();
    cx.wait_for(f.window, Duration::from_secs(600), move |_, cx| {
        !trace_view.read(cx).is_loading()
            && (mode != TraceViewMode::Consistency || panel.read(cx).scene().has_spread())
    })
    .await;
    cx.update_window(f.window, |_, window, cx| draw(window, cx))
        .unwrap();
}

#[gpui_kit::test]
async fn consistency_loads_the_session_once_and_draws_it_behind_the_lap(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    let trace_view = f.test.app.trace_view.clone();
    let stack = f.stack(cx);
    // Lazy: the lap view loads nothing.
    assert_eq!(cx.update(|cx| trace_view.read(cx).loads_started()), 0);
    assert!(!cx.update(|cx| f.traces.read(cx).scene().has_spread()));

    set_view_mode(cx, &f, TraceViewMode::Consistency).await;
    assert_eq!(cx.update(|cx| trace_view.read(cx).loads_started()), 1);
    let (spread, lanes, layers) = cx.update(|cx| {
        let consistency = trace_view.read(cx).spread().unwrap().consistency().clone();
        let scene = f.traces.read(cx).scene().clone();
        let lanes: Vec<String> = scene
            .lanes()
            .iter()
            .filter(|lane| lane.spread.is_some())
            .map(|lane| lane.key.to_string())
            .collect();
        (consistency, lanes, stack.read(cx).layers())
    });
    // Four timed laps in the recording: the three besides the primary.
    assert_eq!(spread.lap_count(), 3, "{:?}", spread.lap_ids());
    assert!(spread.is_meaningful());
    assert!(lanes.iter().any(|key| key == "speed"), "{lanes:?}");
    assert!(lanes.iter().any(|key| key == "brake"), "{lanes:?}");
    assert!(!lanes.iter().any(|key| key == DELTA_KEY));
    assert!(layers.consistency && !layers.events);
    assert!(
        cx.update(|cx| f.traces.read(cx).consistency_notice(cx))
            .is_none()
    );
    assert!(!cx.update(|cx| f.test.app.jobs.read(cx).is_busy()));
    let config = f.config(cx);
    assert_eq!(config.trace.view_mode(), TraceViewMode::Consistency);

    // Leaving and re-entering the mode reuses the spread of this lap.
    set_view_mode(cx, &f, TraceViewMode::Lap).await;
    assert!(!cx.update(|cx| stack.read(cx).layers()).consistency);
    set_view_mode(cx, &f, TraceViewMode::Consistency).await;
    assert_eq!(cx.update(|cx| trace_view.read(cx).loads_started()), 1);

    // The cursor never rebuilds geometry with the spread on.
    let before = cx.update(|cx| stack.read(cx).static_stats(cx));
    let plot = f.plot(cx);
    for fx in [0.2, 0.35, 0.5] {
        mouse(cx, f.window, at(plot, fx, 0.6), None);
    }
    let after = cx.update(|cx| stack.read(cx).static_stats(cx));
    assert_eq!(after.geometry_builds, before.geometry_builds);

    // Another primary lap: one more load, for that lap.
    cx.update(|cx| {
        f.test
            .app
            .session
            .update(cx, |session, cx| session.next_lap(cx))
    });
    cx.run_until_parked();
    let session = f.test.app.session.clone();
    let panel = f.traces.clone();
    let view = trace_view.clone();
    cx.wait_for(f.window, Duration::from_secs(600), move |_, cx| {
        let primary = session
            .read(cx)
            .primary()
            .and_then(|slot| slot.loaded())
            .cloned();
        primary.is_some_and(|lap| view.read(cx).spread_for(&lap).is_some())
            && panel.read(cx).scene().has_spread()
    })
    .await;
    assert_eq!(cx.update(|cx| trace_view.read(cx).loads_started()), 2);
}

#[gpui_kit::test]
async fn events_mark_both_laps_on_their_lanes(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    let stack = f.stack(cx);
    let events = cx.update(|cx| f.traces.read(cx).scene().events().clone());
    let on = |channel: &str, reference: bool| {
        events
            .iter()
            .filter(|mark| mark.channel.as_ref() == channel && mark.reference == reference)
            .count()
    };
    // Two braked corners per lap, each lap: onset, lift, down- and upshift.
    for reference in [false, true] {
        assert_eq!(on("brake", reference), 2, "brake onsets ({reference})");
        assert_eq!(on("throttle", reference), 2, "lifts ({reference})");
        assert!(on("gear", reference) >= 4, "shifts ({reference})");
    }
    assert!(events.windows(2).all(|w| w[0].fraction <= w[1].fraction));
    let brake = events
        .iter()
        .find(|mark| mark.kind == EventMarkKind::BrakeOnset && !mark.reference)
        .unwrap();
    assert!(brake.label.starts_with("P · Brake · "), "{}", brake.label);
    assert!(brake.label.ends_with(" m"), "{}", brake.label);

    set_view_mode(cx, &f, TraceViewMode::Events).await;
    let layers = cx.update(|cx| stack.read(cx).layers());
    assert!(layers.events && !layers.consistency);
    // Events need no session laps.
    assert_eq!(
        cx.update(|cx| f.test.app.trace_view.read(cx).loads_started()),
        0
    );
}

/// Click `id` in the window, let actions and motion land, draw.
fn click_and_settle(cx: &mut TestAppContext, f: &Fixture, id: &'static str) {
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        window.click(id, cx);
    })
    .unwrap();
    cx.executor().advance_clock(Duration::from_millis(400));
    cx.run_until_parked();
}

fn press_and_settle(cx: &mut TestAppContext, f: &Fixture, key: &str) {
    cx.update_window(f.window, |_, window, cx| window.press(key, cx))
        .unwrap();
    cx.executor().advance_clock(Duration::from_millis(400));
    cx.run_until_parked();
}

fn assert_viewport(cx: &mut TestAppContext, f: &Fixture, expected: Viewport, what: &str) {
    let view = cx.update(|cx| f.test.app.viewport.read(cx).viewport());
    assert!(
        (view.start - expected.start).abs() < 1e-9 && (view.end - expected.end).abs() < 1e-9,
        "{what}: {view:?}, expected {expected:?}"
    );
}

#[gpui_kit::test]
async fn the_trace_toolbar_zooms_and_fits(cx: &mut TestAppContext) {
    let f = synthetic_pair(cx).await;
    click_and_settle(cx, &f, "trace-zoom-in");
    let zoomed = cx.update(|cx| f.test.app.viewport.read(cx).viewport());
    assert!(zoomed.span() < 1.0, "{zoomed:?}");
    click_and_settle(cx, &f, "trace-zoom-out");
    let out = cx.update(|cx| f.test.app.viewport.read(cx).viewport());
    assert!(out.span() > zoomed.span(), "{out:?}");
    click_and_settle(cx, &f, "trace-zoom-in");
    click_and_settle(cx, &f, "trace-zoom-fit");
    assert_viewport(cx, &f, Viewport::FULL, "fit is the whole lap");
}

#[gpui_kit::test]
async fn the_corners_view_frames_one_corner_and_returns_to_the_lap(cx: &mut TestAppContext) {
    use omatrack_library::config::TraceViewMode;

    let f = synthetic_pair(cx).await;
    let corners = cx.update(|cx| f.traces.read(cx).scene().corners().to_vec());
    assert!(corners.len() >= 2);
    let framed = |ix: usize| Viewport::frame_corner(corners[ix].start, corners[ix].end);
    let focused = |cx: &mut TestAppContext| {
        cx.update(|cx| f.traces.read(cx).ruler().read(cx).focused_corner())
    };
    // A zoomed lap view to come back to.
    press_and_settle(cx, &f, "=");
    let lap_view = cx.update(|cx| f.test.app.viewport.read(cx).viewport());
    let lap_cursor = cx.update(|cx| f.test.app.cursor.read(cx).fraction());

    // The segment enters Corners: the first corner with approach and exit.
    click_and_settle(cx, &f, "trace-view-corners");
    assert_eq!(
        cx.update(|cx| f.test.app.preferences.read(cx).config().trace.view_mode()),
        TraceViewMode::Corners
    );
    assert_viewport(cx, &f, framed(0), "Corners frames the first corner");
    let view = framed(0);
    assert!(view.start < corners[0].start && view.end > corners[0].end);
    assert_eq!(focused(cx), Some(corners[0].id));
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        assert_eq!(window.find("trace-view-corners").checked(), Some(true));
    })
    .unwrap();

    // The cursor is in the corner (its apex, else its start), so the
    // readouts describe it.
    let in_corner = |cx: &mut TestAppContext, ix: usize| {
        let at = cx.update(|cx| f.test.app.cursor.read(cx).fraction());
        at.is_some_and(|at| at >= corners[ix].start && at <= corners[ix].end)
    };
    assert!(in_corner(cx, 0), "the cursor moves into the framed corner");

    // j / h step through the corners in the same framing.
    press_and_settle(cx, &f, "j");
    assert_viewport(cx, &f, framed(1), "j frames the next corner");
    assert_eq!(focused(cx), Some(corners[1].id));
    assert!(in_corner(cx, 1), "the cursor follows the corner");
    press_and_settle(cx, &f, "h");
    assert_viewport(cx, &f, framed(0), "h frames the previous corner");
    // Fit in Corners fits the corner, not the lap.
    click_and_settle(cx, &f, "trace-zoom-in");
    click_and_settle(cx, &f, "trace-zoom-fit");
    assert_viewport(cx, &f, framed(0), "fit fits the corner");

    // Escape leaves the mode for the view from before it.
    press_and_settle(cx, &f, "escape");
    assert_eq!(
        cx.update(|cx| f.test.app.preferences.read(cx).config().trace.view_mode()),
        TraceViewMode::Lap
    );
    assert_viewport(cx, &f, lap_view, "Escape returns to the lap view");
    assert_eq!(
        cx.update(|cx| f.test.app.cursor.read(cx).fraction()),
        lap_cursor
    );
    assert_eq!(focused(cx), None);

    // alt-2 / alt-1 do the same from the keyboard, and the mode persists.
    press_and_settle(cx, &f, "alt-2");
    assert_viewport(cx, &f, framed(0), "alt-2 enters Corners");
    press_and_settle(cx, &f, "j");
    press_and_settle(cx, &f, "alt-1");
    assert_viewport(cx, &f, lap_view, "alt-1 returns to the lap view");
    assert_eq!(f.config(cx).trace.view_mode(), TraceViewMode::Lap);
    press_and_settle(cx, &f, "alt-3");
    assert_eq!(
        f.config(cx).trace.view_mode,
        Some(TraceViewMode::Consistency)
    );
    // Consistency keeps the lap framing and starts the session load.
    assert_viewport(cx, &f, lap_view, "Consistency keeps the lap view");
    assert_eq!(
        cx.update(|cx| f.test.app.trace_view.read(cx).loads_started()),
        1
    );
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        assert_eq!(window.find("trace-view-consistency").checked(), Some(true));
    })
    .unwrap();
    // The segmented control drives the same modes: Events draws its marks
    // and no apex callouts; Lap brings the callouts back.
    let stack = f.stack(cx);
    click_and_settle(cx, &f, "trace-view-events");
    let layers = cx.update(|cx| stack.read(cx).layers());
    assert!(layers.events && !layers.consistency && !layers.apexes);
    click_and_settle(cx, &f, "trace-view-lap");
    let layers = cx.update(|cx| stack.read(cx).layers());
    assert!(!layers.events && !layers.consistency && layers.apexes);
}

#[gpui_kit::test]
async fn the_channels_menu_and_colour_mode_live_in_the_toolbar(cx: &mut TestAppContext) {
    use omatrack_library::config::TraceColorMode;

    let f = synthetic_pair(cx).await;
    // Steering is opt in; the Channels menu checks it on.
    let title = cx.update(|cx| {
        f.traces
            .read(cx)
            .scene()
            .lane("steering")
            .map(|lane| lane.title.clone())
            .expect("a steering lane")
    });
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find("lane-steering").is_none());
        window.click("trace-channels", cx);
        window.render_frame(cx);
        let mut menu = window.within("popup-menu");
        let item = (0..32usize)
            .find(|ix| {
                menu.try_find(*ix)
                    .is_some_and(|item| item.label() == Some(title.as_ref()))
            })
            .expect("steering is listed");
        menu.click(item, cx);
    })
    .unwrap();
    cx.run_until_parked();
    let visible = f
        .config(cx)
        .channels
        .get("steering")
        .and_then(|channel| channel.visible);
    assert_eq!(visible, Some(true));

    // The colour segments name both modes, the current one checked; a
    // click on the current one changes nothing.
    click_and_settle(cx, &f, "trace-color-lap");
    assert_ne!(f.config(cx).trace.color_mode, Some(TraceColorMode::Channel));
    click_and_settle(cx, &f, "trace-color-channel");
    assert_eq!(f.config(cx).trace.color_mode, Some(TraceColorMode::Channel));
    cx.update_window(f.window, |_, window, cx| {
        window.render_frame(cx);
        assert_eq!(window.find("trace-color-channel").checked(), Some(true));
        assert_eq!(window.find("trace-color-lap").checked(), Some(false));
    })
    .unwrap();
    click_and_settle(cx, &f, "trace-color-channel");
    assert_eq!(f.config(cx).trace.color_mode, Some(TraceColorMode::Channel));
    click_and_settle(cx, &f, "trace-color-lap");
    assert_eq!(f.config(cx).trace.color_mode, Some(TraceColorMode::Lap));
}
