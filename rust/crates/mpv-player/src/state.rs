//! State shared between a [`Player`](crate::Player), its event thread, its
//! render thread and the UI. Everything here is lock-free or briefly locked;
//! nothing in it calls into mpv.

use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};

use crate::clock::PlaybackClock;
use crate::render::RenderStats;
use crate::source::{FrameSlot, MediaStatus};

/// An `f64` stored as bits in an `AtomicU64`.
pub(crate) struct AtomicF64(AtomicU64);

impl AtomicF64 {
    pub(crate) fn new(value: f64) -> Self {
        Self(AtomicU64::new(value.to_bits()))
    }

    pub(crate) fn load(&self) -> f64 {
        f64::from_bits(self.0.load(Ordering::Acquire))
    }

    pub(crate) fn store(&self, value: f64) {
        self.0.store(value.to_bits(), Ordering::Release);
    }
}

pub(crate) struct Shared {
    pub time_pos: AtomicF64,
    pub duration: AtomicF64,
    pub speed: AtomicF64,
    pub volume: AtomicF64,
    pub paused: AtomicBool,
    pub eof: AtomicBool,
    pub seeking: AtomicBool,
    pub muted: AtomicBool,
    /// Whether the current file finished loading. Read it freely; change it
    /// only through [`Shared::mark_loaded`] and [`Shared::mark_unloaded`],
    /// which flip it under the `start_position` lock so a seek deciding
    /// "defer or send" cannot interleave with the load completing.
    pub loaded: AtomicBool,
    pub dwidth: AtomicI64,
    pub dheight: AtomicI64,
    /// Target of an exact seek mpv has not finished yet; relative skips start
    /// from here so two quick presses do not land on the same frame.
    pub pending_seek: Mutex<Option<f64>>,
    /// Set once mpv acknowledged the pending seek (`MPV_EVENT_SEEK`); the next
    /// `PLAYBACK_RESTART` then clears it. A restart belonging to an older seek
    /// must not clear a newer target.
    pub pending_seek_acknowledged: AtomicBool,
    /// A seek requested before the file finished loading, applied on
    /// `FILE_LOADED`. Its lock also guards every change of `loaded`.
    start_position: Mutex<Option<f64>>,
    /// Bumped by every `load`; a frame rendered under an older epoch belongs
    /// to the previous file and is not published.
    pub load_epoch: AtomicU64,
    pub status: Mutex<MediaStatus>,
    pub frames: FrameSlot,
    pub clock: PlaybackClock,
    pub stats: Mutex<RenderStats>,
}

impl Shared {
    pub(crate) fn new(paused: bool, muted: bool, volume: f64) -> Self {
        let clock = PlaybackClock::new();
        clock.set_paused(paused, std::time::Instant::now());
        Self {
            time_pos: AtomicF64::new(0.0),
            duration: AtomicF64::new(0.0),
            speed: AtomicF64::new(1.0),
            volume: AtomicF64::new(volume),
            paused: AtomicBool::new(paused),
            eof: AtomicBool::new(false),
            seeking: AtomicBool::new(false),
            muted: AtomicBool::new(muted),
            loaded: AtomicBool::new(false),
            dwidth: AtomicI64::new(0),
            dheight: AtomicI64::new(0),
            pending_seek: Mutex::new(None),
            pending_seek_acknowledged: AtomicBool::new(false),
            start_position: Mutex::new(None),
            load_epoch: AtomicU64::new(0),
            status: Mutex::new(MediaStatus::Idle),
            frames: FrameSlot::new(),
            clock,
            stats: Mutex::new(RenderStats::default()),
        }
    }

    pub(crate) fn status(&self) -> MediaStatus {
        self.status
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone()
    }

    /// Replaces the status and wakes the view when it changed.
    pub(crate) fn set_status(&self, status: MediaStatus) {
        let changed = {
            let mut current = self
                .status
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if *current == status {
                false
            } else {
                *current = status;
                true
            }
        };
        if changed {
            self.frames.signal();
        }
    }

    /// The decoded video size, once known.
    pub(crate) fn video_size(&self) -> Option<(u32, u32)> {
        let width = u32::try_from(self.dwidth.load(Ordering::Acquire)).ok()?;
        let height = u32::try_from(self.dheight.load(Ordering::Acquire)).ok()?;
        (width > 0 && height > 0).then_some((width, height))
    }

    pub(crate) fn is_playing(&self) -> bool {
        self.loaded.load(Ordering::Acquire)
            && !self.paused.load(Ordering::Acquire)
            && !self.eof.load(Ordering::Acquire)
    }

    pub(crate) fn lock_pending_seek(&self) -> std::sync::MutexGuard<'_, Option<f64>> {
        self.pending_seek
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    fn lock_start_position(&self) -> std::sync::MutexGuard<'_, Option<f64>> {
        self.start_position
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    /// The seek remembered for when the file finishes loading, if any.
    pub(crate) fn start_position(&self) -> Option<f64> {
        *self.lock_start_position()
    }

    /// Remembers `target` as the start position when the file has not
    /// finished loading; returns false (and remembers nothing) when it has,
    /// so the caller seeks now. Atomic with respect to [`Shared::mark_loaded`]:
    /// a deferred target is always taken by the load that completes next.
    pub(crate) fn defer_seek_until_loaded(&self, target: f64) -> bool {
        let mut start = self.lock_start_position();
        if self.loaded.load(Ordering::Acquire) {
            return false;
        }
        *start = Some(target);
        true
    }

    /// Marks the file loaded and hands the deferred start position, if any,
    /// to `apply_start` while still holding the lock, so the start seek is
    /// issued before any seek that sees the file as loaded. `apply_start`
    /// must not block or re-enter the start-position API.
    pub(crate) fn mark_loaded(&self, apply_start: impl FnOnce(f64)) {
        let mut start = self.lock_start_position();
        self.loaded.store(true, Ordering::Release);
        if let Some(target) = start.take() {
            apply_start(target);
        }
    }

    /// Marks the file not loaded. `forget_start` drops a deferred seek too
    /// (a new `load` replaces the file it was meant for).
    pub(crate) fn mark_unloaded(&self, forget_start: bool) {
        let mut start = self.lock_start_position();
        self.loaded.store(false, Ordering::Release);
        if forget_start {
            start.take();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Barrier};

    #[test]
    fn a_deferred_seek_is_taken_by_the_next_load() {
        let shared = Shared::new(true, true, 100.0);
        assert!(shared.defer_seek_until_loaded(12.5));
        assert_eq!(shared.start_position(), Some(12.5));
        let mut applied = None;
        shared.mark_loaded(|target| applied = Some(target));
        assert_eq!(applied, Some(12.5));
        assert_eq!(shared.start_position(), None);
        assert!(
            !shared.defer_seek_until_loaded(3.0),
            "loaded: the caller seeks now"
        );
        assert_eq!(shared.start_position(), None);

        shared.mark_unloaded(false);
        assert!(shared.defer_seek_until_loaded(4.0));
        shared.mark_unloaded(true);
        assert_eq!(shared.start_position(), None, "a new load forgets it");
    }

    /// Regression: the seek used to read `loaded` and store the start
    /// position as two steps, so `FILE_LOADED` could land in between, take
    /// nothing, and leave a stale start position that was never applied.
    #[test]
    fn a_seek_racing_the_load_is_never_lost() {
        use std::sync::atomic::AtomicBool;
        let mut deferred_count = 0;
        for _ in 0..5000 {
            let shared = Arc::new(Shared::new(true, true, 100.0));
            let ready = Arc::new(Barrier::new(2));
            let go = Arc::new(AtomicBool::new(false));
            let loader = {
                let shared = Arc::clone(&shared);
                let ready = Arc::clone(&ready);
                let go = Arc::clone(&go);
                std::thread::spawn(move || {
                    ready.wait();
                    while !go.load(Ordering::Acquire) {
                        std::hint::spin_loop();
                    }
                    let mut applied = None;
                    shared.mark_loaded(|target| applied = Some(target));
                    applied
                })
            };
            ready.wait();
            go.store(true, Ordering::Release);
            let deferred = shared.defer_seek_until_loaded(42.0);
            let taken = loader.join().expect("loader thread");
            if deferred {
                deferred_count += 1;
                assert_eq!(taken, Some(42.0), "a deferred seek reaches the load");
            } else {
                assert_eq!(taken, None);
            }
            assert_eq!(shared.start_position(), None, "nothing stale is left");
        }
        // The deferring order occurred, so the race window was exercised
        // (the old split check fails this test within a few iterations).
        assert!(deferred_count > 0);
    }
}
