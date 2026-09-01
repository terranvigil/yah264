#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# ffboard.py -- the speed board taken THROUGH ONE FFMPEG BINARY, with both
# encoders called as libraries in the same process.
#
# WHY A SECOND BOARD. scripts/perf-comp.sh runs two CLIs and feeds each one Y4M,
# so each encoder's own input reader sits inside the measurement. The obvious
# worry is that the board is partly scoring our Y4M path against x264's. This
# harness removes the question by construction: one demuxer, one process, one
# thread pool, both encoders reached through libavcodec.
#
# WHAT IT FOUND. Nothing, which is the useful part. Run against the CLI board at
# the same operating points the two agree within noise on every clip, so the
# input path was never where the gap lived. Keep this board as the control, not
# as the headline -- the CLI board is cheaper and measures the same thing.
#
# It also needs an ffmpeg built against libyah264, so it cannot run from a
# stock checkout. See docs/ffmpeg-integration-plan.md.
#
# Usage:  RC=crf THREADS=12 X264LIB=... NOASM=1 scripts/ffboard.py
#
# Env:
#   FF        ffmpeg binary built with --enable-libyah264 --enable-libx264
#   X264LIB   install prefix of the libx264 to load (asm or autovec build)
#   Y264LIB   install prefix of libyah264
#   NOASM     1 = force yah264's scalar path (YAH264_NO_ASM), and select the
#             pure-C libx264 via X264LIB. Only yah264 has such a switch: for
#             any other encoder under test the flag affects the REFERENCE only
#   ENC       encoder under test (default libyah264). libx264 is always the
#             reference. libopenh264 is ABR-only, see the RC note below
#   RC        crf (matched operating point, the headline) | abr (rate-matched)
#   THREADS   thread count handed to both encoders
#   CORP      clip directory (default tests/corpus)
#   RUNS, REPEAT_FLOOR, SECONDS, PRESET, VMAF
#
# THE PURE-C ARM IS A TRAP. x264's configure adds -fno-tree-vectorize
# UNCONDITIONALLY (configure ~line 1438, outside any asm test), so every stock
# x264 build has vectorization suppressed -- its C is a fallback behind hand-asm,
# not a tuned target. yah264's YAH264_NO_ASM=1 is only a runtime dispatch
# switch, so our C stays -O3 auto-vectorized. Point X264LIB at a stock
# --disable-asm build and you are comparing our vectorized C against their
# scalar C: that reads goal 2 as 0.73x instead of 1.04x, a third of a supposed
# win that was entirely the flag.
#
# Note the corollary, since it is the obvious thing to reach for instead:
# x264's RUNTIME toggle does not avoid this. `x264 --asm 0` on a stock build
# measures 1.19s where the flag-stripped build measures 0.76s on the same clip
# -- identical to the genuinely-scalar build, because the flag was applied when
# the binary was compiled and no runtime switch can undo it.
#
# Build the pure-C libx264 the way scripts/perf-comp.sh documents: configure
# --disable-asm, strip -fno-tree-vectorize from config.mak, then make.

import os, subprocess, sys, time, json, math, resource

_ROOT       = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_FF_DEFAULT = os.path.join(os.path.dirname(_ROOT), "FFmpeg", "ffmpeg")

FF      = os.environ.get("FF", _FF_DEFAULT)
X264LIB = os.environ.get("X264LIB", "")     # install prefix, required
Y264LIB = os.environ.get("Y264LIB", "")     # install prefix, required
CORP    = os.environ.get("CORP", os.path.join(_ROOT, "tests", "corpus"))
SECONDS = float(os.environ.get("SECONDS", "6"))
THREADS = os.environ.get("THREADS", "12")
NOASM   = os.environ.get("NOASM", "0") == "1"
RUNS    = int(os.environ.get("RUNS", "3"))
FLOOR   = float(os.environ.get("REPEAT_FLOOR", "0.35"))
PRESET  = os.environ.get("PRESET", "medium")
RC      = os.environ.get("RC", "abr")          # abr | abrm | crf
# The encoder under test. libx264 is always the reference the ratio is taken
# against. openh264 exposes no quality knob through ffmpeg, only a bitrate, so
# it can only be boarded at RC=abr; and it has no scalar/SIMD switch of its own,
# so its "pure-C" rows mean openh264 as built against a pure-C x264.
ENC     = os.environ.get("ENC", "libyah264")
WD      = os.environ.get("WD", os.path.join(os.environ.get("TMPDIR", "/tmp"), "ffboard"))
VMAF    = os.environ.get("VMAF", "vmaf")
# Which arm is timed FIRST within each sample pair. The pair is mirrored run to
# run either way (see measure/measure_crf), so this does not change what the
# board cancels -- it only decides which arm leads. Flipping it is the check for
# an order effect the mirroring failed to cancel: if the ratio moves when the
# leader changes, something is warming for the follower and neither reading is
# clean. ORDER=x264 leads with the reference.
ORDER_X_FIRST = os.environ.get("ORDER", "enc").lower() in ("x264", "x", "ref", "b")

# clip:target-kbps, straight from scripts/parity-clips.sh
CLIPS = [("foreman_cif", 400), ("bus_cif", 400), ("stefan_cif", 400),
         ("ducks_720p", 25000), ("park_joy_720p", 12000), ("samsung_720p", 1200)]

# CLIPS=name:kbps[,name:kbps...] boards a different set -- one clip, or a clip
# the standing board does not carry. The board list above stays the default so
# a bare run is still the comparable one; anything else is a side question and
# its MEDIAN/MAX rows are that subset's, not the board's. A single-clip run
# makes median and max the same number, which is the point when the question is
# about one clip rather than about the corpus.
if os.environ.get("CLIPS"):
    try:
        CLIPS = [(c.rsplit(":", 1)[0], int(c.rsplit(":", 1)[1]))
                 for c in os.environ["CLIPS"].split(",") if c.strip()]
    except (ValueError, IndexError):
        sys.exit("ffboard: CLIPS wants name:kbps[,name:kbps...], "
                 f"got '{os.environ['CLIPS']}'")

os.makedirs(WD, exist_ok=True)

# Our encoder answers to both names: a library installed before the yah264
# rename still reads the NEXT264_ spelling. Match either and set both, because
# a knob that silently misses leaves us vectorized while the reference goes
# scalar, and that reads as a win rather than as an error.
OURS = ("libyah264", "libnext264")

def env(under_test):
    e = dict(os.environ, DYLD_LIBRARY_PATH=f"{X264LIB}/lib:{Y264LIB}/lib")
    e.pop("YAH264_NO_ASM", None)
    e.pop("NEXT264_NO_ASM", None)
    if under_test and NOASM:
        if ENC in OURS:
            e["YAH264_NO_ASM"] = "1"
            e["NEXT264_NO_ASM"] = "1"
        else:
            print(f"ffboard: NOASM=1 does not reach {ENC}, which has no scalar "
                  "switch; only the reference is pure-C on these rows",
                  file=sys.stderr)
    return e

# A failed encode is the dangerous failure here, not a loud one. Every command
# runs with its output discarded, so a rejected flag or a missing encoder used to
# return in milliseconds, leave the previous run's file on disk, and be timed as
# a very fast encode against a stale size. The guard belongs on the PRODUCER:
# checking that two outputs match, or that a size looks plausible, cannot tell a
# real result from two empty files.
MIN_OUT = int(os.environ.get("MIN_OUT_BYTES", "128"))

class EncodeFailed(RuntimeError):
    pass

def sh(cmd, e=None, out=None):
    r = subprocess.run(cmd, env=e or env(False),
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise EncodeFailed(
            f"command failed ({r.returncode}): {' '.join(cmd[:9])} ...\n"
            f"    {r.stderr.decode(errors='replace').strip()[:400]}")
    if out is not None:
        sz = os.path.getsize(out) if os.path.exists(out) else 0
        if sz < MIN_OUT:
            raise EncodeFailed(
                f"wrote {sz} bytes to {out}, under the {MIN_OUT}-byte floor: "
                f"treat as a failed encode, not a small one.\n"
                f"    {' '.join(cmd[:9])} ...")
    return r

def probe(clip):
    """frame rate and frame count as ffmpeg sees them."""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=r_frame_rate,nb_frames", "-of", "json", f"{CORP}/{clip}.y4m"],
        capture_output=True, text=True).stdout
    st = json.loads(out)["streams"][0]
    num, den = st["r_frame_rate"].split("/")
    fps = float(num) / float(den)
    have = int(st.get("nb_frames") or 0)
    return fps, have

# ---------------------------------------------------------------------------
# Timing, following scripts/perf-comp.sh rather than inventing a second method.
#
# A CIF cell at 12 threads is one scheduling episode: the first version of this
# harness printed 0.07-0.14 s rows and a ratio built on them is noise. So each
# SAMPLE is k back-to-back executions divided by k, with k picked by a single
# untimed calibration run so a sample lasts at least REPEAT_FLOOR seconds. The
# 720p cells calibrate to k=1 and are unaffected. The calibration run doubles as
# a cache warmup.
#
# The two arms are also interleaved run by run with a mirrored order, so a
# minute of box drift lands on both rather than on whichever went first.

def calibrate(fn):
    t0 = time.perf_counter()
    fn()                                    # untimed: calibration + warmup
    cal = time.perf_counter() - t0
    return max(1, min(10, math.ceil(FLOOR / max(cal, 1e-6))))

# Wall AND cpu, because a wall ratio alone cannot tell "slower" from "doing more
# work" -- and on 2026-08-26 that distinction was the whole finding: at the auto
# thread budget we occupy MORE cores than x264 on bbb_720p and still lose, because
# we spend 1.5x the CPU. A scaling story was written and then withdrawn for want
# of this column. RUSAGE_CHILDREN accumulates over reaped children, so deltas
# around the runs are the per-cell cost; every encode here is a subprocess.
def sample(fn, k):
    c0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.perf_counter()
    for _ in range(k):
        fn()
    wall = (time.perf_counter() - t0) / k
    c1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = ((c1.ru_utime - c0.ru_utime) + (c1.ru_stime - c0.ru_stime)) / k
    return wall, cpu

def median(v):
    v = sorted(v)
    return v[len(v)//2] if len(v) % 2 else (v[len(v)//2-1] + v[len(v)//2]) / 2

def med2(pairs):
    """median wall and median cpu, taken independently over the samples."""
    return median([w for w, _ in pairs]), median([c for _, c in pairs])

def spread_warn(name, clip, ts, k):
    ts = [t for t, _ in ts] if ts and isinstance(ts[0], tuple) else ts
    if len(ts) > 1 and min(ts) > 0 and max(ts) / min(ts) > 1.15:
        print(f"    [warn] {clip} {name}: wall spread {max(ts)/min(ts):.2f}x over "
              f"{len(ts)} samples ({min(ts):.3f}-{max(ts):.3f}s, repeat={k}) -- "
              f"box may be loaded; treat this row as unreliable", flush=True)

# ---------------------------------------------------------------------------
# CRF at a matched operating point (scripts/perf-comp-crf-set.sh).
#
# "CRF 25 vs CRF 25" is two different operating points -- the two scales are
# tens of percent apart in achieved size -- so a speed ratio taken there times
# two encoders doing different amounts of work. Instead each encoder is solved
# onto a common ACHIEVED bitrate: yah264 first, onto the clip's target, then
# x264 onto whatever yah264 actually landed on.
#
# yah264's rate factor rounds to an integer frame QP, so its reachable rates
# are a staircase and the solve lands on a rung rather than the target exactly.
# That is why x264 is solved onto yah264's achieved rate and not onto the
# target: the pair has to match each other, not the nominal number.

SOLVE_CACHE = f"{WD}/solve.json"

def _cache():
    try:
        with open(SOLVE_CACHE) as f:
            return json.load(f)
    except Exception:
        return {}

def _cache_put(key, val):
    c = _cache(); c[key] = val
    with open(SOLVE_CACHE, "w") as f:
        json.dump(c, f)

def kbps_of(size, frames, fps):
    return size * 8 / (frames / fps) / 1000

# The solve is run ONCE per clip and shared by all three boards, because the
# achieved size at a given CRF barely moves with the configuration: it is
# byte-identical between the SIMD and pure-C builds (both encoders), and 0.25%
# (x264) to 0.43% (yah264) across thread counts. So it runs in the fastest
# configuration available -- SIMD, 12 threads -- and the resulting CRF pair is
# reused by the pure-C and single-threaded boards. Doing it per configuration
# meant bisecting 500 frames of 720p on a single pure-C thread, hours of it, to
# land within half a percent of the same answer.
SOLVE_X264LIB = os.environ.get("SOLVE_X264LIB", X264LIB)
# Solve AT the configuration being measured. The old default solved once at 12
# threads and shared the answer with every board, on the recorded assumption that
# achieved size moves 0.25-0.43% across thread counts. That assumption FAILS:
# bbb_720p moves 5.3% between t8 and auto, and at --threads 1 yah264 switches
# into single-thread quality mode (stq), which is a different encoder. A shared
# solve there leaves the arms rate-MISMATCHED and the ratio is then partly an
# operating-point artifact -- one row read +5.2% dSIZE before this change and
# +0.3% after. Cost is contained by seeding the bisect (see solve()).
SOLVE_THREADS = os.environ.get("SOLVE_THREADS", THREADS)

def solve_env():
    e = dict(os.environ, DYLD_LIBRARY_PATH=f"{SOLVE_X264LIB}/lib:{Y264LIB}/lib")
    e.pop("YAH264_NO_ASM", None)
    return e

# Rate-match tolerance, and it is a CORRECTNESS parameter, not a speed one.
# A GOAL leg is decided at +/-0.01, and a 1% bit difference is worth roughly 1%
# of wall, so a 1.5% tolerance can be larger than the margin being claimed. It
# was: at 1.5% this board read G3 median 0.98-1.00x, and the same board at 0.4%
# read 1.01x on two runs. The difference was x264 overshooting the rate on the
# loose rows and being timed doing more work for it (foreman dSIZE -1.1% ->
# -0.1%). 0.5% keeps every row inside +/-0.3% achieved, which is comfortably
# under the margin the goals turn on. Cost is a few more bisect steps, which
# the seeding above mostly absorbs.
SOLVE_TOL = float(os.environ.get("Y264_SOLVE_TOL", "0.005"))

def solve(codec, clip, frames, fps, target, tol=None, iters=18):
    tol = SOLVE_TOL if tol is None else tol
    """Bisect CRF until achieved bitrate is within tol of target, AT SOLVE_THREADS."""
    key = f"{codec}:{clip}:{frames}:{target:.1f}:{PRESET}:t{SOLVE_THREADS}:x{tol}"
    c = _cache()
    if key in c:
        return tuple(c[key])
    # Seed from any solve already done for this clip/target at another thread
    # count. Thread count moves the achieved rate by a few percent, not by
    # octaves, so a +/-2.5 CRF bracket around the known answer holds in practice
    # and turns a 14-step bisect into ~4. If it does not hold -- the bracket
    # converges to an edge without meeting tol -- the full range is re-run, so
    # seeding can only cost time, never correctness.
    seed = None
    pre = f"{codec}:{clip}:{frames}:{target:.1f}:{PRESET}:t"
    for k, v in c.items():
        if k.startswith(pre):
            seed = v[0]; break
    e = solve_env()
    out = f"{WD}/solve.264"
    for lo, hi in ([(seed - 2.5, seed + 2.5), (8.0, 45.0)] if seed
                   else [(8.0, 45.0)]):
        lo, hi = max(8.0, lo), min(45.0, hi)
        best = _bisect(codec, clip, frames, fps, target, tol, iters, lo, hi, e, out)
        if best and abs(best[1] - target) / target < tol:
            break
    _cache_put(key, list(best))
    return best

def _bisect(codec, clip, frames, fps, target, tol, iters, lo, hi, e, out):
    best = None
    for _ in range(iters):
        mid = (lo + hi) / 2
        sh([FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
            "-frames:v", str(frames), "-c:v", codec, "-preset", PRESET,
            "-crf", f"{mid:.3f}", "-threads", SOLVE_THREADS, "-f", "h264", out], e, out)
        k = kbps_of(os.path.getsize(out), frames, fps)
        if best is None or abs(k - target) < abs(best[1] - target):
            best = (mid, k)
        if abs(k - target) / target < tol:
            best = (mid, k)
            break
        if k > target:
            lo = mid                     # too many bits -> raise CRF
        else:
            hi = mid
    return best

def measure_crf(clip, frames, fps, target):
    ncrf, nk = solve(ENC, clip, frames, fps, target)
    xcrf, xk = solve("libx264",    clip, frames, fps, nk)

    base_cmd = [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
                "-frames:v", str(frames), "-f", "null", "-"]
    def enc_cmd(codec, crf, out):
        return [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
                "-frames:v", str(frames), "-c:v", codec, "-preset", PRESET,
                "-crf", f"{crf:.3f}", "-threads", THREADS, "-f", "h264", out]

    fb = lambda: sh(base_cmd)
    fn = lambda: sh(enc_cmd(ENC, ncrf, f"{WD}/n.264"), env(True),  f"{WD}/n.264")
    fx = lambda: sh(enc_cmd("libx264",    xcrf, f"{WD}/x.264"), env(False), f"{WD}/x.264")
    kb = calibrate(fb)
    if ORDER_X_FIRST: kx = calibrate(fx); kn = calibrate(fn)
    else:             kn = calibrate(fn); kx = calibrate(fx)
    bs, ns, xs = [], [], []
    for i in range(RUNS):
        bs.append(sample(fb, kb))
        if (i % 2 == 0) != ORDER_X_FIRST:
            ns.append(sample(fn, kn)); xs.append(sample(fx, kx))
        else:
            xs.append(sample(fx, kx)); ns.append(sample(fn, kn))
    spread_warn(ENC,    clip, ns, kn)
    spread_warn("x264", clip, xs, kx)
    bw, bc = med2(bs); nw, nc = med2(ns); xw, xc = med2(xs)
    return (nw - bw, xw - bw, nc - bc, xc - bc,
            os.path.getsize(f"{WD}/n.264"), os.path.getsize(f"{WD}/x.264"),
            ncrf, xcrf, nk)

def solve_abr(codec, clip, frames, fps, target, tol=None, iters=18):
    """Bisect the ABR TARGET until the ACHIEVED bitrate is within tol of target.

    RC=abr hands both encoders the same target and times them there, which is
    the mode as a user meets it but is NOT a speed measurement: the two land on
    different achieved rates (dsize runs about 2.9% on the standing board
    against a 1.0% bar), and emitting more bits costs time, so the ratio partly
    reports which encoder spent less. RC=abrm removes that term the same way
    RC=crf does, by putting both encoders on the same achieved bitrate.

    Achieved rate is monotone in the requested one, so the same bisection the
    CRF path uses works here with the target as the variable."""
    tol = SOLVE_TOL if tol is None else tol
    key = f"abr:{codec}:{clip}:{frames}:{target:.1f}:{PRESET}:t{SOLVE_THREADS}:x{tol}"
    c = _cache()
    if key in c:
        return tuple(c[key])
    e = solve_env()
    out = f"{WD}/solve.264"
    lo, hi = target * 0.35, target * 3.0
    best = None
    for _ in range(iters):
        mid = (lo + hi) / 2
        sh([FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
            "-frames:v", str(frames), "-c:v", codec, "-preset", PRESET,
            "-b:v", f"{mid:.0f}k", "-threads", SOLVE_THREADS, "-f", "h264", out], e, out)
        k = kbps_of(os.path.getsize(out), frames, fps)
        if best is None or abs(k - target) < abs(best[1] - target):
            best = (mid, k)
        if abs(k - target) / target < tol:
            best = (mid, k)
            break
        if k > target:
            hi = mid                     # too many bits -> ask for fewer
        else:
            lo = mid
    _cache_put(key, list(best))
    return best


def measure_abrm(clip, frames, fps, kbps):
    """ABR at a MATCHED ACHIEVED bitrate.

    yah264 runs at the clip's own target, which is what ABR means for it, and
    x264's target is then solved so that x264 lands on whatever yah264 actually
    achieved. The asymmetry mirrors measure_crf and for the same reason: the
    pair has to match each other, not the nominal number.

    What survives after the bit term is removed is a real ABR speed reading, and
    it need not equal the CRF one -- ABR runs a rate-control feedback loop that
    CRF does not."""
    e = solve_env()
    out = f"{WD}/solve.264"
    sh([FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
        "-frames:v", str(frames), "-c:v", ENC, "-preset", PRESET,
        "-b:v", f"{kbps}k", "-threads", SOLVE_THREADS, "-f", "h264", out], e, out)
    nk = kbps_of(os.path.getsize(out), frames, fps)
    xb, xk = solve_abr("libx264", clip, frames, fps, nk)

    base_cmd = [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
                "-frames:v", str(frames), "-f", "null", "-"]
    def enc_cmd(codec, br, out):
        return [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
                "-frames:v", str(frames), "-c:v", codec, "-preset", PRESET,
                "-b:v", f"{br:.0f}k", "-threads", THREADS, "-f", "h264", out]

    fb = lambda: sh(base_cmd)
    fn = lambda: sh(enc_cmd(ENC,       float(kbps), f"{WD}/n.264"), env(True),  f"{WD}/n.264")
    fx = lambda: sh(enc_cmd("libx264", xb,          f"{WD}/x.264"), env(False), f"{WD}/x.264")
    kb = calibrate(fb)
    if ORDER_X_FIRST: kx = calibrate(fx); kn = calibrate(fn)
    else:             kn = calibrate(fn); kx = calibrate(fx)
    bs, ns, xs = [], [], []
    for i in range(RUNS):
        bs.append(sample(fb, kb))
        if (i % 2 == 0) != ORDER_X_FIRST:
            ns.append(sample(fn, kn)); xs.append(sample(fx, kx))
        else:
            xs.append(sample(fx, kx)); ns.append(sample(fn, kn))
    spread_warn(ENC,    clip, ns, kn)
    spread_warn("x264", clip, xs, kx)
    bw, bc = med2(bs); nw, nc = med2(ns); xw, xc = med2(xs)
    return (nw - bw, xw - bw, nc - bc, xc - bc,
            os.path.getsize(f"{WD}/n.264"), os.path.getsize(f"{WD}/x.264"),
            float(kbps), xb, nk)


def measure(clip, frames, kbps):
    """Baseline and both encodes, interleaved. Returns (n_secs, x_secs, n_size,
    x_size), the encode times already net of the decode-only baseline.

    The baseline is startup plus demux plus decode with no encoder attached.
    Both arms pay it, so subtracting it stops it flattering whichever encoder is
    faster. It is small -- 0.009s for 180 CIF frames, 0.012s for 120 of 720p --
    but it has to be checked rather than assumed, because it is an input to
    every row. This build is --disable-everything, and `-f null -` needs the
    wrapped_avframe encoder to consume the decoded frames; without it the
    command exits 8 in 5ms. A runner that ignored exit codes subtracted those
    5ms of failure from both arms as if it were the decode cost, which quietly
    pulled every ratio toward parity. preflight() proves it runs first."""
    base_cmd = [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
                "-frames:v", str(frames), "-f", "null", "-"]
    def enc_cmd(codec, out):
        c = [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
             "-frames:v", str(frames), "-c:v", codec]
        if codec != "libopenh264":          # openh264 has no preset ladder
            c += ["-preset", PRESET]
        return c + ["-b:v", f"{kbps}k", "-threads", THREADS, "-f", "h264", out]

    fb = lambda: sh(base_cmd)
    fn = lambda: sh(enc_cmd(ENC,       f"{WD}/n.264"), env(True),  f"{WD}/n.264")
    fx = lambda: sh(enc_cmd("libx264", f"{WD}/x.264"), env(False), f"{WD}/x.264")

    kb, kn, kx = calibrate(fb), calibrate(fn), calibrate(fx)
    bs, ns, xs = [], [], []
    for i in range(RUNS):
        bs.append(sample(fb, kb))
        if (i % 2 == 0) != ORDER_X_FIRST:
            ns.append(sample(fn, kn)); xs.append(sample(fx, kx))
        else:
            xs.append(sample(fx, kx)); ns.append(sample(fn, kn))
    spread_warn(ENC,    clip, ns, kn)
    spread_warn("x264", clip, xs, kx)
    bw, bc = med2(bs); nw, nc = med2(ns); xw, xc = med2(xs)
    return (nw - bw, xw - bw, nc - bc, xc - bc,
            os.path.getsize(f"{WD}/n.264"), os.path.getsize(f"{WD}/x.264"))

def vmaf_neg(clip, frames, bitstream):
    """Decode and score against the same frames of the source. VMAF-NEG because
    that is what the quality gate uses."""
    ref, dec = f"{WD}/ref.y4m", f"{WD}/dec.y4m"
    if not os.path.exists(ref):
        sh(["ffmpeg", "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
            "-frames:v", str(frames), "-pix_fmt", "yuv420p", ref])
    sh(["ffmpeg", "-v", "error", "-y", "-i", bitstream, "-pix_fmt", "yuv420p", dec])
    j = f"{WD}/v.json"
    sh([VMAF, "-r", ref, "-d", dec, "--json", "-o", j,
        "--model", "version=vmaf_v0.6.1neg:name=neg"])
    try:
        with open(j) as f:
            return json.load(f)["pooled_metrics"]["neg"]["mean"]
    except Exception:
        return None

def thread_honoured_note():
    """Not run automatically, recorded because it is the check nobody thinks to
    make. `-threads` is handed to ffmpeg, not to the encoder, and a wrapper is
    free to ignore it: ffmpeg's stock libsvtav1 does, which reads as a large
    plumbing win until someone looks at user/real. Verified 2026-08-26 that both
    encoders here honour it -- libyah264 and libx264 both read user/real 0.99 at
    -threads 1, so the single-threaded rows are genuinely serial on both sides.
    Re-run it with /usr/bin/time -p after any wrapper change:

        libyah264  -threads 1   user/real 0.99      -threads 12  10.89
        libx264     -threads 1   user/real 0.99      -threads 12   6.61

    The 12-thread pair is worth keeping in view for its own sake: we occupy 65%
    more cores and burn 2.0x the CPU to finish 1.24x slower in wall time, which
    is goal 3's gap stated as a mechanism rather than a ratio."""

def preflight():
    missing = [n for n, v in (("X264LIB", X264LIB), ("Y264LIB", Y264LIB)) if not v]
    if missing:
        sys.exit(f"ffboard: set {' and '.join(missing)} to the library install prefix(es)")
    if not os.path.exists(FF):
        sys.exit(f"ffboard: no ffmpeg at {FF} -- set FF (see docs/ffmpeg-integration-plan.md)")
    if RC not in ("crf", "abr", "abrm"):
        sys.exit(f"ffboard: RC must be crf, abr or abrm, got '{RC}'")
    # The baseline is a measurement input, so prove it runs before trusting any
    # row that subtracts it.
    probe_clip = next((c for c, _ in CLIPS
                       if os.path.exists(os.path.join(CORP, c + ".y4m"))), None)
    if probe_clip:
        try:
            sh([FF, "-v", "error", "-y", "-i", os.path.join(CORP, probe_clip + ".y4m"),
                "-frames:v", "2", "-f", "null", "-"])
        except EncodeFailed as e:
            sys.exit("ffboard: the decode-only baseline does not run, so every row "
                     f"would subtract a failure.\n  {e}\n  Rebuild ffmpeg with "
                     "--enable-encoder=wrapped_avframe.")
    if ENC == "libopenh264" and RC == "crf":
        sys.exit("ffboard: openh264 exposes no quality knob through ffmpeg, so it "
                 "cannot be solved onto a matched point. Run it at RC=abr, and "
                 "board every row of that table the same way.")

def main():
    preflight()
    tier = 'pure-C' if NOASM else 'SIMD'
    label = f"{ENC} vs libx264, {tier}, {THREADS} thread{'' if THREADS=='1' else 's'}"
    print(f"  {label}   rc={RC}  window={SECONDS:g}s  preset={PRESET}  "
          f"median of {RUNS} samples, {FLOOR:g}s repeat floor, "
          f"{'x264' if ORDER_X_FIRST else ENC.replace('lib','')} first")
    enc_col = ENC.replace("lib", "")[:6]
    if RC == "crf":
        print(f"  {'clip':<16}{'kbps':>8}{'n crf':>7}{'x crf':>7}{enc_col+' s':>9}"
              f"{'x264 s':>9}{'x264 x':>9}{'work':>7}{'cores':>12}{'dVMAF':>8}{'dsize':>8}")
        print("  " + "-" * 100)
    elif RC == "abrm":
        # "kbps" is the ACHIEVED rate both encoders were put on; the two target
        # columns are what each had to be ASKED for to land there. dsize is the
        # proof the match worked and should sit near zero -- if it does not, the
        # solve did not converge and the row is not a speed reading.
        print(f"  {'clip':<16}{'kbps':>8}{'n targ':>7}{'x targ':>7}{enc_col+' s':>9}"
              f"{'x264 s':>9}{'x264 x':>9}{'work':>7}{'cores':>12}{'dVMAF':>8}{'dsize':>8}")
        print("  " + "-" * 100)
    else:
        print(f"  {'clip':<16}{'kbps':>7}{enc_col+' s':>9}{'x264 s':>9}{'x264 x':>9}"
              f"{'work':>7}{'cores':>12}{'dVMAF':>8}{'dsize':>8}{'n rate':>8}{'x rate':>8}")
        print("  " + "-" * 100)
    ratios, dvs, dss, works = [], [], [], []
    for clip, kbps in CLIPS:
        path = f"{CORP}/{clip}.y4m"
        if not os.path.exists(path):
            print(f"  {clip:<16}{'(missing)':>7}")
            continue
        fps, have = probe(clip)
        frames = int(round(SECONDS * fps))
        if have:
            frames = min(frames, have)
        for f in (f"{WD}/ref.y4m",):
            if os.path.exists(f):
                os.remove(f)
        if RC == "crf":
            nt, xt, ncpu, xcpu, nsz, xsz, ncrf, xcrf, nk = \
                measure_crf(clip, frames, fps, kbps)
        elif RC == "abrm":
            nt, xt, ncpu, xcpu, nsz, xsz, ncrf, xcrf, nk = \
                measure_abrm(clip, frames, fps, kbps)
        else:
            nt, xt, ncpu, xcpu, nsz, xsz = measure(clip, frames, kbps)
        nv = vmaf_neg(clip, frames, f"{WD}/n.264")
        xv = vmaf_neg(clip, frames, f"{WD}/x.264")
        r  = nt / xt
        dv = (nv - xv) if (nv is not None and xv is not None) else float("nan")
        ds = 100.0 * (nsz / xsz - 1)
        # Rate error against the target, both sides, because dsize alone reads
        # as an efficiency result when it is often just one encoder missing the
        # target. x264's ABR undershoots high-motion CIF badly enough that a
        # +10% dsize row is x264 spending less, not yah264 spending more.
        # work = the CPU-seconds ratio, cores = occupancy each side achieved.
        # Read them together: a wall ratio near 1.00 built on work 1.5x means we
        # are only level because we are occupying more of the machine, and it
        # will regress the moment the reference threads better.
        w  = (ncpu / xcpu) if xcpu > 0 else float("nan")
        nco = ncpu / nt if nt > 0 else float("nan")
        xco = xcpu / xt if xt > 0 else float("nan")
        cores = f"{nco:.1f}/{xco:.1f}"
        ratios.append(r); dvs.append(dv); dss.append(ds); works.append(w)
        if RC in ("crf", "abrm"):
            # crf prints rate factors to 2dp; abrm prints kbit/s targets, where
            # a decimal would be noise.
            fmt = "{:>7.0f}" if RC == "abrm" else "{:>7.2f}"
            print(f"  {clip:<16}{nk:>8.0f}{fmt.format(ncrf)}{fmt.format(xcrf)}"
                  f"{nt:>9.2f}{xt:>9.2f}{r:>8.2f}x{w:>6.2f}x{cores:>12}"
                  f"{dv:>+8.2f}{ds:>+7.1f}%", flush=True)
        else:
            secs = frames / fps
            nre = 100.0 * ((nsz * 8 / secs / 1000) / kbps - 1)
            xre = 100.0 * ((xsz * 8 / secs / 1000) / kbps - 1)
            print(f"  {clip:<16}{kbps:>7}{nt:>9.2f}{xt:>9.2f}{r:>8.2f}x"
                  f"{w:>6.2f}x{cores:>12}{dv:>+8.2f}{ds:>+7.1f}%"
                  f"{nre:>+7.1f}%{xre:>+7.1f}%", flush=True)
    if ratios:
        print("  " + "-" * 100)
        pad = f"{'':>8}{'':>7}{'':>7}{'':>9}{'':>9}" if RC == "crf" \
              else f"{'':>7}{'':>9}{'':>9}"
        print(f"  {'MEDIAN':<16}{pad}{median(ratios):>8.2f}x"
              f"{median(works):>6.2f}x{'':>12}{median(dvs):>+8.2f}"
              f"{median(dss):>+7.1f}%")
        print(f"  {'MAX':<16}{pad}{max(ratios):>8.2f}x{max(works):>6.2f}x")

main()
