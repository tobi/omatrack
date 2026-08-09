<div align="center">
  <img src="assets/omatrack.svg" width="112" height="112" alt="Omatrack logo">
  <h1>Omatrack</h1>
  <p>Native, cross-format motorsport telemetry analysis.</p>

  [![CI](https://github.com/tobi/omatrack/actions/workflows/ci.yml/badge.svg)](https://github.com/tobi/omatrack/actions/workflows/ci.yml)
  [![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
</div>

Omatrack is a Qt 6 workstation for drivers and engineers who need one analysis workflow across heterogeneous logger formats. It normalizes every source into the same 50 Hz lap model, so traces, delta, cursor readouts, corner analysis, and synchronized onboard video use one analytical truth.

Omatrack is under active development. Linux is the primary desktop target;
Linux, macOS, and Windows are built and tested in CI.

## Screenshots

### Distance-aligned lap comparison

![Omatrack comparison workspace with primary and reference telemetry traces](docs/screenshots/lap-comparison.webp)

Shared cursor readouts, proportional lap strips, range selection, delta, and
manual damper alignment all use the same normalized lap model.

### Corner analysis

![Omatrack corner analysis comparing braking, turn-in, apex, and throttle pickup](docs/screenshots/corner-analysis.webp)

Corner metrics and trace excerpts keep the primary and reference laps aligned
through braking, turn-in, apex, and throttle pickup.

## Current capabilities

- Open Pi/Cosworth `.pds`, MoTeC `.ld`, Racelogic `.vbo`, and AiM `aimd` telemetry embedded in `.mp4`.
- Resolve `.ldx` files to their companion `.ld`; play ordinary MP4, MOV, MKV, AVI, M4V, and WebM video without treating it as telemetry.
- Group a library as Track → Date → Session → Laps, with lazy parsing and fastest-lap selection.
- Compare primary and reference laps through a cached track-station map that remains useful when GPS is sparse or absent.
- Overlay standard and raw channels, share a cursor, pan, zoom, select ranges, pin lanes, and manually align specialist signals.
- Derive distance-aligned cumulative delta and corner metrics from the normalized lap arrays.
- Consume Track Atlas corner ranges through an independent offline cache,
  spatially map them from the layout centerline when lap GPS is usable, and
  retain user-owned YAML overrides.
- Synchronize embedded onboard video through libmpv's OpenGL Render API.
- Follow the active Omarchy palette on Linux, with a Qt system-palette fallback elsewhere.
- Inspect parsing, channel mapping, lap detection, and unification through `omatrack-cli`.

## Architecture

```mermaid
flowchart TD
    F["PDS / LD / VBO / MP4"] --> R["motorsport-telemetry-rs"]
    R --> B["Panic-safe bulk C ABI"]
    B --> C["Qt-free C++ normalization core"]
    C --> CLI["omatrack-cli"]
    C --> S["TelemetryStore"]
    A["Track Atlas JSONL"] --> S
    S --> T["C++ trace renderer"]
    S --> V["libmpv video renderer"]
    S --> Q["Qt Quick Material UI"]
```

Vendor decoding stays in Rust. Cross-format racing analysis stays in the Qt-free C++ core. Session state and caching live in `TelemetryStore`; QML owns layout rather than telemetry loops. See [AGENTS.md](AGENTS.md) for the full architecture and engineering contracts.

## Requirements

- CMake 3.21+
- Ninja
- C++17 compiler
- Qt 6.5+ with Core, Gui, Quick, Quick Controls 2, QML, and Network
- libmpv development files
- libyaml development files
- Rust/Cargo 1.84+
- `pkg-config`

### Arch Linux

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf \
  qt6-base qt6-declarative mpv libyaml rust
```

### Ubuntu and other Linux distributions

Install Qt 6.5 or newer from your distribution or the Qt installer, then install the native dependencies. On Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build pkg-config libmpv-dev libyaml-dev
```

### Windows

Use the **MSYS2 UCRT64** shell:

```sh
pacman -S --needed git \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-libyaml \
  mingw-w64-ucrt-x86_64-mpv \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-qt6-base \
  mingw-w64-ucrt-x86_64-qt6-declarative \
  mingw-w64-ucrt-x86_64-rust
```

### macOS

Install the native dependencies with Homebrew, then install Qt 6.5 or newer
from Qt or Homebrew:

```sh
brew install cmake ninja pkg-config mpv libyaml rust
```

## Build and run

```sh
git clone https://github.com/tobi/omatrack.git
cd omatrack
cmake --preset release
cmake --build --preset release
./build/omatrack /path/to/telemetry-directory
```

On macOS, run `./build/Omatrack.app/Contents/MacOS/Omatrack`. On Windows, run
`./build/omatrack.exe` from the UCRT64 shell.

Open a single supported file with the file picker or pass its containing directory. Configuration is stored in `$XDG_CONFIG_HOME/omatrack/omatrack.yml` on Linux, falling back to `~/.config/omatrack/omatrack.yml`.
An existing pre-rename `racecraft.yml`, legacy `QSettings` preferences, and Track Atlas cache are imported once; legacy files remain untouched as a backup.

## Headless inspection

```sh
./build/omatrack-cli parse /path/to/copied-session.pds
./build/omatrack-cli unify /path/to/copied-session.pds \
  --output /tmp/session.unified.csv
```

`unify` refuses to overwrite an existing file. Its CSV includes GPS latitude and longitude when available; treat exported files as sensitive location data. Omatrack never rewrites source telemetry or video.

## Install

```sh
cmake --install build --prefix "$HOME/.local"
```

The install target provides `omatrack`, `omatrack-cli`, platform deployment
metadata, and license notices. Tagged releases publish a Linux AppImage, a
macOS disk image, and a Windows zip from GitHub Actions. The macOS build is
ad-hoc signed but not Apple-notarized.

## Privacy and network behavior

Telemetry and video stay local. Omatrack does not upload session data. Track Atlas connectivity is independent of telemetry parsing: when its metadata or selected-layout geometry cache is missing or older than 24 hours, Omatrack requests the public data from `raw.githubusercontent.com`; manual refresh performs the same metadata request. Fresh caches are used without a startup request. Offline starts retain cached corner geometry; without it, GPS laps do not silently substitute distance-based corner locations.

libmpv can open URLs through its API, but Omatrack's normal file selectors and session library provide local paths.


## Test and lint

```sh
ctest --preset release
cmake --build --preset release --target lint
```

The test suite covers the Rust integration bridge, C++ normalization and comparison behavior, formatting, Rust lints, and QML invariants. Parser-crate tests live upstream in `motorsport-telemetry-rs`. CI runs the release build and tests on Linux, macOS, and Windows.

Update all upstream parser crates to the same commit and regenerate the lockfile with:

```sh
./scripts/update-motorsport-telemetry.sh \
  --smoke-file /path/to/copied-session.mp4
```

The optional smoke file is parsed only after the Rust, build, lint, and unit checks pass. Supply a branch, tag, or full commit as the final argument to pin something other than upstream `HEAD`.

## Project status and boundaries

- Session parsing is lazy, but opening a source currently decodes and retains whole channel arrays; this is not a streaming reader.
- Track Atlas corner ranges and GPS-based centerline station mapping are wired
  through the app. First-class corner complexes and full geometry rendering
  remain planned work.
- Embedded AiM telemetry time is translated to MP4 presentation time through
  the upstream edit-list offset exposed by Omatrack's C bridge.
- Separate-file persisted video associations and multi-video alignment are not implemented.
- The GUI is file-based post-session analysis today.

Issues and focused pull requests are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) before changing parser, normalization, or rendering behavior.

## License

Omatrack is released under the [MIT License](LICENSE). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for bundled font/parser licenses, system dependency terms, and Track Atlas data attribution.
