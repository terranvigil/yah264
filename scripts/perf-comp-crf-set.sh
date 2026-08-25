#!/usr/bin/env bash
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# perf-comp-crf-set.sh -- the speed comparison over the clip set in CRF, taken
# at a MATCHED OPERATING POINT rather than at a matched CRF number.
#
# WHY CRF AT ALL. CRF is what people actually run for VOD and file encoding, so
# it is the mode the headline should quote. ABR's one virtue as a benchmark is
# that it is rate-matched by construction -- a measurement convenience, not a
# reason to headline a mode most users do not choose.
#
# WHY NOT "CRF 25 vs CRF 25". Because those are two different operating points.
# Equal-CRF size divergence vs x264 measures -54.6%..+45.3% on this tree, and
# -29.7%..+11.4% even with N264_CRF_CPLX=1 -- 41 points of spread
# (docs/archive/crf-x264-scale.md). A speed ratio taken there is a content-luck number:
# it times two encoders doing different amounts of work and reports the
# difference as if it were the encoders.
#
# WHAT THIS DOES INSTEAD. Per clip, scripts/crf-solve.py sweeps CRF on each
# encoder and interpolates to a common ACHIEVED BITRATE; perf-comp.sh then times
# both AT that point. No scale alignment is needed or attempted -- the operating
# point is held fixed by measurement, not by assuming two integers agree.
#
# This is MORE rigorous than the ABR scoreboard, not less. ABR matches the rate
# but leaves quality wherever each encoder happens to land, and the corpus had
# to be hand-recalibrated to put both encoders in a band where the metric
# discriminates at all. Here the rate is matched AND the residual quality
# difference is reported as a legitimate dVMAF.
#
# WHY A BITRATE AND NOT A VMAF. Both are implemented -- POINT=vmaf runs the
# other one -- and on paper matched-VMAF is the more honest question ("same
# output quality, who is faster"). The bitrate is the headline anyway, and the
# reason is measured rather than argued:
#
#   MATCHED-VMAF DOES NOT CURRENTLY PRODUCE A COMPARABLE PAIR. Run at matched
#   VMAF on this tree, next264 lands +22.2% (foreman_cif) and +24.4% (bus_cif)
#   in SIZE against x264, and perf-comp.sh's 5% guard fires on both -- correctly.
#   Two encodes 24% apart in bits are not doing comparable work, so a speed
#   ratio taken there measures the bit gap as much as the encoders.
#
#   That is not the BD-rate story and must not be read as one. next264 is
#   BD-rate AHEAD of x264 medium (-2.76%). The +24% is specific to CRF mode's
#   bit ALLOCATION: at a matched rate next264 scores 1.3-3.5 VMAF below x264
#   here, so buying back those points costs it a quarter more bits. An earlier
#   draft of this comment guessed "~3% apart, next264 is BD-ahead" from the BD
#   number instead of measuring. It was off by 8x, and this file is exactly
#   where a plausible unmeasured constant does its damage.
#
# So POINT=vmaf is kept as a DIAGNOSTIC -- it is how the allocation gap above
# was sized -- and the headline is the bitrate, for three further reasons:
#   1. It satisfies perf-comp.sh's 5%-size guard BY CONSTRUCTION (measured
#      match: -1.7%..+0.6% across the set). That guard exists because a CRF run
#      with no bitrate target was once misread as an ABR undershoot, and the
#      right way past it is to genuinely match the bytes, not to special-case
#      it. The guard stays armed and silent rather than suppressed.
#   2. Bitrate is EXACT -- a byte count. VMAF is an estimate with its own noise
#      (a subsampled one was worth 2.5 points; see perf-comp.sh's VMAF_FPS),
#      and matching on it injects that noise into the operating point itself,
#      on top of the timing noise already being fought.
#   3. The corpus is already calibrated as per-clip bitrates, so the two
#      scoreboards sit at the same operating points and their rows can be read
#      against each other.
#
# Usage: scripts/perf-comp-crf-set.sh [pure|asm]
# Env: SET_THREADS, SET_SECONDS, SET_PRESET, RUNS, CLIPS, POINT=kbps|vmaf,
#      NEXT264, X264_C, X264_ASM, VMAF
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-pure}"
SECONDS_PER="${SET_SECONDS:-6}"
PRESET="${SET_PRESET:-medium}"
POINT="${POINT:-kbps}"
THREADS="${SET_THREADS:-1}"

. "$root/scripts/parity-clips.sh"

# The arg strings are built ONCE and handed to both the solver and the measuring
# run. Solving under different flags than the measurement lands on a rate the
# measurement does not reproduce, and neither run would look wrong on its own.
N_ARGS="--preset $PRESET --cabac --transform-8x8"
X_ARGS="--preset $PRESET"

printf '%-18s %9s %9s %8s %8s %8s\n' clip "x264 x" "dVMAF" "dsize" "kbit/s" "drift"
printf '%-18s %9s %9s %8s %8s %8s\n' ------------------ --------- --------- -------- -------- --------
xs=""; qs=""; ss=""; n=0; warn=""
for entry in $CLIPS; do
    clip="${entry%%:*}"; br="${entry##*:}"
    path="$root/tests/corpus/$clip.y4m"
    [ -f "$path" ] || { printf '%-18s %9s\n' "$clip" "(missing)"; continue; }

    # --- solve: where is the common operating point on each encoder's scale? ---
    # Solved at the tier's THREAD COUNT because x264's output is not
    # thread-invariant (measured: 0.125% of size between 1 and 18 threads at CRF
    # 25). next264's is invariant on every axis tested, so it costs nothing to
    # be strict here. The solve is cached; it is a property of the binaries and
    # the clip, not of the box's load, so unlike a timing cache it cannot go
    # stale in a way that poisons a number.
    if [ "$POINT" = vmaf ]; then
        tgt_flag="--target-vmaf $(python3 - "$root" "$clip" <<'PY'
import sys
# VMAF operating points measured at the calibrated ABR rates (parity-clips.sh).
band = {"foreman_cif": 93.1, "bus_cif": 94.3, "stefan_cif": 92.2,
        "samsung_720p": 88.3, "park_joy_720p": 90.8, "ducks_720p": 90.1}
print(band.get(sys.argv[2], 91.0))
PY
)"
    else
        tgt_flag="--target-kbps $br"
    fi
    sol=$(NEXT264="${NEXT264:-$root/build/cli/next264}" \
          python3 "$root/scripts/crf-solve.py" --clip "$path" --seconds "$SECONDS_PER" \
              $tgt_flag --threads "$THREADS" --preset "$PRESET" \
              --n264-args "$N_ARGS" --x264-args "$X_ARGS" 2>&1)
    if ! printf '%s\n' "$sol" | grep -q '^solved=1'; then
        printf '%-18s %9s   %s\n' "$clip" "NO-POINT" \
            "$(printf '%s\n' "$sol" | grep -E '^(solve_warn|match_pct|.*saturated)=' | tr '\n' ' ')"
        printf '%s\n' "$sol" | tail -3 >&2
        warn="$warn $clip:unsolved"
        continue
    fi
    ncrf=$(printf '%s\n' "$sol" | sed -n 's/^n264_crf=//p')
    xcrf=$(printf '%s\n' "$sol" | sed -n 's/^x264_crf=//p')
    point=$(printf '%s\n' "$sol" | sed -n 's/^common_point=//p')
    drift=$(printf '%s\n' "$sol" | sed -n 's/^drift_pct=//p')
    for e in $(printf '%s\n' "$sol" | grep -o '^[nx]264_saturated'); do warn="$warn $clip:$e"; done

    # --- measure: time both encoders AT that point ---
    # RC_MODE=crf is explicit rather than inferred, and N264_CRF/X264_CRF are the
    # per-side overrides perf-comp.sh already exposes -- no bitrate flag goes
    # near this run, so nothing can silently drop one side back into ABR.
    out=$(THREADS="$THREADS" NEXT264="${NEXT264:-$root/build/cli/next264}" \
        RC_MODE=crf N264_CRF="$ncrf" X264_CRF="$xcrf" \
        NEXT264_ARGS="$N_ARGS" X264_ARGS="$X_ARGS" \
        PURE_C="$([ "$MODE" = pure ] && echo 1 || echo 0)" \
        bash "$root/scripts/perf-comp.sh" "$path" "$ncrf" "$SECONDS_PER" 2>&1)
    line=$(printf '%s\n' "$out" | grep 'speed: x264 is' || true)
    if [ -z "$line" ]; then
        printf '%-18s %9s\n' "$clip" "FAILED"
        printf '%s\n' "$out" | tail -3 >&2
        continue
    fi
    x=$(printf '%s\n' "$line" | sed -n 's/.*x264 is \([0-9.]*\)x faster.*/\1/p')
    q=$(printf '%s\n' "$line" | sed -n 's/.*quality: next264 \([-+0-9.]*\) VMAF.*/\1/p')
    s=$(printf '%s\n' "$line" | sed -n 's/.*size: next264 \([-+0-9.%]*\).*/\1/p')
    # The guard is left ARMED. If it fires here the solve did not hold at
    # measurement time and the row is not a matched-point row, so say so on the
    # row itself rather than in stderr nobody reads.
    if printf '%s\n' "$out" | grep -q 'sizes differ'; then
        warn="$warn $clip:guard-fired"; s="$s!"
    fi
    printf '%-18s %9s %9s %8s %8s %7s%%\n' "$clip" "${x}x" "$q" "$s" \
        "$(python3 -c "print(f'{float(\"$point\"):.0f}')")" "$drift"
    xs="$xs $x"; qs="$qs $q"; ss="$ss ${s%\%}"; n=$((n+1))
done
# Same aggregation as the ABR board: median is the headline, max is the clip
# that has to be named, dVMAF is quoted at the matched point where it is a
# legitimate quality delta rather than an operating-point artifact.
if [ "$n" -gt 0 ]; then
    python3 - "$n" "$([ "$MODE" = pure ] && echo pure-C || echo as-shipped-SIMD)" \
        "$THREADS" "$POINT" "$xs" "$qs" "$ss" <<'AGG'
import statistics, sys
n, mode, thr, point, xs, qs, ss = sys.argv[1:8]
x = [float(v) for v in xs.split()]
q = [float(v) for v in qs.split() if v not in ("", "n/a")]
print(f"{'MEAN':<18} {statistics.mean(x):>8.2f}x")
print(f"{'MEDIAN':<18} {statistics.median(x):>8.2f}x   "
      f"({n} clips, {mode}, {thr} thread(s), CRF @ matched {point})")
print(f"{'MAX':<18} {max(x):>8.2f}x")
if q:
    print(f"{'dVMAF':<18} {statistics.median(q):>8.2f}    "
          f"(median; worst {min(q):+.2f})")
# COMPRESSION, surfaced rather than gated. The board holds the RATE fixed and
# lets quality move, so dsize is bounded by the solve tolerance and is NOT a
# BD-rate -- but it is not nothing either: an arm that spends bits shows up
# here first. PART_EARLYTERM=3 moved this row +0.6 with the dVMAF leg reading
# only -0.03 (docs/archive/board-operating-point-pin.md). The authoritative efficiency
# number stays scripts/run_band.py's BD on the CRF band.
sz = [float(v) for v in ss.split() if v not in ("", "n/a") and not v.endswith("!")]
if sz:
    print(f"{'dSIZE':<18} {statistics.median(sz):>+8.2f}%   "
          f"(median vs x264 at the matched point; worst {max(sz):+.2f}%)")
AGG
fi
[ -n "$warn" ] && echo "   [!] flags:$warn" >&2
exit 0
