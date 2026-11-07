#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""CRF or VBR ladders on a 10-bit HDR10 (PQ, BT.2020, limited range) Y4M,
yah264 against x264 High 10, scored with VMAF v1's HDR model and PSNR on the
10-bit code values, then BD-rate over the rungs both ladders cover.

  scripts/hdr_ladder.py SRC.y4m [--crf 18,22,26,30,34 | --vbr K1,K2,...]
                        [--max-crf 30] [--yah-lookahead N] [--x264-lookahead N]
                        [--encoders x264,yah264] [--threads 3] [--work DIR]
                        [--fresh]

Both encoders run at medium. The source must be a 4:2:0 10-bit Y4M
(C420p10) at exactly 1920x1080 or 3840x2160: the HDR model is scored at
its own display size (3d0h at 1080p, 1d5h_2160 at 2160p, the hfr variant
at 50 fps and up), and anything smaller would need the project's
upscale-to-1080p rule first, so it is refused. Read the printed rungs and
check the two ladders overlap in bitrate before trusting a BD figure;
--max-crf drops the top rungs for a second read.

--vbr runs one rung per target K kbit/s instead, with --bitrate K,
--vbv-maxrate 1.5K and --vbv-bufsize 2K on both encoders (the parity
spec's VBR ladder), and prints each rung's achieved rate against its target
and the leaky-bucket verdict of scripts/vbv_check.py on the stream. The spec
takes the targets from the reference's achieved rates at CRF 20/23/26/29/32
on the same clip; --encoders x264 runs that CRF ladder alone.

Both encoders run their preset's lookahead depth unless --yah-lookahead or
--x264-lookahead passes --rc-lookahead to that side; every reading prints
the depth each side ran with. CAMBI is the pooled mean of the banding
feature the HDR model computes in the same run.

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
# both encoders' medium preset runs a 40-frame mb-tree window
PRESET_LOOKAHEAD = 40
# bump when a rung's result gains a field, so older cached rungs re-run
RESULT_VERSION = 2


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


def run(cmd, ok=(0,)):
    r = subprocess.run(["nice", "-n", "10"] + cmd, capture_output=True, text=True)
    if r.returncode not in ok:
        sys.exit(f"hdr_ladder: failed: {' '.join(cmd)}\n{r.stderr[-1500:]}")
    return r


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


def vbv_args(kbps):
    return round(kbps * 1.5), round(kbps * 2)


def rung(enc, src, mode, q, work, threads, w, h, fps, bin_md5, lookahead, fresh):
    tag = f"{mode}{q}" + (f"_la{lookahead}" if lookahead is not None else "")
    stem = os.path.join(work, f"{enc}_{tag}")
    js = stem + ".json"
    out, dec, vj = stem + ".264", stem + ".dec.y4m", stem + ".vmaf.json"
    if mode == "crf":
        rc = ["--crf", str(q)]
    else:
        maxrate, bufsize = vbv_args(q)
        rc = ["--bitrate", str(q), "--vbv-maxrate", str(maxrate), "--vbv-bufsize", str(bufsize)]
    if lookahead is not None:
        rc += ["--rc-lookahead", str(lookahead)]
    if enc == "yah264":
        cmd = [YAH, "--input-y4m", src] + rc + ["--threads", str(threads), "-o", out]
    else:
        cmd = [X264, "--preset", "medium", "--profile", "high10", "--output-depth", "10"] + rc + \
              ["--threads", str(threads), "--log-level", "error", "-o", out, src]
    key = {"bin_md5": bin_md5, "cmd": cmd, "model": model_path(w, h, fps),
           "version": RESULT_VERSION}
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
    pooled = vm["pooled_metrics"]
    cambi = [k for k in pooled if k.startswith("cambi")]
    if not cambi:
        sys.exit(f"hdr_ladder: {model_path(w, h, fps)} computes no CAMBI feature")
    frames_cambi = [f["metrics"][cambi[0]] for f in vm["frames"]]
    r = {"mode": mode, "q": q, "lookahead": lookahead,
         "kbps": os.path.getsize(out) * 8 / (n / fps) / 1000,
         "vmaf": pooled["vmaf"]["mean"], "psnr_y": py, "psnr_yuv": pyuv,
         "cambi": pooled[cambi[0]]["mean"], "cambi_max": max(frames_cambi), "key": key}
    if mode == "vbr":
        maxrate, bufsize = vbv_args(q)
        v = run([sys.executable, os.path.join(ROOT, "scripts", "vbv_check.py"), out,
                 "--maxrate", str(maxrate), "--bufsize", str(bufsize), "--y4m", src, "--quiet"],
                ok=(0, 1))
        r["rate_err"] = (r["kbps"] / q - 1) * 100
        r["vbv"] = v.stdout.strip().splitlines()[0] if v.stdout.strip() else "no verdict"
        r["vbv_ok"] = v.returncode == 0
    json.dump(r, open(js, "w"))
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--crf", default="18,22,26,30,34")
    ap.add_argument("--vbr", default=None, help="kbit/s targets; replaces the CRF ladder")
    ap.add_argument("--max-crf", type=float, default=None)
    ap.add_argument("--yah-lookahead", type=int, default=None)
    ap.add_argument("--x264-lookahead", type=int, default=None)
    ap.add_argument("--encoders", default="x264,yah264")
    ap.add_argument("--threads", type=int, default=3)
    ap.add_argument("--work", default=None)
    ap.add_argument("--fresh", action="store_true", help="ignore cached rungs")
    a = ap.parse_args()
    if not VMAF or not MODELS:
        sys.exit("hdr_ladder: set VMAF_HDR and VMAF_HDR_MODELS")
    encs = a.encoders.split(",")
    if not encs or any(e not in ("x264", "yah264") for e in encs):
        sys.exit("hdr_ladder: --encoders takes x264, yah264 or both")
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
    mode = "vbr" if a.vbr else "crf"
    qs = [int(q) for q in (a.vbr or a.crf).split(",")]
    la = {"yah264": a.yah_lookahead, "x264": a.x264_lookahead}
    depth = {e: la[e] if la[e] is not None else PRESET_LOOKAHEAD for e in la}
    print(f"{os.path.basename(a.src)} {w}x{h} {fps:.3f} fps, {mode.upper()}, medium, "
          f"--threads {a.threads}, lookahead x264 {depth['x264']} / yah264 {depth['yah264']}"
          + (", VBV maxrate 1.5K buffer 2K" if mode == "vbr" else ""))
    lad = {e: [rung(e, a.src, mode, q, work, a.threads, w, h, fps, bin_md5[e], la[e], a.fresh)
               for q in qs] for e in encs}
    for e, rs in lad.items():
        for r in rs:
            line = (f"{e:7s} {mode}{r['q']:<6} {r['kbps']:9.0f} kbps  vmaf {r['vmaf']:6.2f}  "
                    f"psnr-y {r['psnr_y']:6.2f}  psnr-yuv {r['psnr_yuv']:6.2f}  "
                    f"cambi {r['cambi']:.3f} (max {r['cambi_max']:.3f})")
            if mode == "vbr":
                line += f"  rate {r['rate_err']:+.1f}%  vbv {'ok' if r['vbv_ok'] else 'UNDERFLOW'}"
            print(line + ("  (cached)" if r.get("cached") else ""))
    if len(lad) < 2:
        return
    if a.max_crf is not None and mode == "crf":
        lad = {e: [r for r in rs if r["q"] <= a.max_crf] for e, rs in lad.items()}
    b, t = sorted(lad["x264"], key=lambda r: r["kbps"]), sorted(lad["yah264"], key=lambda r: r["kbps"])
    lo, hi = max(b[0]["kbps"], t[0]["kbps"]), min(b[-1]["kbps"], t[-1]["kbps"])
    print(f"bitrate overlap: {lo:.0f}-{hi:.0f} kbps" if hi > lo else "bitrate overlap: NONE")
    for m in ("vmaf", "psnr_y", "psnr_yuv"):
        bd = bd_rate([r["kbps"] for r in b], [r[m] for r in b], [r["kbps"] for r in t], [r[m] for r in t])
        print(f"BD-rate {m:8s} yah264 vs x264 High 10: " + (f"{bd:+.2f}%" if bd is not None else "no overlap"))


main()
