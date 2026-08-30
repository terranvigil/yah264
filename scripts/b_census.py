#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""Decoder-side B-frame MB census via ffmpeg -debug mb_type.

Same parse as scripts/mbcensus.py but counts B frames, and prints the raw
char histogram so the direct/bi/L0/L1 split is visible whatever ffmpeg's
legend turns out to emit. Usage: b_census.py file.264
"""
import re, subprocess, sys

f = sys.argv[1]
# -threads 1: a threaded decode interleaves the debug log, and the grid parse
# then captures a run-dependent subset of rows (the same file read 154k MBs on
# one run and 172k on the next). A serial decode makes the census
# deterministic. scripts/mbcensus.py shares the parse and the flaw.
r = subprocess.run(
    ["ffmpeg", "-v", "debug", "-threads", "1", "-debug", "mb_type",
     "-i", f, "-f", "null", "-"],
    capture_output=True, text=True)
lines = r.stderr.splitlines()

ftype = None
counts = {}
bframes = 0
row_re = re.compile(r"\[h264 @ [^\]]+\]\s+\d+ (.*)$")
hdr_re = re.compile(r"New frame, type: (\w)")
grid_re = re.compile(r"\[h264 @ [^\]]+\]\s+0\s+128\s+")
in_grid = False
for ln in lines:
    m = hdr_re.search(ln)
    if m:
        ftype = m.group(1)
        if ftype == "B":
            bframes += 1
        in_grid = False
        continue
    if grid_re.search(ln):
        in_grid = True
        continue
    if ftype != "B" or not in_grid:
        continue
    m = row_re.match(ln)
    if not m:
        in_grid = False
        continue
    row = m.group(1)
    for i in range(0, len(row) - 1, 3):
        c = row[i]
        if c == " ":
            continue
        counts[c] = counts.get(c, 0) + 1

total = sum(counts.values())
if not total:
    print(f"{f}: no B MBs parsed"); sys.exit(0)
skip = counts.get("S", 0) + counts.get("s", 0)
intra = counts.get("I", 0) + counts.get("i", 0) + counts.get("A", 0)
direct = counts.get("d", 0) + counts.get("D", 0)
coded = total - skip
print(f"{f}: B-frames {bframes}  MBs {total}  "
      f"skip {skip} ({100*skip/total:.1f}%)  "
      f"direct(nonskip) {direct} ({100*direct/total:.2f}%)  "
      f"intra {intra} ({100*intra/total:.2f}%)  "
      f"coded(non-skip) {coded} ({100*coded/total:.1f}%)")
print("  chars:", dict(sorted(counts.items(), key=lambda kv: -kv[1])))
