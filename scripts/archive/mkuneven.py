#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""Build a 720p Y4M with deliberately UNEVEN shot lengths.

One long shot next to many short ones -- the distribution a real scene-cut
split can produce and an arithmetic keyint split never can. Shots are cut from
a cut-free source (ducks_720p) so no shot has an INTERNAL cut, and consecutive
shots get different luma transforms so every boundary is a hard cut the
pre-scan must find.

usage: mkuneven.py <src.y4m> <dst.y4m> [long] [nshort] [short]
"""
import sys

W, H = 1280, 720
YS, CS = W * H, (W // 2) * (H // 2)
FS = YS + 2 * CS
INV = bytes(255 - i for i in range(256))

src, dst = sys.argv[1], sys.argv[2]
LONG = int(sys.argv[3]) if len(sys.argv) > 3 else 300
NSHORT = int(sys.argv[4]) if len(sys.argv) > 4 else 24
SHORT = int(sys.argv[5]) if len(sys.argv) > 5 else 25

f = open(src, 'rb')
src_hdr = f.readline()                          # Y4M stream header
base = f.tell()

# The output's frame rate is the SOURCE's, not a constant. This used to stamp a
# literal `F25:1` onto frames copied verbatim out of ducks_720p, which is F50:1,
# so tests/corpus/uneven_720p.y4m has been telling every consumer that 50fps
# content is 25fps -- a 2x error in any bits->kbit/s or frames->seconds read of
# it. Nothing downstream had noticed only because the shot-split work that uses
# this clip reports milliseconds. Copy the token across and it cannot drift.
FRATE = next((t for t in src_hdr.decode('ascii', 'replace').split()
              if t.startswith('F')), None)
if FRATE is None:
    raise SystemExit(f"mkuneven: {src} has no F<num>:<den> in its Y4M header; "
                     f"refusing to assume a frame rate")


def read_frame(i):
    f.seek(base + i * (6 + FS))                # "FRAME\n" is 6 bytes
    assert f.read(6) == b'FRAME\n', i
    return f.read(FS)


def xform(buf, mode):
    if mode == 0:
        return buf
    y, c = buf[:YS], buf[YS:]
    if mode & 1:
        y = y.translate(INV)                   # inversion: a guaranteed cut
    if mode & 2:
        r = (211 * mode) % W                   # horizontal roll
        y = b''.join(y[o + r:o + W] + y[o:o + r] for o in range(0, YS, W))
    if mode & 4:
        y = b''.join(y[o:o + W] for o in range(YS - W, -W, -W))   # vflip
    return y + c


shots = [(0, LONG, 0)] + [(LONG, SHORT, 1 + (i % 7)) for i in range(NSHORT)]

out = open(dst, 'wb')
out.write(b'YUV4MPEG2 W%d H%d %s Ip A1:1 C420jpeg\n'
          % (W, H, FRATE.encode('ascii')))
total, bounds, lens = 0, [], []
for start, n, mode in shots:
    bounds.append(total)
    lens.append(n)
    for i in range(n):
        out.write(b'FRAME\n')
        out.write(xform(read_frame(start + i), mode))
        total += 1
out.close()
print(f"{dst}: {total} frames, {len(shots)} shots")
print(f"  expected cuts at: {bounds}")
print(f"  lengths: {lens}")
