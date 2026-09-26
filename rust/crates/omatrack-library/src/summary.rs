//! The recording's own metadata, obtained through a cheap index open.
//!
//! This summary builds the library tree and is stored by the index cache. It contains
//! metadata layer 4 only, so preferences and `TRACK.yml` edits never invalidate it.

use omatrack_core::laps::{Lap, LapKind, classify_laps};
use omatrack_core::mapping::{ChannelOverrides, gps_coordinate_degrees, lower_trimmed};
use omatrack_core::recording::Recording;
use serde::{Deserialize, Serialize};

/// One lap as the index remembers it (plain data).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[non_exhaustive]
pub struct LapSummary {
    pub id: i32,
    pub start_time: f64,
    pub end_time: f64,
    pub time_ms: f64,
    pub complete: bool,
    pub pit_lap: bool,
    /// Upstream role: `unknown`, `flying`, `out`, `in`, `out-in`, `pit`.
    pub kind: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub source_number: Option<i32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub first_video_frame: Option<u64>,
}

fn kind_key(kind: LapKind) -> &'static str {
    match kind {
        LapKind::Unknown => "unknown",
        LapKind::Flying => "flying",
        LapKind::Out => "out",
        LapKind::In => "in",
        LapKind::OutIn => "out-in",
        LapKind::Pit => "pit",
    }
}

fn kind_from_key(key: &str) -> LapKind {
    match key {
        "flying" => LapKind::Flying,
        "out" => LapKind::Out,
        "in" => LapKind::In,
        "out-in" => LapKind::OutIn,
        "pit" => LapKind::Pit,
        _ => LapKind::Unknown,
    }
}

fn finite(value: f64) -> f64 {
    if value.is_finite() { value } else { 0.0 }
}

impl LapSummary {
    pub fn from_lap(lap: &Lap) -> Self {
        Self {
            id: lap.id,
            start_time: finite(lap.start_time),
            end_time: finite(lap.end_time),
            time_ms: finite(lap.time_ms),
            complete: lap.complete,
            pit_lap: lap.is_pit_lap,
            kind: kind_key(lap.kind).to_string(),
            source_number: lap.source_number,
            first_video_frame: lap.first_video_frame,
        }
    }

    /// The core lap (classification included).
    pub fn to_lap(&self) -> Lap {
        let mut lap = Lap::new(
            self.id,
            self.start_time,
            self.end_time,
            self.time_ms,
            self.complete,
        );
        lap.is_pit_lap = self.pit_lap;
        lap.kind = kind_from_key(&self.kind);
        lap.source_number = self.source_number;
        lap.first_video_frame = self.first_video_frame;
        lap
    }
}

/// A recording's own metadata from an index open (plain data).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[non_exhaustive]
pub struct RecordingSummary {
    /// `aimd`, `pds`, `ld`, `vbo`, `telemetry`, `jsonl`, ...
    pub format: String,
    /// Unix-epoch nanoseconds at file t = 0, or -1 without a wall clock.
    pub utc_start_ns: i64,
    /// IANA timezone the recording declares (may be empty).
    pub timezone: String,
    pub duration_ns: u64,
    /// Dominant positive driver code; 0 when the logger has none.
    pub driver_id: f64,
    /// Median GPS position `[lat, lon]` in degrees, when the logger has GPS.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gps: Option<[f64; 2]>,
    /// The recording carries a usable video clock.
    pub has_video: bool,
    pub channel_count: usize,
    /// Classified laps in recording order.
    pub laps: Vec<LapSummary>,
}

/// Median of 19 probes of the mapped GPS channels (port of
/// `SessionHandle::captureGpsLocation`).
#[expect(
    clippy::neg_cmp_op_on_partial_ord,
    reason = "Negated ordered comparisons deliberately include unordered (NaN) values; preserve that behavior."
)]
fn gps_location(recording: &Recording) -> Option<[f64; 2]> {
    let mapping = recording.map_channels(&ChannelOverrides::new());
    let (&lat_index, &lon_index) = (mapping.get("gps_lat")?, mapping.get("gps_lon")?);
    let channels = recording.channels();
    let (lat_channel, lon_channel) = (channels.get(lat_index)?, channels.get(lon_index)?);
    let duration = lat_channel.duration_sec.min(lon_channel.duration_sec);
    if !(duration > 0.0) {
        return None;
    }
    let lat_unit = lower_trimmed(&lat_channel.unit);
    let lon_unit = lower_trimmed(&lon_channel.unit);
    let mut latitudes = Vec::new();
    let mut longitudes = Vec::new();
    for sample in 1..=19 {
        let time = duration * f64::from(sample) / 20.0;
        let (Some(lat), Some(lon)) = (
            recording.sample_at(lat_index, time, true),
            recording.sample_at(lon_index, time, true),
        ) else {
            continue;
        };
        let lat = gps_coordinate_degrees(lat, &lat_unit, false);
        let lon = gps_coordinate_degrees(lon, &lon_unit, true);
        if !lat.is_finite()
            || !lon.is_finite()
            || lat.abs() > 90.0
            || lon.abs() > 180.0
            || (lat.abs() < 0.001 && lon.abs() < 0.001)
        {
            continue;
        }
        latitudes.push(lat);
        longitudes.push(lon);
    }
    if latitudes.is_empty() {
        return None;
    }
    latitudes.sort_by(f64::total_cmp);
    longitudes.sort_by(f64::total_cmp);
    Some([
        latitudes[latitudes.len() / 2],
        longitudes[longitudes.len() / 2],
    ])
}

impl RecordingSummary {
    /// Summarize an (index-)opened recording: laps detected and classified,
    /// driver id, GPS location, video presence.
    pub fn from_recording(recording: &Recording) -> Self {
        let mut laps = recording.detect_laps();
        classify_laps(&mut laps);
        let driver_id = recording.detect_driver_id(&ChannelOverrides::new());
        Self {
            format: recording.format_name().to_string(),
            utc_start_ns: recording.utc_start_ns(),
            timezone: recording.timezone().to_string(),
            duration_ns: recording.duration_ns(),
            driver_id: if driver_id.is_finite() && driver_id > 0.0 {
                driver_id
            } else {
                0.0
            },
            gps: gps_location(recording),
            has_video: recording.video_clock().valid()
                || omatrack_core::session::is_video_path(std::path::Path::new(recording.path())),
            channel_count: recording.channels().len(),
            laps: laps.iter().map(LapSummary::from_lap).collect(),
        }
    }

    /// The laps as core laps, classified, in recording order.
    pub fn laps(&self) -> Vec<Lap> {
        self.laps.iter().map(LapSummary::to_lap).collect()
    }

    /// The detected driver id, when there is one.
    pub fn driver_id(&self) -> Option<f64> {
        (self.driver_id > 0.0).then_some(self.driver_id)
    }
}
