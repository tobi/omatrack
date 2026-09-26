//! Side-by-side dump of two recordings of the same run (typically an `AiM` MP4 and its
//! `.telemetry` companion): GPS, main channels, laps and video frame sync.
//!
//! Port of `compareTelemetrySources`; the C++ builds it with an `ostringstream` in
//! `std::fixed` mode, reproduced here with `%.Nf`.

use crate::cfmt::fixed;
use crate::laps::Lap;
use crate::mapping::{ChannelMapping, ChannelOverrides};
use crate::recording::{RawChannel, Recording};
use std::fmt::Write as _;

const CONCEPTS: [&str; 10] = [
    "speed",
    "throttle",
    "driver_throttle",
    "brake",
    "steering",
    "gear",
    "distance",
    "gps_lat",
    "gps_lon",
    "gps_speed",
];

fn source_duration(source: &Recording) -> f64 {
    let mut duration: f64 = 0.0;
    for channel in source.channels() {
        duration = crate::num::max(duration, channel.duration_sec);
    }
    for lap in source.source_laps() {
        duration = crate::num::max(duration, lap.end_time);
    }
    duration
}

fn mapped<'a>(
    source: &'a Recording,
    mapping: &ChannelMapping,
    concept: &str,
) -> Option<(usize, &'a RawChannel)> {
    let index = *mapping.get(concept)?;
    source.channels().get(index).map(|channel| (index, channel))
}

fn optional(value: Option<f64>, precision: usize) -> String {
    match value {
        Some(v) if v.is_finite() => fixed(v, precision),
        _ => "-".to_string(),
    }
}

fn sample_text(source: &Recording, index: Option<usize>, time: f64, linear: bool) -> String {
    let Some(index) = index else {
        return "-".to_string();
    };
    match source.sample_at(index, time, linear) {
        None => "-".to_string(),
        Some(value) if !value.is_finite() => "nan".to_string(),
        Some(value) => fixed(value, 6),
    }
}

#[expect(
    clippy::let_underscore_must_use,
    reason = "Formatting these strings and primitives into a String cannot fail."
)]
fn raw_anchors(out: &mut String, source: &Recording, index: usize) {
    let samples = source.samples(index);
    if samples.is_empty() {
        out.push_str("    raw: -\n");
        return;
    }
    let forward = |start: usize| -> String {
        samples[start..]
            .iter()
            .find(|v| v.is_finite())
            .map_or_else(|| "nan".to_string(), |v| fixed(*v, 6))
    };
    let backward = |start: usize| -> String {
        samples[..=start]
            .iter()
            .rev()
            .find(|v| v.is_finite())
            .map_or_else(|| "nan".to_string(), |v| fixed(*v, 6))
    };
    let last = samples.len() - 1;
    let mid = last / 2;
    let _ = writeln!(
        out,
        "    raw[0]={}  [{mid}]={}  [{last}]={}",
        forward(0),
        forward(mid),
        backward(last)
    );
}

fn laps_of(source: &Recording) -> Vec<Lap> {
    if source.source_laps().is_empty() {
        source.detect_laps()
    } else {
        source.source_laps().to_vec()
    }
}

/// Render the comparison report.
#[expect(
    clippy::cast_possible_wrap,
    reason = "Preserve the C++ port's sample-index widths and rounding at this numerical boundary; verified by parity."
)]
#[expect(
    clippy::too_many_lines,
    reason = "Keep the ported analysis/report stages in source order so numerical and CLI parity remain auditable."
)]
#[expect(
    clippy::let_underscore_must_use,
    reason = "Formatting these strings and primitives into a String cannot fail."
)]
pub fn compare_telemetry_sources(
    left: &Recording,
    right: &Recording,
    left_label: &str,
    right_label: &str,
) -> String {
    let no_overrides = ChannelOverrides::new();
    let left_map = left.map_channels(&no_overrides);
    let right_map = right.map_channels(&no_overrides);
    let left_duration = source_duration(left);
    let right_duration = source_duration(right);
    let duration = crate::num::max(left_duration, right_duration);

    let mut out = String::new();
    let _ = writeln!(out, "compare {left_label} vs {right_label}");
    let _ = writeln!(
        out,
        "  format  {} / {}",
        left.format_name(),
        right.format_name()
    );
    let _ = writeln!(out, "  path    {}", left.path());
    let _ = writeln!(out, "          {}", right.path());
    let _ = writeln!(
        out,
        "  channels {} / {}",
        left.channels().len(),
        right.channels().len()
    );
    let _ = writeln!(
        out,
        "  duration {} / {} s  d={}",
        fixed(left_duration, 6),
        fixed(right_duration, 6),
        fixed(right_duration - left_duration, 6)
    );
    let _ = writeln!(
        out,
        "  offset  {} / {} s",
        optional(left.video_presentation_offset_sec(), 9),
        optional(right.video_presentation_offset_sec(), 9)
    );

    let left_laps = laps_of(left);
    let right_laps = laps_of(right);
    let _ = writeln!(out, "  laps    {} / {}", left_laps.len(), right_laps.len());
    let lap_count = left_laps.len().max(right_laps.len());
    for i in 0..lap_count.min(12) {
        let _ = write!(out, "    L{}  ", i + 1);
        match left_laps.get(i) {
            Some(lap) => {
                let _ = write!(
                    out,
                    "{}->{}",
                    fixed(lap.start_time, 3),
                    fixed(lap.end_time, 3)
                );
            }
            None => out.push('-'),
        }
        out.push_str("  /  ");
        match right_laps.get(i) {
            Some(lap) => {
                let _ = write!(
                    out,
                    "{}->{}",
                    fixed(lap.start_time, 3),
                    fixed(lap.end_time, 3)
                );
            }
            None => out.push('-'),
        }
        if let (Some(l), Some(r)) = (left_laps.get(i), right_laps.get(i)) {
            let _ = write!(
                out,
                "  dStart={}  dEnd={}",
                fixed(r.start_time - l.start_time, 6),
                fixed(r.end_time - l.end_time, 6)
            );
        }
        out.push('\n');
    }

    out.push_str("  mapped channels:\n");
    for concept in CONCEPTS {
        let left_channel = mapped(left, &left_map, concept);
        let right_channel = mapped(right, &right_map, concept);
        let _ = write!(out, "    {concept:<16} {left_label}=");
        let describe = |out: &mut String,
                        source: &Recording,
                        entry: Option<(usize, &RawChannel)>| match entry {
            Some((index, channel)) => {
                let _ = write!(
                    out,
                    "{} unit='{}' {}Hz n={}",
                    channel.name,
                    channel.unit,
                    fixed(channel.frequency_hz, 3),
                    source.samples(index).len()
                );
            }
            None => out.push('-'),
        };
        describe(&mut out, left, left_channel);
        let _ = write!(out, "\n                    {right_label}=");
        describe(&mut out, right, right_channel);
        out.push('\n');
        if let Some((index, _)) = left_channel {
            raw_anchors(&mut out, left, index);
        }
        // The same channel object only when both sides are one recording.
        if let Some((index, _)) = right_channel
            && !(std::ptr::eq(left, right) && left_channel.map(|(i, _)| i) == Some(index))
        {
            raw_anchors(&mut out, right, index);
        }
    }

    let mut times: Vec<f64> = Vec::new();
    if duration > 0.0 {
        times.push(0.0);
        times.push(crate::num::min(1.0, duration));
        times.push(duration * 0.25);
        times.push(duration * 0.5);
        times.push(duration * 0.75);
        times.push(crate::num::max(0.0, duration - 1.0));
    }
    for lap in &left_laps {
        times.push(lap.start_time);
        if lap.end_time > lap.start_time {
            times.push(0.5 * (lap.start_time + lap.end_time));
        }
    }
    times.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    times.dedup_by(|b, a| (*a - *b).abs() < 1e-6);
    times.truncate(16);

    out.push_str("  samples:\n");
    for &time in &times {
        let _ = writeln!(out, "    t={}s", fixed(time, 6));
        for concept in CONCEPTS {
            let left_index = left_map.get(concept).copied();
            let right_index = right_map.get(concept).copied();
            let linear = concept != "gear";
            let left_text = sample_text(left, left_index, time, linear);
            let right_text = sample_text(right, right_index, time, linear);
            let _ = write!(
                out,
                "      {concept:<16} {left_label}={left_text}  {right_label}={right_text}"
            );
            if let (Some(li), Some(ri)) = (left_index, right_index)
                && let (Some(lv), Some(rv)) = (
                    left.sample_at(li, time, linear),
                    right.sample_at(ri, time, linear),
                )
                && lv.is_finite()
                && rv.is_finite()
            {
                let _ = write!(out, "  d={}", fixed(rv - lv, 6));
            }
            out.push('\n');
        }
        let left_frame = left.video_frame_at(time);
        let right_frame = right.video_frame_at(time);
        let _ = write!(out, "      {:<16} {left_label}=", "video_frame");
        match left_frame {
            Some(frame) => out.push_str(&frame.to_string()),
            None => out.push('-'),
        }
        let _ = write!(out, "  {right_label}=");
        match right_frame {
            Some(frame) => out.push_str(&frame.to_string()),
            None => out.push('-'),
        }
        if let (Some(l), Some(r)) = (left_frame, right_frame) {
            let _ = write!(out, "  d={}", r as i64 - l as i64);
        }
        out.push('\n');
    }
    out
}
