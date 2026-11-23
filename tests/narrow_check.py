#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""narrow_check.py - narrow and short pictures.

    python3 tests/narrow_check.py path/to/cli/y264

The clips are 16x16, 16x48, 16x64, 32x48 and 48x16: one or two macroblocks
wide, and one macroblock-row tall. Each row's CABAC rate estimate is seeded
from a snapshot the row above takes at its snapshot column (wpp_snap_col), and
on a picture one macroblock wide that column is 0, the row's last. The I, P
and B row wavefronts each take it, so the cases here run all three.

A -Ddev_knobs=true build also checks what the seed means: one macroblock
wide, every row's seed must equal the state the row above ended in, compared
against a copy kept apart from the snapshot, and the encoder aborts when it
doesn't. A wrong seed that is the same at every thread count, such as the
slice's initial state, fails every group below that way.

Three groups, on synthetic clips at 8 and 10 bits:

1. wavefront == serial (needs a -Ddev_knobs=true build). The same encode at
   one thread and on one GOP worker with three frame threads
   (Y264_GOP_FORCE_G=1, Y264_GOP_FORCE_K=3) must give the same bytes: an
   all-intra encode, and IPB encodes with --keyint above 1 and B frames.
   The IPB arms run at a fixed QP and pin the designs whose output varies by
   thread count on purpose (Y264_STQ, Y264_RC_CARRY, Y264_DIRECT_AUTO,
   Y264_RCP_LAG, as conformance.sh does), so what is left to differ is the
   estimator. (CRF still moves with the frame threads, so it isn't one.)
2. the threaded path without knobs. One GOP at three threads with B frames,
   no --dump-recon, must report three frame threads, exit cleanly and decode
   to every frame.
3. recon-match. Every narrow encode, CABAC and CAVLC, one and three threads,
   decodes under ffmpeg to the encoder's own reconstruction.

Without dev knobs group 1 and the seed check are skipped. Groups 2 and 3 at
three threads still reach the wavefronts, but by themselves they can't see a
wrong seed: under -Db_sanitize=address,undefined (`meson test -C build-san
narrow`) they are the out-of-range context check, since every group fails on
a sanitizer report.

Skips (exit 77) when ffmpeg is absent, since it makes the clips and decodes.
"""
import hashlib, os, re, shlex, shutil, subprocess, sys, tempfile

WRAP = shlex.split(os.environ.get("YAH264_WRAP", ""))
# sanitizer reports, and the dev-build seed check
SAN = ("runtime error:", "ERROR: AddressSanitizer", "isn't seeded from")
NF = 12
PINS = {"Y264_STQ": "0", "Y264_RC_CARRY": "0", "Y264_DIRECT_AUTO": "0", "Y264_RCP_LAG": "0"}
K3 = {"Y264_GOP_FORCE_G": "1", "Y264_GOP_FORCE_K": "3"}


def clip(ff, d, w, h, bd):
    p = os.path.join(d, f"n{w}x{h}_{bd}.y4m")
    pf = "yuv420p" if bd == 8 else "yuv420p10le"
    subprocess.run([ff, "-v", "error", "-y", "-f", "lavfi", "-i",
                    f"testsrc2=size={w}x{h}:rate=25", "-frames:v", str(NF),
                    "-pix_fmt", pf, "-strict", "-1", p], check=True)
    return p


def encode(cli, src, out, args, env=None, recon=None):
    """-> (exit code, sanitizer lines, frame threads the CLI reported or 0)"""
    if os.path.exists(out):
        os.remove(out)
    cmd = WRAP + [cli, "--input-y4m", src, "-o", out] + args
    if recon:
        cmd += ["--dump-recon", recon]
    e = {k: v for k, v in os.environ.items() if not k.startswith("Y264_")}
    e.update(env or {})
    r = subprocess.run(cmd, capture_output=True, text=True, env=e, timeout=300)
    san = [l for l in r.stderr.splitlines() if any(s in l for s in SAN)]
    m = re.search(r"x (\d+) frame-thread", r.stderr)
    return r.returncode, san, int(m.group(1)) if m else 0


def md5_of(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def frames_md5(data, size):
    return [hashlib.md5(data[i:i + size]).hexdigest()
            for i in range(0, len(data), size)]


def recon_frames(path, size):
    with open(path, "rb") as f:
        f.readline()
        out = []
        while f.readline():
            out.append(hashlib.md5(f.read(size)).hexdigest())
    return out


def decode(ff, path, pf):
    return subprocess.run([ff, "-v", "error", "-i", path, "-f", "rawvideo",
                           "-pix_fmt", pf, "-"], capture_output=True).stdout


def main():
    cli = sys.argv[1]
    ff = shutil.which("ffmpeg")
    if not ff:
        print("narrow_check: no ffmpeg, skipped")
        return 77
    ver = subprocess.run(WRAP + [cli, "--version"], capture_output=True, text=True)
    dev = "(dev knobs)" in ver.stdout + ver.stderr
    bad = 0

    def report(ok, tag, why=""):
        nonlocal bad
        bad += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {tag}{(': ' + why) if why and not ok else ''}")

    with tempfile.TemporaryDirectory(prefix="y264narrow-") as d:
        clips = {(w, h, bd): clip(ff, d, w, h, bd)
                 for (w, h) in ((16, 16), (16, 48), (16, 64), (32, 48), (48, 16))
                 for bd in (8, 10)}
        out, out3 = os.path.join(d, "o.264"), os.path.join(d, "o3.264")

        # 1. wavefront == serial
        arms = (("all-intra", ["--qp", "26", "--keyint", "1"], {}),
                ("IPB qp", ["--qp", "26", "--keyint", "6", "--bframes", "2"], PINS),
                ("IPB flat-B", ["--qp", "30", "--keyint", "12", "--bframes", "3",
                                "--b-pyramid", "none", "--ref", "3"], PINS))
        if not dev:
            print("SKIP wavefront == serial and the seed check: both need a "
                  "-Ddev_knobs=true build; without them this test can't see a wrong "
                  "seed except under a sanitizer")
        for (w, h, bd), src in clips.items() if dev else ():
            if w > 32 or h < 48:
                continue
            for name, args, pins in arms:
                tag = f"{w}x{h} {bd}-bit {name}: wavefront == serial"
                rc1, san1, _ = encode(cli, src, out, args + ["--threads", "1"], pins)
                rc3, san3, ft = encode(cli, src, out3, args + ["--threads", "3"], {**pins, **K3})
                if rc1 or rc3 or san1 or san3:
                    report(False, tag, f"exit {rc1}/{rc3} {(san1 + san3)[:1]}")
                elif ft != 3:
                    report(False, tag, f"ran {ft} frame thread(s), not 3")
                else:
                    report(md5_of(out) == md5_of(out3), tag, "streams differ")

        # 2. the threaded path, no knobs
        for (w, h, bd), src in clips.items():
            pf = "yuv420p" if bd == 8 else "yuv420p10le"
            size = w * h * 3 // 2 * (1 if bd == 8 else 2)
            for ent in ("cabac", "cavlc"):
                tag = f"{w}x{h} {bd}-bit {ent} one GOP at t3"
                rc, san, ft = encode(cli, src, out3, ["--crf", "23", "--" + ent, "--bframes", "2",
                                                      "--threads", "3"])
                if rc or san:
                    report(False, tag, f"exit {rc} {san[:1]}")
                elif ft != 3:
                    report(False, tag, f"ran {ft} frame thread(s), not 3")
                else:
                    n = len(frames_md5(decode(ff, out3, pf), size))
                    report(n == NF, tag, f"decodes to {n} of {NF} frames")

        # 3. recon-match
        for (w, h, bd), src in clips.items():
            pf = "yuv420p" if bd == 8 else "yuv420p10le"
            size = w * h * 3 // 2 * (1 if bd == 8 else 2)
            for ent in ("cabac", "cavlc"):
                for t in (1, 3):
                    rec = os.path.join(d, "r.y4m")
                    if os.path.exists(rec):
                        os.remove(rec)
                    tag = f"{w}x{h} {bd}-bit {ent} t{t} recon-match"
                    rc, san, _ = encode(cli, src, out,
                                        ["--crf", "23", "--" + ent, "--threads", str(t)], recon=rec)
                    if rc or san:
                        report(False, tag, f"exit {rc} {san[:1]}")
                        continue
                    a = recon_frames(rec, size)
                    report(len(a) == NF and a == frames_md5(decode(ff, out, pf), size), tag,
                           "decode differs from the reconstruction")
    print(f"narrow_check: {bad} failure(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
