#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""frame_stats_check.py - --frame-stats against the stream it describes.

    python3 tests/frame_stats_check.py path/to/cli/yah264

On a synthetic 176x144 clip, no corpus and no ffmpeg needed, for frame
pictures, a sliced encode and --tff/--bff field coding, on the GOP-parallel
path and the one-instance path (--dump-recon):

- one line per input frame, every frame named once;
- the lines' bytes sum to the stream's slice NAL bytes, start codes included,
  so a sliced or field-coded frame counts all of its pictures;
- field-coded lines carry both fields, whose bytes sum to the line's, and a
  frame-coded line carries no "fields" key.
"""
import json, os, subprocess, sys, tempfile

W, H, NF = 176, 144, 30


def clip(path):
    with open(path, "wb") as f:
        f.write(b"YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C420jpeg\n" % (W, H))
        for n in range(NF):
            f.write(b"FRAME\n")
            y = bytearray(W * H)
            for r in range(H):
                for c in range(W):
                    y[r * W + c] = ((c + 2 * n) * 7 + (r + n) * 5 + ((c * r + n) * 31 >> 5)) & 0xFF
            f.write(bytes(y))
            f.write(bytes([128 + (n & 15)]) * (W * H // 4))
            f.write(bytes([128 - (n & 15)]) * (W * H // 4))


def slice_bytes(path):
    """Bytes of the type 1 and 5 NALs, each from its start code to the next."""
    data = open(path, "rb").read()
    starts, i = [], 0
    while True:
        i = data.find(b"\x00\x00\x01", i)
        if i < 0:
            break
        starts.append(i - 1 if i > 0 and data[i - 1] == 0 else i)
        i += 3
    total = 0
    for k, s in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(data)
        hdr = data[s + (4 if data[s + 2] == 0 else 3)]
        if hdr & 0x1F in (1, 5):
            total += end - s
    return total


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    enc = sys.argv[1]
    fails = 0
    cells = [("frames", []), ("slices", ["--slices", "3"]),
             ("tff", ["--tff"]), ("bff", ["--bff"]), ("tff-sliced", ["--tff", "--slices", "2"]),
             ("tff-abr", ["--tff", "--bitrate", "400", "--vbv-maxrate", "400", "--vbv-bufsize", "400"])]
    with tempfile.TemporaryDirectory(prefix="fstchk") as d:
        src = os.path.join(d, "c.y4m")
        clip(src)
        for name, extra in cells:
            for path, pargs in (("gop", ["--threads", "2", "--keyint", "12"]),
                                ("serial", ["--threads", "1", "--dump-recon", os.path.join(d, "r.y4m")])):
                out, fst = os.path.join(d, "o.264"), os.path.join(d, "s.jsonl")
                for f in (out, fst):        # a stale file from the last cell must not pass
                    if os.path.exists(f):
                        os.unlink(f)
                r = subprocess.run([enc, "--input-y4m", src, "--bframes", "2",
                                    "--no-progress", "-o", out, "--frame-stats", fst] + pargs
                                   + (extra if "--bitrate" in extra else ["--crf", "26"] + extra),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                tag = f"{name}/{path}"
                if r.returncode != 0:
                    print(f"FAIL {tag}: exit {r.returncode}: {r.stderr.decode()[-300:]}")
                    fails += 1
                    continue
                lines = [json.loads(l) for l in open(fst)]
                frames = sorted(l["frame"] for l in lines)
                got, want = sum(l["bytes"] for l in lines), slice_bytes(out)
                field = name.startswith(("tff", "bff"))
                bad = []
                if frames != list(range(NF)):
                    bad.append(f"{len(lines)} lines naming {frames[:8]}..., want frames 0..{NF - 1}")
                if got != want:
                    bad.append(f"bytes sum to {got}, the stream's slices hold {want}")
                for l in lines:
                    fl = l.get("fields")
                    if not field and fl is not None:
                        bad.append(f"frame {l['frame']} has fields on a frame-coded encode")
                    elif field and (fl is None or len(fl) != 2 or sum(x["bytes"] for x in fl) != l["bytes"]
                                    or fl[0]["type"] != l["type"]
                                    or any(x["type"] not in "IPB" or x["bytes"] <= 0 for x in fl)):
                        bad.append(f"frame {l['frame']}: fields {fl} against bytes {l['bytes']} type {l['type']}")
                    if bad:
                        break
                if bad:
                    print(f"FAIL {tag}: " + "; ".join(bad))
                    fails += 1
                else:
                    print(f"ok   {tag}: {len(lines)} lines, {got} slice bytes")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
