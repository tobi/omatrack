# Omatrack 2.0 agent contract

Omatrack 2.0 is the Rust + GPUI rewrite, built on branch `gpui-port` under
[`rust/`](rust/) with **gpui-kit 0.6.6** and its component set
(`gpui_kit::component` = gpui-component 0.6.6). This file is its product and
engineering contract. The Qt 1.x application has been removed; its
contract is kept for design history at
[docs/legacy/AGENTS.qt.md](docs/legacy/AGENTS.qt.md).

Status markers: **[done]** committed on `gpui-port` and gated; **[wip]** being
built now by the port workflow (uncommitted); **[plan]** designed, not started.
Never describe [wip] or [plan] as shipped.

## 1. What Omatrack is

A native, keyboard-first racing-telemetry workstation for drivers and race
engineers doing post-session analysis. It turns heterogeneous logger files
(Pi/Cosworth `.pds`, MoTeC `.ld`, Racelogic `.vbo`, AiM `aimd` in `.mp4`,
native `.telemetry`, MTJ JSONL) into one model of sessions, laps, channels,
tracks, corners and corner complexes, and compares a primary lap with a
reference lap across traces, delta, corner analysis, track map and synchronized
onboard video. It is generic: it works exceptionally well for an LMP2 IMSA team,
but paths, drivers, car numbers, classes and events are configuration, never
code.

## 2. Product principles

1. **Racing concepts first**, vendor details below the product boundary.
   Corners and complexes (named contiguous groups) are both first-class; never
   flatten one into the other or infer a complex from adjacency.
2. **One analytical truth.** Traces, cursor values, delta, corner notes and
   video derive from one 50 Hz lap and one cached alignment map.
3. **A pro, GPUI-native workstation**, not a copy of the QML layout: docks,
   palette, dense tables, keyboard-first; mono numerics, colour only for role
   and comparison, no wizards or decorative panels.
4. **Performance is a product contract** (section 8).
5. **Source truth is immutable.** Telemetry and video are never rewritten,
   renamed, deleted or written beside. Caches, edits and exports are separate.
6. **Track Atlas is authoritative**; local edits are overlays.
7. **Linux + Omarchy first.** Follow the live Omarchy palette; otherwise the
   built-in gpui-component dark theme. No web stack, no second toolkit.
8. **Clean migrations**: no compatibility branches unless asked.
9. **Degrade explicitly**: missing channels, GPS, video or atlas data degrade
   visibly; nothing is fabricated.

## 3. Read before you touch X

The vendored gpui-kit skills are **normative**. Read the files, not this
summary or training data. Never invent a gpui-kit API: verify it in
`~/.cargo/registry/src/*/{gpui-kit,gpui-component,gpui-base}-0.6.6`. Reports
and reviews list the skill files read. The `gpui-kit*` skills are vendored via
`skills-lock.json` and untracked: read, never edit or commit.

Always before GPUI code: [gpui-kit SKILL.md](.agents/skills/gpui-kit/SKILL.md),
then [coding-guides.md](.agents/skills/gpui-kit/references/coding-guides.md)
("Architecture at a glance", "Rules for coding agents", "Common failure modes",
"Implementation checklist"; the whole guide for a new crate or feature).
Before any visible surface, first: [gpui-kit-design-guides SKILL.md](.agents/skills/gpui-kit-design-guides/SKILL.md)
and [design-guides.md](.agents/skills/gpui-kit-design-guides/references/design-guides.md);
finish with its Design review and Accessibility checklists.

| Touching | Read first |
|---|---|
| Any kit component (Dock, Sidebar, Tree, DataTable, Command, TitleBar, StatusBar, Dialog, Sheet) | [conventions.md](.agents/skills/gpui-kit/references/conventions.md), [usage.md](.agents/skills/gpui-kit/references/usage.md), [recipes.md](.agents/skills/gpui-kit/references/recipes.md) |
| Trace lanes, track map, damper strip, lap strip, video surface | [element.md](.agents/skills/gpui-kit/references/gpui/element.md), [element-api.md](.agents/skills/gpui-kit/references/gpui/element-api.md), [element-patterns.md](.agents/skills/gpui-kit/references/gpui/element-patterns.md), [element-best-practices.md](.agents/skills/gpui-kit/references/gpui/element-best-practices.md), [element-advanced.md](.agents/skills/gpui-kit/references/gpui/element-advanced.md), [element-examples.md](.agents/skills/gpui-kit/references/gpui/element-examples.md), [element-id.md](.agents/skills/gpui-kit/references/gpui/element-id.md), [TRACE_RENDERING.md](docs/TRACE_RENDERING.md) |
| App state, entities, subscriptions | [entity.md](.agents/skills/gpui-kit/references/gpui/entity.md), [entity-api.md](.agents/skills/gpui-kit/references/gpui/entity-api.md), [entity-patterns.md](.agents/skills/gpui-kit/references/gpui/entity-patterns.md), [entity-best-practices.md](.agents/skills/gpui-kit/references/gpui/entity-best-practices.md), [entity-advanced.md](.agents/skills/gpui-kit/references/gpui/entity-advanced.md), [event.md](.agents/skills/gpui-kit/references/gpui/event.md), [global.md](.agents/skills/gpui-kit/references/gpui/global.md), [context.md](.agents/skills/gpui-kit/references/gpui/context.md) |
| Actions, keymap, palette, focus | [action.md](.agents/skills/gpui-kit/references/gpui/action.md), [focus-handle.md](.agents/skills/gpui-kit/references/gpui/focus-handle.md) |
| Background loads, scans, mpv events, theme watcher | [async.md](.agents/skills/gpui-kit/references/gpui/async.md) |
| Panel layout, sizing, scrolling | [layout-style.md](.agents/skills/gpui-kit/references/gpui/layout-style.md); design-guides "Layout patterns", "Designing data-heavy interfaces" |
| Theme tokens | coding-guides "Theme and styling"; [usage.md](.agents/skills/gpui-kit/references/usage.md) "Theming"; design-guides "Visual language" |
| Any UI test | [test.md](.agents/skills/gpui-kit/references/gpui/test.md), [test-examples.md](.agents/skills/gpui-kit/references/gpui/test-examples.md), [test-reference.md](.agents/skills/gpui-kit/references/gpui/test-reference.md) |
| Core, CLI, parity | Section 6, [rust/AGENTS.md](rust/AGENTS.md), [rust/parity/run.sh](rust/parity/run.sh) |

**Retired 1.x app.** [docs/legacy/AGENTS.qt.md](docs/legacy/AGENTS.qt.md)
and [.agents/skills/omatrack/SKILL.md](.agents/skills/omatrack/SKILL.md)
describe the retired Qt 1.x app, kept for design history only; nothing in them
applies to `rust/`.

## 4. Architecture

### 4.1 Crate map

[rust/Cargo.toml](rust/Cargo.toml): resolver 3, edition 2024,
`unsafe_code = "deny"`. Dependencies point down the table; no cycles. GPUI-free
crates stay GPUI-free.

| Crate | Status | Owns | Must not own |
|---|---|---|---|
| [omatrack-core](rust/crates/omatrack-core) (no GPUI) | **[done]**, byte parity | Recording open via pinned `motorsport-telemetry-rs` (no C ABI), mapping, laps, 50 Hz `UnifiedLap`, alignment, delta, embedded Track Atlas, corners, playback rules, video clock, `ChannelProvider`, `session` (`load_lap`, `Analysis`) | UI, config, executors |
| [omatrack-cli](rust/crates/omatrack-cli) (no GPUI) | **[done]** | `parse \| unify \| corners \| compare`, the headless command surface | A second analysis |
| [omatrack-library](rust/crates/omatrack-library) (no GPUI) | **[done]** | Paths, `omatrack.yml`, `TRACK.yml`, metadata precedence, `Location`, index cache, catalog, recents | Analysis, rendering |
| [omatrack-trace](rust/crates/omatrack-trace) | **[done]** decimate, scales, layout, mesh, lanes, overlay, `TraceStack`, `trace_bench`, corner ruler, track map (slope and heat modes), damper strip | Trace math and trace/map/damper elements | Session state, parsing |
| [mpv-player](rust/crates/mpv-player) | **[done]** | libmpv 2.5 player + `VideoView` (section 9) | Any Omatrack type |
| [omatrack-ui](rust/crates/omatrack-ui) | **[done]** theme + bundled Inter / Geist Mono, type scale (`TypeScale`, tabular figures), `RoleChip`, `Readout`, `Swatch`, `LapStrip`, `VideoHud`, `DeltaText` | Omarchy loader, domain components on tokens | What the kit provides |
| [omatrack-app](rust/crates/omatrack-app) (bin `omatrack2`) | **[done]** shell, state, workspace, panels (incl. the Laps sidebar and Where the time goes), actions, keymap, `sync` (video), `preferences`, `dialogs` (metadata, `TRACK.yml`); **[plan]** e2e | Entities, workspace, panels, palette, video sync | Analysis, format branches |

Waves: 1 foundations **[done]**; 2 app backbone + domain components **[done]**;
3 panels (traces, video sync, corners/laps/inspector/channels/map,
preferences/metadata/`TRACK.yml`) **[done]**; 4 integration, design review
(first UX round **[done]**), keyboard audit, trace/mpv hardening **[plan]**;
5 final verification **[plan]**.

### 4.2 Data flow

```text
Location::scan -> index cache -> LibrarySnapshot (Track>Date>Session>Laps) -> Library + Laps panels
Location::open -> Recording -> mapping -> laps -> unify: UnifiedLap @ 50 Hz
  -> session::load_lap -> LoadedLap {Arc<UnifiedLap>, laps, VideoBinding, TrackLayout?,
                                     overlay groups, lap strip cells}
  -> Analysis::build(primary, reference?, strategy, manual offset, corner override, cancel)
       -> Arc<Comparison> (the one map + delta), corner rows + notes, complexes, consistency
app Session entity -> TraceScene (Arc) -> TraceStack, corner ruler, map, damper strip
                   -> Corners / Laps / Inspector / Channels / Map panels, StatusBar
                   -> VideoController -> Player x2 (primary is the clock) -> VideoView + VideoHud
```

Everything from core/library is `Arc`-shared, `Send + Sync`, GPUI-free.

### 4.3 Seams and where changes belong

Crate seams **[done]**: `session::{load_lap, Analysis}`,
`omatrack_trace::{TraceScene, FractionMap, ViewportState, CursorState,
TraceStack}`, `mpv_player::{Player, FrameSource, VideoView, Follower}`,
`omatrack_ui::theme::install`, `omatrack_app::AppState` (entity
handles), `CommandRegistry`, one `init(cx)` + `Panel` per panel module.

New format or track data: upstream (`motorsport-telemetry-rs`, Track Atlas),
then bump the pin. New channel/unit rule: core mapping. New metric: core, via
`Analysis`. New corner note: a `CornerCheck` (6.6). New preference: a typed
`omatrack-library::config` field. New surface: a kit component; a first-party
element only for hot telemetry drawing, with measured cost. Never branch UI on
file format.

## 5. Configuration and metadata

- `omatrack.yml` (`$XDG_CONFIG_HOME/omatrack/`, else `~/.config/omatrack/`) is
  the only user config store: locations, channel display, drivers, last
  selection, recents (max 6), per-track corner overrides, video/trace settings,
  `workspace.layout`. Hand-editable, unknown keys preserved, atomic writes, never
  written into telemetry or caches.
- `TRACK.yml` in any folder: recordings inherit every one above them, merged
  root to leaf (closer wins per key). App edits are atomic and keep unrelated
  keys.
- **Metadata precedence**, implemented once in `omatrack-library::metadata`,
  higher wins: (1) `recording_metadata[<path>]` in `omatrack.yml`; (2) the
  `TRACK.yml` chain; (3) `tracks.<slug>`, track assignments by event date,
  `driver.mappings`; (4) what the recording says; (5) file/folder-name
  inference. The index cache holds layer 4 only.
- Driver codes may be float-backed or fractional: never coerce to integers. An
  exact `driver.mappings` entry beats `*`.
- Catalog dates use the venue timezone (atlas), then the recording's, then
  local.

## 6. Analytical contracts (carried over unchanged)

Ported from the 1.x core with byte parity, now held by
[rust/parity/run.sh](rust/parity/run.sh) against the frozen baseline (89 cases,
0 diffs). Behaviour changes here are product decisions and must keep it green
or re-baseline it deliberately (`--rebaseline`), with the reason in the
commit.

### 6.1 Time and source truth

- Telemetry time is integer nanoseconds from the file's first sample. A
  flattened array is not a clock: laps, resampling and media sync keep source
  chunk time bases (AiM: each gap-free run fitted with its own period, split
  when a sample drifts more than half a period).
- Player time is MP4 presentation time; the only conversion is
  `presentation = telemetry + signed per-video offset`. Frame lookup uses the
  presentation-order frame table, never `seconds x nominal FPS`.
- Parser errors are explicit `Result`s; no panic escapes a parser call.

### 6.2 Channel mapping and the normalized lap

`mapping` maps vendor names to standard concepts; `unify` builds a `UnifiedLap`
on the lap's **50 Hz** absolute-time grid:

- speed km/h (automatic mapping rejects declared non-speed units; bare
  `speed`/`velocity` never substring-match engine speed); throttle, driver
  throttle, clutch in `[0, 1]`; brake bar with pedal fallback; steering degrees;
  integer gear; monotonic distance in metres from 0; longitudinal (and lateral
  when present) acceleration; four dampers; GPS lat/lon degrees (east-positive,
  incl. the angular-minute convention) and reported position/speed accuracy.
- All arrays share `time`'s length and grid, sampled through the source clock
  so late starts, drops and gaps never shift events.
- Native lap distance is accepted only when continuity and total agree with
  integrated velocity; otherwise speed propagates short-term,
  accuracy-weighted GPS speed removes drift, and good fixes anchor the station
  map. Poor GPS never injects jitter; a missing optional channel never makes a
  lap unloadable. Overrides and raw channels come through `LoadOptions` and
  `ChannelProvider`, never format branches.

### 6.3 Lap detection and classification

- Lap boundaries come from upstream `motorsport-telemetry-rs` source metadata.
  The core's beacon / lap-time / lap-number / lap-distance splitting is a
  fallback only when upstream found none. Source-vs-`.telemetry` lap
  disagreement is an upstream bug.
- `classify_laps`: leading/trailing fragments and crossings implausibly shorter
  than the median are incomplete (`Out`/`In`/`Frag`), as are crossings covering
  much less distance than the best pair; complete laps far above the median are
  pit laps; the stationary interval between in- and out-lap is `Pit`. Only
  `counts_for_best` laps feed fastest marks, best times and default selection.
  Similar duration never invents a boundary.
- Lap-strip widths are a transient projection (`stopped`: ~4 Hz probe,
  excluding only finite speeds ≤ 1 km/h); a pit stop is one fixed 36 px cell,
  driven intervals have a 12 px floor. It never alters lap times,
  classification, cursor fractions or video timing.

### 6.4 Distance alignment and confidence

One cached primary-to-reference map per pair, chosen from strategies both laps
support, consumed identically by reference traces, delta, readouts, damper
strip, corner rows and video. Any independent index/distance/time mapping is a
bug.

- **Base** (`Lap distance %` / `Lap time %`): share of lap distance when both
  laps have native distance whose totals agree within 2%, else share of lap
  time (speed-fused distance drifts several percent per lap).
- **Verified GPS** (`GPS · continuous` dense, `GPS · re-sync` sparse): a fix
  anchors only if both laps' positions are self-consistent (speed implied by
  ±0.5 s of positions matches vehicle speed) and matched speeds agree; receiver
  accuracy figures are not trusted. Matches are monotonic, near the base, within
  60° of the primary's heading (never the other leg of a hairpin). Never pin the
  correction to zero at start/finish. A GPS map fitting both speed traces worse
  than the base is rejected.
- **Pre-corner dampers**: front-damper signature correlation before each
  corner, dropping anchors with clearly different speeds; needs corner zones and
  moving damper data on both laps. **Manual dampers**: one user offset on the
  base, set in the damper strip.
- **Default**: GPS when both laps carry it; else lap % on a distance base,
  dampers on a time base. Persisted in `video.reference_sync`.
- Every result reports basis, anchor count and confidence (HIGH/MED/LOW),
  shown by the Sync selector and in the status bar. Deltas under LOW confidence
  (e.g. 12–15 m turn-in) are presented as approximate.
- Swap inverts the manual offset; cursor and viewport fractions stay put.

### 6.5 Delta

A station-aligned cumulative time delta starting at zero, computed once in
`comparison` from the selected map. The same cached array feeds the delta lane
and every numeric delta (cursor, range, corner Δt, HUD). Nothing recomputes it.
Two views derive from that array in `comparison`/`session`, never in a view:
the loss rate (Δ per metre over ±`LOSS_RATE_HALF_WINDOW_M`, the heat map's
input) and `Analysis::time_split` (corners = Δ summed over the union of corner
zones, straights = the rest; they sum to the final delta).

### 6.6 Corner analysis via analyzers

The spec for what a corner comparison says is the corner analysis in
[tobi/ac-tracer](https://github.com/tobi/ac-tracer)
(`lib/windows/corner_analysis.lua`).

```text
corners::metrics::measure_corner(lap, start, end)   one pass over the samples
  -> CornerMetrics  (entry/apex/exit, brake, turn-in, coast, trail, downshift, grip)
CornerContext {primary, reference metrics, delta-trace time deltas}
  -> corners::checks::run(&ctx) over REGISTRY -> Vec<CornerNote {id, text, severity}>
```

- A check is a `CornerCheck` impl (`id`, `requires_reference`, `analyze`)
  appended to `REGISTRY` (order = display order); never an `if` in app state
  or a string built in a view.
- All sample scans happen once in `measure_corner`; checks are O(1) over
  scalars, never read raw arrays, never allocate per sample. Single-lap corners
  run primary-only checks. `run_with` takes an extended list (the out-of-tree
  seam); registration is compile-time.
- Thresholds are named constants (`corners/metrics.rs`, `checks.rs`): the
  numbers are the product decision.
- Deliberate Lua deviations: downshift reaction is relative to the reference,
  not an absolute 5 m; brake pressure needs a real brake zone on one lap. Not
  ported (no channels): lockups, TC, off-track, rev limiter. Without lateral G,
  turn-in uses the steering fallback and combined-grip checks stay silent.
- Verify a check by running `omatrack-cli corners` on two real laps of one
  track and reading the notes: true of that driving, silent where nothing went
  wrong. A check that fires everywhere is noise.
- `CornerRange` and `CornerComplex` stay distinct; membership and atlas
  landmarks are preserved, never rebuilt in a view.

### 6.7 Track Atlas and its cache

- Identity, layout, centerline, labels, `corner_ranges`, `corner_complexes`
  come from [Track Atlas](https://github.com/tobi/track-atlas); no competing
  local schema. Resolution by name/alias/slug, GPS and lap length (`track.rs`).
- 2.0 embeds the catalog via the pinned `motorsport-track-atlas` crate
  (`atlas_revision()`); updating data is a pin bump. It is ODbL: show
  `track::ATTRIBUTION` wherever atlas data appears (Preferences > Tracks).
- If a network refresh returns, the 1.x cache contract applies: cache under
  `$XDG_CACHE_HOME/omatrack/`, never in `omatrack.yml`; skip refresh while
  younger than 24 h; work offline; fall back to embedded/cached data, never
  fabricated corners.
- A user corner edit copies the whole zone list to `tracks.<track>.corners` in
  `omatrack.yml`, which wins on load.

### 6.8 Index cache and generation

`$XDG_CACHE_HOME/omatrack/index/rs1/{generation}/`, keyed by POSIX
`(dev, ino, size, mtime)`; failures never cached; other generations pruned at
scan start; nothing written beside a source. `{generation}` =
`converter_generation()` (format version + parser pin, e.g. `10-cac837feb12f`),
so a pin bump regenerates every cache.

### 6.9 Video sync semantics

- `VideoBinding {path, VideoClock, identity}`: verify the video is the one the
  telemetry describes (BLAKE3) before applying the clock. Missing or mismatched
  identity disables sync and shows a warning badge.
- **The primary recording is the clock**: 1x (0.25x in slow motion), never
  rate-corrected or sought to chase telemetry during play. Its `time-pos` feeds
  only `PlaybackClock`; cursor, HUD and traces sample the clock per frame, never
  from mpv callbacks. A cursor jump seeks both (the playing primary only beyond
  `PLAYING_SEEK_ERROR`). Constants: `omatrack_core::playback`.
- The reference follows the shared map. Pacing (`video.reference_playback`):
  `corners` (default) holds 1x through corners and uses the next straight to
  arrive together at turn-in; `gps` follows the map's local slope with short
  error correction; `recording` plays 1x and re-syncs only at lap
  start/selection, jumps and pause. Every mode hard-seeks the reference on
  pause, after a primary jump (> `PRIMARY_JUMP_SECONDS`) and on lap change.
- Lap end: pause, 3-2-1, select the next lap of the same session, resume
  (reference unchanged). Continuous (`video.continuous_playback`): no
  countdown; the next lap is adopted at the current position without seeking;
  the playhead holds at 33% of the viewport, unclamped; prefetch past 70%.
- HUD gap bar (±8 m along-track) only when both GPS fixes report < 1 m
  accuracy. `video.muted`, `video.hud_position` (the stage band's place,
  normalized, written at drag end) persist. Opening a video docks it; nothing
  enters fullscreen by itself.
- **Fullscreen stage** (F or the control row's button): the video panel renders
  over the whole window on black in place of the title bar, filmstrip row,
  docks and status bar. The dock layout is never touched (no dock zoom); the
  window enters fullscreen best effort. `stage::plan` lays one centred
  column: the same `Filmstrip` entity above, the delta lane touching the
  pictures, the pictures (aspect-fit; layouts 1–5, an inset beside the large
  picture, clear of its burned-in dashboard), the band just below them. Floating controls hide after `CONTROLS_HIDE_AFTER`
  (2 s) idle unless hovered and return on pointer motion or any keystroke.
  Escape leaves it and restores focus. Stage-only overlays: the telemetry
  band (`omatrack_trace::TelemetryHud`, 1000:210, 10% lap window clamped to
  the lap, playhead at 86% / 33% continuous, throttle and brake sub-lanes in
  the lap roles with the reference through the shared map, pedals, steering
  dial with P/R notches, gear and speed with their deltas, gap bar gated as
  above, draggable, HUD toggle) and the delta lane (P/R labels over their own
  pictures, centred Δ as the largest figure, gain/loss coloured, muted with
  `≈` under LOW confidence). Docked, the Δ is the gap lane's figure in the
  traces right below; the control row says only where the cursor is
  (`Cursor at 368 m, Turn 1`, `straight after T3` between corners) and the
  along-track gap when gated as above, never over the pictures.

## 7. UI architecture rules (GPUI)

On top of [coding-guides.md](.agents/skills/gpui-kit/references/coding-guides.md)
and [design-guides.md](.agents/skills/gpui-kit-design-guides/references/design-guides.md):

- **Kit first.** `TitleBar`; `DockArea` + `Panel`s (layout in
  `workspace.layout`, versioned, default on load error with a notification,
  Reset layout); `Sidebar` + `Tree` library; `DataTable` for Corners;
  `Command` palette in a `Dialog`; `StatusBar`; notifications (errors
  persistent, info autohide); a full-window Preferences screen (`Sidebar`
  section list + `GroupBox` cards, replacing the dock area and status bar
  while open; the dock stays alive behind it). Never rebuild what the kit
  has.
- **Right dock default** (layout v5): one tab group led by **Time lost**
  (`panels::time_goes`, "Where the time goes": heat map, corners by Δt
  largest first with loss bar, s and entry-speed Δ, the corners/straights
  split, and a card for the selected corner with its speeds, entry/exit Δt
  and the `CornerCheck` notes as sentences, plus "Open … in detail" =
  `FocusCorner` + the Corners tab); Corners, Laps, Channels, Map and
  Inspector are tabs behind it. The selected corner is the focused one, else
  the one under the cursor, else the largest loss. Without GPS the map is
  omitted and the table stands alone.
- **First-party components** only where the kit has none: trace lanes
  (in the idiom of gpui-component chart/plot: scales, axis, grid, crossline),
  `LapStrip`, `CornerRuler`, `TrackMap`, delta lane, `DamperStrip`, `VideoHud`,
  `VideoView`. Hot drawing is a custom `Element`
  ([element.md](.agents/skills/gpui-kit/references/gpui/element.md)), not a div
  tree.
- **Window chrome, top to bottom**: the `TitleBar`, the comparison in one
  line: track + event/date; the P pill "against" the R pill (role disc,
  driver, lap, time; a click focuses the left lap list, a small swap
  button sits between them); the headline lap-time Δ (`s off reference`,
  gain/loss coloured, never `≈`: it is the laps' own times via
  `Analysis::lap_time_delta`, exact at any confidence); the sync button
  (`Synced by lap time % · Low confidence`, warning-tinted outline when
  LOW/NONE, neutral otherwise) opening the strategy menu; Commands and
  Preferences. Below 90 rem it drops the event, the Δ words, the sync verb
  and the Commands label. Then the **filmstrip** (`workspace::Filmstrip`,
  one full-width `Entity` above the docks), the `DockArea`, the
  `StatusBar`, kept minimal: jobs and the range readout left, the palette
  name right; the cursor, its Δ and the sync live elsewhere, never twice.
  The filmstrip has one
  row per role recording (the primary's, then the reference's; two laps of
  one recording share a row with both roles marked): a fixed gutter (role
  marker, driver/session, selected lap time), the swap button, then
  `LapStrip` cells. Panels carry no lap strip of their own. It is
  re-homeable: a fullscreen surface renders the same entity
  (`Workspace::filmstrip()`), never a second strip.
- **The centre** (video over traces) has no panel title bars
  (`PanelKind::has_title_bar`) and **one control row**, under the
  pictures: a filled play disc, `0.25×`, `Per lap | Continuous`,
  `Distance | Time`, then right-aligned the cursor place and the chips that
  need attention (a degraded sync, identity), and small icon buttons: mute,
  video layout (and reference pacing), fullscreen and a `…` tools menu (FIT,
  Resize lanes, Edit corners, zoom). Every control dispatches the action of
  its key. The traces carry no toolbar; a range selection's statistics
  float at the top right of the lanes.
- **Corner ruler**: labels only, centred over their zones on two staggered
  rows by corner index (T1 T3 T5 above, T2 T4 below), one form throughout
  (full names, else short forms), never overlapping. The focused corner,
  else the one under the cursor, is a filled chip. Complexes that group two
  or more corners are a quiet bracket line above the rows. Zones shade every
  lane as quiet columns (`muted` at low alpha, in the overlay); edges show
  only as grips while editing.
- **Left dock: [Laps | Library]** (layout v5). The **Laps sidebar**
  (`panels::laps`, `PanelKind::Laps`) is the default left surface: the
  primary's event (its track and day in the `LibrarySnapshot`, plus the
  reference's recording when it comes from elsewhere), one group per
  recording in catalog order (driver name, `N timed laps, best m:ss.sss`, a
  lap-time trend over `counts_for_best` laps with the best dotted, painted
  through `omatrack_trace::mesh`). Timed laps list lap, time, a gap bar
  scaled to that group's own best-to-worst spread, and the gap via
  `format_delta` (`Best` on the best); out / in / pit / partial laps wait
  behind `Show out and in laps (N)` unless they hold a role. Role laps are
  filled with the role badge; groups without a role start collapsed. The
  Library tree is the tab beside it, reached by Ctrl+1, Ctrl+O, the palette
  `Browse library` and the sidebar's empty state. The Laps table on the
  right is gone (one lap list, not two).
- **gpui-omarchy was evaluated and rejected**: it disables gpui-component,
  lacks key components, and its theme conflicts with gpui-component's.
- **State** ([entity.md](.agents/skills/gpui-kit/references/gpui/entity.md)):
  models are `Entity<T>` in `AppState`; views hold handles and observe.
  `Session` owns primary/reference, strategy, manual offset and `Analysis`.
  Cursor motion notifies only `CursorState` observers (overlays, readouts),
  never the static trace layer. Stateless leaves are `RenderOnce`.
- **Identity** ([element-id.md](.agents/skills/gpui-kit/references/gpui/element-id.md)):
  rows keyed by stable library ids (section + path), never index.
- **Nothing blocking on the UI thread**
  ([async.md](.agents/skills/gpui-kit/references/gpui/async.md)): no parse,
  resample, walk, I/O, hashing or analysis on the foreground executor. Each
  pipeline role (scan, primary load, reference load, analysis, overlay) owns
  one `Option<Task<_>>` slot: latest wins, replacing cancels. Results apply on
  the UI thread, then notify; current data stays visible while a replacement
  loads; progress shows through Jobs and the StatusBar.
- **Selection**: a rescan never resets loaded laps, cursor, viewport or
  alignment. Swap (`x`) cancels pending loads, keeps cursor and viewport,
  inverts manual offset. Changing a lap clears pair tuning, keeps cursor and
  viewport.
- **Overlays through `Root`**; escape closes the topmost, restoring focus.
- **Preferences** writes go through the Preferences entity: debounced, atomic,
  off-thread, flushed on quit.
- **Primary = active lap, reference = compare lap**, fixed colours everywhere.
- **Lanes**: by default the pinned gap lane, Speed (tall), Throttle, Brake
  (its own lane; `combine_with_previous` overlays it) and Gear. Steering,
  RPM and every other channel are opt in (Channels panel,
  `channels.<key>.visible`).
- **Lane legends**: the chrome column is `omatrack_trace::CHROME_REMS` wide
  (lanes, damper strip and the ruler row share it). Line 1 is title (medium)
  + unit (muted); below it, P / R / Δ sit in fixed-width tabular columns on
  the same spines in every lane (P in the primary role, R in the reference
  role, Δ muted), with a P R Δ key in the ruler row; the channel column
  (a line swatch) exists only while a lane is shared. The gap lane
  (`Gap to R`) is the only big figure: the gap at the cursor (idle: the
  change in view) in gain/loss, over `s at cursor, ends +2.44` (`… in view`
  when zoomed); under LOW confidence it reads `≈` and muted. A shared
  lane's title and two rows fit `MIN_LANE_HEIGHT`. Nothing clips
  (headless-tested).

## 8. Trace rendering performance contract

Semantics carry over from [docs/TRACE_RENDERING.md](docs/TRACE_RENDERING.md).

- **Budget:** design target **8.33 ms** per frame (120 Hz); **16.67 ms** is the
  hard ceiling. Cursor/hover updates < 0.1 ms.
- **Decimation** (`omatrack_trace::decimate`, 1:1 port of `TraceDecimator`):
  min/max per device column **in source order** at true sub-column positions,
  one path rule at every zoom. NaN is a pen-up, even inside one column.
  Reference positions invert the shared map locally (within a device pixel),
  never by lap endpoints. A slope corridor removes only sub-pixel detail
  (≤ 0.1 physical px). Area and stroke share one path; the primary is decimated
  once. Zoom floor 1e-7 lap fraction; it never implies more resolution.
- **Paint:** logical-pixel strokes, one strip per pen-down run, shared-vertex
  joins (miter limit 2, no caps), width independent of zoom. Fills are
  non-overlapping trapezoids to a transparent baseline (premultiplied MSAA
  double-blends overlaps). Order: primary area, reference outline, primary
  outline. Delta: diverging gain/loss fill.
- **Forbidden:** `shape::Line`/`Area` or `PathBuilder` for traces (they skip
  NaN instead of lifting the pen; lyon u16 indices hit the **65,535-vertex
  cliff** where later lanes silently vanish). Use `omatrack_trace::mesh`
  (`stroke`, `fill_to_baseline`) writing plain triangles into GPUI `Path`
  vertices.
- **Two passes:** the static layer is a `.cached(...)` view rebuilt only on
  scene, viewport, layout, style or theme change; `TraceStack::static_rebuilds()`
  counts rebuilds and tests assert a cursor move leaves it unchanged. Cursor,
  hover, selection, corner drag and readouts are overlay-only.
- **No per-frame allocation:** decimation buffers, meshes and label layouts are
  retained in element state and reused (`point_capacity`); after warm-up no
  `Vec` growth, sample-array clones, `format!` of unchanged labels or `Arc`
  rebuilds per frame. Scene data is `Arc<[f64]>`. Geometry caches never key on
  colour. Off-screen lanes are skipped.
- **Bench:** `cargo run --release -p omatrack-trace --example trace_bench`
  (8 lanes x 2 laps, 1x–10,000x sweep, 2560 device px, dpr 2): geometry avg
  ≤ 4 ms, worst ≤ 8.33 ms, hover < 0.1 ms. Report before/after for renderer
  changes. The worst-frame target is not yet met on every run; wave 4 hardens
  it **[plan]**.

## 9. mpv-player contract

[rust/crates/mpv-player](rust/crates/mpv-player) **[done]**, no Omatrack
dependency.

- FFI from `libmpv2-sys =4.0.1` (mpv 2.5 bindings incl. the SW render API);
  `build.rs` probes libmpv ≥ 2.5. Only `ffi` and `render` carry
  `#[allow(unsafe_code)]`. Never spawn the mpv CLI or embed a foreign window.
- `Player`: load, play/pause, exact and relative seek, speed, mute, volume,
  `apply(FollowAction)`, `state()`, `clock()`, `events()`, `frame_source()`.
- An event thread mirrors properties; `time-pos` feeds only the lock-free
  `PlaybackClock` (`estimate(Instant)` for per-frame pulls). A render thread
  drives the **software** render API into a latest-wins `FrameSlot` behind
  `trait FrameSource` (GPU interop can replace it).
- Frames: `bgr0` with alpha forced to 0xFF (BGRA `RenderImage`), 64-byte
  stride, rendered at display size (≈ 2560 px cap); every replaced image is
  dropped from the atlas; the reference may cap at 30 fps when CPU-bound. A
  1080p SW cost above ~8 ms is reported with an EGL/PBO follow-up.
- `VideoView` paints aspect-fit on a caller-supplied letterbox token (the
  theme background docked, black on the fullscreen stage), themed
  loading/error states, animation frames only while playing, draws only while
  visible (`set_visible`).
- `Follower::step(target, rate, policy) -> FollowAction {None, SetSpeed,
  HardSeek}` is mechanics only; targets come from `omatrack_core::playback`.
- Scalers stay builtin bilinear
  ([VIDEO_SCALER_COMPATIBILITY.md](docs/VIDEO_SCALER_COMPATIBILITY.md)).

## 10. Theme

- Tokens only (`cx.theme()`); a hex literal outside the loader defaults and
  tests is a bug.
- `omatrack_ui::theme` reads `~/.local/state/omarchy/current/theme/colors.toml`
  (then `$XDG_STATE_HOME/omarchy/current`, then legacy
  `~/.config/omarchy/current`), accepts ANSI `color0..15` and semantic keys,
  maps onto the gpui-component `Theme`, and hot-reloads via a `notify` watcher.
  No palette: built-in gpui-component dark. `ThemeStatus {name, source}` shows
  in the status bar.
- Fonts: the desktop fontconfig choice when one is configured and installed,
  else the bundled Inter (UI) and Geist Mono (code), OFL, registered by
  `theme::install`; the active families show next to the theme status.
- Type ([`omatrack_ui::typography`](rust/crates/omatrack-ui/src/typography.rs)):
  one interface family. Every number is `.numeric()` (Inter `tnum`, tabular
  figures), never the monospace family, which is kept for paths, file
  contents and identifiers. Sizes come only from the `TypeScale` steps
  (caption 11 / label 12 / body 13 / title 14 / heading 16 / display 20 px
  at the default rem); painted labels use `TypeStep::size` and
  `tabular_figures()`. Regular for values, medium for row and lane names,
  semibold only for a surface heading. Displayed negatives use U+2212
  (`omatrack_ui::MINUS`, via `format_value` / `format_delta`).
- Roles: primary = `primary` (Omarchy accent); reference = `warning`; gain =
  `success`; loss = `danger`; grid `border`/`muted`; labels
  `muted_foreground`; extra channels `chart_1..5`;
  `channels.<key>.color`/`reference_color` override. A channel sharing a
  lane (brake overlaid on throttle) draws its primary in its chart hue and its
  reference in the reference role, both quieter (60% over the background).
  Legend and inspector values always carry the lap role colour. success /
  danger mean only Δ, never a pedal. With no Omarchy palette the built-in
  dark theme's primary is `blue-400` (its own primary is white).
- Heat ramp (`TracePalette::heat`, "less — more"): a quiet tone
  (`muted_foreground` at 28%) blended to `danger` in `HEAT_LEVELS` steps,
  losing stations spread from their 25% to their 95% quantile; gains stay
  quiet (the ramp says where time goes; gains read in the delta lane). The
  Time lost loss bars use the same ramp.
- Consumers `observe_global::<Theme>`; a theme change repaints, never rebuilds
  geometry.

## 11. Keyboard and actions

Actions live in `omatrack-app/src/actions.rs` (namespace `omatrack`), bound in
`keymap.rs`, all reachable from the palette with their `Kbd`.
Single-letter keys bind in `Workspace && !Input` so they never reach text
fields (tested). `SelectLap {session, lap, role}` and `FocusCorner {id}` back
palette items. See [action.md](.agents/skills/gpui-kit/references/gpui/action.md),
[focus-handle.md](.agents/skills/gpui-kit/references/gpui/focus-handle.md).

| Keys | Action |
|---|---|
| ctrl-k / ctrl-, / ctrl-o / ctrl-q | Palette / Preferences / Open folder / Quit |
| ctrl-b / ctrl-j | Toggle left dock ([Laps, Library]) / right dock ([Time lost, Corners, Channels, Map, Inspector] tabs) |
| ctrl-1 … ctrl-6 | Focus Library / Traces / Video / Corners / Laps / Map |
| space; left / right | Play/pause; ±2 s |
| m / s / p | Mute / 0.25x / continuous playback |
| f / escape | Fullscreen video stage / exit, focus restored (escape also closes overlays) |
| 1–5 | Split, primary+PiP, reference+PiP, primary only, reference only |
| x / a | Swap roles / edit corners |
| h / j | Previous / next corner (no wrap) |
| = / - / ctrl-0 (ctrl-= / ctrl--) | Zoom in / out / reset |
| [ / ] / t | Previous / next lap / toggle Distance-Time axis |
| up / down; enter / alt-enter (Library, Laps) | Move; set primary / reference (Laps: enter on a group or disclosure opens it) |
| ctrl-s / escape (resize, corner edit) | Save / cancel |
| escape; up / down (Preferences) | Back to the workspace (focus restored); previous / next section |

Pointer: left-drag selects, middle-drag and horizontal scroll pan, wheel (also
shift/ctrl) zooms about the pointer, vertical wheel scrolls overflowing lanes,
double-click resets. Focusing a corner (click, `h`/`j`) centres it in the left
half with one 140 ms OutCubic transition (retargeted on repeat), masks
neighbour laps behind `« L8` / `L10 »` rules instead of re-framing, and shows
notes; escape restores the pre-focus viewport.

Filmstrip: a click selects that row's role's lap (cursor and viewport stay
put); clicking the lap the role already holds moves the cursor to the lap
start; right-click (or alt-click) sets the lap as reference. Each role's
selected cell is filled in its role colour. Swap is `x`, the palette, or the
button on the filmstrip gutter.

Laps sidebar: the same rules. A click sets the lap for its group's role
(the reference when the group holds only the reference, else the
primary); right-click or alt-click sets the reference. The footer names
Enter / Alt+Enter for the lap under the keyboard cursor (a focus ring,
shown only while the panel has focus).

## 12. Testing and verification

Gates, from `rust/`; a failing gate is a failure:

```sh
scripts/check.sh    # fmt --check, clippy -D warnings, test --workspace --locked,
                    # real_* tests (OMATRACK_FIXTURES), parity if the baseline exists
parity/run.sh       # against the frozen baseline: 89 cases, 0 diffs
cargo run --release -p omatrack-trace --example trace_bench
cargo build --release --locked -p omatrack-app
scripts/screenshot.sh [-o out.png] [-k keys]   # headless visual check (cage + grim)
```

- **Lockfile:** the gpui stack is yanked: always `--locked`, never
  `cargo update` ([seed-cargo-lock.sh](rust/scripts/seed-cargo-lock.sh)).
  Parallel runners use `CARGO_TARGET_DIR=rust/target-<slug>` and `-p` gates.
- **Parity harness:** the Rust CLI against a frozen baseline
  (`rust/parity/baseline/`, captured from the last byte-identical run of the
  retired C++ oracle), `diff -r` of stdout, stderr, CSVs, exit codes;
  `--rebaseline` accepts a deliberate change. Excluded: `--version`,
  non-numeric `--lap`/`--zone` (the port exits 2 where the oracle aborted).
  `.pds`/`.ld`/`.vbo` rest on synthetic tests. Baseline and outputs carry GPS:
  gitignored.
- **Screenshots:** `scripts/screenshot.sh` renders `omatrack2` on a headless
  Wayland output (`nix shell` cage + grim, keys via wtype) into
  `target/shot/`. Run it unsandboxed (it binds a Wayland socket). It is a
  headless visual check only: native on-device visuals, live Omarchy
  switching and frame time stay UNVERIFIED.
- **Headless GPUI tests** (`#[gpui_kit::test]`, `TestAppContext`;
  [test.md](.agents/skills/gpui-kit/references/gpui/test.md),
  [test-examples.md](.agents/skills/gpui-kit/references/gpui/test-examples.md)):
  every panel, action, keymap context and overlay gets a test driving real
  actions and asserting entity state. There is no display here: native
  visuals, live Omarchy switching and on-device frame time are reported
  UNVERIFIED, never claimed.
- **Real files:** `#[ignore]`d `real_*` tests over `$OMATRACK_FIXTURES`
  (default `~/Documents/Telemetry/26T07_PLM`). E2E **[plan]**: load library,
  Run4 fastest primary, Run1 fastest reference, zoom, swap, palette "T5"
  focuses T5, cursor move leaves `static_rebuilds()` unchanged.
- **`~/Documents/Telemetry` is READ-ONLY private data.** Never write, touch or
  create anything there; derived files go under the target dir. After real
  runs: MP4 sha256 unchanged, `find ~/Documents/Telemetry -newer <marker>`
  empty. Never commit or upload its contents; CSV exports hold GPS.
- Final greps: no `gpui-omarchy`, no out-of-scope code, no raw colours outside
  the theme loader and tests.

## 13. Out of scope (for now) and their seams

Not built, with no code, stubs, flags, config keys, UI or docs: remote storage
(WebDAV/S3/GCS), Lua plugins, image/video-derived telemetry, writing
`.telemetry`, self-update, USB copy/sync. They may return (Qt designs in
[docs/legacy/AGENTS.qt.md](docs/legacy/AGENTS.qt.md)). Two cheap seams remain:

- **Channel provider** `omatrack_core::overlay::ChannelProvider`: plugins or
  image-derived channels would be providers resampled onto the 50 Hz grid and
  drawn through the ordinary lane path.
- **File source** `omatrack_library::Location` (+ the serde-enum `locations`
  config, unknown types kept): remote storage would be another `Location` with
  its own cache, never a parallel list.

## 14. Commits

- On `gpui-port`, logical steps, subject `<scope>: <summary>` (`core`, `cli`,
  `library`, `trace`, `mpv-player`, `ui`, `app`, `rust`, `docs`), a short body
  with what, why and gate tails for behaviour changes, ending with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>` for
  agent-authored commits.
- Agents edit `rust/` concurrently: `git add <paths>` only, never `-A`/`.`;
  never stash, reset, checkout or clean others' work; never commit the
  `gpui-kit*` skills, telemetry, video, parity output or CSVs.

## 15. Definition of done

Right crate (section 4); analytical contracts and parity intact; every
section 12 gate passing with real output; a headless test for UI changes; frame
budget met for rendering changes; source telemetry untouched; tokens and kit
components only; this file updated when architecture, a contract, a seam or a
crate's status changes.
