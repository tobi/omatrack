# Progressive video telemetry

Omatrack collects image-derived telemetry as **data**, not as a screenshot/demo
panel. It keeps native telemetry first and uses the image reader for supported
local videos without a telemetry/data track.

## Watching and scanning

- Opening a new video starts fullscreen; Escape returns to the workspace and F
  restores fullscreen. Changing the cursor or lap does not continuously force it.
- **Extract telemetry** enables image-derived collection while watching. Values
  are collected on a 5 Hz video-time grid. Already collected coverage survives
  seeks; current readouts never borrow values from another source/model.
- **Scan from cursor** fills faster than playback, including while paused. The
  current cursor has priority. The scan proceeds forward, then wraps to backfill
  earlier holes so a recording opened in the middle can eventually be complete.
  **Pause scan** stops ahead work without throwing away collected data.
- Leave fullscreen to see the four image-derived **time traces** beneath the
  video. They use actual decoded presentation times, not invented distance/GPS.
  Click a trace to seek; wheel zoom, middle drag pans, double-click resets.
- Progress and cache state live in normal playback controls. There is no separate
  inferred-number HUD floating over the footage. The existing native telemetry HUD
  remains unchanged when genuine native telemetry is available.

## `.telemetry` caching

Partial progress is saved automatically in the application's private cache under
`image-telemetry/v1/`. Reopening the original recording loads matching coverage;
fully scanned recordings use their completed cache without running inference
again. Source file identity, model content and sampling/layout/schema revisions
bind the cache. A changed source/model cannot silently reuse mismatched data.

The file is standard **zstd-compressed MTJ JSONL `.telemetry`**, not a new sidecar
format. It contains:

- Four explicitly image-derived prediction channels.
- Independent visited, supported-layout and per-field known masks.
- Actual decoded presentation PTS and exact source-origin provenance.
- Source/model identity, timing/regularization policy and completion state in
  standard header pass parameters.

MTJ sample values sit on a 200 ms lattice. The separate actual-PTS channel preserves
which decoded image supplied each observation. A future frame beyond a grid cell
does not fill that earlier cell. Unknown/unreadable/unsupported values are null,
not zero. **Scanned** does not mean every field is readable or that predictions
are ground truth. Transient decode failures do not establish complete coverage.

Cache writes are atomic and locked. Concurrent instances merge nonconflicting
coverage; a late partial result cannot overwrite completed/known data. Nothing is
written beside, embedded into, or substituted for the original source recording.
A cache-write failure remains visible rather than being called saved.

## Current reader scope

The reviewed reader recognizes one **1920×1080 orange AiM layout** through
conservative image-structure checks. It reads gear, the displayed stint counter,
brake **visible fill** and throttle **visible fill**. This is not a general gauge
detector, physical brake-pressure measurement or authoritative lap classifier.
Different geometry/layouts can remain unknown even when other gauges are visible.

Native telemetry takes precedence. Any data/`aimd` track conservatively vetoes
fallback, even when the native parser could not use it; parser failure is not proof
of absent telemetry. Remote inference is withheld until a local/downloaded video
is available. No video/crops are sent to an external inference service.

See [reader contracts and model setup](GAUGE_READER_RUNTIME.md). Model weights and
private footage are not included in this repository or covered by its MIT license.

## Build and run

Install the normal Qt/libmpv development dependencies, an explicit ONNX Runtime
C/C++ SDK, FFmpeg development libraries and zstd development support. The Python
ONNX wheel alone is not the C++ SDK. Export a trusted compatible model using the
[documented recipe](GAUGE_READER_RUNTIME.md), then configure:

```sh
cmake --preset release \
  -DONNXRUNTIME_ROOT=/path/to/onnxruntime-sdk \
  -DOMATRACK_GAUGE_MODEL=/path/to/gauge-reader.onnx
cmake --build --preset release
./build/omatrack --new-instance /path/to/video.mp4
```

`OMATRACK_GAUGE_MODEL` is an explicit **private staging** choice: it verifies the
known export hash and places the model in `models/gauge-reader.onnx` beside the
executable. It neither downloads nor publishes weights. Alternatively select a
trusted local model with **Model…** in the playback controls. Configuration uses
the existing single `omatrack.yml` document:

```yaml
video:
  image_telemetry: true
  image_model: /absolute/path/to/gauge-reader.onnx
```

An empty `image_model` uses the staged model. The source/model identities must be
readable to validate cache reuse, but a complete cache does not construct the
reader or decode the video again for extraction. The video player independently
decodes for playback. Missing or incompatible models fail explicitly rather than
inventing values. Without the optional runtime, ordinary playback/native telemetry
remain available.

The [isolated Linux development helper](VIDEO_BUILD_ENVIRONMENT.md) is optional;
other platforms need their matching SDK and normal build dependencies.
[File associations](VIDEO_ASSOCIATIONS.md) are optional, not an OS-default takeover.

## Verification and limitations

The cache tests round-trip through the real Rust telemetry parser and cover
partial/resume/complete, source/model changes, exact PTS reconstruction,
concurrent union, corruption, cancellation and source isolation. The scheduler
tests cursor priority and wrap/backfill. Native trace tests cover known/unvisited
gaps, step-valued counters, actual-time placement and pixel-bounded geometry.
Real-video acceptance must also exercise scan throughput, persistence/reopen with
zero new model runs, cursor responsiveness and visible rendering.

These are model predictions, not newly admitted golden telemetry. There are no
invented GPS coordinates, physical pressure, distance or verified lap boundaries.
The original recording's telemetry/analysis model is never overwritten.

Playback uses [builtin bilinear scaling](VIDEO_SCALER_COMPATIBILITY.md) to avoid a
causally reproduced upstream scaler-LUT padding bug. This display policy does not
alter the original-resolution image-reader input. Hardware/OS-specific behavior
and target-laptop latency need their own measurements; small weights alone do not
establish a frame-rate guarantee.
