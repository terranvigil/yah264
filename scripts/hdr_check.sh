#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# hdr_check.sh -- the HDR check behind site/hdr.md: two HDR10 clips from
# Netflix Open Content, a CRF ladder at 1080p and 4K, each encode scored with
# HDR-VMAF v1.1.8 (and its PQ CAMBI).
#
#   YAH264=build-rel/cli/yah264 \
#   VMAF_HDR_BIN=/path/to/vmaf VMAF_HDR_MODELS=/path/to/vmaf/model \
#   WORK=/scratch/hdr scripts/hdr_check.sh
#
# VMAF_HDR_BIN is a libvmaf `vmaf` tool built with -Denable_zimg=true, which
# the HDR models need for their colorspace conversion. VMAF_HDR_MODELS is the
# repository's model/ directory, holding hdr_vmaf_v1.1.8 and its _hfr twin.
# Needs ffmpeg with zscale, curl, python3 and numpy. The downloads are about
# 17 GB (300 Sparks TIFF frames and 10 s of Sol Levante ProRes), and every step
# is skipped when its output already exists, so a rerun only scores.
set -euo pipefail

Y=${YAH264:-build-rel/cli/yah264}
VMAF=${VMAF_HDR_BIN:?set VMAF_HDR_BIN to a zimg-enabled libvmaf vmaf tool}
M=${VMAF_HDR_MODELS:?set VMAF_HDR_MODELS to libvmaf model/ directory}
W=${WORK:?set WORK to a scratch directory with ~30 GB free}
CRFS=${CRFS:-18 22 26 30 34}
THREADS=${THREADS:-6}
Y=$(cd "$(dirname "$Y")" && pwd)/$(basename "$Y")
NOC=https://s3.amazonaws.com/download.opencontent.netflix.com
# both titles were graded on a 1000-nit P3 D65 display
MD='G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)'
COLOR=(--colorprim bt2020 --transfer smpte2084 --colormatrix bt2020 --range limited)
VCOLOR=()
for s in ref dist; do
  VCOLOR+=(--color_range_$s limited --color_primaries_$s bt2020 --color_trc_$s pq --color_matrix_$s bt2020nc)
done

mkdir -p "$W/sparks_tif" "$W/enc"
cd "$W"

fetch() {
  [ -s sol_src.mov ] || ffmpeg -v error -nostdin -ss 160 \
    -i "$NOC/SolLevante/hdr10/SolLevante_HDR10_r2020_ST2084_UHD_24fps_1000nit.mov" \
    -frames:v 240 -c:v copy -an -y sol_src.mov
  seq -f "%05g" 5880 6179 | xargs -P 8 -I{} sh -c '[ -s sparks_tif/{}.tif ] ||
    curl -sf -o sparks_tif/{}.tif "$0/sparks/HDR10_1000nit_PQ_2020_image_sequence/SPARKS_HDR10_4K_2020_PQ_1000nits_{}.tif"' "$NOC"
}

y4m() { local out=$1; shift; ffmpeg -v error -nostdin "$@" -strict -1 -f yuv4mpegpipe -y "$out"; }

prep() {
  [ -s sol_2160.y4m ] || y4m sol_2160.y4m -i sol_src.mov -vf "zscale=f=spline36,format=yuv420p10le"
  # the TIFFs are untagged RGB, and DCI-wide; crop to UHD around the center
  [ -s sparks_2160.y4m ] || y4m sparks_2160.y4m -framerate 60000/1001 -start_number 5880 -i sparks_tif/%05d.tif \
    -vf "crop=3840:2160,setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=gbr:range=pc,zscale=m=2020_ncl:r=tv:f=spline36,format=yuv420p10le"
  for c in sol sparks; do
    [ -s ${c}_1080.y4m ] || y4m ${c}_1080.y4m -i ${c}_2160.y4m -vf "zscale=w=1920:h=1080:f=spline36"
    [ -s $c.cll ] || cll ${c}_2160.y4m > $c.cll
  done
}

# MaxCLL,MaxFALL in cd/m^2 per CTA-861.3: the brightest max(R,G,B) of any
# pixel, and the brightest frame's mean of it
cll() {
  ffmpeg -v error -nostdin -i "$1" -vf \
    "zscale=tin=smpte2084:min=2020_ncl:pin=bt2020:rin=tv:t=linear:npl=10000:p=bt2020,format=gbrpf32le" \
    -f rawvideo - | python3 -c '
import sys, numpy as np
n = 3840 * 2160
cll = fall = 0.0
while True:
    b = sys.stdin.buffer.read(n * 12)
    if len(b) < n * 12:
        break
    m = np.frombuffer(b, np.float32).reshape(3, n).max(axis=0) * 10000
    cll, fall = max(cll, float(m.max())), max(fall, float(m.mean()))
print(f"{round(cll)},{round(fall)}")'
}

model() {
  local d=hdr_vmaf_v1.1.8 h=""
  [ $1 = sparks ] && { d=hdr_vmaf_v1.1.8_hfr; h=_hfr; }
  [ $2 = 2160 ] && echo "$M/$d/hdr_vmaf_v1.1.8${h}_1d5h_2160.json" || echo "$M/$d/hdr_vmaf_v1.1.8${h}_3d0h.json"
}

one() {
  local c=$1 r=$2 q=$3 o=enc/$1_$2_crf$3
  [ -s $o.264 ] || "$Y" --input-y4m ${c}_${r}.y4m -o $o.264 --crf $q --threads "$THREADS" --no-progress \
    "${COLOR[@]}" --mastering-display "$MD" --cll "$(cat $c.cll)" 2> $o.log
  if [ ! -s $o.json ]; then
    y4m $o.y4m -i $o.264 -pix_fmt yuv420p10le
    "$VMAF" --reference ${c}_${r}.y4m --distorted $o.y4m --threads "$THREADS" "${VCOLOR[@]}" \
      --model path="$(model $c $r)" --json --output $o.json > $o.vmaf.log 2>&1
    rm -f $o.y4m
  fi
  python3 - "$c" "$r" "$q" "$o" <<'EOF'
import json, os, sys
c, r, q, o = sys.argv[1:]
secs = {"sol": 240 / 24, "sparks": 300 / (60000 / 1001)}[c]
m = json.load(open(o + ".json"))["pooled_metrics"]
cambi = next(v["mean"] for k, v in m.items() if k.startswith("cambi"))
print(f"{c:7s} {r:>5s}p  crf {q:>2s}  {os.path.getsize(o + '.264') * 8 / secs / 1e6:7.2f} Mbit/s"
      f"  HDR-VMAF {m['vmaf']['mean']:6.2f}  CAMBI {cambi:5.2f}")
EOF
}

fetch
prep
for c in sol sparks; do for r in 1080 2160; do for q in $CRFS; do one $c $r $q; done; done; done
