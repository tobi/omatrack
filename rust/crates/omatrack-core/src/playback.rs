//! Reference-video pacing and hard-seek rules (the pure half of
//! `VideoSyncController` and `TelemetryStore::referencePlaybackRate`).
//!
//! The primary recording is the clock: it plays at 1x (0.25x in slow
//! motion) and is never rate-corrected to chase telemetry. The reference
//! follows through the shared alignment map in one of three pacing modes.
//! Every mode hard-seeks the reference to the mapped station on pause, after
//! a primary jump, and when either selected lap changes during play.

use crate::num::{clamp, max, min};

/// Continuous playback keeps the playhead at this share of the viewport.
pub const CONTINUOUS_PLAYHEAD_ANCHOR: f64 = 0.33;
/// Prefetch the next lap once the cursor passes this fraction.
pub const PREFETCH_NEXT_LAP_FRACTION: f64 = 0.7;
/// Slow-motion rate of the primary clock (`S`, fullscreen only).
pub const SLOW_MOTION_RATE: f64 = 0.25;
/// Reference re-pacing tick while both videos play.
pub const REFERENCE_SYNC_INTERVAL_MS: u64 = 100;
/// Paused re-verification delay after an aligning seek.
pub const PAUSED_ALIGNMENT_INTERVAL_MS: u64 = 120;
/// Next-lap countdown tick (3-2-1).
pub const LAP_ADVANCE_INTERVAL_MS: u64 = 500;
/// A primary move larger than this between ticks is a jump: hard-seek.
pub const PRIMARY_JUMP_SECONDS: f64 = 0.5;
/// Paused alignment tolerance before re-seeking (and attempts allowed).
pub const PAUSED_RESEEK_ERROR: f64 = 0.025;
pub const PAUSED_MAX_ATTEMPTS: u32 = 3;
/// A cursor jump seeks the playing primary only beyond this error.
pub const PLAYING_SEEK_ERROR: f64 = 0.2;

/// How the reference recording is paced against the primary.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Hash)]
pub enum ReferencePlayback {
    /// Hold 1x through corners; use the straight to arrive together at the
    /// next turn-in.
    #[default]
    Corners,
    /// Follow the map's local slope continuously with a short correction.
    Gps,
    /// Both at 1x; synchronize only at lap start, jumps and pause.
    Recording,
}

impl ReferencePlayback {
    /// `video.reference_playback` value.
    pub fn key(self) -> &'static str {
        match self {
            Self::Corners => "corners",
            Self::Gps => "gps",
            Self::Recording => "recording",
        }
    }
    pub fn from_key(key: &str) -> Option<Self> {
        match key {
            "corners" => Some(Self::Corners),
            "gps" => Some(Self::Gps),
            "recording" => Some(Self::Recording),
            _ => None,
        }
    }
}

/// The reference sync indicator shown on the video chrome.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SyncState {
    Wait,
    NoMap,
    Aligning,
    Locked,
    Best,
    RealTime,
    Gps,
    Corner,
    Hold,
    Straight,
}

impl SyncState {
    pub fn label(self) -> &'static str {
        match self {
            Self::Wait => "WAIT",
            Self::NoMap => "NO MAP",
            Self::Aligning => "ALIGNING",
            Self::Locked => "LOCKED",
            Self::Best => "BEST",
            Self::RealTime => "1\u{d7}",
            Self::Gps => "GPS",
            Self::Corner => "CORNER",
            Self::Hold => "HOLD",
            Self::Straight => "STRAIGHT",
        }
    }
}

/// What the store knows at the cursor, for pacing.
pub trait PacingSource {
    /// Primary cursor lap fraction.
    fn cursor_fraction(&self) -> f64;
    /// Corner zones (start, end) in primary lap fractions, sorted.
    fn corners(&self) -> &[(f64, f64)];
    /// Primary lap-relative time at a fraction.
    fn primary_time_at(&self, fraction: f64) -> f64;
    /// Reference presentation time at a primary fraction; <= 0 unknown.
    fn reference_video_time_at(&self, fraction: f64) -> f64;
    /// Local map slope at the cursor (reference s per primary s).
    fn map_rate(&self) -> f64;
}

/// Cursor inside any corner zone (with the store's 1e-6 slack).
pub fn cursor_in_corner(cursor: f64, corners: &[(f64, f64)]) -> bool {
    corners
        .iter()
        .any(|&(start, end)| cursor + 1e-6 >= start && cursor <= end + 1e-6)
}

/// Start of the next corner after the cursor, or -1.
pub fn next_corner_start(cursor: f64, corners: &[(f64, f64)]) -> f64 {
    const EPS: f64 = 1e-4;
    corners
        .iter()
        .map(|&(start, _)| start)
        .find(|&start| start > cursor + EPS)
        .unwrap_or(-1.0)
}

/// Reference playback rate (before the primary clock rate multiplies it).
pub fn reference_rate(
    mode: ReferencePlayback,
    reference_media_time: f64,
    s: &dyn PacingSource,
) -> f64 {
    if !reference_media_time.is_finite() {
        return 1.0;
    }
    let cursor = s.cursor_fraction();
    match mode {
        ReferencePlayback::Recording => 1.0,
        ReferencePlayback::Gps => {
            // Map slope plus a short-horizon correction; no corner holds.
            const CORRECTION_SECONDS: f64 = 0.5;
            let error = s.reference_video_time_at(clamp(cursor, 0.0, 1.0)) - reference_media_time;
            clamp(s.map_rate() + error / CORRECTION_SECONDS, 0.5, 2.0)
        }
        ReferencePlayback::Corners => {
            if cursor_in_corner(cursor, s.corners()) {
                return 1.0;
            }
            const LOCK_SECONDS: f64 = 0.25;
            const MIN_RATE: f64 = 0.70;
            const MAX_RATE: f64 = 1.80;
            const UNGUIDED_HORIZON: f64 = 4.0;
            if s.corners().is_empty() {
                let error =
                    s.reference_video_time_at(clamp(cursor, 0.0, 1.0)) - reference_media_time;
                let remaining = s.primary_time_at(1.0) - s.primary_time_at(cursor);
                let horizon = min(UNGUIDED_HORIZON, max(LOCK_SECONDS, remaining));
                return clamp(1.0 + error / horizon, MIN_RATE, MAX_RATE);
            }
            let mut horizon = next_corner_start(cursor, s.corners());
            if horizon < 0.0 {
                horizon = 1.0;
            }
            let remaining_primary = s.primary_time_at(horizon) - s.primary_time_at(cursor);
            if remaining_primary < LOCK_SECONDS {
                return 1.0;
            }
            let arrive = s.reference_video_time_at(horizon);
            if arrive <= 0.0 {
                return 1.0;
            }
            let remaining_reference = arrive - reference_media_time;
            if remaining_reference <= 0.0 {
                return MIN_RATE;
            }
            clamp(remaining_reference / remaining_primary, MIN_RATE, MAX_RATE)
        }
    }
}

/// One reference-sync decision.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum SyncAction {
    /// Nothing to do yet (reference not loaded).
    Wait,
    /// No mapped station: play the reference at 1x.
    NoMap,
    /// Seek the reference to `target` and play at `rate`; when paused,
    /// schedule a paused re-verification.
    HardSeek {
        target: f64,
        rate: f64,
        state: SyncState,
    },
    /// Keep playing at `rate`.
    Pace { rate: f64, state: SyncState },
}

/// Inputs to [`sync_reference`].
#[derive(Debug, Clone, Copy)]
pub struct SyncInput {
    pub reference_loaded: bool,
    /// Reference presentation time at the primary cursor; <= 0 unknown.
    pub target: f64,
    pub reference_position: f64,
    pub primary_paused: bool,
    /// Primary movement since the last sync decision.
    pub primary_delta: f64,
    /// 1.0, or the slow-motion rate.
    pub primary_clock_rate: f64,
    pub force: bool,
    pub mode: ReferencePlayback,
    pub cursor_in_corner: bool,
    /// [`reference_rate`] for the current reference position.
    pub paced_rate: f64,
}

/// `VideoSyncController::syncReferenceVideo` as a pure decision.
pub fn sync_reference(input: SyncInput) -> SyncAction {
    if !input.reference_loaded {
        return SyncAction::Wait;
    }
    if input.target <= 0.0 {
        return SyncAction::NoMap;
    }
    let error = input.target - input.reference_position;
    if input.force || input.primary_paused || input.primary_delta.abs() > PRIMARY_JUMP_SECONDS {
        return SyncAction::HardSeek {
            target: input.target,
            rate: if input.primary_paused {
                1.0
            } else {
                input.primary_clock_rate
            },
            state: if input.primary_paused {
                SyncState::Aligning
            } else {
                SyncState::Locked
            },
        };
    }
    if input.mode == ReferencePlayback::Recording {
        return SyncAction::Pace {
            rate: input.primary_clock_rate,
            state: SyncState::RealTime,
        };
    }
    let rate = input.paced_rate * input.primary_clock_rate;
    let near = error.abs() < 0.08;
    let state = match input.mode {
        ReferencePlayback::Gps => {
            if near {
                SyncState::Locked
            } else {
                SyncState::Gps
            }
        }
        _ if input.cursor_in_corner => {
            if near {
                SyncState::Corner
            } else {
                SyncState::Hold
            }
        }
        _ => {
            if near {
                SyncState::Locked
            } else {
                SyncState::Straight
            }
        }
    };
    SyncAction::Pace { rate, state }
}

/// After a paused aligning seek lands: re-seek (`Some(target)`) or settle.
pub fn verify_paused(
    target: f64,
    reference_position: f64,
    attempts: u32,
) -> Result<SyncState, f64> {
    if target <= 0.0 {
        return Ok(SyncState::NoMap);
    }
    let error = target - reference_position;
    if error.abs() > PAUSED_RESEEK_ERROR && attempts < PAUSED_MAX_ATTEMPTS {
        return Err(target);
    }
    Ok(if error.abs() <= 0.05 {
        SyncState::Locked
    } else {
        SyncState::Best
    })
}

/// Whether a cursor move should seek the primary player.
pub fn primary_needs_seek(position: f64, target: f64, paused: bool) -> bool {
    let error = (position - target).abs();
    if paused {
        error > PAUSED_RESEEK_ERROR
    } else {
        // Playing: the file is the clock; only an explicit jump seeks.
        error > PLAYING_SEEK_ERROR
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn jumps_and_pause_hard_seek() {
        let input = SyncInput {
            reference_loaded: true,
            target: 12.0,
            reference_position: 11.0,
            primary_paused: false,
            primary_delta: 0.9,
            primary_clock_rate: 1.0,
            force: false,
            mode: ReferencePlayback::Corners,
            cursor_in_corner: false,
            paced_rate: 1.1,
        };
        assert!(matches!(sync_reference(input), SyncAction::HardSeek { .. }));
        let steady = SyncInput {
            primary_delta: 0.1,
            ..input
        };
        assert_eq!(
            sync_reference(steady),
            SyncAction::Pace {
                rate: 1.1,
                state: SyncState::Straight
            }
        );
    }
}
