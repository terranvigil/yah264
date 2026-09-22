#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""longform_throughput.py -- the number a video engineer asks for first.

Every speed figure this project publishes is taken on a six-second window of a
test clip. That is the right instrument for deciding whether a kernel paid for
itself and the wrong one for the question an engineer actually has, which is
"how long does this encoder take on my film, and is the file any good". This
harness answers that one: whole two-minute segments of real titles, at 720p,
1080p and 2160p, both encoders as an engineer would run them.

WHAT "AS AN ENGINEER WOULD RUN THEM" MEANS HERE, precisely, because every word
of it is a decision someone could reasonably have made differently:

  - THE COMMAND-LINE ENCODERS, not the libraries. scripts/ffboard.py exists to
    take the input reader out of a speed reading, and for a board that is the
    right call. Here it is the wrong one: the thing being measured is the
    process an engineer starts, and its startup, its Y4M reader and its output
    writer are part of what they wait for. Peak RSS is not even definable for a
    library arm that shares an address space with the reference.
  - ONE PROCESS, SHIPPED DEFAULTS. --preset medium, the encoder's own default
    thread count, no chunking, no per-shot orchestration, no --cut-split and no
    --shot-crf. Those are opt-in analysis modes (the owner's standing rule) and
    a throughput figure that quietly included one would not be this encoder's.
  - THE WHOLE SEGMENT IN ONE GO. Not a window inside a window.

THE SEGMENTS ARE NOT DECODED HERE, and that is deliberate. They are the Y4M
files the sibling yah265 throughput campaign staged on the internal disk from
the same masters at the same offsets (~/media/throughput). Decoding our own
copies would cost hours, ~180 GB the internal disk does not have, and would
leave the two encoders' numbers merely similar rather than comparable. This
harness reads them and never writes to that directory. If they are gone, the
recipe that made them is beside them (prep.sh) and STAGE below says what each
one has to be.

FRAME COUNT COMES FROM THE FILE SIZE, not from ffprobe: a raw Y4M has no frame
index, ffprobe reports nb_frames=N/A for every one of these, and a harness that
believed it would encode a prefix of the segment and call it the title.

THE RATE MATCH. The reference runs at one CRF per size -- x264's own default,
23, at every size, so the point is the reference's idea of typical and not
ours. Ours is then bisected onto the bytes x264 produced, which is the
direction `make review` matches in and the one that keeps the reference
untouched. Both are scored at that point, VMAF-NEG and PSNR-Y.

AND A FIVE-RUNG LADDER BESIDE IT, because a speed figure at one operating point
can always be bought with bits. Both encoders run the SAME five CRF values and
the BD-rate is integrated over the rate range the two curves share, which is
scripts/hd_band.py's fix B and is what BD-rate is defined to do. It needs no
solve, so it costs ten encodes a cell instead of fifty.

VMAF READS THE DECODE THROUGH A FIFO. A 2160p two-minute decode is 36 GB of
Y4M, and three of those do not fit beside the segments. ffmpeg writes the
decoded frames into a named pipe and libvmaf reads them as it goes, so the
whole pass costs one file's worth of nothing. --subsample 4 scores every fourth
frame; it is the project's long-form convention and it moves the pooled mean by
well under the differences being read.

Usage:
    scripts/longform_throughput.py ladder <cell>...   five rungs, both arms
    scripts/longform_throughput.py point  <cell>...   the matched-byte point
    scripts/longform_throughput.py timed  <cell>...   THE TIMED LEG
    scripts/longform_throughput.py report [<cell>...]
    scripts/longform_throughput.py cells              list what exists

A cell is title_size, e.g. chimera_2160p. Bare `all` means every staged cell.

Everything except `timed` is untimed, reads no clock for a result, and runs
under the box share (THREADS, VMAF_THREADS, default 6). `timed` reads clocks
and must have the box to itself.
"""
import os, sys, json, subprocess, re, shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scratch                                                  # noqa: E402
import bdcompare as bd                                          # noqa: E402

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STAGE   = os.environ.get("LF_STAGE", os.path.expanduser("~/media/throughput"))
YAH264  = os.environ.get("YAH264", os.path.join(_ROOT, "build-rel", "cli", "yah264"))
X264    = os.environ.get("X264", shutil.which("x264") or "x264")
FFMPEG  = os.environ.get("FFMPEG", "ffmpeg")
VMAF    = os.environ.get("VMAF", "vmaf")
PRESET  = os.environ.get("PRESET", "medium")
# The share for the untimed phases. The timed phase ignores it: it runs both
# encoders at their own DEFAULT thread count, which is the whole point.
THREADS      = os.environ.get("THREADS", "6")
# The box share is SIX THREADS IN TOTAL, and libvmaf is not the only process in
# a scoring pass: the ffmpeg feeding the FIFO is another. Four plus two is the
# budget, not four plus whatever ffmpeg decides -- an unpinned ffmpeg takes one
# thread per core and a "VMAF_THREADS=4" pass then reads as eleven.
VMAF_THREADS = os.environ.get("VMAF_THREADS", "4")
FF_THREADS   = os.environ.get("FF_THREADS", "2")
SUBSAMPLE    = os.environ.get("SUBSAMPLE", "4")
# THE BD LADDER'S OWN WINDOW, in seconds, and the one economy in this item.
#
# The matched point and every timed number are the WHOLE segment; nothing about
# the throughput answer is shortened. The ladder is a different question -- did
# the speed cost bits -- and it costs ten encodes and ten VMAF passes a cell.
# At 2160p on a two-minute segment that is about half an hour of encoding and a
# quarter hour of scoring PER CELL on an idle box, more than the whole rest of
# the item put together, to answer a question that 30 seconds of the same
# content answers as well: 720 frames at 24p is six times the board's own
# window and six times the project's 120-frame floor for a BD sweep.
#
# 0 means the whole segment, which is what the 720p and 1080p ladders were run
# at. A cell's BD is self-contained -- both arms, five rungs, the same frames --
# so a 30 s ladder and a 120 s ladder sit side by side in one table as long as
# the table says which is which. `ladder_frames` is recorded with every point.
LADDER_SECS  = float(os.environ.get("LADDER_SECS", "0"))
# THE THREAD COUNT IS PART OF THE OPERATING POINT, and this is the trap the
# board learned the hard way. Neither encoder is thread-count invariant:
# measured on this tree at 120 frames of park_joy_720p, preset medium, crf 23,
# yah264 writes 4 721 678 / 4 721 255 / 4 692 103 bytes at t6 / auto / t1 and
# x264 writes 4 462 782 / 4 475 437 at t6 / auto. So the MATCHED POINT is
# solved at the DEFAULT thread count, the one the timed leg runs at, and the
# bytes quoted beside a wall are the bytes that wall produced. The five-rung
# LADDER runs under the box share instead: it answers a quality question, both
# arms move by the same third of a percent, and a BD integral does not notice.
def default_threads(arm):
    return "0" if arm == "ours" else "auto"


# OUR 4K CELLS CANNOT RUN AT SHIPPED DEFAULTS ON THIS BOX, and the reason is a
# guard that is wrong rather than a machine that is too small.
#
#   yah264: this encode needs 33.4 GiB of memory and the limit is 32.0 GiB
#   yah264:   its window is (--threads + 1) x --keyint = 2880 frame(s)
#
# The guard prices the window at min(frames, (threads+1) x keyint) frames
# resident. At 18 threads and keyint 250 that is 4750 frames, capped by the
# segment at 2880, and at 11.87 MiB a 3840x2160 frame it refuses anything over
# about 2761 frames -- every 4K input longer than 115 seconds, whatever the
# encoder would actually have held. Lift the limit and the same encode runs
# with a window of 208 FRAMES and a peak RSS of 19.98 GB, against the 33.4 GiB
# predicted: measured here on chimera_2160p, 2880 frames in 151.7 s, 628 CPU-s.
#
# So the 2160p cells run with the limit raised and NOTHING ELSE CHANGED. That
# keeps the wall, the CPU seconds, the thread layout and the peak RSS exactly
# what the shipped encoder does; the only thing removed is a refusal that the
# encoder's own behaviour does not justify. Y264_STREAM_WINDOW would also work
# and is bit-identical (checked: 600 frames of meridian_2160p, md5 equal with
# and without, same 3-worker x 8-thread schedule), but it BOUNDS the buffer, so
# it would flatter the peak-RSS column this leg is asked to report.
#
# The refusal itself is a RESULT, not an obstacle, and the record says so.
MEM_ESCAPE = os.environ.get("Y264_MEM_ESCAPE", "60000")

def arm_env(arm, cell):
    e = dict(os.environ)
    if arm == "ours" and cell.endswith("_2160p") and MEM_ESCAPE:
        e["Y264_MAX_INPUT_MB"] = MEM_ESCAPE
    return e

# The reference's own default rate factor, at every size. Not ours to choose.
X264_CRF = float(os.environ.get("X264_CRF", "23"))
# Five rungs PER SIZE, not one ladder for all three. VMAF discriminates between
# about 55 and 95 and a rung outside that band is a fit point on a flat part of
# the curve, which is worth less than nothing to a BD integral. The same rate
# factor does not land in the same place at three resolutions: measured on these
# segments, crf 25 reads NEG 91.4 at 720p and crf 26 reads 86.7 at 2160p, so the
# 2160p ladder sits about three points of CRF lower than the 720p one. RUNGS
# overrides all three at once, for a diagnostic run on one cell.
SIZE_RUNGS = {"720p":  [22.0, 25.0, 28.0, 31.0, 34.0],
              "1080p": [21.0, 24.0, 27.0, 30.0, 33.0],
              "2160p": [19.0, 22.0, 25.0, 28.0, 31.0]}
RUNGS    = ([float(x) for x in os.environ["RUNGS"].split(",")]
            if os.environ.get("RUNGS") else None)

def rungs_for(cell):
    return RUNGS or SIZE_RUNGS[cell.rsplit("_", 1)[1]]
ROUNDS   = int(os.environ.get("ROUNDS", "2"))       # timed rounds, default arm
T1_SECS  = float(os.environ.get("T1_SECS", "60"))   # one-thread sub-leg length
CACHE    = scratch.cache_dir("longform")
RESULTS  = os.path.join(CACHE, "results.json")

# title -> (source label, content class, the staged basename pattern, seconds)
# The patterns are the files the yah265 throughput campaign staged; a segment
# that is not there is simply not a cell.
TITLES = {
    "chimera":    ("Chimera DCI4k 23.98p (tone-mapped to BT.709)", "live",
                   "chimera_{size}_120s.y4m"),
    "sollevante": ("Sol Levante UHD 24p",                          "animation",
                   "sollevante_{size}_120s.y4m"),
    "meridian":   ("Meridian UHD 59.94p (tone-mapped to BT.709)",  "live",
                   "meridian_{size}_90s.y4m"),
}
SIZES = ("720p", "1080p", "2160p")
GEOM  = {"720p": (1280, 720), "1080p": (1920, 1080), "2160p": (3840, 2160)}


def cell_path(cell):
    title, size = cell.rsplit("_", 1)
    return os.path.join(STAGE, TITLES[title][2].format(size=size))


def all_cells():
    return [f"{t}_{s}" for t in TITLES for s in SIZES
            if os.path.exists(cell_path(f"{t}_{s}"))]


def y4m_shape(path):
    """(frames, fps) read off the header and the file size.

    A Y4M is a text header line, then `FRAME\\n` + one planar picture per frame,
    with no index anywhere. ffprobe answers nb_frames=N/A for every file here,
    so the only honest count is arithmetic."""
    with open(path, "rb") as f:
        hdr = f.readline()
    tags = hdr.decode("ascii", "replace").split()
    w = h = None
    num, den = 25, 1
    for t in tags[1:]:
        if t[0] == "W":
            w = int(t[1:])
        elif t[0] == "H":
            h = int(t[1:])
        elif t[0] == "F":
            num, den = (int(x) for x in t[1:].split(":"))
    if w is None or h is None:
        sys.exit(f"longform: {path} has no geometry in its Y4M header")
    per = w * h * 3 // 2 + len(b"FRAME\n")
    frames = (os.path.getsize(path) - len(hdr)) // per
    return frames, num / den, w, h


def results():
    try:
        with open(RESULTS) as f:
            return json.load(f)
    except Exception:
        return {}


def put(key, val):
    """Read, add, write, atomically.

    The whole file is rewritten on every entry, so a kill in the middle of a
    write truncates a record that cost hours of 4K encodes. Writing a sibling
    and renaming it makes the replacement one filesystem operation.

    It does NOT make two processes safe together: both would read the same
    file, and the second to write would drop whatever the first added. Run one
    at a time -- the phases are sequential for the box's sake anyway."""
    r = results(); r[key] = val
    tmp = RESULTS + ".new"
    with open(tmp, "w") as f:
        json.dump(r, f, indent=1, sort_keys=True)
        f.flush(); os.fsync(f.fileno())
    os.replace(tmp, RESULTS)
    return val


# ---------------------------------------------------------------------------
# The two encoders, spelled once.

def enc_cmd(arm, src, crf, out, threads, frames=None):
    if arm == "ours":
        c = [YAH264, "--input-y4m", src, "--preset", PRESET,
             "--crf", f"{crf:.3f}", "--threads", str(threads),
             "-o", out, "--quiet", "--no-progress"]
        if frames:
            c += ["--frames", str(frames)]
    else:
        c = [X264, "--preset", PRESET, "--crf", f"{crf:.3f}",
             "--threads", str(threads), "-o", out, src, "--quiet"]
        if frames:
            c += ["--frames", str(frames)]
    return c


# macOS `time -l` writes the three times on ONE line, VALUE FIRST:
#     0.01 real         0.00 user         0.00 sys
# and then the rusage block, also value first. That is NOT the `-p` format
# (`real 0.01` per line) and a regex written for `-p` matches nothing here.
TIME_RE = {
    "real":   re.compile(r"([\d.]+)\s+real\b"),
    "user":   re.compile(r"([\d.]+)\s+user\b"),
    "sys":    re.compile(r"([\d.]+)\s+sys\b"),
    "maxrss": re.compile(r"(\d+)\s+maximum resident set size"),
}


def timed_run(cmd, e=None):
    """(wall, cpu_seconds, peak_rss_bytes) for one encode.

    /usr/bin/time -l rather than a clock around subprocess.run, because the
    peak RSS is the reason this leg exists in the shape it does and only the
    kernel knows it. -l writes the report to stderr AFTER the child exits, so
    the encoder's own stderr is discarded separately."""
    r = subprocess.run(["/usr/bin/time", "-l"] + cmd, env=e,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.exit(f"longform: encode failed ({r.returncode}): {' '.join(cmd[:6])} ...\n"
                 f"  {r.stderr.decode(errors='replace')[-600:]}")
    rep = r.stderr.decode(errors="replace")
    got = {}
    for k, rx in TIME_RE.items():
        m = rx.search(rep)
        if not m:
            sys.exit(f"longform: /usr/bin/time -l printed no '{k}' -- "
                     f"the report cannot be parsed, so nothing is recorded\n{rep[-400:]}")
        got[k] = float(m.group(1))
    return got["real"], got["user"] + got["sys"], got["maxrss"]


def encode(arm, src, crf, out, threads, frames=None, e=None):
    r = subprocess.run(enc_cmd(arm, src, crf, out, threads, frames), env=e,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0 or not os.path.exists(out) or os.path.getsize(out) < 1024:
        sys.exit(f"longform: encode failed ({r.returncode}) {arm} crf {crf} "
                 f"{os.path.basename(src)}\n  {r.stderr.decode(errors='replace')[-600:]}")
    return os.path.getsize(out)


def _fifo(wd, name):
    p = os.path.join(wd, name)
    if os.path.exists(p):
        os.remove(p)
    os.mkfifo(p)
    return p


def score(src, bitstream, wd, frames=None):
    """(VMAF-NEG, PSNR-Y) of a bitstream against the segment it came from.

    Nothing lands on disk. ffmpeg writes the decoded Y4M into a named pipe and
    libvmaf reads it frame by frame; at 2160p that is 36 GB the internal disk
    does not have to find, ten times over on a ladder cell. When only the first
    `frames` were encoded the reference is fed through a second pipe, cut to the
    same length, because libvmaf aligns the two streams frame for frame and a
    reference that outlives the distorted one is a warning and a pooled mean
    over the wrong denominator."""
    dfifo = _fifo(wd, "dec.fifo")
    j = os.path.join(wd, "v.json")
    procs = []
    dec = subprocess.Popen(
        [FFMPEG, "-v", "error", "-y", "-threads", FF_THREADS,
         "-i", bitstream, "-pix_fmt", "yuv420p", "-f", "yuv4mpegpipe", dfifo],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    procs.append(dec)
    ref = src
    if frames:
        ref = _fifo(wd, "ref.fifo")
        procs.append(subprocess.Popen(
            [FFMPEG, "-v", "error", "-y", "-threads", FF_THREADS,
             "-i", src, "-frames:v", str(frames),
             "-pix_fmt", "yuv420p", "-f", "yuv4mpegpipe", ref],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE))
    v = subprocess.run(
        [VMAF, "--threads", VMAF_THREADS, "--subsample", SUBSAMPLE,
         "-r", ref, "-d", dfifo, "--json", "-o", j, "--feature", "psnr",
         "--model", "version=vmaf_v0.6.1neg:name=neg"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for p in procs:
        p.wait()
    if v.returncode != 0:
        sys.exit(f"longform: vmaf failed on {bitstream}\n"
                 f"  {v.stderr.decode(errors='replace')[-600:]}")
    with open(j) as f:
        pooled = json.load(f)["pooled_metrics"]
    for p in (dfifo, os.path.join(wd, "ref.fifo")):
        if os.path.exists(p):
            os.remove(p)
    return pooled["neg"]["mean"], pooled["psnr_y"]["mean"]


# ---------------------------------------------------------------------------

def do_ladder(cell):
    src = cell_path(cell)
    frames, fps, w, h = y4m_shape(src)
    lf = min(frames, int(round(LADDER_SECS * fps))) if LADDER_SECS else frames
    lim = lf if lf < frames else None
    wd = scratch.mkdtemp("longform")
    try:
        for arm in ("x264", "ours"):
            for crf in rungs_for(cell):
                # The key carries the window ONLY when it is not the whole
                # segment, so the 720p and 1080p ladders already taken at full
                # length keep their entries instead of being recomputed.
                key = (f"ladder:{cell}:{arm}:{crf:.1f}:{PRESET}"
                       + (f":f{lf}" if lim else ""))
                if key in results():
                    print(f"  {cell:<20}{arm:<6}crf {crf:>5.1f}  cached", flush=True)
                    continue
                out = os.path.join(wd, "l.264")
                sz = encode(arm, src, crf, out, THREADS, frames=lim,
                            e=arm_env(arm, cell))
                neg, psnr = score(src, out, wd, frames=lim)
                put(key, {"bytes": sz, "neg": neg, "psnr_y": psnr,
                          "kbps": sz * 8 / (lf / fps) / 1000,
                          "frames": lf, "fps": fps, "threads": THREADS})
                print(f"  {cell:<20}{arm:<6}crf {crf:>5.1f}  "
                      f"{sz*8/(lf/fps)/1000:>9.0f} kbit/s  NEG {neg:6.2f}  "
                      f"PSNR-Y {psnr:6.2f}   ({lf} frames)", flush=True)
    finally:
        shutil.rmtree(wd, ignore_errors=True)


def do_point(cell):
    """x264 at its own default rate factor; ours bisected onto its bytes."""
    src = cell_path(cell)
    frames, fps, w, h = y4m_shape(src)
    wd = scratch.mkdtemp("longform")
    try:
        xkey = f"point:{cell}:x264:{X264_CRF:.1f}:{PRESET}:tdefault"
        if xkey in results():
            xr = results()[xkey]
            print(f"  {cell:<20}x264  crf {X264_CRF:>5.1f}  cached "
                  f"{xr['bytes']} bytes", flush=True)
        else:
            out = os.path.join(wd, "x.264")
            sz = encode("x264", src, X264_CRF, out, default_threads("x264"),
                        e=arm_env("x264", cell))
            neg, psnr = score(src, out, wd)
            xr = put(xkey, {"crf": X264_CRF, "bytes": sz, "neg": neg,
                            "psnr_y": psnr, "frames": frames, "fps": fps,
                            "kbps": sz * 8 / (frames / fps) / 1000})
            print(f"  {cell:<20}x264  crf {X264_CRF:>5.1f}  {sz} bytes  "
                  f"NEG {neg:6.2f}  PSNR-Y {psnr:6.2f}", flush=True)

        nkey = f"point:{cell}:ours:onto{xr['bytes']}:{PRESET}:tdefault"
        if nkey in results():
            nr = results()[nkey]
            print(f"  {cell:<20}ours  crf {nr['crf']:>5.2f}  cached "
                  f"{nr['bytes']} bytes", flush=True)
            return
        # Seed the bracket from the ladder if it is there: our curve is already
        # sampled at five rungs, so the two that straddle the target bound the
        # bisect and turn ten encodes into three or four. Ten-minute encodes are
        # what makes that worth doing.
        # The ladder may have been taken on a shorter window than the point,
        # so its BYTES do not bracket the point's. Its CRF-to-rate ORDERING
        # does, which is all a bracket needs: scale each rung's bytes to the
        # full segment before comparing.
        lad = sorted([(float(k.split(":")[3]),
                       v["bytes"] * frames / v["frames"])
                      for k, v in results().items()
                      if k.startswith(f"ladder:{cell}:ours:")])
        lo, hi = 10.0, 45.0
        below = [c for c, b in lad if b >= xr["bytes"]]
        above = [c for c, b in lad if b <= xr["bytes"]]
        if below:
            lo = max(below)
        if above:
            hi = min(above)
        best, out = None, os.path.join(wd, "n.264")
        for _ in range(12):
            mid = (lo + hi) / 2
            sz = encode("ours", src, mid, out, default_threads("ours"),
                        e=arm_env("ours", cell))
            err = abs(sz - xr["bytes"]) / xr["bytes"]
            if best is None or err < best[2]:
                best = (mid, sz, err)
                shutil.copyfile(out, os.path.join(wd, "n.best.264"))
            print(f"    solve ours crf {mid:6.3f} -> {sz} bytes "
                  f"({100*(sz/xr['bytes']-1):+.2f}%)", flush=True)
            if err < 0.005:
                break
            if sz > xr["bytes"]:
                lo = mid
            else:
                hi = mid
        neg, psnr = score(src, os.path.join(wd, "n.best.264"), wd)
        put(nkey, {"crf": best[0], "bytes": best[1], "neg": neg,
                   "psnr_y": psnr, "frames": frames, "fps": fps,
                   "kbps": best[1] * 8 / (frames / fps) / 1000,
                   "dsize_pct": 100 * (best[1] / xr["bytes"] - 1)})
        print(f"  {cell:<20}ours  crf {best[0]:>5.2f}  {best[1]} bytes  "
              f"({100*(best[1]/xr['bytes']-1):+.2f}%)  NEG {neg:6.2f}  "
              f"PSNR-Y {psnr:6.2f}", flush=True)
    finally:
        shutil.rmtree(wd, ignore_errors=True)


def do_timed(cell):
    """THE TIMED LEG. Box unused, one cell at a time, nothing else running.

    Both arms at their own DEFAULT thread count -- `--threads 0` is ours, and
    x264's default is its own auto rule -- because the question is what the
    shipped encoder does on the shipped machine. The two arms are interleaved
    round by round with the order reversed, so a minute of box drift lands on
    both rather than on whichever went first.

    Output goes to /dev/null. docs/instruments.md records a timed 9.5 MB write
    reading 2.1-10.2 s against 1.63 s to /dev/null and faking a resolution-
    shaped effect; the bytes are already known from the untimed point run."""
    src = cell_path(cell)
    frames, fps, w, h = y4m_shape(src)
    r = results()
    xr = r.get(f"point:{cell}:x264:{X264_CRF:.1f}:{PRESET}:tdefault")
    if not xr:
        sys.exit(f"longform: {cell} has no matched point yet -- run "
                 f"`{sys.argv[0]} point {cell}` first (untimed)")
    nr = next((v for k, v in r.items()
               if k.startswith(f"point:{cell}:ours:onto{xr['bytes']}:")), None)
    if not nr:
        sys.exit(f"longform: {cell} has an x264 point but no solved point for "
                 f"ours -- run `{sys.argv[0]} point {cell}` first")

    t1_frames = max(1, int(round(T1_SECS * fps)))
    out = {"cell": cell, "frames": frames, "fps": fps, "w": w, "h": h,
           "x264_crf": xr["crf"], "ours_crf": nr["crf"],
           "x264_bytes": xr["bytes"], "ours_bytes": nr["bytes"],
           "dsize_pct": nr["dsize_pct"],
           "x264_neg": xr["neg"], "ours_neg": nr["neg"],
           "x264_psnr_y": xr["psnr_y"], "ours_psnr_y": nr["psnr_y"],
           "t1_frames": t1_frames, "rounds": ROUNDS}

    for arm, crf in (("ours", nr["crf"]), ("x264", xr["crf"])):
        out[arm] = {"default": [], "t1": []}
    for i in range(ROUNDS):
        order = (("x264", xr["crf"]), ("ours", nr["crf"])) if i % 2 else \
                (("ours", nr["crf"]), ("x264", xr["crf"]))
        for arm, crf in order:
            wall, cpu, rss = timed_run(
                enc_cmd(arm, src, crf, "/dev/null", default_threads(arm)),
                arm_env(arm, cell))
            out[arm]["default"].append([wall, cpu, rss])
            print(f"  {cell:<20}{arm:<6}default  round {i+1}  "
                  f"{wall:8.2f}s wall {cpu:9.2f}s cpu  "
                  f"{rss/2**30:5.2f} GiB rss  {frames/wall:7.1f} fps", flush=True)
    # One thread, on the first minute only: the serial number is what a scaling
    # story is built on and the whole segment at one thread is an hour a cell.
    for arm, crf in (("ours", nr["crf"]), ("x264", xr["crf"])):
        wall, cpu, rss = timed_run(
            enc_cmd(arm, src, crf, "/dev/null", 1, frames=t1_frames),
            arm_env(arm, cell))
        out[arm]["t1"].append([wall, cpu, rss])
        print(f"  {cell:<20}{arm:<6}t1       {t1_frames} frames  "
              f"{wall:8.2f}s wall {cpu:9.2f}s cpu  {rss/2**30:5.2f} GiB rss  "
              f"{t1_frames/wall:7.1f} fps", flush=True)
    # The decode-only baseline: what the Y4M read costs on its own, so a reader
    # can see how much of each wall above is the file and not the encoder. Not
    # subtracted anywhere -- an engineer waits for it too.
    wall, cpu, rss = timed_run([FFMPEG, "-v", "error", "-y", "-i", src,
                                "-f", "null", "-"])
    out["decode_baseline"] = [wall, cpu, rss]
    print(f"  {cell:<20}{'read':<6}baseline {wall:8.2f}s wall "
          f"{cpu:9.2f}s cpu", flush=True)
    put(f"timed:{cell}:{PRESET}", out)


def med(v):
    v = sorted(v)
    return v[len(v)//2] if len(v) % 2 else (v[len(v)//2-1] + v[len(v)//2]) / 2


def bd_of(cell):
    """BD-rate on VMAF-NEG, ours against x264, over the rate range the two
    ladders share. Positive is worse: we spend that much more rate for the
    same quality."""
    r = results()
    # One window per curve. A cell whose ladder was re-run on a shorter window
    # can hold both sets; mixing them would fit a curve through two different
    # pieces of content, so the LONGEST window that has a full set of rungs
    # wins and the other is ignored.
    want = None
    for arm in ("x264", "ours"):
        by_win = {}
        for k, v in r.items():
            if k.startswith(f"ladder:{cell}:{arm}:"):
                by_win.setdefault(v["frames"], []).append(v)
        full = [w for w, vs in by_win.items() if len(vs) >= 4]
        if not full:
            return None
        want = max(full) if want is None else min(want, max(full))

    def curve(arm):
        pts = sorted((v["kbps"], v["neg"]) for k, v in r.items()
                     if k.startswith(f"ladder:{cell}:{arm}:")
                     and v["frames"] == want)
        return [p[0] for p in pts], [p[1] for p in pts]
    rx, mx = curve("x264")
    rn, mn = curve("ours")
    if len(rx) < 4 or len(rn) < 4:
        return None
    try:
        return bd.bd_rate(rx, mx, rn, mn)
    except Exception:
        return None


def do_report(cells):
    r = results()
    print()
    print("## Long-form throughput, whole segments, both encoders as shipped")
    print()
    print(f"| cell | content | frames | ours crf | x264 crf | dsize | "
          f"ours fps | x264 fps | ours/x264 | ours cpu-s | x264 cpu-s | "
          f"ours RSS | x264 RSS | dVMAF-NEG | dPSNR-Y | BD-rate |")
    print("|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|")
    for cell in cells:
        t = r.get(f"timed:{cell}:{PRESET}")
        if not t:
            continue
        title = cell.rsplit("_", 1)[0]
        ow = med([x[0] for x in t["ours"]["default"]])
        xw = med([x[0] for x in t["x264"]["default"]])
        oc = med([x[1] for x in t["ours"]["default"]])
        xc = med([x[1] for x in t["x264"]["default"]])
        orss = max(x[2] for x in t["ours"]["default"]) / 2**30
        xrss = max(x[2] for x in t["x264"]["default"]) / 2**30
        bdr = bd_of(cell)
        print(f"| {cell} | {TITLES[title][1]} | {t['frames']} | "
              f"{t['ours_crf']:.2f} | {t['x264_crf']:.2f} | "
              f"{t['dsize_pct']:+.2f}% | {t['frames']/ow:.1f} | "
              f"{t['frames']/xw:.1f} | **{ow/xw:.2f}x** | {oc:.0f} | {xc:.0f} | "
              f"{orss:.2f} GiB | {xrss:.2f} GiB | "
              f"{t['ours_neg']-t['x264_neg']:+.2f} | "
              f"{t['ours_psnr_y']-t['x264_psnr_y']:+.2f} | "
              + (f"{bdr:+.2f}% |" if bdr is not None else "n/a |"))
    print()
    print("## The same cells as an engineer plans a job: x realtime, and one "
          "hour of film")
    print()
    print("| cell | fps (ours) | fps (x264) | x realtime (ours) | "
          "x realtime (x264) | one hour of film (ours) | (x264) | "
          "one thread, first minute: ours/x264 |")
    print("|---|--:|--:|--:|--:|--:|--:|--:|")
    for cell in cells:
        t = r.get(f"timed:{cell}:{PRESET}")
        if not t:
            continue
        ow = med([x[0] for x in t["ours"]["default"]])
        xw = med([x[0] for x in t["x264"]["default"]])
        ofps, xfps = t["frames"] / ow, t["frames"] / xw
        o1 = med([x[0] for x in t["ours"]["t1"]])
        x1 = med([x[0] for x in t["x264"]["t1"]])
        hour = lambda fps: 3600 * t["fps"] / fps / 60      # minutes per hour
        print(f"| {cell} | {ofps:.1f} | {xfps:.1f} | {ofps/t['fps']:.2f}x | "
              f"{xfps/t['fps']:.2f}x | {hour(ofps):.0f} min | "
              f"{hour(xfps):.0f} min | {o1/x1:.2f}x |")
    print()


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == "cells":
        for c in all_cells():
            p = cell_path(c)
            frames, fps, w, h = y4m_shape(p)
            print(f"  {c:<22}{w}x{h}  {fps:7.3f} fps  {frames:>6} frames  "
                  f"{frames/fps:6.1f} s  {os.path.getsize(p)/2**30:6.1f} GiB")
        return
    args = sys.argv[2:] or ["all"]
    cells = all_cells() if args == ["all"] else args
    for c in cells:
        if c not in all_cells():
            sys.exit(f"longform: no staged segment for '{c}' "
                     f"({cell_path(c) if '_' in c else '?'})")
    if cmd == "ladder":
        for c in cells:
            do_ladder(c)
    elif cmd == "point":
        for c in cells:
            do_point(c)
    elif cmd == "timed":
        for c in cells:
            do_timed(c)
    elif cmd == "report":
        do_report(cells)
    else:
        sys.exit(__doc__)


main()
