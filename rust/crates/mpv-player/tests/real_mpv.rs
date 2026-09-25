//! Real-file checks against the AiM onboard recordings. Ignored by default;
//! run with
//!
//! ```sh
//! OMATRACK_FIXTURES=$HOME/Documents/Telemetry/26T07_PLM \
//!   cargo test -p mpv-player -- --include-ignored real_ --nocapture
//! ```
//!
//! The recordings are opened read-only by mpv, with a null audio output and
//! muted. Every wait is bounded, and the tests run one at a time so the
//! printed software-render timings are not skewed by each other.

use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use gpui_kit::{DevicePixels, Size};
use mpv_player::{FrameSource, MediaStatus, Player, PlayerOptions, VideoFrame};

static SERIAL: Mutex<()> = Mutex::new(());

fn run1() -> Option<PathBuf> {
    recording("Run1")
}

fn recording(run: &str) -> Option<PathBuf> {
    let Some(root) = std::env::var_os("OMATRACK_FIXTURES") else {
        eprintln!("OMATRACK_FIXTURES not set; skipping");
        return None;
    };
    let dir = PathBuf::from(root).join("CT1");
    let found = std::fs::read_dir(&dir)
        .unwrap_or_else(|error| panic!("cannot read {}: {error}", dir.display()))
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .find(|path| {
            let name = path
                .file_name()
                .and_then(|name| name.to_str())
                .unwrap_or_default();
            name.contains(run) && name.to_ascii_uppercase().ends_with(".MP4")
        });
    Some(found.unwrap_or_else(|| panic!("no {run} MP4 in {}", dir.display())))
}

fn wait_until(timeout: Duration, mut condition: impl FnMut() -> bool) -> bool {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if condition() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    condition()
}

fn device(width: i32, height: i32) -> Option<Size<DevicePixels>> {
    Some(Size::new(DevicePixels(width), DevicePixels(height)))
}

fn open(path: &Path) -> (Player, std::sync::Arc<dyn FrameSource>) {
    let player = Player::new(PlayerOptions::default().muted(true).audio_output("null"))
        .expect("create player");
    player.load(path).expect("load");
    assert!(
        wait_until(Duration::from_secs(20), || player.state().loaded),
        "file did not load: {:?}",
        player.state()
    );
    let source = player.frame_source();
    (player, source)
}

/// Waits until the player is settled (not seeking) and a frame newer than
/// `after_generation` has been published.
fn wait_for_frame(player: &Player, source: &dyn FrameSource, after_generation: u64) -> VideoFrame {
    assert!(
        wait_until(Duration::from_secs(20), || {
            !player.state().seeking
                && source.frame_generation() > after_generation
                && source.latest_frame().is_some()
        }),
        "no frame rendered: {:?}",
        player.state()
    );
    source.latest_frame().unwrap()
}

fn drop_within(player: Player, timeout: Duration) {
    let (done_tx, done_rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        drop(player);
        let _ = done_tx.send(());
    });
    done_rx
        .recv_timeout(timeout)
        .expect("dropping the player hung (threads or render context not released)");
}

/// Average SW render cost while playing for `duration` at `size`.
fn measure(
    player: &Player,
    source: &dyn FrameSource,
    size: (i32, i32),
    duration: Duration,
) -> (f64, f64, f64, u64, (u32, u32)) {
    source.set_target_size(device(size.0, size.1));
    // Let the size change land, then measure steady-state playback.
    std::thread::sleep(Duration::from_millis(300));
    player.reset_render_stats();
    std::thread::sleep(duration);
    let stats = player.render_stats();
    (
        stats.average_render_ms(),
        stats.average_convert_ms(),
        stats.max_render_time.as_secs_f64() * 1000.0,
        stats.frames,
        stats.last_size,
    )
}

#[test]
#[ignore = "needs OMATRACK_FIXTURES"]
fn real_frame_is_not_black_and_sw_render_timings() {
    let _serial = SERIAL.lock().unwrap_or_else(|poison| poison.into_inner());
    let Some(path) = run1() else { return };
    let (player, source) = open(&path);
    assert_eq!(player.state().status, MediaStatus::Ready);
    assert!(
        wait_until(Duration::from_secs(10), || player.state().dwidth > 0),
        "video size never reported"
    );
    let state = player.state();
    assert_eq!((state.dwidth, state.dheight), (1920, 1080), "{state:?}");

    source.set_target_size(device(960, 540));
    let generation = source.frame_generation();
    player.seek_exact(30.0);
    let frame = wait_for_frame(&player, source.as_ref(), generation);
    assert_eq!(
        frame.size(),
        Size::new(DevicePixels(960), DevicePixels(540))
    );

    let bytes = frame.image().as_bytes(0).expect("frame bytes");
    assert_eq!(bytes.len(), 960 * 540 * 4);
    assert!(
        bytes.chunks_exact(4).all(|pixel| pixel[3] == 0xFF),
        "alpha forced opaque"
    );
    let pixels = (bytes.len() / 4) as f64;
    let mean = bytes
        .chunks_exact(4)
        .map(|pixel| (u32::from(pixel[0]) + u32::from(pixel[1]) + u32::from(pixel[2])) as f64 / 3.0)
        .sum::<f64>()
        / pixels;
    let lit = bytes
        .chunks_exact(4)
        .filter(|pixel| pixel[0].max(pixel[1]).max(pixel[2]) > 24)
        .count() as f64
        / pixels;
    eprintln!("frame at 30 s: mean level {mean:.1}, lit fraction {lit:.3}");
    assert!(
        mean > 10.0 && lit > 0.10,
        "frame is black: mean {mean:.1}, lit {lit:.3}"
    );

    player.play();
    let (render_540, convert_540, max_540, frames_540, size_540) =
        measure(&player, source.as_ref(), (960, 540), Duration::from_secs(3));
    let (render_1080, convert_1080, max_1080, frames_1080, size_1080) = measure(
        &player,
        source.as_ref(),
        (1920, 1080),
        Duration::from_secs(3),
    );
    player.pause();
    eprintln!(
        "SW render {}x{}: {render_540:.2} ms/frame avg (max {max_540:.2}), convert {convert_540:.2} ms, {frames_540} frames in 3 s",
        size_540.0, size_540.1
    );
    eprintln!(
        "SW render {}x{}: {render_1080:.2} ms/frame avg (max {max_1080:.2}), convert {convert_1080:.2} ms, {frames_1080} frames in 3 s",
        size_1080.0, size_1080.1
    );
    assert_eq!(size_540, (960, 540));
    assert_eq!(size_1080, (1920, 1080));
    assert!(
        frames_540 > 30 && frames_1080 > 30,
        "playback rendered too few frames"
    );
    drop_within(player, Duration::from_secs(10));
}

#[test]
#[ignore = "needs OMATRACK_FIXTURES"]
fn real_exact_seek_lands_within_one_frame() {
    let _serial = SERIAL.lock().unwrap_or_else(|poison| poison.into_inner());
    let Some(path) = run1() else { return };
    let (player, source) = open(&path);
    source.set_target_size(device(640, 360));

    let generation = source.frame_generation();
    player.seek_exact(12.345);
    assert_eq!(player.target_position(), 12.345);
    let frame = wait_for_frame(&player, source.as_ref(), generation);
    let reported = player.query_time_pos().expect("time-pos");
    eprintln!(
        "seek 12.345 -> time-pos {reported:.6}, frame media time {:.6}",
        frame.media_time()
    );
    assert!(
        (reported - 12.345).abs() <= 1.0 / 60.0,
        "time-pos {reported}"
    );
    assert!((player.state().time_pos - 12.345).abs() <= 1.0 / 60.0);
    assert!((player.clock().estimate(Instant::now()) - 12.345).abs() <= 1.0 / 60.0);

    // Relative seeks start from the pending target, not a stale position.
    player.seek_relative(2.0);
    player.seek_relative(2.0);
    assert!((player.target_position() - 16.345).abs() < 1e-9);
    assert!(wait_until(Duration::from_secs(20), || {
        let state = player.state();
        !state.seeking && (state.time_pos - 16.345).abs() <= 1.0 / 60.0
    }));
    drop_within(player, Duration::from_secs(10));
}

#[test]
#[ignore = "needs OMATRACK_FIXTURES"]
fn real_clock_advances_when_playing_and_pause_holds() {
    let _serial = SERIAL.lock().unwrap_or_else(|poison| poison.into_inner());
    let Some(path) = run1() else { return };
    let (player, source) = open(&path);
    source.set_target_size(device(640, 360));
    let generation = source.frame_generation();
    player.seek_exact(60.0);
    wait_for_frame(&player, source.as_ref(), generation);

    let clock = player.clock();
    let start = player.state().time_pos;
    player.play();
    assert!(source.is_playing());
    std::thread::sleep(Duration::from_millis(1500));
    let state = player.state();
    let estimate = clock.estimate(Instant::now());
    eprintln!(
        "after 1.5 s of play: time-pos {:.3} (from {start:.3}), clock {estimate:.3}",
        state.time_pos
    );
    assert!(!state.paused);
    assert!(
        state.time_pos - start > 1.0,
        "time did not advance: {start} -> {}",
        state.time_pos
    );
    assert!(
        (estimate - state.time_pos).abs() < 0.1,
        "clock {estimate} vs time-pos {}",
        state.time_pos
    );

    player.pause();
    assert!(!source.is_playing());
    assert!(wait_until(Duration::from_secs(5), || player
        .query_time_pos()
        .is_ok()));
    std::thread::sleep(Duration::from_millis(200));
    let held = player.query_time_pos().unwrap();
    let held_clock = clock.estimate(Instant::now());
    std::thread::sleep(Duration::from_millis(700));
    let later = player.query_time_pos().unwrap();
    eprintln!("paused at {held:.6}, 0.7 s later {later:.6}");
    assert!(
        (later - held).abs() < 1e-6,
        "pause did not hold: {held} -> {later}"
    );
    assert!((clock.estimate(Instant::now()) - held_clock).abs() < 1e-9);
    drop_within(player, Duration::from_secs(10));
}

#[test]
#[ignore = "needs OMATRACK_FIXTURES"]
fn real_drop_while_playing_is_clean() {
    let _serial = SERIAL.lock().unwrap_or_else(|poison| poison.into_inner());
    let Some(path) = run1() else { return };
    let (player, source) = open(&path);
    source.set_target_size(device(1920, 1080));
    player.play();
    assert!(wait_until(Duration::from_secs(10), || player
        .render_stats()
        .frames
        > 5));
    drop_within(player, Duration::from_secs(10));
    // A view may hold the source a little longer than the player; reading
    // it after the player is gone must stay safe.
    let _ = source.latest_frame();
    let _ = source.status();
}

#[test]
#[ignore = "needs OMATRACK_FIXTURES"]
fn real_reload_of_a_same_size_file_keeps_rendering() {
    let _serial = SERIAL.lock().unwrap_or_else(|poison| poison.into_inner());
    let Some(first) = run1() else { return };
    let second = recording("Run5").expect("fixtures");
    let (player, source) = open(&first);
    source.set_target_size(device(640, 360));
    let generation = source.frame_generation();
    player.seek_exact(20.0);
    wait_for_frame(&player, source.as_ref(), generation);

    // mpv sends no size change between two 1920x1080 files; rendering must
    // resume anyway, and nothing from the first file may linger.
    player.load(&second).expect("load second");
    assert!(
        source.latest_frame().is_none(),
        "load clears the previous frame"
    );
    assert_eq!(source.status(), MediaStatus::Loading);
    player.seek_exact(20.0);
    assert!(wait_until(Duration::from_secs(20), || player
        .state()
        .loaded));
    let generation = source.frame_generation();
    let frame = wait_for_frame(&player, source.as_ref(), generation.saturating_sub(1));
    assert_eq!(
        frame.size(),
        Size::new(DevicePixels(640), DevicePixels(360))
    );
    assert_eq!(
        (player.state().dwidth, player.state().dheight),
        (1920, 1080)
    );
    assert!((player.query_time_pos().unwrap() - 20.0).abs() <= 1.0 / 60.0);
    drop_within(player, Duration::from_secs(10));
}

#[test]
#[ignore = "needs OMATRACK_FIXTURES"]
fn real_frame_rate_cap_skips_frames() {
    let _serial = SERIAL.lock().unwrap_or_else(|poison| poison.into_inner());
    let Some(path) = run1() else { return };
    let (player, source) = open(&path);
    player.set_max_fps(Some(30.0));
    let generation = source.frame_generation();
    source.set_target_size(device(640, 360));
    player.seek_exact(40.0);
    wait_for_frame(&player, source.as_ref(), generation);
    player.play();
    std::thread::sleep(Duration::from_millis(300));
    player.reset_render_stats();
    std::thread::sleep(Duration::from_secs(2));
    let stats = player.render_stats();
    eprintln!(
        "30 fps cap on 60 fps video: {} rendered, {} skipped in 2 s",
        stats.frames, stats.skipped
    );
    assert!(
        stats.frames <= 70,
        "cap not applied: {} frames",
        stats.frames
    );
    assert!(stats.frames >= 40, "too few frames: {}", stats.frames);
    assert!(
        stats.skipped >= 40,
        "decoded frames were not consumed: {}",
        stats.skipped
    );
    drop_within(player, Duration::from_secs(10));
}

#[test]
#[ignore = "needs OMATRACK_FIXTURES"]
fn real_withdrawn_target_size_stops_drawing_while_playing() {
    let _serial = SERIAL.lock().unwrap_or_else(|poison| poison.into_inner());
    let Some(path) = run1() else { return };
    let (player, source) = open(&path);
    let generation = source.frame_generation();
    source.set_target_size(device(640, 360));
    player.seek_exact(50.0);
    wait_for_frame(&player, source.as_ref(), generation);
    player.play();

    // What a hidden or released VideoView reports.
    source.set_target_size(None);
    std::thread::sleep(Duration::from_millis(300));
    player.reset_render_stats();
    std::thread::sleep(Duration::from_secs(1));
    let hidden = player.render_stats();
    eprintln!(
        "no target while playing: {} rendered, {} skipped in 1 s",
        hidden.frames, hidden.skipped
    );
    assert_eq!(hidden.frames, 0, "nothing is drawn without a target");
    assert!(
        hidden.skipped >= 20,
        "decoded frames were not consumed: {}",
        hidden.skipped
    );

    // Shown again: the current frame is redrawn and playback renders.
    let generation = source.frame_generation();
    source.set_target_size(device(640, 360));
    wait_for_frame(&player, source.as_ref(), generation);
    assert!(player.render_stats().frames >= 1);
    drop_within(player, Duration::from_secs(10));
}

#[test]
#[ignore = "needs OMATRACK_FIXTURES"]
fn real_seek_requested_while_loading_is_applied_once() {
    let _serial = SERIAL.lock().unwrap_or_else(|poison| poison.into_inner());
    let Some(path) = run1() else { return };
    let player = Player::new(PlayerOptions::default().muted(true).audio_output("null"))
        .expect("create player");
    let source = player.frame_source();
    source.set_target_size(device(640, 360));
    player.load(&path).expect("load");
    // Before FILE_LOADED: remembered, and relative seeks build on it.
    player.seek_exact(21.0);
    player.seek_relative(2.0);
    assert!((player.target_position() - 23.0).abs() < 1e-9);
    assert!(
        wait_until(Duration::from_secs(20), || {
            let state = player.state();
            state.loaded && !state.seeking && (state.time_pos - 23.0).abs() <= 1.0 / 60.0
        }),
        "deferred seek not applied: {:?}",
        player.state()
    );
    // Nothing stale is left behind: the next relative seek starts from the
    // real position.
    player.seek_relative(2.0);
    assert!(
        (player.target_position() - 25.0).abs() <= 1.0 / 60.0,
        "{}",
        player.target_position()
    );
    assert!(wait_until(Duration::from_secs(20), || {
        let state = player.state();
        !state.seeking && (state.time_pos - 25.0).abs() <= 1.0 / 60.0
    }));
    drop_within(player, Duration::from_secs(10));
}
