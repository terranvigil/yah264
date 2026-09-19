#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The HD band: BD-VMAF-NEG at matched achieved bitrate, several arms at once.

`band_at_rate.py` answers the same question for the standing twelve-clip
ladder, which is seven CIF clips and five HD ones. The clips engineers test
with are 720p and 1080p at the bitrates people use, and docs/hd-parity-plan.md
makes that band the bar every stage-2 candidate is read against. This is that
band, with two differences that are about cost rather than method:

- The default side is the ANCHOR. Its five rungs are five fixed CRF points, so
  its bytes need no solving, and every arm is solved onto the bytes it
  produced. One default curve serves every arm in the run, where a
  `band_at_rate.py` per arm re-solves it each time.
- Each arm's solve is BRACKETED around the anchor CRF rather than bisected from
  10 to 48. A candidate that moved the byte count by a percent has moved the
  CRF by about a tenth of a point, so a window of +/-1.5 holds it with room to
  spare. A solve that lands on either edge of its window is reported as
  UNBRACKETED and not scored -- the saving must not be able to fake a result.

    ARMS='c1=Y264_B_PREME_SKIP=1;c3=Y264_RD_SURV_RANK=1' python3 scripts/hd_band.py
    ARMS='c1=Y264_B_PREME_SKIP=1' CLIPS=sunflower_1080p JOBS=4 python3 scripts/hd_band.py
"""
import os, re, statistics, subprocess, sys
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import scratch                                                  # noqa: E402
import bdcompare as bd

bd.SUBSAMPLE = 1

# Six 720p and six 1080p, weighted to live action, and carrying the four clips
# `make review` prints plus the two cells the stage-0 profile was taken on.
BAND = ["bbb_720p", "perseverance_720p", "park_joy_720p",
        "shields_720p", "fourpeople_720p", "ducks_720p",
        "bbb10s_1080p_o120", "perseverance_1080p", "sunflower_1080p",
        "pedestrian_1080p", "riverbed_1080p", "crowd_run_1080p"]
# Five rungs. HD at low-to-mid rate is the regime the plan names, so the ladder
# sits where those clips are actually shipped rather than at the standing band's
# near-transparent top.
RUNGS = [float(x) for x in os.environ.get("RUNGS", "23,26,29,32,35").split(",")]
FRAMES = int(os.environ.get("FRAMES", "120"))
JOBS = int(os.environ.get("JOBS", "4"))
N = os.environ.get("YAH264", "build/cli/yah264")
WIN = float(os.environ.get("WINDOW", "1.5"))
ITERS = int(os.environ.get("ITERS", "7"))

BASE = (f'{N} --input-y4m {{src}} --frames {FRAMES} --preset medium --cabac '
        f'--transform-8x8 --crf {{q}} --threads 1 -o {{out}}')

clips = [c.strip() for c in os.environ.get("CLIPS", "").split(",") if c.strip()] or BAND
arms = []
for spec in os.environ.get("ARMS", "").split(";"):
    spec = spec.strip()
    if not spec:
        continue
    name, _, env = spec.partition("=")
    arms.append((name, env))
if not arms:
    sys.exit("set ARMS='name=ENV=VAL[ ENV2=VAL2];name2=...' : this is a self-A/B gate")


def enc(env, q, src, out):
    cmd = BASE.format(q=f"{q:.4f}", src=src, out=out)
    if env:
        cmd = env + " " + cmd
    subprocess.run(cmd, shell=True, capture_output=True, cwd=ROOT)
    return os.path.getsize(out) if os.path.exists(out) else 0


def solve(env, src, target, out, centre):
    """The arm's CRF landing nearest `target` bytes, inside a window around the
    anchor. Returns (crf, bracketed).

    The window is not probed at its edges first. Bisecting it directly costs
    two encodes less, and a solve that ran out of window announces itself
    anyway: it converges onto an edge, so a result within one step of either
    end is reported UNBRACKETED and dropped rather than scored."""
    lo, hi = centre - WIN, centre + WIN
    step = (hi - lo) / (1 << ITERS)
    for _ in range(ITERS):
        mid = (lo + hi) / 2.0
        if enc(env, mid, src, out) > target:
            lo = mid
        else:
            hi = mid
    q = (lo + hi) / 2.0
    return q, (q > centre - WIN + 2 * step and q < centre + WIN - 2 * step)


def run(clip):
    src = os.path.join(ROOT, "tests", "corpus", clip + ".y4m")
    work = scratch.mkdtemp("hdband")
    out = os.path.join(work, "s.264")
    lines, anchor = [], []
    for q in RUNGS:
        sz = enc("", q, src, out)
        v = bd.vmaf_of(out, src, work)
        if not sz or not v or "VMAF-NEG" not in v:
            return clip, [f"{clip}: default rung {q} failed"], {}
        anchor.append((q, sz, v["VMAF-NEG"]))
        lines.append(f"    {clip:<20} default crf {q:5.2f}  {sz:>9} B  NEG {v['VMAF-NEG']:6.2f}")
    res = {}
    for name, env in arms:
        pts, ok = [], True
        for q0, target, _ in anchor:
            q, bracketed = solve(env, src, target, out, q0)
            sz = enc(env, q, src, out)
            v = bd.vmaf_of(out, src, work)
            if not v or "VMAF-NEG" not in v:
                ok = False
                break
            pts.append((sz, v["VMAF-NEG"]))
            lines.append(f"    {clip:<20} {name:<8} crf {q:5.2f}  {sz:>9} B  "
                         f"NEG {v['VMAF-NEG']:6.2f}  ({100.0 * sz / target - 100:+.2f}% of target)"
                         + ("" if bracketed else "  UNBRACKETED"))
            if not bracketed:
                ok = False
        if not ok:
            lines.append(f"  {clip:<20} {name}: not scored")
            continue
        r = bd.bd_rate([a[1] for a in anchor], [a[2] for a in anchor],
                       [p[0] for p in pts], [p[1] for p in pts])
        if isinstance(r, float):
            res[name] = r
            lines.append(f"  {clip:<20} {name:<8} BD-VMAF-NEG {r:+.2f}%")
    return clip, lines, res


print(f"band:  {len(clips)} clips, rungs {RUNGS}, {FRAMES} frames")
for name, env in arms:
    print(f"arm {name}: {env}")
print()
allres = {name: {} for name, _ in arms}
with ThreadPoolExecutor(max_workers=JOBS) as ex:
    for fut in as_completed([ex.submit(run, c) for c in clips]):
        clip, lines, res = fut.result()
        print("\n".join(lines), flush=True)
        for name, v in res.items():
            allres[name][clip] = v

print("\n=== BD-VMAF-NEG at matched achieved bitrate (negative = arm better) ===")
for name, _ in arms:
    r = allres[name]
    if not r:
        print(f"  {name}: nothing scored")
        continue
    vals = list(r.values())
    print(f"\n  {name}")
    for clip, v in sorted(r.items(), key=lambda kv: kv[1]):
        print(f"    {clip:<20} {v:+7.2f}%")
    worst = max(r.items(), key=lambda kv: kv[1])
    print(f"    median {statistics.median(vals):+.2f}%   mean {statistics.mean(vals):+.2f}%   "
          f"worst {worst[0]} {worst[1]:+.2f}%   scored {len(vals)}/{len(clips)}")
