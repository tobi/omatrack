#!/usr/bin/env bash
# Regression harness: the Rust headless CLI against a frozen baseline.
#
# The baseline (parity/baseline/, gitignored: it holds GPS) is the output of
# the retired C++ oracle, captured on 2026-09-26 from a run where the Rust
# CLI and the oracle were byte-identical (89 cases). Its .telemetry inputs
# were written by the oracle's companion writer; Rust does not write
# .telemetry, so they are kept with the baseline.
#
# Every case runs into $out/rust/; absolute paths are rewritten to tokens
# (@FIXTURES@, @TELEMETRY@, @OUT@) and the tree is compared with `diff -r`
# against parity/baseline/expected/: stdout, stderr, CSVs, exit codes. Any
# difference fails.
#
#   parity/run.sh               compare
#   parity/run.sh --rebaseline  accept the current output as the baseline
#                               (a deliberate behaviour change: give the
#                               reason in the commit)
#
# $out is $PARITY_OUT when set, else `parity-out/` inside a private
# CARGO_TARGET_DIR, else the gitignored rust/parity/out/. Runs sharing a
# directory are serialized by a lock on it. Fixtures: $OMATRACK_FIXTURES
# (default ~/Documents/Telemetry/26T07_PLM), read-only.
#
# Excluded on purpose: `--version` and non-numeric --lap/--zone values (the
# oracle aborted on them; the port prints usage and exits 2).
set -euo pipefail
rust_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rebaseline=0
[[ ${1:-} == --rebaseline ]] && rebaseline=1
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
baseline="$rust_dir/parity/baseline"
fixtures="$(cd "${OMATRACK_FIXTURES:-$HOME/Documents/Telemetry/26T07_PLM}" && pwd)"

[[ -d "$baseline/expected" && -d "$baseline/telemetry" ]] ||
    { echo "no baseline at $baseline (expected/ and telemetry/)" >&2; exit 2; }
(cd "$rust_dir" && CARGO_TARGET_DIR="$target_dir" cargo build --quiet --release --locked -p omatrack-cli)
port="$target_dir/release/omatrack-cli"

mapfile -t mp4s < <(find "$fixtures" -type f -iname '*.mp4' | sort)
((${#mp4s[@]} >= 2)) || { echo "need at least two MP4 fixtures under $fixtures" >&2; exit 2; }

mkdir -p "$out"
# One run per output directory at a time.
exec 9>"$out/.lock"
flock 9
rm -rf "$out/rust" "$out/normalized"
mkdir -p "$out/rust"

telemetry=()
for f in "${mp4s[@]}"; do
    t="$baseline/telemetry/$(basename "${f%.*}").telemetry"
    [[ -s "$t" ]] || { echo "baseline lacks $(basename "$t")" >&2; exit 2; }
    telemetry+=("$t")
done

cases=0
# run_case NAME ARGS...: argv[0] "omatrack-cli", cwd = $out/rust.
run_case() {
    local name="$1"
    shift
    cases=$((cases + 1))
    local code
    set +e
    (cd "$out/rust" && exec -a omatrack-cli "$port" "$@" >"$name.stdout" 2>"$name.stderr")
    code=$?
    set -e
    echo "$code" >"$out/rust/$name.code"
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

# Paths differ between machines and output directories; compare tokens.
cp -r "$out/rust" "$out/normalized"
find "$out/normalized" -type f -exec sed -i -e "s#$baseline/telemetry#@TELEMETRY@#g" \
    -e "s#$out#@OUT@#g" -e "s#$fixtures#@FIXTURES@#g" {} +

if ((rebaseline)); then
    rm -rf "$baseline/expected"
    cp -r "$out/normalized" "$baseline/expected"
    echo "parity: baseline replaced ($cases cases)"
elif diff -r -x '.*' "$baseline/expected" "$out/normalized" >"$out/diff.txt"; then
    echo "parity: $cases cases, 0 diffs against the baseline (stdout, stderr, CSV, exit codes)"
else
    echo "parity: DIFFERENCES in $(grep -c '^diff\|^Only' "$out/diff.txt" || true) files ($cases cases); see $out/diff.txt" >&2
    head -n 60 "$out/diff.txt" >&2
    exit 1
fi
