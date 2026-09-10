# Trace rendering: quality without a second graphics stack

## Decision

Use **Qt Quick's public scene-graph API, joined coverage meshes, and
premultiplied vertex gradients**. Keep `QSGVertexColorMaterial` and the existing
single geometry batch per surface. Do not introduce Skia, a painted item, or a
QML object for every telemetry sample.

This is vertex antialiasing, not a new curve-fitting algorithm. Telemetry is a
piecewise-linear signal; decorative Bézier smoothing would invent peaks and
move braking events. The rendering must improve without changing the evidence.

### Options researched

| Option | Assessment for this application |
| --- | --- |
| **Qt Quick Shapes `CurveRenderer`** | A good native choice for general vector paths. Qt documents shader-based curve rendering, high-quality AA without MSAA, and no curve re-tessellation just to scale a path. However, our zoom changes the visible sample set and its alignment, not just a transform; a transformed stroke would also change width. Updating Shape path data still requires CPU preprocessing. Its public interface is QML; using the renderer's private C++ classes would couple us to Qt internals. Not selected for the dense telemetry path. |
| **Qt scene-graph coverage mesh** | Selected. Public APIs, direct normalized-array access, viewport-bounded geometry, screen-space stroke width, shared joins, one batch, no intermediate texture. Qt itself documents vertex AA as a native scene-graph technique. |
| **Skia GPU** | Capable path renderer, but adds a substantial build/deployment dependency and GPU/texture integration alongside Qt and libmpv. It does not fix incorrect decimation. Not justified for straight-line telemetry and simple fills. |
| **QPainter / Canvas / supersampled texture** | Avoid. Re-rasterization and texture uploads on every zoom, more memory, and a raster that can blur or fatten when scaled. Prior Omatrack QPainter measurements already exceeded the interaction budget. |

These alternatives were researched, not all implemented and benchmarked. The
measurements below are for the selected implementation, not a claim that it
outperforms every possible CurveRenderer or Skia integration.

Official Qt 6.11 references:

- [Shape: renderer types, asynchronous preprocessing, scaling limitations](https://doc.qt.io/qt-6/qml-qtquick-shapes-shape.html)
- [Scene graph renderer: vertex AA, batching, retention, MSAA](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph-renderer.html)
- [Public custom geometry example](https://doc.qt.io/qt-6/qtquick-scenegraph-customgeometry-example.html)

## What was wrong

1. Every segment joint received a square stamp. This made dense curves visibly
   heavy and caused translucent reference segments to overlap at their joints.
2. The decimator always emitted high then low, even on a monotonic slope, and
   reused boundary samples in adjacent columns. It manufactured sawteeth.
3. Sparse reference points were positioned using an inverse of the **whole
   viewport's endpoints**, although the comparison map can be nonlinear.
4. Edge quality depended on a requested multisampled window format.
5. `interpolateAlignmentFraction()` copied the entire alignment vector for
   **each lookup**. Reference rendering did thousands of these per redraw.
6. The geometry benchmark used DPR 1 even when the actual surface used DPR 2;
   its old timing and quad counts understated the native workload.

## Rendering contract

- `TraceDecimator` keeps source-ordered min/max samples at sub-column positions.
  Sparse regions use actual samples, not one interpolated joint per pixel.
- Local inversion of the shared forward reference map is bounded to one device
  column. There is no independent reference alignment model.
- Non-finite samples break the path, including a gap inside a dense column.
- A linear-time slope corridor removes only subpixel detail: at most **0.1
  physical pixel of vertical error** in its input path. Source arrays are never
  filtered or changed. This bound does not claim to reconstruct all oscillations
  within a decimated device column.
- Stroke width is measured in **logical screen pixels**, not lap coordinates.
  Four shared vertices per joint form the core and a one-physical-pixel AA
  fringe. Joins are bounded; no separate square stamps or segment-end caps.
- The antialiasing mechanism works without MSAA. The application's existing
  MSAA request remains useful for other geometry.
- Fills use the same path as the outline and interpolate premultiplied color
  toward a transparent zero baseline (clamped to the lane). Delta has a
  red/green diverging fill. No column rectangles or overlapping fill strips.
- Paint order is **primary area → reference outline → primary outline**.
  Even a strong area fill cannot bury the reference. The primary path is built
  once and reused for both passes.
- Buffers grow geometrically and reuse capacity. Keep 32-bit indices: the Qt
  batch renderer's 16-bit merge path must not truncate a large trace surface.
- Cursor movement retains the static traces. Only the small overlay rebuilds.
- Zoom now reaches below one source sample, with a numerical floor of `1e-7`
  lap fraction instead of the old 500× limit. This is inspection, not extra
  measurement resolution.

## Controls and persistence

Every channel row has a **Style…** panel containing its active/reference color
pickers, line width, and area fill. **Reset style** restores those four values
for that channel; it does not change lane height or same-line grouping.

```yaml
channels:
  throttle:
    visible: true
    color: "#ffd400"
    reference_color: "#ff2d2d"
    stroke_width: 1.25
    fill_opacity: 0.28
    height_percent: 30
    combine_with_previous: false
```

Defaults are 1.25 logical px for both roles, no fill for most channels, 28% peak
fill for throttle/brake/clutch, and 20% for delta. The lap being compared is
bright yellow, while the reference lap is bright red. Speed reserves 50% of the
trace area, throttle and brake share a 30% overlaid lane, and gear uses 5%.
Each sample channel has an independent **Same line** checkbox and editable
height stepper.
Consecutive checked channels share the preceding visible sample lane while
retaining independent vertical scales. Changing the height from any channel in
that shared line updates the whole line. Hidden samples, delta without a reference,
and overlay-group boundaries break adjacency. The store resolves lane roots once;
the renderer, displayed percentages, edits and resets consume that same grouping.
The stepper accepts 1–100% of the
visible trace area in 1% increments. Editing it switches off FIT so the chosen
percentage is exact; configurations above 100% remain available by vertical
scrolling. The toolbar **FIT** toggle switches between these modes, persisted as
`trace.fit_channels` in `omatrack.yml` (default true). With manual overflow,
unmodified vertical wheel/trackpad motion scrolls and the right-edge scrollbar
reaches the last lane; Ctrl/Shift+wheel still zooms and horizontal motion still
pans. The ruler and distance axis stay fixed. Brake uses Overlay by default to preserve the pedal overlay.
Speed, pedals, and Gear default to 50%, 30%, and 5% respectively.
Width accepts 0.5–4 px; fill accepts 0–100%. Values are validated in C++, exposed
as typed model roles, and saved through the existing debounced `omatrack.yml`
writer. Raw-channel appearance persists; sidecars retain their existing
host-local settings lifetime. No new configuration file is introduced.

## Trace height editing

The toolbar's **Resize** action enters a dedicated mode like corner editing.
Every sample-lane divider gets a visible grip and the lanes show their current
height. Dragging beyond the next lane's minimum pushes through further
neighbours; a lane can occupy nearly the entire pane. **Save** / Ctrl+S keeps
the new proportions and selects FIT; **Cancel** / Escape restores the previous layout,
including manual mode and scroll position;
**Reset heights** previews the defaults. Channels and the right-click lane menu
also offer entry points.

`TraceLaneSizing.h` fits positive finite weights to the available pixel budget,
with a 20 px minimum (reduced when the pane is too small). There is no 2× weight
cap. Fixed group/span chrome is reserved first; all sample traces, including raw
channels and delta, share the rest. Manual mode uses the exact percentages instead.
Resize temporarily projects these lanes into FIT without changing the saved mode.

Preview weights live in a store draft, not in preferences. Other preference
writes therefore cannot accidentally save an unfinished resize. Save writes
`channels.<key>.weight`, including raw-channel overrides; merely browsing raw
channels does not serialize hundreds of default weights. Height changes have a
separate notification from channel configuration, preserving range and sample
caches. The lane-label chrome uses a typed value list with a count-backed
repeater, keeping its delegates alive throughout the drag.

Run `OMATRACK_AUTOTEST_TRACE_RESIZE=/path/to/copied-recording` through the native
acceptance wrapper. It tests growing past 2×, neighbour borrowing, fit, unchanged
playhead/viewport, draft isolation, Cancel, Reset, and saving a raw lane. Repeat
with the same scratch configuration and
`OMATRACK_AUTOTEST_TRACE_RESIZE_RESTORE=1` to check raw-weight and exact manual-mode
reload persistence. The extended run checks hidden-middle overlay chains, wheel
scroll versus modified-wheel zoom, native scrollbar dragging to the final lane,
manual→Resize→Cancel (including scroll and preference isolation), Save selecting
FIT, the FIT button, and scroll clamping after height or window changes.
The native check also verifies that label delegates survive a drag, that a
raw-channel-ready notification does not cancel it, and that Escape, Ctrl+S,
and switching edit modes preserve the committed layout. All 25 unit-test
executables passed; the tall/raw-lane geometry check averaged 3.1 ms on the
DPR-2 setup below, and the saved raw weight reloaded without the old 2× clamp.

### Scrolling scene layers

Static trace content and cursor/readout content each have a rectangular scene-graph
clip for the visible lane area. Ruler/axis chrome and ruler hover annotations stay
outside that clip. Each layer still uses the same indexed `TraceSceneBuilder`
batch; offscreen lanes are not submitted, and scrolling does not invalidate sample
or range caches. QML lane labels use the same plot bounds and resolved row geometry.
Layout calculation is read-only on the render thread; only GUI-thread layout/input
changes clamp the transient scroll offset.

## PR #7 follow-up verification (2026-09-10)

Copied multi-lap VBO, native Hyprland/OpenGL, DPR 2, isolated configuration:

- Expanded resize/manual acceptance and a second-process restore both passed:
  hidden-middle grouping, exact heights, wheel/scrollbar reachability, manual
  Cancel, Save/FIT selection, preference isolation and scroll clamping.
- Static resize geometry averaged 1.32 ms; manual visible-lane geometry 1.17 ms.
- Single-lap hover averaged 0.0347 ms before / 0.0349 ms after; zoom geometry
  averaged 1.63 / 1.51 ms (worst 1.98 / 2.65 ms), with the same 11,119 quads.
- At 120 Hz, the two-lap 10,000× zoom sweep passed before and after: frame
  callback p50/p95/p99 were 8.05/10.37/13.07 ms before and 8.01/11.03/14.51 ms
  after (310 intervals each). This remains a cadence measurement, not a claim
  of locked 120 fps. Inspected overview, detail, Channels and scrolled-lane captures.
- All 45 configured CTest entries passed, including Qt 6 semantic QML lint
  and C++/QML formatting. This local build omits the optional ORT SDK; packaged
  inference/runtime validation remains the responsibility of cross-platform CI.

## Verification (2026-09-03)

Linux/Hyprland, Qt 6.11.2, Intel Core Ultra X7 358H; copied multi-lap Cosworth
recordings. Native OpenGL, 1280×800 logical window, DPR 2, 2560×1600@120 headless
output. Scratch configuration; source recordings are not modified.

- 24 unit-test executables passed, including ordered extrema, monotonic ramps
  across zoom/DPR, nonlinear alignment, subpixel error bounds, NaN gaps, clipped
  lap boundaries, coverage widths, >65,535 vertices, style validation and
  serialization, and non-owning alignment lookup equivalence.
- Seven-lane active/reference zoom microbenchmark: **5.92 ms average,
  9.50 ms worst**. It includes cold scratch-buffer construction, and now uses
  the actual DPR. Hover overlay: **0.057 ms average**.
- Eight-lane native, frame-paced 10,000× zoom sweep (including delta): **8.29 ms
  median callback interval, 10.09 ms p95, 12.87 ms p99**, 310 measured intervals.
  Warmup and screenshot readbacks are excluded.
- Captures inspected at overview, zoom and sub-sample scales, plus a custom
  0.75 px / 55% fill / different reference color and the channel editor.
  A separate process with `OMATRACK_AUTOTEST_TRACE_RESTORE=1` verified that
  this style was loaded back from the scratch `omatrack.yml`.
- Additional native sweeps: DPR 1 had 8.31 / 9.06 / 10.96 ms p50/p95/p99;
  fractional DPR 1.5 had 8.31 / 8.99 / 12.49 ms. Both captured successfully.
- Native libmpv playback of a reflink-copied AiM MP4 passed keyboard seeks,
  75% volume, ready/loaded checks, and presentation/telemetry synchronization
  (reported error below 1 ms in the non-HUD acceptance run). The HUD image was
  also inspected, including its now smoothly tessellated circular dial.
  The separate HUD **layout** assertion fails identically on the saved
  pre-change binary and this build (same size/position); it is a pre-existing
  acceptance/layout mismatch, not claimed fixed here.

**Limits:** 120 Hz is the design target, not a guarantee for every frame on every
machine. The cold geometry worst case exceeds 8.33 ms, and the callback p95 is
above one refresh interval. Callback timing also includes compositor/GUI
scheduling; it is not a GPU timestamp or a formal flicker metric. Do not turn
these numbers into a claim of locked 120 fps. These results meet the 60 Hz hard
budget and sustain approximately 120 Hz through most of the tested sweep;
full-lap/noisy-channel redraws remain the place to profile next.

The QML build succeeds. `qmllint` still reports pre-existing `FilterChangeProxyModel`
registration/unresolved-model warnings and compiler warnings in the application;
these are not asserted clean by this work. The new color picker has no lint
warnings after using a typed color rather than implicit string concatenation.

### Reproduce

```sh
cmake --preset acceptance
cmake --build --preset acceptance
ctest --test-dir build-acceptance -L unit --output-on-failure

OMATRACK_HEADLESS_MODE=2560x1600@120 \
XDG_CONFIG_HOME="$PWD/build-acceptance/trace-test-config" \
OMATRACK_AUTOTEST="$PWD/build-acceptance/screenshots/traces.png" \
OMATRACK_AUTOTEST_TRACE_RENDERING=/path/to/copied-multi-lap.pds \
  scripts/autotest.sh ./build-acceptance/omatrack --mute /path/to/copied-fixtures
```

The dedicated check writes `_zoom`, `_detail`, `_styled`, and `_channels` images
in addition to the overview. Repeat with the same scratch configuration and
`OMATRACK_AUTOTEST_TRACE_RESTORE=1` to verify the customized style survived exit. For DPR 1 or 1.5, also set
`OMATRACK_HEADLESS_SCALE` and choose the corresponding physical output size.
The wrapper follows the headless output's actual active workspace after hotplug
restoration, without focusing it or switching the developer's workspace.
