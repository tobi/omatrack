# Third-party notices

Omatrack's MIT license covers the application code and original project assets. The following components and services retain separate terms.

## Bundled components

### Motorsport telemetry for Rust

Omatrack depends on [`tobi/motorsport-telemetry-rs`](https://github.com/tobi/motorsport-telemetry-rs) at the revision recorded in `third_party/motorsport-telemetry/Cargo.lock`. It is distributed under the MIT License. The local `third_party/motorsport-telemetry` directory contains only Omatrack's C ABI adapter, header-generation tool, and dependency manifests; no vendor parser source is copied into this repository.

### cbindgen

The Rust-to-C bridge header is generated at build time with
[`cbindgen`](https://github.com/mozilla/cbindgen), distributed under the Mozilla
Public License 2.0. Its pinned version and checksum are recorded in
`third_party/motorsport-telemetry/Cargo.lock`.

### Geist fonts

The Geist and Geist Mono font files under `src/app/assets/fonts` are distributed under the SIL Open Font License 1.1. The license text is preserved at `src/app/assets/fonts/OFL.txt`.

## System dependencies

Omatrack links to these dependencies supplied by the operating system or build environment; their source is not copied into this repository.

- **Qt 6** — available under the GNU LGPL v3, GNU GPL v2/v3, or commercial terms. See <https://www.qt.io/licensing>.
- **libmpv** — license depends on how mpv was built; LGPL v2.1-or-later is available for qualifying builds, while builds with GPL components are GPL. See <https://github.com/mpv-player/mpv/blob/master/Copyright>.
- **libyaml** — MIT License. See <https://github.com/yaml/libyaml>.

Distributors are responsible for satisfying the terms of the exact dependency builds they ship.

## Network-fetched Track Atlas data

Omatrack independently downloads and caches Track Atlas metadata from <https://github.com/tobi/track-atlas>; it does not consume the optional Track Atlas facade from `motorsport-telemetry-rs`. No Track Atlas dataset is bundled in this repository. Track Atlas combines MIT-licensed curated overrides with data from OpenStreetMap and other attributed upstream sources; see its current `LICENSE` and `ATTRIBUTION.md` before redistributing cached data.
