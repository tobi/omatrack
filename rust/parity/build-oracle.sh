#!/usr/bin/env bash
# Build the C++ headless CLI (cli/Headless.cpp over src/core + the Rust C ABI
# bridge) as the parity oracle, with no Qt and no libyaml. Output:
#   rust/target/oracle/omatrack-cli        the oracle
#   rust/target/oracle/write-telemetry     .telemetry companion writer
set -euo pipefail
rust_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo="$(cd "$rust_dir/.." && pwd)"
out="$rust_dir/target/oracle"
T="$out/cargo"
GEN="$out/gen"
mkdir -p "$T" "$GEN"

cd "$repo/third_party/motorsport-telemetry"
CARGO_TARGET_DIR="$T" cargo run --quiet --locked -p omatrack-bridge-header -- "$GEN/omatrack_bridge.h"
CARGO_TARGET_DIR="$T" cargo build --quiet --locked --release -p omatrack-bridge

cd "$repo"
g++ -std=c++20 -O2 -Isrc -I"$GEN" -DOMATRACK_VERSION='"oracle"' \
    src/core/*.cpp cli/Headless.cpp cli/main.cpp \
    -Wl,--whole-archive "$T/release/libomatrack_bridge.a" -Wl,--no-whole-archive \
    -lpthread -ldl -lm -o "$out/omatrack-cli"

cc -std=c11 -O2 -I"$GEN" "$rust_dir/parity/write_telemetry.c" \
    -Wl,--whole-archive "$T/release/libomatrack_bridge.a" -Wl,--no-whole-archive \
    -lpthread -ldl -lm -o "$out/write-telemetry"

echo "oracle: $out/omatrack-cli ($("$out/omatrack-cli" --version))"
