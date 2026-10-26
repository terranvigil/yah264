#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""tp_band.py -- the two-pass band: BD-VMAF-NEG and rate accuracy of
`--pass 1` / `--pass 2 --bitrate R` against an anchor binary.

No other band runs two-pass: run_band.py's ABR band and the CRF bands are
single-pass. This one is built on bdcompare.py (its VMAF score cache and its
BD integral) and adds the two things a two-pass arm is judged on: quality at
the rate it delivered, and how far that rate is from the target.

  ARMS='new=@/abs/build/cli/yah264' YAH264=/abs/baseline python3 scripts/tp_band.py
  ARMS='a=@/bin Y264_<KNOB>=1;b=@/bin Y264_<KNOB>=2' ...      (env per arm)

Rates per clip come from scripts/ladders.json (the ABR band's four rungs) plus
a fifth at 0.8x the lowest, so the BD has five points. Pass 1 is a CQP
analysis encode whose stats do not depend on the target, so it runs once per
(clip, binary, env) and every rung reuses it.

Env: CLIPS (default: every ladder clip), FRAMES (120), JOBS (3), THREADS (1),
LONG=1 (the long set: 300-frame CIF clips and 480 frames of sintel, multi-GOP
and cut-bearing, where the I-frame and blur windows actually matter).
Prints per clip: BD-VMAF-NEG and BD-PSNR-Y of each arm vs the anchor, and the
anchor's and the arm's rate error per rung; then a median/worst summary.
"""
import concurrent.futures as fut
import hashlib
import json
import math
import os
import shlex
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bdcompare                                                 # noqa: E402
import scratch                                                   # noqa: E402

ROOT = os.path.dirname(HERE)
ANCHOR = os.environ.get("YAH264", os.path.join(ROOT, "build/cli/yah264"))
FRAMES = int(os.environ.get("FRAMES", "120"))
JOBS = int(os.environ.get("JOBS", "3"))
THREADS = os.environ.get("THREADS", "1")
LONG = os.environ.get("LONG", "0") == "1"
bdcompare.MODELS = "neg"
bdcompare.SUBSAMPLE = 1

ladders = json.load(open(os.path.join(HERE, "ladders.json")))
if LONG:
    long_set = {"foreman_cif": 300, "akiyo_cif": 300, "mobile_cif": 300,
                "coastguard_cif": 300, "tempete_cif": 260, "sintel_720p": 480}
    clips = {c: (ladders[c], n) for c, n in long_set.items()}
else:
    clips = {c: (ladders[c], FRAMES) for c in ladders}
if os.environ.get("CLIPS"):
    want = os.environ["CLIPS"].split(",")
    clips = {c: clips[c] for c in want}


def parse_arms(spec):
    arms = []
    for part in [p for p in spec.split(";") if p.strip()]:
        name, rest = part.split("=", 1)
        toks = shlex.split(rest)
        binp = toks[0][1:] if toks and toks[0].startswith("@") else ANCHOR
        env = [t for t in (toks[1:] if toks and toks[0].startswith("@") else toks)]
        arms.append((name.strip(), binp, env))
    return arms


ARMS = parse_arms(os.environ.get("ARMS", ""))
if not ARMS:
    sys.exit("tp_band: set ARMS='name=@/path/to/yah264 [ENV=v ...];...'")
ALL = [("base", ANCHOR, [])] + ARMS

cache = scratch.cache_dir("tpband")
os.makedirs(cache, exist_ok=True)
work = scratch.mkdtemp("tpb")
_sha = {}


def bin_sha(p):
    if p not in _sha:
        _sha[p] = hashlib.sha1(open(p, "rb").read()).hexdigest()[:16]
    return _sha[p]


def y4m_fps(path):
    with open(path, "rb") as f:
        head = f.readline(512).decode("ascii", "replace").split()
    for t in head:
        if t.startswith("F"):
            a, b = t[1:].split(":")
            return float(a) / float(b)
    return 25.0


srcs, sids, fps, nfr = {}, {}, {}, {}
for c, (_, n) in clips.items():
    # SRC_<clip>=path reads that clip's ladder against another source (an
    # offset cut, say), keeping the ladder and the name.
    full = os.environ.get("SRC_" + c) or os.path.join(ROOT, "tests", "corpus", c + ".y4m")
    cut = os.path.join(work, f"{c}_{n}.y4m")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", full, "-frames:v", str(n),
                    cut], check=True)
    srcs[c], sids[c], fps[c] = cut, bdcompare.src_identity(full, f"frames{n}"), y4m_fps(full)
    # a clip shorter than FRAMES (stefan has 90) codes all it has
    with open(cut, "rb") as f:
        nfr[c] = f.read().count(b"FRAME\n")


def run(cmd, env):
    e = dict(os.environ)
    for kv in env:
        k, v = kv.split("=", 1)
        e[k] = v
    return subprocess.run(cmd, env=e, capture_output=True, text=True)


def key(*parts):
    return hashlib.sha1("|".join(str(p) for p in parts).encode()).hexdigest()[:24]


def common(clip, binp):
    return [binp, "--input-y4m", srcs[clip], "--frames", str(clips[clip][1]),
            "--preset", "medium", "--cabac", "--transform-8x8", "--threads", THREADS]


def pass1(clip, binp, env):
    st = os.path.join(cache, key("p1", bin_sha(binp), env, sids[clip], THREADS) + ".stats")
    if not os.path.exists(st):
        tmp = st + f".{os.getpid()}.tmp"
        r = run(common(clip, binp) + ["--pass", "1", "--bitrate", str(clips[clip][0][0]),
                                      "--stats", tmp, "-o", os.devnull], env)
        if r.returncode != 0:
            sys.exit(f"tp_band: pass 1 failed {clip} {binp}: {r.stderr[-400:]}")
        os.replace(tmp, st)
    return st


def pass2(clip, binp, env, rate, st):
    out = os.path.join(cache, key("p2", bin_sha(binp), env, sids[clip], THREADS, rate) + ".264")
    if not os.path.exists(out):
        tmp = out + f".{os.getpid()}.tmp.264"
        # pass 2 rewrites nothing in the stats file, but give each cell its own copy
        # so concurrent rungs never share a file handle.
        stc = os.path.join(work, os.path.basename(out) + ".stats")
        with open(st, "rb") as a, open(stc, "wb") as b:
            b.write(a.read())
        r = run(common(clip, binp) + ["--pass", "2", "--bitrate", str(rate),
                                      "--stats", stc, "-o", tmp], env)
        if r.returncode != 0 or not os.path.exists(tmp):
            sys.exit(f"tp_band: pass 2 failed {clip} {binp} {rate}: {r.stderr[-400:]}")
        os.replace(tmp, out)
    return out


INSTR = os.environ.get("INSTR", "0") == "1"


def instr_of(clip, binp, env, rate, st):
    """Instructions retired by the pass-2 encode of one cell (INSTR=1), from
    `/usr/bin/time -l` on macOS. A separate run to /dev/null, so the timed
    process writes nothing; counts are near-deterministic and load-immune,
    which is what lets this run JOBS-wide."""
    ck = os.path.join(cache, key("instr", bin_sha(binp), env, sids[clip], THREADS, rate) + ".json")
    try:
        return json.load(open(ck))["instr"]
    except (OSError, ValueError):
        pass
    stc = os.path.join(work, os.path.basename(ck) + ".stats")
    with open(st, "rb") as a, open(stc, "wb") as b:
        b.write(a.read())
    r = run(["/usr/bin/time", "-l"] + common(clip, binp) +
            ["--pass", "2", "--bitrate", str(rate), "--stats", stc, "-o", os.devnull], env)
    v = None
    for line in r.stderr.splitlines():
        if "instructions retired" in line:
            v = float(line.split()[0])
    if v is None:
        sys.exit(f"tp_band: no instruction count for {clip} {binp} {rate}")
    json.dump({"instr": v}, open(ck, "w"))
    return v


def psnr_y(bitstream, clip):
    ck = os.path.join(cache, key("psnry", bdcompare._md5_file(bitstream), sids[clip]) + ".json")
    try:
        return json.load(open(ck))["psnr_y"]
    except (OSError, ValueError):
        pass
    import numpy as np
    w, h = [int(t[1:]) for t in open(srcs[clip], "rb").readline().decode().split()
            if t[:1] in ("W", "H")][:2]
    fs = w * h * 3 // 2
    d = bdcompare.decode_raw(bitstream)
    s = bdcompare.decode_raw(srcs[clip])
    n = min(len(d), len(s)) // fs
    acc = 0.0
    for i in range(n):
        a = d[i * fs:i * fs + w * h].astype(np.int32)
        b = s[i * fs:i * fs + w * h].astype(np.int32)
        mse = max(float(np.mean((a - b) ** 2)), 1e-6)
        acc += 10 * math.log10(255 * 255 / mse)
    v = acc / max(n, 1)
    json.dump({"psnr_y": v}, open(ck, "w"))
    return v


def cell(clip, arm, rate):
    name, binp, env = arm
    st = pass1(clip, binp, env)
    out = pass2(clip, binp, env, rate, st)
    m = bdcompare.vmaf_of(out, srcs[clip], work, sids[clip])
    kbps = os.path.getsize(out) * 8 / (nfr[clip] / fps[clip]) / 1000
    ins = instr_of(clip, binp, env, rate, st) if INSTR else None
    return (clip, name, rate, kbps, m.get(bdcompare.GATE) if m else None, psnr_y(out, clip), ins)


def rungs(clip):
    lad = clips[clip][0]
    # RATE_SCALE moves the whole ladder (the ladder shift: a real effect keeps
    # its sign when every rung moves 13%; the band's own chaos does not)
    sc = float(os.environ.get("RATE_SCALE", "1"))
    r = [int(round(x * sc)) for x in [lad[0] * 0.8] + list(lad)]
    # DROP_TOP=1 drops the top rung, for a clip whose top rung no encode can
    # reach (touchdown_420 overshoots it by 120%+ on every binary)
    return r[:-1] if os.environ.get("DROP_TOP") == "1" else r


# pass 1 first, one per (clip, arm), so the rung cells never race to create it
with fut.ThreadPoolExecutor(max_workers=JOBS) as ex:
    list(ex.map(lambda ca: pass1(ca[0], ca[1][1], ca[1][2]),
                [(c, a) for c in clips for a in ALL]))
with fut.ThreadPoolExecutor(max_workers=JOBS) as ex:
    res = list(ex.map(lambda t: cell(*t),
                      [(c, a, r) for c in clips for a in ALL for r in rungs(c)]))

tab = {}
tins = {}
for clip, name, rate, kbps, v, p, ins in res:
    tab.setdefault((clip, name), []).append((rate, kbps, v, p))
    tins[(clip, name, rate)] = (ins, v)

summ = {a[0]: {"bd": [], "bdp": [], "err": []} for a in ALL}
for clip in clips:
    base = sorted(tab[(clip, "base")])
    berr = [100 * (k / r - 1) for r, k, _, _ in base]
    summ["base"]["err"] += berr
    print(f"{clip:16} base  err% " + " ".join(f"{e:+.2f}" for e in berr)
          + "   " + " ".join(f"{k:.0f}/{v:.2f}" for _, k, v, _ in base))
    for name, _, _ in ARMS:
        a = sorted(tab[(clip, name)])
        aerr = [100 * (k / r - 1) for r, k, _, _ in a]
        bd = bdcompare.bd_rate([k for _, k, _, _ in base], [v for _, _, v, _ in base],
                               [k for _, k, _, _ in a], [v for _, _, v, _ in a])
        bdp = bdcompare.bd_rate([k for _, k, _, _ in base], [p for _, _, _, p in base],
                                [k for _, k, _, _ in a], [p for _, _, _, p in a])
        summ[name]["bd"].append(bd if bd is not None else float("nan"))
        summ[name]["bdp"].append(bdp if bdp is not None else float("nan"))
        summ[name]["err"] += aerr
        print(f"{clip:16} {name:5} err% " + " ".join(f"{e:+.2f}" for e in aerr)
              + f"   BD-NEG {bd:+.2f}%  BD-PSNRY {bdp:+.2f}%" if bd is not None
              else f"{clip:16} {name:5} no overlap")

print(f"\nvmaf scores: {bdcompare.SCORE_STATS['miss']} computed, "
      f"{bdcompare.SCORE_STATS['hit']} cached")
be = [abs(x) for x in summ["base"]["err"]]
print(f"base   |rate err| median {statistics.median(be):.2f}%  worst {max(be):.2f}%")
for name, _, _ in ARMS:
    s = summ[name]
    ae = [abs(x) for x in s["err"]]
    ok = [x for x in s["bd"] if not math.isnan(x)]
    okp = [x for x in s["bdp"] if not math.isnan(x)]
    print(f"{name:6} BD-NEG median {statistics.median(ok):+.2f}% worst {max(ok):+.2f}%  "
          f"BD-PSNRY median {statistics.median(okp):+.2f}% worst {max(okp):+.2f}%  "
          f"|rate err| median {statistics.median(ae):.2f}% worst {max(ae):.2f}%")


def iso_instr(base_pts, v):
    """Base instructions at VMAF-NEG v, log-linear between the base rungs that
    bracket it; None outside the base curve's range."""
    pts = sorted(base_pts)
    for (v0, i0), (v1, i1) in zip(pts, pts[1:]):
        if v0 <= v <= v1 and v1 > v0:
            f = (v - v0) / (v1 - v0)
            return math.exp(math.log(i0) + f * (math.log(i1) - math.log(i0)))
    return None


if INSTR:
    print("\ninstructions, pass 2 (arm / base - 1, per rung; iso = against base at "
          "the arm's VMAF-NEG, interpolated on the base ladder)")
    for name, _, _ in ARMS:
        tot_a = tot_b = 0.0
        hit_a = hit_b = 0.0
        iso_a = iso_b = 0.0
        worst = (-1e9, "")
        for clip in clips:
            rs = rungs(clip)
            rr, iso = [], []
            bpts = [(tins[(clip, "base", r)][1], tins[(clip, "base", r)][0]) for r in rs]
            for r in rs:
                ia, va = tins[(clip, name, r)]
                ib, _ = tins[(clip, "base", r)]
                tot_a += ia; tot_b += ib
                kb = [k for rr_, k, _, _ in tab[(clip, "base")] if rr_ == r][0]
                if abs(kb / r - 1) <= 0.10:
                    hit_a += ia; hit_b += ib
                rr.append(100 * (ia / ib - 1))
                bi = iso_instr(bpts, va)
                if bi:
                    iso_a += ia; iso_b += bi
                    iso.append(100 * (ia / bi - 1))
            w = max(rr)
            if w > worst[0]:
                worst = (w, clip)
            print(f"{clip:16} {name:5} instr% " + " ".join(f"{x:+.2f}" for x in rr)
                  + f"   worst {w:+.2f}  iso " + " ".join(f"{x:+.2f}" for x in iso))
        print(f"{name:6} INSTR aggregate over rungs the base lands within 10% of: "
              f"{100 * (hit_a / hit_b - 1):+.2f}%")
        print(f"{name:6} INSTR aggregate {100 * (tot_a / tot_b - 1):+.2f}%  worst rung "
              f"{worst[0]:+.2f}% ({worst[1]})  iso-quality aggregate "
              f"{100 * (iso_a / iso_b - 1) if iso_b else float('nan'):+.2f}%")
