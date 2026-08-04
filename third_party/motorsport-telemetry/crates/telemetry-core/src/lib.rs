use std::sync::Arc;

pub mod units;

pub use units::{
    can_convert, convert, lookup as lookup_unit, normalize as normalize_unit, ConvertError,
    Dimension, UnitDef, UNITS,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SampleType {
    U8,
    I16,
    U16,
    I32,
    U32,
    F32,
    F64,
}

impl SampleType {
    pub fn from_pds_code(code: u32) -> Self {
        match code {
            1 => Self::U8,
            2 => Self::I16,
            3 => Self::U16,
            4 => Self::I32,
            5 => Self::U32,
            7 => Self::F64,
            _ => Self::F32,
        }
    }

    pub fn code(self) -> u32 {
        match self {
            Self::U8 => 1,
            Self::I16 => 2,
            Self::U16 => 3,
            Self::I32 => 4,
            Self::U32 => 5,
            Self::F32 => 6,
            Self::F64 => 7,
        }
    }

    pub fn byte_width(self) -> usize {
        match self {
            Self::U8 => 1,
            Self::I16 | Self::U16 => 2,
            Self::I32 | Self::U32 | Self::F32 => 4,
            Self::F64 => 8,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::U8 => "uint8",
            Self::I16 => "int16",
            Self::U16 => "uint16",
            Self::I32 => "int32",
            Self::U32 => "uint32",
            Self::F32 => "float32",
            Self::F64 => "float64",
        }
    }

    pub fn is_float(self) -> bool {
        matches!(self, Self::F32 | Self::F64)
    }
}

#[derive(Debug, Clone)]
pub struct Chunk {
    pub sample_period_ns: u64,
    pub sample_count: u64,
    /// Byte offset for binary formats, column-local offset for text formats.
    pub data_ptr: u64,
    pub sample_base: u64,
    pub time_base_ns: u64,
}

/// Where a channel's unit string came from.
///
/// Units drive downstream conversion and display, so a guess must never be
/// indistinguishable from a value the file actually declared.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum UnitSource {
    /// The file stored an explicit unit string for this channel.
    Declared,
    /// The unit is fixed by the file format's specification for this channel
    /// (for example Pi/Cosworth PDS storing SI base units, or a VBOX builtin
    /// column that is always km/h).
    SpecDefault,
    /// No unit information is available. `unit` is empty.
    #[default]
    Unknown,
}

impl UnitSource {
    pub fn name(self) -> &'static str {
        match self {
            Self::Declared => "declared",
            Self::SpecDefault => "spec_default",
            Self::Unknown => "unknown",
        }
    }
}

#[derive(Debug, Clone)]
pub struct Channel {
    pub id: u32,
    pub name: String,
    pub unit: String,
    /// Provenance of `unit`. Never infer a unit from a channel name and report
    /// it as [`UnitSource::Declared`].
    pub unit_source: UnitSource,
    pub sample_type: SampleType,
    pub chunks: Vec<Chunk>,
    pub sample_count: u64,
    pub duration_ns: u64,
}

impl Channel {
    pub fn first_period_ns(&self) -> Option<u64> {
        self.chunks.first().map(|chunk| chunk.sample_period_ns)
    }

    pub fn frequency_hz(&self) -> Option<f64> {
        self.first_period_ns().map(|period| 1e9 / period as f64)
    }

    pub fn uses_step_interpolation(&self) -> bool {
        if !self.sample_type.is_float() {
            return true;
        }
        let name = self
            .name
            .to_ascii_lowercase()
            .chars()
            .filter(|c| c.is_ascii_alphanumeric())
            .collect::<String>();
        [
            "gear",
            "lapnumber",
            "lapbeacon",
            "laptrigger",
            "switch",
            "status",
            "state",
            "flag",
            "alarm",
            "satellites",
            "solutiontype",
        ]
        .iter()
        .any(|token| name.contains(token))
    }
}

pub trait TelemetrySource: Send + Sync {
    fn path(&self) -> &str;
    fn format(&self) -> &'static str;
    fn channels(&self) -> &[Channel];

    /// Media time corresponding to telemetry time zero, when this source is
    /// embedded in a timed container.
    fn media_time_offset_ns(&self) -> Option<i64> {
        None
    }
    fn decode(&self, channel_index: usize, chunk_index: usize, local_index: u64) -> f64;

    fn sample_time_ns(&self, channel_index: usize, chunk_index: usize, local_index: u64) -> u64 {
        let chunk = &self.channels()[channel_index].chunks[chunk_index];
        chunk.time_base_ns + local_index * chunk.sample_period_ns
    }

    fn sample_at(&self, channel_index: usize, time_ns: u64, linear: bool) -> Option<f64> {
        let channel = self.channels().get(channel_index)?;
        if time_ns >= channel.duration_ns || channel.chunks.is_empty() {
            return None;
        }
        let chunk_index = channel.chunks.partition_point(|chunk| {
            chunk
                .time_base_ns
                .saturating_add(chunk.sample_count.saturating_mul(chunk.sample_period_ns))
                <= time_ns
        });
        let chunk = channel.chunks.get(chunk_index)?;
        let relative = time_ns.saturating_sub(chunk.time_base_ns);
        let sample = (relative / chunk.sample_period_ns).min(chunk.sample_count - 1);
        let a = self.decode(channel_index, chunk_index, sample);
        if !linear || channel.uses_step_interpolation() {
            return Some(a);
        }

        let sample_time = chunk.time_base_ns + sample * chunk.sample_period_ns;
        let (b, next_time) = if sample + 1 < chunk.sample_count {
            (
                self.decode(channel_index, chunk_index, sample + 1),
                sample_time + chunk.sample_period_ns,
            )
        } else if let Some(next_chunk) = channel.chunks.get(chunk_index + 1) {
            (
                self.decode(channel_index, chunk_index + 1, 0),
                next_chunk.time_base_ns,
            )
        } else {
            return Some(a);
        };
        let interval = next_time.saturating_sub(sample_time);
        if interval == 0 {
            return Some(a);
        }
        let fraction = time_ns.saturating_sub(sample_time) as f64 / interval as f64;
        Some(a + (b - a) * fraction)
    }
}

pub type SourceRef = Arc<dyn TelemetrySource>;

#[cfg(test)]
mod tests {
    use super::*;

    fn channel(name: &str, sample_type: SampleType) -> Channel {
        Channel {
            id: 1,
            name: name.into(),
            unit: String::new(),
            unit_source: UnitSource::Unknown,
            sample_type,
            chunks: Vec::new(),
            sample_count: 0,
            duration_ns: 0,
        }
    }

    struct TestSource {
        channel: Channel,
        values: [f64; 2],
    }

    impl TelemetrySource for TestSource {
        fn path(&self) -> &str {
            "test"
        }
        fn format(&self) -> &'static str {
            "test"
        }
        fn channels(&self) -> &[Channel] {
            std::slice::from_ref(&self.channel)
        }
        fn decode(&self, _channel_index: usize, _chunk_index: usize, local_index: u64) -> f64 {
            self.values[local_index as usize]
        }
    }

    fn two_sample_source(sample_type: SampleType) -> TestSource {
        TestSource {
            channel: Channel {
                id: 1,
                name: "Speed".into(),
                unit: String::new(),
                unit_source: UnitSource::Unknown,
                sample_type,
                chunks: vec![Chunk {
                    sample_period_ns: 1_000_000_000,
                    sample_count: 2,
                    data_ptr: 0,
                    sample_base: 0,
                    time_base_ns: 0,
                }],
                sample_count: 2,
                duration_ns: 2_000_000_000,
            },
            values: [10.0, 20.0],
        }
    }

    #[test]
    fn discrete_channels_use_step_interpolation_even_when_stored_as_float() {
        assert!(channel("Gear_Pos", SampleType::F32).uses_step_interpolation());
        assert!(channel("Lap Beacon", SampleType::F32).uses_step_interpolation());
        assert!(!channel("Speed_Ref", SampleType::F32).uses_step_interpolation());
        assert!(!channel("Speed_Ref", SampleType::F64).uses_step_interpolation());
        for sample_type in [
            SampleType::U8,
            SampleType::I16,
            SampleType::U16,
            SampleType::I32,
            SampleType::U32,
        ] {
            assert!(channel("Speed_Ref", sample_type).uses_step_interpolation());
        }
    }

    #[test]
    fn linear_mode_never_interpolates_integer_source_channels() {
        let integer = two_sample_source(SampleType::I32);
        let float = two_sample_source(SampleType::F32);
        assert_eq!(integer.sample_at(0, 500_000_000, true), Some(10.0));
        assert_eq!(float.sample_at(0, 500_000_000, true), Some(15.0));
    }
}
