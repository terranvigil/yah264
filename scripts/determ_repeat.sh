#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# determ_repeat.sh - same binary, same config, same thread count, N times.
# Every run must produce one bitstream.
#
# This is the gate that was missing. The tree's determinism scripts compare
# ACROSS thread counts (stair_determ.sh) or against a reference build
# (w2_canary.sh); nothing re-ran one configuration and compared it with itself,
# so a shipped default spent a day emitting 3-5 distinct bitstreams per 12 runs
# at foreman --ref 1 t18 without any battery noticing. TSan is silent on it --
# the reader was consuming a flag published before the data it advertised, which
# is a happens-before violation TSan can miss when the write lands first often
# enough.
#
# Thread-VARIANT output is allowed here by owner ruling; same-config
# reproducibility is not optional, because every identity gate in this tree
# (escape-env md5 equality, A/B md5 difference, delete probes gated on output
# identity) reads a lottery ticket without it.
#
#   scripts/determ_repeat.sh                       # the default matrix
#   RUNS=20 CLIPS='foreman_cif' scripts/determ_repeat.sh
#   ARM='Y264_B_8X8=1' scripts/determ_repeat.sh    # gate an arm the same way
#   ARGS='--direct temporal' scripts/determ_repeat.sh   # ...or a MODE
#
# ARM is env, ARGS is encoder flags, and they are not interchangeable: a flag in
# the ARM slot makes env(1) reject the whole command, every run then produces
# nothing, and the nothings all have the same md5 -- a clean sweep for an encode
# that never ran. scripts/stair_determ.sh grew the same pair of slots after that
# happened there on 2026-09-01.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENC="${ENC:-$ROOT/build/cli/yah264}"
WORK="${WORK:-${TMPDIR:-/tmp}/determ_repeat.$$}"
RUNS="${RUNS:-12}"
FRAMES="${FRAMES:-120}"
CLIPS="${CLIPS:-foreman_cif bus_cif stefan_cif samsung_720p}"
THREADS="${THREADS:-8 18}"
REFS="${REFS:-1 3}"
ARM="${ARM:-}"
ARGS="${ARGS:-}"

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

fails=0; total=0
for clip in $CLIPS; do
    src="$ROOT/tests/corpus/$clip.y4m"
    [ -f "$src" ] || { echo "  skip $clip (no clip)"; continue; }
    for r in $REFS; do
        for t in $THREADS; do
            total=$((total + 1))
            n=0
            for i in $(seq "$RUNS"); do
                out="$WORK/r.264"; rm -f "$out"
                # shellcheck disable=SC2086
                env $ARM "$ENC" --input-y4m "$src" --frames "$FRAMES" --ref "$r" \
                    --crf 22 --threads "$t" $ARGS -o "$out" >/dev/null 2>&1 || {
                        echo "  ENCFAIL $clip ref$r t$t"; n=0; break; }
                # Guard the producer: two missing files have equal md5s.
                [ -s "$out" ] || { echo "  EMPTY $clip ref$r t$t"; n=0; break; }
                md5 -q "$out" 2>/dev/null || md5sum "$out" | cut -d' ' -f1
            done > "$WORK/hashes"
            n=$(sort -u "$WORK/hashes" | wc -l | tr -d ' ')
            if [ "$n" != 1 ]; then
                echo "  FAIL $clip ref$r t$t: $n distinct bitstreams in $RUNS runs"
                fails=$((fails + 1))
            fi
        done
    done
done
echo "DETERM-REPEAT $((total - fails))/$total configs reproducible over $RUNS runs each  arm='${ARM:-<default>}' args='${ARGS:-<default>}'"
[ "$fails" -eq 0 ]
