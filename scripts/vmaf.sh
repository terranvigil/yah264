#!/usr/bin/env bash
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# vmaf.sh <clip.y4m> [crf] [frames] -- encode a clip with next264 and print its
# absolute VMAF / VMAF-NEG (and VMAF-v1 if NEXT264_VMAF_MODEL is set) against the
# source. Mirrors scripts/bdcompare.py's decode+vmaf path. Env:
#   NEXT264   encoder binary (default build/cli/next264)
#   VMAF      libvmaf CLI (default 'vmaf')
#   NEXT264_VMAF_MODEL  path to the enable_float VMAF v1 model (optional)
#   ENCCFG    encoder flags (default: --crf <crf> --cabac --bframes 2)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${NEXT264:-$root/build/cli/next264}"
clip="${1:?usage: vmaf.sh <clip.y4m> [crf] [frames]}"
crf="${2:-28}"
frames="${3:-0}"
vmaf="${VMAF:-vmaf}"

command -v ffmpeg >/dev/null 2>&1 || { echo "vmaf.sh: ffmpeg not found on PATH"; exit 2; }
command -v "$vmaf" >/dev/null 2>&1 || { echo "vmaf.sh: vmaf CLI '$vmaf' not found (set VMAF=/path/to/vmaf)"; exit 2; }
[ -f "$clip" ] || { echo "vmaf.sh: clip not found: $clip"; exit 2; }

wd="$(mktemp -d)"; trap 'rm -rf "$wd"' EXIT
frflag=""; [ "$frames" -gt 0 ] 2>/dev/null && frflag="--frames $frames"
enccfg="${ENCCFG:---crf $crf --cabac --bframes 2}"

# shellcheck disable=SC2086
"$CLI" --input-y4m "$clip" $enccfg $frflag -o "$wd/out.264" 2>/dev/null
bytes=$(wc -c < "$wd/out.264")
ffmpeg -v error -y -i "$wd/out.264" -pix_fmt yuv420p "$wd/dec.y4m"

# Match the reference length to the encode so VMAF aligns cleanly (no
# "ended before" warning when only a prefix of the clip was encoded).
ref="$clip"
if [ "$frames" -gt 0 ] 2>/dev/null; then
    ffmpeg -v error -y -i "$clip" -frames:v "$frames" "$wd/ref.y4m"
    ref="$wd/ref.y4m"
fi

models="--model version=vmaf_v0.6.1:name=vmaf --model version=vmaf_v0.6.1neg:name=vmaf_neg"
if [ -n "${NEXT264_VMAF_MODEL:-}" ] && [ -f "${NEXT264_VMAF_MODEL}" ]; then
    models="--model path=${NEXT264_VMAF_MODEL}:name=vmaf_v1 $models"
fi
# shellcheck disable=SC2086
"$vmaf" -r "$ref" -d "$wd/dec.y4m" --json -o "$wd/v.json" $models

python3 - "$wd/v.json" "$bytes" "$crf" "$clip" "$frames" <<'PY'
import json, sys
pm = json.load(open(sys.argv[1]))["pooled_metrics"]
b, crf, clip, fr = int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
import os
print(f"{os.path.basename(clip)}  crf={crf}  frames={fr or 'all'}  size={b} bytes")
for k, lab in (("vmaf_v1", "VMAF-v1"), ("vmaf", "VMAF"), ("vmaf_neg", "VMAF-NEG")):
    v = pm.get(k, {}).get("mean")
    if v is not None:
        print(f"  {lab:9s} {v:.3f}")
PY
