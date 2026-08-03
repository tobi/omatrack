# Racecraft agent guide

## Mission

Racecraft is a native racing-telemetry workstation. Its primary target is Linux under Omarchy, built with Qt 6, Qt Quick, and Material controls. It should turn heterogeneous logger files into a coherent, driver-facing model of sessions, laps, channels, tracks, corners, and corner complexes.

This is not a single-format file viewer or a generic chart demo. The product direction is a full telemetry system that can ingest every major race format through one analysis pipeline. The current parser bridge supports Pi/Cosworth `.pds`, MoTeC `.ld` (with `.ldx` treated as a companion), and Racelogic `.vbo`; that is the starting set, not the intended limit.

## Product goal

Build a generic telemetry workstation that happens to work exceptionally well
for an LMP2 IMSA team. Do not overfit the implementation to that team: paths,
driver names, car numbers, classes, event assumptions, and other team-specific
values must not be hardcoded in the codebase. Make those values easy to
configure, persist, and override through the application model and preferences.

## Product philosophy

### Racing concepts first

Expose the concepts drivers and engineers use: session, lap, reference lap, distance, delta, braking point, turn-in, apex, throttle pickup, corner, and complex. File chunks, sample encodings, parser quirks, and vendor naming belong below the product boundary.

A corner is an individual turn/apex and its analysis range. A corner complex is a named, contiguous group of corners that drivers discuss as one section. Preserve both levels: a user must be able to inspect the complex as a whole and drill into its members. Never flatten complexes into synthetic single corners or infer a complex merely because two turns are adjacent.

### One normalized analytical truth

Every format feeds the same cross-format model. Standard channels, units, lap bounds, monotonic distance, and comparison semantics must not depend on the source vendor. Format-specific decoding belongs in Rust; cross-format racing analysis belongs in the Qt-free C++ core.

The current canonical lap is a 50 Hz `UnifiedLap`. Primary/reference traces, cursor values, delta, and corner analysis must derive from the same normalized arrays so two views cannot disagree.

### Dense, calm, expert UX

Optimize for excellent information density, not empty space or mobile-sized chrome. Keep the current hierarchy: compact session context, proportional lap strip, session tree, large trace workspace, and focused inspectors. Numeric data uses a compact monospace treatment; color communicates role and comparison consistently.

Dense does not mean noisy. Default views show the channels that answer common driving questions. Raw and specialist channels remain one action away. Prefer progressive disclosure, direct manipulation, keyboard/mouse fluency, and side-by-side context over wizard flows and decorative panels.

### Performance is a product contract

Interactive rendering targets 60–120 fps. A frame is 16.67 ms at 60 Hz and 8.33 ms at 120 Hz; hot interaction paths should be designed for the 8.33 ms budget.

- Never parse, resample, scan whole channels, perform network I/O, or rebuild static geometry on cursor movement.
- Keep static traces separate from the lightweight cursor/selection overlay.
- Cache normalized laps, raw-channel resamples, delta arrays, geometry, and overview rasters; invalidate them only when their inputs change.
- Bound draw work to the viewport and pixel budget. Do not submit every source sample when fewer points can produce the same image.
- Avoid per-frame heap allocation and avoid copies of full telemetry arrays.
- Benchmark hover and zoom paths before and after renderer changes. A visually correct regression that misses the frame budget is not complete.

### Native Linux and Omarchy are deliberate

Use Qt Quick Material for application chrome and C++/Qt rendering for hot paths. Do not introduce a web stack or a second UI toolkit. Omarchy is the first-class desktop integration: follow its active color theme when available, while retaining the current `SystemPalette` fallback so the app still starts outside Omarchy. Cross-platform behavior is welcome when it does not weaken the Linux experience or complicate the hot path.

### Preserve source truth

Telemetry and onboard-video inputs are immutable evidence. Never rewrite, rename, or delete source files. Normalization, aliases, corner edits, caches, and exported analysis are separate state. Be especially careful with event data outside this source tree.

### Track Atlas is authoritative

[Track Atlas](https://github.com/tobi/track-atlas) is the upstream source of track/layout identity, real geometry, label layers, corner points, `corner_ranges`, and `corner_complexes`. Consume its schema; do not create a competing local track schema or duplicate curated metadata in application code.

Network access is an enhancement, not a boot requirement. Cache upstream data, continue to work offline, and fail back cleanly. Bundled corner CSV files are compatibility fallbacks, not the long-term source of truth.

## Feature model

### Ingestion and session library

- Recursively scan configured directories and open individual telemetry files.
- Dispatch formats through the Rust parser workspace.
- Infer inexpensive metadata from filenames/folders before parsing samples.
- Group the library as Track → Date → Session → Laps.
- Detect lap boundaries from the best available beacon, lap-time, lap-number, or lap-distance signal.
- Mark outlaps and fastest laps and cache parsed/unified laps lazily per session.
- Persist session directories and user-facing aliases with `QSettings`.

### Normalization and analysis

`TelemetryEngine` maps vendor channel names to standard concepts and normalizes them into a 50 Hz lap:

- speed in km/h
- throttle, driver throttle, and clutch in `[0, 1]`
- brake pressure in bar, with pedal-position fallback
- steering in degrees
- integer gear
- monotonic cumulative distance in metres
- longitudinal acceleration, four dampers, and GPS latitude/longitude

Native lap distance is unwrapped across start-line resets and rejected when jumps are implausible; speed integration is the fallback. Missing optional channels remain representable and must not make an otherwise useful lap unloadable.

### Trace workspace

- Overlay an active lap and optional reference lap.
- Show a distance-aligned cumulative delta that starts at zero.
- Render standard channels plus opt-in raw source channels.
- Configure channel visibility, color, and lane weight; pin lanes while scrolling.
- Share one cursor/readout across traces.
- Pan, zoom, select ranges, scroll channels, and navigate with mouse and keyboard.
- Keep corner/complex ranges visible without obscuring the data.
- Allow manual reference alignment for signals such as damper traces without changing the underlying lap data.

### Embedded video playback

- Open MP4, MOV, MKV, AVI, M4V, and WebM files inside the main analysis workspace.
- Render through libmpv's OpenGL Render API in `MpvVideoItem`; never spawn the mpv CLI or embed a foreign native window.
- Keep playback controls compact: play/pause, exact seek, five-second steps, single-frame advance, and mute.
- Treat video files as read-only. Playback must not parse or rewrite embedded telemetry.
- Qt Quick must use the OpenGL graphics API before the first window because `QQuickFramebufferObject` and libmpv share that context.
- `RACECRAFT_VIDEO=/path/to/video` opens a startup video and is also used by the GUI acceptance harness.

### Corner intelligence

- Resolve track and layout against Track Atlas aliases, external IDs, series, and lap length.
- Honor the layout’s label model and default driver-facing labels.
- Treat individual `corner_ranges` and grouped `corner_complexes` as distinct first-class analysis scopes.
- Preserve complex membership and any Track Atlas landmarks instead of reconstructing them in QML.
- Provide single-lap and primary/reference summaries: time, entry/apex/exit speed, gear, steering, brake/lift/turn-in/apex/throttle points, deltas, and trace excerpts.
- Support automatic brake-zone fallback, direct range editing, and local user overrides when authoritative data is unavailable.
- Keep Track Atlas cache refresh and offline fallback explicit in preferences.

The current implementation imports `corner_ranges` and exposes a corner inspector. `corner_complexes`, full geometry, and the rest of the Track Atlas range layers are product requirements still to be carried through the application model; extend the model rather than overloading `CornerZone` until it loses meaning.

### Headless tools and automation

`racecraft-cli` is the headless acceptance surface for parsing, mapping, lap detection, and 50 Hz unification. The GUI also has screenshot and paint-benchmark modes driven by `RACECRAFT_AUTOTEST*` environment variables.

## Architecture

```text
.pds / .ld(+.ldx) / .vbo / future race formats
              |
              v
third_party/motorsport-telemetry/crates/*     Rust, vendor-specific parsing
              |
              v
third_party/motorsport-telemetry/bridge       panic-safe bulk C ABI
              |
              v
src/core/TelemetryEngine                      Qt-free normalization + laps
              |
              +--------------------+
              |                    |
              v                    v
cli/main.cpp                       src/app/TelemetryStore
headless acceptance                sessions, state, cache, Track Atlas
                                           |
                                  +--------+-----------+---------+
                                  |                    |         |
                                  v                    v         v
                           src/app/TraceView  src/app/MpvVideoItem  src/qml/Main.qml
                           telemetry canvas  libmpv rendering      Material UI
```

### Build graph

CMake builds the vendored Rust workspace into `libracecraft_bridge.a`, links it into the static `racecraft_core`, then links that core into both `racecraft-cli` and the Qt application. The Qt application also links libmpv through `pkg-config`. `src/qml/application.qrc` embeds QML, Geist fonts, and compatibility corner CSVs.

### Layer responsibilities

| Layer | Paths | Owns | Must not own |
|---|---|---|---|
| Vendor parsers | `third_party/motorsport-telemetry/crates/` | File validation, memory mapping, chunks, typed decoding, vendor metadata | Qt, UI state, racecraft-specific presentation |
| C ABI | `third_party/motorsport-telemetry/bridge/` | Extension dispatch, opaque handles, bulk decode, stable strings, thread-local errors | Analysis policy or exceptions/panics crossing FFI |
| Core | `src/core/TelemetryEngine.*` | Channel mapping, units, lap detection, resampling, `UnifiedLap` | Qt types, QML, settings, network access |
| Session/store | `src/app/TelemetryStore.*` | Lazy session handles, selection, comparison, viewport, caches, preferences, Track Atlas, corner analysis | Pixel-level paint loops or vendor byte parsing |
| Renderer | `src/app/TraceView.*` | Frame-budget-sensitive painting and direct trace interaction | Parsing, network access, persistent product state |
| Video renderer | `src/app/MpvVideoItem.*` | libmpv lifecycle, OpenGL FBO rendering, playback state, exact seek and frame-step | Telemetry extraction, session association, or QML layout policy |
| QML UI | `src/qml/Main.qml` | Material windows, layout, delegates, controls, high-level orchestration | Full telemetry loops, duplicated analysis, format branches |
| Bootstrap | `src/main.cpp` | Qt startup, type registration, theme bridge, automation harness | Product analysis |
| CLI | `cli/main.cpp` | Reproducible headless acceptance and inspection | A second analysis implementation |

### Core data contracts

- `RawChannel`: decoded physical samples, unit, sample type, frequency, and duration for one source channel.
- `Lap`: source-session bounds and lap time.
- `UnifiedLap`: same-rate, lap-relative arrays used by every analysis and rendering feature.
- `SessionHandle`: owns one file, defers parsing until needed, and caches unified laps.
- `TelemetryStore`: the single Qt-facing source of truth for active/reference selection and UI state.
- `CornerZone`: the current individual corner range. Do not stretch it to represent every Track Atlas layer; introduce explicit domain types when complexes and geometry enter the model.

### Invariants

1. `racecraft_core` remains Qt-free and usable by the CLI.
2. The UI never branches on telemetry format. Add a parser or mapping; do not add `.pds`/`.ld` special cases to QML.
3. Unified channel arrays share the lap’s 50 Hz time base. Keep their lengths aligned with `time`.
4. Unified distance starts at zero and is monotonic.
5. Comparisons are distance-aligned. Index-aligned or raw-time-aligned deltas are incorrect unless a feature explicitly requires another domain.
6. The same cached delta feeds both the plotted trace and numeric cursor readout.
7. Primary means active lap; compare means reference lap. Preserve that semantic and its colors throughout the UI.
8. Track identity and corner metadata come from Track Atlas when available; local edits are overlays, not upstream truth.
9. Parser errors become explicit failures. No Rust panic, C++ exception, or invalid pointer crosses the ABI boundary.
10. Optional channels and optional network data degrade gracefully; silent fabrication does not.

## Where changes belong

- New file format or source encoding: add/extend a Rust parser crate and expose only generic capabilities through the bridge.
- New cross-format channel or unit rule: `TelemetryEngine` and `UnifiedLap`.
- New lap/corner comparison metric: C++ analysis in the store/core, exposed as compact view data.
- New persistent user preference: `TelemetryStore`/`QSettings`; never write it into telemetry.
- New Material control, inspector, or layout: QML.
- New high-frequency visual: `TraceView` or another focused C++ Quick item, with measured frame cost.
- New track metadata: contribute it to Track Atlas. Racecraft should consume the upstream result.
- New Track Atlas layer support: parse it into a typed application model, retaining IDs, labels, ranges, members, and landmarks.

Do not fix parser ambiguity with filename-specific UI conditionals. Do not copy analysis into JavaScript for convenience. Do not move cold UI layout into C++ without a measured reason.

## Build and run

Requirements: CMake 3.21+, `pkg-config`, libmpv development files, a C++17 compiler, Qt 6.5+ (`Core`, `Gui`, `Quick`, `QuickControls2`, `Widgets`, `Qml`, `Network`), and Rust/Cargo 1.84+.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

./build/racecraft /path/to/telemetry-directory
RACECRAFT_VIDEO=/path/to/onboard.mp4 ./build/racecraft /path/to/telemetry-directory
./build/racecraft-cli parse /path/to/copied-session.pds
./build/racecraft-cli unify /path/to/copied-session.pds
```

`racecraft-cli unify` writes `<input>.unified.csv` beside the input. Run it only on a copied fixture or in a location where generated output is acceptable; never use raw event telemetry as a disposable test fixture.

The Rust workspace can be checked independently:

```sh
cargo test --manifest-path third_party/motorsport-telemetry/Cargo.toml
```

## Verification

Use real, copied telemetry for the format and behavior being changed. Synthetic samples are useful for narrow math checks but are not evidence that a vendor file still parses.

### Parser, bridge, or core

1. Build `racecraft-cli`.
2. Run `racecraft-cli parse` on each affected format; inspect format, mapped channels, and detected laps.
3. Run `racecraft-cli unify` when mapping, units, resampling, distance, or lap bounds changed; inspect sample count and physical plausibility.
4. Run the relevant Rust crate tests, then the workspace tests for shared-trait or bridge changes.

### Store, Track Atlas, or comparison logic

1. Open a real multi-lap session.
2. Select active and reference laps with different lengths.
3. Exercise cursor, distance delta, raw channels, corner selection, and alignment as applicable.
4. For Track Atlas work, verify a cached/offline start and a known track/layout match. Verify both individual corners and complexes when the changed model supports them.

### UI and renderer

The base autotest opens the first session’s fastest lap, renders the app, saves a screenshot, and exits:

```sh
QT_QPA_PLATFORM=offscreen \
RACECRAFT_AUTOTEST=/tmp/racecraft.png \
./build/racecraft /path/to/copied-telemetry
```

Add feature flags as needed:

- `RACECRAFT_AUTOTEST_COMPARE=1`
- `RACECRAFT_AUTOTEST_WINDOWS=1`
- `RACECRAFT_AUTOTEST_SELECTION=1`
- `RACECRAFT_AUTOTEST_ALIGNMENT=1`
- `RACECRAFT_AUTOTEST_CORNER=1`
- `RACECRAFT_AUTOTEST_HOVER=1`
- `RACECRAFT_AUTOTEST_ZOOM=1`
- `RACECRAFT_VIDEO=/path/to/onboard.mp4`

`HOVER` and `ZOOM` print average paint time. Treat 16.67 ms as the hard 60 fps ceiling and 8.33 ms as the design target for continuous interaction. Inspect the screenshot as well; timing alone cannot catch illegible density, overlap, incorrect colors, or stale comparison state.

For visual work, also run the app in the target Linux/Omarchy desktop. Offscreen output does not verify native palette integration, font rendering, window behavior, pointer feel, or high-refresh animation.

Embedded libmpv playback must be verified on the native Linux/Omarchy OpenGL scene graph. The offscreen platform can capture the surrounding QML but does not establish the shared `QQuickFramebufferObject` render context. When `RACECRAFT_VIDEO` is set, the native autotest exits non-zero unless the player is ready, the file is loaded, and duration is available.

## Current boundaries to keep explicit

- The bridge currently dispatches only `.pds`, `.ld`, and `.vbo`; `.ldx` resolves to its `.ld` companion and is not parsed independently.
- Session parsing is lazy, but opening a source currently decodes and retains whole channel arrays. Do not describe it as streaming.
- The app currently consumes Track Atlas `corner_ranges`; first-class complexes and geometry are not wired through yet.
- `sessionStartUnixTime()`/`hasGlobalTime()` do not currently provide global session time.
- The GUI is file-based post-session analysis today. Future live or database-backed work must preserve the same normalized core instead of bypassing it.
- The app currently embeds one video with manual playback controls. It does not yet extract `aimd` telemetry, persist session/video associations, or align multiple videos.

## Definition of done

A change is complete only when it:

- lives in the correct layer and preserves the contracts above;
- works for the affected real format/session, not only a hand-built sample;
- keeps source telemetry immutable;
- preserves dense, coherent primary/reference UX;
- uses Track Atlas rather than local track metadata where applicable;
- demonstrates the relevant rendering path still meets the frame budget; and
- updates this guide when it changes the architecture, product philosophy, or a stated current boundary.
