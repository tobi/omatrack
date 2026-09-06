<!-- origin: PUBLIC — architecture and generated-test evidence only. -->
# Image-derived time traces

`ImageTelemetryTraces` is a native Qt Quick view of the progressively collected
image-derived recording. It belongs **below the video in the normal docked
workspace**, not in a separate live-reading HUD. The controller owns collection,
scan-ahead scheduling, validation and the native `.telemetry` cache; the view only
consumes an immutable snapshot and asks the host to seek.

The four lanes are **Gear**, **Displayed lap**, **Brake fill %**, and
**Throttle fill %**. The horizontal axis is absolute **video presentation time**.
There is no inferred GPS, distance, classified lap table, brake pressure, or
native-session replacement in this component.

## Input and QML interface

The shared contract is `src/inference/ImageTelemetrySeries.h`:

- `ImageTelemetrySnapshot` is `shared_ptr<const ImageTelemetrySeries>`.
- A `cells` entry represents a 200 ms collection interval. `visited` and
  `layoutSupported` do not imply any particular numeric field is known.
- Each field is optional. A known value requires valid actual presentation and
  source PTS, with `sourcePts = presentationPts + timelineOrigin`.
- Actual presentation PTS lies inside its cell, but **need not equal the cell's
  collection target**. The renderer plots the actual PTS, not an ordinal/FPS
  clock, source-clock offset, or a fabricated regular timestamp.
- A published snapshot must never be mutated. The renderer retains its shared
  ownership through scene-graph synchronization and drawing.

The controller interface consumed by the view is exactly:

```cpp
omatrack::inference::ImageTelemetrySnapshot series() const;
void timelineChanged(); // signal
```

Application wiring supplies time and all visual style through registered types.
This fragment belongs inside the application's existing QML scope:

```qml
ImageTelemetryTraces {
    id: imageTraces
    objectName: "imageTelemetryTraces"
    controller: imageTelemetry
    duration: videoPlayer.duration
    position: videoPlayer.position
    backgroundColor: Style.traceBackgroundColor
    gridColor: Style.borderColor
    foregroundColor: Style.foregroundColor
    mutedColor: Style.mutedTextColor
    cursorColor: Style.foregroundColor
    gearColor: Style.orangeColor
    lapColor: Style.accentColor
    brakeColor: Style.brakeTelemetryColor
    throttleColor: Style.throttleTelemetryColor
    font: Qt.font({family: Style.monoFontFamily, pixelSize: Style.smallFontSize})
    onSeekRequested: seconds => videoPlayer.seek(seconds)
}
```

The containing layout controls visibility and sizing. The component supplies no
independent palette, platform-color fallback, or font-family literal.

| Property / method | Meaning |
| --- | --- |
| `controller` | Typed progressive-series owner |
| `duration` | Full video duration in seconds, including unvisited time |
| `position` | Current video playhead in seconds; does not alter the viewport |
| `viewStart`, `viewEnd` | Transient viewport bounds in absolute seconds |
| `setView(start, end)` | Sets a bounded viewport without moving the playhead |
| `resetView()` | Shows the full recording duration |
| `seekRequested(seconds)` | Click intent; the host owns actual seeking |

Left click seeks. Middle drag pans. Vertical wheel zooms around the pointer;
horizontal trackpad movement pans. Double click resets the viewport. These
interactions never parse, decode, run a model, write a cache, or change recording
metadata.

## Evidence and gap semantics

- **Unvisited** intervals have subtle background shading.
- **Visited but unknown** fields use a separate muted dash treatment. A cell may
  be known in one lane and unknown in another.
- Unknown readouts are `—`, never zero. A genuine observed zero remains zero.
- Gear and displayed-counter paths use previous-value steps at their actual PTS.
  Fills interpolate only between adjacent known observations.
- The first observation after a gap begins at its actual PTS: no backward
  extrapolation into the unobserved portion of that cell.
- A final known cell has a bounded previous-value drawing tail that stops before
  the next cell. No interpolation or hold passes through an unknown/unvisited
  cell. This tail is a view projection, **not another recorded sample**.
- Isolated observations remain small coverage-stroke marks at their actual PTS,
  even when there is insufficient neighbouring evidence for a continuous trace.

A narrow missing interval must remain a break even when smaller than one device
pixel. At dense overview scales, a column containing unknown evidence is a
conservative pen-up. Real observed extrema in that column can still appear as
isolated marks. Neither averaging unknown into zero nor drawing a continuous
line through a gap is permitted.

## Rendering and cache boundaries

The implementation is a `QQuickItem` using the existing `TraceSceneBuilder`,
`TraceTextCache`, and `TraceDecimator` conventions: joined coverage strokes,
source-ordered min/max selection, cached text textures and 32-bit indexed
geometry. There is no Canvas, painted item, web renderer, or image screenshot
used as a trace.

Two independent scene-graph subtrees separate costs:

1. **Static:** labels, scales, time ticks, coverage masks, trace paths and fills.
   Rebuilt only for a different immutable snapshot, viewport, size, style or DPR.
2. **Cursor:** the playhead, four readouts and markers. Position changes update
   only this subtree. Readout lookup inspects one cell and an immediate neighbour
   in O(1); it does not call `visitedCount()`, `complete()`, or scan a channel.

Each subtree has its own builder/text cache. Their owner is a scene-graph node,
so GPU textures are released on the render thread rather than by a GUI-thread
item destructor. A cached node retains the exact immutable snapshot its cursor
uses; it never keeps raw pointers into a worker-mutated vector.

`ImageTelemetryTraceGeometry` is the view-only projection helper. The selected
cell range comes from the shared O(1) `slotRange()` plus clipping neighbours.
For sparse/zoomed inputs it preserves explicit anchors and categorical step
corners. Dense overview inputs are reduced into device-pixel columns **before**
passing through the shared decimator. This bounds temporary projected arrays,
coverage-mask rectangles and isolated markers by the pixel budget, rather than
allocating a full duplicate channel or emitting one rectangle per tiny gap.
Extrema remain in source order at actual presentation positions.

Cold projection still examines the relevant source cells to preserve peaks.
That cost occurs on snapshot/viewport changes, **not cursor movement**. The
renderer never serializes its projection, assigns source identity, or competes
with the cache format.

## Verification

`tests/ImageTelemetryTraceGeometryTest.cpp` uses generated mathematical samples
only. It covers:

- actual PTS instead of collection-target time, including signed source origins;
- unvisited versus visited-unknown cells and independent field masks;
- discrete steps versus continuous fill interpolation;
- exact recording-end clipping, bounded cell tails and invalid timestamp evidence;
- rejected nonfinite/out-of-domain values;
- min/max spike preservation, singleton marks and output pixel budgets;
- dense overview masks, chronological extrema and actual-time peak positions;
- narrow viewport work independent of the full recording length;
- constant-time cursor lookups and cold-path synthetic benchmarks.

The CMake target `image-telemetry-trace-test` links the helper and
existing drawing helpers against Qt Core/Gui/Quick/Test. Run:

```sh
ctest --test-dir build-acceptance -R image-telemetry-trace-test --output-on-failure
```

A disjoint development build passed all nine QtTest entries (including setup
and cleanup) and compiled the complete renderer and its generated meta-object
code. A deliberately adversarial **generated** 24-hour / 432,000-cell benchmark
measured approximately 5.7 ms for cold projection and 0.6 ms for four viewport
paths after pixel-column aggregation; the earlier full temporary-array approach
cost about 39 ms and was replaced. An alternating-known/missing generated input
cost approximately 5.2 ms for projection plus isolated-marker geometry. Helper
cursor lookups were constant-time, in the tens-of-nanoseconds range.

These are CPU helper measurements, not a claim about end-to-end frame time,
GPU upload, a laptop, or a compositor's refresh rate. Integrated native OpenGL
acceptance also verified actual partial/full docked traces, click-to-seek, and
120 completed cursor frames with no static rebuild. Header/footer bands use
unambiguous names rather than the inherited `QQuickItem::Top`/`Bottom` enum names;
text uses the shared builder's ordinary rectangle alignment. C++ instrumentation
is available without adding product UI:

```cpp
quint64 staticBuildCount() const;
quint64 cursorBuildCount() const;
double lastStaticBuildMs() const;
double lastCursorBuildMs() const;
```

For an interaction check, freeze an immutable snapshot, move only `position`
through normal frame callbacks, and require `staticBuildCount()` to stay fixed
while `cursorBuildCount()` advances. A subsequent new snapshot must invalidate
static geometry exactly once. Verify source clearing, zoom/pan, click-to-seek,
and unknown gaps without inferring extra samples or laps.

No recording paths, footage, model weights, private readings, or private
screenshots are included in this public document or its generated unit tests.
