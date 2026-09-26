# Third-party notices

Omatrack's MIT license covers the application code and original project
assets. The components below retain their own terms. Versions and revisions
are those pinned in `rust/Cargo.toml` and `rust/Cargo.lock`; licenses were read
from each crate's published manifest and license files. No third-party source
is copied into this repository; the only bundled third-party assets are the
fonts listed under [Bundled fonts](#bundled-fonts).

## Statically linked Rust crates

### GPUI and gpui-kit

- `gpui-pre` 0.3.6 and its platform crates (`gpui-pre-linux`,
  `gpui-pre-wgpu`, `gpui-pre-platform`, …) — Apache License 2.0.
- `gpui-kit` 0.6.6, `gpui-component` 0.6.6, `gpui-base` 0.6.6 — Apache
  License 2.0.
- `gpui-kit-assets` 0.6.6 — Apache License 2.0; it embeds
  [Lucide](https://lucide.dev/) icons under the ISC License
  (`LICENSE-LUCIDE` in that crate).

### Motorsport telemetry parsers and Track Atlas crate

[`tobi/motorsport-telemetry-rs`](https://github.com/tobi/motorsport-telemetry-rs)
at revision `cac837feb12fe112bd85a159e2d6d49543647313` (1.3.5):
`aim-telemetry`, `cosworth-telemetry`, `motec-telemetry`,
`racelogic-telemetry`, `motorsport-telemetry-core`, `telemetry-format` and
`motorsport-track-atlas` — MIT License. The embedded track catalog inside
`motorsport-track-atlas` carries its own data terms (below).

### libmpv bindings

`libmpv2-sys` 4.0.1 (FFI declarations for libmpv) — declared `LGPL-2.1`.

### Other crates

The remaining dependency graph is predominantly MIT and/or Apache-2.0, with
crates under BSD-2-Clause, BSD-3-Clause, ISC, Zlib, 0BSD, CC0-1.0,
Unlicense (dual with MIT), Unicode-3.0 (ICU data crates), bzip2-1.0.6
(`libbz2-rs-sys`) and MPL-2.0 (`option-ext`, via `dirs-sys`; `cbindgen` and `dwrote` appear
only in the macOS and Windows platform graphs). For the exact list of a build,
run `cargo metadata --locked` (or a tool such as `cargo about`) in `rust/`;
this file does not reproduce every license text.

## Bundled fonts

Embedded into the `omatrack2` binary from `rust/crates/omatrack-ui/assets/fonts`
and used when the desktop configures no UI or monospace font of its own. The
files are the upstream static builds, unmodified.

- **Inter** 4.1 (`Inter-Regular`, `-Medium`, `-SemiBold`, `-Bold`) —
  Copyright (c) 2016 The Inter Project Authors
  (<https://github.com/rsms/inter>). SIL Open Font License 1.1; full text in
  `rust/crates/omatrack-ui/assets/fonts/Inter-OFL.txt`.
- **Geist Mono** 1.7.0 (`GeistMono-Regular`, `-Medium`, `-SemiBold`) —
  Copyright 2024 The Geist Project Authors
  (<https://github.com/vercel/geist-font>). SIL Open Font License 1.1; full
  text in `rust/crates/omatrack-ui/assets/fonts/GeistMono-OFL.txt`.

The OFL permits bundling and embedding the fonts with software; the fonts
themselves may not be sold on their own, and the license texts travel with
them.

## Dynamically linked system libraries

Supplied by the operating system; not bundled.

- **libmpv** (mpv ≥ 0.41, client API ≥ 2.5) — the license depends on how mpv
  was built: LGPL-2.1-or-later for builds without GPL components, GPL
  otherwise (distribution packages are commonly GPL builds). See
  <https://github.com/mpv-player/mpv/blob/master/Copyright>. It pulls in
  FFmpeg, whose terms likewise depend on its build
  (<https://ffmpeg.org/legal.html>).
- **Graphics and windowing** used by GPUI on Linux (Wayland client libraries,
  libxkbcommon, libxcb, the Vulkan loader, fontconfig, FreeType) — each under
  its own permissive or FreeType/fontconfig license as shipped by the
  distribution.

Distributors are responsible for satisfying the terms of the exact
dependency builds they ship.

## Track Atlas data

The track catalog embedded through `motorsport-track-atlas` is generated from
[Track Atlas](https://github.com/tobi/track-atlas). Omatrack shows this
attribution (`omatrack_core::track::ATTRIBUTION`) wherever atlas data appears:

> Track data: Track Atlas (https://github.com/tobi/track-atlas).
> Geometry and named-corner coordinates are derived from OpenStreetMap data
> via the Overpass API: © OpenStreetMap contributors, licensed under the Open
> Database License (ODbL), https://opendatacommons.org/licenses/odbl/.
> Ordered corner metadata and the colloquial corner-name base layer come from
> Lovely-Sim-Racing/lovely-track-data
> (https://github.com/Lovely-Sim-Racing/lovely-track-data); corner and
> straight names are credited upstream to Racing Circuits.
> Curated overrides (official names, complex grouping) are offered under the
> Track Atlas MIT license.

Centerlines and corner locations are derived data and retain the ODbL
attribution. Reuse of the lovely-track-data layer follows that project's
upstream terms, which are not restated here.
