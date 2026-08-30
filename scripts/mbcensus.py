#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""Decoder-side P-frame MB census via ffmpeg -debug mb_type.

Usage: mbcensus.py file.264
Prints: frames, total P MBs, skip%, intra%, coded-inter count.
"""
import re, subprocess, sys

f = sys.argv[1]
# -threads 1: a threaded decode interleaves the debug log; the grid parse then
# captures a run-dependent subset of rows AND can attribute one frame type's
# rows to another (found 08-29 building scripts/b_census.py on this parse).
# Serial decode gives full, deterministic coverage.
r = subprocess.run(
    ["ffmpeg", "-v", "debug", "-threads", "1", "-debug", "mb_type",
     "-i", f, "-f", "null", "-"],
    capture_output=True, text=True)
lines = r.stderr.splitlines()

ftype = None
counts = {}
pframes = 0
row_re = re.compile(r"\[h264 @ [^\]]+\]\s+\d+ (.*)$")
hdr_re = re.compile(r"New frame, type: (\w)")
grid_re = re.compile(r"\[h264 @ [^\]]+\]\s+0\s+128\s+")
in_grid = False
for ln in lines:
    m = hdr_re.search(ln)
    if m:
        ftype = m.group(1)
        if ftype == "P":
            pframes += 1
        in_grid = False
        continue
    if grid_re.search(ln):
        in_grid = True
        continue
    if ftype != "P" or not in_grid:
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
skip = counts.get("S", 0) + counts.get("s", 0)
intra = counts.get("I", 0) + counts.get("i", 0) + counts.get("A", 0)
coded = total - skip
print(f"{f}: P-frames {pframes}  MBs {total}  "
      f"skip {skip} ({100*skip/total:.1f}%)  "
      f"intra {intra} ({100*intra/total:.2f}%)  "
      f"coded(non-skip) {coded} ({100*coded/total:.1f}%)")
print("  chars:", dict(sorted(counts.items(), key=lambda kv: -kv[1])))
