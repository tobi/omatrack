//! AiM Sports `aimd` telemetry embedded in ISO Base Media (MP4) files.
//!
//! The MP4 is memory-mapped and only the samples belonging to the `aimd`
//! track are inspected. Video and audio payloads are never copied or decoded.
//! AiM channel definitions are read from the stream's `CHS` records; channel
//! order, offsets and names are not fixed in this reader.

use memmap2::Mmap;
use motorsport_telemetry_core::{Channel, Chunk, SampleType, TelemetrySource, UnitSource};
use std::{collections::HashMap, fs::File, path::Path};
use thiserror::Error;

const AIMD: &[u8; 4] = b"aimd";
const RECORD_START: &[u8; 2] = b"(S";

#[derive(Debug, Error)]
pub enum AimError {
    #[error("I/O error for {path}: {source}")]
    Io {
        path: String,
        source: std::io::Error,
    },
    #[error("MP4 file {path} has no aimd telemetry track")]
    NoAimdTrack { path: String },
    #[error("invalid AiM MP4 {path}: {message}")]
    Invalid { path: String, message: String },
}

fn invalid(path: &str, message: impl Into<String>) -> AimError {
    AimError::Invalid {
        path: path.into(),
        message: message.into(),
    }
}
fn be16(data: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_be_bytes(data.get(at..at + 2)?.try_into().ok()?))
}
fn be32(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_be_bytes(data.get(at..at + 4)?.try_into().ok()?))
}
fn be64(data: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_be_bytes(data.get(at..at + 8)?.try_into().ok()?))
}
fn be_i32(data: &[u8], at: usize) -> Option<i32> {
    Some(i32::from_be_bytes(data.get(at..at + 4)?.try_into().ok()?))
}
fn be_i64(data: &[u8], at: usize) -> Option<i64> {
    Some(i64::from_be_bytes(data.get(at..at + 8)?.try_into().ok()?))
}
fn le16(data: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(data.get(at..at + 2)?.try_into().ok()?))
}
fn le32(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

#[derive(Clone, Copy, Debug)]
struct BoxRef {
    kind: [u8; 4],
    payload: usize,
    end: usize,
}

fn boxes(data: &[u8], start: usize, end: usize) -> impl Iterator<Item = BoxRef> + '_ {
    let mut at = start;
    std::iter::from_fn(move || {
        if at.checked_add(8)? > end || end > data.len() {
            return None;
        }
        let base = at;
        let size32 = be32(data, base)? as u64;
        let kind: [u8; 4] = data.get(base + 4..base + 8)?.try_into().ok()?;
        let (header, size) = match size32 {
            0 => (8usize, (end - base) as u64),
            1 => (16usize, be64(data, base + 8)?),
            value => (8usize, value),
        };
        if size < header as u64 || size > usize::MAX as u64 {
            at = end;
            return None;
        }
        let box_end = base.checked_add(size as usize)?;
        if box_end > end {
            at = end;
            return None;
        }
        at = box_end;
        Some(BoxRef {
            kind,
            payload: base + header,
            end: box_end,
        })
    })
}

fn child(data: &[u8], parent: BoxRef, kind: &[u8; 4]) -> Option<BoxRef> {
    boxes(data, parent.payload, parent.end).find(|item| &item.kind == kind)
}

fn box_timescale(data: &[u8], item: BoxRef) -> Option<u32> {
    let version = *data.get(item.payload)?;
    let at = item.payload + if version == 1 { 20 } else { 12 };
    be32(data, at).filter(|value| *value != 0)
}

fn edit_offset_ns(data: &[u8], trak: BoxRef, movie_timescale: u32, media_timescale: u32) -> i64 {
    let Some(edts) = child(data, trak, b"edts") else {
        return 0;
    };
    let Some(elst) = child(data, edts, b"elst") else {
        return 0;
    };
    let Some(&version) = data.get(elst.payload) else {
        return 0;
    };
    let Some(count) = be32(data, elst.payload + 4) else {
        return 0;
    };
    let mut at = elst.payload + 8;
    let mut empty_duration = 0u64;
    for _ in 0..count {
        let entry = if version == 1 {
            let Some(duration) = be64(data, at) else {
                return 0;
            };
            let Some(media_time) = be_i64(data, at + 8) else {
                return 0;
            };
            at += 20;
            (duration, media_time)
        } else {
            let Some(duration) = be32(data, at) else {
                return 0;
            };
            let Some(media_time) = be_i32(data, at + 4) else {
                return 0;
            };
            at += 12;
            (duration as u64, media_time as i64)
        };
        if entry.1 == -1 {
            empty_duration = empty_duration.saturating_add(entry.0);
            continue;
        }
        let empty_ns = empty_duration as i128 * 1_000_000_000i128 / movie_timescale as i128;
        let media_ns = entry.1 as i128 * 1_000_000_000i128 / media_timescale as i128;
        return (empty_ns - media_ns).clamp(i64::MIN as i128, i64::MAX as i128) as i64;
    }
    (empty_duration as i128 * 1_000_000_000i128 / movie_timescale as i128)
        .clamp(i64::MIN as i128, i64::MAX as i128) as i64
}

#[derive(Debug)]
struct TrackSamples {
    timescale: u32,
    presentation_offset_ns: i64,
    samples: Vec<(u64, u32, u64)>, // file offset, byte size, decode timestamp
}

#[derive(Clone, Copy)]
struct Stsc {
    first_chunk: u32,
    samples_per_chunk: u32,
}

fn parse_track(
    data: &[u8],
    trak: BoxRef,
    path: &str,
    movie_timescale: u32,
) -> Result<Option<TrackSamples>, AimError> {
    let Some(mdia) = child(data, trak, b"mdia") else {
        return Ok(None);
    };
    let mdhd = child(data, mdia, b"mdhd").ok_or_else(|| invalid(path, "aimd track has no mdhd"))?;
    let version = *data
        .get(mdhd.payload)
        .ok_or_else(|| invalid(path, "truncated mdhd"))?;
    let timescale_at = mdhd.payload + if version == 1 { 20 } else { 12 };
    let timescale = be32(data, timescale_at)
        .filter(|v| *v != 0)
        .ok_or_else(|| invalid(path, "invalid aimd timescale"))?;
    let minf = child(data, mdia, b"minf").ok_or_else(|| invalid(path, "aimd track has no minf"))?;
    let stbl = child(data, minf, b"stbl").ok_or_else(|| invalid(path, "aimd track has no stbl"))?;

    let stsd = child(data, stbl, b"stsd").ok_or_else(|| invalid(path, "track has no stsd"))?;
    // AiM uses the standard `meta` handler and identifies telemetry by the
    // sample-entry FourCC. Do not rely on a localized handler name.
    if data.get(stsd.payload + 12..stsd.payload + 16) != Some(AIMD) {
        return Ok(None);
    }

    let stsz = child(data, stbl, b"stsz").ok_or_else(|| invalid(path, "aimd track has no stsz"))?;
    let default_size =
        be32(data, stsz.payload + 4).ok_or_else(|| invalid(path, "truncated stsz"))?;
    let count =
        be32(data, stsz.payload + 8).ok_or_else(|| invalid(path, "truncated stsz"))? as usize;
    let mut sizes = Vec::with_capacity(count);
    if default_size == 0 {
        for index in 0..count {
            sizes.push(
                be32(data, stsz.payload + 12 + index * 4)
                    .ok_or_else(|| invalid(path, "truncated stsz entries"))?,
            );
        }
    } else {
        sizes.resize(count, default_size);
    }

    let offsets = if let Some(stco) = child(data, stbl, b"stco") {
        let n =
            be32(data, stco.payload + 4).ok_or_else(|| invalid(path, "truncated stco"))? as usize;
        (0..n)
            .map(|i| be32(data, stco.payload + 8 + i * 4).map(u64::from))
            .collect::<Option<Vec<_>>>()
    } else if let Some(co64) = child(data, stbl, b"co64") {
        let n =
            be32(data, co64.payload + 4).ok_or_else(|| invalid(path, "truncated co64"))? as usize;
        (0..n)
            .map(|i| be64(data, co64.payload + 8 + i * 8))
            .collect::<Option<Vec<_>>>()
    } else {
        None
    }
    .ok_or_else(|| invalid(path, "aimd track has no chunk offsets"))?;

    let stsc_box =
        child(data, stbl, b"stsc").ok_or_else(|| invalid(path, "aimd track has no stsc"))?;
    let stsc_count =
        be32(data, stsc_box.payload + 4).ok_or_else(|| invalid(path, "truncated stsc"))? as usize;
    let mut stsc = Vec::with_capacity(stsc_count);
    for i in 0..stsc_count {
        let at = stsc_box.payload + 8 + i * 12;
        stsc.push(Stsc {
            first_chunk: be32(data, at).ok_or_else(|| invalid(path, "truncated stsc entries"))?,
            samples_per_chunk: be32(data, at + 4)
                .ok_or_else(|| invalid(path, "truncated stsc entries"))?,
        });
    }
    if stsc.first().is_none_or(|entry| entry.first_chunk != 1) {
        return Err(invalid(path, "invalid stsc first chunk"));
    }

    let stts = child(data, stbl, b"stts").ok_or_else(|| invalid(path, "aimd track has no stts"))?;
    let stts_count =
        be32(data, stts.payload + 4).ok_or_else(|| invalid(path, "truncated stts"))? as usize;
    let mut timestamps = Vec::with_capacity(count);
    let mut time = 0u64;
    for i in 0..stts_count {
        let at = stts.payload + 8 + i * 8;
        let n = be32(data, at).ok_or_else(|| invalid(path, "truncated stts entries"))?;
        let delta = be32(data, at + 4).ok_or_else(|| invalid(path, "truncated stts entries"))?;
        for _ in 0..n {
            timestamps.push(time);
            time = time.saturating_add(delta as u64);
        }
    }
    if timestamps.len() != sizes.len() {
        return Err(invalid(path, "stts/stsz sample counts differ"));
    }

    let mut locations = Vec::with_capacity(count);
    let mut sample = 0usize;
    for (chunk_index, chunk_offset) in offsets.into_iter().enumerate() {
        let chunk_number = chunk_index as u32 + 1;
        let entry = stsc
            .iter()
            .rev()
            .find(|entry| entry.first_chunk <= chunk_number)
            .unwrap();
        let mut offset = chunk_offset;
        for _ in 0..entry.samples_per_chunk {
            if sample >= sizes.len() {
                break;
            }
            let size = sizes[sample];
            if offset
                .checked_add(size as u64)
                .is_none_or(|end| end > data.len() as u64)
            {
                return Err(invalid(path, "aimd sample points outside the MP4"));
            }
            locations.push((offset, size, timestamps[sample]));
            offset += size as u64;
            sample += 1;
        }
    }
    if locations.len() != sizes.len() {
        return Err(invalid(path, "stsc does not address every aimd sample"));
    }
    Ok(Some(TrackSamples {
        timescale,
        presentation_offset_ns: edit_offset_ns(data, trak, movie_timescale, timescale),
        samples: locations,
    }))
}

fn aimd_track(data: &[u8], path: &str) -> Result<TrackSamples, AimError> {
    let moov = boxes(data, 0, data.len())
        .find(|item| &item.kind == b"moov")
        .ok_or_else(|| invalid(path, "MP4 has no moov box"))?;
    let movie_timescale = child(data, moov, b"mvhd")
        .and_then(|mvhd| box_timescale(data, mvhd))
        .unwrap_or(1);
    for trak in boxes(data, moov.payload, moov.end).filter(|item| &item.kind == b"trak") {
        if let Some(track) = parse_track(data, trak, path, movie_timescale)? {
            return Ok(track);
        }
    }
    Err(AimError::NoAimdTrack { path: path.into() })
}

#[derive(Clone, Copy, Debug)]
struct SampleRef {
    value_offset: u64,
    time_ns: u64,
}

#[derive(Debug)]
struct AimChannel {
    record_id: u16,
    width: usize,
    representation: Representation,
    samples: Vec<SampleRef>,
}

#[derive(Clone, Copy, Debug)]
enum Representation {
    U8,
    I32,
    U32,
    F32,
    GpsLatitude,
    GpsLongitude,
    GpsAltitude,
    GpsSpeed,
    GpsHeading,
    GpsSatellites,
    GpsPositionAccuracy,
    GpsSpeedAccuracy,
    GpsVelocityX,
    GpsVelocityY,
    GpsVelocityZ,
    GpsItow,
    GpsWeek,
    GpsDop,
    GpsFixFlags,
}

impl Representation {
    fn is_gps(self) -> bool {
        !matches!(self, Self::U8 | Self::I32 | Self::U32 | Self::F32)
    }
}

/// An AiM telemetry stream embedded in an MP4 recording.
#[derive(Debug)]
pub struct AimFile {
    pub path: String,
    pub channels: Vec<Channel>,
    data: Mmap,
    aim_channels: Vec<AimChannel>,
    media_time_offset_ns: i64,
}

fn c_string(bytes: &[u8]) -> String {
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).trim().to_owned()
}

fn tagged_blocks(sample: &[u8], tag: [u8; 3]) -> impl Iterator<Item = &[u8]> {
    let mut at = 0usize;
    std::iter::from_fn(move || {
        let relative = sample
            .get(at..)?
            .windows(5)
            .position(|window| window[0] == b'<' && window[1] == b'h' && window[2..5] == tag)?;
        let start = at + relative;
        let size = le32(sample, start + 6)? as usize;
        let payload = start.checked_add(12)?;
        let end = payload.checked_add(size)?;
        at = end;
        (end <= sample.len()).then_some(&sample[payload..end])
    })
}

fn gps_channel(
    id: u32,
    name: &str,
    unit: &str,
    sample_type: SampleType,
    representation: Representation,
) -> (Channel, AimChannel) {
    (
        Channel {
            id,
            name: name.into(),
            unit: unit.into(),
            unit_source: UnitSource::SpecDefault,
            sample_type,
            chunks: Vec::new(),
            sample_count: 0,
            duration_ns: 0,
        },
        AimChannel {
            record_id: u16::MAX,
            width: 56,
            representation,
            samples: Vec::new(),
        },
    )
}

fn gps_channels(first_id: u32) -> Vec<(Channel, AimChannel)> {
    use Representation::*;
    [
        ("GPS Latitude", "deg", SampleType::F64, GpsLatitude),
        ("GPS Longitude", "deg", SampleType::F64, GpsLongitude),
        ("GPS Altitude", "m", SampleType::F64, GpsAltitude),
        ("GPS Speed", "m/s", SampleType::F64, GpsSpeed),
        ("GPS Heading", "deg", SampleType::F64, GpsHeading),
        ("GPS Satellites", "count", SampleType::U8, GpsSatellites),
        (
            "GPS Position Accuracy",
            "m",
            SampleType::F64,
            GpsPositionAccuracy,
        ),
        (
            "GPS Speed Accuracy",
            "m/s",
            SampleType::F64,
            GpsSpeedAccuracy,
        ),
        ("GPS ECEF Velocity X", "m/s", SampleType::F64, GpsVelocityX),
        ("GPS ECEF Velocity Y", "m/s", SampleType::F64, GpsVelocityY),
        ("GPS ECEF Velocity Z", "m/s", SampleType::F64, GpsVelocityZ),
        ("GPS iTOW", "ms", SampleType::U32, GpsItow),
        ("GPS Week", "count", SampleType::U16, GpsWeek),
        ("GPS DOP", "ratio", SampleType::F64, GpsDop),
        ("GPS Fix Flags", "raw", SampleType::U32, GpsFixFlags),
    ]
    .into_iter()
    .enumerate()
    .map(|(index, (name, unit, sample_type, representation))| {
        gps_channel(
            first_id + index as u32,
            name,
            unit,
            sample_type,
            representation,
        )
    })
    .collect()
}

fn schema(sample: &[u8], path: &str) -> Result<Vec<(Channel, AimChannel)>, AimError> {
    if sample.get(6..10) != Some(b"amv0") {
        return Err(invalid(path, "first aimd sample has no amv0 signature"));
    }
    let mut result = Vec::new();
    let mut seen = HashMap::new();
    let mut gps_record = None;
    for payload in tagged_blocks(sample, *b"CHS") {
        if payload.len() < 100 {
            continue;
        }
        let record_id = le32(payload, 0).unwrap_or(u32::MAX);
        let width = le32(payload, 72).unwrap_or(0) as usize;
        let code = c_string(&payload[24..32]);
        let name = c_string(&payload[32..64]);
        if code == "GPS0" && width == 56 {
            gps_record = Some(record_id);
            continue;
        }
        if record_id > u16::MAX as u32 || name.is_empty() || !matches!(width, 1 | 4) {
            continue;
        }
        if seen.insert(record_id as u16, name.clone()).is_some() {
            return Err(invalid(
                path,
                format!("duplicate AiM record id {record_id}"),
            ));
        }
        // Representation is carried by the CHS schema rather than inferred
        // from the displayed name. Width-one records are statuses. Observed
        // encoding kind 12 is an integer timer and schema class 0x1003 is the
        // unsigned protocol clock; remaining scalar records are IEEE f32.
        let encoding_kind = le32(payload, 20).unwrap_or(0);
        let schema_class = le32(payload, 80).unwrap_or(0);
        let representation = if width == 1 {
            Representation::U8
        } else if schema_class == 0x1003 {
            Representation::U32
        } else if encoding_kind == 12 {
            Representation::I32
        } else {
            Representation::F32
        };
        let sample_type = match representation {
            Representation::U8 => SampleType::U8,
            Representation::I32 => SampleType::I32,
            Representation::U32 => SampleType::U32,
            Representation::F32 => SampleType::F32,
            _ => unreachable!("GPS representations are added after CHS scalar parsing"),
        };
        let channel = Channel {
            id: record_id,
            name,
            unit: String::new(),
            unit_source: UnitSource::Unknown,
            sample_type,
            chunks: Vec::new(),
            sample_count: 0,
            duration_ns: 0,
        };
        result.push((
            channel,
            AimChannel {
                record_id: record_id as u16,
                width,
                representation,
                samples: Vec::new(),
            },
        ));
    }
    if let Some(record_id) = gps_record {
        result.extend(gps_channels(record_id.saturating_add(1)));
    }
    if result.is_empty() {
        return Err(invalid(
            path,
            "aimd schema contains no supported scalar CHS records",
        ));
    }
    Ok(result)
}

fn period_chunks(samples: &[SampleRef]) -> Vec<Chunk> {
    if samples.is_empty() {
        return Vec::new();
    }
    if samples.len() == 1 {
        return vec![Chunk {
            sample_period_ns: 1,
            sample_count: 1,
            data_ptr: 0,
            sample_base: 0,
            time_base_ns: samples[0].time_ns,
        }];
    }
    // Logger clocks quantize nominal periods to whole milliseconds. Use the
    // modal delta only to identify acquisition gaps, then fit each contiguous
    // chunk across its exact endpoints so rounding drift cannot accumulate.
    let mut counts = HashMap::<u64, usize>::new();
    for pair in samples.windows(2) {
        let delta = pair[1].time_ns.saturating_sub(pair[0].time_ns);
        if delta > 0 {
            *counts.entry(delta).or_default() += 1;
        }
    }
    let nominal_period = counts
        .into_iter()
        .max_by_key(|&(delta, count)| (count, std::cmp::Reverse(delta)))
        .map(|(delta, _)| delta)
        .unwrap_or(1);
    let make_chunk = |start: usize, end: usize| {
        let sample_count = end - start;
        let sample_period_ns = if sample_count > 1 {
            samples[end - 1]
                .time_ns
                .saturating_sub(samples[start].time_ns)
                / (sample_count as u64 - 1)
        } else {
            nominal_period
        }
        .max(1);
        Chunk {
            sample_period_ns,
            sample_count: sample_count as u64,
            data_ptr: 0,
            sample_base: start as u64,
            time_base_ns: samples[start].time_ns,
        }
    };
    let mut chunks = Vec::new();
    let mut start = 0usize;
    for index in 1..samples.len() {
        let actual = samples[index].time_ns;
        let delta = actual.saturating_sub(samples[index - 1].time_ns);
        let expected = samples[start]
            .time_ns
            .saturating_add(nominal_period.saturating_mul((index - start) as u64));
        let drift = actual.abs_diff(expected);
        if delta >= nominal_period.saturating_mul(2) || drift > (nominal_period / 2).max(1) {
            chunks.push(make_chunk(start, index));
            start = index;
        }
    }
    chunks.push(make_chunk(start, samples.len()));
    chunks
}

impl AimFile {
    pub fn open(path: impl AsRef<Path>) -> Result<Self, AimError> {
        let path_ref = path.as_ref();
        let display = path_ref.to_string_lossy().into_owned();
        let file = File::open(path_ref).map_err(|source| AimError::Io {
            path: display.clone(),
            source,
        })?;
        // SAFETY: the mapping is read-only and kept alive by AimFile. As with
        // the other binary readers, callers must not concurrently truncate or
        // rewrite the file while it is open.
        let data = unsafe { Mmap::map(&file) }.map_err(|source| AimError::Io {
            path: display.clone(),
            source,
        })?;
        let track = aimd_track(&data, &display)?;
        let first = track
            .samples
            .first()
            .ok_or_else(|| invalid(&display, "empty aimd track"))?;
        let first_bytes = &data[first.0 as usize..first.0 as usize + first.1 as usize];
        if be16(first_bytes, 0).map(usize::from) != Some(first_bytes.len().saturating_sub(2)) {
            return Err(invalid(
                &display,
                "aimd packet length does not match MP4 sample size",
            ));
        }
        let definitions = schema(first_bytes, &display)?;
        let (mut channels, mut aim_channels): (Vec<_>, Vec<_>) = definitions.into_iter().unzip();
        let by_record = aim_channels
            .iter()
            .enumerate()
            .filter(|(_, channel)| !channel.representation.is_gps())
            .map(|(i, channel)| (channel.record_id, i))
            .collect::<HashMap<_, _>>();
        let gps_indices = aim_channels
            .iter()
            .enumerate()
            .filter(|(_, channel)| channel.representation.is_gps())
            .map(|(index, _)| index)
            .collect::<Vec<_>>();

        let mut clock_deltas = Vec::with_capacity(track.samples.len());
        for &(offset, size, packet_timestamp) in track.samples.iter().skip(1) {
            let packet = &data[offset as usize..offset as usize + size as usize];
            let mut packet_times = Vec::new();
            if be16(packet, 0).map(usize::from) != Some(packet.len().saturating_sub(2)) {
                return Err(invalid(
                    &display,
                    "aimd packet length does not match MP4 sample size",
                ));
            }
            if packet.get(6..10) != Some(b"amv0") {
                return Err(invalid(&display, "aimd packet has no amv0 signature"));
            }
            let mut at = 10usize;
            while let Some(relative) = packet
                .get(at..)
                .and_then(|tail| tail.windows(2).position(|window| window == RECORD_START))
            {
                let start = at + relative;
                let timestamp = le32(packet, start + 2)
                    .ok_or_else(|| invalid(&display, "truncated AiM sample record"))?;
                let record_id = le16(packet, start + 6)
                    .ok_or_else(|| invalid(&display, "truncated AiM sample record"))?;
                let Some(&index) = by_record.get(&record_id) else {
                    at = start + 2;
                    continue;
                };
                let width = aim_channels[index].width;
                let value = start + 8;
                if packet.get(value + width) != Some(&b')') {
                    at = start + 2;
                    continue;
                }
                aim_channels[index].samples.push(SampleRef {
                    value_offset: offset + value as u64,
                    time_ns: timestamp as u64 * 1_000_000,
                });
                packet_times.push(timestamp);
                at = value + width + 1;
            }

            let mut gps_at = 10usize;
            while let Some(relative) = packet
                .get(gps_at..)
                .and_then(|tail| tail.windows(5).position(|window| window == b"<hGPS"))
            {
                let header = gps_at + relative;
                let size = le32(packet, header + 6).unwrap_or(0) as usize;
                let payload = header + 12;
                let end = payload.saturating_add(size);
                if size == 56 && end <= packet.len() {
                    let timestamp = le32(packet, payload).unwrap_or(0);
                    packet_times.push(timestamp);
                    for &index in &gps_indices {
                        aim_channels[index].samples.push(SampleRef {
                            value_offset: offset + payload as u64,
                            time_ns: timestamp as u64 * 1_000_000,
                        });
                    }
                }
                gps_at = end.max(header + 5);
            }
            if !packet_times.is_empty() {
                packet_times.sort_unstable();
                let packet_clock_ns = packet_times[packet_times.len() / 2] as i128 * 1_000_000i128;
                let media_ns = track.presentation_offset_ns as i128
                    + packet_timestamp as i128 * 1_000_000_000i128 / track.timescale as i128;
                clock_deltas.push(packet_clock_ns - media_ns);
            }
        }
        if aim_channels
            .iter()
            .all(|channel| channel.samples.is_empty())
        {
            return Err(invalid(
                &display,
                "aimd packets contain no supported scalar records",
            ));
        }
        let origin = aim_channels
            .iter()
            .filter_map(|channel| channel.samples.first())
            .map(|sample| sample.time_ns)
            .min()
            .unwrap_or(0);
        for raw in &mut aim_channels {
            for sample in &mut raw.samples {
                sample.time_ns = sample.time_ns.saturating_sub(origin);
            }
        }
        for (channel, raw) in channels.iter_mut().zip(&aim_channels) {
            channel.sample_count = raw.samples.len() as u64;
            channel.chunks = period_chunks(&raw.samples);
            channel.duration_ns = raw
                .samples
                .last()
                .map(|sample| sample.time_ns + channel.first_period_ns().unwrap_or(1))
                .unwrap_or(0);
        }
        clock_deltas.sort_unstable();
        let clock_delta = clock_deltas
            .get(clock_deltas.len() / 2)
            .copied()
            .unwrap_or(origin as i128);
        let media_time_offset_ns =
            (origin as i128 - clock_delta).clamp(i64::MIN as i128, i64::MAX as i128) as i64;
        Ok(Self {
            path: display,
            channels,
            data,
            aim_channels,
            media_time_offset_ns,
        })
    }
}

fn gps_i32(data: &[u8], offset: usize) -> i32 {
    i32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
}

fn gps_u32(data: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
}

fn ecef_position(data: &[u8]) -> (f64, f64, f64) {
    let x = gps_i32(data, 16) as f64 / 100.0;
    let y = gps_i32(data, 20) as f64 / 100.0;
    let z = gps_i32(data, 24) as f64 / 100.0;
    let a = 6_378_137.0_f64;
    let e2 = 6.694_379_990_14e-3_f64;
    let longitude = y.atan2(x);
    let p = x.hypot(y);
    let mut latitude = z.atan2(p * (1.0 - e2));
    let mut altitude = 0.0;
    for _ in 0..8 {
        let n = a / (1.0 - e2 * latitude.sin().powi(2)).sqrt();
        altitude = p / latitude.cos() - n;
        latitude = z.atan2(p * (1.0 - e2 * n / (n + altitude)));
    }
    (latitude.to_degrees(), longitude.to_degrees(), altitude)
}

fn gps_velocity(data: &[u8]) -> (f64, f64, f64) {
    (
        gps_i32(data, 32) as f64 / 100.0,
        gps_i32(data, 36) as f64 / 100.0,
        gps_i32(data, 40) as f64 / 100.0,
    )
}

fn gps_heading(data: &[u8]) -> f64 {
    let (latitude, longitude, _) = ecef_position(data);
    let latitude = latitude.to_radians();
    let longitude = longitude.to_radians();
    let (vx, vy, vz) = gps_velocity(data);
    let east = -longitude.sin() * vx + longitude.cos() * vy;
    let north = -latitude.sin() * longitude.cos() * vx - latitude.sin() * longitude.sin() * vy
        + latitude.cos() * vz;
    if east.hypot(north) < 0.01 {
        0.0
    } else {
        east.atan2(north).to_degrees().rem_euclid(360.0)
    }
}

impl TelemetrySource for AimFile {
    fn path(&self) -> &str {
        &self.path
    }
    fn format(&self) -> &'static str {
        "aimd"
    }
    fn channels(&self) -> &[Channel] {
        &self.channels
    }
    fn media_time_offset_ns(&self) -> Option<i64> {
        Some(self.media_time_offset_ns)
    }
    fn decode(&self, channel_index: usize, chunk_index: usize, local_index: u64) -> f64 {
        let chunk = &self.channels[channel_index].chunks[chunk_index];
        let raw = &self.aim_channels[channel_index];
        let sample = raw.samples[(chunk.sample_base + local_index) as usize];
        let at = sample.value_offset as usize;
        match raw.representation {
            Representation::U8 => self.data[at] as f64,
            Representation::I32 => {
                i32::from_le_bytes(self.data[at..at + 4].try_into().unwrap()) as f64
            }
            Representation::U32 => {
                u32::from_le_bytes(self.data[at..at + 4].try_into().unwrap()) as f64
            }
            Representation::F32 => {
                f32::from_le_bytes(self.data[at..at + 4].try_into().unwrap()) as f64
            }
            Representation::GpsLatitude => ecef_position(&self.data[at..at + 56]).0,
            Representation::GpsLongitude => ecef_position(&self.data[at..at + 56]).1,
            Representation::GpsAltitude => ecef_position(&self.data[at..at + 56]).2,
            Representation::GpsSpeed => {
                let (x, y, z) = gps_velocity(&self.data[at..at + 56]);
                x.hypot(y).hypot(z)
            }
            Representation::GpsHeading => gps_heading(&self.data[at..at + 56]),
            Representation::GpsSatellites => (gps_u32(&self.data[at..at + 56], 48) >> 24) as f64,
            Representation::GpsPositionAccuracy => {
                gps_u32(&self.data[at..at + 56], 28) as f64 / 100.0
            }
            Representation::GpsSpeedAccuracy => gps_u32(&self.data[at..at + 56], 44) as f64 / 100.0,
            Representation::GpsVelocityX => gps_velocity(&self.data[at..at + 56]).0,
            Representation::GpsVelocityY => gps_velocity(&self.data[at..at + 56]).1,
            Representation::GpsVelocityZ => gps_velocity(&self.data[at..at + 56]).2,
            Representation::GpsItow => gps_u32(&self.data[at..at + 56], 4) as f64,
            Representation::GpsWeek => le16(&self.data[at..at + 56], 12).unwrap() as f64,
            Representation::GpsDop => {
                (gps_u32(&self.data[at..at + 56], 48) & 0x00ff_ffff) as f64 / 100.0
            }
            Representation::GpsFixFlags => gps_u32(&self.data[at..at + 56], 52) as f64,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn boxes_reject_truncated_size() {
        let data = [0, 0, 0, 20, b'm', b'o', b'o', b'v', 1, 2];
        assert_eq!(boxes(&data, 0, data.len()).count(), 0);
    }

    fn mp4_box(kind: &[u8; 4], payload: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(payload.len() + 8);
        out.extend_from_slice(&((payload.len() + 8) as u32).to_be_bytes());
        out.extend_from_slice(kind);
        out.extend_from_slice(payload);
        out
    }

    #[test]
    fn edit_list_maps_media_time_to_movie_time() {
        let mut payload = vec![0; 8];
        payload[4..8].copy_from_slice(&2u32.to_be_bytes());
        payload.extend_from_slice(&1000u32.to_be_bytes());
        payload.extend_from_slice(&(-1i32).to_be_bytes());
        payload.extend_from_slice(&0x0001_0000u32.to_be_bytes());
        payload.extend_from_slice(&5000u32.to_be_bytes());
        payload.extend_from_slice(&200i32.to_be_bytes());
        payload.extend_from_slice(&0x0001_0000u32.to_be_bytes());
        let elst = mp4_box(b"elst", &payload);
        let edts = mp4_box(b"edts", &elst);
        let data = mp4_box(b"trak", &edts);
        let trak = boxes(&data, 0, data.len()).next().unwrap();
        assert_eq!(edit_offset_ns(&data, trak, 1000, 1000), 800_000_000);
    }

    #[test]
    fn chunk_period_fits_quantized_logger_clock() {
        let samples = [0, 10_000_000, 21_000_000, 31_000_000].map(|time_ns| SampleRef {
            value_offset: 0,
            time_ns,
        });
        let chunks = period_chunks(&samples);
        assert_eq!(chunks.len(), 1);
        assert_eq!(chunks[0].sample_period_ns, 10_333_333);
    }

    #[test]
    fn chunks_split_before_clock_rate_drift_accumulates() {
        let mut time_ns = 0u64;
        let mut samples = vec![SampleRef {
            value_offset: 0,
            time_ns,
        }];
        for delta in [10_000_000; 20].into_iter().chain([11_000_000; 10]) {
            time_ns += delta;
            samples.push(SampleRef {
                value_offset: 0,
                time_ns,
            });
        }
        let chunks = period_chunks(&samples);
        assert!(chunks.len() > 1);
        for chunk in chunks {
            let last = (chunk.sample_base + chunk.sample_count - 1) as usize;
            let reconstructed =
                chunk.time_base_ns + (chunk.sample_count - 1) * chunk.sample_period_ns;
            assert!(reconstructed.abs_diff(samples[last].time_ns) <= 1);
        }
    }

    fn fixture_samples() -> (Vec<u8>, Vec<u8>) {
        let mut schema = vec![0, 0, 0x40, 0, 0, 0];
        schema.extend_from_slice(b"amv0s1");
        let mut definition = vec![0; 112];
        definition[0..4].copy_from_slice(&42u32.to_le_bytes());
        definition[24..27].copy_from_slice(b"RPM");
        definition[32..35].copy_from_slice(b"RPM");
        definition[72..76].copy_from_slice(&4u32.to_le_bytes());
        schema.extend_from_slice(b"<hCHS\0");
        schema.extend_from_slice(&(definition.len() as u32).to_le_bytes());
        schema.extend_from_slice(&[1, b'>']);
        schema.extend_from_slice(&definition);
        let mut gps_definition = vec![0; 112];
        gps_definition[0..4].copy_from_slice(&55u32.to_le_bytes());
        gps_definition[24..28].copy_from_slice(b"GPS0");
        gps_definition[32..36].copy_from_slice(b"GPS0");
        gps_definition[72..76].copy_from_slice(&56u32.to_le_bytes());
        schema.extend_from_slice(b"<hCHS\0");
        schema.extend_from_slice(&(gps_definition.len() as u32).to_le_bytes());
        schema.extend_from_slice(&[1, b'>']);
        schema.extend_from_slice(&gps_definition);
        let size = schema.len() - 2;
        schema[0..2].copy_from_slice(&(size as u16).to_be_bytes());

        let mut values = vec![0, 0, 0x40, 0, 0, 0];
        values.extend_from_slice(b"amv0");
        values.extend_from_slice(RECORD_START);
        values.extend_from_slice(&100u32.to_le_bytes());
        values.extend_from_slice(&42u16.to_le_bytes());
        values.extend_from_slice(&1234.5f32.to_le_bytes());
        values.push(b')');
        let mut gps = vec![0; 56];
        gps[0..4].copy_from_slice(&100u32.to_le_bytes());
        gps[4..8].copy_from_slice(&573_634_560u32.to_le_bytes());
        gps[12..14].copy_from_slice(&2429u16.to_le_bytes());
        gps[16..20].copy_from_slice(&16_174_352i32.to_le_bytes());
        gps[20..24].copy_from_slice(&(-460_842_617i32).to_le_bytes());
        gps[24..28].copy_from_slice(&439_210_627i32.to_le_bytes());
        gps[28..32].copy_from_slice(&783u32.to_le_bytes());
        gps[32..36].copy_from_slice(&5i32.to_le_bytes());
        gps[36..40].copy_from_slice(&(-10i32).to_le_bytes());
        gps[40..44].copy_from_slice(&8i32.to_le_bytes());
        gps[44..48].copy_from_slice(&6u32.to_le_bytes());
        gps[48..52].copy_from_slice(&0x0900_00f8u32.to_le_bytes());
        gps[52..56].copy_from_slice(&4096u32.to_le_bytes());
        values.extend_from_slice(b"<hGPS\0");
        values.extend_from_slice(&(gps.len() as u32).to_le_bytes());
        values.extend_from_slice(&[1, b'>']);
        values.extend_from_slice(&gps);
        let size = values.len() - 2;
        values[0..2].copy_from_slice(&(size as u16).to_be_bytes());
        (schema, values)
    }

    fn fixture_mp4(with_aimd: bool) -> Vec<u8> {
        let ftyp = mp4_box(b"ftyp", b"isom\0\0\0\0isom");
        let (schema, values) = fixture_samples();
        let mut media = Vec::new();
        media.extend_from_slice(&schema);
        media.extend_from_slice(&values);
        let mdat = mp4_box(b"mdat", &media);
        let chunk_offset = (ftyp.len() + 8) as u32;

        let mut mdhd = vec![0; 24];
        mdhd[12..16].copy_from_slice(&1000u32.to_be_bytes());
        let mdhd = mp4_box(b"mdhd", &mdhd);
        let mut hdlr = vec![0; 24];
        hdlr[8..12].copy_from_slice(b"meta");
        let hdlr = mp4_box(b"hdlr", &hdlr);
        let sample_type = if with_aimd { AIMD } else { b"text" };
        let mut stsd = vec![0; 16];
        stsd[7] = 1;
        stsd[8..12].copy_from_slice(&8u32.to_be_bytes());
        stsd[12..16].copy_from_slice(sample_type);
        let stsd = mp4_box(b"stsd", &stsd);
        let mut stts = vec![0; 16];
        stts[7] = 1;
        stts[8..12].copy_from_slice(&2u32.to_be_bytes());
        stts[12..16].copy_from_slice(&100u32.to_be_bytes());
        let stts = mp4_box(b"stts", &stts);
        let mut stsc = vec![0; 20];
        stsc[7] = 1;
        stsc[8..12].copy_from_slice(&1u32.to_be_bytes());
        stsc[12..16].copy_from_slice(&2u32.to_be_bytes());
        stsc[16..20].copy_from_slice(&1u32.to_be_bytes());
        let stsc = mp4_box(b"stsc", &stsc);
        let mut stsz = vec![0; 20];
        stsz[8..12].copy_from_slice(&2u32.to_be_bytes());
        stsz[12..16].copy_from_slice(&(schema.len() as u32).to_be_bytes());
        stsz[16..20].copy_from_slice(&(values.len() as u32).to_be_bytes());
        let stsz = mp4_box(b"stsz", &stsz);
        let mut stco = vec![0; 12];
        stco[7] = 1;
        stco[8..12].copy_from_slice(&chunk_offset.to_be_bytes());
        let stco = mp4_box(b"stco", &stco);
        let mut stbl_payload = Vec::new();
        for item in [stsd, stts, stsc, stsz, stco] {
            stbl_payload.extend(item);
        }
        let stbl = mp4_box(b"stbl", &stbl_payload);
        let minf = mp4_box(b"minf", &stbl);
        let mut mdia_payload = Vec::new();
        mdia_payload.extend(mdhd);
        mdia_payload.extend(hdlr);
        mdia_payload.extend(minf);
        let mdia = mp4_box(b"mdia", &mdia_payload);
        let trak = mp4_box(b"trak", &mdia);
        let moov = mp4_box(b"moov", &trak);
        [ftyp, mdat, moov].concat()
    }

    #[test]
    fn tagged_schema_uses_declared_name_and_record_id() {
        let mut sample = vec![0, 0, 0x40, 0, 0, 0];
        sample.extend_from_slice(b"amv0s1");
        let mut payload = vec![0; 112];
        payload[0..4].copy_from_slice(&42u32.to_le_bytes());
        payload[24..27].copy_from_slice(b"RPM");
        payload[32..35].copy_from_slice(b"RPM");
        payload[72..76].copy_from_slice(&4u32.to_le_bytes());
        sample.extend_from_slice(b"<hCHS\0");
        sample.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        sample.extend_from_slice(&[1, b'>']);
        sample.extend_from_slice(&payload);
        let parsed = schema(&sample, "fixture.mp4").unwrap();
        assert_eq!(parsed[0].0.name, "RPM");
        assert_eq!(parsed[0].1.record_id, 42);
        assert_eq!(parsed[0].1.width, 4);
    }

    #[test]
    fn reads_aimd_track_and_fails_fast_without_one() {
        let unique = format!("aim-{}.mp4", std::process::id());
        let path = std::env::temp_dir().join(unique);
        std::fs::write(&path, fixture_mp4(true)).unwrap();
        let file = AimFile::open(&path).unwrap();
        assert_eq!(file.media_time_offset_ns(), Some(100_000_000));
        assert_eq!(file.channels[0].name, "RPM");
        assert_eq!(file.channels[0].sample_count, 1);
        assert_eq!(file.decode(0, 0, 0), 1234.5);
        let latitude = file
            .channels
            .iter()
            .position(|channel| channel.name == "GPS Latitude")
            .unwrap();
        let longitude = file
            .channels
            .iter()
            .position(|channel| channel.name == "GPS Longitude")
            .unwrap();
        assert!((file.decode(latitude, 0, 0) - 43.797_816).abs() < 1e-6);
        assert!((file.decode(longitude, 0, 0) + 87.989_895).abs() < 1e-6);
        let speed = file
            .channels
            .iter()
            .position(|channel| channel.name == "GPS Speed")
            .unwrap();
        let satellites = file
            .channels
            .iter()
            .position(|channel| channel.name == "GPS Satellites")
            .unwrap();
        assert!((file.decode(speed, 0, 0) - 0.137_477).abs() < 1e-6);
        assert_eq!(file.decode(satellites, 0, 0), 9.0);
        drop(file);
        std::fs::write(&path, fixture_mp4(false)).unwrap();
        assert!(matches!(
            AimFile::open(&path),
            Err(AimError::NoAimdTrack { .. })
        ));
        std::fs::remove_file(path).unwrap();
    }
}
