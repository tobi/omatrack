#!/usr/bin/env bash
# Screenshot omatrack2 on a headless Wayland output (no display needed).
#
#   scripts/screenshot.sh [-o out.png] [-s 1920x1080] [-S scale] [-w seconds] [-k keys]...
#
# Runs the release binary inside `cage` (wlroots, headless backend, pixman)
# and captures it with `grim`; both come from nixpkgs via `nix shell`, so
# nothing is installed. Config and cache live in a private directory under
# the target dir (never ~/.config, never beside the telemetry); the library
# points at $OMATRACK_FIXTURES (default ~/Documents/Telemetry/26T07_PLM,
# read-only) and preselects Run4 L10 against Run1 L8 unless
# $OMATRACK_SHOT_CONFIG names another omatrack.yml.
#
# -k sends keys with `wtype` before the capture, in order, 0.6 s apart; each
# value is passed to wtype verbatim (e.g. -k '-M ctrl -k 4 -m ctrl' -k j).
# -w is the settle time after launch (default 10 s: scan + load + analysis).
# -S sets the output scale (default 1); -s 3840x2160 -S 2 renders a 1920x1080
# logical workspace at HiDPI density, the way text looks on a 4K panel.
#
# Needs: nix, the Wayland socket permission of a normal user session. Inside
# an agent sandbox, run it unsandboxed (it binds a Wayland socket).
set -euo pipefail
rust_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$rust_dir/target/shot/shot.png"
size=1920x1080
wait_s=10
scale=1
keys=()
while getopts "o:s:S:w:k:" opt; do
    case $opt in
    o) out="$OPTARG" ;;
    s) size="$OPTARG" ;;
    S) scale="$OPTARG" ;;
    w) wait_s="$OPTARG" ;;
    k) keys+=("$OPTARG") ;;
    *) sed -n '2,22p' "$0" >&2; exit 2 ;;
    esac
done
[[ $out == /* ]] || out="$PWD/$out"

target_dir="${CARGO_TARGET_DIR:-$rust_dir/target}"
[[ $target_dir == /* ]] || target_dir="$rust_dir/$target_dir"
(cd "$rust_dir" && CARGO_TARGET_DIR="$target_dir" cargo build --quiet --release --locked -p omatrack-app)
bin="$target_dir/release/omatrack2"

work="$target_dir/shot/run.$$"
rm -rf "$work"
mkdir -p "$work/cfg/omatrack" "$work/cache" "$(dirname "$out")"
# The Wayland socket path must stay under 108 bytes, so the runtime
# directory is a short temporary one, not under a deep target dir.
xdg="$(mktemp -d "${TMPDIR:-/tmp}/omashot.XXXXXX")"
chmod 700 "$xdg"
fixtures="$(cd "${OMATRACK_FIXTURES:-$HOME/Documents/Telemetry/26T07_PLM}" && pwd)"
if [[ -n ${OMATRACK_SHOT_CONFIG:-} ]]; then
    cp "$OMATRACK_SHOT_CONFIG" "$work/cfg/omatrack/omatrack.yml"
else
    run() { find "$fixtures" -type f -iname "*_$1_*.mp4" | head -1; }
    cat >"$work/cfg/omatrack/omatrack.yml" <<EOF
locations:
  - type: folder
    name: Fixtures
    target: $fixtures
selection:
  primary_key: $(run Run4)
  primary_lap: 10
  compare_key: $(run Run1)
  compare_lap: 8
EOF
fi
cat >"$work/app.sh" <<EOF
#!/bin/sh
export XDG_CONFIG_HOME="$work/cfg" XDG_CACHE_HOME="$work/cache"
exec "$bin" >"$work/app.log" 2>&1
EOF
chmod +x "$work/app.sh"

export XDG_RUNTIME_DIR="$xdg"
unset WAYLAND_DISPLAY DISPLAY HYPRLAND_INSTANCE_SIGNATURE
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    nix shell nixpkgs#cage -c cage -- "$work/app.sh" >"$work/cage.log" 2>&1 &
cage_pid=$!
trap 'kill "$cage_pid" 2>/dev/null || true; wait "$cage_pid" 2>/dev/null || true; rm -rf "$xdg"' EXIT

for _ in $(seq 100); do [[ -S "$xdg/wayland-0" ]] && break; sleep 0.1; done
[[ -S "$xdg/wayland-0" ]] || { cat "$work/cage.log" >&2; exit 1; }
export WAYLAND_DISPLAY=wayland-0 OUT="$out"
nix shell nixpkgs#grim nixpkgs#wlr-randr nixpkgs#wtype -c bash -c '
    set -euo pipefail
    wlr-randr --output HEADLESS-1 --custom-mode "$1" --scale "$4"
    sleep "$2"
    shift 4
    for k in "$@"; do eval "wtype $k"; sleep 0.6; done
    sleep 0.5
    grim "$OUT"
' _ "$size" "$wait_s" -- "$scale" "${keys[@]}" 2>>"$work/cage.log" || { tail -20 "$work/cage.log" >&2; exit 1; }
if ! kill -0 "$cage_pid" 2>/dev/null; then
    echo "omatrack2 exited early:" >&2
    tail -20 "$work/app.log" >&2
    exit 1
fi
echo "$out  (log: $work/app.log)"
