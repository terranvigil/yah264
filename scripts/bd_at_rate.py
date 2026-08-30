#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""BD-rate between two encoder configs measured at MATCHED ACHIEVED BITRATE.

The ad-hoc `bdcompare --points` sweep compares at a matched CRF *number*, which
is only sound while both arms' CRF-to-quality mapping is fixed. Any change that
translates the CRF axis -- mb-tree on/off does exactly this, since it engages
Y264_CRF_CL_SHIFT -- makes that comparison measure ladder placement instead of
the encoder (docs/archive/crf-mbtree-shift-nonresult.md).

So: for each target size, binary-search each arm's CRF onto it independently,
then BD over the resulting (bytes, VMAF) pairs. Placement cancels by
construction.
"""
import argparse, os, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bdcompare as bd

bd.SUBSAMPLE = 1


def enc(tmpl, q, src, out):
    subprocess.run(tmpl.format(q=f"{q:.4f}", src=src, out=out),
                   shell=True, capture_output=True)
    return os.path.getsize(out) if os.path.exists(out) else 0


def solve(tmpl, src, target, out, lo=10.0, hi=48.0, iters=10):
    """CRF that lands nearest `target` bytes. Monotone: higher CRF = fewer bits."""
    for _ in range(iters):
        mid = (lo + hi) / 2.0
        if enc(tmpl, mid, src, out) > target:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2.0


def curve(tmpl, src, targets, work, label):
    pts = []
    out = os.path.join(work, "s.264")
    for t in targets:
        q = solve(tmpl, src, t, out)
        sz = enc(tmpl, q, src, out)
        v = bd.vmaf_of(out, src, work)
        if not v or "VMAF-NEG" not in v:
            return None
        pts.append((sz, v["VMAF-NEG"]))
        # qp~ is MODEL-derived (our CRF staircase codes qp = crf + 5.4, flat
        # term; AQ/mb-tree modulate per MB around it) -- an annotation for
        # reading regime knobs in QP space, not an achieved measurement.
        print(f"    {label:<22} target {t:>9} -> crf {q:5.2f} (qp~{q + 5.4:4.1f})  {sz:>9} B  "
              f"NEG {v['VMAF-NEG']:6.2f}", flush=True)
    return pts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--clip", required=True)
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--targets", required=True,
                    help="comma-separated target byte sizes")
    ap.add_argument("--label-a", default="A")
    ap.add_argument("--label-b", default="B")
    args = ap.parse_args()

    src = f"tests/corpus/{args.clip}.y4m"
    targets = [int(x) for x in args.targets.split(",")]
    work = tempfile.mkdtemp(prefix="bdrate.")
    a = curve(args.a, src, targets, work, args.label_a)
    b = curve(args.b, src, targets, work, args.label_b)
    if not a or not b:
        print(f"{args.clip}: encode/VMAF failed"); return
    ra, ma = [p[0] for p in a], [p[1] for p in a]
    rb, mb = [p[0] for p in b], [p[1] for p in b]
    r = bd.bd_rate(rb, mb, ra, ma)
    print(f"  {args.clip:<15} BD-rate(VMAF-NEG) {args.label_a} vs {args.label_b}: "
          f"{r:+.2f}%" if isinstance(r, float) else f"  {args.clip}: {r}")


main()
