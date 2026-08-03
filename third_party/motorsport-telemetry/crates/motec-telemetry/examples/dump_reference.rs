//! Dump a PDS as `name\tunit\tfreq\thexbits,...` for cross-validation.

use cosworth_telemetry::CosworthFile;
use motorsport_telemetry_core::TelemetrySource;
use std::io::{BufWriter, Write};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let input = args.next().expect("usage: dump_reference IN.pds OUT.tsv");
    let output = args.next().expect("usage: dump_reference IN.pds OUT.tsv");
    let pds = CosworthFile::open(&input)?;
    let mut out = BufWriter::new(std::fs::File::create(&output)?);

    for index in 0..pds.channels().len() {
        let channel = &pds.channels()[index];
        if channel.sample_count == 0 {
            continue;
        }
        write!(
            out,
            "{}\t{}\t{}\t",
            channel.name,
            channel.unit,
            channel.frequency_hz().unwrap_or(0.0).round() as u64
        )?;
        let mut first = true;
        for (chunk_index, chunk) in channel.chunks.iter().enumerate() {
            for sample in 0..chunk.sample_count {
                if !first {
                    write!(out, ",")?;
                }
                first = false;
                write!(
                    out,
                    "{:x}",
                    pds.decode(index, chunk_index, sample).to_bits()
                )?;
            }
        }
        writeln!(out)?;
    }
    out.flush()?;
    println!("wrote {output}");
    Ok(())
}
