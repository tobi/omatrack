//! The one primary -> reference map every surface consumes.
//!
//! Traces, the delta lane, cursor readouts, corner notes and synchronized
//! video must derive from the same cached alignment and the same manual
//! offset (invariants 5 and 6), so they can never disagree. `Comparison`
//! owns that map for one lap pair and strategy, plus the cached cumulative
//! delta. Ports the `TelemetryStore` functions `deltaTrace`,
//! `compareTimeForPrimaryFraction`, `compareFractionForPrimaryFraction`,
//! `primaryFractionForCompareFraction`, `cursorTimeDelta`,
//! `cursorSpeedDelta`, `comparisonVideoRate` and
//! `effectiveComparisonSyncStrategy`.

use crate::alignment::{self, AlignmentResult, Options, Strategy};
use crate::monotonic;
use crate::num::{clamp, llround};
use crate::unify::UnifiedLap;
use std::sync::Arc;

/// Strategies both laps can support, in menu order.
pub fn available_strategies(
    primary: &UnifiedLap,
    reference: &UnifiedLap,
    has_corners: bool,
) -> Vec<Strategy> {
    let mut out = Vec::new();
    if alignment::gps_available(primary, reference) {
        out.push(Strategy::Gps);
    }
    let dampers = alignment::damper_available(primary, reference);
    if dampers && has_corners {
        out.push(Strategy::PreCornerDampers);
    }
    if dampers {
        out.push(Strategy::ManualDampers);
    }
    out.push(Strategy::LapPercentage);
    out
}

/// The requested strategy when both laps support it, else the automatic
/// choice: GPS, then pre-corner dampers over a lap-time base, then lap %.
pub fn effective_strategy(
    requested: Option<Strategy>,
    primary: &UnifiedLap,
    reference: &UnifiedLap,
    has_corners: bool,
) -> Strategy {
    let available = available_strategies(primary, reference, has_corners);
    if let Some(requested) = requested
        && available.contains(&requested)
    {
        return requested;
    }
    if available.contains(&Strategy::Gps) {
        return Strategy::Gps;
    }
    // With the logger's own distance, lap distance % is as good as damper
    // matching; dampers earn the default only over a lap-time base.
    let distance_base = alignment::distance_base_available(primary, reference);
    if available.contains(&Strategy::PreCornerDampers) && !distance_base {
        return Strategy::PreCornerDampers;
    }
    Strategy::LapPercentage
}

/// Half-width (metres of primary lap distance) of the window the loss rate
/// is measured over: wide enough to smooth GPS and 50 Hz jitter out of the
/// delta's slope, narrow enough to keep a braking zone apart from its apex.
pub const LOSS_RATE_HALF_WINDOW_M: f64 = 8.0;

/// Time lost per metre (s/m, + the primary is slower) at every primary
/// sample: the slope of the cumulative `delta` over `distance`, measured
/// across `±half_window` metres (clipped at the lap ends). A window spanning
/// no distance (standing still) reads 0; the result is always finite and as
/// long as `delta`, or empty when the arrays disagree.
pub fn loss_rate(delta: &[f64], distance: &[f64], half_window: f64) -> Vec<f64> {
    let n = delta.len();
    if n < 2 || distance.len() != n {
        return Vec::new();
    }
    let (mut low, mut high) = (0usize, 0usize);
    (0..n)
        .map(|i| {
            let here = distance[i];
            while low < i && distance[low] < here - half_window {
                low += 1;
            }
            high = high.max(i);
            while high + 1 < n && distance[high + 1] <= here + half_window {
                high += 1;
            }
            let span = distance[high] - distance[low];
            let rate = (delta[high] - delta[low]) / span;
            if span > 1e-6 && rate.is_finite() {
                rate
            } else {
                0.0
            }
        })
        .collect()
}

/// One aligned lap pair.
#[derive(Debug, Clone)]
pub struct Comparison {
    primary: Arc<UnifiedLap>,
    reference: Arc<UnifiedLap>,
    strategy: Strategy,
    /// Manual damper offset in primary lap fraction (ManualDampers only).
    manual_offset: f64,
    alignment: AlignmentResult,
    delta: Vec<f64>,
    loss_rate: Vec<f64>,
}

impl Comparison {
    /// Align `reference` to `primary` with the (already effective) strategy.
    /// Never call from a paint, cursor or playback callback: this is the
    /// static per-pair work.
    pub fn new(
        primary: Arc<UnifiedLap>,
        reference: Arc<UnifiedLap>,
        strategy: Strategy,
        corner_starts: Vec<f64>,
        manual_offset: f64,
    ) -> Self {
        let alignment = if primary.time.len() >= 2 && reference.time.len() >= 2 {
            alignment::compute(
                &primary,
                &reference,
                &Options {
                    strategy,
                    corner_starts,
                },
            )
        } else {
            AlignmentResult::default()
        };
        let mut comparison = Self {
            primary,
            reference,
            strategy,
            manual_offset: if strategy == Strategy::ManualDampers {
                manual_offset
            } else {
                0.0
            },
            alignment,
            delta: Vec::new(),
            loss_rate: Vec::new(),
        };
        comparison.rebuild_delta();
        comparison
    }

    pub fn primary(&self) -> &Arc<UnifiedLap> {
        &self.primary
    }
    pub fn reference(&self) -> &Arc<UnifiedLap> {
        &self.reference
    }
    pub fn strategy(&self) -> Strategy {
        self.strategy
    }
    pub fn alignment(&self) -> &AlignmentResult {
        &self.alignment
    }
    pub fn manual_offset(&self) -> f64 {
        self.manual_offset
    }
    /// User-facing basis ("GPS · continuous", "Lap time %", ...).
    pub fn basis(&self) -> &str {
        &self.alignment.basis
    }
    /// HIGH / MED / LOW / NONE.
    pub fn confidence(&self) -> &'static str {
        alignment::confidence_label(&self.alignment.basis, self.alignment.gps_anchors)
    }

    /// Whether the delta says *where* on the lap time is lost: the map
    /// follows the track (GPS or pre-corner damper anchors, or a lap
    /// distance base). Over a lap-time base the delta is the lap-time gap
    /// spread in proportion to elapsed time, so its slope, and any split of
    /// it by place, only restates where the car is slow: the loss rate is
    /// then empty and callers must not rank places by the delta.
    pub fn places_time_loss(&self) -> bool {
        match self.alignment.basis.as_str() {
            alignment::BASIS_GPS_CONTINUOUS
            | alignment::BASIS_GPS_RESYNC
            | alignment::BASIS_DAMPERS_PRE_CORNER
            | alignment::BASIS_LAP_DISTANCE => true,
            alignment::BASIS_DAMPERS_MANUAL => self.alignment.distance_base,
            _ => false,
        }
    }

    /// Change the manual damper offset. Only the delta is rebuilt: the
    /// offset is applied at lookup, like the store did.
    pub fn set_manual_offset(&mut self, offset: f64) {
        if self.strategy != Strategy::ManualDampers {
            return;
        }
        self.manual_offset = offset;
        self.rebuild_delta();
    }

    fn shifted(&self, fraction: f64) -> f64 {
        clamp(fraction - self.manual_offset, 0.0, 1.0)
    }

    /// Reference lap time (s) at a primary fraction; -1 without a map.
    pub fn compare_time_for_primary_fraction(&self, fraction: f64) -> f64 {
        if self.alignment.time.len() != self.primary.time.len() || self.alignment.time.is_empty() {
            return -1.0;
        }
        monotonic::interpolate_fraction(&self.alignment.time, self.shifted(fraction))
    }

    /// Reference lap fraction at a primary fraction (identity without a map).
    pub fn compare_fraction_for_primary_fraction(&self, fraction: f64) -> f64 {
        alignment::interpolate_fraction(&self.alignment.fraction, self.shifted(fraction))
    }

    /// Primary fraction at a reference fraction (inverse map).
    pub fn primary_fraction_for_compare_fraction(&self, fraction: f64) -> f64 {
        let primary = alignment::invert_fraction(&self.alignment.fraction, fraction);
        clamp(primary + self.manual_offset, 0.0, 1.0)
    }

    fn rebuild_delta(&mut self) {
        self.delta.clear();
        self.loss_rate.clear();
        let primary = &self.primary;
        let reference = &self.reference;
        if primary.len() < 3 || reference.len() < 3 {
            return;
        }
        // Delta starts at zero; the same map as traces and video.
        let count = primary.len();
        let mut delta = vec![0.0; count];
        let mut base = 0.0;
        for i in 0..count {
            let fraction = i as f64 / (count - 1) as f64;
            let reference_time = self.compare_time_for_primary_fraction(fraction);
            if reference_time < 0.0 {
                return;
            }
            let raw = primary.time[i] - reference_time;
            if i == 0 {
                base = raw;
            }
            delta[i] = raw - base;
        }
        if self.places_time_loss() {
            self.loss_rate = loss_rate(&delta, &primary.distance, LOSS_RATE_HALF_WINDOW_M);
        }
        self.delta = delta;
    }

    /// Cumulative time delta (s) on the primary 50 Hz grid, starting at 0.
    /// Positive: the primary is slower. Empty when alignment failed.
    pub fn delta(&self) -> &[f64] {
        &self.delta
    }

    /// Time lost per metre (s/m) on the primary grid: the delta's slope over
    /// ±[`LOSS_RATE_HALF_WINDOW_M`] of lap distance ([`loss_rate`]). Finite
    /// everywhere; empty when the delta is, or when the map does not place
    /// time loss ([`Self::places_time_loss`]).
    pub fn loss_rate(&self) -> &[f64] {
        &self.loss_rate
    }

    /// Delta at a primary fraction (NaN without a delta).
    pub fn time_delta_at(&self, fraction: f64) -> f64 {
        let delta = &self.delta;
        if delta.len() < 2 {
            return f64::NAN;
        }
        let position = clamp(fraction, 0.0, 1.0) * (delta.len() - 1) as f64;
        let low = position.floor() as usize;
        let high = (low + 1).min(delta.len() - 1);
        let value = delta[low] + (delta[high] - delta[low]) * (position - low as f64);
        if value.is_finite() { value } else { f64::NAN }
    }

    /// Primary minus reference speed at the mapped station (NaN if unknown).
    pub fn speed_delta_at(&self, fraction: f64) -> f64 {
        if self.primary.speed.len() < 2 || self.reference.speed.len() < 2 {
            return f64::NAN;
        }
        let primary_fraction = clamp(fraction, 0.0, 1.0);
        let compare_fraction = self.compare_fraction_for_primary_fraction(primary_fraction);
        let p = monotonic::interpolate_fraction(&self.primary.speed, primary_fraction);
        let r = monotonic::interpolate_fraction(&self.reference.speed, compare_fraction);
        if !p.is_finite() || !r.is_finite() {
            return f64::NAN;
        }
        p - r
    }

    /// Local slope of the map around the cursor (reference seconds per
    /// primary second), clamped to [0.5, 2].
    pub fn video_rate_at(&self, cursor_fraction: f64) -> f64 {
        let primary = &self.primary;
        let map = &self.alignment.time;
        if map.len() != primary.time.len() || primary.time.len() < 3 {
            return 1.0;
        }
        let center =
            llround(clamp(cursor_fraction, 0.0, 1.0) * (primary.time.len() - 1) as f64) as usize;
        let radius = 5.max(primary.sample_rate / 2) as usize;
        let low = center.saturating_sub(radius);
        let high = (center + radius).min(primary.time.len() - 1);
        let primary_span = primary.time[high] - primary.time[low];
        if primary_span <= 0.0 {
            return 1.0;
        }
        clamp((map[high] - map[low]) / primary_span, 0.5, 2.0)
    }

    /// The same pair with roles exchanged: the reference becomes primary,
    /// the manual offset inverts. Cursor and viewport fractions are the
    /// caller's and stay where they are.
    pub fn swapped(&self, corner_starts: Vec<f64>) -> Self {
        Self::new(
            self.reference.clone(),
            self.primary.clone(),
            self.strategy,
            corner_starts,
            -self.manual_offset,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn loss_rate_is_the_delta_slope_over_distance() {
        // 1 m per sample; the primary loses 0.01 s/m over metres 40..60.
        let distance: Vec<f64> = (0..100).map(f64::from).collect();
        let delta: Vec<f64> = distance
            .iter()
            .map(|d| (d.clamp(40.0, 60.0) - 40.0) * 0.01)
            .collect();
        let rate = loss_rate(&delta, &distance, 4.0);
        assert_eq!(rate.len(), 100);
        assert!(rate.iter().all(|r| r.is_finite()));
        assert!((rate[50] - 0.01).abs() < 1e-12, "{}", rate[50]);
        assert_eq!(rate[10], 0.0);
        assert_eq!(rate[90], 0.0);
        // The window smooths the step at 40 m over ±4 m.
        assert!(rate[40] > 0.0 && rate[40] < 0.01);
    }

    #[test]
    fn loss_rate_reads_zero_standing_still_and_empty_on_mismatch() {
        let distance = [0.0, 0.0, 0.0, 0.0];
        let delta = [0.0, 0.1, 0.2, 0.3];
        assert_eq!(loss_rate(&delta, &distance, 5.0), vec![0.0; 4]);
        assert!(loss_rate(&delta, &distance[..3], 5.0).is_empty());
        let nan = [0.0, f64::NAN, 0.2, 0.3];
        let moving = [0.0, 1.0, 2.0, 3.0];
        assert!(loss_rate(&nan, &moving, 1.0).iter().all(|r| r.is_finite()));
    }
    /// A lap of `n` samples over `metres` at the speed profile `speed`
    /// (km/h), with the logger's own distance when `native`.
    fn lap(speed: impl Fn(f64) -> f64, metres: f64, native: bool) -> Arc<UnifiedLap> {
        let n = 501;
        let mut lap = UnifiedLap {
            distance_source: if native {
                crate::DistanceSource::Native
            } else {
                crate::DistanceSource::SpeedFused
            },
            ..UnifiedLap::default()
        };
        let mut time = 0.0;
        for i in 0..n {
            let share = i as f64 / (n - 1) as f64;
            let v = speed(share);
            if i > 0 {
                time += metres / (n - 1) as f64 / (v / 3.6);
            }
            lap.time.push(time);
            lap.distance.push(share * metres);
            lap.speed.push(v);
        }
        Arc::new(lap)
    }

    #[test]
    fn only_a_map_that_follows_the_track_places_time_loss() {
        // The primary is slower only in the middle fifth of the lap.
        let slow = |s: f64| {
            if (0.4..0.6).contains(&s) {
                100.0
            } else {
                150.0
            }
        };
        let fast = |_: f64| 150.0;
        let on_distance = Comparison::new(
            lap(slow, 1000.0, true),
            lap(fast, 1000.0, true),
            Strategy::LapPercentage,
            Vec::new(),
            0.0,
        );
        assert_eq!(on_distance.basis(), alignment::BASIS_LAP_DISTANCE);
        assert!(on_distance.places_time_loss());
        let rate = on_distance.loss_rate();
        assert_eq!(rate.len(), 501);
        assert!(rate[100].abs() < 1e-9 && rate[250] > 0.0, "{}", rate[250]);

        // Without the logger's distance the base is lap time: the delta is
        // the lap-time gap spread evenly, which places nothing.
        let on_time = Comparison::new(
            lap(slow, 1000.0, false),
            lap(fast, 1000.0, false),
            Strategy::LapPercentage,
            Vec::new(),
            0.0,
        );
        assert_eq!(on_time.basis(), alignment::BASIS_LAP_TIME);
        assert!(!on_time.places_time_loss());
        assert!(on_time.loss_rate().is_empty());
        assert!(!on_time.delta().is_empty(), "the delta itself stays");
    }
}
