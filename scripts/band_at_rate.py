#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
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

    ARM='Y264_CRF_LEVEL=2.0' python3 scripts/band_at_rate.py
    ARM='Y264_CRF_LEVEL=2.0' CLIPS=bus_cif,fourpeople_720p JOBS=2 python3 scripts/band_at_rate.py
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
# (two hand-bisected crossovers).
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


# KEYINT puts --keyint on BOTH arms. It exists for an arm whose whole effect is
# at the keyframe: at the default 250 a 120-frame cell has exactly one keyframe,
# its first, so such an arm reads 0.00% on every clip and the band measures
# nothing. Putting it in ARM_ARGS instead would move the keyframe cadence on one
# side only, which is two changes and not one. Unset leaves the command string
# byte-for-byte what it has always been, so every historical number stands and
# bd_at_rate's command-keyed cache still hits.
KEYINT = os.environ.get("KEYINT", "")
# THREADS reads an arm on the threaded path; 1 (the default) keeps every
# historical command string, so the cache still hits.
THREADS = int(os.environ.get("THREADS", "1"))
BASE = (f'{N} --input-y4m {{src}} --frames {FRAMES} --preset medium --cabac '
        f'--transform-8x8 --crf {{q}}'
        + (f' --keyint {int(KEYINT)}' if KEYINT else '')
        + f' --threads {THREADS} -o {{out}}')
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


# EARLY STOP, as in hd_band.py: the arm is refused, and the clips not yet
# started are dropped, once one clip reads worse than EARLY_ONE or EARLY_N
# clips worse than EARLY_SOME (percent). Either already fails a per-clip bar of
# +0.5%, so the dropped clips cannot change the verdict. EARLY_STOP=0 reads all.
EARLY_STOP = os.environ.get("EARLY_STOP", "1") == "1"
EARLY_ONE = float(os.environ.get("EARLY_ONE", "1.0"))
EARLY_SOME = float(os.environ.get("EARLY_SOME", "0.5"))
EARLY_N = int(os.environ.get("EARLY_N", "3"))

print(f"arm:     {arm}")
print(f"default: {BASE}")
print(f"vmaf models {os.environ.get('VMAF_MODELS', 'neg')}; early stop "
      + (f"on (one clip > {EARLY_ONE:+.2f}% or {EARLY_N} > {EARLY_SOME:+.2f}%)\n"
         if EARLY_STOP else "off\n"))
results, refused = {}, None
with ThreadPoolExecutor(max_workers=JOBS) as ex:
    # as_completed, not map: a 720p clip takes an order of magnitude longer than
    # a CIF one, and in submission order one of those hides every result behind
    # it for an hour.
    futs = [ex.submit(run, c) for c in ladders]
    for fut in as_completed(futs):
        if fut.cancelled():
            continue
        clip, out = fut.result()
        print(out.rstrip())
        m = re.search(r"BD-rate\(VMAF-NEG\).*?:\s*([-+][\d.]+)%", out)
        if m:
            results[clip] = float(m.group(1))
        sys.stdout.flush()
        if EARLY_STOP and not refused and m:
            v = float(m.group(1))
            some = [c for c, x in results.items() if x > EARLY_SOME]
            if v > EARLY_ONE:
                refused = f"{clip} {v:+.2f}% > {EARLY_ONE:+.2f}%"
            elif len(some) >= EARLY_N:
                refused = f"{len(some)} clips > {EARLY_SOME:+.2f}% ({', '.join(sorted(some))})"
            if refused:
                n = sum(1 for f in futs if f.cancel())
                print(f"  EARLY STOP: refused on {refused}; {n} clips not started are dropped",
                      flush=True)

print("\n=== BD-rate at matched achieved bitrate (negative = arm better) ===")
for clip, v in sorted(results.items(), key=lambda kv: kv[1]):
    print(f"  {clip:<18} {v:+7.2f}%")
if results:
    vals = list(results.values())
    print(f"\n  median {statistics.median(vals):+.2f}%   mean {statistics.mean(vals):+.2f}%   "
          f"negative {sum(1 for v in vals if v < 0)}/{len(vals)}   worst {max(vals):+.2f}%")
if refused:
    print(f"  REFUSED by early stop on {refused}; read {len(results)}/{len(ladders)} clips")
