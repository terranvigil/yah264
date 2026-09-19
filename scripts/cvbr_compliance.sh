#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# cvbr_compliance.sh -- the capped-VBR VBV compliance gate, as a script.
#
# 36 cells: six clips x three caps (0.8x / 0.6x / 0.4x each clip's ABR target)
# x both Y264_RC_PIPE_VBV paths. Plus 18 x264 reference cells at the same caps.
# A cell is clean when scripts/vbv_check.py reports zero underflows.
#
# WHY THIS IS A FILE AND NOT A COMMAND LINE. This gate produced the numbers in
# the local measurement records (5/36 at HEAD e933296, 34/36 with the fix,
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
# HRD=1 ADDS THE ANNEX C LEG (item B-hrd). The same 36 cells are re-encoded with
# --nal-hrd vbr and again with --nal-hrd cbr, and each one is read by
# scripts/hrd_check.py, which takes its bucket out of the STREAM rather than off
# this command line. The two checkers answer different questions and the row
# prints both: vbv_check asks "did our rate control respect the cap we gave it",
# hrd_check asks "would a receiver's buffer survive what we emitted".
#
# WHY THE ROW CARRIES THE VBV VERDICT BESIDE THE HRD ONE. The HRD declaration is
# the encoder's own buffer model written down, so a cell whose rate control
# already underflows its model emits a stream that underflows the declaration --
# the same defect read through a second instrument, not a new one. Reporting a
# raw "N of 36 HRD-clean" would therefore score the rate control and call it a
# signalling result. The leg is read as: every cell VBV-clean on the BASE build
# must be HRD-clean in both modes, and the cells the base build already fails
# are named with their HRD verdict and left alone. YAH264_BASE is that base
# build (default: the same binary, which is right only when nothing in the tree
# has moved the rate control -- point it at a main build when it has).
#
# Usage: scripts/cvbr_compliance.sh
#        HRD=1 YAH264_BASE=scratch/build-main/cli/yah264 scripts/cvbr_compliance.sh
# Env:   CLIPS CVBR_SECONDS FRACS THREADS PRESET YAH264 YAH264_BASE X264 HRD
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# NOT `SECONDS` -- bash reserves that for the shell's own elapsed time, so
# ${SECONDS:-6} would silently read however long this shell had been alive.
SECONDS_PER="${CVBR_SECONDS:-6}"
THREADS="${THREADS:-18}"
YAH264="${YAH264:-$root/build/cli/yah264}"
# The build whose VBV verdict PARTITIONS the HRD leg. Defaults to the binary
# under test, which is the right answer only when the change under test did not
# touch the rate control; when it might have, point this at a main build so the
# partition is a property of main and not of the arm being judged.
YAH264_BASE="${YAH264_BASE:-$YAH264}"
HRD="${HRD:-}"
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

# clip:abr_kbps:y264_crf:x264_crf -- the same calibration perf-comp-modes.sh
# uses, and it goes stale the same way. Each row prints its achieved rate, so a
# stale entry shows up as a rate far from the cap rather than as a quiet pass.
CLIPS="${CLIPS:-foreman_cif:400:21.4:22.7 bus_cif:400:28.6:27.9 stefan_cif:400:26.2:27.4 samsung_720p:1200:21.4:25.5 park_joy_720p:12000:26.2:25.0 ducks_720p:25000:22.6:20.8}"
FRACS="${FRACS:-0.8 0.6 0.4}"

[ -x "$YAH264" ] || { echo "cvbr_compliance: no yah264 at $YAH264" >&2; exit 2; }
. "$root/scripts/scratch.sh"
y264_scratch_dir cvbr wd

n_clean=0; n_total=0; x_clean=0; x_total=0
# h_*: every HRD arm. hp_*: only the arms whose cell is VBV-clean on the base
# build, which is the partition the leg is actually read on -- a cell whose rate
# control already underflows emits a stream that underflows the declaration too,
# and counting those against the signalling would score the wrong subsystem.
h_clean=0; h_total=0; hp_clean=0; hp_total=0
hp_bad=""; vbv_bad=""
if [ -n "$HRD" ]; then
    printf '%-16s %6s %5s %4s  %-9s %-4s %-4s %s\n' \
           clip cap pipe enc vbv vbr cbr 'hrd peak / first fail'
    printf '%-16s %6s %5s %4s  %-9s %-4s %-4s %s\n' \
           ---------------- ------ ----- ---- --------- ---- ---- ---------------------
else
    printf '%-16s %6s %5s %4s  %-9s %s\n' clip cap pipe enc verdict detail
    printf '%-16s %6s %5s %4s  %-9s %s\n' ---------------- ------ ----- ---- --------- ------
fi

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
            # DELETE BEFORE EVERY ITERATION, and read the encoder's exit code.
            # A loop that writes to a fixed filename and then checks it scores
            # the PREVIOUS iteration whenever the encoder writes nothing, which
            # is what an unknown flag does -- it exits 2 and prints help. That
            # turns an arm that never ran into a clean row.
            rm -f "$out"
            Y264_RC_PIPE_VBV="$pipe" "$YAH264_BASE" --input-y4m "$ref" \
                --preset "$PRESET" --cabac --transform-8x8 --crf "$ncrf" \
                --vbv-maxrate "$cap" --vbv-bufsize "$cap" \
                --threads "$THREADS" -o "$out" >/dev/null 2>&1
            enc=$?
            line=$(python3 "$root/scripts/vbv_check.py" "$out" --maxrate "$cap" \
                     --bufsize "$cap" --y4m "$ref" --quiet 2>/dev/null)
            rc=$?
            # An encoder that wrote nothing is not a clean cell.
            { [ "$enc" = 0 ] && [ -s "$out" ]; } || rc=99
            n_total=$((n_total+1)); [ "$rc" = 0 ] && n_clean=$((n_clean+1))
            det=$(printf '%s' "$line" | sed -n 's/.*header\], \([0-9.]*\) kbps.*min-fill [-0-9.]* kbit (\([-0-9.]*\)%).*, \([0-9]*\) underflow.*/\1 kbps  fill \2%  \3 under/p')
            vbv_verdict=$([ "$rc" = 0 ] && echo ok || echo UNDERFLOW)
            if [ -z "$HRD" ]; then
                printf '%-16s %6s %5s %4s  %-9s %s\n' "$clip" "$cap" "$pipe" y264 \
                       "$vbv_verdict" "$det"
                continue
            fi
            # The Annex C leg. Same cell, same cap, same pipe path; the only
            # difference is the declaration, and under vbr not even the slice
            # bits move. Each mode is its own encode because cbr pads and vbr
            # does not, so they are different streams by construction.
            hv=""; hc=""; hdet=""
            for mode in vbr cbr; do
                hout="$wd/h_$mode.264"
                rm -f "$hout"
                Y264_RC_PIPE_VBV="$pipe" "$YAH264" --input-y4m "$ref" \
                    --preset "$PRESET" --cabac --transform-8x8 --crf "$ncrf" \
                    --vbv-maxrate "$cap" --vbv-bufsize "$cap" --nal-hrd "$mode" \
                    --threads "$THREADS" -o "$hout" >/dev/null 2>&1
                henc=$?
                hline=$(python3 "$root/scripts/hrd_check.py" "$hout" 2>&1)
                hrc=$?
                { [ "$henc" = 0 ] && [ -s "$hout" ]; } || hrc=99
                # Exit 3 is "the stream carries no HRD", which in THIS leg is a
                # failure of the thing under test, not a vacuous pass.
                v=$([ "$hrc" = 0 ] && echo ok || { [ "$hrc" = 3 ] && echo NO-HRD || echo FAIL; })
                pk=$(printf '%s' "$hline" | sed -n 's/.*peak fullness [0-9]* bit (\([0-9.]*\)% of CpbSize).*/\1/p' | head -1)
                f1=$(printf '%s' "$hline" | sed -n 's/.*FAIL \([A-Z]*\) nal sched 0 au \([0-9]*\).*/\1@\2/p' | head -1)
                if [ "$mode" = vbr ]; then hv="$v"; else hc="$v"; fi
                hdet="$hdet $mode:${pk:-?}%${f1:+ $f1}"
                h_total=$((h_total+1)); [ "$hrc" = 0 ] && h_clean=$((h_clean+1))
                if [ "$rc" = 0 ]; then
                    hp_total=$((hp_total+1)); [ "$hrc" = 0 ] && hp_clean=$((hp_clean+1))
                    [ "$hrc" = 0 ] || hp_bad="$hp_bad $clip/$cap/p$pipe/$mode"
                fi
            done
            [ "$rc" = 0 ] || vbv_bad="$vbv_bad $clip/$cap/p$pipe($hv,$hc)"
            printf '%-16s %6s %5s %4s  %-9s %-4s %-4s %s\n' "$clip" "$cap" "$pipe" y264 \
                   "$vbv_verdict" "$hv" "$hc" "$hdet"
        done
        # The x264 reference bar is about the RATE CONTROL, which the HRD leg is
        # not judging, so HRD=1 skips it rather than paying 18 more encodes to
        # print a column nothing in that leg reads.
        if [ -x "$X264" ] && [ -z "$HRD" ]; then
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
echo "yah264 clean: $n_clean/$n_total    x264 clean: $x_clean/$x_total"
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
# declared occupancy to the next one instead (the local measurement records,
# "The joins"), and both windows now read 34/36 with the 720p cells clearing by
# 35-47% of buffer where they used to clear by 0.1-1.3%.
#
# The two that still fail -- samsung/480, both paths -- are a mid-stream scene
# cut, not a GOP join. Do not read a regression into that cell moving around:
# it is the same frame either way.
if [ -n "$HRD" ]; then
    echo
    echo "hrd clean (all arms):            $h_clean/$h_total"
    echo "hrd clean where the cell is VBV-clean on the base build: $hp_clean/$hp_total"
    [ -n "$vbv_bad" ] && echo "cells the BASE build already underflows (HRD verdict vbr,cbr):$vbv_bad"
    [ -n "$hp_bad" ] && echo "HRD FAILURES ON VBV-CLEAN CELLS -- these are the leg:$hp_bad"
    # The leg is the partition, not the raw count. A cell whose rate control
    # underflows its own buffer model emits a stream that underflows the
    # declaration of that model, so failing the whole run on it would report a
    # rate-control defect as a signalling one and hide the signalling result
    # inside it. Those cells are named above and left exactly as the encoder's
    # own model leaves them.
    [ "$hp_clean" = "$hp_total" ] || exit 1
    exit 0
fi
[ "$n_clean" = "$n_total" ] || exit 1
