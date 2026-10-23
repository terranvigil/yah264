#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# vmaf.sh <clip.y4m> [crf] [frames] -- encode a clip with yah264 and print its
# absolute VMAF v1 and CAMBI against the source. Mirrors scripts/bdcompare.py's
# v1 path: both sides at 10-bit 4:2:0, the model picked by the source's height
# and frame rate, CAMBI told the encode's coded bit depth. Env:
#   YAH264   encoder binary (default build/cli/yah264)
#   VMAF_V1_BIN     float-enabled libvmaf 3.x CLI (default ~/.local/opt/vmaf-v1/bin/vmaf;
#                   the Homebrew vmaf cannot load the v1 models)
#   VMAF_V1_MODELS  v1 model directory (default ~/.local/share/vmaf/model)
#   ENCCFG    encoder flags (default: --crf <crf> --cabac --bframes 2)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${YAH264:-$root/build/cli/yah264}"
clip="${1:?usage: vmaf.sh <clip.y4m> [crf] [frames]}"
crf="${2:-28}"
frames="${3:-0}"
vmaf="${VMAF_V1_BIN:-$HOME/.local/opt/vmaf-v1/bin/vmaf}"
models_dir="${VMAF_V1_MODELS:-$HOME/.local/share/vmaf/model}"

command -v ffmpeg >/dev/null 2>&1 || { echo "vmaf.sh: ffmpeg not found on PATH"; exit 2; }
[ -x "$vmaf" ] || { echo "vmaf.sh: v1 libvmaf CLI '$vmaf' not found (set VMAF_V1_BIN=/path/to/vmaf)"; exit 2; }
[ -f "$clip" ] || { echo "vmaf.sh: clip not found: $clip"; exit 2; }

. "$root/scripts/scratch.sh"
y264_scratch_dir vmaf wd
frflag=""; [ "$frames" -gt 0 ] 2>/dev/null && frflag="--frames $frames"
enccfg="${ENCCFG:---crf $crf --cabac --bframes 2}"

# shellcheck disable=SC2086
"$CLI" --input-y4m "$clip" $enccfg $frflag -o "$wd/out.264" 2>/dev/null
bytes=$(wc -c < "$wd/out.264")
ffmpeg -v error -y -i "$wd/out.264" -fps_mode passthrough -pix_fmt yuv420p10le -strict -1 "$wd/dec.y4m"

# Match the reference length to the encode so VMAF aligns cleanly (no
# "ended before" warning when only a prefix of the clip was encoded).
frref=""; [ "$frames" -gt 0 ] 2>/dev/null && frref="-frames:v $frames"
# shellcheck disable=SC2086
ffmpeg -v error -y -i "$clip" $frref -pix_fmt yuv420p10le -strict -1 "$wd/ref.y4m"

model=$(python3 - "$clip" "$models_dir" <<'PY'
import sys
head = open(sys.argv[1], "rb").readline(512).decode("ascii", "replace").split()
h = int(next(t[1:] for t in head if t.startswith("H")))
n, d = next((t[1:] for t in head if t.startswith("F")), "30:1").split(":")
hfr = "_hfr" if int(n) / max(1, int(d)) >= 50 else ""
print(f"{sys.argv[2]}/vmaf_v1.0.16{hfr}/vmaf_v1.0.16{hfr}_{'1d5h_2160' if h >= 1440 else '3d0h'}.json")
PY
)
[ -f "$model" ] || { echo "vmaf.sh: VMAF v1 model not found: $model"; exit 2; }
case "$(ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt -of csv=p=0 "$wd/out.264")" in
    *12*) ebd=12 ;; *10*) ebd=10 ;; *) ebd=8 ;;
esac
"$vmaf" -q -r "$wd/ref.y4m" -d "$wd/dec.y4m" --json -o "$wd/v.json" \
    --model "path=$model:name=vmaf_v1:cambi.enc_bitdepth=$ebd"

python3 - "$wd/v.json" "$bytes" "$crf" "$clip" "$frames" <<'PY'
import json, sys
pm = json.load(open(sys.argv[1]))["pooled_metrics"]
b, crf, clip, fr = int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
import os
print(f"{os.path.basename(clip)}  crf={crf}  frames={fr or 'all'}  size={b} bytes")
cambi = next((k for k in pm if k.startswith("cambi")), None)
for k, lab in (("vmaf_v1", "VMAF-v1"), (cambi, "CAMBI")):
    v = pm.get(k, {}).get("mean") if k else None
    if v is not None:
        print(f"  {lab:9s} {v:.3f}")
PY
