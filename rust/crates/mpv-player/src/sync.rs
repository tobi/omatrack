//! Generic follow mechanics: keep one player's clock on a moving target.
//!
//! The application decides *where* a follower should be (for Omatrack, the
//! reference recording's station mapped from the primary lap, and the local
//! map slope as the target rate). [`Follower`] only decides *how* to get
//! there: nudge the playback speed for small errors, hard-seek for large
//! ones, and stay quiet when already aligned.

use std::time::Instant;

use crate::clock::PlaybackClock;

/// Tuning for [`Follower::step`].
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct FollowPolicy {
    /// Errors larger than this many seconds hard-seek instead of correcting
    /// with speed.
    pub hard_seek_threshold: f64,
    /// Largest speed correction added to (or subtracted from) the target
    /// rate.
    pub max_rate_delta: f64,
    /// Speed correction per second of error (proportional gain).
    pub correction_gain: f64,
    /// Errors within this many seconds count as aligned: no correction.
    pub deadband: f64,
    /// Speed changes smaller than this are not sent to the player.
    pub rate_epsilon: f64,
    /// Lowest speed the follower is ever asked to play at.
    pub min_rate: f64,
}

impl Default for FollowPolicy {
    fn default() -> Self {
        Self {
            hard_seek_threshold: 0.5,
            max_rate_delta: 0.25,
            correction_gain: 0.8,
            deadband: 1.0 / 60.0,
            rate_epsilon: 0.005,
            min_rate: 0.05,
        }
    }
}

impl FollowPolicy {
    /// Sets [`FollowPolicy::hard_seek_threshold`].
    #[must_use]
    pub fn hard_seek_threshold(mut self, seconds: f64) -> Self {
        self.hard_seek_threshold = seconds;
        self
    }

    /// Sets [`FollowPolicy::max_rate_delta`].
    #[must_use]
    pub fn max_rate_delta(mut self, delta: f64) -> Self {
        self.max_rate_delta = delta;
        self
    }

    /// Sets [`FollowPolicy::correction_gain`].
    #[must_use]
    pub fn correction_gain(mut self, gain: f64) -> Self {
        self.correction_gain = gain;
        self
    }

    /// Sets [`FollowPolicy::deadband`].
    #[must_use]
    pub fn deadband(mut self, seconds: f64) -> Self {
        self.deadband = seconds;
        self
    }
}

/// What the follower's player should do now.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum FollowAction {
    /// Nothing: aligned, or waiting for a seek to land.
    None,
    /// Change the playback speed.
    SetSpeed(f64),
    /// Exact-seek to this media time.
    HardSeek(f64),
}

/// Keeps one player (the follower) on a target position and rate.
///
/// Call [`Follower::step`] once per frame with the target, then apply the
/// action (for example with [`Player::apply`](crate::Player::apply)).
#[derive(Clone, Debug)]
pub struct Follower {
    clock: PlaybackClock,
    /// The speed last requested from the follower's player.
    commanded_rate: f64,
}

impl Follower {
    /// A follower of the player owning `clock`, assumed to play at 1×.
    pub fn new(clock: PlaybackClock) -> Self {
        Self {
            clock,
            commanded_rate: 1.0,
        }
    }

    /// The speed last requested through a [`FollowAction::SetSpeed`].
    pub fn commanded_rate(&self) -> f64 {
        self.commanded_rate
    }

    /// Decides the next action from the follower clock's current estimate.
    ///
    /// `target_rate <= 0` means the target is holding still (paused): the
    /// follower then only hard-seeks when it is outside the deadband.
    pub fn step(
        &mut self,
        target_secs: f64,
        target_rate: f64,
        policy: &FollowPolicy,
    ) -> FollowAction {
        let sample = self.clock.sample();
        if sample.seeking {
            // Let the in-flight seek land before judging the error again.
            return FollowAction::None;
        }
        let position = sample.estimate(Instant::now());
        self.step_from(position, target_secs, target_rate, policy)
    }

    /// [`Follower::step`] with an explicit follower position.
    pub fn step_from(
        &mut self,
        position: f64,
        target_secs: f64,
        target_rate: f64,
        policy: &FollowPolicy,
    ) -> FollowAction {
        if !position.is_finite() || !target_secs.is_finite() || !target_rate.is_finite() {
            return FollowAction::None;
        }
        let error = target_secs - position;

        if target_rate <= 0.0 {
            return if error.abs() > policy.deadband {
                FollowAction::HardSeek(target_secs.max(0.0))
            } else {
                FollowAction::None
            };
        }

        if error.abs() > policy.hard_seek_threshold {
            return self.hard_seek_now(target_secs);
        }

        let correction = if error.abs() <= policy.deadband {
            0.0
        } else {
            (error * policy.correction_gain).clamp(-policy.max_rate_delta, policy.max_rate_delta)
        };
        let desired = (target_rate + correction).max(policy.min_rate);
        if (desired - self.commanded_rate).abs() < policy.rate_epsilon {
            return FollowAction::None;
        }
        self.commanded_rate = desired;
        FollowAction::SetSpeed(desired)
    }

    /// Forces a hard seek to `target_secs` (for example after the leader
    /// jumped). The follower's commanded rate is kept.
    pub fn hard_seek_now(&mut self, target_secs: f64) -> FollowAction {
        FollowAction::HardSeek(target_secs.max(0.0))
    }

    /// Records that the follower's speed was set outside the follower (for
    /// example reset to 1× on pause), so the next step compares against it.
    pub fn set_commanded_rate(&mut self, rate: f64) {
        if rate.is_finite() && rate > 0.0 {
            self.commanded_rate = rate;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn follower() -> Follower {
        Follower::new(PlaybackClock::new())
    }

    #[test]
    fn aligned_follower_does_nothing() {
        let mut follower = follower();
        let policy = FollowPolicy::default();
        assert_eq!(
            follower.step_from(10.0, 10.005, 1.0, &policy),
            FollowAction::None
        );
    }

    #[test]
    fn small_errors_correct_with_speed_within_the_limit() {
        let mut follower = follower();
        let policy = FollowPolicy::default();
        // 0.1 s behind: speed up by gain × error.
        match follower.step_from(10.0, 10.1, 1.0, &policy) {
            FollowAction::SetSpeed(speed) => assert!((speed - 1.08).abs() < 1e-9, "{speed}"),
            other => panic!("{other:?}"),
        }
        // 0.45 s ahead (just inside the seek threshold): clamp the slowdown.
        match follower.step_from(10.45, 10.0, 1.0, &policy) {
            FollowAction::SetSpeed(speed) => assert!((speed - 0.75).abs() < 1e-9, "{speed}"),
            other => panic!("{other:?}"),
        }
        // The same request again is not re-sent.
        assert_eq!(
            follower.step_from(10.45, 10.0, 1.0, &policy),
            FollowAction::None
        );
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn target_rate_carries_through_when_aligned() {
        let mut follower = follower();
        let policy = FollowPolicy::default();
        assert_eq!(
            follower.step_from(5.0, 5.0, 1.2, &policy),
            FollowAction::SetSpeed(1.2)
        );
        assert_eq!(follower.commanded_rate(), 1.2);
        assert_eq!(
            follower.step_from(5.0, 5.0, 1.201, &policy),
            FollowAction::None
        );
    }

    #[test]
    fn large_errors_hard_seek_on_both_sides_of_the_threshold() {
        let mut follower = follower();
        let policy = FollowPolicy::default().hard_seek_threshold(0.5);
        assert_eq!(
            follower.step_from(10.0, 10.6, 1.0, &policy),
            FollowAction::HardSeek(10.6)
        );
        assert_eq!(
            follower.step_from(10.6, 10.0, 1.0, &policy),
            FollowAction::HardSeek(10.0)
        );
        assert!(matches!(
            follower.step_from(10.0, 10.49, 1.0, &policy),
            FollowAction::SetSpeed(_)
        ));
        assert_eq!(follower.hard_seek_now(-1.0), FollowAction::HardSeek(0.0));
    }

    #[test]
    fn a_paused_target_only_seeks_outside_the_deadband() {
        let mut follower = follower();
        let policy = FollowPolicy::default();
        assert_eq!(
            follower.step_from(3.0, 3.01, 0.0, &policy),
            FollowAction::None
        );
        assert_eq!(
            follower.step_from(3.0, 3.2, 0.0, &policy),
            FollowAction::HardSeek(3.2)
        );
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn speed_never_drops_below_the_minimum() {
        let mut follower = follower();
        let policy = FollowPolicy::default()
            .max_rate_delta(1.0)
            .correction_gain(10.0);
        match follower.step_from(10.4, 10.0, 0.1, &policy) {
            FollowAction::SetSpeed(speed) => assert_eq!(speed, policy.min_rate),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn step_waits_while_the_follower_is_seeking() {
        let clock = PlaybackClock::new();
        let mut follower = Follower::new(clock.clone());
        clock.seek_started(1.0, Instant::now());
        assert_eq!(
            follower.step(50.0, 1.0, &FollowPolicy::default()),
            FollowAction::None
        );
        clock.seek_finished(Instant::now());
        assert_eq!(
            follower.step(50.0, 1.0, &FollowPolicy::default()),
            FollowAction::HardSeek(50.0)
        );
    }

    #[test]
    fn non_finite_inputs_are_ignored() {
        let mut follower = follower();
        let policy = FollowPolicy::default();
        assert_eq!(
            follower.step_from(f64::NAN, 1.0, 1.0, &policy),
            FollowAction::None
        );
        assert_eq!(
            follower.step_from(1.0, f64::INFINITY, 1.0, &policy),
            FollowAction::None
        );
    }
}
