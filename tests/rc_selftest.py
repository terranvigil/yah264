#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""rc_selftest.py - the rate controller's meson test, with no corpus.

    python3 tests/rc_selftest.py path/to/cli/yah264

Writes a synthetic QCIF clip (a moving textured pattern with a cut halfway,
so the VBV and the bits models see a regime change) and runs
scripts/rc_replay.py's self-test matrix on it: every rate-control mode
(CQP, CRF, ABR, capped ABR, CBR, CRF+VBV, VBR with HRD, 2-pass, the step
and QP bounds, no B frames, no pyramid, no lookahead, short keyint, slices,
PAFF, CAVLC, a faster preset) is encoded once while recording its rate-
control inputs, then replayed with the pixel work off. Every replay must
reproduce every slice QP of its recording (EXACT). That holds only if the
controller is a function of its recorded inputs and decide order, which is
the t1 determinism contract, and it exercises every mode's decide and
account path in a few seconds.
"""
import os, subprocess, sys, tempfile

W, H, N, CUT = 176, 144, 48, 24


def frame(n):
    # two textures, each drifting: x/y gradients with a hashed fine texture so
    # the encoder has residual to spend bits on at every QP
    a = 3 if n < CUT else 7
    b = 5 if n < CUT else 2
    sx, sy = 2 * n, n
    y = bytearray(W * H)
    for j in range(H):
        row = j * W
        jj = j + sy
        for i in range(W):
            ii = i + sx
            y[row + i] = (a * ii + b * jj + ((ii * 2654435761 ^ jj * 40503) >> 7 & 31)) & 0xff
    u = bytes([(96 + n) & 0xff]) * (W * H // 4) if n < CUT else bytes([160]) * (W * H // 4)
    v = bytes([128]) * (W * H // 4)
    return b"FRAME\n" + bytes(y) + u + v


CUT_CLIPS = ("shields", "park_joy", "ducks", "in_to_tree", "old_town", "stockholm", "sintel")


def make_cuts(corpus, out):
    """8 pictures from each of seven 720p corpus clips, back to back; False
    when any is missing."""
    srcs = [os.path.join(corpus, f"{c}_720p.y4m") for c in CUT_CLIPS]
    if not all(os.path.exists(p) for p in srcs):
        return False
    fs = 1280 * 720 * 3 // 2
    with open(out, "wb") as o:
        o.write(b"YUV4MPEG2 W1280 H720 F50:1 Ip A1:1 C420jpeg\n")
        for p in srcs:
            with open(p, "rb") as f:
                f.readline()
                f.seek(f.tell() + 20 * (fs + 6))
                for _ in range(8):
                    f.read(6)
                    o.write(b"FRAME\n" + f.read(fs))
    return True


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: rc_selftest.py CLI")
    cli = os.path.abspath(sys.argv[1])
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    work = tempfile.mkdtemp(prefix="yah264-rcself.")
    clip = os.path.join(work, "synth_qcif.y4m")
    with open(clip, "wb") as f:
        f.write(f"YUV4MPEG2 W{W} H{H} F25:1 Ip A1:1 C420jpeg\n".encode())
        for n in range(N):
            f.write(frame(n))
    env = dict(os.environ, BIN=cli, CLIP=clip, FRAMES=str(N))
    for k in list(env):
        if k.startswith("Y264_"):
            del env[k]
    env.pop("PLAY_BIN", None); env.pop("PLAY_ENV", None); env.pop("ONLY", None); env.pop("KEEP", None)
    p = subprocess.run([sys.executable, os.path.join(root, "scripts", "rc_replay.py")], env=env)
    rc = p.returncode
    # the --nal-hrd hold binds on real content where the synthetic clip
    # leaves it slack, and it reads the coded payload's emulation bytes, so
    # the CBR cell runs once more on a corpus clip when there is one
    corpus = os.path.join(root, "tests", "corpus")
    real = os.path.join(corpus, "foreman_cif.y4m")
    if os.path.exists(real):
        q = subprocess.run([sys.executable, os.path.join(root, "scripts", "rc_replay.py")],
                           env=dict(env, CLIP=real, FRAMES="120", ONLY="^cbr"))
        rc = rc or q.returncode
    # a cut every 8 pictures: the hold's reserve reads the next key off the
    # lookahead's cut flags, which bind there and nowhere slower
    cut = os.path.join(work, "cuts_720p.y4m")
    if make_cuts(corpus, cut):
        q = subprocess.run([sys.executable, os.path.join(root, "scripts", "rc_replay.py")],
                           env=dict(env, CLIP=cut, FRAMES="56", ONLY="^cbr"))
        rc = rc or q.returncode
        os.remove(cut)
    os.remove(clip)
    os.rmdir(work)
    sys.exit(rc)


main()
