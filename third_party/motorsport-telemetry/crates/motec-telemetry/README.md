# motec-telemetry

Standalone memory-mapped MoTeC `.ld` parser. It validates the LD header, walks channel metadata, decodes float and integer channels, and applies MoTeC scale/shift/multiplier conversion.

```rust
use motec_telemetry::MotecFile;
use motorsport_telemetry_core::TelemetrySource;

let file = MotecFile::open("run.ld")?;
println!("{} / {} / {}", file.driver, file.vehicle, file.venue);
for channel in file.channels() {
    println!("{}: {:?} Hz", channel.name, channel.frequency_hz());
}
# Ok::<(), Box<dyn std::error::Error>>(())
```

Sample parsing does not require the companion `.ldx`. The writer creates one alongside every exported LD, recovering beacon markers from a dedicated lap trigger (or an increasing lap counter as fallback) and carrying supplied session metadata:

```rust
use motec_telemetry::{write_motec, MotecMetadata};
# use motorsport_telemetry_core::TelemetrySource;
# fn export(source: &dyn TelemetrySource) -> Result<(), Box<dyn std::error::Error>> {
write_motec(source, &MotecMetadata::default(), "run.ld")?;
// Writes run.ld and run.ldx.
# Ok(())
# }
```

```sh
cargo run -p motec-telemetry --example inspect_motec -- run.ld
```
