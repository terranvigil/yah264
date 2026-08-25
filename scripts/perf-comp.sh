#!/usr/bin/env bash
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# perf-comp.sh <clip.y4m> [crf] [seconds] -- head-to-head speed + quality of
# next264 vs x264 at matched settings. Trims the clip to a known frame count,
# encodes it with both, wall-clocks each, and scores VMAF (subsampled to ~5fps).
#
# RATE CONTROL. The [crf] argument is used ONLY when neither NEXT264_ARGS nor
# X264_ARGS selects a rate: pass --bitrate through those (perf-comp-set.sh does)
# and the run is ABR, and the crf argument is ignored rather than stacked on top.
# The script prints which mode it resolved. This matters because the two modes
# answer different questions: ABR compares two encoders at a MATCHED bitrate,
# where a VMAF delta is a quality result; CRF compares them at matched
# rate-factor NUMBERS, which is not a matched operating point -- the CRF scales
# are not the same scale, so the size delta is content luck (measured here:
# foreman -10%, bus +9%, park_joy +44%, sintel -55%) and the VMAF delta is not a
# quality verdict. The script now says so when the sizes diverge. Use
# scripts/bdcompare.py for quality; it normalises bits by construction.
#
# TWO SPEED MODES (SIMD does NOT affect quality -- both encoders' asm/NEON
# kernels are bit-exact with their scalar C, so VMAF/size are identical either
# way; the mode only changes the SPEED comparison):
#
#   optimized (default) -- each encoder with its CPU-SIMD on: x264's hand-asm
#                          (SSE/AVX2/NEON) vs next264's NEON intrinsics. This is
#                          the real-world "as shipped" speed number.
#   pure-C   (PURE_C=1)  -- no hand-asm on either side, both -O3 AUTO-VECTORIZED
#                          C: x264-noasm-autovec vs next264 NEXT264_NO_ASM=1. This
#                          is the product-relevant pure-C number (a shipped pure-C
#                          next264 is -O3 auto-vec too). NOT "both scalar" -- that
#                          mislabel + a -fno-tree-vectorize x264 flattered the gap
#                          to ~1.5x; fair is ~2.2-2.4x. Run via perf-comp-purec.sh.
#
# Note: "asm" and "SIMD" are the same category (CPU vector units) -- x264 writes
# its SIMD as hand assembly, next264 as C intrinsics. GPU/Metal is a different
# category (a separate processor); x264 has no Metal path (only an optional,
# off-by-default OpenCL *lookahead*), so there is no x264 GPU number to compare.
#
# Prints:
#   next264 took A s (XX fps),  x264 took B s (YY fps)
#   next264 vmaf is xxx,  x264 vmaf is yyy
#
# Env overrides:
#   PURE_C       1 = pure-C mode (x264 --no-asm + NEXT264_NO_ASM=1); default 0
#   NEXT264      next264 binary (default build/cli/next264)
#   X264_ASM     optimized x264 (default ../x264/x264-asm, NEON)
#   X264_C       pure-C x264    (default ../x264/x264-noasm, cpu-caps none)
#   X264         override either (wins over the mode-selected default)
#   VMAF         libvmaf CLI    (default 'vmaf')
#   THREADS      threads for both encoders (default: online CPU count)
#   NEXT264_ARGS extra/replacement next264 flags (default: --cabac --bframes 2)
#   X264_ARGS    extra/replacement x264 flags     (default: --bframes 2 --tune psnr:off)
#   VMAF_FPS     target VMAF sampling rate in fps  (default 0 = score EVERY frame)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"

clip="${1:-$root/tests/corpus/ducks_720p.y4m}"
crf="${2:-25}"
seconds="${3:-15}"

NEXT264="${NEXT264:-$root/build/cli/next264}"
# x264 reference binaries built FROM SOURCE (../x264), self-contained, same source
# version, so the comparison is fair and never picks up a stray/asm x264 from PATH.
# x264-asm = NEON. For the pure-C default we use x264-noasm-AUTOVEC (no asm, but the
# C is -O3 auto-vectorized), NOT plain x264-noasm: x264's configure adds
# -fno-tree-vectorize, so plain x264-noasm is GENUINELY SCALAR while next264's
# NEXT264_NO_ASM=1 is only a runtime dispatch switch -- its C fallback is still the
# auto-vectorized -O3 object. Comparing next264-autovec vs x264-scalar is the one
# lopsided quadrant and flattered the gap to ~1.5x; the fair (both-autovec) gap is
# ~2.2-2.4x (docs/archive/purec-harness-bias-handoff.md). Build:
#   cd ../x264 && ./configure --disable-lavf --disable-ffms --disable-avs --disable-swscale && make  # -> x264-asm
#   x264-noasm-autovec: configure --disable-asm ..., strip -fno-tree-vectorize from config.mak, make
X264_ASM="${X264_ASM:-$root/../x264/x264-asm}"
X264_C="${X264_C:-$root/../x264/x264-noasm-autovec}"
VMAF="${VMAF:-vmaf}"
THREADS="${THREADS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
# MATCHED-CONFIG DEFAULTS (fixed 2026-07-15, two sessions). The old defaults ran
# next264 with NO --preset -- its subme-10 PLACEBO default -- vs x264's medium
# default; that ~2x apples-to-oranges trap produced the bogus "4.36x pure-C" /
# "8x, +76% size" readings. next264's preset only sets subme (NOT the ME method or
# subpel tier), so pin ref/bframes to x264 medium too (ref=3 bframes=3, matching
# hex/subme7/8x8dct) for a TRUE medium-vs-medium number. Override NEXT264_ARGS/
# X264_ARGS for other tiers; see docs/pure-c-speed-parity.md.
NEXT264_ARGS="${NEXT264_ARGS:---preset medium --cabac --transform-8x8 --ref 3 --bframes 3}"
X264_ARGS="${X264_ARGS:-}"   # empty = x264's full --preset medium defaults (ref3/bframes3/hex/subme7)
# VMAF SAMPLING. Default 0 = score every frame. This used to be 5 fps, i.e. a
# stride of round(fps/5) -- 10 on the 50 fps clips, 6 on 30 fps, 5 on 25 fps --
# and that stride ALIASES against the mini-GOP frame-type cadence, which at
# --bframes 3 has period 4. Sampling every 10th frame of a 300-frame encode
# lands on a systematically skewed mix of I/P/B frames, and the skew differs
# between two encodes whose frame types fall differently.
#
# Measured on park_joy_720p, x264 at CRF 25, 1 thread vs 18 (same encoder, same
# settings, 0.12% apart in bytes):
#     subsample 10 -> 91.031 vs 93.592     a 2.56 VMAF gap
#     subsample  1 -> 91.812 vs 91.799     no gap
#     global PSNR  -> 31.005 vs 31.006     no gap
# The 2.56 was entirely the sampler. It is larger than most quality results this
# project ships on, and it sat in the dVMAF column of every scoreboard row.
#
# So the default scores every frame. On 300 frames of 720p that is 5.7s against
# 1.9s -- a few seconds on rows that already cost 30-80s of encoding, which is
# not a trade worth a 2.5 VMAF error. Set VMAF_FPS>0 to subsample anyway (it is
# still useful for a rough read), but do not compare two subsampled runs whose
# frame types may differ, and do not quote one as a quality verdict.
VMAF_FPS="${VMAF_FPS:-0}"
PURE_C="${PURE_C:-0}"

# Mode selection: pure-C uses next264 scalar (NEXT264_NO_ASM=1) vs the pure-C x264
# build (no asm compiled in -- no fragile runtime --no-asm needed). Optimized uses
# each encoder's SIMD. X264 override wins if set explicitly.
if [ "$PURE_C" = 1 ]; then
    # No expected-gap number in this label. It used to say "fair gap ~2.2x",
    # which was a CIF measurement baked in as a constant -- so a 720p run printed
    # "~2.2x" and then reported 3.6x underneath it, and the banner got read as
    # the truth and the measurement as a regression. The gap is strongly
    # clip-dependent (CIF ~2.5x, 720p 2.7-3.6x); report what you measured, over a
    # clip SET (make perf-comp-purec-set), never one clip against a fixed number.
    n_env="NEXT264_NO_ASM=1 "; x_asm=""; mode="pure-C (no hand-asm, both -O3 auto-vectorized)"
    X264="${X264:-$X264_C}"
else
    n_env=""; x_asm=""; mode="optimized (CPU-SIMD on, as-shipped)"
    X264="${X264:-$X264_ASM}"
fi
[ -x "$X264" ] || { echo "perf-comp: x264 build not found: $X264 (build it in ../x264 -- see header)" >&2; exit 2; }

for t in "$NEXT264" "$X264" "$VMAF" ffmpeg ffprobe; do
    command -v "$t" >/dev/null 2>&1 || [ -x "$t" ] || { echo "perf-comp: '$t' not found on PATH" >&2; exit 2; }
done
[ -f "$clip" ] || { echo "perf-comp: clip not found: $clip" >&2; exit 2; }

wd="$(mktemp -d)"; trap 'rm -rf "$wd"' EXIT

# Clip fps as a rational -> a plain number, for the VMAF subsample factor.
fps_rat=$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$clip")
fps=$(python3 -c "import sys;n,d=(sys.argv[1].split('/')+['1'])[:2];print(float(n)/float(d))" "$fps_rat")
reqframes=$(python3 -c "import sys;print(int(round(float(sys.argv[1])*float(sys.argv[2]))))" "$fps" "$seconds")

# Trim to a known frame count -> reference for both encodes and for VMAF. This
# fixes the exact N (min of requested and clip length) and keeps VMAF aligned.
ref="$wd/ref.y4m"
ffmpeg -v error -y -i "$clip" -frames:v "$reqframes" -pix_fmt yuv420p "$ref"
N=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$ref")
[ "$N" -gt 0 ] 2>/dev/null || { echo "perf-comp: trimmed reference has no frames" >&2; exit 2; }

# ~VMAF_FPS sampling: score every Nth frame so the effective rate is VMAF_FPS.
# VMAF_FPS<=0 means no subsampling at all -- see the knob's comment above for
# why that is the default.
subsample=$(python3 -c "
import sys
fps, want = float(sys.argv[1]), float(sys.argv[2])
print(1 if want <= 0 else max(1, int(round(fps / want))))" "$fps" "$VMAF_FPS")

# WHICH RATE-CONTROL MODE ARE WE ACTUALLY MEASURING? This script used to append
# `--crf $crf` to BOTH command lines unconditionally, and callers then added
# `--bitrate` on top via NEXT264_ARGS/X264_ARGS (perf-comp-set.sh does exactly
# that). Two flags, two encoders, two different precedence rules: next264 takes
# --bitrate whenever it is present regardless of order (cli/next264_cli.c, the
# `else if (crf10 > 0)` arm is only reached when bitrate is 0), x264 takes
# whichever came LAST. They happened to agree, so the ABR runs were sound -- but
# only by luck of argument order, and nothing in the output said which mode had
# been measured. Reorder X264_ARGS and x264 silently drops to CRF while next264
# stays in ABR, and the table still prints a confident dVMAF.
#
# It also made the bare invocation dangerously easy to misread. `perf-comp.sh
# foreman_cif.y4m 30 3` is CONSTANT-QUALITY -- there is no bitrate target in it
# at all -- yet its output was quoted in f60fd43 as next264 "undershooting the
# bitrate target" by 11%. At equal CRF the two encoders sit at different points
# on their own RD curves and the size delta is content luck, not tracking: on
# this tree it is foreman -10%, bus +9%, sintel -55%, park_joy +44%.
#
# So: resolve the mode once, pass exactly one rate flag, and say so out loud.
#
# 2026-08-11, second pass: the scoreboard had only ever run ABR, so "goal 2" was
# an ABR number that wasn't labelled one. RC_MODE now selects explicitly and the
# inference below is only a fallback for old callers. The modes are NOT
# interchangeable measurements -- see docs/rc-mode-matrix.md for which of them
# compare a matched operating point and which do not.
rc_mode="${RC_MODE:-}"
if [ -z "$rc_mode" ]; then
    case " $NEXT264_ARGS $X264_ARGS " in
        *" --pass "*)                              rc_mode=2pass ;;
        *" --bitrate "*" --vbv-maxrate "*|*" --vbv-maxrate "*" --bitrate "*) rc_mode=cbr ;;
        *" --vbv-maxrate "*)                       rc_mode=cvbr ;;
        *" --bitrate "*)                           rc_mode=abr ;;
        *" --qp "*)                                rc_mode=cqp ;;
        *)                                         rc_mode=crf ;;
    esac
fi
case "$rc_mode" in
    crf|cqp|abr|cbr|cvbr|2pass) ;;
    *) echo "perf-comp: unknown RC_MODE '$rc_mode' (crf|cqp|abr|cbr|cvbr|2pass)" >&2; exit 2 ;;
esac
n_rc=""; x_rc=""
# crf and cvbr both need the rate factor on the command line; cvbr's caller
# supplies the --vbv-* cap through the ARGS vars on top of it.
case "$rc_mode" in
    crf|cvbr) n_rc="--crf ${N264_CRF:-$crf}"; x_rc="--crf ${X264_CRF:-$crf}" ;;
esac
# Bitrate target (kbit/s) for the rate-accuracy line; both sides are asserted
# equal. Only abr/cbr/2pass HAVE a target -- capped VBR is a quality mode with a
# ceiling, so its achieved rate is legitimately below the cap and a "rate error"
# against the cap would be meaningless. What matters for cvbr (and cbr) is
# whether the cap was RESPECTED, which is checked stream-side further down.
rc_target=$(printf '%s\n' "$NEXT264_ARGS" | sed -n 's/.*--bitrate \([0-9]*\).*/\1/p')
x_target=$(printf '%s\n' "$X264_ARGS" | sed -n 's/.*--bitrate \([0-9]*\).*/\1/p')
case "$rc_mode" in cvbr|crf|cqp) rc_target="" ;; esac
if [ -n "$rc_target" ] && [ -n "$x_target" ] && [ "$rc_target" != "$x_target" ]; then
    echo "perf-comp: the two sides target different bitrates ($rc_target vs $x_target kbit/s)" >&2
    exit 2
fi
# VBV cap, for the stream-side compliance check on cbr/cvbr.
vbv_max=$(printf '%s\n' "$NEXT264_ARGS" | sed -n 's/.*--vbv-maxrate \([0-9]*\).*/\1/p')
vbv_buf=$(printf '%s\n' "$NEXT264_ARGS" | sed -n 's/.*--vbv-bufsize \([0-9]*\).*/\1/p')

# x264-side baseline cache: the reference numbers (seconds/vmaf/bytes) are a pure
# function of (x264 binary, clip, frames, rate config, threads, runs, vmaf
# sampling), so cache them and only re-measure next264 on repeat runs. Keyed on
# the binary's md5 -- a rebuilt x264 invalidates itself. N264_REFENC_CACHE=0
# bypasses (and refreshes). Disabled when OUTDIR wants the x264.mp4 artifacts.
cachedir="$root/tests/.perfcache"
xkey=""
if [ "${N264_REFENC_CACHE:-1}" = 1 ] && [ -z "${OUTDIR:-}" ]; then
    # rc_mode is in the key because it changes which rate flag x264 is handed;
    # without it a pre-2026-08-11 CRF entry can answer an ABR question.
    xkey=$(python3 - "$X264" "$(basename "$clip")" "$N" "$crf" "$X264_ARGS" "$THREADS" "${RUNS:-1}" "$subsample" "$rc_mode" "$x_rc" <<'KEY'
import hashlib, sys
h = hashlib.md5()
h.update(open(sys.argv[1], 'rb').read())
h.update('|'.join(sys.argv[2:]).encode())
print(h.hexdigest())
KEY
)
fi

# Wall-clock a command (encoder chatter silenced), echo real seconds.
timed() {
    # MEDIAN of RUNS wall times, not the minimum, and that distinction is not
    # pedantry -- it was measured. Two IDENTICAL copies of the same arm differ
    # 7.6% on best-of-N minima and 0.2% on medians (docs/archive/bf3-scaling-diagnosis.md,
    # session 3); reading minima flipped one clip's sign three times across
    # batches. This function used to return the min, and that is the direct cause
    # of the scoreboard's own instability: a 2026-08-11 run had samsung_720p at
    # 6.6x in the pure-C tier and 0.9x in the SIMD tier, which no configuration
    # difference can produce.
    #
    # A minimum is the right statistic when noise is one-sided contamination you
    # want to discard. It is the wrong one here because the pool's scheduling
    # genuinely varies run to run, so the fastest run is an outlier of the thing
    # being measured rather than a cleaner look at it.
    #
    # SPREAD is reported to stderr when it is wide, because a median hides a
    # cooked box: after hours of continuous benchmarking this machine has
    # returned a 5x range for one command, at which point no statistic helps.
    #
    # THE TIMER IS perf_counter, NOT `/usr/bin/time -p`, and that is not a
    # detail. POSIX `time -p` prints TWO DECIMALS. At 12 threads the CIF cells
    # run 0.10-0.13 s, so one quantum is 8-10% of the cell and the scoreboard
    # cannot resolve anything smaller. Measured 2026-08-17: a round worth
    # +2.3% on foreman_cif and +2.7% on stefan_cif (bench/bin_ab.py, medians of
    # 9, interleaved) printed the SAME 1.22x and 1.20x here before and after,
    # while the three 720p rows -- long enough to have resolution -- each moved
    # 0.02. The board was systematically under-reporting its own progress, and
    # the median lands between a resolvable clip and an unresolvable one, so it
    # printed 1.17x for two binaries 3% apart.
    #
    # docs/archive/goal3-width-scaling.md already found this for the MAX leg and
    # re-timed those cells by hand. This fixes the harness instead.
    #
    # Both timers bracket the same thing (fork -> exec -> reap of `sh -c cmd`),
    # so the numbers are comparable apart from the resolution; they are not
    # comparable to boards taken before this change at the 0.01x level.
    # REPEATED SAMPLES BELOW THE FLOOR (2026-08-19). perf_counter fixed the
    # PRINTING resolution, but a 0.05-0.13 s CIF cell at 12 threads is still
    # one scheduling episode per sample, and the documented +-0.04 per-clip
    # spread lives there -- goal 2's median has to be quoted as a 1.00-1.04
    # BAND because of it. Each timing sample is therefore k back-to-back
    # executions divided by k, with k chosen by one untimed calibration run so
    # a sample lasts at least PERF_REPEAT_FLOOR seconds (default 0.35; the
    # 720p cells calibrate to k=1 and are unchanged). The encode, the solve
    # pins and the quality/size columns are untouched -- only the timed
    # window lengthens. PERF_REPEAT=k forces a count; the calibration run
    # doubles as a cache warmup, so the samples are also more homogeneous.
    RUNS="${RUNS:-1}" python3 - "$1" <<'PY'
import math, os, statistics, subprocess, sys, time
cmd = sys.argv[1]
def run():
    r = subprocess.run(["sh", "-c", cmd], stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        sys.exit(1)
rep = int(os.environ.get("PERF_REPEAT", "0"))
floor = float(os.environ.get("PERF_REPEAT_FLOOR", "0.35"))
t0 = time.perf_counter()
run()                                   # calibration + warmup, untimed
cal = time.perf_counter() - t0
if rep <= 0:
    rep = max(1, min(10, math.ceil(floor / max(cal, 1e-6))))
ts = []
for _ in range(int(os.environ.get("RUNS", "1"))):
    t0 = time.perf_counter()
    for _ in range(rep):
        run()
    ts.append((time.perf_counter() - t0) / rep)
med = statistics.median(ts)
if len(ts) > 1 and min(ts) > 0 and max(ts) / min(ts) > 1.15:
    print(f"  [warn] wall spread {max(ts)/min(ts):.2f}x over {len(ts)} samples "
          f"({min(ts):.3f}-{max(ts):.3f}s, repeat={rep}) -- box may be loaded "
          f"or thermally saturated; treat this row as unreliable", file=sys.stderr)
print(f"{med:.4f}")
PY
}

# Time TWO commands INTERLEAVED, run by run with a mirrored order, and echo
# "median_a median_b".
#
# WHY, measured 2026-08-17. Timing all N runs of one encoder and then all N of
# the other puts every minute of box drift on one arm. On this board's CIF rows
# at 12 threads -- cells that run 0.10-0.13 s -- THE SAME BINARY re-run through
# the sequential form read foreman 1.19x then 1.29x, stefan 1.19x then 1.31x,
# and the tier median 1.16x then 1.21x. That is a bigger swing than a good
# round produces, so a sequential A/B of two binaries measures the box.
#
# The old two-decimal timer HID this rather than avoiding it: quantisation
# snapped a 0.10 s cell to the same printed value run after run, which is why
# the board was recorded as "stable to 0.01" while actually being this noisy.
#
# Interleaving is the fix the rest of the tree already uses
# (bench/mtceiling/capacity.py, bench/bin_ab.py): the box's load moves slower
# than one burst and faster than one round.
timed2() {
    # Same repeated-sample floor as timed() above, with ONE repeat count k
    # SHARED by both arms (a per-arm k would time two different sample shapes
    # and put the difference in the ratio). k is calibrated off the FASTER
    # arm's untimed warmup run, so both arms' samples clear the floor.
    RUNS="${RUNS:-1}" python3 - "$1" "$2" <<'PY'
import math, os, statistics, subprocess, sys, time
cmds = [sys.argv[1], sys.argv[2]]
def run(k):
    r = subprocess.run(["sh", "-c", cmds[k]], stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        sys.exit(1)
rep = int(os.environ.get("PERF_REPEAT", "0"))
floor = float(os.environ.get("PERF_REPEAT_FLOOR", "0.35"))
cal = [0.0, 0.0]
for k in (0, 1):                        # calibration + warmup, untimed
    t0 = time.perf_counter()
    run(k)
    cal[k] = time.perf_counter() - t0
if rep <= 0:
    rep = max(1, min(10, math.ceil(floor / max(min(cal), 1e-6))))
ts = [[], []]
for i in range(int(os.environ.get("RUNS", "1"))):
    for k in ((0, 1) if i % 2 == 0 else (1, 0)):   # mirrored: neither arm always first
        t0 = time.perf_counter()
        for _ in range(rep):
            run(k)
        ts[k].append((time.perf_counter() - t0) / rep)
for k, t in enumerate(ts):
    if len(t) > 1 and min(t) > 0 and max(t) / min(t) > 1.15:
        print(f"  [warn] wall spread {max(t)/min(t):.2f}x over {len(t)} samples of "
              f"arm {k} ({min(t):.3f}-{max(t):.3f}s, repeat={rep}) -- box may be "
              f"loaded or thermally saturated; treat this row as unreliable", file=sys.stderr)
print(f"{statistics.median(ts[0]):.4f} {statistics.median(ts[1]):.4f}")
PY
}

case "$rc_mode" in
    crf)   rc_desc="CRF ${n_rc#--crf } / x264 ${x_rc#--crf } (constant quality -- NO bitrate target)" ;;
    cqp)   rc_desc="CQP (fixed --qp from the caller's args)" ;;
    abr)   rc_desc="ABR target ${rc_target} kbit/s" ;;
    cbr)   rc_desc="CBR target ${rc_target} kbit/s, VBV cap ${vbv_max} kbit/s buf ${vbv_buf} kbit" ;;
    cvbr)  rc_desc="capped VBR: CRF ${n_rc#--crf } / x264 ${x_rc#--crf }, VBV cap ${vbv_max} kbit/s buf ${vbv_buf} kbit" ;;
    2pass) rc_desc="2-pass ABR target ${rc_target} kbit/s (wall = pass 1 + pass 2)" ;;
esac
vmaf_desc="every frame"
[ "$subsample" -gt 1 ] && vmaf_desc="~${VMAF_FPS}fps, subsample=${subsample} -- MAY ALIAS WITH FRAME TYPE"
echo ">> $(basename "$clip")  ${N} frames @ ${fps%.*} fps  threads=${THREADS}  vmaf: ${vmaf_desc}"
echo ">> rate control: ${rc_desc}"
echo ">> mode: ${mode}"

# 2-pass is timed as the SUM of both passes, because that is what a 2-pass
# encode costs. Running only pass 2 off a warm stats file would report a number
# no user ever experiences. The `&&` keeps both passes inside one /usr/bin/time.
if [ "$rc_mode" = 2pass ]; then
    n_cmd="${n_env}'$NEXT264' --input-y4m '$ref' --pass 1 --stats '$wd/n.stats' $NEXT264_ARGS --threads $THREADS -o '$wd/p1.264' && \
           ${n_env}'$NEXT264' --input-y4m '$ref' --pass 2 --stats '$wd/n.stats' $NEXT264_ARGS --threads $THREADS -o '$wd/next.264'"
    x_cmd="'$X264' --pass 1 --stats '$wd/x.stats' $x_asm $X264_ARGS --threads $THREADS -o '$wd/xp1.264' '$ref' && \
           '$X264' --pass 2 --stats '$wd/x.stats' $x_asm $X264_ARGS --threads $THREADS -o '$wd/x264.264' '$ref'"
else
    n_cmd="${n_env}'$NEXT264' --input-y4m '$ref' $n_rc $NEXT264_ARGS --threads $THREADS -o '$wd/next.264'"
    x_cmd="'$X264' $x_rc $x_asm $X264_ARGS --threads $THREADS -o '$wd/x264.264' '$ref'"
fi

x_cached=0
if [ -n "$xkey" ] && [ -f "$cachedir/$xkey" ]; then
    # Cached baseline: our arm is timed NOW against an x264 number measured in
    # some other box state, so this path cannot be interleaved and its ratio
    # carries whatever drift sits between the two. Fine while iterating on one
    # binary; never for a board.
    n_sec=$(timed "$n_cmd")
    read -r x_sec x_vmaf x_bytes < "$cachedir/$xkey"
    x_cached=1
    echo ">> x264 baseline: cached (tests/.perfcache/$xkey; N264_REFENC_CACHE=0 to re-measure)"
else
    read -r n_sec x_sec <<EOF
$(timed2 "$n_cmd" "$x_cmd")
EOF
fi

ffmpeg -v error -y -i "$wd/next.264" -pix_fmt yuv420p "$wd/next.y4m"
[ "$x_cached" = 1 ] || ffmpeg -v error -y -i "$wd/x264.264"  -pix_fmt yuv420p "$wd/x264.y4m"

n_bytes=$(wc -c < "$wd/next.264")
[ "$x_cached" = 1 ] || x_bytes=$(wc -c < "$wd/x264.264")

vmaf_of() {  # <decoded.y4m> -> mean VMAF (v0.6.1)
    "$VMAF" -r "$ref" -d "$1" --subsample "$subsample" \
        --model version=vmaf_v0.6.1:name=vmaf --json -o "$wd/v.json" >/dev/null 2>&1
    python3 -c "import json,sys;print(f\"{json.load(open(sys.argv[1]))['pooled_metrics']['vmaf']['mean']:.3f}\")" "$wd/v.json"
}
n_vmaf=$(vmaf_of "$wd/next.y4m")
if [ "$x_cached" = 0 ]; then
    x_vmaf=$(vmaf_of "$wd/x264.y4m")
    if [ -n "$xkey" ]; then
        mkdir -p "$cachedir"
        printf '%s %s %s\n' "$x_sec" "$x_vmaf" "$x_bytes" > "$cachedir/$xkey"
    fi
fi

# VBV COMPLIANCE is the structural check for cbr/cvbr, and it is the right kind
# of check: whether the stream respects the buffer is a property of the
# bitstream, not a timing argument, so one decode settles it. A capped-VBR row
# whose cap was violated is not a slower-or-faster result, it is an invalid
# encode, and reporting speed for it would be reporting the speed of cheating.
vbv_verdict=""
if [ "$rc_mode" = cbr ] || [ "$rc_mode" = cvbr ]; then
    if [ -n "$vbv_max" ] && [ -n "$vbv_buf" ]; then
        # --y4m, not --fps: the checker reads the rate out of the very file both
        # encoders were handed, so there is no framerate parameter in this call
        # for anyone to get wrong. $ref is the trimmed reference and ffmpeg
        # carries the source's F token through verbatim (F50:1, F30000:1001, ...).
        nvb=$(python3 "$root/scripts/vbv_check.py" "$wd/next.264" --maxrate "$vbv_max" \
                --bufsize "$vbv_buf" --y4m "$ref" --quiet >/dev/null 2>&1 && echo ok || echo UNDERFLOW)
        vbv_verdict="next264 VBV: $nvb"
        if [ "$x_cached" = 0 ] && [ -f "$wd/x264.264" ]; then
            xvb=$(python3 "$root/scripts/vbv_check.py" "$wd/x264.264" --maxrate "$vbv_max" \
                    --bufsize "$vbv_buf" --y4m "$ref" --quiet >/dev/null 2>&1 && echo ok || echo UNDERFLOW)
            vbv_verdict="$vbv_verdict,  x264 VBV: $xvb"
        fi
        echo ">> $vbv_verdict"
    fi
fi

python3 - "$N" "$n_sec" "$x_sec" "$n_vmaf" "$x_vmaf" "$n_bytes" "$x_bytes" "$fps" "$(basename "$clip")" "$rc_mode" "${rc_target:-0}" <<'PY'
import sys
N=int(sys.argv[1]); ns=float(sys.argv[2]); xs=float(sys.argv[3])
nv=float(sys.argv[4]); xv=float(sys.argv[5]); nb=int(sys.argv[6]); xb=int(sys.argv[7]); fps=float(sys.argv[8])
clip=sys.argv[9]; rc_mode=sys.argv[10]; target=float(sys.argv[11])
def kbps(b): return b*8/1000.0/(N/fps)
print()
print(f"next264 took {ns:.2f} s ({N/ns:.1f} fps),  x264 took {xs:.2f} s ({N/xs:.1f} fps)")
print(f"next264 vmaf is {nv:.2f},  x264 vmaf is {xv:.2f}")
print(f"next264 {nb/1024:.0f} KiB ({kbps(nb):.0f} kbps),  x264 {xb/1024:.0f} KiB ({kbps(xb):.0f} kbps)")

# RATE ACCURACY is a first-class result, not a footnote: a speed number taken at
# a rate the encoder missed is not a comparison. Print the signed error against
# the target for both sides whenever there IS a target.
if rc_mode in ("abr", "cbr", "2pass") and target > 0:
    print(f"rate accuracy vs {target:.0f} kbit/s target:  "
          f"next264 {kbps(nb)/target*100-100:+.1f}%,  x264 {kbps(xb)/target*100-100:+.1f}%")
print()
# Two decimals on the speed ratio, not one. The set scripts parse THIS string
# and average it, so a 1-decimal ratio quantises every row to 0.05 and the mean
# inherits it -- 1.4x and 1.5x are 7% apart and were rendering as adjacent
# ticks. The box's own repeatability is 1.004x over 6 runs and these are
# medians, so the second digit is measurement, not decoration.
print(f"[{clip}]  speed: x264 is {ns/xs:.2f}x faster   quality: next264 {nv-xv:+.2f} VMAF   size: next264 {nb/xb*100-100:+.1f}%")

# A VMAF delta only means "better" when the two encodes spent comparable bits.
# In CRF the two encoders' rate-factor scales are NOT the same scale -- next264's
# CRF resolves to a flat qp = crf + 5.4 with no complexity term on the default
# path (src/encoder/encoder.c rc_set_qp_crf; the 140/1.8/1.5 calibration is
# behind `crf_cl && mbtree_on` and so is unreachable by default), while x264
# modulates qscale by blurred complexity. So equal CRF puts them at different
# operating points by an amount that is pure content luck, measured on this tree
# as foreman -10%, bus +9%, park_joy +44%, sintel -55%. Quoting the dVMAF from
# such a run as a quality verdict is what produced the "-1.9 VMAF, undershooting
# the bitrate target" reading in f60fd43, on a command that had no target at all.
spread = abs(nb/xb*100-100)
if spread > 5:
    print(f"   [!] sizes differ {spread:.0f}% -- the two encodes did NOT spend comparable")
    print(f"       bits, so the VMAF delta above is an operating-point difference, not a")
    print(f"       quality verdict. For quality use scripts/bdcompare.py --vmaf --no-cache;")
    print(f"       for a matched-rate comparison run ABR (perf-comp-set.sh passes --bitrate).")
PY
echo "   (${mode}; quality/size are SIMD-invariant -- identical in either mode)"
if [ -n "${PURE_C:-}" ]; then
    echo "   single-clip number; the pure-C gap is clip-dependent -- 'make perf-comp-set' prints the campaign table"
fi

# Keep the encodes for side-by-side viewing (e.g. VLC). Set OUTDIR=<dir> to save
# next264 / x264 / source as .mp4 plus the raw bitstreams. The .mp4s are a
# LOSSLESS re-encode (qp 0) at the clip's framerate -- raw Annex-B .264 carries no
# timestamps, so a plain `-c copy` mux guesses the fps and drops the tail B-frames
# (next264 emits all frames -- the .264 has them; it's a muxing artifact). The
# lossless re-encode preserves every frame + exact pixels + correct timing.
if [ -n "${OUTDIR:-}" ]; then
    mkdir -p "$OUTDIR"
    ll() { ffmpeg -v error -y -r "$fps" -i "$1" -c:v libx264 -qp 0 -preset ultrafast -pix_fmt yuv420p "$2"; }
    ll "$wd/next.264" "$OUTDIR/next264.mp4"
    ll "$wd/x264.264" "$OUTDIR/x264.mp4"
    ffmpeg -v error -y -i "$ref" -c:v libx264 -qp 0 -preset ultrafast -pix_fmt yuv420p "$OUTDIR/source.mp4"
    cp "$wd/next.264" "$OUTDIR/next264.264"; cp "$wd/x264.264" "$OUTDIR/x264.264"
    echo ""
    echo ">> saved to $OUTDIR:  next264.mp4  x264.mp4  source.mp4  (all $N frames; + .264 bitstreams)"
fi
