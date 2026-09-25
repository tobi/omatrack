#!/usr/bin/env bash
# The Rust workspace gate. Run from anywhere; every step must pass.
#   1. rustfmt        2. clippy -D warnings        3. unit/integration tests
#   4. real-file tests (`real_*`, #[ignore]) against copied AiM MP4s
#   5. byte parity against the C++ oracle CLI, when it has been built
# An exported CARGO_TARGET_DIR is honoured throughout (parity/run.sh runs the
# CLI from it); the oracle always lives in rust/target/oracle.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

step() { printf '\n==> %s\n' "$*"; }

step "cargo fmt --all --check"
cargo fmt --all --check

step "cargo clippy --workspace --all-targets --locked -- -D warnings"
cargo clippy --workspace --all-targets --locked -- -D warnings

step "cargo test --workspace --locked"
cargo test --workspace --locked

fixtures="${OMATRACK_FIXTURES:-}"
if [[ -z "$fixtures" && -d "$HOME/Documents/Telemetry/26T07_PLM" ]]; then
    fixtures="$HOME/Documents/Telemetry/26T07_PLM"
fi
if [[ -n "$fixtures" ]]; then
    step "real-file tests (OMATRACK_FIXTURES=$fixtures)"
    OMATRACK_FIXTURES="$fixtures" cargo test --workspace --locked -- --include-ignored real_
else
    step "real-file tests skipped (no OMATRACK_FIXTURES)"
fi

if [[ -x target/oracle/omatrack-cli ]]; then
    step "parity against the C++ oracle"
    parity/run.sh
else
    step "parity skipped (run parity/build-oracle.sh first)"
fi

printf '\ncheck: OK\n'
