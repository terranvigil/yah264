#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# pipe_window_check.sh - a piped Y4M longer than (threads + 1) x keyint must
# finish, and its stream must decode to every input frame. Whether it codes the
# same bitstream as the same file read by path is reported as INFO only:
# repeatable byte-exact output is not a requirement (owner, 2026-09-25), and
# the regression this guards is the hang, not the bytes.
#
# A pipe has no length, so the threaded path learns GOP boundaries as it reads
# them and narrows its read-ahead window to (workers + 1) x 16 frames once the
# split is decided. A worker that finished a GOP and waited for the next
# boundary did not count as starved, so the reader parked at the window with the
# boundary a keyint away: `ffmpeg ... | yah264 --input-y4m -` hung at any thread
# count whose window was below keyint, which is every default on a machine with
# fewer than ~15 cores. The shipped build only met pipes at 18 threads, where the
# window happens to exceed keyint, so nothing caught it until 2026-09-24.
#
# Tiny synthetic frames keep it to seconds; the alarm turns a hang into a FAIL.
#
#   scripts/pipe_window_check.sh [build-dir]
set -u
BUILD=${1:-build}
Y=$BUILD/cli/yah264
command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null || { echo ">> pipe window check skipped (no ffmpeg/ffprobe)"; exit 0; }
[ -x "$Y" ] || { echo "pipe window check: no binary at $Y"; exit 1; }

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
NF=420
clip=$work/clip.y4m
ffmpeg -v error -f lavfi -i testsrc2=size=192x144:rate=30 -frames:v "$NF" \
    -pix_fmt yuv420p "$clip" || { echo "pipe window check: clip generation failed"; exit 1; }

fail=0
for t in 1 2 4; do
    perl -e 'alarm 60; exec @ARGV' "$Y" --input-y4m - --threads "$t" --keyint 100 \
        -o "$work/pipe.264" --quiet --no-progress < <(cat "$clip") >/dev/null 2>&1
    rc=$?
    "$Y" --input-y4m "$clip" --threads "$t" --keyint 100 \
        -o "$work/file.264" --quiet --no-progress >/dev/null 2>&1
    if [ $rc -eq 142 ]; then
        echo "FAIL threads $t: the piped encode hung"; fail=1
    elif [ $rc -ne 0 ] || [ ! -s "$work/pipe.264" ]; then
        echo "FAIL threads $t: the piped encode exited $rc"; fail=1
    else
        nf=$(ffprobe -v error -count_frames -select_streams v:0 \
            -show_entries stream=nb_read_frames -of csv=p=0 "$work/pipe.264" 2>/dev/null)
        if [ "${nf:-0}" != "$NF" ]; then
            echo "FAIL threads $t: the piped stream decodes to ${nf:-0} frames, want $NF"; fail=1
        elif ! cmp -s "$work/pipe.264" "$work/file.264"; then
            if [ "$t" = 1 ]; then       # one thread must repeat (owner, 2026-09-25)
                echo "FAIL threads 1: pipe and file bitstreams differ"; fail=1
            else
                echo "INFO threads $t: pipe and file bitstreams differ (not a failure)"
            fi
        fi
    fi
    rm -f "$work/pipe.264" "$work/file.264"
done
# A read-ahead window overridden below --keyint (Y264_STREAM_WINDOW). One
# thread, and a split forced onto more workers than the window holds GOPs,
# both wait for the split to be decidable before any worker exists; they
# used to park the reader at the window and wait forever (2026-09-29).
for spec in "1 30" "1 1" "4 30 Y264_GOP_FORCE_G=4"; do
    set -- $spec
    t=$1 w=$2 extra=${3:-}
    perl -e 'alarm 60; exec @ARGV' env Y264_STREAM_WINDOW="$w" $extra "$Y" --input-y4m - \
        --threads "$t" --keyint 100 -o "$work/pipe.264" --quiet --no-progress \
        < <(cat "$clip") >/dev/null 2>&1
    rc=$?
    if [ $rc -eq 142 ]; then
        echo "FAIL threads $t window $w ${extra}: the piped encode hung"; fail=1
    elif [ $rc -ne 0 ] || [ ! -s "$work/pipe.264" ]; then
        echo "FAIL threads $t window $w ${extra}: the piped encode exited $rc"; fail=1
    fi
    rm -f "$work/pipe.264"
done
# A piped 2160p encode at --threads 0 with no memory override. The input
# pre-check used to price (threads + 1) x keyint frames of input, which on a
# box with many cores is more than half of physical RAM at 2160p, and refused
# to start; the store that actually bounds the input is capped far below it.
#
# The store never goes below two GOPs, which at the default keyint is 500
# frames of 2160p, 5.8 GiB, and the limit is half of physical RAM. Under
# 16 GiB the encoder is right to refuse, so the leg is skipped there and says so.
uhd=$work/uhd.y4m
uhd_note="a piped 2160p auto-thread encode finish"
ram_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null)
[ -n "$ram_kb" ] || ram_kb=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1024 ))
if [ "$ram_kb" -gt 0 ] && [ "$ram_kb" -lt $((16 * 1024 * 1024)) ]; then
    echo "INFO 2160p piped leg skipped: $((ram_kb / 1024)) MiB of RAM is under the 16 GiB it needs (not a failure)"
    uhd_note="the piped 2160p leg was SKIPPED for RAM"
else
    ffmpeg -v error -f lavfi -i testsrc2=size=3840x2160:rate=30 -frames:v 12 \
        -pix_fmt yuv420p "$uhd" || { echo "pipe window check: 2160p clip generation failed"; exit 1; }
    perl -e 'alarm 120; exec @ARGV' env -u Y264_MAX_INPUT_MB "$Y" --input-y4m - \
        --threads 0 -o "$work/uhd.264" --quiet --no-progress < <(cat "$uhd") >/dev/null 2>&1
    rc=$?
    if [ $rc -ne 0 ] || [ ! -s "$work/uhd.264" ]; then
        echo "FAIL 2160p piped at --threads 0: exited $rc"; fail=1
    fi
fi
[ $fail -eq 0 ] && echo ">> pipe window check: threads 1/2/4 finish and decode to all $NF frames; small window overrides and $uhd_note"
exit $fail
