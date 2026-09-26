#!/usr/bin/env bash
# One strict lint command for agents, local development and CI.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
exec cargo clippy --workspace --all-targets --all-features --locked -- -D warnings
