<!-- origin: PUBLIC — test contracts only, no private source material. -->
# Progressive image telemetry acceptance

This suite verifies **collected, persistent time-series telemetry**, not a
standalone live-value HUD. It uses real decoded frames and the reviewed model,
then reopens the resulting standard `.telemetry` recording. It never injects
predicted values, GPS, distance, or classified laps.

Implementation: `ImageTelemetryScanAutotest.h/.cpp`. Compile it only in an
acceptance build. Application bootstrap invokes it ahead of other harnesses:

```cpp
if (!omatrack::autotest::installImageTelemetryScan(engine, *store))
    omatrack::autotest::install(engine, *store);
```

The entry point returns false when `OMATRACK_AUTOTEST_IMAGE_SCAN` is absent.
Do not run the obsolete live-HUD harness alongside it: that harness's old
expectation that seeking erases all observations is deliberately wrong for a
progressively collected recording.

## Inputs and modes

All source/model paths come from the caller. Source video and model files stay
immutable. Every run must use fresh scratch `XDG_CONFIG_HOME` and
`XDG_CACHE_HOME`; the suite reads/writes no developer configuration. No fixed
recording name, driver, track, host path, or private sample is embedded in code.

| Environment | Meaning |
| --- | --- |
| `OMATRACK_AUTOTEST_IMAGE_SCAN` | `supported` (or `1`), `native`, `blank`, or `metadata-veto` |
| `OMATRACK_VIDEO` | Actual source selected for that mode |
| `OMATRACK_AUTOTEST_IMAGE_BLANK` | Distinct no-HUD video for source-switch/reopen tests; required for `supported` |
| `OMATRACK_AUTOTEST` | Absolute private screenshot destination in an ignored build directory |
| `XDG_CONFIG_HOME`, `XDG_CACHE_HOME` | Fresh isolated application state |

Scratch preferences must enable image telemetry and select the reviewed model,
or use the correctly staged default model. The suite requires native Qt Quick
OpenGL and real image-runtime availability. It does not accept the offscreen
software scene graph as video/trace-rendering evidence.

Supported input should be a **30–180 second** video-only clip with the reviewed
HUD. This is long enough to interrupt scanning before completion while keeping
full-coverage acceptance bounded. A metadata-free stream-copy remux is suitable;
never strip metadata from or rewrite the original recording. Negative modes use
a genuine native-telemetry recording, a generated blank video, or a playable
video with an actual metadata/DATA track respectively.

Run through the project's isolated-display wrapper, never bare on the desktop.
The wrapper must provide real GL and fresh scratch state without taking focus.
Keep images, cache files, logs and any private runner scripts in ignored build
locations. This public document intentionally contains no private artifact links.

## Supported sequence

1. **Watch collection.** Play until several cells have actually been visited and
   model inference has produced known values. Leave fullscreen through the real
   enabled Escape shortcut handler and require the time traces in the normal
   docked workspace.
2. **Cursor priority without losing history.** Pause and seek into the middle of
   the source. Current readouts must invalidate immediately, but every previously
   visited cell and known observation must remain. The first new coverage
   publication must include the requested cursor cell.
3. **Pause a real partial scan.** Enable scan-ahead while paused. Stop around one
   third coverage and wait for the current bounded batch to settle. The fixture
   must remain incomplete; completing too early is a failed precondition, not a
   successful partial-cache test.
4. **Source change and partial-cache reuse.** Switch to the distinct blank video,
   then reopen the original. Source retirement must persist legitimate partial
   work. Reopened coverage must retain every observed value, mask and actual
   timestamp and initially require zero model calls. No original reading may
   appear under the blank source.
5. **Wrap and backfill.** Seek to an unvisited cell near the end, verify cursor
   priority again, then resume scan-ahead. Completion must include the earlier
   gaps, not only the suffix after the cursor. Both `complete` and
   `cacheComplete` must become true.
6. **Standard recording roundtrip.** A worker opens the actual cache through
   `TelemetrySource::open`, not a fake source or the worker draft. It verifies all
   eleven channels, cell counts, 5 Hz lattice, per-cell source-clock lookup,
   values, known/visited/layout masks and actual presentation PTS. Source lap
   metadata must remain empty. The independently validated cache loader must
   reconstruct identical immutable evidence, including the exact source clock.
7. **Complete-cache reuse.** Switch away and reopen again. The complete cache
   must load without re-extraction (`inferenceRuns == 0`) and remain at zero
   model calls while playback advances. Record end-to-end reopen-to-cache-hit
   latency, including media open and identity validation.
8. **Actual trace interaction and rendering.** Freeze the real completed
   immutable snapshot in `ImageTelemetryTraces`, move only its playhead through
   120 completed Qt Quick frame callbacks, and require static-build count to
   remain unchanged while cursor builds advance. Record cursor/static geometry
   cost. A viewport change must cause exactly one static rebuild. A real mouse
   event delivered to the view must emit a time seek and reach that position in
   the actual player without re-extracting a complete cache.
9. **Visual guard.** The final docked image must contain real video and colored
   native trace geometry, not just application chrome. Screenshot inspection is
   still required for legibility, lane semantics and gap behavior.

The test never substitutes its own channel values. Freezing a shared pointer for
renderer measurement changes only which immutable snapshot the view retains;
it does not mutate the controller, cache or source. The original controller
binding is restored before the click-to-seek check.

## Negative cases

### Native telemetry

A real native session/unified lap must retain precedence. The image controller
must be ineligible, have no known image samples or model calls, and keep current
image values unknown. After a hold period, a worker checks the fresh scratch
cache tree contains **no inferred `.telemetry` file**.

### Metadata veto

The source must actually take the standalone path while carrying a media DATA
track. The suite requires the explicit metadata-veto status, zero known fields,
zero model calls and no inferred cache file. Parser failure alone is not proof
that native telemetry is absent.

### No HUD

A truly blank/no-HUD source may legitimately cache **visited unknown coverage**
so the application does not rescan the same unsupported frames forever. Complete
coverage and persistence are required, with zero known fields and zero model
calls. Its standard `.telemetry` roundtrip must preserve null values rather than
turning them into zero. This is different from the native/metadata-veto case,
where no inferred cache is allowed.

## Timing and failure semantics

The suite is finite: a 50 ms state timer, a 60-second per-phase deadline, and a
180-second total deadline. There are no nested event loops, sleeps or direct
calls to private inference/publication functions. Disk reads, standard parser
roundtrips and cache-tree checks use `AsyncJob` workers.

Coverage monotonicity is checked across actual publications and reopens. A known
value cannot be erased; visited cells cannot become unvisited; existing actual
PTS cannot be silently replaced. Legitimate additional work completed before a
source-change cancellation may increase coverage and is retained rather than
rejected merely for being newer than the last GUI snapshot.

Scan throughput is reported as **new visited coverage-seconds per wall-second**,
with separate bounded scan segments. It includes the observed batch settling
and final persistence where applicable. It is not a nominal model FPS claim and
does not treat repeated observations of one frame as additional coverage.
End-to-end cache-hit timing is labeled separately. Renderer timing is CPU
geometry time over real frame submissions, not hardware scanout or a laptop
performance guarantee.

Failures print phase, aggregate state and cache-write status/path, return nonzero,
and retain private failure/partial screenshots. Cache paths are diagnostic output
in ignored private logs, not public documentation. Retention diagnostics count
lost cells, lost/changed known fields and changed timestamps without printing
individual values. The suite does not log individual inferred values,
raw source timestamps, credentials, or source/model contents. Existing scaler
regression guards remain relevant; see
[VIDEO_SCALER_COMPATIBILITY.md](VIDEO_SCALER_COMPATIBILITY.md).

## Verification status

**All four integrated modes passed** after the exact-parser fix: supported,
native, blank and metadata veto. The supported run verified watch collection,
cursor priority, retained partial coverage, interruption/reopen reuse, full
wrap/backfill and persistence, the independent standard-reader roundtrip, and
complete-cache playback with **zero re-extraction**. Native/metadata-veto modes
created no inferred cache; blank footage cached visited-unknown coverage without
model calls. The integrated non-lint CTest suite passed **34/34** entries.

Observed aggregate measurements on the tested native Linux/OpenGL setup:

| Measurement | Observed |
| --- | ---: |
| Scan-ahead published-coverage throughput | approximately 6.3 coverage-seconds / wall-second |
| End-to-end partial-cache reopen | approximately 0.65–0.70 s |
| End-to-end complete-cache reopen | approximately 0.54–0.70 s |
| Independent 11-channel standard-reader roundtrip | approximately 37 ms |
| Cursor geometry over 120 completed frame callbacks | approximately 0.08 ms median, 0.11 ms p95 |
| Static trace rebuilds during cursor sweep | zero |

These are aggregate test observations, not source readings or guaranteed rates.
They include the stated scheduling/persistence costs and do not establish
hardware scanout FPS, target-laptop performance, or audible playback. Actual
partial and complete docked trace images were inspected privately.

The initial failure remains a useful regression history: newer published
coverage was not persisted because the MTJ JSON reader could change f64 bits on
reload. A generated-data reproducer isolated nontrivial scaled float32 and f64
cases; the strict merge correctly rejected conflicts rather than overwriting
known data. Enabling `serde_json/float_roundtrip` resolved it. Repeated saves and
the real interruption/reopen flow now pass with **unchanged strict equality**—
no tolerance widening, model change, or schema migration was used.

See [IMAGE_TELEMETRY_TRACES.md](IMAGE_TELEMETRY_TRACES.md) for renderer contracts
and generated unit tests. Private footage, input readings, machine paths,
worker identifiers and screenshots do not belong in this public document.
