#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# perf-comp-set.sh -- run the speed comparison over a CLIP SET and print one
# table, because there is no single "the" gap vs x264.
#
# Quoting any one clip as "the gap" is how a 2.5x CIF measurement became a
# campaign headline that a 720p clip then looked like a regression against.
# Report the set, or say which clip you mean.
#
# 2026-08-11: this ran `--ref 1 --bframes 2` on both encoders for years. Those
# overrides predate `--preset medium` matching x264's, and bframes 2 is BELOW
# x264 medium's own default of 3 -- so the headline was quoted at a setting
# neither encoder would choose. It also happened to be near yah264's worst
# case: MT scaling swings 2x with bframes while single-threaded work stays flat
# (bf3/bf7 scale 5-10x and beat x264, bf1/bf2/bf4 scale 3-4x), root-caused to
# pool occupancy in docs/archive/bframes-scaling-mechanism.md. The overrides are gone;
# both encoders now take the preset's defaults, which is the config a user
# actually gets. NOTE the numbers are NOT comparable to the pre-2026-08-11
# series. The occupancy defect at bframes 2 is understood but NOT fixed -- it
# is simply no longer what the scoreboard measures, which is a reason to keep
# reading docs/archive/bframes-scaling-mechanism.md, not a reason to consider it closed.
#
# Usage: scripts/perf-comp-set.sh [pure|asm]   (default: pure)
# Env: YAH264, X264_C, X264_ASM, VMAF, SET_SECONDS, SET_CRF, CLIPS
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-pure}"
SECONDS_PER="${SET_SECONDS:-6}"
CRF="${SET_CRF:-30}"
PRESET="${SET_PRESET:-medium}"

# SET_RC picks the rate-control mode. abr is the DEFAULT and every historical
# number from this script is one, so leave it alone to reproduce a past run.
#
# crf exists because it is the mode most people actually encode in, and because
# "same setting, what do I get" is the question a reviewer asks first. The two
# are not interchangeable: in ABR both encoders are handed the same bitrate and
# the interesting columns are time and quality, while in CRF both are handed the
# same rate factor and the file SIZES come out different, because the two CRF
# scales are not the same scale. That is why the size column is printed either
# way and why the CRF header says out loud what it means.
SET_RC="${SET_RC:-abr}"
case "$SET_RC" in
    abr|crf) ;;
    *) echo "perf-comp-set: SET_RC must be abr or crf, got '$SET_RC'" >&2; exit 2 ;;
esac

# The clip set and its per-clip operating points, shared with the matched-point
# CRF scoreboard so the two cannot drift apart. Full calibration rationale is in
# that file.
#
# NOTE these numbers are NOT comparable to any earlier series -- this is the
# second discontinuity in one day (the first was dropping --ref 1 --bframes 2).
#
# Rate error is NOT zero at these points and cannot be made zero: x264's own ABR
# undershoots high-motion CIF (bus -8.4%, stefan -15.6% at 400 kbit/s) and
# overshoots ducks (+11.7% at 25000). That is x264's rate control, not
# saturation and not something a target choice fixes, so perf-comp.sh prints the
# signed error for both sides on every ABR row and it should be read, not
# assumed away.
. "$root/scripts/parity-clips.sh"

script="$root/scripts/perf-comp.sh"
[ "$MODE" = pure ] && export PURE_C=1

if [ "$SET_RC" = crf ]; then
    echo "## CRF $CRF, both encoders, preset $PRESET, ${SECONDS_PER}s, threads ${SET_THREADS:-1}"
    echo "## Same rate factor on both sides, so the SIZES differ -- read all three"
    echo "## columns together. dsize is how much bigger yah264's file is."
else
    echo "## ABR, both encoders on each clip's target, preset $PRESET, ${SECONDS_PER}s, threads ${SET_THREADS:-1}"
fi
printf '%-22s %10s %10s %8s\n' clip "x264 x" "dVMAF" "dsize"
printf '%-22s %10s %10s %8s\n' ---------------------- ---------- ---------- --------
xs=""; qs=""; n=0
for entry in $CLIPS; do
    clip="${entry%%:*}"; br="${entry##*:}"
    path="$root/tests/corpus/$clip.y4m"
    [ -f "$path" ] || { printf '%-22s %10s\n' "$clip" "(missing)"; continue; }
    if [ "$SET_RC" = crf ]; then
        yargs="--preset $PRESET --cabac --transform-8x8"; xargs="--preset $PRESET"
    else
        yargs="--preset $PRESET --cabac --transform-8x8 --bitrate $br"; xargs="--preset $PRESET --bitrate $br"
    fi
    out=$(THREADS="${SET_THREADS:-1}" YAH264="${YAH264:-$root/build/cli/yah264}" \
        RC_MODE="$SET_RC" YAH264_ARGS="$yargs" X264_ARGS="$xargs" \
        bash "$script" "$path" "$CRF" "$SECONDS_PER" 2>&1)
    line=$(printf '%s\n' "$out" | grep 'speed: x264 is' || true)
    if [ -z "$line" ]; then
        printf '%-22s %10s\n' "$clip" "FAILED"
        printf '%s\n' "$out" | tail -3 >&2
        continue
    fi
    x=$(printf '%s\n' "$line" | sed -n 's/.*x264 is \([0-9.]*\)x faster.*/\1/p')
    q=$(printf '%s\n' "$line" | sed -n 's/.*quality: yah264 \([-+0-9.]*\) VMAF.*/\1/p')
    s=$(printf '%s\n' "$line" | sed -n 's/.*size: yah264 \([-+0-9.%]*\).*/\1/p')
    printf '%-22s %10s %10s %8s\n' "$clip" "${x}x" "$q" "$s"
    xs="$xs $x"; qs="$qs $q"; n=$((n+1))
done
# MEDIAN is the headline, not the mean. A six-clip mean is one outlier's
# hostage -- samsung alone moved the 08-13 board a tenth -- and the claim
# format the plan asks for is median + max + dVMAF, so the harness prints all
# three rather than leaving the reader to recompute them off the rows. MEAN is
# kept so the older series stays readable against the new one.
[ "$n" -gt 0 ] && python3 - "$n" "$([ "$MODE" = pure ] && echo pure-C || echo as-shipped-SIMD)" \
    "${SET_THREADS:-1}" "$xs" "$qs" <<'AGG'
import statistics, sys
n, mode, thr, xs, qs = sys.argv[1:6]
x = [float(v) for v in xs.split()]
q = [float(v) for v in qs.split() if v not in ("", "n/a")]
row = lambda lbl, v: print(f"{lbl:<22} {v:>9.2f}x")
row("MEAN", statistics.mean(x))
print(f"{'MEDIAN':<22} {statistics.median(x):>9.2f}x   "
      f"({n} clips, {mode}, {thr} thread(s))")
row("MAX", max(x))
if q:
    print(f"{'dVMAF':<22} {statistics.median(q):>9.2f}    "
          f"(median; worst {min(q):+.2f})")
AGG
