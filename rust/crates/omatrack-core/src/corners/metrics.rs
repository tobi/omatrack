//! Per-lap corner metrics: the one pass over a corner's samples every check
//! reads from (port of `measureCorner` / `detectTurnIn` in
//! `CornerAnalysis.cpp`, itself ported from ac-tracer's corner analysis).

use crate::num::{clamp_i, max, min, trunc_i32};
use crate::unify::UnifiedLap;

/// Brake considered applied.
pub const BRAKE_ON_BAR: f64 = 2.0;
/// A corner braked for in earnest.
pub const BRAKE_ZONE_BAR: f64 = 20.0;
/// "Braking hard".
pub const HEAVY_BRAKE_BAR: f64 = 30.0;
/// Above a heel-toe blip.
pub const BLIP_THROTTLE: f64 = 0.30;
/// Longest allowed brake/throttle overlap.
pub const BLIP_SECONDS: f64 = 0.5;
/// Sustained throttle application.
pub const THROTTLE_ON_FRACTION: f64 = 0.9;
/// Throttle lift-off.
pub const LIFT_THROTTLE: f64 = 0.9;
/// Application must hold this long.
pub const SUSTAINED_SECONDS: f64 = 0.2;
/// Blip window around a gear shift.
pub const GEAR_SHIFT_SECONDS: f64 = 0.5;
/// Fewest samples a turn-in search needs.
pub const MIN_SAMPLES: i32 = 8;

/// Everything the checks need about one lap through one corner.
///
/// Distances are metres from the corner start; times seconds; speeds km/h; brake bar;
/// steering degrees. An undetermined value is NaN (or -1 for indices).
#[derive(Debug, Clone, PartialEq)]
pub struct CornerMetrics {
    pub valid: bool,
    pub first_index: i32,
    pub last_index: i32,
    pub apex_index: i32,
    pub turn_in_index: i32,
    pub brake_index: i32,
    pub lift_index: i32,
    pub throttle_index: i32,
    pub start_distance: f64,
    pub length_meters: f64,
    pub time: f64,
    /// Fastest before the apex.
    pub entry_speed: f64,
    /// Slowest in the zone.
    pub apex_speed: f64,
    /// Fastest after the apex.
    pub exit_speed: f64,
    pub brake_point: f64,
    pub lift_point: f64,
    pub turn_in_point: f64,
    pub apex_point: f64,
    pub throttle_point: f64,
    /// Lift-off to brake application.
    pub coast_meters: f64,
    pub max_steering: f64,
    pub max_brake: f64,
    pub min_throttle: f64,
    pub min_gear: i32,
    /// Brake ramp 10% -> 90% of this corner's peak, bar/s.
    pub brake_rise_rate: f64,
    /// Peak brake pressure to release.
    pub trail_brake_seconds: f64,
    /// Longest throttle application while braking hard.
    pub brake_throttle_overlap_seconds: f64,
    /// Brake application to first/last downshift.
    pub downshift_first_ms: f64,
    pub downshift_last_ms: f64,
    pub downshift_distance: f64,
    /// Combined lateral + braking acceleration, turn-in->mid and mid->apex.
    pub combined_grip_early: f64,
    pub combined_grip_mid: f64,
    pub peak_lateral_g: f64,
    pub has_lateral_g: bool,
}

impl Default for CornerMetrics {
    fn default() -> Self {
        Self {
            valid: false,
            first_index: -1,
            last_index: -1,
            apex_index: -1,
            turn_in_index: -1,
            brake_index: -1,
            lift_index: -1,
            throttle_index: -1,
            start_distance: 0.0,
            length_meters: 0.0,
            time: 0.0,
            entry_speed: 0.0,
            apex_speed: 0.0,
            exit_speed: 0.0,
            brake_point: 0.0,
            lift_point: 0.0,
            turn_in_point: 0.0,
            apex_point: 0.0,
            throttle_point: 0.0,
            coast_meters: 0.0,
            max_steering: 0.0,
            max_brake: 0.0,
            min_throttle: 1.0,
            min_gear: 0,
            brake_rise_rate: 0.0,
            trail_brake_seconds: 0.0,
            brake_throttle_overlap_seconds: 0.0,
            downshift_first_ms: 0.0,
            downshift_last_ms: 0.0,
            downshift_distance: 0.0,
            combined_grip_early: 0.0,
            combined_grip_mid: 0.0,
            peak_lateral_g: 0.0,
            has_lateral_g: false,
        }
    }
}

/// Turn-in: find where lateral load starts building, then walk back to where
/// the steering left its approach baseline. Steering alone is the fallback
/// without an accelerometer channel.
#[expect(
    clippy::cast_sign_loss,
    reason = "Preserve the C++ port's sample-index widths and rounding at this numerical boundary; verified by parity."
)]
#[expect(
    clippy::neg_cmp_op_on_partial_ord,
    reason = "Negated ordered comparisons deliberately include unordered (NaN) values; preserve that behavior."
)]
fn detect_turn_in(lap: &UnifiedLap, first: i32, apex: i32, has_lateral_g: bool) -> i32 {
    const SUSTAINED: i32 = 3;

    let count = apex - first + 1;
    if count < MIN_SAMPLES || lap.steering.len() < (apex + 1) as usize {
        return -1;
    }
    let steer = |i: i32| lap.steering[i as usize].abs();
    let lat = |i: i32| lap.g_force_lat[i as usize].abs();
    let mut peak_steer = 0.0;
    let mut peak_lat = 0.0;
    for i in first..=apex {
        peak_steer = max(peak_steer, steer(i));
        if has_lateral_g {
            peak_lat = max(peak_lat, lat(i));
        }
    }
    // The first 15% of the approach is the baseline.
    let baseline_count = 3.max(10.min(count * 15 / 100));
    let mut baseline_steer = 0.0;
    let mut baseline_lat = 0.0;
    for i in first..first + baseline_count {
        baseline_steer += steer(i);
        if has_lateral_g {
            baseline_lat += lat(i);
        }
    }
    baseline_steer /= f64::from(baseline_count);
    baseline_lat /= f64::from(baseline_count);

    let steer_range = peak_steer - baseline_steer;
    if steer_range < 5.0 {
        return -1;
    }
    let committed_steer = baseline_steer + steer_range * 0.45;
    let lat_range = peak_lat - baseline_lat;
    let lat_threshold = baseline_lat + max(0.12, lat_range * 0.18);

    let mut onset = -1;
    let use_lateral = has_lateral_g && lat_range >= 0.15;
    let mut i = first + baseline_count;
    while i <= apex - SUSTAINED + 1 {
        let sustained = (i..i + SUSTAINED).all(|k| {
            let value = if use_lateral { lat(k) } else { steer(k) };
            let threshold = if use_lateral {
                lat_threshold
            } else {
                committed_steer
            };
            !(value < threshold)
        });
        if sustained {
            onset = i;
            break;
        }
        i += 1;
    }
    if onset < 0 {
        return -1;
    }
    if use_lateral {
        // The lateral event must belong to a committed steering input, not
        // a bump or a kerb strike.
        let from = (first + baseline_count).max(onset - 6);
        let to = (onset + 3).min(apex);
        if !(from..=to).any(|i| steer(i) >= committed_steer) {
            return -1;
        }
    }
    // Walk back through the same contiguous steering build.
    let baseline_exit = baseline_steer + max(1.0, steer_range * 0.07);
    let search_start = (first + baseline_count).max(onset - 20);
    while onset > search_start && steer(onset - 1) > baseline_exit {
        onset -= 1;
    }
    onset
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_sign_loss,
    reason = "Preserve the C++ port's sample-index widths and rounding at this numerical boundary; verified by parity."
)]
fn near_gear_shift(lap: &UnifiedLap, index: i32, window: i32) -> bool {
    if lap.gear.len() < 2 {
        return false;
    }
    let last = lap.gear.len() as i32 - 1;
    let gear = lap.gear[index.clamp(0, last) as usize];
    ((index - window).max(0)..=last.min(index + window)).any(|i| lap.gear[i as usize] != gear)
}

/// Measure one corner of one lap; `start_fraction`/`end_fraction` are lap
/// fractions. The caller maps a reference zone through the shared
/// primary -> reference station map first.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_sign_loss,
    reason = "Preserve the C++ port's sample-index widths and rounding at this numerical boundary; verified by parity."
)]
#[expect(
    clippy::too_many_lines,
    reason = "Keep the ported analysis/report stages in source order so numerical and CLI parity remain auditable."
)]
#[expect(
    clippy::neg_cmp_op_on_partial_ord,
    reason = "Negated ordered comparisons deliberately include unordered (NaN) values; preserve that behavior."
)]
pub fn measure_corner(
    lap: &UnifiedLap,
    start_fraction: f64,
    end_fraction: f64,
    allow_lateral_g: bool,
) -> CornerMetrics {
    let mut m = CornerMetrics::default();
    let last = lap.len() as i32 - 1;
    if last < 2 || lap.distance.len() < (last + 1) as usize || lap.time.len() < (last + 1) as usize
    {
        return m;
    }
    let first = clamp_i(
        i64::from(trunc_i32((start_fraction * f64::from(last)).floor())),
        0,
        i64::from(last),
    ) as i32;
    let finish = clamp_i(
        i64::from(trunc_i32((end_fraction * f64::from(last)).ceil())),
        i64::from(first),
        i64::from(last),
    ) as i32;
    if finish - first < 2 {
        return m;
    }
    let at = |values: &[f64], i: i32| values[i as usize];

    m.valid = true;
    m.first_index = first;
    m.last_index = finish;
    m.start_distance = at(&lap.distance, first);
    m.length_meters = max(1.0, at(&lap.distance, finish) - m.start_distance);
    m.time = at(&lap.time, finish) - at(&lap.time, first);
    // An unmapped channel unifies to zeros, so the lap "has" lateral G only
    // when the corner actually carries a signal.
    let lateral_sized = lap.g_force_lat.len() >= (finish + 1) as usize;
    let start_distance = m.start_distance;
    let distance_from = |i: i32| at(&lap.distance, i) - start_distance;

    let mut apex_speed = f64::INFINITY;
    let mut apex_index = first;
    let mut min_gear = 99;
    for i in first..=finish {
        let speed = at(&lap.speed, i);
        if speed.is_finite() && speed < apex_speed {
            apex_speed = speed;
            apex_index = i;
        }
        m.max_steering = max(m.max_steering, at(&lap.steering, i).abs());
        m.max_brake = max(m.max_brake, at(&lap.brake, i));
        m.min_throttle = min(m.min_throttle, at(&lap.throttle, i));
        if (i as usize) < lap.gear.len() {
            min_gear = min_gear.min(lap.gear[i as usize]);
        }
        if lateral_sized {
            let lateral = at(&lap.g_force_lat, i).abs();
            if lateral.is_finite() && lateral > 1.0e-6 {
                m.has_lateral_g = true;
            }
            m.peak_lateral_g = max(m.peak_lateral_g, lateral);
        }
    }
    m.apex_index = apex_index;
    m.apex_speed = if apex_speed.is_finite() {
        apex_speed
    } else {
        0.0
    };
    m.apex_point = distance_from(apex_index);
    m.min_gear = if min_gear == 99 { 0 } else { min_gear };

    // Entry: fastest before the apex; exit: fastest after it.
    let mut entry = 0.0;
    for i in first..=apex_index {
        entry = max(entry, at(&lap.speed, i));
    }
    let mut exit = 0.0;
    for i in apex_index..=finish {
        exit = max(exit, at(&lap.speed, i));
    }
    m.entry_speed = entry;
    m.exit_speed = exit;

    for i in first..=finish {
        if m.brake_index < 0 && at(&lap.brake, i) > BRAKE_ON_BAR {
            m.brake_index = i;
        }
        if m.lift_index < 0 && at(&lap.throttle, i) < LIFT_THROTTLE {
            m.lift_index = i;
        }
    }
    m.brake_point = if m.brake_index >= 0 {
        distance_from(m.brake_index)
    } else {
        f64::NAN
    };
    m.lift_point = if m.lift_index >= 0 {
        distance_from(m.lift_index)
    } else {
        f64::NAN
    };
    m.coast_meters = if m.brake_index >= 0 && m.lift_index >= 0 {
        max(0.0, m.brake_point - m.lift_point)
    } else {
        f64::NAN
    };

    let turn_in = detect_turn_in(lap, first, apex_index, m.has_lateral_g && allow_lateral_g);
    m.turn_in_index = turn_in;
    m.turn_in_point = if turn_in >= 0 {
        distance_from(turn_in)
    } else {
        f64::NAN
    };

    // Throttle pickup: first sustained application after the apex that is
    // not a rev-matching blip around a shift.
    let rate = lap.sample_rate.max(1);
    let sustained_samples = 1.max(crate::num::llround(SUSTAINED_SECONDS * f64::from(rate)) as i32);
    let shift_window = 1.max(crate::num::llround(GEAR_SHIFT_SECONDS * f64::from(rate)) as i32);
    for i in apex_index..=finish {
        if at(&lap.throttle, i) < THROTTLE_ON_FRACTION {
            continue;
        }
        if near_gear_shift(lap, i, shift_window) {
            continue;
        }
        let sustained = (i..i + sustained_samples)
            .take_while(|&k| k <= finish)
            .all(|k| !(at(&lap.throttle, k) < THROTTLE_ON_FRACTION));
        if !sustained {
            continue;
        }
        m.throttle_index = i;
        break;
    }
    m.throttle_point = if m.throttle_index >= 0 {
        distance_from(m.throttle_index)
    } else {
        f64::NAN
    };

    // Brake pressure ramp: 10% -> 90% of this corner's peak.
    m.brake_rise_rate = f64::NAN;
    if m.max_brake >= 20.0 {
        let onset_bar = max(5.0, m.max_brake * 0.1);
        let target_bar = m.max_brake * 0.9;
        let mut onset_time = f64::NAN;
        for i in first..=finish {
            let pressure = at(&lap.brake, i);
            if !onset_time.is_finite() && pressure >= onset_bar {
                onset_time = at(&lap.time, i);
            }
            if onset_time.is_finite() && pressure >= target_bar {
                let rise = max(0.02, at(&lap.time, i) - onset_time);
                m.brake_rise_rate = (target_bar - onset_bar) / rise;
                break;
            }
        }
    }

    // Trail braking: peak pressure to release.
    m.trail_brake_seconds = f64::NAN;
    if m.max_brake >= 20.0 {
        let mut peak_index = -1;
        let mut peak = 0.0;
        for i in first..=finish {
            if at(&lap.brake, i) > peak {
                peak = at(&lap.brake, i);
                peak_index = i;
            }
        }
        if peak_index >= 0 {
            let release_bar = max(5.0, peak * 0.1);
            let mut below = 0;
            for i in peak_index + 1..=finish {
                if at(&lap.brake, i) <= release_bar {
                    below += 1;
                    if below >= 3 {
                        m.trail_brake_seconds =
                            max(0.0, at(&lap.time, i - 2) - at(&lap.time, peak_index));
                        break;
                    }
                } else {
                    below = 0;
                }
            }
        }
    }

    // Throttle held while braking hard, beyond a heel-toe blip.
    let mut overlap_run = 0;
    let mut longest_overlap = 0;
    for i in first..=finish {
        if at(&lap.brake, i) >= HEAVY_BRAKE_BAR && at(&lap.throttle, i) >= BLIP_THROTTLE {
            overlap_run += 1;
            longest_overlap = longest_overlap.max(overlap_run);
        } else {
            overlap_run = 0;
        }
    }
    m.brake_throttle_overlap_seconds = f64::from(longest_overlap) / f64::from(rate);

    // Downshift sequence, measured from brake application.
    m.downshift_first_ms = f64::NAN;
    m.downshift_last_ms = f64::NAN;
    m.downshift_distance = f64::NAN;
    if m.brake_index >= 0 && lap.gear.len() > finish as usize {
        let brake_time = at(&lap.time, m.brake_index);
        let mut previous = lap.gear[m.brake_index as usize];
        for i in m.brake_index + 1..=finish {
            let gear = lap.gear[i as usize];
            if gear < previous {
                let elapsed = (at(&lap.time, i) - brake_time) * 1000.0;
                if !m.downshift_first_ms.is_finite() {
                    m.downshift_first_ms = elapsed;
                    m.downshift_distance = distance_from(i) - m.brake_point;
                }
                m.downshift_last_ms = elapsed;
            }
            previous = gear;
        }
    }

    // Combined lateral + braking grip.
    m.combined_grip_early = f64::NAN;
    m.combined_grip_mid = f64::NAN;
    if m.has_lateral_g && turn_in >= 0 && apex_index > turn_in + 1 {
        let split = i32::midpoint(turn_in, apex_index);
        let average = |from: i32, to: i32| -> f64 {
            let mut sum = 0.0;
            let mut count = 0;
            for i in from..=to {
                let lateral = at(&lap.g_force_lat, i);
                let longitudinal = if (i as usize) < lap.g_force_long.len() {
                    at(&lap.g_force_long, i)
                } else {
                    0.0
                };
                if !lateral.is_finite() || !longitudinal.is_finite() {
                    continue;
                }
                let braking = max(0.0, -longitudinal);
                sum += (lateral * lateral + braking * braking).sqrt();
                count += 1;
            }
            if count >= 2 {
                sum / f64::from(count)
            } else {
                f64::NAN
            }
        };
        m.combined_grip_early = average(turn_in, split);
        m.combined_grip_mid = average(split + 1, apex_index);
    }
    m
}
