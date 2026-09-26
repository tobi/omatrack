//! `parse <file>`: format, clock, channels, mapping and laps.

use crate::{Out, printf};
use omatrack_core::{ChannelOverrides, format_lap_time};
use std::ffi::OsStr;

pub(crate) fn run(path: &OsStr) -> i32 {
    let Some(src) = super::open(path) else {
        return 1;
    };
    let mut out = Out::stdout();
    printf!(out, "format: %s\n", src.format_name());
    if src.utc_start_ns() >= 0 {
        printf!(out, "utc start: %lld ns (Unix epoch)\n", src.utc_start_ns());
    } else {
        out.str("utc start: unknown (no wall clock in this recording)\n");
    }
    if let Some(offset) = src.video_presentation_offset_sec() {
        printf!(out, "video presentation offset: %.6f s\n", offset);
    }
    printf!(out, "channels: %zu\n", src.channels().len());
    for (i, ch) in src.channels().iter().enumerate() {
        printf!(
            out,
            "  [%zu] %-40s %-10s freq=%5.1fHz n=%zu dur=%.1fs\n",
            i,
            &ch.name,
            &ch.unit,
            ch.frequency_hz,
            src.samples(i).len(),
            ch.duration_sec
        );
    }

    let mapping = src.map_channels(&ChannelOverrides::new());
    out.str("mapping:\n");
    for (field, &index) in &mapping {
        printf!(out, "  %-14s -> %s\n", field, &src.channels()[index].name);
    }

    let laps = src.detect_laps();
    printf!(out, "laps: %zu\n", laps.len());
    for lap in &laps {
        printf!(
            out,
            "  #%d %s  %8.3fs -> %8.3fs  %s\n",
            lap.id,
            &format_lap_time(lap.time_ms),
            lap.start_time,
            lap.end_time,
            if lap.complete {
                ""
            } else {
                "(partial: out/in fragment)"
            }
        );
    }

    let mut failures = 0;
    if src.channels().is_empty() {
        out.str("FAIL: no channels\n");
        failures += 1;
    }
    if laps.is_empty() {
        out.str("FAIL: no laps detected\n");
        failures += 1;
    }
    for concept in ["speed", "gear", "throttle", "brake"] {
        if !mapping.contains_key(concept) {
            printf!(out, "INFO: optional %s concept not mapped\n", concept);
        }
    }
    if failures > 0 {
        printf!(out, "parse: FAILED (%d)\n", failures);
        return 1;
    }
    out.str("parse: OK\n");
    0
}
