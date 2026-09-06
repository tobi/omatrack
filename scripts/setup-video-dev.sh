#!/usr/bin/env bash
# Native aarch64 development dependencies, confined to an ignored build directory.
# No shell activation, system package installation, or desktop configuration.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${OMATRACK_VIDEO_DEV_WORK:-"$ROOT/build-video-dev"}
PREFIX="$WORK/prefix"
MAMBA="$WORK/bootstrap/bin/micromamba"
export MAMBA_ROOT_PREFIX="$WORK/mamba"

usage() {
    printf '%s\n' \
        'Usage: scripts/setup-video-dev.sh install|install-xvfb|env|exec COMMAND...|xvfb COMMAND...' \
        'Default prefix: <checkout>/build-video-dev/prefix (ignored).' \
        'Use exec for configure/build/test; xvfb adds a private X server and scratch user state.'
}

activate() {
    [[ -d "$PREFIX/conda-meta" ]] || { echo 'Run install first.' >&2; exit 1; }
    export PATH="$PREFIX/bin:$PATH"
    export CMAKE_PREFIX_PATH="$PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
    export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    # pkgconf treats its own prefix as a system search directory, but the
    # host compiler does not. Retain those flags for direct compiler probes.
    export PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1 PKG_CONFIG_ALLOW_SYSTEM_LIBS=1
    # Keep build resource use modest alongside inference and other workers.
    export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-8}
    export CARGO_BUILD_JOBS=${CARGO_BUILD_JOBS:-8}
}

case "${1:-}" in
install)
    [[ $(uname -s)/$(uname -m) == Linux/aarch64 ]] || {
        echo 'This helper currently targets native Linux/aarch64 only.' >&2; exit 1;
    }
    mkdir -p "$WORK/bootstrap" "$WORK/logs"
    if [[ ! -x "$MAMBA" ]]; then
        curl -fLsS https://micro.mamba.pm/api/micromamba/linux-aarch64/2.9.0 \
            -o "$WORK/bootstrap/micromamba.tar.bz2"
        printf '%s  %s\n' \
            e705ffeed90ce0659eb546e4b1e1028c9eaf0bc9cc854867b19ac5ce0ba5852f \
            "$WORK/bootstrap/micromamba.tar.bz2" | sha256sum --check --status
        tar -xjf "$WORK/bootstrap/micromamba.tar.bz2" -C "$WORK/bootstrap" bin/micromamba
    fi
    action=create
    [[ ! -d "$PREFIX/conda-meta" ]] || action=install
    "$MAMBA" "$action" --no-rc -y -p "$PREFIX" -c conda-forge --strict-channel-priority \
        'qt6-main=6.11.2' 'mpv=0.41.0' yaml pkgconf \
        libgl-devel libegl-devel xorg-xorgproto expat zlib meson
    # pkgconf intentionally does not ship the compatibility program name.
    [[ -e "$PREFIX/bin/pkg-config" ]] || ln -s pkgconf "$PREFIX/bin/pkg-config"
    "$MAMBA" list --no-rc -p "$PREFIX" --explicit > "$WORK/conda-explicit.txt"
    ;;
install-xvfb)
    # Download/extract only: no apt update/install, sudo, or dpkg package registration.
    mkdir -p "$WORK/debs" "$WORK/x11"
    (cd "$WORK/debs" && apt-get download xvfb libxfont2 mesa-utils-bin)
    for deb in "$WORK/debs/"*.deb; do dpkg-deb -x "$deb" "$WORK/x11"; done
    ;;
env)
    printf 'export PATH=%q:"$PATH"\n' "$PREFIX/bin"
    printf 'export CMAKE_PREFIX_PATH=%q\n' "$PREFIX"
    printf 'export PKG_CONFIG_PATH=%q\n' "$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
    printf 'export LD_LIBRARY_PATH=%q\n' "$PREFIX/lib"
    printf 'export PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1 PKG_CONFIG_ALLOW_SYSTEM_LIBS=1\n'
    printf 'export CMAKE_BUILD_PARALLEL_LEVEL=8 CARGO_BUILD_JOBS=8\n'
    ;;
exec)
    shift
    [[ $# -gt 0 ]] || { usage; exit 2; }
    activate
    exec "$@"
    ;;
xvfb)
    shift
    [[ $# -gt 0 ]] || { usage; exit 2; }
    [[ -x "$WORK/x11/usr/bin/Xvfb" ]] || { echo 'Run install-xvfb first.' >&2; exit 1; }
    # Xvfb has no hardware acceleration: llvmpipe is real OpenGL, not Qt's
    # geometry-less software scene graph. Never use the user's live DISPLAY.
    RUN=$(mktemp -d "$WORK/xvfb-run.XXXXXX")
    mkdir -p "$RUN/home" "$RUN/config" "$RUN/cache" "$RUN/data" "$RUN/runtime"
    chmod 700 "$RUN/runtime"
    # The server accepts the cookie irrespective of its display-number
    # record; add the actual client record once -displayfd chooses a number.
    # Never expose the private video through an unauthenticated local socket.
    command -v xauth >/dev/null
    COOKIE=$(mcookie)
    export XAUTHORITY="$RUN/Xauthority"
    touch "$XAUTHORITY"
    chmod 600 "$XAUTHORITY"
    xauth -f "$XAUTHORITY" add :0 . "$COOKIE"
    mkfifo "$RUN/display-fd"
    exec 3<>"$RUN/display-fd"
    LD_LIBRARY_PATH="$WORK/x11/usr/lib/aarch64-linux-gnu" \
        "$WORK/x11/usr/bin/Xvfb" -displayfd 3 -screen 0 1280x800x24 \
        -nolisten tcp -auth "$XAUTHORITY" +extension GLX +render -noreset >"$RUN/xvfb.log" 2>&1 &
    SERVER_PID=$!
    trap 'kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; exec 3>&-' EXIT
    read -r -t 20 DISPLAY_NUMBER <&3 || { echo "Xvfb failed; see $RUN/xvfb.log" >&2; exit 1; }
    xauth -f "$XAUTHORITY" add ":$DISPLAY_NUMBER" . "$COOKIE"
    unset COOKIE
    activate
    export DISPLAY=":$DISPLAY_NUMBER"
    unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
    export QT_QPA_PLATFORM=xcb QSG_RHI_BACKEND=opengl LIBGL_ALWAYS_SOFTWARE=1
    export QT_FORCE_STDERR_LOGGING=1
    export HOME="$RUN/home" XDG_CONFIG_HOME="$RUN/config" XDG_CACHE_HOME="$RUN/cache"
    export XDG_DATA_HOME="$RUN/data" XDG_RUNTIME_DIR="$RUN/runtime"
    printf 'Isolated DISPLAY=%s; state/logs=%s\n' "$DISPLAY" "$RUN"
    "$@"
    ;;
*) usage; exit 2 ;;
esac
