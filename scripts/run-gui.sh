#!/usr/bin/env bash
# Launch the Omatrack GUI on the desktop it is launched from.
#
#   scripts/run-gui.sh <binary> [--herdr] [args...]
#
# Desktop targeting (Linux/Wayland, Omarchy/Hyprland). Hyprland already maps a
# freshly launched window onto the focused workspace, but the app restores its
# own window bounds from omatrack.yml, which can resurrect a stale monitor or
# workspace from a previous session. So after the window maps we re-anchor it
# explicitly onto the workspace the launching terminal is on (the active one),
# which is what "same desktop as it is launched from" means here. On any other
# platform this is a plain exec.
#
# Herdr mode (--herdr). When the caller is inside a Herdr pane (HERDR_ENV=1),
# the app is launched from a *named right split* of the calling pane titled
# "omatrack" -- reusing one that already exists, otherwise split --current
# --direction right and rename it. Keep the caller's focus and cwd. Outside a
# Herdr pane --herdr degrades to the same-desktop launch above.
set -euo pipefail

BIN="$1"; shift

HERDR=0
if [ "${1:-}" = "--herdr" ]; then HERDR=1; shift; fi

# Plain launch: place the window on the workspace the terminal is on.
launch_desktop() {
    if ! command -v hyprctl >/dev/null 2>&1 || [ -z "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]; then
        # No Hyprland: just exec; the parent window manager / session handles it.
        exec "$BIN" "$@"
    fi

    # Hyprland: capture the workspace we are launching from and pin the window there.
    ws="$(hyprctl activeworkspace -j 2>/dev/null | jq -r '.name // (.id|tostring) // empty' || true)"
    "$BIN" "$@" &
    pid=$!
    addr=""
    if [ -n "$ws" ]; then
        for _ in $(seq 1 50); do
            addr="$(hyprctl clients -j 2>/dev/null |
                jq -r --argjson pid "$pid" '.[] | select(.pid==$pid) | .address' |
                head -1 || true)"
            [ -n "$addr" ] && break
            sleep 0.1
        done
        if [ -n "$addr" ]; then
            # Move it only if Hyprland placed it on a different workspace (stale restore).
            cws="$(hyprctl clients -j 2>/dev/null |
                jq -r --arg a "$addr" '.[] | select(.address==$a) | .workspace.name' |
                head -1 || true)"
            if [ -n "$cws" ] && [ "$cws" != "$ws" ]; then
                hyprctl dispatch -- movetoworkspace "$ws,address:$addr" >/dev/null 2>&1 || true
            fi
        fi
    fi
    wait "$pid"
    exit $?
}

# Herdr: find-or-create the named right split, launch the app in it, return.
launch_herdr() {
    if [ "${HERDR_ENV:-}" != "1" ] || ! command -v herdr >/dev/null 2>&1; then
        launch_desktop "$@"
        return
    fi

    ws="${HERDR_WORKSPACE_ID:?herdr caller context missing}"
    pane="$(herdr pane list --workspace "$ws" 2>/dev/null |
        jq -er --arg ws "$ws" '.result.panes[]
            | select((.title // "") == "omatrack" or (.label // "") == "omatrack")
            | .pane_id' |
        head -1 || true)"

    if [ -z "$pane" ]; then
        pane="$(herdr pane split --current --direction right \
            --cwd "$PWD" --ratio 0.4 --no-focus 2>/dev/null |
            jq -er '.result.pane.pane_id')"
        herdr pane rename "$pane" omatrack >/dev/null 2>&1 || true
    fi

    # The app is long-running; run it in the pane and let the make target return.
    cmd="cd -- ${PWD@Q} && exec ${BIN@Q}"
    for a in "$@"; do cmd+=" ${a@Q}"; done
    herdr pane run "$pane" "$cmd" >/dev/null 2>&1 || true
    exit 0
}

if [ "$HERDR" = "1" ]; then
    launch_herdr "$@"
else
    launch_desktop "$@"
fi
