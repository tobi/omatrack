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

## Runtime dependencies

Omatrack links to these dependencies supplied by the operating system or build
environment. Portable release artifacts bundle their redistributable dynamic
libraries and transitive runtime dependencies; source builds normally use the
system copies. Their source is not copied into this repository.

- **Qt 6** — available under the GNU LGPL v3, GNU GPL v2/v3, or commercial terms. See <https://www.qt.io/licensing>.
- **libmpv** — license depends on how mpv was built; LGPL v2.1-or-later is available for qualifying builds, while builds with GPL components are GPL. See <https://github.com/mpv-player/mpv/blob/master/Copyright>.
- **libyaml** — MIT License. See <https://github.com/yaml/libyaml>.
- **ONNX Runtime (optional image reader)** — MIT License and its bundled third-party notices. See <https://github.com/microsoft/onnxruntime>. An enabled build links the explicitly selected SDK; Linux install rules retain its `LICENSE` and `ThirdPartyNotices.txt` under the application's documentation directory.
- **FFmpeg libraries (optional independent image decoder)** — libavformat, libavcodec, libavutil and libswscale; licensing depends on the exact build and enabled components (LGPL/GPL). See <https://ffmpeg.org/legal.html>.

The image reader includes original C++ preprocessing that reproduces Pillow's
BILINEAR resampling semantics, validated against Pillow. No Pillow source is
bundled; Pillow retains its separate HPND-style terms at
<https://github.com/python-pillow/Pillow/blob/main/LICENSE>.

Model weights and private evaluation footage are **not covered by Omatrack's MIT
license and are not bundled in this repository**. Explicit local model staging
is a build convenience, not permission to publish or redistribute the model or
its source data.

Distributors are responsible for satisfying the terms of the exact dependency builds they ship.

## Network-fetched Track Atlas data

Omatrack independently downloads and caches Track Atlas metadata from <https://github.com/tobi/track-atlas>; it does not consume the optional Track Atlas facade from `motorsport-telemetry-rs`. No Track Atlas dataset is bundled in this repository. Track Atlas combines MIT-licensed curated overrides with data from OpenStreetMap and other attributed upstream sources; see its current `LICENSE` and `ATTRIBUTION.md` before redistributing cached data.
