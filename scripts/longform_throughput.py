#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""longform_throughput.py -- the number a video engineer asks for first.

Every speed figure this project publishes is taken on a six-second window of a
test clip. That is the right instrument for deciding whether a kernel paid for
itself and the wrong one for the question an engineer actually has, which is
"how long does this encoder take on my film, and is the file any good". This
harness answers that one: whole titles (ten minutes and more) and the staged
two-minute segments, at 720p, 1080p and 2160p, both encoders as an engineer
would run them.

WHAT "AS AN ENGINEER WOULD RUN THEM" MEANS HERE, precisely, because every word
of it is a decision someone could reasonably have made differently:

  - THE COMMAND-LINE ENCODERS, not the libraries. Their startup and output
    writer are part of what an engineer waits for, and peak RSS is not even
    definable for a library arm that shares an address space.
  - ONE PROCESS, SHIPPED DEFAULTS. --preset medium, the encoder's own default
    thread count, no chunking, no per-shot orchestration, no --cut-split and
    no --shot-crf.
  - BOTH ENCODERS READ STDIN, FROM THE SAME FEEDER. Never a file path in a
    timed run. Measured 2026-09-24: the reference CLI reading a large Y4M by
    path is capped by its own input path (600 frames of meridian 1080p at
    auto threads, 3.9 s by file against 1.56 s through a pipe; one thread
    20.2 against 15.6 s). Every long-form ratio taken by path before that
    date is void, and the report ignores those records (they are keyed
    without the ":stdin" suffix). scripts/lf_feed.py is the feeder; its
    docstring has the details.

TWO KINDS OF CELL. A cell is title_size.

  - SEGMENTS: the Y4M files the sibling yah265 campaign staged on the internal
    disk (~/media/throughput), 90-120 s each; e.g. chimera_2160p. Fed by
    copying blocks from the file.
  - FULL TITLES: the masters on the external drive, 10+ minutes; e.g.
    tears-full_720p. A 12-minute 4K title is ~500 GB of Y4M, and the internal
    disk has ~57 GB free while the USB drive reads at ~82 MB/s (4K raw at
    ~8 fps), so a full title is never staged: it is decoded on the fly by
    VideoToolbox (bit-identical to the software decode) and scaled by
    scale_vt for 720p/1080p. The same pipeline is the VMAF reference.

SIZES: 720p is 1280 wide and 1080p 1920 wide, the height following the
master's aspect rounded to even; 2160p is the master's own size. Cosmos
Laundromat's master is 1920x804, so it has 720p and 1080p rows only.

THE RATE MATCH. The reference runs at x264's own default rate factor, 23, at
every size. Ours is solved onto the bytes it produced (secant, see do_point).
Both are scored at that point, VMAF-NEG and PSNR-Y, every frame. Beside it, a
five-rung ladder gives a BD-rate on the same frames.

THE TIMED LEGS (the coordinator books the exclusive windows; `plan` prints
them with estimated walls):

  timed     G3: SIMD, default threads, both arms, ROUNDS rounds interleaved,
            whole cell. Then SIMD one-thread, whole cell, informative. Then the
            READ BASELINE: the feeder alone into /dev/null.
  purec     G1 (pure C, one thread) and G2 (pure C, default threads) on a
            STRATIFIED SAMPLE: one 60-second window per 3 minutes of title,
            centred in its stratum, at offsets fixed by the frame count and
            recorded with the result. The CRFs are the full-title matched ones.
            A read baseline per window.
  validate  the one full-title pure-C check of the sample: Tears of Steel
            720p, one thread, whole title, both arms. `report` sets it beside
            the sampled estimate.

Usage:
    scripts/longform_throughput.py cells                list what exists
    scripts/longform_throughput.py plan [<cell>...]     DRY RUN of the timed legs
    scripts/longform_throughput.py verify-feed <cell>   chunked == sequential?
    scripts/longform_throughput.py point  <cell>...     the matched-byte point
    scripts/longform_throughput.py ladder <cell>...     five rungs, both arms
    scripts/longform_throughput.py rescore <cell>...    re-score, no re-solve
    scripts/longform_throughput.py timed  <cell>...     TIMED: G3 + SIMD t1
    scripts/longform_throughput.py purec  <cell>...     TIMED: sampled G1/G2
    scripts/longform_throughput.py validate             TIMED: the check cell
    scripts/longform_throughput.py stage                copy USB-bound masters in
    scripts/longform_throughput.py report [<cell>...]

Bare `all` means every cell that exists; `full` every full-title cell;
`segments` every staged segment.

Everything except timed/purec/validate is untimed, reads no clock for a
result, and runs under the box share (SOLVE_THREADS 4 for an encoder plus a
one-thread feeder; VMAF_THREADS 3 + FF_THREADS 2 + a feeder when scoring).
The timed commands refuse to start on a loaded box (LOAD_MAX).
"""
import os, sys, json, subprocess, re, shutil, math, tempfile, time, hashlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scratch                                                  # noqa: E402
import bdcompare as bd                                          # noqa: E402

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STAGE   = os.environ.get("LF_STAGE", os.path.expanduser("~/media/throughput"))
MEDIA   = os.environ.get("LF_MEDIA", "/Volumes/seagate/media")
# Masters copied to the internal disk by `stage`, used in place of the USB
# copy when present and the same size (see USB_MBPS).
MASTERS = os.environ.get("LF_MASTERS", os.path.join(_ROOT, "scratch", "masters"))
FEED    = os.path.join(_ROOT, "scripts", "lf_feed.py")
YAH264  = os.environ.get("YAH264", os.path.join(_ROOT, "build-rel", "cli", "yah264"))
# THE TWO REFERENCE BINARIES, PINNED. The SIMD arm is the installed CLI,
# 0.165.3222 b35605a. The pure-C arm is the same commit built with
# --disable-asm and -fno-tree-vectorize stripped from config.mak (the fair
# build: our pure-C arm is a runtime dispatch switch whose C is still
# auto-vectorized, so a stock --disable-asm reference would be scalar C
# against vectorized C; docs/instruments.md "which reference binary").
# Build recipe: git clone --shared ../x264 <dir> && git checkout b35605a &&
# ./configure --disable-asm --disable-lavf --disable-ffms --disable-avs
# --disable-swscale --disable-lsmash --bit-depth=8, strip the flag, make x264.
X264    = os.environ.get("X264", "/opt/homebrew/bin/x264")
X264_C  = os.environ.get("X264_C", os.path.expanduser(
    "~/src/build/x264cli-purec-b35605a/x264"))
FFMPEG  = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")
VMAF    = os.environ.get("VMAF", "vmaf")
PRESET  = os.environ.get("PRESET", "medium")
# Our pure-C arm: the same binary with the runtime dispatch forced to C. The
# knob is PRESENCE-based (NO_ASM=0 still forces scalar), so the SIMD arm must
# REMOVE it, not set it to 0 -- the same as scripts/ffboard.py.
PURE_C_ENV = ("YAH264_NO_ASM", "NEXT264_NO_ASM")
# The box share for the untimed phases. Six threads IN TOTAL: an encoder at
# SOLVE_THREADS plus a one-thread feeder, or VMAF + the bitstream decode + a
# feeder. An unpinned ffmpeg takes a thread per core.
VMAF_THREADS = os.environ.get("VMAF_THREADS", "3")
FF_THREADS   = os.environ.get("FF_THREADS", "2")
FEED_THREADS = os.environ.get("FEED_THREADS", "1")
# Decoders the feeder runs in parallel. Untimed: one (the encoders are at four
# threads and slower than one decoder). Timed, below native size: three, since
# one VideoToolbox stream delivers ~230 fps and both encoders run faster than
# that at 720p. At native size the encoders are far below one stream's rate.
FEED_WORKERS_UNTIMED = int(os.environ.get("FEED_WORKERS_UNTIMED", "1"))
FEED_WORKERS_TIMED   = int(os.environ.get("FEED_WORKERS_TIMED", "3"))
# SCORE EVERY FRAME. At `--bframes 3` the mini-GOP period is 4, so
# `--subsample 4` samples the SAME anchor slot every time: measured on 240
# frames of chimera_1080p at x264 crf 23, subsample 1 reads NEG 90.12,
# subsample 4 reads 89.75. The bias does not cancel in a delta (two arms
# with different IDR placement have different GOP phase). Found by the
# shot-aware sibling, 2026-09-22.
SUBSAMPLE    = os.environ.get("SUBSAMPLE", "1")
# The BD ladder's own window, in seconds from the start of the cell. 0 is the
# whole cell. The matched point and every timed number are always the whole
# cell (or, for purec, the recorded windows).
LADDER_SECS  = float(os.environ.get("LADDER_SECS", "0"))
# THE THREAD COUNT IS PART OF THE OPERATING POINT. Neither encoder is thread-
# count invariant: 120 frames of park_joy_720p at crf 23, yah264 writes
# 4 721 678 / 4 721 255 / 4 692 103 bytes at t6 / auto / t1 (t1 selects the
# single-thread quality path) and x264 4 462 782 / 4 475 437 at t6 / auto.
# The solve runs at SOLVE_THREADS for the box's sake and records it; four or
# six against the default is inside the match bar, one is not. The one-thread
# and pure-C legs reuse the default-thread CRFs by owner decision (2026-09-24)
# and their bytes are therefore not re-matched.
def default_threads(arm):
    return "0" if arm == "ours" else "auto"

SOLVE_THREADS = os.environ.get("SOLVE_THREADS", "4")

def solve_threads(arm):
    return SOLVE_THREADS or default_threads(arm)

# OUR 4K CELLS NEED THE INPUT-MEMORY GUARD RAISED, and nothing else changed.
# The guard prices the window at min(frames, (threads+1) x keyint) frames
# resident; at 18 threads and keyint 250 that refuses any 4K input over about
# 115 s, although the encoder's real window is ~208 frames (chimera_2160p:
# 2880 frames, peak RSS 19.98 GB against 33.4 GiB predicted). On a pipe the
# encoder does not know the length, but the guard is kept raised so a 4K
# full title and a 4K segment run the same configuration.
MEM_ESCAPE = os.environ.get("Y264_MEM_ESCAPE", "60000")
MATCH_BAR = float(os.environ.get("MATCH_BAR", "1.0"))
REVERSE_OFF = os.environ.get("REVERSE_OFF", "0") == "1"
X264_CRF = float(os.environ.get("X264_CRF", "23"))
SIZE_RUNGS = {"720p":  [22.0, 25.0, 28.0, 31.0, 34.0],
              "1080p": [21.0, 24.0, 27.0, 30.0, 33.0],
              "2160p": [19.0, 22.0, 25.0, 28.0, 31.0]}
RUNGS    = ([float(x) for x in os.environ["RUNGS"].split(",")]
            if os.environ.get("RUNGS") else None)

def rungs_for(cell):
    return RUNGS or SIZE_RUNGS[cell.rsplit("_", 1)[1]]

ROUNDS    = int(os.environ.get("ROUNDS", "2"))      # G3 rounds
T1_ROUNDS = int(os.environ.get("T1_ROUNDS", "1"))   # SIMD one-thread rounds
PC_ROUNDS = int(os.environ.get("PC_ROUNDS", "1"))   # pure-C rounds per window
# The stratified sample: one WINDOW_SECS window per STRATUM_SECS of title,
# centred in its stratum. Owner decision 2026-09-24.
WINDOW_SECS  = float(os.environ.get("WINDOW_SECS", "60"))
STRATUM_SECS = float(os.environ.get("STRATUM_SECS", "180"))
VALIDATE_CELL = os.environ.get("VALIDATE_CELL", "tears-full_720p")
# The timed commands refuse a box whose one-minute load is above this.
LOAD_MAX = float(os.environ.get("LOAD_MAX", "1.5"))
CACHE    = scratch.cache_dir("longform")
RESULTS  = os.path.join(CACHE, "results.json")
TSUFFIX  = ":stdin"          # every valid timed record; by-path ones are void

# ---------------------------------------------------------------------------
# The sources.

# SEGMENTS: title -> (label, class, staged basename pattern). A segment that
# is not on disk is simply not a cell.
SEGMENTS = {
    "chimera":    ("Chimera DCI4k 23.98p, centre-cropped to UHD", "live",
                   "chimera_{size}_120s.y4m"),
    "sollevante": ("Sol Levante UHD 24p",                        "animation",
                   "sollevante_{size}_120s.y4m"),
    # The master is 8-bit 4:2:0 H.264 despite its file name (no transfer tags,
    # checked 2026-09-24), and the 09-21 staging did no tone mapping: decode,
    # lanczos for the two smaller sizes, yuv420p.
    "meridian":   ("Meridian UHD 59.94p",                        "live",
                   "meridian_{size}_90s.y4m"),
}

# FULL TITLES. frames are counted, not trusted from a duration: nb_frames from
# the mp4/mov sample table, the packet count for the mkv, each checked against
# the last packet's timestamp (2026-09-24). t0 is the master's start_time; the
# feeder's timestamp checks read the master's own clock.
FULL = {
    # Meridian: the file is 8-bit 4:2:0 H.264 with no colour tags, whatever
    # its name says, so the conversion the 09-21 segments used (decode, then
    # yuv420p, which is a no-op here) is all there is to reproduce. At native
    # size the VideoToolbox decode is bit-identical to those segments (first
    # 120 frames of meridian_2160p_90s.y4m, checked). The two smaller sizes
    # are scale_vt, not the segments' lanczos: 45.4 dB PSNR-Y apart on 240
    # frames, so a full-title 1080p row and a segment 1080p row are different
    # sources and are never set against each other.
    "meridian-full": dict(file="Meridian_UHD4k5994_HDR_P3PQ.mp4",
                          label="Meridian UHD 59.94p", cls="live",
                          native=(3840, 2160), fps=(60000, 1001), t0=0.0,
                          frames=43093),
    "tears-full":    dict(file="longform/tearsofsteel_4k.mov",
                          label="Tears of Steel 4K 24p", cls="live",
                          native=(3840, 1714), fps=(24, 1), t0=0.0,
                          frames=17616),
    # Still downloading on 2026-09-24. Not a cell until the file is whole; its
    # frame count, rate and start time are probed then and cached.
    "stem2-full":    dict(file="longform/ASC_StEM2_178_UHD_24_100nits_Rec709_Stereo_ProRes422HQ.mov",
                          label="ASC StEM2 UHD 24p", cls="live",
                          native=(3840, 2160), fps=None, t0=None, frames=None,
                          complete_bytes=68581102591, est_frames=24480,
                          notice="StEM2 -- Copyright 2022 -- American Society "
                                 "of Cinematographers -- All rights reserved."),
    "sintel-full":   dict(file="Sintel.2010.4k.mkv",
                          label="Sintel 4K 24p", cls="animation",
                          native=(4096, 1744), fps=(24, 1), t0=0.0,
                          frames=21312),
    "bbb-full":      dict(file="bbb_sunflower_2160p_30fps_normal.mp4",
                          label="Big Buck Bunny 2160p 30p", cls="animation",
                          native=(3840, 2160), fps=(30, 1), t0=0.066667,
                          frames=19036),
    "cosmos-full":   dict(file="cosmos_laundromat_1080p.mp4",
                          label="Cosmos Laundromat 1080p 24p", cls="animation",
                          native=(1920, 804), fps=(24, 1), t0=0.0,
                          frames=17532, sizes=("720p", "1080p")),
}
SIZES = ("720p", "1080p", "2160p")
SEG_GEOM = {"720p": (1280, 720), "1080p": (1920, 1080), "2160p": (3840, 2160)}
SIZE_W = {"720p": 1280, "1080p": 1920}
# The USB drive's sustained read, MB/s. A master read faster than this caps
# the feeder: Tears of Steel is 382 kB a frame, so ~215 fps from USB, below
# what both encoders do at 720p. `stage` copies such masters in.
USB_MBPS = float(os.environ.get("USB_MBPS", "82"))


def split(cell):
    title, size = cell.rsplit("_", 1)
    return title, size


def is_full(cell):
    return split(cell)[0] in FULL


def master_path(title):
    f = FULL[title]
    usb = os.path.join(MEDIA, f["file"])
    local = os.path.join(MASTERS, os.path.basename(f["file"]))
    try:
        if os.path.getsize(local) == os.path.getsize(usb):
            return local
    except OSError:
        pass
    return usb


def master_ready(title):
    f = FULL[title]
    p = os.path.join(MEDIA, f["file"])
    if not os.path.exists(p):
        return False
    return os.path.getsize(p) == f.get("complete_bytes", os.path.getsize(p))


def probe_master(title):
    """Fill fps/t0/frames for a master the table could not pin in advance."""
    f = FULL[title]
    if f["frames"]:
        return f
    pc = os.path.join(CACHE, "probe.json")
    try:
        with open(pc) as fh:
            cache = json.load(fh)
    except Exception:
        cache = {}
    path = os.path.join(MEDIA, f["file"])
    key = f"{title}:{os.path.getsize(path)}"
    if key not in cache:
        r = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0",
                            "-count_packets", "-show_entries",
                            "stream=r_frame_rate,start_time,nb_read_packets",
                            "-of", "json", path], capture_output=True, text=True)
        st = json.loads(r.stdout)["streams"][0]
        num, den = (int(x) for x in st["r_frame_rate"].split("/"))
        cache[key] = {"fps": [num, den], "t0": float(st.get("start_time") or 0),
                      "frames": int(st["nb_read_packets"])}
        with open(pc, "w") as fh:
            json.dump(cache, fh, indent=1)
    f.update(fps=tuple(cache[key]["fps"]), t0=cache[key]["t0"],
             frames=cache[key]["frames"])
    return f


def even(x):
    return int(x / 2 + 0.5) * 2


def full_geom(title, size):
    nw, nh = FULL[title]["native"]
    if size == "2160p" or SIZE_W[size] >= nw:
        return nw, nh
    w = SIZE_W[size]
    return w, even(w * nh / nw)


def cell_path(cell):
    title, size = split(cell)
    return os.path.join(STAGE, SEGMENTS[title][2].format(size=size))


def cell_info(cell):
    """frames, (fps num, den), w, h, class, label -- for either kind."""
    title, size = split(cell)
    if title in FULL:
        f = probe_master(title) if master_ready(title) else FULL[title]
        w, h = full_geom(title, size)
        frames = f["frames"] or f.get("est_frames")
        return dict(frames=frames, fps=f["fps"] or (24, 1), w=w, h=h,
                    cls=f["cls"], label=f["label"], full=True)
    frames, fps, w, h, num, den = y4m_shape(cell_path(cell))
    return dict(frames=frames, fps=(num, den), w=w, h=h,
                cls=SEGMENTS[title][1], label=SEGMENTS[title][0], full=False)


def fps_of(info):
    return info["fps"][0] / info["fps"][1]


def all_cells(which="all"):
    out = []
    if which in ("all", "full"):
        for t, f in FULL.items():
            if master_ready(t):
                out += [f"{t}_{s}" for s in f.get("sizes", SIZES)]
    if which in ("all", "segments"):
        out += [f"{t}_{s}" for t in SEGMENTS for s in SIZES
                if os.path.exists(cell_path(f"{t}_{s}"))]
    return out


def ordered(cells):
    """Live action first, then animation (owner rule), each by title."""
    live = [c for c in cells if cell_info(c)["cls"] == "live"]
    anim = [c for c in cells if c not in live]
    return live, anim


def y4m_shape(path):
    """(frames, fps, w, h, num, den) off the header and the file size. A raw
    Y4M has no frame index and ffprobe says nb_frames=N/A for all of these,
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
    return frames, num / den, w, h, num, den


def windows(cell):
    """The stratified sample: [(start, count)], one WINDOW_SECS window per
    STRATUM_SECS of title, centred in its stratum. A pure function of the
    frame count and rate, so the offsets are fixed; every result records
    them as well."""
    info = cell_info(cell)
    fps, frames = fps_of(info), info["frames"]
    n = max(1, int(frames / fps / STRATUM_SECS + 0.5))
    wlen = min(frames, int(round(WINDOW_SECS * fps)))
    st = frames / n
    return [(int(k * st + (st - wlen) / 2), wlen) for k in range(n)]


def feeder(cell, start=0, count=None, workers=None):
    """argv of the process that writes the cell (or frames start..start+count)
    as Y4M on stdout."""
    info = cell_info(cell)
    count = count or info["frames"] - start
    if not is_full(cell):
        return [sys.executable, FEED, "y4m", "--src", cell_path(cell),
                "--start", str(start), "--count", str(count)]
    title, size = split(cell)
    f = FULL[title]
    nw, nh = f["native"]
    if workers is None:
        workers = FEED_WORKERS_UNTIMED
    return [sys.executable, FEED, "vt", "--src", master_path(title),
            "--native", f"{nw}x{nh}", "--size", f"{info['w']}x{info['h']}",
            "--fps", f"{info['fps'][0]}/{info['fps'][1]}",
            "--t0", f"{f['t0'] or 0:.6f}", "--start", str(start),
            "--count", str(count), "--workers", str(workers),
            "--threads", FEED_THREADS]


def timed_workers(cell):
    if not is_full(cell):
        return 1
    title, size = split(cell)
    return 1 if full_geom(title, size) == FULL[title]["native"] else FEED_WORKERS_TIMED


def results():
    try:
        with open(RESULTS) as f:
            return json.load(f)
    except Exception:
        return {}


def put(key, val):
    """Read, add, write, atomically (sibling + rename). NOT safe for two
    processes at once: run one at a time."""
    r = results(); r[key] = val
    tmp = RESULTS + ".new"
    with open(tmp, "w") as f:
        json.dump(r, f, indent=1, sort_keys=True)
        f.flush(); os.fsync(f.fileno())
    os.replace(tmp, RESULTS)
    return val


# ---------------------------------------------------------------------------
# The two encoders, spelled once. Both read stdin.

def enc_cmd(arm, crf, out, threads, purec=False):
    if arm == "ours":
        return [YAH264, "--input-y4m", "-", "--preset", PRESET,
                "--crf", f"{crf:.3f}", "--threads", str(threads),
                "-o", out, "--quiet", "--no-progress"]
    return [X264_C if purec else X264, "--demuxer", "y4m", "--preset", PRESET,
            "--crf", f"{crf:.3f}", "--threads", str(threads), "-o", out,
            "--quiet", "-"]


def arm_env(arm, cell, purec=False):
    e = dict(os.environ)
    for k in PURE_C_ENV:
        e.pop(k, None)
    if arm == "ours":
        if purec:
            for k in PURE_C_ENV:
                e[k] = "1"
        if cell.endswith("_2160p") and MEM_ESCAPE:
            e["Y264_MAX_INPUT_MB"] = MEM_ESCAPE
    return e


def env_prefix(arm, cell, purec=False):
    """The environment an arm runs under, as a shell would spell it."""
    bits = [f"-u {k}" for k in PURE_C_ENV] if not purec else \
           [f"{k}=1" for k in PURE_C_ENV]
    if arm == "ours" and cell.endswith("_2160p") and MEM_ESCAPE:
        bits.append(f"Y264_MAX_INPUT_MB={MEM_ESCAPE}")
    return "env " + " ".join(bits) if arm == "ours" else \
           "env " + " ".join(f"-u {k}" for k in PURE_C_ENV)


# macOS `time -l`: the three times on ONE line, value first, then the rusage
# block, also value first. Not the `-p` format.
TIME_RE = {
    "real":   re.compile(r"([\d.]+)\s+real\b"),
    "user":   re.compile(r"([\d.]+)\s+user\b"),
    "sys":    re.compile(r"([\d.]+)\s+sys\b"),
    "maxrss": re.compile(r"(\d+)\s+maximum resident set size"),
}


def parse_time(rep):
    got = {}
    for k, rx in TIME_RE.items():
        m = rx.search(rep)
        if not m:
            sys.exit(f"longform: /usr/bin/time -l printed no '{k}' -- "
                     f"nothing is recorded\n{rep[-400:]}")
        got[k] = float(m.group(1))
    return got["real"], got["user"] + got["sys"], got["maxrss"]


def timed_run(cmd, e=None):
    """(wall, cpu_seconds, peak_rss_bytes) of one process, stdout discarded.
    Used for the read baseline: the feeder alone."""
    r = subprocess.run(["/usr/bin/time", "-l"] + cmd, env=e,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.exit(f"longform: failed ({r.returncode}): {' '.join(cmd[:6])} ...\n"
                 f"  {r.stderr.decode(errors='replace')[-600:]}")
    return parse_time(r.stderr.decode(errors="replace"))


def pipe_run(feed, enc, env, timed=False):
    """feeder | encoder. Returns a dict; with timed=True the encoder's own
    wall, CPU and peak RSS from /usr/bin/time, plus the feeder's CPU and the
    whole pipeline's wall. A feeder failure is fatal even when the encoder
    succeeded -- an encoder fed a truncated stream finishes early and
    reports a flattering wall."""
    ferr = tempfile.TemporaryFile()
    t0 = time.monotonic()
    fp = subprocess.Popen(feed, stdout=subprocess.PIPE, stderr=ferr)
    ep = subprocess.Popen((["/usr/bin/time", "-l"] if timed else []) + enc,
                          stdin=fp.stdout, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE, env=env)
    fp.stdout.close()
    _, eerr = ep.communicate()
    _, st, ru = os.wait4(fp.pid, 0)
    fp.returncode = os.waitstatus_to_exitcode(st)
    wall = time.monotonic() - t0
    ferr.seek(0)
    ftxt = ferr.read().decode(errors="replace")
    etxt = eerr.decode(errors="replace")
    if ep.returncode != 0:
        sys.exit(f"longform: encode failed ({ep.returncode}): "
                 f"{' '.join(enc[:4])} ...\n  {etxt[-600:]}\n  feeder: {ftxt[-300:]}")
    if fp.returncode != 0:
        sys.exit(f"longform: the feeder failed ({fp.returncode}), so the encode "
                 f"saw a short stream and is discarded:\n  {ftxt[-600:]}")
    out = {"pipe_wall": wall, "feed_cpu": ru.ru_utime + ru.ru_stime,
           "feed_rss": ru.ru_maxrss}
    if timed:
        out["wall"], out["cpu"], out["rss"] = parse_time(etxt)
    return out


def encode(arm, cell, crf, out, threads, start=0, count=None, purec=False):
    """Untimed: encode frames start..start+count of the cell into `out`."""
    pipe_run(feeder(cell, start, count), enc_cmd(arm, crf, out, threads, purec),
             arm_env(arm, cell, purec))
    if not os.path.exists(out) or os.path.getsize(out) < 1024:
        sys.exit(f"longform: {arm} wrote no bitstream for {cell} crf {crf}")
    return os.path.getsize(out)


def _fifo(wd, name):
    p = os.path.join(wd, name)
    if os.path.exists(p):
        os.remove(p)
    os.mkfifo(p)
    return p


def score(cell, bitstream, wd, start=0, count=None):
    """(VMAF-NEG, PSNR-Y) of a bitstream against the frames it was made from.

    Nothing lands on disk. The reference is the SAME feeder that fed the
    encoder, into libvmaf's stdin; the bitstream is decoded by ffmpeg into a
    named pipe. libvmaf aligns frame for frame, so the scored count is checked
    against the cell's: a short pass is a pooled mean over the wrong frames."""
    info = cell_info(cell)
    count = count or info["frames"] - start
    dfifo = _fifo(wd, "dec.fifo")
    j = os.path.join(wd, "v.json")
    dec = subprocess.Popen(
        [FFMPEG, "-v", "error", "-y", "-threads", FF_THREADS,
         "-i", bitstream, "-pix_fmt", "yuv420p", "-f", "yuv4mpegpipe", dfifo],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    ferr = tempfile.TemporaryFile()
    fp = subprocess.Popen(feeder(cell, start, count), stdout=subprocess.PIPE,
                          stderr=ferr)
    v = subprocess.run(
        [VMAF, "--threads", VMAF_THREADS, "--subsample", SUBSAMPLE,
         "-r", "/dev/stdin", "-d", dfifo, "--json", "-o", j, "--feature", "psnr",
         "--model", "version=vmaf_v0.6.1neg:name=neg"],
        stdin=fp.stdout, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    fp.stdout.close()
    frc = fp.wait()
    dec.wait()
    os.remove(dfifo)
    if v.returncode != 0:
        sys.exit(f"longform: vmaf failed on {bitstream}\n"
                 f"  {v.stderr.decode(errors='replace')[-600:]}")
    if frc != 0:
        ferr.seek(0)
        sys.exit(f"longform: the reference feeder failed ({frc}):\n  "
                 f"{ferr.read().decode(errors='replace')[-600:]}")
    with open(j) as f:
        jj = json.load(f)
    want = (count + int(SUBSAMPLE) - 1) // int(SUBSAMPLE)
    if len(jj["frames"]) != want:
        sys.exit(f"longform: vmaf scored {len(jj['frames'])} frames of {cell}, "
                 f"wanted {want}")
    pooled = jj["pooled_metrics"]
    return pooled["neg"]["mean"], pooled["psnr_y"]["mean"]


# ---------------------------------------------------------------------------
# Untimed.

def do_ladder(cell):
    info = cell_info(cell)
    frames, fps = info["frames"], fps_of(info)
    lf = min(frames, int(round(LADDER_SECS * fps))) if LADDER_SECS else frames
    lim = lf if lf < frames else None
    wd = scratch.mkdtemp("longform")
    try:
        for arm in ("x264", "ours"):
            for crf in rungs_for(cell):
                key = (f"ladder:{cell}:{arm}:{crf:.1f}:{PRESET}"
                       + (f":f{lf}" if lim else ""))
                if key in results():
                    print(f"  {cell:<20}{arm:<6}crf {crf:>5.1f}  cached", flush=True)
                    continue
                out = os.path.join(wd, "l.264")
                sz = encode(arm, cell, crf, out, solve_threads(arm), 0, lim)
                neg, psnr = score(cell, out, wd, 0, lim)
                put(key, {"bytes": sz, "neg": neg, "psnr_y": psnr,
                          "kbps": sz * 8 / (lf / fps) / 1000, "frames": lf,
                          "fps": fps, "threads": solve_threads(arm),
                          "subsample": SUBSAMPLE})
                print(f"  {cell:<20}{arm:<6}crf {crf:>5.1f}  "
                      f"{sz*8/(lf/fps)/1000:>9.0f} kbit/s  NEG {neg:6.2f}  "
                      f"PSNR-Y {psnr:6.2f}   ({lf} frames)", flush=True)
    finally:
        shutil.rmtree(wd, ignore_errors=True)


def _secant(arm, cell, target, crf, slope, wd, tag):
    """Solve `arm`'s rate factor onto `target` bytes. [(crf, bytes)], best."""
    best, pts = None, []
    work = os.path.join(wd, f"{tag}.264")
    keep = os.path.join(wd, f"{tag}.best.264")
    for _ in range(8):
        crf = round(max(8.0, min(45.0, crf)), 3)
        if any(abs(crf - c) < 1e-6 for c, _ in pts):
            break                          # asked the same question twice
        t = time.monotonic()
        sz = encode(arm, cell, crf, work, solve_threads(arm))
        el = time.monotonic() - t
        err = abs(sz - target) / target
        if best is None or err < best[2]:
            best = (crf, sz, err)
            shutil.copyfile(work, keep)
        print(f"    solve {arm} crf {crf:6.3f} -> {sz} bytes "
              f"({100*(sz/target-1):+.2f}%)  [{el:.0f} s, untimed]", flush=True)
        pts.append((crf, sz))
        if err < 0.005:
            break
        # A local slope from the last two points beats the prior, except when
        # both sit on one step of the staircase (slope zero or wrong-signed).
        if len(pts) >= 2:
            (ca, ba), (cb, bb) = pts[-2], pts[-1]
            if ba > 0 and bb > 0 and abs(cb - ca) > 1e-6:
                m = math.log(bb / ba) / (cb - ca)
                if m < -0.02:
                    slope = min(-0.05, max(-0.30, m))
        crf = crf + math.log(target / sz) / slope
    return pts, best, keep


def do_point(cell):
    """x264 at its own default rate factor; ours solved onto its bytes.

    SOLVE BY SECANT, NOT BY BISECTION. Bytes against rate factor is very close
    to a straight line in the log (slopes -0.11 to -0.15 a point on every cell
    so far), so encode once, step by ln(target/actual)/slope, and update the
    slope from the last two points. Three to five encodes instead of sixteen,
    which at a full 4K title is the difference between an evening and a week.

    WHEN OUR CURVE STEPS OVER THE TARGET, MOVE THE REFERENCE INSTEAD. The
    frame QP is an integer, so the curve is a staircase, and a target inside a
    step has no rate factor that reaches it (chimera_1080p: crf 22.945 writes
    61 243 233 bytes, 22.969 writes 57 093 569, x264's 58 863 822 between).
    Then x264, whose staircase lands elsewhere, is solved onto our nearest
    point, and the record says so."""
    info = cell_info(cell)
    frames, fps = info["frames"], fps_of(info)
    wd = scratch.mkdtemp("longform")
    try:
        xkey = f"point:{cell}:x264:{X264_CRF:.1f}:{PRESET}:tdefault"
        if xkey in results():
            xr = results()[xkey]
            print(f"  {cell:<20}x264  crf {xr['crf']:>6.3f}  cached "
                  f"{xr['bytes']} bytes", flush=True)
        else:
            out = os.path.join(wd, "x.264")
            t = time.monotonic()
            sz = encode("x264", cell, X264_CRF, out, solve_threads("x264"))
            el = time.monotonic() - t
            neg, psnr = score(cell, out, wd)
            xr = put(xkey, {"crf": X264_CRF, "bytes": sz, "neg": neg,
                            "psnr_y": psnr, "frames": frames, "fps": fps,
                            "solve_threads": solve_threads("x264"),
                            "subsample": SUBSAMPLE, "untimed_encode_s": el,
                            "kbps": sz * 8 / (frames / fps) / 1000})
            print(f"  {cell:<20}x264  crf {X264_CRF:>5.1f}  {sz} bytes  "
                  f"NEG {neg:6.2f}  PSNR-Y {psnr:6.2f}", flush=True)

        if next((k for k in results()
                 if k.startswith(f"point:{cell}:ours:onto{xr['bytes']}:")), None):
            nr = next(v for k, v in results().items()
                      if k.startswith(f"point:{cell}:ours:onto{xr['bytes']}:"))
            print(f"  {cell:<20}ours  crf {nr['crf']:>6.3f}  cached "
                  f"{nr['bytes']} bytes", flush=True)
            return
        # The prior slope: from our ladder if there is one, else the typical.
        lad = sorted([(float(k.split(":")[3]), v["bytes"], v["frames"])
                      for k, v in results().items()
                      if k.startswith(f"ladder:{cell}:ours:")])
        slope = -0.13
        if len(lad) >= 2:
            (c0, b0, _), (c1, b1, _) = lad[0], lad[-1]
            if b0 > 0 and b1 > 0 and c1 > c0:
                slope = math.log(b1 / b0) / (c1 - c0)
        slope = min(-0.05, max(-0.30, slope))
        scaled = [(c, b * frames / f) for c, b, f in lad if f]
        under = [c for c, b in scaled if b <= xr["bytes"]]
        start = min(under) if under else (max(c for c, _ in scaled) if scaled
                                          else X264_CRF)
        target = xr["bytes"]
        pts, best, nbest = _secant("ours", cell, target, start, slope, wd, "n")
        if best[2] > MATCH_BAR / 100 and len(pts) >= 2:
            near = sorted(pts, key=lambda cb: abs(cb[1] - target))[:2]
            if abs(near[0][0] - near[1][0]) < 0.01:
                print(f"    the two nearest rate factors are {near[0][0]:.3f} "
                      f"and {near[1][0]:.3f} and write {near[0][1]} and "
                      f"{near[1][1]} bytes: the target is inside a step",
                      flush=True)
        nkey = f"point:{cell}:ours:onto{xr['bytes']}:{PRESET}:tdefault"
        if best[2] > MATCH_BAR / 100 and not REVERSE_OFF:
            print(f"    our curve steps over the target ({100*best[2]:.2f}% at "
                  f"crf {best[0]:.3f}); matching x264 onto our point instead",
                  flush=True)
            xcrf = X264_CRF + math.log(best[1] / xr["bytes"]) / slope
            _, xbest, xfile = _secant("x264", cell, best[1], xcrf, slope, wd, "x")
            if xbest[2] < best[2]:
                xneg, xpsnr = score(cell, xfile, wd)
                xr = put(xkey, {"crf": xbest[0], "bytes": xbest[1], "neg": xneg,
                                "psnr_y": xpsnr, "frames": frames, "fps": fps,
                                "solve_threads": solve_threads("x264"),
                                "subsample": SUBSAMPLE,
                                "matched": "reverse: x264 moved onto ours",
                                "kbps": xbest[1] * 8 / (frames / fps) / 1000})
                nkey = f"point:{cell}:ours:onto{xbest[1]}:{PRESET}:tdefault"
                best = (best[0], best[1], xbest[2])
                print(f"  {cell:<20}x264  crf {xbest[0]:>6.3f}  {xbest[1]} bytes"
                      f"  (reverse match)  NEG {xneg:6.2f}  PSNR-Y {xpsnr:6.2f}",
                      flush=True)
        if best[2] > MATCH_BAR / 100:
            print(f"    [warn] {cell}: {100*best[2]:.2f}% apart at the closest "
                  "reachable pair -- the timed legs will refuse it", flush=True)
        neg, psnr = score(cell, nbest, wd)
        put(nkey, {"crf": best[0], "bytes": best[1], "neg": neg,
                   "psnr_y": psnr, "frames": frames, "fps": fps,
                   "kbps": best[1] * 8 / (frames / fps) / 1000,
                   "solve_threads": solve_threads("ours"), "subsample": SUBSAMPLE,
                   "dsize_pct": 100 * (best[1] / xr["bytes"] - 1)})
        print(f"  {cell:<20}ours  crf {best[0]:>6.3f}  {best[1]} bytes  "
              f"({100*(best[1]/xr['bytes']-1):+.2f}%)  NEG {neg:6.2f}  "
              f"PSNR-Y {psnr:6.2f}", flush=True)
    finally:
        shutil.rmtree(wd, ignore_errors=True)


def do_rescore(cell):
    """Re-score every cached point/ladder entry of a cell at the CURRENT
    subsample without solving. The re-encode must reproduce the recorded byte
    count or the entry is left alone: a different size was a different encode
    (thread count, binary, preset) and its score would silently replace a good
    number with one from somewhere else."""
    info = cell_info(cell)
    frames = info["frames"]
    r = results()
    todo = sorted(k for k in r
                  if k.startswith(f"ladder:{cell}:") or k.startswith(f"point:{cell}:"))
    wd = scratch.mkdtemp("longform")
    try:
        for key in todo:
            v = r[key]
            if v.get("subsample") == SUBSAMPLE:
                print(f"  {key}: already at subsample {SUBSAMPLE}", flush=True)
                continue
            arm = key.split(":")[2]
            crf = float(key.split(":")[3]) if key.startswith("ladder:") else v["crf"]
            lim = v["frames"] if v["frames"] < frames else None
            th = v.get("threads") or v.get("solve_threads") or solve_threads(arm)
            out = os.path.join(wd, "r.264")
            sz = encode(arm, cell, crf, out, th, 0, lim)
            if sz != v["bytes"]:
                print(f"  [SKIP] {key}: re-encode wrote {sz} bytes, the record "
                      f"says {v['bytes']} ({100*(sz/v['bytes']-1):+.3f}%). Not "
                      "the same encode -- left alone.", flush=True)
                continue
            neg, psnr = score(cell, out, wd, 0, lim)
            v2 = dict(v)
            v2.update(neg=neg, psnr_y=psnr, subsample=SUBSAMPLE)
            put(key, v2)
            print(f"  {cell:<20}{arm:<6}crf {crf:>6.3f}  NEG {v['neg']:6.2f} -> "
                  f"{neg:6.2f}  PSNR-Y {v['psnr_y']:6.2f} -> {psnr:6.2f}",
                  flush=True)
    finally:
        shutil.rmtree(wd, ignore_errors=True)


def _frame_md5s(argv, per, hdr_expect=None):
    """Per-frame md5 of a feeder's output, and its header."""
    p = subprocess.Popen(argv, stdout=subprocess.PIPE)
    hdr = p.stdout.readline()
    out = []
    while True:
        b = p.stdout.read(per)
        if not b:
            break
        if len(b) != per:
            sys.exit("longform: a feeder wrote a partial frame")
        out.append(hashlib.md5(b).hexdigest())
    if p.wait() != 0:
        sys.exit(f"longform: feeder failed ({p.returncode}): {' '.join(argv[:4])}")
    return hdr, out


def do_verify_feed(cell):
    """Untimed. The chunked feeder the timed legs use against one sequential
    decode, frame by frame: the whole title, then every purec window. A
    mismatch means the chunk cuts or the window seeks do not reproduce the
    title, and no timed number from this cell is valid until it is fixed."""
    if not is_full(cell):
        sys.exit("longform: verify-feed is for full-title cells")
    info = cell_info(cell)
    per = info["w"] * info["h"] * 3 // 2 + len(b"FRAME\n")
    w = max(2, timed_workers(cell))
    t = time.monotonic()
    h1, seq = _frame_md5s(feeder(cell, workers=1), per)
    print(f"  {cell}: sequential {len(seq)} frames [{time.monotonic()-t:.0f} s]",
          flush=True)
    t = time.monotonic()
    h2, chk = _frame_md5s(feeder(cell, workers=w), per)
    bad = [i for i, (a, b) in enumerate(zip(seq, chk)) if a != b]
    ok = h1 == h2 and len(seq) == len(chk) == info["frames"] and not bad
    print(f"  {cell}: chunked x{w} {len(chk)} frames [{time.monotonic()-t:.0f} s]"
          f"  {'IDENTICAL' if ok else 'DIFFERS'}"
          + (f" (first bad frame {bad[0]}, {len(bad)} bad)" if bad else ""),
          flush=True)
    wins = []
    for s, n in windows(cell):
        _, wm = _frame_md5s(feeder(cell, s, n, workers=w), per)
        wok = wm == seq[s:s + n]
        wins.append([s, n, wok])
        ok = ok and wok
        print(f"  {cell}: window {s}+{n}  {'IDENTICAL' if wok else 'DIFFERS'}",
              flush=True)
    put(f"verify-feed:{cell}", {"ok": ok, "frames": len(seq), "workers": w,
                                "windows": wins, "header": h1.decode().strip(),
                                "date": time.strftime("%Y-%m-%d %H:%M")})
    if not ok:
        sys.exit(f"longform: {cell}: the feeder does NOT reproduce the title")


def do_stage():
    """Copy the masters that the USB drive cannot feed fast enough to the
    internal disk (MASTERS). Only those whose bytes-per-frame caps the feeder
    below FAST_FPS; everything else reads fine from the drive."""
    fast = float(os.environ.get("FAST_FPS", "450"))
    os.makedirs(MASTERS, exist_ok=True)
    for t, f in FULL.items():
        if not master_ready(t):
            continue
        usb = os.path.join(MEDIA, f["file"])
        info = probe_master(t)
        cap = USB_MBPS * 1e6 / (os.path.getsize(usb) / info["frames"])
        dst = os.path.join(MASTERS, os.path.basename(f["file"]))
        if cap >= fast:
            print(f"  {t}: USB feeds ~{cap:.0f} fps, not staged")
            continue
        if os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(usb):
            print(f"  {t}: already staged ({os.path.getsize(dst)/1e9:.1f} GB)")
            continue
        free = shutil.disk_usage(os.path.dirname(MASTERS)).free
        if free - os.path.getsize(usb) < 40e9:
            print(f"  {t}: NOT staged, would leave {(free-os.path.getsize(usb))/1e9:.0f}"
                  " GB free (floor 40)")
            continue
        print(f"  {t}: USB feeds ~{cap:.0f} fps; copying "
              f"{os.path.getsize(usb)/1e9:.1f} GB ...", flush=True)
        shutil.copyfile(usb, dst + ".part")
        os.replace(dst + ".part", dst)


# ---------------------------------------------------------------------------
# Timed.

def load_guard():
    l1 = os.getloadavg()[0]
    print(f"  load average {l1:.2f} (LOAD_MAX {LOAD_MAX:g})", flush=True)
    if l1 > LOAD_MAX and os.environ.get("LF_FORCE") != "1":
        sys.exit("longform: the box is not idle; a timed leg would measure the "
                 "neighbours. LF_FORCE=1 overrides, and the record says so.")


def matched(cell):
    r = results()
    xr = r.get(f"point:{cell}:x264:{X264_CRF:.1f}:{PRESET}:tdefault")
    if not xr:
        sys.exit(f"longform: {cell} has no matched point -- run "
                 f"`{sys.argv[0]} point {cell}` first (untimed)")
    nr = next((v for k, v in r.items()
               if k.startswith(f"point:{cell}:ours:onto{xr['bytes']}:")), None)
    if not nr:
        sys.exit(f"longform: {cell} has no solved point for ours")
    # A wall ratio at two file sizes is not a speed reading.
    if abs(nr["dsize_pct"]) > MATCH_BAR:
        sys.exit(f"longform: {cell} is not rate-matched ({nr['dsize_pct']:+.2f}%,"
                 f" bar +/-{MATCH_BAR:g}%)")
    if is_full(cell):
        v = r.get(f"verify-feed:{cell}")
        if not (v and v["ok"]):
            sys.exit(f"longform: {cell}: the chunked feeder has not been verified "
                     f"against a sequential decode -- run "
                     f"`{sys.argv[0]} verify-feed {cell}` first (untimed)")
    return xr, nr


def leg(arm, cell, crf, threads, purec, start=0, count=None, workers=1):
    r = pipe_run(feeder(cell, start, count, workers),
                 enc_cmd(arm, crf, "/dev/null", threads, purec),
                 arm_env(arm, cell, purec), timed=True)
    return [r["wall"], r["cpu"], r["rss"], r["feed_cpu"], r["pipe_wall"]]


def read_baseline(cell, start=0, count=None, workers=1):
    wall, cpu, rss = timed_run(feeder(cell, start, count, workers))
    return [wall, cpu, rss]


def stamp():
    return {"date": time.strftime("%Y-%m-%d %H:%M"),
            "load1": os.getloadavg()[0], "forced": os.environ.get("LF_FORCE") == "1",
            "x264": X264, "x264_c": X264_C, "yah264": YAH264,
            "versions": versions()}


def versions():
    v = {}
    for name, b in (("x264", X264), ("x264_c", X264_C)):
        try:
            v[name] = subprocess.run([b, "--version"], capture_output=True,
                                     text=True).stdout.splitlines()[0]
        except Exception as e:
            v[name] = f"unavailable ({e})"
    try:
        v["yah264"] = subprocess.run(["git", "-C", _ROOT, "rev-parse", "--short",
                                      "HEAD"], capture_output=True,
                                     text=True).stdout.strip()
    except Exception:
        pass
    return v


def do_timed(cell):
    """G3 (SIMD, default threads, ROUNDS rounds, order reversed each round),
    SIMD one-thread on the whole cell, then the read baseline. Output to
    /dev/null: the bytes are known from the untimed point."""
    xr, nr = matched(cell)
    info = cell_info(cell)
    frames = info["frames"]
    w = timed_workers(cell)
    out = {"cell": cell, "frames": frames, "fps": fps_of(info), "w": info["w"],
           "h": info["h"], "x264_crf": xr["crf"], "ours_crf": nr["crf"],
           "x264_bytes": xr["bytes"], "ours_bytes": nr["bytes"],
           "dsize_pct": nr["dsize_pct"], "x264_neg": xr["neg"],
           "ours_neg": nr["neg"], "x264_psnr_y": xr["psnr_y"],
           "ours_psnr_y": nr["psnr_y"], "rounds": ROUNDS, "feed_workers": w,
           "fields": "wall, cpu, rss, feeder cpu, pipeline wall", **stamp()}
    crfs = {"ours": nr["crf"], "x264": xr["crf"]}
    for arm in crfs:
        out[arm] = {"default": [], "t1": []}
    for i in range(ROUNDS):
        for arm in (("x264", "ours") if i % 2 else ("ours", "x264")):
            m = leg(arm, cell, crfs[arm], default_threads(arm), False, workers=w)
            out[arm]["default"].append(m)
            print(f"  {cell:<22}{arm:<6}G3 r{i+1}  {m[0]:8.2f}s wall {m[1]:9.2f}s "
                  f"cpu  {m[2]/2**30:5.2f} GiB  {frames/m[0]:7.1f} fps  "
                  f"(feeder {m[3]:.1f} cpu-s)", flush=True)
    for i in range(T1_ROUNDS):
        for arm in (("x264", "ours") if i % 2 else ("ours", "x264")):
            m = leg(arm, cell, crfs[arm], 1, False, workers=w)
            out[arm]["t1"].append(m)
            print(f"  {cell:<22}{arm:<6}t1 r{i+1}  {m[0]:8.2f}s wall {m[1]:9.2f}s "
                  f"cpu  {frames/m[0]:7.1f} fps", flush=True)
    out["read"] = read_baseline(cell, workers=w)
    print(f"  {cell:<22}read  baseline {out['read'][0]:8.2f}s wall "
          f"{out['read'][1]:9.2f}s cpu  {frames/out['read'][0]:7.1f} fps", flush=True)
    put(f"timed:{cell}:{PRESET}{TSUFFIX}", out)


def do_purec(cell):
    """G1 (pure C, one thread) and G2 (pure C, default threads) on the
    stratified windows, both arms, order reversed window by window, and a read
    baseline per window. The CRFs are the full-title matched ones."""
    xr, nr = matched(cell)
    info = cell_info(cell)
    w = timed_workers(cell)
    wins = windows(cell)
    crfs = {"ours": nr["crf"], "x264": xr["crf"]}
    out = {"cell": cell, "frames": info["frames"], "fps": fps_of(info),
           "windows": wins, "rounds": PC_ROUNDS, "feed_workers": w,
           "crfs": crfs, "fields": "wall, cpu, rss, feeder cpu, pipeline wall",
           "G1": {"ours": [], "x264": []}, "G2": {"ours": [], "x264": []},
           "read": [], **stamp()}
    for wi, (s, n) in enumerate(wins):
        for g, th in (("G1", None), ("G2", None)):
            ms = {"ours": [], "x264": []}
            for r in range(PC_ROUNDS):
                for arm in (("x264", "ours") if (wi + r) % 2 else ("ours", "x264")):
                    t = 1 if g == "G1" else default_threads(arm)
                    m = leg(arm, cell, crfs[arm], t, True, s, n, w)
                    ms[arm].append(m)
                    print(f"  {cell:<22}{arm:<6}{g} w{wi} {s}+{n}  {m[0]:8.2f}s "
                          f"wall {m[1]:9.2f}s cpu  {n/m[0]:7.1f} fps", flush=True)
            for arm in ms:
                out[g][arm].append(ms[arm])
        out["read"].append(read_baseline(cell, s, n, w))
    put(f"purec:{cell}:{PRESET}{TSUFFIX}", out)


def do_validate():
    """The sample's check: pure C, one thread, the WHOLE validation title,
    both arms. `report` sets it beside the sampled G1 estimate."""
    cell = VALIDATE_CELL
    xr, nr = matched(cell)
    info = cell_info(cell)
    w = timed_workers(cell)
    crfs = {"ours": nr["crf"], "x264": xr["crf"]}
    out = {"cell": cell, "frames": info["frames"], "crfs": crfs,
           "feed_workers": w, "fields": "wall, cpu, rss, feeder cpu, pipeline wall",
           **stamp()}
    for arm in ("ours", "x264"):
        m = leg(arm, cell, crfs[arm], 1, True, workers=w)
        out[arm] = m
        print(f"  {cell:<22}{arm:<6}G1 full  {m[0]:8.2f}s wall  "
              f"{info['frames']/m[0]:7.1f} fps", flush=True)
    out["read"] = read_baseline(cell, workers=w)
    put(f"validate:{cell}:{PRESET}{TSUFFIX}", out)


# ---------------------------------------------------------------------------
# The dry run.

# COARSE THROUGHPUT MODEL for the plan, megapixels per second, per encoder
# configuration and size. The SIMD default-thread and one-thread figures are
# our side of the 2026-09-23 segment leg (whose x264 side was input-capped and
# is void; the plan assumes parity, which is the bar) and chimera_2160p's
# 2880 frames in 151.7 s. Pure C is 1.4-1.6x slower at one thread on a 240-
# frame probe (2026-09-24, both encoders); 1.5 is used for both G1 and G2.
# Content moves these by +-50% (sollevante ran at half chimera's rate), so
# an estimate is a booking figure, not a result.
EST_MPXS = {"simd":   {"720p": 280.0, "1080p": 250.0, "2160p": 160.0},
            "simd1":  {"720p": 52.0,  "1080p": 47.0,  "2160p": 40.0}}
PUREC_SLOW = 1.5
# One VideoToolbox decoder delivers ~230 fps of a 4K master (measured
# 2026-09-24: 206-250 fps, Meridian and Tears of Steel); three in parallel
# ~435 after their pre-roll. A 1080p master decodes ~3x faster.
VT_FPS1, VT_FPS3 = 230.0, 435.0


def est_fps(cell, cfg):
    info = cell_info(cell)
    size = split(cell)[1]
    mpx = info["w"] * info["h"] / 1e6
    base = EST_MPXS["simd1" if cfg in ("simd1", "g1") else "simd"][size] / mpx
    return base / PUREC_SLOW if cfg in ("g1", "g2") else base


def feed_fps(cell, workers):
    if not is_full(cell):
        return 3000.0                     # an internal-disk block copy
    title, _ = split(cell)
    f = FULL[title]
    nat = f["native"][0] * f["native"][1]
    vt = (VT_FPS3 if workers > 1 else VT_FPS1) * (3840 * 2160 / nat) ** 0.7
    p = master_path(title)
    if p.startswith(MEDIA):
        size = os.path.getsize(p) if os.path.exists(p) else 0
        frames = cell_info(cell)["frames"]
        if size and frames:
            vt = min(vt, USB_MBPS * 1e6 / (size / frames))
    return vt


def shell(argv):
    return " ".join(a if re.fullmatch(r"[\w@%+=:,./-]+", a) else f"'{a}'"
                    for a in argv)


def do_plan(cells, full_print):
    groups = {"G3 + SIMD t1, full titles": 0.0, "sampled G1/G2 (pure C)": 0.0,
              "validation cell": 0.0, "G3 + SIMD t1, segments": 0.0}
    bound = []
    rows = []
    live, anim = ordered(cells)
    print("# Long-form timed legs -- DRY RUN (nothing below has been run)\n")
    print(f"x264 SIMD arm  : {shell([X264])}  ({versions().get('x264')})")
    print(f"x264 pure-C arm: {shell([X264_C])}  ({versions().get('x264_c')})")
    print(f"yah264         : {shell([YAH264])}  (git {versions().get('yah264')})")
    print(f"G3 rounds {ROUNDS}, SIMD t1 rounds {T1_ROUNDS}, pure-C rounds "
          f"{PC_ROUNDS}; windows {WINDOW_SECS:g} s per {STRATUM_SECS:g} s\n")
    for grp, cs in (("LIVE ACTION", live), ("ANIMATION", anim)):
        if not cs:
            continue
        print(f"## {grp}\n")
        for cell in cs:
            info = cell_info(cell)
            frames = info["frames"]
            w = timed_workers(cell)
            ffps = feed_fps(cell, w)
            r = results()
            xr = r.get(f"point:{cell}:x264:{X264_CRF:.1f}:{PRESET}:tdefault")
            nr = next((v for k, v in r.items() if xr and
                       k.startswith(f"point:{cell}:ours:onto{xr['bytes']}:")), None)
            xc = xr["crf"] if xr else X264_CRF
            oc = nr["crf"] if nr else None
            ver = r.get(f"verify-feed:{cell}")
            state = ("SOLVED" if nr else "NOT SOLVED") + \
                    ("" if not is_full(cell) else
                     (", feed VERIFIED" if ver and ver["ok"] else ", feed NOT VERIFIED"))
            print(f"### {cell}  {info['w']}x{info['h']} {fps_of(info):.3f} fps, "
                  f"{frames} frames ({frames/fps_of(info)/60:.1f} min) -- {state}")
            ocs = f"{oc:.3f}" if oc is not None else "<solve>"
            fd = shell(feeder(cell, workers=w))
            if full_print or cell == (live + anim)[0]:
                print(f"  feed : {fd}")
            else:
                print(f"  feed : {fd[:110]} ...")

            def cmd(arm, crf, th, purec=False, start=0, count=None):
                c = enc_cmd(arm, 0.0, "/dev/null", th, purec)
                c[c.index("--crf") + 1] = crf
                return f"{env_prefix(arm, cell, purec)} {shell(c)}"
            g3 = frames / min(est_fps(cell, "simd"), ffps)
            t1 = frames / min(est_fps(cell, "simd1"), ffps)
            rd = frames / ffps
            leg_s = 2 * ROUNDS * g3 + 2 * T1_ROUNDS * t1 + rd
            print(f"  G3   : ours: <feed> | {cmd('ours', ocs, 0)}")
            print(f"         x264: <feed> | {cmd('x264', f'{xc:.3f}', 'auto')}")
            print(f"  t1   : the same with --threads 1")
            print(f"  read : <feed> > /dev/null")
            print(f"  est  : G3 {g3/60:5.1f} min/run x{2*ROUNDS}, t1 {t1/60:5.1f} "
                  f"min/run x{2*T1_ROUNDS}, read {rd/60:4.1f} min "
                  f"= {leg_s/3600:5.2f} h   (feeder ~{ffps:.0f} fps, "
                  f"{w} decoder{'s' if w > 1 else ''})")
            if est_fps(cell, "simd") > 0.8 * ffps:
                bound.append(cell)
                print(f"  !!   : estimated G3 rate {est_fps(cell, 'simd'):.0f} fps is "
                      f"within 20% of the feeder's ~{ffps:.0f}: FEEDER-BOUND RISK")
            grp_key = ("G3 + SIMD t1, full titles" if is_full(cell)
                       else "G3 + SIMD t1, segments")
            groups[grp_key] += leg_s
            if is_full(cell):
                wins = windows(cell)
                n = sum(c for _, c in wins)
                g1 = n / min(est_fps(cell, "g1"), ffps)
                g2 = n / min(est_fps(cell, "g2"), ffps)
                pc = PC_ROUNDS * 2 * (g1 + g2) + n / ffps
                groups["sampled G1/G2 (pure C)"] += pc
                print(f"  purec: windows {', '.join(f'{s}+{c}' for s, c in wins)}"
                      f" ({n} frames, {100*n/frames:.0f}% of the title)")
                print(f"         G1 ours: <feed --start S --count N> | "
                      f"{cmd('ours', ocs, 1, True)}")
                print(f"         G1 x264: <feed --start S --count N> | "
                      f"{cmd('x264', f'{xc:.3f}', 1, True)}")
                print(f"         G2: the same at --threads 0 / auto")
                print(f"  est  : G1 {g1/60:5.1f} min/arm, G2 {g2/60:5.1f} min/arm, "
                      f"read {n/ffps/60:4.1f} min = {pc/3600:5.2f} h")
                if cell == VALIDATE_CELL:
                    v = frames / min(est_fps(cell, "g1"), ffps)
                    groups["validation cell"] += 2 * v + rd
                    print(f"  valid: pure C, one thread, whole title, both arms: "
                          f"{v/60:5.1f} min/arm = {(2*v+rd)/3600:5.2f} h")
            rows.append(cell)
            print()
    print("## Exclusive-window hours (estimates)\n")
    tot = 0.0
    for k, v in groups.items():
        print(f"  {k:<30} {v/3600:6.1f} h")
        tot += v
    print(f"  {'TOTAL':<30} {tot/3600:6.1f} h")
    if bound:
        print(f"\n  feeder-bound risk: {', '.join(bound)}")
    stem = [c for c in cells if c.startswith("stem2-full")]
    if not stem and "stem2-full" in FULL:
        print("\n  stem2-full: not a cell yet (the master is still downloading); "
              "its estimated legs are not in the totals.")


# ---------------------------------------------------------------------------
# The report.

def med(v):
    v = sorted(v)
    return v[len(v)//2] if len(v) % 2 else (v[len(v)//2-1] + v[len(v)//2]) / 2


def bd_of(cell):
    """BD-rate on VMAF-NEG, ours against x264, over the shared rate range;
    positive is worse. One window per curve: the longest with a full set."""
    r = results()
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
                     if k.startswith(f"ladder:{cell}:{arm}:") and v["frames"] == want)
        return [p[0] for p in pts], [p[1] for p in pts]
    rx, mx = curve("x264")
    rn, mn = curve("ours")
    if len(rx) < 4 or len(rn) < 4:
        return None
    try:
        return bd.bd_rate(rx, mx, rn, mn)
    except Exception:
        return None


def bar_line(ratios, dv, ds):
    if not ratios:
        return
    rs = sorted(ratios)
    print(f"\nBar (published): median {med(rs):.2f}x (<= 1.00), worst "
          f"{rs[-1]:.2f}x (<= 1.15), dVMAF-NEG worst {min(dv):+.2f} (>= -0.5), "
          f"|dsize| worst {max(abs(x) for x in ds):.2f}% (<= 1.0)")


def do_report(cells):
    r = results()
    live, anim = ordered(cells)
    print("\n## G3: SIMD, default threads, both encoders reading stdin\n")
    print("| cell | frames | ours crf | x264 crf | dsize | ours fps | x264 fps |"
          " **ours/x264** | read fps | ours cpu-s | x264 cpu-s | ours RSS |"
          " x264 RSS | dVMAF-NEG | dPSNR-Y | BD-rate | t1 ours/x264 |")
    print("|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|")
    ratios, dv, ds = [], [], []
    for grp in (live, anim):
        if grp is anim and live and anim:
            print("| *animation* |" + " |" * 16)
        for cell in grp:
            t = r.get(f"timed:{cell}:{PRESET}{TSUFFIX}")
            if not t:
                continue
            ow = med([x[0] for x in t["ours"]["default"]])
            xw = med([x[0] for x in t["x264"]["default"]])
            oc = med([x[1] for x in t["ours"]["default"]])
            xc = med([x[1] for x in t["x264"]["default"]])
            orss = max(x[2] for x in t["ours"]["default"]) / 2**30
            xrss = max(x[2] for x in t["x264"]["default"]) / 2**30
            o1 = med([x[0] for x in t["ours"]["t1"]])
            x1 = med([x[0] for x in t["x264"]["t1"]])
            bdr = bd_of(cell)
            rd = t["read"][0]
            flag = " (feeder-bound?)" if min(ow, xw) < 1.1 * rd else ""
            ratios.append(ow / xw); dv.append(t["ours_neg"] - t["x264_neg"])
            ds.append(t["dsize_pct"])
            print(f"| {cell}{flag} | {t['frames']} | {t['ours_crf']:.3f} | "
                  f"{t['x264_crf']:.3f} | {t['dsize_pct']:+.2f}% | "
                  f"{t['frames']/ow:.1f} | {t['frames']/xw:.1f} | **{ow/xw:.2f}x** | "
                  f"{t['frames']/rd:.0f} | {oc:.0f} | {xc:.0f} | {orss:.2f} GiB | "
                  f"{xrss:.2f} GiB | {t['ours_neg']-t['x264_neg']:+.2f} | "
                  f"{t['ours_psnr_y']-t['x264_psnr_y']:+.2f} | "
                  + (f"{bdr:+.2f}% |" if bdr is not None else "n/a |")
                  + f" {o1/x1:.2f}x |")
    bar_line(ratios, dv, ds)

    print("\n## G1 / G2: pure C, stratified sample (one 60 s window per 3 min)\n")
    print("| cell | windows | sampled frames | G1 ours/x264 | G2 ours/x264 |"
          " G1 est. full-title wall ours / x264 | read fps |")
    print("|---|--:|--:|--:|--:|--:|--:|")
    for grp in (live, anim):
        if grp is anim and live and anim:
            print("| *animation* |" + " |" * 6)
        for cell in grp:
            p = r.get(f"purec:{cell}:{PRESET}{TSUFFIX}")
            if not p:
                continue
            n = sum(c for _, c in p["windows"])

            def tot(g, arm):
                return sum(med([m[0] for m in ms]) for ms in p[g][arm])
            g1 = tot("G1", "ours") / tot("G1", "x264")
            g2 = tot("G2", "ours") / tot("G2", "x264")
            sc = p["frames"] / n
            rd = sum(x[0] for x in p["read"])
            print(f"| {cell} | {len(p['windows'])} | {n} | **{g1:.2f}x** | "
                  f"**{g2:.2f}x** | {tot('G1','ours')*sc/60:.0f} / "
                  f"{tot('G1','x264')*sc/60:.0f} min | {n/rd:.0f} |")
    v = r.get(f"validate:{VALIDATE_CELL}:{PRESET}{TSUFFIX}")
    p = r.get(f"purec:{VALIDATE_CELL}:{PRESET}{TSUFFIX}")
    if v:
        print(f"\nValidation, {VALIDATE_CELL}, pure C one thread, whole title: "
              f"ours {v['ours'][0]/60:.1f} min, x264 {v['x264'][0]/60:.1f} min, "
              f"ratio {v['ours'][0]/v['x264'][0]:.3f}x.")
        if p:
            n = sum(c for _, c in p["windows"])
            sc = p["frames"] / n
            eo = sum(med([m[0] for m in ms]) for ms in p["G1"]["ours"]) * sc
            ex = sum(med([m[0] for m in ms]) for ms in p["G1"]["x264"]) * sc
            print(f"Sampled estimate: ours {eo/60:.1f} min ({100*(eo/v['ours'][0]-1):+.1f}%),"
                  f" x264 {ex/60:.1f} min ({100*(ex/v['x264'][0]-1):+.1f}%), "
                  f"ratio {eo/ex:.3f}x (measured {v['ours'][0]/v['x264'][0]:.3f}x).")
    if any(c.startswith("stem2-full") for c in cells):
        print(f"\n{FULL['stem2-full']['notice']}")
    print()


def do_cells():
    for c in all_cells():
        info = cell_info(c)
        src = master_path(split(c)[0]) if is_full(c) else cell_path(c)
        print(f"  {c:<22}{info['w']}x{info['h']}  {fps_of(info):7.3f} fps  "
              f"{info['frames']:>6} frames  {info['frames']/fps_of(info)/60:5.1f} min"
              f"  {info['cls']:<9} {src}")
    for t, f in FULL.items():
        if not master_ready(t):
            p = os.path.join(MEDIA, f["file"])
            have = os.path.getsize(p) if os.path.exists(p) else 0
            print(f"  {t:<22}NOT READY: {have/1e9:.1f} of "
                  f"{f.get('complete_bytes', 0)/1e9:.1f} GB")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == "cells":
        return do_cells()
    if cmd == "stage":
        return do_stage()
    if cmd == "validate":
        load_guard()
        return do_validate()
    args = [a for a in sys.argv[2:] if not a.startswith("--")]
    args = args or ["all"]
    cells = []
    for a in args:
        cells += all_cells(a) if a in ("all", "full", "segments") else [a]
    have = all_cells()
    for c in cells:
        if c not in have:
            sys.exit(f"longform: '{c}' is not a cell (see `{sys.argv[0]} cells`)")
    if cmd == "plan":
        return do_plan(cells, "--full" in sys.argv)
    if cmd == "report":
        return do_report(cells)
    if cmd in ("timed", "purec"):
        load_guard()
    fn = {"ladder": do_ladder, "point": do_point, "rescore": do_rescore,
          "timed": do_timed, "purec": do_purec,
          "verify-feed": do_verify_feed}.get(cmd)
    if not fn:
        sys.exit(__doc__)
    for c in cells:
        fn(c)


main()
