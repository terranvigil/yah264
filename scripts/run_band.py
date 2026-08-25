#!/usr/bin/env python3
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""Run the BD comparison on the band-calibrated ladders from calibrate_band.py.

Reports per clip, no headline mean, and flags any clip whose reference curve is
too flat in the band for a BD fit to mean anything (that is what produced the
+341269% touchdown reading on the fixed-multiplier ladder).
"""
import os, sys, json, math, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FRAMES = 120
N = "build/cli/next264"

ladders = json.load(open(os.path.join(HERE, "ladders.json")))

# CLIPS=a,b: run a subset. The two bands' gates are not the same set of clips --
# the ABR band is gated on akiyo and park_joy (the propagation-damage pair) while
# the CRF band is gated on all twelve -- and without this the cheap half of a
# gate costs the same as the expensive one.
_sel = os.environ.get("CLIPS", "")
if _sel:
    _want = [c.strip() for c in _sel.split(",") if c.strip()]
    _miss = [c for c in _want if c not in ladders]
    if _miss:
        sys.exit(f"no ladder for {_miss}; have {list(ladders)}")
    ladders = {c: ladders[c] for c in _want}

# x264's own curve, from the calibration pass, used to judge measurability:
# (kbps, vmaf-neg) at crf 40..16
CURVE = {
 "foreman_cif":   [(53,48.42),(80,63.93),(122,77.01),(192,85.91),(315,91.90),(558,95.40),(1045,97.43)],
 "bus_cif":       [(92,54.39),(145,70.93),(239,84.31),(401,94.62),(681,99.58),(1176,99.94),(2026,99.95)],
 "stefan_cif":    [(65,49.52),(103,68.26),(165,81.55),(276,90.81),(485,95.93),(858,97.83),(1538,98.92)],
 "akiyo_cif":     [(18,59.39),(25,73.44),(36,81.43),(58,87.51),(96,91.06),(170,93.34),(309,94.72)],
 "mobile_cif":    [(94,54.04),(143,68.68),(235,80.00),(432,88.15),(818,93.59),(1502,97.23),(2637,99.41)],
 "ducks_720p":    [(1090,22.20),(1891,37.12),(3380,53.98),(6155,69.57),(11211,81.81),(20294,90.28),(38141,95.77)],
 "park_joy_720p": [(822,27.12),(1653,46.47),(3404,65.01),(6693,80.05),(12751,90.92),(23447,97.67),(41681,99.85)],
 "samsung_720p":  [(285,59.23),(406,71.86),(597,81.36),(903,88.04),(1419,92.46),(2297,95.15),(3844,96.56)],
 "sintel_720p":   [(112,53.07),(170,66.44),(265,78.20),(430,86.15),(720,90.99),(1218,93.98),(2125,95.76)],
 "touchdown_420": [(486,43.77),(793,59.00),(1246,72.84),(2147,82.59),(5420,88.57),(18609,92.21),(50772,95.71)],
}

# calibrate_band.py now writes the curve it measured; prefer it over the pasted
# copy above, which only covers the original ten clips.
_cj = os.path.join(HERE, "curves.json")
if os.path.exists(_cj):
    CURVE.update({c: [tuple(p) for p in pts] for c, pts in json.load(open(_cj)).items()})

# ARM: when set, the comparison is next264-against-next264 (env-gated arm vs the
# shipped default) instead of next264-against-x264. That is the right shape for
# gating a speed change for BD-neutrality -- same ladder both sides, so the
# rates are matched by construction and no ladder-placement artifact can enter.
ARM = os.environ.get("ARM", "")
# ARM_ARGS: extra CLI flags for the arm side, for knobs that are CLI
# options rather than env vars (--aq-strength, --psy-rd, ...).
ARM_ARGS = os.environ.get("ARM_ARGS", "")


def band_slope(clip):
    """VMAF-NEG points gained per doubling of rate, inside the ladder span.
    A BD number needs a reference curve that actually moves here."""
    lad = ladders[clip]
    lo, hi = lad[0], lad[-1]
    c = CURVE[clip]
    def v_at(r):
        for i in range(len(c) - 1):
            if c[i][0] <= r <= c[i + 1][0]:
                (r0, v0), (r1, v1) = c[i], c[i + 1]
                t = (math.log(r) - math.log(r0)) / (math.log(r1) - math.log(r0))
                return v0 + t * (v1 - v0)
        return None
    vlo, vhi = v_at(lo), v_at(hi)
    if vlo is None or vhi is None:
        return None
    return (vhi - vlo) / math.log2(hi / lo)


def crf_ladder(clip):
    """CRFs whose x264 VMAF-NEG lands on the same four band targets."""
    crfs = [40, 36, 32, 28, 24, 20, 16]
    c = CURVE[clip]
    out = []
    for t in (88.0, 90.0, 92.0, 94.0):
        got = None
        for i in range(len(c) - 1):
            if c[i][1] <= t <= c[i + 1][1]:
                f = (t - c[i][1]) / (c[i + 1][1] - c[i][1])
                got = crfs[i] + f * (crfs[i + 1] - crfs[i])
                break
        if got is not None:
            out.append(int(round(got)))
    out = sorted(set(out))
    return out if len(out) >= 3 else None


def run(clip, points, mode):
    src_arg = "--bitrate {q}" if mode == "abr" else "--crf {q}"
    x_arg = "--bitrate {q}" if mode == "abr" else "--crf {q}"
    ours = (f'{N} --input-y4m {{src}} --frames {FRAMES} --preset medium --cabac '
            f'--transform-8x8 {src_arg} --threads 1 -o {{out}}')
    a = (ARM + " " + ours) if ARM else ours
    if ARM_ARGS:
        a = a.replace(' -o {out}', ' ' + ARM_ARGS + ' -o {out}')
    xcmd = (f'x264 --preset medium {x_arg} --frames {FRAMES} '
            f'--threads 1 --demuxer y4m -o {{out}} {{src}}')
    # ARM_VS_X264=1: score the ARM against X264 instead of against our default.
    # Without it an arm can only be measured self-A/B, which answers "is this
    # better than us" and never "how much of the gap to them does it close" --
    # and for a clip we are BEHIND on, the second question is the one that
    # decides whether the arm is the answer or a rounding error.
    b = xcmd if (not ARM or os.environ.get("ARM_VS_X264") == "1") else ours
    cmd = ["python3", "scripts/bdcompare.py", "--vmaf", "--no-cache",
           "--subsample", "1", "--clips", clip, "--frames", str(FRAMES),
           "--jobs", "8", "--points", ",".join(str(x) for x in points),
           "--a", a, "--b", b]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    out = [l for l in (r.stdout + r.stderr).splitlines() if l.startswith(clip)]
    return out + per_rung(clip, out)


def per_rung(clip, lines):
    """PER-RUNG paired deltas, printed beside the BD number.

    BD-rate is the mean horizontal gap between two RD curves, integrated with
    UNIFORM weight over the quality interval. Two things it therefore does not
    say, and both matter when quoting a result to anyone who ships a ladder:

      - it weights every operating point equally, and nobody's viewers are
        distributed that way -- the number that matters is the gap at the rungs
        actually shipped, which may be nothing like the average;
      - it converts the whole difference into RATE at matched quality, but a
        real ladder ships fixed rungs, so at each rung the difference arrives
        as a MIX of a little rate and a little quality. BD scores the rate half
        and drops the quality half.

    So print, per rung, both halves: what the arm did to the rate and what it
    did to the score. A reader can then see which half this particular arm is
    paying out in, instead of inferring a bandwidth saving that only exists if
    the ladder is re-cut. (The board's dVMAF leg is the project's other half of
    this; these columns are the same idea per operating point.)

    READ THEM ONLY WHERE THE TWO SIDES SIT AT COMPARABLE RATES -- i.e. a
    self-A/B, where both sides run the same ladder on the same encoder. Against
    x264 the two CRF scales are different operating points (41 points of size
    spread, docs/archive/crf-x264-scale.md), so these columns are dominated by that
    offset and NOT by an efficiency difference: bus reads +42-45% rate and
    +2.4-4.5 VMAF against x264 at every rung, which is the scale mismatch, and
    the BD number is the one that normalises it away. The columns and the BD
    number answer different questions and each is misleading in the other's
    setting."""
    def parse(tag):
        for l in lines:
            if f" {tag}: " in l:
                return [p for p in l.split(f" {tag}: ", 1)[1].split() if "/" in p]
        return []
    a, b = parse("a"), parse("b")
    if not a or len(a) != len(b):
        return []
    rows = []
    for i, (pa, pb) in enumerate(zip(a, b)):
        try:
            ra, va = pa.split("/"); rb, vb = pb.split("/")
            ra = float(ra.rstrip("k")); rb = float(rb.rstrip("k"))
            dv = float(va) - float(vb)
            dr = 100.0 * (ra - rb) / rb if rb else 0.0
        except ValueError:
            return []
        rows.append(f"{clip:<14} rung{i}: dRate {dr:+6.2f}%  dVMAF {dv:+6.3f}")
    return rows


print("clip                slope/oct  ladder")
for c in ladders:
    s = band_slope(c)
    print(f"{c:<18} {s:>6.2f}    {ladders[c]}")
print()

# BANDS=crf|abr runs one band instead of both. The standing rule gates quality
# arms on the CRF band (the ABR band's per-clip noise floor spans 0.2 to 11
# points), so a gating round only needs that half and pays half the wall.
_bands = os.environ.get("BANDS", "abr,crf").split(",")
for mode in [m for m in ("abr", "crf") if m in _bands]:
    print(f"############ {mode.upper()} on the band-calibrated ladder ############")
    for c in ladders:
        pts = ladders[c] if mode == "abr" else crf_ladder(c)
        if not pts:
            print(f"{c}: no in-band CRF ladder"); continue
        s = band_slope(c)
        flag = "  <-- reference curve too flat here, BD not meaningful" if (s is not None and s < 2.0) else ""
        for line in run(c, pts, mode):
            print(line + (flag if "BD-rate(VMAF-NEG)" in line else ""))
    print()
