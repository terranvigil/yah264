#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""gop_drop_check.py - a GOP-parallel encode from a file codes every frame.

    python3 tests/gop_drop_check.py path/to/cli/yah264 [corpus-dir]

The seek reader starts before the GOP schedule is published, and it must
not lose its place when the schedule replaces display order with the
longest-first queue. These inputs open on a short GOP followed by a longer
one, the shape where a reader that loses its place leaves the longest GOP
unread. Each case counts the coded frames in the output and wants every input
frame and a clean exit, at two and three threads, a few runs each, since how
far the reader gets before the publish is a matter of timing.

The input is spliced on the fly from CIF corpus clips (mobile, foreman,
bus) when they are present, else from synthetic QCIF segments, which have the
same GOP shape.
"""
import os, subprocess, sys, tempfile

RUNS = 3


def read_y4m(path, count):
    """the header line and the first `count` frame payloads of a Y4M file"""
    with open(path, "rb") as f:
        hdr = f.readline()
        tok = dict((t[:1], t[1:]) for t in hdr.split()[1:])
        w, h = int(tok[b"W"]), int(tok[b"H"])
        size = w * h * 3 // 2
        frames = []
        for _ in range(count):
            line = f.readline()
            if not line.startswith(b"FRAME"):
                raise ValueError(f"{path}: fewer than {count} frames")
            frames.append(f.read(size))
        return hdr, frames


def synth(seed, count, w=176, h=144):
    out = []
    for n in range(count):
        y = bytearray(w * h)
        for j in range(h):
            row = j * w
            for i in range(w):
                y[row + i] = (seed * i + (seed + 3) * j + 2 * n +
                              ((i * 2654435761 ^ j * 40503 ^ seed) >> 7 & 31)) & 0xff
        out.append(bytes(y) + bytes([(64 + 16 * seed) & 0xff]) * (w * h // 2))
    return out


def splice(path, parts, corpus):
    """parts: (clip name, frame count, synthetic seed)"""
    if corpus and all(os.path.exists(os.path.join(corpus, c)) for c, _, _ in parts):
        hdr, frames = None, []
        for clip, count, _ in parts:
            h, fr = read_y4m(os.path.join(corpus, clip), count)
            hdr = hdr or h
            frames += fr
    else:
        hdr = b"YUV4MPEG2 W176 H144 F25:1 Ip A1:1 C420jpeg\n"
        frames = [f for _, count, seed in parts for f in synth(seed, count)]
    with open(path, "wb") as f:
        f.write(hdr)
        for fr in frames:
            f.write(b"FRAME\n" + fr)
    return len(frames)


def coded_frames(path):
    """slices that open a picture (first_mb_in_slice 0, a leading 1 bit)"""
    data = open(path, "rb").read()
    n, i = 0, 0
    while True:
        i = data.find(b"\x00\x00\x01", i)
        if i < 0 or i + 4 >= len(data):
            return n
        nal_type = data[i + 3] & 0x1f
        if nal_type in (1, 5) and data[i + 4] & 0x80:
            n += 1
        i += 3


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit("usage: gop_drop_check.py CLI [corpus-dir]")
    cli = os.path.abspath(sys.argv[1])
    corpus = sys.argv[2] if len(sys.argv) == 3 else None
    work = tempfile.mkdtemp(prefix="yah264-gopdrop.")
    env = {k: v for k, v in os.environ.items() if not k.startswith("Y264_")}

    qp = os.path.join(work, "qp.txt")
    plan = os.path.join(work, "plan.txt")
    with open(qp, "w") as f:
        f.write("10 I -1\n")
    with open(plan, "w") as f:
        f.write("8 8 idr\n100 100 idr\n")
    a = os.path.join(work, "a.y4m")
    b = os.path.join(work, "b.y4m")
    na = splice(a, [("mobile_cif.y4m", 10, 1), ("foreman_cif.y4m", 100, 5)], corpus)
    nb = splice(b, [("foreman_cif.y4m", 8, 2), ("bus_cif.y4m", 150, 7),
                    ("mobile_cif.y4m", 100, 3)], corpus)
    cases = [
        ("qpfile key frame", a, na, ["--keyint", "40", "--qpfile", qp]),
        ("plan idr", b, nb, ["--plan", plan]),
    ]

    out = os.path.join(work, "out.264")
    fail = 0
    for name, clip, want, args in cases:
        bad = 0
        for t in ("2", "3"):
            for run in range(RUNS):
                if os.path.exists(out):
                    os.remove(out)
                p = subprocess.run([cli, "--input-y4m", clip, "--crf", "26", "--threads", t,
                                    "-o", out, "--quiet", "--no-progress"] + args,
                                   env=env, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE, text=True)
                got = coded_frames(out) if os.path.exists(out) else 0
                if p.returncode != 0 or got != want:
                    print(f"FAIL {name}, threads {t}, run {run + 1}: exit {p.returncode}, "
                          f"{got} of {want} frames coded {p.stderr.strip()[:200]}")
                    bad = 1
        fail |= bad
        if not bad:
            print(f"ok   {name}: {want} frames at threads 2 and 3, {RUNS} runs each")
    for fn in os.listdir(work):
        os.remove(os.path.join(work, fn))
    os.rmdir(work)
    sys.exit(fail)


if __name__ == "__main__":
    main()
