# vbo-telemetry

Standalone Racelogic VBOX `.vbo` parser. It handles section-based files, optional column names and units, UTC time-of-day conversion, midnight rollover, irregular timestamps, and custom channels.

```rust
use motorsport_telemetry_core::TelemetrySource;
use vbo_telemetry::VboFile;

let file = VboFile::open("run.vbo")?;
let velocity = file.channels().iter().position(|c| c.name == "velocity kmh").unwrap();
println!("first speed={}", file.decode(velocity, 0, 0));
# Ok::<(), Box<dyn std::error::Error>>(())
```

```sh
cargo run -p vbo-telemetry --example inspect_vbo -- run.vbo
```
