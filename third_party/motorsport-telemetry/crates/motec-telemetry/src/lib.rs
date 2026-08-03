#[cfg(not(target_os = "emscripten"))]
use memmap2::Mmap;
use motorsport_telemetry_core::{Channel, Chunk, SampleType, TelemetrySource, UnitSource};
#[cfg(not(target_os = "emscripten"))]
use std::fs::File;
#[cfg(not(target_os = "emscripten"))]
use std::path::Path;
use thiserror::Error;

const MAGIC: u32 = 0x40;
pub(crate) const CHANNEL_META_SIZE: usize = 124;
const MIN_FILE_SIZE: usize = 0x1a0;
const MAX_CHANNELS: usize = 4096;

pub mod ldx;
pub mod write;
pub use ldx::{infer_lap_markers, write_motec_ldx_bytes, LapMarkers};
pub use write::{
    motec_sidecar_path, write_motec, write_motec_bytes, write_motec_sidecar, MotecMetadata,
    MotecWriteError,
};

#[derive(Debug, Error)]
pub enum MotecError {
    #[error("I/O error for {path}: {source}")]
    Io {
        path: String,
        source: std::io::Error,
    },
    #[error("invalid MoTeC LD file {path}: {message}")]
    Invalid { path: String, message: String },
}

#[derive(Debug, Clone)]
struct Encoding {
    datatype_a: u16,
    width: usize,
    shift: i16,
    mul: i16,
    scale: i16,
    decimal_places: i16,
}

#[derive(Debug)]
enum Storage {
    #[cfg(not(target_os = "emscripten"))]
    Mapped(Mmap),
    Owned(Box<[u8]>),
}
impl std::ops::Deref for Storage {
    type Target = [u8];
    fn deref(&self) -> &Self::Target {
        match self {
            #[cfg(not(target_os = "emscripten"))]
            Self::Mapped(value) => value,
            Self::Owned(value) => value,
        }
    }
}

#[derive(Debug)]
pub struct MotecFile {
    pub path: String,
    pub driver: String,
    pub vehicle: String,
    pub venue: String,
    pub date: String,
    pub time: String,
    pub channels: Vec<Channel>,
    encodings: Vec<Encoding>,
    data: Storage,
}

fn invalid(path: &str, message: impl Into<String>) -> MotecError {
    MotecError::Invalid {
        path: path.into(),
        message: message.into(),
    }
}

fn u16le(data: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        data.get(offset..offset + 2)?.try_into().ok()?,
    ))
}
fn i16le(data: &[u8], offset: usize) -> Option<i16> {
    Some(i16::from_le_bytes(
        data.get(offset..offset + 2)?.try_into().ok()?,
    ))
}
fn u32le(data: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        data.get(offset..offset + 4)?.try_into().ok()?,
    ))
}
fn text(data: &[u8], offset: usize, length: usize) -> String {
    data.get(offset..offset.saturating_add(length).min(data.len()))
        .unwrap_or_default()
        .iter()
        .take_while(|&&byte| byte != 0)
        .map(|&byte| char::from(byte))
        .collect::<String>()
        .trim()
        .to_owned()
}

impl MotecFile {
    #[cfg(not(target_os = "emscripten"))]
    pub fn open(path: impl AsRef<Path>) -> Result<Self, MotecError> {
        let path = path.as_ref();
        let display = path.to_string_lossy().into_owned();
        let file = File::open(path).map_err(|source| MotecError::Io {
            path: display.clone(),
            source,
        })?;
        let data = unsafe { Mmap::map(&file) }.map_err(|source| MotecError::Io {
            path: display.clone(),
            source,
        })?;
        Self::parse(display, Storage::Mapped(data))
    }

    pub fn from_bytes(path: impl Into<String>, data: Vec<u8>) -> Result<Self, MotecError> {
        Self::parse(path.into(), Storage::Owned(data.into_boxed_slice()))
    }

    fn parse(display: String, data: Storage) -> Result<Self, MotecError> {
        if data.len() < MIN_FILE_SIZE {
            return Err(invalid(&display, "file is too small"));
        }
        let magic = u32le(&data, 0).unwrap_or(0);
        if magic != MAGIC {
            return Err(invalid(
                &display,
                format!("expected magic 0x40, got 0x{magic:x}"),
            ));
        }

        let mut channels = Vec::new();
        let mut encodings = Vec::new();
        let mut address = u32le(&data, 0x08).unwrap_or(0) as usize;
        while address > 0
            && address + CHANNEL_META_SIZE <= data.len()
            && channels.len() < MAX_CHANNELS
        {
            let next = u32le(&data, address + 0x04).unwrap_or(0) as usize;
            let data_ptr = u32le(&data, address + 0x08).unwrap_or(0) as u64;
            let requested_count = u32le(&data, address + 0x0c).unwrap_or(0) as u64;
            let datatype_a = u16le(&data, address + 0x12).unwrap_or(0);
            let width = u16le(&data, address + 0x14).unwrap_or(0) as usize;
            let frequency = u16le(&data, address + 0x16).unwrap_or(0) as u64;
            let encoding = Encoding {
                datatype_a,
                width,
                shift: i16le(&data, address + 0x18).unwrap_or(0),
                mul: i16le(&data, address + 0x1a).unwrap_or(0),
                scale: i16le(&data, address + 0x1c).unwrap_or(0),
                decimal_places: i16le(&data, address + 0x1e).unwrap_or(0),
            };
            let name = text(&data, address + 0x20, 32);
            // Per the LD layout the 124-byte channel block holds name at 0x20
            // (32 bytes), short name at 0x40 (8 bytes) and unit at 0x48 (12
            // bytes).
            let unit = text(&data, address + 0x48, 12);
            // LD channel blocks carry a real unit string; absent means unknown.
            let unit_source = if unit.is_empty() {
                UnitSource::Unknown
            } else {
                UnitSource::Declared
            };
            let valid_width = matches!(width, 2 | 4 | 8);
            let count = if valid_width && data_ptr < data.len() as u64 {
                requested_count.min((data.len() as u64 - data_ptr) / width as u64)
            } else {
                0
            };
            let period_ns = 1_000_000_000u64.checked_div(frequency).unwrap_or(0);
            let chunks = if count > 0 && period_ns > 0 {
                vec![Chunk {
                    sample_period_ns: period_ns,
                    sample_count: count,
                    data_ptr,
                    sample_base: 0,
                    time_base_ns: 0,
                }]
            } else {
                Vec::new()
            };
            let sample_type = match (datatype_a, width) {
                // 0x08/8 is MoTeC's little-endian f64 (seen on GPS channels).
                (0x08, 8) => SampleType::F64,
                (0x07, 8) => SampleType::F64,
                (0x07, _) => SampleType::F32,
                (_, 2) => SampleType::I16,
                (_, 4) => SampleType::I32,
                _ => SampleType::F32,
            };
            channels.push(Channel {
                id: channels.len() as u32,
                name,
                unit,
                unit_source,
                sample_type,
                chunks,
                sample_count: count,
                duration_ns: count.saturating_mul(period_ns),
            });
            encodings.push(encoding);
            if next == 0 || next <= address {
                break;
            }
            address = next;
        }
        if channels.is_empty() {
            return Err(invalid(&display, "no channel metadata found"));
        }
        Ok(Self {
            path: display,
            driver: text(&data, 0x9e, 64),
            vehicle: text(&data, 0xde, 64),
            venue: text(&data, 0x15e, 64),
            date: text(&data, 0x5e, 16),
            time: text(&data, 0x7e, 16),
            channels,
            encodings,
            data,
        })
    }
}

impl TelemetrySource for MotecFile {
    fn path(&self) -> &str {
        &self.path
    }
    fn format(&self) -> &'static str {
        "motec"
    }
    fn channels(&self) -> &[Channel] {
        &self.channels
    }

    fn decode(&self, channel_index: usize, _chunk_index: usize, local_index: u64) -> f64 {
        let channel = &self.channels[channel_index];
        let encoding = &self.encodings[channel_index];
        let offset = channel.chunks[0].data_ptr as usize + local_index as usize * encoding.width;
        // Float channels carry raw IEEE values; the scale/shift/mul transform
        // applies to the integer encodings only.
        if encoding.datatype_a == 0x08 && encoding.width == 8 {
            return f64::from_le_bytes(self.data[offset..offset + 8].try_into().unwrap());
        }
        if encoding.datatype_a == 0x07 {
            return match encoding.width {
                8 => f64::from_le_bytes(self.data[offset..offset + 8].try_into().unwrap()),
                4 => f32::from_le_bytes(self.data[offset..offset + 4].try_into().unwrap()) as f64,
                _ => 0.0,
            };
        }
        let raw = match encoding.width {
            2 => i16::from_le_bytes(self.data[offset..offset + 2].try_into().unwrap()) as f64,
            4 => i32::from_le_bytes(self.data[offset..offset + 4].try_into().unwrap()) as f64,
            _ => return 0.0,
        };
        let scale = if encoding.scale == 0 {
            1.0
        } else {
            encoding.scale as f64
        };
        let multiplier = if encoding.mul == 0 {
            1.0
        } else {
            encoding.mul as f64
        };
        (raw / scale * 10f64.powi(-(encoding.decimal_places as i32)) + encoding.shift as f64)
            * multiplier
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn u16_at(data: &mut [u8], at: usize, value: u16) {
        data[at..at + 2].copy_from_slice(&value.to_le_bytes());
    }
    fn i16_at(data: &mut [u8], at: usize, value: i16) {
        data[at..at + 2].copy_from_slice(&value.to_le_bytes());
    }
    fn u32_at(data: &mut [u8], at: usize, value: u32) {
        data[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    fn fixture() -> tempfile::NamedTempFile {
        let mut data = vec![0u8; 0x500];
        u32_at(&mut data, 0, MAGIC);
        u32_at(&mut data, 8, 0x200);
        data[0x9e..0xa2].copy_from_slice(b"Tobi");
        data[0xde..0xe5].copy_from_slice(b"Oreca07");
        data[0x15e..0x165].copy_from_slice(b"Mosport");
        let speed = 0x200;
        u32_at(&mut data, speed + 4, 0x27c);
        u32_at(&mut data, speed + 8, 0x380);
        u32_at(&mut data, speed + 0xc, 3);
        u16_at(&mut data, speed + 0x12, 0x07);
        u16_at(&mut data, speed + 0x14, 4);
        u16_at(&mut data, speed + 0x16, 2);
        data[speed + 0x20..speed + 0x25].copy_from_slice(b"Speed");
        data[speed + 0x48..speed + 0x4b].copy_from_slice(b"m/s");
        for (index, value) in [1.0_f32, 2.0, 3.0].into_iter().enumerate() {
            data[0x380 + index * 4..0x384 + index * 4].copy_from_slice(&value.to_le_bytes());
        }
        let brake = 0x27c;
        u32_at(&mut data, brake + 8, 0x3a0);
        u32_at(&mut data, brake + 0xc, 2);
        u16_at(&mut data, brake + 0x12, 0x03);
        u16_at(&mut data, brake + 0x14, 2);
        u16_at(&mut data, brake + 0x16, 1);
        i16_at(&mut data, brake + 0x1c, 1);
        i16_at(&mut data, brake + 0x1e, 1);
        data[brake + 0x20..brake + 0x29].copy_from_slice(b"P_F_BRAKE");
        data[brake + 0x48..brake + 0x4b].copy_from_slice(b"bar");
        i16_at(&mut data, 0x3a0, 423);
        i16_at(&mut data, 0x3a2, -10);
        let mut file = tempfile::NamedTempFile::new().unwrap();
        file.write_all(&data).unwrap();
        file
    }

    #[test]
    fn decodes_float_and_scaled_integer_channels() {
        let fixture = fixture();
        let file = MotecFile::open(fixture.path()).unwrap();
        assert_eq!(
            (&file.driver, &file.vehicle, &file.venue),
            (&"Tobi".into(), &"Oreca07".into(), &"Mosport".into())
        );
        assert_eq!(file.decode(0, 0, 2), 3.0);
        assert!((file.decode(1, 0, 0) - 42.3).abs() < 1e-10);
        assert!((file.decode(1, 0, 1) + 1.0).abs() < 1e-10);
        assert_eq!(file.sample_at(0, 250_000_000, true), Some(1.5));
    }

    #[test]
    fn rejects_wrong_magic_and_truncated_files() {
        let mut file = tempfile::NamedTempFile::new().unwrap();
        file.write_all(&vec![0u8; MIN_FILE_SIZE]).unwrap();
        assert!(matches!(
            MotecFile::open(file.path()),
            Err(MotecError::Invalid { .. })
        ));
    }
}
