//! Port of tests/CornerAnalysisTest.cpp: synthetic 50 Hz corners whose shape
//! is obvious by construction, and the notes a driver would read.

#![cfg(test)]

use omatrack_core::UnifiedLap;
use omatrack_core::corners::{
    CornerContext, CornerNote, NoteSeverity, REGISTRY, auto_generate_corners, checks,
    measure_corner,
};
use std::collections::HashSet;

#[derive(Clone, Copy)]
struct Shape {
    entry_speed: f64,
    apex_speed: f64,
    brake_start: f64,
    apex: f64,
    throttle_on: f64,
    peak_brake_bar: f64,
    brake_ramp_seconds: f64,
    trail_seconds: f64,
    max_steering: f64,
    gear_in: i32,
    gear_apex: i32,
    overlap_seconds: f64,
    gear_shift_delay_seconds: f64,
    lift_lead_seconds: f64,
    steering_delay_seconds: f64,
}

impl Default for Shape {
    fn default() -> Self {
        Self {
            entry_speed: 200.0,
            apex_speed: 100.0,
            brake_start: 0.20,
            apex: 0.50,
            throttle_on: 0.60,
            peak_brake_bar: 80.0,
            brake_ramp_seconds: 0.2,
            trail_seconds: 1.0,
            max_steering: 60.0,
            gear_in: 6,
            gear_apex: 3,
            overlap_seconds: 0.0,
            gear_shift_delay_seconds: 0.0,
            lift_lead_seconds: 0.0,
            steering_delay_seconds: 0.0,
        }
    }
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn make_lap(shape: Shape) -> UnifiedLap {
    const RATE: i32 = 50;
    const DURATION: f64 = 20.0;
    let count = (f64::from(RATE) * DURATION) as usize;
    let mut lap = UnifiedLap {
        sample_rate: RATE,
        ..Default::default()
    };
    let mut distance = 0.0;
    for i in 0..count {
        let t = i as f64 / f64::from(RATE);
        let f = i as f64 / (count - 1) as f64;
        let mut speed = shape.entry_speed;
        if f >= shape.brake_start && f < shape.apex {
            let phase = (f - shape.brake_start) / (shape.apex - shape.brake_start);
            speed = shape.entry_speed + (shape.apex_speed - shape.entry_speed) * phase;
        } else if f >= shape.apex {
            let phase = (f - shape.apex) / (1.0 - shape.apex);
            speed = shape.apex_speed + (shape.entry_speed - shape.apex_speed) * phase;
        }
        let brake = if f >= shape.brake_start && f < shape.apex {
            let since = t - shape.brake_start * DURATION;
            let ramp = (since / shape.brake_ramp_seconds.max(0.02)).min(1.0);
            let release = (1.0
                - (since - shape.brake_ramp_seconds).max(0.0) / shape.trail_seconds.max(0.05))
            .max(0.0);
            shape.peak_brake_bar * ramp * release
        } else {
            0.0
        };
        let mut throttle = 1.0;
        let lift_time = shape.brake_start * DURATION - shape.lift_lead_seconds;
        if t >= lift_time && f < shape.throttle_on {
            throttle = 0.0;
        }
        if f >= shape.throttle_on {
            throttle = 1.0;
        }
        if shape.overlap_seconds > 0.0 {
            let overlap_start = shape.brake_start * DURATION + 0.05;
            if t >= overlap_start && t < overlap_start + shape.overlap_seconds {
                throttle = 0.5;
            }
        }
        let mut steering = 0.0;
        let steer_start = shape.brake_start * DURATION + shape.steering_delay_seconds;
        if t >= steer_start {
            let phase =
                ((t - steer_start) / (shape.apex * DURATION - steer_start).max(0.05)).min(1.0);
            steering = shape.max_steering * phase;
        }
        let gear = if f >= shape.brake_start {
            let since = t - shape.brake_start * DURATION;
            let phase = ((since - shape.gear_shift_delay_seconds).max(0.0) / 0.6).min(1.0);
            shape.gear_in - (phase * f64::from(shape.gear_in - shape.gear_apex)).round() as i32
        } else {
            shape.gear_in
        };
        lap.time.push(t);
        lap.speed.push(speed);
        lap.throttle.push(throttle);
        lap.brake.push(brake);
        lap.steering.push(steering);
        lap.gear.push(gear);
        lap.g_force_long.push(if brake > 0.0 { -1.2 } else { 0.4 });
        lap.g_force_lat.push(0.0);
        distance += speed / 3.6 / f64::from(RATE);
        lap.distance.push(distance);
    }
    lap
}

fn context<'a>(
    primary: &'a UnifiedLap,
    reference: &'a UnifiedLap,
    start: f64,
    end: f64,
) -> CornerContext<'a> {
    let mut c = CornerContext::new(primary, measure_corner(primary, start, end, true));
    c.reference = Some(reference);
    c.reference_metrics = measure_corner(reference, start, end, true);
    c
}

fn notes_for(primary: Shape, reference: Shape, start: f64, end: f64) -> Vec<CornerNote> {
    let p = make_lap(primary);
    let r = make_lap(reference);
    checks::run(&context(&p, &r, start, end))
}

fn text<'a>(notes: &'a [CornerNote], id: &str) -> Option<&'a str> {
    notes.iter().find(|n| n.id == id).map(|n| n.text.as_str())
}

fn has(notes: &[CornerNote], id: &str) -> bool {
    text(notes, id).is_some()
}

#[test]
fn measures_the_corner_shape() {
    let m = measure_corner(&make_lap(Shape::default()), 0.10, 0.80, true);
    assert!(m.valid);
    assert!(m.entry_speed > m.apex_speed && m.exit_speed > m.apex_speed);
    assert!((m.apex_speed - 100.0).abs() < 1.0);
    assert_eq!(m.min_gear, 3);
    assert!(m.max_brake > 70.0);
    assert!(m.brake_point > 0.0 && m.apex_point > m.brake_point);
    assert!(m.trail_brake_seconds.is_finite() && m.brake_rise_rate.is_finite());
}

#[test]
fn empty_inverted_and_degenerate_ranges_are_invalid() {
    let lap = make_lap(Shape::default());
    assert!(!measure_corner(&lap, 0.5, 0.5, true).valid);
    assert!(!measure_corner(&UnifiedLap::default(), 0.1, 0.8, true).valid);
    assert!(!measure_corner(&lap, 0.80, 0.10, true).valid);
}

#[test]
fn missing_brake_and_lift_points_are_nan() {
    let flat = Shape {
        peak_brake_bar: 0.0,
        throttle_on: 0.0,
        brake_start: 0.99,
        ..Default::default()
    };
    let m = measure_corner(&make_lap(flat), 0.10, 0.80, true);
    assert!(m.valid && !m.brake_point.is_finite() && !m.lift_point.is_finite());
}

#[test]
fn unmapped_lateral_g_is_not_lateral_g_and_turn_in_uses_steering() {
    let m = measure_corner(&make_lap(Shape::default()), 0.10, 0.80, true);
    assert!(!m.has_lateral_g && !m.combined_grip_early.is_finite());
    assert!(m.turn_in_point.is_finite() && m.turn_in_point < m.apex_point);
}

#[test]
fn identical_laps_say_nothing() {
    let notes = notes_for(Shape::default(), Shape::default(), 0.10, 0.80);
    assert!(notes.is_empty(), "{notes:?}");
}

#[test]
fn reports_slow_downshifts() {
    let lazy = Shape {
        gear_shift_delay_seconds: 1.2,
        ..Default::default()
    };
    let notes = notes_for(lazy, Shape::default(), 0.10, 0.80);
    assert!(has(&notes, "downshift_reaction") && has(&notes, "downshift_timing"));
}

#[test]
fn entry_speed_notes() {
    let slower = Shape {
        entry_speed: 180.0,
        ..Default::default()
    };
    assert!(
        text(
            &notes_for(slower, Shape::default(), 0.10, 0.80),
            "entry_speed"
        )
        .unwrap()
        .contains("slower")
    );
    let faster = Shape {
        entry_speed: 230.0,
        ..Default::default()
    };
    assert!(
        text(
            &notes_for(faster, Shape::default(), 0.10, 0.80),
            "entry_speed"
        )
        .unwrap()
        .contains("faster")
    );
    let nudge = Shape {
        entry_speed: 205.0,
        ..Default::default()
    };
    assert!(!has(
        &notes_for(nudge, Shape::default(), 0.10, 0.80),
        "entry_speed"
    ));
}

#[test]
fn reports_a_later_brake_release() {
    let short = Shape {
        trail_seconds: 0.35,
        ..Default::default()
    };
    assert!(has(
        &notes_for(short, Shape::default(), 0.10, 0.80),
        "trail_braking"
    ));
}

#[test]
fn throttle_while_braking_versus_heel_toe_blip() {
    let sloppy = Shape {
        overlap_seconds: 1.2,
        ..Default::default()
    };
    let notes = notes_for(sloppy, Shape::default(), 0.10, 0.80);
    let note = notes
        .iter()
        .find(|n| n.id == "brake_throttle_overlap")
        .unwrap();
    assert_eq!(note.severity, NoteSeverity::Error);
    let blip = Shape {
        overlap_seconds: 0.2,
        ..Default::default()
    };
    assert!(!has(
        &notes_for(blip, Shape::default(), 0.10, 0.80),
        "brake_throttle_overlap"
    ));
}

#[test]
fn steering_gear_and_brake_notes() {
    let more = Shape {
        max_steering: 90.0,
        ..Default::default()
    };
    assert!(
        text(
            &notes_for(more, Shape::default(), 0.10, 0.80),
            "steering_input"
        )
        .unwrap()
        .contains("more")
    );
    let less = Shape {
        max_steering: 30.0,
        ..Default::default()
    };
    assert!(
        text(
            &notes_for(less, Shape::default(), 0.10, 0.80),
            "steering_input"
        )
        .unwrap()
        .contains("less")
    );
    let lower = Shape {
        gear_apex: 2,
        ..Default::default()
    };
    assert!(
        text(
            &notes_for(lower, Shape::default(), 0.10, 0.80),
            "gear_usage"
        )
        .unwrap()
        .contains("lower")
    );
    let higher = Shape {
        gear_apex: 4,
        ..Default::default()
    };
    assert!(
        text(
            &notes_for(higher, Shape::default(), 0.10, 0.80),
            "gear_usage"
        )
        .unwrap()
        .contains("higher")
    );
    let light = Shape {
        peak_brake_bar: 50.0,
        ..Default::default()
    };
    assert!(
        text(
            &notes_for(light, Shape::default(), 0.10, 0.80),
            "brake_pressure"
        )
        .unwrap()
        .contains("lighter")
    );
    let hard = Shape {
        peak_brake_bar: 110.0,
        ..Default::default()
    };
    assert!(
        text(
            &notes_for(hard, Shape::default(), 0.10, 0.80),
            "brake_pressure"
        )
        .unwrap()
        .contains("harder")
    );
    let flat = Shape {
        peak_brake_bar: 0.0,
        brake_start: 0.99,
        ..Default::default()
    };
    assert!(!has(&notes_for(flat, flat, 0.10, 0.80), "brake_pressure"));
}

#[test]
fn single_lap_runs_only_primary_checks() {
    let sloppy = make_lap(Shape {
        overlap_seconds: 1.2,
        ..Default::default()
    });
    let c = CornerContext::new(&sloppy, measure_corner(&sloppy, 0.10, 0.80, true));
    let notes = checks::run(&c);
    assert!(!c.comparing());
    assert!(has(&notes, "brake_throttle_overlap") && !has(&notes, "entry_speed"));
}

#[test]
fn every_check_has_a_stable_id() {
    let mut ids = HashSet::new();
    for check in REGISTRY {
        assert!(!check.id().is_empty());
        assert!(ids.insert(check.id()), "duplicate id {}", check.id());
    }
    assert!(ids.len() >= 12);
    for id in [
        "entry_speed",
        "turn_in",
        "coasting",
        "throttle_timing",
        "brake_application_rate",
        "downshift_timing",
        "combined_grip_early",
        "combined_grip_mid",
    ] {
        assert!(ids.contains(id), "{id}");
    }
}

#[test]
fn coasting_and_turn_in_notes() {
    let coast = Shape {
        lift_lead_seconds: 1.6,
        ..Default::default()
    };
    assert!(
        text(&notes_for(coast, Shape::default(), 0.10, 0.80), "coasting")
            .unwrap()
            .contains("more")
    );
    let late = Shape {
        steering_delay_seconds: 1.4,
        ..Default::default()
    };
    assert!(
        text(&notes_for(late, Shape::default(), 0.10, 0.80), "turn_in")
            .unwrap()
            .contains("later")
    );
    assert!(
        text(&notes_for(Shape::default(), late, 0.10, 0.80), "turn_in")
            .unwrap()
            .contains("earlier")
    );
}

#[test]
fn turn_in_note_prefers_aligned_delta() {
    let p = make_lap(Shape {
        steering_delay_seconds: 1.4,
        ..Default::default()
    });
    let r = make_lap(Shape::default());
    let mut c = context(&p, &r, 0.10, 0.80);
    assert!((c.primary_metrics.turn_in_point - c.reference_metrics.turn_in_point).abs() >= 10.0);
    c.turn_in_delta = 3.0;
    assert!(!has(&checks::run(&c), "turn_in"));
    c.turn_in_delta = 15.0;
    let notes = checks::run(&c);
    let t = text(&notes, "turn_in").unwrap();
    assert!(t.contains("15m") && t.contains("later"));
}

#[test]
fn throttle_timing_notes() {
    let late = Shape {
        throttle_on: 0.74,
        ..Default::default()
    };
    let early = Shape {
        throttle_on: 0.52,
        ..Default::default()
    };
    assert!(
        text(&notes_for(late, early, 0.10, 0.90), "throttle_timing")
            .unwrap()
            .contains("late")
    );
    assert!(
        text(
            &notes_for(Shape::default(), late, 0.10, 0.90),
            "throttle_timing"
        )
        .unwrap()
        .contains("early")
    );
}

#[test]
fn reports_a_slower_brake_application() {
    let slow = Shape {
        brake_ramp_seconds: 2.0,
        ..Default::default()
    };
    assert!(
        text(
            &notes_for(slow, Shape::default(), 0.10, 0.80),
            "brake_application_rate"
        )
        .unwrap()
        .contains("slower")
    );
}

#[test]
fn combined_grip_needs_lateral_g() {
    let mut p = make_lap(Shape::default());
    let mut r = make_lap(Shape::default());
    let paint = |lap: &mut UnifiedLap, peak: f64| {
        for i in 0..lap.g_force_lat.len() {
            lap.g_force_lat[i] = ((lap.steering[i].abs() - 30.0) / 30.0).max(0.0) * peak;
        }
    };
    paint(&mut p, 0.2);
    paint(&mut r, 2.4);
    let notes = checks::run(&context(&p, &r, 0.10, 0.80));
    assert!(has(&notes, "combined_grip_early") || has(&notes, "combined_grip_mid"));
    let lap = make_lap(Shape::default());
    let notes = checks::run(&context(&lap, &lap, 0.10, 0.80));
    assert!(!has(&notes, "combined_grip_early") && !has(&notes, "combined_grip_mid"));
}

#[test]
fn severity_names_are_stable() {
    assert_eq!(NoteSeverity::Info.name(), "info");
    assert_eq!(NoteSeverity::Warning.name(), "warning");
    assert_eq!(NoteSeverity::Error.name(), "error");
}

#[test]
fn auto_generated_corners_bracket_brake_zones() {
    let lap = make_lap(Shape::default());
    let zones = auto_generate_corners(&lap);
    assert_eq!(zones.len(), 1);
    assert_eq!(zones[0].name, "Turn 1");
    assert!(zones[0].start < 0.20 && zones[0].end > 0.20);
}

#[test]
fn notes_read_as_sentences_with_spaced_units() {
    use omatrack_core::corners::checks::sentence;
    assert_eq!(sentence("throttle 23m late"), "Throttle 23 m late.");
    assert_eq!(
        sentence("first downshift 120ms later than reference (8m into braking)"),
        "First downshift 120 ms later than reference (8 m into braking)."
    );
    assert_eq!(
        sentence("reference trail-brakes 0.4s longer"),
        "Reference trail-brakes 0.4 s longer."
    );
    assert_eq!(sentence("2 gears lower"), "2 gears lower.");
    assert_eq!(sentence("Closely matched."), "Closely matched.");
    assert_eq!(sentence(""), "");
}
