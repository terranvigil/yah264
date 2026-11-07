#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""CRF ladders on a 10-bit HDR10 (PQ, BT.2020, limited range) Y4M, yah264
against x264 High 10, scored with VMAF v1's HDR model and PSNR on the 10-bit
code values, then BD-rate over the rungs both ladders cover.

  scripts/hdr_ladder.py SRC.y4m [--crf 18,22,26,30,34] [--max-crf 30]
                        [--threads 3] [--work DIR]

Both encoders run at medium. The HDR model is picked from the clip:
3d0h at 1080p and below, 1d5h_2160 above, the hfr variant at 50 fps and
up. The clips this is written for are 1080p and 2160p, so nothing is
scaled; a smaller clip needs the project's upscale-to-1080p rule first.
Read the printed rungs and check the two ladders overlap in bitrate before
trusting a BD figure; --max-crf drops the top rungs for a second read.

  YAH264    yah264 CLI (default build/cli/yah264)
  X264      x264 CLI (default x264)
  VMAF_HDR  a vmaf CLI built with the HDR models' colour options
  VMAF_HDR_MODELS  the directory holding hdr_vmaf_v1.1.8[_hfr]/
"""
import argparse, json, math, os, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bdcompare import bd_rate                                   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
YAH = os.environ.get("YAH264", os.path.join(ROOT, "build", "cli", "yah264"))
X264 = os.environ.get("X264", "x264")
VMAF = os.environ.get("VMAF_HDR")
MODELS = os.environ.get("VMAF_HDR_MODELS")


def y4m_info(path):
    with open(path, "rb") as f:
        hdr = f.readline().decode("latin-1")
    tok = {t[0]: t[1:] for t in hdr.split()[1:]}
    num, den = map(int, tok["F"].split(":"))
    if "p10" not in tok.get("C", ""):
        sys.exit(f"hdr_ladder: {path} is not a 10-bit Y4M")
    return int(tok["W"]), int(tok["H"]), num / den


def model_path(w, h, fps):
    d = "hdr_vmaf_v1.1.8" + ("_hfr" if fps >= 50 else "")
    m = "1d5h_2160" if max(w, h) > 1920 else "3d0h"
    return os.path.join(MODELS, d, f"{d}_{m}.json")


def run(cmd):
    r = subprocess.run(["nice", "-n", "10"] + cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"hdr_ladder: failed: {' '.join(cmd)}\n{r.stderr[-1500:]}")


def psnr(src, dec, w, h):
    import numpy as np
    fs = w * h * 3 // 2
    sse = np.zeros(3)
    n = 0
    with open(src, "rb") as a, open(dec, "rb") as b:
        a.readline(); b.readline()
        while a.readline():
            if not b.readline():
                sys.exit("hdr_ladder: decode is short")
            s = np.frombuffer(a.read(2 * fs), dtype=np.uint16).astype(np.int64)
            d = np.frombuffer(b.read(2 * fs), dtype=np.uint16).astype(np.int64)
            e = (s - d) ** 2
            sse += [e[:w * h].sum(), e[w * h:w * h * 5 // 4].sum(), e[w * h * 5 // 4:].sum()]
            n += 1
    mse = sse / (np.array([w * h, w * h / 4, w * h / 4]) * n)
    p = [10 * math.log10(1023 * 1023 / m) for m in mse]
    return n, p[0], (6 * p[0] + p[1] + p[2]) / 8


def rung(enc, src, crf, work, threads, w, h, fps):
    stem = os.path.join(work, f"{enc}_crf{crf}")
    js = stem + ".json"
    if os.path.exists(js):
        return json.load(open(js))
    out, dec, vj = stem + ".264", stem + ".dec.y4m", stem + ".vmaf.json"
    if enc == "yah264":
        run([YAH, "--input-y4m", src, "--crf", str(crf), "--threads", str(threads), "-o", out])
    else:
        run([X264, "--preset", "medium", "--profile", "high10", "--output-depth", "10",
             "--crf", str(crf), "--threads", str(threads), "--log-level", "error", "-o", out, src])
    run(["ffmpeg", "-v", "error", "-nostdin", "-i", out, "-fps_mode", "passthrough",
         "-pix_fmt", "yuv420p10le", "-strict", "-1", "-f", "yuv4mpegpipe", "-y", dec])
    col = []
    for side in ("ref", "dist"):
        col += [f"--color_range_{side}", "limited", f"--color_primaries_{side}", "bt2020",
                f"--color_trc_{side}", "pq", f"--color_matrix_{side}", "bt2020nc"]
    run([VMAF, "--reference", src, "--distorted", dec, "--threads", str(threads)] + col +
        ["--model", "path=" + model_path(w, h, fps), "--json", "--output", vj])
    n, py, pyuv = psnr(src, dec, w, h)
    os.remove(dec)
    vm = json.load(open(vj))
    if len(vm["frames"]) != n:
        sys.exit("hdr_ladder: the VMAF frame count disagrees with the decode")
    r = {"crf": crf, "kbps": os.path.getsize(out) * 8 / (n / fps) / 1000,
         "vmaf": vm["pooled_metrics"]["vmaf"]["mean"], "psnr_y": py, "psnr_yuv": pyuv}
    json.dump(r, open(js, "w"))
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--crf", default="18,22,26,30,34")
    ap.add_argument("--max-crf", type=float, default=None)
    ap.add_argument("--threads", type=int, default=3)
    ap.add_argument("--work", default=None)
    a = ap.parse_args()
    if not VMAF or not MODELS:
        sys.exit("hdr_ladder: set VMAF_HDR and VMAF_HDR_MODELS")
    w, h, fps = y4m_info(a.src)
    work = a.work or os.path.join(ROOT, "local", "hdr_ladder",
                                  os.path.splitext(os.path.basename(a.src))[0])
    os.makedirs(work, exist_ok=True)
    crfs = [int(c) for c in a.crf.split(",")]
    lad = {e: [rung(e, a.src, c, work, a.threads, w, h, fps) for c in crfs]
           for e in ("x264", "yah264")}
    for e, rs in lad.items():
        print(f"{e:7s} " + "  ".join(f"crf{r['crf']}:{r['kbps']:.0f}k/{r['vmaf']:.2f}/{r['psnr_y']:.2f}dB"
                                     for r in rs))
    if a.max_crf is not None:
        lad = {e: [r for r in rs if r["crf"] <= a.max_crf] for e, rs in lad.items()}
    for m in ("vmaf", "psnr_y", "psnr_yuv"):
        b, t = sorted(lad["x264"], key=lambda r: r["kbps"]), sorted(lad["yah264"], key=lambda r: r["kbps"])
        bd = bd_rate([r["kbps"] for r in b], [r[m] for r in b], [r["kbps"] for r in t], [r[m] for r in t])
        print(f"BD-rate {m:8s} yah264 vs x264 High 10: " + (f"{bd:+.2f}%" if bd is not None else "no overlap"))


main()
