# Optional video file associations

Omatrack plays **MP4, MOV, MKV, AVI, M4V and WebM**. Advertising an
**Open With** capability is not the same as making Omatrack the default player.
Video association is optional; opening a file from Omatrack never requires it.

## Windows

`src/app/WindowsAssociations.*` owns the catalog and the per-user registry
operations. `FileAssociationsDialog.qml` lists each format separately.

| Formats | Install/update default |
|---|---|
| `.pds`, `.ld`, `.ldx`, `.vbo`, `.telemetry` | On, unchanged |
| `.mp4`, `.mov`, `.mkv`, `.avi`, `.m4v`, `.webm` | Off |

An `.ldx` continues to open its sibling `.ld`; it is not a recording itself.
Install/update hooks register only the five telemetry types. They neither
enroll additional videos nor revoke an existing video opt-in. The first-run
prompt explains the defaults and labels every video row **optional**. The
format list scrolls within a bounded area so eleven choices fit a small window.
A toggle re-reads registration so a failed write does not leave a falsely
checked box.

An explicit opt-in writes the format's `Omatrack.<extension>` ProgID, quoted
executable/file open command, icon, its own `OpenWithProgids` value, and the
classic extension default under `HKCU\Software\Classes`. No elevation or
machine-wide registration is used. **The checkbox reports that registration,
not Windows' effective default application.** Windows' protected Explorer
`UserChoice` is never read or rewritten; an existing user-selected player can
still win, and Windows may require the user to choose Omatrack in Open With /
Default apps.

Disabling or uninstalling removes only Omatrack's ProgID, its named
`OpenWithProgids` value, and an extension default **if it still names
Omatrack**. It never deletes the extension tree: other applications' defaults,
MIME metadata and Open With entries survive. Repeated removal is harmless.
There is no saved copy of a former classic extension default; removing
Omatrack's value allows Windows' remaining registrations/user choice to apply,
not a fabricated restoration of another player.

## Linux

`packaging/linux/io.github.tobi.omatrack.desktop` advertises the existing three
telemetry MIME types plus the video capabilities:

- MP4/M4V: `video/mp4` (also `video/x-m4v` for desktop databases using that name).
- MOV: `video/quicktime`.
- MKV: `video/x-matroska`.
- AVI: `video/vnd.avi` and its historical `video/x-msvideo` name.
- WebM: `video/webm`.

The normal `Exec=omatrack %f` file-open path is retained. Existing
`packaging/linux/omatrack.xml` telemetry definitions are unchanged; **do not
redefine generic video MIME types with Omatrack-specific globs**. MIME types
can cover additional suffixes, but this change does not widen Omatrack's
six-extension player admission list.

This is an Open With declaration only. There is no `xdg-mime default`,
`mimeapps.list` rewrite, or automatic selection of Omatrack over another player.
Users may select it themselves through their desktop's Open With controls.

## macOS

`packaging/macos/Info.plist.in` preserves CMake's bundle metadata fields and adds
one document declaration for the six video extensions. Its role is **Viewer**
and `LSHandlerRank` is **Alternate**, never Owner or Default. Extension-scoped
matching avoids a catch-all `public.movie` / `public.video` claim and does not
export competing definitions of third-party movie types. The user chooses
Omatrack with Finder's Open With command; the app does not set Launch Services
defaults.

`src/app/CMakeLists.txt` sets `MACOSX_BUNDLE_INFO_PLIST` to this template.
CMake creates the plist inside the bundle; the release workflow stages the
bundle with `cmake --install`, verifies it and signs it. A separate plist-copy
script is not needed.

Finder delivers `QFileOpenEvent` rather than just command-line arguments.
`FileOpenEvents` in the application bootstrap queues early document-open events
until the store is ready, then uses the same open-and-raise path as the
single-instance handoff. The event handler does not parse or infer telemetry.

## Verification and boundaries

- `tests/windows_associations_test.cpp`: the pure catalog test runs on every
  platform. It asserts the exact five telemetry defaults, all six video
  opt-ins, nonempty labels and unique lowercase extensions.
- Windows-only cases cover per-video enable/disable, quoted open commands,
  install/update defaults, retained opt-ins, idempotent removal, third-party
  defaults/Open With/metadata preservation, untouched UserChoice and rejection
  of unsupported extensions. **Before any production registry call**, each
  case redirects HKCU with `RegOverridePredefKey` into a unique test-only key.
  Redirection failure fails the test closed. Cleanup restores HKCU and removes
  that key. No test should ever run install/uninstall APIs against real OS
  associations.
- Run the configured unit test and read-only packaging checks with:

  ```sh
  cmake --build build --target windows-associations-test
  ctest --test-dir build -R '^windows-associations-test$' --output-on-failure
  uv run --no-project tests/video_associations_packaging_test.py
  desktop-file-validate packaging/linux/io.github.tobi.omatrack.desktop
  ```

  Substitute the build directory used for your configuration. The unit target
  links Qt Test/Core, plus `advapi32` and `shell32` on Windows.
- Linux validation passed the catalog QtTest, all three packaging tests,
  desktop-file validation and formatting checks. Targeted Qt 6 qmllint reported
  no semantic errors; compiler warnings about shadowable AppUpdater members
  remain.
- An offscreen Qt 6.11.2 Material layout check used an in-memory mock Updater
  (no registry calls), showing the top and bottom of the scrollable list at
  680×640. All five telemetry boxes were checked and all six video boxes
  unchecked. This checks layout only, not native registration.
- Native Windows registry cases, the Windows first-run dialog and Finder
  delivery still require Windows/macOS validation. Linux tests and plist
  parsing do not establish those native behaviors. No OS default was changed
  during development.

The association catalog matches the player admission list in
`omatrack::isVideoFile()`, local discovery globs and the file-open filters.
Registering an extension does not establish that a recording contains native
telemetry or a supported image-reader layout.

The current AppUpdater association API does not expose detailed registry-write
errors, and its registry operations remain synchronous. The dialog re-reads the
observed registration after a toggle, but cannot display a detailed failure
reason through that API.
