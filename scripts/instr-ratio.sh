#!/usr/bin/env bash
# Instructions retired from `/usr/bin/time -l` on macOS, from `perf stat` on
# Linux. Same columns either way.
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# instr-ratio.sh -- print the INSTRUCTION ratio beside the wall ratio.
#
# Wall is instructions times cycles-per-instruction. A board row that reads
# 1.4x can be 1.4x the work, or the same work at 1.4x the CPI, and those two
# have completely different fixes. the local measurement records made the check
# once by hand (`/usr/bin/time -l` reports instructions retired on macOS) and
# recommended adopting it as a standing companion to every work-factor claim:
# it costs two runs per clip and it is the check that would have caught a
# mislabeled factor a month earlier.
#
# This is deliberately the SIMPLEST possible harness. One run a side, no
# medians -- instruction counts are near-deterministic where wall is not, which
# is the entire reason the check is cheap. If the two ratios track, the row is
# work volume and the fix is to do less. If they diverge, the row is CPI and no
# amount of removed work will collect it.
#
# Pure-C, single thread, ABR at the board's calibrated points, so the numbers
# sit beside the goal-1 row of `make parity-status` and can be read against it.
#
# Usage: scripts/instr-ratio.sh [seconds]
# Env: CLIPS (same format as parity-clips.sh), YAH264, X264_C
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
SECONDS_PER="${1:-6}"
PRESET="${SET_PRESET:-medium}"
YAH264="${YAH264:-$root/build/cli/yah264}"
X264_C="${X264_C:-$root/../x264/x264-noasm-autovec}"
. "$root/scripts/parity-clips.sh"

# Two counters, one column pair. macOS: `time -l` writes its report to stderr
# and names instructions retired in it. Linux: `perf stat -x,` does, and the
# wall is taken around the same invocation rather than out of perf's own
# "seconds time elapsed" line, which -x, does not print.
#
# The Linux arm needs perf to be READABLE, which on a stock kernel it is not:
# perf_event_paranoid defaults to 2 or 4 and the counter comes back empty, so
# the row prints NO-COUNTER rather than a plausible zero. Fix it on the box
# (`sysctl kernel.perf_event_paranoid=1`) or run the container privileged --
# docs/instruments.md has the line. A VM without PMU passthrough cannot do
# this at all, which is why the x86 campaign reads it on the rented metal and
# not under emulation: an emulator's instruction count is the EMULATOR's.
if [ "$(uname -s)" = "Darwin" ]; then
measure() {  # -> "<instructions> <wall_seconds>"
    local rep
    rep=$(/usr/bin/time -l "$@" 2>&1 >/dev/null)
    printf '%s %s\n' \
        "$(printf '%s\n' "$rep" | awk '/instructions retired/{print $1; exit}')" \
        "$(printf '%s\n' "$rep" | awk '/ real /{print $1; exit}')"
}
else
measure() {  # -> "<instructions> <wall_seconds>"
    local rep t0 t1
    t0=$(date +%s.%N)
    rep=$(perf stat -x, -e instructions,cycles,task-clock -- "$@" 2>&1 >/dev/null)
    t1=$(date +%s.%N)
    printf '%s %s\n' \
        "$(printf '%s\n' "$rep" | awk -F, '$3 ~ /^instructions/ && $1 ~ /^[0-9]/ {print $1; exit}')" \
        "$(awk "BEGIN{printf \"%.3f\", $t1 - $t0}")"
}
if ! command -v perf >/dev/null; then
    echo "instr-ratio: no perf on PATH; every row will read NO-COUNTER." >&2
fi
fi

printf '%-18s %14s %14s %9s %9s\n' clip "y264 instr" "x264 instr" "instr x" "wall x"
printf '%-18s %14s %14s %9s %9s\n' ------------------ -------------- -------------- --------- ---------
for entry in $CLIPS; do
    clip="${entry%%:*}"; br="${entry##*:}"
    path="$root/tests/corpus/$clip.y4m"
    [ -f "$path" ] || { printf '%-18s %14s\n' "$clip" "(missing)"; continue; }
    fps=$(awk 'NR==1{for(i=1;i<=NF;i++) if($i ~ /^F/){sub(/^F/,"",$i); split($i,a,":"); print a[1]/a[2]; exit}}' "$path")
    nf=$(python3 -c "print(int(round($fps*$SECONDS_PER)))")

    n=$(measure env YAH264_NO_ASM=1 "$YAH264" --input-y4m "$path" --preset "$PRESET" \
            --cabac --transform-8x8 --bitrate "$br" --threads 1 --frames "$nf" -o /dev/null)
    x=$(measure "$X264_C" --preset "$PRESET" --bitrate "$br" --threads 1 \
            --frames "$nf" -o /dev/null "$path")
    ni=${n%% *}; nw=${n##* }; xi=${x%% *}; xw=${x##* }
    [ -n "$ni" ] && [ -n "$xi" ] || { printf '%-18s %14s\n' "$clip" "NO-COUNTER"; continue; }
    python3 - "$clip" "$ni" "$xi" "$nw" "$xw" <<'R'
import sys
c, ni, xi, nw, xw = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
print(f"{c:<18} {ni/1e9:>13.1f}G {xi/1e9:>13.1f}G {ni/xi:>8.2f}x {nw/xw:>8.2f}x")
R
done
echo
echo "instr x and wall x are both yah264/x264: >1 means we do more / take longer."
echo "They track => the row is WORK VOLUME. They diverge => the row is CPI."
