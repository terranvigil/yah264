#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ffboard.py -- the speed board taken THROUGH ONE FFMPEG BINARY, with both
# encoders called as libraries in the same process.
#
# WHY A SECOND BOARD. scripts/perf-comp.sh runs two CLIs and feeds each one Y4M,
# so each encoder's own input reader sits inside the measurement. The obvious
# worry is that the board is partly scoring our Y4M path against x264's. This
# harness removes the question by construction: one demuxer, one process, one
# thread pool, both encoders reached through libavcodec.
#
# WHAT IT FOUND. Nothing, which is the useful part. Run against the CLI board at
# the same operating points the two agree within noise on every clip, so the
# input path was never where the gap lived. Keep this board as the control, not
# as the headline -- the CLI board is cheaper and measures the same thing.
#
# It also needs an ffmpeg built against libyah264, so it cannot run from a
# stock checkout. See docs/ffmpeg-integration-plan.md.
#
# Usage:  RC=crf THREADS=12 X264LIB=... NOASM=1 scripts/ffboard.py
#
# THE PUBLISHED BOARD: the ten clips of
# scripts/parity-clips.sh, RC=crf, timed to /dev/null, goal 1 at THREADS=1 with
# its own solve, goals 2 and 3 at THREADS=0 (auto). CLIPS=legacy is the six-clip
# set the boards before 2026-09-02 were taken on.
#
# Env:
#   FF        ffmpeg binary built with --enable-libyah264 --enable-libx264
#             (default: ../build/ffboard/ffmpeg-yah264/ffmpeg, then the old
#             /tmp path; Y264_FFBOARD_ROOT moves the whole set)
#   X264LIB   install prefix of the libx264 to load (asm or autovec build)
#   Y264LIB   install prefix of libyah264
#   NOASM     1 = force yah264's scalar path (YAH264_NO_ASM), and select the
#             pure-C libx264 via X264LIB. Only yah264 has such a switch: for
#             any other encoder under test the flag affects the REFERENCE only
#   ENC       encoder under test (default libyah264). libx264 is always the
#             reference. libopenh264 is ABR-only, see the RC note below
#   RC        crf (matched operating point, the headline) | abr (rate-matched)
#   THREADS   thread count handed to both encoders
#   CORP      clip directory (default tests/corpus)
#   RUNS, REPEAT_FLOOR, SECONDS, PRESET, VMAF
#   Y264_NO_SOLVE_CACHE=1, or the flag --no-solve-cache: neither read nor write
#             the solve cache. The cache is keyed by the identity of all three
#             binaries (see below), so this is for a box whose cache is under
#             suspicion rather than for routine use
#   Y264_HEADER_CHECK=0   run with a Y264LIB whose public header is not the one
#             this ffmpeg was built against. There is no good reason; it exists
#             so a diagnostic run can reproduce the failure deliberately
#   Y264_DSIZE_TOL, Y264_DSIZE_TOL_MAX   the size-match guard's bars, in percent
#             (defaults 2.0 on the median and 5.0 on any one clip)
#
# THREE BINARIES DECIDE WHAT THIS BOARD SAYS, and two defects in one week came
# from not writing that down:
#
#   1. THE SOLVE CACHE USED TO BE KEYED WITHOUT THEM (2026-09-18). It held
#      clip:target:threads and nothing about which libraries produced the
#      answer, so a cache from the libnext264 era was reused against a yah264
#      whose CRF scale had moved since. The board that came out read dsize -12%
#      on the median and -24 to -37% on five clips: the two encoders were at
#      different operating points and every wall ratio on it was meaningless.
#      The key now carries a short hash of each dylib the ffmpeg will actually
#      load (symlinks resolved), each library's pkg-config version, and a hash
#      of the ffmpeg binary itself. Change any of the three and the old entries
#      are simply not found. Every solve prints hit or miss with that key.
#
#   2. THE WRAPPER IS ABI-BOUND TO THE HEADER IT WAS BUILT WITH (2026-09-19).
#      B-partitions added a field to yah264_param_t after the ffmpeg was built,
#      so the wrapper handed the library a struct in the old layout and the
#      library read every field past that point as some other field's bytes. It
#      encoded. park_joy solved to CRF 42.7 at 55 Mbps and timed 32x slower
#      than x264. Two answers, and the board wants both: the library refuses a
#      parameter struct that is not its own size (yah264.h ABI 3), and this
#      script refuses a Y264LIB whose public header is not the one recorded
#      beside the ffmpeg at build time.
#
# The third guard is downstream of both: whatever the solve did, if the two
# encoders did not land on the same bytes the table is not a speed reading, and
# printing its median would be an invitation to quote it. The board says
# INVALID and exits non-zero instead.
#
# THE PURE-C ARM IS A TRAP. x264's configure adds -fno-tree-vectorize
# UNCONDITIONALLY (configure ~line 1438, outside any asm test), so every stock
# x264 build has vectorization suppressed -- its C is a fallback behind hand-asm,
# not a tuned target. yah264's YAH264_NO_ASM=1 is only a runtime dispatch
# switch, so our C stays -O3 auto-vectorized. Point X264LIB at a stock
# --disable-asm build and you are comparing our vectorized C against their
# scalar C: that reads goal 2 as 0.73x instead of 1.04x, a third of a supposed
# win that was entirely the flag.
#
# Note the corollary, since it is the obvious thing to reach for instead:
# x264's RUNTIME toggle does not avoid this. `x264 --asm 0` on a stock build
# measures 1.19s where the flag-stripped build measures 0.76s on the same clip
# -- identical to the genuinely-scalar build, because the flag was applied when
# the binary was compiled and no runtime switch can undo it.
#
# Build the pure-C libx264 the way scripts/perf-comp.sh documents: configure
# --disable-asm, strip -fno-tree-vectorize from config.mak, then make.

import os, subprocess, sys, time, json, math, resource, hashlib, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scratch                                                  # noqa: E402

_ROOT       = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The ffmpeg with both encoders is the one docs/ffmpeg-integration-plan.md
# builds; a stock ../FFmpeg tree has neither and used to be the default here,
# failing one cell at a time.
#
# THE BUILD MOVED OUT OF /tmp. macOS sweeps /tmp, and it did: the whole set --
# ffmpeg, the installed libyah264, both libx264 builds -- was gone on
# 2026-09-17 and the in-process arm of that day's board could not run. The
# home is ../build/ffboard beside the checkout now, which survives a sweep and
# a reboot. /tmp is kept as a fallback for a box that still has the old one,
# and Y264_FFBOARD_ROOT overrides the directory for a build kept elsewhere.
_FFB_ROOT   = os.environ.get("Y264_FFBOARD_ROOT",
                             os.path.join(os.path.dirname(_ROOT), "build", "ffboard"))
_FF_DEFAULT = next((f for f in (os.path.join(_FFB_ROOT, "ffmpeg-yah264", "ffmpeg"),
                                "/tmp/ffmpeg-yah264/ffmpeg",
                                os.path.join(os.path.dirname(_ROOT), "FFmpeg", "ffmpeg"))
                    if os.path.exists(f)),
                   os.path.join(_FFB_ROOT, "ffmpeg-yah264", "ffmpeg"))

FF      = os.environ.get("FF", _FF_DEFAULT)
X264LIB = os.environ.get("X264LIB", "")     # install prefix, required
Y264LIB = os.environ.get("Y264LIB", "")     # install prefix, required
CORP    = os.environ.get("CORP", os.path.join(_ROOT, "tests", "corpus"))
SECONDS = float(os.environ.get("SECONDS", "6"))
THREADS = os.environ.get("THREADS", "12")
NOASM   = os.environ.get("NOASM", "0") == "1"
RUNS    = int(os.environ.get("RUNS", "3"))
FLOOR   = float(os.environ.get("REPEAT_FLOOR", "0.35"))
PRESET  = os.environ.get("PRESET", "medium")
RC      = os.environ.get("RC", "abr")          # abr | abrm | crf
# The encoder under test. libx264 is always the reference the ratio is taken
# against. openh264 exposes no quality knob through ffmpeg, only a bitrate, so
# it can only be boarded at RC=abr; and it has no scalar/SIMD switch of its own,
# so its "pure-C" rows mean openh264 as built against a pure-C x264.
ENC     = os.environ.get("ENC", "libyah264")
# A board writes several GB of bitstream and y4m per cell through the temp
# volume. A full one does not fail loudly when the volume runs out: cells come
# back with truncated recon and read as content failures. Check first.
scratch.df_guard(20, what='ffboard')

# TWO DIRECTORIES, because they answer to different clocks. The solve cache
# is a RECORD: it survives between boards and between weeks and must not be
# swept, so it lives in the named cache ($Y264_CACHE, default
# ~/.cache/yah264/ffboard) rather than under $TMPDIR, which macOS sweeps --
# the same sweep that took the ffmpeg build on 2026-09-17. WD is per-run
# scratch, several GB of it at 4K, and it is removed when the board exits.
CACHE   = scratch.cache_dir("ffboard")
WD      = os.environ.get("WD") or scratch.mkdtemp("ffboard")
VMAF    = os.environ.get("VMAF", "vmaf")
# Timed encodes write to /dev/null; the file the size and VMAF columns need is
# produced once, untimed. docs/instruments.md records a timed 9.5 MB write
# reading 2.1-10.2 s against 1.63 s to /dev/null and faking a resolution-
# shaped effect; until 2026-09-02 this harness timed the write anyway.
# TIMED_NULL=0 restores the old behaviour for comparison with earlier boards.
TIMED_NULL = os.environ.get("TIMED_NULL", "1") == "1"
# Which arm is timed FIRST within each sample pair. The pair is mirrored run to
# run either way (see measure/measure_crf), so this does not change what the
# board cancels -- it only decides which arm leads. Flipping it is the check for
# an order effect the mirroring failed to cancel: if the ratio moves when the
# leader changes, something is warming for the follower and neither reading is
# clean. ORDER=x264 leads with the reference.
ORDER_X_FIRST = os.environ.get("ORDER", "enc").lower() in ("x264", "x", "ref", "b")

# clip:target-kbps, READ from scripts/parity-clips.sh so the two harnesses cannot
# drift again (they did on 2026-08-31, when that file was rebalanced to ten
# clips with 1080p and this list stayed at the legacy six, so every published
# goal figure until 2026-09-02 was a six-clip number with no 1080p in it).
# CLIPS=legacy boards the old six for comparison with those figures.
def _clips_from_sh(var):
    try:
        for ln in open(os.path.join(_ROOT, "scripts", "parity-clips.sh")):
            if ln.startswith(var + "="):
                body = ln.split("=", 1)[1].strip().strip('"')
                if body.startswith("${"):            # ${CLIPS:-a:1 b:2}
                    body = body[body.index(":-") + 2:].rstrip("}")
                return [(c.rsplit(":", 1)[0], int(c.rsplit(":", 1)[1]))
                        for c in body.split()]
    except (OSError, ValueError, IndexError):
        pass
    return None
CLIPS = _clips_from_sh("CLIPS") or [
    ("foreman_cif", 400), ("bus_cif", 400), ("stefan_cif", 400),
    ("ducks_720p", 25000), ("park_joy_720p", 12000), ("samsung_720p", 1200),
    ("shields_720p", 2200), ("sunflower_1080p", 1500), ("pedestrian_1080p", 2800),
    ("riverbed_1080p", 12500)]
if os.environ.get("CLIPS", "").strip().lower() == "legacy":
    CLIPS = _clips_from_sh("CLIPS_LEGACY") or CLIPS[:6]
    os.environ["CLIPS"] = ""

# CLIPS=name:kbps[,name:kbps...] boards a different set -- one clip, or a clip
# the standing board does not carry. The board list above stays the default so
# a bare run is still the comparable one; anything else is a side question and
# its MEDIAN/MAX rows are that subset's, not the board's. A single-clip run
# makes median and max the same number, which is the point when the question is
# about one clip rather than about the corpus.
if os.environ.get("CLIPS"):
    try:
        CLIPS = [(c.rsplit(":", 1)[0], int(c.rsplit(":", 1)[1]))
                 for c in os.environ["CLIPS"].split(",") if c.strip()]
    except (ValueError, IndexError):
        sys.exit("ffboard: CLIPS wants name:kbps[,name:kbps...], "
                 f"got '{os.environ['CLIPS']}'")

os.makedirs(WD, exist_ok=True)

# Flags. The script has taken its configuration from the environment since it
# was written and that does not change; --no-solve-cache is a flag as well
# because it is the one knob reached for at the keyboard, in the middle of
# doubting a board, and an env name is the wrong shape for that.
NO_SOLVE_CACHE = (os.environ.get("Y264_NO_SOLVE_CACHE", "0") == "1"
                  or "--no-solve-cache" in sys.argv[1:])
_UNKNOWN = [a for a in sys.argv[1:] if a != "--no-solve-cache"]
if _UNKNOWN:
    sys.exit(f"ffboard: unknown argument(s) {' '.join(_UNKNOWN)} -- this board "
             "is configured through the environment; the only flag is "
             "--no-solve-cache")

# ---------------------------------------------------------------------------
# The identity of the three binaries a reading depends on.
#
# Read the file the ffmpeg will ACTUALLY LOAD, not the prefix it was pointed
# at. `otool -L` names the leaf the binary asks dyld for, DYLD_LIBRARY_PATH
# then supplies it from X264LIB/Y264LIB, and the leaf is frequently a symlink
# onto a versioned file. So: take the leaf from the binary, find it under the
# prefix, resolve the link, hash the bytes on the other end. A prefix name is
# not an identity -- y264inst is rebuilt in place, which is exactly how the
# stale-cache board happened.

def _sha12(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:12]

def _blob_sha(path):
    """git's blob hash of a file, computed without git or a repository, so the
    recipe can write it with `git hash-object` and this can read it anywhere."""
    with open(path, "rb") as f:
        d = f.read()
    return hashlib.sha1(b"blob %d\0" % len(d) + d).hexdigest()

def _otool_leaves(binary):
    try:
        out = subprocess.run(["otool", "-L", binary], capture_output=True,
                             text=True).stdout
    except OSError:
        return []
    return [os.path.basename(ln.strip().split(" ")[0])
            for ln in out.splitlines()[1:] if ln.strip()]

def _dylib_under(prefix, token, avoid=()):
    libdir = os.path.join(prefix, "lib")
    want = [n for n in _otool_leaves(FF)
            if token in n and not any(a in n for a in avoid)]
    cands = [os.path.join(libdir, n) for n in want]
    # Fallback for a binary otool cannot read, or a linker that recorded a path
    # this prefix does not spell the same way.
    for pat in (f"lib{token}.*.dylib", f"lib{token}.dylib", f"lib{token}.so.*"):
        cands += [p for p in sorted(glob.glob(os.path.join(libdir, pat)))
                  if not any(a in os.path.basename(p) for a in avoid)]
    for c in cands:
        if os.path.exists(c):
            return os.path.realpath(c)
    return None

def _pc_version(prefix, name):
    """The library's own version string, where one is cheap to read."""
    try:
        for ln in open(os.path.join(prefix, "lib", "pkgconfig", name + ".pc")):
            if ln.lower().startswith("version:"):
                return ln.split(":", 1)[1].strip()
    except OSError:
        pass
    return "?"

def _lib_identity(varname, prefix, token, pcname, avoid=()):
    lib = _dylib_under(prefix, token, avoid)
    if not lib:
        sys.exit(f"ffboard: {varname}={prefix} holds no lib{token} shared "
                 f"library, so the board cannot say what it measured. "
                 f"Looked in {os.path.join(prefix, 'lib')}")
    return {"var": varname, "prefix": prefix, "dylib": lib,
            "sha": _sha12(lib), "version": _pc_version(prefix, pcname)}

# The header the ffmpeg's wrapper was compiled against, recorded beside the
# binary at build time. See docs/ffmpeg-integration-plan.md; one line of the
# recipe writes it.
HEADER_REC   = os.path.join(os.path.dirname(os.path.abspath(FF)),
                            "yah264-header.sha")
HEADER_CHECK = os.environ.get("Y264_HEADER_CHECK", "1") != "0"

IDENT = None        # filled by identify(), which preflight() calls
BOARD_ID = "?"      # the short form the cache key and the log carry

def identify():
    """Hash the three binaries and check the wrapper's header against the
    library's. Returns nothing; sets IDENT and BOARD_ID."""
    global IDENT, BOARD_ID
    y = _lib_identity("Y264LIB", Y264LIB, "yah264", "yah264", avoid=("yah264_10",))
    x = _lib_identity("X264LIB", X264LIB, "x264", "x264", avoid=("yah264",))
    ff = {"var": "FF", "prefix": "", "dylib": os.path.abspath(FF),
          "sha": _sha12(FF), "version": ""}
    IDENT = {"ff": ff, "y264": y, "x264": x}
    BOARD_ID = hashlib.sha256(
        f"{ff['sha']}|{y['sha']}:{y['version']}|{x['sha']}:{x['version']}"
        .encode()).hexdigest()[:12]
    header_gate(y)

def header_gate(y):
    """Refuse a Y264LIB whose public header is not the one the ffmpeg's wrapper
    was built against.

    The wrapper reads yah264_param_t by offset, so a header that has moved a
    field is a wrapper that writes the wrong fields, and neither the loader nor
    the encoder complained about it before ABI 3. This is the check that does
    not need the library to cooperate: it compares what the ffmpeg was compiled
    against with what is installed beside the dylib it will load."""
    inst = os.path.join(y["prefix"], "include", "yah264.h")
    if not HEADER_CHECK:
        print("  [warn] Y264_HEADER_CHECK=0: the wrapper's header is NOT being "
              "checked against the library's. A mismatch here encodes nonsense "
              "quietly; do not publish a number from this run.", flush=True)
        return
    if not os.path.exists(HEADER_REC):
        sys.exit(
            f"ffboard: {FF} has no record of the yah264 header it was built "
            f"against ({HEADER_REC} is missing), so this board cannot tell a "
            "current wrapper from one a field behind the library.\n"
            "  Write it where the ffmpeg was built, from the yah264 checkout "
            "that produced the install:\n"
            f"    git hash-object include/yah264.h > {HEADER_REC}\n"
            "  and rebuild the ffmpeg if the header has moved since. The "
            "recipe is in docs/ffmpeg-integration-plan.md.")
    if not os.path.exists(inst):
        sys.exit(f"ffboard: {y['var']}={y['prefix']} installs no "
                 f"include/yah264.h, so the wrapper's header cannot be "
                 "checked against it.")
    rec = open(HEADER_REC).read().split()
    built = rec[0] if rec else ""
    have = _blob_sha(inst)
    if built != have:
        sys.exit(
            "ffboard: this ffmpeg's wrapper was built against a DIFFERENT "
            "include/yah264.h than the library it would load, and the two "
            "disagree about yah264_param_t's layout.\n"
            f"  ffmpeg  {os.path.abspath(FF)}\n"
            f"    built against header blob {built}  (recorded in {HEADER_REC})\n"
            f"  library {y['dylib']}\n"
            f"    installs header blob      {have}  ({inst})\n"
            "  Rebuild the ffmpeg against this install (docs/ffmpeg-"
            "integration-plan.md), or point Y264LIB at the install the ffmpeg "
            "was built against. A board taken across this gap is the "
            "2026-09-19 one: it runs, and it is nonsense.")

def identity_lines():
    y, x, ff = IDENT["y264"], IDENT["x264"], IDENT["ff"]
    return [f"  binaries [{BOARD_ID}]  ffmpeg {ff['sha']}  "
            f"{os.path.basename(y['dylib'])} {y['sha']} v{y['version']}  "
            f"{os.path.basename(x['dylib'])} {x['sha']} v{x['version']}",
            f"  {'':<11}  yah264 {y['prefix']}",
            f"  {'':<11}  x264   {x['prefix']}"]

# Our encoder answers to both names: a library installed before the yah264
# rename still reads the NEXT264_ spelling. Match either and set both, because
# a knob that silently misses leaves us vectorized while the reference goes
# scalar, and that reads as a win rather than as an error.
OURS = ("libyah264", "libnext264")

def env(under_test):
    e = dict(os.environ, DYLD_LIBRARY_PATH=f"{X264LIB}/lib:{Y264LIB}/lib")
    e.pop("YAH264_NO_ASM", None)
    e.pop("NEXT264_NO_ASM", None)
    if under_test and NOASM:
        if ENC in OURS:
            e["YAH264_NO_ASM"] = "1"
            e["NEXT264_NO_ASM"] = "1"
        else:
            print(f"ffboard: NOASM=1 does not reach {ENC}, which has no scalar "
                  "switch; only the reference is pure-C on these rows",
                  file=sys.stderr)
    return e

# A failed encode is the dangerous failure here, not a loud one. Every command
# runs with its output discarded, so a rejected flag or a missing encoder used to
# return in milliseconds, leave the previous run's file on disk, and be timed as
# a very fast encode against a stale size. The guard belongs on the PRODUCER:
# checking that two outputs match, or that a size looks plausible, cannot tell a
# real result from two empty files.
MIN_OUT = int(os.environ.get("MIN_OUT_BYTES", "128"))

class EncodeFailed(RuntimeError):
    pass

def sh(cmd, e=None, out=None):
    r = subprocess.run(cmd, env=e or env(False),
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise EncodeFailed(
            f"command failed ({r.returncode}): {' '.join(cmd[:9])} ...\n"
            f"    {r.stderr.decode(errors='replace').strip()[:400]}")
    if out is not None:
        sz = os.path.getsize(out) if os.path.exists(out) else 0
        if sz < MIN_OUT:
            raise EncodeFailed(
                f"wrote {sz} bytes to {out}, under the {MIN_OUT}-byte floor: "
                f"treat as a failed encode, not a small one.\n"
                f"    {' '.join(cmd[:9])} ...")
    return r

def probe(clip):
    """frame rate and frame count as ffmpeg sees them."""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=r_frame_rate,nb_frames", "-of", "json", f"{CORP}/{clip}.y4m"],
        capture_output=True, text=True).stdout
    st = json.loads(out)["streams"][0]
    num, den = st["r_frame_rate"].split("/")
    fps = float(num) / float(den)
    have = int(st.get("nb_frames") or 0)
    return fps, have

# ---------------------------------------------------------------------------
# Timing, following scripts/perf-comp.sh rather than inventing a second method.
#
# A CIF cell at 12 threads is one scheduling episode: the first version of this
# harness printed 0.07-0.14 s rows and a ratio built on them is noise. So each
# SAMPLE is k back-to-back executions divided by k, with k picked by a single
# untimed calibration run so a sample lasts at least REPEAT_FLOOR seconds. The
# 720p cells calibrate to k=1 and are unaffected. The calibration run doubles as
# a cache warmup.
#
# The two arms are also interleaved run by run with a mirrored order, so a
# minute of box drift lands on both rather than on whichever went first.

def calibrate(fn):
    t0 = time.perf_counter()
    fn()                                    # untimed: calibration + warmup
    cal = time.perf_counter() - t0
    return max(1, min(10, math.ceil(FLOOR / max(cal, 1e-6))))

# Wall AND cpu, because a wall ratio alone cannot tell "slower" from "doing more
# work" -- and on 2026-08-26 that distinction was the whole finding: at the auto
# thread budget we occupy MORE cores than x264 on bbb_720p and still lose, because
# we spend 1.5x the CPU. A scaling story was written and then withdrawn for want
# of this column. RUSAGE_CHILDREN accumulates over reaped children, so deltas
# around the runs are the per-cell cost; every encode here is a subprocess.
def sample(fn, k):
    c0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.perf_counter()
    for _ in range(k):
        fn()
    wall = (time.perf_counter() - t0) / k
    c1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = ((c1.ru_utime - c0.ru_utime) + (c1.ru_stime - c0.ru_stime)) / k
    return wall, cpu

def median(v):
    v = sorted(v)
    return v[len(v)//2] if len(v) % 2 else (v[len(v)//2-1] + v[len(v)//2]) / 2

def med2(pairs):
    """median wall and median cpu, taken independently over the samples."""
    return median([w for w, _ in pairs]), median([c for _, c in pairs])

def spread_warn(name, clip, ts, k):
    ts = [t for t, _ in ts] if ts and isinstance(ts[0], tuple) else ts
    if len(ts) > 1 and min(ts) > 0 and max(ts) / min(ts) > 1.15:
        print(f"    [warn] {clip} {name}: wall spread {max(ts)/min(ts):.2f}x over "
              f"{len(ts)} samples ({min(ts):.3f}-{max(ts):.3f}s, repeat={k}) -- "
              f"box may be loaded; treat this row as unreliable", flush=True)

# ---------------------------------------------------------------------------
# CRF at a matched operating point (scripts/perf-comp-crf-set.sh).
#
# "CRF 25 vs CRF 25" is two different operating points -- the two scales are
# tens of percent apart in achieved size -- so a speed ratio taken there times
# two encoders doing different amounts of work. Instead each encoder is solved
# onto a common ACHIEVED bitrate: yah264 first, onto the clip's target, then
# x264 onto whatever yah264 actually landed on.
#
# yah264's rate factor rounds to an integer frame QP, so its reachable rates
# are a staircase and the solve lands on a rung rather than the target exactly.
# That is why x264 is solved onto yah264's achieved rate and not onto the
# target: the pair has to match each other, not the nominal number.

# The cache survives between boards and between WEEKS, which is what makes it
# worth having and what made it dangerous. Every key is prefixed with BOARD_ID,
# the hash of the three binaries, so an entry produced by another build of
# either library is not merely stale, it is unreachable. Entries from before
# this change carry no prefix and are likewise never matched again.
SOLVE_CACHE = f"{CACHE}/solve.json"

def _cache():
    if NO_SOLVE_CACHE:
        return {}
    try:
        with open(SOLVE_CACHE) as f:
            return json.load(f)
    except Exception:
        return {}

def _cache_put(key, val):
    if NO_SOLVE_CACHE:
        return
    c = _cache(); c[key] = val
    with open(SOLVE_CACHE, "w") as f:
        json.dump(c, f)

def _cache_say(hit, what):
    """One line per solve. A board that reused a whole table of cached answers
    used to look exactly like a board that solved them, which is how the
    2026-09-18 one was read as a result for most of a day."""
    if NO_SOLVE_CACHE:
        print(f"    solve cache: off  [{BOARD_ID}] {what}", flush=True)
    else:
        print(f"    solve cache: {'hit ' if hit else 'miss'} "
              f"[{BOARD_ID}] {what}", flush=True)

def kbps_of(size, frames, fps):
    return size * 8 / (frames / fps) / 1000

SOLVE_X264LIB = os.environ.get("SOLVE_X264LIB", X264LIB)
# Solve AT the configuration being measured. The old default solved once at 12
# threads and shared the answer with every board, on the recorded assumption that
# achieved size moves 0.25-0.43% across thread counts. That assumption FAILS:
# bbb_720p moves 5.3% between t8 and auto, and at --threads 1 yah264 switches
# into single-thread quality mode (stq), which is a different encoder. A shared
# solve there leaves the arms rate-MISMATCHED and the ratio is then partly an
# operating-point artifact -- one row read +5.2% dSIZE before this change and
# +0.3% after. Cost is contained by seeding the bisect (see solve()).
SOLVE_THREADS = os.environ.get("SOLVE_THREADS", THREADS)

def solve_env():
    e = dict(os.environ, DYLD_LIBRARY_PATH=f"{SOLVE_X264LIB}/lib:{Y264LIB}/lib")
    e.pop("YAH264_NO_ASM", None)
    return e

# Rate-match tolerance, and it is a CORRECTNESS parameter, not a speed one.
# A GOAL leg is decided at +/-0.01, and a 1% bit difference is worth roughly 1%
# of wall, so a 1.5% tolerance can be larger than the margin being claimed. It
# was: at 1.5% this board read G3 median 0.98-1.00x, and the same board at 0.4%
# read 1.01x on two runs. The difference was x264 overshooting the rate on the
# loose rows and being timed doing more work for it (foreman dSIZE -1.1% ->
# -0.1%). 0.5% keeps every row inside +/-0.3% achieved, which is comfortably
# under the margin the goals turn on. Cost is a few more bisect steps, which
# the seeding above mostly absorbs.
SOLVE_TOL = float(os.environ.get("Y264_SOLVE_TOL", "0.005"))

def solve(codec, clip, frames, fps, target, tol=None, iters=18):
    tol = SOLVE_TOL if tol is None else tol
    """Bisect CRF until achieved bitrate is within tol of target, AT SOLVE_THREADS."""
    key = (f"{BOARD_ID}:{codec}:{clip}:{frames}:{target:.1f}:{PRESET}"
           f":t{SOLVE_THREADS}:x{tol}")
    what = f"crf {codec} {clip} -> {target:.1f} kbps t{SOLVE_THREADS}"
    c = _cache()
    if key in c:
        _cache_say(True, what)
        return tuple(c[key])
    _cache_say(False, what)
    # Seed from any solve already done for this clip/target at another thread
    # count. Thread count moves the achieved rate by a few percent, not by
    # octaves, so a +/-2.5 CRF bracket around the known answer holds in practice
    # and turns a 14-step bisect into ~4. If it does not hold -- the bracket
    # converges to an edge without meeting tol -- the full range is re-run, so
    # seeding can only cost time, never correctness.
    seed = None
    pre = f"{BOARD_ID}:{codec}:{clip}:{frames}:{target:.1f}:{PRESET}:t"
    for k, v in c.items():
        if k.startswith(pre):
            seed = v[0]; break
    e = solve_env()
    out = f"{WD}/solve.264"
    for lo, hi in ([(seed - 2.5, seed + 2.5), (8.0, 45.0)] if seed
                   else [(8.0, 45.0)]):
        lo, hi = max(8.0, lo), min(45.0, hi)
        best = _bisect(codec, clip, frames, fps, target, tol, iters, lo, hi, e, out)
        if best and abs(best[1] - target) / target < tol:
            break
    _cache_put(key, list(best))
    return best

def _bisect(codec, clip, frames, fps, target, tol, iters, lo, hi, e, out):
    best = None
    for _ in range(iters):
        mid = (lo + hi) / 2
        sh([FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
            "-frames:v", str(frames), "-c:v", codec, "-preset", PRESET,
            "-crf", f"{mid:.3f}", "-threads", SOLVE_THREADS, "-f", "h264", out], e, out)
        k = kbps_of(os.path.getsize(out), frames, fps)
        if best is None or abs(k - target) < abs(best[1] - target):
            best = (mid, k)
        if abs(k - target) / target < tol:
            best = (mid, k)
            break
        if k > target:
            lo = mid                     # too many bits -> raise CRF
        else:
            hi = mid
    return best

def measure_crf(clip, frames, fps, target):
    ncrf, nk = solve(ENC, clip, frames, fps, target)
    xcrf, xk = solve("libx264",    clip, frames, fps, nk)

    base_cmd = [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
                "-frames:v", str(frames), "-f", "null", "-"]
    def enc_cmd(codec, crf, out):
        return [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
                "-frames:v", str(frames), "-c:v", codec, "-preset", PRESET,
                "-crf", f"{crf:.3f}", "-threads", THREADS, "-f", "h264", out]

    fb = lambda: sh(base_cmd)
    fn = lambda: sh(enc_cmd(ENC, ncrf, f"{WD}/n.264"), env(True),  f"{WD}/n.264")
    fx = lambda: sh(enc_cmd("libx264",    xcrf, f"{WD}/x.264"), env(False), f"{WD}/x.264")
    if TIMED_NULL:
        fn(); fx()          # the sized and scored files, produced once, untimed
        fn = lambda: sh(enc_cmd(ENC, ncrf, "/dev/null"), env(True))
        fx = lambda: sh(enc_cmd("libx264",    xcrf, "/dev/null"), env(False))
    kb = calibrate(fb)
    if ORDER_X_FIRST: kx = calibrate(fx); kn = calibrate(fn)
    else:             kn = calibrate(fn); kx = calibrate(fx)
    bs, ns, xs = [], [], []
    for i in range(RUNS):
        bs.append(sample(fb, kb))
        if (i % 2 == 0) != ORDER_X_FIRST:
            ns.append(sample(fn, kn)); xs.append(sample(fx, kx))
        else:
            xs.append(sample(fx, kx)); ns.append(sample(fn, kn))
    spread_warn(ENC,    clip, ns, kn)
    spread_warn("x264", clip, xs, kx)
    bw, bc = med2(bs); nw, nc = med2(ns); xw, xc = med2(xs)
    return (nw - bw, xw - bw, nc - bc, xc - bc,
            os.path.getsize(f"{WD}/n.264"), os.path.getsize(f"{WD}/x.264"),
            ncrf, xcrf, nk)

def solve_abr(codec, clip, frames, fps, target, tol=None, iters=18):
    """Bisect the ABR TARGET until the ACHIEVED bitrate is within tol of target.

    RC=abr hands both encoders the same target and times them there, which is
    the mode as a user meets it but is NOT a speed measurement: the two land on
    different achieved rates (dsize runs about 2.9% on the standing board
    against a 1.0% bar), and emitting more bits costs time, so the ratio partly
    reports which encoder spent less. RC=abrm removes that term the same way
    RC=crf does, by putting both encoders on the same achieved bitrate.

    Achieved rate is monotone in the requested one, so the same bisection the
    CRF path uses works here with the target as the variable."""
    tol = SOLVE_TOL if tol is None else tol
    key = (f"{BOARD_ID}:abr:{codec}:{clip}:{frames}:{target:.1f}:{PRESET}"
           f":t{SOLVE_THREADS}:x{tol}")
    c = _cache()
    what = f"abr {codec} {clip} -> {target:.1f} kbps t{SOLVE_THREADS}"
    if key in c:
        _cache_say(True, what)
        return tuple(c[key])
    _cache_say(False, what)
    e = solve_env()
    out = f"{WD}/solve.264"
    lo, hi = target * 0.35, target * 3.0
    best = None
    for _ in range(iters):
        mid = (lo + hi) / 2
        sh([FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
            "-frames:v", str(frames), "-c:v", codec, "-preset", PRESET,
            "-b:v", f"{mid:.0f}k", "-threads", SOLVE_THREADS, "-f", "h264", out], e, out)
        k = kbps_of(os.path.getsize(out), frames, fps)
        if best is None or abs(k - target) < abs(best[1] - target):
            best = (mid, k)
        if abs(k - target) / target < tol:
            best = (mid, k)
            break
        if k > target:
            hi = mid                     # too many bits -> ask for fewer
        else:
            lo = mid
    _cache_put(key, list(best))
    return best


def measure_abrm(clip, frames, fps, kbps):
    """ABR at a MATCHED ACHIEVED bitrate.

    yah264 runs at the clip's own target, which is what ABR means for it, and
    x264's target is then solved so that x264 lands on whatever yah264 actually
    achieved. The asymmetry mirrors measure_crf and for the same reason: the
    pair has to match each other, not the nominal number.

    What survives after the bit term is removed is a real ABR speed reading, and
    it need not equal the CRF one -- ABR runs a rate-control feedback loop that
    CRF does not."""
    e = solve_env()
    out = f"{WD}/solve.264"
    sh([FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
        "-frames:v", str(frames), "-c:v", ENC, "-preset", PRESET,
        "-b:v", f"{kbps}k", "-threads", SOLVE_THREADS, "-f", "h264", out], e, out)
    nk = kbps_of(os.path.getsize(out), frames, fps)
    xb, xk = solve_abr("libx264", clip, frames, fps, nk)

    base_cmd = [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
                "-frames:v", str(frames), "-f", "null", "-"]
    def enc_cmd(codec, br, out):
        return [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
                "-frames:v", str(frames), "-c:v", codec, "-preset", PRESET,
                "-b:v", f"{br:.0f}k", "-threads", THREADS, "-f", "h264", out]

    fb = lambda: sh(base_cmd)
    fn = lambda: sh(enc_cmd(ENC,       float(kbps), f"{WD}/n.264"), env(True),  f"{WD}/n.264")
    fx = lambda: sh(enc_cmd("libx264", xb,          f"{WD}/x.264"), env(False), f"{WD}/x.264")
    if TIMED_NULL:
        fn(); fx()
        fn = lambda: sh(enc_cmd(ENC,       float(kbps), "/dev/null"), env(True))
        fx = lambda: sh(enc_cmd("libx264", xb,          "/dev/null"), env(False))
    kb = calibrate(fb)
    if ORDER_X_FIRST: kx = calibrate(fx); kn = calibrate(fn)
    else:             kn = calibrate(fn); kx = calibrate(fx)
    bs, ns, xs = [], [], []
    for i in range(RUNS):
        bs.append(sample(fb, kb))
        if (i % 2 == 0) != ORDER_X_FIRST:
            ns.append(sample(fn, kn)); xs.append(sample(fx, kx))
        else:
            xs.append(sample(fx, kx)); ns.append(sample(fn, kn))
    spread_warn(ENC,    clip, ns, kn)
    spread_warn("x264", clip, xs, kx)
    bw, bc = med2(bs); nw, nc = med2(ns); xw, xc = med2(xs)
    return (nw - bw, xw - bw, nc - bc, xc - bc,
            os.path.getsize(f"{WD}/n.264"), os.path.getsize(f"{WD}/x.264"),
            float(kbps), xb, nk)


def measure(clip, frames, kbps):
    """Baseline and both encodes, interleaved. Returns (n_secs, x_secs, n_size,
    x_size), the encode times already net of the decode-only baseline.

    The baseline is startup plus demux plus decode with no encoder attached.
    Both arms pay it, so subtracting it stops it flattering whichever encoder is
    faster. It is small -- 0.009s for 180 CIF frames, 0.012s for 120 of 720p --
    but it has to be checked rather than assumed, because it is an input to
    every row. This build is --disable-everything, and `-f null -` needs the
    wrapped_avframe encoder to consume the decoded frames; without it the
    command exits 8 in 5ms. A runner that ignored exit codes subtracted those
    5ms of failure from both arms as if it were the decode cost, which quietly
    pulled every ratio toward parity. preflight() proves it runs first."""
    base_cmd = [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
                "-frames:v", str(frames), "-f", "null", "-"]
    def enc_cmd(codec, out):
        c = [FF, "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
             "-frames:v", str(frames), "-c:v", codec]
        if codec != "libopenh264":          # openh264 has no preset ladder
            c += ["-preset", PRESET]
        return c + ["-b:v", f"{kbps}k", "-threads", THREADS, "-f", "h264", out]

    fb = lambda: sh(base_cmd)
    fn = lambda: sh(enc_cmd(ENC,       f"{WD}/n.264"), env(True),  f"{WD}/n.264")
    fx = lambda: sh(enc_cmd("libx264", f"{WD}/x.264"), env(False), f"{WD}/x.264")
    if TIMED_NULL:
        fn(); fx()
        fn = lambda: sh(enc_cmd(ENC,       "/dev/null"), env(True))
        fx = lambda: sh(enc_cmd("libx264", "/dev/null"), env(False))

    kb, kn, kx = calibrate(fb), calibrate(fn), calibrate(fx)
    bs, ns, xs = [], [], []
    for i in range(RUNS):
        bs.append(sample(fb, kb))
        if (i % 2 == 0) != ORDER_X_FIRST:
            ns.append(sample(fn, kn)); xs.append(sample(fx, kx))
        else:
            xs.append(sample(fx, kx)); ns.append(sample(fn, kn))
    spread_warn(ENC,    clip, ns, kn)
    spread_warn("x264", clip, xs, kx)
    bw, bc = med2(bs); nw, nc = med2(ns); xw, xc = med2(xs)
    return (nw - bw, xw - bw, nc - bc, xc - bc,
            os.path.getsize(f"{WD}/n.264"), os.path.getsize(f"{WD}/x.264"))

def vmaf_neg(clip, frames, bitstream):
    """Decode and score against the same frames of the source. VMAF-NEG because
    that is what the quality gate uses, and PSNR-Y alongside it because the
    floor (2026-09-14, owner) is read at equal bytes on the same frames.

    `--feature psnr` is one more extractor on a pass that has already decoded
    and read both files, so the second number costs no encode and no decode.
    It is inert for the first: the pooled neg mean is bit-identical with the
    feature on and off (checked to nine decimals), so the dVMAF column
    reproduces to the digit across the change that added this.
    """
    ref, dec = f"{WD}/ref.y4m", f"{WD}/dec.y4m"
    if not os.path.exists(ref):
        sh(["ffmpeg", "-v", "error", "-y", "-i", f"{CORP}/{clip}.y4m",
            "-frames:v", str(frames), "-pix_fmt", "yuv420p", ref])
    sh(["ffmpeg", "-v", "error", "-y", "-i", bitstream, "-pix_fmt", "yuv420p", dec])
    j = f"{WD}/v.json"
    sh([VMAF, "-r", ref, "-d", dec, "--json", "-o", j, "--feature", "psnr",
        "--model", "version=vmaf_v0.6.1neg:name=neg"])
    try:
        with open(j) as f:
            pooled = json.load(f)["pooled_metrics"]
        return pooled["neg"]["mean"], pooled["psnr_y"]["mean"]
    except Exception:
        return None, None

def thread_honoured_note():
    """Not run automatically, recorded because it is the check nobody thinks to
    make. `-threads` is handed to ffmpeg, not to the encoder, and a wrapper is
    free to ignore it: ffmpeg's stock libsvtav1 does, which reads as a large
    plumbing win until someone looks at user/real. Verified 2026-08-26 that both
    encoders here honour it -- libyah264 and libx264 both read user/real 0.99 at
    -threads 1, so the single-threaded rows are genuinely serial on both sides.
    Re-run it with /usr/bin/time -p after any wrapper change:

        libyah264  -threads 1   user/real 0.99      -threads 12  10.89
        libx264     -threads 1   user/real 0.99      -threads 12   6.61

    The 12-thread pair is worth keeping in view for its own sake: we occupy 65%
    more cores and burn 2.0x the CPU to finish 1.24x slower in wall time, which
    is goal 3's gap stated as a mechanism rather than a ratio."""

def preflight():
    missing = [n for n, v in (("X264LIB", X264LIB), ("Y264LIB", Y264LIB)) if not v]
    if missing:
        sys.exit(f"ffboard: set {' and '.join(missing)} to the library install prefix(es)")
    if not os.path.exists(FF):
        sys.exit(f"ffboard: no ffmpeg at {FF} -- set FF (see docs/ffmpeg-integration-plan.md)")
    encs = subprocess.run([FF, "-hide_banner", "-encoders"], capture_output=True,
                          text=True).stdout
    for name in (ENC, "libx264"):
        if name not in encs:
            sys.exit(f"ffboard: {FF} has no {name} encoder -- point FF at the build "
                     "from docs/ffmpeg-integration-plan.md "
                     f"({os.path.join(_FFB_ROOT, 'ffmpeg-yah264', 'ffmpeg')})")
    if RC not in ("crf", "abr", "abrm"):
        sys.exit(f"ffboard: RC must be crf, abr or abrm, got '{RC}'")
    # Hash the three binaries and gate the wrapper's header against the
    # library's, before any encode runs and so before the cache is touched.
    identify()
    # The baseline is a measurement input, so prove it runs before trusting any
    # row that subtracts it.
    probe_clip = next((c for c, _ in CLIPS
                       if os.path.exists(os.path.join(CORP, c + ".y4m"))), None)
    if probe_clip:
        try:
            sh([FF, "-v", "error", "-y", "-i", os.path.join(CORP, probe_clip + ".y4m"),
                "-frames:v", "2", "-f", "null", "-"])
        except EncodeFailed as e:
            sys.exit("ffboard: the decode-only baseline does not run, so every row "
                     f"would subtract a failure.\n  {e}\n  Rebuild ffmpeg with "
                     "--enable-encoder=wrapped_avframe.")
    if ENC == "libopenh264" and RC == "crf":
        sys.exit("ffboard: openh264 exposes no quality knob through ffmpeg, so it "
                 "cannot be solved onto a matched point. Run it at RC=abr, and "
                 "board every row of that table the same way.")

# ---------------------------------------------------------------------------
# The size-match guard.
#
# Every row of a crf or abrm board is a RATE-MATCHED pair by construction: the
# solve puts both encoders on the same achieved bitrate and the ratio is then a
# speed reading. When the solve does not do that, nothing downstream notices.
# The dsize column goes double digits, the wall ratio compares two encoders
# doing different amounts of work, and the median prints in the same format it
# always does. That is the 2026-09-18 board: dsize -12% on the median, -24 to
# -37% on five clips, and a number that was read as a result for most of a day.
#
# So the board refuses to hand over a median it cannot stand behind. Two bars,
# because they catch different failures: the MEDIAN bar catches a scale that
# has moved under the whole table, and the PER-CLIP bar catches the one clip
# whose solve did not converge inside a table that otherwise matched.
#
# RC=abr is exempt and says so. It hands both encoders the same target and does
# NOT deliver the same bits -- that is the mode as a user meets it, the board's
# own docstring says the ratio partly reports which encoder spent less, and the
# n rate / x rate columns are printed for exactly that reason. Setting the env
# bars explicitly turns the guard on there too.
DSIZE_TOL     = float(os.environ.get("Y264_DSIZE_TOL", "2.0"))
DSIZE_TOL_MAX = float(os.environ.get("Y264_DSIZE_TOL_MAX", "5.0"))
DSIZE_GUARD   = RC in ("crf", "abrm") or "Y264_DSIZE_TOL" in os.environ

def size_guard(rows):
    """rows: [(clip, dsize_pct)]. Prints and exits non-zero if unmatched."""
    if not rows:
        return
    if not DSIZE_GUARD:
        print(f"  size-match guard: not applied at RC={RC}, which does not "
              "match bits by construction (read the n rate / x rate columns)",
              flush=True)
        return
    med = median([d for _, d in rows])
    worst = max(rows, key=lambda cd: abs(cd[1]))
    bad_med = abs(med) > DSIZE_TOL
    bad_one = abs(worst[1]) > DSIZE_TOL_MAX
    if not (bad_med or bad_one):
        print(f"  size-match guard: OK, dsize median {med:+.1f}% "
              f"(bar +/-{DSIZE_TOL:g}%), worst {worst[0]} {worst[1]:+.1f}% "
              f"(bar +/-{DSIZE_TOL_MAX:g}%)", flush=True)
        return
    over = [f"{c} {d:+.1f}%" for c, d in rows if abs(d) > DSIZE_TOL_MAX]
    bar = "  " + "=" * 100
    print(bar)
    print("  INVALID: sizes unmatched -- this table is NOT a speed reading")
    print(f"  dsize median {med:+.1f}% against a +/-{DSIZE_TOL:g}% bar; "
          f"worst clip {worst[0]} {worst[1]:+.1f}% against +/-{DSIZE_TOL_MAX:g}%")
    if over:
        print("  outside the per-clip bar: " + ", ".join(over))
    print("  The two encoders were timed at different operating points, so "
          "every ratio above")
    print("  is partly the bit difference. Do not quote the median. The solve "
          "is where to look:")
    print("    - re-run with --no-solve-cache, which is the failure this "
          "board has actually had")
    print("    - check Y264_SOLVE_TOL and that the bisect converged "
          "(SOLVE_THREADS == THREADS)")
    print(bar, flush=True)
    sys.exit(2)

def main():
    preflight()
    tier = 'pure-C' if NOASM else 'SIMD'
    label = f"{ENC} vs libx264, {tier}, {THREADS} thread{'' if THREADS=='1' else 's'}"
    print(f"  {label}   rc={RC}  window={SECONDS:g}s  preset={PRESET}  "
          f"median of {RUNS} samples, {FLOOR:g}s repeat floor, "
          f"{'x264' if ORDER_X_FIRST else ENC.replace('lib','')} first")
    for ln in identity_lines():
        print(ln)
    enc_col = ENC.replace("lib", "")[:6]
    if RC == "crf":
        print(f"  {'clip':<16}{'kbps':>8}{'n crf':>7}{'x crf':>7}{enc_col+' s':>9}"
              f"{'x264 s':>9}{'x264 x':>9}{'work':>7}{'cores':>12}{'dVMAF':>8}{'dsize':>8}"
              f"{'dPSNR-Y':>9}")
        print("  " + "-" * 100)
    elif RC == "abrm":
        # "kbps" is the ACHIEVED rate both encoders were put on; the two target
        # columns are what each had to be ASKED for to land there. dsize is the
        # proof the match worked and should sit near zero -- if it does not, the
        # solve did not converge and the row is not a speed reading.
        print(f"  {'clip':<16}{'kbps':>8}{'n targ':>7}{'x targ':>7}{enc_col+' s':>9}"
              f"{'x264 s':>9}{'x264 x':>9}{'work':>7}{'cores':>12}{'dVMAF':>8}{'dsize':>8}"
              f"{'dPSNR-Y':>9}")
        print("  " + "-" * 100)
    else:
        print(f"  {'clip':<16}{'kbps':>7}{enc_col+' s':>9}{'x264 s':>9}{'x264 x':>9}"
              f"{'work':>7}{'cores':>12}{'dVMAF':>8}{'dsize':>8}{'dPSNR-Y':>9}"
              f"{'n rate':>8}{'x rate':>8}")
        print("  " + "-" * 100)
    ratios, dvs, dss, works, dps, dsc = [], [], [], [], [], []
    for clip, kbps in CLIPS:
        path = f"{CORP}/{clip}.y4m"
        if not os.path.exists(path):
            print(f"  {clip:<16}{'(missing)':>7}")
            continue
        fps, have = probe(clip)
        frames = int(round(SECONDS * fps))
        if have:
            frames = min(frames, have)
        for f in (f"{WD}/ref.y4m",):
            if os.path.exists(f):
                os.remove(f)
        if RC == "crf":
            nt, xt, ncpu, xcpu, nsz, xsz, ncrf, xcrf, nk = \
                measure_crf(clip, frames, fps, kbps)
        elif RC == "abrm":
            nt, xt, ncpu, xcpu, nsz, xsz, ncrf, xcrf, nk = \
                measure_abrm(clip, frames, fps, kbps)
        else:
            nt, xt, ncpu, xcpu, nsz, xsz = measure(clip, frames, kbps)
        nv, npy = vmaf_neg(clip, frames, f"{WD}/n.264")
        xv, xpy = vmaf_neg(clip, frames, f"{WD}/x.264")
        r  = nt / xt
        dv = (nv - xv) if (nv is not None and xv is not None) else float("nan")
        ds = 100.0 * (nsz / xsz - 1)
        # PSNR-Y at equal bytes: a FLOOR, not a decision metric. Every row of
        # this board is already rate-matched, so the difference is read at the
        # same bits by construction. The verdict is adjudicated by
        # scripts/psnr_leg.py at the foot, on the SAME rule the CLI boards use.
        dp = (npy - xpy) if (npy is not None and xpy is not None) else float("nan")
        # Rate error against the target, both sides, because dsize alone reads
        # as an efficiency result when it is often just one encoder missing the
        # target. x264's ABR undershoots high-motion CIF badly enough that a
        # +10% dsize row is x264 spending less, not yah264 spending more.
        # work = the CPU-seconds ratio, cores = occupancy each side achieved.
        # Read them together: a wall ratio near 1.00 built on work 1.5x means we
        # are only level because we are occupying more of the machine, and it
        # will regress the moment the reference threads better.
        w  = (ncpu / xcpu) if xcpu > 0 else float("nan")
        nco = ncpu / nt if nt > 0 else float("nan")
        xco = xcpu / xt if xt > 0 else float("nan")
        cores = f"{nco:.1f}/{xco:.1f}"
        ratios.append(r); dvs.append(dv); dss.append(ds); works.append(w)
        dps.append((clip, dp)); dsc.append((clip, ds))
        if RC in ("crf", "abrm"):
            # crf prints rate factors to 2dp; abrm prints kbit/s targets, where
            # a decimal would be noise.
            fmt = "{:>7.0f}" if RC == "abrm" else "{:>7.2f}"
            print(f"  {clip:<16}{nk:>8.0f}{fmt.format(ncrf)}{fmt.format(xcrf)}"
                  f"{nt:>9.2f}{xt:>9.2f}{r:>8.2f}x{w:>6.2f}x{cores:>12}"
                  f"{dv:>+8.2f}{ds:>+7.1f}%{dp:>+9.2f}", flush=True)
        else:
            secs = frames / fps
            nre = 100.0 * ((nsz * 8 / secs / 1000) / kbps - 1)
            xre = 100.0 * ((xsz * 8 / secs / 1000) / kbps - 1)
            print(f"  {clip:<16}{kbps:>7}{nt:>9.2f}{xt:>9.2f}{r:>8.2f}x"
                  f"{w:>6.2f}x{cores:>12}{dv:>+8.2f}{ds:>+7.1f}%{dp:>+9.2f}"
                  f"{nre:>+7.1f}%{xre:>+7.1f}%", flush=True)
    if ratios:
        print("  " + "-" * 100)
        pad = f"{'':>8}{'':>7}{'':>7}{'':>9}{'':>9}" if RC == "crf" \
              else f"{'':>7}{'':>9}{'':>9}"
        # nan is filtered out BEFORE the median, not left for it to survive. A
        # nan does not sort -- it stops wherever the comparisons leave it and
        # drags a neighbour into the middle slot -- so one unscored clip would
        # not blank this column, it would quietly print the wrong clip's number.
        pv = [d for _, d in dps if d == d]
        print(f"  {'MEDIAN':<16}{pad}{median(ratios):>8.2f}x"
              f"{median(works):>6.2f}x{'':>12}{median(dvs):>+8.2f}"
              f"{median(dss):>+7.1f}%"
              + (f"{median(pv):>+9.2f}" if pv else f"{'n/a':>9}"), flush=True)
        print(f"  {'MAX':<16}{pad}{max(ratios):>8.2f}x{max(works):>6.2f}x",
              flush=True)
        # The sizes have to have matched or none of the above is a reading.
        # Checked here, after the table is printed and before the PSNR leg is
        # adjudicated: the rows are worth seeing either way, the verdict is not
        # worth taking at unmatched bytes, and the exit code is what stops the
        # median reaching a page.
        size_guard(dsc)
        # The floor's verdict and its debt list, printed by the one adjudicator
        # both boards share so this board cannot pass a clip the CLI board
        # fails. Rows whose PSNR did not score come through as nan and are
        # dropped by psnr_leg.py rather than poisoning a median.
        subprocess.run(
            [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                          "psnr_leg.py"), "16"]
            + [f"{c}={d:.2f}" if d == d else f"{c}=n/a" for c, d in dps])

main()
