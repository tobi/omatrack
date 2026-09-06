#!/usr/bin/env bash
# Pinned public fixture for native-runtime smoke tests; never bundled by this helper.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
directory=${OMATRACK_PUBLIC_MODEL_DIR:-"$root/build-hf-model"}
mkdir -p "$directory"
directory=$(cd -- "$directory" && pwd)
model="$directory/gauge-reader.onnx"
expected=97029f70068f4ec276b3d6bc28810763275806f579d91ddd4701b544af392147
revision=8de50124f55dcecf497f1af95b870ddeb1bab128
hash_file() {
    if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}
if [[ ! -f "$model" || $(hash_file "$model") != "$expected" ]]; then
    temporary=$(mktemp "$directory/model.XXXXXX")
    trap 'rm -f "$temporary"' EXIT
    curl -q --fail --location --silent --show-error --retry 2 --max-time 120 \
        "https://huggingface.co/tobil/omatrack-telemetry-reader/resolve/$revision/gauge-reader.onnx" \
        -o "$temporary"
    [[ $(wc -c < "$temporary" | tr -d ' ') == 2213746 && $(hash_file "$temporary") == "$expected" ]] || {
        echo 'Public test model size/SHA256 mismatch' >&2; exit 1;
    }
    mv "$temporary" "$model"
    trap - EXIT
fi
printf '%s\n' "$model"
