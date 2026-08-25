#!/usr/bin/env bash
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# cvbr_compliance.sh -- the capped-VBR VBV compliance gate, as a script.
#
# 36 cells: six clips x three caps (0.8x / 0.6x / 0.4x each clip's ABR target)
# x both N264_RC_PIPE_VBV paths. Plus 18 x264 reference cells at the same caps.
# A cell is clean when scripts/vbv_check.py reports zero underflows.
#
# WHY THIS IS A FILE AND NOT A COMMAND LINE. This gate produced the numbers in
# docs/archive/capped-vbr-cap-overshoot.md (5/36 at HEAD e933296, 34/36 with the fix,
# 18/18 for x264), and it was improvised at a shell prompt both times. The first
# improvisation passed ONE --fps for all six clips, and since the corpus mixes
# 29.97 / 30 / 50 fps that checked the two 50fps clips against a bucket 1.67x
# too forgiving and reported their bitrate 1.67x low. It read 12/36 clean when
# the truth was 5/36 -- the mode was WORSE than the write-up first said, and the
# error flattered it. An uncommitted gate gets re-derived by hand every time
# somebody re-opens the question, and a hand-derived framerate is exactly the
# parameter that keeps coming out wrong.
#
# So: fps is never typed here. vbv_check.py --y4m reads it from the clip's own
# Y4M header, and prints the rate it resolved on every verdict line.
#
# Usage: scripts/cvbr_compliance.sh
# Env:   CLIPS CVBR_SECONDS FRACS THREADS PRESET NEXT264 X264
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# NOT `SECONDS` -- bash reserves that for the shell's own elapsed time, so
# ${SECONDS:-6} would silently read however long this shell had been alive.
SECONDS_PER="${CVBR_SECONDS:-6}"
THREADS="${THREADS:-18}"
NEXT264="${NEXT264:-$root/build/cli/next264}"
PRESET="${PRESET:-medium}"
# The reference bar is x264's, so a run without x264 is not a weaker run of this
# gate -- it is a different gate, one with no bar in it. $root/../x264 resolves
# only from a normal checkout; from a git worktree under .claude/worktrees it
# points at nothing and every reference cell silently became 0/0. This file
# exists because a silently-wrong parameter produced a wrong headline once
# already, so: try the likely places, and if none of them has it, say so loudly
# rather than printing a table that looks complete.
if [ -z "${X264:-}" ]; then
    for cand in "$root/../x264/x264-asm" "$HOME/src/x264/x264-asm" \
                "$root/../../x264/x264-asm" "$(command -v x264 || true)"; do
        [ -n "$cand" ] && [ -x "$cand" ] && { X264="$cand"; break; }
    done
fi
X264="${X264:-}"
[ -x "$X264" ] || echo "cvbr_compliance: NO x264 REFERENCE (set X264=/path/to/x264) -- the x264 column will read 0/0 and this run has no bar to clear" >&2

# clip:abr_kbps:n264_crf:x264_crf -- the same calibration perf-comp-modes.sh
# uses, and it goes stale the same way. Each row prints its achieved rate, so a
# stale entry shows up as a rate far from the cap rather than as a quiet pass.
CLIPS="${CLIPS:-foreman_cif:400:21.4:22.7 bus_cif:400:28.6:27.9 stefan_cif:400:26.2:27.4 samsung_720p:1200:21.4:25.5 park_joy_720p:12000:26.2:25.0 ducks_720p:25000:22.6:20.8}"
FRACS="${FRACS:-0.8 0.6 0.4}"

[ -x "$NEXT264" ] || { echo "cvbr_compliance: no next264 at $NEXT264" >&2; exit 2; }
wd="$(mktemp -d)"; trap 'rm -rf "$wd"' EXIT

n_clean=0; n_total=0; x_clean=0; x_total=0
printf '%-16s %6s %5s %4s  %-9s %s\n' clip cap pipe enc verdict detail
printf '%-16s %6s %5s %4s  %-9s %s\n' ---------------- ------ ----- ---- --------- ------

for entry in $CLIPS; do
    IFS=: read -r clip br ncrf xcrf <<<"$entry"
    src="$root/tests/corpus/$clip.y4m"
    [ -f "$src" ] || { printf '%-16s %s\n' "$clip" "(missing)"; continue; }
    # Trim to a fixed window so every cell codes the same frames. The frame
    # count comes from the clip's OWN rate -- 6 seconds is 180 frames at 30 and
    # 300 at 50, and using one number for both is the same bug in another coat.
    ref="$wd/$clip.y4m"
    if [ ! -f "$ref" ]; then
        if [ -n "${CVBR_FRAMES:-}" ]; then
            # A FIXED frame count across clips of different rates means the
            # clips get different DURATIONS, and duration is what decides how
            # many keyints the window crosses. That is not a neutral choice --
            # see the keyint note below -- so it is opt-in and labelled.
            nf="$CVBR_FRAMES"
        else
            fps=$(python3 -c "
import sys; sys.path.insert(0, '$root/scripts')
from vbv_check import fps_from_y4m; print(fps_from_y4m('$src'))")
            nf=$(python3 -c "print(int(round($fps * $SECONDS_PER)))")
        fi
        ffmpeg -v error -y -i "$src" -frames:v "$nf" -pix_fmt yuv420p "$ref" || continue
    fi
    for frac in $FRACS; do
        cap=$(python3 -c "print(int($br*$frac))")
        for pipe in 1 0; do
            out="$wd/n.264"
            N264_RC_PIPE_VBV="$pipe" "$NEXT264" --input-y4m "$ref" \
                --preset "$PRESET" --cabac --transform-8x8 --crf "$ncrf" \
                --vbv-maxrate "$cap" --vbv-bufsize "$cap" \
                --threads "$THREADS" -o "$out" >/dev/null 2>&1
            line=$(python3 "$root/scripts/vbv_check.py" "$out" --maxrate "$cap" \
                     --bufsize "$cap" --y4m "$ref" --quiet 2>/dev/null)
            rc=$?
            n_total=$((n_total+1)); [ "$rc" = 0 ] && n_clean=$((n_clean+1))
            det=$(printf '%s' "$line" | sed -n 's/.*header\], \([0-9.]*\) kbps.*min-fill [-0-9.]* kbit (\([-0-9.]*\)%).*, \([0-9]*\) underflow.*/\1 kbps  fill \2%  \3 under/p')
            printf '%-16s %6s %5s %4s  %-9s %s\n' "$clip" "$cap" "$pipe" n264 \
                   "$([ "$rc" = 0 ] && echo ok || echo UNDERFLOW)" "$det"
        done
        if [ -x "$X264" ]; then
            "$X264" --preset "$PRESET" --crf "$xcrf" --vbv-maxrate "$cap" \
                --vbv-bufsize "$cap" --threads "$THREADS" -o "$wd/x.264" "$ref" >/dev/null 2>&1
            line=$(python3 "$root/scripts/vbv_check.py" "$wd/x.264" --maxrate "$cap" \
                     --bufsize "$cap" --y4m "$ref" --quiet 2>/dev/null)
            rc=$?
            x_total=$((x_total+1)); [ "$rc" = 0 ] && x_clean=$((x_clean+1))
            det=$(printf '%s' "$line" | sed -n 's/.*header\], \([0-9.]*\) kbps.*min-fill [-0-9.]* kbit (\([-0-9.]*\)%).*, \([0-9]*\) underflow.*/\1 kbps  fill \2%  \3 under/p')
            printf '%-16s %6s %5s %4s  %-9s %s\n' "$clip" "$cap" "-" x264 \
                   "$([ "$rc" = 0 ] && echo ok || echo UNDERFLOW)" "$det"
        fi
    done
done

echo
echo "next264 clean: $n_clean/$n_total    x264 clean: $x_clean/$x_total"
if [ -n "${CVBR_FRAMES:-}" ]; then
    win="fixed ${CVBR_FRAMES}-frame windows (durations DIFFER across clips)"
else
    win="${SECONDS_PER}s windows (frame count per clip's own rate)"
fi
echo "(${win}, ${THREADS} threads, preset ${PRESET}; fps per clip from each Y4M header)"
# THE WINDOW IS PART OF THE RESULT, so it is printed with it. At CVBR_FRAMES=180
# every clip stays inside one GOP at the default keyint 250; at 6-second windows
# the 50fps clips run 300 frames and cross a keyint. That difference is the
# whole reason this script defaults to real seconds: a gate that never crosses a
# keyint cannot see anything about how GOPs join.
#
# It saw plenty. On HEAD 9b81c38 the two windows read 34/36 and 29/36, and every
# one of the five extra failures was a second-GOP failure, because each
# gop_worker opened an encoder that primed vbv_fill full. GOPs now hand a
# declared occupancy to the next one instead (docs/archive/capped-vbr-cap-overshoot.md,
# "The joins"), and both windows now read 34/36 with the 720p cells clearing by
# 35-47% of buffer where they used to clear by 0.1-1.3%.
#
# The two that still fail -- samsung/480, both paths -- are a mid-stream scene
# cut, not a GOP join. Do not read a regression into that cell moving around:
# it is the same frame either way.
[ "$n_clean" = "$n_total" ] || exit 1
