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

FOUR KINDS OF CELL. A cell is title_size.

  - SEGMENTS: the Y4M files the sibling yah265 campaign staged on the internal
    disk (~/media/throughput), 90-120 s each; e.g. chimera_2160p. Fed by
    copying blocks from the file.
  - FULL TITLES: the masters on the external drive, 10+ minutes; e.g.
    tears-full_720p. A 12-minute 4K title is ~500 GB of Y4M, and the internal
    disk has ~57 GB free while the USB drive reads at ~82 MB/s (4K raw at
    ~8 fps), so a full title is never staged: it is decoded on the fly by
    VideoToolbox (bit-identical to the software decode) and scaled by
    scale_vt for 720p/1080p. The same pipeline is the VMAF reference.
  - EXCERPTS (tier L of /Volumes/T7/media/PARITY-SPEC.md): 600 s of a full
    title from the spec's offset, e.g. tears-x10_720p; start frame
    round(offset x fps), count round(600 x fps). Decoded like a full title,
    or read from a staged Y4M with --staged (below).
  - TIER S CLIPS (spec draft 2): the twelve shared short clips in
    /Volumes/T7/media/tier_s, encoded whole, e.g. s-foreman_cif. Fed by
    block copy, matched at x264's default rate factor like every other cell.

STAGED RAW FEED. At 720p a VideoToolbox feeder is slower than both encoders
(the 2026-09-25 720p G3 ratios are feeder-bound), so `stage-excerpt` writes
an excerpt once as Y4M to /Volumes/T7/staging/yah264/y4m (LF_STAGE_Y4M;
--internal-2160 or LF_STAGE_2160_INTERNAL=1 sends 2160p excerpts to
LF_STAGE_INTERNAL instead), verifies frame count and md5, and `--staged` on
any command feeds both encoders from that file by block copy. Every timed
command first times the feeder alone and refuses a FEEDER-BOUND cell (see
feed_guard).

RECORDS NAME THEIR BUILD (since 2026-09-26): every new point, ladder and
timed key ends in the sha256 of both binaries and the VMAF subsample, so a
rebuild cannot be served stale numbers. Older records are read as
"legacy-key" (see ident()).

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
    scripts/longform_throughput.py point  <cell>...     the matched-byte point (SOLVE_THREADS=4;
                                                        SOLVE_THREADS= point is the G3 pair)
    scripts/longform_throughput.py retag                once: re-key old points by thread count
    scripts/longform_throughput.py ladder <cell>...     five rungs, both arms
    scripts/longform_throughput.py rescore <cell>...    re-score, no re-solve
    scripts/longform_throughput.py timed  <cell>...     TIMED: G3 + SIMD t1
    scripts/longform_throughput.py purec  <cell>...     TIMED: sampled G1/G2
    scripts/longform_throughput.py validate             TIMED: the check cell
    scripts/longform_throughput.py stage                copy USB-bound masters in
    scripts/longform_throughput.py stage-excerpt <cell>...  excerpt -> Y4M, verified
    scripts/longform_throughput.py report [<cell>...]
    scripts/longform_throughput.py report-spec          the shared-spec page + CSV
    scripts/longform_throughput.py selftest-verdict     check the full-parity lines

Bare `all` means every cell that exists; `full` every full-title cell;
`segments` every staged segment; `excerpts` every tier L excerpt; `tier-s`
every tier S clip. `--staged` makes any command read excerpts from their
staged Y4M.

report-spec writes PARITY-REPORT-yah264-<date>.md and .csv to LF_REPORT_DIR
(default scratch/parity) in the layout of the spec's section 5, from the
records alone: nothing is run. Tier S comes from the s-<clip> cells when
they have records, else from the legacy board logs (LF_BOARD_LOGS), marked
"legacy board (rate-pinned)".

LF_TEST_FRAMES=N cuts every cell to N frames for a dry run of the machinery;
it refuses to run unless Y264_CACHE points at a scratch cache.

Everything except timed/purec/validate is untimed, reads no clock for a
result, and runs under the box share (SOLVE_THREADS 4 for an encoder plus a
one-thread feeder; VMAF_THREADS 3 + FF_THREADS 2 + a feeder when scoring).
The timed commands refuse to start on a loaded box (LOAD_MAX).
"""
import os, sys, json, subprocess, re, shutil, math, tempfile, time, hashlib
import csv

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
# And any stride other than 1 must be COPRIME with the mini-GOP period
# (bframes + 1; 4 at medium's --bframes 3), or it samples a fixed subset of
# GOP slots: 2 and 4 are refused at bframes 3, 3 and 5 are allowed.
BFRAMES = int(os.environ.get("LF_BFRAMES", "3"))
if not SUBSAMPLE.isdigit() or int(SUBSAMPLE) < 1 or \
        math.gcd(int(SUBSAMPLE), BFRAMES + 1) != 1:
    sys.exit(f"longform: SUBSAMPLE={SUBSAMPLE} shares a factor with the mini-GOP "
             f"period {BFRAMES + 1} (bframes {BFRAMES}); it would score the same "
             "GOP slots every time. Use 1, or a stride coprime with it.")
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
SIZE_RUNGS = {"cif":   [22.0, 25.0, 28.0, 31.0, 34.0],
              "720p":  [22.0, 25.0, 28.0, 31.0, 34.0],
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
WINDOW_SECS  = float(os.environ.get("WINDOW_SECS", "30"))
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
    # The ProRes 422 HQ master (68.6 GB, 10-bit 4:2:2) reads at ~29 fps over
    # USB and does not fit the internal disk, so the cell reads a one-time
    # hardware HEVC copy on the internal disk (2026-09-25): 8-bit 4:2:0, the
    # form both encoders consume, VideoToolbox q90, 138 Mbit/s, 51.6 dB
    # PSNR-Y against the 8-bit source on a 240-frame probe. Both encoders and
    # the VMAF reference read the same frames.
    "stem2-full":    dict(file=os.path.join(MASTERS, "stem2_uhd_hevc_q90.mp4"),
                          label="ASC StEM2 UHD 24p", cls="live",
                          native=(3840, 2160), fps=(24, 1), t0=0.0, frames=25114,
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
    # Added 2026-09-26 (scene scan: the most representative 10-min window starts
    # at 50 s, 2% class shift). Live action, 60p, film grain added in post;
    # ProRes 422 10-bit on the T7, decoded to 8-bit 4:2:0 by the feeder.
    "nocturne-full": dict(file="/Volumes/T7/media/longform/nocturne_filmgrain/nocturne_60fps_uhd_filmgrain_sdr.mov",
                          label="Nocturne (film grain) UHD 60p", cls="live",
                          native=(3840, 2160), fps=(60, 1), t0=0.0,
                          frames=39870),
    # Added 2026-09-26 as a tier C grain cell, not tier L: a 1924 LoC film
    # scan (real grain), 1440x1080 4:3, so 720p and 1080p only.
    "lash1924-full": dict(file="/Volumes/T7/media/longform/lash_of_the_whip_1924/02049825.mov",
                          label="Lash of the Whip (1924) film scan", cls="live",
                          native=(1440, 1080), fps=(24, 1), t0=0.0,
                          frames=65729, sizes=("720p", "1080p")),
}
SIZES = ("720p", "1080p", "2160p")
SEG_GEOM = {"720p": (1280, 720), "1080p": (1920, 1080), "2160p": (3840, 2160)}
SIZE_W = {"720p": 1280, "1080p": 1920}
# The USB drive's sustained read, MB/s. A master read faster than this caps
# the feeder: Tears of Steel is 382 kB a frame, so ~300 fps from USB, below
# what both encoders do at 720p. `stage` copies such masters in. 114 is a
# clean 4 GB sequential read on 2026-09-25 with nothing else on the drive;
# the drive (a 2 TB Seagate Portable, a 5400 rpm 2.5" disk rated ~120 MB/s)
# is the limit, not the link: it sits on a 5 Gb/s USB 3 link. The earlier 82
# was read while a 68 GB download was writing to the same disk.
USB_MBPS = float(os.environ.get("USB_MBPS", "114"))
# The T7 SSD's sustained read, MB/s, for the dry run's feeder model only; a
# timed leg measures its own feeder first (feed_guard). Provisional until
# the coordinator's 4K staged read test lands.
T7_MBPS  = float(os.environ.get("T7_MBPS", "900"))

# TIER L (PARITY-SPEC section 2): 10-minute excerpts of the full titles,
# <title>-x10_<size>. 600 s from the offset, start frame round(offset x fps),
# count round(600 x fps), the offset past the opening card. The whole-title
# cells stay beside them.
EXCERPT_SECS = 600
EXCERPTS = {"tears-x10":    ("tears-full", 60),
            "meridian-x10": ("meridian-full", 30),
            "stem2-x10":    ("stem2-full", 235),    # representative window (scene scan, 2026-09-26)
            "sintel-x10":   ("sintel-full", 195),   # representative window (scene scan, 2026-09-26)
            "bbb-x10":      ("bbb-full", 30),       # runs into ~35 s of credits
            "cosmos-x10":   ("cosmos-full", 30),
            "nocturne-x10": ("nocturne-full", 50),   # representative window (scene scan)
            "lash1924-x10": ("lash1924-full", 40)}   # tier C grain; representative window

# END CREDITS ARE NOT PART OF A CELL (PARITY-SPEC section 2, 2026-10-05): VMAF-NEG
# saturates on text over a flat background, and a wall ratio taken across the
# credits times frames no viewer weighs. CREDITS_LAST is the last story frame of
# an excerpt, excerpt-relative and the same at every size (luma scan and contact
# sheets, 2026-10-06 and 2026-10-08). Each listed excerpt has a cropped cell,
# <title>-x10c_<size>: the same start, ending at that frame. It is a cell of
# its own, so no record of the whole 600 s is ever read as a cropped one; the
# whole-excerpt cells stay, and the spec report marks a number taken from one
# "credits included". A cropped cell reads the first frames of the whole
# excerpt's staged Y4M.
CREDITS_LAST = {"tears-x10": 12671, "bbb-x10": 13928, "sintel-x10": 13177,
                "cosmos-x10": 13723, "stem2-x10": 12939, "meridian-x10": 32886,
                "nocturne-x10": 34319}
EXCERPT_FRAMES = {}
for _t, _last in CREDITS_LAST.items():
    EXCERPTS[_t + "c"] = EXCERPTS[_t]
    EXCERPT_FRAMES[_t + "c"] = _last + 1


def uncropped(cell):
    """The whole-excerpt cell a cropped cell is cut from, else None."""
    title, size = cell.rsplit("_", 1)
    return f"{title[:-1]}_{size}" if title in EXCERPT_FRAMES else None

# TIER S (PARITY-SPEC draft 2): the twelve shared short clips, encoded WHOLE
# and matched at the reference's default rate factor like every other cell,
# s-<clip>, fed by block copy from the library's Y4M. Not the board's pinned
# bitrates: that board goes on as an internal progress line only.
TIER_S_DIR = os.environ.get("LF_TIER_S", "/Volumes/T7/media/tier_s")
TIER_S = {"s-foreman_cif":     ("foreman_cif.y4m", "live"),
          "s-bus_cif":         ("bus_cif.y4m", "live"),
          "s-stefan_cif":      ("stefan_cif.y4m", "live"),
          "s-ducks_720p":      ("ducks_720p.y4m", "live"),
          "s-park_joy_720p":   ("park_joy_720p.y4m", "live"),
          "s-fourpeople_720p": ("fourpeople_720p.y4m", "live"),
          "s-shields_720p":    ("shields_720p.y4m", "live"),
          "s-sunflower_1080p": ("sunflower_1080p.y4m", "live"),
          "s-pedestrian_1080p": ("pedestrian_1080p.y4m", "live"),
          "s-riverbed_1080p":  ("riverbed_1080p.y4m", "live"),
          "s-meridian_2160p":  ("src_meridian_modern_600.y4m", "live"),
          "s-bbb_2160p":       ("src_bbb4k_300.y4m", "animation")}

# TIER C (PARITY-SPEC section 2, content types): the sports and screen clips, c-<clip>,
# encoded whole and matched like a tier S clip. They are short (130 frames at 30 fps; the
# boxing clip 4.2 s at 60 fps), so an all-cores ratio on them is a thin reading. The two
# 10-bit sources (Skater227, BoxingPractice) are read from 8-bit 4:2:0 conversions
# (ffmpeg -pix_fmt yuv420p), and every report says so. BoxingPractice is 4096x2160 and
# sits in the 2160p class. Lash of the Whip, the grain cell, is an excerpt (EXCERPTS).
TIER_C_DIR = os.environ.get("LF_TIER_C", os.path.expanduser("~/media/tier_c_int"))
TIER_C = {"c-worldcup_1080p":     ("WorldCup_1920x1080_30p.y4m", "live"),
          "c-worldcup_far_1080p": ("WorldCup_far_1920x1080_30p.y4m", "live"),
          "c-skater227_1080p":    ("Skater227_1920x1080_30fps_8bit.y4m", "live"),
          "c-boxing_2160p":       ("BoxingPractice_4096x2160_60fps_8bit.y4m", "live"),
          "c-wikipedia_1080p":    ("Wikipedia_1920x1080p30.y4m", "screen"),
          "c-debugging_1080p":    ("Debugging_1920x1080_30fps_8bit_420.y4m", "screen"),
          "c-spreadsheet_1080p":  ("Spreadsheet_1920x1080_30fps_8bit_420_130f.y4m", "screen")}
TIER_C_10BIT = ("c-skater227_1080p", "c-boxing_2160p")
CLIPS = {**{c: (os.path.join(TIER_S_DIR, f), k) for c, (f, k) in TIER_S.items()},
         **{c: (os.path.join(TIER_C_DIR, f), k) for c, (f, k) in TIER_C.items()}}

# STAGED RAW FEED. A master decoded on the fly by VideoToolbox tops out near
# 435 fps at 4K-source (three decoders), below both encoders at 720p, so the
# 720p G3 ratios of 2026-09-25 were feeder-bound. `stage-excerpt` writes an
# excerpt once as 8-bit 4:2:0 Y4M and `--staged` makes every command read
# that file by block copy instead. 2160p excerpts are 150-450 GB each; the
# switch below sends them to an internal path instead of the T7.
STAGE_Y4M = os.environ.get("LF_STAGE_Y4M", "/Volumes/T7/staging/yah264/y4m")
STAGE_INTERNAL = os.environ.get("LF_STAGE_INTERNAL",
                                os.path.expanduser("~/media/staging-y4m"))
STAGE_2160_INTERNAL = (os.environ.get("LF_STAGE_2160_INTERNAL") == "1"
                       or "--internal-2160" in sys.argv)
STAGE_FLOOR_GB = float(os.environ.get("STAGE_FLOOR_GB", "40"))
STAGE_WORKERS = int(os.environ.get("STAGE_WORKERS", "1"))
STAGED = "--staged" in sys.argv
# TEST MODE: every cell is cut to its first LF_TEST_FRAMES frames. For dry
# runs of the machinery only; refused unless Y264_CACHE points away from the
# real results, and staged files get a .test<N> suffix.
TEST_FRAMES = int(os.environ.get("LF_TEST_FRAMES", "0"))
if TEST_FRAMES and not os.environ.get("Y264_CACHE"):
    sys.exit("longform: LF_TEST_FRAMES needs Y264_CACHE set to a scratch cache, "
             "so a test record can never land beside the real results")


def split(cell):
    title, size = cell.rsplit("_", 1)
    return title, size


def kind(cell):
    """s (tier S clip), seg (staged segment), full (whole title) or x10."""
    if cell in CLIPS:
        return "s"
    title = split(cell)[0]
    if title in FULL:
        return "full"
    if title in EXCERPTS:
        return "x10"
    return "seg"


def is_full(cell):
    """A cell cut from a compressed master (a whole title or an excerpt)."""
    return kind(cell) in ("full", "x10")


def base_title(title):
    return EXCERPTS[title][0] if title in EXCERPTS else title


def excerpt_span(cell):
    """(start frame in the master, frame count) of an excerpt cell."""
    title, _ = split(cell)
    base, off = EXCERPTS[title]
    f = probe_master(base) if master_ready(base) else FULL[base]
    fps = f["fps"][0] / f["fps"][1]
    n = int(round(EXCERPT_SECS * fps))
    return int(round(off * fps)), min(n, EXCERPT_FRAMES.get(title, n))


def stage_path(cell):
    d = STAGE_INTERNAL if (split(cell)[1] == "2160p" and STAGE_2160_INTERNAL) \
        else STAGE_Y4M
    return os.path.join(d, f"{cell}.test{TEST_FRAMES}.y4m" if TEST_FRAMES
                        else f"{cell}.y4m")


def staged_file(cell):
    """The verified staged Y4M of a cell, or None. The record names the file;
    it counts only while the file is there at the recorded size."""
    src = uncropped(cell) or cell
    want = cell_info(src)["frames"]
    best = None
    for k, v in results().items():
        if not k.startswith(f"stage:{src}@") or not v.get("ok"):
            continue
        p = v["path"]
        try:
            if os.path.getsize(p) != v["bytes"] or v["frames"] != want:
                continue
        except OSError:
            continue
        if best is None or p == stage_path(src):
            best = p
    return best


def staged_now(cell):
    """The staged file this run reads for the cell (with --staged), else None."""
    if not (STAGED and is_full(cell)):
        return None
    p = staged_file(cell)
    if not p:
        sys.exit(f"longform: --staged, but {cell} has no verified staged Y4M -- "
                 f"run `{sys.argv[0]} stage-excerpt {cell}` first")
    return p


def feed_tag(cell):
    return ":staged" if staged_now(cell) else ""


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
    """The Y4M a block-copy cell reads (a segment or a tier S clip)."""
    if cell in CLIPS:
        return CLIPS[cell][0]
    title, size = split(cell)
    return os.path.join(STAGE, SEGMENTS[title][2].format(size=size))


def cell_info(cell):
    """frames, (fps num, den), w, h, class, label -- for every kind."""
    title, size = split(cell)
    k = kind(cell)
    if k in ("full", "x10"):
        base = base_title(title)
        f = probe_master(base) if master_ready(base) else FULL[base]
        w, h = full_geom(base, size)
        frames = f["frames"] or f.get("est_frames")
        label = f["label"]
        if k == "x10":
            st, frames = excerpt_span(cell)
            label += f", 10-minute excerpt from frame {st}"
        d = dict(frames=frames, fps=f["fps"] or (24, 1), w=w, h=h,
                 cls=f["cls"], label=label, full=True)
    else:
        frames, fps, w, h, num, den = y4m_shape(cell_path(cell))
        cls, label = ((CLIPS[cell][1], os.path.basename(CLIPS[cell][0])) if k == "s"
                      else (SEGMENTS[title][1], SEGMENTS[title][0]))
        d = dict(frames=frames, fps=(num, den), w=w, h=h, cls=cls, label=label,
                 full=False)
    if TEST_FRAMES:
        d["frames"] = min(d["frames"], TEST_FRAMES)
    return d


def fps_of(info):
    return info["fps"][0] / info["fps"][1]


def all_cells(which="all"):
    out = []
    if which in ("all", "full"):
        for t, f in FULL.items():
            if master_ready(t):
                out += [f"{t}_{s}" for s in f.get("sizes", SIZES)]
    if which in ("all", "excerpts"):
        for t, (base, _) in EXCERPTS.items():
            if not master_ready(base):
                continue
            for s in FULL[base].get("sizes", SIZES):
                st, n = excerpt_span(f"{t}_{s}")
                if st + n <= probe_master(base)["frames"]:
                    out.append(f"{t}_{s}")
    if which in ("all", "segments"):
        out += [f"{t}_{s}" for t in SEGMENTS for s in SIZES
                if os.path.exists(cell_path(f"{t}_{s}"))]
    if which in ("all", "tier-s"):
        out += [c for c in TIER_S if os.path.exists(cell_path(c))]
    if which in ("all", "tier-c"):
        out += [c for c in TIER_C if os.path.exists(cell_path(c))]
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


def feeder(cell, start=0, count=None, workers=None, source="auto"):
    """argv of the process that writes the cell (or frames start..start+count
    of it) as Y4M on stdout. source "vt" forces the master decode even under
    --staged (stage-excerpt uses it)."""
    info = cell_info(cell)
    count = count or info["frames"] - start
    if not is_full(cell):
        return [sys.executable, FEED, "y4m", "--src", cell_path(cell),
                "--start", str(start), "--count", str(count)]
    sp = staged_now(cell) if source == "auto" else None
    if sp:
        return [sys.executable, FEED, "y4m", "--src", sp,
                "--start", str(start), "--count", str(count)]
    title, size = split(cell)
    base = base_title(title)
    f = FULL[base]
    nw, nh = f["native"]
    off = excerpt_span(cell)[0] if kind(cell) == "x10" else 0
    if workers is None:
        workers = FEED_WORKERS_UNTIMED
    return [sys.executable, FEED, "vt", "--src", master_path(base),
            "--native", f"{nw}x{nh}", "--size", f"{info['w']}x{info['h']}",
            "--fps", f"{info['fps'][0]}/{info['fps'][1]}",
            "--t0", f"{f['t0'] or 0:.6f}", "--start", str(off + start),
            "--count", str(count), "--workers", str(workers),
            "--threads", FEED_THREADS]


def master_fed(cell):
    """Decoded from a compressed master in this run (not staged, not Y4M)."""
    return is_full(cell) and not staged_now(cell)


def timed_workers(cell):
    if not master_fed(cell):
        return 1
    title, size = split(cell)
    base = base_title(title)
    # At native size one decoder used to be enough. Two cropped 2160p cells are not:
    # the 4096-wide Sintel master decodes at ~155 fps and the StEM2 HEVC copy at
    # ~92-124, under 1.5x our rate. FEED_WORKERS_NATIVE=N feeds a native-size cell
    # from N chunked decoders; the leg then needs the cell's verify-feed record,
    # taken at the same N (verify-feed checks max(2, N) decoders).
    return int(os.environ.get("FEED_WORKERS_NATIVE", "1")) \
        if full_geom(base, size) == FULL[base]["native"] else FEED_WORKERS_TIMED


def results():
    """Every record. A missing file is an empty store; an UNREADABLE one is a
    hard stop. Reading it as {} (as this did until 2026-10-05) let the next
    put() write a one-record file over it: two concurrent solves interleaved
    their writes, the next reader got a parse error, and 880 KB of records
    became 5 KB."""
    try:
        f = open(RESULTS)
    except FileNotFoundError:
        return {}
    with f:
        try:
            return json.load(f)
        except ValueError as err:
            sys.exit(f"longform: {RESULTS} does not parse ({err}); refusing to "
                     "read or write records until it is restored")


def put(key, val):
    """Read, add, write, under an exclusive lock on a sibling lock file, so
    any number of processes may record at once. The write goes to a temporary
    file unique to this process and is renamed over the store, so a reader
    never sees a half-written file."""
    import fcntl
    with open(RESULTS + ".lock", "a") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        r = results(); r[key] = val
        fd, tmp = tempfile.mkstemp(prefix="results.", suffix=".new",
                                   dir=os.path.dirname(RESULTS))
        with os.fdopen(fd, "w") as f:
            json.dump(r, f, indent=1, sort_keys=True)
            f.flush(); os.fsync(f.fileno())
        os.replace(tmp, RESULTS)
    return val


# ---------------------------------------------------------------------------
# WHICH BUILD MADE A RECORD. Until 2026-09-26 a point, ladder or timed record
# was keyed by cell, arm, rate factor and preset only, so after a rebuild of
# either encoder the cache silently served the old build's numbers (and a
# record scored at another VMAF subsample looked the same as one scored every
# frame). Every record written since carries, at the end of its key,
#   :b<ours>:r<reference[+reference]>:s<subsample>
# where <ours> and <reference> are the first 12 hex of a sha256 over the
# binary and every non-system library it links. A key written by another
# build is unreachable, not merely old. Records from before the change are
# still read, and every consumer says "legacy-key" when it uses one: the
# computing commands only with LF_LEGACY=1, the reports always.

_BIN_IDS = {}
LEGACY_OK = os.environ.get("LF_LEGACY") == "1"
IDENT_RE = re.compile(r":b[0-9a-f]{12}:r[0-9a-f+]+:s\d+$")


def linked_libs(path):
    """The non-system dylibs a binary links, resolved where they exist. A
    CLI that is a thin shell over a shared library changes with the library,
    so the identity has to cover it."""
    try:
        out = subprocess.run(["otool", "-L", path], capture_output=True,
                             text=True).stdout
    except OSError:
        return []
    libs = []
    for line in out.splitlines()[1:]:
        p = line.strip().split(" (")[0]
        if p.startswith(("/usr/lib/", "/System/")):
            continue
        if p.startswith("@"):
            p = os.path.join(os.path.dirname(os.path.realpath(path)),
                             p.split("/", 1)[1])
        if os.path.exists(p):
            libs.append(p)
    return libs


def bin_id(path):
    try:
        st = os.stat(path)
    except OSError:
        return "0" * 12
    k = (path, st.st_size, st.st_mtime_ns)
    if k not in _BIN_IDS:
        h = hashlib.sha256()
        for p in [path] + linked_libs(path):
            with open(p, "rb") as f:
                for b in iter(lambda: f.read(1 << 20), b""):
                    h.update(b)
        _BIN_IDS[k] = h.hexdigest()[:12]
    return _BIN_IDS[k]


def ident(*refs):
    """The key suffix naming this build of ours, the reference binaries the
    record used (the SIMD CLI unless given), and the VMAF subsample."""
    refs = refs or (X264,)
    return (f":b{bin_id(YAH264)}:r{'+'.join(bin_id(r) for r in refs)}"
            f":s{SUBSAMPLE}")


def ids(*refs):
    """The same identity, spelled out inside the record."""
    refs = refs or (X264,)
    return {"ours_sha": bin_id(YAH264),
            "ref_sha": "+".join(bin_id(r) for r in refs), "subsample": SUBSAMPLE}


def rec(base, *refs, legacy=None):
    """(record, tag, key) for this build: tag "new" for a key carrying the
    current identity, "legacy-key" for the pre-identity key (only when
    `legacy`, default LF_LEGACY=1), else (None, None, None)."""
    r = results()
    k = base + ident(*refs)
    if k in r:
        return r[k], "new", k
    if (LEGACY_OK if legacy is None else legacy) and base in r:
        return r[base], "legacy-key", base
    return None, None, None


def rec_any(base, *refs):
    """For the reports: the best record under `base` from ANY build. The
    current identity first, then the newest record of another build, then
    the legacy key. Tag: "new", "other-build" or "legacy-key"."""
    r = results()
    k = base + ident(*refs)
    if k in r:
        return r[k], "new", k
    other = sorted((v.get("date", ""), kk) for kk, v in r.items()
                   if kk.startswith(base + ":b") and IDENT_RE.search(kk))
    if other:
        kk = other[-1][1]
        return r[kk], "other-build", kk
    if base in r:
        return r[base], "legacy-key", base
    return None, None, None


def strip_ident(key):
    return IDENT_RE.sub("", key)


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


def pipe_run(feed, enc, env, timed=False, prefix=()):
    """feeder | encoder. Returns a dict; with timed=True the encoder's own
    wall, CPU and peak RSS from /usr/bin/time, plus the feeder's CPU and the
    whole pipeline's wall. A feeder failure is fatal even when the encoder
    succeeded -- an encoder fed a truncated stream finishes early and
    reports a flattering wall."""
    ferr = tempfile.TemporaryFile()
    t0 = time.monotonic()
    fp = subprocess.Popen(feed, stdout=subprocess.PIPE, stderr=ferr)
    ep = subprocess.Popen(list(prefix) + (["/usr/bin/time", "-l"] if timed else []) + enc,
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
    # LF_RSS_LOG=<file>: every untimed 2160p encode runs under /usr/bin/time -l and
    # appends its peak resident size there (the box has 64 GB; two default-thread
    # 2160p encodes of ours at once ran it out of memory on 2026-10-08).
    rss_log = os.environ.get("LF_RSS_LOG") if split(cell)[1] == "2160p" else None
    # LF_4K_LOCK=<file>: every untimed 2160p ENCODE (not the scoring that follows)
    # takes this box-wide lock and waits for kern.memorystatus_level >= 40, so one
    # default-thread 2160p encode of ours runs at a time while a job's hours of
    # scoring hold nothing. LF_4K_LOCK_SIDE, when set, is the lock a one-thread
    # 2160p encode takes instead (one small job beside the big one).
    prefix = ()
    lock = os.environ.get("LF_4K_LOCK") if split(cell)[1] == "2160p" else None
    if lock:
        gate = ("/bin/bash", "-c",
                'while [ "$(sysctl -n kern.memorystatus_level)" -lt 40 ]; do sleep 30; done; '
                'exec "$@"', "gate")
        if str(threads) == "1" and os.environ.get("LF_4K_LOCK_SIDE"):
            lock = os.environ["LF_4K_LOCK_SIDE"]
        # LF_4K_LOCK_SIDE=none: one-thread 2160p encodes take no lock (the queue's
        # width bounds how many run at once); the memory check still applies
        prefix = gate if lock == "none" else ("/usr/bin/lockf", lock) + gate
    r = pipe_run(feeder(cell, start, count), enc_cmd(arm, crf, out, threads, purec),
                 arm_env(arm, cell, purec), timed=bool(rss_log), prefix=prefix)
    if rss_log:
        with open(rss_log, "a") as f:
            f.write(f"{time.strftime('%m-%d %H:%M:%S')} [rss] {cell} {arm} "
                    f"threads {threads or 'default'} crf {crf:.3f}: max RSS "
                    f"{r['rss']/2**30:.2f} GiB, {count or cell_info(cell)['frames'] - start} frames\n")
    if not os.path.exists(out) or os.path.getsize(out) < 1024:
        sys.exit(f"longform: {arm} wrote no bitstream for {cell} crf {crf}")
    return os.path.getsize(out)


def _fifo(wd, name):
    p = os.path.join(wd, name)
    if os.path.exists(p):
        os.remove(p)
    os.mkfifo(p)
    return p


# SUPPLEMENTARY QUALITY (PARITY-SPEC 5.2a, reported, not part of the bar):
# harmonic-mean, minimum-frame, 1%-low and first-second VMAF-NEG from the
# per-frame scores the VMAF pass already writes; YUV-PSNR 6:1:1 from the
# psnr_cb/psnr_cr it also writes; SSIM 6:1:1 from a second, cheap pass
# (ffmpeg's ssim filter, one more decode and feed, no encode); plain VMAF
# v0.6.1 as a second model in the same libvmaf pass. The second model is
# NOT cheap: it has its own adm/vif extractors (NEG's are the enhancement-
# gain-limited variants), +80% libvmaf CPU on a 24-frame 1080p probe, so it
# is opt-in (LF_PLAIN_VMAF=1). LF_SSIM=0 skips the SSIM pass.
PLAIN_VMAF = os.environ.get("LF_PLAIN_VMAF", "0") == "1"
SSIM_PASS = os.environ.get("LF_SSIM", "1") == "1"


def w611(y, u, v):
    return (6 * y + u + v) / 8


def ssim_611(cell, bitstream, wd, start, count):
    """(SSIM 6:1:1, frames the ssim filter saw) against the same feeder.
    shortest=1 matters: by default the filter repeats the last frame of the
    shorter input, so a half-length decode still counted every frame."""
    stats = os.path.join(wd, "ssim.log")
    ferr = tempfile.TemporaryFile()
    fp = subprocess.Popen(feeder(cell, start, count), stdout=subprocess.PIPE,
                          stderr=ferr)
    r = subprocess.run(
        [FFMPEG, "-v", "error", "-nostdin", "-threads", FF_THREADS,
         "-filter_threads", "1", "-i", bitstream, "-f", "yuv4mpegpipe",
         "-i", "pipe:0", "-lavfi",
         f"[0:v]format=yuv420p[d];[d][1:v]ssim=stats_file={stats}:shortest=1",
         "-f", "null", "-"], stdin=fp.stdout, stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE)
    fp.stdout.close()
    frc = fp.wait()
    if r.returncode != 0 or frc != 0:
        ferr.seek(0)
        sys.exit(f"longform: the SSIM pass failed on {bitstream}\n  "
                 f"{r.stderr.decode(errors='replace')[-400:]}\n  "
                 f"{ferr.read().decode(errors='replace')[-300:]}")
    vals = []
    with open(stats) as f:
        for ln in f:
            m = re.search(r"Y:([\d.]+) U:([\d.]+) V:([\d.]+)", ln)
            if m:
                vals.append(w611(*(float(x) for x in m.groups())))
    os.remove(stats)
    return (sum(vals) / len(vals) if vals else None), len(vals)


def score(cell, bitstream, wd, start=0, count=None):
    """(VMAF-NEG, PSNR-Y, extra) of a bitstream against the frames it was
    made from; `extra` holds the 5.2a columns and the frame-count check.

    Nothing lands on disk. The reference is the SAME feeder that fed the
    encoder, into libvmaf's stdin; the bitstream is decoded by ffmpeg into a
    named pipe. libvmaf aligns frame for frame, so the scored count is checked
    against the cell's: a short pass is a pooled mean over the wrong frames.
    Since 2026-09-26 a short count is RECORDED (frames_ok false, and the
    report marks the cell frame-mismatch) instead of aborting the run, and
    the SSIM pass counts the decoded frames a second, independent way."""
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
    models = ["--model", "version=vmaf_v0.6.1neg:name=neg"]
    if PLAIN_VMAF:
        models += ["--model", "version=vmaf_v0.6.1:name=vmaf"]
    v = subprocess.run(
        [VMAF, "--threads", VMAF_THREADS, "--subsample", SUBSAMPLE,
         "-r", "/dev/stdin", "-d", dfifo, "--json", "-o", j, "--feature", "psnr"]
        + models,
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
    pooled = jj["pooled_metrics"]
    frames = jj["frames"]
    neg = [fr["metrics"]["neg"] for fr in frames]
    low = sorted(neg)[:max(1, math.ceil(0.01 * len(neg)))]
    fps = fps_of(info)
    first = [fr["metrics"]["neg"] for fr in frames if fr["frameNum"] < fps]
    extra = {
        "neg_hmean": pooled["neg"].get("harmonic_mean"),
        "neg_min": pooled["neg"].get("min"),
        "neg_low1": sum(low) / len(low) if low else None,
        "neg_first1s": sum(first) / len(first) if first else None,
        "vmaf": pooled["vmaf"]["mean"] if "vmaf" in pooled else None,
        "psnr_yuv611": w611(pooled["psnr_y"]["mean"], pooled["psnr_cb"]["mean"],
                            pooled["psnr_cr"]["mean"])
        if "psnr_cb" in pooled else None,
        "frames_scored": len(frames), "frames_want": want}
    ok = len(frames) == want
    if SSIM_PASS:
        extra["ssim_yuv611"], extra["frames_decoded"] = \
            ssim_611(cell, bitstream, wd, start, count)
        ok = ok and extra["frames_decoded"] == count
    extra["frames_ok"] = ok
    if not ok:
        print(f"  [FRAME-MISMATCH] {cell}: vmaf scored {len(frames)} (want "
              f"{want}), decoded {extra.get('frames_decoded', '?')} (want "
              f"{count}); recorded as frames_ok false", flush=True)
    return pooled["neg"]["mean"], pooled["psnr_y"]["mean"], extra


# ---------------------------------------------------------------------------
# Untimed.

# THE SOLVE'S THREAD COUNT IS PART OF THE POINT KEY (since 2026-10-05). The key
# used to say ":tdefault" whatever SOLVE_THREADS was, and SOLVE_THREADS defaults
# to 4, so every point was a 4-thread solve under a "default threads" name. The
# reference's bytes move with its thread count, so a G3 leg (default threads)
# timed against a 4-thread point missed the size bar by 2-6% on the 10-minute
# excerpts. Now: ":t4" for a SOLVE_THREADS=4 solve, ":tdefault" only for a real
# default-thread one (SOLVE_THREADS= ). G3 reads the default-thread pair; the
# pure-C legs and the one-thread legs keep the 4-thread pair. `retag` renames
# the records written before this into their real thread count, once.
def ttag(threads=None):
    t = SOLVE_THREADS if threads is None else threads
    return f"t{t}" if t else "tdefault"


def xkey_base(cell, tag=None):
    return f"point:{cell}:x264:{X264_CRF:.1f}:{PRESET}:{tag or ttag()}"


def nkey_base(cell, target, tag=None):
    return f"point:{cell}:ours:onto{target}:{PRESET}:{tag or ttag()}"


def do_retag():
    """Rename the points keyed ":tdefault" that a SOLVE_THREADS=N solve wrote
    into ":tN" (their solve_threads field says which), under the store lock."""
    import fcntl
    with open(RESULTS + ".lock", "a") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        r = results()
        moves = []
        for k, v in list(r.items()):
            if not k.startswith("point:") or ":tdefault" not in k:
                continue
            t = str(v.get("solve_threads") or "")
            if t and t.isdigit():
                moves.append((k, k.replace(":tdefault", f":t{t}", 1)))
        for a, b in moves:
            r[b] = r.pop(a)
        fd, tmp = tempfile.mkstemp(prefix="results.", suffix=".new",
                                   dir=os.path.dirname(RESULTS))
        with os.fdopen(fd, "w") as f:
            json.dump(r, f, indent=1, sort_keys=True)
            f.flush(); os.fsync(f.fileno())
        os.replace(tmp, RESULTS)
    for a, b in moves:
        print(f"  {a}\n    -> {b}")
    print(f"retag: {len(moves)} point record(s) renamed")


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
                       + (f":f{lf}" if lim else "")) + ident()
                if key in results():
                    print(f"  {cell:<20}{arm:<6}crf {crf:>5.1f}  cached", flush=True)
                    continue
                out = os.path.join(wd, "l.264")
                sz = encode(arm, cell, crf, out, solve_threads(arm), 0, lim)
                neg, psnr, qx = score(cell, out, wd, 0, lim)
                put(key, {"bytes": sz, "neg": neg, "psnr_y": psnr, **qx,
                          "kbps": sz * 8 / (lf / fps) / 1000, "frames": lf,
                          "fps": fps, "threads": solve_threads(arm),
                          "date": time.strftime("%Y-%m-%d %H:%M"), **ids()})
                print(f"  {cell:<20}{arm:<6}crf {crf:>5.1f}  "
                      f"{sz*8/(lf/fps)/1000:>9.0f} kbit/s  NEG {neg:6.2f}  "
                      f"PSNR-Y {psnr:6.2f}   ({lf} frames)", flush=True)
    finally:
        shutil.rmtree(wd, ignore_errors=True)


def _secant(arm, cell, target, crf, slope, wd, tag, first=None):
    """Solve `arm`'s rate factor onto `target` bytes.
    ([(crf, bytes)], best, best file, slope). `first` is an encode already made
    at `crf` -- (bytes, path) -- which then counts as the first step."""
    best, pts = None, []
    work = os.path.join(wd, f"{tag}.264")
    keep = os.path.join(wd, f"{tag}.best.264")
    for _ in range(8):
        crf = round(max(8.0, min(45.0, crf)), 3)
        if any(abs(crf - c) < 1e-6 for c, _ in pts):
            break                          # asked the same question twice
        t = time.monotonic()
        if first and not pts:
            sz = first[0]
            shutil.copyfile(first[1], work)
        else:
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
        # Inside a step of the staircase: the nearest rate factors above and
        # below the target are closer than the encoder resolves, so another
        # encode returns one of the two sizes already seen (tears-full_720p
        # spent three encodes learning that 23.543 and 23.554 are one step).
        hi = [c for c, b in pts if b > target]
        lo = [c for c, b in pts if b < target]
        if hi and lo and min(lo) - max(hi) < 0.015:
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
        # Once the target is bracketed, never step outside the bracket: a
        # slope learned on one tread of the staircase overshoots it, and the
        # solve then dithers between the same two sizes until it runs out of
        # steps (foreman_cif spent eight encodes on 553542 / 539630 bytes).
        # Outside the bracket, or on a rate factor already asked, bisect it.
        if hi and lo:
            h, l = max(hi), min(lo)
            if not (h < crf < l) or any(abs(round(crf, 3) - c) < 1e-6 for c, _ in pts):
                crf = (h + l) / 2.0
    # The slope a later build's warm start begins from: the widest pair of
    # points with different sizes, not the last local slope, which on a tread
    # of the staircase is flat or clamped.
    sp = sorted(pts)
    for i in range(len(sp)):
        for j in range(len(sp) - 1, i, -1):
            (ca, ba), (cb, bb) = sp[i], sp[j]
            if ba != bb and ba > 0 and bb > 0 and cb - ca > 0.05:
                m = math.log(bb / ba) / (cb - ca)
                if m < 0:
                    return pts, best, keep, min(-0.05, max(-0.30, m))
    return pts, best, keep, slope


# ---------------------------------------------------------------------------
# REUSE ACROSS BUILDS. A point is keyed on the binary's sha, so every rebuild
# used to re-solve every cell from scratch: about 30 minutes for a 4K segment,
# two hours for a round of tier S plus the segments. Most rebuilds either do
# not change our output at all (a lossless speed change) or move the bytes by
# a percent or two. So, before solving a cell for a new build:
#   - the reference point does not depend on our build, so the newest record
#     for the same reference binary and subsample is carried over as it is (its direct
#     twin when that record had been moved onto our point by a reverse match);
#   - ours is encoded ONCE at the newest earlier build's solved rate factor;
#     if that bitstream's md5 equals the stored one, the output is identical
#     and so is everything measured from it, and the solve is recorded for the
#     new build with no further encodes: `reused`;
#   - otherwise the secant starts from that rate factor and that solve's
#     slope, with the encode just made as its first step: `warm`.
# A cell with no earlier solve, or LF_NO_REUSE=1, solves from scratch: `cold`.
NO_REUSE = os.environ.get("LF_NO_REUSE", "0") == "1"


def _other_builds(prefix):
    """[(date, key, record)] under `prefix` from builds other than this one,
    with the same reference binaries and subsample, newest first."""
    me = bin_id(YAH264)
    ref = ident().split(":r", 1)[1]                    # "<ref>:s<sub>"
    out = []
    for k, v in results().items():
        if not k.startswith(prefix + ":b") or not IDENT_RE.search(k):
            continue
        b, rest = k[len(prefix) + 2:].split(":r", 1)
        if b != me and rest == ref:
            out.append((v.get("date", ""), k, v))
    return sorted(out, reverse=True)


def prior_ref(cell):
    """The newest reference point from another build of ours, at the rate factor it
    was asked for (never one moved onto that build's point), or None."""
    for _, k, v in _other_builds(xkey_base(cell)):
        if not v.get("matched"):
            return v, k
        d = results().get(xkey_base(cell) + ":direct" + k[len(xkey_base(cell)):])
        if d:
            return d, k + " (direct)"
    return None, None


def prior_ours(cell):
    """The newest solved point for ours from another build, with the
    reference record it was paired with, or (None, None, None)."""
    cands = []
    me = bin_id(YAH264)
    ref = ident().split(":r", 1)[1]
    for k, v in results().items():
        if not k.startswith(f"point:{cell}:ours:onto") or not IDENT_RE.search(k):
            continue
        if f":{PRESET}:{ttag()}:b" not in k:
            continue
        b, rest = k.split(f":{ttag()}:b", 1)[1].split(":r", 1)
        if b != me and rest == ref:
            cands.append((v.get("date", ""), k, v, b))
    if not cands:
        return None, None, None
    _, k, v, b = max(cands)
    x = results().get(xkey_base(cell) + f":b{b}:r{ref}")
    return v, k, x


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
        xkey = xkey_base(cell) + ident()
        xr, xtag, _ = rec(xkey_base(cell))
        if not xr and not NO_REUSE:
            px, pk = prior_ref(cell)
            if px:
                xr = put(xkey, {**px, **ids(), "reused_from": pk})
                xtag = "reused from another build of ours"
        if xr:
            print(f"  {cell:<20}x264  crf {xr['crf']:>6.3f}  cached "
                  f"{xr['bytes']} bytes  ({xtag})", flush=True)
        else:
            out = os.path.join(wd, "x.264")
            t = time.monotonic()
            sz = encode("x264", cell, X264_CRF, out, solve_threads("x264"))
            el = time.monotonic() - t
            neg, psnr, qx = score(cell, out, wd)
            xr = put(xkey, {"crf": X264_CRF, "bytes": sz, "neg": neg, **qx,
                            "psnr_y": psnr, "frames": frames, "fps": fps,
                            "solve_threads": solve_threads("x264"),
                            "untimed_encode_s": el,
                            "date": time.strftime("%Y-%m-%d %H:%M"), **ids(),
                            "kbps": sz * 8 / (frames / fps) / 1000})
            xtag = "new"
            print(f"  {cell:<20}x264  crf {X264_CRF:>5.1f}  {sz} bytes  "
                  f"NEG {neg:6.2f}  PSNR-Y {psnr:6.2f}", flush=True)

        nr, ntag, _ = rec(nkey_base(cell, xr["bytes"]),
                          legacy=(xtag == "legacy-key"))
        if nr:
            print(f"  {cell:<20}ours  crf {nr['crf']:>6.3f}  cached "
                  f"{nr['bytes']} bytes  ({ntag})", flush=True)
            return
        if xtag == "legacy-key":
            sys.exit(f"longform: {cell}: the x264 point is a legacy-key record "
                     "with no pair for ours; run without LF_LEGACY=1 to solve "
                     "both sides on this build")
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
        first, how = None, "cold"
        po, pk, px = (None, None, None) if NO_REUSE else prior_ours(cell)
        same_t = po is not None and str(po.get("solve_threads")) == str(solve_threads("ours"))
        if po:
            out = os.path.join(wd, "r.264")
            t = time.monotonic()
            sz = encode("ours", cell, po["crf"], out, solve_threads("ours"))
            el = time.monotonic() - t
            md5 = file_md5(out)
            prior_target = int(pk.split(":ours:onto", 1)[1].split(":", 1)[0])
            same_x = (px is not None and px.get("bytes") == prior_target)
            if same_t and po.get("md5") == md5 and (prior_target == target or same_x):
                # identical output: every number measured from it stands
                if prior_target != target:
                    # that build's reference point had been moved onto ours, and
                    # an identical ours is matched by the same move
                    if xkey in results():
                        put(xkey_base(cell) + ":direct" + ident(), results()[xkey])
                    xr = put(xkey, {**px, **ids(), "reused_from": pk})
                nkey = nkey_base(cell, prior_target) + ident()
                put(nkey, {**po, **ids(), "reused_from": pk,
                           "retag": "retagged, bytes identical",
                           "date": time.strftime("%Y-%m-%d %H:%M")})
                print(f"  {cell:<20}ours  crf {po['crf']:>6.3f}  reused: md5 "
                      f"{md5[:12]} matches the build {pk.split(':b')[-1][:12]} "
                      f"solve  [1 encode, {el:.0f} s, untimed]", flush=True)
                return
            first, how = (sz, out), "warm"
            start, slope = po["crf"], po.get("slope", slope)
            print(f"  {cell:<20}ours  warm: from crf {po['crf']:.3f} "
                  f"(build {pk.split(':b')[-1][:12]}), slope {slope:.3f}; output "
                  + ("changed" if same_t and po.get("md5") else
                     "md5 not on record" if same_t else
                     f"not comparable (that solve ran at {po.get('solve_threads')} "
                     f"threads, this one at {solve_threads('ours')})"),
                  flush=True)
        else:
            par = uncropped(cell)
            pc = sorted((v.get("date", ""), v["crf"]) for k, v in results().items()
                        if par and k.startswith(f"point:{par}:ours:onto")
                        and f":{PRESET}:{ttag()}:b" in k)
            if pc and not scaled:
                start = pc[-1][1]
            print(f"  {cell:<20}ours  cold: no earlier solve for this cell"
                  + (f"; starting from {par}'s crf {start:.3f}" if pc and not scaled
                     else ""), flush=True)
        pts, best, nbest, slope = _secant("ours", cell, target, start, slope, wd,
                                          "n", first)
        if best[2] > MATCH_BAR / 100 and len(pts) >= 2:
            near = sorted(pts, key=lambda cb: abs(cb[1] - target))[:2]
            if abs(near[0][0] - near[1][0]) < 0.01:
                print(f"    the two nearest rate factors are {near[0][0]:.3f} "
                      f"and {near[1][0]:.3f} and write {near[0][1]} and "
                      f"{near[1][1]} bytes: the target is inside a step",
                      flush=True)
        nkey = nkey_base(cell, xr["bytes"]) + ident()
        if best[2] > MATCH_BAR / 100 and not REVERSE_OFF:
            print(f"    our curve steps over the target ({100*best[2]:.2f}% at "
                  f"crf {best[0]:.3f}); matching x264 onto our point instead",
                  flush=True)
            xcrf = X264_CRF + math.log(best[1] / xr["bytes"]) / slope
            _, xbest, xfile, _ = _secant("x264", cell, best[1], xcrf, slope, wd, "x")
            if xbest[2] < best[2]:
                xneg, xpsnr, xqx = score(cell, xfile, wd)
                # the direct record is kept under its own key, never lost
                if xkey in results():
                    put(xkey_base(cell) + ":direct" + ident(), results()[xkey])
                xr = put(xkey, {"crf": xbest[0], "bytes": xbest[1], "neg": xneg,
                                **xqx,
                                "psnr_y": xpsnr, "frames": frames, "fps": fps,
                                "solve_threads": solve_threads("x264"),
                                "date": time.strftime("%Y-%m-%d %H:%M"), **ids(),
                                "matched": "reverse: x264 moved onto ours",
                                "kbps": xbest[1] * 8 / (frames / fps) / 1000})
                nkey = nkey_base(cell, xbest[1]) + ident()
                best = (best[0], best[1], xbest[2])
                print(f"  {cell:<20}x264  crf {xbest[0]:>6.3f}  {xbest[1]} bytes"
                      f"  (reverse match)  NEG {xneg:6.2f}  PSNR-Y {xpsnr:6.2f}",
                      flush=True)
        if best[2] > MATCH_BAR / 100:
            print(f"    [warn] {cell}: {100*best[2]:.2f}% apart at the closest "
                  "reachable pair -- the timed legs will refuse it", flush=True)
        neg, psnr, qx = score(cell, nbest, wd)
        put(nkey, {"crf": best[0], "bytes": best[1], "neg": neg, **qx,
                   "psnr_y": psnr, "frames": frames, "fps": fps,
                   "kbps": best[1] * 8 / (frames / fps) / 1000,
                   "md5": file_md5(nbest), "slope": slope, "solve": how,
                   "encodes": len(pts),
                   "solve_threads": solve_threads("ours"),
                   "date": time.strftime("%Y-%m-%d %H:%M"), **ids(),
                   "dsize_pct": 100 * (best[1] / xr["bytes"] - 1)})
        print(f"  {cell:<20}ours  crf {best[0]:>6.3f}  {best[1]} bytes  "
              f"({100*(best[1]/xr['bytes']-1):+.2f}%)  NEG {neg:6.2f}  "
              f"PSNR-Y {psnr:6.2f}  [{how}, {len(pts)} encodes]", flush=True)
    finally:
        shutil.rmtree(wd, ignore_errors=True)


# A PAIR NEAR THE EDGE OF THE BAR IS ONE ODD RUN FROM A VOID LEG. Default-thread
# output is not repeatable byte for byte under load (2026-10-10: two stored
# sizes differed from a repeat by 0.0001-0.01%), so a pair sitting at +-1.0%
# can time out of the bar. `refine` takes this build's pair at the current
# thread tag and, when it is beyond REFINE_OVER percent, steps ours toward the
# reference (target +-0.5%) and re-scores. When our curve steps over the target
# (the integer-QP staircase) it moves the reference onto our stored point
# instead, as `point` does, and only the reference is re-scored.
REFINE_OVER = float(os.environ.get("REFINE_OVER", "0.8"))


def do_refine(cell):
    info = cell_info(cell)
    frames, fps = info["frames"], fps_of(info)
    xkey = xkey_base(cell) + ident()
    xr = results().get(xkey)
    nr = results().get(nkey_base(cell, xr["bytes"]) + ident()) if xr else None
    if not xr or not nr:
        sys.exit(f"longform: {cell} has no pair ({ttag()}) on this build to refine")
    d = nr["dsize_pct"]
    if abs(d) <= REFINE_OVER:
        print(f"  {cell:<20}{ttag()}: {d:+.2f}%, inside +-{REFINE_OVER:g}%: left alone", flush=True)
        return
    before = {"crf": nr["crf"], "bytes": nr["bytes"], "dsize_pct": d,
              "x264_crf": xr["crf"], "x264_bytes": xr["bytes"]}
    slope = min(-0.05, max(-0.30, nr.get("slope") or -0.13))
    wd = scratch.mkdtemp("longform")
    try:
        target = xr["bytes"]
        crf0 = nr["crf"] + math.log(target / nr["bytes"]) / slope
        pts, best, nbest, slope = _secant("ours", cell, target, crf0, slope, wd, "n")
        if best[2] <= 0.005:
            neg, psnr, qx = score(cell, nbest, wd)
            put(nkey_base(cell, target) + ident(),
                {**nr, "crf": best[0], "bytes": best[1], "neg": neg, **qx, "psnr_y": psnr,
                 "kbps": best[1] * 8 / (frames / fps) / 1000, "md5": file_md5(nbest),
                 "slope": slope, "dsize_pct": 100 * (best[1] / target - 1),
                 "refined": before, "date": time.strftime("%Y-%m-%d %H:%M"), **ids()})
            print(f"  {cell:<20}{ttag()}: refined ours crf {before['crf']:.3f} -> {best[0]:.3f}, "
                  f"{d:+.2f}% -> {100*(best[1]/target-1):+.2f}%  NEG {neg:6.2f}  "
                  f"PSNR-Y {psnr:6.2f}  [{len(pts)} encodes]", flush=True)
            return
        print(f"    our curve steps over the target (closest {100*(best[1]/target-1):+.2f}% at "
              f"crf {best[0]:.3f}); moving the reference onto our stored point", flush=True)
        xcrf = xr["crf"] + math.log(nr["bytes"] / xr["bytes"]) / slope
        xpts, xbest, xfile, _ = _secant("x264", cell, nr["bytes"], xcrf, slope, wd, "x")
        if xbest[2] >= abs(d) / 100:
            print(f"  {cell:<20}{ttag()}: no closer pair found; left at {d:+.2f}%", flush=True)
            return
        xneg, xpsnr, xqx = score(cell, xfile, wd)
        if not xr.get("matched"):
            put(xkey_base(cell) + ":direct" + ident(), xr)
        put(xkey, {"crf": xbest[0], "bytes": xbest[1], "neg": xneg, **xqx, "psnr_y": xpsnr,
                   "frames": frames, "fps": fps, "solve_threads": solve_threads("x264"),
                   "date": time.strftime("%Y-%m-%d %H:%M"), **ids(),
                   "matched": "reverse: x264 moved onto ours",
                   "kbps": xbest[1] * 8 / (frames / fps) / 1000})
        put(nkey_base(cell, xbest[1]) + ident(),
            {**nr, "dsize_pct": 100 * (nr["bytes"] / xbest[1] - 1), "refined": before,
             "date": time.strftime("%Y-%m-%d %H:%M")})
        print(f"  {cell:<20}{ttag()}: reference moved crf {before['x264_crf']:.3f} -> {xbest[0]:.3f}; "
              f"ours unchanged at crf {nr['crf']:.3f}: {d:+.2f}% -> "
              f"{100*(nr['bytes']/xbest[1]-1):+.2f}%  reference NEG {xneg:6.2f}  PSNR-Y {xpsnr:6.2f}",
              flush=True)
    finally:
        shutil.rmtree(wd, ignore_errors=True)


def do_rescore(cell):
    """Re-score every cached point/ladder entry of a cell at the CURRENT
    subsample without solving. The re-encode must reproduce the recorded byte
    count or the entry is left alone: a different size was a different encode
    (thread count, binary, preset) and its score would silently replace a good
    number with one from somewhere else.

    Since 2026-09-26 the rescored entry is ADDED under the current identity
    (build + subsample) and the old record is left as it was; a key that
    already carries the current identity is skipped."""
    info = cell_info(cell)
    frames = info["frames"]
    r = results()
    todo = sorted(k for k in r
                  if (k.startswith(f"ladder:{cell}:") or k.startswith(f"point:{cell}:"))
                  and ":direct" not in k)
    wd = scratch.mkdtemp("longform")
    try:
        for key in todo:
            v = r[key]
            target = strip_ident(key) + ident()
            if target in results():
                print(f"  {key}: already on this build at subsample {SUBSAMPLE}",
                      flush=True)
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
            neg, psnr, qx = score(cell, out, wd, 0, lim)
            v2 = dict(v)
            v2.update(neg=neg, psnr_y=psnr, rescored_from=key, **qx,
                      date=time.strftime("%Y-%m-%d %H:%M"), **ids())
            put(target, v2)
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
    if not is_full(cell) or STAGED:
        sys.exit("longform: verify-feed is for master-decoded cells (a whole "
                 "title or an excerpt), without --staged")
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


def do_stage_excerpt(cell):
    """Untimed. Write an excerpt ONCE as 8-bit 4:2:0 Y4M (stage_path) from the
    same VideoToolbox feeder the untimed phases use, then prove it:
      - lf_feed's own per-chunk timestamp check (first frame, last frame,
        count) runs during the decode, as always;
      - the file holds exactly the cell's frame count, whole frames only,
        under the header the master decode wrote;
      - the block-copy feeder a staged leg will use, reading the file back,
        writes a stream with the same md5 as the one decoded.
    A verified file is recorded as stage:<cell>@<path>; `--staged` reads only
    files with such a record, at the recorded size."""
    if kind(cell) != "x10":
        sys.exit("longform: stage-excerpt is for excerpt cells (<title>-x10_<size>)")
    if uncropped(cell):
        sys.exit(f"longform: {cell} reads the staged file of {uncropped(cell)}; "
                 "stage that one")
    info = cell_info(cell)
    n = info["frames"]
    path = stage_path(cell)
    if staged_file(cell) == path:
        print(f"  {cell}: already staged and verified at {path}", flush=True)
        return
    per = info["w"] * info["h"] * 3 // 2 + len(b"FRAME\n")
    d = os.path.dirname(path)
    os.makedirs(d, exist_ok=True)
    need = n * per
    free = shutil.disk_usage(d).free
    if free - need < STAGE_FLOOR_GB * 1e9:
        sys.exit(f"longform: {cell} needs {need/1e9:.1f} GB at {d}, which has "
                 f"{free/1e9:.0f} GB free (floor {STAGE_FLOOR_GB:g} GB)"
                 + ("" if STAGE_2160_INTERNAL or split(cell)[1] != "2160p" else
                    "; --internal-2160 sends a 2160p excerpt to "
                    f"{STAGE_INTERNAL}"))
    argv = feeder(cell, 0, n, STAGE_WORKERS, source="vt")
    print(f"  {cell}: staging {n} frames ({need/1e9:.1f} GB) to {path}\n"
          f"    {shell(argv)}", flush=True)
    part = path + ".part"
    h = hashlib.md5()
    t = time.monotonic()
    ferr = tempfile.TemporaryFile()
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=ferr)
    with open(part, "wb") as f:
        for b in iter(lambda: p.stdout.read(8 << 20), b""):
            h.update(b)
            f.write(b)
    if p.wait() != 0:
        ferr.seek(0)
        os.remove(part)
        sys.exit(f"longform: the feeder failed staging {cell}:\n  "
                 f"{ferr.read().decode(errors='replace')[-600:]}")
    md5 = h.hexdigest()
    with open(part, "rb") as f:
        hdr = f.readline()
    size = os.path.getsize(part)
    body = size - len(hdr)
    tags = hdr.decode("ascii", "replace").split()
    geom = (f"W{info['w']}" in tags and f"H{info['h']}" in tags
            and not any(t.startswith("C") and not t.startswith("C420")
                        for t in tags))
    frames, rem = divmod(body, per)
    back = hashlib.md5()
    rp = subprocess.Popen([sys.executable, FEED, "y4m", "--src", part,
                           "--start", "0", "--count", str(n)],
                          stdout=subprocess.PIPE)
    for b in iter(lambda: rp.stdout.read(8 << 20), b""):
        back.update(b)
    rrc = rp.wait()
    ok = geom and rem == 0 and frames == n and rrc == 0 and back.hexdigest() == md5
    print(f"  {cell}: {frames} frames (want {n}), remainder {rem} B, header "
          f"{hdr.decode().strip()!r}, md5 {md5}, read back "
          f"{back.hexdigest()}: {'VERIFIED' if ok else 'FAILED'} "
          f"[{time.monotonic()-t:.0f} s]", flush=True)
    if not ok:
        os.remove(part)
        sys.exit(f"longform: {cell}: the staged file does not reproduce the "
                 "decode; removed, nothing recorded")
    os.replace(part, path)
    put(f"stage:{cell}@{path}", {
        "ok": True, "cell": cell, "path": path, "frames": n, "bytes": size,
        "md5": md5, "header": hdr.decode().strip(), "source": argv[4],
        "start_frame": excerpt_span(cell)[0], "workers": STAGE_WORKERS,
        "test_frames": TEST_FRAMES, "date": time.strftime("%Y-%m-%d %H:%M")})


# ---------------------------------------------------------------------------
# Timed.

def load_guard():
    l1 = os.getloadavg()[0]
    print(f"  load average {l1:.2f} (LOAD_MAX {LOAD_MAX:g})", flush=True)
    if l1 > LOAD_MAX and os.environ.get("LF_FORCE") != "1":
        sys.exit("longform: the box is not idle; a timed leg would measure the "
                 "neighbours. LF_FORCE=1 overrides, and the record says so.")


def matched(cell, tag=None, seeks=True):
    """The matched pair for THIS build (LF_LEGACY=1 also accepts a pre-
    identity pair, and the timed record then says "legacy-key"). `seeks` is
    False for a leg that reads the whole cell front to back."""
    r = results()
    xr, xtag, xk = rec(xkey_base(cell, tag))
    how = ("SOLVE_THREADS= " if tag == "tdefault" else "")
    if not xr:
        sys.exit(f"longform: {cell} has no matched point ({tag or ttag()}) on this "
                 f"build -- run `{how}{sys.argv[0]} point {cell}` first (untimed)")
    nr, ntag, nk = rec(nkey_base(cell, xr["bytes"], tag), legacy=(xtag == "legacy-key"))
    if not nr:
        sys.exit(f"longform: {cell} has no solved point for ours ({tag or ttag()})")
    print(f"  {cell}: matched pair {xtag} {tag or ttag()} (x264 crf {xr['crf']:.3f}, ours "
          f"crf {nr['crf']:.3f}, {nr['dsize_pct']:+.2f}%)", flush=True)
    xr = dict(xr, key=xk, tag=xtag)
    nr = dict(nr, key=nk, tag=ntag)
    for side, rr in (("x264", xr), ("ours", nr)):
        if rr.get("frames_ok") is False:
            sys.exit(f"longform: {cell}: the {side} point was scored on a short "
                     "decode (frame-mismatch); re-solve before timing it")
    # A wall ratio at two file sizes is not a speed reading.
    if abs(nr["dsize_pct"]) > MATCH_BAR:
        sys.exit(f"longform: {cell} is not rate-matched ({nr['dsize_pct']:+.2f}%,"
                 f" bar +/-{MATCH_BAR:g}%)")
    # verify-feed proves the chunked decode and the window seeks against one
    # sequential decode. A whole-cell leg fed by ONE decoder is that sequential
    # decode, the same one the solve and the scoring read: nothing to prove.
    if master_fed(cell) and (seeks or timed_workers(cell) > 1):
        v = r.get(f"verify-feed:{cell}")
        if not (v and v["ok"]):
            sys.exit(f"longform: {cell}: the chunked feeder has not been verified "
                     f"against a sequential decode -- run "
                     f"`{sys.argv[0]} verify-feed {cell}` first (untimed)")
    return xr, nr


LEG_FIELDS = "wall, cpu, rss, feeder cpu, pipeline wall, bytes, md5"


def assert_stdin(arm, feed, enc):
    """PARITY-SPEC section 4.3: both arms read Y4M on stdin from one feeder.
    A by-path leg times the reader, not the encoder (see the docstring), so a
    command line that is not the stdin form is refused, not measured."""
    if arm == "ours":
        ok = enc[1:3] == ["--input-y4m", "-"]
    else:
        ok = (enc[-1] == "-" and "--demuxer" in enc
              and enc[enc.index("--demuxer") + 1] == "y4m")
    if not (ok and FEED in feed):
        sys.exit(f"longform: refusing a timed {arm} leg that does not read Y4M "
                 f"on stdin from lf_feed:\n  {shell(feed)} | {shell(enc)}")


def file_md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(8 << 20), b""):
            h.update(b)
    return h.hexdigest()


def leg(arm, cell, crf, threads, purec, start=0, count=None, workers=1, wd=None):
    """One timed encode. The bitstream goes to a scratch file (not /dev/null,
    since 2026-09-26) so its size and md5 are recorded and printed: an arm
    that did not run, or ran the other side's binary, is visible."""
    out = os.path.join(wd, f"{arm}.264")
    if os.path.exists(out):
        os.remove(out)
    feed = feeder(cell, start, count, workers)
    enc = enc_cmd(arm, crf, out, threads, purec)
    assert_stdin(arm, feed, enc)
    r = pipe_run(feed, enc, arm_env(arm, cell, purec), timed=True)
    size = os.path.getsize(out) if os.path.exists(out) else 0
    md5 = file_md5(out) if size else "-"
    if os.path.exists(out):
        os.remove(out)
    if size < 1024:
        sys.exit(f"longform: {cell}: the {arm} leg wrote {size} bytes -- the arm "
                 "did not run. Nothing is recorded.")
    return [r["wall"], r["cpu"], r["rss"], r["feed_cpu"], r["pipe_wall"], size, md5]


def pair_guard(cell, what, ours, x264):
    """Two encoders never write the same bitstream. If the two arms of a pair
    print one md5, the same binary ran on both sides (a path or an env knob
    pointed the wrong way) and the ratio is the machine against itself."""
    same = {m[6] for m in ours} & {m[6] for m in x264}
    if same:
        sys.exit(f"longform: {cell} {what}: ours and x264 printed the same md5 "
                 f"({', '.join(sorted(same))}), and they are meant to differ. "
                 "One binary ran on both sides; nothing is recorded.")


# THE FEEDER HAS TO OUTRUN BOTH ENCODERS, CLEARLY (PARITY-SPEC section 4.2).
# Before any timed leg the feeder alone is timed over the first
# FEEDCHECK_SECS of the span, and the leg refuses to start unless it runs at
# least FEED_MARGIN x the faster encoder's expected rate: the larger of the
# booking model and the best rate any record of this cell (or the whole
# title an excerpt is cut from) has measured. The report's own validity line
# uses the same 1.5x on the read baseline (spec section 4.2).
FEED_MARGIN = float(os.environ.get("FEED_MARGIN", "1.5"))
FEEDCHECK_SECS = float(os.environ.get("FEEDCHECK_SECS", "60"))


def related(cell):
    out = [cell]
    if kind(cell) == "x10":
        title, size = split(cell)
        out.append(f"{base_title(title)}_{size}")
    return out


def measured_fps(cell, cfg):
    """The best rate either arm has shown on this cell in a record, fps."""
    best = 0.0
    for c in related(cell):
        t = timed_rec(c)[0]
        if t and cfg in ("simd", "simd1"):
            grp = "default" if cfg == "simd" else "t1"
            for arm in ("ours", "x264"):
                ws = [m[0] for m in t[arm].get(grp, [])]
                if ws:
                    best = max(best, t["frames"] / med(ws))
        p = purec_rec(c)[0]
        g = {"g1": "G1", "g2": "G2", "simd1": "S1"}.get(cfg)
        if p and g and p.get(g):
            n = sum(k for _, k in p["windows"])
            for arm in ("ours", "x264"):
                if p[g].get(arm):
                    tot = sum(med([m[0] for m in ms]) for ms in p[g][arm])
                    best = max(best, n / tot)
    return best


def expected_fps(cell, cfg):
    model, seen = est_fps(cell, cfg), measured_fps(cell, cfg)
    return (seen, "measured") if seen > model else (model, "model")


def feed_guard(cell, cfgs, start=0, count=None, workers=1):
    info = cell_info(cell)
    count = count or info["frames"] - start
    n = min(count, max(1, int(round(FEEDCHECK_SECS * fps_of(info)))))
    wall, _, _ = timed_run(feeder(cell, start, n, workers))
    ffps = n / max(wall, 1e-3)
    (exp, src), cfg = max((expected_fps(cell, c), c) for c in cfgs)
    ok = ffps >= FEED_MARGIN * exp
    over = os.environ.get("LF_FEEDER_OK") == "1"
    rec_ = {"feed_fps": ffps, "frames": n, "expected_fps": exp,
            "expected_from": src, "cfg": cfg, "margin": FEED_MARGIN, "ok": ok,
            "override": over and not ok,
            "source": "staged" if staged_now(cell) else
                      ("vt" if is_full(cell) else "y4m"),
            "date": time.strftime("%Y-%m-%d %H:%M")}
    print(f"  {cell}: feeder alone {ffps:7.1f} fps over {n} frames; the faster "
          f"encoder is expected at {exp:.1f} fps ({src}, {cfg}); needs "
          f"{FEED_MARGIN:g}x = {FEED_MARGIN*exp:.1f}: "
          f"{'OK' if ok else 'FEEDER-BOUND'}", flush=True)
    if not ok:
        if over:
            print("  LF_FEEDER_OK=1: timing anyway, and the record says so",
                  flush=True)
        else:
            put(f"feedcheck:{cell}:{rec_['date']}", rec_)
            sys.exit(f"longform: {cell}: FEEDER-BOUND -- refusing to time it. "
                     "Stage it (`stage-excerpt`, then `--staged`), or set "
                     "LF_FEEDER_OK=1 to time a void number knowingly.")
    return rec_


def read_baseline(cell, start=0, count=None, workers=1):
    wall, cpu, rss = timed_run(feeder(cell, start, count, workers))
    return [wall, cpu, rss]


def stamp():
    return {"date": time.strftime("%Y-%m-%d %H:%M"),
            "load1": os.getloadavg()[0], "forced": os.environ.get("LF_FORCE") == "1",
            "x264": X264, "x264_c": X264_C, "yah264": YAH264,
            "sha": {"yah264": bin_id(YAH264), "x264": bin_id(X264),
                    "x264_c": bin_id(X264_C)},
            "subsample": SUBSAMPLE, "versions": versions()}


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


def point_fields(xr, nr):
    return {"x264_crf": xr["crf"], "ours_crf": nr["crf"],
            "x264_bytes": xr["bytes"], "ours_bytes": nr["bytes"],
            "dsize_pct": nr["dsize_pct"], "x264_neg": xr["neg"],
            "ours_neg": nr["neg"], "x264_psnr_y": xr["psnr_y"],
            "ours_psnr_y": nr["psnr_y"], "point_tag": xr["tag"],
            "point_keys": [xr["key"], nr["key"]],
            "solve_threads": nr.get("solve_threads"),
            "x264_matched": xr.get("matched")}


def show(cell, arm, what, m, n):
    print(f"  {cell:<22}{arm:<6}{what}  {m[0]:8.2f}s wall {m[1]:9.2f}s cpu  "
          f"{m[2]/2**30:5.2f} GiB  {n/m[0]:7.1f} fps  {m[5]} B  md5 {m[6]}",
          flush=True)


def do_timed(cell):
    """G3 (SIMD, default threads, ROUNDS rounds, order reversed each round),
    SIMD one-thread on the whole cell for a segment or a tier S clip (a title
    or an excerpt takes it on the stratified windows in `purec`, the S1
    group), then the read baseline. The feeder is timed alone first and a
    FEEDER-BOUND cell is refused. G3 runs at default threads, so it reads the
    pair solved at default threads (see ttag)."""
    xr, nr = matched(cell, "tdefault", seeks=False)
    info = cell_info(cell)
    frames = info["frames"]
    w = timed_workers(cell)
    fc = feed_guard(cell, ["simd"], workers=w)
    out = {"cell": cell, "frames": frames, "fps": fps_of(info), "w": info["w"],
           "h": info["h"], **point_fields(xr, nr), "rounds": ROUNDS,
           "feed_workers": w, "feed_check": fc, "feed": fc["source"],
           "fields": LEG_FIELDS, **stamp()}
    crfs = {"ours": nr["crf"], "x264": xr["crf"]}
    for arm in crfs:
        out[arm] = {"default": [], "t1": []}
    wd = scratch.mkdtemp("longform-timed")
    try:
        for i in range(ROUNDS):
            for arm in (("x264", "ours") if i % 2 else ("ours", "x264")):
                m = leg(arm, cell, crfs[arm], default_threads(arm), False,
                        workers=w, wd=wd)
                out[arm]["default"].append(m)
                show(cell, arm, f"G3 r{i+1}", m, frames)
        # A VARIATION EVENT: default-thread output can differ by a few bytes from run
        # to run, and on a pair near the bar one round can read over 1% while the
        # others do not. That is not a mismatched cell: log it and time one more
        # round, so the median size and wall rest on the agreeing rounds.
        rd = [100 * (o[5] / x[5] - 1) for o, x in zip(out["ours"]["default"],
                                                      out["x264"]["default"])]
        over = [abs(v) > MATCH_BAR for v in rd]
        if any(over) and not all(over):
            out["variation_event"] = rd
            print(f"  {cell}: VARIATION EVENT: round size deltas "
                  + ", ".join(f"{v:+.3f}%" for v in rd) + "; timing one more round", flush=True)
            for arm in ("ours", "x264"):
                m = leg(arm, cell, crfs[arm], default_threads(arm), False, workers=w, wd=wd)
                out[arm]["default"].append(m)
                show(cell, arm, f"G3 r{ROUNDS+1}", m, frames)
            out["rounds"] = ROUNDS + 1
        pair_guard(cell, "G3", out["ours"]["default"], out["x264"]["default"])
        for i in range(0 if is_full(cell) else T1_ROUNDS):
            for arm in (("x264", "ours") if i % 2 else ("ours", "x264")):
                m = leg(arm, cell, crfs[arm], 1, False, workers=w, wd=wd)
                out[arm]["t1"].append(m)
                show(cell, arm, f"t1 r{i+1}", m, frames)
        if out["ours"]["t1"]:
            pair_guard(cell, "SIMD t1", out["ours"]["t1"], out["x264"]["t1"])
    finally:
        shutil.rmtree(wd, ignore_errors=True)
    out["timed_dsize_pct"] = 100 * (med([m[5] for m in out["ours"]["default"]]) /
                                    med([m[5] for m in out["x264"]["default"]]) - 1)
    out["read"] = read_baseline(cell, workers=w)
    print(f"  {cell:<22}read  baseline {out['read'][0]:8.2f}s wall "
          f"{out['read'][1]:9.2f}s cpu  {frames/out['read'][0]:7.1f} fps", flush=True)
    put(f"timed:{cell}:{PRESET}{TSUFFIX}{feed_tag(cell)}{ident()}", out)


def do_purec(cell):
    """G1 (pure C, one thread) and G2 (pure C, default threads) on the
    stratified windows, both arms, order reversed window by window, and a read
    baseline per window. The CRFs are the cell's matched ones. A clip shorter
    than a window is one window: the whole clip."""
    wp = {g: win_rec(cell, g) for g in ("G1", "G2")}
    # the pair gives the record its quality fields; with both window solves in
    # hand a cell that has only the default-thread pair can run too
    if all(wp.values()) and not rec(xkey_base(cell))[0]:
        xr, nr = matched(cell, "tdefault")
    else:
        xr, nr = matched(cell)
    info = cell_info(cell)
    w = timed_workers(cell)
    wins = windows(cell)
    fc = feed_guard(cell, ["g2", "simd1"], wins[0][0], wins[0][1], w)
    crfs = {"ours": nr["crf"], "x264": xr["crf"]}
    gcrf = {g: ({"ours": wp[g]["ours_crf"], "x264": wp[g]["x264_crf"]}
                if wp.get(g) else crfs) for g in ("G1", "G2")}
    gcrf["S1"] = gcrf["G1"]
    for g in ("G1", "G2"):
        print(f"  {cell}: {g} rate factors ours {gcrf[g]['ours']:.3f}, reference "
              f"{gcrf[g]['x264']:.3f} (" + ("matched on the windows" if wp.get(g)
                                            else "the whole cell's pair") + ")", flush=True)
    out = {"cell": cell, "frames": info["frames"], "fps": fps_of(info),
           "windows": wins, "rounds": PC_ROUNDS, "feed_workers": w,
           "crfs": crfs, "fields": LEG_FIELDS, "feed_check": fc,
           "feed": fc["source"], **point_fields(xr, nr),
           "G1": {"ours": [], "x264": []}, "G2": {"ours": [], "x264": []},
           "S1": {"ours": [], "x264": []},
           "win_match": {g: ({k: wp[g][k] for k in ("ours_crf", "x264_crf", "dsize_pct",
                                                    "before_dsize_pct")}
                             if wp.get(g) else None) for g in ("G1", "G2")},
           "read": [], **stamp()}
    wd = scratch.mkdtemp("longform-timed")
    try:
        for wi, (s, n) in enumerate(wins):
            # S1 is SIMD at one thread: the informative column, sampled on full
            # titles since 2026-09-25 (it was most of the exclusive-window hours)
            # S1 (SIMD at one thread) is informative, not a goal, and on a 2160p
            # excerpt it is a third of the leg. It runs only when asked (LF_S1=1).
            for g in (("G1", "G2", "S1") if os.environ.get("LF_S1") == "1" else ("G1", "G2")):
                ms = {"ours": [], "x264": []}
                for r in range(PC_ROUNDS):
                    for arm in (("x264", "ours") if (wi + r) % 2 else ("ours", "x264")):
                        t = default_threads(arm) if g == "G2" else 1
                        m = leg(arm, cell, gcrf[g][arm], t, g != "S1", s, n, w, wd)
                        ms[arm].append(m)
                        show(cell, arm, f"{g} w{wi} {s}+{n}", m, n)
                pair_guard(cell, f"{g} window {wi}", ms["ours"], ms["x264"])
                for arm in ms:
                    out[g][arm].append(ms[arm])
            out["read"].append(read_baseline(cell, s, n, w))
    finally:
        shutil.rmtree(wd, ignore_errors=True)
    put(f"purec:{cell}:{PRESET}{TSUFFIX}{feed_tag(cell)}{ident(X264_C, X264)}", out)


# THE G1/G2 SIZE MATCH IS MADE ON THE TIMED WINDOWS (owner ruling, 2026-10-10).
# G1 and G2 time 30-second windows, each coded as its own stream. At the whole
# cell's rate factors the two encoders' summed window bytes differed by up to
# 15%, and a cell over 5% is void for that goal. `winpoint` solves ours onto
# the reference's summed window bytes (reference at its default rate factor)
# at each timed thread count: one thread for G1, default threads for G2. It
# solves OURS with the SIMD binary (our SIMD and pure C write one bitstream, a
# hard gate at one thread) onto the PURE-C reference's bytes (the reference's
# two builds do not write one bitstream). LF_WIN_VERIFY=1 re-encodes the solved
# point with the other build of each at both thread counts and records what
# agrees. The pure-C leg prints its own window bytes, so a mismatch cannot hide.
WIN_TOL = float(os.environ.get("WIN_TOL", "0.5"))        # percent; the bar is 1
WIN_VOID = float(os.environ.get("WIN_VOID", "5.0"))      # percent; over this the goal is void


def win_bytes(arm, cell, crf, threads, wins, wd, purec=False):
    tot, mds = 0, []
    out = os.path.join(wd, f"w-{arm}.264")
    for s, n in wins:
        tot += encode(arm, cell, crf, out, threads, s, n, purec)
        mds.append(file_md5(out))
        os.remove(out)
    return tot, mds


def win_rec(cell, g):
    return rec(f"winpoint:{cell}:{g}:{PRESET}")[0]


def do_winpoint(cell):
    info = cell_info(cell)
    wins = windows(cell)
    # A clip timed whole (tier S) is matched at the 4-thread pair's rate factors;
    # at one thread or at default threads its size can leave the 1% bar. With
    # LF_WIN_WHOLE=1 the same match is made on the whole clip per thread count.
    if sum(n for _, n in wins) >= info["frames"] and os.environ.get("LF_WIN_WHOLE") != "1":
        print(f"  {cell}: one window is the whole clip; nothing to solve "
              "(LF_WIN_WHOLE=1 matches it per thread count)", flush=True)
        return
    r = results()
    prior = sorted((v.get("date", ""), v["crf"]) for k, v in r.items()
                   if k.startswith(f"point:{cell}:ours:onto") and ":t4:b" in k) or \
        sorted((v.get("date", ""), v["crf"]) for k, v in r.items()
               if k.startswith(f"point:{cell}:ours:onto"))
    start = prior[-1][1] if prior else X264_CRF
    wd = scratch.mkdtemp("longform-win")
    try:
        for g, to, tx in (("G1", "1", "1"),
                          ("G2", default_threads("ours"), default_threads("x264"))):
            key = f"winpoint:{cell}:{g}:{PRESET}{ident()}"
            if key in results():
                v = results()[key]
                if abs(v["dsize_pct"]) <= 1.0 or v.get("ref_moved"):
                    print(f"  {cell:<20}{g}  cached: ours crf {v['ours_crf']:.3f} "
                          f"({v['dsize_pct']:+.2f}%)", flush=True)
                    continue
                # OUR SIZE STEPS OVER THE TARGET on these windows (the integer-QP
                # staircase is coarse on 30 seconds). Move the pure-C reference's
                # rate factor onto our closest point instead, as `point` does for a
                # whole cell; the leg then runs the reference off its default.
                tb, slope, xpts = v["ours_bytes"], -0.13, []
                xcrf = v["x264_crf"] + math.log(tb / v["x264_bytes"]) / slope
                for _ in range(6):
                    xcrf = round(xcrf, 3)
                    xb2, xmd2 = win_bytes("x264", cell, xcrf, tx, wins, wd, purec=True)
                    d = 100 * (tb / xb2 - 1)
                    xpts.append((xcrf, xb2, d, xmd2))
                    print(f"    {cell} {g} windows: reference crf {xcrf:.3f} -> {xb2} bytes "
                          f"(ours {d:+.2f}%)", flush=True)
                    if abs(d) <= WIN_TOL:
                        break
                    if len(xpts) >= 2 and xpts[-1][1] != xpts[-2][1] and xpts[-1][0] != xpts[-2][0]:
                        slope = min(-0.05, max(-0.30, math.log(xpts[-1][1] / xpts[-2][1])
                                               / (xpts[-1][0] - xpts[-2][0])))
                    xcrf = xcrf + math.log(tb / xb2) / slope
                xbest = min(xpts, key=lambda q: abs(q[2]))
                if abs(xbest[2]) < abs(v["dsize_pct"]):
                    put(key, {**v, "x264_crf": xbest[0], "x264_bytes": xbest[1],
                              "x264_md5": xbest[3], "dsize_pct": xbest[2], "ref_moved": True,
                              "ours_only_dsize_pct": v["dsize_pct"],
                              "date": time.strftime("%Y-%m-%d %H:%M")})
                    print(f"  {cell:<20}{g}  reference moved to crf {xbest[0]:.3f} on the windows: "
                          f"{v['dsize_pct']:+.2f}% -> {xbest[2]:+.2f}%  [{len(xpts)} evaluations]"
                          + ("  [warn] still over the 1% bar" if abs(xbest[2]) > 1.0 else ""),
                          flush=True)
                else:
                    put(key, {**v, "ref_moved": "tried, no closer"})
                    print(f"  {cell:<20}{g}  no closer pair with the reference moved; "
                          f"left at {v['dsize_pct']:+.2f}%", flush=True)
                continue
            # the target is the PURE-C reference's bytes, the binary the legs time:
            # its bitstream is not the SIMD build's (0.01% apart on a probe), unlike ours
            xb, xmd = win_bytes("x264", cell, X264_CRF, tx, wins, wd, purec=True)
            pts, crf, slope = [], start, -0.13
            for _ in range(8):
                ob, omd = win_bytes("ours", cell, crf, to, wins, wd)
                d = 100 * (ob / xb - 1)
                pts.append((crf, ob, d, omd))
                print(f"    {cell} {g} windows: ours crf {crf:.3f} -> {ob} bytes "
                      f"({d:+.2f}% of {xb})", flush=True)
                if abs(d) <= WIN_TOL:
                    break
                if len(pts) >= 2 and pts[-1][1] != pts[-2][1] and pts[-1][0] != pts[-2][0]:
                    slope = min(-0.05, max(-0.30, math.log(pts[-1][1] / pts[-2][1])
                                           / (pts[-1][0] - pts[-2][0])))
                crf = round(crf + math.log(xb / ob) / slope, 3)
            best = min(pts, key=lambda q: abs(q[2]))
            v = {"x264_crf": X264_CRF, "x264_bytes": xb, "ours_crf": best[0],
                 "ours_bytes": best[1], "dsize_pct": best[2],
                 "before_dsize_pct": pts[0][2], "before_crf": pts[0][0],
                 "threads": {"ours": to, "x264": tx}, "windows": wins,
                 "encodes": len(pts), "ours_md5": best[3], "x264_md5": xmd,
                 "date": time.strftime("%Y-%m-%d %H:%M"), **ids()}
            if os.environ.get("LF_WIN_VERIFY") == "1":
                cb, cmd = win_bytes("ours", cell, best[0], to, wins, wd, purec=True)
                sb, sxm = win_bytes("x264", cell, X264_CRF, tx, wins, wd)
                v["purec_identical"] = {"ours": cmd == best[3], "x264": sxm == xmd,
                                        "ours_purec_bytes": cb, "x264_simd_bytes": sb}
                print(f"    {cell} {g}: pure C against SIMD at these threads: ours "
                      f"{'IDENTICAL' if cmd == best[3] else f'DIFFERS ({100*(cb/best[1]-1):+.3f}% bytes)'}"
                      f", reference {'IDENTICAL' if sxm == xmd else f'DIFFERS ({100*(sb/xb-1):+.3f}% bytes)'}",
                      flush=True)
            put(key, v)
            print(f"  {cell:<20}{g}  ours crf {best[0]:.3f} on the windows: {best[2]:+.2f}% "
                  f"(was {pts[0][2]:+.2f}% at crf {pts[0][0]:.3f})  [{len(pts)} evaluations]"
                  + ("  [warn] over the 1% bar" if abs(best[2]) > 1.0 else ""), flush=True)
    finally:
        shutil.rmtree(wd, ignore_errors=True)


def do_validate():
    """The sample's check: pure C, one thread, the WHOLE validation title,
    both arms. `report` sets it beside the sampled G1 estimate."""
    cell = VALIDATE_CELL
    xr, nr = matched(cell)
    info = cell_info(cell)
    w = timed_workers(cell)
    fc = feed_guard(cell, ["g1"], workers=w)
    crfs = {"ours": nr["crf"], "x264": xr["crf"]}
    out = {"cell": cell, "frames": info["frames"], "crfs": crfs,
           "feed_workers": w, "fields": LEG_FIELDS, "feed_check": fc,
           "feed": fc["source"], **point_fields(xr, nr), **stamp()}
    wd = scratch.mkdtemp("longform-timed")
    try:
        for arm in ("ours", "x264"):
            m = leg(arm, cell, crfs[arm], 1, True, workers=w, wd=wd)
            out[arm] = m
            show(cell, arm, "G1 full", m, info["frames"])
        pair_guard(cell, "G1 full", [out["ours"]], [out["x264"]])
    finally:
        shutil.rmtree(wd, ignore_errors=True)
    out["read"] = read_baseline(cell, workers=w)
    put(f"validate:{cell}:{PRESET}{TSUFFIX}{feed_tag(cell)}{ident(X264_C)}", out)


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
EST_MPXS = {"simd":   {"cif": 200.0, "720p": 280.0, "1080p": 250.0,
                       "2160p": 160.0},
            "simd1":  {"cif": 50.0, "720p": 52.0,  "1080p": 47.0,  "2160p": 40.0}}
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


def y4m_read_fps(path, info):
    """Modelled block-copy rate of a Y4M: internal disk, T7 SSD or USB disk."""
    mbps = (USB_MBPS if path.startswith(MEDIA) else
            T7_MBPS if path.startswith("/Volumes/") else 5000.0)
    return mbps * 1e6 / (info["w"] * info["h"] * 1.5)


def feed_fps(cell, workers):
    info = cell_info(cell)
    if not is_full(cell):
        return y4m_read_fps(cell_path(cell), info)
    sp = staged_now(cell)
    if sp:
        return y4m_read_fps(sp, info)
    title, _ = split(cell)
    title = base_title(title)
    f = FULL[title]
    nat = f["native"][0] * f["native"][1]
    vt = (VT_FPS3 if workers > 1 else VT_FPS1) * (3840 * 2160 / nat) ** 0.7
    p = master_path(title)
    if p.startswith(MEDIA):
        size = os.path.getsize(p) if os.path.exists(p) else 0
        frames = (probe_master(title) if master_ready(title) else f)["frames"]
        if size and frames:
            vt = min(vt, USB_MBPS * 1e6 / (size / frames))
    return vt


def shell(argv):
    return " ".join(a if re.fullmatch(r"[\w@%+=:,./-]+", a) else f"'{a}'"
                    for a in argv)


def do_plan(cells, full_print):
    groups = {"G3, full titles": 0.0, "sampled G1/G2 + SIMD t1": 0.0,
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
            xr, xtag, _ = rec(xkey_base(cell))
            nr = rec(nkey_base(cell, xr["bytes"]),
                     legacy=(xtag == "legacy-key"))[0] if xr else None
            if not xr:                    # show what an older build solved
                xr, xtag, _ = rec_any(xkey_base(cell))
                nr = rec_any(nkey_base(cell, xr["bytes"]))[0] if xr else None
                xtag = xtag and f"{xtag}, NOT this build"
            xc = xr["crf"] if xr else X264_CRF
            oc = nr["crf"] if nr else None
            ver = r.get(f"verify-feed:{cell}")
            state = ((f"SOLVED ({xtag})" if nr else "NOT SOLVED") +
                     ("" if not master_fed(cell) else
                      (", feed VERIFIED" if ver and ver["ok"]
                       else ", feed NOT VERIFIED")) +
                     (", STAGED" if staged_now(cell) else ""))
            print(f"### {cell}  {info['w']}x{info['h']} {fps_of(info):.3f} fps, "
                  f"{frames} frames ({frames/fps_of(info)/60:.1f} min) -- {state}")
            ocs = f"{oc:.3f}" if oc is not None else "<solve>"
            fd = shell(feeder(cell, workers=w))
            if full_print or cell == (live + anim)[0]:
                print(f"  feed : {fd}")
            else:
                print(f"  feed : {fd[:110]} ...")

            def cmd(arm, crf, th, purec=False, start=0, count=None):
                c = enc_cmd(arm, 0.0, f"<scratch>/{arm}.264", th, purec)
                c[c.index("--crf") + 1] = crf
                return f"{env_prefix(arm, cell, purec)} {shell(c)}"
            g3 = frames / min(est_fps(cell, "simd"), ffps)
            t1 = frames / min(est_fps(cell, "simd1"), ffps)
            rd = frames / ffps
            t1_runs = 0 if is_full(cell) else 2 * T1_ROUNDS
            leg_s = 2 * ROUNDS * g3 + t1_runs * t1 + rd
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
            grp_key = ("G3, full titles" if is_full(cell)
                       else "G3 + SIMD t1, segments")
            groups[grp_key] += leg_s
            if is_full(cell):
                wins = windows(cell)
                n = sum(c for _, c in wins)
                g1 = n / min(est_fps(cell, "g1"), ffps)
                g2 = n / min(est_fps(cell, "g2"), ffps)
                s1 = n / min(est_fps(cell, "simd1"), ffps)
                pc = PC_ROUNDS * 2 * (g1 + g2 + s1) + n / ffps
                groups["sampled G1/G2 + SIMD t1"] += pc
                print(f"  purec: windows {', '.join(f'{s}+{c}' for s, c in wins)}"
                      f" ({n} frames, {100*n/frames:.0f}% of the title)")
                print(f"         G1 ours: <feed --start S --count N> | "
                      f"{cmd('ours', ocs, 1, True)}")
                print(f"         G1 x264: <feed --start S --count N> | "
                      f"{cmd('x264', f'{xc:.3f}', 1, True)}")
                print(f"         G2: the same at --threads 0 / auto; "
                      f"S1: SIMD build at --threads 1")
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


def timed_rec(cell, staged=None):
    """(record, tag, key) of the cell's G3 leg from any build; the staged
    feed is preferred when both exist (it is the one that is not feeder-
    bound). staged=False/True restricts it."""
    for tag in ((":staged", "") if staged is None else
                ((":staged",) if staged else ("",))):
        got = rec_any(f"timed:{cell}:{PRESET}{TSUFFIX}{tag}")
        if got[0]:
            return got
    return None, None, None


def purec_rec(cell):
    for tag in (":staged", ""):
        got = rec_any(f"purec:{cell}:{PRESET}{TSUFFIX}{tag}", X264_C, X264)
        if got[0]:
            return got
    return None, None, None


def validate_rec(cell):
    for tag in (":staged", ""):
        got = rec_any(f"validate:{cell}:{PRESET}{TSUFFIX}{tag}", X264_C)
        if got[0]:
            return got
    return None, None, None


def point_pair(cell):
    """(x264 record, ours record, tag) from any build, the two from the SAME
    build: the x264 point first, then ours solved onto its bytes."""
    best = (None, None, None)
    for tag in ("tdefault", "t4"):
        xr, xtag, xk = rec_any(xkey_base(cell, tag))
        if not xr:
            continue
        suffix = IDENT_RE.search(xk)
        nk = nkey_base(cell, xr["bytes"], tag) + (suffix.group(0) if suffix else "")
        nr = results().get(nk)
        if nr:
            return xr, nr, xtag
        if best[0] is None:
            best = (xr, None, xtag)
    return best


def bd_of(cell):
    """(BD-rate on VMAF-NEG, tag): ours against x264 over the shared rate
    range; positive is worse. One build per curve pair (the current one, else
    the newest other, else the legacy keys), one window per curve: the
    longest with a full set on both sides."""
    r = results()
    groups = {}
    for k, v in r.items():
        if not k.startswith(f"ladder:{cell}:"):
            continue
        m = IDENT_RE.search(k)
        g = m.group(0) if m else ""
        arm = k.split(":")[2]
        groups.setdefault(g, {}).setdefault(arm, {}).setdefault(
            v["frames"], []).append(v)
    cur = ident()
    def newest(g):
        return max(v.get("date", "") for a in groups[g].values()
                   for vs in a.values() for v in vs)
    order = sorted(groups, key=newest, reverse=True)
    order.sort(key=lambda g: (g != cur, g == ""))
    for g in order:
        arms = groups[g]
        if "x264" not in arms or "ours" not in arms:
            continue
        both = [w for w in arms["x264"] if w in arms["ours"]
                and len(arms["x264"][w]) >= 4 and len(arms["ours"][w]) >= 4]
        if not both:
            continue
        w = max(both)

        def curve(arm):
            pts = sorted((v["kbps"], v["neg"]) for v in arms[arm][w])
            return [p[0] for p in pts], [p[1] for p in pts]
        rx, mx = curve("x264")
        rn, mn = curve("ours")
        try:
            tag = "new" if g == cur else ("legacy-key" if g == "" else "other-build")
            return bd.bd_rate(rx, mx, rn, mn), tag
        except Exception:
            continue
    return None, None


# BD FROM THE LADDER LANE. LF_LAD_JSON names a ladder store keyed
# "<cell>|crf|<arm>|<rung>|t<threads>|<build>" (five CRF rungs) and
# "<cell>|vbr|<arm>|k<kbit>|t<threads>|<build>". Its cells are the cropped
# tier L cells under their <title>-x10_<size> names. The BD figure is the mean
# log-rate gap of two cubic fits over the shared quality range, the ladder
# lane's own formula, so a number here equals the one that lane prints.
LAD_JSON = os.environ.get("LF_LAD_JSON", "")
_LAD = None


def _lad():
    global _LAD
    if _LAD is None:
        try:
            with open(LAD_JSON) as f:
                _LAD = json.load(f)
        except (OSError, ValueError):
            _LAD = {}
    return _LAD


def _lad_bd(rr, mr, rt, mt):
    import numpy as np
    deg = min(3, len(mr) - 1)
    f1 = np.polyfit(mr, np.log(rr), deg)
    f2 = np.polyfit(mt, np.log(rt), deg)
    lo, hi = max(min(mr), min(mt)), min(max(mr), max(mt))
    if hi <= lo:
        return None
    xs = np.linspace(lo, hi, 100)
    return (math.exp(float(np.mean(np.polyval(f2, xs) - np.polyval(f1, xs)))) - 1) * 100


# VBR WALL LEGS. LF_VBR_JSONL names the timed VBR legs' results, one JSON
# object per line (cell, k, ratio, both achieved bitrates, feed rate, the
# binary's md5); the newest line of a cell wins. One rung per cell: the
# reference's CRF 23 rate as the target, maxrate 1.5K, buffer 2K.
VBR_JSONL = os.environ.get("LF_VBR_JSONL", "")


def vbr_wall_lines():
    if not VBR_JSONL:
        return []
    legs = {}
    try:
        with open(VBR_JSONL) as f:
            for ln in f:
                ln = ln.strip()
                if ln:
                    v = json.loads(ln)
                    legs[v["cell"].replace("-x10c_", "-x10_")] = v
    except (OSError, ValueError, KeyError):
        return []
    prev = {}
    try:
        with open(os.environ.get("LF_VBR_PREV_JSONL", "")) as f:
            for ln in f:
                if ln.strip():
                    v = json.loads(ln)
                    prev[v["cell"].replace("-x10c_", "-x10_")] = v
    except (OSError, ValueError, KeyError):
        prev = {}
    pmd5 = ", ".join(sorted({v.get("ours_md5", "")[:8] for v in prev.values()}))
    out, ratios, missing = [], {}, []
    md5s = sorted({v.get("ours_md5", "")[:8] for v in legs.values()})
    for tier, cell, title, size in spec_cells():
        if tier not in ("M", "L"):
            continue
        v = legs.get(cell)
        if v is None:
            pv = prev.get(cell)
            out.append(f"| {title}_{size} | {tier} | - | not measured | - | - | - | - |"
                       + (f" {pv['ratio']:.2f} |" if pv else " - |" if prev else ""))
            missing.append(f"{title}_{size}")
            continue
        n = v["frames"]
        fast = max(n / v["ours_wall"], n / v["x264_wall"])
        fx = v["feed_fps"] / fast
        eo = 100 * (v["ours_kbps"] / v["k"] - 1)
        ex = 100 * (v["x264_kbps"] / v["k"] - 1)
        flag = " feeder-bound" if v.get("feeder_bound") or fx < FEED_VALID else ""
        flag += f" **over +-{TRACK_BAR:g}%**" if abs(eo) > TRACK_BAR else ""
        out.append(f"| {title}_{size} | {tier} | {v['k']} | {v['ratio']:.2f} | "
                   f"{v['ours_kbps']:.0f} ({eo:+.1f}%) | {v['x264_kbps']:.0f} ({ex:+.1f}%) | "
                   f"{fx:.1f}x | {v['date']}{flag} |"
                   + (f" {prev[cell]['ratio']:.2f} |" if cell in prev else " - |" if prev else ""))
        if not flag.startswith(" feeder"):
            ratios.setdefault((size, tier), []).append(v["ratio"])
    head = ["", "## VBR wall (G3 only: SIMD, default threads, one rung per cell)",
            "Owner ruling 2026-10-10: VBR speed is judged at G3 only, on one rung, the "
            "reference's CRF 23 rate as the target with maxrate 1.5x and buffer 2x, lookahead "
            f"40 both sides. Binary md5 {', '.join(md5s)}. Tracking is the achieved bitrate "
            "against the target in the timed leg; feed is the feeder's rate over the faster "
            "encoder's.",
            "Median / worst per class and tier: " + "; ".join(
                f"{s_}-{t_} {med(sorted(r_)):.2f} / {max(r_):.2f} ({len(r_)} cells)"
                for (s_, t_), r_ in sorted(ratios.items())) + ".",
            "| cell | tier | target kbit/s | ratio | ours kbit/s | reference kbit/s | feed | measured |"
            + (f" ratio on {pmd5} |" if prev else ""),
            "|---|---|---|---|---|---|---|---|" + ("---|" if prev else "")]
    if missing:
        out += ["", "Not measured: " + ", ".join(missing) + "."]
    return head + out


# The arm of the ladder store that holds the final binary's curves. A cell
# with fewer than four of its rungs is "pending on the final binary"; curves
# from earlier builds are history and never counted.
LAD_FINAL = os.environ.get("LF_LAD_FINAL_ARM", "final")
# Arms of an earlier binary whose CRF coding the final binary shares (the
# timing lane sets this after comparing one-thread CRF streams of the two
# binaries): their CRF curves count where the final binary has none. VBR
# curves of these arms are history. The "ours" arm is tried after them and
# only when crf_same_as_ours() holds.
LAD_CRF_SAME = [a for a in os.environ.get("LF_LAD_CRF_SAME", "").split(",") if a and a != LAD_FINAL]
TRACK_BAR = 10.0


def _lad_pair(o, x):
    try:
        return (_lad_bd([v["kbps"] for v in x], [v["neg"] for v in x],
                        [v["kbps"] for v in o], [v["neg"] for v in o]),
                _lad_bd([v["kbps"] for v in x], [v["psnr"] for v in x],
                        [v["kbps"] for v in o], [v["psnr"] for v in o]))
    except Exception:
        return None, None


def _lad_rows(lc, mode, box_first=False):
    """{arm: {stride: {rung: row}}} for one ladder cell and mode. The ladder
    lane keeps a cell's rows under <cell> and <cell>@s<N>; a row's own "sub"
    is the stride it was scored at (the base name can hold rows of more than
    one stride), and the newest row of an arm, stride and rung wins.

    Rows measured on a cloud box (provenance "where": cloud, merged under
    cloud-<arm>) go three ways by their frames verdict. "identical" frames:
    the row joins the arm's own pool, marked "_cloud", and fills only rungs
    this machine has not measured (a box is seeded with this machine's rungs
    and runs the missing ones, so one curve can be part cloud). A
    "cloud-only (...)" verdict (the box coded its own decode of the title):
    the row stays under cloud-<arm> and is never mixed. Anything else
    (another downscale): dropped."""
    out = {}
    alias = []
    if mode == "crf":
        alias = list(LAD_CRF_SAME) + (["ours"] if crf_same_as_ours()[0] else [])
    for k, v in _lad().items():
        p = k.split("|")
        if len(p) != 6 or p[1] != mode:
            continue
        name, _, sfx = p[0].partition("@s")
        if name != lc:
            continue
        arm, pv = p[2], v.get("prov") or {}
        cloud = pv.get("where") == "cloud" or arm.startswith("cloud-")
        if cloud:
            arm = arm[6:] if arm.startswith("cloud-") else arm
            verdict = pv.get("frames_vs_mac") or ""
            if verdict.startswith("cloud-only"):
                arm = "cloud-" + arm
            elif verdict != "identical":
                continue
        # At CRF the final binary and the arms it shares its CRF coding with
        # are one pool (same bytes): its own row first, then theirs.
        rank, src_arm = 0, arm
        if mode == "crf" and alias:
            pre = "cloud-" if arm.startswith("cloud-") else ""
            base, la = (arm[len(pre):].split("-la") + [""])[:2]
            if base in alias and not arm.endswith("-cmp"):
                rank = 1 + alias.index(base)
                arm = pre + LAD_FINAL + (f"-la{la}" if la else "")
        sub = v.get("sub") or (int(sfx) if sfx.isdigit() else 1)
        d = out.setdefault(arm, {}).setdefault(sub, {})
        row = dict(v, _build=p[5], _th=p[4], _cloud=cloud, _rank=rank, _arm=src_arm)
        old = d.get(p[3])
        # box_first: the box's row where both exist (a box's own reading of
        # a ladder, for the variation note); otherwise this machine's
        new_key = (cloud != box_first, rank)
        if old is None or new_key < old["_key"] or (
                new_key == old["_key"] and v.get("date", "") >= old.get("date", "")):
            d[p[3]] = dict(row, _key=new_key)
    return out


def _cloud_ok(rows):
    """How a cloud curve's source frames relate to this machine's: "identical"
    (checked frame for frame), a "cloud-only (...)" verdict when the box coded
    its own decode of the title and both arms saw those frames, else None
    (another downscale, or a mix: the curve does not count)."""
    v = {(r.get("prov") or {}).get("frames_vs_mac") or "" for r in rows}
    if v == {"identical"}:
        return "identical"
    if len(v) == 1 and next(iter(v)).startswith("cloud-only"):
        return next(iter(v))
    return None


def _curve(arms, mode, tier, arm, ref, stride, src):
    """One arm's curve against one reference arm, both scored at `stride`,
    or None (fewer than four shared rungs, or cloud rows on other frames)."""
    o_, x_ = arms.get(arm, {}).get(stride, {}), arms.get(ref, {}).get(stride, {})
    rungs = sorted(set(o_) & set(x_), key=lambda q: float(q.lstrip("k")))
    if len(rungs) < 4:
        return None
    o, x = [o_[q] for q in rungs], [x_[q] for q in rungs]
    frames = _cloud_ok(o + x) if src.startswith("cloud") else "identical"
    if frames is None:
        return None
    bn, bp = _lad_pair(o, x)
    if bn is None:
        return None
    c = {"arm": arm, "build": o[0]["_build"], "rungs": rungs, "n": len(rungs),
         "md5": (o[0].get("bin_md5") or "")[:8], "sub": stride, "th": o[0]["_th"],
         "bn": bn, "bp": bp, "src": src, "frames": frames,
         "ncloud": sum(1 for r in o + x if r.get("_cloud")), "nrows": len(o + x),
         "label": next((r["prov"].get("label") for r in o + x
                        if (r.get("prov") or {}).get("label")), ""),
         "from": sorted({r["_arm"] for r in o if r.get("_rank")}),
         "itype": (o[0].get("prov") or {}).get("itype", ""),
         "prov": stride == 5 and tier == "L"}
    if mode == "vbr":
        c["track_o"] = [100 * (v["kbps"] / float(q[1:]) - 1) for v, q in zip(o, rungs)]
        c["track_x"] = [100 * (v["kbps"] / float(q[1:]) - 1) for v, q in zip(x, rungs)]
    return c


_CRF_SAME = None


def crf_same_as_ours():
    """(holds, shared rungs, byte-identical rungs, worst size gap %): does the
    final binary code CRF like the build of the store's "ours" arm? Judged on
    every CRF rung the store holds for both arms. Encodes at 6 threads vary
    from run to run by a few thousandths of a percent, so "holds" allows
    0.01% in size; the counts are printed with the claim."""
    global _CRF_SAME
    if _CRF_SAME is None:
        by = {}
        for k, v in _lad().items():
            p = k.split("|")
            if len(p) == 6 and p[1] == "crf" and p[2] in ["ours", LAD_FINAL] + LAD_CRF_SAME:
                by.setdefault((p[0].split("@")[0], p[3]), {}).setdefault(
                    "ours" if p[2] == "ours" else "fin", set()).add(v.get("bytes"))
        n = same = 0
        worst = 0.0
        for arms in by.values():
            if len(arms) == 2:
                n += 1
                o, f = arms["ours"], arms["fin"]
                if o & f:
                    same += 1
                else:
                    worst = max(worst, min(abs(x / y - 1) * 100 for x in f for y in o if x and y))
        _CRF_SAME = (n >= 5 and worst <= 0.01, n, same, worst)
    return _CRF_SAME


def lad_curve(cell, mode="crf", tier="L"):
    """The ladder store's verdict for one cell and mode ("crf" or "vbr"), or
    None when it holds nothing for it. "state" is "final" (a curve of the
    final binary: the only kind that counts), "pending" or "legacy" (only
    even-stride scores, void). Among the final binary's curves the one scored
    at the lowest odd stride wins, both arms at that stride: every frame
    beats 3 beats 5, and a tier L curve at stride 5 is provisional. At one
    stride: this machine's rows, then cloud rows (cloud against cloud, frames
    identical to this machine's), then, for CRF only, the "ours" arm when
    crf_same_as_ours() holds. "hist" is an earlier build's VBR curve."""
    lc = cell.replace("-x10c_", "-x10_")
    arms = _lad_rows(lc, mode)
    if not arms:
        return None
    same = crf_same_as_ours() if mode == "crf" else (False, 0, 0, 0.0)

    def curve(arm, ref, stride, src):
        return _curve(arms, mode, tier, arm, ref, stride, src)

    strides = sorted({s_ for a in arms.values() for s_ in a})
    # this machine's pool first (with cloud rows on identical frames filling
    # its gaps); a curve on a box's own frames only when that finds nothing
    pool = [(LAD_FINAL, "x264", "local")]
    own = [("cloud-" + LAD_FINAL, "cloud-x264", "cloud")]

    def first(cands):
        for st in [s_ for s_ in strides if s_ % 2]:
            for arm, ref, src in cands:
                c = curve(arm, ref, st, src)
                if c:
                    return c
        return None

    fin, second = first(pool), first(own)
    if fin is None:
        fin, second = second, None
    hist = None
    if mode == "vbr" or not same[0]:
        for st in [s_ for s_ in strides if s_ % 2]:
            for arm in (LAD_CRF_SAME if mode == "vbr" else []) + ["int", "ours"]:
                hist = hist or curve(arm, "x264", st, "local")
    part = max((len(r) for a in (LAD_FINAL, "cloud-" + LAD_FINAL) for r in arms.get(a, {}).values()),
               default=0)
    if fin:
        return {"state": "final", "final": fin, "hist": hist, "part": part, "second": second}
    if not any(s_ % 2 for s_ in strides):
        return {"state": "legacy", "stride": strides[0], "final": None, "hist": None, "part": 0}
    return {"state": "pending", "final": None, "hist": hist, "part": part}


_CSTAT = None


def cloud_state(lc, mode, arm):
    """The ladder lane's state for one cloud ladder ("complete", "running",
    "not completed", "held", "dropped") from cloud_status.json beside the
    ladder store, or "" when it has no entry."""
    global _CSTAT
    if _CSTAT is None:
        try:
            with open(os.path.join(os.path.dirname(LAD_JSON), "cloud_status.json")) as f:
                _CSTAT = json.load(f)
        except (OSError, ValueError):
            _CSTAT = {}
    e = _CSTAT.get(f"{lc}|{mode}|cloud-{arm}")
    return (e.get("state") or "") if isinstance(e, dict) else ""


def lad_sweep(lc, mode, tier):
    """{depth: curve} for one ladder cell: the final binary at lookahead 30,
    40 (its default: the counted curve's arm) and 50 against the reference
    at its default depth, each at the lowest odd stride it was scored at."""
    arms = _lad_rows(lc, mode)
    out = {}
    for d in (30, 40, 50):
        sfx = "" if d == 40 else f"-la{d}"
        cands = [(LAD_FINAL + sfx, "x264", "local"), ("cloud-" + LAD_FINAL + sfx, "cloud-x264", "cloud")]
        for st in sorted({s_ for a in arms.values() for s_ in a if s_ % 2}):
            for arm, ref, src in cands:
                out[d] = out.get(d) or _curve(arms, mode, tier, arm, ref, st, src)
    return out


# The cells the lookahead sweep is expected on ("cell:mode", ladder names);
# one with no row yet prints as pending.
SWEEP_EXPECT = [x for x in os.environ.get(
    "LF_SWEEP_EXPECT",
    "bbb-x10_1080p:vbr,tears-x10_1080p:vbr,meridian-x10_1080p:vbr,sintel-x10_720p:vbr,"
    "sollevante_1080p:vbr,sintel-x10_2160p:vbr,tears-x10_2160p:vbr,bbb-x10_2160p:vbr,"
    "sollevante_2160p:vbr,meridian_2160p:vbr,chimera_2160p:vbr").split(",") if x]


# LF_SWEEP_CLOSED=1 once the sweep queues have ended (the cloud boxes are cut
# off 45 minutes after their main queue): a depth with no curve then reads
# "not completed", not "pending".
SWEEP_CLOSED = os.environ.get("LF_SWEEP_CLOSED") == "1"
SWEEP_SPREAD = 0.4      # points: the run-to-run spread seen on one threaded VBR ladder


def sweep_lines():
    """The lookahead-depth section: every cell-mode with a swept curve in the
    ladder store, plus the expected ones still to come."""
    if not LAD_JSON:
        return []
    have = set()
    for k in _lad():
        p = k.split("|")
        if len(p) == 6 and "-la" in p[2]:
            a = p[2].replace("cloud-", "").split("-la")[0]
            if a == LAD_FINAL or (p[1] == "crf" and a in LAD_CRF_SAME):
                have.add((p[0].split("@")[0], p[1]))
    for x in SWEEP_EXPECT:
        have.add(tuple(x.split(":")))
    tiers = {}
    for tier, cell, title, size in spec_cells():
        tiers[cell] = (tier, f"{title}_{size} ({tier})")
    rows = []
    for lc, mode in sorted(have, key=lambda x: (x[1], x[0])):
        tier, name = tiers.get(lc, ("L", lc))
        sw = lad_sweep(lc, mode, tier)
        cells_ = []
        for d in (30, 40, 50):
            c = sw.get(d)
            if not c:
                st_ = cloud_state(lc, mode, LAD_FINAL + ("" if d == 40 else f"-la{d}"))
                cells_.append("not completed" if SWEEP_CLOSED or st_ in ("not completed", "dropped")
                              else "running" if st_ in ("running", "held") else "pending")
                continue
            tag = ("" if not c["ncloud"] else " (cloud)" if c["ncloud"] == c["nrows"] else " (part cloud)")
            cells_.append(f"{c['bn']:+.2f}%" + tag
                          + (f" (s{c['sub']})" if c["sub"] != 1 else ""))
        got = {d: c["bn"] for d, c in sw.items() if c}
        best = "-"
        if len(got) == 3:
            best = (f"{min(got, key=got.get)}" if max(got.values()) - min(got.values()) > SWEEP_SPREAD
                    else "within spread")
        rows.append(f"| {name} | {mode.upper()} | {cells_[0]} | {cells_[1]} | {cells_[2]} | {best} |")
    return ["", "## Lookahead depth sweep (BD-VMAF-NEG against the reference)",
            "Ours at --rc-lookahead 30, 40 (its default, the depth every other figure on this "
            "page uses) and 50; the reference at its default depth of 40 in every column (its "
            "depth is not swept). Same rungs, 6 threads, final binary; (sN) = VMAF-NEG scored "
            "every Nth frame, no mark = every frame. \"best\" names the depth with the lowest BD "
            f"only when the three differ by more than {SWEEP_SPREAD:g} points, the run-to-run "
            "spread of one threaded VBR ladder (see the variation note above). CRF rows come "
            "from the binary the CRF curves above name (same CRF coding).",
            "| cell | mode | depth 30 | depth 40 | depth 50 | best |", "|---|---|---|---|---|---|"] + rows


def cloud_wall_lines():
    """The supplementary wall-ratio table measured on a rented Linux arm64
    instance (LF_CLOUD_WALL_JSONL: one JSON object per leg; the newest leg
    of a cell, goal and thread setting wins; LF_CLOUD_BUILD_INFO: the
    box's build record). Never part of the parity table."""
    path = os.environ.get("LF_CLOUD_WALL_JSONL", "")
    legs = {}
    try:
        with open(path) as f:
            for ln in f:
                if ln.strip():
                    v = json.loads(ln)
                    legs[v["label"]] = v
    except (OSError, ValueError, KeyError):
        return []
    if not legs:
        return []
    info = {}
    try:
        with open(os.environ.get("LF_CLOUD_BUILD_INFO", "")) as f:
            info = dict(ln.rstrip("\n").split("=", 1) for ln in f if "=" in ln)
    except OSError:
        pass
    # The legs' own feeder-alone pass is the first read of a staged file from
    # the volume; LF_CLOUD_FEED_WARM ({cell: fps}, measured after the legs with
    # the file cached, as the encoders read it) replaces it for the 1.5x rule.
    try:
        with open(os.environ.get("LF_CLOUD_FEED_WARM", "")) as f:
            warm = json.load(f)
    except (OSError, ValueError):
        warm = {}
    for v in legs.values():
        if v["cell"] in warm:
            fast = v["frames"] / min(v["ours_wall"], v["x264_wall"])
            v["feed_x"] = round(warm[v["cell"]] / fast, 2)
            v["feeder_bound"] = warm[v["cell"]] < FEED_VALID * fast
    gname = {"g1": "G1", "g2": "G2", "g3": "G3"}
    out = ["", "## Linux arm64 wall ratios (cloud, supplementary)",
           "Measured on a rented VM (AWS c8g.24xlarge, Graviton4, 96 vCPU), not on the machine "
           "of the parity table above; nothing here changes that table. Same method: both "
           "encoders read the same frames on stdin from one feeder, rounds interleaved, median "
           "wall, windows summed. The rate factors and windows are the ones solved on the parity "
           "machine; the size match shown is what they gave on this box, and a leg over 1% is "
           "void. G1 = pure C at one thread, G2 = pure C at each encoder's default threads, "
           "G3 = SIMD at default threads; \"threads\" is what each process was seen running "
           "(ours / reference), \"cores\" its CPU time over wall. Source frames are "
           "software-decoded here and were checked identical to the parity machine's.",
           "Builds: ours " + info.get("yah264_commit", "?").split(" md5")[0]
           + " (clang " + info.get("gcc", "").split("clang=")[-1].split()[0] + ", release -O3; "
           "pure C = the same binary with its SIMD switched off, as on the parity machine); "
           "reference " + info.get("x264", "?").split(" md5")[0] + ": for G3 built with the same "
           "clang; for G1/G2 the same commit without assembly, 8-bit, the compiler free to "
           "vectorise its C, built with gcc 13.3. The clang build of that pure-C reference is "
           "the guard, not the table's: its one-thread stream differs from the gcc build's and "
           "from the parity machine's (0.025% in size on the check clip), while the gcc build's "
           "is byte-identical to both.",
           "| cell | goal | threads seen | cores used | ours wall | reference wall | ratio | size match | "
           "box CPU at start | feed | parity machine's ratio |",
           "|---|---|---|---|---|---|---|---|---|---|---|"]
    order = {"g3": 0, "g2": 1, "g1": 2}
    ok = {}
    # the guard: the reference built with the other compiler, on a few cells
    is_g = lambda k: k.endswith(":gcc") or k.endswith(":clang")          # noqa: E731
    guard, glines, worse = {k: v for k, v in legs.items() if is_g(k)}, [], []
    gains = []
    legs = {k: v for k, v in legs.items() if not is_g(k)}
    for lab, gv in sorted(guard.items()):
        pv = legs.get(lab.rsplit(":", 1)[0])
        if not pv:
            continue
        gain = 100 * (pv["x264_wall"] / gv["x264_wall"] - 1)        # > 0: the other build of the reference is faster
        best = min(pv["x264_wall"], gv["x264_wall"])
        name = (pv["cell"][2:] if pv["cell"].startswith("s-") else pv["cell"]).replace("-x10c", "")
        glines.append(f"| {name} | {gname[pv['goal']]} | {pv.get('ref_cc', '?')}: {pv['x264_wall']:.1f} s | "
                      f"{gv.get('ref_cc', '?')}: {gv['x264_wall']:.1f} s | "
                      f"{(gv if gain > 0 else pv).get('ref_cc', '?')} ({abs(gain):.1f}%) | {pv['ratio']:.2f} | "
                      f"{pv['ours_wall'] / best:.2f} |")
        if gain > 3.0:
            gains.append(gain)
            worse.append(f"{name} {gname[pv['goal']]} (the {gv.get('ref_cc')} build is {gain:.1f}% faster; ratio "
                         f"{pv['ours_wall'] / best:.2f} against it, {pv['ratio']:.2f} in the table)")
    if guard:
        out.insert(2, ("**On these cells the reference's other build is more than 3% faster than the one in "
                       "the table: " + "; ".join(worse) + ". Read those ratios against the faster build. "
                       "Every other cell of the same goals was timed only against the slower build of "
                       "the reference, so its ratio in the table flatters us by a similar margin: the "
                       f"gap seen is {min(gains):.0f}% to {max(gains):.0f}% of the reference's wall.**")
                   if worse else
                   "The reference was timed from both compilers on the cells listed under the table; the "
                   "build not used in the table is not more than 3% faster on any of them.")
    for lab, v in sorted(legs.items(), key=lambda kv: (order.get(kv[1]["goal"], 9), "-t" in kv[0], kv[0])):
        flags = []
        if v.get("void_size"):
            flags.append("VOID: size over 1%")
        if v.get("feeder_bound"):
            flags.append("VOID: feeder-bound")
        if v.get("quiet") is False:
            flags.append("VOID: box not quiet")
        ta = v["threads_arg"]
        goal = gname[v["goal"]] + (f" at {ta['ours']} threads both" if "-t" in lab else "")
        out.append(f"| {(v['cell'][2:] if v['cell'].startswith('s-') else v['cell']).replace('-x10c', '')} | {goal} | "
                   f"{v['threads_seen']['ours']} / {v['threads_seen']['x264']} | "
                   f"{v['cores_used']['ours']} / {v['cores_used']['x264']} | {v['ours_wall']:.1f} s | "
                   f"{v['x264_wall']:.1f} s | {v['ratio']:.2f}" + (" (" + "; ".join(flags) + ")" if flags else "")
                   + f" | {v['dsize_pct']:+.2f}% | {v['box_cpu_start']}% | {v['feed_x']:.1f}x | "
                   + (f"{v['mac_ratio']:.2f}" if "-t" not in lab else f"{v['mac_ratio']:.2f} (at its default threads)") + " |")
        if not flags and "-t" not in lab:
            ok.setdefault(v["goal"], []).append((v["ratio"], v["mac_ratio"]))
    for g in ("g1", "g2", "g3"):
        if g in ok:
            r = sorted(x for x, _ in ok[g])
            m = sorted(y for _, y in ok[g])
            out.append(f"{gname[g]}: median {med(r):.2f}, worst {r[-1]:.2f} over {len(r)} valid cells here; "
                       f"the same cells on the parity machine: median {med(m):.2f}, worst {m[-1]:.2f}.")
    if glines:
        out += ["", "Reference compiler guard (the same legs with the reference built by the other compiler):",
                "| cell | goal | reference wall, table's build | reference wall, other build | faster build | "
                "ratio in the table | ratio against the faster build |", "|---|---|---|---|---|---|---|"] + glines
    g1 = [v for v in legs.values() if v["goal"] == "g1" and v.get("md5_ours_is_mac") is not None]
    if g1:
        out.append(f"One-thread streams: ours byte-identical to the parity machine's in "
                   f"{sum(1 for v in g1 if v['md5_ours_is_mac'])} of {len(g1)} G1 legs, the reference's in "
                   f"{sum(1 for v in g1 if v['md5_x264_is_mac'])} of {len(g1)}.")
    return out


GATING_TIERS = ("S", "M", "L")      # tier C speed is shown and does not gate (PARITY-SPEC)


def gate_state():
    """What the full-parity line needs beyond the speed table: the ladder
    curves (BD and VBR tracking), the VBR wall cells and the lookahead
    sweep, each as lists of the cell-modes missing or failing."""
    g = {"bd_total": 0, "bd_landed": 0, "bd_fail": [], "bd_missing": [], "track_fail": [],
         "vbr_total": 0, "vbr_missing": [], "vbr_fail": [], "sweep_pending": [], "has_lad": bool(LAD_JSON),
         "has_vbr": bool(VBR_JSONL)}
    if LAD_JSON:
        for tier, cell, title, size in spec_cells():
            if tier not in ("M", "L"):
                continue
            c = f"{cell.rsplit('_', 1)[0]}c_{size}" if cell.rsplit("_", 1)[0] in CREDITS_LAST else cell
            for mode in ("crf", "vbr"):
                name = f"{title}_{size} ({tier}) {mode.upper()}"
                g["bd_total"] += 1
                cv = lad_curve(c, mode, tier)
                f = cv and cv.get("final")
                if not f:
                    g["bd_missing"].append(name)
                    continue
                g["bd_landed"] += 1
                if f["bn"] > 1.0:
                    g["bd_fail"].append(f"{name} {f['bn']:+.2f}%")
                if mode == "vbr" and any(abs(e) > TRACK_BAR for e in f["track_o"]):
                    g["track_fail"].append(name)
        tiers = {cell: f"{title}_{size} ({tier})" for tier, cell, title, size in spec_cells()}
        have = set(tuple(x.split(":")) for x in SWEEP_EXPECT)
        for k in _lad():
            p = k.split("|")
            if len(p) == 6 and "-la" in p[2]:
                have.add((p[0].split("@")[0], p[1]))
        for lc, mode in sorted(have, key=lambda x: (x[1], x[0])):
            if lc not in tiers:
                continue
            sw = lad_sweep(lc, mode, tiers[lc][-2])
            miss = [str(d) for d in (30, 40, 50) if not sw.get(d)]
            if miss:
                g["sweep_pending"].append(f"{tiers[lc]} {mode.upper()} depth {'/'.join(miss)}")
    if VBR_JSONL:
        legs = {}
        try:
            with open(VBR_JSONL) as f:
                for ln in f:
                    if ln.strip():
                        v = json.loads(ln)
                        legs[v["cell"].replace("-x10c_", "-x10_")] = v
        except (OSError, ValueError, KeyError):
            legs = {}
        by = {}
        for tier, cell, title, size in spec_cells():
            if tier not in ("M", "L"):
                continue
            g["vbr_total"] += 1
            v = legs.get(cell)
            if v is None:
                g["vbr_missing"].append(f"{title}_{size} ({tier})")
            elif v.get("feeder_bound"):
                g["vbr_missing"].append(f"{title}_{size} ({tier}) (feeder-bound)")
            else:
                by.setdefault((size, tier), []).append(v["ratio"])
                if abs(100 * (v["ours_kbps"] / v["k"] - 1)) > TRACK_BAR:
                    g["track_fail"].append(f"{title}_{size} ({tier}) VBR wall leg")
        for (size, tier), r in sorted(by.items()):
            if med(sorted(r)) > MED_BAR or max(r) > WORST_BAR:
                g["vbr_fail"].append(f"{size}-{tier} {med(sorted(r)):.2f} / {max(r):.2f}")
    return g


def parity_lines(combos, qok, g):
    """The full-parity lines of the verdict. `combos` is [(class, tier, goal,
    state)] with state pass / open / void / none; only tiers S, M and L gate.
    Full parity is "yes" only when every gating combination is measured and
    passing AND every gated ladder curve has landed inside the BD bar, every
    VBR rung tracks, every VBR wall cell is measured inside the speed bar,
    and the lookahead sweep has landed at all three depths on every swept
    cell. Tier C speed is named on its own line, never hidden and never
    counted; the unfinished sweep cells are named too."""
    gate = [x for x in combos if x[1] in GATING_TIERS]
    measured = [x for x in gate if x[3] in ("pass", "open")]
    passing = [x for x in measured if x[3] == "pass" and qok.get((x[0], x[1]))]
    unmeasured = [f"{c}-{t} {gl}" + (" (void)" if st == "void" else "")
                  for c, t, gl, st in gate if st in ("none", "void")]
    why = []
    if len(passing) < len(measured):
        why.append(f"{len(measured) - len(passing)} speed combination(s) open")
    if unmeasured:
        why.append(f"{len(unmeasured)} speed combination(s) not measured")
    if g.get("has_lad"):
        if g["bd_missing"]:
            why.append(f"{len(g['bd_missing'])} ladder curve(s) missing")
        if g["bd_fail"]:
            why.append(f"{len(g['bd_fail'])} ladder curve(s) over the BD bar")
        if g["track_fail"]:
            why.append(f"{len(g['track_fail'])} VBR cell(s) outside +-{TRACK_BAR:g}% tracking")
    else:
        why.append("no ladder store read")
    if g.get("has_vbr"):
        if g["vbr_missing"]:
            why.append(f"{len(g['vbr_missing'])} VBR wall cell(s) missing")
        if g["vbr_fail"]:
            why.append(f"{len(g['vbr_fail'])} VBR wall class(es) over the speed bar")
    else:
        why.append("no VBR wall legs read")
    if g.get("sweep_pending"):
        # the bar holds "with the depth swept rather than inherited": a cell
        # is swept when its ladder has landed at depths 30, 40 and 50
        why.append(f"lookahead sweep unfinished on {len(g['sweep_pending'])} cell-mode(s)")
    out = [f"Full parity:           {'no: ' + '; '.join(why) if why else 'yes'}. "
           f"Speed: {len(passing)} of {len(gate)} gating combinations pass (class, tier S/M/L and goal; "
           f"pass = median <= {MED_BAR:.2f}, worst <= {WORST_BAR:.2f} and quality holding). "
           + (f"Ladders: {g['bd_landed'] - len(g['bd_fail'])} of {g['bd_total']} curves landed inside "
              f"the BD bar. " if g.get("has_lad") else "")
           + (f"VBR wall: {g['vbr_total'] - len(g['vbr_missing'])} of {g['vbr_total']} cells measured."
              if g.get("has_vbr") else "")]
    if unmeasured:
        out.append(f"Not measured (gating): {', '.join(unmeasured)}")
    for key, lab in (("bd_missing", "Ladders missing:      "), ("bd_fail", "Ladders over the bar: "),
                     ("track_fail", "Tracking outside bar: "), ("vbr_missing", "VBR wall missing:     "),
                     ("vbr_fail", "VBR wall over the bar:")):
        if g.get(key):
            out.append(f"{lab} {', '.join(g[key])}")
    tc = [x for x in combos if x[1] not in GATING_TIERS]
    if tc:
        okc = [f"{c}-{t} {gl}" for c, t, gl, st in tc if st == "pass"]
        opc = [f"{c}-{t} {gl}" for c, t, gl, st in tc if st == "open"]
        gap = [f"{c}-{t} {gl}" + (" (void)" if st == "void" else " (not measured)")
               for c, t, gl, st in tc if st in ("none", "void")]
        out.append("Tier C speed, not gating: "
                   + "; ".join(p_ for p_ in (
                       ("inside the bar: " + ", ".join(okc)) if okc else "",
                       ("over the bar: " + ", ".join(opc)) if opc else "",
                       ("not measured / void: " + ", ".join(gap)) if gap else "") if p_))
    if g.get("sweep_pending"):
        out.append("Sweep incomplete:      " + ", ".join(g["sweep_pending"]))
    return out


def rests_on_lines(g):
    """A short note under the verdict: what the answer rests on."""
    if not g.get("has_lad"):
        return []
    crf_other, cloud, own = 0, [], []
    for tier, cell, title, size in spec_cells():
        if tier not in ("M", "L"):
            continue
        c = f"{cell.rsplit('_', 1)[0]}c_{size}" if cell.rsplit("_", 1)[0] in CREDITS_LAST else cell
        for mode in ("crf", "vbr"):
            f = (lad_curve(c, mode, tier) or {}).get("final")
            if not f:
                continue
            name = f"{title}_{size} {mode.upper()}"
            if mode == "crf" and f.get("from"):
                crf_other += 1
            if f["src"] == "cloud":
                own.append(name)
            elif f["ncloud"]:
                cloud.append(name)
    out = ["What this rests on: "
           + os.environ.get("LF_REPORT_RESTS", "").strip()
           + (f" {crf_other} of the CRF ladder curves use rows of earlier binaries with the same "
              "CRF coding (see the BD section)." if crf_other else "")
           + (f" {len(cloud) + len(own)} ladder curves have rungs measured on rented cloud "
              "instances" + (f"; {len(own)} of them on a box's own decode of the title, not this "
                             f"machine's frames ({', '.join(own)})" if own else "") + "."
              if cloud or own else "")
           + " Tier C is outside the count: its speed is shown and does not gate."]
    return out


def selftest_verdict():
    """A check of the full-parity lines: tier C never gates and stays
    visible; a missing ladder, a failed tracking cell or a missing VBR wall
    cell each turn the answer to no; an unfinished sweep is its own line."""
    full = {"bd_total": 2, "bd_landed": 2, "bd_fail": [], "bd_missing": [], "track_fail": [],
            "vbr_total": 1, "vbr_missing": [], "vbr_fail": [], "sweep_pending": [], "has_lad": True,
            "has_vbr": True}
    qok = {("1080p", "S"): True, ("2160p", "C"): True}
    combos = [("1080p", "S", g_, "pass") for g_ in GOALS] + [
        ("2160p", "C", "G1", "none"), ("2160p", "C", "G2", "none"), ("2160p", "C", "G3", "void")]
    out = parity_lines(combos, qok, full)
    assert out[0].startswith("Full parity:           yes"), out[0]
    assert "3 of 3 gating combinations" in out[0], out[0]
    assert any(ln.startswith("Tier C speed, not gating") and "2160p-C G3 (void)" in ln
               and "2160p-C G1 (not measured)" in ln for ln in out), out
    assert not any(ln.startswith("Not measured (gating)") for ln in out), out
    for key, val in (("bd_missing", ["x CRF"]), ("bd_fail", ["x VBR +1.01%"]), ("track_fail", ["x VBR"]),
                     ("vbr_missing", ["x"]), ("vbr_fail", ["1080p-L 1.02 / 1.20"])):
        o2 = parity_lines(combos, qok, dict(full, **{key: val}))
        assert o2[0].startswith("Full parity:           no"), (key, o2[0])
    o3 = parity_lines(combos, qok, dict(full, sweep_pending=["x VBR depth 30/50"]))
    assert o3[0].startswith("Full parity:           no") and "sweep unfinished on 1" in o3[0], o3[0]
    assert o3[-1].startswith("Sweep incomplete") and "x VBR depth 30/50" in o3[-1], o3
    o4 = parity_lines(combos[:2] + [("1080p", "S", "G3", "none")] + combos[3:], qok, full)
    assert o4[0].startswith("Full parity:           no") and "Not measured (gating): 1080p-S G3" in o4[1], o4
    o5 = parity_lines(combos, {("1080p", "S"): False}, full)
    assert o5[0].startswith("Full parity:           no"), o5[0]
    print("selftest-verdict: ok")


def credits_lines():
    """The supplementary end-credits table, from the ladder lane's own rows
    file (LF_CRED_ROWS): its header lines and table are passed through, with
    a reading per title. LF_CRED_READINGS ("title=reading,...") overrides
    the rule-made reading."""
    path = os.environ.get("LF_CRED_ROWS", "")
    try:
        with open(path) as f:
            lines = [ln.rstrip("\n") for ln in f]
    except OSError:
        return []
    over = dict(x.split("=", 1) for x in os.environ.get("LF_CRED_READINGS", "").split(",") if "=" in x)
    head = [ln for ln in lines if ln and not ln.startswith("|") and "RESULT" not in ln]
    tab = [ln for ln in lines if ln.startswith("|")]
    if len(tab) < 2:
        return []
    out = ["", "## End credits (supplementary rows, not a gate)",
           "Every cell above stops at its last story frame. The credits that follow are "
           "measured on their own so a weakness there stays visible: one matched point per "
           "title, PSNR-Y only (VMAF-NEG on credits weighs frames no viewer does). \"extr\" = "
           "the figure is extrapolated beyond the measured points. " + " ".join(head),
           tab[0] + " reading |", "|" + "---|" * (tab[0].count("|"))]
    for ln in tab[1:]:
        c = [x.strip() for x in ln.strip("|").split("|")]
        if len(c) < 6 or set(c[0]) <= set("-: "):
            continue
        try:
            a, b = float(c[4].split("%")[0]), float(c[5].split("%")[0])
        except ValueError:
            a = b = None
        if c[0] in over:
            rd = over[c[0]]
        elif a is None:
            rd = "-"
        elif (a > 0) != (b > 0) and "extr" in c[4] and "extr" in c[5]:
            rd = "no overlap; figure extrapolated"
        elif a > 1.0 or b > 1.0:
            rd = "**behind**"
        else:
            rd = "ahead"
        out.append(ln.rstrip() + f" {rd} |")
    return out


def gate_note_lines():
    """The run-to-run variation note for threaded capped VBR, from the cloud
    gate's record (LF_GATE_NOTE, JSON)."""
    try:
        with open(os.environ.get("LF_GATE_NOTE", "")) as f:
            g = json.load(f)
    except (OSError, ValueError):
        return []
    runs = next((v for k, v in g.items() if k.endswith("_bd_vmaf_neg") and isinstance(v, dict)), None)
    if not runs:
        return []
    cell = next(k for k in g if k.endswith("_bd_vmaf_neg")).split("_vbr_")[0]
    vals = list(runs.values())
    return ["",
            "### Variation of a threaded capped-VBR ladder",
            f"One ladder ({cell} VBR, final binary), identical source frames and binary, read "
            + ", ".join(f"{v:+.2f}% on " + ("this machine" if k.startswith("Mac") else
                                               "cloud " + " ".join(k.split()[:2]))
                        for k, v in runs.items())
            + f": a spread of {max(vals) - min(vals):.2f} points from thread timing alone. "
            "Everything deterministic agreed exactly between the machines (one-thread streams "
            "md5-identical on three clips, reference rung sizes within 0.0022%, the two CRF "
            "check ladders chimera_1080p -10.80 against -10.80 and sollevante_1080p -3.94 "
            "against -3.94). So a VBR BD figure on this page carries about +-0.2 points of "
            "run-to-run spread whichever machine produced it, and a cloud VBR ladder counts as "
            "measured whatever it reads (owner, 2026-10-12)."]


def variation_extra():
    """More readings of ladders that were measured more than once: repeats on
    this machine (cells <cell>@r<N> in the ladder store) and reruns on a
    cloud box (arms <arm>-cmp against x264-cmp). Evidence of run-to-run and
    machine-to-machine spread; never table rows."""
    reps, cmps = {}, set()
    for k in _lad():
        p = k.split("|")
        if len(p) != 6:
            continue
        m = re.match(r"(.+)@r(\d+)$", p[0])
        if m:
            reps.setdefault((m.group(1), p[1]), set()).add(int(m.group(2)))
        if p[2].endswith("-cmp"):
            cmps.add((p[0].split("@")[0], p[1]))
    tiers = {}
    for tier, cell, title, size in spec_cells():
        tiers[cell] = (tier, f"{title}_{size} ({tier})")

    def one(arms, arm, ref, tier):
        for st in sorted({s_ for a in arms.values() for s_ in a if s_ % 2}):
            c = _curve(arms, "", tier, arm, ref, st, "local")
            if c:
                return c
        return None

    def where(c):
        return ("this machine" if not c["ncloud"] else
                "cloud" if c["ncloud"] == c["nrows"] else "part cloud")

    out = []
    for (cell, mode), ns in sorted(reps.items()):
        tier, name = tiers.get(cell, ("L", cell))
        main = (lad_curve(cell, mode, tier) or {}).get("final")
        got = [f"{main['bn']:+.2f}% (the counted curve, {where(main)})"] if main else []
        for n in sorted(ns):
            c = one(_lad_rows(f"{cell}@r{n}", mode), LAD_FINAL, "x264", tier)
            got.append(f"{c['bn']:+.2f}% (repeat {n})" if c else f"repeat {n} not landed")
        out.append(f"- {name} {mode.upper()}, the same ladder again on this machine: " + ", ".join(got) + ".")
    for tier, cell, title, size in spec_cells():
        if tier not in ("M", "L"):
            continue
        for mode in ("crf", "vbr"):
            main = (lad_curve(cell, mode, tier) or {}).get("final")
            if not main or main["src"] == "cloud" or not 0 < main["ncloud"] < main["nrows"]:
                continue
            c = one(_lad_rows(cell, mode, box_first=True), LAD_FINAL, "x264", tier)
            if c and c["ncloud"] == c["nrows"]:
                out.append(f"- {title}_{size} ({tier}) {mode.upper()} is part cloud at {main['bn']:+.2f}% "
                           f"(this machine's rungs where it has them); on the box's rungs alone "
                           f"it reads {c['bn']:+.2f}%.")
    for cell, mode in sorted(cmps):
        tier, name = tiers.get(cell, ("L", cell))
        arms = _lad_rows(cell, mode)
        main = (lad_curve(cell, mode, tier) or {}).get("final")
        c = None
        c = one(arms, LAD_FINAL + "-cmp", "x264-cmp", tier)
        if c and main:
            out.append(f"- {name} {mode.upper()}, same ladder, {where(main)} against a cloud box's "
                       f"own full run: {main['bn']:+.2f}% against {c['bn']:+.2f}%.")
        elif main:
            out.append(f"- {name} {mode.upper()}: a cloud box's own full run of this ladder has "
                       "not landed.")
    return out


def lad_bd(cell, mode="crf", tier="L"):
    """(BD-VMAF-NEG %, BD-PSNR-Y %, note) for one cell and mode from the final
    binary's curve in the ladder store; (None, None, note) while that curve is
    pending or only void even-stride scores exist; (None, None, None) with no
    ladder."""
    c = lad_curve(cell, mode, tier)
    if c is None:
        return None, None, None
    if c["state"] == "legacy":
        return None, None, (f"LEGACY {mode.upper()} ladder only (VMAF-NEG scored every "
                            f"{c['stride']} frames, an even stride: void, being rescored)")
    if c["state"] == "pending":
        return None, None, f"{mode.upper()} BD pending on the final binary"
    f = c["final"]
    err = ""
    if mode == "vbr":
        rng = lambda e: f"{min(e):+.1f}..{max(e):+.1f}%"          # noqa: E731
        err = f"; rate error ours {rng(f['track_o'])}, reference {rng(f['track_x'])}"
    return f["bn"], f["bp"], (
        f"{mode.upper()} BD from the ladder lane: {f['n']} rungs, {f['th']} both sides, "
        f"VMAF-NEG {lad_how(f)}" + err)


def lad_how(f):
    """How a counted curve was scored and where it came from, for the page."""
    how = "every frame" if f["sub"] == 1 else f"every {f['sub']} frames"
    if f["prov"]:
        how = "stride 5, provisional"
    if f["src"] == "cloud":
        how += "; cloud, on the box's own frames [*]"
    elif f["ncloud"]:
        how += "; cloud" if f["ncloud"] == f["nrows"] else "; part cloud"
    if f.get("from"):
        names = {"ours": "the earlier build d2fc019"}
        how += ("; rows of " + " and ".join(names.get(a_, f"arm {a_}") for a_ in f["from"])
                + ", same CRF coding")
    return how


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
            t = timed_rec(cell)[0]
            if not t:
                continue
            ow = med([x[0] for x in t["ours"]["default"]])
            xw = med([x[0] for x in t["x264"]["default"]])
            oc = med([x[1] for x in t["ours"]["default"]])
            xc = med([x[1] for x in t["x264"]["default"]])
            orss = max(x[2] for x in t["ours"]["default"]) / 2**30
            xrss = max(x[2] for x in t["x264"]["default"]) / 2**30
            if t["ours"]["t1"]:
                o1 = med([x[0] for x in t["ours"]["t1"]])
                x1 = med([x[0] for x in t["x264"]["t1"]])
            else:                               # full title: the sampled S1 group
                p = purec_rec(cell)[0] or {}
                s1 = p.get("S1") or {}
                o1 = sum(med([m[0] for m in ms]) for ms in s1.get("ours", [])) or None
                x1 = sum(med([m[0] for m in ms]) for ms in s1.get("x264", [])) or None
            bdr = bd_of(cell)[0]
            rd = t["read"][0]
            flag = " (feeder-bound?)" if min(ow, xw) < FEED_VALID * rd else ""
            ratios.append(ow / xw); dv.append(t["ours_neg"] - t["x264_neg"])
            ds.append(t["dsize_pct"])
            print(f"| {cell}{flag} | {t['frames']} | {t['ours_crf']:.3f} | "
                  f"{t['x264_crf']:.3f} | {t['dsize_pct']:+.2f}% | "
                  f"{t['frames']/ow:.1f} | {t['frames']/xw:.1f} | **{ow/xw:.2f}x** | "
                  f"{t['frames']/rd:.0f} | {oc:.0f} | {xc:.0f} | {orss:.2f} GiB | "
                  f"{xrss:.2f} GiB | {t['ours_neg']-t['x264_neg']:+.2f} | "
                  f"{t['ours_psnr_y']-t['x264_psnr_y']:+.2f} | "
                  + (f"{bdr:+.2f}% |" if bdr is not None else "n/a |")
                  + (f" {o1/x1:.2f}x{'' if t['ours']['t1'] else ' (sampled)'} |"
                     if o1 and x1 else " pending |"))
    bar_line(ratios, dv, ds)

    print(f"\n## G1 / G2 (and S1, SIMD one thread): stratified sample (one {WINDOW_SECS:g} s window per {STRATUM_SECS/60:g} min)\n")
    print("| cell | windows | sampled frames | G1 ours/x264 | G2 ours/x264 |"
          " G1 est. full-title wall ours / x264 | read fps |")
    print("|---|--:|--:|--:|--:|--:|--:|")
    for grp in (live, anim):
        if grp is anim and live and anim:
            print("| *animation* |" + " |" * 6)
        for cell in grp:
            p = purec_rec(cell)[0]
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
    v = validate_rec(VALIDATE_CELL)[0]
    p = purec_rec(VALIDATE_CELL)[0]
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


# ---------------------------------------------------------------------------
# The shared report (PARITY-SPEC section 5): one page and one CSV, the same
# layout for all three codecs.

REPORT_DATE = os.environ.get("LF_REPORT_DATE", time.strftime("%Y-%m-%d"))
REPORT_DIR = os.environ.get("LF_REPORT_DIR", os.path.join(_ROOT, "scratch", "parity"))
BOARD_LOGS = os.environ.get("LF_BOARD_LOGS", os.path.expanduser(
    "~/src/yah264-board4k/scratch/timed"))
BOARD_QUAL = os.environ.get("LF_BOARD_QUAL", os.path.expanduser(
    "~/.cache/yah264/ffboard/quality.json"))
CSV_COLS = ("codec,date,commit,ref,ref_version,tier,class,title,size,goal,"
            "ours_wall_s,ref_wall_s,ratio,feed_fps,ours_bytes,ref_bytes,dsize_pct,"
            "ours_vmafneg,ref_vmafneg,dvmafneg,ours_psnr_y,ref_psnr_y,dpsnr_y,"
            "bd_vmafneg_pct,ours_vmafneg_hmean,ref_vmafneg_hmean,ours_vmafneg_min,"
            "ref_vmafneg_min,ours_vmafneg_low1,ref_vmafneg_low1,"
            "ours_vmafneg_first1s,ref_vmafneg_first1s,ours_vmaf,ref_vmaf,"
            "ours_psnr_yuv611,ref_psnr_yuv611,ours_ssim_yuv611,ref_ssim_yuv611,"
            "frames_decoded_ok,rounds,valid,note").split(",")
# 5.2a columns: CSV name suffix -> record field
SUPP = (("vmafneg_hmean", "neg_hmean", 2), ("vmafneg_min", "neg_min", 2),
        ("vmafneg_low1", "neg_low1", 2), ("vmafneg_first1s", "neg_first1s", 2),
        ("vmaf", "vmaf", 2), ("psnr_yuv611", "psnr_yuv611", 2),
        ("ssim_yuv611", "ssim_yuv611", 4))
CLASS_OF = {"cif": "CIF", "720p": "720p", "1080p": "1080p", "2160p": "2160p"}
SPEC_ROWS = [("CIF", "S"), ("720p", "S"), ("720p", "L"), ("720p", "C"),
             ("1080p", "S"), ("1080p", "M"), ("1080p", "L"), ("1080p", "C"),
             ("2160p", "S"), ("2160p", "M"), ("2160p", "L"), ("2160p", "C")]
GOALS = ("G1", "G2", "G3")
# section 4.2 (spec update 2026-09-26): the report's validity line is the
# same margin the timing refusal uses, feed fps >= 1.5x the faster arm
FEED_VALID = FEED_MARGIN
# quality (spec update 2026-09-26): the MEDIAN dVMAF-NEG of a class and tier
# against the published -0.5 bar, a per-cell floor of -1.0 VMAF-NEG and
# -1.0 dB PSNR-Y, and every cell's size within 1%
SIZE_BAR, VMAF_BAR, MED_BAR, WORST_BAR = 1.0, -0.5, 1.00, 1.15
VMAF_FLOOR, PSNR_FLOOR = -1.0, -1.0
PSNR_BAR = PSNR_FLOOR
BOARD_ROW = re.compile(
    r"^\s{2}(\w+_(?:cif|720p|1080p|2160p))\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+"
    r"([\d.]+)\s+([\d.]+)\s+([\d.]+)x\s+([\d.]+)x\s+\S+\s+([+-][\d.]+)\s+"
    r"([+-][\d.]+)%\s+([+-][\d.]+)\s*$")
BOARD_QLINE = re.compile(r"qual cache: \w+\s+\[(\w+)\] (yah264|x264) (\S+) @ "
                         r"([\d.]+) t(\d+)")


def parse_board(goal):
    """The legacy board's per-clip table for one goal (g1/g2/g3.log). There
    is no machine-readable board output; the table is the contract, and the
    absolute scores come from the board's own quality cache when the cached
    entry is there."""
    path = os.path.join(BOARD_LOGS, f"{goal.lower()}.log")
    try:
        text = open(path).read()
    except OSError:
        return None
    try:
        qc = json.load(open(BOARD_QUAL))
    except Exception:
        qc = {}
    m = re.search(r"median of (\d+) samples", text)
    samples = int(m.group(1)) if m else 1
    win = (re.search(r"window=(\S+)", text) or [None, "?"])[1]
    bid = (re.search(r"binaries \[(\w+)\]", text) or [None, "?"])[1]
    q = {}
    for ln in BOARD_QLINE.finditer(text):
        b, codec, clip, pt, th = ln.groups()
        pre, suf = f"{b}:crf:lib{codec}:{clip}:", f":{pt}:{PRESET}:t{th}"
        hit = next((v for k, v in qc.items() if k.startswith(pre)
                    and k.endswith(suf)), None)
        if hit:
            q[(codec, clip)] = hit
    rows = {}
    for ln in text.splitlines():
        mm = BOARD_ROW.match(ln)
        if not mm:
            continue
        clip = mm.group(1)
        ys, xs = float(mm.group(5)), float(mm.group(6))
        rows[clip] = dict(kbps=int(mm.group(2)), ncrf=float(mm.group(3)),
                          xcrf=float(mm.group(4)), ours_wall=ys, ref_wall=xs,
                          ratio=float(mm.group(7)), dvmaf=float(mm.group(9)),
                          dsize=float(mm.group(10)), dpsnr=float(mm.group(11)),
                          ours_q=q.get(("yah264", clip)), ref_q=q.get(("x264", clip)))
    date = time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(path)))
    return dict(rows=rows, samples=samples, window=win, board_id=bid, date=date)


def fmt(x, nd=2, sign=False):
    if x is None or x == "":
        return ""
    return f"{x:+.{nd}f}" if sign else f"{x:.{nd}f}"


# LEGS THAT RAN ON A BOX THAT WAS NOT QUIET. LF_NOT_QUIET names a text file,
# one "<timed|purec> <cell> <record date>" per line; a record listed there is
# reported with valid = not-quiet and stays out of the class statistics.
NOT_QUIET = set()
if os.environ.get("LF_NOT_QUIET"):
    try:
        with open(os.environ["LF_NOT_QUIET"]) as _f:
            for _l in _f:
                _p = _l.split(None, 2)
                if len(_p) == 3 and not _l.startswith("#"):
                    NOT_QUIET.add((_p[0], _p[1], _p[2].strip()))
    except OSError:
        pass
# BOX STATE AT EACH LEG'S START. LF_LEG_NOTES names a text file written from the
# timing block's logs, one "<timed|purec>|<cell>|<record date>|<box CPU %>|<what
# was on the CPU>" per line; the matching row's note then states the reading,
# and names the process when something other than the encoders held a core.
LEG_NOTES = {}
if os.environ.get("LF_LEG_NOTES"):
    try:
        with open(os.environ["LF_LEG_NOTES"]) as _f:
            for _l in _f:
                _p = _l.rstrip("\n").split("|", 4)
                if len(_p) == 5:
                    LEG_NOTES[(_p[0], _p[1], _p[2])] = (_p[3], _p[4])
    except OSError:
        pass


def leg_note(kind_, cell, date):
    v = LEG_NOTES.get((kind_, cell, date))
    if not v:
        return []
    cpu, what = v
    out = [f"box CPU at start {cpu}%"]
    if what.strip():
        out.append(f"on the CPU beside the leg: {what.strip()}")
    return out


# First scored frame of the cells whose source opens on black (PARITY-SPEC
# section 2); the staged files are already cut there. The tier S Meridian clip
# has none since 2026-10-14: it is title frames 1782-2381, past the archival
# opening, and scores from its own frame 0. The tier M Meridian segment moved
# the same day: title frames 1782-7116, so its first frame is the film's own
# footage rather than the first non-black one.
SPEC_START = {"meridian": 1782, "chimera": 120, "sollevante": 193}
LOOKAHEAD = "lookahead: ours 40 / reference 40 (each encoder's default; the depth sweep has not run)"
# Dominant content class (detail/motion, share of the 10-minute window) from
# the 2026-09-26 scene scan. Sintel and StEM2 moved to another window after
# the scan, so theirs is the whole title's. Tiers S, M and the tier C clips
# were not scanned.
CONTENT_CLASS = {"tears": "detailed/moving 40%", "meridian": "mid/static 98%",
                 "sintel": "mid/moving 42% (whole title)",
                 "bbb": "detailed/moving 41%", "cosmos": "mid/moving 68%",
                 "stem2": "mid/moving 33% (whole title)",
                 "nocturne": "mid/moving 59%",
                 "lash1924": "detailed/moving 59%"}


def span_note(cell):
    """The frames a cell covers, for the report's note column."""
    title, size = split(cell)
    n = cell_info(cell)["frames"]
    if title in EXCERPT_FRAMES or title in EXCERPTS:
        st, _ = excerpt_span(cell)
        return (f"frames 0-{n - 1} of the excerpt (master frame {st} on)"
                + (", end credits excluded" if title in EXCERPT_FRAMES else ""))
    s0 = SPEC_START.get(cell, SPEC_START.get(title) if kind(cell) == "seg" else None)
    if s0:
        why = ("past the archival opening" if title == "meridian"
               else "starts at the first non-black frame")
        return f"source frames {s0}-{s0 + n - 1} ({why})"
    return f"frames 0-{n - 1}"


def harness_rows(cell, tier, title, size, extra_notes=()):
    """CSV rows (dicts) for one harness cell: G3 from the timed record, G1
    and G2 from the sampled pure-C record, quality from the matched point."""
    xr, nr, ptag = point_pair(cell)
    t, ttag, _ = timed_rec(cell)
    p, pctag, _ = purec_rec(cell)
    if not (xr and nr) and not t and not p:
        return []
    bdv, bdtag = bd_of(cell)
    cls = CLASS_OF[size]
    notes = list(extra_notes)
    try:
        notes.append(span_note(cell))
    except Exception:
        pass
    notes.append(LOOKAHEAD)
    if bdv is None and LAD_JSON:
        bdv, _, bnote = lad_bd(cell, "crf", tier)
        if bnote:
            notes.append(bnote)
        vb, _, vnote = lad_bd(cell, "vbr", tier)
        if vb is not None:
            notes.append(f"VBR BD-VMAF-NEG {vb:+.2f}%; {vnote}")
    supp, fok = {}, ""
    if xr and nr:
        for col, fld, nd in SUPP:
            supp[f"ours_{col}"] = fmt(nr.get(fld), nd)
            supp[f"ref_{col}"] = fmt(xr.get(fld), nd)
        # a record without the field predates the 5.2a columns, and then the
        # old scoring pass aborted on any short count: it passed the check
        oks = [rr.get("frames_ok", True) for rr in (xr, nr)]
        fok = "yes" if all(oks) else "no"
        q = dict(ours_bytes=nr["bytes"], ref_bytes=xr["bytes"],
                 dsize=nr["dsize_pct"], oneg=nr["neg"], xneg=xr["neg"],
                 opsnr=nr["psnr_y"], xpsnr=xr["psnr_y"],
                 ocrf=nr["crf"], xcrf=xr["crf"], st=nr.get("solve_threads"),
                 matched=xr.get("matched"))
    else:
        q = dict(ours_bytes=t["ours_bytes"], ref_bytes=t["x264_bytes"],
                 dsize=t["dsize_pct"], oneg=t["ours_neg"], xneg=t["x264_neg"],
                 opsnr=t["ours_psnr_y"], xpsnr=t["x264_psnr_y"],
                 ocrf=t["ours_crf"], xcrf=t["x264_crf"], st=t.get("solve_threads"),
                 matched=t.get("x264_matched"))
    notes.append(f"ours crf {q['ocrf']:.3f} (solved onto the reference's size)")
    if abs(q["xcrf"] - X264_CRF) > 1e-6:
        notes.append(f"reference crf {q['xcrf']:.3f}, not its default "
                     f"{X264_CRF:g} (moved onto our nearest size)")
    if q["st"] and str(q["st"]) not in ("0", "auto"):
        notes.append(f"size and scores from a {q['st']}-thread solve")
    if size == "2160p" and MEM_ESCAPE:
        notes.append(f"ours: input-memory guard raised (Y264_MAX_INPUT_MB="
                     f"{MEM_ESCAPE})")
    tags = {x for x in (ptag, ttag if t else None, pctag if p else None) if x}
    if "legacy-key" in tags:
        notes.append("legacy-key")
    if "other-build" in tags:
        notes.append("record from an earlier build")
    meta = t or p or {}
    base = dict(codec="yah264", date=REPORT_DATE,
                commit=(meta.get("versions") or {}).get("yah264", ""),
                tier=tier, **{"class": cls}, title=title, size=size)

    def row(goal, ow=None, xw=None, feed=None, rounds=None, sampled=None,
            md5=None, timed_dsize=None, quality=True, more=()):
        r = dict(base, goal=goal,
                 ref="x264" if goal == "G3" else "x264 (pure C)",
                 ref_version=((meta.get("versions") or {}).get(
                     "x264" if goal == "G3" else "x264_c", "") or
                     "x264 0.165.3222 b35605a").replace("x264 ", ""))
        r.update(ours_wall_s=fmt(ow), ref_wall_s=fmt(xw),
                 ratio=fmt(ow / xw) if ow and xw else "",
                 feed_fps=fmt(feed, 1) if feed else "",
                 ours_bytes=q["ours_bytes"], ref_bytes=q["ref_bytes"],
                 dsize_pct=fmt(q["dsize"], sign=True), rounds=rounds or "")
        if quality:
            r.update(ours_vmafneg=fmt(q["oneg"]), ref_vmafneg=fmt(q["xneg"]),
                     dvmafneg=fmt(q["oneg"] - q["xneg"], sign=True),
                     ours_psnr_y=fmt(q["opsnr"]), ref_psnr_y=fmt(q["xpsnr"]),
                     dpsnr_y=fmt(q["opsnr"] - q["xpsnr"], sign=True))
            if goal == "G3" and bdv is not None:
                r["bd_vmafneg_pct"] = fmt(bdv, sign=True)
            r.update(supp)
        r["frames_decoded_ok"] = fok
        r["_frames_bad"] = fok == "no"
        why = list(more)
        r["_feed_ok"] = None
        size_off = abs(q["dsize"]) > SIZE_BAR or (
            timed_dsize is not None and abs(timed_dsize) > SIZE_BAR)
        r["_size_off"] = size_off
        r["_timed_dsize"] = timed_dsize
        r["_notes"] = notes + why
        if sampled:
            r["_notes"].append(sampled)
        if md5:
            r["_notes"].append(md5)
        return r
    rows = []
    if t:
        n = t["frames"]
        ow = med([m[0] for m in t["ours"]["default"]])
        xw = med([m[0] for m in t["x264"]["default"]])
        feed = n / t["read"][0]
        m5 = t["ours"]["default"][0]
        md5 = (f"md5 ours {m5[6][:8]} ref {t['x264']['default'][0][6][:8]}"
               if len(m5) > 6 else "")
        how = {"staged": "staged Y4M", "vt": "VideoToolbox decode",
               "y4m": "Y4M block copy"}.get(
            t.get("feed"), "VideoToolbox decode" if is_full(cell) else "Y4M block copy")
        more = [f"feed: {how}"]
        if (t.get("feed_check") or {}).get("override"):
            more.append("feeder check overridden (LF_FEEDER_OK)")
        more.append("threads: default both sides (ours --threads 0, reference auto)")
        more += leg_note("timed", cell, t.get("date", ""))
        if t.get("feed") == "vt" and size == "2160p":
            more.append(f"decoder-fed at {feed / max(n / ow, n / xw):.1f}x the faster encoder: a "
                        "decoder-fed 2160p ratio can differ from a staged one by up to about "
                        "0.06 in either direction, more often in our favour")
        r = row("G3", ow, xw, feed, len(t["ours"]["default"]), md5=md5,
                timed_dsize=t.get("timed_dsize_pct"), more=more)
        r["_not_quiet"] = ("timed", cell, t.get("date", "")) in NOT_QUIET
        r["_date"] = t.get("date", "")
        r["_feed_ok"] = feed >= FEED_VALID * max(n / ow, n / xw)
        r["_fps"] = (n / ow, n / xw, feed)
        rows.append(r)
    elif xr and nr:
        r = row("G3", more=["not timed yet"])
        rows.append(r)
    if p:
        n = sum(k for _, k in p["windows"])
        rd = sum(x[0] for x in p["read"])
        for g in ("G1", "G2"):
            if not p.get(g) or not p[g].get("ours"):
                continue
            ow = sum(med([m[0] for m in ms]) for ms in p[g]["ours"])
            xw = sum(med([m[0] for m in ms]) for ms in p[g]["x264"])
            samp = (f"sampled: {len(p['windows'])} window(s), {n} of "
                    f"{p['frames']} frames" if n < p["frames"] else "")
            ob = sum(med([m[5] for m in ms]) for ms in p[g]["ours"])
            xb = sum(med([m[5] for m in ms]) for ms in p[g]["x264"])
            wd = 100 * (ob / xb - 1) if xb else None
            thr = ("threads: 1 both sides" if g == "G1" else
                   "threads: default both sides (ours --threads 0, reference auto)")
            wm = (p.get("win_match") or {}).get(g)
            if wd is None:
                wb = []
            elif n >= p["frames"]:
                # a clip timed whole: its size at this thread count must hold the bar itself
                wb = [f"size at this thread count {wd:+.2f}%"
                      + (f" (matched at this thread count; {wm['before_dsize_pct']:+.2f}% before)"
                         if wm else " (the 4-thread pair's rate factors)")]
                if abs(wd) > MATCH_BAR:
                    wb.append(f"VOID for {g}: size over {MATCH_BAR:g}% at this thread count")
            elif wm:
                wb = [f"window bytes {wd:+.2f}% (rate factors matched on the windows at "
                      f"this thread count; {wm['before_dsize_pct']:+.2f}% before the match)"]
            else:
                wb = [f"window bytes {wd:+.2f}% (the whole cell's rate factors, solved at "
                      "4 threads; not matched on the windows)"]
            if wm and abs(wm.get("x264_crf", X264_CRF) - X264_CRF) > 1e-6:
                wb.append(f"reference at crf {wm['x264_crf']:.3f} on the windows, not its default "
                          f"{X264_CRF:g} (our size steps over the target there)")
            if wd is not None and n < p["frames"] and abs(wd) > WIN_VOID:
                wb.append(f"VOID for {g}: window bytes over {WIN_VOID:g}%")
            r = row(g, ow, xw, n / rd, p.get("rounds", 1), sampled=samp,
                    quality=False, more=[thr] + wb + leg_note("purec", cell, p.get("date", "")))
            r["_win_dsize"] = wd if n < p["frames"] else None
            r["_win_before"] = wm["before_dsize_pct"] if wm else None
            if r["_win_dsize"] is not None and abs(wd) > WIN_VOID:
                r["_size_off"] = True
            if wd is not None and n >= p["frames"] and abs(wd) > MATCH_BAR:
                r["_size_off"] = True
            r["_not_quiet"] = ("purec", cell, p.get("date", "")) in NOT_QUIET
            r["_date"] = p.get("date", "")
            r["_feed_ok"] = (n / rd) >= FEED_VALID * max(n / ow, n / xw)
            r["_fps"] = (n / ow, n / xw, n / rd)
            rows.append(r)
    for r in rows:
        if cell in TIER_C:
            r["_nogate"] = True
            if r.get("ratio"):
                r.setdefault("_notes", []).append("speed not gating (4 s clip)")
        finish_valid(r)
    return rows


def finish_valid(r):
    """PARITY-SPEC section 4 in one word, the first that applies: a void
    number (feeder-bound, size-mismatch) before a weak one (single-run)."""
    v = []
    if r.get("_frames_bad"):
        v.append("frame-mismatch")
    if r.get("_feed_ok") is False:
        v.append("feeder-bound")
    if r.get("_size_off"):
        v.append("size-mismatch")
    if r.get("_not_quiet"):
        v.append("not-quiet")
    if r.get("rounds") == 1:
        v.append("single-run")
    r["valid"] = v[0] if v else "yes"
    extra = [f"also {x}" for x in v[1:]]
    r["note"] = "; ".join([n for n in r.get("_notes", []) if n] + extra)


def board_rows(clip, boards):
    """Legacy board rows for a tier S clip, one per goal that has a board."""
    title, size = clip.rsplit("_", 1)
    out = []
    for g in GOALS:
        b = boards.get(g)
        if not b or clip not in b["rows"]:
            continue
        x = b["rows"][clip]
        oq, rq = x["ours_q"], x["ref_q"]
        r = dict(codec="yah264", date=REPORT_DATE, commit=f"ffboard {b['board_id']}",
                 ref="x264" if g == "G3" else "x264 (pure C)",
                 ref_version="0.165.x (library)", tier="S",
                 **{"class": CLASS_OF[size]}, title=title, size=size, goal=g,
                 ours_wall_s=fmt(x["ours_wall"]), ref_wall_s=fmt(x["ref_wall"]),
                 ratio=fmt(x["ratio"]), feed_fps="",
                 ours_bytes=oq[0] if oq else "", ref_bytes=rq[0] if rq else "",
                 dsize_pct=fmt(x["dsize"], sign=True),
                 ours_vmafneg=fmt(oq[1]) if oq else "",
                 ref_vmafneg=fmt(rq[1]) if rq else "",
                 dvmafneg=fmt(x["dvmaf"], sign=True),
                 ours_psnr_y=fmt(oq[2]) if oq else "",
                 ref_psnr_y=fmt(rq[2]) if rq else "",
                 dpsnr_y=fmt(x["dpsnr"], sign=True), bd_vmafneg_pct="",
                 rounds=b["samples"])
        r["_size_off"] = abs(x["dsize"]) > SIZE_BAR
        r["_notes"] = [f"legacy board (rate-pinned): both solved to {x['kbps']} "
                       f"kbit/s, {b['window']} window, library encoders in ffmpeg; "
                       f"ours crf {x['ncrf']:.2f}, reference crf {x['xcrf']:.2f} "
                       f"(not its default {X264_CRF:g})"]
        r["_board"] = True
        finish_valid(r)
        out.append(r)
    return out


def num(r, k):
    try:
        return float(r[k])
    except (KeyError, ValueError, TypeError):
        return None


def spec_cells():
    """[(tier, cell-or-clip, title, size, source)] in spec order."""
    out = []
    for c in TIER_S:
        clip = c[2:]
        out.append(("S", c, *clip.rsplit("_", 1)))
    for t in ("chimera", "sollevante", "meridian"):
        for s in ("1080p", "2160p"):
            out.append(("M", f"{t}_{s}", t, s))
    for t, (base, _) in EXCERPTS.items():
        if t in EXCERPT_FRAMES:
            continue                # reported under its title, see tier_l_rows
        for s in FULL[base].get("sizes", SIZES):
            # Lash of the Whip is the tier C grain cell, not tier L
            out.append(("C" if t.startswith("lash") else "L", f"{t}_{s}",
                        t.replace("-x10", ""), s))
    for c in TIER_C:
        out.append(("C", c, *c[2:].rsplit("_", 1)))
    return out


def tier_l_rows(cell, title, size, crop_log):
    """Rows for one tier L cell under the credits rule. Where the title has a
    cropped cell, each goal comes from it when it has that goal's number; a
    goal only the whole excerpt has is reported "credits included", and its
    ratio is indicative: it is out of the class statistics."""
    t = cell.rsplit("_", 1)[0]
    if t not in CREDITS_LAST:
        return harness_rows(cell, "L", title, size)
    last = CREDITS_LAST[t]
    crop = harness_rows(f"{t}c_{size}", "L", title, size,
                        [f"cropped: frames 0-{last}, end credits excluded"])
    whole = harness_rows(cell, "L", title, size,
                         [f"credits included: the whole excerpt (story ends at "
                          f"frame {last}); indicative"])
    out = []
    for g in GOALS:
        c = next((r for r in crop if r["goal"] == g), None)
        w = next((r for r in whole if r["goal"] == g), None)
        if c and (num(c, "ratio") is not None or not
                  (w and num(w, "ratio") is not None)):
            out.append(c)
            crop_log.append((f"{title}_{size}", g, "cropped", last,
                             num(c, "ratio"), c["valid"]))
        elif w:
            if num(w, "ratio") is not None and w["valid"] in ("yes", "single-run"):
                w["valid"] = "credits-included"
            out.append(w)
            crop_log.append((f"{title}_{size}", g, "credits included", last,
                             num(w, "ratio"), w["valid"]))
    return out


def group_stats(rows):
    ok = [r for r in rows if num(r, "ratio") is not None
          and r["valid"] in ("yes", "single-run") and not r.get("_nogate")]
    if not ok:
        return None
    rs = sorted(ok, key=lambda r: num(r, "ratio"))
    return dict(med=med([num(r, "ratio") for r in rs]), worst=num(rs[-1], "ratio"),
                worst_cell=f"{rs[-1]['title']}_{rs[-1]['size']}",
                single=any(r["valid"] == "single-run" for r in ok), n=len(ok))


def do_report_spec():
    boards = {g: parse_board(g) for g in GOALS}
    boards = {g: b for g, b in boards.items() if b}
    rows, missing, standins, board_used, crop_log = [], [], [], [], []
    for tier, cell, title, size in spec_cells():
        got = []
        if tier == "S":
            got = harness_rows(cell, "S", title, size)
            if not got:
                got = board_rows(f"{title}_{size}", boards)
                if got:
                    board_used.append(f"{title}_{size}")
        elif tier == "M":
            got = harness_rows(cell, "M", title, size)
        elif tier == "C":
            got = harness_rows(cell, "C", title, size,
                               ["10-bit source converted to 8-bit 4:2:0"]
                               if cell in TIER_C_10BIT else ())
        else:
            got = tier_l_rows(cell, title, size, crop_log)
            if not got:
                full = f"{title}-full_{size}"
                got = harness_rows(full, "L", f"{title}-full", size,
                                   ["whole title, standing in for the 10-minute "
                                    "excerpt (not measured yet)"])
                if got:
                    standins.append(full)
        if not got:
            missing.append((tier, f"{title}_{size}"))
        rows += got

    # the page
    commits = sorted({r["commit"] for r in rows if r["commit"] and
                      not r["commit"].startswith("ffboard")})
    lf_dates = sorted(d for d in (
        (timed_rec(c)[0] or {}).get("date") for _, c, _, _ in spec_cells())
        if d) + sorted(d for d in (
        (timed_rec(f"{t}-full_{s}")[0] or {}).get("date")
        for t in ("tears", "meridian", "stem2", "sintel", "bbb", "cosmos")
        for s in SIZES) if d)
    lf_dates.sort()
    try:
        brand = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                               capture_output=True, text=True).stdout.strip()
        ncpu = subprocess.run(["sysctl", "-n", "hw.ncpu"], capture_output=True,
                              text=True).stdout.strip()
        macos = subprocess.run(["sw_vers", "-productVersion"], capture_output=True,
                               text=True).stdout.strip()
    except OSError:
        brand, ncpu, macos = "?", "?", "?"
    L = []
    L.append(f"# yah264 parity report, {REPORT_DATE}\n")
    # LF_REPORT_ENCODER names the builds that were measured (the timing
    # lane writes it: the records carry the harness commit, not the binary's)
    enc = os.environ.get("LF_REPORT_ENCODER") or f"yah264 {', '.join(commits) or '-'}"
    L.append(f"Encoder: {enc}, preset medium.  "
             f"Reference: x264 0.165.3222 b35605a, preset medium.")
    rd_ = sorted(r["_date"] for r in rows if r.get("_date"))
    nb = (f"timed legs {rd_[0]} to {rd_[-1]}" if rd_ else "-")
    if board_used:
        bd_ = sorted(b["date"] for b in boards.values())
        nb += f"; tier S board {bd_[0]} to {bd_[-1][11:]} (daytime)"
    L.append(f"Machine: {brand}, {ncpu} cores, macOS {macos}.  Measured: {nb}.")
    ver = versions()
    xv, xcv = ver.get("x264", "?"), ver.get("x264_c", "?")
    L.append(f"Reference binaries: SIMD `{X264}` = {xv}; pure C `{X264_C}` = {xcv}, "
             "the same commit configured with --disable-asm --disable-lavf --disable-ffms "
             "--disable-avs --disable-swscale --disable-lsmash --bit-depth=8, with "
             "-fno-tree-vectorize removed so its C is vectorised like ours.")
    if "b35605a" not in xv or "b35605a" not in xcv:
        L.append("**WARNING: a reference binary is NOT x264 b35605a (see the line above). "
                 "Every ratio and every ladder taken with it is against another reference.**")
    L.append("")
    L.append("How to read: a speed ratio is our time / the reference's time at the same file")
    L.append("size; below 1.00 we're faster. Pass = median <= 1.00 and worst <= 1.15, quality")
    L.append('no worse than -0.5 VMAF-NEG. "-" = not measured yet.')
    L.append('A dagger (†) marks a figure that includes a cell timed in one run only; "void"')
    L.append("means every cell there was measured but failed a validity rule (see the last")
    L.append("section). Quality holds for a class and tier when the median dVMAF-NEG is")
    L.append("at least -0.5, no cell is more than 1.0 VMAF-NEG or 1.0 dB PSNR-Y behind,")
    L.append("and every cell's size is within 1%. Both encoders read the same 8-bit")
    L.append("4:2:0 Y4M frames on stdin.")
    L.append("")

    def grp(cls, tier, goal=None):
        return [r for r in rows if r["class"] == cls and r["tier"] == tier
                and (goal is None or r["goal"] == goal)]

    verdict_goal = {}
    speed_lines = []
    for cls, tier in SPEC_ROWS:
        cells_txt, states, opens = [], {}, []
        for g in GOALS:
            rs = [r for r in grp(cls, tier, g) if num(r, "ratio") is not None]
            st = group_stats(rs)
            if st:
                cells_txt.append(f"{st['med']:.2f} / {st['worst']:.2f}"
                                 + (" †" if st["single"] else ""))
                ok = st["med"] <= MED_BAR and st["worst"] <= WORST_BAR
                states[g] = "pass" if ok else "open"
                if not ok:
                    opens.append((st["worst"], g, st["worst_cell"]))
            elif rs:
                cells_txt.append("void")
                states[g] = "void"
            else:
                cells_txt.append("-")
                states[g] = "none"
            verdict_goal.setdefault(g, []).append((cls, tier, states[g], st))
        if opens:
            w = max(opens)
            v = f"OPEN (worst {w[2]} {w[0]:.2f}, {w[1]})"
        elif all(states[g] == "pass" for g in GOALS):
            v = "PASS"
        elif any(states[g] == "pass" for g in GOALS):
            v = ("incomplete: " + ", ".join(g for g in GOALS if states[g] != "pass")
                 + " not measured or void")
        else:
            v = "-"
        speed_lines.append(f"| {cls} | {tier} | " + " | ".join(cells_txt) + f" | {v} |")

    qual_lines, qual_state = [], []
    for cls, tier in SPEC_ROWS:
        rs = [r for r in grp(cls, tier) if r["goal"] == "G3"
              and num(r, "dvmafneg") is not None
              and r["valid"] != "frame-mismatch"]
        if not rs:
            qual_lines.append(f"| {cls} | {tier} | - | - | - | - |")
            qual_state.append((cls, tier, "none", None))
            continue
        dv = sorted(rs, key=lambda r: num(r, "dvmafneg"))
        dp = sorted(rs, key=lambda r: num(r, "dpsnr_y"))
        wv, wp = num(dv[0], "dvmafneg"), num(dp[0], "dpsnr_y")
        mv = med([num(r, "dvmafneg") for r in rs])
        name = lambda r: f"{r['title']}_{r['size']}"          # noqa: E731
        worst = name(dv[0])
        why = []
        if mv < VMAF_BAR:
            why.append(f"median {mv:+.2f}")
        floor = sorted((r for r in rs if num(r, "dvmafneg") < VMAF_FLOOR
                        or num(r, "dpsnr_y") < PSNR_FLOOR),
                       key=lambda r: (num(r, "dvmafneg"), num(r, "dpsnr_y")))
        if floor:
            why.append("floor: " + ", ".join(
                f"{name(r)} {num(r, 'dvmafneg'):+.2f} VMAF / "
                f"{num(r, 'dpsnr_y'):+.2f} dB" for r in floor))
            worst = name(floor[0])
        off = [r for r in rs if num(r, "dsize_pct") is not None
               and abs(num(r, "dsize_pct")) > SIZE_BAR]
        if off:
            why.append("size: " + ", ".join(
                f"{name(r)} {num(r, 'dsize_pct'):+.2f}%" for r in off))
        v = f"OPEN ({'; '.join(why)})" if why else "PASS"
        qual_state.append((cls, tier, "pass" if not why else "open",
                           "; ".join(why) or worst))
        qual_lines.append(f"| {cls} | {tier} | {mv:+.2f} / {wv:+.2f} | "
                          f"{wp:+.2f} | {worst} | {v} |")

    def vline(states):
        ps = [f"{c}-{t}" for c, t, s, _ in states if s == "pass"]
        os_ = [f"{c}-{t} ({st['worst_cell']} {st['worst']:.2f})"
               if isinstance(st, dict) else f"{c}-{t} ({st})"
               for c, t, s, st in states if s == "open"]
        vd = [f"{c}-{t}" for c, t, s, _ in states if s == "void"]
        nm = [f"{c}-{t}" for c, t, s, _ in states if s == "none"]
        parts = []
        if ps:
            parts.append("PASS in " + ", ".join(ps))
        if os_:
            parts.append("OPEN in " + ", ".join(os_))
        if vd:
            parts.append("void in " + ", ".join(vd))
        if nm:
            parts.append("not measured in " + ", ".join(nm))
        return "; ".join(parts) or "-"

    # FULL PARITY IS COUNTED PER (class, tier, goal) COMBINATION: it passes
    # when that goal's median <= 1.00 and worst <= 1.15 there and the class
    # and tier's quality holds. m counts only combinations with valid
    # measurements; the rest are listed as unmeasured, never as failures.
    qok = {(c, t): st == "pass" for c, t, st, _ in qual_state}
    combos = [(c, t, g, st) for g in GOALS for c, t, st, _ in verdict_goal[g]]
    L.append("## Verdict")
    L.append(f"G1 pure C, 1 thread:   {vline(verdict_goal['G1'])}")
    L.append(f"G2 pure C, all cores:  {vline(verdict_goal['G2'])}")
    L.append(f"G3 SIMD, all cores:    {vline(verdict_goal['G3'])}")
    L.append(f"Quality:               {vline(qual_state)}")
    gs = gate_state()
    L += parity_lines(combos, qok, gs)
    L += rests_on_lines(gs)
    L.append("")
    L.append("## Speed (median / worst ratio, per class and tier)")
    L.append("| class | tier | G1 | G2 | G3 | verdict |")
    L.append("|---|---|---|---|---|---|")
    L += speed_lines
    if board_used:
        L.append("")
        L.append(f"Tier S: {len(board_used)} of {len(TIER_S)} clips come from the legacy "
                 "board (rate-pinned): each clip solved to a fixed bitrate over a "
                 f"{next(iter(boards.values()))['window']} window, library encoders "
                 "inside ffmpeg, not the spec's whole clip at CRF 23.")
    if standins:
        L.append(f"Tier L comes from {len(standins)} whole-title cells standing in for "
                 "the 10-minute excerpts, which are not measured yet.")
    L.append("")
    L.append("## Quality at matched size (VMAF-NEG delta median / worst; PSNR-Y worst)")
    L.append("| class | tier | dVMAF | dPSNR-Y | worst cell | verdict |")
    L.append("|---|---|---|---|---|---|")
    L += qual_lines
    # spec 5.2: a cell inside the floor but below the published bar is named
    # even when its class and tier pass, so a median cannot hide it
    soft = sorted((num(r, "dvmafneg"), f"{r['title']}_{r['size']}", r["tier"])
                  for r in rows if r["goal"] == "G3"
                  and num(r, "dvmafneg") is not None
                  and VMAF_FLOOR <= num(r, "dvmafneg") < VMAF_BAR)
    L.append("")
    L.append("Soft cells (within the floor, below -0.5): " + (", ".join(
        f"{c} {d:+.2f} ({t})" for d, c, t in soft) if soft else "none") + ".")
    if crop_log:
        L.append("")
        L.append("## Tier L end frames (end credits excluded)")
        L.append("A cropped cell runs from its start frame to the last story frame "
                 "(excerpt-relative, the same at every size). A credits-included "
                 "ratio is indicative only and is not in the table above.")
        by = {}
        for name, g, how, last, ratio, valid in crop_log:
            by.setdefault(name, []).append(
                f"{g} {how}" + (f" {ratio:.2f}" if ratio is not None else " -")
                + (f" [{valid}]" if valid not in ("yes", "credits-included") else ""))
        ends = {}
        for name, g, how, last, ratio, valid in crop_log:
            ends[name] = last
        for name in by:
            L.append(f"- {name} (end frame {ends[name]}): " + "; ".join(by[name]))
    wins = [(f"{r['title']}_{r['size']}", r["tier"], r["goal"], r["_win_dsize"],
             r.get("_win_before"))
            for r in rows if r.get("_win_dsize") is not None]
    if wins:
        L.append("")
        L.append("## G1/G2 window bytes (2-minute and 10-minute cells)")
        L.append("G1 and G2 time 30-second windows, each coded as its own stream. Summed "
                 "window size, ours against the reference, in the timed legs (G1 one thread, "
                 "G2 default threads); in brackets the same figure at the whole cell's rate "
                 "factors, before ours was matched on the windows. Owner ruling 2026-10-10: "
                 f"a goal whose windows differ by more than {WIN_VOID:g}% is void for that cell. "
                 "\"not matched\" = the leg predates the window match.")
        by = {}
        for name, tier, g, d, b in wins:
            by.setdefault((tier, name), {})[g] = (d, b)
        for (tier, name), gd in sorted(by.items()):
            flag = (f"  **VOID: over {WIN_VOID:g}%**"
                    if any(abs(d) > WIN_VOID for d, _ in gd.values()) else "")
            L.append(f"- {name} ({tier}): " + ", ".join(
                f"{g} {gd[g][0]:+.2f}% (" + (f"{gd[g][1]:+.2f}% before" if gd[g][1] is not None
                                            else "not matched") + ")"
                for g in ("G1", "G2") if g in gd) + flag)
    if LAD_JSON:
        f2 = lambda x: "-" if x is None else f"{x:+.2f}%"          # noqa: E731
        bl, hl, tl, landed, fails, pend, nprov = [], [], [], 0, [], [], 0
        near, s5, ncloud, own, own2, cl_all, cl_part = [], [], 0, {}, [], [], []
        for tier, cell, title, size in spec_cells():
            if tier not in ("M", "L"):
                continue
            c = f"{cell.rsplit('_', 1)[0]}c_{size}" if cell.rsplit("_", 1)[0] in CREDITS_LAST else cell
            parts = []
            for mode in ("crf", "vbr"):
                cv = lad_curve(c, mode, tier)
                M = mode.upper()
                name = f"{title}_{size} ({tier}) {M}"
                if cv is None:
                    st_ = cloud_state(c.replace("-x10c_", "-x10_"), mode, LAD_FINAL)
                    parts.append(f"{M} no ladder" + (f" (cloud ladder {st_})" if st_ else ""))
                    pend.append(name + (f" (cloud ladder {st_})" if st_ else " (no ladder)"))
                    continue
                if cv["state"] == "legacy":
                    parts.append(f"{M} void (only scores at an even stride of "
                                 f"{cv['stride']}; being rescored)")
                    pend.append(name + " (even stride only)")
                    continue
                f = cv["final"]
                if cv["state"] == "final":
                    landed += 1
                    nprov += f["prov"]
                    if f["prov"]:
                        s5.append(name)
                    if f["ncloud"]:
                        ncloud += 1
                        (cl_all if f["ncloud"] == f["nrows"] else cl_part).append(name)
                    if f["src"] == "cloud":
                        own[f"{title}_{size}"] = f["label"] or f["frames"]
                    elif cv.get("second"):
                        s2 = cv["second"]
                        own2.append(f"{name}: a cloud box also measured this ladder on its own "
                                    f"frames ({s2['label'] or s2['frames']}): {s2['bn']:+.2f}% there, "
                                    f"{f['bn']:+.2f}% here; this machine's curve is the one counted.")
                    if 0.5 <= f["bn"] <= 1.0:
                        near.append(f"{name} {f['bn']:+.2f}%")
                    over = f["bn"] > 1.0
                    if over:
                        fails.append(f"{title}_{size} {M} ({f['bn']:+.2f}%)")
                    parts.append(f"{M} BD-VMAF-NEG {f2(f['bn'])} (PSNR-Y {f2(f['bp'])}; "
                                 f"{f['n']} rungs, {lad_how(f)})"
                                 + ("  **OVER the +1.0% bar**" if over else ""))
                else:
                    st_ = cloud_state(c.replace("-x10c_", "-x10_"), mode, LAD_FINAL)
                    pend.append(name + (f" ({cv['part']} rungs landed)" if cv["part"] else "")
                                + (f" (cloud ladder {st_})" if st_ else ""))
                    parts.append(f"{M} pending on the final binary"
                                 + (f" ({cv['part']} rungs landed)" if cv["part"] else ""))
                if mode == "vbr" and f:
                    bad = [e for e in f["track_o"] if abs(e) > TRACK_BAR]
                    tl.append(f"- {title}_{size} ({tier}): " + ", ".join(
                        f"{q[1:]} kbit/s ours {eo:+.1f}% / reference {ex:+.1f}%"
                        for q, eo, ex in zip(f["rungs"], f["track_o"], f["track_x"]))
                        + ("  (cloud)" if f["ncloud"] == f["nrows"] else "  (part cloud)" if f["ncloud"] else "")
                        + (f"  **{len(bad)} rung(s) over +-{TRACK_BAR:g}%**" if bad else ""))
                h = cv["hist"]
                if h:
                    hl.append(f"- {name}: {f2(h['bn'])} (PSNR-Y {f2(h['bp'])}) on build "
                              f"{h['build']}, " + ("every frame" if h["sub"] == 1
                                                   else f"every {h['sub']} frames"))
            bl.append(f"- {title}_{size} ({tier}): " + "; ".join(parts))
        same = crf_same_as_ours()
        L.append("")
        L.append("## BD-rate from the ladder lane (bar: BD-VMAF-NEG <= +1.0%)")
        L.append(f"{landed} of {landed + len(pend)} cell-modes have a landed curve; "
                 f"{landed - len(fails)} of those {landed} pass"
                 + (f"; failing: {', '.join(fails)}" if fails else "")
                 + f"; {len(pend)} pending or without a ladder"
                 + (f"; {ncloud} of the landed curves have rungs measured in the cloud" if ncloud else "")
                 + (f"; {nprov} are provisional (stride 5)" if nprov else "")
                 + ". Within 0.5 points of the bar: " + (", ".join(near) if near else "none") + ".")
        L.append("Five rungs, both encoders at 6 threads, lookahead 40 both sides (the sweep is "
                 "its own section below), negative = ours needs fewer bits. Only curves of the "
                 "final binary count. VMAF-NEG is scored on every frame where such a curve "
                 "exists, else on every 3rd; a tier L curve scored on every 5th is provisional "
                 "(it has moved a BD by 0.8 points on identical streams) and even strides are "
                 "void.")
        if ncloud or any(k.split("|")[2].startswith("cloud-") for k in _lad() if k.count("|") == 5):
            L.append("Curves marked \"cloud\" were measured on rented ARM instances and count "
                     "the same as this machine's (owner, 2026-10-12): both encoders ran there, "
                     "on the final binary's Linux build. Their source frames were "
                     "software-decoded, and were checked identical to this machine's frames "
                     "except where a curve says \"on the box's own frames\"; \"part cloud\" = a "
                     "box measured only the rungs this machine had not.")
        for cell_, why in sorted(own.items()):
            L.append(f"[*] {cell_}: {why}. Both encoders coded those frames.")
        L += own2
        if cl_all or cl_part:
            L.append("Measured in the cloud: " + (", ".join(cl_all) or "none")
                     + ". Part cloud (some rungs from a box, the rest from this machine, "
                     "identical frames): " + (", ".join(cl_part) or "none") + ".")
        note = os.environ.get("LF_LAD_CRF_NOTE", "")
        if note:
            L.append("CRF on the final binary: " + note)
        if same[1]:
            L.append(("Separately, the" if note else "CRF on the final binary: the")
                     + f" \"earlier build\" rows (build d2fc019) were compared with the rows of "
                     f"the later binaries in the store: of {same[1]} CRF rungs held for both, "
                     f"{same[2]} are byte-identical"
                     + (f" and the rest differ by at most {same[3]:.4f}% in size (6-thread "
                        "encodes vary that much from run to run)" if same[2] < same[1] else "")
                     + (". Those rows therefore count where the final binary has no curve of "
                        "its own." if same[0] else
                        ". That is not close enough: those rows do not count."))
        if s5:
            L.append("Still at stride 5 (no every-frame or stride-3 score of both arms in the "
                     "store): " + ", ".join(s5) + ".")
        L += bl
        if tl:
            L.append("")
            L.append(f"### VBR rate tracking per rung, final binary (bar: within +-{TRACK_BAR:g}% "
                     "of the target over the whole cell)")
            L.append("Achieved bitrate against the rung's target, on the curve counted above.")
            L += tl
        gl, ve = gate_note_lines(), variation_extra()
        L += gl
        if ve:
            L += ([] if gl else ["", "### Variation of a threaded capped-VBR ladder"]) + ve
        if pend:
            L.append("")
            L.append("### Pending on the final binary")
            L.append("; ".join(pend) + ".")
        if hl:
            L.append("")
            L.append("### History, not counted: VBR curves of earlier builds")
            L.append("These predate the final binary and say nothing about it.")
            L += hl
    L += sweep_lines()
    L += vbr_wall_lines()
    L += credits_lines()
    L += cloud_wall_lines()
    titles = sorted({r["title"] for r in rows if r["tier"] in ("L", "C")})
    cc = [f"{t} {CONTENT_CLASS[t]}" for t in titles if t in CONTENT_CLASS]
    if cc:
        L.append("")
        L.append("## Dominant content class per title (scene scan of 2026-09-26)")
        L.append("; ".join(cc) + ". Tiers S and M and the tier C sports and screen clips "
                 "were not scanned.")
    if os.environ.get("LF_REPORT_NOTES"):
        try:
            with open(os.environ["LF_REPORT_NOTES"]) as f:
                txt = f.read().strip()
            if txt:
                L.append("")
                L.append("## Notes from the timing lane")
                L += txt.splitlines()
        except OSError:
            pass
    L.append("")
    L.append("## Changed since the last report")
    L += changed_lines(rows, board_used, standins)
    L.append("")
    L.append("## Not valid or not measured")
    L += invalid_lines(rows, missing, board_used, standins, boards)

    os.makedirs(REPORT_DIR, exist_ok=True)
    md = os.path.join(REPORT_DIR, f"PARITY-REPORT-yah264-{REPORT_DATE}.md")
    cs = os.path.join(REPORT_DIR, f"PARITY-REPORT-yah264-{REPORT_DATE}.csv")
    with open(md, "w") as f:
        f.write("\n".join(L) + "\n")
    with open(cs, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_COLS, extrasaction="ignore",
                           lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in CSV_COLS})
    print(f"  wrote {md}\n  wrote {cs} ({len(rows)} rows)")


def changed_lines(rows, board_used, standins):
    prev = sorted(f for f in (os.listdir(REPORT_DIR) if os.path.isdir(REPORT_DIR)
                              else [])
                  if re.fullmatch(r"PARITY-REPORT-yah264-\d{4}-\d{2}-\d{2}\.csv", f)
                  and f[21:31] < REPORT_DATE)
    out = []
    if prev:
        with open(os.path.join(REPORT_DIR, prev[-1])) as f:
            old = {(r["tier"], r["title"], r["size"], r["goal"]): r
                   for r in csv.DictReader(f)}
        moved = []
        for r in rows:
            o = old.get((r["tier"], r["title"], r["size"], r["goal"]))
            a, b = num(o or {}, "ratio"), num(r, "ratio")
            if a is not None and b is not None and abs(b - a) >= 0.03:
                moved.append((abs(b - a), f"- {r['title']}_{r['size']} {r['goal']} "
                              f"(tier {r['tier']}): {a:.2f} -> {b:.2f}"))
            elif o is None and b is not None:
                moved.append((0, f"- {r['title']}_{r['size']} {r['goal']} "
                              f"(tier {r['tier']}): new, {b:.2f}"))
        out.append(f"Against {prev[-1]}:")
        out += [m for _, m in sorted(moved, reverse=True)[:5]] or \
               ["- no ratio moved by 0.03 or more"]
    else:
        out.append("- First report in this layout (PARITY-SPEC draft 2); there is no "
                   "earlier one to compare with.")
    if board_used:
        out.append(f"- Tier S is still the legacy board ({len(board_used)} clips): the "
                   "spec's whole-clip CRF 23 cells (s-<clip>) exist in the harness "
                   "but are not measured yet.")
    if standins:
        out.append(f"- Tier L: {len(standins)} whole-title records stand in for the "
                   "10-minute excerpts; the excerpt cells (<title>-x10_<size>) and "
                   "the staged raw feed that keeps 720p off the decoder limit are "
                   "built, not yet run.")
    out.append("- Records written from now on carry the binary and scoring identity "
               "in their keys; everything above predates that and is marked "
               "legacy-key in the CSV.")
    return out


def invalid_lines(rows, missing, board_used, standins, boards):
    out = []
    fb = [r for r in rows if r["valid"] == "feeder-bound" or
          "also feeder-bound" in r["note"]]
    for r in fb:
        o, x, feed = r["_fps"]
        out.append(f"- FEEDER-BOUND, ratio void: {r['title']}_{r['size']} "
                   f"{r['goal']} (tier {r['tier']}). The feeder alone read "
                   f"{feed:.0f} fps; the faster encoder ran {max(o, x):.0f} fps "
                   f"(needs {FEED_VALID:g}x = {FEED_VALID*max(o, x):.0f}).")
    for r in rows:
        if r["valid"] == "frame-mismatch":
            out.append(f"- Frame mismatch, void: {r['title']}_{r['size']} "
                       f"{r['goal']}: the decode or the scoring pass did not "
                       "return every frame.")
    for r in rows:
        if r["valid"] == "size-mismatch" or "also size-mismatch" in r["note"]:
            td = r.get("_timed_dsize")
            # the timed run's own bytes when they are what missed the bar; the
            # point's delta beside it (it can sit inside the bar while the run
            # at a different thread count does not)
            what = (f"{td:+.2f}% in the timed run (point {r['dsize_pct']}%)"
                    if td is not None and abs(td) > SIZE_BAR else f"{r['dsize_pct']}%")
            out.append(f"- Size mismatch, void: {r['title']}_{r['size']} "
                       f"{r['goal']}: {what} (bar +/-{SIZE_BAR:g}%).")
    nq = sorted({f"{r['title']}_{r['size']} {r['goal']}" for r in rows
                 if r["valid"] == "not-quiet" or "also not-quiet" in r["note"]})
    if nq:
        out.append("- Not quiet, indicative only (the box was over 150% CPU at the "
                   "leg's start): " + ", ".join(nq) + ".")
    one = sorted({f"{r['title']}_{r['size']} {r['goal']}" for r in rows
                  if r["valid"] == "single-run" or "also single-run" in r["note"]})
    if one:
        out.append("- From one run only: " + ", ".join(one) + ".")
    untimed = sorted({f"{r['title']}_{r['size']}" for r in rows
                      if r["goal"] == "G3" and "not timed yet" in r["note"]})
    if untimed:
        out.append("- Solved and scored but not timed yet (quality only): "
                   + ", ".join(untimed) + ".")
    for tier in ("S", "M", "L", "C"):
        have = {r["goal"] for r in rows if r["tier"] == tier
                and num(r, "ratio") is not None}
        gone = [g for g in GOALS if g not in have]
        if gone:
            out.append(f"- Tier {tier}: {', '.join(gone)} not measured on any cell.")
    if missing:
        out.append("- No record at all: " + ", ".join(
            f"{c} (tier {t})" for t, c in missing) + ".")
    leg = sum(1 for r in rows if "legacy-key" in r["note"])
    if leg:
        out.append(f"- {leg} rows rest on legacy-key records (written before the "
                   "binary identity was part of the key), so nothing in the record "
                   "itself proves which build ran; they are reported, not re-run.")
    if any(r.get("_board") for r in rows):
        out.append("- The legacy board ran in daytime; its quiet state was not "
                   "recorded. It feeds frames from memory inside ffmpeg, so it has "
                   "no feeder rate.")
    out.append("- The supplementary columns (spec 5.2a: harmonic-mean, minimum, "
               "1%-low and first-second VMAF-NEG, plain VMAF, YUV-PSNR and SSIM "
               "6:1:1) are blank for every record scored before 2026-09-26, which "
               "kept only the pooled VMAF-NEG and PSNR-Y. Plain VMAF is opt-in "
               "(LF_PLAIN_VMAF=1) because its second model costs about 80% more "
               "scoring CPU.")
    out.append("- No output md5 was recorded before 2026-09-26 (spec rule 5); "
               "timed legs record and print one from now on.")
    out.append("- The long-form legs ran with the harness's load-average guard "
               "overridden, after the night block's own whole-box CPU check "
               "passed; they are counted as quiet on that check.")
    return out


def do_cells():
    for c in all_cells():
        info = cell_info(c)
        src = (staged_file(c) or master_path(base_title(split(c)[0]))) if is_full(c) \
            else cell_path(c)
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
    if cmd == "feeder-argv":
        # for tools outside the harness that must read a cell exactly as a leg does:
        # prints the feeder command (JSON argv), frame count and rate of one cell
        c = [x for x in sys.argv[2:] if not x.startswith("--")][0]
        i = cell_info(c)
        print(json.dumps({"argv": feeder(c, workers=timed_workers(c)), "frames": i["frames"],
                          "fps": fps_of(i), "w": i["w"], "h": i["h"]}))
        return
    if cmd == "stage":
        return do_stage()
    if cmd == "selftest-verdict":
        selftest_verdict()
        return
    if cmd == "report-spec":
        return do_report_spec()
    if cmd == "retag":
        return do_retag()
    if cmd == "validate":
        load_guard()
        return do_validate()
    args = [a for a in sys.argv[2:] if not a.startswith("--")]
    args = args or ["all"]
    cells = []
    for a in args:
        cells += (all_cells(a) if a in ("all", "full", "segments", "excerpts",
                                         "tier-s", "tier-c") else [a])
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
          "timed": do_timed, "purec": do_purec, "winpoint": do_winpoint,
          "refine": do_refine,
          "verify-feed": do_verify_feed,
          "stage-excerpt": do_stage_excerpt}.get(cmd)
    if not fn:
        sys.exit(__doc__)
    for c in cells:
        fn(c)


main()
