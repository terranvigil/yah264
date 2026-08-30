#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# recon_sweep.sh - recon-match one env-gated arm across a config matrix.
#
# `make conformance` gates the DEFAULT path. When a new coding tool lives behind
# an env knob, its decoder-conformance surface is the cross product of the knobs
# that reach it -- reference count, B-frame count, entropy coder, direct mode,
# transform. This runs that cross product: encode with --dump-recon, decode with
# ffmpeg, compare per-frame framemd5. Nothing else; it is `check_clip` from
# conformance.sh widened over configs instead of over clips.
#
#   ARM='Y264_B_8X8=1' scripts/recon_sweep.sh
#   ARM='Y264_B_8X8=1 Y264_B8_DIRECT=0' CLIPS='foreman mobile' QPS='26 37' \
#       scripts/recon_sweep.sh
#
# Two traps this exists to not step in again, both of which have cost a round:
#
#  1. **Delete the output files before every iteration.** A config the CLI
#     rejects writes nothing, and the comparison then re-scores the PREVIOUS
#     iteration's leftovers as a pass. Every arm of one B_8x8 matrix "passed"
#     that way while never running.
#  2. **Check the exit code.** Unknown flags exit 2 and print help; in zsh a
#     config held in a scalar does not word-split either, so a config string
#     built in a variable arrives as one argument. Configs here come from a
#     heredoc read line by line, and $cfg is deliberately unquoted at the call.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENC="${ENC:-$ROOT/build/cli/yah264}"
FIX="${FIX:-$ROOT/tests/.fixtures/v1}"
WORK="${WORK:-${TMPDIR:-/tmp}/recon_sweep.$$}"
ARM="${ARM:-}"
CLIPS="${CLIPS:-foreman mobile bus stefan akiyo coastguard}"
QPS="${QPS:-6 18 26 37 51}"
FRAMES="${FRAMES:-0}"

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

md5frames() { ffmpeg -v error -i "$1" -f framemd5 - 2>/dev/null | grep -v '^#' | awk '{print $NF}'; }

fails=0; total=0
for clip in $CLIPS; do
    src="$FIX/corpus_${clip}_cif_48f.y4m"
    [ -f "$src" ] || { echo "  skip $clip (no fixture)"; continue; }
    for qp in $QPS; do
        while IFS= read -r cfg; do
            [ -n "$cfg" ] || continue
            total=$((total + 1))
            out="$WORK/s.264"; rec="$WORK/s.rec.y4m"
            rm -f "$out" "$rec"
            # shellcheck disable=SC2086
            env $ARM "$ENC" --input-y4m "$src" --qp "$qp" --threads 1 \
                ${FRAMES:+--frames $FRAMES} $cfg -o "$out" --dump-recon "$rec" \
                >/dev/null 2>&1
            rc=$?
            if [ $rc -ne 0 ]; then
                echo "  ENCFAIL rc=$rc $clip qp$qp $cfg"; fails=$((fails + 1)); continue
            fi
            a="$(md5frames "$rec")"; b="$(md5frames "$out")"
            if [ -z "$a" ] || [ "$a" != "$b" ]; then
                echo "  FAIL $clip qp$qp $cfg"; fails=$((fails + 1))
            fi
        done <<'CFGS'
--ref 1
--ref 3
--ref 5
--ref 1 --cavlc
--ref 3 --cavlc
--ref 3 --direct temporal
--ref 3 --direct temporal --cavlc
--ref 3 --bframes 1
--ref 3 --bframes 5
--ref 3 --no-transform-8x8
CFGS
    done
done
echo "RECON-SWEEP $((total - fails))/$total  arm='${ARM:-<default>}'"
[ "$fails" -eq 0 ]
