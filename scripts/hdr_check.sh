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
# 17 GB (300 Sparks TIFF frames and 10 s of Sol Levante ProRes), and WORK needs
# about 45 GB in all. Every step writes under a temporary name and keeps its
# output only when it finishes, so an interrupted run picks up where it
# stopped, and a run over finished outputs just prints the table again.
# Delete WORK/enc after rebuilding the encoder, or the old encodes are reused.
# The site's numbers were made at the default THREADS=6; another thread count
# gives slightly different streams.
set -euo pipefail

abs() { case $1 in /*) echo "$1" ;; */*) echo "$PWD/$1" ;; *) command -v "$1" || echo "$1" ;; esac; }
Y=$(abs "${YAH264:-build-rel/cli/yah264}")
VMAF=$(abs "${VMAF_HDR_BIN:?set VMAF_HDR_BIN to a zimg-enabled libvmaf vmaf tool}")
M=$(abs "${VMAF_HDR_MODELS:?set VMAF_HDR_MODELS to libvmaf model/ directory}")
W=${WORK:?set WORK to a scratch directory with ~45 GB free}
CRFS=${CRFS:-18 22 26 30 34}
THREADS=${THREADS:-6}
NOC=https://s3.amazonaws.com/download.opencontent.netflix.com
# both titles were graded on a 1000-nit P3 D65 display
MD='G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)'
COLOR=(--colorprim bt2020 --transfer smpte2084 --colormatrix bt2020 --range limited)
VCOLOR=()
for s in ref dist; do
  VCOLOR+=("--color_range_$s" limited "--color_primaries_$s" bt2020 "--color_trc_$s" pq "--color_matrix_$s" bt2020nc)
done
frames() { case $1 in sol) echo 240 ;; sparks) echo 300 ;; esac; }
fps() { case $1 in sol) echo 24 ;; sparks) echo 60000/1001 ;; esac; }

die() { echo "hdr_check: $*" >&2; exit 1; }
[ -x "$Y" ] || die "no encoder at $Y (set YAH264)"
[ -x "$VMAF" ] || die "no vmaf tool at $VMAF"
for d in hdr_vmaf_v1.1.8 hdr_vmaf_v1.1.8_hfr; do [ -d "$M/$d" ] || die "no $d under $M"; done
command -v curl > /dev/null || die "needs curl"
ffmpeg -hide_banner -filters 2> /dev/null | grep -q ' zscale ' || die "needs ffmpeg with zscale"
python3 -c 'import numpy' 2> /dev/null || die "needs python3 with numpy"

mkdir -p "$W/sparks_tif" "$W/enc"
cd "$W"

# frames in a video file, counted by decoding packets
count() { ffprobe -v error -count_packets -select_streams v:0 -show_entries stream=nb_read_packets -of csv=p=0 "$1"; }

fetch() {
  if [ ! -s sol_src.mov ]; then
    ffmpeg -v error -nostdin -ss 160 \
      -i "$NOC/SolLevante/hdr10/SolLevante_HDR10_r2020_ST2084_UHD_24fps_1000nit.mov" \
      -frames:v 240 -c:v copy -an -f mov -y sol_src.mov.tmp
    [ "$(count sol_src.mov.tmp)" = 240 ] || die "Sol Levante download came out short"
    mv sol_src.mov.tmp sol_src.mov
  fi
  seq -f "%05g" 5880 6179 | xargs -P 8 -I{} sh -c '[ -s sparks_tif/{}.tif ] || {
    curl -sf --retry 3 -o sparks_tif/{}.tif.tmp "$0/sparks/HDR10_1000nit_PQ_2020_image_sequence/SPARKS_HDR10_4K_2020_PQ_1000nits_{}.tif" &&
    mv sparks_tif/{}.tif.tmp sparks_tif/{}.tif; }' "$NOC"
}

# write $out through a temporary name; with a clip name, check its frame count
y4m() {
  local out=$1 clip=$2; shift 2
  ffmpeg -v error -nostdin "$@" -strict -1 -f yuv4mpegpipe -y "$out.tmp"
  [ -z "$clip" ] || [ "$(count "$out.tmp")" = "$(frames $clip)" ] || die "$out has the wrong frame count"
  mv "$out.tmp" "$out"
}

# the TIFFs are untagged RGB, and DCI-wide; crop to UHD around the center
SPARKS_IN=(-framerate 60000/1001 -start_number 5880 -i sparks_tif/%05d.tif)
SPARKS_RGB="crop=3840:2160,setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=gbr:range=pc"

prep() {
  [ -s sol_2160.y4m ] || y4m sol_2160.y4m sol -i sol_src.mov -vf "zscale=f=spline36,format=yuv420p10le"
  [ -s sparks_2160.y4m ] || y4m sparks_2160.y4m sparks "${SPARKS_IN[@]}" \
    -vf "$SPARKS_RGB,zscale=m=2020_ncl:r=tv:f=spline36,format=yuv420p10le"
  for c in sol sparks; do
    [ -s ${c}_1080.y4m ] || y4m ${c}_1080.y4m $c -i ${c}_2160.y4m -vf "zscale=w=1920:h=1080:f=spline36"
  done
  # light levels come from the masters, before 4:2:0: rebuilding RGB from
  # subsampled chroma overshoots at bright edges and inflates MaxCLL
  [ -s sol.cll ] || cll sol 3840 2160 -i sol_src.mov -vf "zscale=t=linear:npl=10000:p=bt2020,format=gbrpf32le"
  [ -s sparks.cll ] || cll sparks 3840 2160 "${SPARKS_IN[@]}" -vf "$SPARKS_RGB,zscale=t=linear:npl=10000,format=gbrpf32le"
}

# MaxCLL,MaxFALL in cd/m^2 per CTA-861.3: the brightest max(R,G,B) of any
# pixel, and the brightest frame's mean of it
cll() {
  local c=$1 w=$2 h=$3; shift 3
  ffmpeg -v error -nostdin "$@" -f rawvideo - | python3 -c '
import sys, numpy as np
n, want = int(sys.argv[1]) * int(sys.argv[2]), int(sys.argv[3])
cll = fall = 0.0
got = 0
while True:
    b = sys.stdin.buffer.read(n * 12)
    if len(b) < n * 12:
        break
    m = np.frombuffer(b, np.float32).reshape(3, n).max(axis=0) * 10000
    cll, fall = max(cll, float(m.max())), max(fall, float(m.mean()))
    got += 1
if got != want or cll <= 0:
    sys.exit(f"cll: read {got} of {want} frames, MaxCLL {cll:.0f}")
print(f"{round(cll)},{round(fall)}")' "$w" "$h" "$(frames $c)" > $c.cll.tmp
  mv $c.cll.tmp $c.cll
}

model() {
  local d=hdr_vmaf_v1.1.8 h=""
  [ $1 = sparks ] && { d=hdr_vmaf_v1.1.8_hfr; h=_hfr; }
  [ $2 = 2160 ] && echo "$M/$d/hdr_vmaf_v1.1.8${h}_1d5h_2160.json" || echo "$M/$d/hdr_vmaf_v1.1.8${h}_3d0h.json"
}

failed() { echo "hdr_check: $1 failed, from $2:" >&2; tail -5 "$2" >&2; exit 1; }

one() {
  local c=$1 r=$2 q=$3 o=enc/$1_$2_crf$3
  if [ ! -s $o.264 ]; then
    "$Y" --input-y4m ${c}_${r}.y4m -o $o.264.tmp --crf $q --threads "$THREADS" --no-progress \
      "${COLOR[@]}" --mastering-display "$MD" --cll "$(cat $c.cll)" 2> $o.log || failed "encode $o" $o.log
    mv $o.264.tmp $o.264
  fi
  # a score older than its encode belongs to an earlier encode
  if [ ! -s $o.json ] || [ $o.264 -nt $o.json ]; then
    # expanded now, while $o is in scope; a killed decode leaves ~7 GB at 4K
    # shellcheck disable=SC2064
    trap "rm -f '$W/$o.y4m.tmp'" EXIT
    y4m $o.y4m $c -i $o.264 -pix_fmt yuv420p10le
    "$VMAF" --reference ${c}_${r}.y4m --distorted $o.y4m --threads "$THREADS" "${VCOLOR[@]}" \
      --model path="$(model $c $r)" --json --output $o.json.tmp > $o.vmaf.log 2>&1 || failed "score $o" $o.vmaf.log
    rm -f $o.y4m
    trap - EXIT
    mv $o.json.tmp $o.json
  fi
  python3 - "$o" "$(frames $c)" "$(fps $c)" <<'EOF'
import json, os, sys
from fractions import Fraction
o, want, fps = sys.argv[1], int(sys.argv[2]), Fraction(sys.argv[3])
j = json.load(open(o + ".json"))
if len(j["frames"]) != want:
    sys.exit(f"{o}: scored {len(j['frames'])} of {want} frames")
m = j["pooled_metrics"]
cambi = next(v for k, v in m.items() if k.startswith("cambi"))
c, r, q = os.path.basename(o).split("_")
secs = want / fps
print(f"{c:7s} {r:>5s}p  {q:>6s}  {os.path.getsize(o + '.264') * 8 / float(secs) / 1e6:7.2f} Mbit/s"
      f"  HDR-VMAF {m['vmaf']['mean']:6.2f}  CAMBI mean {cambi['mean']:5.2f} max {cambi['max']:5.2f}")
EOF
}

fetch
prep
for c in sol sparks; do for r in 1080 2160; do for q in $CRFS; do one $c $r $q; done; done; done
