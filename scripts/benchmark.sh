#!/usr/bin/env bash
# Omatrack profiler-guided benchmark suite.
#
# Runs each autotest scenario under qmlprofiler with simultaneous RSS
# polling, parses traces for frame timing, and emits a TSV summary.
#
# Usage: scripts/benchmark.sh <pds-telemetry-dir> <video-telemetry-dir>
#
# Requires: build-acceptance preset, Qt 6 qmlprofiler, Wayland session.

set -uo pipefail
cd "$(dirname "$0")/.."

BINARY=./build-acceptance/omatrack
QMLPROFILER=/usr/lib/qt6/bin/qmlprofiler
PARSER=.agents/skills/qt-qml-profiler/references/scripts/parse-qmlprofiler-trace.py
TRACES_DIR=profiler/traces
REPORTS_DIR=profiler/reports
SHOTS_DIR=profiler/screenshots
mkdir -p "$TRACES_DIR" "$REPORTS_DIR" "$SHOTS_DIR"

SEBRING="${1:?Usage: scripts/benchmark.sh <pds-telemetry-dir> <video-telemetry-dir>}"
RAM="${2:?Usage: scripts/benchmark.sh <pds-telemetry-dir> <video-telemetry-dir>}"

# Auto-discover video files in the video telemetry directory
VIDEO1=$(find "$RAM" -type f \( -name '*.mp4' -o -name '*.MP4' \) 2>/dev/null | head -1)
VIDEO2=$(find "$RAM" -type f \( -name '*.mp4' -o -name '*.MP4' \) 2>/dev/null | sed -n '2p')
VIDEO3=$(find "$RAM" -type f \( -name '*.mp4' -o -name '*.MP4' \) 2>/dev/null | sed -n '3p')

TIMESTAMP=$(date +%Y-%m-%d-%H%M%S)
GIT_HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
SUMMARY="$REPORTS_DIR/benchmark-$TIMESTAMP.tsv"
echo "# Omatrack benchmark $TIMESTAMP (git: $GIT_HASH)" > "$SUMMARY"
echo -e "scenario\tdata\tframe_p50\tframe_p95\tframe_p99\tframe_max\tframes_>33\tframes_>50\tjs_ms\tcreating_ms\tbinding_ms\ttop_hotspot\thotspot_ms\thotspot_file\tpeak_rss_mb\tfinal_rss_mb\tsamples\thover_ms\tzoom_avg_ms\tzoom_worst_ms\tzoom_quads" >> "$SUMMARY"

# ─── Helper: run one scenario under qmlprofiler with RSS polling ───────
run_scenario() {
    local name="$1" datadir="$2"; shift 2
    local trace="$TRACES_DIR/bench-${name}-${TIMESTAMP}.qtd"
    local shot="$SHOTS_DIR/bench-${name}-${TIMESTAMP}.png"
    local rsslog="/tmp/bench-rss-$$.tsv"
    local datatype
    datatype=$(basename "$datadir")

    echo "▶ $name ($datatype)"

    # RSS poller: wait for omatrack process to appear, then track until exit
    : > "$rsslog"
    (
        for i in $(seq 1 100); do
            pgrep -x omatrack >/dev/null 2>&1 && break
            sleep 0.05
        done
        while true; do
            local pid
            pid=$(pgrep -x omatrack | head -1)
            [ -z "$pid" ] && break
            awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null >> "$rsslog"
            sleep 0.02
        done
    ) &
    local poller=$!

    # Run under qmlprofiler
    local autotest_output
    autotest_output=$(env QT_QPA_PLATFORM=wayland QT_FORCE_STDERR_LOGGING=1 \
        OMATRACK_AUTOTEST="$shot" "$@" \
        $QMLPROFILER -o "$trace" -- $BINARY "$datadir" 2>&1)
    wait "$poller" 2>/dev/null || true

    # Extract AUTOTEST benchmark numbers
    local hover_ms="n/a" zoom_avg="n/a" zoom_worst="n/a" zoom_quads="n/a"
    if echo "$autotest_output" | grep -q 'hover overlay'; then
        hover_ms=$(echo "$autotest_output" | grep 'hover overlay' | sed 's/.*average_ms: //' | sed 's/ .*//')
    fi
    if echo "$autotest_output" | grep -q 'zoom geometry'; then
        zoom_avg=$(echo "$autotest_output" | grep 'zoom geometry' | sed 's/.*average_ms: //' | sed 's/ .*//')
        zoom_worst=$(echo "$autotest_output" | grep 'worst_ms:' | sed 's/.*worst_ms: //' | sed 's/ .*//')
        zoom_quads=$(echo "$autotest_output" | grep 'quads:' | tail -1 | sed 's/.*quads: //' | sed 's/ .*//')
    fi
    echo "$autotest_output" | grep 'AUTOTEST' | sed 's/^/  /'

    # RSS stats
    local peak=0 final=0 samples=0
    if [ -s "$rsslog" ]; then
        peak=$(sort -n "$rsslog" | tail -1)
        final=$(tail -1 "$rsslog")
        samples=$(wc -l < "$rsslog")
    fi
    rm -f "$rsslog"
    local peak_mb final_mb
    peak_mb=$(echo "scale=1; ${peak:-0} / 1024" | bc)
    final_mb=$(echo "scale=1; ${final:-0} / 1024" | bc)
    echo "  RSS: peak=${peak_mb}MB final=${final_mb}MB samples=${samples}"

    # Parse trace
    local row=""
    if [ -f "$trace" ] && [ "$(stat -c%s "$trace")" -gt 1000 ]; then
        local parsed="/tmp/bench-parsed-$$.json"
        python3 "$PARSER" "$trace" > "$parsed" 2>&1 || true
        row=$(python3 -c "
import json
with open('$parsed') as f: d = json.load(f)
a = d.get('animations', {})
types = {t['type']: t for t in d.get('by_type', [])}
proj = [h for h in d.get('hotspots', []) if 'qt-project.org' not in h.get('filename','')]
top = proj[0] if proj else {}
f = top.get('filename','n/a').split('/')[-1]
print('\t'.join([
    f'{a.get(\"frame_ms_p50\",0):.2f}',
    f'{a.get(\"frame_ms_p95\",0):.2f}',
    f'{a.get(\"frame_ms_p99\",0):.2f}',
    f'{a.get(\"frame_ms_max\",0):.2f}',
    str(a.get('frames_over_33ms',0)),
    str(a.get('frames_over_50ms',0)),
    f'{types.get(\"Javascript\",{}).get(\"total_ms\",0):.0f}',
    f'{types.get(\"Creating\",{}).get(\"total_ms\",0):.0f}',
    f'{types.get(\"Binding\",{}).get(\"total_ms\",0):.0f}',
    top.get('details','n/a').replace('\t',' ')[:35],
    f'{top.get(\"total_ms\",0):.1f}',
    f,
]))
" 2>/dev/null < "$parsed" || echo -e "0\t0\t0\t0\t0\t0\t0\t0\t0\tn/a\t0\tn/a")
        rm -f "$parsed"
    else
        row=$(echo -e "0\t0\t0\t0\t0\t0\t0\t0\t0\tn/a\t0\tn/a")
    fi

    echo -e "${name}\t${datatype}\t${row}\t${peak_mb}\t${final_mb}\t${samples}\t${hover_ms}\t${zoom_avg}\t${zoom_worst}\t${zoom_quads}" >> "$SUMMARY"
    echo ""
}

# ─── Helper: pure memory run (no qmlprofiler) ──────────────────────────
run_memory_only() {
    local name="$1" datadir="$2"; shift 2
    local rsslog="/tmp/bench-rss-mem-$$.tsv"
    : > "$rsslog"
    (
        for i in $(seq 1 100); do
            pgrep -x omatrack >/dev/null 2>&1 && break
            sleep 0.05
        done
        while true; do
            local pid
            pid=$(pgrep -x omatrack | head -1)
            [ -z "$pid" ] && break
            awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null >> "$rsslog"
            sleep 0.02
        done
    ) &
    local poller=$!
    env QT_QPA_PLATFORM=wayland QT_FORCE_STDERR_LOGGING=1 \
        OMATRACK_AUTOTEST=/dev/null "$@" \
        $BINARY "$datadir" &>/dev/null
    wait "$poller" 2>/dev/null || true
    local peak=0 final=0
    if [ -s "$rsslog" ]; then
        peak=$(sort -n "$rsslog" | tail -1)
        final=$(tail -1 "$rsslog")
    fi
    rm -f "$rsslog"
    echo "  ${name}: peak=$(echo "scale=1;${peak:-0}/1024"|bc)MB final=$(echo "scale=1;${final:-0}/1024"|bc)MB"
}

# ─── Banner ────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════════════════════════"
echo "  OMATRACK PROFILER + MEMORY BENCHMARK SUITE"
echo "  PDS data: $SEBRING"
echo "  Video data: $RAM"
echo "  Timestamp: $TIMESTAMP"
echo "════════════════════════════════════════════════════════════════"
echo ""

# ─── Baseline memory (no qmlprofiler overhead) ────────────────────────
echo "── Baseline Memory (no profiler) ──"
mkdir -p /tmp/empty-telemetry
run_memory_only "empty-dir" /tmp/empty-telemetry
run_memory_only "startup-mem" "$SEBRING"
run_memory_only "interaction-mem" "$SEBRING" \
    OMATRACK_AUTOTEST_HOVER=1 OMATRACK_AUTOTEST_ZOOM=1 \
    OMATRACK_AUTOTEST_SELECTION=1 OMATRACK_AUTOTEST_CORNER=1
run_memory_only "comparison-mem" "$SEBRING" \
    OMATRACK_AUTOTEST_COMPARE=1 OMATRACK_AUTOTEST_SELECTION=1 \
    OMATRACK_AUTOTEST_CORNER=1
run_memory_only "video-mem" "$RAM" \
    OMATRACK_VIDEO="$VIDEO1" OMATRACK_AUTOTEST_SELECTION=1 \
    OMATRACK_AUTOTEST_CORNER=1 OMATRACK_AUTOTEST_VIDEO_HUD=1
run_memory_only "heavy-mem" "$RAM" \
    OMATRACK_AUTOTEST_HOVER=1 OMATRACK_AUTOTEST_ZOOM=1 \
    OMATRACK_AUTOTEST_SELECTION=1 OMATRACK_AUTOTEST_CORNER=1 \
    OMATRACK_AUTOTEST_VIDEO_HUD=1
run_memory_only "lap-switch-mem" "$SEBRING" \
    OMATRACK_AUTOTEST_LAP_SWITCH=1
echo ""

# ─── Profiled scenarios (qmlprofiler + RSS) ───────────────────────────
echo "── Profiled Scenarios (qmlprofiler + RSS) ──"
echo ""

# Sebring PDS scenarios
run_scenario startup "$SEBRING"
run_scenario interaction "$SEBRING" \
    OMATRACK_AUTOTEST_HOVER=1 OMATRACK_AUTOTEST_ZOOM=1 \
    OMATRACK_AUTOTEST_SELECTION=1 OMATRACK_AUTOTEST_CORNER=1
run_scenario comparison "$SEBRING" \
    OMATRACK_AUTOTEST_COMPARE=1 OMATRACK_AUTOTEST_SELECTION=1 \
    OMATRACK_AUTOTEST_CORNER=1
run_scenario corner-edit "$SEBRING" \
    OMATRACK_AUTOTEST_COMPARE=1 OMATRACK_AUTOTEST_CORNER_EDIT=1
run_scenario windows "$SEBRING" \
    OMATRACK_AUTOTEST_WINDOWS=1 OMATRACK_AUTOTEST_SELECTION=1
run_scenario lap-loading "$SEBRING" \
    OMATRACK_AUTOTEST_LAP_LOADING=1 OMATRACK_AUTOTEST_SELECTION=1
run_scenario lap-switch "$SEBRING" \
    OMATRACK_AUTOTEST_LAP_SWITCH=1
run_scenario lap-switch-compare "$SEBRING" \
    OMATRACK_AUTOTEST_LAP_SWITCH=1 OMATRACK_AUTOTEST_COMPARE=1 \
    OMATRACK_AUTOTEST_CORNER=1
run_scenario channel-browser "$SEBRING" \
    OMATRACK_AUTOTEST_CHANNEL_BROWSER=1

# Road America video scenarios
if [ -d "$RAM" ]; then
    run_scenario video-race "$RAM" \
        OMATRACK_AUTOTEST_SELECTION=1 OMATRACK_AUTOTEST_CORNER=1 \
        OMATRACK_AUTOTEST_VIDEO_HUD=1
    if [ -f "$VIDEO1" ]; then
        run_scenario video-tl "$RAM" \
            OMATRACK_VIDEO="$VIDEO1" OMATRACK_AUTOTEST_SELECTION=1 \
            OMATRACK_AUTOTEST_CORNER=1 OMATRACK_AUTOTEST_VIDEO_HUD=1
    fi
    if [ -f "$VIDEO1" ] && [ -f "$VIDEO2" ]; then
        run_scenario video-compare "$RAM" \
            OMATRACK_VIDEO="$VIDEO1" \
            OMATRACK_AUTOTEST_SECOND_VIDEO="$VIDEO2" \
            OMATRACK_AUTOTEST_COMPARE=1 OMATRACK_AUTOTEST_SELECTION=1 \
            OMATRACK_AUTOTEST_CORNER=1 OMATRACK_AUTOTEST_VIDEO_HUD=1
    fi
    run_scenario heavy-ram "$RAM" \
        OMATRACK_AUTOTEST_HOVER=1 OMATRACK_AUTOTEST_ZOOM=1 \
        OMATRACK_AUTOTEST_SELECTION=1 OMATRACK_AUTOTEST_CORNER=1 \
        OMATRACK_AUTOTEST_VIDEO_HUD=1
    run_scenario lap-switch-ram "$RAM" \
        OMATRACK_AUTOTEST_LAP_SWITCH=1 OMATRACK_AUTOTEST_VIDEO_HUD=1
    if [ -f "$VIDEO1" ]; then
        run_scenario brake-sync "$RAM" \
            OMATRACK_VIDEO="$VIDEO1" OMATRACK_AUTOTEST_BRAKE_SYNC=1
    fi
    if [ -f "$VIDEO1" ] && [ -f "$VIDEO2" ]; then
        run_scenario dual-video "$RAM" \
            OMATRACK_VIDEO="$VIDEO1" \
            OMATRACK_AUTOTEST_SECOND_VIDEO="$VIDEO2" \
            OMATRACK_AUTOTEST_DUAL_VIDEO=1
    fi
    if [ -f "$VIDEO3" ]; then
        run_scenario standalone-video "$RAM" \
            OMATRACK_VIDEO="$VIDEO3" \
            OMATRACK_AUTOTEST_STANDALONE_VIDEO=1
    fi
    if [ -f "$VIDEO1" ]; then
        run_scenario alignment "$RAM" \
            OMATRACK_VIDEO="$VIDEO1" OMATRACK_AUTOTEST_COMPARE=1 \
            OMATRACK_AUTOTEST_ALIGNMENT=1 OMATRACK_AUTOTEST_SELECTION=1
    fi
fi

# ─── CLI scenarios (parser + corner analysis, no qmlprofiler) ────────
CLI=./build/omatrack-cli
if [ -x "$CLI" ] && [ -d "$SEBRING" ]; then
    PDS_FILE=$(find "$SEBRING" -name '*.pds' -type f 2>/dev/null | head -1)
    PDS_REF=$(find "$SEBRING" -name '*.pds' -type f 2>/dev/null | sed -n '2p')
    if [ -n "$PDS_FILE" ]; then
        echo "▶ cli-parse-pds"
        S=$(date +%s%N); "$CLI" parse "$PDS_FILE" >/dev/null 2>&1; E=$(date +%s%N)
        parse_ms=$(( (E-S)/1000000 ))
        S=$(date +%s%N); "$CLI" unify "$PDS_FILE" --output /tmp/bench-unify-$$.csv >/dev/null 2>&1; E=$(date +%s%N)
        unify_ms=$(( (E-S)/1000000 ))
        rm -f /tmp/bench-unify-$$.csv
        echo "  PDS parse: ${parse_ms} ms, unify: ${unify_ms} ms"
        echo -e "cli-parse-pds\t$(basename "$SEBRING")\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\t1\tPDS parse+unify\tn/a\tn/a\t${parse_ms}\tn/a\tn/a\tn/a" >> "$SUMMARY"
    fi
    if [ -n "$PDS_FILE" ] && [ -n "$PDS_REF" ]; then
        echo "▶ cli-corners"
        S=$(date +%s%N); "$CLI" corners "$PDS_FILE" --reference "$PDS_REF" --zone 0.30:0.36 >/dev/null 2>&1; E=$(date +%s%N)
        corner_ms=$(( (E-S)/1000000 ))
        echo "  corner analysis: ${corner_ms} ms"
        echo -e "cli-corners\t$(basename "$SEBRING")\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\t1\tcorner analysis\tn/a\tn/a\t${corner_ms}\tn/a\tn/a\tn/a" >> "$SUMMARY"
    fi
    if [ -f "$VIDEO1" ]; then
        echo "▶ cli-parse-mp4"
        S=$(date +%s%N); "$CLI" parse "$VIDEO1" >/dev/null 2>&1; E=$(date +%s%N)
        mp4_parse_ms=$(( (E-S)/1000000 ))
        S=$(date +%s%N); "$CLI" unify "$VIDEO1" --output /tmp/bench-mp4-$$.csv >/dev/null 2>&1; E=$(date +%s%N)
        mp4_unify_ms=$(( (E-S)/1000000 ))
        rm -f /tmp/bench-mp4-$$.csv
        echo "  MP4 parse: ${mp4_parse_ms} ms, unify: ${mp4_unify_ms} ms"
        echo -e "cli-parse-mp4\t$(basename "$RAM")\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\tn/a\t1\tMP4 parse+unify\tn/a\tn/a\t${mp4_parse_ms}\tn/a\tn/a\tn/a" >> "$SUMMARY"
    fi
fi

# ─── Summary ──────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════════════════════════"
echo "  SUMMARY"
echo "════════════════════════════════════════════════════════════════"
echo ""
column -t -s $'\t' "$SUMMARY" | cut -c1-250
echo ""
echo "Full TSV: $SUMMARY"
echo "Traces:   $TRACES_DIR/bench-*-${TIMESTAMP}.qtd"
echo "Screenshots: $SHOTS_DIR/bench-*-${TIMESTAMP}.png"
