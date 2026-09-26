//! Deterministic synthetic laps for tests, examples and benchmarks.
//!
//! A 90 s lap at 50 Hz with braking zones, gear changes and sensor noise,
//! plus a slightly different reference lap on its own grid and a nonlinear
//! alignment map, so every code path (decimation extrema, the per-column
//! map, fills, steps, Δ) is exercised with realistic density.

use std::sync::Arc;

use crate::scene::{CornerBand, FractionMap, LaneKind, LaneSeries, TraceScene, YRange};

/// A smooth nonlinear primary → reference map.
pub struct WarpMap;

impl FractionMap for WarpMap {
    fn reference_fraction(&self, primary: f64) -> f64 {
        let f = primary.clamp(0.0, 1.0);
        (f + 0.012 * (std::f64::consts::TAU * f).sin()).clamp(0.0, 1.0)
    }
}

struct Noise(u64);

impl Noise {
    #[expect(
        clippy::cast_precision_loss,
        reason = "The benchmark uses bounded synthetic sample counts, pixel projections and floating-point timing statistics."
    )]
    fn next(&mut self) -> f64 {
        self.0 = self
            .0
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        ((self.0 >> 11) as f64 / (1u64 << 53) as f64) - 0.5
    }
}

/// One synthetic lap: (speed, throttle, brake, gear, steering, rpm, `g_long`).
struct Lap {
    speed: Vec<f64>,
    throttle: Vec<f64>,
    brake: Vec<f64>,
    gear: Vec<f64>,
    steering: Vec<f64>,
    rpm: Vec<f64>,
    g_long: Vec<f64>,
    distance: Vec<f64>,
    time: Vec<f64>,
}

const CORNERS: [(f64, f64); 8] = [
    (0.08, 0.12),
    (0.18, 0.22),
    (0.30, 0.34),
    (0.41, 0.44),
    (0.52, 0.57),
    (0.66, 0.70),
    (0.78, 0.81),
    (0.90, 0.94),
];

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    reason = "The benchmark uses bounded synthetic sample counts, pixel projections and floating-point timing statistics."
)]
fn lap(samples: usize, pace: f64, random_seed: u64) -> Lap {
    let mut noise = Noise(random_seed);
    let mut lap = Lap {
        speed: Vec::with_capacity(samples),
        throttle: Vec::with_capacity(samples),
        brake: Vec::with_capacity(samples),
        gear: Vec::with_capacity(samples),
        steering: Vec::with_capacity(samples),
        rpm: Vec::with_capacity(samples),
        g_long: Vec::with_capacity(samples),
        distance: Vec::with_capacity(samples),
        time: Vec::with_capacity(samples),
    };
    let mut distance = 0.0;
    for i in 0..samples {
        let f = i as f64 / (samples - 1) as f64;
        // Proximity to the nearest corner apex: 1 at the apex.
        let mut corner = 0.0f64;
        let mut braking = 0.0f64;
        for (start, end) in CORNERS {
            let mid = (start + end) * 0.5;
            let width = (end - start) * 0.9;
            corner = corner.max((1.0 - ((f - mid) / width).powi(2)).max(0.0));
            let approach = (start - 0.03, start);
            if f >= approach.0 && f <= approach.1 {
                braking = braking.max((f - approach.0) / 0.03);
            }
        }
        let speed = (235.0 - 150.0 * corner) * pace + 1.5 * noise.next();
        let throttle = if braking > 0.0 {
            0.0
        } else {
            (1.0 - corner * 1.2).clamp(0.0, 1.0)
        };
        let brake = braking * 85.0 + f64::from(u8::from(braking > 0.0)) * 2.0 * noise.next().abs();
        let gear = (speed / 40.0).floor().clamp(1.0, 6.0);
        let rpm = 3000.0 + (speed % 40.0) / 40.0 * 5500.0 + 50.0 * noise.next();
        let steering = 160.0
            * corner
            * (if (f * 10.0) as i64 % 2 == 0 {
                1.0
            } else {
                -1.0
            })
            + 3.0 * noise.next();
        let g_long = if braking > 0.0 {
            -1.4 * braking
        } else {
            0.4 * throttle
        } + 0.05 * noise.next();
        distance += speed / 3.6 * 0.02;
        lap.speed.push(speed);
        lap.throttle.push(throttle);
        lap.brake.push(brake);
        lap.gear.push(gear);
        lap.steering.push(steering);
        lap.rpm.push(rpm);
        lap.g_long.push(g_long);
        lap.distance.push(distance);
        lap.time.push(i as f64 * 0.02);
    }
    lap
}

/// Samples of the synthetic primary lap (90 s at 50 Hz).
pub const PRIMARY_SAMPLES: usize = 4501;
/// Samples of the synthetic reference lap (92 s at 50 Hz).
pub const REFERENCE_SAMPLES: usize = 4601;

/// An 8-lane, two-lap scene: speed, throttle, brake (combined with
/// throttle by default styles), gear, steering, rpm, `g_long` and Δ.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    reason = "The benchmark uses bounded synthetic sample counts, pixel projections and floating-point timing statistics."
)]
pub fn scene() -> TraceScene {
    let primary = lap(PRIMARY_SAMPLES, 1.0, 7);
    let reference = lap(REFERENCE_SAMPLES, 0.985, 11);
    let map = WarpMap;
    // Δt through the same map: primary time minus reference time.
    let delta: Vec<f64> = (0..PRIMARY_SAMPLES)
        .map(|i| {
            let f = i as f64 / (PRIMARY_SAMPLES - 1) as f64;
            let rf = map.reference_fraction(f);
            primary.time[i] - rf * reference.time[REFERENCE_SAMPLES - 1]
        })
        .collect();
    let arc = |v: Vec<f64>| -> Arc<[f64]> { v.into() };
    let pair =
        |key: &'static str, title: &'static str, unit: &'static str, kind, p: &[f64], r: &[f64]| {
            LaneSeries::new(key, title, kind, arc(p.to_vec()))
                .with_unit(unit)
                .with_reference(Some(arc(r.to_vec())))
        };
    let lanes = vec![
        pair(
            "speed",
            "Speed",
            "km/h",
            LaneKind::Line,
            &primary.speed,
            &reference.speed,
        ),
        pair(
            "throttle",
            "Throttle",
            "%",
            LaneKind::Area,
            &primary.throttle,
            &reference.throttle,
        )
        .with_y_range(YRange::new(0.0, 1.0)),
        pair(
            "brake",
            "Brake",
            "bar",
            LaneKind::Area,
            &primary.brake,
            &reference.brake,
        ),
        pair(
            "gear",
            "Gear",
            "",
            LaneKind::Step,
            &primary.gear,
            &reference.gear,
        )
        .with_y_range(YRange::new(0.0, 7.0)),
        pair(
            "steering",
            "Steering",
            "deg",
            LaneKind::Line,
            &primary.steering,
            &reference.steering,
        ),
        pair(
            "rpm",
            "RPM",
            "rpm",
            LaneKind::Line,
            &primary.rpm,
            &reference.rpm,
        ),
        pair(
            "g_long",
            "G long",
            "g",
            LaneKind::Line,
            &primary.g_long,
            &reference.g_long,
        ),
        LaneSeries::new("delta", "Δ time", LaneKind::Delta, arc(delta)).with_unit("s"),
    ];
    let corners = CORNERS
        .iter()
        .enumerate()
        .map(|(i, (s, e))| CornerBand::new(i as u32 + 1, format!("T{}", i + 1), *s, *e))
        .collect();
    TraceScene::new(arc(primary.distance), arc(primary.time))
        .with_lanes(lanes)
        .with_map(Some(Arc::new(WarpMap)))
        .with_corners(corners, Vec::new())
        .with_neighbour_labels(Some("L7".into()), Some("L9".into()))
}
