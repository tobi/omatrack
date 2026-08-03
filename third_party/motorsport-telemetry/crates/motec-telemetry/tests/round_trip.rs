//! Round-trip fidelity tests: source -> MoTeC LD -> source.
//!
//! These assert bit-exact recovery of every sample, not approximate equality.

use motec_telemetry::{
    motec_sidecar_path, write_motec, write_motec_bytes, MotecFile, MotecMetadata, MotecWriteError,
};
use motorsport_telemetry_core::{Channel, Chunk, SampleType, TelemetrySource, UnitSource};

/// An in-memory source used to drive the writer with controlled shapes.
struct Synthetic {
    channels: Vec<Channel>,
    values: Vec<Vec<Vec<f64>>>,
}

impl TelemetrySource for Synthetic {
    fn path(&self) -> &str {
        "synthetic"
    }
    fn format(&self) -> &'static str {
        "synthetic"
    }
    fn channels(&self) -> &[Channel] {
        &self.channels
    }
    fn decode(&self, channel_index: usize, chunk_index: usize, local_index: u64) -> f64 {
        self.values[channel_index][chunk_index][local_index as usize]
    }
}

/// Build a single-chunk channel at `frequency` Hz holding `values`.
fn single_chunk(
    id: u32,
    name: &str,
    unit: &str,
    sample_type: SampleType,
    frequency: u64,
    values: Vec<f64>,
) -> (Channel, Vec<Vec<f64>>) {
    let period = 1_000_000_000 / frequency;
    let count = values.len() as u64;
    (
        Channel {
            id,
            name: name.into(),
            unit: unit.into(),
            // A unit given here stands in for one the file declared.
            unit_source: if unit.is_empty() {
                UnitSource::Unknown
            } else {
                UnitSource::Declared
            },
            sample_type,
            chunks: vec![Chunk {
                sample_period_ns: period,
                sample_count: count,
                data_ptr: 0,
                sample_base: 0,
                time_base_ns: 0,
            }],
            sample_count: count,
            duration_ns: count * period,
        },
        vec![values],
    )
}

fn source_from(parts: Vec<(Channel, Vec<Vec<f64>>)>) -> Synthetic {
    let mut channels = Vec::new();
    let mut values = Vec::new();
    for (channel, chunk_values) in parts {
        channels.push(channel);
        values.push(chunk_values);
    }
    Synthetic { channels, values }
}

/// Assert every channel round-trips: identity, shape, and every sample bit-exact.
fn assert_lossless(source: &dyn TelemetrySource, written: &MotecFile) {
    let expected: Vec<usize> = (0..source.channels().len())
        .filter(|&i| source.channels()[i].sample_count > 0)
        .collect();
    assert_eq!(
        written.channels().len(),
        expected.len(),
        "channel count changed"
    );

    for (out_index, &in_index) in expected.iter().enumerate() {
        let before = &source.channels()[in_index];
        let after = &written.channels()[out_index];

        assert_eq!(after.name, before.name, "name changed");
        assert_eq!(after.unit, before.unit, "unit changed for {}", before.name);
        assert_eq!(
            after.sample_count, before.sample_count,
            "sample count changed for {}",
            before.name
        );
        assert_eq!(
            after.frequency_hz(),
            before.frequency_hz(),
            "frequency changed for {}",
            before.name
        );
        assert_eq!(
            after.duration_ns, before.duration_ns,
            "duration changed for {}",
            before.name
        );

        // Flatten the source's chunked samples and compare to the LD block.
        let mut flat = Vec::with_capacity(before.sample_count as usize);
        for (chunk_index, chunk) in before.chunks.iter().enumerate() {
            for sample in 0..chunk.sample_count {
                flat.push(source.decode(in_index, chunk_index, sample));
            }
        }
        assert_eq!(flat.len() as u64, after.sample_count);
        for (sample, &want) in flat.iter().enumerate() {
            let got = written.decode(out_index, 0, sample as u64);
            assert_eq!(
                got.to_bits(),
                want.to_bits(),
                "channel {} sample {sample}: wrote {want:?} read back {got:?}",
                before.name
            );
        }
    }
}

#[test]
fn float64_channels_round_trip_bit_exactly() {
    // Values chosen to defeat f32: large magnitudes plus fine mantissa detail,
    // mirroring the "Global Time" / GPS channels in real PDS exports.
    let values = vec![
        0.0,
        -0.0,
        1.0,
        -1.0,
        1.0 / 3.0,
        std::f64::consts::PI,
        1_601_234_567.891_234_5,
        -1_601_234_567.891_234_5,
        f64::MIN_POSITIVE,
        f64::MAX,
        f64::MIN,
        45.085_623_299_999_995,
        10.312_556_7,
        1e-300,
        -1e300,
    ];
    let source = source_from(vec![single_chunk(
        1,
        "Global Time",
        "s",
        SampleType::F64,
        50,
        values,
    )]);
    let bytes = write_motec_bytes(&source, &MotecMetadata::default()).unwrap();
    let written = MotecFile::from_bytes("mem.ld", bytes).unwrap();
    assert_eq!(written.channels()[0].sample_type, SampleType::F64);
    assert_lossless(&source, &written);
}

#[test]
fn every_sample_type_round_trips_at_full_precision() {
    let parts = vec![
        single_chunk(
            1,
            "U8_Chan",
            "",
            SampleType::U8,
            10,
            vec![0.0, 1.0, 127.0, 255.0],
        ),
        single_chunk(
            2,
            "I16_Chan",
            "deg",
            SampleType::I16,
            20,
            vec![-32768.0, -1.0, 0.0, 32767.0],
        ),
        single_chunk(
            3,
            "U16_Chan",
            "kPa",
            SampleType::U16,
            50,
            vec![0.0, 1.0, 65535.0],
        ),
        single_chunk(
            4,
            "I32_Chan",
            "mm",
            SampleType::I32,
            100,
            vec![-2147483648.0, 0.0, 2147483647.0],
        ),
        single_chunk(
            5,
            "U32_Chan",
            "ms",
            SampleType::U32,
            5,
            vec![0.0, 4294967295.0],
        ),
        single_chunk(
            6,
            "F32_Chan",
            "g",
            SampleType::F32,
            200,
            vec![0.5, -0.25, 1.0e10, f32::MAX as f64],
        ),
        single_chunk(
            7,
            "F64_Chan",
            "m/s",
            SampleType::F64,
            50,
            vec![std::f64::consts::E, 1e-17, -98765.4321],
        ),
    ];
    let source = source_from(parts);
    let bytes = write_motec_bytes(&source, &MotecMetadata::default()).unwrap();
    let written = MotecFile::from_bytes("mem.ld", bytes).unwrap();
    assert_lossless(&source, &written);

    // Widths must be chosen so that the integer ranges survive: u16 and u32
    // cannot ride in i16/i32 without wrapping.
    let widened: Vec<SampleType> = written.channels().iter().map(|c| c.sample_type).collect();
    assert_eq!(
        widened,
        vec![
            SampleType::I16, // U8 fits i16
            SampleType::I16,
            SampleType::I32, // U16 widened
            SampleType::I32,
            SampleType::F64, // U32 widened; exact in f64
            SampleType::F32,
            SampleType::F64,
        ]
    );
}

#[test]
fn latin1_units_round_trip() {
    let source = source_from(vec![single_chunk(
        1,
        "Brake Temp FL",
        "°C",
        SampleType::F64,
        20,
        vec![100.0, 200.0],
    )]);
    let bytes = write_motec_bytes(&source, &MotecMetadata::default()).unwrap();
    let written = MotecFile::from_bytes("temperature.ld", bytes).unwrap();
    assert_eq!(written.channels()[0].unit, "°C");
    assert_lossless(&source, &written);
}

#[test]
fn multi_chunk_channels_flatten_without_losing_samples() {
    // Three contiguous chunks at 50 Hz, as produced by an interrupted log.
    let period = 20_000_000u64;
    let chunk_values = vec![
        vec![1.5, 2.5, 3.5],
        vec![4.5, 5.5],
        vec![6.5, 7.5, 8.5, 9.5],
    ];
    let mut chunks = Vec::new();
    let mut sample_base = 0u64;
    let mut time_base = 0u64;
    for values in &chunk_values {
        chunks.push(Chunk {
            sample_period_ns: period,
            sample_count: values.len() as u64,
            data_ptr: 0,
            sample_base,
            time_base_ns: time_base,
        });
        sample_base += values.len() as u64;
        time_base += values.len() as u64 * period;
    }
    let source = Synthetic {
        channels: vec![Channel {
            id: 1,
            name: "Speed_Ref".into(),
            unit: "m/s".into(),
            unit_source: UnitSource::Unknown,
            sample_type: SampleType::F64,
            chunks,
            sample_count: sample_base,
            duration_ns: time_base,
        }],
        values: vec![chunk_values],
    };

    let bytes = write_motec_bytes(&source, &MotecMetadata::default()).unwrap();
    let written = MotecFile::from_bytes("mem.ld", bytes).unwrap();
    assert_eq!(written.channels()[0].chunks.len(), 1, "LD holds one block");
    assert_eq!(written.channels()[0].sample_count, 9);
    assert_lossless(&source, &written);
}

#[test]
fn session_identity_survives_the_round_trip() {
    let source = source_from(vec![single_chunk(
        1,
        "Speed",
        "m/s",
        SampleType::F64,
        50,
        vec![1.0, 2.0],
    )]);
    let metadata = MotecMetadata {
        driver: "Tobi Lutke".into(),
        vehicle: "Oreca 07".into(),
        venue: "Sebring".into(),
        event: "Sebring Test 2026".into(),
        session: "Qualifying".into(),
        short_comment: "round trip".into(),
        date: "22/04/2026".into(),
        time: "17:20:00".into(),
        ..Default::default()
    };
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("session.ld");
    write_motec(&source, &metadata, &path).unwrap();
    let written = MotecFile::open(&path).unwrap();
    let sidecar = std::fs::read_to_string(motec_sidecar_path(&path)).unwrap();

    assert!(sidecar.contains("Id=\"Driver\" Value=\"Tobi Lutke\""));
    assert!(sidecar.contains("Id=\"Event\" Value=\"Sebring Test 2026\""));
    assert_eq!(written.driver, "Tobi Lutke");
    assert_eq!(written.vehicle, "Oreca 07");
    assert_eq!(written.venue, "Sebring");
    assert_eq!(written.date, "22/04/2026");
    assert_eq!(written.time, "17:20:00");
    assert_lossless(&source, &written);
}

#[test]
fn motec_to_motec_round_trip_is_stable() {
    let source = source_from(vec![
        single_chunk(
            1,
            "Speed_Ref",
            "m/s",
            SampleType::F64,
            50,
            vec![10.5, 20.25, 30.125],
        ),
        single_chunk(2, "gear", "", SampleType::I16, 50, vec![1.0, 2.0, 3.0]),
    ]);
    let first = write_motec_bytes(&source, &MotecMetadata::default()).unwrap();
    let parsed = MotecFile::from_bytes("first.ld", first.clone()).unwrap();
    let metadata = MotecMetadata::from_file(&parsed);
    let second = write_motec_bytes(&parsed, &metadata).unwrap();
    assert_eq!(first, second, "re-encoding a parsed LD is not byte-stable");
}

#[test]
fn refuses_to_truncate_oversized_text() {
    let long_name = "C".repeat(32);
    let source = source_from(vec![single_chunk(
        1,
        &long_name,
        "",
        SampleType::F64,
        50,
        vec![1.0],
    )]);
    assert!(matches!(
        write_motec_bytes(&source, &MotecMetadata::default()),
        Err(MotecWriteError::FieldTooLong { .. })
    ));

    let source = source_from(vec![single_chunk(
        1,
        "Speed",
        "veryverylongunit",
        SampleType::F64,
        50,
        vec![1.0],
    )]);
    assert!(matches!(
        write_motec_bytes(&source, &MotecMetadata::default()),
        Err(MotecWriteError::FieldTooLong { .. })
    ));
}

#[test]
fn refuses_shapes_ld_cannot_represent() {
    // Mixed sample periods within one channel.
    let source = Synthetic {
        channels: vec![Channel {
            id: 1,
            name: "Mixed".into(),
            unit: "".into(),
            unit_source: UnitSource::Unknown,
            sample_type: SampleType::F64,
            chunks: vec![
                Chunk {
                    sample_period_ns: 20_000_000,
                    sample_count: 2,
                    data_ptr: 0,
                    sample_base: 0,
                    time_base_ns: 0,
                },
                Chunk {
                    sample_period_ns: 10_000_000,
                    sample_count: 2,
                    data_ptr: 0,
                    sample_base: 2,
                    time_base_ns: 40_000_000,
                },
            ],
            sample_count: 4,
            duration_ns: 60_000_000,
        }],
        values: vec![vec![vec![1.0, 2.0], vec![3.0, 4.0]]],
    };
    assert!(matches!(
        write_motec_bytes(&source, &MotecMetadata::default()),
        Err(MotecWriteError::Channel { .. })
    ));

    // A time gap between chunks cannot be encoded in a single LD block.
    let source = Synthetic {
        channels: vec![Channel {
            id: 1,
            name: "Gapped".into(),
            unit: "".into(),
            unit_source: UnitSource::Unknown,
            sample_type: SampleType::F64,
            chunks: vec![
                Chunk {
                    sample_period_ns: 20_000_000,
                    sample_count: 2,
                    data_ptr: 0,
                    sample_base: 0,
                    time_base_ns: 0,
                },
                Chunk {
                    sample_period_ns: 20_000_000,
                    sample_count: 2,
                    data_ptr: 0,
                    sample_base: 2,
                    time_base_ns: 10_000_000_000,
                },
            ],
            sample_count: 4,
            duration_ns: 10_040_000_000,
        }],
        values: vec![vec![vec![1.0, 2.0], vec![3.0, 4.0]]],
    };
    assert!(matches!(
        write_motec_bytes(&source, &MotecMetadata::default()),
        Err(MotecWriteError::Channel { .. })
    ));

    // A non-integer frequency has no u16 Hz representation.
    let source = Synthetic {
        channels: vec![Channel {
            id: 1,
            name: "Odd".into(),
            unit: "".into(),
            unit_source: UnitSource::Unknown,
            sample_type: SampleType::F64,
            chunks: vec![Chunk {
                sample_period_ns: 3_333_333,
                sample_count: 2,
                data_ptr: 0,
                sample_base: 0,
                time_base_ns: 0,
            }],
            sample_count: 2,
            duration_ns: 6_666_666,
        }],
        values: vec![vec![vec![1.0, 2.0]]],
    };
    assert!(matches!(
        write_motec_bytes(&source, &MotecMetadata::default()),
        Err(MotecWriteError::Channel { .. })
    ));
}

#[test]
fn empty_sources_are_rejected() {
    let source = Synthetic {
        channels: vec![Channel {
            id: 1,
            name: "Nothing".into(),
            unit: "".into(),
            unit_source: UnitSource::Unknown,
            sample_type: SampleType::F64,
            chunks: Vec::new(),
            sample_count: 0,
            duration_ns: 0,
        }],
        values: vec![vec![]],
    };
    assert!(matches!(
        write_motec_bytes(&source, &MotecMetadata::default()),
        Err(MotecWriteError::Empty)
    ));
}
