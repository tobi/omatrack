#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <extracted-AppDir>" >&2
  exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# Resolved relative to this script at runtime.
# shellcheck disable=SC1091
source "$script_dir/runtime-policy.sh"

bundle=$(realpath "$1")
failures=0
declare -a elf_files=()
declare -A bundled_elf_names=()
origin_token=\$ORIGIN
braced_origin_token=\$\{ORIGIN\}

# Check the packaged dependency names instead of asking the host loader to
# resolve them. ldd may prefer a runner library even when the same SONAME is
# present in an AppImage and selected by its packaged loader at runtime.
while IFS= read -r -d '' candidate; do
  if ! readelf -h "$candidate" >/dev/null 2>&1; then
    continue
  fi
  elf_files+=("$candidate")
  bundled_elf_names["${candidate##*/}"]=1
done < <(find "$bundle" \( -type f -o -type l \) -print0)

if ((${#elf_files[@]} == 0)); then
  echo "No ELF binaries found in $bundle" >&2
  exit 1
fi

for candidate in "${elf_files[@]}"; do
  dynamic=$(readelf -d "$candidate")

  while IFS= read -r dependency; do
    [[ -n "$dependency" ]] || continue
    if [[ -z "${bundled_elf_names[$dependency]+present}" ]] && \
      ! omatrack_linux_system_library "$dependency"; then
      echo "Unbundled Linux dependency: $dependency" \
        "(required by $candidate)" >&2
      failures=1
    fi
  done < <(
    sed -n 's/.*(NEEDED).*Shared library: \[\([^]]*\)\].*/\1/p' \
      <<<"$dynamic"
  )

  while IFS= read -r search_paths; do
    if [[ -z "$search_paths" || "$search_paths" == :* || \
      "$search_paths" == *: || "$search_paths" == *::* ]]; then
      echo "Unsafe Linux runtime search path: $search_paths" \
        "(declared by $candidate)" >&2
      failures=1
      continue
    fi
    IFS=: read -r -a entries <<<"$search_paths"
    for entry in "${entries[@]}"; do
      case "$entry" in
        "$origin_token" | "$origin_token"/* | "$braced_origin_token" | \
          "$braced_origin_token"/*) ;;
        *)
          echo "Unsafe Linux runtime search path: $entry" \
            "(declared by $candidate)" >&2
          failures=1
          ;;
      esac
    done
  done < <(
    sed -n \
      's/.*(\(RPATH\|RUNPATH\)).*Library \(rpath\|runpath\): \[\([^]]*\)\].*/\3/p' \
      <<<"$dynamic"
  )
done

exit "$failures"
