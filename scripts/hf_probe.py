#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""HF-retention probe: where does the high-frequency texture go, per encoder.

Encodes a clip with yah264 and x264 under the minimal quality-mechanism
reproducer (CQP, IPPP, no psy / no mb-tree / no AQ, ref 3), decodes both, and
reports per-frequency-band DCT energy retention (recon energy / source energy)
split by the DECODER'S per-MB class (skip / inter-coded / intra) and by a
source-side motion class. This is the instrument behind the "we buy MSE, x264
buys texture" mechanism (the local measurement records): equal PSNR with lower
HF retention means the loss is texture, and the class split says WHICH decision
sheds it (skip choice vs coefficient decisions on coded MBs).

Bands are zigzag rings of the 8x8 DCT (u+v): LF 1-2, MF 3-6, HF 7-14; DC is
excluded everywhere. Retention is an energy ratio on the luma plane, frames
1..N-1 (frame 0 is the IDR and all-intra by construction).

Usage:
  python3 scripts/hf_probe.py bus_cif 100 36,42
  ARMS="next-vs|next-med|x264-med" python3 scripts/hf_probe.py foreman_cif 100 42
  NEXT_EXTRA="--subme 9" X264_EXTRA="--trellis 0" ... to perturb either side.

The MB grid comes from `ffmpeg -debug mb_type` (verified printing on ffmpeg 7/8;
the probe hard-fails if no grid is found rather than reporting all-unknown).
"""
import os, re, subprocess, sys
import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRATCH = os.environ.get("HF_SCRATCH", "/tmp/hf_probe")
YAH264 = os.path.join(REPO, "build/cli/yah264")
X264 = os.environ.get("X264", "x264")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(" ".join(cmd) + "\n" + r.stderr.decode()[-2000:] + "\n")
        sys.exit(1)
    return r

def read_y4m(path, frames):
    with open(path, "rb") as f:
        hdr = f.readline().decode()
        w = int(re.search(r"W(\d+)", hdr).group(1))
        h = int(re.search(r"H(\d+)", hdr).group(1))
        ys = []
        fsz = w * h * 3 // 2
        for _ in range(frames):
            line = f.readline()
            if not line.startswith(b"FRAME"):
                break
            buf = f.read(fsz)
            if len(buf) < fsz:
                break
            ys.append(np.frombuffer(buf[: w * h], np.uint8).reshape(h, w))
    return w, h, np.stack(ys).astype(np.float64)

def read_yuv(path, w, h, frames):
    fsz = w * h * 3 // 2
    ys = []
    with open(path, "rb") as f:
        for _ in range(frames):
            buf = f.read(fsz)
            if len(buf) < fsz:
                break
            ys.append(np.frombuffer(buf[: w * h], np.uint8).reshape(h, w))
    return np.stack(ys).astype(np.float64)

def dct_mat():
    D = np.zeros((8, 8))
    for u in range(8):
        for x in range(8):
            c = np.sqrt(1 / 8) if u == 0 else np.sqrt(2 / 8)
            D[u, x] = c * np.cos((2 * x + 1) * u * np.pi / 16)
    return D

D8 = dct_mat()
RING = np.add.outer(np.arange(8), np.arange(8))  # u+v per coefficient
BANDS = {"LF": (RING >= 1) & (RING <= 2), "MF": (RING >= 3) & (RING <= 6),
         "HF": RING >= 7, "AC": RING >= 1}

def block_energies(frames):
    """(nframes, H/8, W/8, 8, 8) squared DCT coefficients."""
    n, h, w = frames.shape
    b = frames.reshape(n, h // 8, 8, w // 8, 8).transpose(0, 1, 3, 2, 4)
    coef = np.einsum("ux,nijxy,vy->nijuv", D8, b, D8)
    return coef * coef

def mb_classes(bitstream, mbw, mbh, frames):
    """Per-frame (mbh, mbw) class array: 0 skip, 1 inter, 2 intra, from
    ffmpeg -debug mb_type. Frame order is decode order == display (IPPP)."""
    r = subprocess.run([FFMPEG, "-hide_banner", "-threads", "1", "-debug",
                        "mb_type", "-i", bitstream, "-f", "null", "-"],
                       capture_output=True)
    txt = r.stderr.decode(errors="replace")
    grids, cur = [], None
    for line in txt.splitlines():
        m = re.match(r"\[h264 @ [0-9a-fx]+\] New frame, type: ", line)
        if m:
            cur = np.full((mbh, mbw), -1, np.int8)
            grids.append(cur)
            continue
        m = re.match(r"\[h264 @ [0-9a-fx]+\]\s+(\d+) (.*)$", line)
        if m is None or cur is None:
            continue
        y = int(m.group(1))
        if y % 16:
            continue
        row, chars = y // 16, m.group(2)
        if row >= mbh:
            continue
        for mx in range(mbw):
            c = chars[mx * 3] if mx * 3 < len(chars) else " "
            cur[row, mx] = 0 if c == "S" else 2 if c in "iIA" else 1
    if len(grids) < frames:
        sys.exit(f"mb_type grids: got {len(grids)}, want {frames} ({bitstream})")
    return np.stack(grids[:frames])

def encode(arm, clip, frames, qp, out):
    src = os.path.join(REPO, "tests/corpus", clip + ".y4m")
    if arm.startswith("next"):
        cmd = [YAH264, "--input-y4m", src, "--frames", str(frames),
               "--qp", str(qp), "--bframes", "0", "--ref", "3",
               "--aq-strength", "0", "--psy-rd", "0", "--rc-lookahead", "0",
               "--no-scenecut", "--threads", "1", "-o", out]
        if arm == "next-vs":
            cmd += ["--preset", "veryslow"]
        cmd += os.environ.get("NEXT_EXTRA", "").split()
    else:
        cmd = [X264, "--preset", "medium", "--no-psy", "--no-mbtree",
               "--aq-mode", "0", "--bframes", "0", "--ref", "3",
               "--qp", str(qp), "--frames", str(frames), "--no-scenecut",
               "--threads", "1", "--demuxer", "y4m", "--no-progress",
               "-o", out, src]
        cmd += os.environ.get("X264_EXTRA", "").split()
    run([c for c in cmd if c])
    return os.path.getsize(out)

def vmaf_neg(bitstream, src):
    dec = bitstream + ".y4m"
    js = bitstream + ".json"
    run([FFMPEG, "-v", "error", "-y", "-i", bitstream, "-pix_fmt", "yuv420p", dec])
    run([os.environ.get("VMAF", "vmaf"), "-r", src, "-d", dec, "--json", "-o", js,
         "--model", "version=vmaf_v0.6.1neg:name=neg"])
    import json
    v = json.load(open(js))["pooled_metrics"]["neg"]["mean"]
    os.unlink(dec); os.unlink(js)
    return v

def main():
    clip, frames, qps = sys.argv[1], int(sys.argv[2]), [int(q) for q in sys.argv[3].split(",")]
    arms = os.environ.get("ARMS", "next-vs|x264-med").split("|")
    os.makedirs(SCRATCH, exist_ok=True)
    src = os.path.join(REPO, "tests/corpus", clip + ".y4m")
    w, h, sy = read_y4m(src, frames)
    mbw, mbh = w // 16, h // 16
    se = block_energies(sy)                                   # source energies
    # source motion class per MB, frames 1..N-1: mean |diff| vs prev frame
    md = np.abs(sy[1:] - sy[:-1]).reshape(frames - 1, mbh, 16, mbw, 16).mean(axis=(2, 4))
    moving = md > 2.0
    score = os.environ.get("HF_VMAF") == "1"
    print(f"# {clip} {w}x{h} {frames}f  bands: LF u+v 1-2, MF 3-6, HF 7-14 (8x8 DCT, luma)")
    print(f"# arm qp bits psnrY{' NEG   ' if score else ''} | AC LF MF HF | HF@inter HF@skip HF@moving HF@static | skip% intra%")
    for qp in qps:
        for arm in arms:
            bs = os.path.join(SCRATCH, f"{clip}_{arm}_{qp}.264")
            yuv = bs + ".yuv"
            bits = encode(arm, clip, frames, qp, bs) * 8
            run([FFMPEG, "-y", "-hide_banner", "-threads", "1", "-i", bs,
                 "-pix_fmt", "yuv420p", "-f", "rawvideo", yuv])
            ry = read_yuv(yuv, w, h, frames)
            assert ry.shape[0] == frames, f"decoded {ry.shape[0]} frames"
            os.unlink(yuv)
            cls = mb_classes(bs, mbw, mbh, frames)
            re_ = block_energies(ry)
            mse = ((ry[1:] - sy[1:]) ** 2).mean()
            psnr = 10 * np.log10(255 * 255 / mse)
            # inter frames only; map MB class / motion class to the 4 8x8 blocks
            c8 = np.repeat(np.repeat(cls[1:], 2, 1), 2, 2)
            m8 = np.repeat(np.repeat(moving, 2, 1), 2, 2)
            s, r = se[1:], re_[1:]
            def ret(mask, sel=None):
                num = r[..., mask]; den = s[..., mask]
                if sel is not None:
                    num, den = num[sel], den[sel]
                d = den.sum()
                return num.sum() / d if d > 0 else float("nan")
            row = [ret(BANDS[b]) for b in ("AC", "LF", "MF", "HF")]
            split = [ret(BANDS["HF"], c8 == 1), ret(BANDS["HF"], c8 == 0),
                     ret(BANDS["HF"], m8), ret(BANDS["HF"], ~m8)]
            shares = [(cls[1:] == 0).mean() * 100, (cls[1:] == 2).mean() * 100]
            neg = f" {vmaf_neg(bs, src):6.2f}" if score else ""
            print(f"{arm:9s} {qp:2d} {bits:9d} {psnr:6.2f}{neg} | "
                  + " ".join(f"{v:.3f}" for v in row) + " | "
                  + " ".join(f"{v:.3f}" for v in split) + " | "
                  + f"{shares[0]:5.1f} {shares[1]:4.1f}")

if __name__ == "__main__":
    main()
