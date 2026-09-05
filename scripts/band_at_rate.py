#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""The CRF band, gated at MATCHED ACHIEVED BITRATE, over the whole corpus.

`run_band.py BANDS=crf` runs its ladder at a matched CRF NUMBER, which is only
sound while the arm leaves the CRF-to-bitrate mapping alone. An arm that moves
the operating point -- anything touching the AQ level, the pedestal or the
mb-tree strength -- measures its own ladder placement there instead of its
efficiency (the local measurement records). `bd_at_rate.py` is the tool for
that case but takes byte targets one clip at a time.

So: the same band ladders `run_band.py` gates on (scripts/ladders.json, in
kbps), converted to byte targets against each clip's own frame rate, run through
`bd_at_rate.py` per clip, several clips at a time. ARM/ARM_ARGS have the same
meaning as in run_band.py, and the A side is the arm.

    ARM='Y264_MBT_DERIVED=1' python3 scripts/band_at_rate.py
    ARM='Y264_MBT_DERIVED=1' CLIPS=bus_cif,fourpeople_720p JOBS=2 python3 scripts/band_at_rate.py
"""
import json, os, re, statistics, subprocess, sys
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FRAMES = int(os.environ.get("FRAMES", "120"))
# YAH264=<path> points the gate at a different build -- needed for anything
# behind a build option rather than an env knob (the -Dgpu builds live outside
# build/, so the default binary cannot exercise them).
N = os.environ.get("YAH264", "build/cli/yah264")

ladders = json.load(open(os.path.join(HERE, "ladders.json")))
# BANDS=band (default, the standing 88-94 gate -- numbers comparable to every
# run before this knob existed), deep (the 55-83 extension, ladders_deep.json,
# where the regime-shaped arms live), or all (both ladders concatenated per
# clip). The deep ladders exist so a deep-quant win is visible in THIS gate
# instead of needing bd.sh's fixed CRF windows with their 29-32 hole
# (docs/hf-mechanism-portfolio.md's two hand-bisected crossovers).
BANDS = os.environ.get("BANDS", "band")
if BANDS in ("deep", "all"):
    deep = json.load(open(os.path.join(HERE, "ladders_deep.json")))
    if BANDS == "deep":
        ladders = deep
    else:
        ladders = {c: sorted(set(deep.get(c, []) + ladders[c])) for c in ladders}
sel = os.environ.get("CLIPS", "")
if sel:
    want = [c.strip() for c in sel.split(",") if c.strip()]
    missing = [c for c in want if c not in ladders]
    if missing:
        sys.exit(f"no ladder for {missing}; have {list(ladders)}")
    ladders = {c: ladders[c] for c in want}

ARM = os.environ.get("ARM", "")
ARM_ARGS = os.environ.get("ARM_ARGS", "")
JOBS = int(os.environ.get("JOBS", "4"))
if not ARM and not ARM_ARGS:
    sys.exit("set ARM= and/or ARM_ARGS= : this is a self-A/B gate")


def fps_of(clip):
    """Frame rate from the Y4M header (F<num>:<den>), as a float."""
    with open(os.path.join(ROOT, "tests", "corpus", clip + ".y4m"), "rb") as f:
        hdr = f.readline().decode("latin-1")
    m = re.search(r" F(\d+):(\d+)", hdr)
    return int(m.group(1)) / int(m.group(2)) if m else 30.0


def targets_of(clip):
    """The band ladder's kbps as byte counts for the encoded frame span."""
    secs = FRAMES / fps_of(clip)
    return [int(kbps * 1000.0 / 8.0 * secs) for kbps in ladders[clip]]


BASE = (f'{N} --input-y4m {{src}} --frames {FRAMES} --preset medium --cabac '
        f'--transform-8x8 --crf {{q}} --threads 1 -o {{out}}')
arm = ((ARM + " ") if ARM else "") + BASE
if ARM_ARGS:
    arm = arm.replace(" -o {out}", " " + ARM_ARGS + " -o {out}")


def run(clip):
    cmd = ["python3", "scripts/bd_at_rate.py", "--clip", clip,
           "--frames", str(FRAMES),
           "--targets", ",".join(str(t) for t in targets_of(clip)),
           "--label-a", "arm", "--label-b", "default",
           "--a", arm, "--b", BASE]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return clip, r.stdout + r.stderr


print(f"arm:     {arm}")
print(f"default: {BASE}\n")
results = {}
with ThreadPoolExecutor(max_workers=JOBS) as ex:
    # as_completed, not map: a 720p clip takes an order of magnitude longer than
    # a CIF one, and in submission order one of those hides every result behind
    # it for an hour.
    for fut in as_completed([ex.submit(run, c) for c in ladders]):
        clip, out = fut.result()
        print(out.rstrip())
        m = re.search(r"BD-rate\(VMAF-NEG\).*?:\s*([-+][\d.]+)%", out)
        if m:
            results[clip] = float(m.group(1))
        sys.stdout.flush()

print("\n=== BD-rate at matched achieved bitrate (negative = arm better) ===")
for clip, v in sorted(results.items(), key=lambda kv: kv[1]):
    print(f"  {clip:<18} {v:+7.2f}%")
if results:
    vals = list(results.values())
    print(f"\n  median {statistics.median(vals):+.2f}%   mean {statistics.mean(vals):+.2f}%   "
          f"negative {sum(1 for v in vals if v < 0)}/{len(vals)}   worst {max(vals):+.2f}%")
