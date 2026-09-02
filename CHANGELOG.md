# Changelog

All notable user-facing changes are documented here.

## 1.7.2 — 2026-09-02

- Corner drag and hover stay on the cursor overlay. Live geometry no longer
  rebuilds static traces, comparison alignment, the corners inspector, or
  video time on every mouse move. Alignment and inspector stats commit when
  the edit is saved.
- Video playhead is sampled on an 80 ms timer, not libmpv `time-pos`. QML
  binds `sampledMediaTime`. HUD and trace overlay are separate deadline-queue
  keys. USB, import, plugins, filmstrip and event mode are unchanged.

## 1.7.1 — 2026-09-02

- Video publishes its position; overlay, header and traces sample it through
  a deadline queue at display refresh and never run inside the player
  callback, so Windows laptops stop dropping frames. USB, import and plugins
  are unchanged.

## 1.7.0 — 2026-08-31

- Preferences → Plugins: plugin folder, enable/disable, reload, and an
  “Install weather example” button that copies the bundled example into
  the plugin folder (never overwriting an existing plugin).
- Lua trace-group plugins: drop `plugin.lua` into
  `~/.config/omatrack/plugins/<name>/`, enable it under Channels… → PLUGINS,
  and its channels join the trace workspace as an overlay group. Plugins see
  the open session (track, GPS location, wall-clock window, lap) and have
  `http`, jailed `io`, `json` and a persisted `kv` cache; every call runs
  sandboxed on a worker. Example: `plugins/weather` — Open-Meteo temperature,
  precipitation, wind, humidity and pressure across the session.
- Racelogic VBO sessions now report their GPS location to the sidebar (track
  detection, Track Atlas, plugins): the session-location sampler only
  converted radians and rejected arc-minute coordinates as out of range. One
  `gpsCoordinateDegrees()` rule is now shared with unification.
- Parser pinned to `motorsport-telemetry-rs` `c1ac439`: Racelogic VBO files
  now carry a wall clock (`utc_start_ns` from the header date plus the UTC
  time-of-day column, and `File created on … @ …` headers parse), so
  sidecars and plugins can place against them. The converter generation
  advances, so remote `.telemetry` caches regenerate.

- Rescanning discovery no longer destroys and reloads the active/reference lap
  objects or resets their cursor, viewport and manual alignment. Startup now
  restores a reference lap from the same recording as the primary.
- Swapping roles cancels stale pending file/lap loads, preserves the mapped
  cursor and viewport (including neighbour-lap overscroll), and inverts manual
  alignment. Selecting a genuinely different lap pair clears the old tuning.
  Host changes cancel outstanding sidecar work instead of attaching stale joins.
- USB import shows a read-only preview when a stick is discovered — every
  file with its resolved destination and size — and copies only on the button.
  Copies stream with live progress, are cancellable, never overwrite, and
  verify the source did not change. Existing targets are skipped and labelled
  unverified. Fixed a destination-jail escape through a symlink above a
  not-yet-created folder, silent loss of unknown `{tokens}` in the naming
  format, and integers rendering as `1.0` in Lua rename scripts.
- Sidebar, channel list and driver list keep the row under the top edge in
  place across rescans, metadata arrivals and filter changes (shared
  `ScrollAnchor`, replacing the numeric scroll-position workaround).
- Recording metadata follows one precedence rule everywhere: per-recording
  overrides in `omatrack.yml`, then the folder `TRACK.yml` chain (closest
  folder wins), then the recording itself. Track and driver names no longer
  re-read `TRACK.yml` from disk on every sidebar row; a rescan picks up a
  changed `TRACK.yml` for the recordings below it without reloading laps.
- Event mode owns the track/day filter: leaving it restores your previous
  filters instead of leaving the event's track behind, and clearing filters
  also leaves event mode.

- The same lap filmstrip is available in docked and fullscreen playback,
  including both roles when comparing two laps from one recording. It uses
  available letterboxing and reserves a compact lane when needed, without
  covering the player controls or PiP. Current lap ordinals remain visible.
- Keep incomplete source intervals out of complete-lap/best-lap classification;
  duration alone cannot recover missing boundaries. Invalidate index summaries
  cached with the old promotion heuristic.
- USB discovery no longer probes every mounted filesystem (including offline
  NAS mounts). Polling, scans and watch-path discovery run on workers, not the
  GUI thread, and unchanged mount polls do not rebuild the sidebar.
- Fix heap corruption in incremental list updates when a recording appears
  in more than one section. Rows are matched once, file identities include
  their section, and arriving session metadata does not replace the file row.
- Keep the embedded Lua 5.4 sandbox symbols private so they cannot interpose
  libmpv's LuaJIT ABI and crash player initialization.
- Fix the filmstrip label's hidden right-click target and the Main.qml
  formatting failure that blocked the 1.6.1 release jobs.

## 1.6.1 — 2026-08-30

- Header update control shows download percent and bytes next to the icon.
- Sidebar, channels, laps, and other list models refresh with insert/remove
  instead of resetting, so scroll position and selection survive a rescan or
  filter change.
- Event mode filters the library to one track and day, with a first-class
  session name (`c1`, `c2`, …) shown in the toolbar and passed to USB copy
  and Lua rename. Stored in `omatrack.yml`.
- USB volumes are detected (`mountedUsbVolumes()` is wired) and shown as a
  transient `USB — …` sidebar section. Nothing is copied until the Copy
  overlay in the video slot is used. Destination and `{track}/{date}/{session}/{original}`
  live in `omatrack.yml`.
- Optional Lua 5.4 rename sandbox (no `io`/`os`/`load`/`setmetatable`,
  instruction hook plus memory cap) returns a jailed relative path only.
- Location and USB mount watches debounce into the existing scan job.
- Sidebar `openIndex` cache lives under `$XDG_CACHE_HOME` and is keyed by
  POSIX identity plus `converterGeneration()`. Failures are not stored.
  Nothing is written beside the source recording.
- Fullscreen overlay reference traces take color, dash/dot, and a white
  similar-thickness preset from preferences.
- `X` swaps primary and reference analysis roles (no-op without a reference)
  and is offered on the filmstrip label. Text fields keep `X`.

## 1.6.0 — 2026-08-29

- Updated the telemetry readers to the tested `motorsport-telemetry-rs`
  revision `72224ca`. Cosworth recordings retain channel start offsets and
  acquisition gaps rather than compressing missing data; sampling a gap no
  longer returns values from a later chunk. Raw overlapping samples remain
  available instead of being silently dropped.
- VBOX custom units now stay aligned with their channels. GPS crossings of
  a declared start/finish gate can recover laps after a CAN counter reset;
  unreliable GPS falls back to counter/timer recovery. GPS-derived boundaries
  are estimates, not vendor-certified timing.
- Lap recovery respects declared timer units, ignores large timer resyncs
  and transient counter spikes, and accounts for slower lap-counter sampling.
- MoTeC exports that store units in the short-name field regain usable units.
  Native recording migration releases its old mapping before replacing the
  file, fixing access-denied failures on Windows.
- Default driving-channel mapping recognizes vehicle speed and throttle
  pedal channels without mistaking engine RPM for road speed. Incompatible
  speed units are rejected, GPS ground velocity can serve as a fallback,
  and a refuelling-probe flag is not selected as fuel quantity. Angular-minute
  GPS coordinates are normalized to east-positive degrees rather than passed
  through as invalid positions. Synthetic mapping/unification regressions
  cover these real-recording combinations.
- The converter-generation change invalidates remote normalizations made by
  the older readers. Source recordings are not changed.

## 1.5.6 — 2026-08-29

- Filmstrip binds the live lap models, so laps show as soon as a session is
  selected (video already sought to the best lap while the strip stayed empty).
  Incomplete-only sessions fill the lane instead of 30 px stubs.
- Gear is no longer shifted down one on a flying lap that never uses 1st;
  the 2–7 logger encoding is applied only when a 7 is present.
- Lap detection falls back to `Current_Lap_Time` resets and `Previous_LT`
  steps when the transponder/counter is quiet, without grabbing Delta/Ref.
- Assign-track and atlas-slug fields autocomplete the Track Atlas list;
  short venue codes such as `IND` uniquely prefix-match `indianapolis`.

## 1.5.5 — 2026-08-23

- Local recordings are opened directly by the parser on every open: no
  conversion, no hashing, nothing written beside them. Native `.telemetry`
  is generated only for recordings on a connected server, keyed by ETag and
  converter generation, and shared through the remote cache.
- A second `omatrack` launch hands its file to the running instance instead
  of opening a second window; Windows `file:` URL paths and the `.ldx`
  association are handled.
- Built-in dark fallback palette when Omarchy's theme is unavailable; the
  selection colour is never the accent.
- Video: skips apply to the in-flight seek target; render overlays
  revalidate lap pointers; the label column is sized by the visible lanes.
- `--mute` flag and a headless autotest runner (`scripts/autotest.sh`).
- Release tooling pins tagged linuxdeploy releases rather than the moving
  `continuous` asset.

## 1.5.0 — 2026-08-22 (not published)

- Upstream `motorsport-telemetry-rs` advanced to 1.1.0. AiM sample
  timelines now follow the logger's own timestamps instead of a nominal
  rate (previously up to 0.5 s of drift over a 20-minute recording), lap
  boundaries come from the lap timer with the counter's numbering, and the
  fastest lap is always one of the listed laps. Cosworth `Global Time` is
  read as the recording's absolute clock, so PDS files without GPS get a
  real session date. Cross-lap GPS alignment finds two to three times as
  many anchors on real recordings as a result.
- The CLI and the GUI can no longer disagree about laps: every open path
  takes its lap list from upstream, for vendor files and `.telemetry`
  alike. `omatrack-cli corners` gained `--lap` / `--reference-lap` and
  prints the alignment basis, anchor count and confidence.
- Normalized-telemetry caches are keyed by converter generation
  (`.omatrack/c/{format version}-{upstream rev}/`). Advancing the upstream
  pin regenerates every normalization instead of trusting a file an older
  decoder wrote; stale generations are pruned at the start of a scan.
- Lap classification: a trailing recording fragment no longer adopts the
  previous complete lap's reported time, and a double beacon trigger
  collapses onto one crossing instead of leaving a gap between laps.
- GPS alignment anchors must agree with the primary's travel direction
  (within 60°), so the other leg of a hairpin or a jittering fix is never
  an anchor.
- Preference writes can no longer overlap: a slow write landing after a
  newer one could previously revert `omatrack.yml`.
- The acceptance harness runs against a scratch `XDG_CONFIG_HOME` rather
  than the developer's own `omatrack.yml`.
- Raw source channels open the recording index-only instead of decoding
  every channel of the file.
- The headless commands moved into the main binary: `omatrack parse`,
  `omatrack unify`, `omatrack corners`, `omatrack compare` run before Qt is
  initialised, with no window and no `omatrack.yml`. `omatrack-cli` is now a
  test-only binary over the same code and is no longer installed or shipped.
- `omatrack --version` prints the version and exits; previously the GUI
  launched.
- The Windows installer asset is named `Omatrack-<version>-windows-x86_64-Setup.exe`
  like the Linux and macOS assets.
- Restored telemetry-synchronized video seeking when a native recording
  catalogs more than one linked video.
- Added selectable reference synchronization for dual-video comparison:
  continuous GPS variable-speed matching by default, pre-corner GPS or damper
  matching when those inputs exist, manual damper alignment, and lap
  percentage fallback. The same selected map now drives traces, delta,
  readouts, and video, correcting the lap-end drift caused by speed-fused
  distance disagreement.
- Selecting a lap, toggling a channel, or muting video no longer writes
  `omatrack.yml` synchronously on the UI thread; preference writes are
  debounced and land on a worker. Track Atlas cache reads, sidecar sibling
  discovery, `TRACK.yml` writes, and location removal moved off the UI
  thread as well.
- The local normalized-telemetry cache is now counted *and* evicted by the
  configured cache limit; previously it grew without bound.
- Cancelling a library rescan aborts an in-flight WebDAV/S3 listing, and a
  pin/unpin can no longer be lost under a concurrent sync.
- `omatrack-cli corners --reference` maps the reference zone through the
  same primary→reference alignment the GUI uses, and shares the GUI's lap
  classification.
- Upstream `motorsport-telemetry-rs` advanced to 1.0 (`843e2c5`): AiM
  recordings are sampled through their explicit per-sample timestamps, so
  AiM lap boundaries and resampled values are more accurate (they can differ
  from earlier releases by up to ~0.1 s at a lap boundary).

## 1.3.0 — 2026-08-16

- MTX JSONL sidecars (`.ext.jsonl` / `.mtx.jsonl`, plain or zstd) can be
  dropped onto an open lap, video, or traces. If the sidecar timespan
  overlaps the open host, it is joined by integer nanoseconds and appended
  as a collapsible folder: header chrome, span (stint) track, and sample
  channels with the file's default visibility. Matching sidecars are
  found next to the open recording, in Documents, and in configured
  library folders. Host `utc` is taken from the catalog or derived from
  GPS week/iTOW so a weekend biometric or weather file lines up. A span
  is a channel: one lane per `n` (a car, a sleep series, an exercise).
  Overlay traces share the open lap/video clock and zoom; samples
  outside that recording are dropped. Hovering a span shows its
  metadata in a card that follows the pointer above the traces.
  MTX is an overlay, not a library session.
- The sidebar has a filter strip at the bottom: driver and year pills,
  plus a track dropdown. The first pill click is exclusive; later clicks
  add more.
- Fullscreen pip layouts pin the large recording to the left when it is
  the active car, and to the right when it is the reference.
- Video synchronization now uses the native recording's complete clock:
  signed per-video presentation offsets and the presentation-order frame
  timestamp table. Local companions are matched to video by BLAKE3 and remote
  cache entries by their synchronized object ETag before any seek or
  playback-to-cursor mapping. Missing or mismatched identity disables sync and
  shows a warning instead of applying timing to the wrong recording. This also
  removes nominal-FPS frame arithmetic, so variable-frame-rate onboard video
  follows MP4 presentation time directly.

## 1.2.0 — 2026-08-15

- Portable Linux AppImages, Windows Velopack installs, and macOS apps
  check GitHub Releases from the header, keep an update icon after Later
  (which snoozes the prompt for a week), and replace themselves after
  verifying `SHA256SUMS.txt`. Windows uses a per-user Velopack installer
  (no UAC) and `Update.exe` to apply the nupkg. First run asks about file
  associations: `.pds` / `.ld` / `.vbo` / `.telemetry` default on, `.mp4`
  default off. From-source builds do not self-update.

## 1.1.0 — 2026-08-14

- Zoomed traces are polylines through the 50 Hz samples, not one
  axis-aligned bar per pixel. Zoomed-out envelopes still use a min/max
  column, now sized to a device pixel so a 2× display is not twice as
  blocky.
- Native `.telemetry` is the only persisted analysis file. First open of a
  `.pds` / `.ld` / `.vbo` / AiM video writes hidden `.{filename}.telemetry`
  and every later open reads that. JSON sidecars, Motec companions, and the
  session-index cache are gone. Analysis never reopens a Motec `.ld`.
  Load-time and precomputed analysis belongs in `.telemetry`, not a second
  store. Pinned `motorsport-telemetry-rs` now persists presentation offset
  and `video_frames.bin`; a companion written before that is rewritten
  from the AiM extract instead of migrated in place.
- `--verbose` (or `OMATRACK_VERBOSE=1`) logs file opens, cache hits and
  misses, writes, video/cursor seeks, and an AiM vs `.telemetry` dump of
  GPS, main channels, laps, presentation offset, and video frames.
  `omatrack-cli compare <aimd.mp4> <file.telemetry>` prints the same
  report. Library path is kept distinct from the parser path.
- The fullscreen video HUD follows the native `.telemetry` clock at the
  current media time for the whole recording, not only the selected lap.
  Drag uses a pointer handler so the strip moves with the cursor instead
  of sticking in a window-move grab.
- Library sync fetches only hidden `.telemetry` companions. Leftover
  `.json` / `.ld` / `.ldx` sidecars are skipped, a failed companion no
  longer aborts the whole sync, and the cache walk waits until the I/O
  thread has closed the last reply.
- Reaching the end of the current lap while onboard video is playing
  pauses, counts down the next lap (3, 2, 1), then selects that lap and
  resumes. The reference lap is left unchanged. Space cancels.
- Primary onboard video is the clock: it always plays at 1× and each
  frame moves the telemetry cursor. The reference recording snaps to the
  mapped station on pause, then uses the next straight to speed up or
  slow down so both videos arrive together at turn-in. Corners stay at
  1× so a turn is never time-warped.
- Left and Right skip the primary recording by 2 seconds whenever video
  is showing, and the traces follow that seek. They no longer require
  the video pane to be focused.
- Folder listings in the sidebar group recordings by day, oldest first.
  Rows show session and driver instead of the filename, with best lap
  time always on the right and start time, laps, and drive time below.
- Fullscreen video has five compose layouts (hotkeys 1–5): split, active
  with a reference pip, reference with an active pip, active only, and
  reference only. Pip layouts inset the main recording. `S` toggles
  0.25× slow motion. The top bar shows the layout name plus driver,
  lap N/M, and fuel for both recordings. Dragging the telemetry HUD no
  longer jumps to the origin on the first move. HUD reference traces
  follow the mapped compare lap instead of the session clock.
- Corner notes use the same track-station-aligned metres as the overlay
  gauges, so a 3 m later turn-in is no longer reported as 28 m later.
- The fullscreen HUD pedal traces are a window of track progress, not
  time, so primary and reference answer "what was the pedal here?" and
  no longer slide at different speeds. Reference traces are solid, thinner
  and dimmer copies of the same green throttle and red brake colours.
- A live delta bar sits at the centre of the fullscreen video, 15% from
  the top. It can be dragged and resized like the telemetry HUD. The
  number is the accumulated time versus the reference at this station;
  the bar colour is whether that gap is improving right now (relative
  speed). Green is gaining, red is losing, so a car can be behind on the
  number and still show green. The steering wheel no longer repeats ΔT:
  gear and speed sit in the hub, the rim is a 10 px black ring, and
  steering is a white notch (narrower grey for the reference).
- Comparison Δt is time at the same lap-progress station, at 50 Hz (the
  unified grid). It is no longer the leftover of a 5 Hz speed-signature
  warp, and a start/finish-pinned GPS time overlay no longer turns it
  into a lap-long climb. Traces, the cursor, and video still share this
  one map.
- Hovering a corner header shows the time delta to the reference tucked
  against the inside-right of that tab. A dragged range no longer prints
  primary and reference durations, only Δ.

## 0.9.11 — 2026-08-12

- Library sync, Track Atlas refresh, cache clear, and raw-channel reloads
  no longer freeze the window. Network work runs on a dedicated I/O thread;
  a second Rescan cancels the one still in flight.
- A hidden `.<video filename>.json` recording sidecar can now make an external
  MoTeC file and its video one session. The sidecar is synchronized before the
  media, supplies the complete lap list and media seek anchors, and avoids both
  a full video download and a probe of a zero-byte stream stand-in. A lone
  remote AiM MP4 is extracted once; that client writes a hidden
  `.<video filename>.ld` companion create-only so later clients skip the
  video. Lap lists stay in the JSON sidecar. Right-click a connection — in Preferences or on its file-tree
  root — to rescan the server.
- Mounted USB telemetry is discovered automatically. A drive appears as a
  transient `USB — …` sidebar section only when its recursive scan finds a
  supported telemetry or video file, and disappears again after unmounting;
  it is never added to `omatrack.yml`.

- Opening a file selects a representative racing lap. Vendor-supplied
  out, in, and fragment laps are classified the same way as heuristic
  splits — short crossings and poor lap-distance coverage no longer win
  as the session best.

- Switching laps after opening a file keeps the traces. A half-finished
  edit had left the store unable to adopt a loaded lap, so the first
  selection worked and every later one drew an empty workspace.
- A rescan no longer blanks the active and reference laps. The last pair
  is opened again once the library snapshot is in place.
- Corner comparison uses the same primary→reference track-station map as
  traces and delta, instead of remapping the reference zone by raw metres.
- Throttle maps to powertrain TPS when both pedal and TPS exist. Gear is
  sampled as an integer so a 6→3 skip no longer invents 5 and 4.
- GPS gaps stay empty instead of repeating the last fix. Speed units
  accept `kph`/`kmh`. A missing brake or lift point is unset, not zero.
- `omatrack-cli` picks the fastest complete racing lap, not an out-lap
  that happens to be shorter than 30 seconds.
- Right-clicking streamed video opens recording metadata from the cache
  path, not the signed URL. Re-clicking the active file no longer jumps
  back to the fastest lap.
- Left and Right only seek video when the video pane is focused, so they
  still step the trace cursor. Right seeks 15 seconds. Escape leaves
  corner focus from anywhere in the window.
- The header keeps the active driver in the usual muted colour; only the
  “vs …” suffix is orange. Driver ID fields can be cleared. Removing a
  library location asks first.

- S3 listings from Cloudflare R2 now populate the library. R2 percent-encodes
  object keys and only says so after the key list, so the previous parser
  treated every object as outside the prefix and stored an empty cache.
- Server listings now keep portable recording companions
  (`.<video>.json`, `.<video>.ld`) next to the media. A miss on a lone AiM
  MP4 extracts the `aimd` track and publishes those companions create-only so
  other clients skip the parse. Right-click a connection — in
  Preferences or on its file-tree root — to rescan the server.
- Preferences library status now sits in a fixed column so the dots, messages
  and file counts line up, and connection errors wrap instead of eliding.
- The in-place corner panel is a three-column report card: label, bar or
  gauge, signed value. The old Prim / Ref / Δ table is gone; speed uses
  magnitude bars, brake / turn-in / throttle use a centre-zero gauge, and
  gear is a pair of role-coloured tiles.
- S3 and Google Cloud Storage buckets can now be telemetry sources, alongside
  WebDAV. Pick "S3 bucket" or "Google Cloud Storage" from "Connect…", give it
  an `s3://bucket/prefix` or `gs://bucket/prefix` address and an access key,
  and the bucket appears in the library like any other location — synchronized
  into a local cache, reused without downloads, and readable offline. Google
  is reached through its S3-compatible endpoint, so it wants an HMAC
  interoperability key rather than a service-account file. A `region` and an
  `endpoint` can be set per connection, which is also what makes MinIO,
  Cloudflare R2, and Backblaze work.
- A bucket can be connected by pasting one address. The full form —
  `s3://ACCESS_KEY:SECRET_KEY@bucket/prefix?region=eu-west-2&scheme=https&endpoint_override=host`,
  and the same for `gs://` — is understood wherever an address is accepted,
  including a hand-edited `omatrack.yml`, and a WebDAV URL may carry
  `user:pass@` the same way. The keys and settings are lifted straight out
  into the connection's own fields, so what is stored and shown as the address
  stays the plain bucket and prefix. A misspelled parameter is reported rather
  than quietly ignored.
- Onboard video from a server now plays over the network instead of being
  downloaded first. A session's video is thousands of times larger than its
  telemetry, and mirroring one filled the disk to hold something the player
  reads perfectly well over the wire. Video that an earlier version already
  downloaded is handed back on the first synchronization after upgrading.
- Onboard video on a server can be kept for a flight. Right-click a recording
  and choose "Download for offline use": it is fetched in the background with
  progress and a cancel, plays from disk afterwards, survives synchronizations
  and restarts, and is given back from the same menu. Downloaded recordings sit
  outside the cache limit — they are only there because you asked — and
  Preferences reports how much they take.
- A streamed recording no longer dies when the laptop does. Signed addresses
  last twelve hours, and one that expires while the machine is asleep is
  replaced automatically: playback resumes where it was instead of showing an
  error. The same recovery covers a connection that dropped and came back.
- Streamed video now buffers properly. mpv is given a real streaming cache, so
  scrubbing back through the corner you just watched no longer re-fetches it
  over the network.
- The download cache now has a limit — 20 GB unless `cache: {limit: …}` in
  `omatrack.yml` says otherwise — and drops the files least recently opened
  when it is exceeded. Anything dropped is fetched again when it is next
  wanted. Preferences shows how much the cache is holding and can empty it.

## 0.9.6 — 2026-08-10

- The telemetry library is now one list of locations in preferences. Local
  folders and server connections sit in the same list with the same enable
  switch, status, and file count, and each row can be renamed, reordered, or
  disabled without being forgotten. "Connect…" builds its menu from the
  connection types the backend offers, so WebDAV is the first of a set rather
  than a special case. Configuration moved from `telemetry_dirs` plus
  `webdav.connections` to a single `locations` list in `omatrack.yml`, and
  existing files are folded into the new shape on first launch.

- Fixed driver names in the recording/folder metadata dialog losing keyboard
  focus after a single character; the editors now survive model updates.
- Added authenticated WebDAV telemetry sources with streamed, locally cached
  discovery/downloads, ETag reuse, and offline fallback.
- Windows release zips now use a flat layout: `omatrack.exe`, `omatrack-cli.exe`
  and their load-time DLLs sit at the archive root next to `qt.conf`, with Qt
  plugins, QML modules, and docs under `lib/`. `scripts/package-windows.sh`
  assembles the zip from the `release` preset.
- The default telemetry library folder resolves through the system Documents
  location (`Documents/Telemetry`, OneDrive-aware on Windows) and is created
  when missing instead of silently falling back to the home directory.
- Trace lanes always fit the workspace height: no vertical scrolling, no lane
  pinning. Lane size is the channel's weight share, and right-clicking a lane
  offers double/normal/half size alongside hide.
- Trace navigation: left-drag selects a range, middle-drag pans, the wheel
  zooms with or without modifiers, double-click resets the zoom, and on-screen
  zoom icons back the gestures.
- Corners are analysed in place. Clicking a corner in the trace ruler zooms the
  workspace onto it, centred in the middle of the left half, dims the traces
  outside the zone, marks brake, turn-in, apex and throttle pickup along the
  bottom (full-height lines on hover) and fades in a right-side panel with
  entry/apex/exit speeds, time made on entry and exit, brake-point and turn-in
  deltas, and the automatic checks. Escape or the panel's close button restores
  the previous viewport. The separate corner inspector window is gone.
- Every telemetry surface now renders on the GPU. The traces, cursor overlay,
  damper strip and video HUD build Qt scene-graph geometry — one batched draw
  call each, with cached text textures and 4× multisampling — instead of
  rasterising with QPainter. Building a frame of the seven-lane workspace
  costs 0.32 ms on the CPU (was 12.6 ms) and the scene graph reports 0 ms of
  sync and render time per frame.
- Corner comparisons are now produced by a pluggable analyzer system in the
  Qt-free core, ported from the corner analysis in tobi/ac-tracer. Fourteen
  checks read one shared pass of per-corner metrics — entry/apex/exit speed,
  brake point, turn-in, coasting, trail braking, brake ramp rate, throttle
  pickup, downshift timing, throttle-while-braking — and the corner panel
  shows each as a severity-coloured note. `omatrack-cli corners FILE
  --reference FILE --zone start:end` runs the same analyzers headlessly.
- Lateral acceleration (`g_lat`) is mapped into the unified lap and exported
  by `omatrack-cli unify`, feeding lateral-G turn-in detection and combined
  grip where a car logs it.
- A corner at start/finish keeps its place in the left half of the workspace:
  the viewport runs past the lap and shows the neighbouring lap behind a
  labelled boundary rule (`« L8` / `L10 »`), masked and dimmed, or black when
  there is no such lap. The neighbouring laps load in the background.
- Fixed traces disappearing below a certain lane when zoomed in far: the
  scene-graph batch renderer drops geometry past 65535 vertices, so the
  telemetry surfaces now use 32-bit indices.
- Open telemetry and video files interchangeably from the file dialog, by
  dropping them on the window, or from a recent-file menu; the six most
  recent opens persist in `omatrack.yml` separately from configured scan
  roots.

## 0.9.0 — 2026-08-09

- Added cached GPS/speed track-station alignment shared by traces, delta, cursor
  readouts, and synchronized primary/reference video.
- Realign both videos precisely whenever playback pauses, while retaining
  bounded rate correction during continuous playback.
- Added lazy, cached telemetry-library indexing and reliable AiM filmstrip lap
  metadata without full sample parsing.
- Replaced local vendor parsing with the pinned upstream Rust telemetry crates.
- Added the fullscreen telemetry HUD, video/folder metadata workflow, and
  native trace-rendering performance improvements.
- Added downloadable Linux, macOS, and Windows builds through GitHub Actions.

## 0.1.0 — 2026-08-05

Initial public development release.

- Cross-format Pi/Cosworth PDS, MoTeC LD, Racelogic VBO, and AiM MP4 ingestion.
- Shared 50 Hz lap normalization, distance-aligned comparison, and corner analysis.
- Native Qt Quick trace workspace with embedded libmpv playback.
- Track Atlas corner metadata with cache-aware offline behavior.
- Headless parsing and unification inspection through `omatrack-cli`.
- Linux desktop integration and Linux/Windows continuous integration.
