#!/usr/bin/env bash
# Build-time only; all bytes are pinned by the application-owned manifest.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
directory=${OMATRACK_MODEL_BUNDLE_DIR:-"$root/build-gauge-models"}
mkdir -p "$directory"
directory=$(cd -- "$directory" && pwd)
args=(-DMODE=fetch "-DBUNDLE_DIR=$directory")
if [[ -n ${OMATRACK_MODEL_BUNDLE_SOURCE_DIR:-} ]]; then
    args+=("-DSOURCE_DIR=$OMATRACK_MODEL_BUNDLE_SOURCE_DIR")
fi
cmake "${args[@]}" -P "$root/scripts/gauge-bundle.cmake" >&2
printf '%s\n' "$directory"
