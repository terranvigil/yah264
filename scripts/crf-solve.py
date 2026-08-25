#!/usr/bin/env python3
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""crf-solve.py -- find the CRF each encoder needs to land on a COMMON operating
point, so a CRF speed comparison is taken at the same place on both RD curves.

WHY THIS EXISTS. next264's CRF N and x264's CRF N are not the same operating
point and never were. Measured equal-CRF size divergence vs x264 on this tree is
-54.6%..+45.3% shipped, and -29.7%..+11.4% even with N264_CRF_CPLX=1 -- still 41
points of spread (docs/archive/crf-x264-scale.md). So "CRF 25 vs CRF 25" times two
encoders doing different amounts of work, and any speed ratio from it is a
content-luck number. It is the same trap perf-comp.sh's 5%-size guard was added
to catch.

The fix is not to align the two scales. It is to stop needing them aligned:
sweep CRF on each encoder separately, interpolate to a common ACHIEVED bitrate
(or VMAF), and compare speed there. That holds the operating point fixed by
measurement instead of by assuming two integers mean the same thing.

WHAT IS INTERPOLATED, AND WHY THAT WAY ROUND. The obvious reading of "sweep and
interpolate" is: time a ladder of CRF points on each side, then interpolate the
measured SECONDS to the target rate. Don't. Seconds are the noisy quantity here
(the box's own repeatability bound is ~1.15x, which is why perf-comp.sh medians
and warns), and interpolating across a bracket two CRF steps wide multiplies
that noise by the bracket width. Bitrate is the exact quantity -- it is a byte
count.

So this script interpolates the EXACT axis and lets the caller measure the noisy
one directly: it sweeps CRF, interpolates log(rate) vs CRF to solve for the CRF
that hits the target, and lands there. perf-comp.sh then times both encoders AT
the solved point, medianed, with no interpolation on the timing axis at all.
Same operating point, one less noise multiplier.

Interpolation is in log(rate)-vs-CRF because that relation is close to a
straight line over the couple of CRF units a solve moves through (~11-13% rate
per CRF unit), and a secant on a near-linear function converges in 2-3 encodes.
A polynomial fit over a wide ladder buys nothing and rings near saturation.

WHAT THE CALLER GETS. A line of `key=value` pairs, including n264_crf and
x264_crf. Feed those to perf-comp.sh as N264_CRF / X264_CRF with RC_MODE=crf:
    eval "$(scripts/crf-solve.py --clip ... --target-kbps 400)"
    RC_MODE=crf N264_CRF=$n264_crf X264_CRF=$x264_crf scripts/perf-comp.sh ...
Because both sides were solved to the same achieved rate, the run satisfies
perf-comp.sh's 5%-size guard BY CONSTRUCTION -- the guard stays armed and
meaningful rather than being suppressed, and the dVMAF it prints becomes a real
quality reading instead of an operating-point artifact.

CALIBRATION REUSE. Solving is done with each encoder's fastest build so the
solve is cheap, and that is exact for next264 and near-exact for x264. Measured
on this tree (park_joy_720p, 300 frames, CRF 25, preset medium):
  - next264 is bit-identical across {SIMD, no-asm} x {1, 18} threads -- all four
    md5s equal. One solve serves all three parity goals exactly.
  - x264 is NOT: all four md5s differ. The spread is small in the only respect
    that matters here -- size moves 0.023% (asm->noasm, 1t) and 0.125% (1t->18t,
    asm) -- but it is not zero, and perf-comp.sh's header claim that both
    encoders' asm is "bit-exact with their scalar C, so VMAF/size are identical
    either way" is false for x264. Auto-vectorising x264's float ratecontrol
    reassociates it, and mb-tree decisions move.
    So: x264 is solved at the TIER'S thread count (which carries the 0.125%
    term) using the asm binary (which carries the 0.023% one). The residual is
    ~0.02% of rate, four orders below the 5% guard and two below the solve
    tolerance, and it is bounded rather than assumed.

Usage:
  crf-solve.py --clip tests/corpus/foreman_cif.y4m --seconds 6 --target-kbps 400
  crf-solve.py --clip ... --target-vmaf 92          # match quality instead
Env: NEXT264, X264_ASM, VMAF, N264_CRF_CACHE=0 to bypass the solve cache.
"""
import argparse
import hashlib
import json
import math
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Rate response to CRF, used only as the FIRST secant step before any second
# point exists. ~11.5% of rate per CRF unit is the H.264 rule of thumb; it only
# has to be the right order of magnitude, since every step after this one uses
# the measured slope. Deliberately not tuned per clip -- a seed that pretends to
# be a measurement is how a constant gets quoted as a result later.
SEED_SLOPE = -0.115          # d log(rate) / d CRF
CRF_MIN, CRF_MAX = 1.0, 50.0

# THE GRIDS ARE NOT THE SAME, AND THAT DECIDES THE WHOLE DESIGN.
#
# next264's CLI parses --crf into tenths (cli/next264_cli.c: crf10), so it LOOKS
# continuous. It is not. rc_set_qp_crf (src/encoder/encoder.c) ends in
#     e->qp = (int)lround(qp);
# -- the rate factor collapses to an INTEGER frame QP. Fractional CRF changes
# nothing except when it happens to cross a rounding boundary, so the set of
# achievable bitrates is a discrete ladder one QP step apart. Measured on
# foreman_cif: CRF 21.8/21.9/22.0 all give 416 kbit/s, CRF 22.1/22.3 both give
# 362 -- a 15% jump across a 0.1 CRF change, with a plateau either side.
#
# Two consequences, both load-bearing:
#   1. Secant-searching next264 on a 0.1 grid is searching a staircase. The
#      first version of this script did exactly that and burned 7 encodes
#      oscillating between two plateaus without converging, while x264 landed in
#      3. Searching on the 1.0 grid instead moves exactly one QP per step and
#      converges monotonically.
#   2. next264 CANNOT be solved to an arbitrary bitrate in CRF mode at all. The
#      worst-case miss is half a step, ~6-7% of rate, which is OUTSIDE
#      perf-comp.sh's 5% size guard. Forcing a round-number target would
#      therefore trip that guard on roughly half of all clips -- through no
#      fault of the measurement.
#
# So the common point is not a round number we impose. It is chosen from the set
# next264 can actually reach, and x264 -- whose CRF really is continuous, ~1.4%
# of rate per 0.1 -- is then solved ONTO it. That satisfies the 5% guard by
# construction rather than by suppressing it, and it needs no scale alignment,
# which was the point of moving to a matched operating point in the first place.
GRID = {"next264": 1.0, "x264": 0.1}


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def clip_fps(path):
    """Frame rate READ OFF THE CLIP. Never hardcoded: this corpus mixes 30, 30000/1001,
    50 and 25 fps, and three genuine hardcoded-fps bugs were found in scripts/ in
    one week. tests/corpus/uneven_720p.y4m is additionally mislabeled on disk."""
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                        "-show_entries", "stream=r_frame_rate", "-of", "csv=p=0", path],
                       capture_output=True, text=True)
    tok = r.stdout.strip()
    if not tok:
        sys.exit(f"crf-solve: could not read frame rate from {path}")
    n, _, d = tok.partition("/")
    return float(n) / float(d or 1)


def trim(clip, seconds, work):
    """Trim to a whole number of frames -- byte-for-byte the same reference
    perf-comp.sh builds, so the rate this script solves for is the rate that run
    will achieve. Keep the two formulas identical if either changes."""
    fps = clip_fps(clip)
    n = int(round(fps * seconds))
    ref = os.path.join(work, "ref.y4m")
    sh(f'ffmpeg -v error -y -i {shlex.quote(clip)} -frames:v {n} -pix_fmt yuv420p {shlex.quote(ref)}')
    if not os.path.exists(ref):
        sys.exit(f"crf-solve: failed to trim {clip}")
    r = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-select_streams", "v:0",
                        "-show_entries", "stream=nb_read_frames", "-of", "csv=p=0", ref],
                       capture_output=True, text=True)
    return ref, int(r.stdout.strip()), fps


def vmaf_of(ref, bitstream, work, subsample):
    dec = os.path.join(work, "dec.y4m")
    js = os.path.join(work, "v.json")
    sh(f'ffmpeg -v error -y -i {shlex.quote(bitstream)} -pix_fmt yuv420p {shlex.quote(dec)}')
    binary = os.environ.get("VMAF", "vmaf")
    r = sh(f'{shlex.quote(binary)} -r {shlex.quote(ref)} -d {shlex.quote(dec)} '
           f'--subsample {subsample} --model version=vmaf_v0.6.1:name=vmaf '
           f'--json -o {shlex.quote(js)}')
    if r.returncode != 0:
        sys.exit(f"crf-solve: vmaf failed: {r.stderr[-400:]}")
    return json.load(open(js))["pooled_metrics"]["vmaf"]["mean"]


class Encoder:
    """One side of the comparison: how to run it, and the sweep it has done."""

    def __init__(self, name, binary, template, threads):
        self.name, self.binary, self.template, self.threads = name, binary, template, threads
        self.points = []        # (crf, kbps, vmaf|None), in visit order

    def encode(self, ref, crf, work, tag):
        out = os.path.join(work, f"{self.name}_{tag}.264")
        cmd = self.template.format(src=shlex.quote(ref), crf=f"{crf:.1f}",
                                   out=shlex.quote(out), threads=self.threads)
        r = sh(cmd)
        if r.returncode != 0 or not os.path.exists(out):
            sys.exit(f"crf-solve: {self.name} encode failed at crf {crf}:\n{r.stderr[-600:]}")
        return out


def solve(enc, ref, nframes, fps, target, mode, work, tol, max_iters, subsample, seed):
    """Sweep CRF on this encoder's grid, interpolate to the target, return the
    closest point actually MEASURED as (crf, kbps, vmaf).

    Secant on log(rate) (or on VMAF) vs CRF. Every iteration is one encode and
    contributes a real point to enc.points, so the ladder that gets printed IS
    the sweep -- there is no separate throwaway probe phase to disbelieve.
    """
    grid = GRID[enc.name]

    def snap(c):
        return max(CRF_MIN, min(CRF_MAX, round(c / grid) * grid))

    def measure(crf, tag):
        for c, k, v in enc.points:               # never pay for a repeat point
            if abs(c - crf) < grid / 2:
                return k, v
        out = enc.encode(ref, crf, work, tag)
        kbps = os.path.getsize(out) * 8 / 1000.0 / (nframes / fps)
        v = vmaf_of(ref, out, work, subsample) if mode == "vmaf" else None
        enc.points.append((crf, kbps, v))
        return kbps, v

    def value(kbps, v):
        return math.log(kbps) if mode == "kbps" else v

    def hit(kbps, v):
        return abs(kbps / target - 1.0) <= tol if mode == "kbps" else abs(v - target) <= tol

    goal = math.log(target) if mode == "kbps" else target

    crf = snap(seed)
    kbps, v = measure(crf, "s0")
    prev_crf, prev_val = crf, value(kbps, v)

    for it in range(max_iters):
        cur_val = value(kbps, v)
        if hit(kbps, v):
            break
        # Slope from the two most recent DISTINCT points; seed slope until then.
        if it == 0 or abs(crf - prev_crf) < 1e-9 or abs(cur_val - prev_val) < 1e-12:
            slope = SEED_SLOPE if mode == "kbps" else -(SEED_SLOPE * 40.0)
        else:
            slope = (cur_val - prev_val) / (crf - prev_crf)
        if abs(slope) < 1e-9:
            break
        step = (goal - cur_val) / slope
        # Cap the step. An uncapped secant on a flat (saturated) stretch throws
        # the next point at a clamp and then oscillates between the two rails
        # instead of converging; capping turns that into a slow walk, which the
        # saturation check downstream can diagnose honestly.
        step = max(-8.0, min(8.0, step))
        nxt = snap(crf + step)
        if abs(nxt - crf) < grid / 2:
            # Already on the finest step this encoder has. For next264 that is
            # not a failure to converge, it is the staircase: the target lies
            # BETWEEN two reachable rates and no CRF exists that hits it. Take
            # the neighbouring step too, so the caller can see both rungs and
            # pick the closer one rather than believing the search stalled.
            nxt = snap(crf + (grid if step > 0 else -grid))
            if abs(nxt - crf) < grid / 2:
                break
        prev_crf, prev_val = crf, cur_val
        crf = nxt
        kbps, v = measure(crf, f"s{it+1}")

    # Report the best point actually MEASURED, never the interpolated root: the
    # caller re-encodes at this CRF, so a CRF that was never run is a CRF whose
    # achieved rate is a guess. On next264's staircase the root is usually on a
    # plateau anyway, where it means nothing.
    if mode == "kbps":
        return min(enc.points, key=lambda p: abs(math.log(p[1] / target)))
    return min(enc.points, key=lambda p: abs(p[2] - target))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clip", required=True)
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--target-kbps", type=float)
    ap.add_argument("--target-vmaf", type=float)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--preset", default="medium")
    # The caller passes the SAME arg strings it will hand perf-comp.sh. Not a
    # convenience: a solve run under different flags than the measurement lands
    # on a rate the measurement does not reproduce, and nothing downstream would
    # notice, because both runs individually look fine.
    ap.add_argument("--n264-args", default=None)
    ap.add_argument("--x264-args", default=None)
    ap.add_argument("--tol", type=float, default=0.02,
                    help="convergence tolerance: fraction of rate (kbps mode) "
                         "or absolute VMAF points (vmaf mode)")
    ap.add_argument("--max-iters", type=int, default=6)
    # 1 = score every frame. A stride aliases against the mini-GOP frame-type
    # cadence and is worth up to 2.5 VMAF between two encodes that are actually
    # identical in PSNR -- see scripts/perf-comp.sh's VMAF_FPS comment. In
    # --target-vmaf mode that error would move the operating point itself.
    ap.add_argument("--subsample", type=int, default=1)
    ap.add_argument("--seed-crf", type=float, default=25.0)
    ap.add_argument("--no-cache", dest="cache", action="store_false")
    args = ap.parse_args()

    if (args.target_kbps is None) == (args.target_vmaf is None):
        sys.exit("crf-solve: pass exactly one of --target-kbps / --target-vmaf")
    mode = "kbps" if args.target_kbps is not None else "vmaf"
    target = args.target_kbps if mode == "kbps" else args.target_vmaf
    # VMAF tolerance is in points, not a fraction; 0.02 VMAF is below the
    # metric's own subsampled noise, so give that mode a sane default.
    tol = args.tol if mode == "kbps" else (args.tol if args.tol > 0.05 else 0.3)

    next264 = os.environ.get("NEXT264", os.path.join(ROOT, "build", "cli", "next264"))
    # Solve with the ASM x264 regardless of the tier being timed: see the header
    # -- it costs ~0.023% of rate and is several times faster to converge with.
    x264 = os.environ.get("X264_ASM", os.path.join(ROOT, "..", "x264", "x264-asm"))
    for b in (next264, x264):
        if not os.path.exists(b):
            sys.exit(f"crf-solve: binary not found: {b}")

    clip = args.clip if os.path.isabs(args.clip) else os.path.join(ROOT, args.clip)
    if not os.path.exists(clip):
        sys.exit(f"crf-solve: clip not found: {clip}")

    # THE PIN, and why it exists (2026-08-18).
    #
    # The common point is whatever rung next264 lands on, and the rungs are ~13%
    # of rate apart. Re-deriving it per BUILD makes two boards incomparable: on
    # 2026-08-18 a build and a two-week-older one with BYTE-IDENTICAL output on
    # all six board clips read dVMAF -0.56 and -0.49, because foreman and bus
    # solved onto different rungs (411 vs 409 kbit/s, 379 vs 375). Four of the
    # six clips read identically. The goals are scored to 0.01 against a 0.5
    # bar, so a 0.07 swing from rung choice alone is larger than every margin
    # the board is asked to defend.
    #
    # So the OPERATING POINT is pinned per (clip, args, target, tier) and NOT
    # per binary: the first solve establishes it, later builds are solved onto
    # the same point instead of re-choosing one. The solve stays a real solve --
    # if a build's rate curve genuinely moves, it lands on the nearest rung to
    # the pin and `pin_drift_pct` says so, which is a finding rather than a
    # silent shift. N264_CRF_PIN=0 bypasses; delete tests/.crfpin to re-derive.
    pin_target, pin_state = target, "none"
    if os.environ.get("N264_CRF_PIN", "1") == "1":
        ph = hashlib.md5()
        ph.update("|".join(str(x) for x in [
            os.path.basename(clip), args.seconds, mode, target, args.threads,
            args.preset, tol, args.seed_crf, args.subsample,
            args.n264_args, args.x264_args]).encode())
        pinfile = os.path.join(ROOT, "tests", ".crfpin", ph.hexdigest())
        if os.path.exists(pinfile):
            try:
                pin_target = float(open(pinfile).read().strip())
                pin_state = "hit"
            except ValueError:
                pin_state = "none"
    else:
        pinfile = None

    # Cache the solved CRFs. The solve is a pure function of the binaries, clip,
    # frame count, encoder args, thread count and target -- none of which is the
    # box's load state, so unlike a TIMING cache this one cannot go stale in a
    # way that poisons a number. It caches WHERE to measure, not what was
    # measured. N264_CRF_CACHE=0 bypasses.
    key = None
    if args.cache and os.environ.get("N264_CRF_CACHE", "1") == "1":
        h = hashlib.md5()
        for b in (next264, x264):
            h.update(open(b, "rb").read())
        h.update("|".join(str(x) for x in [
            os.path.basename(clip), args.seconds, mode, target, args.threads,
            args.preset, tol, args.seed_crf, args.subsample,
            args.n264_args, args.x264_args, pin_target]).encode())
        key = os.path.join(ROOT, "tests", ".crfcache", h.hexdigest())
        if os.path.exists(key):
            sys.stdout.write(open(key).read())
            print("crf_cached=1")
            return

    work = tempfile.mkdtemp(prefix="crfsolve_")
    ref, nframes, fps = trim(clip, args.seconds, work)

    # The encoder command lines MUST match what the caller will measure with, or
    # the solve lands on a rate the measured run does not reproduce.
    n_args = args.n264_args if args.n264_args is not None else \
        f"--preset {args.preset} --cabac --transform-8x8"
    x_args = args.x264_args if args.x264_args is not None else f"--preset {args.preset}"
    n_tmpl = (f"{shlex.quote(next264)} --input-y4m {{src}} --crf {{crf}} {n_args} "
              f"--threads {{threads}} -o {{out}}")
    x_tmpl = (f"{shlex.quote(x264)} --crf {{crf}} {x_args} "
              f"--threads {{threads}} -o {{out}} {{src}}")

    encs = [Encoder("next264", next264, n_tmpl, args.threads),
            Encoder("x264", x264, x_tmpl, args.threads)]
    res = {}

    # ORDER MATTERS, and it is the whole trick.
    #
    # next264 goes FIRST, and is solved towards the nominal target on its own
    # coarse (integer-QP) grid. Whatever rung it lands on becomes THE COMMON
    # POINT. x264 goes second and is solved onto that measured rate, not onto
    # the nominal target -- its CRF is continuous, so it can land wherever it is
    # asked to.
    #
    # Doing it the other way round, or forcing both to the round number, would
    # leave the two sides up to half a next264 QP step apart (~6-7% of rate) and
    # trip perf-comp.sh's 5% guard on a run that is not actually wrong. Solving
    # the continuous encoder onto the discrete one's reachable set is what makes
    # the match tight by construction. The nominal target still chooses WHICH
    # rung -- it sets the operating point, it just does not have to be hit
    # exactly.
    n_crf, n_kbps, n_vmaf = solve(encs[0], ref, nframes, fps, pin_target, mode, work,
                                  tol, args.max_iters, args.subsample, args.seed_crf)
    res["next264"] = (n_crf, n_kbps, n_vmaf)
    common = n_kbps if mode == "kbps" else n_vmaf
    if pinfile and pin_state != "hit":
        os.makedirs(os.path.dirname(pinfile), exist_ok=True)
        with open(pinfile, "w") as f:
            f.write(f"{common:.6f}\n")
        pin_state = "set"
    x_crf, x_kbps, x_vmaf = solve(encs[1], ref, nframes, fps, common, mode, work,
                                  tol, args.max_iters, args.subsample, args.seed_crf)
    res["x264"] = (x_crf, x_kbps, x_vmaf)

    lines = []
    lines.append(f"clip={os.path.basename(clip)}")
    lines.append(f"frames={nframes}")
    lines.append(f"fps={fps:.3f}")
    lines.append(f"point_mode={mode}")
    lines.append(f"nominal_target={target:g}")
    # The point both encoders were actually placed at, as opposed to the one
    # that was asked for. Quote THIS one.
    lines.append(f"common_point={common:.3f}")
    # Pin state, so a board can never quote a number without saying which point
    # it was taken at. `pin_drift_pct` is how far this build's chosen rung sits
    # from the pinned point: 0 means the operating point reproduced exactly,
    # anything else means the rate curve moved and the comparison shifted with
    # it. Read it before reading dVMAF.
    lines.append(f"pin_state={pin_state}")
    if pin_state == "hit":
        lines.append(f"pin_target={pin_target:.3f}")
        lines.append(f"pin_drift_pct={100.0 * (common - pin_target) / pin_target:+.2f}")
    for e in encs:
        crf, kbps, v = res[e.name]
        tag = "n264" if e.name == "next264" else "x264"
        lines.append(f"{tag}_crf={crf:.1f}")
        lines.append(f"{tag}_kbps={kbps:.1f}")
        if v is not None:
            lines.append(f"{tag}_vmaf={v:.3f}")
        lines.append(f"{tag}_sweep={'/'.join(f'{c:.1f}:{k:.0f}' for c, k, _ in e.points)}")
        lines.append(f"{tag}_encodes={len(e.points)}")

    # TWO DIFFERENT ERRORS, and only one of them is a defect.
    #
    #   drift_pct -- how far next264's chosen rung sits from the NOMINAL target.
    #     Expected to be non-zero and up to half a QP step. It moves the
    #     operating point slightly; it does not make the comparison unfair,
    #     because both encoders move with it. Informational.
    #   match_pct -- how far apart the TWO ENCODERS ended up. This is the one
    #     that matters: it is the comparison's validity, and it is what
    #     perf-comp.sh's 5% size guard measures downstream.
    #
    # Conflating these is exactly the confusion that produced the "undershooting
    # the bitrate target" reading on a command that had no target in it.
    n_k, x_k = res["next264"][1], res["x264"][1]
    if mode == "kbps":
        drift = n_k / target - 1.0
        match = x_k / n_k - 1.0
    else:
        drift = res["next264"][2] - target
        match = res["x264"][2] - res["next264"][2]
    lines.append("drift_pct={:+.2f}".format(drift * 100 if mode == "kbps" else drift))
    lines.append("match_pct={:+.2f}".format(match * 100 if mode == "kbps" else match))
    # The number that decides whether perf-comp.sh's 5% guard passes. Always the
    # rate spread, because that guard compares BYTES whichever point mode we ran.
    lines.append("size_spread_pct={:+.2f}".format(n_k / x_k * 100 - 100))

    # A run is usable when the two encoders MATCHED, regardless of where the
    # rung fell. Threshold is the guard's own 5%, minus a margin, so a solve
    # this script calls good cannot then trip the guard downstream.
    match_limit = 0.04 if mode == "kbps" else max(tol * 2, 1.0)
    ok = abs(match) <= match_limit
    lines.append("solved={}".format(1 if ok else 0))
    if not ok:
        lines.append("solve_warn=encoders_not_matched")
    # Saturation is reported independently of success: a solve that converged
    # only because it ran into a CRF rail is one whose operating point was
    # chosen by the clamp, and near the rails the rate curve flattens, so the
    # next run may land somewhere quite different for no code reason.
    for e in encs:
        crfs = [p[0] for p in e.points]
        if crfs and (min(crfs) <= CRF_MIN + 1e-9 or max(crfs) >= CRF_MAX - 1e-9):
            lines.append(f"{'n264' if e.name == 'next264' else 'x264'}_saturated=1")

    body = "".join(l + "\n" for l in lines)
    sys.stdout.write(body)
    if key:
        os.makedirs(os.path.dirname(key), exist_ok=True)
        with open(key, "w") as f:
            f.write(body)
    print("crf_cached=0")


if __name__ == "__main__":
    main()
