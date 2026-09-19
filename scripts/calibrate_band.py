#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Find, per clip, the ABR ladder whose x264-medium operating points land inside
the VMAF-NEG 88-94 band.  Fixed multipliers off a 'calibrated centre' do NOT do
this -- they put akiyo/touchdown at saturation and sintel below the band, which
makes the BD fit degenerate (touchdown read +341269%).

Method: sweep x264 CRF, measure (bitrate, VMAF-NEG) for each, then interpolate
the bitrates at four VMAF-NEG targets spanning the band.  The ladder is defined
by the REFERENCE encoder's own curve, which is what 'calibrated band' means.

A run costs hours and the ladders are gate inputs, so the tree's copies are
NOT the default destination: without --write the two json files land in a
fresh scratch directory whose path is printed before anything is encoded.
"""
import argparse, os, sys, json, math, tempfile
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import scratch                                                  # noqa: E402
import bdcompare as bd

bd.SUBSAMPLE = 1
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORPUS = os.path.join(ROOT, "tests/corpus")
FRAMES = 120
TARGETS = [88.0, 90.0, 92.0, 94.0]

LADDERS_NAME = "ladders.json"
CURVES_NAME = "curves.json"

CLIPS = ["foreman_cif", "bus_cif", "stefan_cif", "akiyo_cif", "mobile_cif",
         "coastguard_cif", "tempete_cif",
         "ducks_720p", "park_joy_720p", "fourpeople_720p", "sintel_720p",
         "touchdown_420"]

CRFS = [16, 20, 24, 28, 32, 36, 40]


def probe(clip, crf, work, frames):
    src = os.path.join(CORPUS, clip + ".y4m")
    out = os.path.join(work, f"{clip}.{crf}.264")
    cmd = (f'x264 --preset medium --crf {crf} --frames {frames} --threads 1 '
           f'--demuxer y4m -o "{out}" "{src}"')
    r = bd.sh(cmd)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    bits = os.path.getsize(out) * 8
    # kbps at the clip's real framerate, read from the y4m header
    fps = read_fps(src)
    kbps = bits / (frames / fps) / 1000.0
    v = bd.vmaf_of(out, src, work)
    os.unlink(out)
    if not v or "VMAF-NEG" not in v:
        return None
    return (crf, kbps, v["VMAF-NEG"])


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


def interp(pts, target):
    """pts: sorted list of (kbps, vmaf) ascending in vmaf. Log-linear in rate."""
    for i in range(len(pts) - 1):
        (r0, v0), (r1, v1) = pts[i], pts[i + 1]
        if v0 <= target <= v1:
            if v1 == v0:
                return r0
            t = (target - v0) / (v1 - v0)
            return math.exp(math.log(r0) + t * (math.log(r1) - math.log(r0)))
    return None


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        prog="calibrate_band.py",
        description="Solve the band (VMAF-NEG 88-94) ABR ladders onto x264 medium's "
                    "own CRF curve. Hours of encodes; nothing is written until they "
                    "finish.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Without --write, %s / %s go to a fresh scratch directory that the run\n"
               "prints. --write replaces the gate's copies in scripts/." % (LADDERS_NAME,
                                                                            CURVES_NAME))
    p.add_argument("--write", action="store_true",
                   help="replace scripts/%s and scripts/%s in the tree" % (LADDERS_NAME,
                                                                           CURVES_NAME))
    p.add_argument("--out-dir", metavar="DIR",
                   help="write the two json files here instead of a scratch directory")
    p.add_argument("--clips", metavar="A,B",
                   help="comma-separated clip subset (default: all %d)" % len(CLIPS))
    p.add_argument("--crfs", metavar="N,N",
                   help="comma-separated CRF sweep (default: %s)"
                        % ",".join(str(c) for c in CRFS))
    p.add_argument("--frames", type=int, default=FRAMES, metavar="N",
                   help="frames per encode (default: %(default)s)")
    p.add_argument("--jobs", type=int, default=8, metavar="N",
                   help="encodes in flight (default: %(default)s)")
    p.add_argument("--dry-run", action="store_true",
                   help="print the plan and the destinations, encode nothing")
    args = p.parse_args(argv)
    if args.write and args.out_dir:
        p.error("--write and --out-dir name two different destinations; pick one")
    if args.jobs < 1:
        p.error("--jobs must be at least 1")
    if args.frames < 1:
        p.error("--frames must be at least 1")
    args.clip_list = list(CLIPS) if args.clips is None else \
        [c.strip() for c in args.clips.split(",") if c.strip()]
    if not args.clip_list:
        p.error("--clips selected nothing")
    unknown = [c for c in args.clip_list if c not in CLIPS]
    if unknown:
        p.error("unknown clip(s): %s" % ", ".join(unknown))
    try:
        args.crf_list = list(CRFS) if args.crfs is None else \
            [int(c) for c in args.crfs.split(",") if c.strip()]
    except ValueError:
        p.error("--crfs takes integers")
    if len(args.crf_list) < 2:
        p.error("--crfs needs at least two points to interpolate between")
    return args


def destination(args):
    """Where the two json files go, made before the first encode runs."""
    if args.write:
        return HERE
    if args.out_dir:
        if not args.dry_run:
            os.makedirs(args.out_dir, exist_ok=True)
        return os.path.abspath(args.out_dir)
    if args.dry_run:
        return os.path.join(os.environ.get("TMPDIR", "/tmp"),
                            scratch.namespace() + "calibout.XXXXXXXX")
    # NOT registered with scratch for removal: the result is the point of the
    # run. It is named in the shared namespace so clean-scratch can sweep it.
    return tempfile.mkdtemp(prefix="%scalibout." % scratch.namespace())


def announce(args, out):
    cells = len(args.clip_list) * len(args.crf_list)
    print("calibrate_band.py: %d clips x %d CRFs x %d frames = %d x264 encodes, "
          "%d in flight" % (len(args.clip_list), len(args.crf_list), args.frames,
                            cells, args.jobs))
    print("  targets: " + ", ".join("%.0f" % t for t in TARGETS))
    for name in (LADDERS_NAME, CURVES_NAME):
        path = os.path.join(out, name)
        print("  %s %s" % ("OVERWRITE" if os.path.exists(path) else "create   ", path))
    if not args.write:
        print("  the tree's scripts/%s is untouched; pass --write to replace it"
              % LADDERS_NAME)
    sys.stdout.flush()


def main(argv=None):
    args = parse_args(argv)
    out = destination(args)
    announce(args, out)
    if args.dry_run:
        print("dry run: nothing encoded, nothing written.")
        return 0

    work = scratch.mkdtemp("calib")
    results = {}
    jobs = []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for clip in args.clip_list:
            for crf in args.crf_list:
                jobs.append((clip, crf,
                             ex.submit(probe, clip, crf, work, args.frames)))
        for clip, crf, fut in jobs:
            r = fut.result()
            if r:
                results.setdefault(clip, []).append(r)

    ladders = {}
    print("=== x264 medium curve (crf -> kbps / VMAF-NEG) ===")
    for clip in args.clip_list:
        pts = sorted(results.get(clip, []), key=lambda x: x[1])
        if not pts:
            print(f"{clip}: NO DATA")
            continue
        print(f"{clip}: " + "  ".join(f"crf{c}:{k:.0f}k/{v:.2f}" for c, k, v in pts))
        curve = [(k, v) for c, k, v in pts]
        lad = []
        for t in TARGETS:
            r = interp(curve, t)
            if r:
                lad.append(int(round(r)))
        # need all four in band, else the clip cannot be measured in this band
        if len(lad) == len(TARGETS) and len(set(lad)) == len(TARGETS):
            ladders[clip] = lad
        else:
            lo = min(v for _, v in curve); hi = max(v for _, v in curve)
            print(f"    !! cannot span {TARGETS[0]}-{TARGETS[-1]}: "
                  f"x264 VMAF-NEG range is {lo:.2f}-{hi:.2f}")
    print()
    print("=== band-calibrated ABR ladders ===")
    for c, l in ladders.items():
        print(f"{c}: {','.join(str(x) for x in l)}")
    lad_path = os.path.join(out, LADDERS_NAME)
    with open(lad_path, "w") as f:
        json.dump(ladders, f, indent=1)
    # the reference curve itself, so run_band.py stops carrying a hand-pasted
    # copy that goes stale the moment the clip set changes
    curves = {c: [[k, v] for _, k, v in sorted(results.get(c, []), key=lambda x: x[1])]
              for c in args.clip_list if results.get(c)}
    cur_path = os.path.join(out, CURVES_NAME)
    with open(cur_path, "w") as f:
        json.dump(curves, f, indent=1)
    print()
    print("wrote %s" % lad_path)
    print("wrote %s" % cur_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
