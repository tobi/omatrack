use motorsport_telemetry_core::{Channel, Chunk, SampleType, TelemetrySource, UnitSource};
use std::fs;
use std::path::Path;
use thiserror::Error;

const BUILTIN_NAMES: [&str; 12] = [
    "satellites",
    "time",
    "latitude",
    "longitude",
    "velocity kmh",
    "heading",
    "height",
    "vertical velocity m/s",
    "sampleperiod",
    "solution type",
    "avifileindex",
    "avisynctime",
];
const BUILTIN_SHORT: [&str; 12] = [
    "sats",
    "time",
    "lat",
    "long",
    "velocity",
    "heading",
    "height",
    "vert-vel",
    "Tsample",
    "solution_type",
    "avifileindex",
    "avitime",
];

#[derive(Debug, Error)]
pub enum VboError {
    #[error("I/O error for {path}: {source}")]
    Io {
        path: String,
        source: std::io::Error,
    },
    #[error("invalid VBO file {path}: {message}")]
    Invalid { path: String, message: String },
}

#[derive(Default)]
struct Sections {
    header: Vec<String>,
    units: Vec<String>,
    column_names: Vec<String>,
    data: Vec<String>,
}

#[derive(Debug)]
pub struct VboFile {
    pub path: String,
    pub channels: Vec<Channel>,
    pub time_ns: Vec<u64>,
    values: Vec<Vec<f64>>,
}

fn invalid(path: &str, message: impl Into<String>) -> VboError {
    VboError::Invalid {
        path: path.into(),
        message: message.into(),
    }
}

fn sections(text: &str) -> Sections {
    let mut result = Sections::default();
    let mut current = String::new();
    for line in text.lines() {
        let trimmed = line.trim();
        if trimmed.starts_with('[') && trimmed.ends_with(']') {
            current = trimmed[1..trimmed.len() - 1].to_ascii_lowercase();
            continue;
        }
        if trimmed.is_empty() {
            continue;
        }
        match current.as_str() {
            "header" => result.header.push(trimmed.into()),
            "channel units" => result.units.push(trimmed.into()),
            "column names" => {
                result.column_names = trimmed.split_whitespace().map(str::to_owned).collect();
                current.clear();
            }
            "data" => result.data.push(trimmed.into()),
            _ => {}
        }
    }
    result
}

fn time_seconds(raw: f64) -> f64 {
    let hours = (raw / 10000.0).floor();
    let minutes = ((raw % 10000.0) / 100.0).floor();
    hours * 3600.0 + minutes * 60.0 + raw % 100.0
}

fn builtin_unit(name: &str) -> &'static str {
    match name.to_ascii_lowercase().as_str() {
        "time" | "tsample" => "s",
        "lat" | "long" => "min",
        "velocity" => "km/h",
        "heading" => "deg",
        "height" => "m",
        "vert-vel" => "m/s",
        _ => "",
    }
}

impl VboFile {
    pub fn open(path: impl AsRef<Path>) -> Result<Self, VboError> {
        let path = path.as_ref();
        let display = path.to_string_lossy().into_owned();
        let bytes = fs::read(path).map_err(|source| VboError::Io {
            path: display.clone(),
            source,
        })?;
        Self::from_bytes(display, bytes)
    }

    pub fn from_bytes(path: impl Into<String>, bytes: Vec<u8>) -> Result<Self, VboError> {
        let display = path.into();
        if bytes.is_empty() {
            return Err(invalid(&display, "empty file"));
        }
        let text = String::from_utf8(bytes.clone())
            .unwrap_or_else(|_| bytes.iter().map(|&byte| char::from(byte)).collect());
        let parsed = sections(&text);
        if parsed.data.is_empty() {
            return Err(invalid(&display, "missing or empty [data] section"));
        }
        let short_names = if parsed.column_names.is_empty() {
            parsed
                .header
                .iter()
                .enumerate()
                .map(|(index, name)| {
                    if index < BUILTIN_SHORT.len() {
                        BUILTIN_SHORT[index].to_owned()
                    } else {
                        name.clone()
                    }
                })
                .collect::<Vec<_>>()
        } else {
            parsed.column_names.clone()
        };
        if short_names.is_empty() {
            return Err(invalid(&display, "no channel names"));
        }
        let count = short_names.len();
        let mut values = vec![Vec::with_capacity(parsed.data.len()); count];
        for line in &parsed.data {
            let tokens = line.split_whitespace().collect::<Vec<_>>();
            if tokens.len() < 2 {
                continue;
            }
            for (column, output) in values.iter_mut().enumerate() {
                output.push(
                    tokens
                        .get(column)
                        .and_then(|token| token.parse().ok())
                        .unwrap_or(f64::NAN),
                );
            }
        }
        let rows = values.first().map(Vec::len).unwrap_or(0);
        if rows == 0 {
            return Err(invalid(&display, "no valid data rows"));
        }
        let time_column = short_names
            .iter()
            .position(|name| name.eq_ignore_ascii_case("time"))
            .ok_or_else(|| invalid(&display, "no time column"))?;
        let first = time_seconds(values[time_column][0]);
        let mut time_ns = Vec::with_capacity(rows);
        for value in &mut values[time_column] {
            let mut seconds = time_seconds(*value);
            if seconds < first - 43200.0 {
                seconds += 86400.0;
            }
            *value = seconds - first;
            time_ns.push((*value * 1e9).round().max(0.0) as u64);
        }
        let sample_period = short_names
            .iter()
            .position(|name| name.eq_ignore_ascii_case("tsample"))
            .and_then(|index| values[index].iter().copied().find(|value| *value > 0.0))
            .map(|seconds| (seconds * 1e9).round() as u64)
            .or_else(|| {
                time_ns
                    .windows(2)
                    .map(|pair| pair[1].saturating_sub(pair[0]))
                    .find(|delta| *delta > 0)
            })
            .unwrap_or(100_000_000);
        let duration = time_ns
            .last()
            .copied()
            .unwrap_or(0)
            .saturating_add(sample_period);

        let mut channels = Vec::with_capacity(count);
        for index in 0..count {
            let name = parsed.header.get(index).cloned().unwrap_or_else(|| {
                if index < BUILTIN_NAMES.len() {
                    BUILTIN_NAMES[index].into()
                } else {
                    short_names[index].clone()
                }
            });
            let custom_unit_index = index.saturating_sub(BUILTIN_NAMES.len());
            // Builtin VBOX columns have units fixed by the format spec; the
            // trailing custom columns declare theirs in [channel units].
            let (unit, unit_source) = if index < BUILTIN_NAMES.len() {
                let builtin = builtin_unit(&short_names[index]);
                if builtin.is_empty() {
                    (String::new(), UnitSource::Unknown)
                } else {
                    (builtin.to_owned(), UnitSource::SpecDefault)
                }
            } else {
                match parsed
                    .units
                    .get(custom_unit_index)
                    .filter(|unit| unit.as_str() != "(null)" && !unit.is_empty())
                {
                    Some(declared) => (declared.clone(), UnitSource::Declared),
                    None => (String::new(), UnitSource::Unknown),
                }
            };
            channels.push(Channel {
                id: index as u32,
                name,
                unit,
                unit_source,
                sample_type: SampleType::F64,
                chunks: vec![Chunk {
                    sample_period_ns: sample_period,
                    sample_count: rows as u64,
                    data_ptr: 0,
                    sample_base: 0,
                    time_base_ns: 0,
                }],
                sample_count: rows as u64,
                duration_ns: duration,
            });
        }
        Ok(Self {
            path: display,
            channels,
            time_ns,
            values,
        })
    }
}

impl TelemetrySource for VboFile {
    fn path(&self) -> &str {
        &self.path
    }
    fn format(&self) -> &'static str {
        "vbo"
    }
    fn channels(&self) -> &[Channel] {
        &self.channels
    }
    fn decode(&self, channel_index: usize, _chunk_index: usize, local_index: u64) -> f64 {
        self.values[channel_index][local_index as usize]
    }
    fn sample_time_ns(&self, _channel_index: usize, _chunk_index: usize, local_index: u64) -> u64 {
        self.time_ns[local_index as usize]
    }
    fn sample_at(&self, channel_index: usize, time_ns: u64, linear: bool) -> Option<f64> {
        if time_ns >= self.channels[channel_index].duration_ns {
            return None;
        }
        let upper = self.time_ns.partition_point(|time| *time <= time_ns);
        let lower = upper.saturating_sub(1).min(self.time_ns.len() - 1);
        let a = self.values[channel_index][lower];
        if !linear
            || self.channels[channel_index].uses_step_interpolation()
            || upper >= self.time_ns.len()
        {
            return Some(a);
        }
        let interval = self.time_ns[upper].saturating_sub(self.time_ns[lower]);
        if interval == 0 {
            return Some(a);
        }
        let fraction = time_ns.saturating_sub(self.time_ns[lower]) as f64 / interval as f64;
        Some(a + (self.values[channel_index][upper] - a) * fraction)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn fixture(contents: &str) -> tempfile::NamedTempFile {
        let mut file = tempfile::NamedTempFile::new().unwrap();
        file.write_all(contents.as_bytes()).unwrap();
        file
    }

    #[test]
    fn parses_irregular_timestamps_and_interpolates_continuous_values() {
        let fixture = fixture("[header]\ntime\nvelocity kmh\n[column names]\ntime velocity\n[data]\n120000.0 10\n120000.5 20\n120001.5 40\n");
        let file = VboFile::open(fixture.path()).unwrap();
        assert_eq!(file.time_ns, [0, 500_000_000, 1_500_000_000]);
        assert_eq!(file.decode(1, 0, 2), 40.0);
        assert_eq!(file.sample_at(1, 1_000_000_000, true), Some(30.0));
    }

    #[test]
    fn handles_midnight_rollover_and_stepwise_gear() {
        let fixture = fixture("[header]\ntime\ngear\n[column names]\ntime Gear\n[data]\n235959.5 3\n000000.0 4\n000000.5 4\n");
        let file = VboFile::open(fixture.path()).unwrap();
        assert_eq!(file.time_ns, [0, 500_000_000, 1_000_000_000]);
        assert_eq!(file.sample_at(1, 250_000_000, true), Some(3.0));
    }

    #[test]
    fn custom_channels_read_their_declared_units_in_order() {
        // The trailing non-builtin columns declare their units in `[channel
        // units]`, one entry per custom channel, aligned with the first custom
        // column at index `BUILTIN_NAMES.len()`. The first custom channel must
        // take units[0], not units[1].
        let builtin = BUILTIN_SHORT.join(" ");
        let builtin_header = BUILTIN_NAMES.join("\n");
        let fixture = fixture(&format!(
            "[header]\n{builtin_header}\ncustomA\ncustomB\n\
             [channel units]\ncustom-unit-a\ncustom-unit-b\n\
             [column names]\n{builtin} customA customB\n\
             [data]\n\
             120000.0 1 2 3 4 5 6 7 8 9 10 11 12 13\n\
             120001.0 1 2 3 4 5 6 7 8 9 10 11 12 13\n"
        ));
        let file = VboFile::open(fixture.path()).unwrap();
        assert_eq!(file.channels.len(), 14);
        // The first builtin column keeps its spec-fixed unit (or none); the
        // custom channels must carry exactly their declared units in order.
        assert_eq!(file.channels[12].unit, "custom-unit-a");
        assert_eq!(file.channels[12].unit_source, UnitSource::Declared);
        assert_eq!(file.channels[13].unit, "custom-unit-b");
        assert_eq!(file.channels[13].unit_source, UnitSource::Declared);
    }

    #[test]
    fn rejects_missing_data_and_time_sections() {
        let no_data = fixture("[header]\ntime\n");
        assert!(matches!(
            VboFile::open(no_data.path()),
            Err(VboError::Invalid { .. })
        ));
        let no_time = fixture("[column names]\nspeed\n[data]\n1\n2\n");
        assert!(matches!(
            VboFile::open(no_time.path()),
            Err(VboError::Invalid { .. })
        ));
    }
}
