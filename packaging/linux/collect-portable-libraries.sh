#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <installed-executable>" >&2
  exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# Resolved relative to this script at runtime.
# shellcheck disable=SC1091
source "$script_dir/runtime-policy.sh"

binary=$1
dependencies=$(ldd "$binary")
if grep -q 'not found' <<<"$dependencies"; then
  grep 'not found' <<<"$dependencies" >&2
  exit 1
fi

while IFS=$'\t' read -r name path; do
  [[ -n "$name" && -n "$path" ]] || continue
  if ! omatrack_linux_system_library "$name"; then
    realpath "$path"
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
) | sort -u
