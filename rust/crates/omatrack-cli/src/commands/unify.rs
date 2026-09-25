//! `unify <file> --output <csv>`: the fastest lap as a 50 Hz CSV.
//!
//! The export carries GPS positions: it refuses to overwrite, and removes
//! an incomplete file rather than leave a truncated one behind.

use crate::{Out, printf};
use omatrack_core::cfmt::fixed;
use omatrack_core::{
    ChannelOverrides, DistanceSource, UnifiedLap, fastest_lap_index, format_lap_time,
};
use std::ffi::OsStr;
use std::io::Write;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

pub fn run(path: &OsStr, output_path: &OsStr) -> i32 {
    let Some(src) = super::open(path) else {
        return 1;
    };
    let mut out = Out::stdout();
    let mut laps = src.detect_laps();
    if laps.is_empty() {
        out.str("FAIL: no laps to unify\n");
        return 1;
    }
    let best = fastest_lap_index(&mut laps);
    let lap = &laps[best];
    printf!(
        out,
        "unify: lap %d  %s  [%.3f, %.3f]\n",
        lap.id,
        &format_lap_time(lap.time_ms),
        lap.start_time,
        lap.end_time
    );

    let u = src.unify_lap(lap.start_time, lap.end_time, &ChannelOverrides::new());
    printf!(
        out,
        "unified: %zu samples @ %d Hz\n",
        u.len(),
        u.sample_rate
    );

    let mut failures = 0;
    let mut report = |label: &str, n: usize| {
        printf!(out, "  %-14s n=%zu\n", label, n);
    };
    report("speed", u.speed.len());
    report("throttle", u.throttle.len());
    report("brake", u.brake.len());
    report("steering", u.steering.len());
    report("gear", u.gear.len());
    report("distance", u.distance.len());
    printf!(
        out,
        "  distance source: %s\n",
        if u.distance_source == DistanceSource::Native {
            "native"
        } else {
            "speed-fused"
        }
    );
    if u.is_empty() || u.speed.len() != u.len() || u.distance.len() != u.len() {
        out.str("FAIL: required unified arrays are missing or misaligned\n");
        failures += 1;
    }

    let mut peak: f64 = 0.0;
    for &speed in &u.speed {
        peak = omatrack_core::num::max(peak, speed);
    }
    printf!(out, "  peak speed: %.1f km/h\n", peak);
    if failures > 0 {
        printf!(out, "unify: FAILED (%d)\n", failures);
        return 1;
    }

    let output = Path::new(output_path);
    if output.exists() {
        out.str("FAIL: refusing to overwrite ")
            .bytes(output_path.as_bytes())
            .str("\n");
        return 1;
    }
    let file = match std::fs::File::create(output) {
        Ok(file) => file,
        Err(_) => {
            out.str("FAIL: cannot write ")
                .bytes(output_path.as_bytes())
                .str("\n");
            return 1;
        }
    };
    let mut writer = std::io::BufWriter::with_capacity(1 << 16, file);
    let written = write_csv(&mut writer, &u).and_then(|_| writer.flush());
    drop(writer);
    if written.is_err() {
        let _ = std::fs::remove_file(output);
        out.str("FAIL: incomplete export removed: ")
            .bytes(output_path.as_bytes())
            .str("\n");
        return 1;
    }
    out.str("wrote ").bytes(output_path.as_bytes()).str("\n");
    out.str("unify: OK\n");
    0
}

fn write_csv(writer: &mut impl Write, u: &UnifiedLap) -> std::io::Result<()> {
    writer.write_all(
        b"time,speed,throttle,driverThrottle,brake,clutch,steering,gear,distance,gForceLong,\
gForceLat,gpsLat,gpsLon,gpsPositionAccuracy,gpsSpeedAccuracy\n",
    )?;
    let mut line = String::with_capacity(256);
    for i in 0..u.len() {
        line.clear();
        line.push_str(&fixed(u.time[i], 3));
        let mut field = |values: &[f64], precision: usize| {
            line.push(',');
            if let Some(value) = values.get(i) {
                line.push_str(&fixed(*value, precision));
            }
        };
        field(&u.speed, 3);
        field(&u.throttle, 4);
        field(&u.driver_throttle, 4);
        field(&u.brake, 3);
        field(&u.clutch, 4);
        field(&u.steering, 3);
        line.push(',');
        if let Some(gear) = u.gear.get(i) {
            line.push_str(&gear.to_string());
        }
        let mut field = |values: &[f64], precision: usize| {
            line.push(',');
            if let Some(value) = values.get(i) {
                line.push_str(&fixed(*value, precision));
            }
        };
        field(&u.distance, 2);
        field(&u.g_force_long, 4);
        field(&u.g_force_lat, 4);
        field(&u.gps_lat, 8);
        field(&u.gps_lon, 8);
        field(&u.gps_position_accuracy, 3);
        field(&u.gps_speed_accuracy, 3);
        line.push('\n');
        writer.write_all(line.as_bytes())?;
    }
    Ok(())
}
