//! The primary recording's onboard video: one mpv player bound to the
//! primary lap, driven by the playback commands.

use std::path::PathBuf;
use std::sync::Arc;

use gpui_kit::{Context, Entity, EventEmitter, SharedString, Subscription, Task};
use mpv_player::{EndReason, FrameSource, MediaStatus, Player, PlayerEvent, PlayerOptions};
use omatrack_core::session::{IdentityState, LoadedLap};

use crate::state::preferences::Preferences;
use crate::state::session::{Session, SessionEvent};

/// Seek step of the Left/Right keys, seconds.
pub const SEEK_STEP: f64 = 2.0;
/// The `S` slow-motion rate.
pub const SLOW_MOTION: f64 = 0.25;

/// Fullscreen video compositions (keys 1-5).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ComposeLayout {
    #[default]
    Split,
    PrimaryWithReferenceInset,
    ReferenceWithPrimaryInset,
    PrimaryOnly,
    ReferenceOnly,
}

impl ComposeLayout {
    pub const ALL: [Self; 5] = [
        Self::Split,
        Self::PrimaryWithReferenceInset,
        Self::ReferenceWithPrimaryInset,
        Self::PrimaryOnly,
        Self::ReferenceOnly,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Self::Split => "Split",
            Self::PrimaryWithReferenceInset => "Primary with reference inset",
            Self::ReferenceWithPrimaryInset => "Reference with primary inset",
            Self::PrimaryOnly => "Primary only",
            Self::ReferenceOnly => "Reference only",
        }
    }
}

/// Whether video can play in this process.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VideoAvailability {
    /// Playback is switched off (headless tests).
    Disabled,
    /// No player yet: no primary video bound.
    Idle,
    Ready,
    /// libmpv could not start or open the file (user-facing message).
    Failed(SharedString),
}

/// What changed in the [`VideoController`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VideoEvent {
    /// A different source is bound (views rebuild their frame source).
    SourceChanged,
    /// The bound file could not be opened or played (user-facing message).
    Failed(SharedString),
}

/// Owns the primary player. Minimal on purpose: binding, play/pause,
/// ±2 s seeks, persisted mute and 0.25x slow motion. Reference playback and
/// telemetry sync build on it.
pub struct VideoController {
    preferences: Entity<Preferences>,
    enabled: bool,
    audio_output: Option<String>,
    player: Option<Player>,
    bound: Option<PathBuf>,
    identity_warning: Option<SharedString>,
    availability: VideoAvailability,
    slow_motion: bool,
    layout: ComposeLayout,
    _player_events: Option<Task<()>>,
    _subscriptions: Vec<Subscription>,
}

impl EventEmitter<VideoEvent> for VideoController {}

impl VideoController {
    pub fn new(
        preferences: Entity<Preferences>,
        session: &Entity<Session>,
        enabled: bool,
        audio_output: Option<String>,
        cx: &mut Context<Self>,
    ) -> Self {
        let subscriptions = vec![cx.subscribe(session, |this, session, event, cx| {
            if matches!(event, SessionEvent::PrimaryChanged | SessionEvent::Swapped) {
                let primary = session
                    .read(cx)
                    .primary()
                    .and_then(|slot| slot.loaded())
                    .cloned();
                this.bind(primary.as_ref(), cx);
            }
        })];
        Self {
            preferences,
            enabled,
            audio_output,
            player: None,
            bound: None,
            identity_warning: None,
            availability: if enabled {
                VideoAvailability::Idle
            } else {
                VideoAvailability::Disabled
            },
            slow_motion: false,
            layout: ComposeLayout::default(),
            _player_events: None,
            _subscriptions: subscriptions,
        }
    }

    pub fn availability(&self) -> &VideoAvailability {
        &self.availability
    }

    /// The video file bound to the primary lap.
    pub fn bound_path(&self) -> Option<&PathBuf> {
        self.bound.as_ref()
    }

    /// Why the video's timing cannot be trusted, when it cannot.
    pub fn identity_warning(&self) -> Option<&SharedString> {
        self.identity_warning.as_ref()
    }

    /// The frames of the bound player, for a `VideoView`.
    pub fn frame_source(&self) -> Option<Arc<dyn FrameSource>> {
        self.player
            .as_ref()
            .filter(|_| self.bound.is_some())
            .map(Player::frame_source)
    }

    pub fn is_muted(&self, cx: &gpui_kit::App) -> bool {
        self.preferences.read(cx).config().video.is_muted()
    }

    pub fn is_playing(&self) -> bool {
        self.player
            .as_ref()
            .is_some_and(|player| !player.state().paused)
    }

    pub fn is_slow_motion(&self) -> bool {
        self.slow_motion
    }

    pub fn layout(&self) -> ComposeLayout {
        self.layout
    }

    pub fn is_continuous(&self, cx: &gpui_kit::App) -> bool {
        self.preferences
            .read(cx)
            .config()
            .video
            .is_continuous_playback()
    }

    fn ensure_player(&mut self, cx: &mut Context<Self>) -> Option<&Player> {
        if !self.enabled {
            return None;
        }
        if self.player.is_none() {
            let mut options = PlayerOptions::default().muted(self.is_muted(cx));
            if let Some(output) = &self.audio_output {
                options = options.audio_output(output.clone());
            }
            match Player::new(options) {
                Ok(player) => {
                    // `load` only queues the file; opening it can still
                    // fail, and only the event stream says so.
                    let events = player.events();
                    self._player_events = Some(cx.spawn(async move |this, cx| {
                        while let Ok(event) = events.recv().await {
                            if this
                                .update(cx, |this, cx| this.on_player_event(event, cx))
                                .is_err()
                            {
                                break;
                            }
                        }
                    }));
                    self.player = Some(player);
                }
                Err(error) => {
                    let message: SharedString =
                        format!("Video playback is unavailable. {error}").into();
                    self.availability = VideoAvailability::Failed(message.clone());
                    cx.emit(VideoEvent::Failed(message));
                    return None;
                }
            }
        }
        self.player.as_ref()
    }

    /// Bind the primary lap's video (or nothing) and park it at the lap
    /// start, paused.
    pub fn bind(&mut self, lap: Option<&LoadedLap>, cx: &mut Context<Self>) {
        let binding = lap.and_then(|lap| lap.video());
        let path = binding.map(|binding| binding.path.clone());
        self.identity_warning = binding
            .filter(|binding| binding.identity == IdentityState::Mismatch)
            .and_then(|binding| binding.warning.clone())
            .map(SharedString::from);
        if path == self.bound {
            if let (Some(lap), Some(player)) = (lap, self.player.as_ref()) {
                player.seek_exact(lap_start_seconds(lap));
            }
            cx.notify();
            return;
        }
        self.bound = None;
        let (Some(lap), Some(path)) = (lap, path) else {
            if let Some(player) = self.player.as_ref() {
                player.pause();
            }
            if self.enabled {
                self.availability = VideoAvailability::Idle;
            }
            cx.emit(VideoEvent::SourceChanged);
            cx.notify();
            return;
        };
        self.open_file(path, lap_start_seconds(lap), cx);
    }

    /// Open a video file, parked paused at `start_seconds` (any previous
    /// file is replaced). The file opens asynchronously: a file libmpv
    /// cannot play turns [`Self::availability`] into `Failed` and emits
    /// [`VideoEvent::Failed`] once libmpv reports it.
    pub fn open_file(&mut self, path: PathBuf, start_seconds: f64, cx: &mut Context<Self>) {
        let start = start_seconds;
        self.bound = None;
        let Some(player) = self.ensure_player(cx) else {
            cx.emit(VideoEvent::SourceChanged);
            cx.notify();
            return;
        };
        let loaded = player.load(&path);
        match loaded {
            Ok(()) => {
                player.pause();
                player.seek_exact(start);
                self.bound = Some(path);
                self.availability = VideoAvailability::Ready;
            }
            Err(error) => {
                let message: SharedString = format!("Couldn’t open the video. {error}").into();
                self.availability = VideoAvailability::Failed(message.clone());
                cx.emit(VideoEvent::Failed(message));
            }
        }
        cx.emit(VideoEvent::SourceChanged);
        cx.notify();
    }

    fn on_player_event(&mut self, event: PlayerEvent, cx: &mut Context<Self>) {
        let PlayerEvent::EndFile {
            reason: EndReason::Error,
            ..
        } = event
        else {
            return;
        };
        // The player state is authoritative: an error queued for a file
        // that was since replaced finds the new file loading, not failed.
        let status = self.player.as_ref().map(|player| player.state().status);
        if let (Some(_), Some(MediaStatus::Failed(message))) = (&self.bound, status) {
            self.fail(message.into(), cx);
        }
    }

    /// The bound file failed: drop it and say why.
    fn fail(&mut self, message: SharedString, cx: &mut Context<Self>) {
        if let Some(player) = self.player.as_ref() {
            player.pause();
        }
        self.bound = None;
        self.availability = VideoAvailability::Failed(message.clone());
        cx.emit(VideoEvent::SourceChanged);
        cx.emit(VideoEvent::Failed(message));
        cx.notify();
    }

    fn with_player(&self, f: impl FnOnce(&Player)) {
        if let Some(player) = self.player.as_ref().filter(|_| self.bound.is_some()) {
            f(player);
        }
    }

    pub fn toggle_play(&mut self, cx: &mut Context<Self>) {
        self.with_player(Player::toggle);
        cx.notify();
    }

    pub fn seek_by(&mut self, seconds: f64, cx: &mut Context<Self>) {
        self.with_player(|player| player.seek_relative(seconds));
        cx.notify();
    }

    /// Toggle audio and persist `video.muted`.
    pub fn toggle_mute(&mut self, cx: &mut Context<Self>) {
        let muted = !self.is_muted(cx);
        self.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| config.video.muted = Some(muted));
        });
        if let Some(player) = &self.player {
            player.set_mute(muted);
        }
        cx.notify();
    }

    /// 0.25x on the primary clock, or back to 1x.
    pub fn toggle_slow_motion(&mut self, cx: &mut Context<Self>) {
        self.slow_motion = !self.slow_motion;
        let speed = if self.slow_motion { SLOW_MOTION } else { 1.0 };
        if let Some(player) = &self.player {
            player.set_speed(speed);
        }
        cx.notify();
    }

    /// Per-lap or continuous playback (`video.continuous_playback`).
    pub fn toggle_continuous(&mut self, cx: &mut Context<Self>) {
        let continuous = !self.is_continuous(cx);
        self.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| {
                config.video.continuous_playback = Some(continuous)
            });
        });
        cx.notify();
    }

    pub fn set_layout(&mut self, layout: ComposeLayout, cx: &mut Context<Self>) {
        if layout != self.layout {
            self.layout = layout;
            cx.notify();
        }
    }
}

/// Presentation time (s) of the lap's first telemetry sample in its video.
fn lap_start_seconds(lap: &LoadedLap) -> f64 {
    let telemetry = lap.lap().start_time.max(0.0);
    let Some(binding) = lap.video() else {
        return telemetry;
    };
    let nanoseconds = (telemetry * 1e9).round() as u64;
    binding
        .clock
        .presentation_time_ns(nanoseconds, binding.file_index)
        .map(|ns| ns as f64 / 1e9)
        .unwrap_or(telemetry)
}
