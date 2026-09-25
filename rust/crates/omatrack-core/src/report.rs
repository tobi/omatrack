//! Side-by-side dump of two recordings of the same run (typically an AiM
//! MP4 and its `.telemetry` companion): GPS, main channels, laps and video
//! frame sync. Port of `compareTelemetrySources`; the C++ builds it with an
//! `ostringstream` in `std::fixed` mode, reproduced here with `%.Nf`.

use crate::cfmt::fixed;
use crate::laps::Lap;
use crate::mapping::{ChannelMapping, ChannelOverrides};
use crate::recording::{RawChannel, Recording};

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
            .map(|v| fixed(*v, 6))
            .unwrap_or_else(|| "nan".to_string())
    };
    let backward = |start: usize| -> String {
        samples[..=start]
            .iter()
            .rev()
            .find(|v| v.is_finite())
            .map(|v| fixed(*v, 6))
            .unwrap_or_else(|| "nan".to_string())
    };
    let last = samples.len() - 1;
    let mid = last / 2;
    out.push_str(&format!(
        "    raw[0]={}  [{mid}]={}  [{last}]={}\n",
        forward(0),
        forward(mid),
        backward(last)
    ));
}

fn laps_of(source: &Recording) -> Vec<Lap> {
    if source.source_laps().is_empty() {
        source.detect_laps()
    } else {
        source.source_laps().to_vec()
    }
}

/// Render the comparison report.
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
    out.push_str(&format!("compare {left_label} vs {right_label}\n"));
    out.push_str(&format!(
        "  format  {} / {}\n",
        left.format_name(),
        right.format_name()
    ));
    out.push_str(&format!("  path    {}\n", left.path()));
    out.push_str(&format!("          {}\n", right.path()));
    out.push_str(&format!(
        "  channels {} / {}\n",
        left.channels().len(),
        right.channels().len()
    ));
    out.push_str(&format!(
        "  duration {} / {} s  d={}\n",
        fixed(left_duration, 6),
        fixed(right_duration, 6),
        fixed(right_duration - left_duration, 6)
    ));
    out.push_str(&format!(
        "  offset  {} / {} s\n",
        optional(left.video_presentation_offset_sec(), 9),
        optional(right.video_presentation_offset_sec(), 9)
    ));

    let left_laps = laps_of(left);
    let right_laps = laps_of(right);
    out.push_str(&format!(
        "  laps    {} / {}\n",
        left_laps.len(),
        right_laps.len()
    ));
    let lap_count = left_laps.len().max(right_laps.len());
    for i in 0..lap_count.min(12) {
        out.push_str(&format!("    L{}  ", i + 1));
        match left_laps.get(i) {
            Some(lap) => out.push_str(&format!(
                "{}->{}",
                fixed(lap.start_time, 3),
                fixed(lap.end_time, 3)
            )),
            None => out.push('-'),
        }
        out.push_str("  /  ");
        match right_laps.get(i) {
            Some(lap) => out.push_str(&format!(
                "{}->{}",
                fixed(lap.start_time, 3),
                fixed(lap.end_time, 3)
            )),
            None => out.push('-'),
        }
        if let (Some(l), Some(r)) = (left_laps.get(i), right_laps.get(i)) {
            out.push_str(&format!(
                "  dStart={}  dEnd={}",
                fixed(r.start_time - l.start_time, 6),
                fixed(r.end_time - l.end_time, 6)
            ));
        }
        out.push('\n');
    }

    out.push_str("  mapped channels:\n");
    for concept in CONCEPTS {
        let left_channel = mapped(left, &left_map, concept);
        let right_channel = mapped(right, &right_map, concept);
        out.push_str(&format!("    {concept:<16} {left_label}="));
        let describe = |out: &mut String,
                        source: &Recording,
                        entry: Option<(usize, &RawChannel)>| match entry {
            Some((index, channel)) => out.push_str(&format!(
                "{} unit='{}' {}Hz n={}",
                channel.name,
                channel.unit,
                fixed(channel.frequency_hz, 3),
                source.samples(index).len()
            )),
            None => out.push('-'),
        };
        describe(&mut out, left, left_channel);
        out.push_str(&format!("\n                    {right_label}="));
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
        out.push_str(&format!("    t={}s\n", fixed(time, 6)));
        for concept in CONCEPTS {
            let left_index = left_map.get(concept).copied();
            let right_index = right_map.get(concept).copied();
            let linear = concept != "gear";
            let left_text = sample_text(left, left_index, time, linear);
            let right_text = sample_text(right, right_index, time, linear);
            out.push_str(&format!(
                "      {concept:<16} {left_label}={left_text}  {right_label}={right_text}"
            ));
            if let (Some(li), Some(ri)) = (left_index, right_index)
                && let (Some(lv), Some(rv)) = (
                    left.sample_at(li, time, linear),
                    right.sample_at(ri, time, linear),
                )
                && lv.is_finite()
                && rv.is_finite()
            {
                out.push_str(&format!("  d={}", fixed(rv - lv, 6)));
            }
            out.push('\n');
        }
        let left_frame = left.video_frame_at(time);
        let right_frame = right.video_frame_at(time);
        out.push_str(&format!("      {:<16} {left_label}=", "video_frame"));
        match left_frame {
            Some(frame) => out.push_str(&frame.to_string()),
            None => out.push('-'),
        }
        out.push_str(&format!("  {right_label}="));
        match right_frame {
            Some(frame) => out.push_str(&frame.to_string()),
            None => out.push('-'),
        }
        if let (Some(l), Some(r)) = (left_frame, right_frame) {
            out.push_str(&format!("  d={}", r as i64 - l as i64));
        }
        out.push('\n');
    }
    out
}
