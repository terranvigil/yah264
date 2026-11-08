#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""CRF or VBR ladders on a 10-bit HDR10 (PQ, BT.2020, limited range) Y4M,
yah264 against x264 High 10, scored with VMAF v1's HDR model and PSNR on the
10-bit code values, then BD-rate over the quality range both ladders cover.

  scripts/hdr_ladder.py SRC.y4m [--crf 18,22,26,30,34 [--max-crf 30] | --vbr K1,K2,...]
                        [--yah-lookahead N] [--x264-lookahead N]
                        [--encoders x264,yah264] [--threads 3] [--work DIR]
                        [--fresh]

Both encoders run at medium. The source must be a 4:2:0 10-bit Y4M
(C420p10) at exactly 1920x1080 or 3840x2160: the HDR model is scored at
its own display size (3d0h at 1080p, 1d5h_2160 at 2160p, the hfr variant
at 50 fps and up), and anything smaller would need the project's
upscale-to-1080p rule first, so it is refused. Every decode must be clean
and hold exactly the source's frame count, or the run stops. Rungs must be
distinct, and a metric whose quality doesn't rise with bitrate across a
ladder gets no BD figure. Read the printed rungs before trusting a BD figure;
--max-crf leaves out the rungs above it, for a second read.

--vbr runs one rung per target K kbit/s instead of the CRF ladder, with
--bitrate K, --vbv-maxrate 1.5K, --vbv-bufsize 2K and --vbv-init 0.9 on
both encoders (0.9 is x264's own default; yah264 defaults to full, so
it is passed to both). Each rung prints its achieved rate against the
target, flagged when it misses the parity spec's +-10% tracking bar (the
spec judges that bar on real-title cells), and the verdict of
scripts/vbv_check.py on the stream from the same starting fill. The
leaky-bucket check only bites over a span much longer than the buffer: on a
clip of a few seconds the buffer starts nearly full and almost any stream
passes, so treat it as a sanity check there. The spec takes the VBR
targets from the reference's achieved rates at CRF 20/23/26/29/32 on the
same clip; get them with --encoders x264 --crf 20,23,26,29,32.

--threads defaults to 3, a share of the shared box; the spec runs its
ladders at 6 threads, so pass --threads 6 for a reading that matches it.

Both encoders run their preset's lookahead depth unless --yah-lookahead or
--x264-lookahead passes --rc-lookahead to that side. x264 takes 0 to 250 and
the depth printed for it is the one its stream's options SEI records, which
is what it ran with. yah264 caps its window at 64 frames (read from its
--help, like its default), so a deeper --yah-lookahead is refused rather
than printed as run. CAMBI is the pooled mean of the banding feature the
HDR model computes in the same run.

Each rung's result is cached in the work directory, keyed on the encoder
binary and the non-system libraries it links, its full command line and,
for yah264, every Y264_* and YAH264_* variable set; the source's size,
mtime, ctime, inode and sampled md5; this script's own md5; and the scorer
chain (the vmaf and ffmpeg binaries, their libraries and versions, the model
file and, for VBR, vbv_check.py). A change to any of them re-encodes; a
reused rung prints "cached". RESULT_VERSION versions the cached JSON's
layout. --fresh ignores the cache. A run holds a lock on its work directory,
and a second run on the same directory stops at once.

  YAH264    yah264 CLI (default build/cli/yah264)
  X264      x264 CLI (default x264)
  VMAF_HDR  a vmaf CLI built with the HDR models' colour options
  VMAF_HDR_MODELS  the directory holding hdr_vmaf_v1.1.8[_hfr]/
"""
import argparse, fcntl, hashlib, json, math, os, re, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bdcompare import bd_rate                                   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
YAH = os.environ.get("YAH264", os.path.join(ROOT, "build", "cli", "yah264"))
X264 = os.environ.get("X264", "x264")
VMAF = os.environ.get("VMAF_HDR")
MODELS = os.environ.get("VMAF_HDR_MODELS")
VBV_CHECK = os.path.join(ROOT, "scripts", "vbv_check.py")


SIZES = {(1920, 1080): "3d0h", (3840, 2160): "1d5h_2160"}
# x264 takes up to 250 frames of lookahead
MAX_LOOKAHEAD = 250
# yah264's medium default and its cap, used only when its --help doesn't
# state them (it prints both on the --rc-lookahead line)
YAH_DEFAULT_LOOKAHEAD = 40
YAH_MAX_LOOKAHEAD = 64
VBV_INIT = 0.9
RATE_BAR = 10.0
# bump when the cached JSON's layout changes; a change to how a rung is
# measured is caught by the script's md5 in the key
RESULT_VERSION = 4
KNOB_PREFIXES = ("Y264_", "YAH264_")
LOCK = None


def y4m_info(path):
    with open(path, "rb") as f:
        hdr = f.readline().decode("latin-1")
    if not hdr.startswith("YUV4MPEG2 "):
        sys.exit(f"hdr_ladder: {path} is not a Y4M")
    tok = {t[0]: t[1:] for t in hdr.split()[1:]}
    if tok.get("C") != "420p10":
        sys.exit(f"hdr_ladder: {path} is C{tok.get('C', '420')}, not C420p10")
    w, h = int(tok["W"]), int(tok["H"])
    if (w, h) not in SIZES:
        sys.exit(f"hdr_ladder: {path} is {w}x{h}; the HDR models are scored at 1920x1080 "
                 "or 3840x2160, and a smaller clip has to be scaled up to 1080p first")
    num, den = map(int, tok["F"].split(":"))
    return w, h, num / den


def model_path(w, h, fps):
    d = "hdr_vmaf_v1.1.8" + ("_hfr" if fps >= 50 else "")
    return os.path.join(MODELS, d, f"{d}_{SIZES[(w, h)]}.json")


def which(b):
    p = b if os.sep in b else shutil.which(b)
    return os.path.realpath(p) if p else None


def md5_of(path):
    m = hashlib.md5()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            m.update(blk)
    return m.hexdigest()


def source_id(path):
    """Size, inode, mtime, ctime and the md5 of the first and last 8 MiB:
    hashing a whole 2160p excerpt on every run costs more than the check is
    worth, and a re-cut or recopy changes at least one of them (restoring the
    mtime still moves the ctime)."""
    st = os.stat(path)
    m = hashlib.md5()
    with open(path, "rb") as f:
        m.update(f.read(8 << 20))
        f.seek(max(0, st.st_size - (8 << 20)))
        m.update(f.read())
    return {"size": st.st_size, "ino": st.st_ino, "mtime_ns": st.st_mtime_ns,
            "ctime_ns": st.st_ctime_ns, "md5_ends": m.hexdigest()}


def linked_libs(path):
    """The non-system shared libraries a binary loads, followed through the
    libraries they load. Paths the loader resolves at run time (@rpath and
    friends) are kept by name only."""
    seen, todo = {}, [path]
    while todo:
        p = todo.pop()
        if sys.platform == "darwin":
            r = subprocess.run(["otool", "-L", p], capture_output=True, text=True)
            deps = [ln.split(" (")[0].strip() for ln in r.stdout.splitlines()[1:]]
        else:
            r = subprocess.run(["ldd", p], capture_output=True, text=True)
            deps = [ln.split("=>")[-1].split(" (")[0].strip() for ln in r.stdout.splitlines()]
        for d in deps:
            if not d or d in seen or d == p or d.startswith(("/usr/lib/", "/System/", "/lib/", "/lib64/")):
                continue
            if os.path.isabs(d) and os.path.isfile(d):
                seen[d] = md5_of(os.path.realpath(d))
                todo.append(d)
            else:
                seen[d] = None
    return dict(sorted(seen.items()))


def tool_id(path, version_args=None):
    t = {"md5": md5_of(path), "libs": linked_libs(path)}
    if version_args:
        r = subprocess.run([path] + version_args, capture_output=True, text=True)
        t["version"] = (r.stdout + r.stderr).strip()[:2000]
    return t


def yah_lookahead_limits(path):
    """yah264's default and maximum lookahead, as its --help states them."""
    r = subprocess.run([path, "--help"], capture_output=True, text=True)
    text = re.sub(r"\s+", " ", r.stdout + r.stderr)
    m = re.search(r"--rc-lookahead N .*?\(default (\d+).*?at most (\d+)", text)
    if m:
        return int(m.group(1)), int(m.group(2))
    return YAH_DEFAULT_LOOKAHEAD, YAH_MAX_LOOKAHEAD


def run(cmd, ok=(0,)):
    r = subprocess.run(["nice", "-n", "10"] + cmd, capture_output=True, text=True)
    if r.returncode not in ok:
        sys.exit(f"hdr_ladder: failed: {' '.join(cmd)}\n{r.stderr[-1500:]}")
    return r


def psnr(src, dec, w, h):
    import numpy as np
    fs = w * h * 3 // 2
    sse = np.zeros(3)
    n = 0
    with open(src, "rb") as a, open(dec, "rb") as b:
        want, got = y4m_shape(a.readline()), y4m_shape(b.readline())
        if want != got:
            sys.exit(f"hdr_ladder: {dec} decodes as {got}, not the source's {want}")
        while a.readline():
            if not b.readline():
                sys.exit(f"hdr_ladder: {dec} is short: the source has more than {n} frames")
            s = np.frombuffer(a.read(2 * fs), dtype=np.uint16).astype(np.int64)
            d = np.frombuffer(b.read(2 * fs), dtype=np.uint16).astype(np.int64)
            if s.size != d.size:
                sys.exit(f"hdr_ladder: {dec} ends inside frame {n}")
            e = (s - d) ** 2
            sse += [e[:w * h].sum(), e[w * h:w * h * 5 // 4].sum(), e[w * h * 5 // 4:].sum()]
            n += 1
        if b.readline():
            sys.exit(f"hdr_ladder: {dec} is long: it decodes more than the source's {n} frames")
    if n == 0:
        sys.exit("hdr_ladder: the decode has no frames")
    mse = sse / (np.array([w * h, w * h / 4, w * h / 4]) * n)
    p = [10 * math.log10(1023 * 1023 / m) if m > 0 else 100.0 for m in mse]   # lossless caps
    return n, p[0], (6 * p[0] + p[1] + p[2]) / 8


def y4m_shape(line):
    tok = {t[0]: t[1:] for t in line.decode("latin-1").split()[1:]}
    return tuple(tok.get(k) for k in "WHC")


def vbv_args(kbps):
    return round(kbps * 1.5), round(kbps * 2)


def lookahead_in_stream(path):
    """The rc_lookahead= value of the options SEI x264 writes into its stream."""
    with open(path, "rb") as f:
        head = f.read(1 << 16)
    m = re.search(rb"rc_lookahead=(\d+)", head)
    if not m:
        sys.exit(f"hdr_ladder: {path} carries no options SEI with a lookahead depth")
    return int(m.group(1))


def fmt(q):
    return str(int(q)) if q == int(q) else f"{q:g}"


def load_cached(js, key):
    try:
        with open(js) as f:
            r = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(r, dict) or r.get("key") != key:
        return None
    r["cached"] = True
    return r


def rung(enc, src, mode, q, work, threads, w, h, fps, enc_id, ids, lookahead, la_default, fresh):
    tag = f"{mode}{fmt(q)}" + (f"_la{lookahead}" if lookahead is not None else "")
    if enc_id.get("env"):
        # a knob A/B keeps both arms' rungs side by side
        tag += "_env" + hashlib.md5(" ".join(enc_id["env"]).encode()).hexdigest()[:8]
    stem = os.path.join(work, f"{enc}_{tag}")
    js = stem + ".json"
    out, dec, vj = stem + ".264", stem + ".dec.y4m", stem + ".vmaf.json"
    part = stem + ".part.264"
    if mode == "crf":
        rc = ["--crf", fmt(q)]
    else:
        maxrate, bufsize = vbv_args(q)
        rc = ["--bitrate", fmt(q), "--vbv-maxrate", str(maxrate), "--vbv-bufsize", str(bufsize),
              "--vbv-init", str(VBV_INIT)]
    if lookahead is not None:
        rc += ["--rc-lookahead", str(lookahead)]
    if enc == "yah264":
        cmd = [YAH, "--input-y4m", src] + rc + ["--threads", str(threads), "-o", part]
    else:
        cmd = [X264, "--preset", "medium", "--profile", "high10", "--output-depth", "10"] + rc + \
              ["--threads", str(threads), "--log-level", "error", "-o", part, src]
    key = {"encoder": enc_id, "cmd": cmd, "model": model_path(w, h, fps),
           "version": RESULT_VERSION, **ids}
    if not fresh:
        r = load_cached(js, key)
        if r is not None:
            return r
    for p in (js, out, part, dec, vj):
        if os.path.exists(p):
            os.remove(p)
    # the stream is written under a temporary name and must appear fresh and
    # non-empty, so an encoder that exits 0 without writing can't hand an old
    # stream to the scorer
    start = time.time_ns()
    run(cmd)
    if not os.path.exists(part) or os.path.getsize(part) == 0 or os.stat(part).st_mtime_ns < start - 10**9:
        sys.exit(f"hdr_ladder: {enc} exited cleanly but wrote no stream: {' '.join(cmd)}")
    os.replace(part, out)
    try:
        # -xerror and explode turn a damaged stream into an error; anything
        # ffmpeg says at -v error is treated the same, whatever its exit code
        d = run(["ffmpeg", "-v", "error", "-nostdin", "-xerror", "-err_detect", "explode",
                 "-i", out, "-fps_mode", "passthrough", "-pix_fmt", "yuv420p10le",
                 "-strict", "-1", "-f", "yuv4mpegpipe", "-y", dec])
        if d.stderr.strip():
            sys.exit(f"hdr_ladder: {out} decodes with errors:\n{d.stderr[-1500:]}")
        n, py, pyuv = psnr(src, dec, w, h)
        col = []
        for side in ("ref", "dist"):
            col += [f"--color_range_{side}", "limited", f"--color_primaries_{side}", "bt2020",
                    f"--color_trc_{side}", "pq", f"--color_matrix_{side}", "bt2020nc"]
        run([VMAF, "--reference", src, "--distorted", dec, "--threads", str(threads)] + col +
            ["--model", "path=" + model_path(w, h, fps), "--json", "--output", vj])
    finally:
        if os.path.exists(dec):
            os.remove(dec)
    if not os.path.exists(vj) or os.path.getmtime(vj) < os.path.getmtime(out):
        sys.exit(f"hdr_ladder: vmaf exited cleanly but wrote no fresh {vj}")
    with open(vj) as f:
        vm = json.load(f)
    if len(vm["frames"]) != n:
        sys.exit("hdr_ladder: the VMAF frame count disagrees with the decode")
    pooled = vm["pooled_metrics"]
    cambi = [k for k in pooled if k.startswith("cambi")]
    if not cambi:
        sys.exit(f"hdr_ladder: {model_path(w, h, fps)} computes no CAMBI feature")
    frames_cambi = [f["metrics"][cambi[0]] for f in vm["frames"]]
    ran = lookahead_in_stream(out) if enc == "x264" else \
        (lookahead if lookahead is not None else la_default)
    r = {"mode": mode, "q": q, "lookahead": lookahead, "lookahead_run": ran,
         "kbps": os.path.getsize(out) * 8 / (n / fps) / 1000,
         "vmaf": pooled["vmaf"]["mean"], "psnr_y": py, "psnr_yuv": pyuv,
         "cambi": pooled[cambi[0]]["mean"], "cambi_max": max(frames_cambi), "key": key,
         "stream": out}
    if mode == "vbr":
        maxrate, bufsize = vbv_args(q)
        v = run([sys.executable, VBV_CHECK, out, "--maxrate", str(maxrate),
                 "--bufsize", str(bufsize), "--init", str(VBV_INIT), "--y4m", src, "--quiet"],
                ok=(0, 1))
        r["rate_err"] = (r["kbps"] / q - 1) * 100
        r["vbv"] = v.stdout.strip().splitlines()[0] if v.stdout.strip() else "no verdict"
        r["vbv_ok"] = v.returncode == 0
    with open(js + ".tmp", "w") as f:
        json.dump(r, f)
    os.replace(js + ".tmp", js)
    return r


def ladder_list(text, opt, conv):
    if text is None or not text.strip():
        sys.exit(f"hdr_ladder: {opt} needs a comma-separated list")
    try:
        qs = [conv(t) for t in text.split(",")]
    except ValueError as e:
        sys.exit(f"hdr_ladder: {opt} {text}: {e}")
    if len(set(qs)) != len(qs):
        sys.exit(f"hdr_ladder: {opt} {text} repeats a rung; a repeated point skews the BD fit")
    return sorted(qs)


def crf_value(t):
    q = float(t)
    if not 0 <= q <= 51:
        raise ValueError(f"CRF {t} is outside 0..51")
    return int(q) if q == int(q) else q


def kbps_value(t):
    if not t.strip().isdigit() or int(t) <= 0:
        raise ValueError(f"target '{t}' is not a positive whole kbit/s")
    return int(t)


def lookahead_value(t):
    v = int(t)
    if not 0 <= v <= MAX_LOOKAHEAD:
        raise argparse.ArgumentTypeError(f"{t} is outside 0..{MAX_LOOKAHEAD}")
    return v


def strictly_rising(xs):
    return all(b > a for a, b in zip(xs, xs[1:]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--crf", default=None, help="CRF rungs (default 18,22,26,30,34)")
    ap.add_argument("--vbr", default=None, help="kbit/s targets; replaces the CRF ladder")
    ap.add_argument("--max-crf", type=float, default=None)
    ap.add_argument("--yah-lookahead", type=lookahead_value, default=None)
    ap.add_argument("--x264-lookahead", type=lookahead_value, default=None)
    ap.add_argument("--encoders", default="x264,yah264")
    ap.add_argument("--threads", type=int, default=3)
    ap.add_argument("--work", default=None)
    ap.add_argument("--fresh", action="store_true", help="ignore cached rungs")
    a = ap.parse_args()
    if not VMAF or not MODELS:
        sys.exit("hdr_ladder: set VMAF_HDR and VMAF_HDR_MODELS")
    encs = a.encoders.split(",")
    if not encs or any(e not in ("x264", "yah264") for e in encs) or len(set(encs)) != len(encs):
        sys.exit("hdr_ladder: --encoders takes x264, yah264 or both")
    if a.threads < 1:
        sys.exit("hdr_ladder: --threads must be at least 1")
    if a.vbr is not None:
        if a.crf is not None or a.max_crf is not None:
            sys.exit("hdr_ladder: --vbr replaces the CRF ladder; drop --crf and --max-crf")
        mode, qs = "vbr", ladder_list(a.vbr, "--vbr", kbps_value)
    else:
        mode, qs = "crf", ladder_list(a.crf if a.crf is not None else "18,22,26,30,34",
                                      "--crf", crf_value)
        if a.max_crf is not None:
            qs = [q for q in qs if q <= a.max_crf]
    left = f"{len(qs)} rung(s) left to run" + \
        (f" after --max-crf {fmt(a.max_crf)}" if mode == "crf" and a.max_crf is not None else "")
    if len(encs) == 2 and len(qs) < 2:
        sys.exit(f"hdr_ladder: {left}; a BD figure needs at least 2 per side")
    if not qs:
        sys.exit(f"hdr_ladder: {left}")
    la = {"yah264": a.yah_lookahead, "x264": a.x264_lookahead}
    if any(la[e] is not None for e in la if e not in encs):
        sys.exit("hdr_ladder: a lookahead was given for an encoder this run leaves out")
    w, h, fps = y4m_info(a.src)
    # everything a rung needs, checked before the first encode
    bins = {}
    for name, b in [(e, YAH if e == "yah264" else X264) for e in encs] + \
                   [("vmaf", VMAF), ("ffmpeg", "ffmpeg")]:
        p = which(b)
        if not p or not os.access(p, os.X_OK):
            sys.exit(f"hdr_ladder: no {name} binary at '{b}'")
        bins[name] = p
    if not os.path.isfile(model_path(w, h, fps)):
        sys.exit(f"hdr_ladder: no HDR model at {model_path(w, h, fps)}")
    la_default = {"x264": None, "yah264": None}
    if "yah264" in encs:
        la_default["yah264"], yah_max = yah_lookahead_limits(bins["yah264"])
        if la["yah264"] is not None and la["yah264"] > yah_max:
            sys.exit(f"hdr_ladder: --yah-lookahead {la['yah264']} is above yah264's "
                     f"{yah_max}-frame cap")
    work = a.work or os.path.join(ROOT, "local", "hdr_ladder",
                                  os.path.splitext(os.path.basename(a.src))[0])
    os.makedirs(work, exist_ok=True)
    # one run per work directory: two runs sharing it would score and cache
    # each other's streams
    global LOCK
    LOCK = open(os.path.join(work, ".lock"), "w")
    try:
        fcntl.flock(LOCK, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit(f"hdr_ladder: another run holds {work}; wait for it or pass another --work")
    knobs = sorted(f"{k}={v}" for k, v in os.environ.items() if k.startswith(KNOB_PREFIXES))
    enc_id = {e: tool_id(bins[e]) for e in encs}
    if "yah264" in enc_id:
        enc_id["yah264"]["env"] = knobs
    ids = {"src": source_id(a.src), "script_md5": md5_of(os.path.abspath(__file__)),
           "vmaf": tool_id(bins["vmaf"], ["--version"]),
           "ffmpeg": tool_id(bins["ffmpeg"], ["-version"]),
           "model_md5": md5_of(model_path(w, h, fps))}
    if mode == "vbr":
        ids["vbv_check_md5"] = md5_of(VBV_CHECK)
    print(f"{os.path.basename(a.src)} {w}x{h} {fps:.3f} fps, {mode.upper()}, medium, "
          f"--threads {a.threads}"
          + (", VBV maxrate 1.5K buffer 2K init 0.9" if mode == "vbr" else ""))
    if knobs and "yah264" in encs:
        print("yah264 environment: " + " ".join(knobs))
    lad = {e: [rung(e, a.src, mode, q, work, a.threads, w, h, fps, enc_id[e], ids, la[e],
                    la_default[e], a.fresh)
               for q in qs] for e in encs}
    outside, underflow = [], []
    for e, rs in lad.items():
        for r in rs:
            line = (f"{e:7s} {mode}{fmt(r['q']):<6} {r['kbps']:9.0f} kbps  vmaf {r['vmaf']:6.2f}  "
                    f"psnr-y {r['psnr_y']:6.2f}  psnr-yuv {r['psnr_yuv']:6.2f}  "
                    f"cambi {r['cambi']:.3f} (max {r['cambi_max']:.3f})")
            if mode == "vbr":
                miss = abs(r["rate_err"]) > RATE_BAR
                if miss:
                    outside.append(f"{e} {fmt(r['q'])}")
                line += f"  rate {r['rate_err']:+.1f}%" + (f" OUTSIDE +-{RATE_BAR:g}%" if miss else "")
            print(line + ("  (cached)" if r.get("cached") else ""))
            if mode == "vbr":
                if not r["vbv_ok"]:
                    underflow.append(f"{e} {fmt(r['q'])}")
                # the checker's line starts "STATUS STREAM: "; drop the
                # stream by its known path, which may itself hold ": "
                status, _, rest = r["vbv"].partition(" ")
                rest = rest[len(r["stream"]) + 2:] if rest.startswith(r["stream"] + ": ") else rest
                print(f"          vbv {status}: {rest}")
    print("lookahead as run: " + ", ".join(
        f"{e} " + "/".join(str(d) for d in sorted({r['lookahead_run'] for r in rs}))
        for e, rs in lad.items()))
    if mode == "vbr":
        n = sum(len(rs) for rs in lad.values())
        print(f"rate tracking, +-{RATE_BAR:g}% bar: " +
              (f"{len(outside)} of {n} rungs outside ({', '.join(outside)})" if outside
               else f"all {n} rungs within"))
        print("vbv: " + (f"{len(underflow)} of {n} rungs UNDERFLOW ({', '.join(underflow)})"
                         if underflow else f"all {n} rungs OK"))
    for e, rs in lad.items():
        kb = [r["kbps"] for r in rs]   # rs is in rung order
        rising = strictly_rising(kb) if mode == "vbr" else strictly_rising(kb[::-1])
        if not rising:
            print(f"warning: {e}'s bitrate doesn't move monotonically with the {mode.upper()} rungs")
    if len(lad) < 2:
        return
    b, t = sorted(lad["x264"], key=lambda r: r["kbps"]), sorted(lad["yah264"], key=lambda r: r["kbps"])
    lo, hi = max(b[0]["kbps"], t[0]["kbps"]), min(b[-1]["kbps"], t[-1]["kbps"])
    print(f"bitrate overlap: {lo:.0f}-{hi:.0f} kbps" if hi > lo else
          "bitrate overlap: none (BD-rate compares rates at equal quality, over the "
          "quality range printed with each figure)")
    for m in ("vmaf", "psnr_y", "psnr_yuv"):
        head = f"BD-rate {m:8s} yah264 vs x264 High 10: "
        bad = [e for e, rs in (("x264", b), ("yah264", t)) if not strictly_rising([r[m] for r in rs])]
        if bad:
            print(head + f"none, {m} doesn't rise with bitrate on {' and '.join(bad)}")
            continue
        qlo = max(b[0][m], t[0][m])
        qhi = min(b[-1][m], t[-1][m])
        bd = bd_rate([r["kbps"] for r in b], [r[m] for r in b], [r["kbps"] for r in t], [r[m] for r in t])
        print(head + (f"{bd:+.2f}% over {m} {qlo:.2f}-{qhi:.2f}" if bd is not None
                      else "no quality overlap"))


main()
