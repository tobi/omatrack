//! What happens when the primary video runs past the end of its lap.
//!
//! - **Per lap** (default): pause, count 3-2-1 on the video, select the next
//!   lap of the same recording, resume from its start. The reference lap is
//!   unchanged.
//! - **Continuous** (`video.continuous_playback`): no countdown; the next lap
//!   is adopted at the current video position (the playing video is never
//!   sought), the trace viewport keeps the playhead at 33% of its width, and
//!   the next lap is prefetched once the cursor passes 70%.

use omatrack_core::playback::{CONTINUOUS_PLAYHEAD_ANCHOR, PREFETCH_NEXT_LAP_FRACTION};
use omatrack_trace::Viewport;

use super::timeline::LapTimeline;

/// The cursor counts as at the lap end from this fraction on.
pub const LAP_END_FRACTION: f64 = 0.999;
/// The countdown starts at this number.
pub const COUNTDOWN_FROM: u8 = 3;

/// The per-lap advance: a 3-2-1 countdown, then a wait for the next lap to
/// load before playback resumes.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum LapAdvance {
    #[default]
    Idle,
    /// Counting down to `next_lap`; `remaining` is the number shown.
    Counting { remaining: u8, next_lap: i32 },
    /// `next_lap` was selected; play resumes once it is bound.
    Resuming { next_lap: i32 },
}

impl LapAdvance {
    /// Start counting down to `next_lap`.
    pub fn start(next_lap: i32) -> Self {
        Self::Counting {
            remaining: COUNTDOWN_FROM,
            next_lap,
        }
    }

    /// The number on screen while counting.
    pub fn countdown(&self) -> Option<u8> {
        match self {
            Self::Counting { remaining, .. } => Some(*remaining),
            _ => None,
        }
    }

    /// The lap being advanced to.
    pub fn next_lap(&self) -> Option<i32> {
        match self {
            Self::Counting { next_lap, .. } | Self::Resuming { next_lap } => Some(*next_lap),
            Self::Idle => None,
        }
    }

    pub fn is_counting(&self) -> bool {
        matches!(self, Self::Counting { .. })
    }

    /// One countdown tick. After "1" the countdown ends: the lap to select
    /// is returned and the advance waits in [`LapAdvance::Resuming`].
    pub fn tick(&mut self) -> Option<i32> {
        let Self::Counting {
            remaining,
            next_lap,
        } = *self
        else {
            return None;
        };
        if remaining <= 1 {
            *self = Self::Resuming { next_lap };
            return Some(next_lap);
        }
        *self = Self::Counting {
            remaining: remaining - 1,
            next_lap,
        };
        None
    }

    /// The lap `lap_id` is bound: true (and idle again) when it is the one
    /// being resumed into.
    pub fn resume_into(&mut self, lap_id: i32) -> bool {
        if *self == (Self::Resuming { next_lap: lap_id }) {
            *self = Self::Idle;
            return true;
        }
        false
    }

    pub fn cancel(&mut self) {
        *self = Self::Idle;
    }
}

/// What the end of the lap asks for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LapEnd {
    /// Continuous playback: adopt `next_lap` at the current position.
    Adopt { next_lap: i32 },
    /// Per-lap playback: pause and count down to `next_lap`.
    Countdown { next_lap: i32 },
    /// No next lap: per-lap playback pauses at the end, continuous
    /// playback keeps running with the cursor at the lap end.
    Stop,
}

/// Decide the lap-end transition.
pub fn lap_end(continuous: bool, next_lap: Option<i32>) -> LapEnd {
    match (next_lap, continuous) {
        (Some(next_lap), true) => LapEnd::Adopt { next_lap },
        (Some(next_lap), false) => LapEnd::Countdown { next_lap },
        (None, _) => LapEnd::Stop,
    }
}

/// Whether `fraction` crossed into the lap end since `previous`.
pub fn reached_lap_end(previous: Option<f64>, fraction: f64) -> bool {
    fraction >= LAP_END_FRACTION && previous.is_none_or(|previous| previous < LAP_END_FRACTION)
}

/// Whether continuous playback should prefetch the next lap.
pub fn should_prefetch(fraction: f64) -> bool {
    fraction > PREFETCH_NEXT_LAP_FRACTION
}

/// The cursor of the adopted lap: the current video time on its timeline.
/// `None` when that lap's clock cannot map the time.
pub fn adopted_cursor(next: &LapTimeline, video_time: f64) -> Option<f64> {
    next.fraction_at_video(video_time)
}

/// The viewport that keeps `cursor` at [`CONTINUOUS_PLAYHEAD_ANCHOR`] of its
/// width, same span. Deliberately unclamped: across start/finish it runs
/// into the neighbouring lap instead of pinning the lap.
pub fn follow_viewport(viewport: Viewport, cursor: f64) -> Viewport {
    let span = viewport.span();
    let start = cursor - CONTINUOUS_PLAYHEAD_ANCHOR * span;
    Viewport::new(start, start + span)
}
