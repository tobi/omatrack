//! [`Player`]: one libmpv core rendering through the software render API.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::Instant;

use gpui_kit::{DevicePixels, Size};

use crate::clock::PlaybackClock;
use crate::events::{EventThread, OBSERVED, SEEK_USERDATA};
use crate::ffi::{EndReason, MpvError, MpvHandle, SwRenderContext};
use crate::render::{RenderControl, RenderStats, RenderThread};
use crate::source::{FrameSource, MediaStatus, VideoFrame};
use crate::state::Shared;
use crate::sync::FollowAction;

/// Lowest and highest `speed` mpv accepts.
const MIN_SPEED: f64 = 0.01;
const MAX_SPEED: f64 = 100.0;

/// Capacity of the [`PlayerEvent`] channel. Events are notifications; when a
/// consumer falls this far behind, further events are dropped and
/// [`Player::state`] stays authoritative.
const EVENT_CAPACITY: usize = 256;

/// Construction options for [`Player::new`].
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub struct PlayerOptions {
    /// Directory for mpv's demuxer cache of network streams (`demuxer-cache-dir`).
    /// `None` leaves mpv's default.
    pub cache_dir: Option<PathBuf>,
    /// Start muted.
    pub muted: bool,
    /// mpv `hwdec` value. Software rendering needs frames in system memory, so
    /// the default is `auto-copy-safe`.
    pub hwdec: String,
    /// mpv `ao` value; `Some("null")` plays without an audio device.
    /// `None` uses mpv's default output.
    pub audio_output: Option<String>,
    /// Initial volume, 0–100.
    pub volume: f64,
    /// Cap on rendered frames per second (for example 30 for a reference
    /// picture-in-picture); `None` renders every frame.
    pub max_fps: Option<f64>,
    /// Name reported to the audio server (`audio-client-name`).
    pub client_name: String,
}

impl Default for PlayerOptions {
    fn default() -> Self {
        Self {
            cache_dir: None,
            muted: false,
            hwdec: "auto-copy-safe".to_owned(),
            audio_output: None,
            volume: 75.0,
            max_fps: None,
            client_name: "mpv-player".to_owned(),
        }
    }
}

impl PlayerOptions {
    /// Sets [`PlayerOptions::cache_dir`].
    pub fn cache_dir(mut self, dir: impl Into<PathBuf>) -> Self {
        self.cache_dir = Some(dir.into());
        self
    }

    /// Sets [`PlayerOptions::muted`].
    pub fn muted(mut self, muted: bool) -> Self {
        self.muted = muted;
        self
    }

    /// Sets [`PlayerOptions::hwdec`].
    pub fn hwdec(mut self, hwdec: impl Into<String>) -> Self {
        self.hwdec = hwdec.into();
        self
    }

    /// Sets [`PlayerOptions::audio_output`].
    pub fn audio_output(mut self, output: impl Into<String>) -> Self {
        self.audio_output = Some(output.into());
        self
    }

    /// Sets [`PlayerOptions::volume`].
    pub fn volume(mut self, volume: f64) -> Self {
        self.volume = volume;
        self
    }

    /// Sets [`PlayerOptions::max_fps`].
    pub fn max_fps(mut self, max_fps: Option<f64>) -> Self {
        self.max_fps = max_fps;
        self
    }

    /// Sets [`PlayerOptions::client_name`].
    pub fn client_name(mut self, name: impl Into<String>) -> Self {
        self.client_name = name.into();
        self
    }
}

/// A snapshot of the player's observed state.
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub struct PlayerState {
    /// Last reported media time in seconds (see [`Player::clock`] for an
    /// interpolated value).
    pub time_pos: f64,
    /// Media duration in seconds, 0 when unknown.
    pub duration: f64,
    pub paused: bool,
    /// The end of the file was reached (the last frame stays up).
    pub eof: bool,
    pub seeking: bool,
    pub speed: f64,
    pub muted: bool,
    pub volume: f64,
    /// Display width of the decoded video, 0 until known.
    pub dwidth: u32,
    /// Display height of the decoded video, 0 until known.
    pub dheight: u32,
    /// A file is loaded.
    pub loaded: bool,
    pub status: MediaStatus,
}

/// Coarse player notifications, delivered through [`Player::events`].
///
/// `time-pos` is deliberately absent: poll [`Player::clock`] once per frame
/// instead of fanning out a per-frame event.
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub enum PlayerEvent {
    StartFile,
    FileLoaded {
        duration: f64,
    },
    EndFile {
        reason: EndReason,
        error: Option<String>,
    },
    Paused(bool),
    SpeedChanged(f64),
    MuteChanged(bool),
    VolumeChanged(f64),
    DurationChanged(f64),
    VideoSize {
        width: u32,
        height: u32,
    },
    Seeking,
    PlaybackRestart,
    EndOfFile(bool),
    CommandFailed(String),
    Shutdown,
}

/// One libmpv player rendering through the software render API.
///
/// Owns an mpv core, an event thread (property mirroring, [`PlayerEvent`]s)
/// and a render thread (frames into GPUI images). Control methods are
/// asynchronous and never block on mpv; their effect is visible through
/// [`Player::state`], [`Player::clock`] and [`Player::events`].
///
/// Dropping the player stops both threads, frees the render context and then
/// destroys the core.
pub struct Player {
    // Field order is drop order: the render thread (and its context) and the
    // event thread go before the last reference to the core.
    render_thread: RenderThread,
    event_thread: EventThread,
    render: Arc<RenderControl>,
    shared: Arc<Shared>,
    events: async_channel::Receiver<PlayerEvent>,
    handle: Arc<MpvHandle>,
}

impl Player {
    /// Creates and initializes an mpv core with the embedding policy:
    /// `vo=libmpv`, no config/terminal/OSC/input bindings, builtin bilinear
    /// scalers (see `docs/VIDEO_SCALER_COMPATIBILITY.md`), exact seeks,
    /// `keep-open`, starting paused.
    pub fn new(options: PlayerOptions) -> Result<Self, MpvError> {
        let handle = Arc::new(MpvHandle::create()?);
        let volume = options.volume.clamp(0.0, 100.0);
        let mut settings: Vec<(&str, String)> = vec![
            ("config", "no".into()),
            ("terminal", "no".into()),
            ("input-default-bindings", "no".into()),
            ("input-vo-keyboard", "no".into()),
            ("osc", "no".into()),
            ("idle", "yes".into()),
            ("vo", "libmpv".into()),
            // Builtin bilinear everywhere: mpv 0.41's padded scaler LUTs can
            // carry NaNs (GPU path), and bilinear is the cheap choice for the
            // software scalers too.
            ("scale", "bilinear".into()),
            ("cscale", "bilinear".into()),
            ("dscale", "bilinear".into()),
            ("sws-scaler", "bilinear".into()),
            ("zimg-scaler", "bilinear".into()),
            ("hwdec", options.hwdec.clone()),
            ("hr-seek", "yes".into()),
            // Signal a frame when it is due instead of early and then blocking
            // inside mpv_render_context_render until its display time: the
            // render thread stays responsive (size changes, stop) and render
            // timings measure CPU cost, not waiting.
            ("video-timing-offset", "0".into()),
            ("keep-open", "yes".into()),
            ("pause", "yes".into()),
            ("mute", if options.muted { "yes" } else { "no" }.into()),
            ("volume", format!("{volume}")),
            ("audio-client-name", options.client_name.clone()),
        ];
        if let Some(output) = &options.audio_output {
            settings.push(("ao", output.clone()));
        }
        if let Some(dir) = &options.cache_dir {
            let dir = dir.to_str().ok_or_else(|| {
                MpvError::local(format!("cache dir is not UTF-8: {}", dir.display()))
            })?;
            settings.push(("demuxer-cache-dir", dir.to_owned()));
        }
        for (name, value) in &settings {
            handle.set_option_string(name, value)?;
        }
        handle.initialize()?;
        handle.request_log_messages("warn")?;
        for (id, name, format) in OBSERVED {
            handle.observe_property(id, name, format)?;
        }

        let shared = Arc::new(Shared::new(true, options.muted, volume));
        let render = Arc::new(RenderControl::new(options.max_fps));
        let context = SwRenderContext::create(Arc::clone(&handle))?;
        let render_thread = RenderThread::start(context, Arc::clone(&render), Arc::clone(&shared))
            .map_err(|error| MpvError::local(format!("cannot start render thread: {error}")))?;
        let (events_tx, events) = async_channel::bounded(EVENT_CAPACITY);
        let event_thread = EventThread::start(
            Arc::clone(&handle),
            Arc::clone(&shared),
            Arc::clone(&render),
            events_tx,
        )
        .map_err(|error| MpvError::local(format!("cannot start event thread: {error}")))?;

        Ok(Self {
            render_thread,
            event_thread,
            render,
            shared,
            events,
            handle,
        })
    }

    /// Opens `path`, replacing the current file. Playback state (paused,
    /// speed, mute) carries over; the clock resets to 0.
    pub fn load(&self, path: &Path) -> Result<(), MpvError> {
        let path_str = path
            .to_str()
            .ok_or_else(|| MpvError::local(format!("path is not UTF-8: {}", path.display())))?;
        let shared = &*self.shared;
        shared.mark_unloaded(true);
        shared.eof.store(false, Ordering::Release);
        shared.time_pos.store(0.0);
        shared.lock_pending_seek().take();
        shared.clock.reset(Instant::now());
        shared.load_epoch.fetch_add(1, Ordering::AcqRel);
        shared.frames.publish(None);
        shared.set_status(MediaStatus::Loading);
        let result = self
            .handle
            .command_async(0, &["loadfile", path_str, "replace"]);
        if let Err(error) = &result {
            shared.set_status(MediaStatus::Failed(format!(
                "Cannot open video: {}",
                error.message()
            )));
        }
        result
    }

    /// Resumes playback.
    pub fn play(&self) {
        self.set_paused(false);
    }

    /// Pauses playback.
    pub fn pause(&self) {
        self.set_paused(true);
    }

    /// Toggles pause.
    pub fn toggle(&self) {
        self.set_paused(!self.shared.paused.load(Ordering::Acquire));
    }

    fn set_paused(&self, paused: bool) {
        // Optimistic: the UI sees the requested state at once; mpv's `pause`
        // property change confirms it.
        self.shared.paused.store(paused, Ordering::Release);
        self.shared.clock.set_paused(paused, Instant::now());
        self.shared.frames.signal();
        self.report(self.handle.set_flag_async(0, "pause", paused));
    }

    /// Exact seek to `seconds` (clamped to the file). Before the file has
    /// loaded, the seek is remembered and applied on load.
    pub fn seek_exact(&self, seconds: f64) {
        if !seconds.is_finite() {
            return;
        }
        let mut target = seconds.max(0.0);
        let duration = self.shared.duration.load();
        if duration > 0.0 {
            target = target.min(duration);
        }
        let now = Instant::now();
        // Decided under the same lock FILE_LOADED takes the start position
        // with, so a seek racing the load is either deferred and applied by
        // it, or sent now.
        if self.shared.defer_seek_until_loaded(target) {
            self.shared.clock.seek_started(target, now);
            return;
        }
        *self.shared.lock_pending_seek() = Some(target);
        self.shared
            .pending_seek_acknowledged
            .store(false, Ordering::Release);
        self.shared.clock.seek_started(target, now);
        let argument = format!("{target:.6}");
        self.report(
            self.handle
                .command_async(SEEK_USERDATA, &["seek", &argument, "absolute+exact"]),
        );
    }

    /// Exact seek relative to [`Player::target_position`].
    pub fn seek_relative(&self, delta_seconds: f64) {
        self.seek_exact(self.target_position() + delta_seconds);
    }

    /// Where the playhead is or is about to be: the target of an unfinished
    /// seek, else the last reported position.
    pub fn target_position(&self) -> f64 {
        if let Some(target) = *self.shared.lock_pending_seek() {
            return target;
        }
        if let Some(target) = self.shared.start_position() {
            return target;
        }
        self.shared.time_pos.load()
    }

    /// Sets the playback speed (clamped to mpv's 0.01–100).
    pub fn set_speed(&self, speed: f64) {
        if !speed.is_finite() {
            return;
        }
        let speed = speed.clamp(MIN_SPEED, MAX_SPEED);
        self.shared.speed.store(speed);
        self.shared.clock.set_speed(speed, Instant::now());
        self.report(self.handle.set_double_async(0, "speed", speed));
    }

    /// Mutes or unmutes audio.
    pub fn set_mute(&self, muted: bool) {
        self.shared.muted.store(muted, Ordering::Release);
        self.report(self.handle.set_flag_async(0, "mute", muted));
    }

    /// Sets the volume, 0–100.
    pub fn set_volume(&self, volume: f64) {
        if !volume.is_finite() {
            return;
        }
        let volume = volume.clamp(0.0, 100.0);
        self.shared.volume.store(volume);
        self.report(self.handle.set_double_async(0, "volume", volume));
    }

    /// Applies a [`Follower`](crate::Follower) decision.
    pub fn apply(&self, action: FollowAction) {
        match action {
            FollowAction::None => {}
            FollowAction::SetSpeed(speed) => self.set_speed(speed),
            FollowAction::HardSeek(target) => self.seek_exact(target),
        }
    }

    /// Caps rendered frames per second; `None` removes the cap.
    pub fn set_max_fps(&self, max_fps: Option<f64>) {
        self.render.set_max_fps(max_fps);
    }

    /// The current state snapshot.
    pub fn state(&self) -> PlayerState {
        let shared = &*self.shared;
        let (dwidth, dheight) = shared.video_size().unwrap_or((0, 0));
        PlayerState {
            time_pos: shared.time_pos.load(),
            duration: shared.duration.load(),
            paused: shared.paused.load(Ordering::Acquire),
            eof: shared.eof.load(Ordering::Acquire),
            seeking: shared.seeking.load(Ordering::Acquire) || shared.clock.is_seeking(),
            speed: shared.speed.load(),
            muted: shared.muted.load(Ordering::Acquire),
            volume: shared.volume.load(),
            dwidth,
            dheight,
            loaded: shared.loaded.load(Ordering::Acquire),
            status: shared.status(),
        }
    }

    /// The interpolated clock (cheap to clone, lock-free to read).
    pub fn clock(&self) -> PlaybackClock {
        self.shared.clock.clone()
    }

    /// The event stream. All clones share one queue (each event is received
    /// once); keep one consumer.
    pub fn events(&self) -> async_channel::Receiver<PlayerEvent> {
        self.events.clone()
    }

    /// The frame source for a [`VideoView`](crate::VideoView).
    pub fn frame_source(&self) -> Arc<dyn FrameSource> {
        Arc::new(PlayerFrames {
            shared: Arc::clone(&self.shared),
            render: Arc::clone(&self.render),
        })
    }

    /// Software render timings since creation or the last reset.
    pub fn render_stats(&self) -> RenderStats {
        *self
            .shared
            .stats
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    /// Clears [`Player::render_stats`].
    pub fn reset_render_stats(&self) {
        *self
            .shared
            .stats
            .lock()
            .unwrap_or_else(|poison| poison.into_inner()) = RenderStats::default();
    }

    /// Reads the exact current `time-pos` from mpv (a synchronous round trip;
    /// prefer [`Player::state`] or [`Player::clock`] on the UI thread).
    pub fn query_time_pos(&self) -> Result<f64, MpvError> {
        self.handle.get_double("time-pos")
    }

    fn report(&self, result: Result<(), MpvError>) {
        if let Err(error) = result {
            log::warn!("mpv request failed: {error}");
        }
    }
}

impl Drop for Player {
    fn drop(&mut self) {
        // Explicit order: render context first (render.h: before the core is
        // destroyed), then the event thread, then the core via `handle`.
        self.render_thread.stop();
        self.event_thread.stop();
    }
}

/// [`FrameSource`] over a player's shared state and render control.
struct PlayerFrames {
    shared: Arc<Shared>,
    render: Arc<RenderControl>,
}

impl FrameSource for PlayerFrames {
    fn latest_frame(&self) -> Option<VideoFrame> {
        self.shared.frames.latest()
    }

    fn frame_generation(&self) -> u64 {
        self.shared.frames.generation()
    }

    fn frame_signal(&self) -> async_channel::Receiver<()> {
        self.shared.frames.receiver()
    }

    fn set_target_size(&self, size: Option<Size<DevicePixels>>) {
        self.render.set_target_size(size);
    }

    fn status(&self) -> MediaStatus {
        self.shared.status()
    }

    fn is_playing(&self) -> bool {
        self.shared.is_playing()
    }
}
