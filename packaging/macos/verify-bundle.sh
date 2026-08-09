#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <application-bundle>" >&2
  exit 2
fi

bundle=$(cd -- "$1" && pwd -P)
contents="$bundle/Contents"
executable_dir="$contents/MacOS"
failures=0
macho_count=0

while IFS= read -r -d '' link; do
  target=$(readlink "$link")
  if [[ "$target" == /* ]]; then
    echo "Absolute symlink escapes macOS bundle: $link => $target" >&2
    failures=1
  fi
done < <(find "$bundle" -type l -print0)

while IFS= read -r -d '' candidate; do
  if ! file -b "$candidate" | grep -q 'Mach-O'; then
    continue
  fi
  ((macho_count += 1))
  while read -r dependency; do
    [[ -n "$dependency" ]] || continue
    case "$dependency" in
      /System/Library/* | /usr/lib/*)
        ;;
      @executable_path/*)
        relative=${dependency#@executable_path/}
        if [[ ! -e "$executable_dir/$relative" ]]; then
          echo "Unresolved macOS dependency: $dependency" \
            "(required by $candidate)" >&2
          failures=1
        fi
        ;;
      @loader_path/*)
        relative=${dependency#@loader_path/}
        if [[ ! -e "$(dirname "$candidate")/$relative" ]]; then
          echo "Unresolved macOS dependency: $dependency" \
            "(required by $candidate)" >&2
          failures=1
        fi
        ;;
      @rpath/*)
        relative=${dependency#@rpath/}
        if ! find "$contents" -path "*/$relative" -print -quit | grep -q .; then
          echo "Unresolved macOS dependency: $dependency" \
            "(required by $candidate)" >&2
          failures=1
        fi
        ;;
      *)
        echo "Build-machine macOS dependency: $dependency" \
          "(required by $candidate)" >&2
        failures=1
        ;;
    esac
  done < <(
    # LC_ID_DYLIB names the current file; it is not a dependency. Inspect only
    # dylibs the loader will actually open.
    otool -l "$candidate" | awk '
      $1 == "cmd" && ($2 == "LC_LOAD_DYLIB" ||
        $2 == "LC_LOAD_WEAK_DYLIB" ||
        $2 == "LC_REEXPORT_DYLIB" ||
        $2 == "LC_LAZY_LOAD_DYLIB" ||
        $2 == "LC_LOAD_UPWARD_DYLIB") { dependency = 1; next }
      dependency && $1 == "name" { print $2; dependency = 0 }
    '
  )

  while read -r runpath; do
    [[ -n "$runpath" ]] || continue
    case "$runpath" in
      @executable_path/* | @loader_path/* | /System/Library/* | /usr/lib/* | \
        "$bundle"/*)
        ;;
      *)
        echo "Build-machine macOS runpath: $runpath (in $candidate)" >&2
        failures=1
        ;;
    esac
  done < <(
    otool -l "$candidate" | awk '
      $1 == "cmd" && $2 == "LC_RPATH" { getline; getline; print $2 }
    '
  )
done < <(find "$bundle" -type f -print0)

if ((macho_count == 0)); then
  echo "No Mach-O binaries found in $bundle" >&2
  exit 1
fi
exit "$failures"
