# Omatrack agent guide

## Mission

Omatrack is a native racing-telemetry workstation. Its primary target is Linux under Omarchy, built with Qt 6, Qt Quick, and Material controls. It should turn heterogeneous logger files into a coherent, driver-facing model of sessions, laps, channels, tracks, corners, and corner complexes.

This is not a single-format file viewer or a generic chart demo. The product direction is a full telemetry system that can ingest every major race format through one analysis pipeline. The current parser bridge supports Pi/Cosworth `.pds`, MoTeC `.ld`, Racelogic `.vbo`, AiM `aimd` telemetry embedded in `.mp4`, native `.telemetry`, and Motorsport Telemetry JSONL (MTJ recordings and MTX sidecars). That is the starting set, not the intended limit. Local files are opened directly by the parser on every open — no conversion, no fingerprinting, nothing written anywhere. `.telemetry` is generated only for recordings on a connected server: keyed by the object's ETag under `.omatrack/c/{generation}/` both in the local cache and at the remote root, so one conversion of a multi-gigabyte onboard MP4 serves every machine. Motec `.ldx` is not a session. MTX is an overlay, not a library session: drop or sibling-find a `.ext.jsonl` / `.mtx.jsonl`, overlap-join it onto the open host by integer nanoseconds, and show it as a collapsible folder of extra channels and spans. `.telemetry` is Omatrack's native recording: header first, O(1) catalog, lossless channel columns, laps, video links, and frame sync. Any problem that should be solved at load time or pre-computed belongs in that format — extend `telemetry-format` upstream, do not add a second sidecar schema in Qt.

## Product goal

Build a generic telemetry workstation that happens to work exceptionally well
for an LMP2 IMSA team. Do not overfit the implementation to that team: paths,
driver names, car numbers, classes, event assumptions, and other team-specific
values must not be hardcoded in the codebase. Make those values easy to
configure, persist, and override through the application model and preferences.

## Product philosophy

### Racing concepts first

Expose the concepts drivers and engineers use: session, lap, reference lap, distance, delta, braking point, turn-in, apex, throttle pickup, corner, and complex. File chunks, sample encodings, parser quirks, and vendor naming belong below the product boundary.

A corner is an individual turn/apex and its analysis range. A corner complex is a named, contiguous group of corners that drivers discuss as one section. Preserve both levels: a user must be able to inspect the complex as a whole and drill into its members. Never flatten complexes into synthetic single corners or infer a complex merely because two turns are adjacent.

### One normalized analytical truth

Every format feeds the same cross-format model. Standard channels, units, lap bounds, monotonic distance, and comparison semantics must not depend on the source vendor. Format-specific decoding belongs in Rust; cross-format racing analysis belongs in the Qt-free C++ core.

The current canonical lap is a 50 Hz `UnifiedLap`. Primary/reference traces, cursor values, delta, and synchronized video must derive from the same cached track-station alignment so two views cannot disagree.

### Dense, calm, expert UX

Optimize for excellent information density, not empty space or mobile-sized chrome. Keep the current hierarchy: compact session context, proportional lap strip, session tree, large trace workspace, and focused inspectors. Numeric data uses a compact monospace treatment; color communicates role and comparison consistently.

Dense does not mean noisy. Default views show the channels that answer common driving questions. Raw and specialist channels remain one action away. Prefer progressive disclosure, direct manipulation, keyboard/mouse fluency, and side-by-side context over wizard flows and decorative panels.

### Performance is a product contract

Interactive rendering targets 60–120 fps. A frame is 16.67 ms at 60 Hz and 8.33 ms at 120 Hz; hot interaction paths should be designed for the 8.33 ms budget.

- Never parse, resample, scan whole channels, perform network I/O, or rebuild static geometry on cursor movement.
- The main event loop is UX only. Disk, network, parse, and cache work
  run on a worker (`QtConcurrent` or the dedicated I/O thread). Do not
  block the GUI thread on I/O, and do not wait for it with a nested
  `QEventLoop`.
- Keep static traces separate from the lightweight cursor/selection overlay.
  An overlay that caches lap pointers from the static build must compare them
  against the store every frame: `updatePaintNode()` runs on the render
  thread, and a `UnifiedLap` the GUI thread replaced in between is freed
  memory by then.
- Cache normalized laps, raw-channel resamples, delta arrays, geometry, and overview rasters; invalidate them only when their inputs change.
- Bound draw work to the viewport and pixel budget. Do not submit every source sample when fewer points can produce the same image.
- Avoid per-frame heap allocation and avoid copies of full telemetry arrays.
- Benchmark hover and zoom paths before and after renderer changes. A visually correct regression that misses the frame budget is not complete.

### Native Linux and Omarchy are deliberate

Use Qt Quick Material for application chrome and C++/Qt rendering for hot paths. Do not introduce a web stack or a second UI toolkit. Omarchy is the first-class desktop integration: follow its active color theme when available and fall back to `Style`'s complete built-in dark palette everywhere else — never the platform `SystemPalette`, whose single highlight color cannot serve as both `accent` (text, marks) and `selection` (row background) and whose light Windows values clash with the dark video chrome. Cross-platform behavior is welcome when it does not weaken the Linux experience or complicate the hot path.

### Preserve source truth

Telemetry and onboard-video inputs are immutable evidence. Never rewrite, rename, or delete source files. Normalization, aliases, corner edits, caches, and exported analysis are separate state. Be especially careful with event data outside this source tree.

### Prefer clean migrations

Ignore legacy compatibility unless the user explicitly asks for it. When a
schema, workflow, or UI model changes, update the existing owned metadata and
configuration files to the new shape, remove obsolete fields and code paths,
and prune components that are no longer needed. Do not carry compatibility
branches, duplicate representations, or dead migration logic forward by
default. This does not weaken the source-truth rule: telemetry and onboard
video remain immutable evidence.

### Track Atlas is authoritative

[Track Atlas](https://github.com/tobi/track-atlas) is the upstream source of track/layout identity, real geometry, label layers, corner points, `corner_ranges`, and `corner_complexes`. Consume its schema; do not create a competing local track schema or duplicate curated metadata in application code.

Track Atlas connectivity is independent of telemetry parsing. Cache upstream data, skip refreshes while the cache is less than 24 hours old, continue to work offline, and fail back cleanly without fabricated corner metadata. Do not bundle track or corner datasets without documented redistribution rights and attribution.

### Configuration and portable recording metadata

Application-wide user configuration state belongs in `omatrack.yml`
(`$XDG_CONFIG_HOME/omatrack/omatrack.yml`, else `~/.config/omatrack/omatrack.yml`).
It is the single source of truth for telemetry directories, server connection
settings, the download cache limit, recent file history, channel display,
driver naming, last selection, per-track corner overrides, and portable
AppImage update checks, and it is meant
to be read, diffed, and hand-edited. Never add a second configuration store, and never write
configuration into telemetry, caches, or `QSettings`.

Portable recording metadata is the deliberate exception: a folder may contain
a `TRACK.yml`, and recordings inherit metadata from every `TRACK.yml` above
them in root-to-leaf order. Editing folder metadata writes that folder's
`TRACK.yml` atomically and preserves unrelated keys such as `files`; a closer
folder or per-video override wins. Individual video overrides remain under
`recording_metadata` in `omatrack.yml`. Never rewrite telemetry or video files.

#### Recording metadata precedence

There is one rule, implemented once in `TelemetryStore::effectiveMetadata()`,
and every consumer (sidebar rows, track/driver display, folder consensus, lap
loads, HUD) goes through it. Higher wins:

1. `recording_metadata[<recording path>]` in `omatrack.yml` — the per-video
   override the user made in the recording dialog.
2. The `TRACK.yml` chain, merged root-to-leaf: a closer folder's file
   overrides its parents, key by key (`track_metadata::merge`).
3. Per-key preferences that are not path metadata: `tracks.<slug>` overrides,
   `trackAssignments` (by event date), `driver.mappings` (by driver id).
4. What the recording itself says (venue, driver id, car fields, laps).
5. Inference from the filename and folder name.

The index cache (`index/v2`) holds layer 4 only, so layers 1–3 never need
to invalidate it. Layer 2 is a discovery-time snapshot (`fileMetadata_`),
refreshed when the app itself writes a `TRACK.yml`, read once and memoized
for a path opened ahead of its folder scan. It is not re-read per row or per
frame, and an external hand-edit of `TRACK.yml` shows up on the next scan of
that folder, not live — by design.

Upstream data is not configuration. Track Atlas is used as-is by default; the
moment a user edits corner zones for a track, the whole resulting zone list is
copied out to `tracks.<track>.corners` in `omatrack.yml` and that override
wins on load. Caches (Track Atlas snapshot) stay outside the file.

## Feature model

### Ingestion and session library

- Recursively scan configured local directories and the local caches of
  connected servers, and open individual telemetry or video files from the
  command line,
  file dialog, recent-file menu, or application drag/drop. Individual files do
  not become configured scan roots; persist at most the six most recent
  successful opens in `omatrack.yml`.
- One instance per user. A launch from Explorer/Finder/`xdg-open` hands its
  paths to the running Omatrack over `SingleInstance` (a `QLocalServer`
  keyed on the user name) and exits; the primary opens them and raises its
  window. `--new-instance` opts out; acceptance runs never join or listen.
  Paths from dialogs and drops cross QML as `file:` URLs and are decoded by
  `Store.localPathFromUrl()` (`QUrl::toLocalFile`), never by string
  slicing — `file:///C:/…` is the Windows shape and a hand strip breaks it.
- Run directory discovery and lap-summary parsing off the UI thread;
  expose `TelemetryStore::loading` so every session-library surface can retain
  its current data and show progress while a replacement snapshot is built.
- A local file is its own parser path. `openIndex()` reads only what the
  sidebar needs. A compact metadata cache lives under
  `$XDG_CACHE_HOME/omatrack/index/v2/{generation}/`, keyed by POSIX
  `(dev, ino, size, mtime)` and `omatrack::converterGeneration()`. Failures
  are not stored. Nothing is written beside the source recording.
  Only remote recordings get a `.telemetry` (below).
- Dispatch formats through the Rust parser workspace.
- Infer inexpensive metadata from filenames/folders before parsing samples.
- Group the library as Track → Date → Session → Laps.
- The file sidebar groups each folder's recordings by calendar day, oldest
  first. A row is session and driver, not the filename: session name when
  known, then the mapped driver or the raw ID, with best lap always on the
  right and start time, lap count, and drive time on the second line.
- Lap boundaries belong to upstream. Every bridge open — vendor file or
  `.telemetry`, full or index — hands the core the laps from
  `motorsport-telemetry-rs`'s `read_source_metadata` (counter numbers, timer
  reset instants, fastest = shortest plausible complete lap of that list),
  which is also exactly what the converter writes into the catalog. So the
  CLI on an `.mp4`/`.pds` and the GUI on its `.telemetry` see the same laps by
  construction. `TelemetrySource::detectLaps()` only falls back to its own
  beacon / lap-time / lap-number / lap-distance heuristic when upstream found
  no laps at all. A lap disagreement between source and `.telemetry` is an
  upstream bug: fix it there and advance the pin; never paper over it in C++.
- The source recording is the truth for time. AiM chunks follow the logger's
  own millisecond stamps (each gap-free run fitted with its own period, split
  when a sample would drift more than half a period), so a `.telemetry` that
  disagrees with its `.mp4` about when something happened is stale and must
  be regenerated — which the converter generation in the cache key does.
- Classify every detected lap, including vendor-supplied lap lists: leading/trailing recording fragments and crossing pairs implausibly shorter than the session median are incomplete (`Out`/`In`/`Frag`), crossings that cover much less lap-distance than the best pair are incomplete, and complete laps far above the session median are pit in/out laps. Only representative laps (`LapEntry::countsForBest`) feed fastest-lap marks, sidebar best times, and default lap selection.
- Discovery refresh reconciles the session registry without rebuilding the
  active/reference snapshots or resetting viewport, cursor, alignment, closed
  groups or sidecars. Startup selection restoration runs only at startup and
  supports two laps from the same recording. Missing inactive entries are
  pruned only when no role load is pending; active loaded snapshots remain
  usable if a volume temporarily disappears. This is not a claim of automatic
  hot-reload of changed source bytes or inherited metadata.
- A role swap cancels pending file/role loads, moves the cursor and viewport
  to the previous reference position, and inverts manual alignment. An ordinary
  change to either selected lap clears tuning from the previous pair. Changing
  the primary host cancels old overlay jobs and invalidates host-specific joins.
- Every store-backed `ListView` carries a `ScrollAnchor { view; role }`
  (`src/app/ScrollAnchor.*`). It records the identity-role value of the row
  under the top edge (plus pixel offset) before any structural model change
  — insert, remove, move, layout, reset — and re-finds that identity after,
  falling back to the nearest former neighbour. A numeric `contentY` or a
  row index is not an anchor: rows above shift both. The library exposes
  `rowIdentity` (section-scoped) for this; channels and driver mappings use
  `key`. Do not add per-view `contentY` save/restore timers.
- The event filter is owned by `LibraryFilterModel::setEventFilter()`:
  entering event mode stashes the manual track/day facets and applies the
  event's; leaving restores them; `clearAllFilters()` also leaves the event
  filter (and the sidebar turns `Store.eventMode` off) while the event's
  track/date/session stay configured for USB naming.
- Cache parsed/unified laps lazily per session. Opening and normalizing active
  and reference laps runs on the worker pool; `TelemetryStore::lapLoading`
  drives feedback, and per-role generations discard stale rapid selections.
  The old primary's `unifiedCache_` is evicted when the session changes in
  `setPrimary()`, and the old compare's in `setCompare()`, so exploring many
  sessions does not accumulate UnifiedLaps indefinitely. Raw channel arrays
  (`src_`) are freed after unification in `adoptLoadedLap()`;
  `extraChannelData()` re-opens the file on demand for the opt-in raw-channel
  feature.
- The library is one ordered list of locations under `locations` in
  `omatrack.yml`. A location is either a local folder (`type: folder`) or a
  connection to an outside server (`type: webdav`, `s3`, or `gcs`), and both
  carry `id`, `name`, `target`, and `enabled`; a connection may also carry an
  `options` map for protocol tuning such as an S3 `region` or a non-AWS
  `endpoint`. Keep tuning out of `target`: the connection id is a hash of it,
  so a knob added there orphans the whole downloaded cache the first time it
  is adjusted. An address may nonetheless be *typed* whole —
  `s3://KEY:SECRET@bucket/prefix?region=…&scheme=…&endpoint_override=…`, the
  form an S3 console and Arrow both hand out — because `splitAddress()` takes
  it apart at both entry points (the dialog and the config loader) before
  anything is stored. Nothing reaches `target` but the bucket and the prefix,
  which is also what keeps a secret key out of the cache directory name and
  out of the library row on screen. Disabled locations stay configured
  and are skipped by every scan. Keep this list heterogeneous: a new remote
  source is a new `LocationType` plus a `connectionTypes()` entry, never a
  second parallel list. `~/Documents/Telemetry` (resolved through the platform
  Documents location, so Windows OneDrive redirection is honored) is the only
  location on a fresh install and is created if missing.
- USB-backed volumes are discovered on an `AsyncJob` worker using Linux
  mount-table and sysfs data, without statting unrelated network mounts.
  A GUI timer only schedules the poll; directory checks and scans stay on
  workers. `QFileSystemWatcher` watches configured roots and USB mount roots.
  A mount is a transient
  `USB — …` library section only when that scan finds a supported telemetry or
  video file. It is never added to `locations`. Copy is a separate overlay in
  the video slot (the slot is shown for it even with no video open), using
  `usb.dest` / `usb.format` / optional `usb.rename_script` in `omatrack.yml`.
  A newly discovered mount opens the overlay with a read-only *plan*
  (`UsbCopy.h`: per file source → jailed destination, size, New / Existing /
  Invalid) computed on a worker; nothing is written before the button. The
  plan re-computes when the event, destination, format or script changes.
  Copying re-validates each source (size + mtime) and destination against the
  plan, streams into a `.part` temporary in the target directory with a live
  byte counter, publishes with a create-only rename, and preserves the source
  mtime/permissions. Cancel stops between chunks and discards the temporary;
  a target that appears mid-copy is skipped, never overwritten. Existing
  targets are skipped and reported as *unverified* — contents are not
  compared. Two sources resolving to one target invalidate the whole plan.
  `expandCopyFormat()` leaves unknown `{tokens}` in place (the plan reports
  them) and `jailRelativePath()` canonicalizes the nearest existing ancestor
  so a symlink above a not-yet-created folder cannot escape the destination.
  Acceptance builds accept `OMATRACK_AUTOTEST_USB_ROOT` as a stand-in mount.
  Source files stay immutable.
- Remote connections are synchronized into a local discovery cache containing
  zero-byte source stubs and ETag metadata; source bytes are never retained as
  a second telemetry cache. Before parsing a remote file, lookup uses its ETag
  in the local machine's
  `$XDG_CACHE_HOME/.omatrack/c/{generation}/{ETag}.telemetry`, then the
  remote root's `.omatrack/c/{generation}/{ETag}.telemetry`. The sidebar pass
  that runs right after a sync stops there: a remote video nobody has
  converted yet is listed as a plain file, not pulled. Clicking it loads the
  complete source once, converts it to native `.telemetry`, writes the local
  mirror, and publishes create-only to the remote cache so the next machine
  finds it in its sidebar pass. (A remote `.pds`/`.ld` is kilobytes and
  converts during the sidebar pass.) A 412 means another client won; use
  that object rather than overwrite it. Discovery remains usable offline from
  its last stubs and ETag metadata.
- Local recordings are never converted or hashed. `.telemetry` files exist
  only for remote objects, and only under `.omatrack/c/`; the ETag in the
  name is the identity, BLAKE3 is used solely to verify a linked video
  against a catalog that carries one.
- `{generation}` is `omatrack::converterGeneration()`: the native format
  version and the pinned `motorsport-telemetry-rs` revision
  (`10-c1ac439d99d9`), derived by the bridge's `build.rs` from its own
  `Cargo.toml`. A normalization is only trusted if the converter that wrote
  it is the one this build links, so advancing the pin regenerates every
  cache instead of serving a file an older decoder produced. Other
  generations under the local root are pruned at the start of every scan;
  remote objects of older generations are left in place (publication is
  create-only) and simply never read again.
- Video is never downloaded by a sync. One onboard recording runs 5–30 GB
  against telemetry's kilobytes, so the sync writes a zero-byte stand-in at the
  cache path — which keeps discovery, pairing, pins and recents keyed on a
  local path — and the player is handed a streaming URL instead: presigned
  SigV4 for S3 and GCS, credentials in the URL for WebDAV. Those URLs are
  secrets. Never log one and never put one on screen; use
  `QUrl::toDisplayString()` wherever one has to be shown.
- A streaming URL is signed once per recording and reused until it is close to
  expiring, because a fresh signature on every read is a different string and
  everything asking "is the player already showing this?" would answer no.
  When one stops working anyway — a laptop that slept past the twelve-hour
  window, a connection that went away — `MpvVideoItem` reports
  `sourceExpired()`, the store signs a new address for the same file, and
  playback resumes where it was. Three attempts, then the error stands.
- mpv is given a real streaming cache (`cache=auto`, `cache-on-disk=yes`, a
  1 GiB forward and 512 MiB back demuxer window under
  `$XDG_CACHE_HOME/omatrack/mpv`). That is a playback buffer and nothing more:
  mpv unlinks the file as it creates it, so it never survives a session and
  cannot serve offline playback.
- Offline playback is an explicit per-recording choice, made from the file's
  context menu. The wish is a name in the `offline` array of the connection's
  `index.json`, which survives restarts and re-syncs; the transfer is a
  background job with progress and a cancel, never part of a library scan. A
  downloaded recording plays from disk, is neither counted against the cache
  limit nor evictable by it, and is handed back by unpinning it — or by a
  sync finding that the server replaced it, which restores the stub rather
  than keeping a file that is now the wrong one.
- The cache stays under `cache: {limit: 20 GB}`, evicting the least recently
  opened files first. The budget covers both the per-connection sync caches
  and the shared local `$XDG_CACHE_HOME/.omatrack/c/` tree; the Track
  Atlas snapshot lives outside it. Age is the local
  file's modification time, refreshed only on a deliberate open; recording it
  in `index.json` instead would lose the race with a sync, which reads that
  file once and writes it back minutes later. Every read-modify-write of a
  location's `index.json` goes through its `CacheIndex` (one mutex per cache
  directory) so a pin cannot be lost under a concurrent sync. Stubs,
  `index.json`, downloaded recordings, and the file being played are never
  evicted, and the index deliberately keeps listing what was evicted so the
  next sync re-fetches it rather than reading the gap as a server-side delete.

### Normalization and analysis

`TelemetryEngine` maps vendor channel names to standard concepts and normalizes them into a 50 Hz lap:

- speed in km/h (automatic mapping rejects declared non-speed units; bare
  `speed`/`velocity` aliases never substring-match engine speed)
- throttle, driver throttle, and clutch in `[0, 1]`
- brake pressure in bar, with pedal-position fallback
- steering in degrees
- integer gear
- monotonic cumulative distance in metres
- longitudinal acceleration, four dampers, GPS latitude/longitude in degrees
  (east-positive longitude, including the angular-minute reader convention),
  and reported GPS position/speed accuracy

Native lap distance is accepted only when its continuity and total agree with independently integrated velocity. Otherwise wheel/vehicle speed provides short-term propagation, accuracy-weighted GPS speed removes velocity drift, and good positional GPS fixes anchor the cross-lap track-station map. Poor GPS must not inject position jitter; missing optional channels must not make an otherwise useful lap unloadable.

### Trace workspace

- Overlay an active lap and optional reference lap.
- One `LapFilmstrip` instance moves between docked and fullscreen slots. Both
  roles remain visible when comparing laps of the same recording. Fullscreen
  placement uses mpv's reported display aspect ratio and existing letterboxing;
  when space is insufficient (or PiP would overlap), the video viewport reserves
  a compact bottom lane. The filmstrip never resets selection when reparented.
  Incomplete source laps stay incomplete; similar duration does not establish
  missing boundaries. Index-cache v2 invalidates the older promoted summaries.
- Show a track-station-aligned cumulative delta that starts at zero. The same selected primary→reference map drives every reference trace, cursor value, synchronized video frame, and delta.
- Render standard channels plus opt-in raw source channels.
- Configure channel visibility, color, and lane weight; lanes always fit the pane height with no vertical scrolling or pinning, sized in proportion to channel weight. Right-click a lane for size (double/normal/half) and hide.
- Share one cursor/readout across traces.
- Left-drag selects a range; middle-drag pans; wheel, shift+wheel, and ctrl+wheel zoom; double-click resets zoom; on-screen zoom icons back the gestures. Navigate with mouse and keyboard.
- Keep corner/complex ranges visible without obscuring the data.
- Manual damper alignment is one explicit reference-sync strategy. Offer it only when both laps carry front-damper data; selecting it reveals the compact damper strip and applies its offset through the same shared map lookup as traces, delta, cursor values, and video.

### Embedded video playback

- Open MP4, MOV, MKV, AVI, M4V, and WebM video inside the main analysis workspace; an MP4 containing an AiM `aimd` track is also a telemetry session.
- Render through libmpv's OpenGL Render API in `MpvVideoItem`; never spawn the mpv CLI or embed a foreign native window.
- Place video in the resizable section above the traces. Playback chrome stays minimal: the top-left speaker button toggles persisted audio mute, Space toggles playback, and Left/Right skip the primary recording by 2 seconds. Store the mute preference under `video.muted` in `omatrack.yml`. Fullscreen dual-video compose is split, active-with-reference pip, reference-with-active pip, active only, or reference only (keys 1–5). In pip layouts the main recording is inset so it is not confused with a single full-frame video. `S` toggles 0.25× slow motion on the primary clock (the reference follows). The top bar shows the layout name, then driver / lap N/M / fuel for the active and reference recordings.
- Selecting an AiM video session selects its fastest lap and pauses at the
  telemetry cursor. The primary recording is the clock: it always plays at
  1×, each frame advances the telemetry cursor and traces, and it is never
  rate-corrected or sought to chase telemetry during play. An explicit cursor
  jump still seeks both recordings. Reaching the end of the current lap
  pauses, shows a short next-lap 3-2-1, then selects the next lap in the same
  session and resumes; the reference lap is not changed. Primary/reference
  video consumes the same cached alignment map as traces and delta. A compact
  sync dropdown exposes only strategies supported by both selected laps:
  continuous GPS variable-speed matching (the default), pre-corner GPS,
  pre-corner damper matching, manual damper alignment, and lap percentage as
  the universal fallback. Pre-corner choices appear only when corner zones
  exist; damper choices require both laps to carry damper data. The selected
  strategy is persisted under `video.reference_sync` in `omatrack.yml`.
  The reference hard-seeks to the mapped station on pause and after a primary
  jump; during play it holds 1× through corners and uses the following
  straight to speed up or slow down so both recordings arrive together at the
  next turn-in. Without corners, the continuous map supplies the local
  variable rate. Player time is always MP4 presentation time;
  convert to and from file-relative telemetry nanoseconds with the signed
  per-video presentation offset persisted by `telemetry-format`. Use the
  presentation-order frame table for frame lookup; never derive a frame from
  nominal FPS or infer a packet clock from undocumented bytes. Before applying
  the clock, verify that the companion describes the opened video: BLAKE3 for
  local files, the synchronized object ETag for remote cache entries. A
  missing or mismatched identity disables synchronization and stays visible
  as a warning instead of silently seeking the wrong recording.
- Treat video files as read-only. Parsing and playback must never rewrite embedded telemetry or media.
- Qt Quick must use the OpenGL graphics API before the first window because `QQuickFramebufferObject` and libmpv share that context.
- `OMATRACK_VIDEO=/path/to/video` first attempts to open a telemetry-bearing MP4 session, falls back to standalone playback, and is also used by the GUI acceptance harness.
- Right-clicking a video or its session row opens recording metadata. User
  edits and channel-mapping overrides are stored per video under
  `recording_metadata` in `omatrack.yml`. Driver identity is represented by
  `channels.driver_id` plus numeric-code-to-name entries under
  `driver.mappings`; driver codes may be float-backed or fractional, so never
  coerce them to integers or flatten identity into one driver name. A `*`
  mapping is the fallback for any detected driver ID; an exact numeric mapping
  always wins over it.
  Right-clicking a library folder edits
  that folder's `TRACK.yml`; videos inherit the root-to-leaf merge of all
  parent `TRACK.yml` files, which also inform mapping suggestions. Each mapping
  exposes its canonical unit and a searchable source-channel browser with raw
  units, sample rate, and representative values. When
  editing folder metadata, indexed descendant recordings of every supported
  telemetry format inform driver IDs and identity fields; only a clear
  two-thirds consensus is auto-completed, remains reviewable in the dialog,
  and is not written until Save. When
  development changes that schema, update the known `TRACK.yml` files directly
  instead of adding compatibility branches.
- Opening a video without embedded telemetry clears active/reference laps and
  gives the video the full analysis workspace. Telemetry-bearing videos retain
  the synchronized trace workspace below playback.

### Corner intelligence

- Resolve track and layout against Track Atlas aliases, external IDs, series, and lap length.
- Honor the layout’s label model and default driver-facing labels.
- Treat individual `corner_ranges` and grouped `corner_complexes` as distinct first-class analysis scopes.
- Preserve complex membership and any Track Atlas landmarks instead of reconstructing them in QML.
- Provide single-lap and primary/reference summaries: time, entry/apex/exit speed, gear, steering, brake/lift/turn-in/apex/throttle points, deltas, and trace excerpts.
- Support automatic brake-zone fallback, direct range editing, add/rename/delete of zones, and local user overrides when authoritative data is unavailable.
- Inspect corners in place in the trace workspace: clicking a corner in the trace ruler zooms the viewport onto that corner (centred in the middle of the left half of the trace area), dims the traces outside the corner range, and annotates the zoomed view with brake / turn-in / apex / throttle-pickup markers — small ticks along the bottom that become full-height lines on hover.
- Keep the corner in that position even at start/finish. The viewport is
  allowed to run past the lap, and what lies beyond is either the
  neighbouring lap — masked and dimmed so it can never be confused with the
  lap under analysis, behind a boundary rule labelled `« L8` / `L10 »` — or
  black when there is no such lap. Never re-frame the corner to keep the
  viewport inside the lap.
- Fade in a right-side information overlay with entry/apex/exit speeds, time made on entry and exit, brake-point and turn-in deltas, and a checks text area; Escape or the overlay close button restores the previous viewport.
- Keep Track Atlas cache refresh and offline fallback explicit in preferences; corner edits are copied into `omatrack.yml` as a per-track override.

The current implementation imports `corner_ranges`, downloads the selected
layout centerline to map those ranges onto GPS laps spatially, and inspects
corners in place in the trace workspace. The separate corner inspector window
and its dedicated corner renderer are removed; the corner-scoped damper
alignment window is removed with it, leaving the lap-level damper alignment
strip as the no-GPS fallback tool. `corner_complexes`, full geometry
rendering/modeling, and the rest of the Track Atlas range layers are product
requirements still to be carried through the application model; extend the
model rather than overloading `CornerZone` until it loses meaning.

#### Corner checks are plugins

The comparison notes a driver reads ("turn-in 18m earlier than reference",
"throttle while braking") come from `src/core/CornerAnalysis.*`, ported from
the corner analysis in [tobi/ac-tracer](https://github.com/tobi/ac-tracer)
(`lib/windows/corner_analysis.lua`). That Lua file is the reference for what a
corner comparison should say; treat it as the spec when adding checks.

The pipeline is three steps and one extension point:

```text
measureCorner(lap, start, end)      one pass over the corner's samples
        -> CornerMetrics            entry/apex/exit, brake, turn-in, coast,
                                    trail braking, downshift timing, grip
CornerContext{primary, reference metrics, delta-trace time deltas}
        -> CornerAnalysisRegistry::run()
        -> std::vector<CornerNote>{id, text, severity}
```

- A check is a `CornerAnalyzer` subclass with a stable `id()` and a body that
  reads `CornerContext` scalars. Register it in the registry constructor;
  nothing else changes, because the store renders whatever notes come back.
- Efficiency is the reason for the split. Every scan of the sample arrays
  happens once, in `measureCorner()`; analyzers are O(1) over precomputed
  scalars, so the check list can grow without touching the comparison cost.
  Analyzers never read the raw channel arrays and never allocate per sample.
- `requiresReference()` marks the comparisons. Single-lap corners still run
  the primary-only checks.
- Compile-time registration, deliberately — **not** `QPluginLoader`.
  `omatrack_core` is Qt-free (invariant 1) so the CLI and the unit tests run
  the same analyzers the GUI does; a Qt plugin interface would drag Qt into
  the core and buy nothing for a first-party check list. If out-of-tree checks
  are ever wanted, add a loader in the app layer that calls
  `CornerAnalysisRegistry::add()` — the interface is already the boundary.
- Thresholds are named constants at the top of `CornerAnalysis.cpp` because
  the numbers are the product decision, not the code.
- Deviations from the Lua original, all deliberate and commented at the call
  site: the downshift-reaction check compares against the reference instead of
  an absolute 5 m (every corner of a real LMP2 lap clears 5 m), and the
  brake-pressure check requires a real brake zone on one of the two laps
  before commenting (otherwise a flat-out corner reports "lighter braking
  (0 vs 11 bar)").
- Not ported, because `UnifiedLap` has no channel for them: wheel lockups,
  traction-control interventions, off-track excursions, and rev-limiter hits.
  Those need per-wheel speeds and ECU status flags. Lateral G is mapped
  (`g_lat`) but absent from every local fixture, so turn-in currently runs the
  Lua's steering-only fallback and the combined-grip checks stay silent.

### Portable updates

- Only a portable release build may check for updates or replace itself.
  Linux: launched through `$APPIMAGE`. Windows: the Velopack per-user
  install (`Update.exe` next to `current/`, or a leftover zip tree with
  `qt.conf`). macOS: `Omatrack.app` on Apple Silicon, not from a mounted
  disk image. Source trees stay silent.
- `AppUpdater` (`Updater` in QML) asks GitHub Releases `/latest` on the I/O
  thread, at most once a day, and never while `OMATRACK_AUTOTEST` is set.
  It does not upload session data. `updates.check` in `omatrack.yml` is the
  opt-out; Later writes `updates.snooze_until` one week ahead and keeps the
  header icon so the user can still update.
- Linux: one click downloads `Omatrack-*-linux-x86_64.AppImage` next to the
  running file, verifies `SHA256SUMS.txt`, swaps it in place, and relaunches
  from the same path. The running squashfs mount is left alone.
- Windows: tagged releases ship a Velopack per-user installer
  (`Omatrack-*-windows-x86_64-Setup.exe`, renamed from Velopack's output so
  it reads like the AppImage and dmg) plus the `io.github.tobi.omatrack-*-full.nupkg`,
  which keeps Velopack's `{packId}-{version}` name because `Update.exe` and
  `releases.win.json` depend on it.
  One click downloads the nupkg, verifies `SHA256SUMS.txt`, and hands it
  to `Update.exe apply --waitPid`. No UAC; `current/` is replaced in a
  couple of seconds. A leftover zip install is offered the Setup.exe so
  it can migrate. File associations are HKCU: telemetry formats default
  on (`.pds`, `.ld`, `.ldx`, `.vbo`, `.telemetry` — an `.ldx` opens its
  sibling `.ld`), `.mp4` default off, prompted on first run.
- macOS: one click downloads `Omatrack-*-macOS-arm64.dmg`, verifies
  `SHA256SUMS.txt`, copies `Omatrack.app` off the image, then a helper
  waits for this process to exit, dittos the new bundle over the old one,
  and relaunches with `open`. Intel Macs have no published image.
- Tagged releases are the update channel. `.github/workflows/release.yml`
  writes the AppImage under its final GitHub asset name, embeds
  `gh-releases-zsync|tobi|omatrack|latest|Omatrack-*-linux-x86_64.AppImage.zsync`
  in `.upd_info`, publishes the matching `.zsync`, and puts the AppImage and
  Windows Velopack Setup/nupkg, and macOS dmg SHA-256 in `SHA256SUMS.txt`.
  The publish job refuses to cut a release that is missing any of those.

### Headless tools and automation

The headless commands (`omatrack parse|unify|corners|compare`, dispatched in `main()` before Qt is initialised, so no window, no `omatrack.yml`) are the acceptance surface for parsing, mapping, lap detection and classification, 50 Hz unification, primary→reference alignment (`corners --reference` maps the reference zone through `ComparisonAlignment`, exactly as the GUI does, and prints the basis, anchor count and confidence; `--lap`/`--reference-lap` pick laps by id so two laps of one recording can be compared), corner analysis, and AiM vs `.telemetry` compare. They live in `cli/Headless.*` (`omatrack_headless`, Qt-free); `cli/main.cpp` builds the test-only `omatrack-cli` over the same library for CTest and the benchmark scripts — never installed or packaged. The GUI also has screenshot and paint-benchmark modes driven by `OMATRACK_AUTOTEST*` environment variables.

## Architecture

```text
WebDAV / S3 / GCS server ------------> src/app/RemoteCache
                                           + WebDavBackend, S3Backend, SigV4
                                           remote discovery stubs + ETag
                                           shared local/remote `.omatrack/c`
                                           `{ETag}.telemetry` (remote only)
                                           |
                                           v
                                      TelemetryStore scan
.pds / .ld / .vbo / .mp4(aimd) / .telemetry / future race formats
              |
              v
motorsport-telemetry-rs (pinned Cargo dependencies) vendor parsing
              |
              v
third_party/motorsport-telemetry/bridge       Omatrack panic-safe bulk C ABI
              |
              v
src/core/TelemetryEngine                      Qt-free normalization + laps
              |
              +--------------------+
              |                    |
              v                    v
cli/Headless.cpp                   src/app/TelemetryStore
headless commands (omatrack parse…) sessions, state, cache, Track Atlas
                                           |
                                  +--------+-----------+---------+
                                  |                    |         |
                                  v                    v         v
                           src/app/TraceView  src/app/MpvVideoItem  src/app/*.qml
                           telemetry canvas  libmpv rendering      Material UI

Track Atlas JSONL -----------------------> src/app/TelemetryStore
             independent network/cache path
```

### Build graph

CMake is driven through `CMakePresets.json` (Ninja + ccache): `release` builds
into `./build`, `debug` into `./build-debug` with `QT_QML_DEBUG`, `asan` into
`./build-asan`, and `acceptance` into `./build-acceptance` with the
state-mutating GUI acceptance harness explicitly enabled. Production builds
must not contain that harness. `third_party/CMakeLists.txt` compiles the local
bridge and its Git-pinned `motorsport-telemetry-rs` dependency into a
configuration-local Cargo target directory under the CMake build tree. A
Cargo-pinned `cbindgen` tool derives `omatrack_bridge.h` from the Rust exports
into the CMake build tree before C++ compilation; no handwritten copy of that
ABI exists. CTest registers the bridge tests. Vendor parser tests run in the
upstream project. `src/core` builds the Qt-free
`omatrack_core`, linked by `cli/omatrack_headless` (the `parse`/`unify`/`corners`/`compare` commands, compiled into the Qt app and into the test-only `omatrack-cli`).
`src/app/CMakeLists.txt` declares the `Omatrack` QML module through
`qt_add_qml_module`; QML documents and imported C++ types live together so
`qmllint`, `qmlls`, and `qmlcachegen` resolve `import Omatrack`. Geist fonts
and Qt Quick Controls configuration are the only bundled data resources.
Warnings (`-Wall -Wextra`) come from the `omatrack_warnings` interface target.

| Layer | Paths | Owns | Must not own |
|---|---|---|---|
| Vendor parsers | Git-pinned `tobi/motorsport-telemetry-rs` crates | File validation, memory mapping, chunks, typed decoding, vendor metadata | Qt, UI state, omatrack-specific presentation |
| C ABI | `third_party/motorsport-telemetry/bridge/` | Extension dispatch, opaque handles, format-neutral lap metadata, bulk decode, stable strings, thread-local errors | Vendor decoding, analysis policy, or exceptions/panics crossing FFI |
| Core | `src/core/TelemetryEngine.*`, `src/core/MonotonicSeries.h`, `src/core/ComparisonAlignment.*`, `src/core/TrackAtlasSpatial.*` | Channel mapping, units, lap detection and classification (`classifyLaps`), lazy per-channel decode, resampling, `UnifiedLap`, monotonic-series interpolation, primary→reference alignment strategies, GPS-to-centerline station mapping | Qt types, QML, settings, network access |
| Corner analysis | `src/core/CornerAnalysis.*` | Per-corner metrics and the pluggable checks that produce driver-facing notes | Qt types, UI text layout, Track Atlas fetching |
| App updates | `src/app/AppUpdate.*`, `src/app/AppUpdater.*`, `src/app/WindowsAssociations.*` | GitHub latest-release parse, SHA-256 verify, AppImage replace, Velopack `Update.exe` apply, macOS dmg stage-and-swap, Windows file associations, header/preferences state | Telemetry, Track Atlas |
| Remote protocols | `src/app/WebDavBackend.cpp`, `src/app/S3Backend.cpp`, `src/app/SigV4.*` | Listing a server and signing a request — PROPFIND/XML, ListObjectsV2, AWS Signature Version 4 | Cache layout, eviction policy, anything that outlives one request |
| Renderer | `src/app/TraceView.*`, `src/app/TraceLaneLayout.*`, `src/app/TraceInteraction.*`, `src/app/TraceSceneBuilder.*`, `src/app/TraceTextCache.*`, `src/app/TraceSnapshot.h` | Scene-graph geometry generation for the trace surfaces (one shared `envelopePolyline` decimator), lane layout, direct trace interaction; every surface reads store state through the read-only `TraceSnapshot` | Parsing, network access, persistent product state, `friend` access into the store |
| Video sync | `src/app/VideoSyncController.*` | Primary/reference playback timing: reference rate, hard seeks, slow motion, next-lap countdown | Layout, media decoding, alignment math |
| Video renderer | `src/app/MpvVideoItem.*` | libmpv lifecycle, OpenGL FBO rendering, playback state, and exact seek | Telemetry extraction, session association, or QML layout policy |
| QML UI | `src/app/*.qml` | Material windows, layout, delegates, controls, high-level orchestration | Full telemetry loops, duplicated analysis, format branches |
| Bootstrap | `src/app/main.cpp`, `src/app/SingleInstance.*` | Qt startup, style, fonts, store ownership, initial properties, module load, single-instance path hand-off; headless command dispatch before Qt | Product analysis, autotest behaviour |
| Session/store | `src/app/TelemetryStore.*` + collaborators `PreferencesStore.*`, `TrackAtlasManager.*`, `OverlayManager.*`, `LibraryModel.*`, `StoreModels.*`/`StoreTypes.h`, `AsyncJob.h` | Lazy session handles, ETag-keyed normalized telemetry cache for remote recordings, selection, cached selectable primary→reference alignment, comparison, viewport; preferences (debounced `omatrack.yml` writer), Track Atlas, MTX overlays, the library tree model and typed row models/gadgets handed to QML; every background pipeline is an `AsyncJob`/`SerialJobQueue` | Pixel-level paint loops, vendor byte parsing, self-update, hand-rolled `QFutureWatcher`/generation counters |
| Headless commands | `cli/Headless.*`, `cli/main.cpp` | Reproducible headless acceptance and inspection, inside `omatrack` and the test-only `omatrack-cli` | A second analysis implementation, Qt |

### Core data contracts

- `RawChannel`: decoded physical samples, unit, sample type, frequency, and duration for one source channel.
- `Lap`: source-session bounds and lap time.
- `UnifiedLap`: same-rate, lap-relative arrays plus distance provenance and GPS-quality channels used by every analysis and rendering feature.
- `SessionHandle`: owns one library identity (the video or vendor file shown
  in the tree) and opens it directly as the parser path — or, for a remote
  recording, the ETag-keyed `.telemetry`. Unified laps stay in memory. Its
  compact `VideoClock` retains the signed per-video presentation offsets,
  presentation-order frame timestamps, linked filenames, and any BLAKE3
  identities after decoded source arrays are released.
- `TelemetryStore`: the single Qt-facing source of truth for active/reference selection, the selected primary→reference alignment map, and UI state.
- `CornerZone`: the current individual corner range. Do not stretch it to represent every Track Atlas layer; introduce explicit domain types when complexes and geometry enter the model.

### Invariants

1. `omatrack_core` remains Qt-free and usable by the CLI.
2. The UI never branches on telemetry format. Add a parser or mapping; do not add `.pds`/`.ld` special cases to QML.
3. Unified channel arrays share the lap’s 50 Hz absolute-time grid. Keep their lengths aligned with `time`; sample through the source clock so late channel starts, dropped samples, and acquisition gaps cannot shift events.
4. Unified distance starts at zero and is monotonic.
5. Comparisons use one cached primary→reference map selected from the strategies both laps support. Continuous GPS position matching is the default and corrects non-linear drift in speed-fused lap distance; pre-corner GPS and damper strategies interpolate between turn-in anchors; manual damper alignment applies one user offset; lap percentage is the universal fallback. GPS fixes must be accurate, distributed, monotonic matches near the base lap-percentage estimate whose travel direction agrees with the primary's (within 60°, so the other leg of a hairpin or a jittering fix is never an anchor); never pin GPS correction to zero at start/finish, which creates a lap-long low-Hz ramp. Traces, delta, cursor values, and video must consume the same map and the same manual offset. Independent index/distance/time mappings are incorrect.
6. The same cached delta feeds both the plotted trace and numeric cursor readout.
7. Primary means active lap; compare means reference lap. Preserve that semantic and its colors throughout the UI.
8. Track identity and corner metadata come from Track Atlas when available; local edits are overlays, not upstream truth.
9. Parser errors become explicit failures. No Rust panic, C++ exception, or invalid pointer crosses the ABI boundary.
10. Optional channels and optional network data degrade gracefully; silent fabrication does not.
11. A flattened decoded array is not a clock. Lap boundaries, resampling, raw
    channels, and media synchronization must preserve source chunk time bases.
    Telemetry time is integer nanoseconds relative to the file's first sample;
    player time is MP4 presentation time; the only conversion is
    `presentation = telemetry + signed per-video offset`. Variable-frame-rate
    lookup uses the catalog's presentation-order timestamp table, never
    `seconds × nominal FPS`.
12. QML reaches C++ only through registered types. `TelemetryStore` is the
    `Store` singleton (`QML_NAMED_ELEMENT` + `QML_SINGLETON`), the Omarchy
    palette is the `Theme` singleton, portable updates are the `Updater`
    singleton, and launch inputs arrive as root
    `required property` values through
    `QQmlApplicationEngine::setInitialProperties`. Context properties are
    forbidden: Qt documents them as invisible to `qmllint`, `qmlls`, and the
    Qt Quick Compiler, which is exactly the checking this project relies on.
13. Colors, fonts, and the trace color palette come from the `Style` QML
    singleton. A hex literal or a font-family string in a component is a bug.
14. Every QML file starts with `pragma ComponentBehavior: Bound`, qualifies
    every property access through an `id`, and declares a `required property`
    for each model role a delegate consumes. A component that renders
    store-derived rows owns its own cache and refreshes from a
    `Connections { target: Store }` block rather than having data pushed in.
15. Never declare a function inside a QML function. A singleton call inside a
    nested function declaration silently disables `qmllint`'s semantic
    analysis for the whole document, so the file appears clean while nothing
    is being checked.
16. A view whose delegates contain editors never binds its model to a JS array
    that an edit replaces. Assigning a new array rebuilds every delegate and
    destroys the focused item, so typing stops after one character. Bind the
    model to the row *count* and read the row through the index, so an edit
    re-evaluates bindings instead of recreating items.
17. The GUI event loop does no I/O. File reads and writes, HTTP, directory
    walks, parse, unify, Track Atlas refresh, GitHub update checks, AppImage
    downloads, and cache clear belong on a
    worker: HTTP on the dedicated I/O thread via `QPromise`/`QFuture`, the
    rest on `QtConcurrent`. A nested `QEventLoop` must not drive or wait
    for that work. Long jobs take an `IoCancel` so a rescan or a window
    close can abort in-flight requests. The loop applies results and paints.
18. Every background pipeline in the app layer is an `AsyncJob<T>` (latest
    wins: one watcher, one generation, one `IoCancel`, `running()` drives
    the loading property) or a `SerialJobQueue<T>` (one in flight, FIFO).
    Results are delivered after `running()` has already dropped to false, so a
    callback may start the next run. Never add a bare `QFutureWatcher` or a
    `*Generation_` counter to `TelemetryStore` or its collaborators.
19. Incremental model updates consume each old row at most once. Duplicate
    keys must never reuse an already placed prefix row. Library file identity
    is its section plus stable path, not the metadata-derived session key;
    the same recording can appear in Recent and a folder simultaneously.
    Rows cross into QML as typed models and value types: `QAbstractListModel`
    subclasses in `StoreModels.*` (laps, channels, corners, driver mappings,
    sync strategies), the `LibraryModel`/`LibraryFilterModel` tree for the
    session library, and `Q_GADGET` rows in `StoreTypes.h`. Filtering is a
    `QSortFilterProxyModel` (`RowFilterModel`, `LibraryFilterModel`), never a
    JavaScript loop over a copied array. `QVariantList`/`QVariantMap` remain
    only for the metadata dialogs and `libraryLocations()`; do not add more.
20. Renderers read the store through `TelemetryStore::traceSnapshot()` (a
    plain struct of const pointers and one alignment mapper) — no `friend`
    access and no private helpers reached from a `QQuickItem`.
21. Preference writes are debounced: mutate the in-memory state and call
    `schedulePreferencesSave()`; the document is serialised on the GUI thread
    and written by a worker, flushed synchronously only on quit.

## Where changes belong

- New file format or source encoding: add/extend it upstream in `motorsport-telemetry-rs`, advance the pinned revision, and expose only generic capabilities through the bridge.
- Load-time or precomputed analysis (laps, units, video/frame sync, catalog
  scalars): implement it in native `.telemetry` (`telemetry-format`). Do not
  invent a parallel JSON/Motec sidecar or re-derive it in QML.
- New cross-format channel or unit rule: `TelemetryEngine` and `UnifiedLap`.
- New lap/corner comparison metric: C++ analysis in the store/core, exposed as compact view data. A new corner *check* is a `CornerAnalyzer` in `src/core/CornerAnalysis.cpp` — never an inline `if` in the store and never a string built in QML.
- Trace-group plugins: `src/app/PluginHost.*`. A plugin is
  `$XDG_CONFIG_HOME/omatrack/plugins/<dir>/plugin.lua` returning
  `{ id, name, version, channels(session), samples(session, keys) }`
  (contract and globals documented at the top of `PluginHost.h`; example in
  `plugins/weather/`). Every call runs in a fresh sandboxed Lua state on an
  `AsyncJob` worker (latest-wins per plugin); nothing Lua ever runs on the
  GUI thread, and state survives only through the plugin's `kv` store
  (`$XDG_CACHE_HOME/omatrack/plugins/<id>/kv.json`). `http.get` blocks the
  worker on the shared I/O thread (`sendFollowing`); `io` is jailed to the
  plugin folder (read) and its cache folder (read/write) through
  `jailRelativePath`. A plugin's result is an `OverlayGroup` with
  `pluginId` set and `sidecar:`-prefixed keys, so lanes, colours,
  visibility and the renderer are the sidecar path unchanged; only
  `OverlayResample.h` (linear interpolation of explicit-time series onto the
  50 Hz grid) is plugin-specific. Enabled ids persist under
  `plugins.enabled` in `omatrack.yml`; a host change clears the group and
  the new session's `channels()` → `samples()` re-attaches it. Time is
  integer nanoseconds: `t` file-relative or `utc_ns` Unix epoch, converted
  through `session.utc_start_ns`; a recording without a wall clock offers
  plugins no absolute time and they must return `{}`.
- USB copy rename: optional Lua 5.4 + sol2 in `src/app` (`LuaRename.*`), never
  in `omatrack_core`. `rename(ctx)` returns a relative path that is jailed
  under the destination. Embedded Lua symbols must remain hidden: libmpv can
  use a different Lua ABI (LuaJIT 5.1), and executable-level interposition
  crashes its script threads. The Lua test links and initializes both runtimes.
  Example scripts belong in preferences, not plugins.
- New persistent user preference: a typed field on `PreferencesStore` (or
  `AppUpdater` for update state), loaded in `loadPreferences()` and written by
  the debounced `schedulePreferencesSave()` path into `omatrack.yml`; never
  `QSettings`, never a direct `YamlConfig::save()` from a click handler, and
  never write it into telemetry.
- New background work in the app layer: an `AsyncJob`/`SerialJobQueue`
  member on the owning collaborator (`TelemetryStore`, `TrackAtlasManager`,
  `OverlayManager`, `PreferencesStore`), never a bare `QFutureWatcher`.
- New rows for QML: a `Q_GADGET` in `StoreTypes.h` plus a `QAbstractListModel`
  in `StoreModels.*` (or a `LibraryModel` role), never a `QVariantList`.
- New Material control, inspector, or layout: QML.
- New high-frequency visual: `TraceView` or another focused C++ Quick item, with measured frame cost.
- New track metadata: contribute it to Track Atlas. Omatrack should consume the upstream result.
- New Track Atlas layer support: parse it into a typed application model, retaining IDs, labels, ranges, members, and landmarks.
- New blocking I/O: the existing I/O thread or `QtConcurrent`, never the
  GUI event loop and never a nested `QEventLoop`.

Do not fix parser ambiguity with filename-specific UI conditionals. Do not copy analysis into JavaScript for convenience. Do not move cold UI layout into C++ without a measured reason.

## Build and run

Requirements: CMake 3.21+, `pkg-config`, libmpv and libyaml development files, a C++17 compiler, Qt 6.5+ (`Core`, `Gui`, `Quick`, `QuickControls2`, `Qml`, `Network`), and Rust/Cargo 1.84+.

```sh
cmake --preset release
cmake --build --preset release

./build/omatrack /path/to/telemetry-directory
./build/omatrack --verbose /path/to/telemetry-directory
OMATRACK_VIDEO=/path/to/onboard.mp4 ./build/omatrack /path/to/telemetry-directory
./build/omatrack parse /path/to/copied-session.pds
./build/omatrack unify /path/to/copied-session.pds \
  --output /tmp/session.unified.csv
./build/omatrack corners /path/to/active.pds \
  --reference /path/to/reference.pds --zone 0.30:0.36
./build/omatrack corners /path/to/session.mp4 --lap 3 \
  --reference /path/to/session.mp4 --reference-lap 1 --zone 0.30:0.36
./build/omatrack compare /path/to/aim-extract.mp4 \
  /path/to/.video.mp4.telemetry
```

While iterating on a change, compile and run the GUI with `make run-debug`:
it builds the `debug` preset and launches the app on the current desktop —
inside a Herdr terminal that is a dedicated right split so run logs stay
visible, and a plain launch otherwise.

`./build/omatrack --verbose` (or `OMATRACK_VERBOSE=1`) enables the
`omatrack.io` and `omatrack.seek` log categories: library path versus parser
path, cache hits and misses, writes, the frame/location of each seek, and
an AiM vs `.telemetry` dump of GPS, main channels, laps, presentation
offset, and video frames. It also sets `QT_FORCE_STDERR_LOGGING=1` so the
lines are not swallowed by journald. Signed stream URLs are logged without
userinfo or query.

`omatrack unify` requires an explicit output path, refuses to overwrite it, and includes GPS coordinates when available. Treat the exported CSV as sensitive location data.

Windows release zips use a flat layout: the executables and their load-time
DLLs install at the archive root next to a generated `qt.conf`, while Qt
plugins, QML modules, and docs land under `lib/`. `scripts/package-windows.sh`
builds, stages, and zips that layout from the `release` preset:

```sh
./scripts/package-windows.sh   # dist/omatrack-<version>-windows-x86_64.zip
```

Other presets: `debug` (`./build-debug`, `QT_QML_DEBUG` for `qmlprofiler`) and
`asan` (`./build-asan`, ASan + UBSan).

Checks. Every linter is a CMake target and a CTest entry, so one command covers
formatting, static analysis, and the Rust bridge tests:

```sh
ctest --preset release
ctest --test-dir build -L lint
cmake --build --preset release --target lint
```

The individual targets stay available: `cpp_format_check` / `cpp_format`
(clang-format), `qml_format_check` / `qml_format` (qmlformat via
`cmake/QmlFormatCheck.cmake`, since qmlformat has no `--check`), `qml_lint`
(drives the generated `all_qmllint`), `rust_clippy`, and `rust_format_check`.
Configure with `-DOMATRACK_CLANG_TIDY=ON` to add `cpp_tidy` and the
`lint-cpp-tidy` test; the curated set in `.clang-tidy` still reports findings in
the existing sources, so it is opt-in until those are burned down.

Use the Qt 6 tools through `/usr/lib/qt6/bin/`, never through `PATH`: Arch's
`qt5-declarative` owns `/usr/bin/qmllint` and `/usr/bin/qmlformat`, and the Qt 5
binaries reject this project's QML outright — `qmlformat` exits 1 with no output
and `qmllint` misses real errors. The CMake targets already use the versioned
paths.

`qmllint` currently reports only `[compiler]` warnings, all of them caused by
`QVariantList`/`QVariantMap` crossing into QML; those bindings cannot be
compiled ahead of time until the store exposes typed value types. Anything
else appearing there is a regression.

Note for Arch/Omarchy: Qt is built with journald support, so `qWarning()` —
including every `AUTOTEST …` line the acceptance harness prints — goes to the
journal, not stderr. Prefix runs with `QT_FORCE_STDERR_LOGGING=1` or the
harness looks silent.

## Verification

Use real, copied telemetry for the format and behavior being changed. Synthetic samples are useful for narrow math checks but are not evidence that a vendor file still parses.

### Parser, bridge, or core

1. Build `omatrack` (or the test-only `omatrack-cli`, which needs no Qt link).
2. Run `omatrack parse` on each affected format; inspect format, mapped channels, and detected laps.
3. Run `omatrack unify` when mapping, units, resampling, distance, or lap bounds changed; inspect sample count and physical plausibility.
4. Run the relevant Rust crate tests, then the workspace tests for shared-trait or bridge changes.

### Store, Track Atlas, or comparison logic

1. Open a real multi-lap session.
2. Select active and reference laps with different lengths.
3. Exercise cursor, distance delta, raw channels, corner selection, and alignment as applicable.
4. For Track Atlas work, verify a cached/offline start and a known track/layout match. Verify both individual corners and complexes when the changed model supports them.
5. For a corner check, run `omatrack corners` on two real laps of the same
   track and read the notes: they must be true of that driving, and a corner
   the driver did nothing wrong in must stay silent. A check that fires on
   every corner is noise, not analysis. `corner-analysis-test` covers the
   thresholds with synthetic corners.

### UI and renderer

The production `release` preset excludes the GUI acceptance harness. Configure
the dedicated preset before running UI automation; the base autotest opens the
first session’s fastest lap, renders the app, saves a screenshot, and exits.

The telemetry surfaces are scene-graph geometry, which the offscreen
platform's software adaptation cannot draw, so a renderer run needs a real GL
context — the running compositor is the simplest one:

```sh
cmake --preset acceptance
cmake --build --preset acceptance
OMATRACK_AUTOTEST=/tmp/omatrack.png \
scripts/autotest.sh ./build-acceptance/omatrack --mute /path/to/copied-telemetry
```

`scripts/autotest.sh` keeps the run off the developer's screen: under
Hyprland it creates a headless output whose default workspace is bound to it
(a window on an *inactive* workspace gets no frame callbacks, so the scene
graph never renders it and the libmpv FBO never comes up), floats every
`omatrack-autotest` window (the Wayland app_id the harness announces) there
at 1280×800, and removes the output when the command exits. Write
screenshots under `build-acceptance/screenshots/` so they stay with the
build that produced them and out of `/tmp`. The window still has a real GL context, so traces and video
render and the screenshot is the full frame; nothing is written to
`~/.config/hypr`. Without Hyprland it runs the command as given. `--mute`
silences playback for one process without touching `video.muted`.
An acceptance run never touches the developer's `omatrack.yml`: when
`OMATRACK_AUTOTEST` is set and `XDG_CONFIG_HOME` is not, `main()` points
`XDG_CONFIG_HOME` at `$TMPDIR/omatrack-autotest/config`, so driver renames,
corner edits, pins and the positional scan root land in a scratch document.
Set `XDG_CONFIG_HOME` yourself to test against a specific configuration.

Do not launch acceptance runs bare on the desktop: the window takes focus
and the timing-sensitive video checks become flaky.

`QT_QPA_PLATFORM=offscreen` still runs and still writes a screenshot — use it
for dialogs, windows, loading states and selection logic — but every trace,
overlay, damper strip and video HUD comes out blank there.

Add feature flags as needed:

- `OMATRACK_AUTOTEST_FILMSTRIP=/path/to/copied-recording` (two usable laps;
  optional `OMATRACK_AUTOTEST_FILMSTRIP_VIDEO=/path/to/test-video` exercises
  native mpv aspect/letterbox placement; uses one real shared filmstrip).
  The native check verifies docking/fullscreen identity, two same-session
  roles, label right-click and the swap action. An inactive headless window
  invokes the real shortcut handler without stealing desktop keyboard focus;
  that path does not claim to simulate a hardware key in an inactive window.
- `OMATRACK_AUTOTEST_RESCAN_STATE=1` adds snapshot/cursor/viewport/manual
  alignment preservation checks to the filmstrip test; it also checks that a
  new lap pair clears old tuning and that the sidebar row under the top edge
  survives the rescan (needs a library taller than the window).
- `OMATRACK_AUTOTEST_USB_ROOT=/scratch/mount` with
  `OMATRACK_AUTOTEST_USB_DEST=/scratch/dest` verifies that discovery opens
  the preview with per-file destinations, that nothing is written before the
  button, and that the button copies exactly the planned set.
- `OMATRACK_AUTOTEST_PENDING_OPEN=/path/to/another/copied-recording` verifies
  that a queued file load cannot undo a swap.
- `OMATRACK_AUTOTEST_RESTORE_STATE=1` with `OMATRACK_AUTOTEST_PRIMARY_LAP`
  and `OMATRACK_AUTOTEST_REFERENCE_LAP` verifies the scratch configuration's
  same-recording startup selection before the filmstrip test proceeds.
- `OMATRACK_AUTOTEST_PLUGIN=<id>` with `OMATRACK_AUTOTEST_PLUGIN_FILE=` a
  copied recording carrying GPS and a wall clock (the Daytona VBO fixture),
  `XDG_CONFIG_HOME` holding the plugin under `omatrack/plugins/<id>/`: waits
  for `channels()`, enables the plugin, and requires finite resampled values
  on the lap grid plus the group listed last. The weather example makes a
  live Open-Meteo request, so this check needs the network.
- `OMATRACK_AUTOTEST_COMPARE=1`
- `OMATRACK_AUTOTEST_WINDOWS=1`
- `OMATRACK_AUTOTEST_SELECTION=1`
- `OMATRACK_AUTOTEST_ALIGNMENT=1`
- `OMATRACK_AUTOTEST_CORNER=1`
- `OMATRACK_AUTOTEST_HOVER=1`
- `OMATRACK_AUTOTEST_ZOOM=1`
- `OMATRACK_AUTOTEST_RENAME=1`
- `OMATRACK_AUTOTEST_BRAKE_SYNC=1`
- `OMATRACK_AUTOTEST_CORNER_EDIT=1`
- `OMATRACK_AUTOTEST_LOADING=1`
- `OMATRACK_AUTOTEST_LAP_LOADING=1`
- `OMATRACK_AUTOTEST_LAP_SWITCH=1`
- `OMATRACK_AUTOTEST_VIDEO_METADATA=/path/to/video`
- `OMATRACK_AUTOTEST_CHANNEL_BROWSER=1`
- `OMATRACK_AUTOTEST_FOLDER_METADATA=/path/to/folder`
- `OMATRACK_AUTOTEST_DRIVER_TYPING=/path/to/folder`
- `OMATRACK_AUTOTEST_STANDALONE_VIDEO=1`
- `OMATRACK_AUTOTEST_VIDEO_HUD=1`
- `OMATRACK_VIDEO=/path/to/onboard.mp4`

`HOVER` and `ZOOM` print the average time to build one frame's scene-graph geometry (and its quad count); the GPU draws that batch in a single call, and `QSG_RENDER_TIMING=1` shows the resulting `sync`/`render` cost as 0 ms. Treat 16.67 ms as the hard 60 fps ceiling and 8.33 ms as the design target for continuous interaction. Inspect the screenshot as well; timing alone cannot catch illegible density, overlap, incorrect colors, or stale comparison state.

For visual work, also run the app in the target Linux/Omarchy desktop. Offscreen output does not verify native palette integration, font rendering, window behavior, pointer feel, or high-refresh animation.

Embedded libmpv playback must be verified on the native Linux/Omarchy OpenGL scene graph. The offscreen platform can capture the surrounding QML but does not establish the shared `QQuickFramebufferObject` render context. When `OMATRACK_VIDEO` is set, the native autotest exits non-zero unless the player is ready, the file is loaded, duration is available, the default volume is 75%, keyboard seeks remain mapped, playback advances the telemetry cursor, and media time stays aligned. `OMATRACK_AUTOTEST_BRAKE_SYNC=1` pauses on a real heavy-braking sample so the telemetry cursor can be checked against the video's own lap timer and brake graphic. `OMATRACK_AUTOTEST_RENAME=1` exercises the driver rename dialog and persistence path. `OMATRACK_AUTOTEST_DRIVER_TYPING=/path/to/folder` types real key events into the folder-metadata driver-name editor and the preferences driver rename field, and fails unless both keep keyboard focus and all characters: a model binding that rebuilds delegates per keystroke destroys the focused editor after one character.

## Current boundaries to keep explicit

- The bridge dispatches `.pds`, `.ld`, `.vbo`, AiM `aimd` `.mp4`, native
  `.telemetry`, and JSONL (`mtj` recordings, `mtx` sidecars). Motec `.ld`
  is ingest-only and `.ldx` is not a library session. MTX sidecars are not
  library sessions: they are overlap-joined onto the open host and drawn as a
  collapsible folder of header chrome, span tracks, and sample channels. An
  ordinary MP4 without an `aimd` track remains valid for standalone playback.
  Local files are parsed in place; no `.telemetry` is generated for them.
  Remote recordings are normalized once and keyed by ETag: the local mirror
  is `$XDG_CACHE_HOME/.omatrack/c/{generation}/{ETag}.telemetry`; remote
  publication is create-only at
  `ROOT/.omatrack/c/{generation}/{ETag}.telemetry`. No hidden
  `.{video}.telemetry` companion or old AiM range extract is used.
- Session parsing is lazy at two levels. `TelemetrySource::openIndex()`
  reads channel metadata only and `ensureDecoded()` materialises one channel
  on demand (lap detection, driver id, GPS clock); `open()` decodes every
  channel. `src_` is freed after `adoptLoadedLap()` creates the `UnifiedLap`;
  the file is re-opened on demand by `extraChannelData()` for the opt-in
  raw-channel feature. Do not describe this as streaming — a decoded channel
  is still a whole array.
- Every format crate memory-maps its input (`cosworth`, `aim`, `motec`, `vbo`), but the decoders still materialise whole channel arrays into `Vec<f64>` before the C++ side sees them. The mapping avoids the read copy; it is not a zero-copy channel walker. Pushing zero-copy further would change the C ABI, and the renderer measurements below show sample access is not the frame-budget bottleneck — so do it for memory, if at all, not for frame time.
- `TelemetryStore` still hands QML `QVariantList`/`QVariantMap` for the
  folder/video metadata dialogs and `libraryLocations()`; those are the
  remaining `[compiler]` warnings `qmllint` reports. Everything else crosses
  as a model or a `Q_GADGET` (invariant 19). Convert the rest the same way;
  do not add `var`.
- No QML `Canvas` and no `QQuickPaintedItem` remain. Every telemetry surface —
  `TraceView`, `TraceCursorOverlay`, `DamperStripView`, `VideoTelemetryHud` —
  is a `QQuickItem` that builds its frame in `updatePaintNode()` through
  `TraceSceneBuilder`. Do not add a `Canvas` or a painted item: JavaScript and
  QPainter both draw on a CPU thread and cannot hold the frame budget.
- `TraceSceneBuilder` is the one drawing convention. It batches every
  flat-coloured quad of a surface into a single `QSGGeometryNode`
  (`QSGVertexColorMaterial`, `DrawTriangles`) — one draw call per surface — and
  composites text as `QSGTexture` quads cached by `TraceTextCache` on
  (string, font, colour). Lines are expanded to quads because OpenGL core
  profile clamps `glLineWidth` to 1; edge antialiasing comes from the 4×
  multisample format requested in `main()`. Rotated text goes through
  `rotatedText()`, which parents the quad to a `QSGTransformNode`.
- The batch geometry is **indexed with `QSGGeometry::UnsignedIntType`, and that
  is load-bearing**. The scene graph's batch renderer merges compatible
  geometry into 16-bit indexed batches; a zoomed workspace passes 65535
  vertices and every lane after that point silently disappears from the frame
  — no warning, no error, just missing traces. Declaring 32-bit indices keeps
  the node out of that merge path. Do not "simplify" it back to unindexed
  triangles.
- `TraceView` draws a device-pixel min/max envelope when a column covers
  more than one sample. Once there is less than one sample per device
  pixel it switches to a polyline through the samples, so a zoomed slope
  is a line instead of a staircase of axis-aligned bars. Only each
  channel's vertical range is cached.
- Measured on a full-height Sebring lap, seven lanes, 1280×800 at dpr 1.94
  (`OMATRACK_AUTOTEST_ZOOM=1`, `OMATRACK_AUTOTEST_HOVER=1`): geometry build
  **0.32 ms average, 0.74 ms worst** for ~4900 quads, and **0.006 ms** for the
  cursor overlay's 78 quads. The same lap through QPainter cost 12.6 ms.
  `QSG_RENDER_TIMING=1` reports `sync=0, render=0` per frame — the frame is
  bounded by presentation, not by drawing. The harness therefore benchmarks
  `benchmarkGeometry()`, the CPU half of the renderer, because the GPU half no
  longer registers.
- The scene-graph **software** adaptation cannot render custom geometry nodes;
  it is what `QT_QPA_PLATFORM=offscreen` falls back to without a GL context.
  `TraceSceneBuilder` detects it and emits nothing, so offscreen runs still
  produce screenshots and exercise every non-drawing path, but the telemetry
  surfaces are blank. Renderer verification needs a real GL context:
  `QT_QPA_PLATFORM=wayland` (or any real display) against the running
  compositor.
- The app consumes Track Atlas `corner_ranges` and the selected layout's
  centerline for GPS-based corner station mapping. First-class complexes, full
  geometry rendering/modeling, and the remaining range layers are not wired
  through yet.
- Upstream video synchronization follows `motorsport-telemetry-rs`
  `docs/VIDEO_SYNC.md`: file-relative integer-nanosecond telemetry time maps to
  MP4 presentation time through a checked signed per-video offset, and frame
  lookup uses the persisted presentation-order timestamp table. The bridge
  copies that clock and the linked-video identity into the Qt-free core;
  `SessionHandle` verifies the opened recording before primary/reference seeks
  or playback-to-cursor mapping can consume it.
- AiM GPS availability varies by recording. Preserve upstream `NaN` positions
  for no-fix intervals rather than fabricating a path; recordings with valid
  fixes provide spatially usable Road America coordinates and drive Track
  Atlas corner station mapping.
- `sessionStartUnixTime()`/`hasGlobalTime()` do not currently provide global session time.
- The GUI is file-based post-session analysis today. Future live or database-backed work must preserve the same normalized core instead of bypassing it.
- The app embeds one video. A local `aimd` MP4 is opened directly: the AiM
  source supplies the signed presentation offset and the presentation-order
  frame table, and the opened file is its own identity (`ExactSource`). A
  remote one is converted to the ETag-keyed `.telemetry`, whose catalog keeps
  that clock; conversion reads the complete source only after both cache
  locations miss and never reconstructs timing from FPS or stores a hidden
  companion.
- Configuration migrates from the pre-YAML `QSettings` store, the pre-rename `racecraft.yml`/Track Atlas cache, and per-track corner CSVs on first run; legacy stores are read only and left untouched.
- HTTP (sync, range GET, Track Atlas, video fetch, GitHub update checks)
  runs on a dedicated
  I/O thread via `QPromise`/`QFuture`; parse, unify, scan walk, and cache
  clear run on `QtConcurrent`. That split is invariant 17, not optional.
- Portable Linux AppImages, Windows Velopack installs, and macOS arm64
  app bundles may self-update from GitHub Releases. Source builds must not.

## Definition of done

A change is complete only when it:

- lives in the correct layer and preserves the contracts above;
- works for the affected real format/session, not only a hand-built sample;
- keeps source telemetry immutable;
- preserves dense, coherent primary/reference UX;
- uses Track Atlas rather than local track metadata where applicable;
- demonstrates the relevant rendering path still meets the frame budget; and
- updates this guide when it changes the architecture, product philosophy, or a stated current boundary.
