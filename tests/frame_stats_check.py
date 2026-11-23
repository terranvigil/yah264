#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""frame_stats_check.py - --frame-stats against the stream it describes.

    python3 tests/frame_stats_check.py path/to/cli/yah264

On a synthetic CIF clip, no corpus and no ffmpeg needed, for frame
pictures, a sliced encode and --tff/--bff field coding, on the GOP-parallel
path and the one-instance path (--dump-recon):

- one line per input frame, every frame named once;
- line n matches the stream's n-th coded frame: its bytes are all of that
  frame's slice NALs, start codes included, so a sliced or field-coded frame
  counts all of its pictures, and a field-coded line's "fields" give each
  field's slice type and bytes as the stream has them. A frame-coded line
  carries no "fields" key.

Cells cover frame threads inside one instance (--threads 3 on one GOP, the
one-instance path at 3 threads, a two-pass) as well as the GOP-parallel
path.
"""
import json, os, subprocess, sys, tempfile

# CIF and 64 frames: a smaller picture gets one frame thread, and an input
# much past the CLI's 32-frame window is needed before calls start to end
# between the two fields of a pair
W, H, NF = 352, 288, 64


def clip(path, interlace=b"Ip"):
    with open(path, "wb") as f:
        f.write(b"YUV4MPEG2 W%d H%d F25:1 %s A1:1 C420jpeg\n" % (W, H, interlace))
        for n in range(NF):
            f.write(b"FRAME\n")
            y = bytearray(W * H)
            for r in range(H):
                for c in range(W):
                    y[r * W + c] = ((c + 2 * n) * 7 + (r + n) * 5 + ((c * r + n) * 31 >> 5)) & 0xFF
            f.write(bytes(y))
            f.write(bytes([128 + (n & 15)]) * (W * H // 4))
            f.write(bytes([128 - (n & 15)]) * (W * H // 4))


def _ue(data, pos):
    """(value, next bit position) of the ue(v) at bit pos."""
    z = 0
    while not data[pos >> 3] >> (7 - (pos & 7)) & 1:
        z += 1
        pos += 1
    pos += 1
    v = 0
    for _ in range(z):
        v = v << 1 | (data[pos >> 3] >> (7 - (pos & 7)) & 1)
        pos += 1
    return (1 << z) - 1 + v, pos


def stream_frames(path, ppf):
    """The stream's frames in coding order, each [bytes, [(type, bytes) per
    picture]]: slice NALs from their start code to the next, a new picture at
    every first_mb_in_slice 0, ppf pictures to a frame."""
    data = open(path, "rb").read()
    starts, i = [], 0
    while True:
        i = data.find(b"\x00\x00\x01", i)
        if i < 0:
            break
        starts.append(i - 1 if i > 0 and data[i - 1] == 0 else i)
        i += 3
    pics = []
    for k, s in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(data)
        h = s + (4 if data[s + 2] == 0 else 3)
        if data[h] & 0x1F not in (1, 5):
            continue
        first_mb, pos = _ue(data[h + 1:h + 8], 0)
        if first_mb == 0:
            st, _ = _ue(data[h + 1:h + 8], pos)
            pics.append(["PBI??"[st % 5], 0])
        pics[-1][1] += end - s
    frames = []
    for j in range(0, len(pics), ppf):
        group = pics[j:j + ppf]
        frames.append([sum(p[1] for p in group), [tuple(p) for p in group]])
    return frames


def check(lines, frames, field):
    """What's wrong with the stats lines against the stream's frames, or []."""
    bad = []
    names = sorted(l["frame"] for l in lines)
    if names != list(range(NF)):
        bad.append(f"{len(lines)} lines naming {names[:8]}..., want frames 0..{NF - 1}")
    if len(frames) != len(lines):
        bad.append(f"{len(lines)} lines for {len(frames)} coded frames")
    for n, (l, (fb, pics)) in enumerate(zip(lines, frames)):
        fl = l.get("fields")
        want = None if not field else [{"type": t, "bytes": b} for t, b in pics]
        if l["bytes"] != fb or fl != want or (field and l["type"] != pics[0][0]):
            bad.append(f"line {n} (frame {l['frame']}): bytes {l['bytes']} type {l['type']} fields {fl}, "
                       f"the stream's frame {n} holds {fb} in {pics}")
            break
    return bad


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    enc = sys.argv[1]
    fails = 0
    vbv = ["--bitrate", "400", "--vbv-maxrate", "400", "--vbv-bufsize", "400"]
    with tempfile.TemporaryDirectory(prefix="fstchk") as d:
        src, src_it = os.path.join(d, "c.y4m"), os.path.join(d, "it.y4m")
        clip(src)
        clip(src_it, b"It")
        recon, stats = os.path.join(d, "r.y4m"), os.path.join(d, "2pass.stats")
        gop = ["--threads", "2", "--keyint", "12"]
        serial = ["--threads", "1", "--dump-recon", recon]
        cells = []
        for name, extra in (("frames", []), ("slices", ["--slices", "3"]), ("tff", ["--tff"]),
                            ("bff", ["--bff"]), ("tff-sliced", ["--tff", "--slices", "2"])):
            cells += [(f"{name}/gop", gop + ["--crf", "26"] + extra, None),
                      (f"{name}/serial", serial + ["--crf", "26"] + extra, None)]
        # frame threads inside one instance: a call ends between the two
        # fields of a pair, and a frame's index can come out in another call
        cells += [("tff-abr/gop", gop + ["--tff"] + vbv, None),
                  ("tff-abr/serial", serial + ["--tff"] + vbv, None),
                  ("tff/t3", ["--threads", "3", "--keyint", "250", "--crf", "26", "--tff"], None),
                  ("tff/serial-t3", ["--threads", "3", "--dump-recon", recon, "--crf", "26", "--tff"], None),
                  ("slices/t3", ["--threads", "3", "--keyint", "250", "--crf", "26", "--slices", "4"], None),
                  ("tff-sliced/t3", ["--threads", "3", "--keyint", "250", "--crf", "26", "--tff",
                                     "--slices", "4"], None),
                  ("tff/2pass-t2", ["--threads", "2", "--tff", "--bitrate", "400", "--pass", "2",
                                    "--stats", stats],
                   ["--threads", "2", "--tff", "--bitrate", "400", "--pass", "1", "--stats", stats])]
        # an interlaced Y4M field-codes with no option asking for it
        cells.append(("y4m-It/t3", ["--threads", "3", "--keyint", "250", "--crf", "26"], None))
        for tag, args, first in cells:
            field = "--tff" in args or "--bff" in args or tag.startswith("y4m-It")
            csrc = src_it if tag.startswith("y4m-It") else src
            out, fst = os.path.join(d, "o.264"), os.path.join(d, "s.jsonl")
            for f in (out, fst):        # a stale file from the last cell must not pass
                if os.path.exists(f):
                    os.unlink(f)
            base = [enc, "--input-y4m", csrc, "--bframes", "2", "--no-progress"]
            if first:
                r = subprocess.run(base + ["-o", os.devnull] + first,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                if r.returncode != 0:
                    print(f"FAIL {tag}: pass 1 exit {r.returncode}: {r.stderr.decode()[-300:]}")
                    fails += 1
                    continue
            r = subprocess.run(base + ["-o", out, "--frame-stats", fst] + args,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if r.returncode != 0:
                print(f"FAIL {tag}: exit {r.returncode}: {r.stderr.decode()[-300:]}")
                fails += 1
                continue
            lines = [json.loads(l) for l in open(fst)]
            bad = check(lines, stream_frames(out, 2 if field else 1), field)
            if bad:
                print(f"FAIL {tag}: " + "; ".join(bad))
                fails += 1
            else:
                print(f"ok   {tag}: {len(lines)} lines, {sum(l['bytes'] for l in lines)} slice bytes")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
