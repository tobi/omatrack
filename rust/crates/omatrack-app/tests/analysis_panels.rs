//! UI integration tests for the analysis panels: Corners, Laps, Channels,
//! Inspector and Map.
//!
//! The analysis comes from the real pipeline (folder scan, parse, lap load,
//! alignment, corners) over a synthetic Motorsport Telemetry JSONL recording
//! written into the test's temporary directory: four 20 s flying laps with
//! two braked corners each and a GPS circle, the same model as the core's
//! session tests. The `real_*` tests run on the AiM fixtures
//! (`OMATRACK_FIXTURES`, read-only) and are ignored by default.

mod common;

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::time::Duration;

use gpui_kit::component::table::ColumnSort;
use gpui_kit::test::{TestAppContextExt as _, TestWindowExt as _};
use gpui_kit::{AnyWindowHandle, AppContext as _, SharedString, TestAppContext, point, px};
use omatrack_app::actions::{Role, SelectLap};
use omatrack_app::panels::PanelKind;
use omatrack_app::panels::channels::ChannelList;

const RATE: f64 = 50.0;
const LAP_SECONDS: f64 = 20.0;
const FIRST_LAP_START: f64 = 5.0;
const DURATION: f64 = 90.0;

/// One synthetic sample: each lap brakes a little later and carries a
/// little less speed than the one before.
fn sample(t: f64) -> [f64; 8] {
    let lap = ((t - FIRST_LAP_START) / LAP_SECONDS).floor();
    let phase = (t - FIRST_LAP_START) / LAP_SECONDS - lap;
    let k = lap.clamp(0.0, 4.0);
    let (mut speed, mut throttle, mut brake, mut steering, mut gear) =
        (200.0 - 2.0 * k, 100.0, 0.0, 0.0, 6.0);
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
    let angle = std::f64::consts::TAU * phase.rem_euclid(1.0);
    [
        speed,
        throttle,
        brake,
        steering,
        gear,
        5000.0 + speed * 30.0,
        40.0 + 0.004 * angle.sin(),
        -80.0 + 0.004 * angle.cos(),
    ]
}

/// Write the synthetic recording as an MTJ document into `dir`.
fn write_recording(dir: &Path) -> PathBuf {
    const CHANNELS: [(&str, &str); 8] = [
        ("Speed", "km/h"),
        ("Throttle Pos", "%"),
        ("Brake Pressure F", "bar"),
        ("Steering Angle", "deg"),
        ("Gear", ""),
        ("RPM", "rpm"),
        ("GPS Latitude", "deg"),
        ("GPS Longitude", "deg"),
    ];
    let count = (DURATION * RATE) as usize;
    let mut columns = vec![Vec::with_capacity(count); CHANNELS.len()];
    for i in 0..count {
        for (column, value) in columns.iter_mut().zip(sample(i as f64 / RATE)) {
            column.push(value);
        }
    }
    let ns = |seconds: f64| (seconds * 1e9).round() as u64;
    let directory: Vec<String> = CHANNELS
        .iter()
        .map(|(name, unit)| format!("[\"{name}\",\"{unit}\",50,0,{count}]"))
        .collect();
    let mut document = format!(
        "{{\"mtj\":1,\"q\":20000000,\"dur\":{},\"nc\":{n},\"nsc\":{n},\"ns\":{},\"ch\":[{}],\"src\":\"telemetry\",\"drv\":\"Ada\",\"ven\":\"Synthetic Circuit\"}}\n",
        ns(DURATION),
        count * CHANNELS.len(),
        directory.join(","),
        n = CHANNELS.len(),
    );
    let mut laps = vec![format!("[1,0,{},0]", ns(FIRST_LAP_START))];
    for lap in 0..4 {
        let start = FIRST_LAP_START + f64::from(lap) * LAP_SECONDS;
        laps.push(format!(
            "[{},{},{},1]",
            lap + 2,
            ns(start),
            ns(start + LAP_SECONDS)
        ));
    }
    laps.push(format!("[6,{},{},0]", ns(85.0), ns(DURATION)));
    writeln!(document, "[{}]", laps.join(",")).unwrap();
    for ((name, unit), values) in CHANNELS.iter().zip(&columns) {
        let values: Vec<String> = values.iter().map(|v| format!("{v:.7}")).collect();
        let unit = if unit.is_empty() {
            String::new()
        } else {
            format!(",\"u\":\"{unit}\"")
        };
        writeln!(
            document,
            "{{\"n\":\"{name}\",\"hz\":50{unit},\"v\":[{}]}}",
            values.join(",")
        )
        .unwrap();
    }
    std::fs::create_dir_all(dir).unwrap();
    let path = dir.join("Synthetic_Q1.telemetry.jsonl");
    std::fs::write(&path, document).unwrap();
    path
}

/// A started application whose library holds the synthetic recording.
struct Scene {
    _sandbox: common::Sandbox,
    test: common::TestApp,
    handle: AnyWindowHandle,
    /// The recording's catalog session id and its complete lap ids.
    session: SharedString,
    laps: Vec<i32>,
}

async fn scan_synthetic(cx: &mut TestAppContext) -> Scene {
    let sandbox = common::Sandbox::new();
    let recordings = sandbox.dir.path().join("recordings");
    write_recording(&recordings);
    sandbox.write_config(&format!(
        "locations:\n  - type: folder\n    name: Synthetic\n    target: {}\n",
        recordings.display()
    ));
    let test = common::start(cx, sandbox.options());
    let handle: AnyWindowHandle = test.window.into();
    let library = test.app.library.clone();
    cx.update(|cx| library.update(cx, |library, cx| library.rescan(cx)));
    cx.run_until_parked();
    cx.wait_for(handle, Duration::from_secs(60), |_, cx| {
        let library = library.read(cx);
        library.has_scanned() && !library.is_scanning()
    })
    .await;
    let (session, laps) = cx.update(|cx| {
        let snapshot = library.read(cx).snapshot().clone();
        let node = snapshot
            .sessions()
            .next()
            .cloned()
            .expect("the synthetic recording is in the library");
        let laps: Vec<i32> = node
            .laps
            .iter()
            .filter(|lap| lap.complete)
            .map(|lap| lap.lap_id)
            .collect();
        (SharedString::from(node.id.clone()), laps)
    });
    assert_eq!(laps.len(), 4, "four complete laps: {laps:?}");
    Scene {
        _sandbox: sandbox,
        test,
        handle,
        session,
        laps,
    }
}

/// Scan, then compare the second complete lap against the fourth.
async fn analysed(cx: &mut TestAppContext) -> Scene {
    let scene = scan_synthetic(cx).await;
    let (session, primary, reference) = (scene.session.clone(), scene.laps[1], scene.laps[3]);
    cx.update_window(scene.handle, |_, window, cx| {
        for (lap, role) in [(primary, Role::Primary), (reference, Role::Reference)] {
            window.dispatch_action(
                Box::new(SelectLap {
                    session: session.clone(),
                    lap,
                    role,
                }),
                cx,
            );
        }
    })
    .unwrap();
    let state = scene.test.app.session.clone();
    cx.run_until_parked();
    cx.wait_for(scene.handle, Duration::from_secs(60), |_, cx| {
        let session = state.read(cx);
        !session.is_loading()
            && session
                .analysis()
                .is_some_and(|analysis| analysis.reference().is_some())
    })
    .await;
    cx.run_until_parked();
    cx.update_window(scene.handle, |_, window, cx| window.render_frame(cx))
        .unwrap();
    scene
}

/// Make `kind` the visible tab of its dock group.
fn show_panel(test: &common::TestApp, kind: PanelKind, cx: &mut TestAppContext) {
    let workspace = test.workspace.clone();
    let handle: AnyWindowHandle = test.window.into();
    cx.update_window(handle, |_, window, cx| {
        let (area, id) = workspace.read_with(cx, |workspace, cx| {
            (
                workspace.dock_area().clone(),
                workspace.panels().handle(kind).panel_id(cx),
            )
        });
        area.update(cx, |area, cx| area.select_panel(id, window, cx));
        window.render_frame(cx);
    })
    .unwrap();
    cx.run_until_parked();
}

fn corner_dts(scene: &Scene, cx: &mut TestAppContext) -> Vec<f64> {
    let corners = scene
        .test
        .workspace
        .read_with(cx, |w, _| w.panels().corners.clone());
    cx.update(|cx| {
        let table = corners.read(cx).table().expect("the corners table").clone();
        table
            .read(cx)
            .delegate()
            .lines()
            .iter()
            .map(|line| line.dt())
            .collect()
    })
}

#[gpui_kit::test]
async fn corners_sort_by_time_lost_and_a_header_click_resorts(cx: &mut TestAppContext) {
    let scene = analysed(cx).await;
    let dts = corner_dts(&scene, cx);
    assert!(dts.len() >= 2, "the synthetic laps have corners: {dts:?}");
    assert!(
        dts.windows(2).all(|pair| pair[0] >= pair[1]),
        "worst corner first: {dts:?}"
    );
    let corners = scene
        .test
        .workspace
        .read_with(cx, |w, _| w.panels().corners.clone());
    // The worst corner is selected and its notes are on screen.
    let worst = cx.update(|cx| {
        let table = corners.read(cx).table().unwrap().read(cx);
        assert_eq!(table.selected_row(), Some(0));
        table.delegate().lines()[0].id().clone()
    });
    assert_eq!(
        cx.update(|cx| corners.read(cx).selected(cx).cloned()),
        Some(worst.clone())
    );
    cx.update_window(scene.handle, |_, window, cx| {
        window.render_frame(cx);
        let notes = window.find("corner-notes").label().unwrap().to_string();
        assert!(notes.contains("notes"), "{notes}");
    })
    .unwrap();

    // Click the sort control of the Δt header: ascending now.
    cx.update_window(scene.handle, |_, window, cx| {
        let mut panel = window.within("corners-table");
        let header = panel.find(("col-header", 2usize));
        let bounds = header.bounds();
        panel.click_at(
            ("col-header", 2usize),
            point(bounds.size.width - px(8.), bounds.size.height / 2.),
            cx,
        );
    })
    .unwrap();
    cx.run_until_parked();
    let dts = corner_dts(&scene, cx);
    assert!(
        dts.windows(2).all(|pair| pair[0] <= pair[1]),
        "a header click re-sorts: {dts:?}"
    );
    cx.update(|cx| {
        let table = corners.read(cx).table().unwrap().read(cx);
        let column = gpui_kit::component::table::TableDelegate::column(table.delegate(), 2, cx);
        assert_eq!(column.sort, Some(ColumnSort::Ascending));
        // The selection followed the corner, not the row index.
        let ix = table.selected_row().unwrap();
        assert_eq!(table.delegate().lines()[ix].id(), &worst);
    });
}

#[gpui_kit::test]
async fn enter_on_a_corner_row_focuses_it_in_the_traces(cx: &mut TestAppContext) {
    let scene = analysed(cx).await;
    // Ctrl+4 lands on the corners table; Down picks the second row.
    cx.update_window(scene.handle, |_, window, cx| {
        window.press("ctrl-4", cx);
        window.press("down", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let corners = scene
        .test
        .workspace
        .read_with(cx, |w, _| w.panels().corners.clone());
    let id = cx
        .update(|cx| corners.read(cx).selected(cx).cloned())
        .expect("a corner is selected");
    let second = cx.update(|cx| {
        let table = corners.read(cx).table().unwrap().read(cx);
        table.delegate().lines()[1].id().clone()
    });
    assert_eq!(id, second, "Down moved the selection");
    assert_eq!(
        cx.update(|cx| scene.test.workspace.read(cx).focused_corner()),
        None
    );

    cx.update_window(scene.handle, |_, window, cx| window.press("enter", cx))
        .unwrap();
    cx.run_until_parked();
    cx.executor().advance_clock(Duration::from_millis(300));
    cx.run_until_parked();
    let (ix, zone) = cx.update(|cx| {
        let analysis = scene.test.app.session.read(cx).analysis().unwrap().clone();
        let ix = analysis
            .corners()
            .iter()
            .position(|zone| zone.id == id.as_ref())
            .unwrap();
        (ix, analysis.corners()[ix].clone())
    });
    assert_eq!(
        cx.update(|cx| scene.test.workspace.read(cx).focused_corner()),
        Some(ix),
        "Enter dispatched FocusCorner {{ id: {id} }}"
    );
    let viewport = cx.update(|cx| scene.test.app.viewport.read(cx).viewport());
    assert!(
        viewport.start <= zone.start && viewport.end >= zone.end && viewport.span() < 0.9,
        "the viewport frames the corner: {viewport:?} vs {}..{}",
        zone.start,
        zone.end
    );

    // A focus from elsewhere (J) moves the table's selection with it.
    cx.update_window(scene.handle, |_, window, cx| {
        let traces = scene
            .test
            .workspace
            .read(cx)
            .panels()
            .focus_handle(PanelKind::Traces, cx);
        window.focus(&traces, cx);
        window.press("j", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let focused = cx
        .update(|cx| scene.test.workspace.read(cx).focused_corner())
        .expect("J focused a corner");
    let focused_id = cx.update(|cx| {
        scene
            .test
            .app
            .session
            .read(cx)
            .analysis()
            .unwrap()
            .corners()[focused]
            .id
            .clone()
    });
    assert_eq!(
        cx.update(|cx| corners.read(cx).selected(cx).map(|id| id.to_string())),
        Some(focused_id),
        "the row follows the focused corner"
    );
}

#[gpui_kit::test]
fn alt_enter_in_the_laps_table_sets_the_reference(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let snapshot = common::load_synthetic_library(&test, cx);
    let run1 = snapshot
        .sessions()
        .find(|node| node.file_name().contains("Run1"))
        .unwrap()
        .clone();
    let handle: AnyWindowHandle = test.window.into();
    cx.update_window(handle, |_, window, cx| {
        window.dispatch_action(
            Box::new(SelectLap {
                session: run1.id.clone().into(),
                lap: 3,
                role: Role::Primary,
            }),
            cx,
        );
    })
    .unwrap();
    cx.run_until_parked();
    // Ctrl+5 lands on the laps table, on the primary lap's row.
    cx.update_window(handle, |_, window, cx| {
        window.press("ctrl-5", cx);
        window.render_frame(cx);
    })
    .unwrap();
    cx.run_until_parked();
    let laps = test.workspace.read_with(cx, |w, _| w.panels().laps.clone());
    assert_eq!(cx.update(|cx| laps.read(cx).selected(cx)), Some(3));
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        let rows = window.find("laps-table").label().unwrap().to_string();
        assert!(rows.starts_with("5 laps"), "{rows}");
        window.press("down", cx);
        window.press("alt-enter", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update(|cx| {
        let session = test.app.session.read(cx);
        let reference = session.reference().expect("Alt+Enter set a reference");
        assert_eq!(reference.lap_ref().session().as_ref(), run1.id);
        assert_eq!(reference.lap_ref().lap(), 4);
        assert_eq!(
            session.primary().unwrap().lap_ref().lap(),
            3,
            "the primary stays"
        );
    });
    // Enter sets the primary.
    cx.update_window(handle, |_, window, cx| {
        window.press("down", cx);
        window.press("enter", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update(|cx| {
        assert_eq!(
            test.app.session.read(cx).primary().unwrap().lap_ref().lap(),
            5
        );
    });
}

#[gpui_kit::test]
fn channel_switches_and_style_controls_write_preferences(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let handle: AnyWindowHandle = test.window.into();
    show_panel(&test, PanelKind::Channels, cx);
    let preferences = test.app.preferences.clone();
    let visible = |cx: &mut TestAppContext| {
        cx.update(|cx| preferences.read(cx).config().channel_style("speed").visible)
    };
    assert!(visible(cx), "speed shows by default");

    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        window.click("channel-visible:speed", cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert!(!visible(cx), "the switch hid speed");
    cx.update(|cx| preferences.update(cx, |preferences, cx| preferences.flush(cx)));
    assert_eq!(
        sandbox.read_config().channels["speed"].visible,
        Some(false),
        "written to omatrack.yml"
    );

    // Select speed and edit its stroke width.
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        window.click("channel:speed", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let channels = test
        .workspace
        .read_with(cx, |w, _| w.panels().channels.clone());
    assert_eq!(
        cx.update(|cx| channels.read(cx).selected(cx).map(|k| k.to_string())),
        Some("speed".to_string())
    );
    let stroke = |cx: &mut TestAppContext| {
        cx.update(|cx| preferences.read(cx).config().channels["speed"].stroke_width)
    };
    // Typed out of range: stored clamped.
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        window.click("channel-stroke-width-input", cx);
        window.press("ctrl-a", cx);
        window.input("0.1", cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(stroke(cx), Some(0.5), "clamped to the 0.5 minimum");
    // The slider's far end: the 4 px maximum.
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        let slider = window.find("channel-stroke-width").bounds();
        window.click_at(
            "channel-stroke-width",
            point(slider.size.width - px(1.), slider.size.height / 2.),
            cx,
        );
    })
    .unwrap();
    cx.run_until_parked();
    let width = stroke(cx).expect("the slider wrote a width");
    assert!((3.8..=4.0).contains(&width), "{width}");

    // Reset style keeps the visibility choice.
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        window.click("channel-reset-style", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update(|cx| {
        let config = preferences.read(cx).config();
        assert_eq!(config.channels["speed"].stroke_width, None);
        assert_eq!(config.channels["speed"].visible, Some(false));
    });

    // The palette command shows it again.
    cx.update_window(handle, |_, window, cx| {
        window.dispatch_action(
            Box::new(omatrack_app::panels::channels::ToggleChannel {
                key: "speed".into(),
            }),
            cx,
        );
    })
    .unwrap();
    cx.run_until_parked();
    assert!(visible(cx));
    assert_eq!(
        cx.update(|cx| channels.read(cx).list()),
        ChannelList::Channels
    );
}

#[gpui_kit::test]
async fn a_cursor_move_updates_the_inspector_but_not_the_corners_table(cx: &mut TestAppContext) {
    let scene = analysed(cx).await;
    let (corners, inspector) = scene.test.workspace.read_with(cx, |w, _| {
        (w.panels().corners.clone(), w.panels().inspector.clone())
    });
    let cursor = scene.test.app.cursor.clone();
    cx.update(|cx| cursor.update(cx, |cursor, cx| cursor.set_fraction(Some(0.2), cx)));
    cx.run_until_parked();
    cx.update_window(scene.handle, |_, window, cx| window.render_frame(cx))
        .unwrap();
    let before = cx.update(|cx| {
        (
            corners.read(cx).render_count(),
            inspector.read(cx).render_count(),
        )
    });
    let position = cx
        .update_window(scene.handle, |_, window, _| {
            window
                .find("inspector-position")
                .label()
                .unwrap()
                .to_string()
        })
        .unwrap();

    cx.update(|cx| cursor.update(cx, |cursor, cx| cursor.set_fraction(Some(0.6), cx)));
    cx.run_until_parked();
    // Draw like the platform does: only dirty views render (`render_frame`
    // refreshes every view, cached or not).
    cx.update_window(scene.handle, |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
    let after = cx.update(|cx| {
        (
            corners.read(cx).render_count(),
            inspector.read(cx).render_count(),
        )
    });
    assert!(after.1 > before.1, "the inspector re-rendered");
    assert_eq!(after.0, before.0, "the corners table did not re-render");
    cx.update_window(scene.handle, |_, window, _| {
        let moved = window
            .find("inspector-position")
            .label()
            .unwrap()
            .to_string();
        assert_ne!(moved, position, "the readout moved with the cursor");
        let speed = window.find("inspector:speed").label().unwrap().to_string();
        assert!(speed.starts_with("Speed: "), "{speed}");
        assert!(
            !speed.ends_with('—'),
            "a speed value at the cursor: {speed}"
        );
    })
    .unwrap();
}

#[gpui_kit::test]
async fn the_map_draws_gps_and_moves_the_cursor_on_click(cx: &mut TestAppContext) {
    let scene = analysed(cx).await;
    show_panel(&scene.test, PanelKind::Map, cx);
    let map = scene
        .test
        .workspace
        .read_with(cx, |w, _| w.panels().map.clone());
    cx.update(|cx| {
        let map = map.read(cx).map().read(cx);
        assert!(map.data().primary().is_some(), "the primary GPS lap");
        assert!(map.data().reference().is_some(), "the reference GPS lap");
        assert!(!map.data().corners().is_empty(), "corner labels");
    });
    cx.update_window(scene.handle, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("track-map").visible());
    })
    .unwrap();
    // A click on the primary lap moves the cursor there.
    let target = cx.update(|cx| map.read(cx).map().read(cx).primary_position(0.5));
    let target = target.expect("the map painted the primary lap");
    cx.update_window(scene.handle, |_, window, cx| {
        let origin = window.find("track-map").bounds().origin;
        window.click_at("track-map", target - origin, cx);
    })
    .unwrap();
    cx.run_until_parked();
    let fraction = cx
        .update(|cx| scene.test.app.cursor.read(cx).fraction())
        .expect("the click moved the cursor");
    assert!((fraction - 0.5).abs() < 0.05, "{fraction}");
}

// ── real recordings ─────────────────────────────────────────────────────

fn fixtures() -> String {
    std::env::var("OMATRACK_FIXTURES")
        .expect("set OMATRACK_FIXTURES to the 26T07_PLM folder of AiM MP4 recordings")
}

#[gpui_kit::test]
#[ignore]
async fn real_run4_against_run1_fills_the_corners_table_and_the_map(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    sandbox.write_config(&format!(
        "locations:\n  - type: folder\n    name: Fixtures\n    target: {}\n",
        fixtures()
    ));
    let test = common::start(cx, sandbox.options());
    let handle: AnyWindowHandle = test.window.into();
    let library = test.app.library.clone();
    cx.update(|cx| library.update(cx, |library, cx| library.rescan(cx)));
    cx.run_until_parked();
    cx.wait_for(handle, Duration::from_secs(600), |_, cx| {
        let library = library.read(cx);
        library.has_scanned() && !library.is_scanning()
    })
    .await;
    let (run4, run1) = cx.update(|cx| {
        let snapshot = library.read(cx).snapshot().clone();
        let find = |run: &str| {
            snapshot
                .sessions()
                .find(|node| node.file_name().contains(run))
                .cloned()
                .unwrap_or_else(|| panic!("{run} is in the library"))
        };
        (find("Run4"), find("Run1"))
    });
    cx.update_window(handle, |_, window, cx| {
        for (node, role) in [(&run4, Role::Primary), (&run1, Role::Reference)] {
            window.dispatch_action(
                Box::new(SelectLap {
                    session: node.id.clone().into(),
                    lap: node.best_lap_id.expect("a best lap"),
                    role,
                }),
                cx,
            );
        }
    })
    .unwrap();
    let session = test.app.session.clone();
    cx.run_until_parked();
    cx.wait_for(handle, Duration::from_secs(600), |_, cx| {
        let session = session.read(cx);
        !session.is_loading()
            && session
                .analysis()
                .is_some_and(|analysis| analysis.reference().is_some())
    })
    .await;
    cx.run_until_parked();
    cx.update_window(handle, |_, window, cx| window.render_frame(cx))
        .unwrap();

    let corners = test
        .workspace
        .read_with(cx, |w, _| w.panels().corners.clone());
    let lines = cx.update(|cx| {
        corners
            .read(cx)
            .table()
            .unwrap()
            .read(cx)
            .delegate()
            .lines()
            .to_vec()
    });
    assert!(
        lines.len() >= 10,
        "Road Atlanta has 10+ corners: {}",
        lines.len()
    );
    let dts: Vec<f64> = lines.iter().map(|line| line.dt()).collect();
    assert!(
        dts.windows(2)
            .all(|pair| pair[0] >= pair[1] || pair[1].is_nan()),
        "worst corner first: {dts:?}"
    );
    // Brake-point consistency arrives from the background.
    cx.wait_for(handle, Duration::from_secs(600), |_, cx| {
        !corners.read(cx).is_measuring()
    })
    .await;
    cx.run_until_parked();

    show_panel(&test, PanelKind::Map, cx);
    let map = test.workspace.read_with(cx, |w, _| w.panels().map.clone());
    cx.update(|cx| {
        let map = map.read(cx).map().read(cx);
        assert!(map.data().primary().is_some(), "Run4's GPS lap");
        assert!(map.data().reference().is_some(), "Run1's GPS lap");
        assert!(!map.data().centerline().is_empty(), "the atlas centerline");
    });
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        let label = window.find("track-map").label().unwrap().to_string();
        assert!(label.starts_with("Track map"), "{label}");
        assert!(!label.contains("no GPS"), "{label}");
    })
    .unwrap();
    assert!(
        cx.update(|cx| map.read(cx).map().read(cx).primary_position(0.5))
            .is_some(),
        "the map painted the GPS lap"
    );
}
