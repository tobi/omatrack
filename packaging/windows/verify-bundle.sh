#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <installed-tree>" >&2
  exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# Resolved relative to this script at runtime.
# shellcheck disable=SC1091
source "$script_dir/runtime-policy.sh"

bundle=$1
failures=0
binary_count=0
while IFS= read -r -d '' binary; do
  ((binary_count += 1))
  while IFS= read -r dependency; do
    [[ -n "$dependency" ]] || continue
    if find "$bundle" -type f -iname "$dependency" -print -quit | grep -q .;
    then
      continue
    fi
    if ! omatrack_windows_system_library "$dependency"; then
      echo "Unbundled Windows dependency: $dependency" \
        "(required by $binary)" >&2
      failures=1
    fi
  done < <(objdump -p "$binary" | awk '/DLL Name:/{print $3}')
done < <(
  find "$bundle" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0
)

if ((binary_count == 0)); then
  echo "No Windows binaries found in $bundle" >&2
  exit 1
fi
exit "$failures"
