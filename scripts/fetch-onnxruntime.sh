#!/usr/bin/env bash
# Public CPU SDK only. No model, credentials, footage, or system installation.
# Prints the verified SDK prefix for ONNXRUNTIME_ROOT.
set -euo pipefail
version=1.23.2
platform=${1:-}
if [[ -z "$platform" ]]; then
    case "$(uname -s)/$(uname -m)" in
        Linux/x86_64) platform=linux-x64 ;;
        Linux/aarch64) platform=linux-aarch64 ;;
        Darwin/arm64) platform=osx-arm64 ;;
        *) echo 'Specify a supported platform: linux-x64, linux-aarch64, osx-arm64' >&2; exit 2 ;;
    esac
fi
case "$platform" in
    linux-x64) expected=1fa4dcaef22f6f7d5cd81b28c2800414350c10116f5fdd46a2160082551c5f9b ;;
    linux-aarch64) expected=7c63c73560ed76b1fac6cff8204ffe34fe180e70d6582b5332ec094810241e5c ;;
    osx-arm64) expected=b4d513ab2b26f088c66891dbbc1408166708773d7cc4163de7bdca0e9bbb7856 ;;
    *) echo 'Unsupported SDK platform; Windows CI uses the native MSYS2 package.' >&2; exit 2 ;;
esac
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=${OMATRACK_ORT_DOWNLOAD_DIR:-"$root/build-onnxruntime"}
mkdir -p "$work"
work=$(cd -- "$work" && pwd)
name="onnxruntime-$platform-$version"
archive="$work/$name.tgz"
hash_file() {
    if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}
if [[ ! -f "$archive" || $(hash_file "$archive") != "$expected" ]]; then
    temporary=$(mktemp "$work/download.XXXXXX")
    trap 'rm -f "$temporary"' EXIT
    curl --fail --location --silent --show-error --retry 2 \
        "https://github.com/microsoft/onnxruntime/releases/download/v$version/$name.tgz" \
        -o "$temporary"
    [[ $(hash_file "$temporary") == "$expected" ]] || { echo 'SDK SHA256 mismatch' >&2; exit 1; }
    mv "$temporary" "$archive"
    trap - EXIT
fi
# Never trust an unverified moving release asset or an arbitrary extraction path.
tar -tzf "$archive" | awk -v prefix="$name/" '{sub(/^\.\//, ""); if (index($0,prefix)!=1 || $0 ~ /(^|\/)\.\.(\/|$)/) bad=1} END {exit bad}'
if [[ -e "$work/$name" && ( ! -f "$work/$name/include/onnxruntime_cxx_api.h" || ! -d "$work/$name/lib" ) ]]; then
    echo "Incomplete SDK directory; move it aside before retrying: $work/$name" >&2
    exit 1
fi
if [[ ! -d "$work/$name" ]]; then
    stage=$(mktemp -d "$work/extract.XXXXXX")
    trap 'rm -rf "$stage"' EXIT
    tar -xzf "$archive" -C "$stage"
    [[ -f "$stage/$name/include/onnxruntime_cxx_api.h" && -d "$stage/$name/lib" ]]
    mv "$stage/$name" "$work/$name"
    rmdir "$stage"
    trap - EXIT
fi
printf '%s\n' "$work/$name"
