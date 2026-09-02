#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# perf-comp-modes.sh -- the RATE-CONTROL MODE MATRIX: yah264 vs x264 across
# CRF, CQP, ABR, CBR, capped VBR and 2-pass, per clip, one table per mode.
#
# WHY THIS EXISTS. The scoreboard (parity-status.sh -> perf-comp-set.sh) has
# only ever run ABR. "Goal 2 is 1.82x" was therefore an ABR number that was not
# labelled one, and the modes are NOT small variations on each other here -- they
# take structurally different paths through the encoder:
#
#   - 2-pass threads as of docs/rc-mode-matrix.md's follow-up: it used to be
#     SERIAL regardless of --threads, because cli/yah264_cli.c routed only
#     `rc.method != 3` to encode_threaded(). That exclusion was load-bearing --
#     the stats file matches records to frames by position with no frame index,
#     so N per-GOP encoders would truncate each other in pass 1 and all replay
#     GOP 0's records in pass 2. It is lifted by splitting the stats file along
#     the GOP boundaries; Y264_2PASS_MT=0 restores the serial path.
#   - VBV (so CBR and capped VBR) drains every in-flight frame at each anchor
#     (encoder.c:9808) and is excluded from the stair width feature by
#     construction (stair_wide_capable :428, stair_wide_rc_ok :455).
#   - CQP and pure CRF are the only modes that set no RC serialisation at all
#     (neither sets rcp_on).
#
# So a single number cannot stand for all six, and this script deliberately
# prints NO cross-mode mean. Report per mode, per clip, or say which you mean.
#
# WHAT MAKES A CELL COMPARABLE. Two traps, both already paid for in this repo:
#
#   1. Equal CRF is not a matched operating point. The two encoders' rate-factor
#      scales are not the same scale (measured on this tree at equal CRF:
#      foreman -10%, bus +9%, park_joy +44%, sintel -55% in size). So the CRF and
#      capped-VBR rows below run each encoder at its OWN calibrated CRF, chosen
#      per clip to land on the same achieved bitrate. The rate factors differ by
#      up to 4.1 points (samsung: yah264 21.4 vs x264 25.5) -- that spread IS
#      the scale difference, made explicit instead of averaged over.
#   2. CQP is not matched work. x264's parameter validation forces mb-tree
#      and aq off whenever the RC method is CQP (the
#      X264_RC_CQP block ~:951-967); yah264 runs mb-tree at EVERY mode
#      (encoder.c:2293 has no rc.method term). The CQP rows here therefore set
#      Y264_MBTREE_OFF=1, which is exactly that policy applied to yah264. AQ
#      needs no action -- cli/yah264_cli.c:885 already zeroes aq_strength when
#      rc.method == 0. Rows are labelled "mbtree-off" so this is never silent.
#
# CALIBRATION. The per-clip CRF/QP constants below were MEASURED by bisection
# against each clip's ABR target on this tree (2026-08-11, 6s windows, medium
# preset). They are a property of the two encoders and go stale when either
# changes -- which is why every row still prints its own achieved rate and the
# harness flags any cell whose two encodes differ by more than 5% in size. A
# stale calibration announces itself as an UNMATCHED cell rather than quietly
# biasing a speed number. Regenerate with the bisection described in
# docs/rc-mode-matrix.md.
#
# Usage: scripts/perf-comp-modes.sh [pure|asm]
# Env: MODES SET_THREADS SET_SECONDS CLIPS RUNS
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-pure}"
SECONDS_PER="${SET_SECONDS:-6}"
PRESET="${SET_PRESET:-medium}"
THREADS="${SET_THREADS:-1}"
RUNS="${RUNS:-3}"
MODES="${MODES:-crf cqp abr cbr cvbr 2pass}"

# clip:abr_kbps:y264_crf:x264_crf:y264_qp:x264_qp
#
# The ABR targets are NOT the old CIF-2500 / 720p-12000 pair. Those put the CIF
# clips at VMAF 99.5-99.9, where the metric has no headroom and a dVMAF column is
# noise around zero (foreman read 99.55 vs 99.61, bus 99.85 vs 99.86), and they
# put ducks at VMAF 75. These targets were chosen from a measured rate/VMAF
# ladder so every clip lands in VMAF ~88-94, where the metric still
# discriminates and where both encoders track their target. See
# docs/rc-mode-matrix.md for the ladder and the selection rule.
CLIPS="${CLIPS:-foreman_cif:400:21.4:22.7:27:27 bus_cif:400:28.6:27.9:34:35 stefan_cif:400:26.2:27.4:33:34 samsung_720p:1200:21.4:25.5:27:28 park_joy_720p:12000:26.2:25.0:33:33 ducks_720p:25000:22.6:20.8:29:30}"

script="$root/scripts/perf-comp.sh"
[ "$MODE" = pure ] && export PURE_C=1

for m in $MODES; do
    echo
    case "$m" in
        crf)   echo "== CRF (constant quality; each encoder at its own calibrated rate factor) ==" ;;
        cqp)   echo "== CQP (fixed QP; yah264 Y264_MBTREE_OFF=1 to match x264's CQP policy) ==" ;;
        abr)   echo "== ABR/VBR (average bitrate target) ==" ;;
        cbr)   echo "== CBR (bitrate == VBV maxrate, 1s buffer) ==" ;;
        cvbr)  echo "== capped VBR (calibrated CRF + a BINDING VBV cap at 0.8x the ABR target) ==" ;;
        2pass) echo "== 2-pass ABR (wall = pass 1 + pass 2; both passes threaded, Y264_2PASS_MT=0 for serial) ==" ;;
    esac
    printf '%-16s %8s %9s %9s %8s %7s  %s\n' clip "x264 x" "y264 rate" "x264 rate" dVMAF dsize notes
    printf '%-16s %8s %9s %9s %8s %7s  %s\n' ---------------- -------- --------- --------- -------- ------- -----
    for entry in $CLIPS; do
        IFS=: read -r clip br ncrf xcrf nqp xqp <<<"$entry"
        path="$root/tests/corpus/$clip.y4m"
        [ -f "$path" ] || { printf '%-16s %8s\n' "$clip" "(missing)"; continue; }
        # The capped-VBR cap is set BELOW the calibrated CRF rate on purpose, so
        # it binds. A realistic delivery cap (1.2x the average) was measured on
        # this corpus and never engages: foreman at CRF 21.4 runs 416 kbit/s and
        # caps of 480 and 420 both returned a BYTE-IDENTICAL stream, on both
        # encoders -- a 1-second buffer absorbs everything a 6-second
        # single-scene clip throws at it. A row like that exercises none of the
        # VBV code path and would silently duplicate the CRF row. 0.8x forces the
        # limiter to work, which is the point of measuring the mode at all.
        cap=$(python3 -c "print(int($br*0.8))")
        mbt=0
        case "$m" in
            crf)   NA="--preset $PRESET --cabac --transform-8x8"; XA="--preset $PRESET"
                   export Y264_CRF="$ncrf" X264_CRF="$xcrf" ;;
            cqp)   NA="--preset $PRESET --cabac --transform-8x8 --qp $nqp"; XA="--preset $PRESET --qp $xqp"
                   mbt=1; unset Y264_CRF X264_CRF ;;
            abr)   NA="--preset $PRESET --cabac --transform-8x8 --bitrate $br"; XA="--preset $PRESET --bitrate $br"
                   unset Y264_CRF X264_CRF ;;
            cbr)   NA="--preset $PRESET --cabac --transform-8x8 --bitrate $br --vbv-maxrate $br --vbv-bufsize $br"
                   XA="--preset $PRESET --bitrate $br --vbv-maxrate $br --vbv-bufsize $br"
                   unset Y264_CRF X264_CRF ;;
            cvbr)  NA="--preset $PRESET --cabac --transform-8x8 --vbv-maxrate $cap --vbv-bufsize $cap"
                   XA="--preset $PRESET --vbv-maxrate $cap --vbv-bufsize $cap"
                   export Y264_CRF="$ncrf" X264_CRF="$xcrf" ;;
            2pass) NA="--preset $PRESET --cabac --transform-8x8 --bitrate $br"; XA="--preset $PRESET --bitrate $br"
                   unset Y264_CRF X264_CRF ;;
        esac
        # perf-comp.sh's positionals are <clip> [crf] [seconds]. THE SECOND ONE
        # IS A RATE FACTOR, NOT A FRAME RATE. It used to be a literal `30` here,
        # which read like 30fps to three separate readers and got written up in
        # docs/archive/capped-vbr-cap-overshoot.md as the cause of an fps defect it had
        # nothing to do with. perf-comp.sh has never taken a framerate from a
        # caller: it ffprobes the clip (:114) and hands vbv_check.py the source
        # y4m itself. Pass this clip's own calibrated rate factor so the slot
        # holds a value that is correct rather than merely inert -- Y264_CRF /
        # X264_CRF override it on the crf and cvbr rows, and no other row reads
        # it at all.
        out=$(RC_MODE="$m" RUNS="$RUNS" THREADS="$THREADS" \
              YAH264="${YAH264:-$root/build/cli/yah264}" \
              Y264_MBTREE_OFF="$mbt" \
              YAH264_ARGS="$NA" X264_ARGS="$XA" \
              bash "$script" "$path" "$ncrf" "$SECONDS_PER" 2>&1)
        line=$(printf '%s\n' "$out" | grep 'speed: x264 is' || true)
        if [ -z "$line" ]; then
            printf '%-16s %8s\n' "$clip" "FAILED"
            printf '%s\n' "$out" | tail -3 >&2
            continue
        fi
        x=$(printf '%s\n' "$line" | sed -n 's/.*x264 is \([0-9.]*\)x faster.*/\1/p')
        q=$(printf '%s\n' "$line" | sed -n 's/.*quality: yah264 \([-+0-9.]*\) VMAF.*/\1/p')
        s=$(printf '%s\n' "$line" | sed -n 's/.*size: yah264 \([-+0-9%]*\).*/\1/p')
        rl=$(printf '%s\n' "$out" | grep 'rate accuracy' || true)
        nr=$(printf '%s\n' "$rl" | sed -n 's/.*yah264 \([-+0-9.]*\)%.*/\1%/p')
        xr=$(printf '%s\n' "$rl" | sed -n 's/.*x264 \([-+0-9.]*\)%.*/\1%/p')
        # Achieved rates for the modes that have no target to score against.
        if [ -z "$nr" ]; then
            kl=$(printf '%s\n' "$out" | grep 'KiB (' || true)
            nr=$(printf '%s\n' "$kl" | sed -n 's/.*yah264 [0-9]* KiB (\([0-9]*\) kbps).*/\1k/p')
            xr=$(printf '%s\n' "$kl" | sed -n 's/.*x264 [0-9]* KiB (\([0-9]*\) kbps).*/\1k/p')
        fi
        notes=""
        [ "$m" = cqp ] && notes="mbtree-off"
        [ "$m" = 2pass ] && [ "${Y264_2PASS_MT:-1}" = 0 ] && notes="y264 serial"
        vl=$(printf '%s\n' "$out" | grep 'VBV:' || true)
        [ -n "$vl" ] && notes="$notes${notes:+ }$(printf '%s' "$vl" | sed 's/>> //')"
        printf '%s\n' "$out" | grep -q '\[!\] sizes differ' && notes="$notes${notes:+ }UNMATCHED"
        printf '%s\n' "$out" | grep -q 'wall spread' && notes="$notes${notes:+ }NOISY"
        printf '%-16s %8s %9s %9s %8s %7s  %s\n' "$clip" "${x}x" "${nr:--}" "${xr:--}" "$q" "$s" "$notes"
    done
done
unset Y264_CRF X264_CRF
echo
echo "(${MODE}, ${THREADS} thread(s), ${SECONDS_PER}s windows, median of ${RUNS} runs)"
echo "NO cross-mode mean is printed on purpose: the modes take structurally"
echo "different paths through the encoder -- see this script's header."
