#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# pipe_window_check.sh - a piped Y4M longer than (threads + 1) x keyint must
# finish, and must code the same bitstream as the same file read by path.
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
command -v ffmpeg >/dev/null || { echo ">> pipe window check skipped (no ffmpeg)"; exit 0; }
[ -x "$Y" ] || { echo "pipe window check: no binary at $Y"; exit 1; }

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
clip=$work/clip.y4m
ffmpeg -v error -f lavfi -i testsrc2=size=192x144:rate=30 -frames:v 420 \
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
    elif ! cmp -s "$work/pipe.264" "$work/file.264"; then
        echo "FAIL threads $t: pipe and file bitstreams differ"; fail=1
    fi
    rm -f "$work/pipe.264" "$work/file.264"
done
[ $fail -eq 0 ] && echo ">> pipe window check: threads 1/2/4 finish and match the file read"
exit $fail
