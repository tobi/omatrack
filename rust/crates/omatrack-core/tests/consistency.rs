//! Session consistency: lap selection, confidence bands and corner brake
//! spread, mirroring `requestTraceConfidence` / `requestCornerConsistency`.

#![cfg(test)]

use omatrack_core::consistency::{
    self, CONFIDENCE_CHANNELS, ConfidenceBand, corner_consistency, corner_consistency_lap_ids,
    trace_confidence,
};
use omatrack_core::corners::measure_corner;
use omatrack_core::{Lap, SessionError, UnifiedLap};
use std::sync::atomic::AtomicBool;

fn lap(id: i32, time_ms: f64, complete: bool) -> Lap {
    Lap::new(id, 0.0, time_ms / 1000.0, time_ms, complete)
}

/// A 20 s lap: one braked corner starting at `brake_fraction`, with
/// `scale` on the pedal and steering traces.
fn unified(brake_fraction: f64, scale: f64) -> UnifiedLap {
    let count = 1000;
    let mut lap = UnifiedLap::default();
    let mut distance = 0.0;
    for i in 0..count {
        let f = f64::from(i) / f64::from(count - 1);
        let braking = f >= brake_fraction && f < brake_fraction + 0.1;
        let speed = if braking { 120.0 } else { 200.0 };
        lap.time.push(f64::from(i) / 50.0);
        lap.speed.push(speed);
        lap.throttle
            .push(if braking { 0.0 } else { scale.min(1.0) });
        lap.driver_throttle.push(if braking { 0.0 } else { 1.0 });
        lap.brake.push(if braking { 60.0 * scale } else { 0.0 });
        lap.clutch.push(0.0);
        lap.steering.push(if braking { 30.0 * scale } else { 0.0 });
        lap.gear.push(if braking { 3 } else { 6 });
        lap.distance.push(distance);
        lap.g_force_long.push(0.0);
        lap.g_force_lat.push(0.0);
        lap.damper_fl.push((f64::from(i) * 0.3).sin());
        distance += speed / 3.6 / 50.0;
    }
    lap
}

#[test]
fn confidence_uses_the_fastest_half_without_the_primary() {
    // Representative: 1, 2, 3, 5, 6 (4 is incomplete, 7 has no time).
    let laps = vec![
        lap(1, 80_000.0, true),
        lap(2, 70_000.0, true),
        lap(3, 75_000.0, true),
        lap(4, 60_000.0, false),
        lap(5, 90_000.0, true),
        lap(6, 72_000.0, true),
        lap(7, 0.0, true),
    ];
    // Fastest half of five is three: 2, 6, 3.
    assert_eq!(consistency::confidence_lap_ids(&laps, 99), [2, 6, 3]);
    assert_eq!(consistency::confidence_lap_ids(&laps, 6), [2, 3]);
    // Fastest quarter of five is two, primary included.
    assert_eq!(corner_consistency_lap_ids(&laps), [2, 6]);
    assert!(corner_consistency_lap_ids(&[]).is_empty());
}

#[test]
fn band_validity_matches_the_store() {
    assert!(!ConfidenceBand::default().is_valid());
    let series = ConfidenceBand::new;
    assert!(!series(vec![0.0, 1.0], vec![0.5, 1.5], vec![1.0, 2.0], 1).is_valid());
    assert!(!series(vec![0.0, 1.0], vec![0.5], vec![1.0, 2.0], 3).is_valid());
    assert!(series(vec![0.0, 1.0], vec![0.5, 1.5], vec![1.0, 2.0], 2).is_valid());
}

#[test]
fn bands_envelope_the_session() {
    let primary = unified(0.40, 1.0);
    let others = [unified(0.40, 0.8), unified(0.40, 1.0), unified(0.40, 1.2)];
    let cancel = AtomicBool::new(false);
    let confidence = trace_confidence(&primary, others.iter(), &cancel).unwrap();
    assert_eq!(confidence.lap_count(), 3);
    assert_eq!(confidence.consistency().len(), primary.len());
    for key in CONFIDENCE_CHANNELS {
        let band = confidence.band(key).unwrap_or_else(|| panic!("{key}"));
        assert!(band.is_valid(), "{key}");
        assert_eq!(band.lap_count, 3);
        for i in 0..primary.len() {
            assert!(band.lower[i] <= band.median[i] && band.median[i] <= band.upper[i]);
        }
    }
    // Inside the corner: brake 48/60/72 bar -> p10 50.4, p50 60, p90 69.6.
    let brake = confidence.band("brake").unwrap();
    let inside = 450;
    assert!(
        (brake.lower[inside] - 50.4).abs() < 1e-9,
        "{}",
        brake.lower[inside]
    );
    assert!((brake.median[inside] - 60.0).abs() < 1e-9);
    assert!((brake.upper[inside] - 69.6).abs() < 1e-9);
    // Composite: brake spread 19.2/25, steering 9.6/15, throttle and gear 0.
    let expected = (19.2 / 25.0 + 9.6 / 15.0 + 0.0 + 0.0) / 4.0;
    assert!((confidence.consistency()[inside] - expected).abs() < 1e-9);
    // Outside the corner every lap agrees except throttle (0.8..1.0).
    let outside = 100;
    let throttle_spread: f64 = 1.0 - (0.8 + 0.2 * 0.2);
    let expected = (throttle_spread / 0.25).min(1.0) / 4.0;
    assert!((confidence.consistency()[outside] - expected).abs() < 1e-9);
}

#[test]
fn one_lap_gives_no_band() {
    let primary = unified(0.40, 1.0);
    let cancel = AtomicBool::new(false);
    let confidence = trace_confidence(&primary, [unified(0.4, 1.0)].iter(), &cancel).unwrap();
    assert_eq!(confidence.lap_count(), 1);
    assert!(confidence.bands().is_empty());
    assert!(confidence.consistency().iter().all(|v| v.is_nan()));
    let short = UnifiedLap::default();
    assert_eq!(
        trace_confidence(&short, [primary].iter(), &cancel)
            .unwrap()
            .lap_count(),
        0
    );
}

#[test]
fn confidence_honours_cancel() {
    let primary = unified(0.40, 1.0);
    let cancel = AtomicBool::new(true);
    let result = trace_confidence(&primary, [unified(0.4, 1.0)].iter(), &cancel);
    assert_eq!(result.unwrap_err(), SessionError::Cancelled);
}

#[test]
#[expect(
    clippy::manual_midpoint,
    reason = "Keep the test oracle's bounded floating-point operation order explicit."
)]
fn corner_brake_spread_reduces_like_the_store() {
    let laps = [
        unified(0.40, 1.0),
        unified(0.41, 1.0),
        unified(0.43, 1.0),
        unified(0.46, 1.0),
    ];
    // The corner by absolute distance on the first lap's axis, wide enough
    // to contain every lap's brake zone.
    let first = &laps[0];
    let start = first.distance[350];
    let end = first.distance[600];
    let result = corner_consistency(laps.iter(), start, end);
    assert_eq!(result.lap_count, 4);
    assert_eq!(result.valid_lap_count, 4);
    assert_eq!(result.braking_lap_count, 4);

    let mut points: Vec<f64> = laps
        .iter()
        .map(|lap| {
            let s = omatrack_core::monotonic::invert_fraction(&lap.distance, start);
            let e = omatrack_core::monotonic::invert_fraction(&lap.distance, end);
            measure_corner(lap, s, e, true).brake_point
        })
        .collect();
    points.sort_by(f64::total_cmp);
    assert!(points.windows(2).all(|w| w[0] < w[1]));
    let median = (points[1] + points[2]) / 2.0;
    let mean = points.iter().sum::<f64>() / 4.0;
    let std = (points.iter().map(|p| (p - mean).powi(2)).sum::<f64>() / 4.0).sqrt();
    assert!((result.median_brake_point - median).abs() < 1e-9);
    assert!((result.brake_point_std_dev - std).abs() < 1e-9);
    assert!((result.brake_point_range - (points[3] - points[0])).abs() < 1e-9);

    // Odd count: the middle value.
    let odd = corner_consistency(laps[..3].iter(), start, end);
    assert!((odd.median_brake_point - points[1]).abs() < 1e-9);

    // No braking at all: counts, but NaN statistics.
    let empty = corner_consistency([UnifiedLap::default()].iter(), start, end);
    assert_eq!((empty.lap_count, empty.valid_lap_count), (1, 0));
    assert!(empty.median_brake_point.is_nan());
}

/// A lap of `count` samples over 1000 m braking over [`brake_from`,
/// `brake_from` + 100) metres at `pressure`, at a constant pace.
fn paced(count: usize, brake_from: f64, pressure: f64) -> UnifiedLap {
    let mut lap = UnifiedLap::default();
    for i in 0..count {
        let d = 1000.0 * i as f64 / (count - 1) as f64;
        let braking = d >= brake_from && d < brake_from + 100.0;
        lap.time.push(i as f64 / 50.0);
        lap.distance.push(d);
        lap.speed.push(if braking { 100.0 } else { 200.0 });
        lap.throttle.push(if braking { 0.0 } else { 1.0 });
        lap.brake.push(if braking { pressure } else { 0.0 });
        lap.gear.push(if braking { 3 } else { 5 });
    }
    lap
}

#[test]
fn laps_resample_onto_the_primary_by_share_of_distance() {
    // A slower lap (1500 samples) brakes at the primary's station: by index
    // or time it would land half a lap later.
    let primary = paced(1000, 400.0, 50.0);
    let slower = paced(1500, 400.0, 30.0);
    let later = paced(1200, 450.0, 70.0);
    let cancel = AtomicBool::new(false);
    let spread = consistency::build_consistency(
        &primary,
        1,
        [(1, &primary), (2, &slower), (3, &later)],
        &cancel,
    )
    .unwrap();
    assert_eq!(spread.lap_ids(), [2, 3], "the primary is not its own line");
    assert!(spread.is_meaningful());
    // Speed-fused distance drifts: the stations are approximate.
    assert!(spread.is_approximate());
    assert_eq!(spread.samples(), primary.len());
    let brake = spread.channel("brake").unwrap();
    assert_eq!(brake.laps.len(), 2);
    assert!(brake.laps.iter().all(|(_, s)| s.len() == primary.len()));
    let at = |metres: f64| (metres / 1000.0 * 999.0).round() as usize;
    let slower_series = &brake.laps[0].1;
    assert_eq!(slower_series[at(390.0)], 0.0);
    assert!((slower_series[at(410.0)] - 30.0).abs() < 1e-9);
    // The envelope spans the primary and both laps, station by station.
    assert_eq!(brake.min[at(420.0)], 0.0, "the later lap is off the brake");
    assert!((brake.max[at(420.0)] - 50.0).abs() < 1e-9);
    assert!((brake.max[at(480.0)] - 70.0).abs() < 1e-9);
    assert!((brake.min[at(480.0)] - 30.0).abs() < 1e-9);
    assert_eq!(brake.max[at(700.0)], 0.0);
    // Gear holds the nearest sample, never a blend.
    let gear = spread.channel("gear").unwrap();
    assert!(
        gear.laps
            .iter()
            .all(|(_, s)| s.iter().all(|g| *g == 3.0 || *g == 5.0))
    );
    // Channels the laps lack are left out, not drawn flat.
    assert!(spread.channel("damper_fl").is_none());
}

#[test]
fn one_other_lap_is_no_spread_and_cancel_is_honoured() {
    let primary = paced(1000, 400.0, 50.0);
    let other = paced(1100, 400.0, 40.0);
    let cancel = AtomicBool::new(false);
    let single = consistency::build_consistency(&primary, 1, [(2, &other)], &cancel).unwrap();
    assert_eq!(single.lap_count(), 1);
    let mut native = paced(1000, 400.0, 50.0);
    native.distance_source = omatrack_core::unify::DistanceSource::Native;
    let mut native_other = paced(1100, 400.0, 40.0);
    native_other.distance_source = omatrack_core::unify::DistanceSource::Native;
    let exact = consistency::build_consistency(&native, 1, [(2, &native_other)], &cancel).unwrap();
    assert!(!exact.is_approximate(), "native distance on both laps");
    assert!(!single.is_meaningful());
    let mut no_distance = paced(900, 400.0, 40.0);
    no_distance.distance.iter_mut().for_each(|d| *d = 0.0);
    let skipped =
        consistency::build_consistency(&primary, 1, [(3, &no_distance)], &cancel).unwrap();
    assert_eq!(skipped.lap_count(), 0, "a lap without distance is left out");
    cancel.store(true, std::sync::atomic::Ordering::Relaxed);
    assert_eq!(
        consistency::build_consistency(&primary, 1, [(2, &other)], &cancel),
        Err(SessionError::Cancelled)
    );
}

#[test]
fn spread_laps_are_the_timed_laps_near_the_best_fastest_first() {
    let laps = vec![
        lap(1, 80_000.0, true),
        lap(2, 70_000.0, true),
        lap(3, 60_000.0, false),
        lap(4, 73_000.0, true),
        lap(5, 73_400.0, true),
    ];
    // Within SPREAD_MAX_GAP (105%) of the best 70 s; the 80 s lap and the
    // untimed 60 s crossing stay out.
    assert_eq!(consistency::spread_lap_ids(&laps), [2, 4, 5]);
    assert!(consistency::spread_lap_ids(&[]).is_empty());
}
