//! The canonical 50 Hz lap (`UnifiedLap`) and its construction from a
//! recording (port of `TelemetrySource::unifyLap` and `resample`).

use crate::mapping::{
    ChannelOverrides, brake_unit_factor, gps_coordinate_degrees, lower_trimmed, speed_unit_factor,
};
use crate::num::{clamp, llround, max};
use crate::recording::Recording;
use std::collections::BTreeMap;

/// Unified sample rate in Hz.
pub const DEFAULT_SAMPLE_RATE: i32 = 50;
/// Physical sanity bounds: one corrupt logger sample must not turn the
/// distance axis astronomical.
pub const MAXIMUM_SPEED_KMH: f64 = 500.0;
const MAXIMUM_GPS_SPEED_MPS: f64 = MAXIMUM_SPEED_KMH / 3.6;

/// Where the unified distance came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum DistanceSource {
    /// The logger's own lap distance, accepted after validation.
    Native,
    /// Integrated wheel/vehicle speed, gently corrected by GPS speed.
    #[default]
    SpeedFused,
}

/// Same-rate, lap-relative arrays; every array has `time.len()` samples.
#[derive(Debug, Clone, PartialEq)]
pub struct UnifiedLap {
    pub sample_rate: i32,
    /// File-relative source time (s) of sample 0: `time[i] + start_time` is
    /// on the recording's own clock (raw channels, video).
    pub start_time: f64,
    pub distance_source: DistanceSource,
    pub time: Vec<f64>,
    /// km/h
    pub speed: Vec<f64>,
    /// 0-1
    pub throttle: Vec<f64>,
    /// 0-1, pre traction control
    pub driver_throttle: Vec<f64>,
    /// bar (or pedal position * 100)
    pub brake: Vec<f64>,
    /// 0-1
    pub clutch: Vec<f64>,
    /// degrees
    pub steering: Vec<f64>,
    pub gear: Vec<i32>,
    /// metres, starts at 0, monotonic
    pub distance: Vec<f64>,
    pub g_force_long: Vec<f64>,
    pub g_force_lat: Vec<f64>,
    pub damper_fl: Vec<f64>,
    pub damper_fr: Vec<f64>,
    pub damper_rl: Vec<f64>,
    pub damper_rr: Vec<f64>,
    /// degrees, east-positive; NaN without a fix
    pub gps_lat: Vec<f64>,
    pub gps_lon: Vec<f64>,
    pub gps_position_accuracy: Vec<f64>,
    pub gps_speed_accuracy: Vec<f64>,
    /// litres remaining; NaN when unmapped
    pub fuel: Vec<f64>,
}

impl Default for UnifiedLap {
    fn default() -> Self {
        Self {
            sample_rate: DEFAULT_SAMPLE_RATE,
            start_time: 0.0,
            distance_source: DistanceSource::SpeedFused,
            time: Vec::new(),
            speed: Vec::new(),
            throttle: Vec::new(),
            driver_throttle: Vec::new(),
            brake: Vec::new(),
            clutch: Vec::new(),
            steering: Vec::new(),
            gear: Vec::new(),
            distance: Vec::new(),
            g_force_long: Vec::new(),
            g_force_lat: Vec::new(),
            damper_fl: Vec::new(),
            damper_fr: Vec::new(),
            damper_rl: Vec::new(),
            damper_rr: Vec::new(),
            gps_lat: Vec::new(),
            gps_lon: Vec::new(),
            gps_position_accuracy: Vec::new(),
            gps_speed_accuracy: Vec::new(),
            fuel: Vec::new(),
        }
    }
}

impl UnifiedLap {
    pub fn len(&self) -> usize {
        self.time.len()
    }
    pub fn is_empty(&self) -> bool {
        self.time.is_empty()
    }
    /// Lap duration in seconds (last time sample).
    pub fn duration(&self) -> f64 {
        self.time.last().copied().unwrap_or(0.0)
    }
    /// Total lap distance in metres.
    pub fn total_distance(&self) -> f64 {
        match (self.distance.first(), self.distance.last()) {
            (Some(first), Some(last)) => last - first,
            _ => 0.0,
        }
    }
}

fn pedal_factor(unit: &str) -> f64 {
    match unit {
        "rad" => 1.0 / 1.745_329_251_994_329_5,
        "deg" => 0.01,
        _ => 1.0,
    }
}

impl Recording {
    /// Build a 50 Hz [`UnifiedLap`] over `[start_time, end_time]` seconds.
    pub fn unify_lap(
        &self,
        start_time: f64,
        end_time: f64,
        overrides: &ChannelOverrides,
    ) -> UnifiedLap {
        if !start_time.is_finite() || !end_time.is_finite() || !(end_time > start_time) {
            return UnifiedLap::default();
        }
        let duration = end_time - start_time;
        if duration > f64::from(i32::MAX - 1) / f64::from(DEFAULT_SAMPLE_RATE) {
            return UnifiedLap::default();
        }
        let n_samples = (duration * f64::from(DEFAULT_SAMPLE_RATE)) as i32 + 1;
        let n = n_samples as usize;
        let mapping = self.map_channels(overrides);
        let nan = f64::NAN;

        // Sample each channel on the shared 50 Hz absolute-time grid through
        // the source clock: chunks may start late or contain gaps, and
        // slicing flattened arrays by index would shift every later event.
        let mut resampled: BTreeMap<&str, Vec<f64>> = BTreeMap::new();
        let mut units: BTreeMap<&str, String> = BTreeMap::new();
        for (field, &index) in &mapping {
            if self.samples(index).is_empty() {
                continue;
            }
            let gps_position = field == "gps_lat" || field == "gps_lon";
            let nearest = field == "gear";
            let mut values = vec![if gps_position { nan } else { 0.0 }; n];
            let mut sampled = false;
            for sample in 0..n {
                let time = start_time + sample as f64 / f64::from(DEFAULT_SAMPLE_RATE);
                if let Some(value) = self.sample_at(index, time, !nearest) {
                    values[sample] = value;
                    sampled = true;
                } else if sample > 0 && !gps_position {
                    values[sample] = values[sample - 1];
                }
            }
            if !sampled {
                continue;
            }
            units.insert(field.as_str(), lower_trimmed(&self.channels()[index].unit));
            resampled.insert(field.as_str(), values);
        }

        let get = |field: &str, i: usize| -> f64 {
            match resampled.get(field) {
                Some(values) if i < values.len() => values[i],
                _ => {
                    if field == "gps_lat" || field == "gps_lon" {
                        nan
                    } else {
                        0.0
                    }
                }
            }
        };
        let unit_of = |field: &str| -> &str { units.get(field).map(String::as_str).unwrap_or("") };
        let speed_factor = |field: &str, fallback: f64| -> f64 {
            speed_unit_factor(unit_of(field)).unwrap_or(fallback)
        };

        let has_speed = resampled.contains_key("speed");
        let has_brake = resampled.contains_key("brake");
        let has_gps_speed_accuracy = resampled.contains_key("gps_speed_accuracy");
        // Empty unit is km/h: AiM scalars are unitless by design.
        let wheel_speed_factor = speed_factor("speed", 1.0);
        let throttle_factor = pedal_factor(unit_of("throttle"));
        let clutch_factor = pedal_factor(unit_of("clutch"));
        let driver_throttle_factor = pedal_factor(unit_of("driver_throttle"));
        let brake_factor = brake_unit_factor(unit_of("brake")).unwrap_or(1.0);
        let steering_factor = if unit_of("steering") == "rad" {
            180.0 / std::f64::consts::PI
        } else {
            1.0
        };
        let gps_lat_unit = unit_of("gps_lat").to_string();
        let gps_lon_unit = unit_of("gps_lon").to_string();
        let position_accuracy_factor = match unit_of("gps_position_accuracy") {
            "cm" => 0.01,
            "mm" => 0.001,
            "ft" => 0.3048,
            _ => 1.0,
        };
        let gps_speed_accuracy_factor = speed_factor("gps_speed_accuracy", 3.6) / 3.6;
        let gps_speed_factor = speed_factor(
            "gps_speed",
            if self.format_name() == "aimd" {
                3.6
            } else {
                1.0
            },
        ) / 3.6;
        let has_fuel = resampled.contains_key("fuel");
        let fuel_factor = match unit_of("fuel") {
            "ml" => 0.001,
            "gal" | "usgal" => 3.78541,
            "ukgal" | "impgal" => 4.54609,
            _ => 1.0,
        };

        let mut gear_offset = 0;
        if let Some(gears) = resampled.get("gear") {
            let mut min_positive = i32::MAX;
            let mut max_gear = 0;
            for &v in gears {
                let g = llround(v) as i32;
                if g > 0 {
                    min_positive = min_positive.min(g);
                }
                max_gear = max_gear.max(g);
            }
            // N=1, 1st=2 ... 6th=7 encodings: only a 7 proves the shift.
            if min_positive >= 2 && max_gear >= 7 {
                gear_offset = 1;
            }
        }

        let mut u = UnifiedLap {
            sample_rate: DEFAULT_SAMPLE_RATE,
            start_time,
            ..UnifiedLap::default()
        };
        let reserve = |v: &mut Vec<f64>| v.reserve(n);
        for v in [
            &mut u.time,
            &mut u.speed,
            &mut u.throttle,
            &mut u.brake,
            &mut u.clutch,
            &mut u.steering,
            &mut u.distance,
            &mut u.g_force_long,
            &mut u.g_force_lat,
            &mut u.gps_lat,
            &mut u.gps_lon,
            &mut u.gps_position_accuracy,
            &mut u.gps_speed_accuracy,
            &mut u.damper_fl,
            &mut u.damper_fr,
            &mut u.damper_rl,
            &mut u.damper_rr,
            &mut u.driver_throttle,
            &mut u.fuel,
        ] {
            reserve(v);
        }
        u.gear.reserve(n);
        let mut gps_speed_mps: Vec<f64> = Vec::with_capacity(n);

        let dt = 1.0 / f64::from(DEFAULT_SAMPLE_RATE);
        for i in 0..n {
            u.time.push(i as f64 * dt);

            // Carry the last sane value through an outlier.
            let mut speed = if has_speed {
                get("speed", i) * wheel_speed_factor
            } else {
                0.0
            };
            if !speed.is_finite() || speed < 0.0 || speed > MAXIMUM_SPEED_KMH {
                speed = u.speed.last().copied().unwrap_or(0.0);
            }
            u.speed.push(speed);

            let mut th = get("throttle", i) * throttle_factor;
            if th > 1.5 {
                th /= 100.0;
            }
            u.throttle.push(clamp(th, 0.0, 1.0));

            if has_brake {
                u.brake.push(max(0.0, get("brake", i) * brake_factor));
            } else {
                let mut bp = get("brake_pos", i);
                if bp > 1.5 {
                    bp /= 100.0;
                }
                u.brake.push(max(0.0, bp * 100.0));
            }

            let mut clutch = get("clutch", i) * clutch_factor;
            if clutch > 1.5 {
                clutch /= 100.0;
            }
            u.clutch.push(clamp(clutch, 0.0, 1.0));

            u.steering.push(get("steering", i) * steering_factor);
            u.gear
                .push(0.max((llround(get("gear", i)) as i32).wrapping_sub(gear_offset)));

            let mut dth = get("driver_throttle", i) * driver_throttle_factor;
            if dth > 1.5 {
                dth /= 100.0;
            }
            u.driver_throttle.push(clamp(dth, 0.0, 1.0));

            u.g_force_long.push(get("g_long", i));
            u.g_force_lat.push(get("g_lat", i));
            u.damper_fl.push(get("damper_fl", i));
            u.damper_fr.push(get("damper_fr", i));
            u.damper_rl.push(get("damper_rl", i));
            u.damper_rr.push(get("damper_rr", i));

            u.gps_lat.push(gps_coordinate_degrees(
                get("gps_lat", i),
                &gps_lat_unit,
                false,
            ));
            u.gps_lon.push(gps_coordinate_degrees(
                get("gps_lon", i),
                &gps_lon_unit,
                true,
            ));
            u.gps_position_accuracy.push(max(
                0.0,
                get("gps_position_accuracy", i) * position_accuracy_factor,
            ));
            u.gps_speed_accuracy.push(max(
                0.0,
                get("gps_speed_accuracy", i) * gps_speed_accuracy_factor,
            ));
            let mut gps_speed = get("gps_speed", i) * gps_speed_factor;
            if !gps_speed.is_finite() || gps_speed < 0.0 || gps_speed > MAXIMUM_GPS_SPEED_MPS {
                gps_speed = 0.0;
            }
            gps_speed_mps.push(gps_speed);
            u.fuel.push(if has_fuel {
                get("fuel", i) * fuel_factor
            } else {
                nan
            });
        }

        // Distance propagates from wheel speed; accuracy-weighted GPS Doppler
        // speed corrects drift, so poor GPS cannot inject jitter.
        let mut fused_distance = vec![0.0; n];
        let mut fused_speed = vec![0.0; n];
        for i in 0..n {
            let wheel = max(0.0, u.speed[i] / 3.6);
            let gps = gps_speed_mps[i];
            let mut gps_weight = 0.0;
            if gps > 0.0 {
                let accuracy = u.gps_speed_accuracy[i];
                gps_weight = if has_gps_speed_accuracy && accuracy > 0.0 {
                    clamp((1.5 - accuracy) / 1.25, 0.0, 1.0) * 0.5
                } else {
                    0.2
                };
            }
            if wheel <= 0.0 {
                gps_weight = if gps > 0.0 { 1.0 } else { 0.0 };
            }
            fused_speed[i] = wheel * (1.0 - gps_weight) + gps * gps_weight;
        }
        for i in 1..n {
            let step = 0.5 * (fused_speed[i - 1] + fused_speed[i]) * dt;
            fused_distance[i] = fused_distance[i - 1] + max(0.0, step);
        }

        // Native lap distance wins only after its total and continuity agree
        // with the independently integrated velocity.
        let mut native_accepted = false;
        if let Some(raw_distance) = resampled.get("distance")
            && raw_distance.len() >= n
        {
            let mut native = vec![0.0; n];
            let mut rejected_steps = 0usize;
            for i in 1..n {
                let fallback = fused_distance[i] - fused_distance[i - 1];
                let mut delta = raw_distance[i] - raw_distance[i - 1];
                let maximum_plausible = max(10.0, fallback * 8.0 + 1.0);
                if !delta.is_finite() || delta < -0.25 || delta > maximum_plausible {
                    delta = fallback;
                    rejected_steps += 1;
                } else {
                    delta = max(0.0, delta);
                }
                native[i] = native[i - 1] + delta;
            }
            let fused_total = fused_distance[n - 1];
            let native_total = native[n - 1];
            let ratio = if fused_total > 100.0 {
                native_total / fused_total
            } else {
                1.0
            };
            let rejected_fraction = if n > 1 {
                rejected_steps as f64 / (n - 1) as f64
            } else {
                1.0
            };
            native_accepted = fused_total <= 100.0
                || ((0.97..=1.03).contains(&ratio) && rejected_fraction <= 0.02);
            if native_accepted {
                u.distance = native;
            }
        }
        if !native_accepted {
            u.distance = fused_distance;
        }
        u.distance_source = if native_accepted {
            DistanceSource::Native
        } else {
            DistanceSource::SpeedFused
        };
        u
    }
}

/// Linear resample of a uniformly sampled series (port of
/// `MoTecParser.resample`).
pub fn resample(values: &[f64], src_freq: f64, target_freq: f64, duration: f64) -> Vec<f64> {
    if values.is_empty()
        || !src_freq.is_finite()
        || !target_freq.is_finite()
        || !duration.is_finite()
        || !(src_freq > 0.0)
        || !(target_freq > 0.0)
        || !(duration > 0.0)
    {
        return Vec::new();
    }
    if (src_freq - target_freq).abs() < 1e-9 {
        return values.to_vec();
    }
    if duration > f64::from(i32::MAX - 1) / target_freq {
        return Vec::new();
    }
    let n_out = (duration * target_freq) as i32 + 1;
    let max_index = values.len() as i32 - 1;
    let mut out = Vec::with_capacity(n_out as usize);
    for i in 0..n_out {
        let t = f64::from(i) / target_freq;
        let src_index = t * src_freq;
        let lo = (src_index.floor() as i32).min(max_index);
        let hi = (lo + 1).min(max_index);
        let frac = src_index - src_index.floor();
        out.push(values[lo as usize] + (values[hi as usize] - values[lo as usize]) * frac);
    }
    out
}
