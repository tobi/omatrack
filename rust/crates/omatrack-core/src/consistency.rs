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

/// Build the confidence bands of `primary` over `laps` (port of
/// `loadSessionConfidence` after its source open). Each lap is mapped onto
/// the primary grid through the default comparison alignment; laps that do
/// not align are skipped.
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
    pub fn load(
        recording: &Recording,
        laps: &[Lap],
        lap_ids: &[i32],
        overrides: &ChannelOverrides,
        reuse: &[(i32, Arc<UnifiedLap>)],
        cancel: &AtomicBool,
    ) -> Result<Self, SessionError> {
        Self::load_with_progress(
            recording,
            laps,
            lap_ids,
            overrides,
            reuse,
            cancel,
            &mut |_, _| {},
        )
    }

    /// [`Self::load`], reporting `(done, total)` after each lap.
    pub fn load_with_progress(
        recording: &Recording,
        laps: &[Lap],
        lap_ids: &[i32],
        overrides: &ChannelOverrides,
        reuse: &[(i32, Arc<UnifiedLap>)],
        cancel: &AtomicBool,
        progress: &mut dyn FnMut(usize, usize),
    ) -> Result<Self, SessionError> {
        let mut out = Vec::with_capacity(lap_ids.len());
        for (done, &id) in lap_ids.iter().enumerate() {
            cancelled(cancel)?;
            progress(done, lap_ids.len());
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
        progress(lap_ids.len(), lap_ids.len());
        Ok(Self { laps: out })
    }

    /// Every representative lap of `primary`'s session ([`spread_lap_ids`]),
    /// the primary reused, with `(done, total)` progress: the input of
    /// [`Self::consistency`].
    pub fn for_consistency(
        primary: &LoadedLap,
        cancel: &AtomicBool,
        progress: &mut dyn FnMut(usize, usize),
    ) -> Result<Self, SessionError> {
        Self::load_with_progress(
            primary.recording(),
            primary.laps(),
            &spread_lap_ids(primary.laps()),
            primary.overrides(),
            &[(primary.lap_id(), primary.unified().clone())],
            cancel,
            progress,
        )
    }

    /// The session spread of `primary` over every loaded lap but itself
    /// ([`build_consistency`]).
    pub fn consistency(
        &self,
        primary: &LoadedLap,
        cancel: &AtomicBool,
    ) -> Result<Consistency, SessionError> {
        build_consistency(
            primary.unified(),
            primary.lap_id(),
            self.laps.iter().map(|(id, lap)| (*id, lap.as_ref())),
            cancel,
        )
    }

    /// The laps both analyses need for `primary`: the fastest half of its
    /// session's representative laps (the corner quarter is a subset).
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

// ── session spread on the lap-distance base ────────────────────────

/// Channels a session spread carries: the normalized lane channels
/// ([`crate::overlay::STANDARD_CHANNELS`] without the axis and GPS).
pub const SPREAD_CHANNELS: &[&str] = &[
    "speed",
    "throttle",
    "brake",
    "gear",
    "steering",
    "clutch",
    "driver_throttle",
    "g_long",
    "g_lat",
    "damper_fl",
    "damper_fr",
    "damper_rl",
    "damper_rr",
];

/// Fewest other timed laps a session spread is drawn from: with one lap
/// there is no spread to speak of, only a second reference.
pub const MIN_SPREAD_LAPS: usize = 2;

/// Every representative (timed, complete, non-pit) lap of a session,
/// fastest first: the laps a session spread is built from.
pub fn spread_lap_ids(laps: &[Lap]) -> Vec<i32> {
    ranked(laps).into_iter().map(|lap| lap.id).collect()
}

/// One channel of a [`Consistency`]: each other lap's series and the
/// per-station envelope, all on the primary lap's 50 Hz grid.
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct ChannelSpread {
    /// `(lap id, samples)` of every other lap carrying the channel, fastest
    /// first. NaN where the lap has no finite value.
    pub laps: Vec<(i32, Arc<[f64]>)>,
    /// Smallest and largest value per primary sample over the other laps
    /// and the primary itself; NaN where none is finite.
    pub min: Arc<[f64]>,
    pub max: Arc<[f64]>,
}

/// How the primary lap sits in the driver's own session: every other
/// representative lap resampled onto the primary's lap-distance base
/// (share of lap distance, so every lap spans the same stations) and the
/// min–max spread per channel. Built off the UI thread by
/// [`build_consistency`] or [`SessionLaps::consistency`].
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Consistency {
    lap_ids: Vec<i32>,
    channels: BTreeMap<String, ChannelSpread>,
    samples: usize,
}

impl Consistency {
    /// The other laps that were resampled, fastest first.
    pub fn lap_ids(&self) -> &[i32] {
        &self.lap_ids
    }
    /// Number of other laps in the spread.
    pub fn lap_count(&self) -> usize {
        self.lap_ids.len()
    }
    /// Whether there are enough other laps to call it a spread
    /// ([`MIN_SPREAD_LAPS`]).
    pub fn is_meaningful(&self) -> bool {
        self.lap_count() >= MIN_SPREAD_LAPS && !self.channels.is_empty()
    }
    /// The spread of one lane channel key ([`SPREAD_CHANNELS`]).
    pub fn channel(&self, key: &str) -> Option<&ChannelSpread> {
        self.channels.get(key)
    }
    pub fn channels(&self) -> &BTreeMap<String, ChannelSpread> {
        &self.channels
    }
    /// Length of every series: the primary lap's sample count.
    pub fn samples(&self) -> usize {
        self.samples
    }
}

/// Lap fraction on `lap` of each primary sample's share of lap distance.
/// `None` when either lap has no usable distance.
fn distance_share_fractions(primary: &UnifiedLap, lap: &UnifiedLap) -> Option<Vec<f64>> {
    let (pd, od) = (&primary.distance, &lap.distance);
    if pd.len() < 2 || od.len() < 2 {
        return None;
    }
    let (p0, o0) = (pd[0], od[0]);
    let primary_total = pd[pd.len() - 1] - p0;
    let other_total = od[od.len() - 1] - o0;
    if !(primary_total > 0.0 && other_total > 0.0) {
        return None;
    }
    Some(
        pd.iter()
            .map(|d| {
                let share = ((d - p0) / primary_total).clamp(0.0, 1.0);
                invert_fraction(od, o0 + share * other_total)
            })
            .collect(),
    )
}

/// `key` of `lap` sampled at each fraction (gear holds the nearest).
fn resample(lap: &UnifiedLap, key: &str, fractions: &[f64]) -> Arc<[f64]> {
    fractions.iter().map(|&f| sample(lap, key, f)).collect()
}

/// The primary's own samples of `key` (it sits inside its spread).
fn own_values(lap: &UnifiedLap, key: &str) -> Vec<f64> {
    if key == "gear" {
        return lap.gear.iter().map(|g| f64::from(*g)).collect();
    }
    values(lap, key).map(<[f64]>::to_vec).unwrap_or_default()
}

/// Build the session spread of `primary` over `laps` (the laps of its
/// session; the primary itself, if passed, is skipped by id). Each lap is
/// mapped by share of lap distance, never by index or time, so a slower
/// lap's braking point still lands on its station.
pub fn build_consistency<'a>(
    primary: &UnifiedLap,
    primary_id: i32,
    laps: impl IntoIterator<Item = (i32, &'a UnifiedLap)>,
    cancel: &AtomicBool,
) -> Result<Consistency, SessionError> {
    let samples = primary.len();
    let mut result = Consistency {
        samples,
        ..Consistency::default()
    };
    if samples < 2 {
        return Ok(result);
    }
    let mut series: Vec<Vec<(i32, Arc<[f64]>)>> = vec![Vec::new(); SPREAD_CHANNELS.len()];
    for (id, lap) in laps {
        cancelled(cancel)?;
        if id == primary_id || lap.len() < 2 {
            continue;
        }
        let Some(fractions) = distance_share_fractions(primary, lap) else {
            continue;
        };
        for (field, key) in SPREAD_CHANNELS.iter().enumerate() {
            if available(lap, key) && available(primary, key) {
                series[field].push((id, resample(lap, key, &fractions)));
            }
        }
        result.lap_ids.push(id);
    }
    for (field, key) in SPREAD_CHANNELS.iter().enumerate() {
        let laps = std::mem::take(&mut series[field]);
        if laps.is_empty() {
            continue;
        }
        cancelled(cancel)?;
        let own = own_values(primary, key);
        let mut min = vec![f64::NAN; samples];
        let mut max = vec![f64::NAN; samples];
        for i in 0..samples {
            let values = laps
                .iter()
                .map(|(_, values)| values[i])
                .chain(own.get(i).copied());
            for value in values.filter(|v| v.is_finite()) {
                min[i] = if min[i].is_nan() {
                    value
                } else {
                    min[i].min(value)
                };
                max[i] = if max[i].is_nan() {
                    value
                } else {
                    max[i].max(value)
                };
            }
        }
        result.channels.insert(
            (*key).to_string(),
            ChannelSpread {
                laps,
                min: min.into(),
                max: max.into(),
            },
        );
    }
    Ok(result)
}
