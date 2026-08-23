#!/usr/bin/env bash
# Run an Omatrack acceptance command on an invisible Hyprland output.
#
# The renderer needs a real GL context, so the offscreen platform is not an
# option for trace/video checks — but the window does not have to appear on
# the developer's screen. This creates a headless output, routes every
# `omatrack-autotest` window (the app_id the harness announces) to a workspace
# on it, runs the command, and removes the output again.
#
#   scripts/autotest.sh env OMATRACK_AUTOTEST=/tmp/o.png ./build-acceptance/omatrack DIR
#   scripts/autotest.sh ./build-acceptance/omatrack --mute DIR
#
# Every OMATRACK_* variable of the caller is inherited. Outside Hyprland the
# command simply runs as given.
set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <command...>" >&2
    exit 2
fi

if ! command -v hyprctl >/dev/null || ! hyprctl monitors -j >/dev/null 2>&1; then
    export QT_FORCE_STDERR_LOGGING=1
    exec "$@"
fi

OUTPUT=omatrack-headless
WORKSPACE=name:omatrack-autotest
created=0

cleanup() {
    if [[ $created -eq 1 ]]; then
        hyprctl output remove "$OUTPUT" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

if ! hyprctl monitors -j | jq -e --arg n "$OUTPUT" '.[] | select(.name == $n)' >/dev/null; then
    hyprctl output create headless "$OUTPUT" >/dev/null
    created=1
    # Give the compositor a moment to bring the output up before a workspace
    # is bound to it.
    sleep 0.3
fi

# Dynamic rules; they do not survive `hyprctl reload`, which is the point —
# nothing is written to ~/.config/hypr. The output mirrors the developer
# monitors' scale so screenshots come out at the same density as a real run.
hyprctl eval "hl.monitor({ output = \"$OUTPUT\", mode = \"${OMATRACK_HEADLESS_MODE:-2560x1600@60}\", position = \"auto\", scale = ${OMATRACK_HEADLESS_SCALE:-2} })" >/dev/null
hyprctl eval "hl.workspace_rule({ workspace = \"$WORKSPACE\", monitor = \"$OUTPUT\" })" >/dev/null
hyprctl eval "o.window({ class = \"^(omatrack-autotest)$\" }, { workspace = \"$WORKSPACE silent\", no_initial_focus = true, float = true, size = \"${OMATRACK_HEADLESS_WINDOW:-1280 800}\", move = \"0 0\" })" >/dev/null

export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland}"
export QT_FORCE_STDERR_LOGGING=1
"$@"
