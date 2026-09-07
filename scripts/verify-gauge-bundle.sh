#!/usr/bin/env bash
# Verify actual installed/extracted bytes against the app-owned manifest, not
# against a potentially modified manifest read from the package itself.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
[[ $# == 1 ]] || { echo 'usage: verify-gauge-bundle.sh /path/to/installed/models' >&2; exit 2; }
cmake -DMODE=verify "-DBUNDLE_DIR=$1" -P "$root/scripts/gauge-bundle.cmake"
