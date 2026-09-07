# Progressive video telemetry

Omatrack collects image-derived telemetry as **data**, not as a screenshot/demo
panel. It keeps native telemetry first and uses the image reader for supported
local videos without a telemetry/data track.

## Discover, confirm, extract

Image telemetry and managed model downloads are **off on a fresh install**.
Omatrack 1.8.3 includes the proven reader and both discovery models offline;
opening a video or Preferences does not contact the model host. Enable
**Discover gauges**, review the full-source boxes, **Confirm setup**, then choose
**Start extraction**. Remembering an extension proposal defaults on, but every
new/reopened source requires fresh image validation. Native telemetry wins.

Automatic discovery uses the large detector only on current image-verified
1920×1080 orange-AiM frames, tiny V2 otherwise or on large-model failure. Filename
and extension never choose the model. Independently verified **AiM profile** crops
remain readable alongside unselected experimental proposals; other crops do not
become readable merely because the user confirms them.

**Preferences → Image telemetry** (also available from **Model…**) offers optional
managed reader updates. **Enable & download reader (~2.2 MB)** enables discovery,
not extraction, and consents to downloading the compatible reader from the public
[tobil/omatrack-telemetry-reader](https://huggingface.co/tobil/omatrack-telemetry-reader)
repository. No Hugging Face account or token is needed. Video, crops and telemetry
stay local; only model files and update metadata are downloaded. The preferences
page shows download progress, cancellation, failures and available updates.

**Keep reader up to date** is on by default after opt-in and can be turned off
independently of extraction. Automatic activation waits until no video is open,
including paused videos. A downloaded update can be activated deliberately with
**Apply now** while a video is open; it restarts discovery/confirmation and keeps
prediction caches separated through the model-content identity.

**Choose local model…** remains available without network consent. Selecting a
local file turns managed downloads off before changing the active path, so a
pending download/update cannot replace it. Stopping managed downloads leaves the
current model path available for local use; turning extraction off is a separate
choice.

## Watching and scanning

- Opening a new video starts fullscreen; Escape returns to the workspace and F
  restores fullscreen. Changing the cursor or lap does not continuously force it.
- After discovery and confirmation, **Start extraction** enables image-derived
  collection while watching. Values are collected on a 5 Hz video-time grid. Already collected coverage survives
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
`image-telemetry/v1/`. After fresh setup validation, confirmation and Start,
reopening the original recording loads matching coverage; completed caches need
no further reader inference. Source identity, reader/detector content, canonical
selected/confirmed crops and policy revisions bind the cache. Unselected inventory
churn does not invalidate an identical reading; changed crops/source/models cannot
silently reuse mismatched data.

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
private footage are not included in this source repository. Consult the model
repository's own license and model card before using or redistributing its files;
Omatrack's source license does not grant rights to input footage.

## Build and run

Install the normal Qt/libmpv development dependencies, an explicit ONNX Runtime
C/C++ SDK, FFmpeg development libraries and zstd development support. The Python
ONNX wheel alone is not the C++ SDK. Fetch the immutable public bundle at build
time, then configure:

```sh
bundle=$(scripts/fetch-gauge-bundle.sh)
cmake --preset release \
  -DONNXRUNTIME_ROOT=/path/to/onnxruntime-sdk \
  -DOMATRACK_GAUGE_BUNDLE="$bundle"
cmake --build --preset release
./build/omatrack --new-instance /path/to/video.mp4
```

`OMATRACK_GAUGE_BUNDLE` is a **build-time staging** choice: it verifies all 15
allowlisted model/contract/notice files and installs them under `models/` beside
the executable. Runtime has no download ceremony. The selected weights were
authorized for this release; task-weight licensing remains unspecified and the
bundle includes scoped notices. Alternatively select a trusted local reader
through **Model… → Choose local model…**. Configuration uses
the existing single `omatrack.yml` document. For an explicitly enabled local reader:

```yaml
video:
  image_telemetry: true
  image_model: /absolute/path/to/gauge-reader.onnx
  image_model_managed: false
  image_model_updates: true
```

`image_model_managed` defaults to `false`; no model network requests occur while
it is false. `image_model_updates` defaults to `true` but takes effect only after
managed-download consent. `image_telemetry` defaults to `false`; an existing
explicit value is preserved. The UI writes these preferences through the normal
debounced configuration writer.

An empty `image_model` uses the bundled reader. Empty `gauge_detector` selects
automatic image-based routing; `small` forces tiny and `heuristic` forces the
reviewed heuristic. Source/model identities must be readable to validate cache
reuse. Discovery revalidates the setup, but a complete extraction cache does not
construct the reader or decode additional frames for extraction. The video player independently
decodes for playback. Missing or incompatible models fail explicitly rather than
inventing values. Without the optional runtime, ordinary playback/native telemetry
remain available.

`omatrack --check-model-bundle --require-bundled-runtime` is a read-only production
package diagnostic: it verifies the embedded manifest against installed assets,
reports the actual loaded ONNX Runtime module/version, requires that runtime to
be inside the package, and runs all three models on an explicitly synthetic
fixture. It does not open a GUI, read Omatrack preferences, write files or contact
the network. This is runtime integrity, not an accuracy claim. Source builds may
omit `--require-bundled-runtime` to report/use their external SDK.

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
