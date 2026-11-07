#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""CRF ladders on a 10-bit HDR10 (PQ, BT.2020, limited range) Y4M, yah264
against x264 High 10, scored with VMAF v1's HDR model and PSNR on the 10-bit
code values, then BD-rate over the rungs both ladders cover.

  scripts/hdr_ladder.py SRC.y4m [--crf 18,22,26,30,34] [--max-crf 30]
                        [--threads 3] [--work DIR] [--fresh]

Both encoders run at medium. The source must be a 4:2:0 10-bit Y4M
(C420p10) at exactly 1920x1080 or 3840x2160: the HDR model is scored at
its own display size (3d0h at 1080p, 1d5h_2160 at 2160p, the hfr variant
at 50 fps and up), and anything smaller would need the project's
upscale-to-1080p rule first, so it is refused. Read the printed rungs and
check the two ladders overlap in bitrate before trusting a BD figure;
--max-crf drops the top rungs for a second read.

Each rung's result is cached in the work directory, keyed on the md5 of
the encoder binary that made it and the encoder's full command line, so a
rebuilt binary or a changed option re-encodes; a reused rung prints
"cached". --fresh ignores the cache.

  YAH264    yah264 CLI (default build/cli/yah264)
  X264      x264 CLI (default x264)
  VMAF_HDR  a vmaf CLI built with the HDR models' colour options
  VMAF_HDR_MODELS  the directory holding hdr_vmaf_v1.1.8[_hfr]/
"""
import argparse, hashlib, json, math, os, shutil, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bdcompare import bd_rate                                   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
YAH = os.environ.get("YAH264", os.path.join(ROOT, "build", "cli", "yah264"))
X264 = os.environ.get("X264", "x264")
VMAF = os.environ.get("VMAF_HDR")
MODELS = os.environ.get("VMAF_HDR_MODELS")


SIZES = {(1920, 1080): "3d0h", (3840, 2160): "1d5h_2160"}


def y4m_info(path):
    with open(path, "rb") as f:
        hdr = f.readline().decode("latin-1")
    if not hdr.startswith("YUV4MPEG2 "):
        sys.exit(f"hdr_ladder: {path} is not a Y4M")
    tok = {t[0]: t[1:] for t in hdr.split()[1:]}
    if tok.get("C") != "420p10":
        sys.exit(f"hdr_ladder: {path} is C{tok.get('C', '420')}, not C420p10")
    w, h = int(tok["W"]), int(tok["H"])
    if (w, h) not in SIZES:
        sys.exit(f"hdr_ladder: {path} is {w}x{h}; the HDR models are scored at 1920x1080 "
                 "or 3840x2160, and a smaller clip has to be scaled up to 1080p first")
    num, den = map(int, tok["F"].split(":"))
    return w, h, num / den


def model_path(w, h, fps):
    d = "hdr_vmaf_v1.1.8" + ("_hfr" if fps >= 50 else "")
    return os.path.join(MODELS, d, f"{d}_{SIZES[(w, h)]}.json")


def which(b):
    return b if os.sep in b else shutil.which(b)


def md5_of(path):
    m = hashlib.md5()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            m.update(blk)
    return m.hexdigest()


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
    if n == 0:
        sys.exit("hdr_ladder: the decode has no frames")
    mse = sse / (np.array([w * h, w * h / 4, w * h / 4]) * n)
    p = [10 * math.log10(1023 * 1023 / m) if m > 0 else 100.0 for m in mse]   # lossless caps
    return n, p[0], (6 * p[0] + p[1] + p[2]) / 8


def rung(enc, src, crf, work, threads, w, h, fps, bin_md5, fresh):
    stem = os.path.join(work, f"{enc}_crf{crf}")
    js = stem + ".json"
    out, dec, vj = stem + ".264", stem + ".dec.y4m", stem + ".vmaf.json"
    if enc == "yah264":
        cmd = [YAH, "--input-y4m", src, "--crf", str(crf), "--threads", str(threads), "-o", out]
    else:
        cmd = [X264, "--preset", "medium", "--profile", "high10", "--output-depth", "10",
               "--crf", str(crf), "--threads", str(threads), "--log-level", "error", "-o", out, src]
    key = {"bin_md5": bin_md5, "cmd": cmd, "model": model_path(w, h, fps)}
    if not fresh and os.path.exists(js):
        r = json.load(open(js))
        if r.get("key") == key:
            r["cached"] = True
            return r
    run(cmd)
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
         "vmaf": vm["pooled_metrics"]["vmaf"]["mean"], "psnr_y": py, "psnr_yuv": pyuv,
         "key": key}
    json.dump(r, open(js, "w"))
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--crf", default="18,22,26,30,34")
    ap.add_argument("--max-crf", type=float, default=None)
    ap.add_argument("--threads", type=int, default=3)
    ap.add_argument("--work", default=None)
    ap.add_argument("--fresh", action="store_true", help="ignore cached rungs")
    a = ap.parse_args()
    if not VMAF or not MODELS:
        sys.exit("hdr_ladder: set VMAF_HDR and VMAF_HDR_MODELS")
    w, h, fps = y4m_info(a.src)
    # everything a rung needs, checked before the first encode
    bins = {}
    for name, b in (("yah264", YAH), ("x264", X264), ("vmaf", VMAF), ("ffmpeg", "ffmpeg")):
        p = which(b)
        if not p or not os.access(p, os.X_OK):
            sys.exit(f"hdr_ladder: no {name} binary at '{b}'")
        bins[name] = p
    if not os.path.isfile(model_path(w, h, fps)):
        sys.exit(f"hdr_ladder: no HDR model at {model_path(w, h, fps)}")
    bin_md5 = {e: md5_of(bins[e]) for e in ("yah264", "x264")}
    work = a.work or os.path.join(ROOT, "local", "hdr_ladder",
                                  os.path.splitext(os.path.basename(a.src))[0])
    os.makedirs(work, exist_ok=True)
    crfs = [int(c) for c in a.crf.split(",")]
    lad = {e: [rung(e, a.src, c, work, a.threads, w, h, fps, bin_md5[e], a.fresh) for c in crfs]
           for e in ("x264", "yah264")}
    for e, rs in lad.items():
        print(f"{e:7s} " + "  ".join(f"crf{r['crf']}:{r['kbps']:.0f}k/{r['vmaf']:.2f}/{r['psnr_y']:.2f}dB"
                                     + (" (cached)" if r.get("cached") else "") for r in rs))
    if a.max_crf is not None:
        lad = {e: [r for r in rs if r["crf"] <= a.max_crf] for e, rs in lad.items()}
    for m in ("vmaf", "psnr_y", "psnr_yuv"):
        b, t = sorted(lad["x264"], key=lambda r: r["kbps"]), sorted(lad["yah264"], key=lambda r: r["kbps"])
        bd = bd_rate([r["kbps"] for r in b], [r[m] for r in b], [r["kbps"] for r in t], [r[m] for r in t])
        print(f"BD-rate {m:8s} yah264 vs x264 High 10: " + (f"{bd:+.2f}%" if bd is not None else "no overlap"))


main()
