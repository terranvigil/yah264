#!/usr/bin/env bash
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# abr_decode_gate.sh - decode-quality gate for the THREADED ABR path.
#
# Exists because of the 2026-08-20 RCP_LAG=1 escape: a default that emitted
# broken bitstreams only on threaded ABR shapes passed the whole battery --
# conformance cannot reach that path (--dump-recon forces the serial
# streaming path, where the lag never engages), the CRF band never runs ABR,
# and md5 identity gates compare an encoder with itself. This gate closes the
# hole from the DECODER side: encode the board ABR shapes multi-threaded,
# then assert (1) every input frame decodes, (2) mean PSNR vs source clears a
# floor no working encode is anywhere near (broken B emission read 15.7 dB
# where a working encode reads ~30).
#
#   scripts/abr_decode_gate.sh                # default 3-clip matrix
#   CLIPS='bus_cif:400' THREADS=12 scripts/abr_decode_gate.sh
#   ARM='N264_RCP_LAG=1' scripts/abr_decode_gate.sh   # gate an arm
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENC="${ENC:-$ROOT/build/cli/next264}"
WORK="${WORK:-${TMPDIR:-/tmp}/abr_decode_gate.$$}"
CLIPS="${CLIPS:-bus_cif:400 foreman_cif:400 samsung_720p:1200}"
THREADS="${THREADS:-12}"
PSNR_FLOOR="${PSNR_FLOOR:-25}"
ARM="${ARM:-}"

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

fails=0
for spec in $CLIPS; do
    clip="${spec%%:*}"; kbps="${spec##*:}"
    src="$ROOT/tests/corpus/$clip.y4m"
    [ -f "$src" ] || { echo "skip $clip (no corpus file)"; continue; }
    out="$WORK/$clip.264"
    env $ARM "$ENC" --input-y4m "$src" --bitrate "$kbps" --preset medium \
        --cabac --transform-8x8 --ref 3 --bframes 3 --threads "$THREADS" \
        -o "$out" 2>/dev/null
    want=$(ffprobe -v error -count_frames -select_streams v \
           -show_entries stream=nb_read_frames -of csv=p=0 "$src" 2>/dev/null)
    got=$(ffprobe -v error -count_frames -select_streams v \
          -show_entries stream=nb_read_frames -of csv=p=0 "$out" 2>/dev/null)
    psnr=$(ffmpeg -v error -i "$out" -i "$src" \
           -lavfi "psnr=stats_file=$WORK/$clip.psnr" -f null - 2>/dev/null; \
           awk '{for(i=1;i<=NF;i++) if($i ~ /^psnr_avg:/){split($i,a,":"); \
                if (a[2]=="inf") a[2]=99; s+=a[2]; n++}} \
                END{if(n) printf "%.1f", s/n; else print 0}' "$WORK/$clip.psnr")
    ok=ok
    [ "$got" = "$want" ] || ok="FAIL(frames $got/$want)"
    awk -v p="$psnr" -v f="$PSNR_FLOOR" 'BEGIN{exit !(p+0 >= f+0)}' \
        || ok="$ok FAIL(psnr $psnr < $PSNR_FLOOR)"
    [ "$ok" = ok ] || fails=$((fails+1))
    printf '  %-16s t%-3s frames %s/%s  mean-psnr %s dB  %s\n' \
        "$clip" "$THREADS" "$got" "$want" "$psnr" "$ok"
done

if [ "$fails" -gt 0 ]; then
    echo "ABR-DECODE-GATE: $fails clip(s) FAILED  arm='${ARM:-<default>}'"
    exit 1
fi
echo "ABR-DECODE-GATE: all clips pass (frames + psnr>=$PSNR_FLOOR)  arm='${ARM:-<default>}'"
