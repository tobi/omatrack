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

/// Write the synthetic recording as an MTJ document into `dir` (without
/// the two GPS channels unless `gps`).
fn write_recording(dir: &Path, gps: bool, lap_distance: bool) -> PathBuf {
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
    let mut channels: Vec<(&str, &str)> = if gps { &CHANNELS[..] } else { &CHANNELS[..6] }.to_vec();
    let count = (DURATION * RATE) as usize;
    let mut columns = vec![Vec::with_capacity(count); channels.len()];
    for i in 0..count {
        for (column, value) in columns.iter_mut().zip(sample(i as f64 / RATE)) {
            column.push(value);
        }
    }
    if lap_distance {
        // The logger's own lap distance: integrated speed restarting at
        // every lap start, scaled so every lap is one track length (the
        // synthetic laps last alike at different speeds).
        let lap_of = |i: usize| ((i as f64 / RATE - FIRST_LAP_START) / LAP_SECONDS).floor() as i64;
        let mut integral = vec![0.0; count];
        for i in 1..count {
            if lap_of(i) == lap_of(i - 1) {
                integral[i] = integral[i - 1] + sample(i as f64 / RATE)[0] / 3.6 / RATE;
            }
        }
        let mut totals = std::collections::BTreeMap::new();
        for i in 0..count {
            totals.insert(lap_of(i), integral[i]);
        }
        let track = (0..4).map(|lap| totals[&lap]).sum::<f64>() / 4.0;
        let distance: Vec<f64> = (0..count)
            .map(|i| integral[i] * track / totals[&lap_of(i)].max(1.0))
            .collect();
        channels.push(("Lap Distance", "m"));
        columns.push(distance);
    }
    let ns = |seconds: f64| (seconds * 1e9).round() as u64;
    let directory: Vec<String> = channels
        .iter()
        .map(|(name, unit)| format!("[\"{name}\",\"{unit}\",50,0,{count}]"))
        .collect();
    let mut document = format!(
        "{{\"mtj\":1,\"q\":20000000,\"dur\":{},\"nc\":{n},\"nsc\":{n},\"ns\":{},\"ch\":[{}],\"src\":\"telemetry\",\"drv\":\"Ada\",\"ven\":\"Synthetic Circuit\"}}\n",
        ns(DURATION),
        count * channels.len(),
        directory.join(","),
        n = channels.len(),
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
    for ((name, unit), values) in channels.iter().zip(&columns) {
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
    scan_recording(cx, true).await
}

async fn scan_recording(cx: &mut TestAppContext, gps: bool) -> Scene {
    scan_written(cx, gps, false).await
}

/// The synthetic recording with the logger's own lap distance: the laps
/// align on lap distance %, which places time loss.
async fn scan_with_lap_distance(cx: &mut TestAppContext) -> Scene {
    scan_written(cx, true, true).await
}

async fn scan_written(cx: &mut TestAppContext, gps: bool, lap_distance: bool) -> Scene {
    let sandbox = common::Sandbox::new();
    let recordings = sandbox.dir.path().join("recordings");
    write_recording(&recordings, gps, lap_distance);
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
    compare(scan_synthetic(cx).await, cx).await
}

/// [`analysed`] over the recording with the logger's lap distance: the
/// pair aligns on lap distance %, which places time loss.
async fn analysed_on_distance(cx: &mut TestAppContext) -> Scene {
    compare(scan_with_lap_distance(cx).await, cx).await
}

/// Compare the scene's second complete lap against its fourth.
async fn compare(scene: Scene, cx: &mut TestAppContext) -> Scene {
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
    // Time lost leads the right dock; the table is built when Corners shows.
    show_panel(&scene.test, PanelKind::Corners, cx);
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

    // The synthetic pair has no distance or GPS: a time-share alignment
    // keeps track order, so the first click on Δt sorts descending.
    cx.update(|cx| {
        let table = corners.read(cx).table().unwrap().read(cx);
        let column = gpui_kit::component::table::TableDelegate::column(table.delegate(), 2, cx);
        assert_eq!(column.sort, Some(ColumnSort::Default));
    });
    // Click the sort control of the Δt header.
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
        dts.windows(2).all(|pair| pair[0] >= pair[1]),
        "a header click re-sorts: {dts:?}"
    );
    cx.update(|cx| {
        let table = corners.read(cx).table().unwrap().read(cx);
        let column = gpui_kit::component::table::TableDelegate::column(table.delegate(), 2, cx);
        assert_eq!(column.sort, Some(ColumnSort::Descending));
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
        // The table is built (and takes focus) on the frame that first
        // shows the tab.
        window.render_frame(cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(scene.handle, |_, window, cx| {
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
    // The inspector is a tab beside the map.
    show_panel(&scene.test, PanelKind::Inspector, cx);
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

// ── where the time goes ─────────────────────────────────────────────────

fn time_goes(
    scene: &Scene,
    cx: &mut TestAppContext,
) -> gpui_kit::Entity<omatrack_app::panels::TimeGoesPanel> {
    scene
        .test
        .workspace
        .read_with(cx, |w, _| w.panels().time_goes.clone())
}

fn selected_corner(scene: &Scene, cx: &mut TestAppContext) -> Option<SharedString> {
    let panel = time_goes(scene, cx);
    cx.update(|cx| panel.read(cx).selected().map(|line| line.id().clone()))
}

#[gpui_kit::test]
async fn where_the_time_goes_leads_the_right_dock_largest_loss_first(cx: &mut TestAppContext) {
    let scene = analysed_on_distance(cx).await;
    cx.update(|cx| {
        let analysis = scene.test.app.session.read(cx).analysis().unwrap().clone();
        assert_eq!(analysis.comparison().unwrap().basis(), "Lap distance %");
        assert!(analysis.time_loss_placed());
    });
    let panel = time_goes(&scene, cx);
    let (ids, dts, split, final_delta) = cx.update(|cx| {
        let panel = panel.read(cx);
        let analysis = scene.test.app.session.read(cx).analysis().unwrap().clone();
        (
            panel
                .lines()
                .iter()
                .map(|line| line.id().clone())
                .collect::<Vec<_>>(),
            panel
                .lines()
                .iter()
                .map(|line| line.dt())
                .collect::<Vec<_>>(),
            analysis.time_split().expect("a reference: a split"),
            *analysis.delta().last().unwrap(),
        )
    });
    assert!(dts.len() >= 2, "the synthetic laps have corners: {dts:?}");
    assert!(
        dts.windows(2).all(|pair| pair[0] >= pair[1]),
        "largest loss first: {dts:?}"
    );
    assert!((split.corners + split.straights - final_delta).abs() < 1e-9);
    // Nothing focused, the cursor outside every corner: the largest loss.
    assert_eq!(selected_corner(&scene, cx), Some(ids[0].clone()));
    cx.update_window(scene.handle, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("time-goes-panel").visible(), "the default tab");
        assert!(window.find("track-map").visible(), "the heat map");
        let table = window.find("time-goes-table");
        assert!(table.visible());
        let first = window.find(SharedString::from(format!("time-goes-row-{}", ids[0])));
        let second = window.find(SharedString::from(format!("time-goes-row-{}", ids[1])));
        assert!(first.bounds().top() < second.bounds().top(), "row order");
        assert_eq!(first.selected(), Some(true));
        assert_eq!(second.selected(), Some(false));
        let split = window.find("time-goes-split").label().unwrap().to_string();
        assert!(split.starts_with("Corners ≈"), "{split}");
        assert!(split.contains("km/h"), "{split}");
        let card = window.find("time-goes-card").label().unwrap().to_string();
        assert!(card.contains(" km/h"), "units are spaced: {card}");
        assert!(card.contains("vs R"), "{card}");
    })
    .unwrap();

    // The map is in heat mode on the one loss rate, all finite.
    cx.update(|cx| {
        let panel = panel.read(cx);
        let data = panel.map().read(cx).data().clone();
        assert!(data.is_heat());
        let heat = data.heat().unwrap();
        assert!(heat.iter().all(|value| value.is_finite()));
        let analysis = scene.test.app.session.read(cx).analysis().unwrap().clone();
        assert_eq!(heat, analysis.loss_rate());
        assert!(data.reference().is_some(), "the shared map still resolves");
    });
}

#[gpui_kit::test]
async fn the_time_goes_card_follows_the_selected_corner(cx: &mut TestAppContext) {
    let scene = analysed_on_distance(cx).await;
    let panel = time_goes(&scene, cx);
    let lap_order: Vec<SharedString> = cx.update(|cx| {
        let analysis = scene.test.app.session.read(cx).analysis().unwrap().clone();
        analysis
            .corners()
            .iter()
            .map(|zone| SharedString::from(zone.id.clone()))
            .collect()
    });
    // J focuses the first corner in lap order: the card follows it.
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
    assert_eq!(selected_corner(&scene, cx), Some(lap_order[0].clone()));
    cx.update_window(scene.handle, |_, window, cx| {
        window.render_frame(cx);
        let row = window.find(SharedString::from(format!(
            "time-goes-row-{}",
            lap_order[0]
        )));
        assert_eq!(row.selected(), Some(true));
    })
    .unwrap();

    // A row click focuses that corner (FocusCorner) and the card follows.
    let target = lap_order[1].clone();
    cx.update_window(scene.handle, |_, window, cx| {
        window.click(SharedString::from(format!("time-goes-row-{target}")), cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.executor().advance_clock(Duration::from_millis(300));
    cx.run_until_parked();
    assert_eq!(selected_corner(&scene, cx), Some(target.clone()));
    assert_eq!(
        cx.update(|cx| scene.test.workspace.read(cx).focused_corner()),
        Some(1)
    );
    let (name, notes) = cx.update(|cx| {
        let line = panel.read(cx).selected().unwrap().clone();
        (
            line.name().to_string(),
            line.notes().map(|n| n.to_string()).collect::<Vec<_>>(),
        )
    });
    assert!(!notes.is_empty(), "the checks' notes (or Closely matched)");
    for note in &notes {
        assert!(note.ends_with('.'), "a sentence: {note}");
        assert!(
            note.chars().next().is_some_and(char::is_uppercase),
            "{note}"
        );
    }
    cx.update_window(scene.handle, |_, window, cx| {
        window.render_frame(cx);
        let card = window.find("time-goes-card").label().unwrap().to_string();
        assert!(card.starts_with(&name), "{card}");
        for note in &notes {
            assert!(card.contains(note.as_str()), "{card} has {note}");
        }
    })
    .unwrap();
}

#[gpui_kit::test]
async fn open_in_detail_focuses_the_corner_and_shows_the_corners_table(cx: &mut TestAppContext) {
    let scene = analysed(cx).await;
    let worst = selected_corner(&scene, cx).expect("a corner is selected");
    cx.update_window(scene.handle, |_, window, cx| {
        assert!(window.try_find("corners-panel").is_none(), "a tab behind");
        window.click("time-goes-open-corner", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.executor().advance_clock(Duration::from_millis(300));
    cx.run_until_parked();
    let ix = cx.update(|cx| {
        let analysis = scene.test.app.session.read(cx).analysis().unwrap().clone();
        analysis
            .corners()
            .iter()
            .position(|zone| zone.id == worst.as_ref())
    });
    assert_eq!(
        cx.update(|cx| scene.test.workspace.read(cx).focused_corner()),
        ix
    );
    cx.update_window(scene.handle, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("corners-panel").visible(), "the Corners tab");
    })
    .unwrap();
    cx.run_until_parked();
    let corners = scene
        .test
        .workspace
        .read_with(cx, |w, _| w.panels().corners.clone());
    assert_eq!(
        cx.update(|cx| corners.read(cx).selected(cx).cloned()),
        Some(worst)
    );
}

#[gpui_kit::test]
async fn without_gps_or_logger_distance_time_loss_is_not_placed(cx: &mut TestAppContext) {
    // Lap time %: the delta is the lap-time gap spread evenly, so the panel
    // says so instead of ranking corners (no heat, table or split).
    let scene = analysed(cx).await;
    let panel = time_goes(&scene, cx);
    let lap_order: Vec<SharedString> = cx.update(|cx| {
        let analysis = scene.test.app.session.read(cx).analysis().unwrap().clone();
        assert_eq!(analysis.comparison().unwrap().basis(), "Lap time %");
        assert!(!analysis.time_loss_placed());
        analysis
            .corners()
            .iter()
            .map(|zone| SharedString::from(zone.id.clone()))
            .collect()
    });
    cx.update(|cx| {
        let panel = panel.read(cx);
        assert!(!panel.map().read(cx).data().is_heat(), "no heat to show");
        let ids: Vec<_> = panel.lines().iter().map(|l| l.id().clone()).collect();
        assert_eq!(ids, lap_order, "lap order, never ranked");
    });
    cx.update_window(scene.handle, |_, window, cx| {
        window.render_frame(cx);
        let notice = window.find("time-goes-unplaced");
        assert!(notice.visible());
        assert!(notice.label().unwrap().contains("lap time"));
        assert!(window.try_find("time-goes-table").is_none());
        assert!(window.try_find("time-goes-split").is_none());
        assert!(window.try_find("time-goes-legend").is_none());
        let card = window.find("time-goes-card").label().unwrap().to_string();
        assert!(!card.contains("vs R"), "no cross-lap entry delta: {card}");
        assert!(card.contains(" km/h"), "{card}");
    })
    .unwrap();
}

#[gpui_kit::test]
async fn without_gps_the_time_goes_panel_has_no_map(cx: &mut TestAppContext) {
    let scene = compare(scan_recording(cx, false).await, cx).await;
    let panel = time_goes(&scene, cx);
    cx.update(|cx| {
        let panel = panel.read(cx);
        assert!(panel.map().read(cx).data().is_empty(), "nothing to draw");
        assert!(panel.lines().len() >= 2);
    });
    cx.update_window(scene.handle, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find("track-map").is_none(), "no map without GPS");
        assert!(window.find("time-goes-unplaced").visible());
        assert!(window.find("time-goes-card").visible());
    })
    .unwrap();
}

#[gpui_kit::test]
async fn a_cursor_move_leaves_the_heat_map_static_layer_alone(cx: &mut TestAppContext) {
    let scene = analysed_on_distance(cx).await;
    let panel = time_goes(&scene, cx);
    let map = cx.update(|cx| panel.read(cx).map().clone());
    cx.update_window(scene.handle, |_, window, cx| window.render_frame(cx))
        .unwrap();
    let before = cx.update(|cx| map.read(cx).geometry_builds());
    assert!(before > 0, "the heat lap was meshed");
    for fraction in [0.1, 0.3, 0.5] {
        cx.update(|cx| {
            scene
                .test
                .app
                .cursor
                .update(cx, |cursor, cx| cursor.set_fraction(Some(fraction), cx));
        });
        cx.update_window(scene.handle, |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
    }
    // Inside the dock the layer may re-render from its geometry cache (see
    // traces_panel.rs); the meshes are never rebuilt for a cursor move.
    assert_eq!(cx.update(|cx| map.read(cx).geometry_builds()), before);
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
    // Corners is a tab behind Where the time goes; its table is built
    // when it is shown.
    show_panel(&test, PanelKind::Corners, cx);

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
    // Run4's GPS misses the centreline, so the pair aligns by a share of
    // lap time: its Δt ranks corner length, and the table keeps track
    // order instead of sorting by it.
    let orders: Vec<usize> = lines.iter().map(|line| line.order()).collect();
    assert!(
        orders.windows(2).all(|pair| pair[0] < pair[1]),
        "track order under a time-share alignment: {orders:?}"
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

#[gpui_kit::test]
async fn the_first_analysis_puts_the_cursor_at_lap_start_for_every_readout(
    cx: &mut TestAppContext,
) {
    let scene = analysed(cx).await;
    show_panel(&scene.test, PanelKind::Inspector, cx);
    let cursor = scene.test.app.cursor.clone();
    assert_eq!(
        cx.update(|cx| cursor.read(cx).fraction()),
        Some(0.0),
        "the cursor starts where the video and HUD are: lap start"
    );
    cx.update_window(scene.handle, |_, window, _| {
        let inspector = window
            .find("inspector-position")
            .label()
            .unwrap()
            .to_string();
        assert!(inspector.starts_with("0 m"), "{inspector}");
        let speed = window.find("inspector:speed").label().unwrap().to_string();
        assert!(!speed.ends_with('—'), "a value at lap start: {speed}");
    })
    .unwrap();

    // Clearing the cursor reads as "No cursor".
    cx.update(|cx| cursor.update(cx, |cursor, cx| cursor.set_fraction(None, cx)));
    cx.run_until_parked();
    cx.update_window(scene.handle, |_, window, cx| {
        window.render_frame(cx);
        assert_eq!(window.find("inspector-position").label(), Some("No cursor"));
    })
    .unwrap();
}

#[gpui_kit::test]
async fn the_title_bar_states_the_pair_its_gap_and_its_sync(cx: &mut TestAppContext) {
    let scene = analysed(cx).await;
    let session = scene.test.app.session.clone();
    let (basis, anchors, confidence, lap_delta, primary, reference) = cx.update(|cx| {
        let session = session.read(cx);
        let analysis = session.analysis().unwrap();
        let comparison = analysis.comparison().unwrap().clone();
        let info = |role: Role| {
            let info = session.slot(role).unwrap().info();
            format!("{} {}", info.label, info.time)
        };
        (
            comparison.basis().to_owned(),
            comparison.alignment().gps_anchors,
            comparison.confidence().to_owned(),
            analysis.lap_time_delta().expect("both laps are timed"),
            info(Role::Primary),
            info(Role::Reference),
        )
    });
    let summary = omatrack_app::workspace::header::sync_summary(&basis, anchors, &confidence, None);
    let (delta_text, _) =
        omatrack_ui::format_delta(Some(lap_delta), 3, omatrack_ui::DeltaSense::LowerIsBetter);
    let phrase = omatrack_app::workspace::header::lap_time_phrase(lap_delta);
    cx.update_window(scene.handle, |_, window, cx| {
        window.render_frame(cx);
        let track = window.find("header-track").bounds();
        let p = window.find("header-primary");
        let r = window.find("header-reference");
        assert!(
            p.label()
                .unwrap()
                .starts_with(&format!("Primary lap {primary}")),
            "{:?}",
            p.label()
        );
        assert!(
            r.label()
                .unwrap()
                .starts_with(&format!("Reference lap {reference}")),
            "{:?}",
            r.label()
        );
        // The lap-time Δ is exact at any confidence: never `≈`.
        let delta = window.find("header-delta");
        assert_eq!(
            delta.label().map(str::to_owned),
            Some(format!("Lap time {delta_text} {phrase}"))
        );
        let sync = window.find("header-sync");
        assert_eq!(
            sync.label().map(str::to_owned),
            Some(format!("Reference sync: {summary}"))
        );
        // One reading order on one line: track, P against R, Δ, sync.
        let (p, r, delta, sync) = (p.bounds(), r.bounds(), delta.bounds(), sync.bounds());
        assert!(track.right() <= p.left());
        assert!(p.right() < r.left());
        assert!(r.right() <= delta.left());
        assert!(delta.right() <= sync.left());
        for bounds in [p, r, delta, sync] {
            assert!(bounds.top() >= px(0.) && bounds.bottom() <= track.bottom() + px(12.));
        }
        // The filmstrip stays, full width below the title bar.
        let strip = window.find("filmstrip-primary").bounds();
        assert!(strip.top() >= sync.bottom());
        // (Both laps are of one recording here: one row, both roles.)
        // Nothing the title bar says is repeated in the status bar.
        for gone in ["status-sync", "status-cursor", "status-delta"] {
            assert!(window.try_find(gone).is_none(), "{gone}");
        }
        assert!(window.find("theme-status").visible());
    })
    .unwrap();

    // The sync button opens the strategy menu; choosing a strategy asks
    // the session for it.
    let (choice, title) = cx.update(|cx| {
        let session = session.read(cx);
        let analysis = session.analysis().unwrap();
        let current = analysis.strategy();
        // Another strategy when there is one; else the one in effect,
        // asked for explicitly instead of automatically.
        let available = analysis.available_strategies();
        let choice = *available
            .iter()
            .find(|strategy| Some(**strategy) != current)
            .or(available.first())
            .expect("a strategy");
        (choice, choice.label())
    });
    cx.update_window(scene.handle, |_, window, cx| {
        assert!(window.try_find("popup-menu").is_none());
        window.click("header-sync", cx);
        window.render_frame(cx);
        assert!(window.try_find("popup-menu").is_some(), "the menu opened");
        let mut menu = window.within("popup-menu");
        let item = (0..16usize)
            .find(|ix| {
                menu.try_find(*ix)
                    .is_some_and(|item| item.label() == Some(title))
            })
            .expect("the strategy is offered");
        menu.click(item, cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(
        cx.update(|cx| session.read(cx).strategy()),
        omatrack_core::session::StrategyRequest::Prefer(choice)
    );
}

#[gpui_kit::test]
async fn a_pill_goes_to_the_lap_list_and_the_swap_button_swaps(cx: &mut TestAppContext) {
    let scene = analysed(cx).await;
    let session = scene.test.app.session.clone();
    let workspace = scene.test.workspace.clone();
    let before = cx.update(|cx| {
        let session = session.read(cx);
        (
            session.primary().unwrap().lap_ref().clone(),
            session.reference().unwrap().lap_ref().clone(),
        )
    });
    cx.update_window(scene.handle, |_, window, cx| {
        window.click("header-reference", cx);
        window.render_frame(cx);
        let library = workspace.read_with(cx, |w, cx| w.panels().focus_handle(PanelKind::Laps, cx));
        assert!(
            library.contains_focused(window, cx),
            "the lap list has focus"
        );
        window.click("header-swap", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.wait_for(scene.handle, Duration::from_secs(60), |_, cx| {
        let session = session.read(cx);
        !session.is_loading() && session.primary().map(|slot| slot.lap_ref()) == Some(&before.1)
    })
    .await;
    assert_eq!(
        cx.update(|cx| session.read(cx).reference().unwrap().lap_ref().clone()),
        before.0
    );
}

#[gpui_kit::test]
async fn a_focused_corner_is_emphasised_on_the_map(cx: &mut TestAppContext) {
    let scene = analysed(cx).await;
    show_panel(&scene.test, PanelKind::Map, cx);
    let map = scene
        .test
        .workspace
        .read_with(cx, |w, _| w.panels().map.clone());
    assert_eq!(
        cx.update(|cx| map.read(cx).map().read(cx).focused_corner()),
        None
    );
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
    let ix = cx
        .update(|cx| scene.test.workspace.read(cx).focused_corner())
        .expect("J focused a corner");
    assert_eq!(
        cx.update(|cx| map.read(cx).map().read(cx).focused_corner()),
        Some(u32::try_from(ix + 1).unwrap()),
        "the map follows the corner focus"
    );
}
