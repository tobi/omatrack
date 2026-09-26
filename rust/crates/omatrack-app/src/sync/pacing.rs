//! The reference follower: `omatrack_core::playback` decides where the
//! reference recording should be and how fast it should run;
//! [`mpv_player::Follower`] turns that into player commands.
//!
//! The primary recording is the clock and is never paced. The reference's
//! target is the one shared map (invariants 5 and 6):
//! `Comparison::compare_time_for_primary_fraction` at the primary cursor,
//! through the reference lap's video clock.

use std::sync::Arc;

use mpv_player::{FollowAction, FollowPolicy, Follower, PlaybackClock};
use omatrack_core::Comparison;
use omatrack_core::playback::{
    self, PacingSource, ReferencePlayback, SyncAction, SyncInput, SyncState,
};

use super::timeline::LapTimeline;

/// A running reference further than this from its mapped station (s) is
/// hard-sought even between jumps (`corners` and `gps` pacing; `recording`
/// deliberately drifts until the next lap start, jump or pause).
pub const REFERENCE_DRIFT_SECONDS: f64 = 1.5;
/// The reference picture renders at most this many frames per second.
pub const REFERENCE_MAX_FPS: f64 = 30.0;

/// The shared map of one lap pair: both timelines and their comparison.
#[derive(Clone, Debug)]
pub struct PairMap {
    primary: LapTimeline,
    reference: LapTimeline,
    comparison: Arc<Comparison>,
    /// Corner zones (start, end) in primary lap fraction, sorted.
    corners: Vec<(f64, f64)>,
}

impl PairMap {
    pub fn new(
        primary: LapTimeline,
        reference: LapTimeline,
        comparison: Arc<Comparison>,
        mut corners: Vec<(f64, f64)>,
    ) -> Self {
        corners.sort_by(|a, b| a.0.total_cmp(&b.0));
        Self {
            primary,
            reference,
            comparison,
            corners,
        }
    }

    pub fn primary(&self) -> &LapTimeline {
        &self.primary
    }

    pub fn reference(&self) -> &LapTimeline {
        &self.reference
    }

    pub fn comparison(&self) -> &Arc<Comparison> {
        &self.comparison
    }

    pub fn corners(&self) -> &[(f64, f64)] {
        &self.corners
    }

    /// The reference lap time (s) at a primary fraction, straight from the
    /// comparison; `None` without an alignment map.
    pub fn reference_lap_time(&self, fraction: f64) -> Option<f64> {
        let time = self.comparison.compare_time_for_primary_fraction(fraction);
        (time >= 0.0 && time.is_finite()).then_some(time)
    }

    /// Reference presentation time (s) at a primary fraction.
    pub fn reference_target(&self, fraction: f64) -> Option<f64> {
        self.reference
            .video_at_lap_time(self.reference_lap_time(fraction)?)
    }

    fn source(&self, cursor: f64) -> PairSource<'_> {
        PairSource { map: self, cursor }
    }
}

/// [`PacingSource`] over a [`PairMap`] at one cursor.
struct PairSource<'a> {
    map: &'a PairMap,
    cursor: f64,
}

impl PacingSource for PairSource<'_> {
    fn cursor_fraction(&self) -> f64 {
        self.cursor
    }

    fn corners(&self) -> &[(f64, f64)] {
        &self.map.corners
    }

    fn primary_time_at(&self, fraction: f64) -> f64 {
        self.map.primary.lap_time_at(fraction)
    }

    fn reference_video_time_at(&self, fraction: f64) -> f64 {
        self.map.reference_target(fraction).unwrap_or(-1.0)
    }

    fn map_rate(&self) -> f64 {
        self.map.comparison.video_rate_at(self.cursor)
    }
}

/// One pacing decision's inputs.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct PacerInput {
    /// Primary cursor, lap fraction.
    pub cursor: f64,
    /// Primary media time now (s).
    pub primary_position: f64,
    pub primary_paused: bool,
    /// 1.0, or the slow-motion rate.
    pub clock_rate: f64,
    /// Reference media time now (s).
    pub reference_position: f64,
    /// A jump, lap change or pair change: hard-seek whatever the error.
    pub force: bool,
}

impl PacerInput {
    pub fn new(cursor: f64, primary_position: f64, reference_position: f64) -> Self {
        Self {
            cursor,
            primary_position,
            primary_paused: false,
            clock_rate: 1.0,
            reference_position,
            force: false,
        }
    }

    pub fn paused(mut self, paused: bool) -> Self {
        self.primary_paused = paused;
        self
    }

    pub fn clock_rate(mut self, rate: f64) -> Self {
        self.clock_rate = rate;
        self
    }

    pub fn force(mut self, force: bool) -> Self {
        self.force = force;
        self
    }
}

/// What the reference player should do.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum PacerCommand {
    /// Nothing changes.
    None,
    /// Play at this speed.
    SetSpeed(f64),
    /// Exact-seek to `target` and play at `rate`. With `verify` (the
    /// primary is paused) re-check the landing after
    /// `PAUSED_ALIGNMENT_INTERVAL_MS`.
    Seek {
        target: f64,
        rate: f64,
        verify: bool,
    },
}

/// Paces the reference recording against the primary clock.
#[derive(Clone, Debug)]
pub struct ReferencePacer {
    follower: Follower,
    mode: ReferencePlayback,
    last_primary: Option<f64>,
    state: SyncState,
    error: f64,
    paused_attempts: u32,
}

impl ReferencePacer {
    /// A pacer for the reference player owning `clock`.
    pub fn new(clock: PlaybackClock, mode: ReferencePlayback) -> Self {
        Self {
            follower: Follower::new(clock),
            mode,
            last_primary: None,
            state: SyncState::Wait,
            error: 0.0,
            paused_attempts: 0,
        }
    }

    pub fn mode(&self) -> ReferencePlayback {
        self.mode
    }

    pub fn set_mode(&mut self, mode: ReferencePlayback) {
        self.mode = mode;
    }

    /// The indicator for the video chrome.
    pub fn state(&self) -> SyncState {
        self.state
    }

    /// Target minus reference position at the last decision (s).
    pub fn error(&self) -> f64 {
        self.error
    }

    /// Forget the jump baseline (a new pair or player).
    pub fn reset(&mut self) {
        self.last_primary = None;
        self.state = SyncState::Wait;
        self.error = 0.0;
        self.paused_attempts = 0;
    }

    /// The reference was muted, paused or slowed outside the pacer.
    pub fn set_commanded_rate(&mut self, rate: f64) {
        self.follower.set_commanded_rate(rate);
    }

    /// Mechanics for one pacing mode: speed passes through, the drift
    /// threshold hard-seeks; the core's rate already carries its own
    /// correction, so the follower adds none.
    fn policy(mode: ReferencePlayback) -> FollowPolicy {
        let threshold = match mode {
            ReferencePlayback::Recording => f64::INFINITY,
            ReferencePlayback::Corners | ReferencePlayback::Gps => REFERENCE_DRIFT_SECONDS,
        };
        FollowPolicy::default()
            .hard_seek_threshold(threshold)
            .correction_gain(0.0)
            .max_rate_delta(0.0)
            .deadband(0.0)
    }

    /// One decision (`VideoSyncController::syncReferenceVideo`).
    pub fn step(&mut self, map: &PairMap, input: PacerInput) -> PacerCommand {
        let cursor = input.cursor.clamp(0.0, 1.0);
        let target = map.reference_target(cursor).unwrap_or(-1.0);
        let delta = self
            .last_primary
            .map_or(0.0, |last| input.primary_position - last);
        self.last_primary = Some(input.primary_position);
        let source = map.source(cursor);
        let paced_rate = playback::reference_rate(self.mode, input.reference_position, &source);
        let action = playback::sync_reference(SyncInput {
            reference_loaded: true,
            target,
            reference_position: input.reference_position,
            primary_paused: input.primary_paused,
            primary_delta: delta,
            primary_clock_rate: input.clock_rate,
            force: input.force,
            mode: self.mode,
            cursor_in_corner: playback::cursor_in_corner(cursor, map.corners()),
            paced_rate,
        });
        match action {
            SyncAction::Wait => {
                self.state = SyncState::Wait;
                PacerCommand::None
            }
            SyncAction::NoMap => {
                self.state = SyncState::NoMap;
                self.error = 0.0;
                self.speed(input.clock_rate)
            }
            SyncAction::HardSeek {
                target,
                rate,
                state,
            } => {
                self.state = state;
                self.error = 0.0;
                self.follower.set_commanded_rate(rate);
                self.paused_attempts = u32::from(input.primary_paused);
                PacerCommand::Seek {
                    target,
                    rate,
                    verify: input.primary_paused,
                }
            }
            SyncAction::Pace { rate, state } => {
                self.state = state;
                self.error = target - input.reference_position;
                let policy = Self::policy(self.mode);
                match self
                    .follower
                    .step_from(input.reference_position, target, rate, &policy)
                {
                    FollowAction::None => PacerCommand::None,
                    FollowAction::SetSpeed(speed) => PacerCommand::SetSpeed(speed),
                    FollowAction::HardSeek(target) => {
                        // Drifted past the threshold: re-lock at the station.
                        self.state = SyncState::Locked;
                        self.error = 0.0;
                        let rate = input.clock_rate;
                        self.follower.set_commanded_rate(rate);
                        PacerCommand::Seek {
                            target,
                            rate,
                            verify: false,
                        }
                    }
                }
            }
        }
    }

    fn speed(&mut self, rate: f64) -> PacerCommand {
        if (self.follower.commanded_rate() - rate).abs() < 1e-9 {
            return PacerCommand::None;
        }
        self.follower.set_commanded_rate(rate);
        PacerCommand::SetSpeed(rate)
    }

    /// After a paused aligning seek landed: the target to seek again, or
    /// `None` once settled (`verify_paused`).
    pub fn verify_paused(
        &mut self,
        map: &PairMap,
        cursor: f64,
        reference_position: f64,
    ) -> Option<f64> {
        let target = map.reference_target(cursor.clamp(0.0, 1.0)).unwrap_or(-1.0);
        self.error = if target > 0.0 {
            target - reference_position
        } else {
            0.0
        };
        match playback::verify_paused(target, reference_position, self.paused_attempts) {
            Err(target) => {
                self.paused_attempts += 1;
                self.state = SyncState::Aligning;
                Some(target)
            }
            Ok(state) => {
                self.paused_attempts = 0;
                self.state = state;
                None
            }
        }
    }
}
