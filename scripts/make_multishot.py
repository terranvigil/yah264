#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""Build the S0 multi-shot sequences (docs/shot-based-plan.md) from the board
clips: hard cuts at known frames, mixed complexity by construction. Writes
<out>/<name>.y4m and <out>/<name>.cuts ("<first frame> <clip>" per shot) for
scripts/multishot_bd.py.

  scripts/make_multishot.py [--corpus tests/corpus] [--out local/corpus] [names...]
"""
import argparse, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEQS = {
    "ms_cif_30":   [("foreman_cif", 150), ("bus_cif", 150), ("stefan_cif", 90), ("akiyo_cif", 150), ("mobile_cif", 150)],
    "ms_720p_50":  [("ducks_720p", 150), ("park_joy_720p", 150), ("shields_720p", 150), ("in_to_tree_720p", 150), ("old_town_720p", 150)],
    "ms_1080p_25": [("sunflower_1080p", 150), ("pedestrian_1080p", 150), ("riverbed_1080p", 150), ("blue_sky_1080p", 150), ("station2_1080p", 150)],
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", default=os.path.join(ROOT, "tests", "corpus"))
    ap.add_argument("--out", default=os.path.join(ROOT, "local", "corpus"))
    ap.add_argument("names", nargs="*", default=list(SEQS))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for name in a.names:
        parts = SEQS[name]
        out = os.path.join(a.out, name + ".y4m")
        first = 0
        with open(out, "wb") as o, open(os.path.join(a.out, name + ".cuts"), "w") as cf:
            for k, (clip, n) in enumerate(parts):
                src = os.path.join(a.corpus, clip + ".y4m")
                if not os.path.exists(src):
                    sys.exit(f"missing {src} (scripts/fetch_corpus.sh)")
                with open(src, "rb") as f:
                    hdr = f.readline()
                    if not hdr.startswith(b"YUV4MPEG2"):
                        sys.exit(f"{src}: not a Y4M file")
                    tags = dict((t[:1], t[1:]) for t in hdr.split()[1:])
                    w, h = int(tags[b"W"]), int(tags[b"H"])
                    csp = tags.get(b"C", b"420jpeg")
                    if csp[:3] != b"420":
                        sys.exit(f"{src}: {csp.decode()} is not 4:2:0")
                    if k == 0:
                        o.write(hdr)          # the first clip's header names the sequence
                        W, H = w, h
                    elif (w, h) != (W, H):
                        sys.exit(f"{src}: {w}x{h} does not match {W}x{H}")
                    fsz = w * h * 3 // 2
                    for i in range(n):
                        fh = f.readline()
                        if not fh.startswith(b"FRAME"):
                            sys.exit(f"{src}: only {i} frames, {n} wanted")
                        o.write(b"FRAME\n" + f.read(fsz))
                cf.write(f"{first} {clip}\n"); first += n
        print(f"{name}: {first} frames, cuts at {[sum(n for _, n in parts[:i]) for i in range(len(parts))]}")

if __name__ == "__main__":
    main()
