# cosworth-telemetry

Standalone memory-mapped Pi/Cosworth PDS parser. It supports marker and markerless definitions, typed values, compact exports, bounds checking, and authoritative chunk-table ordering.

```rust
use motorsport_telemetry_core::TelemetrySource;
use cosworth_telemetry::CosworthFile;

let file = CosworthFile::open("run.pds")?;
let speed = file.channels().iter().position(|c| c.name == "Speed_Ref").unwrap();
let first = file.decode(speed, 0, 0);
let at_one_second = file.sample_at(speed, 1_000_000_000, true);
# Ok::<(), Box<dyn std::error::Error>>(())
```

```sh
cargo run -p cosworth-telemetry --example inspect_cosworth -- run.pds
```
