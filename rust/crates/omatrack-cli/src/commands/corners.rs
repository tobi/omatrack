//! `corners <file> [--lap N] [--reference <file>] [--reference-lap N]
//! --zone <start:end> ...`: the same corner analyzers the app runs, on the
//! same unified laps, with the reference zone mapped through the same
//! comparison alignment.

use crate::{Out, printf};
use omatrack_core::alignment::{self, AlignmentResult, Options};
use omatrack_core::cfmt::{stod, stoi};
use omatrack_core::corners::{CornerContext, checks, measure_corner};
use omatrack_core::laps::lap_index_by_id;
use omatrack_core::{ChannelOverrides, UnifiedLap, fastest_lap_index, format_lap_time};
use std::ffi::{OsStr, OsString};
use std::os::unix::ffi::OsStrExt;

/// Parse the argument list after the command (`args[0]` is `corners`).
pub fn run_args(args: &[OsString], program: &OsStr) -> i32 {
    let mut reference: Option<&OsStr> = None;
    let mut lap_id = -1;
    let mut reference_lap_id = -1;
    let mut zones: Vec<(f64, f64)> = Vec::new();
    let text = |value: &OsStr| String::from_utf8_lossy(value.as_bytes()).into_owned();
    let usage = || {
        crate::print_usage(program);
        2
    };
    let mut i = 2;
    while i < args.len() {
        let option = args[i].as_bytes();
        let has_value = i + 1 < args.len();
        match option {
            b"--reference" if has_value => {
                i += 1;
                reference = Some(&args[i]);
            }
            b"--lap" if has_value => {
                i += 1;
                let Some(value) = stoi(&text(&args[i])) else {
                    return usage();
                };
                lap_id = value;
            }
            b"--reference-lap" if has_value => {
                i += 1;
                let Some(value) = stoi(&text(&args[i])) else {
                    return usage();
                };
                reference_lap_id = value;
            }
            b"--zone" if has_value => {
                i += 1;
                let value = text(&args[i]);
                let Some(colon) = value.find(':') else {
                    let mut err = Out::stderr();
                    err.str("--zone wants start:end lap fractions\n");
                    return 2;
                };
                let (Some(start), Some(end)) = (stod(&value[..colon]), stod(&value[colon + 1..]))
                else {
                    return usage();
                };
                zones.push((start, end));
            }
            _ => {
                let mut err = Out::stderr();
                err.str("unknown option ").bytes(option).str("\n");
                return 2;
            }
        }
        i += 1;
    }
    if zones.is_empty() {
        let mut err = Out::stderr();
        err.str("corners needs at least one --zone start:end\n");
        return 2;
    }
    run(&args[1], lap_id, reference, reference_lap_id, &zones)
}

/// A point the lap never reached is missing, not zero.
fn metres(value: f64) -> String {
    if value.is_finite() {
        omatrack_core::sprintf!("%.0fm", value)
    } else {
        "-".to_string()
    }
}

pub fn run(
    path: &OsStr,
    lap_id: i32,
    reference_path: Option<&OsStr>,
    reference_lap_id: i32,
    zones: &[(f64, f64)],
) -> i32 {
    let Some(src) = super::open(path) else {
        return 1;
    };
    let mut out = Out::stdout();
    let mut laps = src.detect_laps();
    if laps.is_empty() {
        out.str("FAIL: no laps\n");
        return 1;
    }
    let lap_index = if lap_id < 0 {
        Some(fastest_lap_index(&mut laps))
    } else {
        lap_index_by_id(&laps, lap_id)
    };
    let Some(lap_index) = lap_index else {
        printf!(out, "FAIL: no lap %d in active recording\n", lap_id);
        return 1;
    };
    let lap = &laps[lap_index];
    let overrides = ChannelOverrides::new();
    let primary = src.unify_lap(lap.start_time, lap.end_time, &overrides);
    printf!(
        out,
        "active: lap %d %s\n",
        lap.id,
        &format_lap_time(lap.time_ms)
    );

    let mut reference = UnifiedLap::default();
    if let Some(reference_path) = reference_path {
        out.flush();
        let Some(reference_source) = super::open(reference_path) else {
            return 1;
        };
        let mut reference_laps = reference_source.detect_laps();
        if reference_laps.is_empty() {
            out.str("FAIL: reference has no laps\n");
            return 1;
        }
        let pick_index = if reference_lap_id < 0 {
            Some(fastest_lap_index(&mut reference_laps))
        } else {
            lap_index_by_id(&reference_laps, reference_lap_id)
        };
        let Some(pick_index) = pick_index else {
            printf!(
                out,
                "FAIL: no lap %d in reference recording\n",
                reference_lap_id
            );
            return 1;
        };
        let pick = &reference_laps[pick_index];
        reference = reference_source.unify_lap(pick.start_time, pick.end_time, &overrides);
        printf!(
            out,
            "reference: lap %d %s\n",
            pick.id,
            &format_lap_time(pick.time_ms)
        );
    }

    // The reference zone goes through the same comparison alignment as the
    // app (GPS where both laps carry usable fixes, else lap percentage).
    let mut map = AlignmentResult::default();
    if !reference.time.is_empty() {
        map = alignment::compute(&primary, &reference, &Options::default());
        printf!(
            out,
            "alignment: %s  anchors=%d  confidence=%s%s%s\n",
            if map.basis.is_empty() {
                "none"
            } else {
                map.basis.as_str()
            },
            map.gps_anchors,
            alignment::confidence_label(&map.basis, map.gps_anchors),
            if map.rejection_reason.is_empty() {
                ""
            } else {
                "  rejected: "
            },
            map.rejection_reason.as_str()
        );
    }

    for &(start, end) in zones {
        let mut context = CornerContext::new(&primary, measure_corner(&primary, start, end, true));
        if !reference.time.is_empty() {
            context.reference = Some(&reference);
            let (mut reference_start, mut reference_end) = (start, end);
            if map.fraction.len() == primary.time.len() {
                reference_start = alignment::interpolate_fraction(&map.fraction, start);
                reference_end = alignment::interpolate_fraction(&map.fraction, end);
            }
            context.reference_metrics =
                measure_corner(&reference, reference_start, reference_end, true);
        }

        let m = &context.primary_metrics;
        printf!(
            out,
            "\nzone %.4f-%.4f  %.1fm  %.3fs\n",
            start,
            end,
            m.length_meters,
            m.time
        );
        if !m.valid {
            out.str("  (no samples)\n");
            continue;
        }
        printf!(
            out,
            "  speed   entry %.1f  apex %.1f  exit %.1f km/h\n",
            m.entry_speed,
            m.apex_speed,
            m.exit_speed
        );
        printf!(
            out,
            "  points  brake %-7s turn-in %-7s apex %-7s throttle %s\n",
            &metres(m.brake_point),
            &metres(m.turn_in_point),
            &metres(m.apex_point),
            &metres(m.throttle_point)
        );
        printf!(
            out,
            "  control gear %d  steer %.0f°  brake %.0f bar  trail %.2fs  lateral-g %s\n",
            m.min_gear,
            m.max_steering,
            m.max_brake,
            m.trail_brake_seconds,
            if m.has_lateral_g { "yes" } else { "no" }
        );

        let notes = checks::run(&context);
        if notes.is_empty() {
            out.str("  notes   (none)\n");
            continue;
        }
        for note in &notes {
            printf!(
                out,
                "  %-7s %s [%s]\n",
                note.severity.name(),
                &note.text,
                note.id
            );
        }
    }
    out.str("\ncorners: OK\n");
    0
}
