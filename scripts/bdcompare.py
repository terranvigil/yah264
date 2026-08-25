#!/usr/bin/env python3
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""Quick BD-rate comparison between two encoder configurations.

Encodes truncated corpus clips at several rate points with both configs (in
parallel), decodes, and reports per-clip BD-rate. PSNR by default (fast,
numpy); --vmaf for the perceptual number when it matters. Meant for the tight
measure-ship-or-revert loop, where the full bench.py run is too slow.

Templates use {src}, {q}, {out}. Examples:

  # working tree vs x264 medium, CRF mode, VMAF
  scripts/bdcompare.py \
      --a './build/cli/next264 --input-y4m {src} --crf {q} --cabac --bframes 3 --ref 3 --transform-8x8 --threads 8 -o {out}' \
      --b '../x264/x264-noasm --crf {q} --preset medium --keyint 250 --demuxer y4m -o {out} {src}' \
      --points 20,23,26,29 --vmaf

  # feature evaluation: current binary vs a saved baseline binary, CQP PSNR
  scripts/bdcompare.py \
      --a './build/cli/next264 --input-y4m {src} --qp {q} --cabac --bframes 3 --threads 8 -o {out}' \
      --b './baseline_next264 --input-y4m {src} --qp {q} --cabac --bframes 3 --threads 8 -o {out}'

BD sign: negative means config A uses fewer bits than B at equal quality.
"""
import argparse
import concurrent.futures as fut
import glob
import hashlib
import json
import math
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def decode_raw(path):
    import numpy as np
    p = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
                        "-pix_fmt", "yuv420p", "-"], capture_output=True)
    return np.frombuffer(p.stdout, dtype=np.uint8)


def psnr_of(bitstream, src_raw):
    import numpy as np
    d = decode_raw(bitstream).astype(np.int32)
    n = min(len(d), len(src_raw))
    if n == 0 or len(d) != len(src_raw):
        return None
    mse = np.mean((d - src_raw[:n]) ** 2)
    return 10 * math.log10(255 * 255 / mse)


SUBSAMPLE = 1   # compute VMAF on every Nth frame (set from --subsample); 1 = all


def vmaf_of(bitstream, src_path, work):
    # Unique temp paths (NOT derived from the bitstream) so shared/cached
    # bitstreams don't collide across threads.
    fd, dec = tempfile.mkstemp(suffix=".dec.y4m", dir=work); os.close(fd)
    fd, js = tempfile.mkstemp(suffix=".json", dir=work); os.close(fd)
    sh(f'ffmpeg -v error -y -i "{bitstream}" -pix_fmt yuv420p "{dec}"')
    binary = os.environ.get("VMAF", "vmaf")
    margs, primary, plabel = _vmaf_models()
    sub = f"--subsample {SUBSAMPLE}" if SUBSAMPLE > 1 else ""
    r = sh(f'{shlex.quote(binary)} -r "{src_path}" -d "{dec}" --json -o "{js}" {margs} {sub}')
    try:
        os.unlink(dec)
    except OSError:
        pass
    if r.returncode != 0:
        return None
    pm = json.load(open(js))["pooled_metrics"]
    if primary not in pm:
        return None
    # Report the best available VMAF (v1 when NEXT264_VMAF_MODEL is set) plus
    # NEG, which penalises enhancement/sharpening -- the honest gate for
    # texture-retention features (psy-RD) that plain VMAF over-credits.
    out = {plabel: pm[primary]["mean"]}
    if "vmaf_neg" in pm:
        out["VMAF-NEG"] = pm["vmaf_neg"]["mean"]
    return out


_MODELS = None


def _vmaf_models():
    """(model-args string, primary pooled key, report label). Prefers the v1
    model from NEXT264_VMAF_MODEL, mirroring bench.py; always adds NEG."""
    global _MODELS
    if _MODELS is not None:
        return _MODELS
    parts, primary, label = [], "vmaf", "VMAF"
    env_path = os.environ.get("NEXT264_VMAF_MODEL")
    if env_path and os.path.exists(env_path):
        parts.append(f"--model path={shlex.quote(env_path)}:name=vmaf_v1")
        primary, label = "vmaf_v1", "VMAF-v1"
    parts.append("--model version=vmaf_v0.6.1:name=vmaf")
    parts.append("--model version=vmaf_v0.6.1neg:name=vmaf_neg")
    _MODELS = (" ".join(parts), primary, label)
    return _MODELS


def bd_rate(r_ref, m_ref, r_test, m_test):
    import numpy as np
    deg = min(3, len(m_ref) - 1)
    f1 = np.polyfit(m_ref, np.log(r_ref), deg)
    f2 = np.polyfit(m_test, np.log(r_test), deg)
    lo, hi = max(min(m_ref), min(m_test)), min(max(m_ref), max(m_test))
    if hi <= lo:
        return None
    xs = np.linspace(lo, hi, 100)
    return (math.exp(np.mean(np.polyval(f2, xs) - np.polyval(f1, xs))) - 1) * 100


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="encoder A command template")
    ap.add_argument("--b", required=True, help="encoder B command template (reference)")
    # Default CRF points span VMAF's DISCRIMINATING band (~55-95) on CIF, not the
    # saturated 95-100 top where BD-rate is noisy/overflow-prone (CRF 20-28 pins
    # near 100 on easy clips). Push points up for higher-res content.
    ap.add_argument("--points", default="26,32,38,44")
    ap.add_argument("--clips", default="foreman_cif,mobile_cif")
    ap.add_argument("--class", dest="klass", default=None,
                    help="select all clips of a content class from the "
                         "tests/corpus/CLASSES manifest (e.g. grain, motion); "
                         "overrides --clips. See docs/corpus-broadening-plan.md")
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--vmaf", action="store_true", help="VMAF instead of PSNR")
    ap.add_argument("--subsample", type=int, default=5,
                    help="compute VMAF on every Nth frame (libvmaf --subsample); "
                         "5-10 is ~Nx faster with negligible BD change")
    ap.add_argument("--jobs", type=int, default=16)
    ap.add_argument("--no-cache", dest="cache", action="store_false",
                    help="disable the persistent encode cache (on by default; a "
                         "fixed baseline binary is encoded once across sweeps)")
    args = ap.parse_args()
    global SUBSAMPLE
    SUBSAMPLE = max(1, args.subsample)

    points = [int(q) for q in args.points.split(",")]
    if args.klass:
        manifest = os.path.join(ROOT, "tests", "corpus", "CLASSES")
        if not os.path.exists(manifest):
            sys.exit("bdcompare: --class needs tests/corpus/CLASSES "
                     "(run scripts/fetch_corpus.sh)")
        clips = [ln.split()[0] for ln in open(manifest)
                 if len(ln.split()) == 2 and ln.split()[1] == args.klass]
        if not clips:
            sys.exit(f"bdcompare: no clips of class '{args.klass}' in {manifest}")
        print(f"class {args.klass}: {', '.join(clips)}")
    else:
        clips = args.clips.split(",")
    work = tempfile.mkdtemp(prefix="bdq_")

    srcs = {}
    for c in clips:
        full = os.path.join(ROOT, "tests", "corpus", c + ".y4m")
        if not os.path.exists(full):
            sys.exit(f"bdcompare: no corpus clip {full}")
        cut = os.path.join(work, c + ".y4m")
        sh(f'ffmpeg -v error -y -i "{full}" -frames:v {args.frames} "{cut}"')
        srcs[c] = cut

    # Persistent encode cache: keyed on (template, clip, q, frames, encoder-binary
    # mtime). A fixed baseline binary hits the cache across strength sweeps, so
    # its encodes are computed once, not per invocation (~halves repeated sweeps).
    # The binary mtime in the key invalidates automatically on rebuild.
    cache_dir = os.path.join(tempfile.gettempdir(), "bdcache")
    if args.cache:
        os.makedirs(cache_dir, exist_ok=True)

    def _ckey(tmpl, clip, q):
        binp = (shlex.split(tmpl)[0] if tmpl.strip() else "")
        mt = os.path.getmtime(binp) if os.path.exists(binp) else 0
        h = hashlib.sha1(f"{tmpl}|{clip}|{q}|{args.frames}|{mt}".encode()).hexdigest()[:20]
        return os.path.join(cache_dir, h + ".264")

    def encode(tag, tmpl, clip, q):
        out = os.path.join(work, f"{clip}_{tag}_{q}.264")
        if args.cache:
            ck = _ckey(tmpl, clip, q)
            if os.path.exists(ck):
                return (tag, clip, q, ck)
        cmd = tmpl.format(src=shlex.quote(srcs[clip]), q=q, out=shlex.quote(out))
        r = sh(cmd)
        if r.returncode != 0 or not os.path.exists(out):
            return (tag, clip, q, None)
        if args.cache:
            try:
                os.replace(out, ck); return (tag, clip, q, ck)
            except OSError:
                pass
        return (tag, clip, q, out)

    jobs = []
    with fut.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for clip in clips:
            for q in points:
                jobs.append(ex.submit(encode, "a", args.a, clip, q))
                jobs.append(ex.submit(encode, "b", args.b, clip, q))
        results = [j.result() for j in jobs]

    outs = {(t, c, q): o for t, c, q, o in results}
    if any(o is None for o in outs.values()):
        bad = [k for k, v in outs.items() if v is None]
        sys.exit(f"bdcompare: encode failed for {bad[:3]}")

    src_raw = {}
    if not args.vmaf:
        for c in clips:
            src_raw[c] = decode_raw(srcs[c]).astype("int32")

    def measure(clip, tag, q):
        o = outs[(tag, clip, q)]
        if args.vmaf:
            m = vmaf_of(o, srcs[clip], work)
        else:
            p = psnr_of(o, src_raw[clip])
            m = {"PSNR": p} if p is not None else None
        return (clip, tag, q, os.path.getsize(o), m)

    with fut.ThreadPoolExecutor(max_workers=args.jobs if args.vmaf else 2) as ex:
        ms = [ex.submit(measure, c, t, q) for c in clips for t in ("a", "b")
              for q in points]
        metrics = [m.result() for m in ms]

    table = {}
    for clip, tag, q, size, m in metrics:
        if m is None:
            sys.exit(f"bdcompare: metric failed for {clip} {tag} {q}")
        table.setdefault((clip, tag), []).append((q, size, m))

    # metric names come from the measured dict (e.g. VMAF-v1 + VMAF-NEG, or PSNR)
    names = list(next(iter(table.values()))[0][2].keys())
    for clip in clips:
        for i, nm in enumerate(names):
            rows = {}
            for tag in ("a", "b"):
                pts = sorted(table[(clip, tag)], key=lambda t: t[0], reverse=True)
                rows[tag] = ([p[1] for p in pts], [p[2][nm] for p in pts])
                if i == 0:      # print the size/quality points once per clip
                    print(f"{clip:14} {tag}: " +
                          " ".join(f"{s//1000}k/{m[nm]:.2f}" for _, s, m in pts))
            bd = bd_rate(*rows["b"], *rows["a"])
            # Flag saturation: if the top point on either side is >=98, VMAF is
            # near-pinned and the BD is measured in a low-discrimination regime
            # (unreliable / overflow-prone). Push --points higher.
            hi = max(max(rows["a"][1]), max(rows["b"][1])) if nm != "PSNR" else 0
            warn = "  [!] near-saturation, raise --points" if hi >= 98 else ""
            print(f"{clip:14} BD-rate({nm}) A vs B: "
                  + (f"{bd:+.2f}%" if bd is not None else "no quality overlap") + warn)


if __name__ == "__main__":
    main()
