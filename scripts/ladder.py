#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""
ladder.py -- the Phase 6 headline-claim harness: reproducible BD-rate + encode
speed of yah264 vs x264 (and optionally x265) at MATCHED preset tiers, across the
corpus, emitted as a markdown report anyone can regenerate.

For each preset in --presets it runs, per clip:
  * BD-rate (VMAF-NEG and VMAF) of yah264 vs the reference at the SAME --preset,
    via scripts/bdcompare.py (a full CRF sweep, so the number integrates over the
    quality range rather than one operating point);
  * encode wall-time of each encoder at a mid CRF, as a speed ratio.

Negative BD-rate = yah264 spends FEWER bits at equal quality (ahead). The gate:
at matched preset, yah264's BD-rate <= 0 (quality parity or better) and the speed
ratio is published per tier -- this is the project's headline claim.

Reproduce:  scripts/ladder.py [--presets a,b,..] [--class NAME | --clips a,b]
            [--ref x264|x265] [--frames N] [--points q,q,..] [--out report.md]
Everything is from-source and pinned: yah264 = build/cli/yah264, x264 =
../x264/x264-asm (NEON) unless X264 overrides, x265 = `x265` on PATH.
"""
import argparse
import os
import re
import shlex
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDCMP = os.path.join(ROOT, "scripts", "bdcompare.py")
CORPUS = os.path.join(ROOT, "tests", "corpus")

YAH264 = os.environ.get("YAH264", os.path.join(ROOT, "build", "cli", "yah264"))
X264 = os.environ.get("X264", os.path.join(ROOT, "..", "x264", "x264-asm"))
X265 = os.environ.get("X265", "x265")

# Reference encode templates. {src}/{out}/{q}/{p} are filled per run. yah264 and
# x264 share the --preset name; x265's --preset ladder is the same 10 names.
NEXT_TMPL = "{bin} --input-y4m {src} -o {out} --preset {p} --crf {q}"
X264_TMPL = "{bin} --crf {q} --preset {p} --demuxer y4m -o {out} {src}"
X265_TMPL = "{bin} --input {src} --y4m --preset {p} --crf {q} --output {out}"


def clips_for(args):
    if args.klass:
        manifest = os.path.join(CORPUS, "CLASSES")
        if not os.path.exists(manifest):
            sys.exit("ladder: --class needs tests/corpus/CLASSES (run fetch_corpus.sh)")
        cl = [ln.split()[0] for ln in open(manifest)
              if len(ln.split()) == 2 and ln.split()[1] == args.klass]
        if not cl:
            sys.exit(f"ladder: no clips of class '{args.klass}'")
        return cl
    return args.clips.split(",")


def bd_for_preset(preset, clips, args):
    """Run bdcompare.py (yah264 vs ref at this preset); return {clip: (neg, v1)}."""
    ref_bin, ref_tmpl = (X264, X264_TMPL) if args.ref == "x264" else (X265, X265_TMPL)
    a = NEXT_TMPL.format(bin=shlex.quote(YAH264), src="{src}", out="{out}", p=preset, q="{q}")
    b = ref_tmpl.format(bin=shlex.quote(ref_bin), src="{src}", out="{out}", p=preset, q="{q}")
    cmd = [sys.executable, BDCMP, "--a", a, "--b", b, "--clips", ",".join(clips),
           "--vmaf", "--frames", str(args.frames), "--points", args.points,
           "--jobs", str(args.jobs)]
    if args.no_cache:
        cmd.append("--no-cache")
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    res = {}
    for line in out.splitlines():
        m = re.match(r"(\S+)\s+BD-rate\((VMAF(?:-NEG)?)\) A vs B:\s+([-+]?[\d.]+)%", line)
        if m:
            clip, metric, val = m.group(1), m.group(2), float(m.group(3))
            res.setdefault(clip, {})[metric] = val
    return res


def time_encode(tmpl, binp, src, preset, q, ref):
    out = f"/tmp/ladder_{ref}_{preset}.bin"
    cmd = tmpl.format(bin=shlex.quote(binp), src=shlex.quote(src), out=shlex.quote(out),
                      p=preset, q=q)
    t0 = time.time()
    r = subprocess.run(shlex.split(cmd), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    dt = time.time() - t0
    return dt if r.returncode == 0 and dt > 0 else None


def speed_for_preset(preset, clip, args):
    """Wall-time both encoders on one clip at a mid CRF; return (next_s, ref_s)."""
    src = os.path.join(CORPUS, clip + ".y4m")
    q = args.points.split(",")[len(args.points.split(",")) // 2]
    ns = time_encode(NEXT_TMPL, YAH264, src, preset, q, "next")
    if args.ref == "x264":
        rs = time_encode(X264_TMPL, X264, src, preset, q, "x264")
    else:
        rs = time_encode(X265_TMPL, X265, src, preset, q, "x265")
    return ns, rs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--presets", default="ultrafast,veryfast,fast,medium,slow")
    ap.add_argument("--clips", default="foreman_cif,mobile_cif,stefan_cif,bus_cif,coastguard_cif,tempete_cif")
    ap.add_argument("--class", dest="klass", default=None)
    ap.add_argument("--ref", default="x264", choices=["x264", "x265"])
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--points", default="26,32,38,44")
    ap.add_argument("--jobs", type=int, default=12)
    ap.add_argument("--speed-clip", default="foreman_cif",
                    help="clip used for the per-tier speed ratio")
    ap.add_argument("--no-cache", action="store_true")
    ap.add_argument("--out", default=None, help="write the markdown report here")
    args = ap.parse_args()

    for b, name in ((YAH264, "yah264"), (X264 if args.ref == "x264" else X265, args.ref)):
        if not (os.path.exists(b) or subprocess.run(["which", b], capture_output=True).returncode == 0):
            sys.exit(f"ladder: {name} binary not found: {b}")

    clips = clips_for(args)
    presets = args.presets.split(",")
    lines = []
    lines.append(f"# yah264 vs {args.ref}: preset-ladder BD-rate + speed")
    lines.append("")
    lines.append(f"Corpus: {', '.join(clips)}  |  {args.frames}f  |  CRF {args.points}  "
                 f"|  ref: {os.path.basename(X264 if args.ref=='x264' else X265)}")
    lines.append("BD-rate = yah264 vs " + args.ref + " at the SAME --preset; "
                 "negative = yah264 ahead (fewer bits at equal quality). "
                 f"Speed on {args.speed_clip}, yah264/{args.ref} wall-time (>1 = yah264 slower).")
    lines.append("")
    hdr = "| preset | " + " | ".join(clips) + " | mean | speed |"
    sep = "|" + "---|" * (len(clips) + 3)
    lines.append(hdr)
    lines.append(sep)

    for p in presets:
        print(f"[ladder] {p} ...", file=sys.stderr)
        bd = bd_for_preset(p, clips, args)
        ns, rs = speed_for_preset(p, args.speed_clip, args)
        cells, vals = [], []
        for c in clips:
            v = bd.get(c, {}).get("VMAF-NEG")
            if v is None:
                cells.append("n/a")
            else:
                cells.append(f"{v:+.1f}")
                vals.append(v)
        mean = f"{sum(vals)/len(vals):+.1f}" if vals else "n/a"
        speed = f"{ns/rs:.1f}x" if ns and rs else "n/a"
        lines.append(f"| {p} | " + " | ".join(cells) + f" | **{mean}** | {speed} |")

    lines.append("")
    lines.append("**Reading this table:** medium (and slower) is the true "
                 "apples-to-apples comparison — same tool-set at the same tier. The "
                 "FAST tiers are NOT tool-aligned with " + args.ref + ": yah264's "
                 "preset scales search effort (subme/subpel/ref/lookahead) but keeps "
                 "CABAC + 8x8dct + B-frames on at every tier, whereas " + args.ref +
                 "'s fast/ultrafast STRIP those tools. So a strongly-negative BD at "
                 "ultrafast/veryfast means yah264 is spending more compute for higher "
                 "quality at that tier (see the speed column), not beating " + args.ref +
                 " at equal work. bus is the known inherent lowres-zoom gap "
                 "(docs/archive/mbtree-wholebuf-design.md). Speed is the as-shipped SIMD number "
                 "(x264 NEON asm vs yah264 NEON intrinsics); the pure-C algorithmic "
                 "gap is smaller (see docs/pure-c-speed-parity.md).")
    report = "\n".join(lines)
    print("\n" + report)
    if args.out:
        with open(args.out, "w") as f:
            f.write(report + "\n")
        print(f"\n[ladder] wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
