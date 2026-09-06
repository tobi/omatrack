# Native Linux aarch64 video development environment

## Scope

[scripts/setup-video-dev.sh](../scripts/setup-video-dev.sh) provides an isolated
Linux aarch64 Qt/libmpv/FFmpeg development environment. It installs dependencies
inside the checkout, without system package installation, shell activation or
changes to desktop configuration. Other platforms need their own dependencies.

The `acceptance` preset enables the GUI test harness; it is not a production
release build. The `release` preset excludes that harness. Image extraction also
requires the optional ONNX Runtime SDK and a trusted local reader model; see
[GAUGE_READER_RUNTIME.md](GAUGE_READER_RUNTIME.md).

## Dependencies and isolation

The helper's default working directory is ignored `build-video-dev/`:

| Path | Purpose |
| --- | --- |
| `build-video-dev/prefix` | conda-forge native linux-aarch64 Qt/mpv/development prefix |
| `build-video-dev/bootstrap` | micromamba bootstrap |
| `build-video-dev/mamba` | micromamba package cache/root |
| `build-video-dev/conda-explicit.txt` | Resolved package URLs/builds |
| `build-video-dev/debs` | Downloaded Xvfb/GL diagnostic packages |
| `build-video-dev/x11` | Extracted packages, not system-installed |
| `build-video-dev/xvfb-run.*` | Per-run scratch HOME, XDG directories, Xauthority and X-server log |

Validated dependency versions:

- Qt `qt6-main` **6.11.2**, including Quick, Material/QuickControls2, Qml,
  Network, Concurrent, Test, development headers and Qt 6 tools.
- mpv **0.41.0**, `mpv.pc` / libmpv client API **2.5.0**.
- libyaml (`yaml` package) **0.2.5**, pkgconf **3.0.7**.
- FFmpeg **9.0.1**, `libavformat` / `libavcodec` **63.1.101**,
  `libavutil` **61.1.101**, `libswscale` **10.1.101**.
- libgl/libegl development packages, xorg-xorgproto, expat and zlib.

`install` uses micromamba with `--no-rc`, conda-forge only and strict channel
priority. It verifies the micromamba 2.9.0 bootstrap archive against its pinned
SHA-256. Dependency downloads require network access; the resolved package list
is recorded locally rather than committed as a machine-specific lock.

`install-xvfb` uses the existing Ubuntu package index with `apt-get download` and
`dpkg-deb -x`. It does not run apt update/install, dpkg install or sudo. The
isolated display requires existing `xauth` and `mcookie` commands.

## Configure, build and test

Run from the checkout root:

```sh
scripts/setup-video-dev.sh install
scripts/setup-video-dev.sh install-xvfb

scripts/setup-video-dev.sh exec cmake --preset acceptance \
  -B build-video-dev/acceptance \
  -DPKG_CONFIG_EXECUTABLE="$PWD/build-video-dev/prefix/bin/pkgconf"
scripts/setup-video-dev.sh exec cmake --build build-video-dev/acceptance --parallel 8

scripts/setup-video-dev.sh exec env QT_QPA_PLATFORM=offscreen \
  ctest --test-dir build-video-dev/acceptance --output-on-failure -LE lint --parallel 4
scripts/setup-video-dev.sh exec \
  ctest --test-dir build-video-dev/acceptance --output-on-failure -L lint --parallel 2
```

These offscreen tests do not establish video or custom scene-graph rendering.
Use the real OpenGL acceptance path below for those surfaces.

`exec` prepends the local prefix to the child process's `PATH`,
`CMAKE_PREFIX_PATH`, `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH`, preserving inherited
search paths. It sets `PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1` and
`PKG_CONFIG_ALLOW_SYSTEM_LIBS=1`, and defaults CMake/Cargo build parallelism to
8 unless overridden. These changes apply only to the child process.

The helper supplies `bin/pkg-config -> pkgconf`. The `PKG_CONFIG_ALLOW_SYSTEM_*`
flags are needed for direct compiler probes because pkgconf can otherwise omit
its own prefix's include/library paths as system paths, even when the compiler
does not search them.

Use the prefix's Qt 6 tools, such as
`build-video-dev/prefix/lib/qt6/bin/qmllint`, or the generated CMake targets.
Do not substitute a Qt 5 tool found on PATH. `setup-video-dev.sh env` prints the
build-path exports, not credentials or user configuration.

### Enable the image reader

Supply the matching ONNX Runtime **C/C++ SDK**, not just its Python wheel. Keep
SDKs, model exports and private fixtures under ignored `work/` or outside the
checkout. For example:

```sh
scripts/setup-video-dev.sh exec cmake --preset acceptance \
  -B build-video-dev/image-acceptance \
  -DPKG_CONFIG_EXECUTABLE="$PWD/build-video-dev/prefix/bin/pkgconf" \
  -DOMATRACK_ENABLE_IMAGE_TELEMETRY=ON \
  -DONNXRUNTIME_ROOT=/path/to/onnxruntime-sdk
scripts/setup-video-dev.sh exec cmake --build build-video-dev/image-acceptance --parallel 8
```

Choose a trusted model through **Reader model…** in the application. Alternatively,
add `-DOMATRACK_GAUGE_MODEL=/path/to/reviewed/gauge-reader.onnx` when configuring
to stage the matching model beside that executable and include it in that
build's install rules. This option is empty by default and grants **no model
redistribution rights**. Do not publish a model-bearing install tree without
separate authorization. See the runtime guide for the pinned export and parity
checks.

## Isolated OpenGL acceptance

The helper starts a separate Xvfb using `-displayfd`, disables TCP and creates a
fresh Xauthority cookie. It stops only that child server on exit. A FIFO signals
readiness without a sleep/poll loop. Tests use scratch HOME and XDG config,
cache, data and runtime directories; inherited D-Bus and Wayland session
variables are unset. The helper does not connect to the user's desktop display
or read/write the user's Omatrack configuration.

The child process uses XCB, the OpenGL Qt Quick backend, Mesa software GL and
stderr Qt logging. Mesa llvmpipe provides a **real OpenGL context**, unlike the
geometry-less Qt Quick software adaptation selected by a plain offscreen run.
It is suitable for functional rendering checks, not a hardware-performance claim.

Inspect the isolated GL context with:

```sh
scripts/setup-video-dev.sh xvfb \
  "$PWD/build-video-dev/x11/usr/bin/glxinfo.aarch64-linux-gnu" -B
```

For an authorized native-telemetry recording, keep the input read-only and put
all screenshots/logs in the ignored build tree:

```sh
VIDEO=/path/to/native-recording.mp4
mkdir -p build-video-dev/acceptance/screenshots

scripts/setup-video-dev.sh exec build-video-dev/acceptance/omatrack parse "$VIDEO"
scripts/setup-video-dev.sh xvfb env \
  OMATRACK_AUTOTEST="$PWD/build-video-dev/acceptance/screenshots/video.png" \
  OMATRACK_VIDEO="$VIDEO" QSG_INFO=1 \
  timeout 180 scripts/autotest.sh "$PWD/build-video-dev/acceptance/omatrack" --mute
```

On desktops without Hyprland, `scripts/autotest.sh` runs the command on the
isolated Xvfb display supplied by the helper. Inspect the actual rendered video,
filmstrip and traces; successful loading alone does not prove correct rendering.
For standalone inferred-HUD scenarios, see
[IMAGE_TELEMETRY_ACCEPTANCE.md](IMAGE_TELEMETRY_ACCEPTANCE.md).

## Validation and troubleshooting

Initial dependency validation on Linux aarch64 passed **26/26 non-lint tests**
and **5/5 configured lint checks**, and exercised real Qt Quick OpenGL geometry
and embedded libmpv playback. Those historical totals describe that validation
snapshot, not the current test inventory. C++ formatting and clang-tidy were
not part of that initial run. Re-run the configured checks for each source change.

Common setup issues:

- Missing `expat.pc` or `zlib.pc` can prevent libmpv discovery even when runtime
  libraries exist. Install the development packages into the local prefix.
- Missing `pkg-config` is addressed by the helper's symlink and explicit CMake
  `PKG_CONFIG_EXECUTABLE` option.
- Direct `GL/gl.h` compiler probes may need the pkgconf flags described above.
- A libmpv probe must use the C numeric locale; Omatrack sets this at startup.
- A CLI listing no standalone GL window contexts does not establish that the
  embedded API is unavailable. Test
  `mpv_render_context_create(MPV_RENDER_API_TYPE_OPENGL, ...)` in a Qt GL context.
- Xvfb/llvmpipe may report software-rendering and vsync/timer warnings. A real
  OpenGL backend is still required; these warnings do not validate high-refresh
  performance.
- Muted acceptance does not test audible playback or the desktop audio backend.
- See [VIDEO_SCALER_COMPATIBILITY.md](VIDEO_SCALER_COMPATIBILITY.md) for the
  libmpv scaler-padding issue and the application's bilinear display policy.

Hardware-accelerated desktop behavior, audio and 60–120 Hz interaction budgets
require separate testing on the target system. Keep generated recordings, model
weights, logs, screenshots, user state and dependency trees private. They are
not public fixtures or attachments to a source-code change.
