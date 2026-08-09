# Changelog

All notable user-facing changes are documented here.

## 0.9.0 — 2026-08-09

- Added cached GPS/speed track-station alignment shared by traces, delta, cursor
  readouts, and synchronized primary/reference video.
- Realign both videos precisely whenever playback pauses, while retaining
  bounded rate correction during continuous playback.
- Added lazy, cached telemetry-library indexing and reliable AiM filmstrip lap
  metadata without full sample parsing.
- Replaced local vendor parsing with the pinned upstream Rust telemetry crates.
- Added the fullscreen telemetry HUD, video/folder metadata workflow, and
  native trace-rendering performance improvements.
- Added downloadable Linux, macOS, and Windows builds through GitHub Actions.

## 0.1.0 — 2026-08-05

Initial public development release.

- Cross-format Pi/Cosworth PDS, MoTeC LD, Racelogic VBO, and AiM MP4 ingestion.
- Shared 50 Hz lap normalization, distance-aligned comparison, and corner analysis.
- Native Qt Quick trace workspace with embedded libmpv playback.
- Track Atlas corner metadata with cache-aware offline behavior.
- Headless parsing and unification inspection through `omatrack-cli`.
- Linux desktop integration and Linux/Windows continuous integration.
