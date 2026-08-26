#!/usr/bin/env python3
# Copyright (c) 2026, the next264 authors
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
# It also needs an ffmpeg built against libnext264, so it cannot run from a
# stock checkout. See docs/ffmpeg-integration-plan.md.
#
# Usage:  RC=crf THREADS=12 X264LIB=... NOASM=1 scripts/ffboard.py
#
# Env:
#   FF        ffmpeg binary built with --enable-libnext264 --enable-libx264
#   X264LIB   install prefix of the libx264 to load (asm or autovec build)
#   N264LIB   install prefix of libnext264
#   NOASM     1 = force next264's scalar path (NEXT264_NO_ASM), and select the
#             pure-C libx264 via X264LIB. Only next264 has such a switch: for
#             any other encoder under test the flag affects the REFERENCE only
#   ENC       encoder under test (default libnext264). libx264 is always the
#             reference. libopenh264 is ABR-only, see the RC note below
#   RC        crf (matched operating point, the headline) | abr (rate-matched)
#   THREADS   thread count handed to both encoders
#   CORP      clip directory (default tests/corpus)
#   RUNS, REPEAT_FLOOR, SECONDS, PRESET, VMAF
#
# THE PURE-C ARM IS A TRAP. x264's configure adds -fno-tree-vectorize
# UNCONDITIONALLY (configure ~line 1438, outside any asm test), so every stock
# x264 build has vectorization suppressed -- its C is a fallback behind hand-asm,
# not a tuned target. next264's NEXT264_NO_ASM=1 is only a runtime dispatch
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

import os, subprocess, sys, time, json, math

_ROOT       = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_FF_DEFAULT = os.path.join(os.path.dirname(_ROOT), "FFmpeg", "ffmpeg")

FF      = os.environ.get("FF", _FF_DEFAULT)
X264LIB = os.environ.get("X264LIB", "")     # install prefix, required
N264LIB = os.environ.get("N264LIB", "")     # install prefix, required
CORP    = os.environ.get("CORP", os.path.join(_ROOT, "tests", "corpus"))
SECONDS = float(os.environ.get("SECONDS", "6"))
THREADS = os.environ.get("THREADS", "12")
NOASM   = os.environ.get("NOASM", "0") == "1"
RUNS    = int(os.environ.get("RUNS", "3"))
FLOOR   = float(os.environ.get("REPEAT_FLOOR", "0.35"))
PRESET  = os.environ.get("PRESET", "medium")
RC      = os.environ.get("RC", "abr")          # abr | crf
# The encoder under test. libx264 is always the reference the ratio is taken
# against. openh264 exposes no quality knob through ffmpeg, only a bitrate, so
# it can only be boarded at RC=abr; and it has no scalar/SIMD switch of its own,
# so its "pure-C" rows mean openh264 as built against a pure-C x264.
ENC     = os.environ.get("ENC", "libnext264")
WD      = os.environ.get("WD", os.path.join(os.environ.get("TMPDIR", "/tmp"), "ffboard"))
VMAF    = os.environ.get("VMAF", "vmaf")

# clip:target-kbps, straight from scripts/parity-clips.sh
CLIPS = [("foreman_cif", 400), ("bus_cif", 400), ("stefan_cif", 400),
         ("ducks_720p", 25000), ("park_joy_720p", 12000), ("samsung_720p", 1200)]

os.makedirs(WD, exist_ok=True)

def env(under_test):
    e = dict(os.environ, DYLD_LIBRARY_PATH=f"{X264LIB}/lib:{N264LIB}/lib")
    e.pop("NEXT264_NO_ASM", None)
    if under_test and NOASM and ENC == "libnext264":
        e["NEXT264_NO_ASM"] = "1"
    return e

def sh(cmd, e=None):
    return subprocess.run(cmd, env=e or env(False),
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

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

def sample(fn, k):
    t0 = time.perf_counter()
    for _ in range(k):
        fn()
    return (time.perf_counter() - t0) / k

def median(v):
    v = sorted(v)
    return v[len(v)//2] if len(v) % 2 else (v[len(v)//2-1] + v[len(v)//2]) / 2

def spread_warn(name, clip, ts, k):
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
# onto a common ACHIEVED bitrate: next264 first, onto the clip's target, then
# x264 onto whatever next264 actually landed on.
#
# next264's rate factor rounds to an integer frame QP, so its reachable rates
# are a staircase and the solve lands on a rung rather than the target exactly.
# That is why x264 is solved onto next264's achieved rate and not onto the
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
# (x264) to 0.43% (next264) across thread counts. So it runs in the fastest
# configuration available -- SIMD, 12 threads -- and the resulting CRF pair is
# reused by the pure-C and single-threaded boards. Doing it per configuration
# meant bisecting 500 frames of 720p on a single pure-C thread, hours of it, to
# land within half a percent of the same answer.
SOLVE_X264LIB = os.environ.get("SOLVE_X264LIB", X264LIB)
SOLVE_THREADS = os.environ.get("SOLVE_THREADS", "12")

def solve_env():
    e = dict(os.environ, DYLD_LIBRARY_PATH=f"{SOLVE_X264LIB}/lib:{N264LIB}/lib")
    e.pop("NEXT264_NO_ASM", None)
    return e

def solve(codec, clip, frames, fps, target, tol=0.015, iters=14):
    """Bisect CRF until achieved bitrate is within tol of target."""
    key = f"{codec}:{clip}:{frames}:{target:.1f}:{PRESET}"
    c = _cache()
    if key in c:
        return tuple(c[key])
    e = solve_env()
    out = f"{WD}/solve.264"
    lo, hi, best = 8.0, 45.0, None
    for _ in range(iters):
        mid = (lo + hi) / 2
        sh([FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
            "-frames:v", str(frames), "-c:v", codec, "-preset", PRESET,
            "-crf", f"{mid:.3f}", "-threads", SOLVE_THREADS, "-f", "h264", out], e)
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
    _cache_put(key, list(best))
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
    fn = lambda: sh(enc_cmd(ENC, ncrf, f"{WD}/n.264"), env(True))
    fx = lambda: sh(enc_cmd("libx264",    xcrf, f"{WD}/x.264"), env(False))
    kb, kn, kx = calibrate(fb), calibrate(fn), calibrate(fx)
    bs, ns, xs = [], [], []
    for i in range(RUNS):
        bs.append(sample(fb, kb))
        if i % 2 == 0:
            ns.append(sample(fn, kn)); xs.append(sample(fx, kx))
        else:
            xs.append(sample(fx, kx)); ns.append(sample(fn, kn))
    spread_warn(ENC,    clip, ns, kn)
    spread_warn("x264", clip, xs, kx)
    base = median(bs)
    return (median(ns) - base, median(xs) - base,
            os.path.getsize(f"{WD}/n.264"), os.path.getsize(f"{WD}/x.264"),
            ncrf, xcrf, nk)

def measure(clip, frames, kbps):
    """Baseline and both encodes, interleaved. Returns (n_secs, x_secs, n_size,
    x_size), the encode times already net of the decode-only baseline."""
    base_cmd = [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
                "-frames:v", str(frames), "-f", "null", "-"]
    def enc_cmd(codec, out):
        c = [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
             "-frames:v", str(frames), "-c:v", codec]
        if codec != "libopenh264":          # openh264 has no preset ladder
            c += ["-preset", PRESET]
        return c + ["-b:v", f"{kbps}k", "-threads", THREADS, "-f", "h264", out]

    fb = lambda: sh(base_cmd)
    fn = lambda: sh(enc_cmd(ENC,       f"{WD}/n.264"), env(True))
    fx = lambda: sh(enc_cmd("libx264", f"{WD}/x.264"), env(False))

    kb, kn, kx = calibrate(fb), calibrate(fn), calibrate(fx)
    bs, ns, xs = [], [], []
    for i in range(RUNS):
        bs.append(sample(fb, kb))
        if i % 2 == 0:
            ns.append(sample(fn, kn)); xs.append(sample(fx, kx))
        else:
            xs.append(sample(fx, kx)); ns.append(sample(fn, kn))
    spread_warn(ENC,    clip, ns, kn)
    spread_warn("x264", clip, xs, kx)
    base = median(bs)
    return (median(ns) - base, median(xs) - base,
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
    encoders here honour it -- libnext264 and libx264 both read user/real 0.99 at
    -threads 1, so the single-threaded rows are genuinely serial on both sides.
    Re-run it with /usr/bin/time -p after any wrapper change:

        libnext264  -threads 1   user/real 0.99      -threads 12  10.89
        libx264     -threads 1   user/real 0.99      -threads 12   6.61

    The 12-thread pair is worth keeping in view for its own sake: we occupy 65%
    more cores and burn 2.0x the CPU to finish 1.24x slower in wall time, which
    is goal 3's gap stated as a mechanism rather than a ratio."""

def preflight():
    missing = [n for n, v in (("X264LIB", X264LIB), ("N264LIB", N264LIB)) if not v]
    if missing:
        sys.exit(f"ffboard: set {' and '.join(missing)} to the library install prefix(es)")
    if not os.path.exists(FF):
        sys.exit(f"ffboard: no ffmpeg at {FF} -- set FF (see docs/ffmpeg-integration-plan.md)")
    if RC not in ("crf", "abr"):
        sys.exit(f"ffboard: RC must be crf or abr, got '{RC}'")
    if ENC == "libopenh264" and RC == "crf":
        sys.exit("ffboard: openh264 exposes no quality knob through ffmpeg, so it "
                 "cannot be solved onto a matched point. Run it at RC=abr, and "
                 "board every row of that table the same way.")

def main():
    preflight()
    tier = 'pure-C' if NOASM else 'SIMD'
    label = f"{ENC} vs libx264, {tier}, {THREADS} thread{'' if THREADS=='1' else 's'}"
    print(f"  {label}   rc={RC}  window={SECONDS:g}s  preset={PRESET}  "
          f"median of {RUNS} samples, {FLOOR:g}s repeat floor")
    enc_col = ENC.replace("lib", "")[:6]
    if RC == "crf":
        print(f"  {'clip':<16}{'kbps':>8}{'n crf':>7}{'x crf':>7}{enc_col+' s':>9}"
              f"{'x264 s':>9}{'x264 x':>9}{'dVMAF':>8}{'dsize':>8}")
        print("  " + "-" * 82)
    else:
        print(f"  {'clip':<16}{'kbps':>7}{enc_col+' s':>9}{'x264 s':>9}{'x264 x':>9}"
              f"{'dVMAF':>8}{'dsize':>8}{'n rate':>8}{'x rate':>8}")
        print("  " + "-" * 82)
    ratios, dvs, dss = [], [], []
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
            nt, xt, nsz, xsz, ncrf, xcrf, nk = measure_crf(clip, frames, fps, kbps)
        else:
            nt, xt, nsz, xsz = measure(clip, frames, kbps)
        nv = vmaf_neg(clip, frames, f"{WD}/n.264")
        xv = vmaf_neg(clip, frames, f"{WD}/x.264")
        r  = nt / xt
        dv = (nv - xv) if (nv is not None and xv is not None) else float("nan")
        ds = 100.0 * (nsz / xsz - 1)
        # Rate error against the target, both sides, because dsize alone reads
        # as an efficiency result when it is often just one encoder missing the
        # target. x264's ABR undershoots high-motion CIF badly enough that a
        # +10% dsize row is x264 spending less, not next264 spending more.
        ratios.append(r); dvs.append(dv); dss.append(ds)
        if RC == "crf":
            print(f"  {clip:<16}{nk:>8.0f}{ncrf:>7.2f}{xcrf:>7.2f}{nt:>9.2f}"
                  f"{xt:>9.2f}{r:>8.2f}x{dv:>+8.2f}{ds:>+7.1f}%", flush=True)
        else:
            secs = frames / fps
            nre = 100.0 * ((nsz * 8 / secs / 1000) / kbps - 1)
            xre = 100.0 * ((xsz * 8 / secs / 1000) / kbps - 1)
            print(f"  {clip:<16}{kbps:>7}{nt:>9.2f}{xt:>9.2f}{r:>8.2f}x"
                  f"{dv:>+8.2f}{ds:>+7.1f}%{nre:>+7.1f}%{xre:>+7.1f}%", flush=True)
    if ratios:
        print("  " + "-" * 82)
        pad = f"{'':>8}{'':>7}{'':>7}{'':>9}{'':>9}" if RC == "crf" \
              else f"{'':>7}{'':>9}{'':>9}"
        print(f"  {'MEDIAN':<16}{pad}{median(ratios):>8.2f}x"
              f"{median(dvs):>+8.2f}{median(dss):>+7.1f}%")
        print(f"  {'MAX':<16}{pad}{max(ratios):>8.2f}x")

main()
