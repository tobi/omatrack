//! The interpolated playback clock.
//!
//! libmpv reports `time-pos` once per displayed frame, on its own event
//! thread. Consumers that draw once per display frame (a trace cursor, a
//! telemetry HUD) need a position *now*, not at the last video frame, and must
//! not wait on the event thread for it. [`PlaybackClock`] keeps the last
//! observation (position, the [`Instant`] it arrived, speed, paused and
//! seeking flags) in atomics behind a sequence lock, and
//! [`PlaybackClock::estimate`] extrapolates from it without locking.

use std::sync::atomic::{AtomicU64, Ordering, fence};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// A snapshot of the clock's last observation.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct ClockSample {
    /// Media time in seconds at `observed_at`.
    pub position: f64,
    /// When `position` was observed.
    pub observed_at: Instant,
    /// Playback speed (1.0 is real time).
    pub speed: f64,
    /// Media duration in seconds, or 0 when unknown.
    pub duration: f64,
    /// Whether playback is paused.
    pub paused: bool,
    /// Whether a seek is in flight; the position is the seek target.
    pub seeking: bool,
    /// Bumped whenever the timeline jumps (load, seek), so consumers can
    /// tell a discontinuity from ordinary progress.
    pub generation: u64,
}

impl ClockSample {
    /// The extrapolated media time at `now`.
    ///
    /// Paused, seeking or zero-speed clocks hold their position. A running
    /// clock advances by `speed` × elapsed wall time and is clamped to
    /// `[0, duration]` when the duration is known.
    pub fn estimate(&self, now: Instant) -> f64 {
        let mut position = self.position;
        if !self.paused && !self.seeking && self.speed > 0.0 {
            let elapsed = now.saturating_duration_since(self.observed_at);
            position += elapsed.as_secs_f64() * self.speed;
        }
        position = position.max(0.0);
        if self.duration > 0.0 {
            position = position.min(self.duration);
        }
        position
    }

    /// Whether the clock advances with wall time.
    pub fn is_running(&self) -> bool {
        !self.paused && !self.seeking && self.speed > 0.0
    }
}

const FLAG_PAUSED: u64 = 1;
const FLAG_SEEKING: u64 = 2;

struct Inner {
    origin: Instant,
    /// Sequence lock: odd while a writer is storing fields.
    sequence: AtomicU64,
    position: AtomicU64,
    observed_nanos: AtomicU64,
    speed: AtomicU64,
    duration: AtomicU64,
    flags: AtomicU64,
    generation: AtomicU64,
    /// Serializes writers (the event thread and control calls).
    writer: Mutex<()>,
}

/// A cheaply clonable, thread-safe handle to one player's clock.
///
/// Writers are the player's event thread and its control methods; readers
/// (typically the UI, once per frame) never block.
#[derive(Clone)]
pub struct PlaybackClock {
    inner: Arc<Inner>,
}

impl Default for PlaybackClock {
    fn default() -> Self {
        Self::new()
    }
}

impl std::fmt::Debug for PlaybackClock {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_tuple("PlaybackClock")
            .field(&self.sample())
            .finish()
    }
}

impl PlaybackClock {
    /// A paused clock at 0 s, speed 1.
    pub fn new() -> Self {
        let origin = Instant::now();
        Self {
            inner: Arc::new(Inner {
                origin,
                sequence: AtomicU64::new(0),
                position: AtomicU64::new(0.0_f64.to_bits()),
                observed_nanos: AtomicU64::new(0),
                speed: AtomicU64::new(1.0_f64.to_bits()),
                duration: AtomicU64::new(0.0_f64.to_bits()),
                flags: AtomicU64::new(FLAG_PAUSED),
                generation: AtomicU64::new(0),
                writer: Mutex::new(()),
            }),
        }
    }

    /// The extrapolated media time at `now` (see [`ClockSample::estimate`]).
    pub fn estimate(&self, now: Instant) -> f64 {
        self.sample().estimate(now)
    }

    /// The last observation, read consistently without locking.
    pub fn sample(&self) -> ClockSample {
        let inner = &*self.inner;
        loop {
            let before = inner.sequence.load(Ordering::Acquire);
            if before & 1 == 1 {
                std::hint::spin_loop();
                continue;
            }
            let position = f64::from_bits(inner.position.load(Ordering::Relaxed));
            let observed_nanos = inner.observed_nanos.load(Ordering::Relaxed);
            let speed = f64::from_bits(inner.speed.load(Ordering::Relaxed));
            let duration = f64::from_bits(inner.duration.load(Ordering::Relaxed));
            let flags = inner.flags.load(Ordering::Relaxed);
            let generation = inner.generation.load(Ordering::Relaxed);
            fence(Ordering::Acquire);
            if inner.sequence.load(Ordering::Relaxed) == before {
                return ClockSample {
                    position,
                    observed_at: inner.origin + Duration::from_nanos(observed_nanos),
                    speed,
                    duration,
                    paused: flags & FLAG_PAUSED != 0,
                    seeking: flags & FLAG_SEEKING != 0,
                    generation,
                };
            }
        }
    }

    /// Whether a seek is in flight.
    pub fn is_seeking(&self) -> bool {
        self.sample().seeking
    }

    /// Applies `change` to the current sample as one atomic update.
    fn write(&self, now: Instant, change: impl FnOnce(&mut ClockSample)) {
        let inner = &*self.inner;
        let _writer = inner
            .writer
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        let mut sample = self.sample();
        change(&mut sample);
        let nanos = now
            .saturating_duration_since(inner.origin)
            .as_nanos()
            .min(u128::from(u64::MAX)) as u64;
        let flags = (if sample.paused { FLAG_PAUSED } else { 0 })
            | (if sample.seeking { FLAG_SEEKING } else { 0 });

        let sequence = inner.sequence.load(Ordering::Relaxed);
        inner
            .sequence
            .store(sequence.wrapping_add(1), Ordering::Relaxed);
        fence(Ordering::Release);
        inner
            .position
            .store(sample.position.to_bits(), Ordering::Relaxed);
        inner.observed_nanos.store(nanos, Ordering::Relaxed);
        inner.speed.store(sample.speed.to_bits(), Ordering::Relaxed);
        inner
            .duration
            .store(sample.duration.to_bits(), Ordering::Relaxed);
        inner.flags.store(flags, Ordering::Relaxed);
        inner.generation.store(sample.generation, Ordering::Relaxed);
        inner
            .sequence
            .store(sequence.wrapping_add(2), Ordering::Release);
    }

    /// Re-anchors the extrapolation at `now`, keeping the estimated position.
    /// Used before any change of rate or pause state so the change applies
    /// from the current position rather than the last observation.
    fn rebase(sample: &mut ClockSample, now: Instant) {
        sample.position = sample.estimate(now);
    }

    /// A `time-pos` observation from the player. Ignored while a seek is in
    /// flight: the clock holds the seek target until playback restarts, so a
    /// late report of the pre-seek position cannot make it jump back.
    pub fn observe(&self, position: f64, now: Instant) {
        if !position.is_finite() {
            return;
        }
        self.write(now, |sample| {
            if !sample.seeking {
                sample.position = position.max(0.0);
            }
        });
    }

    /// Pause state changed.
    pub fn set_paused(&self, paused: bool, now: Instant) {
        self.write(now, |sample| {
            Self::rebase(sample, now);
            sample.paused = paused;
        });
    }

    /// Playback speed changed.
    pub fn set_speed(&self, speed: f64, now: Instant) {
        if !speed.is_finite() || speed < 0.0 {
            return;
        }
        self.write(now, |sample| {
            Self::rebase(sample, now);
            sample.speed = speed;
        });
    }

    /// Media duration changed (0 when unknown).
    pub fn set_duration(&self, duration: f64, now: Instant) {
        let duration = if duration.is_finite() {
            duration.max(0.0)
        } else {
            0.0
        };
        self.write(now, |sample| {
            Self::rebase(sample, now);
            sample.duration = duration;
        });
    }

    /// A seek to `target` was issued: the clock holds at the target until
    /// [`PlaybackClock::seek_finished`].
    pub fn seek_started(&self, target: f64, now: Instant) {
        if !target.is_finite() {
            return;
        }
        self.write(now, |sample| {
            sample.position = target.max(0.0);
            sample.seeking = true;
            sample.generation = sample.generation.wrapping_add(1);
        });
    }

    /// Playback restarted after a seek; the clock runs again from its current
    /// position until the next observation corrects it.
    pub fn seek_finished(&self, now: Instant) {
        self.write(now, |sample| sample.seeking = false);
    }

    /// A new file is being loaded: position 0, paused state kept, duration
    /// unknown, new generation.
    pub fn reset(&self, now: Instant) {
        self.write(now, |sample| {
            sample.position = 0.0;
            sample.duration = 0.0;
            sample.seeking = false;
            sample.generation = sample.generation.wrapping_add(1);
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn secs(value: f64) -> Duration {
        Duration::from_secs_f64(value)
    }

    #[test]
    fn paused_clock_holds_its_position() {
        let clock = PlaybackClock::new();
        let t0 = Instant::now();
        clock.observe(12.5, t0);
        assert_eq!(clock.estimate(t0 + secs(3.0)), 12.5);
    }

    #[test]
    fn running_clock_interpolates_by_speed() {
        let clock = PlaybackClock::new();
        let t0 = Instant::now();
        clock.set_paused(false, t0);
        clock.observe(10.0, t0);
        assert!((clock.estimate(t0 + secs(0.5)) - 10.5).abs() < 1e-9);

        clock.set_speed(0.25, t0 + secs(1.0));
        // Rebased at 11.0 when the speed changed.
        let at = clock.estimate(t0 + secs(3.0));
        assert!((at - 11.5).abs() < 1e-9, "{at}");
    }

    #[test]
    fn pausing_freezes_the_extrapolated_position() {
        let clock = PlaybackClock::new();
        let t0 = Instant::now();
        clock.set_paused(false, t0);
        clock.observe(5.0, t0);
        clock.set_paused(true, t0 + secs(2.0));
        assert!((clock.estimate(t0 + secs(10.0)) - 7.0).abs() < 1e-9);
    }

    #[test]
    fn estimate_is_clamped_to_the_duration_and_zero() {
        let clock = PlaybackClock::new();
        let t0 = Instant::now();
        clock.set_duration(20.0, t0);
        clock.set_paused(false, t0);
        clock.observe(19.0, t0);
        assert_eq!(clock.estimate(t0 + secs(5.0)), 20.0);
        clock.observe(-3.0, t0);
        assert_eq!(clock.estimate(t0), 0.0);
    }

    #[test]
    fn seeking_holds_the_target_and_bumps_the_generation() {
        let clock = PlaybackClock::new();
        let t0 = Instant::now();
        clock.set_paused(false, t0);
        let generation = clock.sample().generation;
        clock.seek_started(42.0, t0);
        assert!(clock.is_seeking());
        assert_eq!(clock.estimate(t0 + secs(1.0)), 42.0);
        assert_eq!(clock.sample().generation, generation + 1);
        // A stale pre-seek report does not move the clock off the target.
        clock.observe(3.0, t0 + secs(0.5));
        assert_eq!(clock.estimate(t0 + secs(1.0)), 42.0);
        clock.seek_finished(t0 + secs(1.0));
        assert!((clock.estimate(t0 + secs(1.5)) - 42.5).abs() < 1e-9);
    }

    #[test]
    fn non_finite_observations_are_ignored() {
        let clock = PlaybackClock::new();
        let t0 = Instant::now();
        clock.observe(3.0, t0);
        clock.observe(f64::NAN, t0);
        clock.set_speed(f64::INFINITY, t0);
        assert_eq!(clock.estimate(t0), 3.0);
        assert_eq!(clock.sample().speed, 1.0);
    }

    #[test]
    fn concurrent_readers_see_consistent_samples() {
        let clock = PlaybackClock::new();
        let writer = clock.clone();
        let t0 = Instant::now();
        let handle = std::thread::spawn(move || {
            for step in 0..20_000 {
                let value = f64::from(step);
                // Position and duration always move together.
                writer.write(t0, |sample| {
                    sample.position = value;
                    sample.duration = value + 1.0;
                });
            }
        });
        for _ in 0..20_000 {
            let sample = clock.sample();
            if sample.duration != 0.0 {
                assert_eq!(sample.duration, sample.position + 1.0);
            }
        }
        handle.join().unwrap();
    }
}
