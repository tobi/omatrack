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
elf_count=0

while IFS= read -r -d '' candidate; do
  if ! readelf -h "$candidate" >/dev/null 2>&1; then
    continue
  fi
  ((elf_count += 1))
  # install-qt-action exports its SDK through LD_LIBRARY_PATH. Clear it so the
  # audit exercises only the AppImage RPATHs and the permitted host ABI.
  dependencies=$(env -u LD_LIBRARY_PATH ldd "$candidate" 2>&1 || true)
  while read -r missing; do
    [[ -n "$missing" ]] || continue
    echo "Unresolved Linux dependency: $missing (required by $candidate)" >&2
    failures=1
  done < <(awk '/not found/{print $1}' <<<"$dependencies")

  while IFS=$'\t' read -r name path; do
    [[ -n "$name" && -n "$path" ]] || continue
    if [[ "$path" != "$bundle/"* ]] && \
      ! omatrack_linux_system_library "$name"; then
      echo "Unbundled Linux dependency: $name => $path" \
        "(required by $candidate)" >&2
      failures=1
    fi
  done < <(
    awk '
      $2 == "=>" && $3 ~ /^\// { print $1 "\t" $3 }
      $1 ~ /^\// {
        name = $1
        sub(/^.*\//, "", name)
        print name "\t" $1
      }
    ' <<<"$dependencies"
  )
done < <(find "$bundle" -type f -print0)

if ((elf_count == 0)); then
  echo "No ELF binaries found in $bundle" >&2
  exit 1
fi
exit "$failures"
