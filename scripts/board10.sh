#!/bin/bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# board10.sh - the 10-bit board: BD-rate(VMAF-NEG) against x264 High 10.
#
# One binary encodes both depths since item C3-10bit, so this board says
# nothing on the command line about ten bits: the clip's Y4M C tag picks the
# encoder, on both sides. x264 needs --output-depth 10 as well as
# --profile high10 -- the profile alone constrains the stream and still writes
# eight-bit samples, which reads as a 10-bit board that was never 10-bit.
#
# Each arm is solved onto the SAME five byte targets per clip and the BD is
# taken over the resulting (bytes, VMAF-NEG) pairs, so the two encoders' CRF
# scales cancel by construction (scripts/bd_at_rate.py). Targets come from
# scripts/parity-clips.sh, calibrated to land the sweep inside the VMAF-NEG
# 55-95 band on our own curve.
#
# BD at matched rate is load-immune, so this board does not need a quiet box;
# it needs the core budget kept (JOBS x threads <= 18). Both arms run at
# --threads 4, pinned rather than left at auto, because x264 at auto threads
# codes a different stream on a different box.
#
#   scripts/board10.sh                  all of CLIPS_P10
#   scripts/board10.sh JockeyHarmonics  one clip
#   THREADS=2 scripts/board10.sh
set -u
cd "$(dirname "$0")/.." || exit 1
. scripts/parity-clips.sh

Y=${YAH264:-build/cli/yah264}
X=${X264:-x264}
THREADS=${THREADS:-4}
D=${CLIPS_P10_DIR:-/Volumes/seagate/media/_p10}
want="$*"

for entry in $CLIPS_P10; do
    clip=${entry%%:*}; targets=${entry#*:}
    [ -n "$want" ] && ! echo " $want " | grep -q " $clip " && continue
    src="$D/${clip}_p10.y4m"
    if [ ! -f "$src" ]; then echo "board10: missing $src"; continue; fi
    python3 scripts/bd_at_rate.py \
        --a "$Y --input-y4m {src} --crf {q} --threads $THREADS -o {out}" \
        --b "$X --profile high10 --output-depth 10 --crf {q} --threads $THREADS --log-level none -o {out} {src}" \
        --clip "$clip" --src "$src" --dec-pixfmt yuv420p10le \
        --targets "$targets" --label-a yah264-10 --label-b x264-high10
done
