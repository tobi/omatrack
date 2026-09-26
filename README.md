<div align="center">
  <img src="assets/omatrack.svg" width="112" height="112" alt="Omatrack logo">
  <h1>Omatrack</h1>
  <p>Native, keyboard-first motorsport telemetry analysis.</p>

  [![CI](https://github.com/tobi/omatrack/actions/workflows/ci.yml/badge.svg)](https://github.com/tobi/omatrack/actions/workflows/ci.yml)
  [![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
</div>

Omatrack is a racing-telemetry workstation for drivers and race engineers doing
post-session analysis. It turns heterogeneous logger files into one model of
sessions, laps, channels, tracks, corners and corner complexes, and compares a
primary lap with a reference lap across traces, delta, corner analysis, track
map and synchronized onboard video. Every view derives from one 50 Hz lap and
one cached alignment map, so there is a single analytical truth.

Omatrack 2.0 is a rewrite in Rust on [GPUI](https://www.gpui.rs/) and
gpui-kit, in [`rust/`](rust/). It is under active development: the analysis
core and CLI are complete and regression-tested; the workstation UI is being
built. Linux (and [Omarchy](https://omarchy.org/) in particular) is the first
target. The 1.x Qt application has been retired.

## Supported formats

- Pi/Cosworth `.pds`
- MoTeC `.ld`
- Racelogic `.vbo`
- AiM `aimd` telemetry embedded in `.mp4`
- native `.telemetry` and MTJ JSONL

Decoding comes from the pinned
[`motorsport-telemetry-rs`](https://github.com/tobi/motorsport-telemetry-rs)
crates; track identity, layouts and corners come from the embedded
[Track Atlas](https://github.com/tobi/track-atlas) catalog. Source telemetry
and video are never rewritten, renamed or written beside.

## Build and run

Requirements: stable Rust (1.90+), `pkg-config`, libmpv **2.5 or newer**
development files (mpv 0.41+), and the usual GPUI Linux libraries (Wayland,
libxkbcommon, libxcb, Vulkan loader, fontconfig, freetype). On Arch Linux:

```sh
sudo pacman -S --needed base-devel pkgconf rust mpv \
  wayland libxkbcommon libxkbcommon-x11 libxcb vulkan-icd-loader \
  fontconfig freetype2
```

Older distribution libmpv packages (for example current Ubuntu LTS) are too
old; the build fails with a clear message when libmpv < 2.5.

```sh
git clone https://github.com/tobi/omatrack.git
cd omatrack/rust
cargo build --release --locked -p omatrack-app
target/release/omatrack2
```

Always build with `--locked`: the GPUI stack is yanked from the crates.io
index and resolves only through the checked-in `Cargo.lock`.

## Headless inspection

The same binary runs the analysis headlessly when the first argument is a
command; no window system is touched and no configuration is read.

```sh
omatrack2 parse <file.pds|file.ld|file.vbo|file.mp4|file.telemetry>
omatrack2 unify <file> --output <csv>
omatrack2 corners <file> [--lap N] [--reference <file>] [--reference-lap N] \
  --zone <start:end> [--zone ...]
omatrack2 compare <aimd.mp4> <file.telemetry>
```

`corners` runs the corner analyzers on the fastest lap (or the laps given);
zones are lap fractions. `unify` refuses to overwrite an existing file and
exports GPS latitude and longitude when available: treat the CSV as sensitive
location data.

## Configuration

`$XDG_CONFIG_HOME/omatrack/omatrack.yml` (else `~/.config/omatrack/omatrack.yml`)
is the only user config store: telemetry locations, channel display, drivers,
recents, per-track corner overrides, video and trace settings, and the
workspace layout. It is hand-editable and unknown keys are preserved. A
`TRACK.yml` in any telemetry folder supplies metadata inherited by every
recording below it. Caches live under `$XDG_CACHE_HOME/omatrack/`.

The UI follows the live Omarchy palette
(`~/.local/state/omarchy/current/theme/colors.toml`) and hot-reloads it;
without one it uses the built-in gpui-component dark theme.

## Keyboard

| Keys | Action |
|---|---|
| ctrl-k / ctrl-, / ctrl-o / ctrl-q | Palette / Preferences / Open folder / Quit |
| ctrl-b / ctrl-j | Toggle library / inspector dock |
| ctrl-1 … ctrl-6 | Focus Library / Traces / Video / Corners / Laps / Map |
| space; left / right | Play/pause; ±2 s |
| m / s / p | Mute / 0.25x / continuous playback |
| f / escape | Video fullscreen / exit (escape also closes overlays) |
| 1–5 | Split, primary+PiP, reference+PiP, primary only, reference only |
| x / a | Swap roles / edit corners |
| h / j | Previous / next corner |
| = / - / ctrl-0 | Zoom in / out / reset |
| [ / ] / t | Previous / next lap / toggle Distance-Time axis |
| enter / alt-enter (Library) | Set primary / reference |
| ctrl-s / escape (resize, corner edit) | Save / cancel |

Every action is also reachable from the command palette (ctrl-k).

## Privacy

Telemetry and video stay local; Omatrack uploads nothing. Track Atlas data is
embedded in the build, so no network access is needed.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the gates and conventions, and
[AGENTS.md](AGENTS.md) for the product and engineering contract.

## License

Omatrack is released under the [MIT License](LICENSE). See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for dependency licenses,
libmpv terms and Track Atlas (ODbL) attribution.
