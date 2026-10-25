#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""band_check.py - dark banding against the reference encoder, on real frames.

    python3 scripts/band_check.py
    WINDOWS=nocturne-x10_1080p:29280 ARGS='--aq-strength 0.7' python3 scripts/band_check.py

VMAF v1 scores banding through CAMBI, but CAMBI misses much of what shows in
dark gradients, so an arm that flattens them can pass every BD band. This is the check that
catches it. For each window (a title and a center frame; 96 frames are cut
around it) it encodes the reference at --preset medium --crf 23, solves
yah264 onto the reference's size (within SIZE_TOL), decodes both and scores
the 8x8 blocks the SOURCE makes dark and smooth (luma mean 17-50, standard
deviation 0.3-2.5), the blocks where banding shows:

  flat   share of those blocks the decode left perfectly flat (a band step)
  keep   mean decoded/source standard deviation over them, capped at 2
  dark   PSNR-Y over them
  cambi  CAMBI at libvmaf's own settings, whole frame (10-bit, told the
         encode's bit depth). v1's CAMBI ignores luminance below 0.06 cd/m2,
         which is most of this window, so the gate reads this one
  v1     VMAF v1, and its CAMBI beside it (cambi1)

A window FAILS when yah264's flat share exceeds the reference's by more than
FLAT_MARGIN or its CAMBI exceeds the reference's by more than CAMBI_MARGIN.
A window whose size solve misses by more than SIZE_TOL is reported UNMATCHED
and not judged. The default windows are the ones the dark-banding report was
reproduced on: frames with both very bright and very dark areas, and mostly
dark frames with faint grain.

Env: WINDOWS (title:frame,...), SRC_DIR, YAH264, X264, ARGS (extra yah264
arguments), FLAT_MARGIN (0.05), CAMBI_MARGIN (1.0), SIZE_TOL (0.03),
THREADS (2), OUT (scratch directory, kept). The v1 binary and models are
bdcompare's (VMAF_V1_BIN, VMAF_V1_MODELS).
"""
import os, re, shutil, subprocess, sys, tempfile
import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WINDOWS = os.environ.get("WINDOWS", "nocturne-x10_1080p:29280,nocturne-x10_1080p:3960,"
                         "tears-x10_1080p:10200,tears-x10_1080p:9480,meridian-x10_1080p:2400")
SRC_DIR = os.environ.get("SRC_DIR", "/Volumes/T7/staging/yah264/y4m")
YAH264 = os.environ.get("YAH264", os.path.join(REPO, "build/cli/yah264"))
X264 = os.environ.get("X264", "x264")
ARGS = os.environ.get("ARGS", "").split()
FLAT_MARGIN = float(os.environ.get("FLAT_MARGIN", "0.05"))
CAMBI_MARGIN = float(os.environ.get("CAMBI_MARGIN", "1.0"))
SIZE_TOL = float(os.environ.get("SIZE_TOL", "0.03"))
THREADS = os.environ.get("THREADS", "2")
FRAMES = 96
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bdcompare                                                # noqa: E402


def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode:
        sys.exit(" ".join(cmd) + "\n" + r.stderr.decode(errors="replace")[-2000:])


def y4m_geom(path):
    with open(path, "rb") as f:
        hdr = f.readline()
    w = int(re.search(rb" W(\d+)", hdr).group(1))
    h = int(re.search(rb" H(\d+)", hdr).group(1))
    return hdr, w, h


def cut(src, start, n, out):
    hdr, w, h = y4m_geom(src)
    fsz = w * h * 3 // 2
    with open(src, "rb") as f, open(out, "wb") as o:
        o.write(hdr)
        f.readline()
        first = f.readline()
        step = len(first) + fsz            # every FRAME line here is bare
        f.seek(len(hdr) + start * step)
        for _ in range(n):
            line = f.readline()
            if not line.startswith(b"FRAME"):
                break
            o.write(line + f.read(fsz))


def nframes(path):
    """Frames in a y4m made by cut() (bare FRAME lines)."""
    hdr, w, h = y4m_geom(path)
    return (os.path.getsize(path) - len(hdr)) // (len(b"FRAME\n") + w * h * 3 // 2)


def lumas(path):
    hdr, w, h = y4m_geom(path)
    fsz = w * h * 3 // 2
    with open(path, "rb") as f:
        f.readline()
        while f.readline().startswith(b"FRAME"):
            yield np.frombuffer(f.read(fsz)[: w * h], np.uint8).reshape(h, w).astype(np.float64)


def band_metric(src, dec):
    se_m = cnt_m = 0.0
    keeps, flats, nblk = [], 0, 0
    for s, d in zip(lumas(src), lumas(dec)):
        h, w = (s.shape[0] // 8) * 8, (s.shape[1] // 8) * 8
        sb = s[:h, :w].reshape(h // 8, 8, w // 8, 8).swapaxes(1, 2).reshape(-1, 64)
        db = d[:h, :w].reshape(h // 8, 8, w // 8, 8).swapaxes(1, 2).reshape(-1, 64)
        sm, ss, ds = sb.mean(1), sb.std(1), db.std(1)
        m = (sm > 17) & (sm < 50) & (ss > 0.3) & (ss < 2.5)
        if m.any():
            se_m += ((sb[m] - db[m]) ** 2).sum(); cnt_m += m.sum() * 64
            keeps.append(np.minimum(ds[m] / ss[m], 2))
            flats += int((ds[m] == 0).sum()); nblk += int(m.sum())
    mse = se_m / max(cnt_m, 1)
    return {"blocks": nblk, "flat": flats / max(nblk, 1),
            "keep": float(np.concatenate(keeps).mean()) if keeps else float("nan"),
            "dark": 99.0 if mse == 0 else 10 * np.log10(255 ** 2 / mse)}


def score(src, bs, tag, d):
    dec = os.path.join(d, tag + ".dec.y4m")
    run(["ffmpeg", "-v", "error", "-y", "-threads", "1", "-i", bs,
         "-pix_fmt", "yuv420p", "-f", "yuv4mpegpipe", "-strict", "-1", dec])
    r = band_metric(src, dec)
    r["bytes"] = os.path.getsize(bs)
    os.unlink(dec)
    v = bdcompare._v1_of(bs, src, d)
    if v is None:
        sys.exit(f"band_check: VMAF v1 failed on {bs}")
    r["v1"], r["cambi1"] = v["VMAF-v1"], v["CAMBI"]
    r["cambi"] = full_cambi(src, bs, d)
    return r


def full_cambi(src, bs, d):
    """CAMBI with no visibility threshold, on the 10-bit pair at the coded size."""
    ref = os.path.join(d, "fc.ref.y4m")
    dec = os.path.join(d, "fc.dec.y4m")
    js = os.path.join(d, "fc.json")
    for a, b in ((src, ref), (bs, dec)):
        run(["ffmpeg", "-v", "error", "-y", "-threads", "1", "-i", a, "-pix_fmt",
             "yuv420p10le", "-strict", "-1", "-f", "yuv4mpegpipe", b])
    try:
        run([bdcompare.V1_BIN, "-q", "-r", ref, "-d", dec, "--no_prediction",
             "--feature", f"cambi=enc_bitdepth={bdcompare._enc_bitdepth(bs)}",
             "--json", "-o", js])
        import json
        with open(js) as f:
            pm = json.load(f)["pooled_metrics"]
        return pm[next(k for k in pm if k.startswith("cambi"))]["mean"]
    finally:
        for p in (ref, dec, js):
            if os.path.exists(p):
                os.unlink(p)


def solve(src, target, out):
    """Encode yah264 onto the reference's size: bisect the CRF and keep the
    closest encode. In a short dark window the size can step by more than the
    tolerance between neighboring rate factors; then a multi-pass encode at
    the target bitrate gets closer, and the closest encode is kept."""
    lo, hi, crf = 10.0, 40.0, 23.0
    best, keep, stats = None, out + ".best", out + ".stats"
    try:
        return _solve(src, target, out, lo, hi, crf, keep, stats)
    finally:
        for p in (keep, stats, stats + ".mbtree"):
            if os.path.exists(p):
                os.unlink(p)


def _solve(src, target, out, lo, hi, crf, keep, stats):
    best = None
    for _ in range(14):
        run([YAH264, "--input-y4m", src, "-o", out, "--crf", f"{crf:.3f}",
             "--threads", THREADS, "--quiet", "--no-progress"] + ARGS)
        ratio = os.path.getsize(out) / target
        if best is None or abs(ratio - 1) < abs(best[1] - 1):
            best = (f"crf {crf:.3f}", ratio)
            shutil.copyfile(out, keep)
        if abs(ratio - 1) < 0.02:
            break
        lo, hi = (crf, hi) if ratio > 1 else (lo, crf)
        crf = round((lo + hi) / 2, 3)
    if abs(best[1] - 1) > SIZE_TOL:
        _, _, fps = bdcompare._y4m_geometry(src)
        kbps = target * 8 / (nframes(src) / fps) / 1000
        # pass 3 reads and rewrites the stats, so each one refines against a
        # real encode; a short dark window can need it to land on the target
        for p in (1, 2, 3, 3):
            run([YAH264, "--input-y4m", src, "-o", out, "--bitrate", str(max(1, round(kbps))),
                 "--pass", str(p), "--stats", stats, "--threads", THREADS,
                 "--quiet", "--no-progress"] + ARGS)
            if p == 1:
                continue
            ratio = os.path.getsize(out) / target
            if abs(ratio - 1) < abs(best[1] - 1):
                best = (f"multi-pass {kbps:.0f} kbit/s", ratio)
                shutil.copyfile(out, keep)
            if abs(ratio - 1) < 0.02:
                break
    shutil.move(keep, out)
    return best


def main():
    d = os.environ.get("OUT") or tempfile.mkdtemp(prefix="y264bandchk.")
    os.makedirs(d, exist_ok=True)
    print(f"# scratch {d}; margins flat +{FLAT_MARGIN}, cambi +{CAMBI_MARGIN}; args {' '.join(ARGS) or '(default)'}")
    print(f"# {'window':26s} {'side':6s} {'bytes':>9s} {'flat':>6s} {'keep':>6s} {'dark':>6s} {'cambi':>6s} {'cambi1':>6s} {'v1':>6s}")
    fails = unmatched = 0
    for spec in WINDOWS.split(","):
        title, frame = spec.rsplit(":", 1)
        name = f"{title}_{frame}"
        src = os.path.join(d, name + ".y4m")
        if not os.path.exists(src):
            cut(os.path.join(SRC_DIR, title + ".y4m"), max(0, int(frame) - FRAMES // 2), FRAMES, src)
        ref = os.path.join(d, name + ".ref.264")
        run([X264, "--preset", "medium", "--crf", "23", "--threads", THREADS, "--quiet",
             "-o", ref, src])
        mine = os.path.join(d, name + ".yah.264")
        how, ratio = solve(src, os.path.getsize(ref), mine)
        a, b = score(src, ref, name + ".ref", d), score(src, mine, name + ".yah", d)
        for side, r in (("ref", a), ("yah264", b)):
            print(f"  {name:26s} {side:6s} {r['bytes']:9d} {r['flat']:6.3f} {r['keep']:6.3f} "
                  f"{r['dark']:6.2f} {r['cambi']:6.3f} {r['cambi1']:6.3f} {r['v1']:6.2f}")
        if abs(ratio - 1) > SIZE_TOL:
            verdict = f"UNMATCHED (size {ratio:.3f} at {how})"; unmatched += 1
        elif b["flat"] > a["flat"] + FLAT_MARGIN or b["cambi"] > a["cambi"] + CAMBI_MARGIN:
            verdict = "FAIL"; fails += 1
        else:
            verdict = "pass"
        print(f"  {name:26s} {verdict}: flat {b['flat'] - a['flat']:+.3f}, cambi {b['cambi'] - a['cambi']:+.3f}, "
              f"size {ratio:.3f}")
    print(f"# {fails} window(s) failed, {unmatched} unmatched (not judged)")
    # 1: a window failed; 2: none failed but one couldn't be judged
    sys.exit(1 if fails else (2 if unmatched else 0))


if __name__ == "__main__":
    main()
