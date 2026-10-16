#!/usr/bin/env bash
# macOS/BSD stat(1) flags (`stat -f`); on Linux use `stat -c%s` / `-c%Y`.
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
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
#
# DECODE MODE (2026-09-16, item T-oracles): `openh264-shim.sh --decode IN.264
# OUT.yuv` runs Cisco's h264dec instead, so scripts/conformance.sh can use
# openh264 as a second recon-match oracle beside ffmpeg. Same reason this file
# exists at all -- the interface is the mismatch: h264dec takes its arguments
# positionally, writes raw I420 with no header, and prints statistics on stdout.
# The binary comes from scripts/fetch_openh264.sh (tools/openh264/), because the
# packaged openh264 ships the library and headers only.
#
# WHAT OPENH264 CANNOT DECODE, measured on 2.6.0, and why a caller must gate on
# the STREAM rather than on this script's exit status: it refuses
# frame_mbs_only_flag == 0 while parsing the sequence parameter set (it prints
# "frame_mbs_only_flag (0) not supported") and writes nothing for 4:2:2 or
# 4:4:4 -- both of which show up as an empty output file. But on B SLICES IT
# WRITES THE WRONG PICTURES AND EXITS
# 0: the frame count is right and the content is not. conformance.sh therefore
# asks `h264_syntax.py --probe` what is in the stream before it asks openh264.
set -uo pipefail

if [ "${1:-}" = "--decode" ]; then
    shift
    [ $# -eq 2 ] || { echo "openh264-shim: --decode needs IN.264 OUT.yuv" >&2; exit 2; }
    _root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    DEC="${OPENH264_DEC:-$_root/tools/openh264/h264dec}"
    [ -x "$DEC" ] || { echo "openh264-shim: no h264dec at $DEC (run scripts/fetch_openh264.sh, or set OPENH264_DEC)" >&2; exit 2; }
    rm -f "$2"
    "$DEC" "$1" "$2" >/dev/null 2>&1
    [ -s "$2" ] || exit 4                   # refused the stream outright
    exit 0
fi

OH="${OPENH264:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/../openh264/h264enc-asm}"
# A CACHE, not scratch: the raw planes are re-read across a whole board, so
# they must outlive the run. Under $TMPDIR they were a sweep away from
# vanishing mid-board; they live in the named cache now ($Y264_CACHE,
# default ~/.cache/yah264/oh264raw), which nothing deletes on its own.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/scratch.sh"
CACHE="${OH_RAW_CACHE:-$(y264_cache_dir oh264raw)}"

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
