#!/usr/bin/env bash
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# parity-status.sh -- one scoreboard for the three owner speed-parity goals
# (docs/pure-c-speed-parity.md header; memory: speed-parity-goals):
#   GOAL 1  pure-C, single-threaded   (next264 no-asm vs x264-noasm-autovec)
#   GOAL 2  pure-C, multi-threaded    (same binaries, all online CPUs)
#   GOAL 3  as-shipped SIMD, multi-threaded (next264 SIMD vs x264-asm)
# Each line reports the x264-speed multiple over the 6-clip set (1.00x = parity;
# >1 means x264 is faster). Quality/size columns come from the per-tier tables.
#
# RATE CONTROL. Two scoreboards, selected by PARITY_RC:
#
#   PARITY_RC=abr (default) -- the fast fixed-rate run. Both encoders get the
#       same --bitrate; matched by construction, no calibration, quickest read.
#       Kept as the default precisely so the quick iteration loop stays quick.
#   PARITY_RC=crf           -- CRF at a MATCHED OPERATING POINT. CRF is what
#       people actually run for VOD and file encoding, so this is the number to
#       headline; ABR's virtue is measurement convenience, not user relevance.
#       It costs a per-clip calibration sweep on top (scripts/crf-solve.py
#       solves each encoder onto a common achieved bitrate), because next264's
#       CRF N and x264's CRF N are NOT the same operating point -- 41 points of
#       size spread even after the complexity term is enabled.
#
# The two are NOT the same measurement and their numbers are not comparable. The
# opt-in is deliberate and the cost is stated rather than silently absorbed:
# after the first run the solve is cached, so the marginal cost of the CRF board
# is small, but the first run pays for the sweep.
#
# Usage: scripts/parity-status.sh [quick]
#   quick = 2 clips (foreman_cif + park_joy_720p), ~3x faster, noisier.
# Env: SET_SECONDS/SET_CRF/CLIPS pass through; NPROC overrides the MT thread
#      count; PARITY_RC=abr|crf; POINT=kbps|vmaf (crf board only).
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# MT THREAD COUNT: 12, not "all cores" (2026-08-17, owner).
#
# Two reasons, and the second one is a result rather than hygiene.
#
# 1. At 18-of-18 any stray desktop process steals a core directly from the
#    encode, and our pool contends differently than x264's threading does, so
#    the ratio is not invariant to it. A whole session was spent unable to
#    separate real movement from a busy box. 12 leaves headroom.
# 2. GOAL 2 IS IDENTICAL AT 12 AND 18 -- median 1.00x, max 1.08x, clip for clip
#    (foreman 1.00/1.00, bus 1.08/1.08, stefan 1.00/1.00). So nothing is being
#    flattered on the leg that matters most, and the agreement is itself
#    evidence both runs are stable. GOAL 3 IMPROVES: median 1.22x -> 1.17x, max
#    1.38x -> 1.25x, on five of six clips. That gap is the finding: our
#    as-shipped tier loses ~4% going 12 -> 18 where our pure-C tier loses
#    nothing, so part of what goal 3a books as COVERAGE is width-scaling.
#
# NPROC=18 reproduces the old board.
NPROC="${NPROC:-12}"
PARITY_RC="${PARITY_RC:-abr}"
POINT="${POINT:-kbps}"
case "$PARITY_RC" in
    abr) setscript="$root/scripts/perf-comp-set.sh" ;;
    crf) setscript="$root/scripts/perf-comp-crf-set.sh" ;;
    *)   echo "parity-status: PARITY_RC must be abr or crf (got '$PARITY_RC')" >&2; exit 2 ;;
esac
if [ "${1:-}" = quick ]; then
    export CLIPS="foreman_cif:400 park_joy_720p:12000"
fi

# The x264 baseline cache is keyed on the binary, the clip and the args -- but
# NOT on the box's load state, so a baseline measured while the box was busy is
# reused forever against arms measured when it was idle. That has silently
# poisoned MT readings before. A board run re-measures both sides; set
# N264_REFENC_CACHE=1 explicitly if you are iterating and know what you are doing.
export N264_REFENC_CACHE="${N264_REFENC_CACHE:-0}"

run_tier() {  # $1=mode(pure|asm) $2=threads $3=label; table -> stderr, "median max dvmaf dsize" -> stdout
    { echo; echo "== $3 =="; } >&2
    # Medians, and enough samples for a median to mean anything. This used to be
    # runs=1 for the 1-thread tier ("low-noise, keep it single-run for speed") and
    # best-of-3 for MT. One sample has no dispersion at all, which is why goal 1
    # wobbled 1.00/0.95/1.02 across three readings of the same tree on one day,
    # and min-of-3 is the statistic docs/archive/bf3-scaling-diagnosis.md session 3
    # measured as unreliable (7.6% spread between identical arms). Five runs is
    # the cheapest count where a median is stable on this box; the 1-thread tier
    # keeps 3 because it really is quieter, but not 1.
    runs=3; [ "$2" -gt 1 ] && runs=5
    out=$(RUNS="$runs" SET_THREADS="$2" POINT="$POINT" bash "$setscript" "$1")
    printf '%s\n' "$out" >&2
    printf '%s %s %s %s\n' \
        "$(printf '%s\n' "$out" | sed -n 's/^MEDIAN[[:space:]]*\([0-9.]*x\).*/\1/p')" \
        "$(printf '%s\n' "$out" | sed -n 's/^MAX[[:space:]]*\([0-9.]*x\).*/\1/p')" \
        "$(printf '%s\n' "$out" | sed -n 's/^dVMAF[[:space:]]*\([-+0-9.]*\).*/\1/p')" \
        "$(printf '%s\n' "$out" | sed -n 's/^dSIZE[[:space:]]*\([-+0-9.]*\).*/\1/p')"
}

m1=$(run_tier pure 1        "GOAL 1: pure-C, 1 thread")
m2=$(run_tier pure "$NPROC" "GOAL 2: pure-C, $NPROC threads")
m3=$(run_tier asm  "$NPROC" "GOAL 3: as-shipped SIMD, $NPROC threads")

echo
echo "=================================================================="
echo " next264 speed-parity goals  (x264-speed multiple; 1.00x = parity)"
if [ "$PARITY_RC" = crf ]; then
echo " RATE CONTROL: CRF at a MATCHED OPERATING POINT (common achieved"
echo " ${POINT}).  Each encoder is swept over CRF and solved onto the same"
echo " point, so the two sides spend comparable bits by construction and"
echo " perf-comp.sh's 5% size guard stays armed and silent."
else
echo " RATE CONTROL: ABR only.  These are ABR numbers, not encoder numbers."
fi
echo "=================================================================="
printf ' %-28s %8s %8s %8s %8s\n' "goal" "median" "max" "dVMAF" "dSIZE"
goalrow() { lbl="$1"; set -- $2; printf ' %-28s %8s %8s %8s %8s\n' "$lbl" "${1:-n/a}" "${2:-n/a}" "${3:-n/a}" "${4:-n/a}"; }
goalrow "GOAL 1  pure-C    1 thread" "$m1"
goalrow "GOAL 2  pure-C   $NPROC threads" "$m2"
goalrow "GOAL 3  SIMD     $NPROC threads" "$m3"
echo "------------------------------------------------------------------"
echo " MEDIAN is the headline; MAX names the clip that has to be stated."
echo " Bar (docs/goal-legs.md): median <= 1.00x, no clip above 1.15x,"
echo " dVMAF within 0.5, dSIZE within 1.0% -- all at the matched point."
echo " dSIZE is the FOURTH LEG since 2026-08-18 (owner): bits vs x264, so an"
echo " arm can no longer buy speed with bits unnoticed. It is bounded by the"
echo " solve tolerance and is NOT a BD-rate -- the authoritative efficiency"
echo " number is scripts/run_band.py on the CRF band, where the board's six"
echo " clips read +3.54% and the full corpus reads -0.85%."
echo "=================================================================="
if [ "$PARITY_RC" = crf ]; then
echo " NOT COMPARABLE TO THE ABR SERIES.  Different rate-control mode and a"
echo " different operating point per clip; treat this as a new series, the"
echo " same way the 2026-08-11 medians and preset changes started one. It is"
echo " not 'the ABR number, improved' in either direction."
echo
echo " next264's CRF is a STAIRCASE: rc_set_qp_crf rounds the rate factor to"
echo " an integer frame QP, so only a discrete ladder of rates is reachable"
echo " (~13% apart).  The common point is therefore chosen from the rungs"
echo " next264 can hit and x264 -- whose CRF is continuous -- is solved onto"
echo " it.  The 'drift' column is how far that rung sits from the clip's"
echo " nominal target; it moves both encoders together and is not a defect."
else
echo " ABR is one of six rate-control modes and the others do NOT track it."
echo " 2-pass used to ignore --threads entirely and read 11-20x; it threads now"
echo " (docs/archive/two-pass-threading.md) and reads 1.8-3.1x, still off this figure."
echo " CRF -- the mode most users actually run -- has its own scoreboard:"
echo " 'make parity-status-crf'.  Its numbers are NOT comparable to these."
fi
echo " Run 'make parity-modes' for the per-mode matrix before quoting any of"
echo " the above as \"the\" gap.  docs/rc-mode-matrix.md"
echo "=================================================================="
