# motorsport-telemetry-core

Shared format-neutral channel, chunk, sample-type, and interpolation model used by the PDS, MoTeC, VBO, and DuckDB crates.

`TelemetrySource` exposes exact sample decoding, timestamps, and resampling without depending on DuckDB. Continuous floating-point channels can use linear interpolation; integer and known event/discrete channels use step interpolation.
