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

        # out-of-order live events are judged at their own frame, a refused
        # one is skipped, and the record still replays
        oo, ool, oofs = os.path.join(d, "oo.264"), os.path.join(d, "oo.ctl"), os.path.join(d, "oo.json")
        cap = ["--input-y4m", src, "--threads", "1", "--no-progress", "--level", "4",
               "--bitrate", "800", "--vbv-maxrate", "1200", "--vbv-bufsize", "1200"]
        rd, wr = os.pipe()
        p = subprocess.Popen([enc] + cap + ["--control-fd", str(rd), "--control-out", ool,
                                            "--frame-stats", oofs, "-o", oo],
                             pass_fds=(rd,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(rd)
        os.write(wr, b"60 rate bitrate=300\n30 rate maxrate=600\n70 rate maxrate=600\n"
                     b"80 qp min=30\n40 qp max=28\n")
        os.close(wr)
        out, err = p.communicate()
        err = err.decode()
        if p.returncode != 0 or err.count("skipped") != 2:
            print("FAIL out-of-order events: status %d, %r" % (p.returncode, err.strip()[-300:]))
            fails += 1
        else:
            r = run(enc, cap + ["--control", ool, "-o", os.path.join(d, "oo2.264"),
                                "--frame-stats", os.path.join(d, "oo2.json")])
            if r.returncode != 0 or open(os.path.join(d, "oo2.264"), "rb").read() != open(oo, "rb").read():
                print("FAIL the out-of-order run's record did not replay: %r" % r.stderr.decode()[-300:])
                fails += 1
            else:
                print("  ok   out-of-order events judged at their own frame; the record replays")

        # descriptors that can't be read, a file that isn't one, a NUL byte
        r = run(enc, base + ["--control-fd", "97", "-o", os.path.join(d, "x.264")])
        rdw, wrw = os.pipe()
        r2 = subprocess.run([enc] + base + ["--control-fd", str(wrw), "-o", os.path.join(d, "x.264")],
                            pass_fds=(wrw,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(rdw); os.close(wrw)
        r3 = run(enc, base + ["--control", d, "-o", os.path.join(d, "x.264")])
        nulf = os.path.join(d, "nul.ctl")
        with open(nulf, "wb") as f:
            f.write(b"10 key\x00 junk\n")
        r4 = run(enc, base + ["--control", nulf, "-o", os.path.join(d, "x.264")])
        rdn, wrn = os.pipe()
        p = subprocess.Popen([enc] + base + ["--control-fd", str(rdn), "-o", os.path.join(d, "x.264")],
                             pass_fds=(rdn,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(rdn)
        os.write(wrn, b"next key\x00x\n")
        os.close(wrn)
        _, e5 = p.communicate()
        checks = [(r.returncode == 2, "--control-fd on a closed descriptor"),
                  (r2.returncode == 2, "--control-fd on a write-only descriptor"),
                  (r3.returncode == 2, "--control on a directory"),
                  (r4.returncode == 2 and b"NUL" in r4.stderr, "--control with a NUL byte"),
                  (p.returncode != 0 and b"NUL" in e5, "--control-fd with a NUL byte")]
        bad = [w for ok, w in checks if not ok]
        for w in bad:
            print("FAIL %s was not refused" % w)
        fails += len(bad)
        if not bad:
            print("  ok   unreadable descriptors, a directory and NUL bytes are refused")

        # an unterminated last line counts, and the descriptor's flags are
        # put back (it may be a shared stdin)
        rd, wr = os.pipe()
        keep = os.dup(rd)
        ul = os.path.join(d, "ul.ctl")
        p = subprocess.Popen([enc] + base + ["--control-fd", str(rd), "--control-out", ul,
                                            "-o", os.path.join(d, "ul.264")],
                             pass_fds=(rd,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(rd)
        os.write(wr, b"50 key")
        os.close(wr)
        p.communicate()
        import fcntl
        nb = fcntl.fcntl(keep, fcntl.F_GETFL) & os.O_NONBLOCK
        os.close(keep)
        got = open(ul).read() if os.path.exists(ul) else ""
        if p.returncode != 0 or "50 key" not in got or nb:
            print("FAIL unterminated last line or flag restore: status %d, log %r, nonblock %d"
                  % (p.returncode, got, nb))
            fails += 1
        else:
            print("  ok   an unterminated last line is applied, and the descriptor's flags restored")

        # a refused log leaves an existing output alone
        keepme = os.path.join(d, "keep.264")
        with open(keepme, "wb") as f:
            f.write(b"precious")
        bf = os.path.join(d, "refuse.ctl")
        with open(bf, "w") as f:
            f.write("10 crf value=20\n")
        r = run(enc, base + ["--control", bf, "-o", keepme])
        if r.returncode != 2 or open(keepme, "rb").read() != b"precious":
            print("FAIL a refused --control log cut the existing output")
            fails += 1
        else:
            print("  ok   a refused --control log leaves the existing output as it was")

        # a flood on the descriptor neither stalls the encode nor grows
        # without bound: a few lines a frame, the rest refused as a full queue
        import threading
        rd, wr = os.pipe()
        t0 = time.time()
        p = subprocess.Popen([enc] + base + ["--control-fd", str(rd), "-o", os.path.join(d, "fl.264")],
                             pass_fds=(rd,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(rd)
        stop = [False]

        def flood():
            line = b"100000000 key\n" * 512
            try:
                while not stop[0]:
                    os.write(wr, line)
            except OSError:
                pass
        th = threading.Thread(target=flood, daemon=True)
        th.start()
        _, err = p.communicate(timeout=120)
        stop[0] = True
        try:
            os.close(wr)
        except OSError:
            pass
        dt = time.time() - t0
        if p.returncode != 0 or b"queue is full" not in err or dt > 60:
            print("FAIL a --control-fd flood: status %d, %.1f s, %r" % (p.returncode, dt, err[-200:]))
            fails += 1
        else:
            print("  ok   a --control-fd flood is bounded (the queue fills and later lines are skipped)")

        # more events on one future frame than the feeding window: the live
        # record replays (each lands before its frame goes in)
        crf = ["--input-y4m", src, "--threads", "1", "--no-progress", "--crf", "26"]
        big, bigl = os.path.join(d, "big.264"), os.path.join(d, "big.ctl")
        rd, wr = os.pipe()
        p = subprocess.Popen([enc] + crf + ["--control-fd", str(rd), "--control-out", bigl, "-o", big],
                             pass_fds=(rd,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(rd)
        os.write(wr, b"".join(b"100 qp min=%d\n" % (20 + (i & 1)) for i in range(2000)))
        os.close(wr)
        _, err = p.communicate()
        nlog = len(open(bigl).read().splitlines()) if os.path.exists(bigl) else 0
        r = run(enc, crf + ["--control", bigl, "-o", os.path.join(d, "big2.264")])
        if p.returncode or nlog != 2001 or r.returncode or \
                open(os.path.join(d, "big2.264"), "rb").read() != open(big, "rb").read():
            print("FAIL 2000 events on one frame: live %d, %d log lines, replay %d %r"
                  % (p.returncode, nlog, r.returncode, r.stderr.decode()[-200:]))
            fails += 1
        else:
            print("  ok   2000 events on one frame replay from the 2001-line log")

        # a refusal deep in a long log is found before the output is touched
        keep2 = os.path.join(d, "keep2.264")
        with open(keep2, "wb") as f:
            f.write(b"x" * 4000)
        deep = os.path.join(d, "deep.ctl")
        with open(deep, "w") as f:
            for i in range(1100):
                f.write("%d qp min=%d\n" % (i // 20, 20 + (i & 1)))
            f.write("90 rate bitrate=500\n")
        r = run(enc, crf + ["--control", deep, "-o", keep2])
        if r.returncode != 2 or open(keep2, "rb").read() != b"x" * 4000 or b"deep.ctl:1101" not in r.stderr:
            print("FAIL a refusal at line 1101: status %d, %r" % (r.returncode, r.stderr.decode()[-200:]))
            fails += 1
        else:
            print("  ok   a refusal at line 1101 stops the run before the output is touched")

        # options that need the per-GOP encoders are refused, not dropped
        kc = os.path.join(d, "k.ctl")
        with open(kc, "w") as f:
            f.write("10 key\n")
        refused = 0
        for extra in (["--segment-out", os.path.join(d, "seg%d.264")], ["--cut-split"], ["--shot-crf"],
                      ["--shot-table"], ["--gop-threads", "2"], ["--scan-out", os.path.join(d, "s.scan"), "--cut-split"]):
            r = run(enc, base + ["--control", kc, "-o", os.path.join(d, "g.264")] + extra)
            if r.returncode != 2:
                print("FAIL --control with %s was not refused (status %d)" % (extra[0], r.returncode))
                fails += 1
            else:
                refused += 1
        if refused == 6:
            print("  ok   per-GOP options are refused with live control")

        # file and fd events sharing the queue: a big burst on one frame runs,
        # an fd line never drags the file past its window, an fd line that
        # would break an unfed file event is the one refused, and a log the
        # queue could never hold is refused before the output is cut
        ka = os.path.join(d, "ka.ctl")
        with open(ka, "w") as f:
            f.write("#livectl 1\n" + "10 key\n" * 4200)
        r = run(enc, base + ["--frames", "30", "--control", ka, "-o", os.path.join(d, "ka.264")])
        ok1 = r.returncode == 0 and os.path.getsize(os.path.join(d, "ka.264")) > 1000

        kb = os.path.join(d, "kb.ctl")
        with open(kb, "w") as f:
            f.write("".join("%d key\n" % (100 + i) for i in range(5000)))
        rd, wr = os.pipe()
        p = subprocess.Popen([enc] + crf + ["--control", kb, "--control-fd", str(rd), "-o", os.path.join(d, "kb.264")],
                             pass_fds=(rd,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(rd)
        os.write(wr, b"4999 qp min=20\n")
        os.close(wr)
        _, e2 = p.communicate()
        ok2 = p.returncode == 0 and b"(frame 4999): skipped" in e2

        kc2 = os.path.join(d, "kc.ctl")
        with open(kc2, "w") as f:
            f.write("50 key\n" * 1100 + "60 qp max=30\n")
        rd, wr = os.pipe()
        p = subprocess.Popen([enc] + crf + ["--frames", "100", "--control", kc2, "--control-fd", str(rd),
                                            "-o", os.path.join(d, "kc.264")],
                             pass_fds=(rd,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.close(rd)
        os.write(wr, b"next qp min=40\n")
        os.close(wr)
        _, e3 = p.communicate()
        ok3 = p.returncode == 0 and b"line 1: skipped" in e3

        kd = os.path.join(d, "kd.ctl")
        with open(kd, "w") as f:
            f.write("10 key\n" * 65537)
        keep3 = os.path.join(d, "keep3.264")
        with open(keep3, "wb") as f:
            f.write(b"y" * 100)
        r4 = run(enc, base + ["--control", kd, "-o", keep3])
        ok4 = r4.returncode == 2 and open(keep3, "rb").read() == b"y" * 100
        for ok, what, extra in ((ok1, "4200 events on one frame", r.stderr[-200:]),
                                (ok2, "an fd line past the file window", e2[-200:]),
                                (ok3, "an fd line conflicting with an unfed file event", e3[-200:]),
                                (ok4, "a log the queue cannot hold", r4.stderr[-200:])):
            if not ok:
                print("FAIL %s: %r" % (what, extra))
                fails += 1
        if ok1 and ok2 and ok3 and ok4:
            print("  ok   file and fd events share the queue without a refusal mid-run")

    if fails:
        print("control_check: %d failure(s)" % fails)
        return 1
    print("control_check: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
