# Image telemetry verification

Image telemetry is now a progressively collected recording-time series with a
standard `.telemetry` cache, not a separate inferred-value HUD. Tests for the
previous ephemeral-HUD prototype are not acceptance criteria for persistence:
seeking must preserve already collected coverage.

## Unit and codec checks

Run the normal CMake/CTest suite. Relevant targets include:

- `gauge-reader-test`: image admission, unknown/error handling and optional private
  model/parity fixtures. The fixture-free cases are suitable for public CI.
- `image-scan-scheduler-test`: cursor priority, forward scan, wrap/backfill,
  watch-only behavior and no duplicate work after completion.
- `image-telemetry-cache-test`: standard zstd MTJ roundtrip through the Rust reader,
  partial resume/completion, exact timestamp reconstruction, independent masks,
  source/model invalidation, atomic locked merging, corruption and cancellation.
  It uses generated synthetic fixtures, not private racing data or ONNX Runtime.
- `image-telemetry-trace-test`: actual-time placement, discontinuous coverage,
  categorical step traces, extrema retention and pixel-bounded geometry.
- `video-frame-decoder-test` and `tests/video_decoder_clock_test.py`: native decode
  failure handling, real-codec VFR/nonzero-origin timestamps, seeking and RGB checks.

The generic bridge tests cover ZIP `.telemetry`, plain MTJ and zstd MTJ dispatch;
format/vendor parsing remains in the Rust reader.

## Real application acceptance

Use the separately compiled acceptance harness and a fresh isolated configuration
with a trusted local model. Do not use the developer's actual settings or change
OS associations. Real video/GL checks need an OpenGL-capable display; Qt's offscreen
software scene graph is not proof that video or trace geometry rendered.

The progressive scan harness (`OMATRACK_AUTOTEST_IMAGE_SCAN`) must verify:

1. Watching accumulates observations, and seeking retains source coverage without
   publishing stale current readings.
2. Ahead scanning works while paused, prioritizes the cursor, then wraps/backfills.
3. Partial work survives reopening; full coverage publishes a real `.telemetry`
   whose channels, masks and timestamp data round-trip through the normal parser.
4. Reopening a completed cache performs no new model inference.
5. Native/data-track vetoes preserve source truth. Unsupported/no-gauge input
   remains unknown, not zero or a fabricated completed set of readings.
6. The normal docked time-trace workspace visibly renders real collected data;
   cursor movement does not rebuild static geometry or scan full arrays.
7. Source inputs remain byte-for-byte unchanged, and output stays in the private
   application cache rather than beside the video.

Use caller-supplied local fixtures. Keep screenshots, generated caches, full source
paths and actual telemetry readings under ignored working directories—not in a
public test report or repository. Report measured scan throughput and cache-hit
cost separately from model-only latency; do not extrapolate a server run to a
laptop or high-refresh certification.

## Renderer regression

[The scaler compatibility note](VIDEO_SCALER_COMPATIBILITY.md) documents a proven
upstream mpv padding bug and the application's builtin-bilinear policy. Retain
strict displayed-video checks, including paused frames; a loaded player or passing
numeric assertion alone does not prove that video is visible. Changing display
filters must not change the original-resolution model input.
