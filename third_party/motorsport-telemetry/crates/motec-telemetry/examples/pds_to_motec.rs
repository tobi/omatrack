//! End-to-end PDS -> MoTeC LD -> read-back verification on a real recording.
//!
//! Usage: pds_to_motec INPUT.pds [OUTPUT.ld]
//!
//! Exits non-zero if a single sample, name, unit, rate or count differs.

use cosworth_telemetry::CosworthFile;
use motec_telemetry::{motec_sidecar_path, write_motec, MotecFile, MotecMetadata};
use motorsport_telemetry_core::TelemetrySource;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let input = args.next().expect("usage: pds_to_motec INPUT.pds [OUT.ld]");
    let output = args.next().unwrap_or_else(|| {
        let stem = std::path::Path::new(&input)
            .file_stem()
            .unwrap_or_default()
            .to_string_lossy();
        format!("/tmp/{stem}.ld")
    });

    let pds = CosworthFile::open(&input)?;
    let sampled: Vec<usize> = (0..pds.channels().len())
        .filter(|&i| pds.channels()[i].sample_count > 0)
        .collect();
    println!(
        "read  {input}\n      {} channels ({} with samples)",
        pds.channels().len(),
        sampled.len()
    );

    let metadata = MotecMetadata {
        driver: "PDS Export".into(),
        vehicle: "Unknown".into(),
        venue: "Unknown".into(),
        ..Default::default()
    };
    let start = std::time::Instant::now();
    write_motec(&pds, &metadata, &output)?;
    let elapsed = start.elapsed();
    let written_size = std::fs::metadata(&output)?.len();
    let sidecar = motec_sidecar_path(&output);
    let sidecar_size = std::fs::metadata(&sidecar)?.len();
    println!(
        "wrote {output}\n      {written_size} bytes plus {} byte {} in {:.2?} ({:.1} MB/s)",
        sidecar_size,
        sidecar.display(),
        elapsed,
        written_size as f64 / 1e6 / elapsed.as_secs_f64()
    );

    let ld = MotecFile::open(&output)?;
    println!("read  back {} channels", ld.channels().len());

    let mut failures = 0usize;
    let mut total_samples = 0u64;
    assert_eq!(
        ld.channels().len(),
        sampled.len(),
        "channel count mismatch: LD has {} vs {} sampled PDS channels",
        ld.channels().len(),
        sampled.len()
    );

    println!(
        "\n{:<26} {:>9} {:>7} {:>8}  result",
        "channel", "samples", "Hz", "type"
    );
    for (out_index, &in_index) in sampled.iter().enumerate() {
        let before = &pds.channels()[in_index];
        let after = &ld.channels()[out_index];
        let mut problems: Vec<String> = Vec::new();

        if after.name != before.name {
            problems.push(format!("name {:?} != {:?}", after.name, before.name));
        }
        if after.unit != before.unit {
            problems.push(format!("unit {:?} != {:?}", after.unit, before.unit));
        }
        if after.sample_count != before.sample_count {
            problems.push(format!(
                "count {} != {}",
                after.sample_count, before.sample_count
            ));
        }
        if after.frequency_hz() != before.frequency_hz() {
            problems.push(format!(
                "freq {:?} != {:?}",
                after.frequency_hz(),
                before.frequency_hz()
            ));
        }
        if after.duration_ns != before.duration_ns {
            problems.push(format!(
                "duration {} != {}",
                after.duration_ns, before.duration_ns
            ));
        }

        // Compare every sample bit-for-bit against the flattened PDS stream.
        let mut mismatches = 0u64;
        let mut worst = 0.0f64;
        let mut cursor = 0u64;
        for (chunk_index, chunk) in before.chunks.iter().enumerate() {
            for sample in 0..chunk.sample_count {
                let want = pds.decode(in_index, chunk_index, sample);
                let got = ld.decode(out_index, 0, cursor);
                if got.to_bits() != want.to_bits() {
                    mismatches += 1;
                    let error = (got - want).abs();
                    if error > worst {
                        worst = error;
                    }
                }
                cursor += 1;
                total_samples += 1;
            }
        }
        if mismatches > 0 {
            problems.push(format!(
                "{mismatches} sample mismatches (worst abs err {worst:.6e})"
            ));
        }

        let verdict = if problems.is_empty() {
            "OK".to_string()
        } else {
            failures += 1;
            format!("FAIL: {}", problems.join("; "))
        };
        println!(
            "{:<26} {:>9} {:>7.0} {:>8}  {}",
            before.name,
            before.sample_count,
            before.frequency_hz().unwrap_or(0.0),
            before.sample_type.name(),
            verdict
        );
    }

    println!(
        "\n{} channels, {} samples compared bit-for-bit",
        sampled.len(),
        total_samples
    );
    if failures == 0 {
        println!("ZERO DATA LOSS: every sample, name, unit, rate and count matches.");
        Ok(())
    } else {
        Err(format!("{failures} channel(s) failed the round trip").into())
    }
}
