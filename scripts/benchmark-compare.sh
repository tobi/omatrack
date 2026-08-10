#!/usr/bin/env bash
# Compare two benchmark TSV files and highlight regressions.
#
# Usage: scripts/benchmark-compare.sh <before.tsv> <after.tsv>
#
# Highlights metrics that changed by more than 10% (configurable via
# THRESHOLD environment variable).  Hover and zoom times are checked
# against the 8.33 ms / 120 Hz budget.
#
# Exit code: 0 if no regressions, 1 if any metric regressed.

set -uo pipefail
THRESHOLD="${THRESHOLD:-10}"  # percent

if [ $# -ne 2 ]; then
    echo "Usage: $0 <before.tsv> <after.tsv>"
    exit 2
fi

BEFORE="$1"
AFTER="$2"

# Build an associative array: scenario → "hover zoom_avg zoom_worst quads peak final"
declare -A BEFORE_MAP AFTER_MAP

parse_tsv() {
    local file="$1" mapname="$2"
    # TSV columns (1-indexed):
    # 1:scenario 2:data 3:p50 4:p95 5:p99 6:max 7:>33 8:>50
    # 9:js 10:creating 11:binding 12:hotspot 13:hotspot_ms 14:hotspot_file
    # 15:peak_rss 16:final_rss 17:samples 18:hover 19:zoom_avg 20:zoom_worst 21:quads
    while IFS=$'\t' read -r scenario data p50 p95 p99 max f33 f50 js creating binding hotspot hotspot_ms hotspot_file peak_rss final_rss samples hover_ms zoom_avg_ms zoom_worst_ms zoom_quads; do
        [[ "$scenario" == \#* ]] && continue
        [[ "$scenario" == "scenario" ]] && continue
        [[ -z "$scenario" ]] && continue
        if [ "$hover_ms" != "n/a" ]; then
            eval "${mapname}[$scenario]=\"${hover_ms} ${zoom_avg_ms} ${zoom_worst_ms} ${zoom_quads} ${peak_rss} ${final_rss}\""
        fi
    done < "$file"
}

parse_tsv "$BEFORE" BEFORE_MAP
parse_tsv "$AFTER" AFTER_MAP

REGRESSION=0
BUDGET=8.33

printf "%-22s  %-10s  %-18s  %8s  %s\n" "Scenario" "Metric" "Before→After" "Change" "Status"
printf "%-22s  %-10s  %-18s  %8s  %s\n" "-------" "------" "-------------" "------" "------"

for scenario in "${!BEFORE_MAP[@]}"; do
    [ -z "${AFTER_MAP[$scenario]+x}" ] && continue
    read -r h_b z_b w_b q_b p_b f_b <<< "${BEFORE_MAP[$scenario]}"
    read -r h_a z_a w_a q_a p_a f_a <<< "${AFTER_MAP[$scenario]}"

    for pair in "hover:$h_b:$h_a" "zoom:$z_b:$z_a" "peak:$p_b:$p_a" "final:$f_b:$f_a"; do
        metric="${pair%%:*}"
        rest="${pair#*:}"
        before="${rest%%:*}"
        after="${rest##*:}"
        [[ "$before" == "n/a" || "$after" == "n/a" ]] && continue
        [[ -z "$before" || -z "$after" ]] && continue

        result=$(awk -v b="$before" -v a="$after" -v m="$metric" -v t="$THRESHOLD" -v budget="$BUDGET" '
        BEGIN {
            if (b + 0 == 0) { pct = 0 } else { pct = (a - b) / b * 100 }
            bad = 0
            if (pct > t) { bad = 1 }
            status = bad ? "REGRESSION" : "ok"
            if ((m == "hover" || m == "zoom") && a > budget) { status = "OVER BUDGET"; bad = 1 }
            printf "%+7.1f%%\t%s\t%d", pct, status, bad
        }')
        pct=$(echo "$result" | cut -f1)
        status=$(echo "$result" | cut -f2)
        bad=$(echo "$result" | cut -f3)
        [ "$bad" -eq 1 ] && REGRESSION=1
        printf "%-22s  %-10s  %8s→%-8s  %8s  %s\n" "$scenario" "$metric" "$before" "$after" "$pct" "$status"
    done
done

echo ""
if [ $REGRESSION -eq 0 ]; then
    echo "✓ No regressions detected (threshold: ${THRESHOLD}%)"
else
    echo "✗ Regressions detected (threshold: ${THRESHOLD}%)"
fi
exit $REGRESSION
