#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""M4 training-label harness: per-sequence arm labels + lookahead features.

WHAT IT ANSWERS. The M4 selector needs, for each training sequence, (a) which
of a small set of arms is best there and by how much, and (b) the cheap
input-derived features a runtime selector could actually see. This produces one
JSONL row per sequence carrying both, so the fit and the feature study run
offline against a file rather than against the encoder.

TRAINING CORPUS ONLY. It takes external source files (BVI-AOM), never the
12-clip gate corpus -- that corpus is TEST-ONLY, permanently, and a threshold
fitted on it would be fitted on its own test set. The gate-corpus read is a
separate one-shot validation, run once, at the end.

MATCHED ACHIEVED RATE, NOT MATCHED CRF. Both arms here move the CRF-to-rate
mapping, so a matched-CRF bdcompare would measure ladder placement instead of
the encoder (that error invalidated the whole 08-22/25 M4 table; see
scripts/bd_at_rate.py's header). The baseline's three CRF points define three
BYTE targets, and each arm is binary-searched onto those same byte targets.

  python3 scripts/m4_label.py --src-dir /Volumes/.../bvi-aom/272p \
      --limit 32 --stride 7 --out local/records/m4-bviaom.jsonl

Rows already present in --out are skipped, so the run is resumable.
"""
import argparse, json, os, re, statistics, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bdcompare as bd

bd.SUBSAMPLE = 1
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build/cli/yah264")

# The arms the re-derived ceiling table says carry the per-content split.
# Baseline is the shipped default path, and it is also the abstain choice: an
# arm only counts as the label when it beats the default by more than NOISE.
ARMS = {
    "crf_cplx0":   {"Y264_CRF_CPLX": "0"},
    "mbtree_str2": {"Y264_MBTREE_STRENGTH": "2.0"},
}
NOISE = 0.30    # BD-% below which a win is not a win (the M4 table's threshold)

ENCARGS = ["--preset", "medium", "--cabac", "--bframes", "3", "--ref", "3",
           "--transform-8x8", "--threads", "12"]
PSY = re.compile(r"psyflat: poc=(\d+) type=(\d+) share=(-?\d+) tex=(-?\d+) tdiff=(-?\d+)")
ADM = re.compile(r"adme: poc=(\d+) type=(\d+) score=(-?\d+)")


def encode(src, crf, out, env=None, frames=None):
    cmd = [BIN, "--input-y4m", src, "--crf", f"{crf:.4f}", *ENCARGS, "-o", out]
    if frames:
        cmd[4:4] = ["--frames", str(frames)]
    e = dict(os.environ, **(env or {}))
    p = subprocess.run(cmd, capture_output=True, text=True, env=e)
    return (os.path.getsize(out) if os.path.exists(out) else 0), p.stderr


def solve(src, target, out, env, lo=8.0, hi=51.0, iters=10):
    """CRF landing nearest `target` bytes. Higher CRF = fewer bits (monotone)."""
    for _ in range(iters):
        mid = (lo + hi) / 2.0
        if encode(src, mid, out, env)[0] > target:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2.0


def features(src, work):
    """The frame-level, input-derived signals the encoder already computes:
    flat-MB share, textured share, per-pixel temporal difference (x256, EWMA)
    and the lowres motion score. Medians over the sequence."""
    out = os.path.join(work, "f.264")
    _, err = encode(src, 30.0, out, {"Y264_PSY_FLAT_LOG": "1", "Y264_ADME_LOG": "1"})
    flat, tex, tdiff, motion = [], [], [], []
    for ln in err.splitlines():
        m = PSY.search(ln)
        if m:
            flat.append(int(m.group(3))); tex.append(int(m.group(4)))
            tdiff.append(int(m.group(5)))
        m = ADM.search(ln)
        if m:
            motion.append(int(m.group(3)))
    med = lambda v: float(statistics.median(v)) if v else None
    return dict(flat=med(flat), tex=med(tex), tdiff=med(tdiff), motion=med(motion))


def decode_to_y4m(srcfile, dst):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", srcfile,
                    "-pix_fmt", "yuv420p", dst], capture_output=True)
    return os.path.exists(dst) and os.path.getsize(dst) > 0


def one(srcfile, work, points):
    y4m = os.path.join(work, "src.y4m")
    if not decode_to_y4m(srcfile, y4m):
        return {"error": "decode failed"}
    row = {"seq": os.path.basename(srcfile)}
    row.update(features(y4m, work))

    out = os.path.join(work, "e.264")
    base_pts, targets = [], []
    for crf in points:                      # baseline curve IS the target set
        sz, _ = encode(y4m, float(crf), out)
        v = bd.vmaf_of(out, y4m, work)
        if not sz or not v or "VMAF-NEG" not in v:
            return {"seq": row["seq"], "error": f"baseline crf {crf} failed"}
        base_pts.append((sz, v["VMAF-NEG"]))
        targets.append(sz)
    row["base_bytes"] = targets
    row["base_neg"] = [p[1] for p in base_pts]

    for name, env in ARMS.items():
        pts = []
        for t in targets:
            q = solve(y4m, t, out, env)
            sz, _ = encode(y4m, q, out, env)
            v = bd.vmaf_of(out, y4m, work)
            if not sz or not v or "VMAF-NEG" not in v:
                pts = None
                break
            pts.append((sz, v["VMAF-NEG"]))
        if pts is None:
            row[name] = None
            continue
        row[name] = bd.bd_rate([p[0] for p in base_pts], [p[1] for p in base_pts],
                               [p[0] for p in pts], [p[1] for p in pts])
    best, bestv = "default", -NOISE
    for name in ARMS:
        v = row.get(name)
        if v is not None and v < bestv:
            best, bestv = name, v
    row["label"] = best
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--stride", type=int, default=1,
                    help="take every Nth sequence in sorted order -- content "
                         "diversity without hand-picking")
    ap.add_argument("--points", default="24,30,36",
                    help="baseline CRF points; their achieved bytes become the "
                         "matched-rate targets every arm is solved onto")
    a = ap.parse_args()

    done = set()
    if os.path.exists(a.out):
        for ln in open(a.out):
            try:
                done.add(json.loads(ln)["seq"])
            except Exception:
                pass
    files = sorted(f for f in os.listdir(a.src_dir)
                   if f.endswith((".mp4", ".mkv", ".y4m")))
    files = files[::a.stride]
    if a.limit:
        files = files[:a.limit]
    points = [float(x) for x in a.points.split(",")]

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    for i, f in enumerate(files):
        if f in done:
            continue
        work = tempfile.mkdtemp()
        try:
            row = one(os.path.join(a.src_dir, f), work, points)
        finally:
            subprocess.run(["rm", "-rf", work])
        with open(a.out, "a") as fh:
            fh.write(json.dumps(row) + "\n")
        print(f"[{i + 1}/{len(files)}] {f[:44]:44s} "
              f"tdiff={row.get('tdiff')} cplx0={row.get('crf_cplx0')} "
              f"str2={row.get('mbtree_str2')} -> {row.get('label', row.get('error'))}",
              flush=True)


if __name__ == "__main__":
    main()
