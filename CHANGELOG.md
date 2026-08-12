# Changelog

All notable user-facing changes are documented here.

## Unreleased

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
