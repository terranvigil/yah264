#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""fade_check.py - the dark-cut gate's fade test reads the same at 8 and 10 bits.

    python3 tests/fade_check.py path/to/cli/y264

Writes a synthetic QCIF fade from black (faint grain on a slow ramp, the
mean rising half a level a frame), holds it, then cuts upward, and encodes
it at --output-depth 8 and 10 with Y264_PSY_FLAT_LOG=1. The fade test reads
the lowres mean, which is on the 8-bit scale at every depth, so both depths
must mark the same frames, most of the ramp must be marked, the held frames
must not be, and the cut must not extend a run.
"""
import os, re, subprocess, sys, tempfile

W, H, RAMP, HOLD, AFTER = 176, 144, 40, 16, 16
N = RAMP + HOLD + AFTER


def frame(n):
    # the mean in half levels: the ramp climbs half a level a frame, between
    # the test's 0.2 and four times it, so a test that read the 10-bit mean on
    # the wrong scale would miss the ramp at 10 bits only
    if n < RAMP:
        half = 32 + n
    elif n < RAMP + HOLD:
        half = 32 + RAMP
    else:
        half = 32 + RAMP + 40 + (n - RAMP - HOLD)     # a cut up, then a rise
    y = bytearray(W * H)
    for j in range(H):
        for i in range(W):
            g = ((i * 7 + j * 13 + n * 5) * 2654435761 >> 13) & 3
            odd = (half & 1) and (i + j) & 1          # half the samples a level up
            y[j * W + i] = min(255, half // 2 + odd + i // 44 + g)
    return bytes(y) + bytes([128]) * (W * H // 2)


def flags(cli, src, depth, work):
    env = dict(os.environ, Y264_PSY_FLAT_LOG="1")
    r = subprocess.run([cli, "--input-y4m", src, "--crf", "24", "--threads", "1",
                        "--keyint", "250", "--output-depth", str(depth), "--no-progress",
                        "-o", os.path.join(work, f"f{depth}.264")],
                       env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"fade_check: the {depth}-bit encode failed: {r.stderr[-400:]}")
    out = {}
    for m in re.finditer(r"psyflat: poc=(\d+) .* fade=(\d)", r.stderr):
        out[int(m.group(1)) // 2] = int(m.group(2))
    return out


def main():
    cli = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="y264fade") as work:
        src = os.path.join(work, "fade.y4m")
        with open(src, "wb") as f:
            f.write(f"YUV4MPEG2 W{W} H{H} F25:1 Ip A1:1 C420jpeg\n".encode())
            for n in range(N):
                f.write(b"FRAME\n" + frame(n))
        f8, f10 = flags(cli, src, 8, work), flags(cli, src, 10, work)
    fails = []
    if len(f8) != N or len(f10) != N:
        fails.append(f"expected {N} frames logged, got {len(f8)} and {len(f10)} "
                     "(an IDR restarts POC, or the log changed)")
    elif f8 != f10:
        fails.append("8 and 10 bits mark different frames: "
                     f"{sorted(k for k in f8 if f8[k])} vs {sorted(k for k in f10 if f10[k])}")
    ramp = sum(f8.get(n, 0) for n in range(RAMP))
    if ramp < RAMP - 8:
        fails.append(f"only {ramp}/{RAMP} ramp frames marked")
    held = [n for n in range(RAMP + 1, RAMP + HOLD) if f8.get(n)]
    if held:
        fails.append(f"held frames marked: {held}")
    cut = [n for n in range(RAMP + HOLD, RAMP + HOLD + 6) if f8.get(n)]
    if cut:
        fails.append(f"the cut extended a run: {cut}")
    for m in fails:
        print("FAIL", m)
    ramp10 = sum(f10.get(n, 0) for n in range(RAMP))
    print(f"fade_check: {'FAIL' if fails else 'ok'} (ramp frames marked: {ramp}/{RAMP} at 8 bits, "
          f"{ramp10}/{RAMP} at 10)")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
