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
    os.remove(clip)
    os.rmdir(work)
    sys.exit(p.returncode)


main()
