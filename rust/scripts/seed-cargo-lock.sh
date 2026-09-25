#!/usr/bin/env bash
# Seed rust/Cargo.lock for a workspace whose GPUI stack is YANKED.
#
# gpui-kit 0.6.6, gpui-base, gpui-component, gpui-pre 0.3.6 and gpui-omarchy
# 0.1.3 are yanked in the package index. Cargo still builds yanked versions
# that a lockfile already pins, but it refuses to *select* them during
# resolution, so `cargo generate-lockfile` / `cargo update` fail. The fix is
# to start from the lockfile gpui-omarchy 0.1.3 itself shipped (a known-good
# resolution of the whole stack), turn its root package entry into an
# ordinary registry dependency, and let a plain `cargo build` add only what
# is new (the motorsport-telemetry-rs git crates and our workspace members).
#
# That crate is used only as a source of a known-good lockfile; the workspace
# does not depend on it, and the final `cargo build` prunes its entry. The
# committed Cargo.lock is the real seed: this script is recovery only.
#
# Run once, from anywhere. Never run a blanket `cargo update` afterwards;
# CI builds with --locked.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
registry_src="$(ls -d "${CARGO_HOME:-$HOME/.cargo}"/registry/src/*/gpui-omarchy-0.1.3 | head -n1)"
crate_file="$(ls "${CARGO_HOME:-$HOME/.cargo}"/registry/cache/*/gpui-omarchy-0.1.3.crate | head -n1)"
if [[ -z "$registry_src" || -z "$crate_file" ]]; then
    echo "gpui-omarchy 0.1.3 must be in the cargo registry cache (cargo fetch it once)" >&2
    exit 1
fi
checksum="$(sha256sum "$crate_file" | cut -d' ' -f1)"

python3 - "$registry_src/Cargo.lock" "$here/Cargo.lock" "$checksum" <<'PY'
import re, sys
source, target, checksum = sys.argv[1:4]
text = open(source).read()
blocks = text.split("\n[[package]]\n")
out = []
for block in blocks:
    if re.match(r'name = "gpui-omarchy"\nversion = "0\.1\.3"\n(?!source)', block):
        lines = block.split("\n")
        # name, version, then registry source + checksum like every other entry
        lines[2:2] = ['source = "registry+https://github.com/rust-lang/crates.io-index"',
                      f'checksum = "{checksum}"']
        # tempfile is a dev-dependency of gpui-omarchy, not a dependency of the
        # published crate as a consumer sees it.
        lines = [l for l in lines if l.strip() not in ('"tempfile",',)]
        block = "\n".join(lines)
    out.append(block)
open(target, "w").write("\n[[package]]\n".join(out))
PY

cd "$here"
# Not --locked: cargo must add the git crates and workspace members.
cargo build --workspace
echo "seeded $here/Cargo.lock"
