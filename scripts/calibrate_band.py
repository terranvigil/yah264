#!/usr/bin/env python3
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""Find, per clip, the ABR ladder whose x264-medium operating points land inside
the VMAF-NEG 88-94 band.  Fixed multipliers off a 'calibrated centre' do NOT do
this -- they put akiyo/touchdown at saturation and sintel below the band, which
makes the BD fit degenerate (touchdown read +341269%).

Method: sweep x264 CRF, measure (bitrate, VMAF-NEG) for each, then interpolate
the bitrates at four VMAF-NEG targets spanning the band.  The ladder is defined
by the REFERENCE encoder's own curve, which is what 'calibrated band' means.
"""
import os, sys, json, tempfile, subprocess, shlex, math
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import bdcompare as bd

bd.SUBSAMPLE = 1
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, "tests/corpus")
FRAMES = 120
TARGETS = [88.0, 90.0, 92.0, 94.0]

CLIPS = ["foreman_cif", "bus_cif", "stefan_cif", "akiyo_cif", "mobile_cif",
         "coastguard_cif", "tempete_cif",
         "ducks_720p", "park_joy_720p", "samsung_720p", "sintel_720p",
         "touchdown_420"]

CRFS = [16, 20, 24, 28, 32, 36, 40]


def probe(clip, crf, work):
    src = os.path.join(CORPUS, clip + ".y4m")
    out = os.path.join(work, f"{clip}.{crf}.264")
    cmd = (f'x264 --preset medium --crf {crf} --frames {FRAMES} --threads 1 '
           f'--demuxer y4m -o "{out}" "{src}"')
    r = bd.sh(cmd)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    bits = os.path.getsize(out) * 8
    # kbps at the clip's real framerate, read from the y4m header
    fps = read_fps(src)
    kbps = bits / (FRAMES / fps) / 1000.0
    v = bd.vmaf_of(out, src, work)
    os.unlink(out)
    if not v or "VMAF-NEG" not in v:
        return None
    return (crf, kbps, v["VMAF-NEG"])


_FPS = {}


def read_fps(src):
    if src in _FPS:
        return _FPS[src]
    with open(src, "rb") as f:
        hdr = f.readline().decode("ascii", "replace")
    fps = 30.0
    for tok in hdr.split():
        if tok.startswith("F"):
            try:
                n, d = tok[1:].split(":")
                fps = float(n) / float(d)
            except Exception:
                pass
    _FPS[src] = fps
    return fps


def interp(pts, target):
    """pts: sorted list of (kbps, vmaf) ascending in vmaf. Log-linear in rate."""
    for i in range(len(pts) - 1):
        (r0, v0), (r1, v1) = pts[i], pts[i + 1]
        if v0 <= target <= v1:
            if v1 == v0:
                return r0
            t = (target - v0) / (v1 - v0)
            return math.exp(math.log(r0) + t * (math.log(r1) - math.log(r0)))
    return None


def main():
    work = tempfile.mkdtemp(prefix="calib.")
    results = {}
    jobs = []
    with ThreadPoolExecutor(max_workers=8) as ex:
        for clip in CLIPS:
            for crf in CRFS:
                jobs.append((clip, crf, ex.submit(probe, clip, crf, work)))
        for clip, crf, fut in jobs:
            r = fut.result()
            if r:
                results.setdefault(clip, []).append(r)

    ladders = {}
    print("=== x264 medium curve (crf -> kbps / VMAF-NEG) ===")
    for clip in CLIPS:
        pts = sorted(results.get(clip, []), key=lambda x: x[1])
        if not pts:
            print(f"{clip}: NO DATA")
            continue
        print(f"{clip}: " + "  ".join(f"crf{c}:{k:.0f}k/{v:.2f}" for c, k, v in pts))
        curve = [(k, v) for c, k, v in pts]
        lad = []
        for t in TARGETS:
            r = interp(curve, t)
            if r:
                lad.append(int(round(r)))
        # need all four in band, else the clip cannot be measured in this band
        if len(lad) == len(TARGETS) and len(set(lad)) == len(TARGETS):
            ladders[clip] = lad
        else:
            lo = min(v for _, v in curve); hi = max(v for _, v in curve)
            print(f"    !! cannot span {TARGETS[0]}-{TARGETS[-1]}: "
                  f"x264 VMAF-NEG range is {lo:.2f}-{hi:.2f}")
    print()
    print("=== band-calibrated ABR ladders ===")
    for c, l in ladders.items():
        print(f"{c}: {','.join(str(x) for x in l)}")
    here = os.path.dirname(os.path.abspath(__file__))
    json.dump(ladders, open(os.path.join(here, "ladders.json"), "w"), indent=1)
    # the reference curve itself, so run_band.py stops carrying a hand-pasted
    # copy that goes stale the moment the clip set changes
    curves = {c: [[k, v] for _, k, v in sorted(results.get(c, []), key=lambda x: x[1])]
              for c in CLIPS if results.get(c)}
    json.dump(curves, open(os.path.join(here, "curves.json"), "w"), indent=1)


main()
