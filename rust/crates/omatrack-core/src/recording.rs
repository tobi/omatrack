//! A telemetry recording opened from disk: format dispatch into the pinned
//! `motorsport-telemetry-rs` crates, channel metadata, lazy per-channel
//! decode, source-clock sampling and upstream laps.
//!
//! Port of `TelemetrySource::openImpl` plus the bridge's `parse_path`,
//! `build_handle` and `omatrack_channel_decode_all`, without the C ABI: the
//! core holds the upstream `TelemetrySource` directly.

use crate::laps::{Lap, LapKind};
use crate::mapping::{self, ChannelMapping, ChannelOverrides};
use crate::num::{llround, max};
use crate::video_clock::VideoClock;
use motorsport_telemetry_core::{TelemetrySource, read_source_metadata};
use std::collections::BTreeSet;
use std::fs::File;
use std::io::Read;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::Path;
use std::sync::{Arc, OnceLock};

/// A recording failed to open. The message is the parser's own.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("{0}")]
pub struct OpenError(pub String);

/// One source channel: metadata plus lazily decoded physical samples.
#[derive(Debug)]
pub struct RawChannel {
    pub name: String,
    pub unit: String,
    pub sample_type_code: u32,
    /// Source-reported sample count; after a full open, the decoded count.
    pub sample_count: u64,
    /// Sampling frequency in Hz, from the first chunk period.
    pub frequency_hz: f64,
    /// Total duration in seconds.
    pub duration_sec: f64,
    /// First-sample time, file-relative nanoseconds.
    pub start_ns: u64,
    samples: OnceLock<Arc<[f64]>>,
}

impl RawChannel {
    /// An in-memory channel (tests, synthetic sources).
    pub fn synthetic(
        name: &str,
        unit: &str,
        frequency_hz: f64,
        duration_sec: f64,
        samples: Vec<f64>,
    ) -> Self {
        let cell = OnceLock::new();
        let count = samples.len() as u64;
        let _ = cell.set(Arc::<[f64]>::from(samples));
        Self {
            name: name.to_string(),
            unit: unit.to_string(),
            sample_type_code: 0,
            sample_count: count,
            frequency_hz,
            duration_sec,
            start_ns: 0,
            samples: cell,
        }
    }

    /// True when the source carries data for this channel, decoded or not.
    pub fn has_samples(&self) -> bool {
        self.sample_count > 0 || self.samples.get().is_some_and(|s| !s.is_empty())
    }

    /// Decoded samples if they have been materialised.
    pub fn decoded(&self) -> Option<&Arc<[f64]>> {
        self.samples.get()
    }
}

/// An opened telemetry recording.
pub struct Recording {
    path: String,
    format: String,
    source: Option<Box<dyn TelemetrySource>>,
    channels: Vec<RawChannel>,
    source_laps: Vec<Lap>,
    video_clock: VideoClock,
    utc_start_ns: i64,
    duration_ns: u64,
    timezone: String,
    is_extension: bool,
    channel_visible: Vec<bool>,
}

impl std::fmt::Debug for Recording {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Recording")
            .field("path", &self.path)
            .field("format", &self.format)
            .field("channels", &self.channels.len())
            .field("source_laps", &self.source_laps.len())
            .finish_non_exhaustive()
    }
}

fn panic_message(payload: &(dyn std::any::Any + Send)) -> String {
    payload
        .downcast_ref::<&str>()
        .map(|s| (*s).to_string())
        .or_else(|| payload.downcast_ref::<String>().cloned())
        .unwrap_or_else(|| "unknown panic".to_string())
}

/// C strings cannot carry an interior NUL; the bridge replaced such names
/// with "" and so does the port.
fn c_string_value(value: &str) -> String {
    if value.contains('\0') {
        String::new()
    } else {
        value.to_string()
    }
}

/// Format dispatch (the bridge's `parse_path`, verbatim in behaviour).
fn parse_path(open_path: &Path, index_only: bool) -> Result<Box<dyn TelemetrySource>, String> {
    fn boxed<T: TelemetrySource + 'static, E: std::fmt::Display>(
        result: Result<T, E>,
    ) -> Result<Box<dyn TelemetrySource>, String> {
        result
            .map(|source| Box::new(source) as Box<dyn TelemetrySource>)
            .map_err(|error| error.to_string())
    }
    if telemetry_format::is_jsonl_path(open_path) {
        return boxed(telemetry_format::JsonlRecording::open(open_path));
    }
    let ext = open_path
        .extension()
        .and_then(|value| value.to_str())
        .map(str::to_ascii_lowercase)
        .unwrap_or_default();
    match ext.as_str() {
        "mp4" => {
            if index_only {
                boxed(aim_telemetry::AimFile::open_index(open_path))
            } else {
                boxed(aim_telemetry::AimFile::open(open_path))
            }
        }
        "pds" => boxed(cosworth_telemetry::CosworthFile::open(open_path)),
        "ld" => boxed(motec_telemetry::MotecFile::open(open_path)),
        "vbo" => {
            if index_only {
                boxed(racelogic_telemetry::RacelogicFile::open_metadata(open_path))
            } else {
                boxed(racelogic_telemetry::RacelogicFile::open(open_path))
            }
        }
        "telemetry" => {
            // MTJ may use the standard .telemetry name: dispatch plain/zstd
            // MTJ by magic; ZIP archives are NativeRecording.
            let mut magic = [0u8; 4];
            let jsonl_encoding = File::open(open_path)
                .and_then(|mut file| file.read_exact(&mut magic))
                .is_ok()
                && (magic == [0x28, 0xb5, 0x2f, 0xfd] || magic[0] == b'{');
            if jsonl_encoding {
                telemetry_format::JsonlRecording::open(open_path)
                    .map_err(|error| error.to_string())
                    .and_then(|source| {
                        if source.is_extension() {
                            Err(".telemetry requires an MTJ recording, not an MTX extension"
                                .to_string())
                        } else {
                            Ok(Box::new(source) as Box<dyn TelemetrySource>)
                        }
                    })
            } else {
                boxed(telemetry_format::NativeRecording::open(open_path))
            }
        }
        other => Err(format!("unsupported telemetry format: {other:?}")),
    }
}

/// Upstream laps (`read_source_metadata`) with the C++ id assignment:
/// preserve a unique source number, else the lowest unused id.
fn source_laps(source: &dyn TelemetrySource) -> Vec<Lap> {
    let raw: Vec<_> = read_source_metadata(source)
        .laps
        .into_iter()
        .filter(|lap| lap.end_ns > lap.start_ns)
        .collect();
    let mut used: BTreeSet<i32> = BTreeSet::new();
    let mut next_fallback = 0i32;
    let mut laps = Vec::with_capacity(raw.len());
    for lap in raw {
        let fits = i32::try_from(lap.number).ok();
        let preserved = fits.filter(|number| !used.contains(number));
        let id = match preserved {
            Some(number) => number,
            None => {
                while used.contains(&next_fallback) {
                    next_fallback += 1;
                }
                let id = next_fallback;
                next_fallback += 1;
                id
            }
        };
        used.insert(id);
        let duration_ns = if lap.duration_ns > 0 {
            lap.duration_ns
        } else {
            lap.end_ns - lap.start_ns
        };
        let mut entry = Lap::new(
            id,
            lap.start_ns as f64 / 1e9,
            lap.end_ns as f64 / 1e9,
            duration_ns as f64 / 1e6,
            lap.complete,
        );
        entry.source_number = preserved;
        entry.first_video_frame = lap.first_video_frame;
        entry.kind = LapKind::from_upstream(lap.kind);
        laps.push(entry);
    }
    laps
}

/// The bridge's `omatrack_format` names.
fn format_name(source: &dyn TelemetrySource, is_extension: bool) -> &'static str {
    match source.format() {
        "aimd" => "aimd",
        "pds" => "pds",
        "ld" | "motec" => "ld",
        "vbo" => "vbo",
        "telemetry" => "telemetry",
        "jsonl" if is_extension => "mtx",
        "jsonl" => "jsonl",
        _ if is_extension => "mtx",
        _ => "unknown",
    }
}

/// Every decoded sample of one channel, in chunk (= time) order.
fn decode_all(source: &dyn TelemetrySource, index: usize) -> Vec<f64> {
    let Some(channel) = source.channels().get(index) else {
        return Vec::new();
    };
    let capacity = channel.sample_count as usize;
    let mut out = Vec::with_capacity(capacity);
    for (chunk_index, chunk) in channel.chunks.iter().enumerate() {
        if out.len() >= capacity {
            break;
        }
        let count = (chunk.sample_count as usize).min(capacity - out.len());
        for i in 0..count {
            out.push(source.decode(index, chunk_index, i as u64));
        }
    }
    out
}

impl Recording {
    /// Open for analysis: every channel decoded.
    pub fn open(path: impl AsRef<Path>) -> Result<Self, OpenError> {
        Self::open_impl(path.as_ref(), false)
    }

    /// Open a bounded metadata view for library indexing. AiM retains
    /// complete lap signals but omits the video-frame index; channels decode
    /// on first use.
    pub fn open_index(path: impl AsRef<Path>) -> Result<Self, OpenError> {
        Self::open_impl(path.as_ref(), true)
    }

    fn open_impl(path: &Path, index_only: bool) -> Result<Self, OpenError> {
        let Some(path_str) = path.to_str() else {
            return Err(OpenError("path is not valid UTF-8".to_string()));
        };
        let opened = catch_unwind(AssertUnwindSafe(|| Self::build(path_str, index_only)));
        match opened {
            Ok(result) => result,
            Err(payload) => Err(OpenError(format!("panic: {}", panic_message(&*payload)))),
        }
    }

    fn build(path: &str, index_only: bool) -> Result<Self, OpenError> {
        let source = parse_path(Path::new(path), index_only).map_err(OpenError)?;
        let src = source.as_ref();

        let jsonl = telemetry_format::is_jsonl_path(Path::new(src.path()));
        let channel_duration_ns = src
            .channels()
            .iter()
            .map(|channel| channel.duration_ns)
            .max()
            .unwrap_or(0);
        let mut is_extension = false;
        let mut utc_start_ns = src
            .utc_start_ns()
            .and_then(|value| i64::try_from(value).ok())
            .unwrap_or(-1);
        let mut timezone = c_string_value(&src.timezone());
        let mut duration_ns = channel_duration_ns;
        let mut channel_visible: Vec<bool> = if jsonl {
            vec![true; src.channels().len()]
        } else {
            src.channel_visible().to_vec()
        };
        if jsonl && let Ok(document) = telemetry_format::JsonlRecording::open(src.path()) {
            duration_ns = document.duration_ns();
            is_extension = document.is_extension();
            if let Some(group) = document.sidecar_groups().first() {
                timezone = c_string_value(&group.header.timezone);
                utc_start_ns = i64::try_from(group.header.utc_start_ns).unwrap_or(-1);
                duration_ns = group.duration_ns;
            }
            channel_visible = document.channel_visible().to_vec();
            if channel_visible.len() < src.channels().len() {
                channel_visible.resize(src.channels().len(), true);
            }
        }

        let format = format_name(src, is_extension).to_string();
        let source_laps = source_laps(src);
        let video_clock = VideoClock::from_source(src);

        let mut channels = Vec::with_capacity(src.channels().len());
        for (index, channel) in src.channels().iter().enumerate() {
            let first = channel.chunks.first();
            let period = first.map(|chunk| chunk.sample_period_ns).unwrap_or(0);
            let mut raw = RawChannel {
                name: c_string_value(&channel.name),
                unit: c_string_value(&channel.unit),
                sample_type_code: channel.sample_type.code(),
                sample_count: channel.sample_count,
                frequency_hz: if period > 0 { 1e9 / period as f64 } else { 0.0 },
                duration_sec: channel.duration_ns as f64 / 1e9,
                start_ns: first.map(|chunk| chunk.time_base_ns).unwrap_or(0),
                samples: OnceLock::new(),
            };
            if !index_only && channel.sample_count > 0 {
                let decoded = decode_all(src, index);
                raw.sample_count = decoded.len() as u64;
                let _ = raw.samples.set(Arc::from(decoded));
            }
            channels.push(raw);
        }

        let mut recording = Self {
            path: path.to_string(),
            format,
            source: None,
            channels,
            source_laps,
            video_clock,
            utc_start_ns,
            duration_ns,
            timezone,
            is_extension,
            channel_visible,
        };
        recording.source = Some(source);

        if recording.duration_ns == 0 {
            for channel in &recording.channels {
                recording.duration_ns = recording
                    .duration_ns
                    .max(llround(max(0.0, channel.duration_sec) * 1e9) as u64);
            }
        }
        if recording.utc_start_ns < 0 {
            recording.utc_start_ns = recording.utc_from_gps_clock();
        }
        Ok(recording)
    }

    /// First usable GPS week/iTOW pair, projected back to file t = 0.
    fn utc_from_gps_clock(&self) -> i64 {
        let mapping = self.map_channels(&ChannelOverrides::new());
        let (Some(&week), Some(&itow)) = (mapping.get("gps_week"), mapping.get("gps_itow")) else {
            return -1;
        };
        let week_channel = &self.channels[week];
        let itow_channel = &self.channels[itow];
        let week_samples = self.samples(week);
        let itow_samples = self.samples(itow);
        let count = week_samples.len().min(itow_samples.len());
        let freq = if week_channel.frequency_hz > 0.0 {
            week_channel.frequency_hz
        } else {
            itow_channel.frequency_hz
        };
        for index in 0..count {
            let file_time = if freq > 0.0 { index as f64 / freq } else { 0.0 };
            let utc = crate::meta::utc_start_ns_from_gps(
                week_samples[index],
                itow_samples[index],
                file_time,
            );
            if utc >= 0 {
                return utc;
            }
        }
        -1
    }

    /// An in-memory recording without a parser (tests and synthetic data).
    /// Sampling interpolates the arrays at their declared frequency.
    pub fn synthetic(format: &str, channels: Vec<RawChannel>) -> Self {
        Self {
            path: String::new(),
            format: format.to_string(),
            source: None,
            channels,
            source_laps: Vec::new(),
            video_clock: VideoClock::default(),
            utc_start_ns: -1,
            duration_ns: 0,
            timezone: String::new(),
            is_extension: false,
            channel_visible: Vec::new(),
        }
    }

    /// Replace the upstream laps (synthetic recordings only need this).
    pub fn set_source_laps(&mut self, laps: Vec<Lap>) {
        self.source_laps = laps;
    }

    pub fn path(&self) -> &str {
        &self.path
    }
    /// `aimd`, `pds`, `ld`, `vbo`, `telemetry`, `jsonl`, `mtx` or `unknown`.
    pub fn format_name(&self) -> &str {
        &self.format
    }
    pub fn channels(&self) -> &[RawChannel] {
        &self.channels
    }
    /// Upstream `read_source_metadata` laps, unclassified.
    pub fn source_laps(&self) -> &[Lap] {
        &self.source_laps
    }
    pub fn video_clock(&self) -> &VideoClock {
        &self.video_clock
    }
    /// Unix-epoch nanoseconds at file t = 0, or -1 without a wall clock.
    pub fn utc_start_ns(&self) -> i64 {
        self.utc_start_ns
    }
    /// Exclusive file-relative duration in nanoseconds.
    pub fn duration_ns(&self) -> u64 {
        self.duration_ns
    }
    /// IANA timezone the recording declares (may be empty).
    pub fn timezone(&self) -> &str {
        &self.timezone
    }
    /// True for an MTX sidecar document rather than a host recording.
    pub fn is_extension(&self) -> bool {
        self.is_extension
    }
    /// Per-channel default visibility (MTJ `vis`); missing means visible.
    pub fn channel_default_visible(&self, index: usize) -> bool {
        self.channel_visible.get(index).copied().unwrap_or(true)
    }

    /// Decoded samples of one channel, decoding on first use.
    pub fn samples(&self, index: usize) -> &[f64] {
        let Some(channel) = self.channels.get(index) else {
            return &[];
        };
        channel.samples.get_or_init(|| match &self.source {
            Some(source) if channel.sample_count > 0 => {
                Arc::from(decode_all(source.as_ref(), index))
            }
            _ => Arc::from(Vec::new()),
        })
    }

    /// Shared handle to one channel's decoded samples.
    pub fn samples_arc(&self, index: usize) -> Option<Arc<[f64]>> {
        self.channels.get(index)?;
        self.samples(index);
        self.channels[index].samples.get().cloned()
    }

    /// Offset satisfying presentation time = telemetry time + offset.
    pub fn video_presentation_offset_sec(&self) -> Option<f64> {
        self.video_clock
            .presentation_offset_ns
            .map(|offset| offset as f64 / 1e9)
    }

    /// Exact player presentation time at file-relative telemetry time.
    pub fn video_presentation_time(&self, time_sec: f64) -> Option<f64> {
        let source = self.source.as_ref()?;
        if !time_sec.is_finite() || time_sec < 0.0 {
            return None;
        }
        let time_ns = llround(time_sec * 1e9) as u64;
        source
            .video_presentation_time_ns(time_ns)
            .map(|ns| ns as f64 / 1e9)
    }

    /// Presentation-order video frame at file-relative telemetry time.
    pub fn video_frame_at(&self, time_sec: f64) -> Option<u64> {
        let source = self.source.as_ref()?;
        if !time_sec.is_finite() || time_sec < 0.0 {
            return None;
        }
        source.video_frame_at(llround(time_sec * 1e9) as u64)
    }

    /// Sample a channel at file-relative seconds through the source clock.
    /// Linear by default; `linear = false` for ordinals such as gear.
    pub fn sample_at(&self, index: usize, time_sec: f64, linear: bool) -> Option<f64> {
        if index >= self.channels.len() || !time_sec.is_finite() || time_sec < 0.0 {
            return None;
        }
        if let Some(source) = &self.source {
            let time_ns = llround(time_sec * 1e9) as u64;
            return catch_unwind(AssertUnwindSafe(|| {
                source.sample_at(index, time_ns, linear)
            }))
            .ok()
            .flatten();
        }
        let channel = &self.channels[index];
        let samples = self.samples(index);
        if samples.is_empty() || !(channel.frequency_hz > 0.0) {
            return None;
        }
        let position = time_sec * channel.frequency_hz;
        if position < 0.0 || position > (samples.len() - 1) as f64 {
            return None;
        }
        let low = position.floor() as usize;
        let high = (low + 1).min(samples.len() - 1);
        if !linear {
            return Some(if position - (low as f64) < 0.5 {
                samples[low]
            } else {
                samples[high]
            });
        }
        let fraction = position - low as f64;
        Some(samples[low] + (samples[high] - samples[low]) * fraction)
    }

    /// Sample at integer file-relative nanoseconds (overlay joins).
    pub fn sample_at_ns(&self, index: usize, time_ns: u64, linear: bool) -> Option<f64> {
        if index >= self.channels.len() {
            return None;
        }
        if let Some(source) = &self.source {
            return catch_unwind(AssertUnwindSafe(|| {
                source.sample_at(index, time_ns, linear)
            }))
            .ok()
            .flatten();
        }
        self.sample_at(index, time_ns as f64 / 1e9, linear)
    }

    /// Canonical concept -> channel index.
    pub fn map_channels(&self, overrides: &ChannelOverrides) -> ChannelMapping {
        mapping::map_channels(&self.channels, overrides)
    }

    /// Dominant positive driver code from the mapped channel; 0 if absent.
    pub fn detect_driver_id(&self, overrides: &ChannelOverrides) -> f64 {
        let mapping = self.map_channels(overrides);
        let Some(&index) = mapping.get("driver_id") else {
            return 0.0;
        };
        mapping::dominant_driver_id(self.samples(index), self.channels[index].sample_type_code)
    }

    /// The channel's source-clock series on a uniform grid at its own rounded
    /// frequency, edges held (the lap detectors' input).
    fn detector_series(&self, index: Option<usize>) -> (Vec<f64>, i32) {
        let Some(index) = index else {
            return (Vec::new(), 0);
        };
        let channel = &self.channels[index];
        if !channel.has_samples() || channel.duration_sec <= 0.0 {
            return (Vec::new(), 0);
        }
        let frequency = (crate::num::llround(channel.frequency_hz) as i32).max(1);
        let count = (channel.duration_sec * f64::from(frequency)).ceil() as usize;
        let mut values = vec![0.0; count];
        let mut sampled = false;
        let mut last_value = 0.0;
        for i in 0..count {
            let time = i as f64 / f64::from(frequency);
            let value = if let Some(value) = self.sample_at(index, time, true) {
                if !sampled {
                    for slot in &mut values[..i] {
                        *slot = value;
                    }
                }
                sampled = true;
                last_value = value;
                value
            } else if sampled {
                last_value
            } else {
                0.0
            };
            values[i] = value;
        }
        if !sampled {
            return (Vec::new(), 0);
        }
        (values, frequency)
    }

    fn first_channel(&self, aliases: &[&str], contains: bool) -> Option<usize> {
        for alias in aliases {
            let wanted = mapping::normalize_channel_name(alias);
            if let Some(index) = self.channels.iter().position(|channel| {
                channel.has_samples() && mapping::normalize_channel_name(&channel.name) == wanted
            }) {
                return Some(index);
            }
        }
        if !contains {
            return None;
        }
        for alias in aliases {
            let wanted = mapping::normalize_channel_name(alias);
            if wanted.len() < 4 {
                continue;
            }
            if let Some(index) = self.channels.iter().position(|channel| {
                channel.has_samples()
                    && mapping::normalize_channel_name(&channel.name).contains(&wanted)
            }) {
                return Some(index);
            }
        }
        None
    }

    /// Reliable format-neutral laps: upstream's list when it has one, else
    /// the beacon / lap-number / lap-time / lap-distance heuristics.
    pub fn detect_laps(&self) -> Vec<Lap> {
        use crate::laps::*;
        let lap_distance_id = self.first_channel(&["lap distance corrected", "lap distance"], true);
        let (lap_distance, distance_freq) = self.detector_series(lap_distance_id);

        if !self.source_laps.is_empty() {
            let mut laps = self.source_laps.clone();
            // Completeness means both boundaries are known.
            mark_short_crossings_incomplete(&mut laps);
            if !lap_distance.is_empty() {
                laps = pds_apply_lap_distance_coverage(&laps, &lap_distance, distance_freq.max(1));
            }
            return laps;
        }

        let lap_beacon_id =
            self.first_channel(&["lap_beacon_trig", "laptrigger", "lap_beacon"], true);
        let lap_number_id = self.first_channel(&["lap number"], true);
        // Exact names only: a contains match on "lap time" would pick
        // Delta_Lap_Time / Ref_Lap_Time ahead of Current_Lap_Time.
        let lap_time_id = self.first_channel(
            &[
                "current lap time",
                "lap current lap time",
                "lap time running",
                "lap time",
            ],
            false,
        );
        let previous_lap_time_id = self.first_channel(
            &[
                "previous lap time",
                "previous lt",
                "last lap time",
                "last lt",
            ],
            false,
        );
        let (lap_beacon, beacon_freq) = self.detector_series(lap_beacon_id);
        let (lap_number, number_freq) = self.detector_series(lap_number_id);
        let (lap_time, time_freq) = self.detector_series(lap_time_id);
        let (prev_lap_time, prev_freq) = self.detector_series(previous_lap_time_id);

        let mut max_duration = 0.0;
        for channel in &self.channels {
            max_duration = max(max_duration, channel.duration_sec);
        }
        let beacon_splits = pds_beacon_splits(&lap_beacon, beacon_freq);
        let lap_number_splits = pds_lap_number_splits(&lap_number, number_freq);
        let lap_number_is_authority = lap_number_carries_state(&lap_number);
        let mut lap_time_splits = pds_lap_time_splits(&lap_time, time_freq);
        if lap_time_splits.len() < 2 {
            lap_time_splits = pds_last_lap_time_splits(&prev_lap_time, prev_freq);
        }
        let split_times = select_lap_splits(
            &beacon_splits,
            &lap_number_splits,
            lap_number_is_authority,
            &lap_time_splits,
            &pds_distance_splits(&lap_distance, distance_freq),
        );
        let mut laps = build_laps_from_splits(&split_times, max_duration, true);
        if !prev_lap_time.is_empty() {
            laps = pds_apply_previous_lap_times(
                &laps,
                &prev_lap_time,
                prev_freq.max(1),
                !lap_number_is_authority,
            );
        }
        if !lap_number_is_authority && !lap_distance.is_empty() {
            laps = pds_apply_lap_distance_coverage(&laps, &lap_distance, distance_freq.max(1));
        }
        laps
    }
}
