# Discover → Confirm → Extract

## Native workflow

Enabling **Discover gauges** starts discovery, not reading or cache scanning.
Playback continues. A serial `AsyncJob` decodes a full source frame approximately
every three seconds; observations retain the decoder's actual presentation PTS.
Temporal evidence requires at least two seconds separation from **every** earlier
sample, including frames revisited after seeking. Repeated paused frames do not
increase evidence. Repeated spatial/type matches increase the displayed seen
count; misses/conflicts lower it. This is not a calibrated probability.

The source-normalized boxes cover the whole displayed video, not a HUD subregion.
Display letterboxing is removed before projecting boxes, using the actual player
viewport (including native filmstrip reservation), not the whole stage. Source/display
aspect mismatches withhold boxes and extraction instead of using wrong geometry.
Non-identity stream/frame display matrices are rejected by the image decoder,
including 180-degree rotation/mirroring whose aspect ratio alone would not expose
wrong placement. Playback remains available.
The setup retains source pixel dimensions, normalized rectangles, representation,
semantic label, fill direction, selection and confirmation. A different source
size clears validation; it never establishes reader compatibility by scaling.

The setup panel supports selecting/disabling gauges, moving/resizing rectangles,
relabeling semantics, choosing representation and explicitly choosing bar fill
direction. Unknown representations, semantics and directions remain visible.
Automatic unedited/unconfirmed tracks retire after three independent misses;
user-edited, visually confirmed and saved proposals remain with a stale warning.
Editors and list anchors use stable track identities rather than indices.

Three consistent observations allow **Confirm setup**. A user can instead
explicitly confirm a box and label visually; this is distinct from model evidence.
Confirmation always saves the per-file setup. **Remember for this extension**
defaults ON and also saves an extension-level proposal; it can be unchecked for
this confirmation. Neither action runs **any reader inference**. **Start extraction** is a separate action.
Only selected, confirmed, compatible fields are read at the existing 5 Hz lattice.
**Scan from cursor** becomes available only after that action. Editing or changing
the source/model cancels jobs and discards setup-dependent current snapshots.
Seeking cancels stale results but preserves legitimate same-setup coverage.

Discovery can inspect a local video containing native telemetry. Image extraction
cannot replace that telemetry: standalone eligibility and the independent media
metadata-track veto still gate reads. Remote discovery/extraction require a local
or explicitly downloaded file.

## Reader compatibility: important current boundary

The bundled incumbent is the immutable reviewed CountReader described in
[GAUGE_READER_RUNTIME.md](GAUGE_READER_RUNTIME.md). Its original `read()` route is
unchanged. `readConfigured()` is a separate experimental API with half-open source
pixel crops, exact source geometry, selected masks and explicit directions. It
uses the same metadata/tensor checks, Pillow-compatible crop bytes and count-CTC
policy. Disabled crops are neither preprocessed nor decoded; placeholder batch
positions never become observations.

This API is **not arbitrary-reader approval**. The app's activation policy still
requires exact reviewed orange-AiM crops/types/directions and per-frame structural
admission. Unsupported edits remain saved but unreadable. `readerConfiguration(false)`
exists for separate controlled evaluation, not normal UI activation. The incumbent
is sensitive to crop margins: independent approved-validation diagnostics found
large regressions from even two-pixel shifts and 10% expansions. Do not feed tight
detector glyph boxes directly to this reader. Robust reader promotion or an
independently validated detector-to-reader crop-refinement policy is still needed.

Native private parity tests relocate all four approved crops to a different source
geometry and exercise all four fill directions. Their prepared bytes equal the
original Pillow oracle and their predictions match. This proves the configured
API's geometry and masks, not robustness to changed crop contents or fonts.

## Backends

Default discovery tries the **EXPERIMENTAL V2 detector** only when locally staged
at `models/experimental-detector/gauge-detector.onnx` beside the executable, with
`metadata.json` and `contract.json`. Auto-proposal additionally pins model SHA256
`99bdd483402444e8c799452d9885c1184980a41f2cd80d4af4d7a6b3ae00abc6`.
Missing, rejected or failed detectors use the separately labeled **Heuristic
fallback (fixed reviewed layout)**, whose image-only structural admission proposes
exact reviewed crops. It is never represented as learned detection.

Preferences → Image telemetry can select a trusted local export. In `omatrack.yml`,
`video.gauge_detector` is empty for staged/fallback discovery, `heuristic` for the
explicit reviewed heuristic, or an ONNX path with its companion files. There are
no detector downloads. The detector remains experimental even after confirmation;
new proposals start unselected and never imply reader compatibility.
`GaugeDetectorArtifact` checks bounded file sizes, schema, exact frozen contract
SHA256, input/output/decode/limitations metadata, model SHA256/size/name, and
rechecks artifacts across load. Native ORT validates actual names/shapes/types and
finite outputs. Source setup identity includes the loaded model and metadata hashes.
An error explicitly labels the heuristic fallback rather than claiming detection.

The current candidate ABI is `gauge-detector-center-v1`, RGB float32 NCHW
`[1,3,384,640]`, half-pixel bilinear full-source stretch without antialias or uint8
rounding, divided by 255. Five stride-four heads feed bounded local-max/top-100/NMS
postprocessing (max 32). Representations are digits/bar/wheel/needle; semantics may
be unknown. No fill direction, physical unit or readability is predicted.

**V1 failed independent localization acceptance and is no longer admitted.** The
current locally staged or explicitly selected candidate is V2, scope
`pilot-v2-mil-strong4-backgrounds`, contract SHA256
`87a4cec9e0466250b635e561c74be81f2ce5f15fbd79804a1be639c3719a69d3`.
Its small independent eight-frame audit improved localization, but misses and
false positives remain, including incorrect supported-channel labels. It is **not
trusted detection** (local staging only enables experimental proposals). All
proposals start unselected, the UI labels it experimental, and none bypass reader
compatibility. Native/Python parity is a
separate numerical test, not accuracy certification. V2's changed training scope
was deliberately reviewed/pinned; unknown metadata is not silently admitted.

## Persistence and offline operation

`PreferencesStore` owns `video.gauge_files` and `video.gauge_defaults` in the
existing debounced `omatrack.yml` writer. Defaults and per-file setups reopen as
unconfirmed proposals requiring fresh source-frame validation, including when a
file is replaced at the same path. No source file or adjacent sidecar is written.

The standard predicted `.telemetry` cache carries `setup_sha256` in pass parameters.
The hash includes confirmed geometry, semantic/type/direction/masks, source geometry
and detector identity; the existing reader-content hash and source file identity
remain independent key components. Deserialization validates the setup hash too:
copying a valid cache under a different setup's filename is rejected. Normalized
coordinates are canonicalized at the preferences writer's ten-significant-digit
precision before hashing; YAML round trips must not silently change the cache
identity. This is subpixel serialization, not a relaxed crop-admission tolerance.

The independent auto profile-normalized reader study admitted only 127/662 frames
(19%); full joint-five success was 16.9%. Neither it nor the robust-reader candidate
approves arbitrary or detector-selected crops. Larger detectors need separate
quality/latency and contract review before promotion.

Release build recipes stage the pinned public ~2.2 MB reader via the existing
immutable-commit download and SHA256 check **at build time**. Source/private builds
can use `OMATRACK_GAUGE_MODEL`. Runtime model downloads remain optional opt-in updates;
a packaged reader needs no first-run network. This change does not publish a release
or bundle experimental detector weights.

## Verification

- `gauge-setup-test`: independent PTS, seek revisits, geometry mismatch, proposal
  revalidation, stable identity, stale-track retirement, selected masks and unknowns.
- `gauge-reader-parity-test` (private fixtures): original numerical parity plus
  relocated configured crops, four directions, exact bytes and disabled fields.
- `gauge-detector-test`: synthetic resize/decode/loader/finite guards; optional private
  `--fixtures DIR --model FILE --artifacts DIR` validates real native parity.
- `gauge-detector-artifact-test [model.onnx]`: missing/cancelled/tampered artifact
  rejection and optional real candidate metadata/load validation.
- `video_decoder_transform_test.py`: generated 90°, 180°, horizontal-mirror and
  vertical-mirror metadata transforms are explicitly rejected while untransformed
  decoding remains available.
- `image-telemetry-cache-test`: includes setup-key separation and deserialization
  rejection when valid bytes are copied to another setup's cache key.
- `OMATRACK_AUTOTEST_GAUGE_DISCOVERY=supported` with `OMATRACK_VIDEO`, a distinct
  `OMATRACK_AUTOTEST_IMAGE_BLANK`, scratch XDG config/cache and `OMATRACK_AUTOTEST`
  verifies paused-frame independence, discovery during playback, both explicit gates,
  offline reads, disabled fields, setup identity, source/seek cancellation and PNGs.
  `native` mode verifies discovery with native-priority extraction withheld.
  `detector` mode with no detector override verifies auto-selection of staged V2,
  all proposals unselected and the persistent EXPERIMENTAL warning. The harness
  checks that extension remembering defaults ON and per-file/defaults are saved.
  Supported/native/scan acceptance explicitly selects the heuristic to exercise
  the incumbent reader's exact approved crops, not detected glyph boxes.

Run GL checks through `scripts/autotest.sh` (or isolated
`scripts/setup-video-dev.sh xvfb`). The finite supported/native/experimental discovery GL checks and the updated
progressive scan/reopen/roundtrip/trace harness passed, including the metadata veto.
The scan harness exposed and now covers same-sized paused media reopen: mpv may not
re-send unchanged `dw`/`dh`, so `MpvVideoItem` asynchronously queries the coherent
`video-out-params` pair on load/reconfiguration after clearing its cached geometry.
Final integration verification passed all 45 CTests (including QML/Rust lint),
private native V2 parity/loader guards, and supported/native/staged-V2 discovery,
blank/metadata-veto, progressive scan and cold-process-restart GL checks. The
cold restart first exposed the YAML hash bug above; its regression now verifies
revalidation and complete-cache reuse with zero reader calls. Offline install
contains the hash-verified public reader, never the experimental detector.
Observed progressive throughput was about 6.27 coverage-seconds/wall-second,
complete-cache reopen 399 ms with zero reader calls, and cursor geometry p95
0.115 ms over 120 real frame callbacks with no static rebuilds. These are local
measurements; llvmpipe establishes functional rendering, **not** high-refresh
hardware performance.
Coordinate all GPU-visible GUI runs with same-device training workers. Keep private
frames, caches, model artifacts and screenshots under ignored build/work paths.
