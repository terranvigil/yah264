#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""The band ladders' DEEP extension: per clip, the ABR ladder whose x264-medium
points land at VMAF-NEG 55/65/75/83 -- the deep-quant regime below the standing
band (88-94), where this campaign's regime-shaped arms (the trellis-lambda
ramp, the psy gates) do their work and where the standing gate was blind: the
crossover of a regime arm sat in the unsampled 29-32 CRF hole between
`band_at_rate.py` and `bd.sh`, and two arms had to be bisected by hand to find
it (docs/hf-mechanism-portfolio.md).

Method is calibrate_band.py's, over the deep CRF range. Writes
ladders_deep.json / curves_deep.json; `BANDS=deep` or `BANDS=all` in
band_at_rate.py consumes them. Clips whose x264 curve cannot reach a target
before CRF 51 keep the rungs that solved (>= 3 or the clip is dropped) --
unlike the band, partial coverage down here is better than none, and the
per-rung columns say which rungs exist.
"""
import os, sys, json, tempfile, math
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import bdcompare as bd

bd.SUBSAMPLE = 1
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, "tests/corpus")
FRAMES = 120
TARGETS = [55.0, 65.0, 75.0, 83.0]

CLIPS = ["foreman_cif", "bus_cif", "stefan_cif", "akiyo_cif", "mobile_cif",
         "coastguard_cif", "tempete_cif",
         "ducks_720p", "park_joy_720p", "fourpeople_720p", "sintel_720p",
         "touchdown_420"]

CRFS = [30, 34, 38, 42, 46, 50]

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


def probe(clip, crf, work):
    src = os.path.join(CORPUS, clip + ".y4m")
    out = os.path.join(work, f"{clip}.{crf}.264")
    cmd = (f'x264 --preset medium --crf {crf} --frames {FRAMES} --threads 1 '
           f'--demuxer y4m -o "{out}" "{src}"')
    r = bd.sh(cmd)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    bits = os.path.getsize(out) * 8
    fps = read_fps(src)
    kbps = bits / (FRAMES / fps) / 1000.0
    v = bd.vmaf_of(out, src, work)
    os.unlink(out)
    if not v or "VMAF-NEG" not in v:
        return None
    return (crf, kbps, v["VMAF-NEG"])


def interp(pts, target):
    for i in range(len(pts) - 1):
        (r0, v0), (r1, v1) = pts[i], pts[i + 1]
        if v0 <= target <= v1:
            if v1 == v0:
                return r0
            t = (target - v0) / (v1 - v0)
            return math.exp(math.log(r0) + t * (math.log(r1) - math.log(r0)))
    return None


def main():
    work = tempfile.mkdtemp(prefix="calibdeep.")
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
    print("=== x264 medium deep curve (crf -> kbps / VMAF-NEG) ===")
    for clip in CLIPS:
        pts = sorted(results.get(clip, []), key=lambda x: x[1])
        if not pts:
            print(f"{clip}: NO DATA")
            continue
        print(f"{clip}: " + "  ".join(f"crf{c}:{k:.0f}k/{v:.2f}" for c, k, v in pts))
        curve = [(k, v) for c, k, v in pts]
        lad = [r for t in TARGETS if (r := interp(curve, t))]
        lad = [int(round(r)) for r in lad]
        if len(lad) >= 3 and len(set(lad)) == len(lad):
            ladders[clip] = lad
        else:
            lo = min(v for _, v in curve)
            hi = max(v for _, v in curve)
            print(f"    !! only {len(set(lad))} deep rungs solvable: "
                  f"x264 range is {lo:.2f}-{hi:.2f}")
    print()
    print("=== deep-calibrated ABR ladders ===")
    for c, l in ladders.items():
        print(f"{c}: {','.join(str(x) for x in l)}")
    here = os.path.dirname(os.path.abspath(__file__))
    json.dump(ladders, open(os.path.join(here, "ladders_deep.json"), "w"), indent=1)
    curves = {c: [[k, v] for _, k, v in sorted(results.get(c, []), key=lambda x: x[1])]
              for c in CLIPS if results.get(c)}
    json.dump(curves, open(os.path.join(here, "curves_deep.json"), "w"), indent=1)


main()
