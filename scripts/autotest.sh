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

# Dynamic rules; they do not survive `hyprctl reload`, which is the point —
# nothing is written to ~/.config/hypr. They go in *before* the output exists:
# `default = true` makes the named workspace the one the new output shows, and
# a window on an inactive workspace gets no frame callbacks, so the scene graph
# never renders it, the libmpv FBO never comes up and the video never loads.
# The output mirrors the developer monitors' scale so screenshots come out at
# the same density as a real run.
hyprctl eval "hl.workspace_rule({ workspace = \"$WORKSPACE\", monitor = \"$OUTPUT\", default = true })" >/dev/null
hyprctl eval "hl.monitor({ output = \"$OUTPUT\", mode = \"${OMATRACK_HEADLESS_MODE:-2560x1600@60}\", position = \"auto\", scale = ${OMATRACK_HEADLESS_SCALE:-2} })" >/dev/null
hyprctl eval "o.window({ class = \"^(omatrack-autotest)$\" }, { workspace = \"$WORKSPACE silent\", no_initial_focus = true, float = true, size = \"${OMATRACK_HEADLESS_WINDOW:-1280 800}\", move = \"0 0\" })" >/dev/null

if ! hyprctl monitors -j | jq -e --arg n "$OUTPUT" '.[] | select(.name == $n)' >/dev/null; then
    hyprctl output create headless "$OUTPUT" >/dev/null
    created=1
    # Give the compositor a moment to bring the output and its workspace up.
    sleep 0.5
fi

# A desktop's monitor-restoration hook can replace the default workspace
# during hotplug. Route to the workspace actually shown on the headless
# output, rather than leaving the test on an inactive one with no frame
# callbacks. This does not focus a monitor or switch the user's workspace.
active_workspace=$(hyprctl monitors -j | jq -r --arg n "$OUTPUT" \
    '.[] | select(.name == $n) | .activeWorkspace.name')
if [[ -n "$active_workspace" && "$active_workspace" != null ]]; then
    hyprctl eval "o.window({ class = \"^(omatrack-autotest)$\" }, { workspace = \"name:$active_workspace silent\", no_initial_focus = true })" >/dev/null
fi

export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland}"
export QT_FORCE_STDERR_LOGGING=1
"$@"
