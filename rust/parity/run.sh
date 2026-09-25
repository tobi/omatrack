#!/usr/bin/env bash
# Byte parity of the Rust headless CLI against the C++ oracle.
#
# Runs every case through both binaries (same argv[0], same cwd layout) into
# $out/{cpp,rust}/ and compares stdout, stderr, exported CSVs and exit codes
# with `diff -r`. Any difference fails. Outputs contain GPS positions.
#
# $out is $PARITY_OUT when set, else `parity-out/` inside a private
# CARGO_TARGET_DIR, else the gitignored rust/parity/out/. Runners with their
# own CARGO_TARGET_DIR therefore never share (and wipe) each other's outputs,
# and runs that do share a directory are serialized by a lock on it.
#
# Prerequisites: parity/build-oracle.sh (the oracle and the .telemetry
# writer). Fixtures: $OMATRACK_FIXTURES (default ~/Documents/Telemetry/26T07_PLM),
# read-only; .telemetry companions are written under out/telemetry/ only.
#
# Excluded on purpose: `--version` (the oracle prints its build tag) and
# non-numeric --lap/--zone values (std::stoi/stod abort the C++ process;
# the port prints usage and exits 2 — the documented deviation).
set -euo pipefail
rust_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Cargo resolves a relative CARGO_TARGET_DIR against its working directory,
# which is $rust_dir below; resolve it the same way here.
target_dir="${CARGO_TARGET_DIR:-$rust_dir/target}"
[[ $target_dir == /* ]] || target_dir="$rust_dir/$target_dir"
if [[ -n ${PARITY_OUT:-} ]]; then
    out="$PARITY_OUT"
elif [[ -n ${CARGO_TARGET_DIR:-} ]]; then
    out="$target_dir/parity-out"
else
    out="$rust_dir/parity/out"
fi
[[ $out == /* ]] || out="$PWD/$out"
oracle="$rust_dir/target/oracle/omatrack-cli"
writer="$rust_dir/target/oracle/write-telemetry"
fixtures="${OMATRACK_FIXTURES:-$HOME/Documents/Telemetry/26T07_PLM}"

[[ -x "$oracle" && -x "$writer" ]] || { echo "run parity/build-oracle.sh first" >&2; exit 2; }
(cd "$rust_dir" && CARGO_TARGET_DIR="$target_dir" cargo build --quiet --release --locked -p omatrack-cli)
# A private CARGO_TARGET_DIR builds (and runs) its own CLI; the oracle stays
# under rust/target/oracle either way.
port="$target_dir/release/omatrack-cli"

mapfile -t mp4s < <(find "$fixtures" -type f -iname '*.mp4' | sort)
((${#mp4s[@]} >= 2)) || { echo "need at least two MP4 fixtures under $fixtures" >&2; exit 2; }

mkdir -p "$out/telemetry"
# One run per output directory at a time: a second run waits instead of
# deleting the first one's outputs mid-comparison.
exec 9>"$out/.lock"
flock 9
rm -rf "$out/cpp" "$out/rust"
mkdir -p "$out/cpp" "$out/rust"

telemetry=()
for f in "${mp4s[@]}"; do
    t="$out/telemetry/$(basename "${f%.*}").telemetry"
    [[ -s "$t" ]] || "$writer" "$f" "$t"
    telemetry+=("$t")
done

cases=0
# run_case NAME ARGS...: both binaries, argv[0] "omatrack-cli", cwd = side dir.
run_case() {
    local name="$1"
    shift
    cases=$((cases + 1))
    local side bin code
    for side in cpp rust; do
        [[ $side == cpp ]] && bin="$oracle" || bin="$port"
        set +e
        (cd "$out/$side" && exec -a omatrack-cli "$bin" "$@" >"$name.stdout" 2>"$name.stderr")
        code=$?
        set -e
        echo "$code" >"$out/$side/$name.code"
    done
}

zones_a=(--zone 0.0726:0.1265 --zone 0.17:0.23 --zone 0.45:0.52)
zones_tiling=()
for k in $(seq 0 13); do
    zones_tiling+=(--zone "$(awk -v k="$k" 'BEGIN{printf "%.4f:%.4f", k/14, (k+1)/14}')")
done
zones_atlas=(--zone 0.07265:0.12646 --zone 0.14615:0.19256 --zone 0.19256:0.21445
    --zone 0.21445:0.25596 --zone 0.27888:0.31312 --zone 0.34444:0.36401
    --zone 0.42435:0.50711 --zone 0.50711:0.5618 --zone 0.56565:0.61946
    --zone 0.73015:0.79859 --zone 0.79859:0.86697 --zone 0.86697:0.8939
    --zone 0.8939:0.9531 --zone 0.97583:0.98317)

inputs=("${mp4s[@]}" "${telemetry[@]}")
for i in "${!inputs[@]}"; do
    f="${inputs[$i]}"
    tag="in$i-$(basename "$f" | tr -c 'A-Za-z0-9_.\n-' '_')"
    run_case "parse-$tag" parse "$f"
    run_case "unify-$tag" unify "$f" --output "unify-$tag.csv"
done
# Refuse to overwrite: the CSV from the first unify is still there.
run_case "unify-refuse" unify "${mp4s[0]}" --output "unify-in0-$(basename "${mp4s[0]}" | tr -c 'A-Za-z0-9_.\n-' '_').csv"

for i in "${!mp4s[@]}"; do
    a="${mp4s[$i]}"
    run_case "corners-single-$i" corners "$a" "${zones_a[@]}"
    run_case "corners-single-atlas-$i" corners "$a" "${zones_atlas[@]}"
    run_case "corners-same-recording-$i" corners "$a" --lap 5 --reference "$a" --reference-lap 4 "${zones_atlas[@]}"
    run_case "corners-missing-lap-$i" corners "$a" --lap 99 "${zones_a[@]}"
    run_case "corners-missing-reference-lap-$i" corners "$a" --reference "$a" --reference-lap 77 "${zones_a[@]}"
    run_case "corners-prefix-lap-$i" corners "$a" --lap 3x "${zones_a[@]}"
    for j in "${!mp4s[@]}"; do
        [[ $i == "$j" ]] && continue
        b="${mp4s[$j]}"
        run_case "corners-$i-$j-a" corners "$a" --reference "$b" "${zones_a[@]}"
        run_case "corners-$i-$j-tiling" corners "$a" --reference "$b" "${zones_tiling[@]}"
        run_case "corners-$i-$j-atlas" corners "$a" --reference "$b" "${zones_atlas[@]}"
        for lap in 3 4 6; do
            run_case "corners-$i-$j-lap$lap" corners "$a" --lap "$lap" --reference "$b" --reference-lap 5 "${zones_atlas[@]}"
        done
        run_case "corners-$i-$j-reflap" corners "$a" --reference "$b" --reference-lap 7 "${zones_tiling[@]}"
    done
    run_case "compare-$i" compare "$a" "${telemetry[$i]}"
done
run_case "corners-telemetry-vs-mp4" corners "${telemetry[0]}" --reference "${mp4s[1]}" "${zones_atlas[@]}"

# Error cases.
run_case "err-nonexistent" parse "$out/does-not-exist.mp4"
run_case "err-extension" parse "$out/whatever.xyz"
run_case "err-no-args"
run_case "err-unknown-command" frobnicate
run_case "err-parse-arity" parse
run_case "err-unify-no-output" unify "${mp4s[0]}"
run_case "err-unknown-option" corners "${mp4s[0]}" --bogus 1 --zone 0.1:0.2
run_case "err-missing-zone" corners "${mp4s[0]}" --lap 3
run_case "err-zone-no-colon" corners "${mp4s[0]}" --zone 0.5
run_case "err-zone-last" corners "${mp4s[0]}" --zone
run_case "err-compare-missing" compare "${mp4s[0]}" "$out/missing.telemetry"
run_case "err-unify-bad-source" unify "$out/whatever.xyz" --output x.csv

if diff -r "$out/cpp" "$out/rust" >"$out/diff.txt"; then
    echo "parity: $cases cases, 0 diffs (stdout, stderr, CSV, exit codes identical)"
else
    echo "parity: DIFFERENCES in $(grep -c '^diff\|^Only' "$out/diff.txt" || true) files ($cases cases); see $out/diff.txt" >&2
    head -n 60 "$out/diff.txt" >&2
    exit 1
fi
