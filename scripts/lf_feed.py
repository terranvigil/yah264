#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""lf_feed.py -- the one input path both encoders read in a long-form run.

Every timed encode in scripts/longform_throughput.py reads a Y4M stream on
STDIN from this program, on both sides. No encoder opens a file path in a timed
run: measured 2026-09-24, the reference CLI reading a large Y4M by path is
capped by its own input path (600 frames of meridian 1080p at auto threads,
3.9 s by file against 1.56 s through a pipe), so a by-path wall ratio times the
reader, not the encoder. The same stream is the VMAF reference, so what was
encoded and what it is scored against are one pipeline's output.

Two sources:

  y4m   a staged Y4M segment. Writes the header, seeks to the first wanted
        frame and copies whole blocks. Nothing is parsed per frame. (Not
        `tail -c +N`: on macOS that burned a full core for 83 s per 1.9 GB.)

  vt    a compressed master, decoded on the fly by VideoToolbox and, for a
        size below native, scaled by scale_vt. Measured 2026-09-24 on Tears of
        Steel 4K: the hardware decode is bit-identical to the software decode,
        and scale_vt's output is identical across runs.

THE vt SOURCE RUNS SEVERAL DECODERS AT ONCE, and has to. One VideoToolbox
stream decodes a 4K H.264 master at about 200-250 frames a second (ffmpeg keeps
one frame in flight, so it is latency-bound, not engine-bound), and both
encoders run faster than that at 720p, and the reference at 1080p, at their
default thread counts: one decoder would make the feeder the thing being timed.
Two decoders in parallel measured 490 fps together, three 580. So the span is
cut into chunks and WORKERS decoders take them in order; this process writes
the chunks out in sequence.

EVERY CUT DECODES FROM PREROLL SECONDS EARLIER, and that is not optional. These
masters have almost no IDR pictures (Tears of Steel has exactly one, at frame
0); every other keyframe is an I picture with a recovery point, and pictures
after it still reference pictures before it. Started at such a keyframe,
VideoToolbox drops frames until its references are whole again: measured on
Tears of Steel, a seek to the keyframe at frame 491 first delivered frame 563.
So each chunk seeks PREROLL seconds early, decodes through the recovery, and a
timestamp select passes frames from the exact first frame on.

AND EVERY CHUNK PROVES IT. ffmpeg writes the timestamp of each frame it hands
on (-stats_enc_pre); the first must be the chunk's first frame, the last its
last, and the count its length. A chunk that recovered late, dropped a frame
or ran short is a hard error, because an encoder fed a shifted or truncated
title would report a wall for different work. The same check runs on a
sequential decode from frame 0, so the one-decoder path is guarded too. The
decoder's complaints about the references it never saw are allowed on a
seeking chunk and on no other. `longform_throughput.py verify-feed` compares
the chunked output with one sequential decode, whole title, per frame.

Memory: chunks that run ahead are buffered, capped at BUF_MB in total so a
feeder never competes with a 20 GB 4K encode for the box's 64 GB.
"""
import argparse, collections, os, re, shutil, subprocess, sys, tempfile, threading

FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
# What a decoder says when it starts mid-stream: the VideoToolbox failures on
# the leading pictures, and the software parser's complaints about references
# it never saw (mmco unref, missing reference). Allowed on a seeking chunk
# only; on a chunk that decodes from the first frame any of them is fatal.
LEADING = re.compile(r"hardware accelerator failed to decode picture|"
                     r"Error submitting packet to decoder|No frame decoded|"
                     r"mmco: unref short failure|reference picture missing|"
                     r"Missing reference picture|reference count overflow|"
                     r"co located POCs unavailable|decode_slice_header error|"
                     r"Last message repeated")


def y4m_header(path):
    with open(path, "rb") as f:
        hdr = f.readline()
    tags = hdr.decode("ascii", "replace").split()
    w = int(next(t[1:] for t in tags if t[0] == "W"))
    h = int(next(t[1:] for t in tags if t[0] == "H"))
    return hdr, w * h * 3 // 2 + len(b"FRAME\n")


def feed_y4m(a):
    hdr, per = y4m_header(a.src)
    out = sys.stdout.buffer
    left = a.count * per if a.count else None
    with open(a.src, "rb", buffering=0) as f:
        try:
            out.write(hdr)
            f.seek(len(hdr) + a.start * per)
            while left is None or left > 0:
                b = f.read(8 << 20 if left is None else min(8 << 20, left))
                if not b:
                    break
                out.write(b)
                if left is not None:
                    left -= len(b)
            out.flush()
        except BrokenPipeError:
            os._exit(0)
    if left:
        sys.exit(f"lf_feed: {a.src} ended {left // per} frames short")


def chunk_cmd(a, s, n, seek, stats):
    """One decoder for frames [s, s+n). seek: None (from the master's first
    frame, a plain sequential decode) or 'pre' (seek PREROLL early)."""
    num, den = (int(x) for x in a.fps.split("/"))
    w, h = (int(x) for x in a.size.split("x"))
    nw, nh = (int(x) for x in a.native.split("x"))
    scaled = (w, h) != (nw, nh)
    c = [FFMPEG, "-v", "error", "-nostdin", "-threads", str(a.threads),
         "-filter_threads", "1", "-hwaccel", "videotoolbox"]
    if scaled:
        c += ["-hwaccel_output_format", "videotoolbox_vld"]
    vf = []
    if seek:
        at = max(0.0, s * den / num - a.preroll)
        c += ["-ss", f"{at:.6f}"]
    # -copyts everywhere: the select and the stats file then read the master's
    # own clock, which starts at its start_time (bbb: 2 frames), not zero.
    # Half a frame of slack makes the select immune to timebase rounding.
    c += ["-i", a.src, "-copyts"]
    if seek:
        vf += [f"select='gte(t,{a.t0 + (s - 0.5) * den / num:.6f})'"]
    if scaled:
        vf += [f"scale_vt=w={w}:h={h}", "hwdownload", "format=nv12"]
    vf += ["setsar=1"]
    c += ["-an", "-sn", "-dn", "-vf", ",".join(vf), "-frames:v", str(n),
          "-fps_mode", "passthrough", "-pix_fmt", "yuv420p",
          "-stats_enc_pre", stats, "-stats_enc_pre_fmt", "{t}",
          "-f", "yuv4mpegpipe", "-"]
    return c


def check_stats(a, s, n, stats):
    """None if the chunk delivered exactly frames s..s+n-1, else why not."""
    num, den = (int(x) for x in a.fps.split("/"))
    try:
        with open(stats) as f:
            ts = [float(x) for x in f.read().split()]
    except (OSError, ValueError) as e:
        return f"no frame timestamps ({e})"
    if len(ts) != n:
        return f"{len(ts)} frames, wanted {n}"
    half = 0.5 * den / num
    for what, got, want in (("first", ts[0], s), ("last", ts[-1], s + n - 1)):
        t = a.t0 + want * den / num
        if abs(got - t) > half:
            return (f"{what} frame at t={got:.4f}, wanted frame {want} at "
                    f"t={t:.4f} ({(got - t) * num / den:+.1f} frames)")
    return None


def plan_chunks(a):
    """[(start, count, seek)] covering [a.start, a.start+a.count). The last
    chunk absorbs a remainder shorter than half a chunk."""
    s0, end = a.start, a.start + a.count
    cuts = list(range(s0, end, a.chunk))
    if len(cuts) > 1 and end - cuts[-1] < a.chunk // 2:
        cuts.pop()
    cuts.append(end)
    return [(s, cuts[i + 1] - s, None if s == 0 else "pre")
            for i, s in enumerate(cuts[:-1])]


def feed_vt(a):
    w, h = (int(x) for x in a.size.split("x"))
    per = w * h * 3 // 2 + len(b"FRAME\n")
    chunks = plan_chunks(a) if a.workers > 1 else \
        [(a.start, a.count, None if a.start == 0 else "pre")]
    if a.print_plan:
        for s, n, seek in chunks:
            print(s, n, seek or "-")
        return
    tmp = tempfile.mkdtemp(prefix="lf_feed.")
    out = sys.stdout.buffer
    budget = a.buf_mb << 20
    lock = threading.Condition()
    state = {"buffered": 0, "front": 0, "dead": False}

    class Job:
        def __init__(self, idx, s, n, seek):
            self.idx, self.s, self.n, self.seek = idx, s, n, seek
            self.q = collections.deque()
            self.qb = 0                         # bytes queued in this job alone
            self.eof = False
            self.err = b""
            self.stats = os.path.join(tmp, f"c{idx}.t")
            self.p = subprocess.Popen(chunk_cmd(a, s, n, seek, self.stats),
                                      stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE)
            self.t = threading.Thread(target=self.pump, daemon=True)
            self.te = threading.Thread(target=self.errs, daemon=True)
            self.t.start(); self.te.start()

        def errs(self):
            self.err = self.p.stderr.read()

        def pump(self):
            fd = self.p.stdout.fileno()
            while True:
                # the front chunk is capped too: at 4K the decoder outruns an
                # encoder on a shared box by 10x, and an uncapped front queue
                # grew until the OS killed the feeder (-9) on every 2160p cell
                with lock:
                    while (not state["dead"] and
                           (self.qb > budget if state["front"] == self.idx
                            else state["buffered"] > budget)):
                        lock.wait()
                    if state["dead"]:
                        break
                b = os.read(fd, 4 << 20)
                with lock:
                    if not b:
                        self.eof = True
                        lock.notify_all()
                        break
                    self.q.append(b)
                    self.qb += len(b)
                    state["buffered"] += len(b)
                    lock.notify_all()

    def fail(msg):
        with lock:
            state["dead"] = True
            lock.notify_all()
        for j in jobs:
            if j and j.p.poll() is None:
                j.p.kill()
        shutil.rmtree(tmp, ignore_errors=True)
        sys.exit(f"lf_feed: {msg}")

    jobs, nxt, hdr0, leading = [], 0, None, 0
    try:
        for i in range(len(chunks)):
            while nxt < len(chunks) and nxt < i + a.workers:
                jobs.append(Job(nxt, *chunks[nxt]))
                nxt += 1
            j = jobs[i]
            with lock:
                state["front"] = i
                lock.notify_all()
            got, hdr = 0, b""
            while True:
                with lock:
                    while not j.q and not j.eof:
                        lock.wait()
                    if not j.q and j.eof:
                        break
                    b = j.q.popleft()
                    j.qb -= len(b)
                    state["buffered"] -= len(b)
                    lock.notify_all()
                if hdr is not None:            # strip each chunk's own header
                    hdr += b
                    nl = hdr.find(b"\n")
                    if nl < 0:
                        continue
                    this, b = hdr[:nl + 1], hdr[nl + 1:]
                    hdr = None
                    if hdr0 is None:
                        hdr0 = this
                        out.write(this)
                    elif this != hdr0:
                        fail(f"chunk {i} header {this!r} differs from {hdr0!r}")
                got += len(b)
                out.write(b)
            j.t.join(); j.te.join()
            rc = j.p.wait()
            msgs = [l for l in j.err.decode(errors="replace").splitlines() if l]
            lead = [l for l in msgs if LEADING.search(l)]
            other = [l for l in msgs if not LEADING.search(l)]
            if other or (lead and not j.seek):
                fail(f"chunk {i} (frames {j.s}+{j.n}) decoder said:\n  "
                     + "\n  ".join((other or lead)[:6]))
            leading += len(lead)
            if rc != 0:
                fail(f"chunk {i} (frames {j.s}+{j.n}) exited {rc}")
            if got != j.n * per:
                fail(f"chunk {i} wrote {got / per:.2f} frames, wanted {j.n}")
            why = check_stats(a, j.s, j.n, j.stats)
            if why:
                fail(f"chunk {i} (frames {j.s}+{j.n}): {why}")
            os.remove(j.stats)
            jobs[i] = None                      # let the buffers go
        out.flush()
    except BrokenPipeError:
        with lock:
            state["dead"] = True
            lock.notify_all()
        for j in jobs:
            if j and j.p.poll() is None:
                j.p.kill()
        shutil.rmtree(tmp, ignore_errors=True)
        os._exit(0)
    shutil.rmtree(tmp, ignore_errors=True)
    if a.stats:
        print(f"lf_feed: {len(chunks)} chunks, {leading} leading-picture "
              "reports on seeking chunks", file=sys.stderr)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="mode", required=True)
    y = sub.add_parser("y4m")
    y.add_argument("--src", required=True)
    y.add_argument("--start", type=int, default=0)
    y.add_argument("--count", type=int, default=0, help="0 = to the end")
    v = sub.add_parser("vt")
    v.add_argument("--src", required=True)
    v.add_argument("--native", required=True, help="WxH of the master")
    v.add_argument("--size", required=True, help="WxH to deliver")
    v.add_argument("--fps", required=True, help="num/den")
    v.add_argument("--t0", type=float, default=0.0,
                   help="the master's start_time, seconds")
    v.add_argument("--start", type=int, default=0)
    v.add_argument("--count", type=int, required=True)
    v.add_argument("--workers", type=int, default=1)
    v.add_argument("--chunk", type=int, default=720, help="frames a chunk")
    v.add_argument("--preroll", type=float, default=5.0,
                   help="seconds decoded and discarded before each cut")
    v.add_argument("--threads", type=int, default=1)
    v.add_argument("--buf-mb", type=int, default=1024)
    v.add_argument("--print-plan", action="store_true")
    v.add_argument("--stats", action="store_true")
    a = p.parse_args()
    (feed_y4m if a.mode == "y4m" else feed_vt)(a)


if __name__ == "__main__":
    main()
