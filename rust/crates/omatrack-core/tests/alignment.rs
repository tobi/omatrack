//! Port of tests/AlignmentTest.cpp: the comparison-alignment strategies
//! shared by traces, delta, cursor readouts and synchronized video.

#![cfg(test)]

use omatrack_core::alignment::{
    self, AlignmentResult, Options, Strategy, compute, confidence_label,
    relative_along_track_meters,
};
use omatrack_core::comparison::Comparison;
use omatrack_core::{DistanceSource, UnifiedLap};
use std::f64::consts::PI;
use std::sync::Arc;

const METERS_PER_DEGREE: f64 = 111_320.0;

/// Positions follow the speed channel along a straight north-east line.
fn place_on_line(meters: f64) -> (f64, f64) {
    let north = meters * 0.8;
    let east = meters * 0.6;
    (
        43.0 + north / METERS_PER_DEGREE,
        -88.0 + east / (METERS_PER_DEGREE * (43.0 * PI / 180.0).cos()),
    )
}

#[expect(
    clippy::cast_precision_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn make_lap(samples: usize, gps: bool, dampers: bool) -> UnifiedLap {
    let mut lap = UnifiedLap {
        sample_rate: 50,
        distance_source: DistanceSource::SpeedFused,
        ..Default::default()
    };
    let mut meters = 0.0;
    for i in 0..samples {
        let fraction = if samples > 1 {
            i as f64 / (samples - 1) as f64
        } else {
            0.0
        };
        let speed = 145.0 + 50.0 * (6.0 * PI * fraction).sin();
        if i > 0 {
            meters += speed / 3.6 / 50.0;
        }
        lap.time.push(i as f64 / 50.0);
        lap.speed.push(speed);
        lap.distance.push(meters);
        if gps {
            let (lat, lon) = place_on_line(meters);
            lap.gps_lat.push(lat);
            lap.gps_lon.push(lon);
            lap.gps_position_accuracy.push(1.0);
        }
        if dampers {
            let x = i as f64;
            let value = (0.0017 * x * x).sin() + 0.35 * (0.19 * x).sin() + 0.12 * (0.047 * x).cos();
            lap.damper_fl.push(value);
            lap.damper_fr.push(value + 0.04 * (0.31 * x).sin());
        }
    }
    lap
}

/// The compare lap drives the primary's line on a warped clock.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn warped_lap(primary: &UnifiedLap, amplitude: f64) -> UnifiedLap {
    let mut lap = primary.clone();
    let total = *primary.time.last().unwrap();
    let sample_at = |values: &[f64], t: f64| {
        let position = (t * 50.0).clamp(0.0, (values.len() - 1) as f64);
        let low = (position as usize).min(values.len() - 2);
        values[low] + (values[low + 1] - values[low]) * (position - low as f64)
    };
    for i in 0..lap.time.len() {
        let t = lap.time[i];
        let tau = t + amplitude * (PI * t / total).sin();
        let rate = 1.0 + amplitude * PI / total * (PI * t / total).cos();
        lap.speed[i] = sample_at(&primary.speed, tau) * rate;
        lap.distance[i] = sample_at(&primary.distance, tau);
        if !lap.gps_lat.is_empty() {
            let (lat, lon) = place_on_line(lap.distance[i]);
            lap.gps_lat[i] = lat;
            lap.gps_lon[i] = lon;
        }
    }
    lap
}

fn true_compare_time(primary_time: f64, total: f64, amplitude: f64) -> f64 {
    let mut t = primary_time;
    for _ in 0..40 {
        t -= (t + amplitude * (PI * t / total).sin() - primary_time)
            / (1.0 + amplitude * PI / total * (PI * t / total).cos());
    }
    t
}

fn run(
    primary: &UnifiedLap,
    compare: &UnifiedLap,
    strategy: Strategy,
    corners: &[f64],
) -> AlignmentResult {
    compute(
        primary,
        compare,
        &Options {
            strategy,
            corner_starts: corners.to_vec(),
        },
    )
}

fn monotonic(values: &[f64]) -> bool {
    values.windows(2).all(|w| w[1] + 1e-9 >= w[0])
}

#[test]
#[expect(
    clippy::float_cmp,
    reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
)]
fn non_owning_lookup_matches_and_falls_back() {
    let map = [0.03, 0.2, 0.7, 0.98];
    for f in [-0.1, 0.0, 0.15, 0.5, 0.95, 1.1] {
        assert_eq!(
            alignment::interpolate_fraction(&map, f),
            omatrack_core::monotonic::interpolate_fraction(&map, f)
        );
    }
    assert_eq!(alignment::interpolate_fraction(&[], 0.3), 0.3);
    assert_eq!(alignment::interpolate_fraction(&[0.2, 0.2], 0.3), 0.3);
    assert_eq!(alignment::interpolate_fraction(&[0.2, f64::NAN], 0.3), 0.3);
}

#[test]
#[expect(
    clippy::cast_precision_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn lap_percentage_uses_time_over_speed_fused_distance() {
    const N: usize = 1000;
    let mut primary = make_lap(N, false, false);
    let mut compare = primary.clone();
    for i in 0..N {
        let p = i as f64 / (N - 1) as f64;
        primary.distance[i] = 1000.0 * (p - 0.05 * (PI * p).sin());
        compare.distance[i] = 1000.0 * (p + 0.08 * (PI * p).sin());
    }
    let result = run(&primary, &compare, Strategy::LapPercentage, &[]);
    assert_eq!(result.basis, "Lap time %");
    for index in [100, 500, 780, 900] {
        assert!((result.fraction[index] - index as f64 / (N - 1) as f64).abs() < 1e-6);
    }
}

#[test]
fn lap_percentage_uses_native_distance_when_totals_agree() {
    let mut primary = make_lap(1500, false, false);
    primary.distance_source = DistanceSource::Native;
    let amplitude = 1.5;
    let mut compare = warped_lap(&primary, amplitude);
    compare.distance_source = DistanceSource::Native;
    let result = run(&primary, &compare, Strategy::LapPercentage, &[]);
    assert_eq!(result.basis, "Lap distance %");
    let total = *primary.time.last().unwrap();
    for index in [300, 750, 1200] {
        let want = true_compare_time(primary.time[index], total, amplitude);
        assert!(
            (result.time[index] - want).abs() < 0.03,
            "{} vs {want}",
            result.time[index]
        );
    }
    for d in &mut compare.distance {
        *d *= 1.05;
    }
    assert_eq!(
        run(&primary, &compare, Strategy::LapPercentage, &[]).basis,
        "Lap time %"
    );
}

#[test]
fn verified_gps_corrects_variable_track_progress() {
    let primary = make_lap(1500, true, false);
    let amplitude = 1.5;
    let compare = warped_lap(&primary, amplitude);
    let percentage = run(&primary, &compare, Strategy::LapPercentage, &[]);
    let gps = run(&primary, &compare, Strategy::Gps, &[]);
    assert_eq!(gps.basis, "GPS · continuous");
    assert!(gps.gps_anchors >= 8);
    assert!(monotonic(&gps.fraction));
    assert!(
        gps.fraction
            .iter()
            .all(|f| (-1e-9..=1.0 + 1e-9).contains(f))
    );
    let want = true_compare_time(primary.time[750], *primary.time.last().unwrap(), amplitude);
    assert!((percentage.time[750] - want).abs() > 1.0);
    assert!(
        (gps.time[750] - want).abs() < 0.1,
        "{} vs {want}",
        gps.time[750]
    );
}

#[test]
#[expect(
    clippy::cast_precision_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn patchy_gps_resyncs_where_it_is_good() {
    let mut primary = make_lap(1500, true, false);
    let amplitude = 1.5;
    let mut compare = warped_lap(&primary, amplitude);
    for lap in [&mut primary, &mut compare] {
        let n = lap.time.len();
        for i in 0..n {
            let f = i as f64 / (n - 1) as f64;
            if (f > 0.26 && f < 0.37) || (f > 0.63 && f < 0.74) {
                continue;
            }
            lap.gps_lat[i] = f64::NAN;
            lap.gps_lon[i] = f64::NAN;
        }
    }
    let gps = run(&primary, &compare, Strategy::Gps, &[]);
    assert_eq!(gps.basis, "GPS · re-sync");
    let total = *primary.time.last().unwrap();
    for index in [470, 1030] {
        let want = true_compare_time(primary.time[index], total, amplitude);
        assert!((gps.time[index] - want).abs() < 0.1);
    }
}

#[test]
fn gps_that_disagrees_with_the_car_is_ignored() {
    let primary = make_lap(1500, true, false);
    let mut compare = primary.clone();
    for i in 0..compare.time.len() {
        let (lat, lon) = place_on_line(compare.distance[i.saturating_sub(150)]);
        compare.gps_lat[i] = lat;
        compare.gps_lon[i] = lon;
    }
    let gps = run(&primary, &compare, Strategy::Gps, &[]);
    assert_eq!(gps.basis, "Lap time %");
    assert_eq!(gps.gps_anchors, 0);
    assert!(gps.gps_rejected > 0);
    assert!((gps.fraction[700] - 700.0 / 1499.0).abs() < 1e-6);
}

#[test]
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn gps_anchors_reject_the_other_leg_of_a_hairpin() {
    const SPEED: f64 = 20.0;

    const RATE: f64 = 50.0;
    const LEG: f64 = 10.0;
    const LAG: f64 = 0.5;
    let samples = ((2.0 * LEG + LAG) * RATE) as usize + 1;
    let lon_scale = 1.0 / (METERS_PER_DEGREE * (43.0 * PI / 180.0).cos());
    let position = |t: f64| {
        let t = t.clamp(0.0, 2.0 * LEG);
        let north = if t <= LEG {
            SPEED * t
        } else {
            SPEED * (2.0 * LEG - t)
        };
        let east = if t <= LEG { 0.0 } else { 8.0 };
        (43.0 + north / METERS_PER_DEGREE, -88.0 + east * lon_scale)
    };
    let mut primary = make_lap(samples, true, false);
    primary.speed.iter_mut().for_each(|s| *s = SPEED * 3.6);
    let mut compare = primary.clone();
    for i in 0..samples {
        let t = i as f64 / RATE;
        (primary.gps_lat[i], primary.gps_lon[i]) = position(t);
        (compare.gps_lat[i], compare.gps_lon[i]) = position(t - LAG);
    }
    let result = run(&primary, &compare, Strategy::Gps, &[]);
    assert_eq!(result.basis, "GPS · continuous");
    for t in [3.0, 7.0, 8.0, 9.0, 12.0, 15.0] {
        let mapped = result.time[(t * RATE) as usize];
        assert!(
            (mapped - (t + LAG)).abs() < 0.25,
            "t={t} mapped to {mapped}"
        );
    }
}

#[test]
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn pre_corner_dampers_match_local_signature() {
    const N: usize = 1200;
    const SHIFT: usize = 11;
    let primary = make_lap(N, false, true);
    let mut compare = primary.clone();
    for i in 0..N {
        let source = i.saturating_sub(SHIFT);
        compare.damper_fl[i] = primary.damper_fl[source];
        compare.damper_fr[i] = primary.damper_fr[source];
    }
    let result = run(
        &primary,
        &compare,
        Strategy::PreCornerDampers,
        &[0.30, 0.55, 0.80],
    );
    assert_eq!(result.basis, "Dampers · pre-corner");
    for corner in [0.30, 0.55, 0.80] {
        let index = (corner * (N - 1) as f64).round() as usize;
        let expected = (index + SHIFT) as f64 / (N - 1) as f64;
        assert!((result.fraction[index] - expected).abs() < 0.006);
    }
}

#[test]
fn manual_dampers_uses_percentage_until_offset() {
    let primary = make_lap(300, false, true);
    let result = run(&primary, &primary.clone(), Strategy::ManualDampers, &[]);
    assert_eq!(result.basis, "Dampers · manual");
    assert!((result.fraction[150] - 150.0 / 299.0).abs() < 1e-6);
}

#[test]
fn unavailable_strategies_fall_back_honestly() {
    let primary = make_lap(300, false, false);
    let gps = run(&primary, &primary.clone(), Strategy::Gps, &[]);
    let dampers = run(
        &primary,
        &primary.clone(),
        Strategy::PreCornerDampers,
        &[0.5],
    );
    assert_eq!(gps.basis, "Lap time %");
    assert_eq!(dampers.basis, "Lap time %");
    assert_eq!(gps.gps_anchors, 0);
}

#[expect(
    clippy::cast_precision_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn straight(accuracy: f64, samples: usize) -> UnifiedLap {
    let mut lap = UnifiedLap {
        sample_rate: 50,
        ..Default::default()
    };
    for i in 0..samples {
        lap.time.push(i as f64 / 50.0);
        lap.gps_lat.push(43.0 + i as f64 / 111_320.0);
        lap.gps_lon.push(-88.0);
        lap.gps_position_accuracy.push(accuracy);
    }
    lap
}

#[test]
fn relative_position_is_signed_along_travel() {
    let primary = straight(0.4, 500);
    let compare = straight(0.6, 500);
    let at = 200.0 / 499.0;
    let ahead = relative_along_track_meters(&primary, at, &compare, 205.0 / 499.0, 1.0).unwrap();
    assert!((ahead - 5.0).abs() < 0.05);
    let behind = relative_along_track_meters(&primary, at, &compare, 197.5 / 499.0, 1.0).unwrap();
    assert!((behind + 2.5).abs() < 0.05);
    let same = relative_along_track_meters(&primary, at, &compare, at, 1.0).unwrap();
    assert!(same.abs() < 1e-6);
}

#[test]
fn relative_position_needs_sub_metre_gps() {
    let primary = straight(0.5, 100);
    let coarse = straight(1.0, 100);
    assert!(relative_along_track_meters(&primary, 0.5, &coarse, 0.5, 1.0).is_none());
    let mut parked = primary.clone();
    parked.gps_lat.iter_mut().for_each(|v| *v = 43.0);
    assert!(relative_along_track_meters(&parked, 0.5, &primary, 0.5, 1.0).is_none());
    assert!(relative_along_track_meters(&primary, 0.5, &primary, 1.5, 1.0).is_none());
}

#[test]
fn capabilities_report_only_data_both_laps_carry() {
    let complete = make_lap(300, true, true);
    let missing = make_lap(300, false, false);
    assert!(alignment::gps_available(&complete, &complete));
    assert!(alignment::damper_available(&complete, &complete));
    assert!(!alignment::gps_available(&complete, &missing));
    assert!(!alignment::damper_available(&complete, &missing));

    let primary = make_lap(300, false, true);
    let mut dead = primary.clone();
    dead.damper_fl.iter_mut().for_each(|v| *v = 0.0);
    dead.damper_fr.iter_mut().for_each(|v| *v = 0.0);
    assert!(alignment::damper_available(&primary, &primary));
    assert!(!alignment::damper_available(&primary, &dead));

    let mut clustered = make_lap(300, true, false);
    let compare = clustered.clone();
    for i in 40..300 {
        clustered.gps_lat[i] = f64::NAN;
        clustered.gps_lon[i] = f64::NAN;
        clustered.gps_position_accuracy[i] = f64::NAN;
    }
    assert!(!alignment::gps_available(&clustered, &compare));
}

#[test]
fn confidence_reflects_strategy() {
    assert_eq!(confidence_label("", 0), "NONE");
    assert_eq!(confidence_label("GPS · continuous", 20), "HIGH");
    assert_eq!(confidence_label("GPS · re-sync", 3), "MED");
    assert_eq!(confidence_label("Dampers · pre-corner", 0), "MED");
    assert_eq!(confidence_label("Lap distance %", 0), "MED");
    assert_eq!(confidence_label("Lap time %", 0), "LOW");
}

#[test]
fn fraction_lookup_round_trips() {
    let map: Vec<f64> = (0..21).map(|i| f64::from(i) / 20.0).collect();
    assert!((alignment::interpolate_fraction(&map, 0.35) - 0.35).abs() < 1e-6);
    assert!((alignment::invert_fraction(&map, 0.35) - 0.35).abs() < 1e-6);
    let collapsed = [0.0, 0.0, 0.0];
    assert!((alignment::interpolate_fraction(&collapsed, 0.42) - 0.42).abs() < 1e-6);
    assert!((alignment::invert_fraction(&collapsed, 0.42) - 0.42).abs() < 1e-6);
}

#[test]
fn tiny_lap_produces_no_alignment() {
    let result = compute(
        &make_lap(1, true, true),
        &make_lap(100, true, true),
        &Options::default(),
    );
    assert!(result.time.is_empty() && result.fraction.is_empty() && result.basis.is_empty());
}

// ── Comparison: the one map every surface consumes ──────────────────

#[test]
#[expect(
    clippy::float_cmp,
    reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
)]
fn delta_starts_at_zero_and_matches_readout() {
    let primary = Arc::new(make_lap(1500, true, false));
    let compare = Arc::new(warped_lap(&primary, 1.5));
    let comparison = Comparison::new(primary.clone(), compare, Strategy::Gps, vec![], 0.0);
    let delta = comparison.delta();
    assert_eq!(delta.len(), primary.len());
    assert_eq!(delta[0], 0.0);
    // The plotted trace and the numeric readout are the same cache.
    let at = 750.0 / 1499.0;
    assert!((comparison.time_delta_at(at) - delta[750]).abs() < 1e-12);
    // Reference at the same station: speed delta small everywhere.
    assert!(comparison.speed_delta_at(at).abs() < 5.0);
}

#[test]
fn manual_offset_shifts_lookups_and_inverts_on_swap() {
    let primary = Arc::new(make_lap(300, false, true));
    let compare = Arc::new(make_lap(300, false, true));
    let mut comparison = Comparison::new(primary, compare, Strategy::ManualDampers, vec![], 0.0);
    assert!((comparison.compare_fraction_for_primary_fraction(0.5) - 0.5).abs() < 1e-9);
    comparison.set_manual_offset(0.1);
    assert!((comparison.compare_fraction_for_primary_fraction(0.5) - 0.4).abs() < 1e-9);
    assert!((comparison.primary_fraction_for_compare_fraction(0.4) - 0.5).abs() < 1e-9);
    let swapped = comparison.swapped(vec![]);
    assert!((swapped.manual_offset() + 0.1).abs() < 1e-12);
}
