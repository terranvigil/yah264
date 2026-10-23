#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fit the HD band's per-clip null spread from the point cache.

Every hd_band.py run used to carry two near-null control arms
(Y264_MBT_DEPFLOOR=16 and =8), ten curves a clip, to learn on the spot how far
two changes that intend nothing read apart. The point cache already holds those
reads from every band that ran them, on many binaries, so the spread is a
stored property of each clip and not a thing to re-measure per band. This
script fits it: for every binary with a default curve and a control curve on
the same clip, preset, frame count and rungs, it takes the control's
BD-VMAF-NEG against the default, the way the band scores an arm, and pools
those reads per clip into a mean and a standard deviation.

The fit is written to $Y264_CACHE/hdband/null.json with the date, the binaries
it pooled and a reference binary (YAH264, or --ref). The band reads it, prints
"inside null spread" from it, and uses the reference binary's default curve to
decide per clip whether the defaults have changed since the fit: when the
current anchor's bytes differ from the reference's on any rung, that clip runs
ONE live control again. The reference binary's default curve comes from the
point cache when a band has run on it, and is otherwise encoded here (bytes
only, five encodes a clip, no VMAF; --no-encode skips that).

    python3 scripts/hd_null.py                      # fit, ref = build/cli/yah264
    YAH264=/path/to/yah264 python3 scripts/hd_null.py
    python3 scripts/hd_null.py --show               # print the stored fit
"""
import argparse, datetime, hashlib, json, math, os, shlex, statistics, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import scratch                                                  # noqa: E402
import bdcompare as bd                                          # noqa: E402

NULL_ENVS = ("Y264_MBT_DEPFLOOR=16", "Y264_MBT_DEPFLOOR=8")


def sha12(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:12]


def rung_overlap(rr, rt):
    """Rungs the shared log-rate range spans; hd_band.overlap()'s count."""
    lo, hi = max(min(rr), min(rt)), min(max(rr), max(rt))
    ulo, uhi = min(min(rr), min(rt)), max(max(rr), max(rt))
    span = math.log(uhi) - math.log(ulo)
    if hi <= lo or span <= 0:
        return 0
    frac = (math.log(hi) - math.log(lo)) / span
    return max(0, min(len(rr), round(frac * (len(rr) - 1)) + 1))


def fit(points, pkey, frames, rungs, envs, min_rungs):
    """{clip: [(lib, env, bd), ...]} over every binary that has both curves."""
    curves = {}
    # a VMAF-NEG cell has no gate suffix; a v1 cell ends in ":VMAF-v1" (hd_band._key)
    gate = os.environ.get("VMAF_GATE", "VMAF-NEG")
    for k, v in points.items():
        if gate != "VMAF-NEG":
            if not k.endswith(":" + gate):
                continue
            k = k[:-len(gate) - 1]
        elif k.endswith(":VMAF-v1"):
            continue
        head, _, q = k.rpartition(":")
        parts = head.split(":", 4)
        if len(parts) != 5:
            continue
        lib, pk, fr, clip, env = parts
        if pk != pkey or fr != str(frames):
            continue
        curves.setdefault((lib, clip, env), {})[float(q)] = v
    reads, seen = {}, set()
    for (lib, clip, env), cur in sorted(curves.items()):
        if env not in envs:
            continue
        anc = curves.get((lib, clip, ""))
        if not anc or any(q not in cur or q not in anc for q in rungs):
            continue
        rr = [anc[q][0] for q in rungs]
        mr = [anc[q][1] for q in rungs]
        rt = [cur[q][0] for q in rungs]
        mt = [cur[q][1] for q in rungs]
        if rung_overlap(rr, rt) < min_rungs:
            continue
        # Two binaries with the same output on this clip give the same read
        # twice, and a lossless series of builds would otherwise pool one
        # measurement ten times and shrink the spread to nothing.
        fp = (clip, env, tuple(rr), tuple(rt))
        if fp in seen:
            continue
        seen.add(fp)
        r = bd.bd_rate(rr, mr, rt, mt)
        if isinstance(r, float):
            reads.setdefault(clip, []).append((lib, env, r))
    return reads


def anchor_bytes(ref, clip, rungs, args):
    """The reference binary's default-curve bytes on one clip, encoded here
    (no VMAF) when the point cache has none. The command line is the band's
    own, so the bytes are the ones the band will compare against."""
    tools = "" if args.preset == "ultrafast" else "--cabac --transform-8x8 "
    src = os.path.join(ROOT, "tests", "corpus", clip + ".y4m")
    work = scratch.mkdtemp("hdnull")
    out = os.path.join(work, "a.264")

    def one(q):
        o = f"{out}.{q:g}"
        subprocess.run(f"{shlex.quote(ref)} --input-y4m {shlex.quote(src)} --frames {args.frames} "
                       f"--preset {args.preset} {tools}--crf {q:.4f} --threads {args.threads} "
                       f"-o {shlex.quote(o)}", shell=True, capture_output=True, cwd=ROOT)
        return os.path.getsize(o) if os.path.exists(o) else 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        sizes = list(ex.map(one, rungs))
    scratch.release(work)
    return sizes if all(sizes) else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", help="reference binary (default: $YAH264 or build/cli/yah264)")
    ap.add_argument("--preset", default=os.environ.get("PRESET", "medium"))
    ap.add_argument("--threads", type=int, default=int(os.environ.get("THREADS", "1")))
    ap.add_argument("--frames", type=int, default=int(os.environ.get("FRAMES", "120")))
    ap.add_argument("--rungs", default=os.environ.get("RUNGS", "23,26,29,32,35"))
    ap.add_argument("--min-rungs", type=int, default=int(os.environ.get("MIN_RUNGS", "4")))
    ap.add_argument("--min-reads", type=int, default=4,
                    help="clips with fewer control reads are left out of the fit")
    ap.add_argument("--show", action="store_true", help="print the stored fit and stop")
    ap.add_argument("--no-encode", action="store_true",
                    help="never encode the reference anchor; clips without one in the "
                         "point cache then always run a live control")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("JOBS", "4")))
    args = ap.parse_args()

    cache = scratch.cache_dir("hdband")
    # a v1 null is its own file: the NEG fit stays what the NEG band reads
    gate = os.environ.get("VMAF_GATE", "VMAF-NEG")
    out_path = os.path.join(cache, "null.json" if gate == "VMAF-NEG" else f"null-{gate}.json")
    try:
        stored = json.load(open(out_path))
    except (OSError, ValueError):
        stored = {}
    rungs = [float(x) for x in args.rungs.split(",")]
    pkey = args.preset if args.threads == 1 else f"{args.preset}@t{args.threads}"
    fkey = f"{pkey}:{args.frames}:{','.join(f'{q:g}' for q in rungs)}"

    if args.show:
        f = stored.get(fkey)
        if not f:
            sys.exit(f"hd_null: no stored fit for {fkey} in {out_path}")
        print(f"fit {fkey}: {f['date']}, ref binary {f['ref_lib']}, "
              f"{len(f['libs'])} binaries pooled")
        for clip, c in sorted(f["clips"].items()):
            print(f"  {clip:<22} n {c['n']:>3}  centre {c['centre']:+.2f}  rsd {c['rsd']:.2f}  "
                  f"(mean {c['mean']:+.2f} sd {c['sd']:.2f})  range {c['min']:+.2f}..{c['max']:+.2f}"
                  + ("" if clip in f["anchor"] else "  (no ref anchor)"))
        return

    ref = args.ref or os.environ.get("YAH264", "build/cli/yah264")
    ref = ref if os.path.isabs(ref) else os.path.join(ROOT, ref)
    ref_lib = sha12(ref)
    points = json.load(open(os.path.join(cache, "points.json")))
    reads = fit(points, pkey, args.frames, rungs, NULL_ENVS, args.min_rungs)

    clips, anchor, libs = {}, {}, set()
    for clip, rs in sorted(reads.items()):
        vals = [r for _, _, r in rs]
        if len(vals) < args.min_reads:
            print(f"  {clip:<22} {len(vals)} reads: under {args.min_reads}, left out")
            continue
        libs.update(lib for lib, _, _ in rs)
        # The centre and width the band uses are the robust pair, the median
        # and 1.4826 x the median absolute deviation: the pool mixes binaries
        # weeks apart, and one odd read (fourpeople_720p has a -0.95 among
        # reads otherwise inside -0.3..+0.2) should not set a clip's floor.
        med = statistics.median(vals)
        rsd = 1.4826 * statistics.median([abs(v - med) for v in vals])
        clips[clip] = {"n": len(vals), "centre": med, "rsd": rsd,
                       "mean": statistics.mean(vals), "sd": statistics.stdev(vals),
                       "min": min(vals), "max": max(vals)}
        a = [points.get(f"{ref_lib}:{pkey}:{args.frames}:{clip}::{q:.4f}") for q in rungs]
        if all(a):
            anchor[clip] = [x[0] for x in a]
        elif not args.no_encode and os.path.exists(
                os.path.join(ROOT, "tests", "corpus", clip + ".y4m")):
            got = anchor_bytes(ref, clip, rungs, args)
            if got:
                anchor[clip] = got
    if not clips:
        sys.exit(f"hd_null: no clip has {args.min_reads}+ control reads for {fkey}")
    stored[fkey] = {"date": datetime.date.today().isoformat(), "ref_lib": ref_lib,
                    "ref_path": ref, "envs": list(NULL_ENVS), "libs": sorted(libs),
                    "clips": clips, "anchor": anchor}
    tmp = f"{out_path}.{os.getpid()}.tmp"
    with open(tmp, "w") as f:
        json.dump(stored, f, indent=1)
    os.replace(tmp, out_path)
    print(f"fit {fkey}: {len(clips)} clips from {len(libs)} binaries, ref {ref_lib}, "
          f"written to {out_path}")
    for clip, c in clips.items():
        print(f"  {clip:<22} n {c['n']:>3}  centre {c['centre']:+.2f}  rsd {c['rsd']:.2f}  "
              f"(mean {c['mean']:+.2f} sd {c['sd']:.2f})  range {c['min']:+.2f}..{c['max']:+.2f}"
              + ("" if clip in anchor else "  (ref binary has no default curve here)"))


if __name__ == "__main__":
    main()
