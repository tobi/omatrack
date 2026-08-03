//! Report each channel's unit and where that unit came from.

use cosworth_telemetry::CosworthFile;
use motorsport_telemetry_core::TelemetrySource;
use std::collections::BTreeMap;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = std::env::args().nth(1).expect("usage: show_units FILE.pds");
    let limit: usize = std::env::args()
        .nth(2)
        .and_then(|value| value.parse().ok())
        .unwrap_or(usize::MAX);
    let file = CosworthFile::open(&path)?;

    let mut by_source: BTreeMap<&str, usize> = BTreeMap::new();
    let mut by_unit: BTreeMap<String, usize> = BTreeMap::new();
    for channel in file.channels() {
        *by_source.entry(channel.unit_source.name()).or_default() += 1;
        *by_unit.entry(channel.unit.clone()).or_default() += 1;
    }

    println!("{}: {} channels", path, file.channels().len());
    println!("\nprovenance:");
    for (source, count) in &by_source {
        println!("  {source:<14} {count}");
    }
    println!("\nunits:");
    for (unit, count) in &by_unit {
        let shown = if unit.is_empty() { "(none)" } else { unit };
        println!("  {shown:<10} {count}");
    }

    println!("\n{:<28} {:<10} provenance", "channel", "unit");
    for channel in file.channels().iter().take(limit) {
        println!(
            "{:<28} {:<10} {}",
            channel.name,
            if channel.unit.is_empty() {
                "-"
            } else {
                &channel.unit
            },
            channel.unit_source.name()
        );
    }
    Ok(())
}
