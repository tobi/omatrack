use motec_telemetry::MotecFile;
use motorsport_telemetry_core::TelemetrySource;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = std::env::args().nth(1).expect("usage: inspect FILE.ld");
    let file = MotecFile::open(path)?;
    println!(
        "driver={} vehicle={} venue={}",
        file.driver, file.vehicle, file.venue
    );
    for channel in file
        .channels()
        .iter()
        .filter(|channel| channel.sample_count > 0)
    {
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
