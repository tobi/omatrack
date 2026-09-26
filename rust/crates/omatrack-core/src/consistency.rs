//! Session consistency: how the primary lap sits inside the driver's own
//! spread of representative laps.
//!
//! Ports of the math behind `TelemetryStore::requestTraceConfidence`
//! (`loadSessionConfidence`: a p10/p50/p90 envelope per channel over the
//! fastest half of the other representative laps, each mapped onto the
//! primary lap's track-station grid, plus a composite variation score) and
//! `requestCornerConsistency` (`loadCornerConsistency`: the spread of brake
//! points through one corner over the fastest quarter of the session).
//!
//! Both are background work (they unify several laps); the caller owns the
//! job slot and staleness key. Nothing here touches a UI.

use crate::alignment::{self, Options};
use crate::corners::measure_corner;
use crate::laps::Lap;
use crate::mapping::ChannelOverrides;
use crate::monotonic::{interpolate_fraction, invert_fraction};
use crate::num::llround;
use crate::recording::Recording;
use crate::session::{LoadedLap, SessionError};
use crate::unify::UnifiedLap;
use std::collections::BTreeMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

/// Channels that get a confidence band, keyed like the overlay channels.
pub const CONFIDENCE_CHANNELS: &[&str] = &[
    "speed",
    "throttle",
    "driver_throttle",
    "brake",
    "clutch",
    "steering",
    "gear",
    "damper_fl",
    "g_long",
];

/// Physical spread that counts as full variation, per composite channel.
const CONSISTENCY_FIELDS: &[(&str, f64)] = &[
    ("throttle", 0.25),
    ("brake", 25.0),
    ("steering", 15.0),
    ("gear", 1.0),
];

fn values<'a>(lap: &'a UnifiedLap, key: &str) -> Option<&'a [f64]> {
    Some(match key {
        "speed" => &lap.speed,
        "throttle" => &lap.throttle,
        "driver_throttle" => &lap.driver_throttle,
        "brake" => &lap.brake,
        "clutch" => &lap.clutch,
        "steering" => &lap.steering,
        "damper_fl" => &lap.damper_fl,
        "g_long" => &lap.g_force_long,
        _ => return None,
    })
}

fn available(lap: &UnifiedLap, key: &str) -> bool {
    if key == "gear" {
        return lap.gear.len() >= 2;
    }
    values(lap, key).is_some_and(|v| v.len() >= 2)
}

/// A channel sampled at a lap fraction: interpolated, gear nearest.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Preserve the C++ port's sample-index widths and rounding at this numerical boundary; verified by parity."
)]
fn sample(lap: &UnifiedLap, key: &str, fraction: f64) -> f64 {
    let fraction = fraction.clamp(0.0, 1.0);
    if key == "gear" {
        if lap.gear.is_empty() {
            return f64::NAN;
        }
        let last = lap.gear.len() - 1;
        let index = (llround(fraction * last as f64).max(0) as usize).min(last);
        return f64::from(lap.gear[index]);
    }
    match values(lap, key) {
        Some(values) if !values.is_empty() => interpolate_fraction(values, fraction),
        _ => f64::NAN,
    }
}

/// Representative laps, fastest first.
fn ranked(laps: &[Lap]) -> Vec<&Lap> {
    let mut ranked: Vec<&Lap> = laps
        .iter()
        .filter(|lap| lap.counts_for_best() && lap.time_ms.is_finite() && lap.time_ms > 0.0)
        .collect();
    ranked.sort_by(|a, b| a.time_ms.total_cmp(&b.time_ms));
    ranked
}

/// The laps a trace-confidence band is built from: the fastest half of the
/// representative laps, without the primary lap itself.
pub fn confidence_lap_ids(laps: &[Lap], primary_lap_id: i32) -> Vec<i32> {
    let mut ranked = ranked(laps);
    ranked.truncate(ranked.len().div_ceil(2));
    ranked
        .into_iter()
        .map(|lap| lap.id)
        .filter(|&id| id != primary_lap_id)
        .collect()
}

/// The laps corner consistency is measured over: the fastest quarter of
/// the representative laps (the primary lap included when it is one).
pub fn corner_consistency_lap_ids(laps: &[Lap]) -> Vec<i32> {
    let mut ranked = ranked(laps);
    ranked.truncate(ranked.len().div_ceil(4));
    ranked.into_iter().map(|lap| lap.id).collect()
}

/// A p10 / median / p90 envelope on the primary lap's 50 Hz grid.
#[derive(Debug, Clone, PartialEq, Default)]
#[non_exhaustive]
pub struct ConfidenceBand {
    pub lower: Vec<f64>,
    pub median: Vec<f64>,
    pub upper: Vec<f64>,
    /// Laps that carried this channel.
    pub lap_count: usize,
}

impl ConfidenceBand {
    pub fn new(lower: Vec<f64>, median: Vec<f64>, upper: Vec<f64>, lap_count: usize) -> Self {
        Self {
            lower,
            median,
            upper,
            lap_count,
        }
    }

    /// At least two laps and three aligned series of more than one sample.
    pub fn is_valid(&self) -> bool {
        self.lap_count >= 2
            && self.lower.len() > 1
            && self.lower.len() == self.median.len()
            && self.lower.len() == self.upper.len()
    }
}

/// Session trace confidence for one primary lap.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct TraceConfidence {
    bands: BTreeMap<String, ConfidenceBand>,
    consistency: Vec<f64>,
    lap_count: usize,
}

impl TraceConfidence {
    /// The band for a channel key ([`CONFIDENCE_CHANNELS`]).
    pub fn band(&self, key: &str) -> Option<&ConfidenceBand> {
        self.bands.get(key)
    }
    pub fn bands(&self) -> &BTreeMap<String, ConfidenceBand> {
        &self.bands
    }
    /// Composite session variation per primary sample, 0 (tight) to 1
    /// (full spread); NaN where no channel had a band.
    pub fn consistency(&self) -> &[f64] {
        &self.consistency
    }
    /// Laps that aligned onto the primary grid.
    pub fn lap_count(&self) -> usize {
        self.lap_count
    }
}

fn cancelled(cancel: &AtomicBool) -> Result<(), SessionError> {
    if cancel.load(Ordering::Relaxed) {
        Err(SessionError::Cancelled)
    } else {
        Ok(())
    }
}

/// Build the confidence bands of `primary` over `laps` (port of `loadSessionConfidence`
/// after its source open).
///
/// Each lap is mapped onto the primary grid through the default comparison alignment;
/// laps that do not align are skipped.
#[expect(
    clippy::cast_precision_loss,
    reason = "Preserve the C++ port's sample-index widths and rounding at this numerical boundary; verified by parity."
)]
///
/// # Errors
/// Returns `SessionError::Cancelled` when the cancellation flag is set during the work.
pub fn trace_confidence<'a>(
    primary: &UnifiedLap,
    laps: impl IntoIterator<Item = &'a UnifiedLap>,
    cancel: &AtomicBool,
) -> Result<TraceConfidence, SessionError> {
    let mut result = TraceConfidence::default();
    let count = primary.len();
    if count < 3 {
        return Ok(result);
    }
    // Per channel: one row of `count` samples per aligned lap.
    let mut rows: Vec<Vec<Vec<f64>>> = vec![Vec::new(); CONFIDENCE_CHANNELS.len()];
    let mut field_laps = vec![0usize; CONFIDENCE_CHANNELS.len()];
    for lap in laps {
        cancelled(cancel)?;
        if lap.len() < 3 {
            continue;
        }
        let map = alignment::compute(primary, lap, &Options::default());
        if map.fraction.len() != count {
            continue;
        }
        for (field, key) in CONFIDENCE_CHANNELS.iter().enumerate() {
            let mut row = vec![f64::NAN; count];
            if available(lap, key) {
                field_laps[field] += 1;
                for (slot, &fraction) in row.iter_mut().zip(&map.fraction) {
                    *slot = sample(lap, key, fraction);
                }
            }
            rows[field].push(row);
        }
        result.lap_count += 1;
    }

    let mut column = Vec::with_capacity(result.lap_count);
    for (field, key) in CONFIDENCE_CHANNELS.iter().enumerate() {
        if field_laps[field] < 2 {
            continue;
        }
        cancelled(cancel)?;
        let mut band = ConfidenceBand {
            lower: vec![f64::NAN; count],
            median: vec![f64::NAN; count],
            upper: vec![f64::NAN; count],
            lap_count: field_laps[field],
        };
        for sample_ix in 0..count {
            column.clear();
            column.extend(
                rows[field]
                    .iter()
                    .map(|row| row[sample_ix])
                    .filter(|v| v.is_finite()),
            );
            if column.len() < 2 {
                continue;
            }
            column.sort_by(f64::total_cmp);
            band.lower[sample_ix] = interpolate_fraction(&column, 0.10);
            band.median[sample_ix] = interpolate_fraction(&column, 0.50);
            band.upper[sample_ix] = interpolate_fraction(&column, 0.90);
        }
        result.bands.insert((*key).to_string(), band);
    }

    // Composite variation: each channel's robust p10-p90 spread widened to
    // include the primary lap, scaled by a physical full-heat spread, then
    // averaged over the channels that have a band there.
    result.consistency = vec![f64::NAN; count];
    for sample_ix in 0..count {
        let fraction = sample_ix as f64 / (count.max(2) - 1) as f64;
        let mut score = 0.0;
        let mut fields = 0;
        for &(key, full_heat_spread) in CONSISTENCY_FIELDS {
            let Some(band) = result.bands.get(key).filter(|b| b.is_valid()) else {
                continue;
            };
            let active = sample(primary, key, fraction);
            let lower = band.lower[sample_ix];
            let upper = band.upper[sample_ix];
            if !active.is_finite() || !lower.is_finite() || !upper.is_finite() {
                continue;
            }
            let spread = active.max(upper) - active.min(lower);
            score += (spread / full_heat_spread).clamp(0.0, 1.0);
            fields += 1;
        }
        if fields > 0 {
            result.consistency[sample_ix] = score / f64::from(fields);
        }
    }
    Ok(result)
}

/// Brake-point spread through one corner across a set of laps.
#[derive(Debug, Clone, Copy, PartialEq)]
#[non_exhaustive]
pub struct CornerConsistency {
    /// Laps asked for.
    pub lap_count: usize,
    /// Laps with a measurable corner.
    pub valid_lap_count: usize,
    /// Laps that braked in the corner.
    pub braking_lap_count: usize,
    /// Metres from the zone start; NaN without braking laps.
    pub median_brake_point: f64,
    /// Population standard deviation, metres.
    pub brake_point_std_dev: f64,
    /// Latest minus earliest, metres.
    pub brake_point_range: f64,
}

impl Default for CornerConsistency {
    fn default() -> Self {
        Self {
            lap_count: 0,
            valid_lap_count: 0,
            braking_lap_count: 0,
            median_brake_point: f64::NAN,
            brake_point_std_dev: f64::NAN,
            brake_point_range: f64::NAN,
        }
    }
}

/// Measure a corner given as absolute lap distances (metres) on every lap
/// (port of `loadCornerConsistency` and its result reduction). Each lap
/// locates the corner by its own distance axis.
#[expect(
    clippy::cast_precision_loss,
    reason = "Preserve the C++ port's sample-index widths and rounding at this numerical boundary; verified by parity."
)]
pub fn corner_consistency<'a>(
    laps: impl IntoIterator<Item = &'a UnifiedLap>,
    start_distance: f64,
    end_distance: f64,
) -> CornerConsistency {
    let mut result = CornerConsistency::default();
    let mut points = Vec::new();
    for lap in laps {
        result.lap_count += 1;
        if lap.len() < 3 || lap.distance.len() < 3 {
            continue;
        }
        let start = invert_fraction(&lap.distance, start_distance);
        let end = invert_fraction(&lap.distance, end_distance);
        let metrics = measure_corner(lap, start, end, true);
        if !metrics.valid {
            continue;
        }
        result.valid_lap_count += 1;
        if metrics.brake_index >= 0 {
            points.push(metrics.brake_point);
        }
    }
    result.braking_lap_count = points.len();
    if points.is_empty() {
        return result;
    }
    points.sort_by(f64::total_cmp);
    let middle = points.len() / 2;
    result.median_brake_point = if points.len() % 2 == 0 {
        (points[middle - 1] + points[middle]) * 0.5
    } else {
        points[middle]
    };
    let mean = points.iter().sum::<f64>() / points.len() as f64;
    let squared: f64 = points.iter().map(|p| (p - mean) * (p - mean)).sum();
    result.brake_point_std_dev = (squared / points.len() as f64).sqrt();
    result.brake_point_range = points[points.len() - 1] - points[0];
    result
}

/// Unified laps of one recording, keyed by lap id: the shared input of
/// trace confidence and corner consistency, built once per primary
/// session on a background executor.
#[derive(Debug, Clone, Default)]
pub struct SessionLaps {
    laps: Vec<(i32, Arc<UnifiedLap>)>,
}

impl SessionLaps {
    /// Unify `lap_ids` of `recording` (unknown ids and empty laps are
    /// skipped). `reuse` supplies laps that are already unified.
    ///
    /// # Errors
    /// Returns `SessionError::Cancelled` when the cancellation flag is set during the
    /// work.
    pub fn load(
        recording: &Recording,
        laps: &[Lap],
        lap_ids: &[i32],
        overrides: &ChannelOverrides,
        reuse: &[(i32, Arc<UnifiedLap>)],
        cancel: &AtomicBool,
    ) -> Result<Self, SessionError> {
        let mut out = Vec::with_capacity(lap_ids.len());
        for &id in lap_ids {
            cancelled(cancel)?;
            if let Some((_, unified)) = reuse.iter().find(|(reuse_id, _)| *reuse_id == id) {
                out.push((id, unified.clone()));
                continue;
            }
            let Some(lap) = laps.iter().find(|lap| lap.id == id) else {
                continue;
            };
            let unified = recording.unify_lap(lap.start_time, lap.end_time, overrides);
            if !unified.is_empty() {
                out.push((id, Arc::new(unified)));
            }
        }
        Ok(Self { laps: out })
    }

    /// The laps both analyses need for `primary`: the fastest half of its
    /// session's representative laps (the corner quarter is a subset).
    ///
    /// # Errors
    /// Returns `SessionError::Cancelled` when the cancellation flag is set during the
    /// work.
    pub fn for_primary(primary: &LoadedLap, cancel: &AtomicBool) -> Result<Self, SessionError> {
        // No lap has id i32::MIN: this is the fastest half, primary included.
        let ids = confidence_lap_ids(primary.laps(), i32::MIN);
        Self::load(
            primary.recording(),
            primary.laps(),
            &ids,
            primary.overrides(),
            &[(primary.lap_id(), primary.unified().clone())],
            cancel,
        )
    }

    pub fn get(&self, id: i32) -> Option<&Arc<UnifiedLap>> {
        self.laps
            .iter()
            .find(|(lap_id, _)| *lap_id == id)
            .map(|(_, lap)| lap)
    }

    pub fn len(&self) -> usize {
        self.laps.len()
    }

    pub fn is_empty(&self) -> bool {
        self.laps.is_empty()
    }

    fn select<'a>(&'a self, ids: &'a [i32]) -> impl Iterator<Item = &'a UnifiedLap> + 'a {
        ids.iter()
            .filter_map(move |id| self.get(*id))
            .map(Arc::as_ref)
    }

    /// Trace confidence for `primary` over this session's fastest half,
    /// without the primary lap. Fewer than two such laps yield no bands.
    ///
    /// # Errors
    /// Returns `SessionError::Cancelled` when the cancellation flag is set during the
    /// work.
    pub fn trace_confidence(
        &self,
        primary: &LoadedLap,
        cancel: &AtomicBool,
    ) -> Result<TraceConfidence, SessionError> {
        let ids = confidence_lap_ids(primary.laps(), primary.lap_id());
        if ids.len() < 2 {
            return Ok(TraceConfidence {
                lap_count: ids.len(),
                ..TraceConfidence::default()
            });
        }
        trace_confidence(primary.unified(), self.select(&ids), cancel)
    }

    /// Brake-point consistency through `zone` (primary lap fractions) over
    /// the session's fastest quarter.
    pub fn corner_consistency(
        &self,
        primary: &LoadedLap,
        start: f64,
        end: f64,
    ) -> CornerConsistency {
        let distance = &primary.unified().distance;
        if distance.len() < 2 {
            return CornerConsistency::default();
        }
        let ids = corner_consistency_lap_ids(primary.laps());
        let mut result = corner_consistency(
            self.select(&ids),
            interpolate_fraction(distance, start),
            interpolate_fraction(distance, end),
        );
        result.lap_count = ids.len();
        result
    }
}
