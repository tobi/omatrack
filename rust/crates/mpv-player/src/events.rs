//! The event thread: blocks in `mpv_wait_event`, mirrors properties into the
//! shared atomics and the [`PlaybackClock`](crate::PlaybackClock), and forwards
//! coarse [`PlayerEvent`]s. It never touches UI state; `time-pos` updates go
//! only to the clock and atomics, never into the event channel.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::JoinHandle;
use std::time::Instant;

use crate::ffi::{EndReason, LOG_LEVEL_WARN, MpvHandle, PropertyFormat, PropertyValue, RawEvent};
use crate::player::PlayerEvent;
use crate::render::RenderControl;
use crate::source::MediaStatus;
use crate::state::Shared;

/// Reply userdata for `seek` commands, so a failed seek can release its
/// pending target.
pub(crate) const SEEK_USERDATA: u64 = 1;

pub(crate) const PROP_TIME_POS: u64 = 1;
pub(crate) const PROP_DURATION: u64 = 2;
pub(crate) const PROP_PAUSE: u64 = 3;
pub(crate) const PROP_MUTE: u64 = 4;
pub(crate) const PROP_VOLUME: u64 = 5;
pub(crate) const PROP_SEEKING: u64 = 6;
pub(crate) const PROP_SPEED: u64 = 7;
pub(crate) const PROP_DWIDTH: u64 = 8;
pub(crate) const PROP_DHEIGHT: u64 = 9;
pub(crate) const PROP_EOF: u64 = 10;

/// The properties mirrored into [`Shared`].
pub(crate) const OBSERVED: [(u64, &str, PropertyFormat); 10] = [
    (PROP_TIME_POS, "time-pos", PropertyFormat::Double),
    (PROP_DURATION, "duration", PropertyFormat::Double),
    (PROP_PAUSE, "pause", PropertyFormat::Flag),
    (PROP_MUTE, "mute", PropertyFormat::Flag),
    (PROP_VOLUME, "volume", PropertyFormat::Double),
    (PROP_SEEKING, "seeking", PropertyFormat::Flag),
    (PROP_SPEED, "speed", PropertyFormat::Double),
    (PROP_DWIDTH, "video-out-params/dw", PropertyFormat::Int64),
    (PROP_DHEIGHT, "video-out-params/dh", PropertyFormat::Int64),
    (PROP_EOF, "eof-reached", PropertyFormat::Flag),
];

pub(crate) struct EventThread {
    handle: Arc<MpvHandle>,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl EventThread {
    pub(crate) fn start(
        handle: Arc<MpvHandle>,
        shared: Arc<Shared>,
        render: Arc<RenderControl>,
        events: async_channel::Sender<PlayerEvent>,
    ) -> std::io::Result<Self> {
        let stop = Arc::new(AtomicBool::new(false));
        let thread = {
            let handle = Arc::clone(&handle);
            let stop = Arc::clone(&stop);
            std::thread::Builder::new()
                .name("mpv-events".into())
                .spawn(move || {
                    let mut pump = EventPump {
                        handle: &handle,
                        shared: &shared,
                        render: &render,
                        events: &events,
                        overflow_logged: false,
                    };
                    while !stop.load(Ordering::Acquire) {
                        let event = handle.wait_event(-1.0);
                        if stop.load(Ordering::Acquire) || !pump.handle_event(event) {
                            break;
                        }
                    }
                })?
        };
        Ok(Self {
            handle,
            stop,
            thread: Some(thread),
        })
    }

    pub(crate) fn stop(&mut self) {
        self.stop.store(true, Ordering::Release);
        self.handle.wakeup();
        if let Some(thread) = self.thread.take()
            && thread.join().is_err()
        {
            log::error!("mpv event thread panicked");
        }
    }
}

impl Drop for EventThread {
    fn drop(&mut self) {
        self.stop();
    }
}

struct EventPump<'a> {
    handle: &'a MpvHandle,
    shared: &'a Shared,
    render: &'a RenderControl,
    events: &'a async_channel::Sender<PlayerEvent>,
    overflow_logged: bool,
}

impl EventPump<'_> {
    fn emit(&mut self, event: PlayerEvent) {
        match self.events.try_send(event) {
            Ok(()) => self.overflow_logged = false,
            Err(async_channel::TrySendError::Full(event)) => {
                if !self.overflow_logged {
                    log::warn!(
                        "mpv player event queue full; dropping {event:?} (state() stays authoritative)"
                    );
                    self.overflow_logged = true;
                }
            }
            Err(async_channel::TrySendError::Closed(_)) => {}
        }
    }

    /// Returns false once mpv shut down.
    fn handle_event(&mut self, event: RawEvent) -> bool {
        let shared = self.shared;
        let now = Instant::now();
        match event {
            RawEvent::None | RawEvent::Other(_) => {}
            RawEvent::Shutdown => {
                self.emit(PlayerEvent::Shutdown);
                return false;
            }
            RawEvent::StartFile => {
                // Keeps a seek requested since `load`: it belongs to this file.
                shared.mark_unloaded(false);
                shared.eof.store(false, Ordering::Release);
                shared.dwidth.store(0, Ordering::Release);
                shared.dheight.store(0, Ordering::Release);
                shared.set_status(MediaStatus::Loading);
                self.emit(PlayerEvent::StartFile);
            }
            RawEvent::FileLoaded => {
                self.refresh_video_size();
                shared.set_status(MediaStatus::Ready);
                // Last, so an observer of `loaded` also sees the status and
                // the video size. The flag flips and the deferred seek is
                // sent under one lock: a seek that saw "not loaded" is applied
                // here, and one that sees "loaded" is sent after this one.
                shared.mark_loaded(|start| {
                    if let Err(error) = self.seek(start) {
                        log::warn!("mpv start seek failed: {error}");
                    }
                });
                let duration = shared.duration.load();
                self.emit(PlayerEvent::FileLoaded { duration });
            }
            RawEvent::EndFile { reason, error } => {
                let message =
                    error.map(|error| format!("Video playback failed: {}", error.message()));
                if reason == EndReason::Error {
                    shared.mark_unloaded(false);
                    let message = message
                        .clone()
                        .unwrap_or_else(|| "Video playback failed".to_owned());
                    shared.set_status(MediaStatus::Failed(message));
                }
                self.emit(PlayerEvent::EndFile {
                    reason,
                    error: message,
                });
            }
            RawEvent::Seek => {
                if shared.lock_pending_seek().is_some() {
                    shared
                        .pending_seek_acknowledged
                        .store(true, Ordering::Release);
                }
                shared.seeking.store(true, Ordering::Release);
                if !shared.clock.is_seeking() {
                    // A seek mpv started on its own: hold the clock where it is.
                    shared.clock.seek_started(shared.clock.estimate(now), now);
                }
                self.emit(PlayerEvent::Seeking);
            }
            RawEvent::PlaybackRestart => {
                if shared
                    .pending_seek_acknowledged
                    .swap(false, Ordering::AcqRel)
                {
                    shared.lock_pending_seek().take();
                }
                shared.seeking.store(false, Ordering::Release);
                shared.clock.seek_finished(now);
                self.emit(PlayerEvent::PlaybackRestart);
            }
            RawEvent::VideoReconfig => {
                self.refresh_video_size();
                self.render.request_redraw();
            }
            RawEvent::Property { id, value } => self.handle_property(id, value, now),
            RawEvent::Reply { userdata, error } => {
                if let Some(error) = error {
                    log::warn!("mpv command failed: {error}");
                    if userdata == SEEK_USERDATA {
                        shared.lock_pending_seek().take();
                        shared
                            .pending_seek_acknowledged
                            .store(false, Ordering::Release);
                        if shared.clock.is_seeking() {
                            shared.clock.seek_finished(now);
                        }
                    }
                    self.emit(PlayerEvent::CommandFailed(error.message().to_owned()));
                }
            }
            RawEvent::Log {
                level,
                prefix,
                text,
            } => {
                if level <= LOG_LEVEL_WARN {
                    log::warn!("libmpv [{prefix}] {text}");
                } else {
                    log::debug!("libmpv [{prefix}] {text}");
                }
            }
        }
        true
    }

    /// Reads the display size directly. START_FILE clears the mirrored size,
    /// and mpv sends no property change when the next file has the same
    /// size, so the observed values alone would leave it at zero.
    fn refresh_video_size(&mut self) {
        let width = self
            .handle
            .get_int64("video-out-params/dw")
            .unwrap_or(0)
            .max(0);
        let height = self
            .handle
            .get_int64("video-out-params/dh")
            .unwrap_or(0)
            .max(0);
        let shared = self.shared;
        let width_changed = shared.dwidth.swap(width, Ordering::AcqRel) != width;
        let height_changed = shared.dheight.swap(height, Ordering::AcqRel) != height;
        let changed = width_changed || height_changed;
        if changed {
            self.render.request_redraw();
            if let Some((width, height)) = shared.video_size() {
                self.emit(PlayerEvent::VideoSize { width, height });
            }
        }
    }

    fn seek(&self, target: f64) -> Result<(), crate::MpvError> {
        *self.shared.lock_pending_seek() = Some(target);
        self.shared
            .pending_seek_acknowledged
            .store(false, Ordering::Release);
        self.shared.clock.seek_started(target, Instant::now());
        let target = format!("{target:.6}");
        self.handle
            .command_async(SEEK_USERDATA, &["seek", &target, "absolute+exact"])
    }

    fn handle_property(&mut self, id: u64, value: PropertyValue, now: Instant) {
        let shared = self.shared;
        match (id, value) {
            (PROP_TIME_POS, PropertyValue::Double(position)) => {
                shared.time_pos.store(position.max(0.0));
                shared.clock.observe(position, now);
            }
            (PROP_DURATION, PropertyValue::Double(duration)) => {
                shared.duration.store(duration.max(0.0));
                shared.clock.set_duration(duration, now);
                self.emit(PlayerEvent::DurationChanged(duration.max(0.0)));
            }
            (PROP_DURATION, PropertyValue::Unavailable) => {
                shared.duration.store(0.0);
                shared.clock.set_duration(0.0, now);
            }
            (PROP_PAUSE, PropertyValue::Flag(paused)) => {
                shared.paused.store(paused, Ordering::Release);
                shared.clock.set_paused(paused, now);
                // The view starts or stops requesting animation frames.
                shared.frames.signal();
                self.emit(PlayerEvent::Paused(paused));
            }
            (PROP_MUTE, PropertyValue::Flag(muted)) => {
                shared.muted.store(muted, Ordering::Release);
                self.emit(PlayerEvent::MuteChanged(muted));
            }
            (PROP_VOLUME, PropertyValue::Double(volume)) => {
                shared.volume.store(volume);
                self.emit(PlayerEvent::VolumeChanged(volume));
            }
            (PROP_SEEKING, PropertyValue::Flag(seeking)) => {
                shared.seeking.store(seeking, Ordering::Release);
            }
            (PROP_SPEED, PropertyValue::Double(speed)) => {
                shared.speed.store(speed);
                shared.clock.set_speed(speed, now);
                self.emit(PlayerEvent::SpeedChanged(speed));
            }
            (PROP_DWIDTH | PROP_DHEIGHT, value) => {
                let size = match value {
                    PropertyValue::Int64(size) => size.max(0),
                    _ => 0,
                };
                let target = if id == PROP_DWIDTH {
                    &shared.dwidth
                } else {
                    &shared.dheight
                };
                if target.swap(size, Ordering::AcqRel) != size {
                    self.render.request_redraw();
                    if let Some((width, height)) = shared.video_size() {
                        self.emit(PlayerEvent::VideoSize { width, height });
                    }
                }
            }
            (PROP_EOF, PropertyValue::Flag(eof)) => {
                if shared.eof.swap(eof, Ordering::AcqRel) != eof {
                    shared.frames.signal();
                    self.emit(PlayerEvent::EndOfFile(eof));
                }
            }
            (PROP_EOF, PropertyValue::Unavailable) => {
                shared.eof.store(false, Ordering::Release);
            }
            _ => {}
        }
    }
}
