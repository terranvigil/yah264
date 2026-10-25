#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# parity-clip-calib.sh -- measure the rate/VMAF ladder for ONE clip so its
# operating point can be added to scripts/parity-clips.sh.
#
# The six points in parity-clips.sh were chosen by hand from a ladder like this
# one, and the ladder was never committed -- so the next clip added to a BD
# sweep had no way to be calibrated the same way, and wasn't. sintel_720p and
# touchdown_1080p both entered BD rounds uncalibrated, and touchdown's BD fit
# then read -11.48%, -5.70% and +41.24% across three ladders of somebody's
# choosing. This script exists so "pick the point the way the other six were
# picked" is a command rather than a memory.
#
# Selection rule (verbatim from parity-clips.sh): the lowest ABR target at which
# both encoders track within a few percent AND yah264 lands in VMAF ~88-94, the
# band where the metric still discriminates and where the encoders are actually
# deployed.
#
# The score is VMAF v1, matching perf-comp.sh and the BD gate (docs/vmaf.md).
# The 88-94 band was written in v0.6.1 units and has not been re-derived on v1,
# so read the band rule as approximate until it is: a v1 number and a v0.6.1
# number for the same encode are not the same scale. CAMBI rides along as the
# banding read.
#
# Both sides run the preset's own defaults, as perf-comp-set.sh does since
# 2026-08-11 -- no --ref/--bframes overrides, because the point of the exercise
# is the config a user actually gets.
#
# Usage: scripts/parity-clip-calib.sh <clip-name> <target,target,...>
#   e.g. scripts/parity-clip-calib.sh sintel_720p 1000,2000,3000,4000,6000
# Env: SECONDS_PER (6), PRESET (medium), REF (unset = preset default),
#      YAH264, X264, JOBS (4),
#      VMAF_V1_BIN (float-enabled libvmaf 3.x; default ~/.local/opt/vmaf-v1/bin/vmaf),
#      VMAF_V1_MODELS (default ~/.local/share/vmaf/model)
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"

[ $# -ge 2 ] || { echo "usage: $0 <clip-name> <kbps,kbps,...>" >&2; exit 2; }
clip="$1"; targets="$2"
src="$root/tests/corpus/$clip.y4m"
[ -f "$src" ] || { echo "no corpus clip $src" >&2; exit 2; }

SECONDS_PER="${SECONDS_PER:-6}"
PRESET="${PRESET:-medium}"
YAH264="${YAH264:-$root/build/cli/yah264}"
# ../x264 relative to the repo root, which is wrong inside a git worktree
# (.claude/worktrees/<x>/..) -- fall back to the sibling of the main checkout.
X264="${X264:-$root/../x264/x264-asm}"
[ -x "$X264" ] || X264="$(git -C "$root" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's:/\.git$::')/../x264/x264-asm"
VMAF="${VMAF_V1_BIN:-$HOME/.local/opt/vmaf-v1/bin/vmaf}"
VMAF_MODELS="${VMAF_V1_MODELS:-$HOME/.local/share/vmaf/model}"
JOBS="${JOBS:-4}"
REF="${REF:-}"
# Threads 1 by default, matching perf-comp-set.sh, because that is the config
# the other six points were measured at. Raise it for exploratory sweeps on long
# windows and read the x264 column with care when you do: yah264's output is
# thread-invariant by construction but x264's threaded ABR is not, so its rate
# and VMAF will shift a little with this knob. yah264's own column, which is
# what the band rule reads, does not.
THREADS="${THREADS:-1}"

for t in "$YAH264" "$X264" "$VMAF" ffmpeg ffprobe; do
    command -v "$t" >/dev/null 2>&1 || [ -x "$t" ] || { echo "missing tool: $t" >&2; exit 2; }
done

# Frame rate comes off the CLIP, never from a table -- uneven_720p.y4m is
# mislabeled on disk and any hardcoded fps turns a rate error into fiction.
fps=$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate \
      -of default=nw=1:nk=1 "$src")
fps=$(python3 -c "import sys;n,d=sys.argv[1].split('/');print(float(n)/float(d))" "$fps")
N=$(python3 -c "import sys;print(int(round(float(sys.argv[1])*float(sys.argv[2]))))" "$fps" "$SECONDS_PER")

. "$root/scripts/scratch.sh"
y264_scratch_dir clipcalib wd
ref="$wd/ref.y4m"
ffmpeg -v error -y -i "$src" -frames:v "$N" "$ref" || exit 2
# Trim, THEN count. Asking for more frames than the clip has is silent: ffmpeg
# writes what exists and every rate computed against the requested N is wrong by
# the shortfall. touchdown_420 is 150 frames, so a 6-second window at 29.97 fps
# asks for 180 and reports both encoders undershooting by exactly 150/180 -- a
# clean, symmetric, entirely fictional -16.7%.
have=$(ffprobe -v error -count_frames -select_streams v:0 \
       -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$ref")
if [ "$have" -lt "$N" ]; then
    echo ">> NOTE $clip has only $have frames; asked for $N. Window is $have."
    N="$have"
fi

# The v1 model follows the source: the 3H model below 1440 lines, the 4K 1.5H
# model at and above, the high-frame-rate twin of either at 50 fps and up. Both
# sides are scored at 10-bit 4:2:0, so the reference is converted once here.
v1model=$(python3 -c "
import sys
h, fps, d = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
hfr = '_hfr' if fps >= 50 else ''
print(f\"{d}/vmaf_v1.0.16{hfr}/vmaf_v1.0.16{hfr}_{'1d5h_2160' if h >= 1440 else '3d0h'}.json\")" \
    "$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$ref")" \
    "$fps" "$VMAF_MODELS")
[ -f "$v1model" ] || { echo "missing VMAF v1 model: $v1model" >&2; exit 2; }
ref10="$wd/ref10.y4m"
ffmpeg -v error -y -i "$ref" -pix_fmt yuv420p10le -strict -1 "$ref10" || exit 2
# v1 at display resolution (docs/vmaf.md): a picture smaller than 1920x1080 in
# both dimensions is fitted to it, both sides, and CAMBI is told the coded size
geo=$(python3 "$root/scripts/bdcompare.py" --v1-display "$ref") && [ -n "$geo" ] || { echo "$0: cannot read the v1 display geometry" >&2; exit 2; }
read -r v1scaled v1w v1h v1vf <<< "$geo"
v1disp() {  # <10-bit y4m> -> the file v1 scores: itself, or a display-fitted copy
    if [ "$v1scaled" = 1 ]; then
        ffmpeg -v error -y -i "$1" -vf "$v1vf" -pix_fmt yuv420p10le -strict -1 "$1.disp.y4m"
        echo "$1.disp.y4m"
    else
        echo "$1"
    fi
}
v1copts() {  # <coded bit depth> -> the --model options after the path
    if [ "$v1scaled" = 1 ]; then
        echo "name=vmaf_v1:cambi.enc_bitdepth=$1:cambi.enc_width=$v1w:cambi.enc_height=$v1h"
    else
        echo "name=vmaf_v1:cambi.enc_bitdepth=$1"
    fi
}
ref10d=$(v1disp "$ref10")

nargs="--preset $PRESET --cabac --transform-8x8"
xargs_="--preset $PRESET"
if [ -n "$REF" ]; then nargs="$nargs --ref $REF"; xargs_="$xargs_ --ref $REF"; fi

vmaf_of() {  # <decoded.y4m> <bitstream> -> "<v1> <CAMBI>"
    # CAMBI is told the encode's coded bit depth, since v1 scores at 10-bit
    # whatever the stream was.
    # json path is derived from the (per-point unique) decode path, so parallel
    # points never share one.
    local ebd=8
    case "$(ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt -of csv=p=0 "$2")" in
        *12*) ebd=12 ;; *10*) ebd=10 ;;
    esac
    local d
    d=$(v1disp "$1")
    "$VMAF" -r "$ref10d" -d "$d" --model "path=$v1model:$(v1copts "$ebd")" \
        --json -o "$1.json" >/dev/null 2>&1 || { [ "$d" = "$1" ] || rm -f "$d"; echo "NaN NaN"; return; }
    [ "$d" = "$1" ] || rm -f "$d"
    python3 -c "
import json,sys
pm=json.load(open(sys.argv[1]))['pooled_metrics']
cambi=next(k for k in pm if k.startswith('cambi'))
print(f\"{pm['vmaf_v1']['mean']:.2f} {pm[cambi]['mean']:.2f}\")" "$1.json"
}

kbps_of() {  # <bitstream> -> achieved kbit/s over the trimmed window
    python3 -c "import os,sys;print(f'{os.path.getsize(sys.argv[1])*8*float(sys.argv[2])/int(sys.argv[3])/1000:.1f}')" \
        "$1" "$fps" "$N"
}

pct() { python3 -c "import sys;print(f'{float(sys.argv[1])/float(sys.argv[2])*100-100:+.1f}')" "$1" "$2"; }

one_point() {  # <target> -> one table row, written to $wd/row.<target>
    local br="$1" nk xk nv nn xv xn
    "$YAH264" --input-y4m "$ref" $nargs --bitrate "$br" --threads "$THREADS" \
        -o "$wd/n$br.264" >/dev/null 2>&1
    "$X264" $xargs_ --bitrate "$br" --threads "$THREADS" --demuxer y4m \
        -o "$wd/x$br.264" "$ref" >/dev/null 2>&1
    if [ ! -s "$wd/n$br.264" ] || [ ! -s "$wd/x$br.264" ]; then
        printf '%8s  ENCODE FAILED\n' "$br" > "$wd/row.$br"; return
    fi
    nk=$(kbps_of "$wd/n$br.264"); xk=$(kbps_of "$wd/x$br.264")
    ffmpeg -v error -y -i "$wd/n$br.264" -fps_mode passthrough -pix_fmt yuv420p10le -strict -1 "$wd/n$br.y4m"
    ffmpeg -v error -y -i "$wd/x$br.264" -fps_mode passthrough -pix_fmt yuv420p10le -strict -1 "$wd/x$br.y4m"
    read -r nv nn <<<"$(vmaf_of "$wd/n$br.y4m" "$wd/n$br.264")"
    read -r xv xn <<<"$(vmaf_of "$wd/x$br.y4m" "$wd/x$br.264")"
    rm -f "$wd/n$br.y4m" "$wd/x$br.y4m" "$wd/n$br.y4m.json" "$wd/x$br.y4m.json"
    printf '%8s %10s %8s %10s %8s %9s %9s %9s %9s\n' \
        "$br" "$nk" "$(pct "$nk" "$br")%" "$xk" "$(pct "$xk" "$br")%" \
        "$nv" "$nn" "$xv" "$xn" > "$wd/row.$br"
}

echo ">> $clip  ${N} frames @ ${fps} fps (${SECONDS_PER}s)  preset=$PRESET${REF:+ ref=$REF} threads=$THREADS"
echo ">> band rule: lowest target where BOTH rate errors are small AND y264v1 is 88-94 (band from v0.6.1, not yet re-derived on v1)"
printf '%8s %10s %8s %10s %8s %9s %9s %9s %9s\n' \
    target y264kbps y264err x264kbps x264err y264v1 y264CAMBI x264v1 x264CAMBI
printf '%8s %10s %8s %10s %8s %9s %9s %9s %9s\n' \
    -------- ---------- -------- ---------- -------- --------- --------- --------- ---------

# Points run concurrently (each is a self-contained encode pair writing its own
# files); rows are printed in ladder order afterwards, not completion order.
# Everything measured here is deterministic, so contention cannot change a
# number -- only the wall clock, which this script does not report.
list="${targets//,/ }"
running=0
for br in $list; do
    one_point "$br" &
    running=$((running + 1))
    if [ "$running" -ge "$JOBS" ]; then wait -n 2>/dev/null || wait; running=$((running - 1)); fi
done
wait
for br in $list; do cat "$wd/row.$br" 2>/dev/null; done
