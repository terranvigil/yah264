#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""control_check.py - the CLI's live control (docs/live-control.md).

    python3 tests/control_check.py path/to/cli/yah264

On a synthetic 176x144 clip, no corpus and no ffmpeg needed:

1. --control files that are malformed, or name a version this build doesn't
   read, or whose frames go backwards, or carry an event the encoder refuses,
   stop the CLI with status 2 before it writes a byte, and the message names
   the file and the line.
2. a live run over --control-fd, events sent as the encode runs, some with
   "next", records its applied events with --control-out; replaying that log
   with --control gives the same stream and the same --frame-stats at one
   thread. A numbered line that arrives late is skipped with a warning, and a
   malformed one stops the encode.
3. --control with no events (a file of comments) gives the stream of an
   encode without the option, and the stats lines carry no "control" key.
"""
import json, os, subprocess, sys, tempfile, time

W, H, NF = 176, 144, 120


def clip(path):
    with open(path, "wb") as f:
        f.write(b"YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C420jpeg\n" % (W, H))
        for n in range(NF):
            f.write(b"FRAME\n")
            s = n if n < 60 else 500 + 2 * n
            y = bytearray(W * H)
            for r in range(H):
                base = r * W
                for c in range(W):
                    y[base + c] = ((c + s) * 7 + (r + s // 2) * 5 + ((c * r + s) * 31 >> 5)) & 0xFF
            f.write(bytes(y))
            f.write(bytes([128 + (n & 15)]) * (W * H // 4))
            f.write(bytes([128 - (n & 15)]) * (W * H // 4))


def run(enc, args, **kw):
    return subprocess.run([enc] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    enc = sys.argv[1]
    fails = 0
    with tempfile.TemporaryDirectory(prefix="ctlchk") as d:
        src = os.path.join(d, "c.y4m")
        clip(src)
        base = ["--input-y4m", src, "--threads", "1", "--no-progress",
                "--bitrate", "300", "--vbv-maxrate", "300", "--vbv-bufsize", "300"]

        # 1. refusals before the first byte
        bad = [
            ("10 warp\n", "unknown event kind"),
            ("10 rate\n", "needs at least one"),
            ("10 rate bitrate=x\n", "bad value"),
            ("10 key type=open\n", "takes no argument"),
            ("next key\n", "only accepted on --control-fd"),
            ("#livectl 9\n10 key\n", "unsupported log version"),
            ("20 key\n10 key\n", "must not decrease"),
            ("10 crf value=20\n", "refused"),
            ("10 rate bitrate=200 maxrate=250\n", "refused"),
            ("10 fps num=30 den=1\n", "not supported"),
        ]
        f0 = fails
        for i, (text, want) in enumerate(bad):
            cf = os.path.join(d, "bad%d.ctl" % i)
            out = os.path.join(d, "bad%d.264" % i)
            with open(cf, "w") as f:
                f.write(text)
            r = run(enc, base + ["--control", cf, "-o", out])
            err = r.stderr.decode()
            ok = r.returncode == 2 and want in err and ("%s:" % cf) in err \
                and not (os.path.exists(out) and os.path.getsize(out) > 0)
            if not ok:
                print("FAIL --control %r: status %d, stderr %r" % (text, r.returncode, err.strip()[-200:]))
                fails += 1
        if fails == f0:
            print("  ok   %d bad control logs refused before encoding" % len(bad))

        # 3. a log with no events changes nothing
        empty = os.path.join(d, "empty.ctl")
        with open(empty, "w") as f:
            f.write("#livectl 1\n# nothing\n\n")
        a, b = os.path.join(d, "a.264"), os.path.join(d, "b.264")
        fa, fb = os.path.join(d, "a.json"), os.path.join(d, "b.json")
        r1 = run(enc, base + ["-o", a, "--frame-stats", fa])
        r2 = run(enc, base + ["--control", empty, "-o", b, "--frame-stats", fb])
        same = r1.returncode == 0 and r2.returncode == 0 and open(a, "rb").read() == open(b, "rb").read()
        nokey = all("control" not in json.loads(l) for l in open(fb) if l.strip())
        if not (same and nokey):
            print("FAIL an empty --control log moved the stream or the stats")
            fails += 1
        else:
            print("  ok   an empty --control log leaves stream and stats as they were")

        # 2. live, then replay. The input is piped in a frame at a time and
        # the control lines go down their own pipe between frames, so they
        # arrive while the encode runs, wherever the reader has got to.
        live, rec = os.path.join(d, "live.264"), os.path.join(d, "live.ctl")
        lfs = os.path.join(d, "live.json")
        frames = open(src, "rb").read()
        hdr_end = frames.index(b"\n") + 1
        fsz = len(b"FRAME\n") + W * H * 3 // 2
        rd, wr = os.pipe()
        lbase = ["--input-y4m", "-"] + base[2:]
        p = subprocess.Popen([enc] + lbase + ["--control-fd", str(rd), "--control-out", rec,
                                              "--frame-stats", lfs, "-o", live],
                             pass_fds=(rd,), stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(rd)
        sched = {0: ["#livectl 1"], 20: ["next rate bitrate=150"], 40: ["next key", "0 key"],
                 60: ["next qp min=26 max=46", "next zone last=1000 offset=2"],
                 90: ["next rate bitrate=450"]}
        p.stdin.write(frames[:hdr_end])
        for n in range(NF):
            for line in sched.get(n, []):
                os.write(wr, (line + "\n").encode())
            p.stdin.write(frames[hdr_end + n * fsz: hdr_end + (n + 1) * fsz])
            p.stdin.flush()
            time.sleep(0.002)
        os.close(wr)
        p.stdin.close()
        out, err = p.communicate()
        err = err.decode()
        if p.returncode != 0:
            print("FAIL the live run exited %d: %s" % (p.returncode, err.strip()[-300:]))
            fails += 1
        else:
            if "(frame 0): skipped" not in err:
                print("FAIL the late '0 key' line was not reported as skipped: %r" % err.strip()[-300:])
                fails += 1
            log = [l.strip() for l in open(rec) if l.strip()]
            if not log or log[0] != "#livectl 1" or len(log) != 6:
                print("FAIL --control-out should hold the header and five events: %r" % log)
                fails += 1
            rp, rfs = os.path.join(d, "rep.264"), os.path.join(d, "rep.json")
            r = run(enc, base + ["--control", rec, "-o", rp, "--frame-stats", rfs])
            if r.returncode != 0 or open(rp, "rb").read() != open(live, "rb").read() \
                    or open(rfs).read() != open(lfs).read():
                print("FAIL the recorded log did not replay to the live run's bytes and stats")
                fails += 1
            else:
                print("  ok   live run over --control-fd replays byte for byte (%d events)" % (len(log) - 1))

        # a malformed live line stops the encode
        rd, wr = os.pipe()
        p = subprocess.Popen([enc] + base + ["--control-fd", str(rd), "-o", os.path.join(d, "m.264")],
                             pass_fds=(rd,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(rd)
        os.write(wr, b"next rate bitrate=150\nnext bogus\n")
        os.close(wr)
        out, err = p.communicate()
        if p.returncode == 0 or "--control-fd: line 2" not in err.decode():
            print("FAIL a malformed --control-fd line did not stop the encode (status %d)" % p.returncode)
            fails += 1
        else:
            print("  ok   a malformed --control-fd line stops the encode with its line number")

    if fails:
        print("control_check: %d failure(s)" % fails)
        return 1
    print("control_check: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
