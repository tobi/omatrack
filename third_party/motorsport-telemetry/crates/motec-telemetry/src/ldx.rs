//! MoTeC LDX sidecar generation.
//!
//! LDX is XML metadata beside an LD recording.  The LD remains self-contained,
//! but i2 uses the sidecar for beacon/lap markers and session details.

use crate::write::MotecMetadata;
use motorsport_telemetry_core::TelemetrySource;

const MIN_MARKER_SPACING_NS: u64 = 5_000_000_000;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LapMarkers {
    /// Beacon times relative to the beginning of the recording.
    pub times_ns: Vec<u64>,
    pub source_channel: String,
}

fn normalized(name: &str) -> String {
    name.chars()
        .filter(|c| c.is_ascii_alphanumeric())
        .flat_map(char::to_lowercase)
        .collect()
}

fn channel_index(source: &dyn TelemetrySource, wanted: &str) -> Option<usize> {
    source
        .channels()
        .iter()
        .position(|channel| normalized(&channel.name) == wanted && channel.sample_count > 0)
}

fn transitions(source: &dyn TelemetrySource, index: usize, rising_pulse: bool) -> Vec<u64> {
    let channel = &source.channels()[index];
    let mut previous: Option<f64> = None;
    let mut markers = Vec::new();
    for (chunk_index, chunk) in channel.chunks.iter().enumerate() {
        for local_index in 0..chunk.sample_count {
            let value = source.decode(index, chunk_index, local_index);
            if let Some(before) = previous {
                let changed = if rising_pulse {
                    value.is_finite() && before.is_finite() && value > 0.0 && before <= 0.0
                } else {
                    // Lap counters occasionally reset to zero at shutdown.  A
                    // strictly increasing transition is a crossing; a reset is not.
                    value.is_finite() && before.is_finite() && value > before
                };
                if changed {
                    let time = source.sample_time_ns(index, chunk_index, local_index);
                    if time > 0
                        && markers
                            .last()
                            .is_none_or(|last| time.saturating_sub(*last) >= MIN_MARKER_SPACING_NS)
                    {
                        markers.push(time);
                    }
                }
            }
            previous = Some(value);
        }
    }
    markers
}

/// Recover lap beacons from the strongest channel available.
///
/// Dedicated trigger pulses have better time resolution than the low-rate lap
/// counter, so they are preferred.  Counter channels are conservative
/// fallbacks.  Channel names are exact after punctuation/case normalization to
/// avoid interpreting an unrelated switch as a beacon.
pub fn infer_lap_markers(source: &dyn TelemetrySource) -> Option<LapMarkers> {
    for wanted in ["lapbeacontrig", "laptrigger", "blaptrig", "fiabeacon"] {
        if let Some(index) = channel_index(source, wanted) {
            let times_ns = transitions(source, index, true);
            if !times_ns.is_empty() {
                return Some(LapMarkers {
                    times_ns,
                    source_channel: source.channels()[index].name.clone(),
                });
            }
        }
    }
    for wanted in ["lapnumber", "beaconeventcount"] {
        if let Some(index) = channel_index(source, wanted) {
            let times_ns = transitions(source, index, false);
            if !times_ns.is_empty() {
                return Some(LapMarkers {
                    times_ns,
                    source_channel: source.channels()[index].name.clone(),
                });
            }
        }
    }
    None
}

fn xml(value: &str) -> String {
    let mut escaped = String::with_capacity(value.len());
    for character in value.chars() {
        match character {
            '&' => escaped.push_str("&amp;"),
            '<' => escaped.push_str("&lt;"),
            '>' => escaped.push_str("&gt;"),
            '"' => escaped.push_str("&quot;"),
            '\'' => escaped.push_str("&apos;"),
            _ => escaped.push(character),
        }
    }
    escaped
}

fn lap_summary(markers: &[u64]) -> (usize, Option<(f64, usize)>) {
    let total_laps = markers.len().saturating_add(1);
    let fastest = markers
        .windows(2)
        .enumerate()
        .map(|(index, pair)| ((pair[1] - pair[0]) as f64 / 1e9, index + 2))
        .filter(|(seconds, _)| *seconds > 0.0)
        .min_by(|left, right| left.0.total_cmp(&right.0));
    (total_laps, fastest)
}

fn lap_time(seconds: f64) -> String {
    let minutes = (seconds / 60.0).floor() as u64;
    format!("{minutes}:{:06.3}", seconds - minutes as f64 * 60.0)
}

fn detail(out: &mut String, id: &str, value: &str) {
    out.push_str(&format!(
        "   <String Id=\"{}\" Value=\"{}\"/>\n",
        xml(id),
        xml(value)
    ));
}

/// Build the companion LDX XML.  Unknown details are left empty rather than
/// guessed.  When no reliable beacon channel exists, the sidecar still carries
/// the supplied session identity but does not invent lap boundaries.
pub fn write_motec_ldx_bytes(source: &dyn TelemetrySource, metadata: &MotecMetadata) -> Vec<u8> {
    let markers = infer_lap_markers(source);
    let mut out = String::from(
        "<?xml version=\"1.0\"?>\n<LDXFile Locale=\"English_United States.1252\" DefaultLocale=\"C\" Version=\"1.6\">\n <Layers>\n  <Layer>\n",
    );

    if let Some(markers) = &markers {
        out.push_str("   <MarkerBlock>\n    <MarkerGroup Name=\"Beacons\" Index=\"3\">\n");
        for (index, time_ns) in markers.times_ns.iter().enumerate() {
            // LDX beacon time is expressed in microseconds.
            let time_us = *time_ns as f64 / 1_000.0;
            out.push_str(&format!(
                "     <Marker Version=\"100\" ClassName=\"BCN\" Name=\"Manual.{}\" Flags=\"77\" Time=\"{time_us:.17e}\"/>\n",
                index + 1
            ));
        }
        out.push_str("    </MarkerGroup>\n   </MarkerBlock>\n");
    }

    out.push_str("  </Layer>\n  <Details>\n");
    detail(&mut out, "Event", &metadata.event);
    detail(&mut out, "Venue", &metadata.venue);
    detail(&mut out, "Driver", &metadata.driver);
    detail(&mut out, "Team", &metadata.team);
    detail(&mut out, "Vehicle Id", &metadata.vehicle);
    detail(&mut out, "Vehicle Number", &metadata.vehicle_number);
    detail(&mut out, "Session", &metadata.session);
    detail(&mut out, "Short Comment", &metadata.short_comment);
    detail(&mut out, "Long Comment", &metadata.event_comment);
    out.push_str(&format!(
        "   <DateTime Id=\"Log Date\" Value=\"{}\"/>\n   <DateTime Id=\"Log Time\" Value=\"{}\"/>\n",
        xml(&metadata.date),
        xml(&metadata.time)
    ));
    if let Some(markers) = &markers {
        let (total_laps, fastest) = lap_summary(&markers.times_ns);
        detail(&mut out, "Total Laps", &total_laps.to_string());
        if let Some((seconds, lap)) = fastest {
            detail(&mut out, "Fastest Time", &lap_time(seconds));
            detail(&mut out, "Fastest Lap", &lap.to_string());
        }
        detail(&mut out, "Beacon Source", &markers.source_channel);
    } else {
        detail(&mut out, "Total Laps", "");
        detail(&mut out, "Fastest Time", "");
        detail(&mut out, "Fastest Lap", "");
    }
    out.push_str("  </Details>\n </Layers>\n</LDXFile>\n");
    out.into_bytes()
}

#[cfg(test)]
mod tests {
    use super::*;
    use motorsport_telemetry_core::{Channel, Chunk, SampleType, UnitSource};

    struct Source {
        channel: Channel,
        values: Vec<f64>,
    }

    impl TelemetrySource for Source {
        fn path(&self) -> &str {
            "laps.pds"
        }
        fn format(&self) -> &'static str {
            "pds"
        }
        fn channels(&self) -> &[Channel] {
            std::slice::from_ref(&self.channel)
        }
        fn decode(&self, _: usize, _: usize, local_index: u64) -> f64 {
            self.values[local_index as usize]
        }
    }

    fn source(name: &str, values: Vec<f64>) -> Source {
        let count = values.len() as u64;
        Source {
            channel: Channel {
                id: 1,
                name: name.into(),
                unit: String::new(),
                unit_source: UnitSource::Unknown,
                sample_type: SampleType::U8,
                chunks: vec![Chunk {
                    sample_period_ns: 1_000_000_000,
                    sample_count: count,
                    data_ptr: 0,
                    sample_base: 0,
                    time_base_ns: 0,
                }],
                sample_count: count,
                duration_ns: count * 1_000_000_000,
            },
            values,
        }
    }

    #[test]
    fn trigger_edges_become_beacons_and_summary() {
        let input = source(
            "lap_beacon_trig",
            vec![
                0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0,
            ],
        );
        let metadata = MotecMetadata {
            driver: "A&B".into(),
            ..Default::default()
        };
        let text = String::from_utf8(write_motec_ldx_bytes(&input, &metadata)).unwrap();
        assert!(text.contains("Name=\"Manual.1\"") && text.contains("Name=\"Manual.3\""));
        assert!(text.contains("Id=\"Total Laps\" Value=\"4\""));
        assert!(text.contains("Id=\"Fastest Time\" Value=\"0:06.000\""));
        assert!(text.contains("Value=\"A&amp;B\""));
    }

    #[test]
    fn lap_counter_is_a_fallback_and_resets_are_ignored() {
        let input = source(
            "Lap Number",
            vec![
                1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 3.0, 0.0,
            ],
        );
        let markers = infer_lap_markers(&input).unwrap();
        assert_eq!(markers.times_ns, vec![6_000_000_000, 12_000_000_000]);
    }

    #[test]
    fn no_signal_writes_metadata_without_inventing_markers() {
        let input = source("Speed", vec![1.0, 2.0]);
        let text =
            String::from_utf8(write_motec_ldx_bytes(&input, &MotecMetadata::default())).unwrap();
        assert!(!text.contains("<Marker "));
        assert!(text.contains("Id=\"Total Laps\" Value=\"\""));
    }
}
