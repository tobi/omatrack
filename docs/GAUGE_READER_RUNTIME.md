# GaugeReader: current crop/count reader on native CPU

## Scope and interface

`src/inference/GaugeReader.h/.cpp` is Qt-free C++17. It implements the **current
quarantine-v3 CountReader**, not the old whole-frame exports. No training,
telemetry input, model upload, video rewriting, or Python subprocess is involved
in native inference. ONNX Runtime's default CPU execution provider is used with
one intra-op thread, one inter-op thread and sequential execution.

```cpp
using namespace omatrack::inference;
GaugeReader reader(modelPath); // construct/load and destroy on the worker
if (!reader.ready()) { /* expose reader.modelError() */ }
GaugeRgb24Frame frame{rgb.data(), rgb.size(), width, height, stride};
GaugeResult result = reader.read(frame);
// Associate result with THIS decoded frame's actual PTS in the controller.
```

The frame is borrowed, top-down RGB24 with positive stride. `byteSize` is required;
truncated buffers and overflowing dimensions/strides fail before pixel access.
Padding between rows is allowed; final-row padding is not required. The input must
be the full **1920×1080 source frame**, without display scaling, cropping,
letterboxing or mpv/QML overlays. Other geometries return `UnsupportedGeometry`.
One instance belongs to one serial worker; concurrent calls on it are unsupported.

The result contains:

- `admission`: `NotChecked`, `UnsupportedGeometry`, `Rejected`, or `Supported`.
- `error`: `None`, `InvalidFrame`, `RuntimeUnavailable`, `ModelLoadFailed`, or
  `InferenceFailed`, with a human-readable `detail`.
- `gear`, `stintLap`, `brakeFillPct`, `throttleFillPct`: each is a
  `GaugeObservation { optional<double> value; string unknownReason; }`.
- `latencyMs`: admission + crop/resize + model + CTC decode, excluding model load
  and the caller's video decoding.

**Unknown never becomes zero.** `stintLap` is the displayed counter, not an
inferred/classified session lap. Brake is visible fill in percent, **not bar**,
pressure, pedal travel or calibrated force. No speed, GPS, lap bounds or native
channel replacement is produced. A rejected frame has no observations. Invalid
frames take error precedence; otherwise a missing model/runtime is reported even
if the image would also be rejected. Admission and runtime readiness are separate.
`inspectLayout()` provides the same image-only gate without loading a model.

PTS is deliberately absent from this API. The caller must retain actual decoded
presentation timestamps and discard stale results after source/seek changes. The
reader cannot establish timestamp correctness and makes no screenshot-time claim.
Scheduling, cancellation, persistence and UI are outside this component.

## Conservative admission — not arbitrary HUD detection

The supported configuration is the reviewed **1920×1080 orange AiM layout**.
The export's compatibility metadata uses `omatrack.layout` value
`tds_aim_orange-1920x1080`. Admission uses **only pixels**; there are no source-file
names, driver/team logos, telemetry, digit confidences, model outputs or
remembered values in the gate.

All these structural checks must pass (source pixel coordinates, half-open boxes):

| Anchor | Region/check | Requirement |
|---|---|---|
| Red brake column | `[968,642,984,880]` | ≥94% red-dominant pixels |
| Green throttle column | `[1024,642,1040,880]` | ≥94% green-dominant pixels |
| Narrow red-column edges | x=976 inside; x=960,992 outside, y=646..879 | ≥90% rows inside red **and both flanks not red** |
| Narrow green-column edges | x=1032 inside; x=1016,1048 outside | ≥90% equivalent green structure |
| Upper orange/brown scale track | `[1415,855,1875,877]` | ≥82% orange/brown pixels |
| Lower orange/brown scale track | `[1410,988,1875,1006]` | ≥82% orange/brown pixels |
| RPM scale ticks/text | `[1440,956,1880,981]` | 1.2–15% bright neutral pixels |

Regions are sampled every two pixels; column edges every four rows. The exact
RGB inequalities are named predicates in `GaugeReader.cpp`; dark red/green and
brown deliberately count, so empty bars do not suppress a legitimate zero fill.
The anchors were reviewed on local full-resolution frames from three recordings.
There is no learned detector or copied private image template. Digit crops
additionally require 1.5–60% bright-neutral pixels; an erased/blank digit remains
unknown even if the surrounding layout is admitted.

**Limits:** this is a conservative *fixed-layout heuristic*. Rescaling,
repositioning, mirroring, another skin, overlays, occlusion, colour changes and
compression can reject valid HUDs. An unrelated/adversarial image reproducing the
structure can pass. The false-positive rate on an independent broad no-HUD corpus
has **not** been measured. This is not recognition of the scale labels, arbitrary
HUD detection, or a calibrated confidence score. Non-digit glyphs, unfamiliar
fonts and partial occlusions can still produce wrong numeric predictions; glyph
presence is only a blank-crop guard. Do not label `Supported` as guaranteed truth.

## Exact preprocessing and decoding

The exporter checks SHA256 of the original `model.py`, `count_model.py`,
`decode.py`, and `data.py` before importing the model. It selects the exact pure
`crop()` / `tensor_crops()` definitions and image constants from `data.py` through
AST, avoiding that module's legacy training/dataset imports. It does not rewrite
those functions, load records or labels, or modify the research checkout.

Native preprocessing matches the reviewed configuration:

| Field | Source crop `[left,top,right,bottom]` | Conversion |
|---|---|---|
| Gear | `[1399,1010,1475,1079]` | aspect-preserving resize, black centre pad |
| Displayed stint lap | `[408,994,479,1044]` | aspect-preserving resize, black centre pad |
| Brake | `[956,628,999,894]` | clockwise 90° rotation, resize |
| Throttle | `[1011,628,1055,894]` | clockwise 90° rotation, resize |

These are `floor(left/top)` and `ceil(right/bottom)` of the original normalized
windows at source geometry. Both digit resizes have no half-integer rounding tie.
Bars rotate bottom-to-top into left-to-right. All crops become 192×64 RGB8.
Pillow BILINEAR includes antialiasing on reduction: separable triangular filters,
22-bit normalized coefficients and uint8 rounding after **each** pass. A generic
bilinear interpolation, OpenCV default resize, float-only intermediate, or Qt
scaled display capture is not equivalent. Native bytes were compared exactly to
Pillow 12.3.0; algorithm semantics follow Pillow's `libImaging/Resample.c`.

The ONNX input `crops` is float32 `[4,3,64,192]`, ordered gear/lap/brake/throttle,
NCHW, RGB divided by 255. The graph includes ImageNet mean/std normalization, the
shared encoder, digit logits, sigmoid fill head and digit-count head. Outputs are:

- `digits`: float32 `[4,11,24]`, class 0 blank; classes 1–10 represent digits 0–9.
- `fills`: float32 `[4]`, used only for brake/throttle and multiplied by 100.
- `counts`: float32 `[4,3]`, used only for gear/lap.

Digit count is `argmax(counts)+1`. The native decoder preserves the original CTC
prefix-beam algorithm: width **10 separately per prefix length**, repeated-digit
blank transitions, stable insertion/tie ordering, final selection at the
image-predicted length. It does not substitute greedy decoding, impose a gear
vocabulary, forbid leading zeros, interpolate history, or infer a value from
other fields. One to three decoded digits become an integer. Finite checks reject
NaN/Inf model output rather than coercing it; outside-domain fill remains unknown.

Runtime loading requires the current embedded contract, preprocessing, layout,
decoder and checkpoint metadata, plus exact tensor names/shapes/types. An old
whole-frame model, missing metadata, or another checkpoint is refused. These are
compatibility/provenance checks, **not** a signature authenticating untrusted ONNX
files: install only a trusted local export. The receipt is not required at runtime.

## Reproducible local export (no training)

Run from the Omatrack root. Keep all dependencies, images, fixtures and weights
under ignored `work/gauge-reader/`; no system installation is needed. Existing
research code and config remain untouched. The scripts below explicitly bypass
inherited package indexes; they change no global configuration.

```sh
mkdir -p work/gauge-reader
uv venv work/gauge-reader/venv --python 3.12

env -u UV_INDEX -u UV_EXTRA_INDEX_URL -u PIP_EXTRA_INDEX_URL \
  uv --no-config pip install --python work/gauge-reader/venv/bin/python \
  --default-index https://download.pytorch.org/whl/cpu \
  torch==2.10.0 torchvision==0.25.0

env -u UV_INDEX -u UV_EXTRA_INDEX_URL -u PIP_EXTRA_INDEX_URL \
  uv --no-config pip install --python work/gauge-reader/venv/bin/python \
  --default-index https://pypi.org/simple \
  timm==1.0.29 pillow==12.3.0 numpy==2.4.3 \
  onnx==1.20.1 onnxruntime==1.23.2

uv run --no-project --python work/gauge-reader/venv/bin/python \
  scripts/export-gauge-reader.py \
  --research-dir /path/to/research-checkout \
  --output-dir work/gauge-reader/export-new \
  --image-dir /path/to/private/full-resolution-images \
  --native-test work/gauge-reader/gauge-reader-test
```

`--image PATH` may be repeated instead of `--image-dir`. Output must be empty and
outside the research checkout and input paths. Real local images are required;
random-only parity is insufficient. The default checkpoint is
`work/crop_reader_quarantine_v3/training/selected.pt` beneath `--research-dir`;
`--checkpoint` can relocate it but cannot change its required SHA256:

```text
2b1bedae45f08c9187e8e26a92cc1a01db30bda812e8fb7d85fe51368a80faeb
```

The script uses `weights_only=True`, `pretrained=False`, strict state loading,
CPU evaluation/inference mode, deterministic seeds, and no network model fetch.
It exports a fixed batch-4 opset-17 graph, validates ONNX, compares all three
outputs and decoded strings to PyTorch, and optionally runs the native test before
publishing `gauge-reader.onnx`. The pinned TorchScript exporter and its fixed-shape
tracer warning are intentional; there are no dynamic input shapes. It emits an
ignored receipt with package/source/model hashes, output errors and image hashes.
Private fixtures include full frames, exact crop bytes, expected numeric outputs,
36 CTC oracles and deliberately invalid model files; **do not commit them**.

The validated local artifact is
`work/gauge-reader/export/gauge-reader.onnx` (551,783 parameters, selected epoch
20 zero-based). Independent exports produced identical ONNX bytes:

```text
97029f70068f4ec276b3d6bc28810763275806f579d91ddd4701b544af392147
```

The source distribution includes no model weights or evaluation images. The
optional `OMATRACK_GAUGE_MODEL` setting stages a model into a local build/install;
it does not authorize publishing that model. Generate the complete bad-model,
nonfinite and negative fixtures under **`<output-dir>/fixtures`** with the exporter
above, and keep them private.

## Native dependency and application integration

Verified on Linux aarch64, ONNX Runtime CPU **1.23.2**. Fetch the official SDK
locally (the Python wheel alone does not provide the C++ headers):

```sh
mkdir -p work/gauge-reader/deps
curl -fL --retry 2 \
  https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-linux-aarch64-1.23.2.tgz \
  -o work/gauge-reader/deps/onnxruntime.tgz
printf '%s  %s\n' \
  7c63c73560ed76b1fac6cff8204ffe34fe180e70d6582b5332ec094810241e5c \
  work/gauge-reader/deps/onnxruntime.tgz | sha256sum -c -
tar -xzf work/gauge-reader/deps/onnxruntime.tgz -C work/gauge-reader/deps
```

`cmake/GaugeInference.cmake` builds the C++17 `omatrack_inference` library. Set
`ONNXRUNTIME_ROOT` to the matching SDK prefix to enable the reader; CMake does not
auto-search system SDKs. FFmpeg development libraries are also needed for the
application's independent frame decoder. For example:

```sh
cmake --preset release \
  -DOMATRACK_ENABLE_IMAGE_TELEMETRY=ON \
  -DONNXRUNTIME_ROOT=/path/to/onnxruntime-sdk
cmake --build --preset release
```

Use the [isolated development helper](VIDEO_BUILD_ENVIRONMENT.md) when Qt/libmpv
are supplied by a project-local prefix. Select a trusted model with **Reader
model…**, or set `OMATRACK_GAUGE_MODEL=/path/to/reviewed/gauge-reader.onnx` at
configure time to stage it into that build and install tree. Model staging is
empty by default and grants no redistribution rights.

Build and deployment contracts:

- With the SDK enabled, the library privately defines
  `OMATRACK_HAVE_ONNXRUNTIME=1` and links the matching runtime. Without the SDK,
  the stub retains image admission and explicitly reports `RuntimeUnavailable`.
  ONNX headers are not exposed through the public reader header.
- `OMATRACK_ENABLE_IMAGE_TELEMETRY=OFF` disables the optional runtime/decoder
  dependencies. Playback and native telemetry remain available.
- An ORT-enabled application must load its matching runtime library. Linux
  install rules include the runtime library, its SONAME link and SDK license/
  notices. This is optional compilation, not a `dlopen` implementation that can
  survive a missing runtime after enabling it. Other platforms require matching
  SDKs and separate package verification.
- `gauge-reader-test` compiles `tests/gauge_reader_test.cpp` and a separate copy of
  `GaugeReader.cpp` with `OMATRACK_GAUGE_READER_TESTING=1`, using the same optional
  SDK linkage. Exact byte/CTC test hooks are absent from the production binary.
  Set both `OMATRACK_GAUGE_TEST_MODEL` and `OMATRACK_GAUGE_TEST_FIXTURES` at
  configure time to register the optional private `gauge-reader-parity-test`.
  Do not publish its input model, fixture images or expected readings.

Equivalent standalone build, usable without Qt or CMake:

```sh
ORT="$PWD/work/gauge-reader/deps/onnxruntime-linux-aarch64-1.23.2"
c++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -DOMATRACK_HAVE_ONNXRUNTIME=1 -DOMATRACK_GAUGE_READER_TESTING=1 \
  -Isrc/inference -I"$ORT/include" \
  src/inference/GaugeReader.cpp tests/gauge_reader_test.cpp \
  -L"$ORT/lib" -lonnxruntime -Wl,-rpath,"$ORT/lib" \
  -o work/gauge-reader/gauge-reader-test
work/gauge-reader/gauge-reader-test \
  --model work/gauge-reader/export/gauge-reader.onnx \
  --fixtures work/gauge-reader/export/fixtures

c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc/inference \
  src/inference/GaugeReader.cpp tests/gauge_reader_test.cpp \
  -o work/gauge-reader/gauge-reader-test-stub
work/gauge-reader/gauge-reader-test-stub
```

No-argument tests require no private data/model and cover validation, explicit
missing model/runtime, and 22 synthetic negative images. Add the fixture arguments
only to a private local integration test, not a public CI dependency on images.

## Validation and measured latency

Measured on Linux aarch64 with GCC 13.3, optimized `-O2`, ORT 1.23.2 and one CPU
thread (no CUDA/GPU provider). The following are aggregate validation results;
recording identities, individual timestamps, images and readings are not public
fixtures:

- **19 full-resolution real frames from three recordings**, decoded with PyAV
  16.1.0 while retaining actual PTS/time bases; no mpv screenshot was used.
- Native crop bytes exactly equal Pillow for all 76 real crops.
- All native gear/lap outputs equal the current PyTorch count-constrained decoder.
  Native fill maximum absolute error: **0.0000179 percentage points**.
- PyTorch versus ONNX maximum absolute differences: digits `5.913e-5`, fills
  `1.788e-7`, counts `1.907e-5`. All decoded strings match. A separate read-only
  run using PyTorch `2.14.0+cu130` with CPU inference also matched all 19 digit
  pairs; maximum fill difference from
  the pinned export environment was `0.0000119` percentage points.
- All **36** count-CTC oracles pass, including count 1/2/3, repeated digits,
  equal-logit ties and deterministic random logits.
- All **22** synthetic negatives rejected: five uniform shades, three saturated
  solid colours, twelve random frames, checkerboard, and coloured HUD-like blocks
  without the required scale structure.
- All **76** derived real-scene negatives rejected: expanded unoverlaid
  road/cockpit pixels, removed scale regions with digits retained, translated HUD,
  and mirrored HUD. These are transformations of the reviewed frames, **not an
  independent broad no-HUD-video benchmark**.
- Padded strides give unchanged numerical results. An erased gear crop returns
  unknown while the visible lap remains available. Truncated buffers, invalid
  dimensions, overflow strides, missing/malformed/unversioned/wrong-checkpoint
  models all fail explicitly. A valid-contract graph emitting NaN is refused at
  inference, with every observation unknown; actual runtime tensor shapes/types
  are checked before indexed reads as well as validating load-time declarations.
- Warm full `read()` latency, 38 timed samples: **p50 2.61 ms, p95 3.00 ms,
  maximum 3.05 ms**. This excludes video decode and model loading, and is not a
  Qt/render frame-time benchmark. These are observed worker latencies, not
  universal bounds; other machines must be measured.
- ASan + UBSan + leak detection: same full test suite passed. The distributed
  ORT shared library itself is not sanitizer-instrumented. Sanitizer timings are
  not the optimized performance numbers above.

These results establish **export/preprocessing/native numerical parity**, not
new independently gold-labelled pixel accuracy, model generalization or broad
HUD coverage. PTS mapping, playback, source precedence and asynchronous UI
behavior require separate application-level tests; see
[IMAGE_TELEMETRY_ACCEPTANCE.md](IMAGE_TELEMETRY_ACCEPTANCE.md).
