# Contributing to Omatrack

## Before changing code

Open an issue for broad product or architecture changes. Small fixes with a clear scope can go directly to a pull request.

Omatrack preserves three boundaries:

1. Vendor-specific decoding belongs in `third_party/motorsport-telemetry`.
2. Cross-format normalization and analysis belong in the Qt-free C++ core.
3. QML owns layout and orchestration; hot telemetry loops and rendering do not belong in JavaScript.

Track identity, geometry, and curated corner metadata come from [Track Atlas](https://github.com/tobi/track-atlas). Contribute authoritative metadata upstream rather than duplicating it in this repository.

## Development workflow

Configure and build with the checked-in presets:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Before opening a pull request:

```sh
cmake --build --preset debug --target lint
ctest --preset debug
```

Use `cmake --build --preset debug --target cpp_format` and `qml_format` to apply the project formatters. Rust sources use `cargo fmt`.

## Verification expectations

- Parser or bridge changes: parse a copied real fixture for every affected format and run the corresponding Rust crate tests.
- Normalization or lap changes: run `omatrack unify` on a copied real fixture and inspect sample counts, units, distance monotonicity, and physical plausibility.
- Comparison changes: exercise primary/reference laps with different durations and with missing or degraded GPS.
- UI or renderer changes: run the native application, inspect the changed interaction, and check hover/zoom paint timing against the 8.33 ms design target and 16.67 ms hard ceiling.
- Video changes: verify the native OpenGL scene graph. Offscreen screenshots do not establish libmpv and `QQuickFramebufferObject` context sharing.

Never modify, rename, or delete source telemetry or onboard video. Generated CSV files and caches belong beside copied fixtures or in disposable locations.

## Pull requests

Keep changes focused. Include:

- the problem and chosen behavior;
- affected formats and platforms;
- exact commands or scenarios used for verification;
- before/after timing for renderer hot-path changes;
- screenshots for visible UI changes, with private telemetry details removed.

Do not commit build trees, compile databases, telemetry files, videos, credentials, or machine-specific configuration.
