#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# openh264-shim.sh -- present Cisco's h264enc behind a yah264/x264-shaped CLI so
# the EXISTING harnesses (scripts/bdcompare.py, scripts/perf-comp.sh) can drive it
# unchanged. Nothing here re-implements a comparison; it is purely an adapter.
#
#   usage: openh264-shim.sh [--qp N | --bitrate K] [--threads N] -o OUT SRC.y4m
#
# WHY AN ADAPTER AND NOT A NEW HARNESS. bdcompare takes free-form command
# templates ({src}/{q}/{out}) and perf-comp takes a binary path, so both already
# generalise to a third encoder. What does NOT generalise is the interface:
# h264enc reads RAW I420 only, needs the dimensions and frame count passed
# explicitly, and takes per-layer arguments. That mismatch is this file.
#
# THE RAW-CONVERSION CACHE IS A CORRECTNESS REQUIREMENT, NOT AN OPTIMISATION.
# perf-comp wall-clocks whatever it invokes. A shim that ran ffmpeg on every call
# would bill y4m->yuv conversion to openh264's encode time and report a slower
# encoder than exists. So the conversion lands in a cache keyed by (src, size,
# mtime) and every timed call hits it. Populate it before timing (any warmup run
# does). The write is atomic because bdcompare encodes 16-way parallel and two
# jobs on one clip would otherwise race on a half-written file.
#
# OPENH264 IS A DIFFERENT DESIGN POINT AND THE ROW MUST SAY SO. It targets
# realtime/WebRTC: NO B-frames at all, no trellis, no mb-tree. Comparing it
# against a B-frame configuration measures that design gap, not implementation
# quality, so drive the other encoders with --bframes 0 for any matched claim.
set -uo pipefail

OH="${OPENH264:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/../openh264/h264enc-asm}"
CACHE="${OH_RAW_CACHE:-${TMPDIR:-/tmp}/oh264raw}"

qp=""; bitrate=""; threads=1; out=""; src=""
while [ $# -gt 0 ]; do
    case "$1" in
        --qp|--crf)   qp="$2"; shift 2 ;;
        --bitrate)    bitrate="$2"; shift 2 ;;
        --threads)    threads="$2"; shift 2 ;;
        -o)           out="$2"; shift 2 ;;
        --input-y4m)  src="$2"; shift 2 ;;
        # Flags that TAKE A VALUE must swallow both tokens. Getting this wrong
        # is not a no-op: a one-token shift leaves the value ("--ref 3" -> "3")
        # to be picked up as the positional source, and the shim then probes a
        # file named 3. Keep this list in sync with what the harnesses pass.
        --preset|--tune|--stats|--pass|--ref|--bframes|--keyint|--min-keyint|\
        --subme|--me|--merange|--qcomp|--aq-strength|--deblock|--demuxer|\
        --input-res|--fps|--profile|--level) shift 2 ;;
        --cabac|--transform-8x8|--no-asm|--8x8dct) shift ;;   # valueless, ignored
        -*)           shift ;;                       # unknown valueless flag
        *)            src="$1"; shift ;;             # positional source (x264 shape)
    esac
done
[ -n "$out" ] && [ -n "$src" ] || { echo "openh264-shim: need -o OUT and a source" >&2; exit 2; }
[ -x "$OH" ] || { echo "openh264-shim: no h264enc at $OH (set OPENH264)" >&2; exit 2; }

mkdir -p "$CACHE"
key=$(printf '%s|%s|%s' "$src" "$(stat -f%z "$src")" "$(stat -f%m "$src")" | shasum | cut -c1-20)
raw="$CACHE/$key.yuv"
meta="$CACHE/$key.meta"

# METADATA IS CACHED FOR THE SAME REASON THE PIXELS ARE: this runs inside
# perf-comp's timed region. `ffprobe -count_frames` DECODES THE WHOLE CLIP to
# count, so calling it per invocation billed a full extra decode to openh264 and
# reported a slower encoder than exists (measured: it moved foreman's ratio by
# more than the result being sought). The frame count is derived from the raw
# file's size instead -- exact for I420, and free.
if [ ! -s "$raw" ] || [ ! -s "$meta" ]; then
    read -r w h fps < <(ffprobe -v error -select_streams v:0 \
        -show_entries stream=width,height,r_frame_rate -of csv=p=0 "$src" |
        awk -F, '{split($3,r,"/"); printf "%s %s %s\n", $1, $2, (r[2]?r[1]/r[2]:r[1])}')
    tmp="$raw.$$.part"
    ffmpeg -v error -y -i "$src" -pix_fmt yuv420p -f rawvideo "$tmp" || exit 3
    mv -f "$tmp" "$raw"
    frames=$(( $(stat -f%z "$raw") / (w * h * 3 / 2) ))
    printf '%s %s %s %s\n' "$w" "$h" "$fps" "$frames" > "$meta.$$.part"
    mv -f "$meta.$$.part" "$meta"
fi
read -r w h fps frames < "$meta"
[ "${frames:-0}" -gt 0 ] 2>/dev/null || { echo "openh264-shim: no frames in $src" >&2; exit 2; }

# rc -1 = rate control OFF (fixed QP, the analogue of a --qp sweep); rc 1 =
# bitrate mode, -tarb in kbps. -iper -1 leaves the GOP to the encoder, matching
# how the other two are driven in these comparisons.
if [ -n "$qp" ]; then
    rc=(-rc -1 -lqp 0 "$qp")
else
    rc=(-rc 1 -tarb "${bitrate:-500}" -ltarb 0 "${bitrate:-500}")
fi

# -fs 0 DISABLES FRAME SKIPPING, and it is mandatory for any comparison.
# openh264's rate control drops frames under rate pressure (its default is on).
# Measured on stefan_cif at 500 kbps: 82 of 90 frames survived, which misaligns
# every subsequent frame against the reference and hands VMAF a garbage score --
# -73 to -85 on the hard clips, numbers that look like a catastrophic quality
# result and are really a frame-count mismatch. The other two encoders here
# cannot skip frames, so leaving this on compares different-length videos.
exec "$OH" -org "$raw" -sw "$w" -sh "$h" -frms "$frames" -frin "$fps" \
    -numl 1 -dw 0 "$w" -dh 0 "$h" -frout 0 "$fps" \
    "${rc[@]}" -fs 0 -cabac 1 -threadIdc "$threads" -iper -1 -bf "$out" >/dev/null 2>&1
