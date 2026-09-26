//! One lap's map between video presentation time, the recording's
//! telemetry time and the normalized lap fraction.
//!
//! Telemetry time is file-relative seconds; the unified lap's `time[i] +
//! start_time` is on that clock. Presentation time is the player's media
//! time. The recording's [`VideoClock`] is the only conversion between the
//! two (`presentation = telemetry + per-file offset`); nothing here guesses
//! from frame rates or lap endpoints.

use std::sync::Arc;

use omatrack_core::VideoClock;
use omatrack_core::monotonic;
use omatrack_core::session::{LoadedLap, VideoBinding};
use omatrack_core::unify::UnifiedLap;

/// How a player's media time relates to the recording's telemetry time.
#[derive(Clone, Debug, PartialEq)]
pub enum VideoMap {
    /// The recording's own video clock (catalog offsets, per file).
    Clock {
        clock: Arc<VideoClock>,
        file_index: Option<u32>,
    },
    /// `presentation = telemetry + offset` seconds: an external clock whose
    /// owner knows the relation (a test's mock clock, a host-owned player).
    Offset(f64),
}

impl VideoMap {
    /// The mapping of a lap's bound video.
    pub fn from_binding(binding: &VideoBinding) -> Self {
        Self::Clock {
            clock: binding.clock.clone(),
            file_index: binding.file_index,
        }
    }

    /// Presentation time (s) of file-relative telemetry time (s).
    pub fn presentation_at(&self, telemetry: f64) -> Option<f64> {
        if !telemetry.is_finite() {
            return None;
        }
        match self {
            Self::Offset(offset) => Some(telemetry + offset),
            Self::Clock { clock, file_index } => {
                let nanoseconds = (telemetry.max(0.0) * 1e9).round() as u64;
                clock
                    .presentation_time_ns(nanoseconds, *file_index)
                    .map(|ns| ns as f64 / 1e9)
            }
        }
    }

    /// File-relative telemetry time (s) of a presentation time (s).
    pub fn telemetry_at(&self, presentation: f64) -> Option<f64> {
        if !presentation.is_finite() {
            return None;
        }
        match self {
            Self::Offset(offset) => Some(presentation - offset),
            Self::Clock { clock, file_index } => {
                let nanoseconds = (presentation.max(0.0) * 1e9).round() as u64;
                clock
                    .telemetry_time_ns(nanoseconds, *file_index)
                    .map(|ns| ns as f64 / 1e9)
            }
        }
    }
}

/// Where a video time falls relative to one lap.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct LapPosition {
    /// Lap fraction, clamped to `0..=1`.
    pub fraction: f64,
    /// Lap time on the unified lap's time axis (its first sample is the lap
    /// start, normally 0); unclamped, so negative before the lap and past
    /// the last sample after it.
    pub lap_time: f64,
}

/// The time map of one loaded lap and its video.
#[derive(Clone, Debug)]
pub struct LapTimeline {
    lap_id: i32,
    unified: Arc<UnifiedLap>,
    map: VideoMap,
}

impl LapTimeline {
    /// The timeline of `lap` through `map` (usually
    /// [`VideoMap::from_binding`] of the lap's own video).
    pub fn new(lap: &LoadedLap, map: VideoMap) -> Self {
        Self::from_unified(lap.lap_id(), lap.unified().clone(), map)
    }

    /// The timeline of a lap's unified samples through `map`.
    pub fn from_unified(lap_id: i32, unified: Arc<UnifiedLap>, map: VideoMap) -> Self {
        Self {
            lap_id,
            unified,
            map,
        }
    }

    pub fn lap_id(&self) -> i32 {
        self.lap_id
    }

    pub fn unified(&self) -> &Arc<UnifiedLap> {
        &self.unified
    }

    pub fn map(&self) -> &VideoMap {
        &self.map
    }

    /// Lap length in seconds (0 for an empty lap).
    pub fn duration(&self) -> f64 {
        match (self.unified.time.first(), self.unified.time.last()) {
            (Some(first), Some(last)) => (last - first).max(0.0),
            _ => 0.0,
        }
    }

    fn has_samples(&self) -> bool {
        self.unified.time.len() >= 2
    }

    /// Lap-relative time (s) at a lap fraction.
    pub fn lap_time_at(&self, fraction: f64) -> f64 {
        monotonic::interpolate_fraction(&self.unified.time, fraction)
    }

    /// Lap fraction at lap-relative time (s), clamped.
    pub fn fraction_at_lap_time(&self, lap_time: f64) -> f64 {
        monotonic::invert_fraction(&self.unified.time, lap_time)
    }

    /// Where video time `presentation` falls in the lap; `None` when the
    /// video clock cannot map it (or the lap has no samples).
    pub fn position_at_video(&self, presentation: f64) -> Option<LapPosition> {
        if !self.has_samples() {
            return None;
        }
        let telemetry = self.map.telemetry_at(presentation)?;
        let lap_time = telemetry - self.unified.start_time;
        Some(LapPosition {
            fraction: self.fraction_at_lap_time(lap_time),
            lap_time,
        })
    }

    /// Lap fraction shown at video time `presentation`, clamped.
    pub fn fraction_at_video(&self, presentation: f64) -> Option<f64> {
        self.position_at_video(presentation)
            .map(|position| position.fraction)
    }

    /// Video time of lap-relative time (s), the time axis of the unified
    /// lap (and of `Comparison::compare_time_for_primary_fraction`).
    pub fn video_at_lap_time(&self, lap_time: f64) -> Option<f64> {
        if !self.has_samples() || !lap_time.is_finite() {
            return None;
        }
        self.map.presentation_at(self.unified.start_time + lap_time)
    }

    /// Video time of a lap fraction.
    pub fn video_at_fraction(&self, fraction: f64) -> Option<f64> {
        if !self.has_samples() || !fraction.is_finite() {
            return None;
        }
        self.video_at_lap_time(self.lap_time_at(fraction.clamp(0.0, 1.0)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lap(seconds: f64, start: f64) -> Arc<UnifiedLap> {
        let count = (seconds * 50.0) as usize + 1;
        Arc::new(UnifiedLap {
            start_time: start,
            time: (0..count).map(|i| i as f64 / 50.0).collect(),
            ..UnifiedLap::default()
        })
    }

    #[test]
    fn offset_map_round_trips_through_the_lap() {
        let timeline = LapTimeline::from_unified(3, lap(60.0, 100.0), VideoMap::Offset(2.5));
        // Lap start: telemetry 100 s, video 102.5 s.
        assert_eq!(timeline.video_at_fraction(0.0), Some(102.5));
        let mid = timeline.video_at_fraction(0.5).unwrap();
        assert!((mid - 132.5).abs() < 1e-9, "{mid}");
        let back = timeline.fraction_at_video(mid).unwrap();
        assert!((back - 0.5).abs() < 1e-9, "{back}");
        let after = timeline.position_at_video(170.0).unwrap();
        assert_eq!(after.fraction, 1.0);
        assert!((after.lap_time - 67.5).abs() < 1e-9);
        let before = timeline.position_at_video(90.0).unwrap();
        assert_eq!(before.fraction, 0.0);
        assert!(before.lap_time < 0.0);
    }

    #[test]
    fn video_clock_map_applies_the_file_offset() {
        let clock = VideoClock {
            presentation_offset_ns: Some(1_500_000_000),
            presentation_times_ns: vec![0],
            files: Vec::new(),
        };
        let map = VideoMap::Clock {
            clock: Arc::new(clock),
            file_index: None,
        };
        assert_eq!(map.presentation_at(10.0), Some(11.5));
        assert_eq!(map.telemetry_at(11.5), Some(10.0));
        let empty = VideoMap::Clock {
            clock: Arc::new(VideoClock::default()),
            file_index: None,
        };
        assert_eq!(empty.presentation_at(10.0), None, "no offset, no mapping");
    }
}
