use motorsport_telemetry_core::TelemetrySource;
use vbo_telemetry::VboFile;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = std::env::args().nth(1).expect("usage: inspect FILE.vbo");
    let file = VboFile::open(path)?;
    println!(
        "{}: {} rows, {} channels",
        file.path(),
        file.time_ns.len(),
        file.channels().len()
    );
    for channel in file.channels() {
        println!(
            "{:5.1} Hz {:10} {:8} {}",
            channel.frequency_hz().unwrap_or(0.0),
            channel.sample_count,
            channel.unit,
            channel.name
        );
    }
    Ok(())
}
