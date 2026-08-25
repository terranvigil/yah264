#!/usr/bin/env bash
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# instr-ratio.sh -- print the INSTRUCTION ratio beside the wall ratio.
#
# Wall is instructions times cycles-per-instruction. A board row that reads
# 1.4x can be 1.4x the work, or the same work at 1.4x the CPI, and those two
# have completely different fixes. docs/archive/work-volume-audit.md made the check
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
# Env: CLIPS (same format as parity-clips.sh), NEXT264, X264_C
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
SECONDS_PER="${1:-6}"
PRESET="${SET_PRESET:-medium}"
NEXT264="${NEXT264:-$root/build/cli/next264}"
X264_C="${X264_C:-$root/../x264/x264-noasm-autovec}"
. "$root/scripts/parity-clips.sh"

# `time -l` writes its report to stderr; grab instructions retired and the
# wall it measured on the same run so the pair is never mismatched.
measure() {  # -> "<instructions> <wall_seconds>"
    local rep
    rep=$(/usr/bin/time -l "$@" 2>&1 >/dev/null)
    printf '%s %s\n' \
        "$(printf '%s\n' "$rep" | awk '/instructions retired/{print $1; exit}')" \
        "$(printf '%s\n' "$rep" | awk '/ real /{print $1; exit}')"
}

printf '%-18s %14s %14s %9s %9s\n' clip "n264 instr" "x264 instr" "instr x" "wall x"
printf '%-18s %14s %14s %9s %9s\n' ------------------ -------------- -------------- --------- ---------
for entry in $CLIPS; do
    clip="${entry%%:*}"; br="${entry##*:}"
    path="$root/tests/corpus/$clip.y4m"
    [ -f "$path" ] || { printf '%-18s %14s\n' "$clip" "(missing)"; continue; }
    fps=$(awk 'NR==1{for(i=1;i<=NF;i++) if($i ~ /^F/){sub(/^F/,"",$i); split($i,a,":"); print a[1]/a[2]; exit}}' "$path")
    nf=$(python3 -c "print(int(round($fps*$SECONDS_PER)))")

    n=$(measure env NEXT264_NO_ASM=1 "$NEXT264" --input-y4m "$path" --preset "$PRESET" \
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
echo "instr x and wall x are both next264/x264: >1 means we do more / take longer."
echo "They track => the row is WORK VOLUME. They diverge => the row is CPI."
