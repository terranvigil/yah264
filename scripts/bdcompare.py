#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Quick BD-rate comparison between two encoder configurations.

Encodes truncated corpus clips at several rate points with both configs (in
parallel), decodes, and reports per-clip BD-rate. PSNR by default (fast,
numpy); --vmaf for the perceptual number when it matters. Meant for the tight
measure-ship-or-revert loop, where the full bench.py run is too slow.

Templates use {src}, {q}, {out}. Examples:

  # working tree vs x264 medium, CRF mode, VMAF
  scripts/bdcompare.py \
      --a './build/cli/yah264 --input-y4m {src} --crf {q} --cabac --bframes 3 --ref 3 --transform-8x8 --threads 8 -o {out}' \
      --b '../x264/x264-noasm --crf {q} --preset medium --keyint 250 --demuxer y4m -o {out} {src}' \
      --points 20,23,26,29 --vmaf

  # feature evaluation: current binary vs a saved baseline binary, CQP PSNR
  scripts/bdcompare.py \
      --a './build/cli/yah264 --input-y4m {src} --qp {q} --cabac --bframes 3 --threads 8 -o {out}' \
      --b './baseline_yah264 --input-y4m {src} --qp {q} --cabac --bframes 3 --threads 8 -o {out}'

BD sign: negative means config A uses fewer bits than B at equal quality.
"""
import argparse
import concurrent.futures as fut
import glob
import hashlib
import json
import math
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scratch                                                  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def decode_raw(path):
    import numpy as np
    p = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
                        "-pix_fmt", "yuv420p", "-"], capture_output=True)
    return np.frombuffer(p.stdout, dtype=np.uint8)


def psnr_of(bitstream, src_raw):
    import numpy as np
    d = decode_raw(bitstream).astype(np.int32)
    n = min(len(d), len(src_raw))
    if n == 0 or len(d) != len(src_raw):
        return None
    mse = np.mean((d - src_raw[:n]) ** 2)
    return 10 * math.log10(255 * 255 / mse)


SUBSAMPLE = 1   # compute VMAF on every Nth frame (set from --subsample); 1 = all


# Sample width of the DECODE handed to libvmaf. 8-bit by default, which is
# every board written before item C3-10bit; a 10-bit board sets it to
# "yuv420p10le" so the distorted side is compared at the reference's own width
# instead of being flattened to 8 bits on the way in, which would score the
# conversion rather than the encoder. y4m needs -strict -1 for a p10 tag.
DEC_PIXFMT = "yuv420p"


def _src_chroma(src_path):
    """The Y4M C tag of a source ("420", "422", "444", with any p10 suffix)."""
    try:
        with open(src_path, "rb") as f:
            head = f.readline(512).decode("ascii", "replace").split()
    except OSError:
        return "420"
    return next((t[1:] for t in head if t.startswith("C")), "420")

# Threads for the two tools this module SHELLS OUT to. Both default to one per
# core, so a harness running J clips abreast is really running J x ncpu threads
# and its own core budget is a fiction: six workers on an 18-core box measured a
# load average of 74. Neither tool's OUTPUT depends on this -- the decode is
# exact and libvmaf's score is thread-invariant -- so pinning them costs a
# scored cell nothing and is how a band stays inside the share it was given.
# Unset leaves both tools on their own defaults, which is every board written
# before this line.
_JOBTHREADS = os.environ.get("VMAF_THREADS", "")
_VMAF_T = f"--threads {_JOBTHREADS}" if _JOBTHREADS else ""
_FF_T = f"-threads {_JOBTHREADS}" if _JOBTHREADS else ""


# Which VMAF models one score runs. "neg" runs VMAF-NEG alone, which is all a
# self-A/B band reads, and costs a little over half of "both": on
# sunflower_1080p, 120 frames, one thread, the two-model pass took 14.9 CPU-s
# and NEG alone 8.5, against 7.3 for the encode it scores. "both" adds the
# plain model (or v1, from YAH264_VMAF_MODEL) and is what the spec report and
# the publication passes keep. NEG's pooled mean is the same number in either
# mode, which is what lets the score cache below serve one mode from the other.
MODELS = os.environ.get("VMAF_MODELS", "both")

# The score cache. A VMAF score is a function of the decoded bitstream and the
# reference, so it is keyed on the bitstream's md5 and the source clip's
# identity, NOT on the binary or the command that produced the stream: a
# lossless rebuild, a knob that turns out inert, or an anchor that every band
# re-encodes all produce a stream already scored, and none of them pays libvmaf
# again. One small file per key under $Y264_CACHE/vmafscore, written by rename,
# so concurrent harnesses can share it. VMAF_SCORE_CACHE=0 bypasses it.
SCORE_CACHE = os.environ.get("VMAF_SCORE_CACHE", "1") != "0"
SCORE_STATS = {"hit": 0, "miss": 0}
_score_dir = None


def _md5_file(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def src_identity(path, extra=""):
    """A source clip's cache identity: its resolved path, size and mtime, so a
    re-fetched or re-cut clip is a different source. `extra` names a cut of it
    (a scratch copy of the first N frames carries the full clip's identity plus
    the frame count, since the copy's own path is new every run)."""
    st = os.stat(path)
    return f"{os.path.realpath(path)}|{st.st_size}|{st.st_mtime_ns}|{extra}"


def _score_path(bitstream, src_path, src_id=None):
    global _score_dir
    if _score_dir is None:
        _score_dir = scratch.cache_dir("vmafscore")
    try:
        sid = src_id or src_identity(src_path)
        md5 = _md5_file(bitstream)
    except OSError:
        return None
    v1 = os.environ.get("YAH264_VMAF_MODEL", "")
    ref = "" if _src_chroma(src_path).startswith("420") else "|ref420"
    ident = f"{md5}|{sid}|sub{SUBSAMPLE}|{DEC_PIXFMT}|{v1}{ref}"
    k = hashlib.sha1(ident.encode()).hexdigest()
    return os.path.join(_score_dir, k[:2], k + ".json")


def _score_get(path, want):
    try:
        with open(path) as f:
            got = json.load(f)
    except (OSError, ValueError):
        return None
    if all(w in got for w in want):
        return {w: got[w] for w in want}
    return None


def _score_put(path, vals):
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        old = {}
        try:
            with open(path) as f:
                old = json.load(f)
        except (OSError, ValueError):
            pass
        old.update(vals)
        tmp = f"{path}.{os.getpid()}.{threading.get_ident()}.tmp"
        with open(tmp, "w") as f:
            json.dump(old, f)
        os.replace(tmp, path)
    except OSError:
        pass


def vmaf_of(bitstream, src_path, work, src_id=None):
    """VMAF v1 and its CAMBI. Under VMAF_GATE=VMAF-NEG, the old VMAF-NEG (and
    plain VMAF in "both") instead, plus v1 when VMAF_V1=1. None when a pass
    that was asked for failed."""
    if GATE == "VMAF-v1":
        return _v1_of(bitstream, src_path, work, src_id)
    out = _v0_of(bitstream, src_path, work, src_id)
    if out is None or not V1:
        return out
    v1 = _v1_of(bitstream, src_path, work, src_id)
    if v1 is None:
        return None
    out.update(v1)
    return out


# VMAF v1 (libvmaf 3.x, model v1.0.16) is the quality score (docs/vmaf.md). It
# sees what v0 cannot: banding (CAMBI is one of its features), chroma,
# blockiness and frame rate, and its detail feature carries VMAF-NEG's
# enhancement-gain cap. It wants a float-enabled libvmaf and SDR scored at
# 10-bit, so both sides go through a 10-bit conversion, and CAMBI is told the
# encode's real bit depth. The model follows the source: the 1080p 3H model
# below 1440 lines, the 4K 1.5H model at and above, and the high-frame-rate
# twin of either at 50 fps and up.
V1 = os.environ.get("VMAF_V1", "0") == "1"
# The score a band or ladder gates on. VMAF-NEG remains only to reproduce
# numbers measured on it; under it, VMAF_V1=1 adds the v1 pass.
GATE = os.environ.get("VMAF_GATE", "VMAF-v1")
if GATE not in ("VMAF-NEG", "VMAF-v1"):
    sys.exit(f"bdcompare: VMAF_GATE '{GATE}' is not VMAF-NEG or VMAF-v1")
V1 = V1 or GATE == "VMAF-v1"
GATE_SHORT = "NEG" if GATE == "VMAF-NEG" else "v1"
# an empty value counts as unset, as a caller passing VAR="" means
V1_BIN = os.environ.get("VMAF_V1_BIN") or os.path.expanduser("~/.local/opt/vmaf-v1/bin/vmaf")
V1_MODELS = os.environ.get("VMAF_V1_MODELS") or os.path.expanduser("~/.local/share/vmaf/model")
V1_VER = "v1.0.16"


def _y4m_geometry(src_path):
    """(width, height, fps) from a Y4M header."""
    with open(src_path, "rb") as f:
        head = f.readline(512).decode("ascii", "replace").split()
    w = int(next(t[1:] for t in head if t.startswith("W")))
    h = int(next(t[1:] for t in head if t.startswith("H")))
    fr = next((t[1:] for t in head if t.startswith("F")), "30:1").split(":")
    return w, h, int(fr[0]) / max(1, int(fr[1]))


def v1_model(src_path):
    """The v1 model file for a source, by its height and frame rate."""
    _, h, fps = _y4m_geometry(src_path)
    hfr = fps >= 50
    name = f"vmaf_{V1_VER}{'_hfr' if hfr else ''}_{'1d5h_2160' if h >= 1440 else '3d0h'}.json"
    return os.path.join(V1_MODELS, f"vmaf_{V1_VER}{'_hfr' if hfr else ''}", name)


def _enc_bitdepth(bitstream):
    """The stream's coded bit depth, for CAMBI: v1 is scored at 10-bit, and
    CAMBI judges banding against the depth the encode really had."""
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                        "stream=pix_fmt", "-of", "csv=p=0", bitstream],
                       capture_output=True, text=True)
    pf = r.stdout.strip()
    return 10 if "10" in pf else (12 if "12" in pf else 8)


# VMAF v1 is scored at display resolution: the 1080p 3H model assumes a
# 1920x1080 screen, so a picture smaller than that in both dimensions is
# scaled up to fit it (bicubic, aspect kept), both sides, and CAMBI is told the
# coded size. Scored at native size, a CIF picture's chroma planes shrink to a
# few samples in v1's chroma feature and chroma loss goes unseen.
V1_DISPLAY = (1920, 1080)
# names this scoring rule in keys and files that hold v1 scores, so a score
# taken before it (native size) is never read as one taken under it
V1_RULE = "disp"
V1_SCALE_VF = ("scale=w=1920:h=1080:force_original_aspect_ratio=decrease:"
               "force_divisible_by=2:flags=bicubic")


def v1_scaled(w, h):
    """True when a w x h picture is scored scaled up to the display."""
    return w < V1_DISPLAY[0] and h < V1_DISPLAY[1]


def v1_model_opts(w, h, ebd):
    """The --model suffix after the path: name, CAMBI's encode bit depth and,
    for a scaled picture, its coded size."""
    o = f"name=vmaf_v1:cambi.enc_bitdepth={ebd}"
    if v1_scaled(w, h):
        o += f":cambi.enc_width={w}:cambi.enc_height={h}"
    return o


_v1_ident_memo = {}


def _v1_ident(model):
    """The scorer's identity for the cache key: the libvmaf build and the model
    file's contents, so a rebuilt binary or a same-named model never serves an
    old score."""
    if model not in _v1_ident_memo:
        ver = subprocess.run([V1_BIN, "--version"], capture_output=True, text=True)
        with open(model, "rb") as f:
            msha = hashlib.sha1(f.read()).hexdigest()[:12]
        _v1_ident_memo[model] = f"{(ver.stdout + ver.stderr).strip()}|{msha}"
    return _v1_ident_memo[model]


def _packets(bitstream):
    """Coded frames in a stream, by packet count, or None."""
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_packets",
                        "-show_entries", "stream=nb_read_packets", "-of", "csv=p=0", bitstream],
                       capture_output=True, text=True)
    try:
        return int(r.stdout.strip().split(",")[0])
    except ValueError:
        return None


_ASSIGN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*=")


# launchers a template may put in front of the encoder, with the options of
# each that take a value
_WRAPPERS = {
    "env": ("-u", "-C", "-P", "-S"), "nice": ("-n",), "time": (),
    "taskpolicy": ("-c", "-d", "-g", "-t", "-l", "-p"), "caffeinate": ("-w", "-t"),
    "timeout": ("-s", "-k"),
}


def _resolve(w):
    if os.path.isabs(w) or os.path.exists(w):
        return os.path.abspath(w)
    if os.sep in w:
        return os.path.normpath(os.path.join(ROOT, w))
    return shutil.which(w) or w


_CD = re.compile(r"(^|[;&|(]\s*)cd\s")


def _yah264_words(tmpl):
    """Every yah264 a shell template runs, across pipes and command lists and
    inside quoted command strings, as executable paths. None when one can't
    be resolved: a name that isn't an executable file, or a path after a cd,
    whose target we don't track."""
    if _CD.search(tmpl):
        return None if "yah264" in tmpl else []
    try:
        lex = shlex.shlex(tmpl, posix=True, punctuation_chars=True)
        lex.whitespace_split = True
        words = list(lex)
    except ValueError:
        return None
    found = []
    for w in words:
        if " " in w:                        # a quoted command string
            if "yah264" in w:
                sub = _yah264_words(w)
                if sub is None:
                    return None
                found += sub
            continue
        w = w.split("=", 1)[1] if _ASSIGN.match(w) else w     # YAH264=/path/yah264
        if os.path.basename(w) == "yah264":
            p = _resolve(w)
            if not (os.path.isfile(p) and os.access(p, os.X_OK)):
                return None
            found.append(p)
    return found


def template_binary(tmpl):
    """The program an encode template runs. A yah264 named anywhere in it (a
    pipeline, a command list, an `sh -c` body) is that program; otherwise it's
    the first word that is neither an env assignment nor a launcher (env,
    nice, time, taskpolicy, caffeinate, timeout) or one of its options, as the
    shell would find it. "" when a yah264 in it can't be resolved."""
    ys = _yah264_words(tmpl)
    if ys is None:
        return ""
    if ys:
        return ys[0]
    try:
        words = shlex.split(tmpl)
    except ValueError:
        return ""
    wrap, skip, need_arg = None, False, False
    for w in words:
        if skip:
            skip = False
        elif _ASSIGN.match(w):
            pass
        elif os.path.basename(w) in _WRAPPERS:
            wrap = os.path.basename(w)
            need_arg = wrap == "timeout"
        elif wrap and w.startswith("-"):
            skip = w in _WRAPPERS[wrap]
        elif need_arg:
            need_arg = False            # timeout's duration
        else:
            return _resolve(w)
    return ""


def coded_size_is(bitstream, w, h):
    """True when the stream's coded size is w x h. Fitting both sides to the
    display would otherwise score an encode at another size with the same
    aspect."""
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                        "stream=width,height", "-of", "csv=p=0", bitstream],
                       capture_output=True, text=True)
    return r.stdout.strip().split("\n")[0] == f"{w},{h}"


def v1_pass(bitstream, src_path, work, js, model_args, vf, extra=()):
    """One libvmaf pass of a bitstream against its source, both at 10-bit and
    fitted to the display by `vf`, writing js. Returns the parsed JSON, or None
    when any part of the pass failed or scored fewer frames than the stream
    holds. Nothing lands on disk but the JSON: the reference reaches libvmaf
    on its stdin and the decode on a FIFO.

    Every failure ends the pass. A dead reference is an EOF on stdin. A decode
    that dies before opening its FIFO would leave libvmaf blocked in open(), so
    the FIFO is opened and closed from here until libvmaf sees EOF. A libvmaf
    that dies first leaves the decode blocked in open(), so it is killed, not
    waited on. When the encode is shorter than the source, the reference
    ffmpeg fails on the pipe libvmaf stopped reading, which is expected."""
    try:
        sw, sh_, _ = _y4m_geometry(src_path)
    except (OSError, StopIteration, ValueError):
        return None
    if not coded_size_is(bitstream, sw, sh_):
        return None
    d = tempfile.mkdtemp(prefix="v1.", dir=work)
    dec = os.path.join(d, "dec.y4m")
    os.mkfifo(dec)
    rerr = tempfile.TemporaryFile()
    procs = []
    try:
        conv = subprocess.Popen(
            ["ffmpeg", "-v", "error", "-nostdin", "-i", src_path, *vf,
             "-pix_fmt", "yuv420p10le", "-strict", "-1", "-f", "yuv4mpegpipe", "pipe:1"],
            stdout=subprocess.PIPE, stderr=rerr)
        procs.append(conv)
        dproc = subprocess.Popen(
            ["ffmpeg", "-v", "error", "-nostdin", "-y", *shlex.split(_FF_T), "-i", bitstream,
             "-fps_mode", "passthrough", *vf, "-pix_fmt", "yuv420p10le", "-strict", "-1",
             "-f", "yuv4mpegpipe", dec],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(dproc)
        sub = ["--subsample", str(SUBSAMPLE)] if SUBSAMPLE > 1 else []
        v = subprocess.Popen(
            [V1_BIN, "-q", "-r", "/dev/stdin", "-d", dec, "--json", "-o", js,
             *model_args, *sub, *shlex.split(_VMAF_T), *extra],
            stdin=conv.stdout, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(v)
        conv.stdout.close()
        while True:
            try:
                v.wait(timeout=0.5)
                break
            except subprocess.TimeoutExpired:
                if dproc.poll() not in (None, 0):
                    try:
                        os.close(os.open(dec, os.O_WRONLY | os.O_NONBLOCK))
                    except OSError:
                        pass            # libvmaf has not opened it yet; next tick
        if v.returncode != 0:
            return None
        # libvmaf is done, so nothing reads either side any more: a producer
        # still running now is blocked, and is killed below
        try:
            if dproc.wait(timeout=5) != 0:
                return None
            rc = conv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            return None
        rerr.seek(0)
        if rc != 0 and b"Broken pipe" not in rerr.read():
            return None
        with open(js) as f:
            jj = json.load(f)
        n = _packets(bitstream)
        if n is None or len(jj.get("frames", ())) < -(-n // max(1, SUBSAMPLE)):
            return None                 # libvmaf scored fewer frames than were coded
        return jj
    except (OSError, ValueError, KeyError):
        return None
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
            p.wait()
        rerr.close()
        try:
            os.unlink(dec)
            os.rmdir(d)
        except OSError:
            pass


def _v1_of(bitstream, src_path, work, src_id=None):
    try:
        model = v1_model(src_path)
        ebd = _enc_bitdepth(bitstream)
        w, h, _ = _y4m_geometry(src_path)
    except (OSError, StopIteration, ValueError):
        return None                     # an unreadable source: no score
    if not coded_size_is(bitstream, w, h):
        return None                     # never served from the cache either
    scaled = v1_scaled(w, h)
    vf = ["-vf", V1_SCALE_VF] if scaled else []
    if not os.path.exists(model) or not os.access(V1_BIN, os.X_OK):
        sys.exit(f"bdcompare: VMAF_V1=1 needs {V1_BIN} and {model}")
    want = ["VMAF-v1", "CAMBI"]
    cpath = None
    if SCORE_CACHE:
        base = _score_path(bitstream, src_path, src_id)
        if base:
            k = hashlib.sha1(f"{base}|v1|{os.path.basename(model)}|eb{ebd}|{_v1_ident(model)}"
                             f"|{V1_RULE}".encode()).hexdigest()
            cpath = os.path.join(os.path.dirname(os.path.dirname(base)), k[:2], k + ".json")
            hit = _score_get(cpath, want)
            if hit is not None:
                with _stats_lock:
                    SCORE_STATS["hit"] += 1
                return hit
    fd, js = tempfile.mkstemp(suffix=".v1.json", dir=work); os.close(fd)
    try:
        jj = v1_pass(bitstream, src_path, work, js,
                     ["--model", f"path={model}:{v1_model_opts(w, h, ebd)}"], vf)
    finally:
        try:
            os.unlink(js)
        except OSError:
            pass
    if jj is None:
        return None
    pm = jj["pooled_metrics"]
    cambi = next((k for k in pm if k.startswith("cambi")), None)
    if "vmaf_v1" not in pm or cambi is None:
        return None
    out = {"VMAF-v1": pm["vmaf_v1"]["mean"], "CAMBI": pm[cambi]["mean"]}
    if cpath:
        _score_put(cpath, out)
    with _stats_lock:
        SCORE_STATS["miss"] += 1
    return out


def _v0_of(bitstream, src_path, work, src_id=None):
    margs, primary, plabel = _vmaf_models()
    want = ([plabel] if primary else []) + ["VMAF-NEG"]
    cpath = _score_path(bitstream, src_path, src_id) if SCORE_CACHE else None
    if cpath:
        hit = _score_get(cpath, want)
        if hit is not None:
            with _stats_lock:
                SCORE_STATS["hit"] += 1
            return hit
    # Unique temp paths (NOT derived from the bitstream) so shared/cached
    # bitstreams don't collide across threads.
    fd, dec = tempfile.mkstemp(suffix=".dec.y4m", dir=work); os.close(fd)
    fd, js = tempfile.mkstemp(suffix=".json", dir=work); os.close(fd)
    strict = " -strict -1" if DEC_PIXFMT != "yuv420p" else ""
    # -fps_mode passthrough, and it is not cosmetic. An Annex-B stream carries
    # its rate in the VUI; the y4m muxer settles on one of its own, and where
    # the two disagree ffmpeg DROPS frames to fit. A 120 fps clip decoded 15 of
    # its 64 frames that way, silently, and libvmaf scored the 15 against the
    # first 15 of the reference: VMAF-NEG 16 at qp 4, which reads as an encoder
    # that cannot code the clip rather than as a harness that threw the clip
    # away. Passthrough can neither drop nor duplicate, so it is right wherever
    # the rates already agreed too.
    sh(f'ffmpeg -v error -y {_FF_T} -i "{bitstream}" -fps_mode passthrough '
       f'-pix_fmt {DEC_PIXFMT}{strict} "{dec}"')
    binary = os.environ.get("VMAF", "vmaf")
    sub = f"--subsample {SUBSAMPLE}" if SUBSAMPLE > 1 else ""
    # libvmaf's Y4M reader takes 4:2:0 only: a 4:2:2 or 4:4:4 reference
    # ("y4m format requires conversion") scored nothing, and every band that
    # reached touchdown_1080p read it as FAILED on every arm. VMAF's features
    # are luma-only, so the reference goes through a chroma-only conversion to
    # the decode's format on a FIFO -- luma untouched, the score exact.
    ref, conv = src_path, None
    if not _src_chroma(src_path).startswith("420"):
        ref = dec + ".ref.y4m"
        os.mkfifo(ref)
        conv = subprocess.Popen(
            ["ffmpeg", "-v", "error", "-nostdin", "-y", "-i", src_path,
             "-pix_fmt", DEC_PIXFMT] + (["-strict", "-1"] if DEC_PIXFMT != "yuv420p" else [])
            + ["-f", "yuv4mpegpipe", ref],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    r = sh(f'{shlex.quote(binary)} -r "{ref}" -d "{dec}" --json -o "{js}" '
           f'{margs} {sub} {_VMAF_T}')
    if conv:
        conv.kill()
        conv.wait()
        try:
            os.unlink(ref)
        except OSError:
            pass
    try:
        os.unlink(dec)
    except OSError:
        pass
    if r.returncode != 0:
        return None
    pm = json.load(open(js))["pooled_metrics"]
    if "vmaf_neg" not in pm or (primary and primary not in pm):
        return None
    # Report the best available VMAF (v1 when YAH264_VMAF_MODEL is set) plus
    # NEG, which penalises enhancement/sharpening -- the honest gate for
    # texture-retention features (psy-RD) that plain VMAF over-credits.
    out = {plabel: pm[primary]["mean"]} if primary else {}
    out["VMAF-NEG"] = pm["vmaf_neg"]["mean"]
    if cpath:
        _score_put(cpath, out)
    with _stats_lock:
        SCORE_STATS["miss"] += 1
    return out


_MODELS = {}
_stats_lock = threading.Lock()


def _vmaf_models():
    """(model-args string, primary pooled key, report label) for MODELS.
    "both" prefers the v1 model from YAH264_VMAF_MODEL, mirroring bench.py, and
    always adds NEG; "neg" runs NEG alone and has no primary (None, None)."""
    mode = MODELS
    if mode in _MODELS:
        return _MODELS[mode]
    if mode not in ("both", "neg"):
        sys.exit(f"bdcompare: VMAF models '{mode}' is not one of both, neg")
    if mode == "neg":
        _MODELS[mode] = ("--model version=vmaf_v0.6.1neg:name=vmaf_neg", None, None)
        return _MODELS[mode]
    parts, primary, label = [], "vmaf", "VMAF"
    env_path = os.environ.get("YAH264_VMAF_MODEL")
    if env_path and os.path.exists(env_path):
        parts.append(f"--model path={shlex.quote(env_path)}:name=vmaf_v1")
        primary, label = "vmaf_v1", "VMAF-v1"
    parts.append("--model version=vmaf_v0.6.1:name=vmaf")
    parts.append("--model version=vmaf_v0.6.1neg:name=vmaf_neg")
    _MODELS[mode] = (" ".join(parts), primary, label)
    return _MODELS[mode]


def minigop_guard(stride, templates, who="bdcompare"):
    """Refuse a VMAF subsample stride that shares a factor with the mini-GOP.

    At --bframes 3 the mini-GOP period is 4, and a stride of 2 or 4 scores the
    same GOP slots every time. Stride 4 there scores only anchor frames: it
    moved sunflower_1080p's VMAF-NEG from 92.62 to 94.62 and flipped a band
    arm's sign (+0.28% on every frame, -0.09% subsampled). The bias does not
    cancel in a delta, because two arms need not share a GOP phase. Strides
    coprime with the period (3 and 5 at bframes 3) avoid the bias but add
    +/-0.15-0.2% of noise per clip, so a self-A/B band stays at 1. The period
    is read from each template's --bframes, defaulting to 3 (medium's)."""
    if stride <= 1:
        return
    import re
    for t in templates:
        m = re.search(r"--bframes[ =](\d+)", t or "")
        bf = int(m.group(1)) if m else 3
        if math.gcd(stride, bf + 1) != 1:
            sys.exit(f"{who}: --subsample {stride} shares a factor with the "
                     f"mini-GOP period {bf + 1} (bframes {bf}); it would score "
                     "the same GOP slots every time. Use 1, or a stride coprime "
                     "with the period.")


def bd_rate(r_ref, m_ref, r_test, m_test):
    import numpy as np
    deg = min(3, len(m_ref) - 1)
    f1 = np.polyfit(m_ref, np.log(r_ref), deg)
    f2 = np.polyfit(m_test, np.log(r_test), deg)
    lo, hi = max(min(m_ref), min(m_test)), min(max(m_ref), max(m_test))
    if hi <= lo:
        return None
    xs = np.linspace(lo, hi, 100)
    return (math.exp(np.mean(np.polyval(f2, xs) - np.polyval(f1, xs))) - 1) * 100


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="encoder A command template")
    ap.add_argument("--b", required=True, help="encoder B command template (reference)")
    # Default CRF points span VMAF's DISCRIMINATING band (~55-95) on CIF, not the
    # saturated 95-100 top where BD-rate is noisy/overflow-prone (CRF 20-28 pins
    # near 100 on easy clips). Push points up for higher-res content.
    ap.add_argument("--points", default="26,32,38,44")
    ap.add_argument("--clips", default="foreman_cif,mobile_cif")
    ap.add_argument("--class", dest="klass", default=None,
                    help="select all clips of a content class from the "
                         "tests/corpus/CLASSES manifest (e.g. grain, motion); "
                         "overrides --clips. See docs/corpus-sources.md")
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--vmaf", action="store_true", help="VMAF instead of PSNR")
    ap.add_argument("--subsample", type=int, default=5,
                    help="compute VMAF on every Nth frame (libvmaf --subsample); "
                         "must be 1 or coprime with the mini-GOP period "
                         "(bframes + 1), and 1 for a self-A/B band")
    ap.add_argument("--models", choices=("both", "neg"), default=None,
                    help="VMAF models per score: both (plain or v1, plus NEG; "
                         "the default, and what reports keep) or neg (NEG alone, "
                         "about half the scoring cost; the bands' default). "
                         "Overrides VMAF_MODELS")
    ap.add_argument("--jobs", type=int, default=16)
    ap.add_argument("--no-cache", dest="cache", action="store_false",
                    help="disable the persistent encode cache (on by default; a "
                         "fixed baseline binary is encoded once across sweeps)")
    args = ap.parse_args()
    global SUBSAMPLE, MODELS
    SUBSAMPLE = max(1, args.subsample)
    if args.models:
        MODELS = args.models
    if args.vmaf:
        minigop_guard(SUBSAMPLE, [args.a, args.b])

    points = [int(q) for q in args.points.split(",")]
    if args.klass:
        manifest = os.path.join(ROOT, "tests", "corpus", "CLASSES")
        if not os.path.exists(manifest):
            sys.exit("bdcompare: --class needs tests/corpus/CLASSES "
                     "(run scripts/fetch_corpus.sh)")
        clips = [ln.split()[0] for ln in open(manifest)
                 if len(ln.split()) == 2 and ln.split()[1] == args.klass]
        if not clips:
            sys.exit(f"bdcompare: no clips of class '{args.klass}' in {manifest}")
        print(f"class {args.klass}: {', '.join(clips)}")
    else:
        clips = args.clips.split(",")
    work = scratch.mkdtemp("bdq")

    srcs, src_ids = {}, {}
    for c in clips:
        full = os.path.join(ROOT, "tests", "corpus", c + ".y4m")
        if not os.path.exists(full):
            sys.exit(f"bdcompare: no corpus clip {full}")
        cut = os.path.join(work, c + ".y4m")
        sh(f'ffmpeg -v error -y -i "{full}" -frames:v {args.frames} "{cut}"')
        srcs[c] = cut
        src_ids[c] = src_identity(full, f"frames{args.frames}")

    # Persistent encode cache: keyed on (template, clip, q, frames, encoder-binary
    # mtime). A fixed baseline binary hits the cache across strength sweeps, so
    # its encodes are computed once, not per invocation (~halves repeated sweeps).
    # The binary mtime in the key invalidates automatically on rebuild.
    # A CACHE, not scratch: it is the point of --cache that it survives the
    # run. It lived under $TMPDIR, where a sweep or `make clean-scratch`
    # would take it; it lives in the named cache now ($Y264_CACHE, default
    # ~/.cache/yah264/bdcache) and is nobody's to delete automatically.
    cache_dir = scratch.cache_dir("bdcache")
    if args.cache:
        os.makedirs(cache_dir, exist_ok=True)

    def _ckey(tmpl, clip, q):
        binp = template_binary(tmpl)
        # no binary to key a rebuild on, or one a `cd` makes ambiguous: don't cache
        if not (binp and os.path.isfile(binp)) or _CD.search(tmpl):
            return None
        mt = os.path.getmtime(binp)
        # the encoder inherits the environment's knobs, so they're part of the key
        envk = " ".join(f"{k}={v}" for k, v in sorted(os.environ.items())
                        if k.startswith(("Y264_", "YAH264_")))
        h = hashlib.sha1(f"{tmpl}|{clip}|{q}|{args.frames}|{mt}|{envk}".encode()).hexdigest()[:20]
        return os.path.join(cache_dir, h + ".264")

    errs = {}

    def encode(tag, tmpl, clip, q):
        out = os.path.join(work, f"{clip}_{tag}_{q}.264")
        ck = _ckey(tmpl, clip, q) if args.cache else None
        if ck:
            if os.path.exists(ck):
                return (tag, clip, q, ck)
        cmd = tmpl.format(src=shlex.quote(srcs[clip]), q=q, out=shlex.quote(out))
        r = sh(cmd)
        if r.returncode != 0 or not os.path.exists(out):
            errs[(tag, clip, q)] = f"exit {r.returncode}: {r.stderr[-400:]}"
            return (tag, clip, q, None)
        if ck:
            try:
                os.replace(out, ck); return (tag, clip, q, ck)
            except OSError:
                pass
        return (tag, clip, q, out)

    jobs = []
    with fut.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for clip in clips:
            for q in points:
                jobs.append(ex.submit(encode, "a", args.a, clip, q))
                jobs.append(ex.submit(encode, "b", args.b, clip, q))
        results = [j.result() for j in jobs]

    outs = {(t, c, q): o for t, c, q, o in results}
    if any(o is None for o in outs.values()):
        bad = [k for k, v in outs.items() if v is None]
        sys.exit(f"bdcompare: encode failed for {bad[:3]}\n{errs.get(bad[0], '')}")

    src_raw = {}
    if not args.vmaf:
        for c in clips:
            src_raw[c] = decode_raw(srcs[c]).astype("int32")

    def measure(clip, tag, q):
        o = outs[(tag, clip, q)]
        if args.vmaf:
            m = vmaf_of(o, srcs[clip], work, src_ids[clip])
        else:
            p = psnr_of(o, src_raw[clip])
            m = {"PSNR": p} if p is not None else None
        return (clip, tag, q, os.path.getsize(o), m)

    with fut.ThreadPoolExecutor(max_workers=args.jobs if args.vmaf else 2) as ex:
        ms = [ex.submit(measure, c, t, q) for c in clips for t in ("a", "b")
              for q in points]
        metrics = [m.result() for m in ms]

    if args.vmaf:
        print(f"vmaf scores: {SCORE_STATS['miss']} computed, "
              f"{SCORE_STATS['hit']} from the score cache, "
              + (f"VMAF {V1_VER}" if GATE == "VMAF-v1" else f"models {MODELS}"))
    table = {}
    for clip, tag, q, size, m in metrics:
        if m is None:
            sys.exit(f"bdcompare: metric failed for {clip} {tag} {q}")
        table.setdefault((clip, tag), []).append((q, size, m))

    # metric names come from the measured dict (e.g. VMAF-v1 + VMAF-NEG, or PSNR)
    # CAMBI is a banding score, lower is better, so it gets no BD-rate
    names = [n for n in next(iter(table.values()))[0][2].keys() if n != "CAMBI"]
    for clip in clips:
        for i, nm in enumerate(names):
            rows = {}
            for tag in ("a", "b"):
                pts = sorted(table[(clip, tag)], key=lambda t: t[0], reverse=True)
                rows[tag] = ([p[1] for p in pts], [p[2][nm] for p in pts])
                if i == 0:      # print the size/quality points once per clip
                    print(f"{clip:14} {tag}: " +
                          " ".join(f"{s//1000}k/{m[nm]:.2f}" for _, s, m in pts))
            bd = bd_rate(*rows["b"], *rows["a"])
            # Flag saturation: if the top point on either side is >=98, VMAF is
            # near-pinned and the BD is measured in a low-discrimination regime
            # (unreliable / overflow-prone). Push --points higher.
            hi = max(max(rows["a"][1]), max(rows["b"][1])) if nm != "PSNR" else 0
            warn = "  [!] near-saturation, raise --points" if hi >= 98 else ""
            print(f"{clip:14} BD-rate({nm}) A vs B: "
                  + (f"{bd:+.2f}%" if bd is not None else "no quality overlap") + warn)


if __name__ == "__main__":
    # for the shell scorers: "<scaled 0|1> <w> <h> <scale filter> <rule>" of a y4m
    if len(sys.argv) == 3 and sys.argv[1] == "--v1-display":
        w, h, _ = _y4m_geometry(sys.argv[2])
        print(int(v1_scaled(w, h)), w, h, V1_SCALE_VF, V1_RULE)
        sys.exit(0)
    main()
