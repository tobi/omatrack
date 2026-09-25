//! Primary -> reference lap alignment (port of `ComparisonAlignment.cpp`).
//!
//! Every strategy starts from the lap-percentage base: the same share of lap
//! *distance* when both laps carry native distance whose totals agree within
//! 2%, of lap *time* otherwise. Corrections then have to earn their place:
//! verified GPS anchors (self-consistent fixes, agreeing speeds, travel
//! direction within 60 degrees), pre-corner damper correlation, or one manual
//! offset. Traces, delta, cursor readouts and video all consume the one map
//! this produces.

use crate::monotonic;
use crate::num::{clamp, llround, max, min};
use crate::unify::{DistanceSource, UnifiedLap};

/// The alignment strategies a user can choose.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Hash)]
pub enum Strategy {
    /// GPS matches wherever both fixes are self-consistent.
    #[default]
    Gps,
    /// Front-damper bump signatures correlated before each corner.
    PreCornerDampers,
    /// The base plus one user offset set against the damper traces.
    ManualDampers,
    /// The universal fallback.
    LapPercentage,
}

impl Strategy {
    /// Stable config key (`video.reference_sync`).
    pub fn key(self) -> &'static str {
        match self {
            Self::Gps => "gps",
            Self::PreCornerDampers => "pre-corner-dampers",
            Self::ManualDampers => "manual-dampers",
            Self::LapPercentage => "lap-percentage",
        }
    }

    pub fn from_key(key: &str) -> Option<Self> {
        match key {
            "gps" => Some(Self::Gps),
            "pre-corner-dampers" => Some(Self::PreCornerDampers),
            "manual-dampers" => Some(Self::ManualDampers),
            "lap-percentage" => Some(Self::LapPercentage),
            _ => None,
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Self::Gps => "GPS",
            Self::PreCornerDampers => "Dampers · pre-corner",
            Self::ManualDampers => "Dampers · manual",
            Self::LapPercentage => "Lap %",
        }
    }
}

/// Alignment inputs beyond the two laps.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Options {
    pub strategy: Strategy,
    /// Primary-lap fractions at the starts of configured corners.
    pub corner_starts: Vec<f64>,
}

/// One pass of comparison alignment.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct AlignmentResult {
    /// Compare-lap time (s) for every primary sample, in primary order.
    pub time: Vec<f64>,
    /// Compare-lap fraction (0-1) for every primary sample.
    pub fraction: Vec<f64>,
    /// Stable user-facing basis; empty when alignment is impossible.
    pub basis: String,
    /// Accepted GPS anchors (zero for non-GPS strategies).
    pub gps_anchors: i32,
    /// GPS fixes near a match that failed self-consistency.
    pub gps_rejected: i32,
    /// The base is lap distance rather than lap time.
    pub distance_base: bool,
    /// Non-empty when monotonicity validation rejected the time axis.
    pub rejection_reason: String,
}

pub const BASIS_GPS_CONTINUOUS: &str = "GPS \u{b7} continuous";
pub const BASIS_GPS_RESYNC: &str = "GPS \u{b7} re-sync";
pub const BASIS_DAMPERS_PRE_CORNER: &str = "Dampers \u{b7} pre-corner";
pub const BASIS_DAMPERS_MANUAL: &str = "Dampers \u{b7} manual";
pub const BASIS_LAP_DISTANCE: &str = "Lap distance %";
pub const BASIS_LAP_TIME: &str = "Lap time %";

const PI: f64 = std::f64::consts::PI;
const METERS_PER_DEGREE: f64 = 111_320.0;
const GPS_ANCHOR_RATE: f64 = 5.0;
// Travel-direction gate: heading over +-0.3 s, candidate within 60 degrees.
const HEADING_HALF_WINDOW_SECONDS: f64 = 0.3;
const HEADING_MINIMUM_TRAVEL_METERS: f64 = 3.0;
const HEADING_MINIMUM_AGREEMENT: f64 = 0.5;
const DISTANCE_BASE_TOTAL_TOLERANCE: f64 = 0.02;
// A fix is trusted only if the speed implied by +-0.5 s of positions
// matches vehicle speed.
const GPS_CONSISTENCY_HALF_WINDOW_SECONDS: f64 = 0.5;
const GPS_CONSISTENCY_MINIMUM_SPEED_KMH: f64 = 30.0;
const GPS_CONSISTENCY_RELATIVE_TOLERANCE: f64 = 0.08;
const GPS_CONSISTENCY_ABSOLUTE_TOLERANCE_KMH: f64 = 6.0;
// At the same place on track two cars rarely differ by more than this.
const MATCHED_SPEED_RELATIVE_TOLERANCE: f64 = 0.20;
const MATCHED_SPEED_ABSOLUTE_TOLERANCE_KMH: f64 = 15.0;

#[derive(Debug, Clone, Copy)]
struct Anchor {
    primary_index: usize,
    compare_time: f64,
}

fn distance_base_usable(primary: &UnifiedLap, compare: &UnifiedLap) -> bool {
    if primary.distance_source != DistanceSource::Native
        || compare.distance_source != DistanceSource::Native
        || primary.distance.len() != primary.time.len()
        || compare.distance.len() != compare.time.len()
        || primary.distance.is_empty()
        || compare.distance.is_empty()
    {
        return false;
    }
    let p = primary.distance[primary.distance.len() - 1] - primary.distance[0];
    let c = compare.distance[compare.distance.len() - 1] - compare.distance[0];
    if !(p > 0.0) || !(c > 0.0) || !(p + c).is_finite() {
        return false;
    }
    (p - c).abs() / max(p, c) <= DISTANCE_BASE_TOTAL_TOLERANCE
}

fn lap_time_percentage(primary: &UnifiedLap, compare: &UnifiedLap) -> Vec<f64> {
    let n = primary.time.len();
    let mut times = vec![0.0; n];
    let primary_start = primary.time[0];
    let primary_span = primary.time[n - 1] - primary_start;
    let compare_start = compare.time[0];
    let compare_span = compare.time[compare.time.len() - 1] - compare_start;
    for i in 0..n {
        let pct = if primary_span > 0.0 {
            (primary.time[i] - primary_start) / primary_span
        } else {
            i as f64 / (n - 1) as f64
        };
        times[i] = compare_start + clamp(pct, 0.0, 1.0) * compare_span;
    }
    times
}

fn lap_distance_percentage(primary: &UnifiedLap, compare: &UnifiedLap) -> Vec<f64> {
    let n = primary.time.len();
    let mut times = vec![0.0; n];
    let p0 = primary.distance[0];
    let p_span = primary.distance[primary.distance.len() - 1] - p0;
    let c0 = compare.distance[0];
    let c_span = compare.distance[compare.distance.len() - 1] - c0;
    let mut high = 1usize;
    for i in 0..n {
        let share = clamp((primary.distance[i] - p0) / p_span, 0.0, 1.0);
        let target = c0 + share * c_span;
        while high + 1 < compare.distance.len() && compare.distance[high] < target {
            high += 1;
        }
        let low = high - 1;
        let span = compare.distance[high] - compare.distance[low];
        let local = if span > 0.0 {
            clamp((target - compare.distance[low]) / span, 0.0, 1.0)
        } else {
            0.0
        };
        times[i] = compare.time[low] + local * (compare.time[high] - compare.time[low]);
        if i > 0 {
            times[i] = max(times[i], times[i - 1]);
        }
    }
    times
}

fn lap_percentage(primary: &UnifiedLap, compare: &UnifiedLap) -> (Vec<f64>, bool) {
    let distance = distance_base_usable(primary, compare);
    let times = if distance {
        lap_distance_percentage(primary, compare)
    } else {
        lap_time_percentage(primary, compare)
    };
    (times, distance)
}

fn gps_arrays_available(lap: &UnifiedLap) -> bool {
    lap.gps_lat.len() == lap.time.len()
        && lap.gps_lon.len() == lap.time.len()
        && lap.gps_position_accuracy.len() == lap.time.len()
}

fn gps_fix_usable(latitude: f64, longitude: f64, accuracy: f64) -> bool {
    latitude.is_finite()
        && longitude.is_finite()
        && accuracy.is_finite()
        && latitude.abs() <= 90.0
        && longitude.abs() <= 180.0
        && (latitude.abs() > 1e-8 || longitude.abs() > 1e-8)
        && accuracy > 0.0
        && accuracy <= 25.0
}

fn gps_coverage_available(lap: &UnifiedLap) -> bool {
    if !gps_arrays_available(lap) || lap.time.len() < 8 {
        return false;
    }
    let mut first = lap.time.len();
    let mut last = 0usize;
    let mut count = 0usize;
    for i in 0..lap.time.len() {
        if !gps_fix_usable(lap.gps_lat[i], lap.gps_lon[i], lap.gps_position_accuracy[i]) {
            continue;
        }
        first = first.min(i);
        last = i;
        count += 1;
    }
    // Enough to offer GPS at all; patchy coverage is handled as re-syncs.
    count >= 8 && first < last && (last - first) as f64 / (lap.time.len() - 1) as f64 >= 0.2
}

#[derive(Debug, Clone, Copy)]
struct Heading {
    north: f64,
    east: f64,
}

fn travel_heading(lap: &UnifiedLap, index: usize) -> Option<Heading> {
    let half = max(
        1.0,
        (f64::from(lap.sample_rate) * HEADING_HALF_WINDOW_SECONDS).round(),
    ) as usize;
    let before = index.saturating_sub(half);
    let after = (index + half).min(lap.time.len() - 1);
    if after <= before {
        return None;
    }
    if !gps_fix_usable(
        lap.gps_lat[before],
        lap.gps_lon[before],
        lap.gps_position_accuracy[before],
    ) || !gps_fix_usable(
        lap.gps_lat[after],
        lap.gps_lon[after],
        lap.gps_position_accuracy[after],
    ) {
        return None;
    }
    let mean_latitude = 0.5 * (lap.gps_lat[before] + lap.gps_lat[after]) * PI / 180.0;
    let north = (lap.gps_lat[after] - lap.gps_lat[before]) * METERS_PER_DEGREE;
    let east = (lap.gps_lon[after] - lap.gps_lon[before]) * METERS_PER_DEGREE * mean_latitude.cos();
    let length = north.hypot(east);
    if length < HEADING_MINIMUM_TRAVEL_METERS {
        return None;
    }
    Some(Heading {
        north: north / length,
        east: east / length,
    })
}

fn nearest_gps_index(
    primary: &UnifiedLap,
    primary_index: usize,
    compare: &UnifiedLap,
    base_compare_time: f64,
) -> Option<usize> {
    if !gps_arrays_available(primary)
        || !gps_arrays_available(compare)
        || primary_index >= primary.time.len()
    {
        return None;
    }
    let latitude = primary.gps_lat[primary_index];
    let longitude = primary.gps_lon[primary_index];
    let primary_accuracy = primary.gps_position_accuracy[primary_index];
    if !gps_fix_usable(latitude, longitude, primary_accuracy) {
        return None;
    }
    let primary_heading = travel_heading(primary, primary_index);

    let center =
        monotonic::lower_bound(&compare.time, base_compare_time).min(compare.time.len() - 1);
    let search_radius = (compare.sample_rate * 8).max(1) as usize;
    let begin = center.saturating_sub(search_radius);
    let end = (center + search_radius).min(compare.time.len() - 1);
    let mut best = compare.time.len();
    let mut best_distance = f64::INFINITY;
    for j in begin..=end {
        let compare_accuracy = compare.gps_position_accuracy[j];
        if !gps_fix_usable(compare.gps_lat[j], compare.gps_lon[j], compare_accuracy) {
            continue;
        }
        let mean_latitude = 0.5 * (latitude + compare.gps_lat[j]) * PI / 180.0;
        let north = (compare.gps_lat[j] - latitude) * METERS_PER_DEGREE;
        let east = (compare.gps_lon[j] - longitude) * METERS_PER_DEGREE * mean_latitude.cos();
        let distance = north.hypot(east);
        if distance >= best_distance {
            continue;
        }
        // The other leg of a hairpin can be the nearest fix: reject
        // candidates travelling the other way.
        if let Some(ph) = primary_heading
            && let Some(ch) = travel_heading(compare, j)
        {
            let agreement = ph.north * ch.north + ph.east * ch.east;
            if agreement < HEADING_MINIMUM_AGREEMENT {
                continue;
            }
        }
        best_distance = distance;
        best = j;
    }
    if best == compare.time.len() {
        return None;
    }
    let acceptance = min(
        35.0,
        max(
            18.0,
            primary_accuracy + compare.gps_position_accuracy[best] + 8.0,
        ),
    );
    if best_distance > acceptance {
        return None;
    }
    Some(best)
}

fn gps_self_consistent(lap: &UnifiedLap, index: usize) -> bool {
    if lap.speed.len() != lap.time.len() || index >= lap.time.len() {
        return false;
    }
    let speed = lap.speed[index];
    if !speed.is_finite() || speed < GPS_CONSISTENCY_MINIMUM_SPEED_KMH {
        return false;
    }
    let half = max(
        1.0,
        (f64::from(lap.sample_rate) * GPS_CONSISTENCY_HALF_WINDOW_SECONDS).round(),
    ) as usize;
    if index < half || index + half >= lap.time.len() {
        return false;
    }
    let before = index - half;
    let after = index + half;
    if !gps_fix_usable(
        lap.gps_lat[before],
        lap.gps_lon[before],
        lap.gps_position_accuracy[before],
    ) || !gps_fix_usable(
        lap.gps_lat[after],
        lap.gps_lon[after],
        lap.gps_position_accuracy[after],
    ) {
        return false;
    }
    let dt = lap.time[after] - lap.time[before];
    if !(dt > 0.0) {
        return false;
    }
    let mean_latitude = 0.5 * (lap.gps_lat[before] + lap.gps_lat[after]) * PI / 180.0;
    let north = (lap.gps_lat[after] - lap.gps_lat[before]) * METERS_PER_DEGREE;
    let east = (lap.gps_lon[after] - lap.gps_lon[before]) * METERS_PER_DEGREE * mean_latitude.cos();
    let derived = north.hypot(east) / dt * 3.6;
    (derived - speed).abs()
        <= max(
            GPS_CONSISTENCY_ABSOLUTE_TOLERANCE_KMH,
            GPS_CONSISTENCY_RELATIVE_TOLERANCE * speed,
        )
}

fn speeds_agree(primary: &UnifiedLap, i: usize, compare: &UnifiedLap, j: usize) -> bool {
    let a = primary.speed[i];
    let b = compare.speed[j];
    a.is_finite()
        && b.is_finite()
        && (a - b).abs()
            <= max(
                MATCHED_SPEED_ABSOLUTE_TOLERANCE_KMH,
                MATCHED_SPEED_RELATIVE_TOLERANCE * max(a, b),
            )
}

/// Mean absolute speed difference at the mapped compare times: the correct
/// station map minimises it.
fn mapped_speed_disagreement(primary: &UnifiedLap, compare: &UnifiedLap, times: &[f64]) -> f64 {
    let mut sum = 0.0;
    let mut count = 0usize;
    let mut high = 1usize;
    let mut i = 0usize;
    while i < times.len() {
        while high + 1 < compare.time.len() && compare.time[high] < times[i] {
            high += 1;
        }
        let low = high - 1;
        let span = compare.time[high] - compare.time[low];
        let local = if span > 0.0 {
            clamp((times[i] - compare.time[low]) / span, 0.0, 1.0)
        } else {
            0.0
        };
        let speed = compare.speed[low] + local * (compare.speed[high] - compare.speed[low]);
        if speed.is_finite() && primary.speed[i].is_finite() {
            sum += (primary.speed[i] - speed).abs();
            count += 1;
        }
        i += 5;
    }
    if count > 0 {
        sum / count as f64
    } else {
        f64::INFINITY
    }
}

#[derive(Debug, Default)]
struct GpsAnchors {
    anchors: Vec<Anchor>,
    rejected: i32,
    continuous: bool,
}

fn validated_gps_anchors(
    primary: &UnifiedLap,
    compare: &UnifiedLap,
    base_times: &[f64],
) -> GpsAnchors {
    let mut result = GpsAnchors::default();
    if !gps_available(primary, compare) {
        return result;
    }
    let step = max(1.0, f64::from(primary.sample_rate) / GPS_ANCHOR_RATE) as usize;
    let mut i = 0usize;
    while i < primary.time.len() {
        if let Some(matched) = nearest_gps_index(primary, i, compare, base_times[i]) {
            if !gps_self_consistent(primary, i)
                || !gps_self_consistent(compare, matched)
                || !speeds_agree(primary, i, compare, matched)
            {
                result.rejected += 1;
            } else {
                let compare_time = compare.time[matched];
                let monotonic = result
                    .anchors
                    .last()
                    .is_none_or(|last| compare_time > last.compare_time);
                if monotonic {
                    result.anchors.push(Anchor {
                        primary_index: i,
                        compare_time,
                    });
                }
            }
        }
        i += step;
    }
    if result.anchors.len() < 2 {
        result.anchors.clear();
        return result;
    }
    let mut occupied: u8 = 0;
    for anchor in &result.anchors {
        let bin = (anchor.primary_index * 8 / primary.time.len()).min(7);
        occupied |= 1u8 << bin;
    }
    let occupied_count = occupied.count_ones();
    let anchors = &result.anchors;
    let coverage = (anchors[anchors.len() - 1].primary_index - anchors[0].primary_index) as f64
        / (primary.time.len() - 1) as f64;
    // Continuous: trusted fixes around the whole lap; otherwise re-syncs.
    result.continuous = anchors.len() >= 8 && occupied_count >= 4 && coverage >= 0.5;
    result
}

fn front_damper_series(lap: &UnifiedLap) -> Vec<f64> {
    let left = lap.damper_fl.len() == lap.time.len();
    let right = lap.damper_fr.len() == lap.time.len();
    if !left && !right {
        return Vec::new();
    }
    (0..lap.time.len())
        .map(|i| {
            let a = if left {
                lap.damper_fl[i]
            } else {
                lap.damper_fr[i]
            };
            let b = if right { lap.damper_fr[i] } else { a };
            if a.is_finite() && b.is_finite() {
                (a + b) * 0.5
            } else {
                f64::NAN
            }
        })
        .collect()
}

/// A mapped damper channel is not enough: loggers without the sensors carry
/// a constant channel. Require most samples finite and real motion.
fn front_damper_available(lap: &UnifiedLap) -> bool {
    if lap.damper_fl.len() != lap.time.len() && lap.damper_fr.len() != lap.time.len() {
        return false;
    }
    let series = front_damper_series(lap);
    let mut finite = 0usize;
    let mut sum = 0.0;
    let mut squares = 0.0;
    for &value in &series {
        if !value.is_finite() {
            continue;
        }
        finite += 1;
        sum += value;
        squares += value * value;
    }
    if finite < series.len() * 4 / 5 || finite < 2 {
        return false;
    }
    let mean = sum / finite as f64;
    let variance = squares / finite as f64 - mean * mean;
    variance > 1e-9 * max(1.0, mean * mean)
}

fn damper_time_at_corner(
    primary: &UnifiedLap,
    primary_damper: &[f64],
    primary_index: usize,
    compare: &UnifiedLap,
    compare_damper: &[f64],
    base_compare_time: f64,
) -> Option<f64> {
    const WINDOW_SECONDS: f64 = 2.5;
    const SEARCH_SECONDS: f64 = 2.0;
    const CORRELATION_SAMPLES: usize = 96;
    let primary_window = max(1.0, f64::from(primary.sample_rate) * WINDOW_SECONDS) as usize;
    if primary_index < primary_window {
        return None;
    }
    let center =
        monotonic::lower_bound(&compare.time, base_compare_time).min(compare.time.len() - 1);
    let compare_window = max(1.0, f64::from(compare.sample_rate) * WINDOW_SECONDS) as usize;
    let search = max(1.0, f64::from(compare.sample_rate) * SEARCH_SECONDS) as i64;
    let mut best_score = -1.0;
    let mut best_index = compare.time.len();

    let sample_indices = |sample: usize, compare_index: usize| -> (usize, usize) {
        let local = sample as f64 / (CORRELATION_SAMPLES - 1) as f64;
        let pi = primary_index - primary_window + llround(local * primary_window as f64) as usize;
        let ci = compare_index - compare_window + llround(local * compare_window as f64) as usize;
        (pi, ci)
    };

    for shift in -search..=search {
        let shifted = center as i64 + shift;
        if shifted < compare_window as i64 || shifted >= compare.time.len() as i64 {
            continue;
        }
        let compare_index = shifted as usize;
        let mut primary_mean = 0.0;
        let mut compare_mean = 0.0;
        let mut valid = 0usize;
        for sample in 0..CORRELATION_SAMPLES {
            let (pi, ci) = sample_indices(sample, compare_index);
            if !primary_damper[pi].is_finite() || !compare_damper[ci].is_finite() {
                continue;
            }
            primary_mean += primary_damper[pi];
            compare_mean += compare_damper[ci];
            valid += 1;
        }
        if valid < CORRELATION_SAMPLES * 3 / 4 {
            continue;
        }
        primary_mean /= valid as f64;
        compare_mean /= valid as f64;
        let mut covariance = 0.0;
        let mut primary_variance = 0.0;
        let mut compare_variance = 0.0;
        for sample in 0..CORRELATION_SAMPLES {
            let (pi, ci) = sample_indices(sample, compare_index);
            if !primary_damper[pi].is_finite() || !compare_damper[ci].is_finite() {
                continue;
            }
            let p = primary_damper[pi] - primary_mean;
            let c = compare_damper[ci] - compare_mean;
            covariance += p * c;
            primary_variance += p * p;
            compare_variance += c * c;
        }
        let denominator = (primary_variance * compare_variance).sqrt();
        let score = if denominator > 1e-9 {
            covariance / denominator
        } else {
            -1.0
        };
        if score > best_score {
            best_score = score;
            best_index = compare_index;
        }
    }
    if best_index == compare.time.len() || best_score < 0.25 {
        return None;
    }
    Some(compare.time[best_index])
}

fn pre_corner_damper_anchors(
    primary: &UnifiedLap,
    compare: &UnifiedLap,
    base_times: &[f64],
    corner_starts: &[f64],
) -> Vec<Anchor> {
    let mut anchors: Vec<Anchor> = Vec::new();
    let primary_damper = front_damper_series(primary);
    let compare_damper = front_damper_series(compare);
    if primary_damper.is_empty() || compare_damper.is_empty() {
        return anchors;
    }
    for &fraction in corner_starts {
        let index = llround(clamp(fraction, 0.0, 1.0) * (primary.time.len() - 1) as f64) as usize;
        let Some(compare_time) = damper_time_at_corner(
            primary,
            &primary_damper,
            index,
            compare,
            &compare_damper,
            base_times[index],
        ) else {
            continue;
        };
        let matched = monotonic::lower_bound(&compare.time, compare_time);
        if matched >= compare.time.len() || !speeds_agree(primary, index, compare, matched) {
            continue;
        }
        if let Some(last) = anchors.last()
            && (index <= last.primary_index || compare_time <= last.compare_time)
        {
            continue;
        }
        anchors.push(Anchor {
            primary_index: index,
            compare_time,
        });
    }
    anchors
}

fn apply_anchors(times: &mut [f64], anchors: &[Anchor], compare: &UnifiedLap, median_filter: bool) {
    if anchors.is_empty() {
        return;
    }
    let mut corrections: Vec<f64> = anchors
        .iter()
        .map(|anchor| anchor.compare_time - times[anchor.primary_index])
        .collect();
    if median_filter && corrections.len() >= 3 {
        let filtered: Vec<f64> = (0..corrections.len())
            .map(|i| {
                let begin = i.saturating_sub(2);
                let end = (i + 3).min(corrections.len());
                let mut window = corrections[begin..end].to_vec();
                let middle = window.len() / 2;
                window.select_nth_unstable_by(middle, |a, b| {
                    a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal)
                });
                window[middle]
            })
            .collect();
        corrections = filtered;
    }
    let first_index = anchors[0].primary_index;
    let last_index = anchors[anchors.len() - 1].primary_index;
    let compare_front = compare.time[0];
    let compare_back = compare.time[compare.time.len() - 1];
    let mut anchor = 0usize;
    for i in 0..times.len() {
        let mut correction = corrections[0];
        if i >= last_index {
            correction = corrections[corrections.len() - 1];
        } else if i > first_index {
            while anchor + 1 < anchors.len() && anchors[anchor + 1].primary_index < i {
                anchor += 1;
            }
            correction = corrections[anchor];
            if anchor + 1 < anchors.len() {
                let span = anchors[anchor + 1].primary_index - anchors[anchor].primary_index;
                let local = if span > 0 {
                    (i - anchors[anchor].primary_index) as f64 / span as f64
                } else {
                    0.0
                };
                correction += local * (corrections[anchor + 1] - correction);
            }
        }
        times[i] = clamp(times[i] + correction, compare_front, compare_back);
        if i > 0 {
            times[i] = max(times[i], times[i - 1]);
        }
    }
}

fn is_monotonic_non_decreasing(values: &[f64]) -> bool {
    values.windows(2).all(|w| !(w[1] < w[0] - 1e-9))
}

fn build_fractions(result: &mut AlignmentResult, compare: &UnifiedLap) {
    if !is_monotonic_non_decreasing(&compare.time) {
        result.fraction.clear();
        result.time.clear();
        result.rejection_reason = "compare time is not monotonic".to_string();
        return;
    }
    if !is_monotonic_non_decreasing(&result.time) {
        result.fraction.clear();
        result.time.clear();
        result.rejection_reason = "aligned time is not monotonic".to_string();
        return;
    }
    let n = compare.time.len();
    result.fraction = vec![0.0; result.time.len()];
    let mut high = 1usize;
    for i in 0..result.time.len() {
        let time = result.time[i];
        while high < n && compare.time[high] < time {
            high += 1;
        }
        result.fraction[i] = if high >= n {
            1.0
        } else if time <= compare.time[0] {
            0.0
        } else {
            let low = high - 1;
            let span = compare.time[high] - compare.time[low];
            let local = if span > 0.0 {
                (time - compare.time[low]) / span
            } else {
                0.0
            };
            (low as f64 + local) / (n - 1) as f64
        };
    }
}

/// Both laps carry enough GPS to offer the GPS strategy.
pub fn gps_available(primary: &UnifiedLap, compare: &UnifiedLap) -> bool {
    gps_coverage_available(primary) && gps_coverage_available(compare)
}

/// The lap-percentage base uses lap distance.
pub fn distance_base_available(primary: &UnifiedLap, compare: &UnifiedLap) -> bool {
    distance_base_usable(primary, compare)
}

/// Both laps carry front-damper data that actually moves.
pub fn damper_available(primary: &UnifiedLap, compare: &UnifiedLap) -> bool {
    front_damper_available(primary) && front_damper_available(compare)
}

/// Compute the primary -> compare map. Pure. An unavailable strategy
/// degrades to lap percentage rather than manufacturing an alignment.
pub fn compute(primary: &UnifiedLap, compare: &UnifiedLap, options: &Options) -> AlignmentResult {
    let mut result = AlignmentResult::default();
    if primary.time.len() < 2 || compare.time.len() < 2 {
        return result;
    }
    if !is_monotonic_non_decreasing(&primary.time) {
        result.rejection_reason = "primary time is not monotonic".to_string();
        return result;
    }
    if !is_monotonic_non_decreasing(&compare.time) {
        result.rejection_reason = "compare time is not monotonic".to_string();
        return result;
    }
    let (base_times, distance_base) = lap_percentage(primary, compare);
    result.time = base_times;
    result.distance_base = distance_base;
    let base = if distance_base {
        BASIS_LAP_DISTANCE
    } else {
        BASIS_LAP_TIME
    };

    match options.strategy {
        Strategy::Gps => {
            let gps = validated_gps_anchors(primary, compare, &result.time);
            result.gps_rejected = gps.rejected;
            result.basis = base.to_string();
            if !gps.anchors.is_empty() {
                let mut corrected = result.time.clone();
                apply_anchors(&mut corrected, &gps.anchors, compare, gps.continuous);
                // GPS has to earn its place.
                if mapped_speed_disagreement(primary, compare, &corrected)
                    <= mapped_speed_disagreement(primary, compare, &result.time)
                {
                    result.time = corrected;
                    result.gps_anchors = gps.anchors.len() as i32;
                    result.basis = if gps.continuous {
                        BASIS_GPS_CONTINUOUS
                    } else {
                        BASIS_GPS_RESYNC
                    }
                    .to_string();
                } else {
                    result.gps_rejected += gps.anchors.len() as i32;
                }
            }
        }
        Strategy::PreCornerDampers => {
            let anchors =
                pre_corner_damper_anchors(primary, compare, &result.time, &options.corner_starts);
            if anchors.is_empty() {
                result.basis = base.to_string();
            } else {
                apply_anchors(&mut result.time, &anchors, compare, false);
                result.basis = BASIS_DAMPERS_PRE_CORNER.to_string();
            }
        }
        Strategy::ManualDampers => result.basis = BASIS_DAMPERS_MANUAL.to_string(),
        Strategy::LapPercentage => result.basis = base.to_string(),
    }
    build_fractions(&mut result, compare);
    result
}

fn alignment_map_usable(map: &[f64]) -> bool {
    map.len() >= 2 && map[map.len() - 1] - map[0] >= 0.01
}

/// Compare-lap fraction for a primary fraction. An empty or degenerate map
/// is identity: "not yet aligned" must never read as "at the lap start".
pub fn interpolate_fraction(map: &[f64], primary_fraction: f64) -> f64 {
    if map.len() < 2 || !(map[map.len() - 1] - map[0] >= 0.01) {
        return clamp(primary_fraction, 0.0, 1.0);
    }
    monotonic::interpolate_fraction(map, primary_fraction)
}

/// Inverse of [`interpolate_fraction`], same identity fallback.
pub fn invert_fraction(map: &[f64], compare_fraction: f64) -> f64 {
    if !alignment_map_usable(map) {
        return clamp(compare_fraction, 0.0, 1.0);
    }
    monotonic::invert_fraction(map, compare_fraction)
}

fn precise_gps_at(lap: &UnifiedLap, fraction: f64, max_accuracy: f64) -> Option<(f64, f64)> {
    if !gps_arrays_available(lap)
        || lap.time.len() < 2
        || !fraction.is_finite()
        || !(0.0..=1.0).contains(&fraction)
    {
        return None;
    }
    let position = fraction * (lap.time.len() - 1) as f64;
    let low = (position as usize).min(lap.time.len() - 2);
    let high = low + 1;
    for i in [low, high] {
        let accuracy = lap.gps_position_accuracy[i];
        if !gps_fix_usable(lap.gps_lat[i], lap.gps_lon[i], accuracy) || !(accuracy < max_accuracy) {
            return None;
        }
    }
    let local = clamp(position - low as f64, 0.0, 1.0);
    Some((
        lap.gps_lat[low] + (lap.gps_lat[high] - lap.gps_lat[low]) * local,
        lap.gps_lon[low] + (lap.gps_lon[high] - lap.gps_lon[low]) * local,
    ))
}

/// Signed along-track metres from the primary car to the reference car
/// (positive: reference ahead). `None` unless both fixes are better than
/// `max_accuracy_meters`, the primary is moving, and the cars are on the
/// same stretch of track.
pub fn relative_along_track_meters(
    primary: &UnifiedLap,
    primary_fraction: f64,
    compare: &UnifiedLap,
    compare_fraction: f64,
    max_accuracy_meters: f64,
) -> Option<f64> {
    const MAXIMUM_LATERAL_METERS: f64 = 25.0;
    let from = precise_gps_at(primary, primary_fraction, max_accuracy_meters)?;
    let to = precise_gps_at(compare, compare_fraction, max_accuracy_meters)?;
    let index =
        llround(clamp(primary_fraction, 0.0, 1.0) * (primary.time.len() - 1) as f64) as usize;
    let heading = travel_heading(primary, index)?;
    let mean_latitude = 0.5 * (from.0 + to.0) * PI / 180.0;
    let north = (to.0 - from.0) * METERS_PER_DEGREE;
    let east = (to.1 - from.1) * METERS_PER_DEGREE * mean_latitude.cos();
    let along = north * heading.north + east * heading.east;
    let lateral = east * heading.north - north * heading.east;
    if lateral.abs() > MAXIMUM_LATERAL_METERS {
        return None;
    }
    Some(along)
}

/// `HIGH` / `MED` / `LOW` / `NONE` for a basis and anchor count.
pub fn confidence_label(basis: &str, gps_anchors: i32) -> &'static str {
    if basis.is_empty() {
        return "NONE";
    }
    if basis == BASIS_GPS_CONTINUOUS {
        return if gps_anchors >= 2 { "HIGH" } else { "LOW" };
    }
    if basis == BASIS_GPS_RESYNC || basis == BASIS_DAMPERS_PRE_CORNER || basis == BASIS_LAP_DISTANCE
    {
        return "MED";
    }
    "LOW"
}
