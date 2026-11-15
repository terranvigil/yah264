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
  - pass 3 replaces its stats file only when it succeeds: a failed run or
    one ended by any signal in SIGS leaves the file as it was, or absent,
    and no working copy; through a symlink it replaces the target.
  - a threaded two-pass leaves none of its per-GOP stats files behind,
    whether it ends cleanly, fails, is ended by a signal, or writes its
    stream into a pipe whose reader stopped.
  - every pass refuses a stats file that is also the output or the recon,
    by name, through a symlink or as the standard output, and leaves it as
    it was.
  - a two-pass whose stats file can't be written (a file-size limit stands
    in for a full disk) fails, and pass 3 leaves its file as it was.
"""
import os, resource, shlex, signal, subprocess, sys, tempfile, time

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


def run(enc, args, stdin=None, stdout=subprocess.PIPE):
    p = subprocess.run(WRAP + [enc, "--no-progress"] + args, stdin=stdin,
                       stdout=stdout, stderr=subprocess.PIPE,
                       timeout=LIMIT)
    return p.returncode, p.stderr.decode(errors="replace")


# the signals a two-pass removes its scratch files on
SIGS = tuple(getattr(signal, n) for n in (
    "SIGINT", "SIGTERM", "SIGHUP", "SIGQUIT", "SIGPIPE", "SIGALRM", "SIGXCPU", "SIGXFSZ",
    "SIGUSR1", "SIGUSR2", "SIGVTALRM", "SIGPROF", "SIGEMT", "SIGSYS") if hasattr(signal, n))


def spawn(enc, args, stdout=subprocess.DEVNULL):
    """the encoder on a pipe, with the signals a test sends at their defaults:
    a shell that starts a job in the background ignores SIGINT, and the
    encoder keeps a signal it inherits ignored. no core files"""
    def defaults():
        for s in SIGS:
            signal.signal(s, signal.SIG_DFL)
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    return subprocess.Popen(WRAP + [enc, "--no-progress"] + args, stdin=subprocess.PIPE,
                            stdout=stdout, stderr=subprocess.DEVNULL,
                            preexec_fn=defaults)


def reap(p):
    """end a spawned encoder whatever happened, so a failed case can't hang
    the test on a pipe nobody closes"""
    if p.stdin and not p.stdin.closed:
        try:
            p.stdin.close()
        except OSError:
            pass
    if p.poll() is None:
        p.kill()
    p.wait(timeout=LIMIT)


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
    # pass 1 opening a FIFO nobody reads blocks, and a signal still ends it
    for threads in ("1", "3"):
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            q = spawn(enc, ["--input-y4m", clip, "--threads", threads, "--bitrate", "300",
                            "--pass", "1", "--stats", fifo, "-o", out])
            try:
                q.stdin.close()
                time.sleep(0.5)
                q.send_signal(sig)
                try:
                    rc = q.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    rc = "hung"
            finally:
                reap(q)
            check(f"pass 1 blocked on a FIFO stats file ends by {sig.name}, threads {threads}",
                  rc == -sig, f"rc {rc}")
    os.remove(fifo)


def stats_is_output(enc, tmp):
    d = os.path.join(tmp, "same")
    os.makedirs(d)
    clip, st, out = (os.path.join(d, x) for x in ("s.y4m", "s.stats", "o.264"))
    write_clip(clip)
    base = ["--input-y4m", clip, "--threads", "1", "--bitrate", "300", "--keyint", "4"]
    run(enc, base + ["--pass", "1", "--stats", st, "-o", out])
    plan = open(st, "rb").read()
    os.symlink("s.stats", os.path.join(d, "alias.stats"))
    for p in ("1", "2", "3"):
        for name, extra in (("-o", ["-o", st]),
                            ("-o through ./", ["-o", os.path.join(d, ".", "s.stats")]),
                            ("-o through a symlink", ["-o", os.path.join(d, "alias.stats")]),
                            ("--dump-recon", ["-o", out, "--dump-recon", st])):
            rc, err = run(enc, base + ["--pass", p, "--stats", st] + extra)
            check(f"pass {p} refuses --stats named by {name}",
                  rc == 2 and "same file" in err and open(st, "rb").read() == plan,
                  f"rc {rc}: {err.strip()[-200:]}")
        # the standard output sent into the stats file
        with open(st, "ab") as f:
            rc, err = run(enc, base + ["--pass", p, "--stats", st, "-o", "-"], stdout=f)
        check(f"pass {p} refuses --stats as the standard output",
              rc == 2 and "same file" in err and open(st, "rb").read() == plan,
              f"rc {rc}: {err.strip()[-200:]}")
    # neither there yet
    new = os.path.join(d, "new.stats")
    rc, err = run(enc, base + ["--pass", "1", "--stats", new, "-o", new])
    check("pass 1 refuses --stats and -o naming one new file",
          rc == 2 and "same file" in err and not os.path.exists(new),
          f"rc {rc}: {err.strip()[-200:]}")
    # two new names that differ only in case, where the volume folds case
    probe = os.path.join(d, "fold")
    open(probe, "w").close()
    if os.path.exists(os.path.join(d, "FOLD")):
        rc, err = run(enc, base + ["--pass", "1", "--stats", os.path.join(d, "CS.264"),
                                   "-o", os.path.join(d, "cs.264")])
        check("pass 1 refuses --stats and -o differing only in case",
              rc == 2 and "same file" in err and not os.path.exists(os.path.join(d, "cs.264")),
              f"rc {rc}: {err.strip()[-200:]}")
    os.remove(probe)
    # the input, as the stats file, the output or the recon: refused, untouched
    src = open(clip, "rb").read()
    for p in ("0", "1", "3"):
        for name, extra in (("--stats", ["-o", out, "--stats", clip]),
                            ("-o", ["-o", clip, "--stats", st]),
                            ("--dump-recon", ["-o", out, "--stats", st, "--dump-recon", clip])):
            if p == "0" and name == "--stats":
                continue
            args = base + extra + (["--pass", p] if p != "0" else [])
            if p == "0":
                args = [a for a in args if a not in ("--stats", st)]
            rc, err = run(enc, args)
            check(f"{'no pass' if p == '0' else 'pass ' + p} refuses {name} naming the input",
                  rc == 2 and "the input" in err and open(clip, "rb").read() == src,
                  f"rc {rc}: {err.strip()[-200:]}")
    # character devices are never one file: /dev/null as both
    rc, err = run(enc, base + ["--pass", "1", "--stats", os.devnull, "-o", os.devnull,
                               "--dump-recon", os.devnull])
    check("pass 1 takes /dev/null as the stats file and the output", rc == 0,
          f"rc {rc}: {err.strip()[-200:]}")


def run_capped(enc, args, size):
    """the encoder with no file allowed past size bytes: a write past it
    fails as a full disk's would (EFBIG, SIGXFSZ ignored)"""
    def cap():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (size, size))
    p = subprocess.run(WRAP + [enc, "--no-progress"] + args, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=LIMIT, preexec_fn=cap)
    return p.returncode, p.stderr.decode(errors="replace")


def stats_unwritable(enc, tmp):
    d = os.path.join(tmp, "full")
    os.makedirs(d)
    clip, st = os.path.join(d, "s.y4m"), os.path.join(d, "s.stats")
    write_clip(clip)
    base = ["--input-y4m", clip, "--bitrate", "300", "--keyint", "4", "-o", os.devnull]
    serial = ["--threads", "1", "--dump-recon", os.devnull]
    for name, extra in (("threads 1", ["--threads", "1"]), ("threads 3", ["--threads", "3"]),
                        ("serial", serial)):
        rc, err = run_capped(enc, base + extra + ["--pass", "1", "--stats", st], 100)
        check(f"pass 1 whose stats can't be written fails, {name}",
              rc != 0 and "cannot write stats file" in err, f"rc {rc}: {err.strip()[-300:]}")
    # pass 3 on a file without its optional header lines: the working copy
    # fits under the cap, and the rewrite, which writes them, doesn't
    for name, extra in (("threads 3", ["--threads", "3"]), ("serial", serial)):
        rc, err = run(enc, base + extra + ["--pass", "1", "--stats", st])
        lines = open(st, "rb").read().splitlines(keepends=True)
        with open(st, "wb") as f:
            f.write(b"".join(l for l in lines
                             if not l.startswith((b"#threads", b"#vbv", b"#fields",
                                                  b"#has-end", b"#end"))))
        before = open(st, "rb").read()
        rc, err = run_capped(enc, base + ["--threads", "3", "--pass", "3", "--stats", st],
                             len(before))
        check(f"pass 3 whose stats can't be written keeps its file, {name}",
              rc != 0 and open(st, "rb").read() == before and "is unchanged" in err
              and not leftovers(d, ("s.y4m", "s.stats")),
              f"rc {rc}, left {leftovers(d, ('s.y4m', 's.stats'))}: {err.strip()[-300:]}")


def stats_cut(enc, tmp):
    d = os.path.join(tmp, "cut")
    os.makedirs(d)
    clip, st, out = (os.path.join(d, x) for x in ("s.y4m", "s.stats", "o.264"))
    write_clip(clip)
    base = ["--input-y4m", clip, "--threads", "1", "--bitrate", "300", "--keyint", "4",
            "-o", out]
    run(enc, base + ["--dump-recon", os.devnull, "--pass", "1", "--stats", st])
    lines = open(st, "rb").read().splitlines(keepends=True)
    # a serial plan cut at any record boundary, the end line included
    nrec = sum(1 for l in lines if not l.startswith(b"#"))
    bad = []
    for n in range(len(lines) - nrec, len(lines)):
        with open(st, "wb") as f:
            f.write(b"".join(lines[:n]))
        rc, err = run(enc, base + ["--pass", "2", "--stats", st])
        if rc == 0:
            bad.append(n)
    check("pass 2 refuses a serial plan cut at a record boundary", not bad, f"accepted {bad}")
    # a plan written before the end line existed still reads
    with open(st, "wb") as f:
        f.write(b"".join(l for l in lines if not l.startswith((b"#has-end", b"#end"))))
    rc, err = run(enc, base + ["--pass", "2", "--stats", st])
    check("pass 2 reads a plan with no end line", rc == 0, f"rc {rc}: {err.strip()[-200:]}")


def leftovers(d, keep):
    return sorted(set(os.listdir(d)) - set(keep))


def pass3_atomic(enc, tmp):
    d = os.path.join(tmp, "p3")
    os.makedirs(d)
    clip, out, st = (os.path.join(d, x) for x in ("s.y4m", "o.264", "s.stats"))
    write_clip(clip)
    keep = ("s.y4m", "o.264", "s.stats", "cut.y4m")
    for threads in ("1", "3"):
        rc, err = run(enc, ["--input-y4m", clip, "--threads", threads, "--bitrate", "300",
                            "--keyint", "4", "--pass", "1", "--stats", st, "-o", out])
        os.chmod(st, 0o640)
        before = open(st, "rb").read()
        # a run that fails: the file stays, the working copy goes
        cut = os.path.join(d, "cut.y4m")
        with open(cut, "wb") as f:
            f.write(open(clip, "rb").read()[:-100])
        rc, err = run(enc, ["--input-y4m", cut, "--threads", threads, "--bitrate", "300",
                            "--keyint", "4", "--pass", "3", "--stats", st, "-o", out])
        check(f"failed pass 3 keeps its stats file, threads {threads}",
              rc != 0 and open(st, "rb").read() == before and not leftovers(d, keep)
              and "'%s' is unchanged" % st in err,
              f"rc {rc}, left {leftovers(d, keep)}: {err.strip()[-200:]}")
        # a run that succeeds replaces it, permissions kept
        rc, err = run(enc, ["--input-y4m", clip, "--threads", threads, "--bitrate", "300",
                            "--keyint", "4", "--pass", "3", "--stats", st, "-o", out])
        after = open(st, "rb").read()
        check(f"pass 3 rewrites its stats file, threads {threads}",
              rc == 0 and after != before
              and (os.stat(st).st_mode & 0o777) == 0o640 and not leftovers(d, keep),
              f"rc {rc}, mode {oct(os.stat(st).st_mode & 0o777)}, left {leftovers(d, keep)}: "
              f"{err.strip()[-200:]}")
    # a symlinked --stats: the new records land on the target, the link stays
    sub = os.path.join(d, "sub")
    os.makedirs(sub)
    real, link = os.path.join(sub, "real.stats"), os.path.join(d, "link.stats")
    keep_l = keep + ("sub", "link.stats")
    for threads in ("1", "3"):
        with open(real, "wb") as f:
            f.write(open(st, "rb").read())
        if not os.path.lexists(link):
            os.symlink(os.path.join("sub", "real.stats"), link)
        before = open(real, "rb").read()
        rc, err = run(enc, ["--input-y4m", clip, "--threads", threads, "--bitrate", "300",
                            "--keyint", "4", "--pass", "3", "--stats", link, "-o", out])
        check(f"pass 3 through a symlinked stats file, threads {threads}",
              rc == 0 and os.path.islink(link) and open(real, "rb").read() != before
              and not leftovers(d, keep_l) and not leftovers(sub, ("real.stats",)),
              f"rc {rc}, link {os.path.islink(link)}, left {leftovers(d, keep_l)} "
              f"{leftovers(sub, ('real.stats',))}: {err.strip()[-200:]}")
    # no stats file: a failed run makes none, a clean one writes it
    none = os.path.join(d, "none.stats")
    for threads in ("1", "3"):
        rc, err = run(enc, ["--input-y4m", cut, "--threads", threads, "--bitrate", "300",
                            "--keyint", "4", "--pass", "3", "--stats", none, "-o", out])
        check(f"failed pass 3 with no stats file makes none, threads {threads}",
              rc != 0 and not os.path.exists(none) and not leftovers(d, keep_l)
              and "no '%s' was made" % none in err,
              f"rc {rc}, left {leftovers(d, keep_l)}: {err.strip()[-200:]}")
    rc, err = run(enc, ["--input-y4m", clip, "--threads", "1", "--bitrate", "300",
                        "--keyint", "4", "--pass", "3", "--stats", none, "-o", out])
    check("pass 3 with no stats file writes one",
          rc == 0 and os.path.getsize(none) > 0 and not leftovers(d, keep_l + ("none.stats",)),
          f"rc {rc}, left {leftovers(d, keep_l)}: {err.strip()[-200:]}")
    os.remove(none)
    # a run a signal ends, held mid-input on a pipe so the signal lands
    # while it codes: the file stays, the working copy goes
    before = open(st, "rb").read()
    data = open(clip, "rb").read()
    for sig in SIGS:
        p = spawn(enc, ["--input-y4m", "-", "--threads", "1", "--bitrate", "300",
                        "--keyint", "4", "--pass", "3", "--stats", st, "-o", out])
        try:
            p.stdin.write(data[:len(data) // 2])
            p.stdin.flush()
            t0 = time.time()
            while not leftovers(d, keep_l) and p.poll() is None and time.time() - t0 < LIMIT:
                time.sleep(0.01)
            work = leftovers(d, keep_l)
            p.send_signal(sig)
            try:
                rc = p.wait(timeout=LIMIT)
            except subprocess.TimeoutExpired:
                rc = "hung"
        finally:
            reap(p)
        check(f"pass 3 ended by {sig.name} keeps its stats file",
              work and rc == -sig and open(st, "rb").read() == before
              and not leftovers(d, keep_l),
              f"rc {rc}, working copy {work}, left {leftovers(d, keep_l)}")


def gop_files(enc, tmp):
    d = os.path.join(tmp, "gop")
    os.makedirs(d)
    clip, cut, out, st = (os.path.join(d, x) for x in ("n.y4m", "cut.y4m", "o.264", "n.stats"))
    # noise, big enough that a signal lands while the GOPs code
    w, h, n = 640, 360, 240
    with open(clip, "wb") as f:
        f.write(b"YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C420jpeg\n" % (w, h))
        for _ in range(n):
            f.write(b"FRAME\n" + os.urandom(w * h) + bytes([128]) * (w * h // 2))
    with open(cut, "wb") as f:
        f.write(open(clip, "rb").read()[:-100])
    keep = ("n.y4m", "cut.y4m", "o.264", "n.stats")
    args = ["--threads", "3", "--bitrate", "300", "--keyint", "10", "--stats", st, "-o", out]
    for p in ("1", "2"):
        rc, err = run(enc, ["--input-y4m", clip, "--pass", p] + args)
        check(f"pass {p} at 3 threads leaves no per-GOP files",
              rc == 0 and not leftovers(d, keep), f"rc {rc}, left {leftovers(d, keep)}")
    rc, err = run(enc, ["--input-y4m", cut, "--pass", "2"] + args)
    check("failed pass 2 at 3 threads leaves no per-GOP files",
          rc != 0 and not leftovers(d, keep), f"rc {rc}, left {leftovers(d, keep)}")
    tag = {"1": ".p1gop", "2": ".p2gop"}
    for p in ("1", "2"):
        if p == "2":                # a killed pass 1 may have left no plan
            run(enc, ["--input-y4m", clip, "--pass", "1"] + args)
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGQUIT, signal.SIGPIPE):
            # the files exist only while the GOPs code, so wait for one and
            # signal at once; a run that ends first is tried again
            for _ in range(5):
                q = spawn(enc, ["--input-y4m", clip, "--pass", p] + args)
                seen = []
                try:
                    q.stdin.close()
                    t0 = time.time()
                    while q.poll() is None and time.time() - t0 < LIMIT:
                        seen = [x for x in os.listdir(d) if tag[p] in x]
                        if seen:
                            q.send_signal(sig)
                            break
                        time.sleep(0.002)
                    try:
                        rc = q.wait(timeout=LIMIT)
                    except subprocess.TimeoutExpired:
                        rc = "hung"
                finally:
                    reap(q)
                if seen:
                    break
            check(f"pass {p} at 3 threads ended by {sig.name} leaves no per-GOP files",
                  seen and rc == -sig and not leftovers(d, keep),
                  f"rc {rc}, saw {seen[:2]}, left {leftovers(d, keep)}")
    # the stream into a reader that stops early (`-o - | head -c 1000`): the
    # write fails or SIGPIPE ends the run, and either way nothing is left
    piped = [x for x in args if x not in ("-o", out)] + ["-o", "-"]
    run(enc, ["--input-y4m", clip, "--pass", "1"] + args)
    plan = open(st, "rb").read()
    for name, extra in (("pass 3", ["--pass", "3"]), ("pass 1", ["--pass", "1"]),
                        ("serial pass 1", ["--pass", "1", "--dump-recon", os.devnull])):
        q = spawn(enc, ["--input-y4m", clip] + piped + extra, stdout=subprocess.PIPE)
        try:
            q.stdin.close()
            q.stdout.read(1000)
            q.stdout.close()
            try:
                rc = q.wait(timeout=LIMIT)
            except subprocess.TimeoutExpired:
                rc = "hung"
        finally:
            reap(q)
        check(f"{name} into a pipe whose reader stopped fails and leaves nothing",
              rc not in (0, "hung") and not leftovers(d, keep)
              and (name != "pass 3" or open(st, "rb").read() == plan),
              f"rc {rc}, left {leftovers(d, keep)}")
    # with SIGPIPE ignored the write error ends the run: status 1, and no
    # merge of the GOPs it never finished
    for name, extra in (("pass 3", ["--pass", "3"]), ("pass 1", ["--pass", "1"])):
        if name == "pass 3":
            run(enc, ["--input-y4m", clip, "--pass", "1"] + args)
        plan = open(st, "rb").read()
        q = subprocess.Popen(WRAP + [enc, "--no-progress", "--input-y4m", clip] + piped + extra,
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE,
                             preexec_fn=lambda: signal.signal(signal.SIGPIPE, signal.SIG_IGN))
        try:
            q.stdout.read(1000)
            q.stdout.close()
            try:
                err = q.stderr.read().decode(errors="replace")
                rc = q.wait(timeout=LIMIT)
            except subprocess.TimeoutExpired:
                rc, err = "hung", ""
        finally:
            reap(q)
        check(f"{name} into a closed pipe with SIGPIPE ignored exits 1",
              rc == 1 and "stats hold" not in err and not leftovers(d, keep)
              and (name != "pass 3" or open(st, "rb").read() == plan),
              f"rc {rc}, left {leftovers(d, keep)}: {err.strip()[-200:]}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    enc = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="y264robust.") as tmp:
        truncated_header(enc, tmp)
        missing_stats(enc, tmp)
        fifo_stats(enc, tmp)
        stats_is_output(enc, tmp)
        stats_unwritable(enc, tmp)
        stats_cut(enc, tmp)
        pass3_atomic(enc, tmp)
        gop_files(enc, tmp)
    if failures:
        print(f"cli_robust_check: {len(failures)} failure(s)")
        return 1
    print("cli_robust_check: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
