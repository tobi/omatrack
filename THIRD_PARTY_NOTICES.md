# Third-party notices

Omatrack's MIT license covers the application code and original project assets. The following components and services retain separate terms.

## Bundled components

### Motorsport telemetry parser workspace

`third_party/motorsport-telemetry` is distributed under the MIT License. Its license text is preserved at `third_party/motorsport-telemetry/LICENSE`.

### Geist fonts

The Geist and Geist Mono font files under `src/app/assets/fonts` are distributed under the SIL Open Font License 1.1. The license text is preserved at `src/app/assets/fonts/OFL.txt`.

## System dependencies

Omatrack links to these dependencies supplied by the operating system or build environment; their source is not copied into this repository.

- **Qt 6** — available under the GNU LGPL v3, GNU GPL v2/v3, or commercial terms. See <https://www.qt.io/licensing>.
- **libmpv** — license depends on how mpv was built; LGPL v2.1-or-later is available for qualifying builds, while builds with GPL components are GPL. See <https://github.com/mpv-player/mpv/blob/master/Copyright>.
- **libyaml** — MIT License. See <https://github.com/yaml/libyaml>.

Distributors are responsible for satisfying the terms of the exact dependency builds they ship.

## Network-fetched track data

Omatrack can download and cache Track Atlas metadata from <https://github.com/tobi/track-atlas>. No Track Atlas dataset is bundled in this repository. Track Atlas combines MIT-licensed curated overrides with data from OpenStreetMap and other attributed upstream sources; see its current `LICENSE` and `ATTRIBUTION.md` before redistributing cached data.
