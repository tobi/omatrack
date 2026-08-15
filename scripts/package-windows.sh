#!/usr/bin/env bash
# Assemble a local Windows zip from a fresh release build.
#
# Tagged GitHub releases ship a Velopack installer instead; this script is
# the offline/dev zip. Run from the MSYS2 UCRT64 shell:
#   ./scripts/package-windows.sh
#
# Produces dist/omatrack-<version>-windows-x86_64.zip with a flat layout:
#
#   omatrack.exe   omatrack-cli.exe   *.dll   qt.conf
#   lib/plugins/                         Qt platform/format/style plugins
#   lib/qml/                             Qt QML modules
#   lib/share/doc/omatrack/              README, LICENSE, notices
#
# The install tree is already flat thanks to the WIN32 overrides in
# CMakeLists.txt and src/app/CMakeLists.txt; this script only checks the
# contract and zips the staged prefix. No zip binary is required — the
# archive is produced with `cmake -E tar`, which supports the zip format.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$ROOT/dist}"

fail() { echo "error: $*" >&2; exit 1; }

command -v cmake >/dev/null 2>&1 \
  || fail "cmake is required (run from the MSYS2 UCRT64 shell)"

# ── 1. Build the release tree (never re-zip a stale build). ──────────
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  cmake --preset release
fi
cmake --build --preset release --target omatrack omatrack-cli
[ -x "$BUILD_DIR/omatrack.exe" ] \
  || fail "omatrack.exe not found in $BUILD_DIR"

# ── 2. Stage the install tree. ───────────────────────────────────────
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
app="$stage/omatrack"

cmake --install "$BUILD_DIR" --prefix "$app"

# ── 3. Verify the flat release contract before zipping. ──────────────
[ -f "$app/omatrack.exe" ] || fail "omatrack.exe missing after install"
[ -f "$app/omatrack-cli.exe" ] || fail "omatrack-cli.exe missing after install"
[ -f "$app/qt.conf" ] || fail "qt.conf missing (Qt deployment did not run)"
[ -d "$app/lib/qml" ] || fail "lib/qml missing (QML modules not deployed)"
[ -d "$app/lib/plugins" ] || fail "lib/plugins missing (Qt plugins not deployed)"
if [ -d "$app/bin" ]; then
  [ -z "$(find "$app/bin" -mindepth 1 -print -quit)" ] \
    || fail "bin/ still holds files; the flat layout expects them at the root"
fi

# ── 4. Zip the staged prefix. ────────────────────────────────────────
version="$(
  sed -n 's/^project(omatrack VERSION \([0-9][0-9.]*\).*/\1/p' \
    "$ROOT/CMakeLists.txt" | head -n 1
)"
[ -n "$version" ] || fail "could not read the version from CMakeLists.txt"

mkdir -p "$OUT_DIR"
zip_file="$OUT_DIR/omatrack-$version-windows-x86_64.zip"
rm -f "$zip_file"

(cd "$app" && cmake -E tar c --format=zip "$ROOT/$zip_file" .)

# ── 5. Report. ───────────────────────────────────────────────────────
echo "Wrote $zip_file"
(cd "$app" && {
  echo "Top-level content:" >&2
  find . -maxdepth 1 -mindepth 1 | sort >&2
  echo "" >&2
  echo "Total size:" >&2
  du -sh . >&2
})
