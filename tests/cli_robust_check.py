#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""cli_robust_check.py - the CLI's handling of damaged or awkward input files.

    python3 tests/cli_robust_check.py path/to/cli/yah264

Each case runs the encoder on a small synthetic clip and checks the exit
status and what it says on stderr:

  - a Y4M that ends partway through a FRAME header is a truncated file and
    fails the way a short frame body does, on every reader path.
  - pass 2 or 3 with no stats file warns that it runs without a plan; an
    unreadable or empty one is refused; pass 1, 3, 2 in turn over one file
    runs clean.
  - pass 2 or 3 refuses a stats file that isn't a regular file (a FIFO hung
    on its second open).
"""
import os, shlex, subprocess, sys, tempfile

WRAP = shlex.split(os.environ.get("YAH264_WRAP", ""))
LIMIT = 600 if WRAP else 60

W, H, FRAMES = 64, 48, 8
HDR = b"YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C420jpeg\n" % (W, H)


def frame(n):
    y = bytes((i * 7 + (i // W) * 3 + n * 5) & 0xff for i in range(W * H))
    return y + bytes([128 + n]) * (W * H // 2)


def write_clip(path, tail=b""):
    with open(path, "wb") as f:
        f.write(HDR)
        for n in range(FRAMES):
            f.write(b"FRAME\n" + frame(n))
        f.write(tail)


def run(enc, args, stdin=None):
    p = subprocess.run(WRAP + [enc, "--no-progress"] + args, stdin=stdin,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=LIMIT)
    return p.returncode, p.stderr.decode(errors="replace")


failures = []


def check(name, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + name + ("" if ok else ": " + detail))
    if not ok:
        failures.append(name)


def truncated_header(enc, tmp):
    out = os.path.join(tmp, "o.264")
    for tail, what in ((b"FRA", "3-byte header"), (b"FRAME\n", "header, no body"),
                       (b"FRAME\n" + b"\x80" * 100, "short body")):
        clip = os.path.join(tmp, "t.y4m")
        write_clip(clip, tail)
        for threads in ("1", "3"):
            rc, err = run(enc, ["--input-y4m", clip, "--threads", threads,
                                "--crf", "26", "-o", out])
            check(f"y4m {what}, threads {threads}",
                  rc != 0 and "short read on frame %d" % FRAMES in err,
                  f"rc {rc}: {err.strip()[-200:]}")
        with open(clip, "rb") as f:
            rc, err = run(enc, ["--input-y4m", "-", "--threads", "3", "--crf", "26",
                                "-o", out], stdin=f)
        check(f"y4m {what}, pipe", rc != 0 and "short read" in err,
              f"rc {rc}: {err.strip()[-200:]}")
    # the frames before the cut still encode when --frames stops short of it
    write_clip(clip, b"FRA")
    rc, err = run(enc, ["--input-y4m", clip, "--frames", str(FRAMES), "--threads", "1",
                        "--crf", "26", "-o", out])
    check("y4m 3-byte header past --frames", rc == 0, f"rc {rc}: {err.strip()[-200:]}")
    write_clip(clip)
    rc, err = run(enc, ["--input-y4m", clip, "--threads", "1", "--crf", "26", "-o", out])
    check("y4m whole clip", rc == 0, f"rc {rc}: {err.strip()[-200:]}")


def missing_stats(enc, tmp):
    clip, out = os.path.join(tmp, "s.y4m"), os.path.join(tmp, "o.264")
    write_clip(clip)
    st = os.path.join(tmp, "absent.stats")
    for p in ("2", "3"):
        for threads in ("1", "3"):
            if os.path.exists(st):
                os.remove(st)
            rc, err = run(enc, ["--input-y4m", clip, "--threads", threads, "--bitrate", "300",
                                "--pass", p, "--stats", st, "-o", out])
            check(f"pass {p} with no stats file warns, threads {threads}",
                  rc == 0 and "runs without a plan" in err, f"rc {rc}: {err.strip()[-200:]}")
    # one that is there but can't be read is an error, not a missing plan
    st = os.path.join(tmp, "locked.stats")
    run(enc, ["--input-y4m", clip, "--threads", "1", "--bitrate", "300",
              "--pass", "1", "--stats", st, "-o", out])
    os.chmod(st, 0)
    if os.access(st, os.R_OK):              # root reads it anyway
        print("  skip unreadable stats file (running as root)")
    else:
        for p in ("2", "3"):
            rc, err = run(enc, ["--input-y4m", clip, "--threads", "1", "--bitrate", "300",
                                "--pass", p, "--stats", st, "-o", out])
            check(f"pass {p} refuses an unreadable stats file",
                  rc != 0 and "Permission denied" in err and "without a plan" not in err,
                  f"rc {rc}: {err.strip()[-200:]}")
    os.chmod(st, 0o600)
    st = os.path.join(tmp, "empty.stats")
    open(st, "w").close()
    rc, err = run(enc, ["--input-y4m", clip, "--threads", "1", "--bitrate", "300",
                        "--pass", "2", "--stats", st, "-o", out])
    check("pass 2 with an empty stats file is refused", rc != 0 and "no records" in err,
          f"rc {rc}: {err.strip()[-200:]}")
    st = os.path.join(tmp, "flow.stats")
    for threads in ("1", "3"):
        for p in ("1", "3", "2"):
            rc, err = run(enc, ["--input-y4m", clip, "--threads", threads, "--bitrate", "300",
                                "--keyint", "4", "--pass", p, "--stats", st, "-o", out])
            check(f"pass {p} in the 1-3-2 flow, threads {threads}",
                  rc == 0 and "warning" not in err, f"rc {rc}: {err.strip()[-200:]}")


def fifo_stats(enc, tmp):
    clip, out = os.path.join(tmp, "s.y4m"), os.path.join(tmp, "o.264")
    write_clip(clip)
    fifo = os.path.join(tmp, "stats.fifo")
    os.mkfifo(fifo)
    for p in ("2", "3"):
        for threads in ("1", "3"):
            try:
                rc, err = run(enc, ["--input-y4m", clip, "--threads", threads,
                                    "--bitrate", "300", "--pass", p, "--stats", fifo,
                                    "-o", out])
            except subprocess.TimeoutExpired:
                rc, err = None, "hung"
            check(f"pass {p} refuses a FIFO stats file, threads {threads}",
                  rc not in (0, None) and "regular file" in err, f"rc {rc}: {err.strip()[-200:]}")
    os.remove(fifo)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    enc = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="y264robust.") as tmp:
        truncated_header(enc, tmp)
        missing_stats(enc, tmp)
        fifo_stats(enc, tmp)
    if failures:
        print(f"cli_robust_check: {len(failures)} failure(s)")
        return 1
    print("cli_robust_check: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
