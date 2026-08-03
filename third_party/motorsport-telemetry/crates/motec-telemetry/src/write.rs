//! MoTeC LD writer.
//!
//! Serialises any [`TelemetrySource`] into a MoTeC LD file. The writer is
//! deliberately conservative: rather than silently degrading a recording it
//! refuses to write anything that cannot be recovered byte-for-byte by
//! [`crate::MotecFile`].
//!
//! Layout produced (all little-endian):
//!
//! ```text
//! 0x0000  header          (HEADER_SIZE bytes)
//! 0x06e2  event block     (EVENT_SIZE bytes)   -> venue_ptr
//! 0x0b64  venue block     (VENUE_SIZE bytes)   -> vehicle_ptr
//! 0x0fb0  vehicle block   (VEHICLE_SIZE bytes)
//! 0x10b4  channel meta    (n * CHANNEL_META_SIZE bytes, singly/doubly linked)
//! ...     channel data    (contiguous native-width samples, in channel order)
//! ```

use crate::{CHANNEL_META_SIZE, MAGIC};
use motorsport_telemetry_core::{SampleType, TelemetrySource};
use std::path::Path;
use thiserror::Error;

pub const HEADER_SIZE: usize = 0x6e2;
pub const EVENT_SIZE: usize = 1154;
pub const VENUE_SIZE: usize = 1100;
pub const VEHICLE_SIZE: usize = 260;

const EVENT_PTR: usize = HEADER_SIZE;
const VENUE_PTR: usize = EVENT_PTR + EVENT_SIZE;
const VEHICLE_PTR: usize = VENUE_PTR + VENUE_SIZE;
const META_PTR: usize = VEHICLE_PTR + VEHICLE_SIZE;

/// Field capacities in the LD container. Text longer than these cannot be
/// stored without truncation, so the writer rejects it instead.
const NAME_CAP: usize = 32;
const SHORT_NAME_CAP: usize = 8;
const UNIT_CAP: usize = 12;
const IDENTITY_CAP: usize = 64;
const COMMENT_CAP: usize = 1024;

#[derive(Debug, Error)]
pub enum MotecWriteError {
    #[error("I/O error for {path}: {source}")]
    Io {
        path: String,
        source: std::io::Error,
    },
    #[error("channel {channel:?}: {message}")]
    Channel { channel: String, message: String },
    #[error("field {field} does not fit in {capacity} bytes (got {len}): {value:?}")]
    FieldTooLong {
        field: &'static str,
        capacity: usize,
        len: usize,
        value: String,
    },
    #[error("nothing to write: source has no channels with samples")]
    Empty,
    #[error("file would exceed the 4 GiB addressable by LD 32-bit pointers")]
    TooLarge,
}

/// Session identity written into the LD header and event/venue/vehicle blocks.
#[derive(Debug, Clone, Default)]
pub struct MotecMetadata {
    pub driver: String,
    pub vehicle: String,
    pub vehicle_number: String,
    pub team: String,
    pub venue: String,
    pub event: String,
    pub session: String,
    pub short_comment: String,
    pub event_comment: String,
    /// `dd/mm/yyyy`; defaults to `01/01/1970` when empty.
    pub date: String,
    /// `hh:mm:ss`; defaults to `00:00:00` when empty.
    pub time: String,
}

impl MotecMetadata {
    /// Carry identity across a MoTeC -> MoTeC round trip.
    pub fn from_file(file: &crate::MotecFile) -> Self {
        Self {
            driver: file.driver.clone(),
            vehicle: file.vehicle.clone(),
            venue: file.venue.clone(),
            date: file.date.clone(),
            time: file.time.clone(),
            ..Default::default()
        }
    }
}

/// How a source sample type is encoded on disk.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Encoded {
    datatype_a: u16,
    /// Byte width, which LD also uses as the `datatype` discriminator.
    width: u16,
}

impl Encoded {
    /// Pick the narrowest LD encoding that represents every value of `source`
    /// exactly. LD natively offers i16, i32, f32 and f64.
    fn for_sample_type(source: SampleType) -> Self {
        match source {
            // 0..255 and -32768..32767 both fit i16 exactly.
            SampleType::U8 | SampleType::I16 => Self {
                datatype_a: 0x03,
                width: 2,
            },
            // 0..65535 overflows i16, so widen to i32.
            SampleType::U16 | SampleType::I32 => Self {
                datatype_a: 0x05,
                width: 4,
            },
            // 0..4294967295 overflows i32; every u32 is exact in f64.
            SampleType::U32 | SampleType::F64 => Self {
                datatype_a: 0x08,
                width: 8,
            },
            SampleType::F32 => Self {
                datatype_a: 0x07,
                width: 4,
            },
        }
    }

    fn encode(self, value: f64, out: &mut Vec<u8>) {
        match (self.datatype_a, self.width) {
            (0x08, 8) => out.extend_from_slice(&value.to_le_bytes()),
            (0x07, 4) => out.extend_from_slice(&(value as f32).to_le_bytes()),
            (_, 2) => out.extend_from_slice(&(value as i16).to_le_bytes()),
            (_, 4) => out.extend_from_slice(&(value as i32).to_le_bytes()),
            _ => unreachable!("unsupported encoding {self:?}"),
        }
    }
}

/// A channel flattened into one contiguous, single-rate block.
struct Flattened {
    name: String,
    unit: String,
    frequency_hz: u16,
    encoding: Encoded,
    values: Vec<f64>,
}

fn channel_error(channel: &str, message: impl Into<String>) -> MotecWriteError {
    MotecWriteError::Channel {
        channel: channel.to_owned(),
        message: message.into(),
    }
}

fn checked_text(
    field: &'static str,
    value: &str,
    capacity: usize,
) -> Result<Vec<u8>, MotecWriteError> {
    // LD text is a fixed-width Windows/Latin-1 byte field. MoTeC commonly
    // writes the degree sign as byte 0xb0 (for example `°C`), so UTF-8 bytes
    // cannot be copied directly and ASCII-only validation is too strict.
    let bytes = value
        .chars()
        .map(|character| {
            u8::try_from(character as u32).map_err(|_| MotecWriteError::FieldTooLong {
                field,
                capacity,
                len: value.chars().count(),
                value: value.to_owned(),
            })
        })
        .collect::<Result<Vec<_>, _>>()?;
    if bytes.len() >= capacity {
        return Err(MotecWriteError::FieldTooLong {
            field,
            capacity,
            len: bytes.len(),
            value: value.to_owned(),
        });
    }
    Ok(bytes)
}

/// Verify a channel is representable in LD and collect its samples in order.
fn flatten(
    source: &dyn TelemetrySource,
    index: usize,
) -> Result<Option<Flattened>, MotecWriteError> {
    let channel = &source.channels()[index];
    if channel.sample_count == 0 || channel.chunks.is_empty() {
        return Ok(None);
    }
    let name = &channel.name;

    let period = channel.chunks[0].sample_period_ns;
    if period == 0 {
        return Err(channel_error(name, "sample period is zero"));
    }
    if channel
        .chunks
        .iter()
        .any(|chunk| chunk.sample_period_ns != period)
    {
        return Err(channel_error(
            name,
            "LD stores one rate per channel but the source mixes sample periods",
        ));
    }
    // LD encodes the rate as an integer Hz in a u16.
    if 1_000_000_000u64 % period != 0 {
        return Err(channel_error(
            name,
            format!("sample period {period} ns is not an integer frequency"),
        ));
    }
    let frequency = 1_000_000_000u64 / period;
    if frequency == 0 || frequency > u16::MAX as u64 {
        return Err(channel_error(
            name,
            format!("frequency {frequency} Hz is outside the LD u16 range"),
        ));
    }

    // LD has a single data block per channel, so the chunks must tile time
    // without gaps or overlaps for the flattening to be lossless.
    let mut expected_time = channel.chunks[0].time_base_ns;
    for chunk in &channel.chunks {
        if chunk.time_base_ns != expected_time {
            return Err(channel_error(
                name,
                format!(
                    "non-contiguous chunks: expected time base {expected_time} ns, got {} ns",
                    chunk.time_base_ns
                ),
            ));
        }
        expected_time += chunk.sample_count * chunk.sample_period_ns;
    }

    let mut values = Vec::with_capacity(channel.sample_count as usize);
    for (chunk_index, chunk) in channel.chunks.iter().enumerate() {
        for sample in 0..chunk.sample_count {
            values.push(source.decode(index, chunk_index, sample));
        }
    }
    if values.len() as u64 != channel.sample_count {
        return Err(channel_error(
            name,
            format!(
                "chunk sample counts sum to {} but the channel reports {}",
                values.len(),
                channel.sample_count
            ),
        ));
    }
    if values.len() > u32::MAX as usize {
        return Err(channel_error(name, "more samples than the LD u32 count"));
    }

    // Reject rather than truncate: a clipped name is lost metadata.
    checked_text("channel name", name, NAME_CAP)?;
    checked_text("channel unit", &channel.unit, UNIT_CAP)?;

    Ok(Some(Flattened {
        name: name.clone(),
        unit: channel.unit.clone(),
        frequency_hz: frequency as u16,
        encoding: Encoded::for_sample_type(channel.sample_type),
        values,
    }))
}

fn put(buffer: &mut [u8], offset: usize, bytes: &[u8]) {
    buffer[offset..offset + bytes.len()].copy_from_slice(bytes);
}
fn put_u16(buffer: &mut [u8], offset: usize, value: u16) {
    put(buffer, offset, &value.to_le_bytes());
}
fn put_i16(buffer: &mut [u8], offset: usize, value: i16) {
    put(buffer, offset, &value.to_le_bytes());
}
fn put_u32(buffer: &mut [u8], offset: usize, value: u32) {
    put(buffer, offset, &value.to_le_bytes());
}

/// Serialise `source` as a MoTeC LD file.
pub fn write_motec_bytes(
    source: &dyn TelemetrySource,
    metadata: &MotecMetadata,
) -> Result<Vec<u8>, MotecWriteError> {
    let mut channels = Vec::new();
    for index in 0..source.channels().len() {
        if let Some(flat) = flatten(source, index)? {
            channels.push(flat);
        }
    }
    if channels.is_empty() {
        return Err(MotecWriteError::Empty);
    }

    let driver = checked_text("driver", &metadata.driver, IDENTITY_CAP)?;
    let vehicle = checked_text("vehicle", &metadata.vehicle, IDENTITY_CAP)?;
    let venue = checked_text("venue", &metadata.venue, IDENTITY_CAP)?;
    let event = checked_text("event", &metadata.event, IDENTITY_CAP)?;
    let session = checked_text("session", &metadata.session, IDENTITY_CAP)?;
    let short_comment = checked_text("short_comment", &metadata.short_comment, IDENTITY_CAP)?;
    let event_comment = checked_text("event_comment", &metadata.event_comment, COMMENT_CAP)?;
    let date = checked_text(
        "date",
        if metadata.date.is_empty() {
            "01/01/1970"
        } else {
            &metadata.date
        },
        16,
    )?;
    let time = checked_text(
        "time",
        if metadata.time.is_empty() {
            "00:00:00"
        } else {
            &metadata.time
        },
        16,
    )?;

    let data_ptr = META_PTR + channels.len() * CHANNEL_META_SIZE;
    let data_bytes: usize = channels
        .iter()
        .map(|c| c.values.len() * c.encoding.width as usize)
        .sum();
    let total = data_ptr
        .checked_add(data_bytes)
        .ok_or(MotecWriteError::TooLarge)?;
    if total > u32::MAX as usize {
        return Err(MotecWriteError::TooLarge);
    }

    let mut buffer = vec![0u8; data_ptr];

    // ---- header ----
    put_u32(&mut buffer, 0x00, MAGIC);
    put_u32(&mut buffer, 0x08, META_PTR as u32);
    put_u32(&mut buffer, 0x0c, data_ptr as u32);
    put_u32(&mut buffer, 0x24, EVENT_PTR as u32);
    // Static values observed in MoTeC-authored logs; i2 expects these shapes.
    put_u16(&mut buffer, 0x40, 1);
    put_u16(&mut buffer, 0x42, 0x4240);
    put_u16(&mut buffer, 0x44, 0x000f);
    put_u32(&mut buffer, 0x46, 0x1f44);
    put(&mut buffer, 0x4a, b"ADL");
    put_u16(&mut buffer, 0x52, 420);
    put_u16(&mut buffer, 0x54, 0xadb0);
    put_u32(&mut buffer, 0x56, channels.len() as u32);
    put(&mut buffer, 0x5e, &date);
    put(&mut buffer, 0x7e, &time);
    put(&mut buffer, 0x9e, &driver);
    put(&mut buffer, 0xde, &vehicle);
    put(&mut buffer, 0x15e, &venue);
    put_u32(&mut buffer, 0x5de, 0xc81a4); // "pro logging" magic
    put(&mut buffer, 0x624, &short_comment);

    // ---- event -> venue -> vehicle chain ----
    put(&mut buffer, EVENT_PTR, &event);
    put(&mut buffer, EVENT_PTR + 64, &session);
    put(&mut buffer, EVENT_PTR + 128, &event_comment);
    put_u16(&mut buffer, EVENT_PTR + 1152, VENUE_PTR as u16);

    put(&mut buffer, VENUE_PTR, &venue);
    put_u16(&mut buffer, VENUE_PTR + 1098, VEHICLE_PTR as u16);

    put(&mut buffer, VEHICLE_PTR, &vehicle);

    // ---- channel metadata (doubly linked list) ----
    let mut channel_data_ptr = data_ptr;
    for (index, channel) in channels.iter().enumerate() {
        let at = META_PTR + index * CHANNEL_META_SIZE;
        let prev = if index == 0 {
            0
        } else {
            (at - CHANNEL_META_SIZE) as u32
        };
        let next = if index + 1 == channels.len() {
            0
        } else {
            (at + CHANNEL_META_SIZE) as u32
        };
        #[allow(clippy::identity_op)]
        put_u32(&mut buffer, at + 0x00, prev);
        put_u32(&mut buffer, at + 0x04, next);
        put_u32(&mut buffer, at + 0x08, channel_data_ptr as u32);
        put_u32(&mut buffer, at + 0x0c, channel.values.len() as u32);
        put_u16(&mut buffer, at + 0x10, 0x2ee1u16.wrapping_add(index as u16));
        put_u16(&mut buffer, at + 0x12, channel.encoding.datatype_a);
        put_u16(&mut buffer, at + 0x14, channel.encoding.width);
        put_u16(&mut buffer, at + 0x16, channel.frequency_hz);
        // Identity transform: value = (raw / scale * 10^-dec + shift) * mul.
        put_i16(&mut buffer, at + 0x18, 0); // shift
        put_i16(&mut buffer, at + 0x1a, 1); // mul
        put_i16(&mut buffer, at + 0x1c, 1); // scale
        put_i16(&mut buffer, at + 0x1e, 0); // decimal places
        let encoded_name = checked_text("channel name", &channel.name, NAME_CAP)?;
        let encoded_unit = checked_text("channel unit", &channel.unit, UNIT_CAP)?;
        put(&mut buffer, at + 0x20, &encoded_name);
        let short = &encoded_name[..encoded_name.len().min(SHORT_NAME_CAP - 1)];
        put(&mut buffer, at + 0x40, short);
        put(&mut buffer, at + 0x48, &encoded_unit);
        channel_data_ptr += channel.values.len() * channel.encoding.width as usize;
    }

    // ---- sample data ----
    buffer.reserve(data_bytes);
    for channel in &channels {
        for &value in &channel.values {
            channel.encoding.encode(value, &mut buffer);
        }
    }
    debug_assert_eq!(buffer.len(), total);
    Ok(buffer)
}

/// Return the conventional companion path (`recording.ldx`) for an LD path.
pub fn motec_sidecar_path(path: impl AsRef<Path>) -> std::path::PathBuf {
    let mut sidecar = path.as_ref().to_path_buf();
    sidecar.set_extension("ldx");
    sidecar
}

/// Write the companion MoTeC LDX sidecar and return its byte length.
pub fn write_motec_sidecar(
    source: &dyn TelemetrySource,
    metadata: &MotecMetadata,
    ld_path: impl AsRef<Path>,
) -> Result<u64, MotecWriteError> {
    let path = motec_sidecar_path(ld_path);
    let bytes = crate::ldx::write_motec_ldx_bytes(source, metadata);
    let len = bytes.len() as u64;
    std::fs::write(&path, bytes).map_err(|source| MotecWriteError::Io {
        path: path.to_string_lossy().into_owned(),
        source,
    })?;
    Ok(len)
}

/// Serialise `source` to a MoTeC LD file and write its companion LDX sidecar.
pub fn write_motec(
    source: &dyn TelemetrySource,
    metadata: &MotecMetadata,
    path: impl AsRef<Path>,
) -> Result<(), MotecWriteError> {
    let path = path.as_ref();
    let bytes = write_motec_bytes(source, metadata)?;
    std::fs::write(path, bytes).map_err(|source| MotecWriteError::Io {
        path: path.to_string_lossy().into_owned(),
        source,
    })?;
    write_motec_sidecar(source, metadata, path)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn latin1_characters_encode_as_single_ld_bytes() {
        // LD holds Windows/Latin-1 bytes; the degree sign is 0xb0, one byte,
        // even though its UTF-8 form is two bytes. This must stay symmetric
        // with the reader, which maps each byte back via `char::from`.
        assert_eq!(checked_text("unit", "°C", 12).unwrap(), vec![0xb0, b'C']);
    }

    #[test]
    fn capacity_is_counted_in_encoded_bytes_not_utf8_bytes() {
        // Sixteen degree signs are 16 Latin-1 bytes but 32 UTF-8 bytes; the
        // byte count (not the character count) governs the LD field capacity.
        let sixteen: String = "°".repeat(16);
        assert!(checked_text("unit", &sixteen, 17).is_ok());
        let err = checked_text("unit", &"°".repeat(17), 17).unwrap_err();
        assert!(matches!(
            err,
            MotecWriteError::FieldTooLong {
                capacity: 17,
                len: 17,
                ..
            }
        ));
    }

    #[test]
    fn non_latin1_characters_are_rejected() {
        // U+20AC is outside the single-byte Latin-1 range LD can store.
        let err = checked_text("unit", "€/h", 12).unwrap_err();
        assert!(matches!(err, MotecWriteError::FieldTooLong { .. }));
    }

    #[test]
    fn ascii_text_must_fit_capacity_minus_nul() {
        // ASCII is unaffected by the Latin-1 change: an all-filling field is
        // still rejected because LD text needs a NUL terminator.
        assert!(checked_text("name", &"x".repeat(NAME_CAP - 1), NAME_CAP).is_ok());
        assert!(checked_text("name", &"x".repeat(NAME_CAP), NAME_CAP).is_err());
    }
}
