//! Telemetry <-> video sync.
//!
//! - Pure tests of the sync rules: the reference target is the comparison's
//!   mapping, the countdown state machine, continuous adoption keeping the
//!   playhead at 33%, and the end-of-lap transitions.
//! - Headless UI integration tests with a mock clock and without libmpv: the
//!   cursor follows the clock through the video panel's display-frame pull,
//!   sync leaves the static trace layer alone, keys 1-5 compose, F/Escape
//!   open and close the fullscreen stage (controls auto-hide, telemetry
//!   band, drag end persisted), the countdown and continuous adoption.
//! - `real_*` (ignored; `OMATRACK_FIXTURES`): Run4's and Run1's fastest laps
//!   through libmpv with a null audio output. The recordings are read-only.

mod common;

use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::time::{Duration, Instant};

use gpui_kit::component::Root;
use gpui_kit::test::TestWindowExt as _;
use gpui_kit::{
    AnyWindowHandle, AppContext as _, Entity, InputEvent as _, Modifiers, MouseButton,
    MouseDownEvent, MouseMoveEvent, MouseUpEvent, PlatformInput, TestAppContext, point, px, size,
};
use mpv_player::PlaybackClock;
use omatrack_app::actions::Role;
use omatrack_app::panels::PanelKind;
use omatrack_app::state::{ComposeLayout, VideoController, VideoEvent};
use omatrack_app::sync::identity::IdentityStatus;
use omatrack_app::sync::lap_end::{self, COUNTDOWN_FROM, LAP_END_FRACTION, LapAdvance, LapEnd};
use omatrack_app::sync::pacing::REFERENCE_DRIFT_SECONDS;
use omatrack_app::sync::{
    LapTimeline, PacerCommand, PacerInput, PairMap, ReferencePacer, VideoMap,
};
use omatrack_core::alignment::Strategy;
use omatrack_core::laps::LapKind;
use omatrack_core::playback::{PLAYING_SEEK_ERROR, ReferencePlayback, SyncState};
use omatrack_core::session::LoadedLap;
use omatrack_core::unify::UnifiedLap;
use omatrack_core::{Comparison, Lap, LoadOptions, RawChannel, Recording, load_lap};
use omatrack_trace::{TraceStack, Viewport, synthetic};

// ── synthetic data ───────────────────────────────────────────────────

const RATE: f64 = 50.0;

/// A lap of `seconds` at 50 Hz whose speed shape repeats every lap; the
/// reference is `slowdown` times slower everywhere.
fn unified(seconds: f64, start: f64, slowdown: f64) -> Arc<UnifiedLap> {
    let count = (seconds * slowdown * RATE) as usize + 1;
    let time: Vec<f64> = (0..count).map(|i| i as f64 / RATE).collect();
    let n = (count - 1) as f64;
    let speed: Vec<f64> = (0..count)
        .map(|i| 150.0 + 50.0 * (i as f64 / n * std::f64::consts::TAU).sin())
        .collect();
    let mut distance = Vec::with_capacity(count);
    let mut travelled = 0.0;
    for (i, kmh) in speed.iter().enumerate() {
        if i > 0 {
            travelled += kmh / 3.6 / RATE;
        }
        distance.push(travelled);
    }
    Arc::new(UnifiedLap {
        start_time: start,
        gear: vec![4; count],
        time,
        speed,
        distance,
        ..UnifiedLap::default()
    })
}

fn pair_fixture() -> (PairMap, Arc<Comparison>) {
    let primary = unified(60.0, 100.0, 1.0);
    let reference = unified(60.0, 40.0, 1.05);
    let comparison = Arc::new(Comparison::new(
        primary.clone(),
        reference.clone(),
        Strategy::LapPercentage,
        Vec::new(),
        0.0,
    ));
    let map = PairMap::new(
        LapTimeline::from_unified(8, primary, VideoMap::Offset(5.0)),
        LapTimeline::from_unified(3, reference, VideoMap::Offset(-2.0)),
        comparison.clone(),
        vec![(0.2, 0.3), (0.6, 0.7)],
    );
    (map, comparison)
}

const LAP_SECONDS: f64 = 20.0;
const FIRST_LAP_START: f64 = 5.0;
const DURATION: f64 = 70.0;

/// A synthetic recording: out, laps 1-3 of 20 s, in.
fn recording() -> Arc<Recording> {
    let count = (DURATION * RATE) as usize;
    let mut speed = Vec::with_capacity(count);
    let mut throttle = Vec::with_capacity(count);
    let mut gear = Vec::with_capacity(count);
    for i in 0..count {
        let t = i as f64 / RATE;
        let phase = ((t - FIRST_LAP_START) / LAP_SECONDS).rem_euclid(1.0);
        speed.push(120.0 + 80.0 * phase);
        throttle.push(100.0 * phase);
        gear.push(2.0 + (phase * 4.0).floor());
    }
    let channel = |name: &str, unit: &str, values: Vec<f64>| {
        RawChannel::synthetic(name, unit, RATE, DURATION, values)
    };
    let mut recording = Recording::synthetic(
        "",
        vec![
            channel("Speed", "km/h", speed),
            channel("Throttle Pos", "%", throttle),
            channel("Gear", "", gear),
        ],
    );
    let mut laps = Vec::new();
    let mut out = Lap::new(0, 0.0, FIRST_LAP_START, FIRST_LAP_START * 1000.0, false);
    out.kind = LapKind::Out;
    laps.push(out);
    for id in 1..=3 {
        let start = FIRST_LAP_START + f64::from(id - 1) * LAP_SECONDS;
        let mut lap = Lap::new(id, start, start + LAP_SECONDS, LAP_SECONDS * 1000.0, true);
        lap.kind = LapKind::Flying;
        laps.push(lap);
    }
    let mut last = Lap::new(4, 65.0, DURATION, 5_000.0, false);
    last.kind = LapKind::In;
    laps.push(last);
    recording.set_source_laps(laps);
    Arc::new(recording)
}

fn loaded(recording: &Arc<Recording>, lap: i32) -> LoadedLap {
    load_lap(
        recording.clone(),
        lap,
        &LoadOptions::default(),
        &AtomicBool::new(false),
    )
    .expect("synthetic lap loads")
}

// ── pure rules ───────────────────────────────────────────────────────

#[test]
fn reference_target_is_the_comparison_mapping() {
    let (map, comparison) = pair_fixture();
    for fraction in [0.0, 0.1, 0.25, 0.5, 0.77, 1.0] {
        let reference_time = comparison.compare_time_for_primary_fraction(fraction);
        assert!(reference_time >= 0.0, "a lap-% map exists");
        // Reference lap time -> file time (start 40 s) -> video (offset -2).
        let expected = 40.0 + reference_time - 2.0;
        let target = map.reference_target(fraction).unwrap();
        assert!(
            (target - expected).abs() < 1e-9,
            "{fraction}: {target} vs {expected}"
        );
        assert_eq!(
            Some(target),
            map.reference()
                .video_at_lap_time(comparison.compare_time_for_primary_fraction(fraction))
        );
    }
    // The primary's own station: file time 100 s + lap time, offset +5.
    let primary = map.primary().video_at_fraction(0.5).unwrap();
    assert!((primary - (100.0 + 30.0 + 5.0)).abs() < 1e-9, "{primary}");
}

#[test]
fn pacer_hard_seeks_to_the_mapped_station_and_paces_from_it() {
    let (map, _) = pair_fixture();
    let clock = PlaybackClock::new();
    let mut pacer = ReferencePacer::new(clock, ReferencePlayback::Gps);
    let cursor = 0.4;
    let target = map.reference_target(cursor).unwrap();
    let primary = map.primary().video_at_fraction(cursor).unwrap();

    // A forced decision (jump, lap change) seeks exactly to the map.
    let forced = pacer.step(&map, PacerInput::new(cursor, primary, 0.0).force(true));
    assert_eq!(
        forced,
        PacerCommand::Seek {
            target,
            rate: 1.0,
            verify: false
        }
    );
    // Paused: align and verify.
    let paused = pacer.step(&map, PacerInput::new(cursor, primary, 0.0).paused(true));
    assert!(matches!(paused, PacerCommand::Seek { verify: true, .. }));
    assert_eq!(pacer.state(), SyncState::Aligning);
    assert_eq!(pacer.verify_paused(&map, cursor, target), None);
    assert_eq!(pacer.state(), SyncState::Locked);

    // Aligned and running: GPS pacing follows the local slope (the
    // reference is 5% slower, so it runs about 5% faster), no seek.
    let aligned = pacer.step(&map, PacerInput::new(cursor, primary + 0.1, target));
    match aligned {
        PacerCommand::SetSpeed(rate) => assert!((rate - 1.05).abs() < 0.02, "{rate}"),
        other => panic!("{other:?}"),
    }
    // Slow motion multiplies the paced rate.
    let slow = pacer.step(
        &map,
        PacerInput::new(cursor, primary + 0.2, target).clock_rate(0.25),
    );
    match slow {
        PacerCommand::SetSpeed(rate) => assert!((rate - 0.26).abs() < 0.01, "{rate}"),
        other => panic!("{other:?}"),
    }
    // A primary jump between decisions hard-seeks.
    let jumped = pacer.step(&map, PacerInput::new(cursor, primary + 5.0, target));
    assert!(matches!(jumped, PacerCommand::Seek { .. }), "{jumped:?}");
    // Drift past the threshold hard-seeks back onto the station.
    let drifted = pacer.step(
        &map,
        PacerInput::new(
            cursor,
            primary + 5.05,
            target - REFERENCE_DRIFT_SECONDS - 0.5,
        ),
    );
    assert_eq!(
        drifted,
        PacerCommand::Seek {
            target,
            rate: 1.0,
            verify: false
        }
    );
}

#[test]
fn recording_pacing_never_corrects_drift_between_resyncs() {
    let (map, _) = pair_fixture();
    let mut pacer = ReferencePacer::new(PlaybackClock::new(), ReferencePlayback::Recording);
    let cursor = 0.5;
    let target = map.reference_target(cursor).unwrap();
    let primary = map.primary().video_at_fraction(cursor).unwrap();
    pacer.step(&map, PacerInput::new(cursor, primary, target).force(true));
    let drifting = pacer.step(&map, PacerInput::new(cursor, primary + 0.1, target - 3.0));
    assert!(
        !matches!(drifting, PacerCommand::Seek { .. }),
        "recording speed only re-syncs at lap start, jumps and pause: {drifting:?}"
    );
    assert_eq!(pacer.state(), SyncState::RealTime);
}

#[test]
fn countdown_counts_three_two_one_then_waits_for_the_next_lap() {
    let mut advance = LapAdvance::start(9);
    assert_eq!(advance.countdown(), Some(COUNTDOWN_FROM));
    assert_eq!(advance.tick(), None);
    assert_eq!(advance.countdown(), Some(2));
    assert_eq!(advance.tick(), None);
    assert_eq!(advance.countdown(), Some(1));
    assert_eq!(advance.tick(), Some(9), "after 1 the next lap is selected");
    assert_eq!(advance, LapAdvance::Resuming { next_lap: 9 });
    assert_eq!(advance.countdown(), None);
    assert_eq!(advance.tick(), None, "no second selection");
    assert!(!advance.resume_into(8), "another lap does not resume");
    assert!(advance.resume_into(9));
    assert_eq!(advance, LapAdvance::Idle);

    let mut cancelled = LapAdvance::start(4);
    cancelled.tick();
    cancelled.cancel();
    assert_eq!(cancelled, LapAdvance::Idle);
    assert_eq!(cancelled.tick(), None);
}

#[test]
fn lap_end_transitions() {
    assert_eq!(
        lap_end::lap_end(false, Some(9)),
        LapEnd::Countdown { next_lap: 9 }
    );
    assert_eq!(
        lap_end::lap_end(true, Some(9)),
        LapEnd::Adopt { next_lap: 9 }
    );
    assert_eq!(lap_end::lap_end(false, None), LapEnd::Stop);
    assert_eq!(lap_end::lap_end(true, None), LapEnd::Stop);
    // Edge-triggered: only when crossing into the end.
    assert!(lap_end::reached_lap_end(Some(0.98), 1.0));
    assert!(lap_end::reached_lap_end(None, LAP_END_FRACTION));
    assert!(!lap_end::reached_lap_end(Some(1.0), 1.0));
    assert!(!lap_end::reached_lap_end(Some(0.5), 0.99));
    assert!(!lap_end::should_prefetch(0.7));
    assert!(lap_end::should_prefetch(0.71));
}

#[test]
fn continuous_adoption_takes_the_cursor_from_the_video_and_keeps_the_playhead_at_33_percent() {
    // Two consecutive laps of one file: the video keeps running across.
    let lap8 = LapTimeline::from_unified(8, unified(60.0, 100.0, 1.0), VideoMap::Offset(5.0));
    let lap9 = LapTimeline::from_unified(9, unified(61.0, 160.0, 1.0), VideoMap::Offset(5.0));
    let video = lap8.video_at_fraction(1.0).unwrap() + 0.61;
    assert_eq!(lap8.fraction_at_video(video), Some(1.0), "past lap 8");
    let cursor = lap_end::adopted_cursor(&lap9, video).unwrap();
    assert!((cursor - 0.61 / 61.0).abs() < 1e-6, "{cursor}");

    let viewport = Viewport::new(0.6, 0.8);
    let followed = lap_end::follow_viewport(viewport, cursor);
    assert!((followed.span() - viewport.span()).abs() < 1e-12);
    let share = (cursor - followed.start) / followed.span();
    assert!((share - 0.33).abs() < 1e-9, "{share}");
    assert!(followed.start < 0.0, "unclamped into the previous lap");
}

#[test]
fn identity_status_follows_the_core_states() {
    use omatrack_core::session::IdentityState;
    assert!(IdentityStatus::Trusted(IdentityState::ExactSource).is_trusted());
    assert!(IdentityStatus::External.is_trusted());
    assert!(!IdentityStatus::Checking.is_trusted());
    let untrusted = IdentityStatus::Untrusted {
        state: IdentityState::Mismatch,
        message: "The video was changed after telemetry conversion.".into(),
    };
    assert!(!untrusted.is_trusted());
    assert!(untrusted.summary().contains("Video sync is off"));
}

// ── headless UI integration (mock clock, no libmpv) ─────────────────

/// Offset of the mock video: presentation = telemetry + 3 s.
const VIDEO_OFFSET: f64 = 3.0;

struct Mock {
    test: common::TestApp,
    handle: AnyWindowHandle,
    clock: PlaybackClock,
    recording: Arc<Recording>,
    _sandbox: common::Sandbox,
}

impl Mock {
    fn video(&self) -> Entity<VideoController> {
        self.test.app.video.clone()
    }

    /// Put the mock clock at `seconds`, holding still (speed 0) so the
    /// estimate is exact whatever the wall clock does.
    fn set_clock(&self, seconds: f64) {
        let now = Instant::now();
        self.clock.set_speed(0.0, now);
        self.clock.seek_started(seconds, now);
        self.clock.seek_finished(now);
    }

    /// Deliver one display frame to the workspace window.
    fn frame(&self, cx: &mut TestAppContext) -> usize {
        let ran = cx
            .update_window(self.handle, |_, window, cx| window.simulate_next_frame(cx))
            .unwrap();
        cx.run_until_parked();
        ran
    }

    fn cursor(&self, cx: &mut TestAppContext) -> Option<f64> {
        cx.update(|cx| self.test.app.cursor.read(cx).fraction())
    }
}

/// The workspace with a synthetic primary lap on a mock clock.
fn mock(cx: &mut TestAppContext, lap_id: i32) -> Mock {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let handle: AnyWindowHandle = test.window.into();
    let recording = recording();
    let lap = loaded(&recording, lap_id);
    let clock = PlaybackClock::new();
    let video = test.app.video.clone();
    let (bound_clock, bound_lap) = (clock.clone(), lap);
    cx.update(|cx| {
        video.update(cx, |video, cx| {
            video.attach_external_clock(bound_clock, VideoMap::Offset(VIDEO_OFFSET), cx);
            video.bind(Some(&bound_lap), cx);
        })
    });
    cx.run_until_parked();
    cx.update_window(handle, |_, window, cx| window.render_frame(cx))
        .unwrap();
    Mock {
        test,
        handle,
        clock,
        recording,
        _sandbox: sandbox,
    }
}

/// Video time of a lap fraction on the mock clock.
fn video_at(mock: &Mock, fraction: f64, cx: &mut TestAppContext) -> f64 {
    cx.update(|cx| {
        mock.video()
            .read(cx)
            .timeline(Role::Primary)
            .unwrap()
            .video_at_fraction(fraction)
            .unwrap()
    })
}

fn play(mock: &Mock, cx: &mut TestAppContext) {
    let video = mock.video();
    cx.update(|cx| video.update(cx, |video, cx| video.toggle_play(cx)));
    cx.run_until_parked();
    assert!(cx.update(|cx| video.read(cx).is_playing()));
}

#[gpui_kit::test]
fn the_cursor_follows_the_mock_clock_and_leaves_the_static_traces_alone(cx: &mut TestAppContext) {
    let mock = mock(cx, 2);
    let video = mock.video();
    assert!(cx.update(|cx| video.read(cx).is_synced()));
    // Lap 2 starts at telemetry 25 s: its video starts at 28 s.
    assert!((video_at(&mock, 0.0, cx) - (25.0 + VIDEO_OFFSET)).abs() < 1e-9);

    // A trace stack sharing the workspace's viewport and cursor, in its own
    // window, drawn without refreshing (cached views stay cached).
    let (viewport, cursor) = (mock.test.app.viewport.clone(), mock.test.app.cursor.clone());
    let mut stack: Option<Entity<TraceStack>> = None;
    let traces = cx.open_window(size(px(1200.), px(700.)), |window, cx| {
        let scene = Arc::new(synthetic::scene());
        let view =
            cx.new(|cx| TraceStack::new(scene, viewport.clone(), cursor.clone(), window, cx));
        stack = Some(view.clone());
        Root::new(view, window, cx)
    });
    let stack = stack.unwrap();
    let traces: AnyWindowHandle = traces.into();
    let draw = |cx: &mut TestAppContext| {
        cx.update_window(traces, |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
        cx.run_until_parked();
    };
    for _ in 0..3 {
        draw(cx);
    }
    let rebuilds = cx.update(|cx| stack.read(cx).static_rebuilds(cx));
    let viewport_before = cx.update(|cx| viewport.read(cx).viewport());

    // Nothing plays: no frame pull is scheduled.
    let panel = cx.update(|cx| mock.test.workspace.read(cx).panels().video.clone());
    mock.frame(cx);
    assert!(
        !cx.update(|cx| panel.read(cx).is_pulling()),
        "a paused video pulls nothing"
    );
    play(&mock, cx);
    assert!(
        cx.update(|cx| panel.read(cx).is_pulling()),
        "playing schedules a pull"
    );

    for fraction in [0.1, 0.25, 0.5, 0.8] {
        mock.set_clock(video_at(&mock, fraction, cx));
        assert!(mock.frame(cx) >= 1, "one pull per display frame");
        let followed = mock.cursor(cx).unwrap();
        assert!(
            (followed - fraction).abs() < 1e-6,
            "{followed} vs {fraction}"
        );
        draw(cx);
    }
    assert_eq!(
        cx.update(|cx| stack.read(cx).static_rebuilds(cx)),
        rebuilds,
        "video sync touched the static trace layer"
    );
    assert_eq!(
        cx.update(|cx| viewport.read(cx).viewport()),
        viewport_before,
        "per-lap playback never moves the viewport"
    );

    // The docked Δ readout needs a comparison (the mock has none) and
    // never sits over a picture.
    cx.update_window(mock.handle, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find("video-hud-card").is_none());
    })
    .unwrap();

    // Pausing stops the pull.
    let video = mock.video();
    cx.update(|cx| video.update(cx, |video, cx| video.toggle_play(cx)));
    cx.run_until_parked();
    mock.frame(cx);
    assert!(!cx.update(|cx| panel.read(cx).is_pulling()));
    let still = mock.cursor(cx);
    mock.set_clock(video_at(&mock, 0.2, cx));
    mock.frame(cx);
    assert_eq!(
        mock.cursor(cx),
        still,
        "a paused clock does not drive the cursor"
    );
}

#[gpui_kit::test]
fn an_explicit_cursor_jump_seeks_the_video(cx: &mut TestAppContext) {
    let mock = mock(cx, 2);
    let cursor = mock.test.app.cursor.clone();
    // Paused: a trace click seeks the video exactly.
    cx.update(|cx| cursor.update(cx, |cursor, cx| cursor.set_fraction(Some(0.5), cx)));
    cx.run_until_parked();
    let target = video_at(&mock, 0.5, cx);
    assert!((mock.clock.estimate(Instant::now()) - target).abs() < 1e-9);

    // Playing: the primary is the clock; a small jump does not tug it.
    play(&mock, cx);
    mock.set_clock(target);
    mock.frame(cx);
    let near = 0.5 + 0.5 * PLAYING_SEEK_ERROR / 20.0;
    cx.update(|cx| cursor.update(cx, |cursor, cx| cursor.set_fraction(Some(near), cx)));
    cx.run_until_parked();
    assert!((mock.clock.estimate(Instant::now()) - target).abs() < 1e-9);
    // A real jump does.
    cx.update(|cx| cursor.update(cx, |cursor, cx| cursor.set_fraction(Some(0.9), cx)));
    cx.run_until_parked();
    let far = video_at(&mock, 0.9, cx);
    assert!((mock.clock.estimate(Instant::now()) - far).abs() < 1e-9);
    // The controller's own writes are not jumps: the next frame keeps it.
    mock.set_clock(far);
    mock.frame(cx);
    assert!((mock.cursor(cx).unwrap() - 0.9).abs() < 1e-6);

    // Left/Right seek 2 s and the cursor follows while paused.
    let video = mock.video();
    cx.update(|cx| video.update(cx, |video, cx| video.toggle_play(cx)));
    cx.update(|cx| video.update(cx, |video, cx| video.seek_by(-2.0, cx)));
    cx.run_until_parked();
    let back = mock.cursor(cx).unwrap();
    assert!((back - (0.9 - 2.0 / 20.0)).abs() < 1e-6, "{back}");
}

#[gpui_kit::test]
fn keys_compose_layouts_and_f_escape_open_and_close_the_stage(cx: &mut TestAppContext) {
    let mock = mock(cx, 2);
    let video = mock.video();
    let workspace = mock.test.workspace.clone();
    cx.update_window(mock.handle, |_, window, cx| {
        let traces = workspace
            .read(cx)
            .panels()
            .focus_handle(PanelKind::Traces, cx);
        window.focus(&traces, cx);
        window.render_frame(cx);
    })
    .unwrap();
    for layout in ComposeLayout::ALL
        .iter()
        .rev()
        .chain(ComposeLayout::ALL.iter())
    {
        cx.update_window(mock.handle, |_, window, cx| window.press(layout.key(), cx))
            .unwrap();
        cx.run_until_parked();
        assert_eq!(cx.update(|cx| video.read(cx).layout()), *layout);
    }
    // One video: every composition shows the primary alone.
    cx.update_window(mock.handle, |_, window, cx| {
        window.render_frame(cx);
        assert_eq!(
            window.find("video-stage").label(),
            Some("Video, Primary only")
        );
    })
    .unwrap();

    // F shows the video-only stage over the whole window; Escape restores
    // the workspace with its dock layout and focus unchanged.
    let dock = |cx: &mut TestAppContext| {
        cx.update(|cx| {
            let area = workspace.read(cx).dock_area().read(cx);
            serde_json::to_value(area.dump(cx)).unwrap()
        })
    };
    select_pair(&mock, cx);
    let layout_before = dock(cx);
    cx.update_window(mock.handle, |_, window, cx| {
        window.render_frame(cx);
        assert!(!window.is_fullscreen());
        assert!(window.try_find("workspace-dock").is_some());
        window.press("f", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(mock.handle, |_, window, cx| {
        window.render_frame(cx);
        let workspace = workspace.read(cx);
        assert!(workspace.is_video_fullscreen());
        assert!(
            !workspace.dock_area().read(cx).is_zoomed(),
            "the stage never zooms the dock"
        );
        // Best effort: the test window accepts it.
        assert!(window.is_fullscreen());
        for chrome in ["workspace-dock", "theme-status"] {
            assert!(window.try_find(chrome).is_none(), "{chrome} is hidden");
        }
        assert!(window.try_find("video-fullscreen").is_some());
        assert!(window.try_find("video-panel").is_none());
        // The role labels of the pair head the stage.
        assert!(window.try_find("video-delta").is_some());
        // The filmstrip rides on the stage above the pictures (as in the
        // dock), the delta lane between them, all above the controls.
        let lane = window.find("video-filmstrip-lane").bounds();
        let controls = window.find("video-stage-controls").bounds();
        assert!(
            lane.bottom() <= controls.top(),
            "{lane:?} above {controls:?}"
        );
        let stage = window.find("video-fullscreen").bounds();
        let pane = window.find("primary-video-pane").bounds();
        let delta = window.find("video-delta").bounds();
        assert!(stage.contains(&pane.origin) && lane.bottom() <= delta.top());
        assert!(
            (delta.bottom() - pane.top()).abs() < px(1.),
            "the delta lane touches the pictures: {delta:?} {pane:?}"
        );
        assert!(pane.bottom() <= controls.top());
        let video = workspace.panels().focus_handle(PanelKind::Video, cx);
        assert!(video.is_focused(window), "the stage holds the focus");
    })
    .unwrap();
    assert_eq!(dock(cx), layout_before);
    cx.update_window(mock.handle, |_, window, cx| window.press("escape", cx))
        .unwrap();
    cx.run_until_parked();
    cx.update_window(mock.handle, |_, window, cx| {
        window.render_frame(cx);
        let workspace = workspace.read(cx);
        assert!(!workspace.is_video_fullscreen());
        assert!(!workspace.dock_area().read(cx).is_zoomed());
        assert!(!window.is_fullscreen());
        assert!(window.try_find("workspace-dock").is_some());
        assert!(window.try_find("video-fullscreen").is_none());
        assert!(window.try_find("filmstrip").is_some());
        let traces = workspace.panels().focus_handle(PanelKind::Traces, cx);
        assert!(traces.is_focused(window), "focus returns to the traces");
    })
    .unwrap();
    assert_eq!(dock(cx), layout_before, "the dock layout is unchanged");
}

/// Give the session a pair from the synthetic library (whose files do
/// not exist: the rows and labels are there, the video stays the mock's).
fn select_pair(mock: &Mock, cx: &mut TestAppContext) {
    let snapshot = common::load_synthetic_library(&mock.test, cx);
    let sessions: Vec<_> = snapshot.sessions().collect();
    let (primary, reference) = (sessions[0].id.clone(), sessions[1].id.clone());
    let session = mock.test.app.session.clone();
    cx.update(|cx| {
        session.update(cx, |session, cx| {
            session.set_primary(primary.into(), 3, cx);
            session.set_reference(reference.into(), 3, cx);
        })
    });
    cx.run_until_parked();
}

/// Enter the fullscreen stage on a mock workspace, as F does.
fn enter_stage(
    mock: &Mock,
    cx: &mut TestAppContext,
) -> Entity<omatrack_app::panels::video::VideoPanel> {
    cx.update_window(mock.handle, |_, window, cx| {
        window.render_frame(cx);
        window.press("f", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(mock.handle, |_, window, cx| window.render_frame(cx))
        .unwrap();
    cx.update(|cx| mock.test.workspace.read(cx).panels().video.clone())
}

#[gpui_kit::test]
fn the_stage_stays_up_through_the_keys_that_follow_it(cx: &mut TestAppContext) {
    let mock = mock(cx, 2);
    select_pair(&mock, cx);
    // Ctrl+3 focuses the video panel, f opens the stage; then the keys a
    // user presses on the stage must act on it, never close it.
    for key in ["ctrl-3", "f"] {
        cx.update_window(mock.handle, |_, window, cx| {
            window.render_frame(cx);
            window.press(key, cx);
        })
        .unwrap();
        cx.run_until_parked();
    }
    let on_stage = |cx: &mut TestAppContext| {
        cx.update(|cx| mock.test.workspace.read(cx).is_video_fullscreen())
    };
    assert!(on_stage(cx), "f opens the stage");
    for key in [
        "2", "right", "left", "space", "space", "m", "m", "1", "s", "s",
    ] {
        cx.update_window(mock.handle, |_, window, cx| {
            window.render_frame(cx);
            window.press(key, cx);
        })
        .unwrap();
        cx.run_until_parked();
        assert!(on_stage(cx), "the stage survives {key}");
        cx.update_window(mock.handle, |_, window, cx| {
            window.render_frame(cx);
            assert!(window.try_find("video-fullscreen").is_some(), "{key}");
        })
        .unwrap();
    }
    assert_eq!(
        cx.update(|cx| mock.video().read(cx).layout()),
        ComposeLayout::Split,
        "layout keys act on the stage"
    );
}

#[gpui_kit::test]
fn the_stage_controls_hide_after_two_idle_seconds_and_come_back_on_input(cx: &mut TestAppContext) {
    use omatrack_app::panels::video::CONTROLS_HIDE_AFTER;
    let mock = mock(cx, 2);
    let panel = enter_stage(&mock, cx);
    let visible = |cx: &mut TestAppContext| cx.update(|cx| panel.read(cx).controls_visible());
    assert!(visible(cx), "the controls show on entering");
    cx.executor()
        .advance_clock(CONTROLS_HIDE_AFTER - Duration::from_millis(200));
    cx.run_until_parked();
    assert!(visible(cx), "not before the idle time");
    cx.executor().advance_clock(Duration::from_millis(400));
    cx.run_until_parked();
    assert!(!visible(cx), "hidden after two idle seconds");
    cx.update_window(mock.handle, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find("video-stage-controls").is_none());
    })
    .unwrap();

    // Pointer motion reveals them and restarts the timer.
    pointer(
        cx,
        mock.handle,
        MouseMoveEvent {
            position: point(px(400.), px(300.)),
            pressed_button: None,
            modifiers: Modifiers::default(),
        }
        .to_platform_input(),
    );
    assert!(visible(cx), "pointer motion reveals the controls");
    cx.executor()
        .advance_clock(CONTROLS_HIDE_AFTER + Duration::from_millis(100));
    cx.run_until_parked();
    assert!(!visible(cx));

    // So does a key (here 2: a layout change, which the stage also applies).
    cx.update_window(mock.handle, |_, window, cx| window.press("m", cx))
        .unwrap();
    cx.run_until_parked();
    assert!(visible(cx), "a keypress reveals the controls");
    cx.update_window(mock.handle, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find("video-stage-controls").is_some());
    })
    .unwrap();
}

#[gpui_kit::test]
fn the_telemetry_band_shows_only_on_the_stage_and_its_drag_end_persists(cx: &mut TestAppContext) {
    let mock = mock(cx, 2);
    // Docked: never the band.
    cx.update_window(mock.handle, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find("video-telemetry-hud").is_none());
    })
    .unwrap();
    let panel = enter_stage(&mock, cx);
    cx.update_window(mock.handle, |_, window, _| {
        assert!(window.try_find("video-hud-card").is_none());
        let band = window.find("video-telemetry-hud");
        assert_eq!(band.label(), Some("Telemetry overlay"));
        // The band keeps the Qt 1000:210 proportion.
        let bounds = band.bounds();
        let ratio = bounds.size.height / bounds.size.width;
        assert!((ratio - 0.21).abs() < 0.01, "{ratio}");
        // One video without GPS: no gap bar.
        assert!(window.try_find("video-telemetry-gap").is_none());
        // Without a session pair there is nothing to label or compare.
        assert!(window.try_find("video-delta-bar").is_none());
    })
    .unwrap();

    // The HUD toggle hides and restores the band.
    cx.update_window(mock.handle, |_, window, cx| {
        window.click("video-stage-hud", cx)
    })
    .unwrap();
    cx.run_until_parked();
    assert!(!cx.update(|cx| panel.read(cx).is_hud_shown()));
    cx.update_window(mock.handle, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find("video-telemetry-hud").is_none());
        window.click("video-stage-hud", cx);
    })
    .unwrap();
    cx.run_until_parked();

    // Dragging the band writes `video.hud_position` once, at drag end.
    let (band, stage) = cx
        .update_window(mock.handle, |_, window, cx| {
            window.render_frame(cx);
            (
                window.find("video-telemetry-hud").bounds(),
                window.find("video-fullscreen").bounds(),
            )
        })
        .unwrap();
    assert_eq!(cx.update(|cx| mock.video().read(cx).hud_position(cx)), None);
    let grab = band.center();
    let target = point(stage.center().x, stage.top() + band.size.height);
    pointer(
        cx,
        mock.handle,
        MouseDownEvent {
            button: MouseButton::Left,
            position: grab,
            modifiers: Modifiers::default(),
            click_count: 1,
            first_mouse: false,
        }
        .to_platform_input(),
    );
    for at in [grab + point(px(0.), px(-20.)), target] {
        pointer(
            cx,
            mock.handle,
            MouseMoveEvent {
                position: at,
                pressed_button: Some(MouseButton::Left),
                modifiers: Modifiers::default(),
            }
            .to_platform_input(),
        );
    }
    assert_eq!(
        cx.update(|cx| mock.video().read(cx).hud_position(cx)),
        None,
        "nothing is written while dragging"
    );
    let moved = cx
        .update_window(mock.handle, |_, window, _| {
            window.find("video-telemetry-hud").bounds()
        })
        .unwrap();
    assert!(moved.top() < band.top(), "the band follows the pointer");
    pointer(
        cx,
        mock.handle,
        MouseUpEvent {
            button: MouseButton::Left,
            position: target,
            modifiers: Modifiers::default(),
            click_count: 1,
        }
        .to_platform_input(),
    );
    let (x, y) = cx
        .update(|cx| mock.video().read(cx).hud_position(cx))
        .expect("the drag end is persisted");
    assert!((x - 0.5).abs() < 0.05, "{x}");
    assert!(y < 0.3, "moved up: {y}");
}

#[gpui_kit::test]
fn the_transport_bar_sets_rate_and_playback_mode_and_shows_the_composition(
    cx: &mut TestAppContext,
) {
    let mock = mock(cx, 2);
    let video = mock.video();
    let slow = |cx: &mut TestAppContext| cx.update(|cx| video.read(cx).is_slow_motion());
    let continuous = |cx: &mut TestAppContext| cx.update(|cx| video.read(cx).is_continuous(cx));
    let click = |id: &'static str, cx: &mut TestAppContext| {
        cx.update_window(mock.handle, |_, window, cx| {
            window.render_frame(cx);
            window.click(id, cx);
        })
        .unwrap();
        cx.run_until_parked();
    };
    assert!(!slow(cx));
    // Docked, 0.25x is one toggle (the stage keeps `1x | 0.25x`).
    click("video-slow-motion", cx);
    assert!(slow(cx), "0.25x selects slow motion");
    click("video-slow-motion", cx);
    assert!(!slow(cx), "0.25x again leaves slow motion");

    let before = continuous(cx);
    click(
        if before {
            "video-per-lap"
        } else {
            "video-continuous"
        },
        cx,
    );
    assert_eq!(continuous(cx), !before);
    click(
        if before {
            "video-continuous"
        } else {
            "video-per-lap"
        },
        cx,
    );
    assert_eq!(continuous(cx), before);

    // One video: the primary alone, with the HUD on its picture.
    cx.update_window(mock.handle, |_, window, cx| {
        window.render_frame(cx);
        assert_eq!(
            window.find("video-stage").label(),
            Some("Video, Primary only")
        );
        assert!(window.try_find("video-hud-card").is_none());
    })
    .unwrap();
}

#[gpui_kit::test]
fn per_lap_playback_counts_down_into_the_next_lap(cx: &mut TestAppContext) {
    let mock = mock(cx, 2);
    let video = mock.video();
    play(&mock, cx);
    mock.set_clock(video_at(&mock, 0.9, cx));
    mock.frame(cx);
    // Past the lap end: pause and count 3-2-1.
    mock.set_clock(video_at(&mock, 1.0, cx) + 0.2);
    mock.frame(cx);
    assert_eq!(cx.update(|cx| video.read(cx).countdown()), Some(3));
    assert!(!cx.update(|cx| video.read(cx).is_playing()), "paused");
    cx.update_window(mock.handle, |_, window, cx| {
        window.render_frame(cx);
        assert_eq!(window.find("video-countdown").label(), Some("L3 in 3"));
    })
    .unwrap();
    for expected in [Some(2), Some(1), None] {
        cx.executor().advance_clock(Duration::from_millis(500));
        cx.run_until_parked();
        assert_eq!(cx.update(|cx| video.read(cx).countdown()), expected);
    }
    // After 1: the next lap is requested, starting at its beginning.
    assert_eq!(cx.update(|cx| video.read(cx).advancing_to()), Some(3));
    assert_eq!(mock.cursor(cx), Some(0.0));
    // When that lap is bound (the session loaded it), playback resumes at
    // its start.
    let next = loaded(&mock.recording, 3);
    cx.update(|cx| video.update(cx, |video, cx| video.bind(Some(&next), cx)));
    cx.run_until_parked();
    assert!(cx.update(|cx| video.read(cx).is_playing()), "resumed");
    assert_eq!(cx.update(|cx| video.read(cx).advancing_to()), None);
    // Resumed at the lap start (the mock clock now runs at 1x).
    let start = video_at(&mock, 0.0, cx);
    assert!((mock.clock.estimate(Instant::now()) - start).abs() < 0.05);
    assert_eq!(
        cx.update(|cx| video.read(cx).timeline(Role::Primary).unwrap().lap_id()),
        3
    );

    // Space during a countdown cancels it.
    mock.set_clock(video_at(&mock, 0.95, cx));
    mock.frame(cx);
    mock.set_clock(video_at(&mock, 1.0, cx) + 0.1);
    mock.frame(cx);
    assert_eq!(cx.update(|cx| video.read(cx).countdown()), Some(3));
    cx.update(|cx| video.update(cx, |video, cx| video.toggle_play(cx)));
    assert_eq!(cx.update(|cx| video.read(cx).countdown()), None);
}

#[gpui_kit::test]
fn continuous_playback_adopts_the_next_lap_without_seeking(cx: &mut TestAppContext) {
    let mock = mock(cx, 1);
    let video = mock.video();
    let viewport = mock.test.app.viewport.clone();
    cx.update(|cx| video.update(cx, |video, cx| video.toggle_continuous(cx)));
    cx.update(|cx| {
        viewport.update(cx, |viewport, cx| {
            viewport.set_viewport(Viewport::new(0.4, 0.6), cx)
        })
    });
    play(&mock, cx);
    // Past 70%: the next lap is prefetched; the playhead holds at 33%.
    mock.set_clock(video_at(&mock, 0.8, cx));
    mock.frame(cx);
    cx.run_until_parked();
    let followed = cx.update(|cx| viewport.read(cx).viewport());
    let share = (0.8 - followed.start) / followed.span();
    assert!((share - 0.33).abs() < 1e-6, "{followed:?}");

    // Across the lap end: lap 2 is adopted at the running position.
    let past = video_at(&mock, 1.0, cx) + 2.0;
    mock.set_clock(past);
    mock.frame(cx);
    assert!(cx.update(|cx| video.read(cx).is_playing()), "no pause");
    assert_eq!(
        cx.update(|cx| video.read(cx).countdown()),
        None,
        "no countdown"
    );
    assert_eq!(
        cx.update(|cx| video.read(cx).timeline(Role::Primary).unwrap().lap_id()),
        2
    );
    let adopted = mock.cursor(cx).unwrap();
    assert!((adopted - 2.0 / 20.0).abs() < 1e-3, "{adopted}");
    assert!(
        (mock.clock.estimate(Instant::now()) - past).abs() < 1e-9,
        "the playing video was not sought"
    );
    // The session's load of lap 2 lands: still no seek.
    let lap2 = loaded(&mock.recording, 2);
    cx.update(|cx| video.update(cx, |video, cx| video.bind(Some(&lap2), cx)));
    cx.run_until_parked();
    assert!((mock.clock.estimate(Instant::now()) - past).abs() < 1e-9);
    mock.frame(cx);
    let followed = cx.update(|cx| viewport.read(cx).viewport());
    let share = (adopted - followed.start) / followed.span();
    assert!((share - 0.33).abs() < 1e-3, "{followed:?}");
}

#[gpui_kit::test]
fn a_video_without_a_verified_identity_never_drives_the_cursor(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let handle: AnyWindowHandle = test.window.into();
    // A companion video the telemetry has no identity for.
    let onboard = sandbox.dir.path().join("onboard.mp4");
    std::fs::write(&onboard, b"not the recorded video").unwrap();
    let recording = recording();
    assert!(matches!(
        omatrack_app::sync::identity::check(&recording, &onboard),
        IdentityStatus::Untrusted { .. }
    ));
    let lap = load_lap(
        recording.clone(),
        2,
        &LoadOptions::default().with_video_path(Some(onboard.clone())),
        &AtomicBool::new(false),
    )
    .unwrap();
    assert!(lap.video().is_some(), "the file is bound");
    let video = test.app.video.clone();
    cx.update(|cx| video.update(cx, |video, cx| video.bind(Some(&lap), cx)));
    cx.run_until_parked();
    let warning = cx.update(|cx| {
        let video = video.read(cx);
        assert!(!video.is_synced(), "sync is off for an unverified video");
        assert!(!video.identity(Role::Primary).is_trusted());
        video
            .identity_warning()
            .cloned()
            .expect("a warning says why")
    });
    // A cursor jump does nothing to it, and the badge says why.
    let cursor = test.app.cursor.clone();
    cx.update(|cx| cursor.update(cx, |cursor, cx| cursor.set_fraction(Some(0.5), cx)));
    cx.run_until_parked();
    assert_eq!(cx.update(|cx| cursor.read(cx).fraction()), Some(0.5));
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        let badge = window.find("video-identity-warning");
        assert!(badge.visible());
        assert_eq!(badge.label(), Some(warning.as_ref()));
    })
    .unwrap();
}

fn pointer(cx: &mut TestAppContext, window: AnyWindowHandle, event: PlatformInput) {
    cx.update_window(window, |_, window, cx| {
        window.dispatch_event(event, cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(window, |_, window, cx| window.render_frame(cx))
        .unwrap();
}

// ── real recordings ──────────────────────────────────────────────────

fn fixtures() -> String {
    std::env::var("OMATRACK_FIXTURES")
        .expect("set OMATRACK_FIXTURES to the 26T07_PLM folder of AiM MP4 recordings")
}

/// Run `cx` (and the players' own threads) until `done`, or panic.
fn settle(
    cx: &mut TestAppContext,
    what: &str,
    timeout: Duration,
    mut done: impl FnMut(&mut TestAppContext) -> bool,
) {
    let deadline = Instant::now() + timeout;
    loop {
        cx.run_until_parked();
        if done(cx) {
            return;
        }
        assert!(Instant::now() < deadline, "timed out waiting for {what}");
        std::thread::sleep(Duration::from_millis(20));
    }
}

#[gpui_kit::test]
#[ignore]
fn real_run4_video_drives_the_cursor_and_run1_follows_the_map(cx: &mut TestAppContext) {
    // libmpv runs its own threads; the test waits on them.
    cx.executor().allow_parking();
    let sandbox = common::Sandbox::new();
    sandbox.write_config(&format!(
        "locations:\n  - type: folder\n    target: {}\nvideo:\n  muted: true\n",
        fixtures()
    ));
    let options = sandbox
        .options()
        .video(true)
        .audio_output(Some("null".to_string()));
    let test = common::start(cx, options);
    let handle: AnyWindowHandle = test.window.into();
    let library = test.app.library.clone();
    cx.update(|cx| library.update(cx, |library, cx| library.rescan(cx)));
    settle(cx, "the library scan", Duration::from_secs(600), |cx| {
        cx.update(|cx| {
            let library = library.read(cx);
            library.has_scanned() && !library.is_scanning()
        })
    });
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
    let run4_best = run4.best_lap_id.expect("Run4 has a best lap");
    let run1_best = run1.best_lap_id.expect("Run1 has a best lap");
    let events = std::rc::Rc::new(std::cell::RefCell::new(Vec::<VideoEvent>::new()));
    let sink = events.clone();
    let video = test.app.video.clone();
    cx.update(|cx| {
        cx.subscribe(&video, move |_, event: &VideoEvent, _| {
            sink.borrow_mut().push(event.clone())
        })
        .detach();
    });
    let session = test.app.session.clone();
    cx.update(|cx| {
        session.update(cx, |session, cx| {
            session.set_primary(run4.id.clone().into(), run4_best, cx);
            session.set_reference(run1.id.clone().into(), run1_best, cx);
        })
    });
    settle(
        cx,
        "both videos and the pair map",
        Duration::from_secs(600),
        |cx| {
            cx.update(|cx| {
                let video = video.read(cx);
                video.is_dual()
                    && video.pair().is_some()
                    && video.is_synced()
                    && video.is_reference_synced()
                    && [Role::Primary, Role::Reference].into_iter().all(|role| {
                        video.clock(role).is_some_and(|clock| {
                            let sample = clock.sample();
                            !sample.seeking && sample.duration > 0.0
                        })
                    })
            })
        },
    );
    let identities = cx.update(|cx| {
        let video = video.read(cx);
        (
            video.identity(Role::Primary).clone(),
            video.identity(Role::Reference).clone(),
        )
    });
    println!(
        "identity at bind: primary {:?}, reference {:?}",
        identities.0, identities.1
    );

    // Seek the primary to the lap midpoint (an explicit cursor jump).
    let cursor = test.app.cursor.clone();
    cx.update(|cx| cursor.update(cx, |cursor, cx| cursor.set_fraction(Some(0.5), cx)));
    let frame = 1.0 / 60.0;
    settle(cx, "both seeks to land", Duration::from_secs(60), |cx| {
        cx.update(|cx| {
            let video = video.read(cx);
            let (primary, reference) = (
                video.clock(Role::Primary).unwrap().sample(),
                video.clock(Role::Reference).unwrap().sample(),
            );
            let target = video.reference_target(0.5).unwrap();
            !primary.seeking && !reference.seeking && (reference.position - target).abs() < frame
        })
    });
    // Let the paused alignment verification finish.
    std::thread::sleep(Duration::from_millis(400));
    cx.run_until_parked();
    cx.update(|cx| {
        let video = video.read(cx);
        let pair = video.pair().unwrap();
        let mapped = pair
            .reference()
            .video_at_lap_time(pair.comparison().compare_time_for_primary_fraction(0.5))
            .unwrap();
        let target = video.reference_target(0.5).unwrap();
        assert!((target - mapped).abs() < 1e-9, "{target} vs {mapped}");
        let primary = video.clock(Role::Primary).unwrap().sample().position;
        let expected_primary = video
            .timeline(Role::Primary)
            .unwrap()
            .video_at_fraction(0.5)
            .unwrap();
        let reference = video.clock(Role::Reference).unwrap().sample().position;
        println!(
            "midpoint: primary {primary:.4} s (mapped {expected_primary:.4}), reference {reference:.4} s (target {target:.4}), error {:.4} s, sync {:?}",
            reference - target,
            video.sync_state()
        );
        assert!((primary - expected_primary).abs() < frame, "{primary} vs {expected_primary}");
        assert!((reference - target).abs() < frame, "{reference} vs {target}");
    });

    // Play: each display frame writes the cursor at the primary video's
    // time. The pull happens between two clock readings, so the cursor must
    // lie between the fractions at those readings (one video frame of slack
    // for mpv's own time-pos corrections).
    cx.update(|cx| video.update(cx, |video, cx| video.toggle_play(cx)));
    let expected = |cx: &mut TestAppContext| {
        cx.update(|cx| {
            let video = video.read(cx);
            let estimate = video.clock(Role::Primary).unwrap().estimate(Instant::now());
            video
                .timeline(Role::Primary)
                .unwrap()
                .fraction_at_video(estimate)
                .unwrap()
        })
    };
    let duration = cx.update(|cx| video.read(cx).timeline(Role::Primary).unwrap().duration());
    let slack = frame / duration;
    let started = Instant::now();
    let mut worst = 0.0f64;
    let mut frames = 0;
    while started.elapsed() < Duration::from_millis(1500) {
        std::thread::sleep(Duration::from_millis(16));
        let before = expected(cx);
        cx.update_window(handle, |_, window, cx| window.simulate_next_frame(cx))
            .unwrap();
        let after = expected(cx);
        cx.run_until_parked();
        frames += 1;
        let followed = cx.update(|cx| cursor.read(cx).fraction().unwrap());
        let outside = (before - followed).max(followed - after).max(0.0);
        worst = worst.max(outside);
    }
    let after = cx.update(|cx| cursor.read(cx).fraction().unwrap());
    println!(
        "played {frames} frames: cursor {after:.5}, worst deviation outside the pull window {:.4} s, sync {:?}",
        worst * duration,
        cx.update(|cx| video.read(cx).sync_state())
    );
    assert!(after > 0.5, "the cursor advanced with the video: {after}");
    assert!(
        worst <= slack,
        "the cursor left the video's time by {:.4} s",
        worst * duration
    );
    cx.update(|cx| video.update(cx, |video, cx| video.toggle_play(cx)));
    cx.run_until_parked();

    // X swaps the roles: the players are exchanged (no file reopens), the
    // cursor stays, and both videos land on the swapped pair's stations.
    let paths = cx.update(|cx| {
        let video = video.read(cx);
        (
            video.bound_path().cloned().unwrap(),
            video.reference_bound_path().cloned().unwrap(),
        )
    });
    cx.update(|cx| cursor.update(cx, |cursor, cx| cursor.set_fraction(Some(0.3), cx)));
    cx.run_until_parked();
    events.borrow_mut().clear();
    cx.update(|cx| session.update(cx, |session, cx| session.swap(cx)));
    settle(cx, "the swapped pair", Duration::from_secs(600), |cx| {
        cx.update(|cx| {
            let video = video.read(cx);
            video
                .pair()
                .is_some_and(|pair| pair.primary().lap_id() == run1_best)
                && [Role::Primary, Role::Reference]
                    .into_iter()
                    .all(|role| !video.clock(role).unwrap().sample().seeking)
                && video.reference_target(0.3).is_some_and(|target| {
                    let position = video.clock(Role::Reference).unwrap().sample().position;
                    (position - target).abs() < frame
                })
        })
    });
    cx.update(|cx| {
        let video = video.read(cx);
        assert_eq!(
            video.bound_path(),
            Some(&paths.1),
            "Run1 is now the primary"
        );
        assert_eq!(video.reference_bound_path(), Some(&paths.0));
        assert_eq!(cursor.read(cx).fraction(), Some(0.3), "the cursor stays");
        let primary = video.clock(Role::Primary).unwrap().sample().position;
        let station = video
            .timeline(Role::Primary)
            .unwrap()
            .video_at_fraction(0.3)
            .unwrap();
        println!("after swap: primary {primary:.4} s (station {station:.4})");
        assert!((primary - station).abs() < frame, "{primary} vs {station}");
    });
    let reopened = events
        .borrow()
        .iter()
        .filter(|event| matches!(event, VideoEvent::Failed(_)))
        .count();
    assert_eq!(reopened, 0);

    // The identity check result is reported.
    events.borrow_mut().clear();
    cx.update(|cx| video.update(cx, |video, cx| video.verify_identity(cx)));
    settle(cx, "the identity checks", Duration::from_secs(600), |_| {
        events
            .borrow()
            .iter()
            .filter(|event| matches!(event, VideoEvent::IdentityChecked { .. }))
            .count()
            == 2
    });
    for event in events.borrow().iter() {
        if let VideoEvent::IdentityChecked {
            role,
            summary,
            trusted,
        } = event
        {
            println!("identity {role:?}: {summary} (trusted: {trusted})");
            assert!(trusted, "AiM MP4s are their own video: {summary}");
        }
    }
    drop(sandbox);
}

/// The controller follows the shared cursor from the moment the state is
/// installed, with no deferred step: in the running application `install`
/// runs outside an update, where a deferred callback fires before the
/// global exists and the video would never drive the cursor.
#[gpui_kit::test]
fn install_connects_the_video_to_the_shared_cursor(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    cx.update(|cx| {
        let state = omatrack_app::AppState::install(sandbox.options(), cx);
        assert!(state.video.read(cx).is_connected());
    });
}
