#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""Rate-anchored BD table for the B direct mode, three arms against x264 medium.

Every arm is solved onto the SAME achieved byte targets, so no arm's number
depends on where a CRF point happens to land. The targets come from the
calibrated operating points in docs/corpus-sources.md rather than from a CRF
number, which is what makes this table quotable beside the published one:
scaling a documented kbps by the clip's own duration is reproducible, and a
shared --points set is not (it puts clips of different difficulty in different
bands, and four of the six 1080p clips landed outside the published band that
way on 2026-08-31).

x264 is solved once per clip and reused across arms.

  python3 scripts/direct_rate_table.py                     # all six 1080p clips
  python3 scripts/direct_rate_table.py --clips blue_sky_1080p --repeat 2
"""
import argparse, os, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bdcompare as bd
import importlib.util
spec = importlib.util.spec_from_file_location(
    "bdr", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bd_at_rate.py"))

bd.SUBSAMPLE = 1

# clip -> (calibrated kbps, fps). docs/corpus-sources.md, 6-second windows.
CALIB = {
    "blue_sky_1080p":   (1500, 25),
    "sunflower_1080p":  (1500, 25),
    "station2_1080p":   (2000, 25),
    "pedestrian_1080p": (2800, 25),
    "riverbed_1080p":   (12500, 25),
    "crowd_run_1080p":  (22000, 50),
}
# Ladder around the calibrated point. 0.4-1.15 reproduces the span the
# published blue_sky ladder covered (VMAF-NEG 48 to 93).
MULT = (0.4, 0.55, 0.8, 1.15)

Y = "./build/cli/yah264"
# DRT_EXTRA is appended to EVERY arm including x264, so both sides stay
# configured the same way. DRT_ARMS picks a subset by name. Together they let
# this harness answer a base-path question ("--bframes 0 on both sides, at the
# calibrated operating point") without a second copy of the solve.
EXTRA = os.environ.get("DRT_EXTRA", "")
ONLY  = [a for a in os.environ.get("DRT_ARMS", "").split(",") if a]
ARMS = [
    ("spatial",  f"{Y} --input-y4m {{src}} --crf {{q}} --frames {{n}} --threads 8 -o {{out}}"),
    ("temporal", f"Y264_DIRECT_PERMB=1 {Y} --input-y4m {{src}} --crf {{q}} --frames {{n}} "
                 f"--threads 8 --direct temporal -o {{out}}"),
]
X264 = "x264 --crf {q} --preset medium --keyint 250 --demuxer y4m --frames {n} -o {out} {src}"
if EXTRA:
    ARMS = [(n, t.replace(" -o {out}", f" {EXTRA} -o {{out}}")) for n, t in ARMS]
    X264 = X264.replace(" -o {out}", " " + EXTRA + " -o {out}")
if ONLY:
    ARMS = [(n, t) for n, t in ARMS if n in ONLY]


def enc(tmpl, q, src, out, n):
    subprocess.run(tmpl.format(q=f"{q:.4f}", src=src, out=out, n=n),
                   shell=True, capture_output=True)
    return os.path.getsize(out) if os.path.exists(out) else 0


def solve(tmpl, src, target, out, n, iters=10):
    lo, hi = 10.0, 48.0
    for _ in range(iters):
        mid = (lo + hi) / 2.0
        if enc(tmpl, mid, src, out, n) > target:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2.0


def curve(tmpl, src, targets, work, label, n):
    pts = []
    out = os.path.join(work, f"{label}.264")
    for t in targets:
        q = solve(tmpl, src, t, out, n)
        sz = enc(tmpl, q, src, out, n)
        v = bd.vmaf_of(out, src, work)
        if not v or "VMAF-NEG" not in v:
            return None
        pts.append((sz, v["VMAF-NEG"]))
        print(f"    {label:<10} target {t:>9} -> crf {q:5.2f}  {sz:>9} B  "
              f"NEG {v['VMAF-NEG']:6.2f}", flush=True)
    return pts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clips", default=",".join(CALIB))
    ap.add_argument("--frames", type=int, default=150)
    ap.add_argument("--repeat", type=int, default=1,
                    help="draws per clip; >1 prints the spread, which is what a "
                         "large headline number has to survive")
    args = ap.parse_args()

    rows = {}
    for clip in args.clips.split(","):
        kbps, fps = CALIB[clip]
        secs = args.frames / fps
        base = kbps * 1000 / 8 * secs
        targets = [int(base * m) for m in MULT]
        src = f"tests/corpus/{clip}.y4m"
        for draw in range(args.repeat):
            work = tempfile.mkdtemp(prefix="drt.")
            tag = f"{clip}" + (f" draw{draw + 1}" if args.repeat > 1 else "")
            print(f"##### {tag}  targets {targets}", flush=True)
            ref = curve(X264, src, targets, work, "x264", args.frames)
            if not ref:
                print(f"  {tag}: x264 curve failed"); continue
            rb, mb_ = [p[0] for p in ref], [p[1] for p in ref]
            for name, tmpl in ARMS:
                c = curve(tmpl, src, targets, work, name, args.frames)
                if not c:
                    print(f"  {tag} {name}: curve failed"); continue
                r = bd.bd_rate(rb, mb_, [p[0] for p in c], [p[1] for p in c])
                if isinstance(r, float):
                    rows.setdefault((clip, name), []).append(r)
                    print(f"  {tag:<26} {name:<9} BD-rate(VMAF-NEG) vs x264: {r:+.2f}%",
                          flush=True)
                else:
                    print(f"  {tag} {name}: {r}")

    print("\n| clip | spatial | temporal | delta |")
    print("|---|--:|--:|--:|")
    for clip in args.clips.split(","):
        s = rows.get((clip, "spatial"), [])
        t = rows.get((clip, "temporal"), [])
        if not s or not t:
            continue
        fs = f"{sum(s)/len(s):+.2f}%" + (f" (spread {max(s)-min(s):.2f})" if len(s) > 1 else "")
        ft = f"{sum(t)/len(t):+.2f}%" + (f" (spread {max(t)-min(t):.2f})" if len(t) > 1 else "")
        print(f"| {clip} | {fs} | {ft} | {sum(t)/len(t) - sum(s)/len(s):+.1f} |")
    print("DRT-DONE")


main()
