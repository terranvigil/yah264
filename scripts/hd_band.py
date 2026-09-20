#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The HD band: BD-VMAF-NEG for several arms at once, on one CRF ladder.

`band_at_rate.py` answers the same question for the standing twelve-clip
ladder, which is seven CIF clips and five HD ones. The clips engineers test
with are 720p and 1080p at the bitrates people use, and docs/hd-parity-plan.md
makes that band the bar every stage-2 candidate is read against. This is that
band.

THE DEFAULT MODE IS `interp`, AND IT DOES NOT SOLVE ONTO THE ANCHOR'S BYTES.
Anchor and arm run at the SAME five CRF values and the BD is taken over
whatever rate pairs come back, which is what BD-rate was defined to do: two
curves, one integration over the range they share, no requirement that the
points line up. The matched-byte solve it replaces could not be read below
`medium`. The frame QP is an integer, so a clip's CRF-to-bytes curve is a
staircase, a rung that lands on a step edge has no CRF on either curve that
produces the target bytes, and the solve returns the nearest step instead. 89
of 180 solves at `fast` missed by more than 2%, up to 9.8%, and that
displacement was then read as quality: two arms that intend NOTHING read a
worst clip of +0.61% and +1.05%, so the instrument failed a +0.5% per-clip rule
by itself. More bisection is not the fix -- ITERS already resolves the CRF to
0.023 and ducks_720p's whole step is 0.06 wide.

Three things follow, and all three are printed rather than assumed:

- THE OVERLAP GUARD. The two curves no longer sit at the same rates, so a clip
  whose arm curve has shifted off the anchor's range loses rungs from the
  integration. Every cell prints its rate overlap, its quality overlap and the
  rungs that survived, and a clip under MIN_RUNGS of the five is NOT SCORED.
- FIX A, THE FALLBACK. A clip the guard refuses is re-read with its rungs moved
  to the middle of the steps they land in, probed once per clip and preset and
  cached under $Y264_CACHE as ffboard's solve cache is. Those rows print `[A]`,
  because A narrows the offsets and does not remove them.
- THE CONTROLS. Two near-null arms run in EVERY band by default, and each arm's
  cell prints raw and control-referenced. A per-clip bar below the controls'
  own spread on that clip is not a reading, and the band prints UNRESOLVED
  rather than PASS there.

`SOLVE=1` restores the matched-byte ladder, unchanged, for the pages that
document it.

    ARMS='c1=Y264_B_PREME_SKIP=1;c3=Y264_RD_SURV_RANK=1' python3 scripts/hd_band.py
    ARMS='c1=Y264_B_PREME_SKIP=1' CLIPS=sunflower_1080p JOBS=4 python3 scripts/hd_band.py
    PRESET=fast ARMS='c1=Y264_B_PREME_SKIP=1' python3 scripts/hd_band.py
    SOLVE=1 ARMS='c1=Y264_B_PREME_SKIP=1' python3 scripts/hd_band.py
"""
import hashlib, json, math, os, statistics, subprocess, sys, threading
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import scratch                                                  # noqa: E402
import bdcompare as bd

bd.SUBSAMPLE = 1

# Six 720p and six 1080p, weighted to live action, and carrying the four clips
# `make review` prints plus the two cells the stage-0 profile was taken on.
BAND = ["bbb_720p", "perseverance_720p", "park_joy_720p",
        "shields_720p", "fourpeople_720p", "ducks_720p",
        "bbb10s_1080p_o120", "perseverance_1080p", "sunflower_1080p",
        "pedestrian_1080p", "riverbed_1080p", "crowd_run_1080p"]
# Five rungs. HD at low-to-mid rate is the regime the plan names, so the ladder
# sits where those clips are actually shipped rather than at the standing band's
# near-transparent top.
RUNGS = [float(x) for x in os.environ.get("RUNGS", "23,26,29,32,35").split(",")]
FRAMES = int(os.environ.get("FRAMES", "120"))
JOBS = int(os.environ.get("JOBS", "4"))
N = os.environ.get("YAH264", "build/cli/yah264")
WIN = float(os.environ.get("WINDOW", "1.5"))
ITERS = int(os.environ.get("ITERS", "7"))

# interp is the default; SOLVE=1 is the matched-byte ladder it replaced.
SOLVE = os.environ.get("SOLVE", "0") == "1"
# How many of the five rungs must survive the overlap for a clip to be scored.
MIN_RUNGS = int(os.environ.get("MIN_RUNGS", "4"))
# Fix A, the mid-step rungs, for a clip the guard refuses. FIXA=0 leaves those
# clips unscored instead, which is the honest reading when A is under suspicion.
FIXA = os.environ.get("FIXA", "1") == "1"
PROBE_D = float(os.environ.get("PROBE_D", "0.25"))
# The bars a verdict is read against. Per clip and on the median, in percent.
BAR = float(os.environ.get("BAR", "0.5"))
MEDIAN_BAR = float(os.environ.get("MEDIAN_BAR", "0.2"))

# The two near-null CONTROL arms. A per-clip bar needs the band's own per-clip
# floor measured before it can be read, and one control is a number while two
# that agree are evidence: at `veryfast` these two never disagreed by more than
# 0.30 over twelve clips, so what they read is a systematic offset of the
# instrument and not scatter. They run in every band; CONTROLS=none drops them
# and with them every control-referenced column.
CONTROLS = os.environ.get("CONTROLS", "ctl=Y264_MBT_DEPFLOOR=16;ctl2=Y264_MBT_DEPFLOOR=8")

# PRESET names the band's baseline, and BOTH sides of every A/B move with it:
# an arm is read against the SAME preset with the arm off, never against
# medium. The two tool flags spell out medium's own row, so they are inert
# everywhere except ultrafast, whose row is CAVLC and no 8x8; there they are
# dropped and the preset's tool-set stands, or the band would be measuring
# superfast under ultrafast's name.
PRESET = os.environ.get("PRESET", "medium")
TOOLS = "" if PRESET == "ultrafast" else "--cabac --transform-8x8 "
BASE = (f'{N} --input-y4m {{src}} --frames {FRAMES} --preset {PRESET} {TOOLS}'
        f'--crf {{q}} --threads 1 -o {{out}}')

clips = [c.strip() for c in os.environ.get("CLIPS", "").split(",") if c.strip()] or BAND


def parse_arms(spec):
    out = []
    for one in spec.split(";"):
        one = one.strip()
        if not one:
            continue
        name, _, env = one.partition("=")
        out.append((name, env))
    return out


arms = parse_arms(os.environ.get("ARMS", ""))
if not arms:
    sys.exit("set ARMS='name=ENV=VAL[ ENV2=VAL2];name2=...' : this is a self-A/B gate")
controls = [] if CONTROLS.lower() in ("none", "0", "") else parse_arms(CONTROLS)
# An arm the caller named explicitly wins over the same env arriving as a
# control, so a band that WANTS to read a control as an arm still gets one row.
_seen = {e for _, e in arms}
controls = [(n, e) for n, e in controls if e not in _seen]
ctl_names = [n for n, _ in controls]
allarms = arms + controls


# ---------------------------------------------------------------- identity ---
# The measurement cache survives between bands and between weeks, which is what
# makes it worth having and what makes it dangerous: ffboard's solve cache once
# handed a board answers produced by a library whose CRF scale had since moved,
# and every ratio on that board was meaningless. So every key carries a sha of
# the binary that produced the number, and an entry from another build is not
# merely stale, it is unreachable.
def _sha12(path):
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()[:12]
    except OSError:
        return "?"


BIN = N if os.path.isabs(N) else os.path.join(ROOT, N)
LIB_ID = _sha12(BIN)
CACHE = scratch.cache_dir("hdband")
POINTS_PATH = os.path.join(CACHE, "points.json")
LADDER_PATH = os.path.join(CACHE, "ladder.json")
NO_CACHE = os.environ.get("NO_POINT_CACHE", "0") == "1"
_lock = threading.Lock()
_hits = {"hit": 0, "miss": 0}


def _load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return {}


_points = {} if NO_CACHE else _load(POINTS_PATH)
_ladders = {} if NO_CACHE else _load(LADDER_PATH)


def _save(path, obj):
    if NO_CACHE:
        return
    tmp = f"{path}.{os.getpid()}.tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f)
    os.replace(tmp, path)


# ---------------------------------------------------------------- encoding ---
def enc(env, q, src, out):
    cmd = BASE.format(q=f"{q:.4f}", src=src, out=out)
    if env:
        cmd = env + " " + cmd
    subprocess.run(cmd, shell=True, capture_output=True, cwd=ROOT)
    return os.path.getsize(out) if os.path.exists(out) else 0


def _key(clip, env, q):
    return f"{LIB_ID}:{PRESET}:{FRAMES}:{clip}:{env}:{q:.4f}"


def size_at(env, clip, src, q, work):
    """Bytes only. The ladder probe wants no VMAF, so it pays no VMAF.

    It runs on a pool worker like every other encode: a probe run in the clip's
    own driver thread would be an encode outside the band's core budget, and
    twelve drivers probing at once would double the box share the band was
    given."""
    with _lock:
        hit = _points.get(_key(clip, env, q))
    if hit:
        return hit[0]
    out = os.path.join(work, f"sz{q:.4f}.264")
    sz = enc(env, q, src, out)
    try:
        os.unlink(out)
    except OSError:
        pass
    return sz


def point(env, clip, src, q, work, tag):
    """(bytes, VMAF-NEG) at this CRF, cached across runs.

    A band that reused a whole table of cached cells used to look exactly like
    a band that measured them, so the hit and miss counts are printed per clip
    rather than left to be inferred."""
    k = _key(clip, env, q)
    with _lock:
        hit = _points.get(k)
    if hit:
        with _lock:
            _hits["hit"] += 1
        return hit[0], hit[1]
    out = os.path.join(work, f"{tag}_{q:.4f}.264")
    sz = enc(env, q, src, out)
    v = bd.vmaf_of(out, src, work) if sz else None
    try:
        os.unlink(out)
    except OSError:
        pass
    if not sz or not v or "VMAF-NEG" not in v:
        return None, None
    with _lock:
        _hits["miss"] += 1
        _points[k] = [sz, v["VMAF-NEG"]]
        _save(POINTS_PATH, _points)
    return sz, v["VMAF-NEG"]


# ------------------------------------------------------------- the guard -----
def overlap(rr, rt):
    """(rungs the integration spans, rate overlap as a fraction of the two
    curves' combined range).

    Both curves are measured and neither is solved, so the integration runs
    over the rate range they share, in the log domain the ladder is built in.
    What that costs is LADDER, not points: each curve is fitted over all five
    of its own rungs and the shared range lies inside both, so neither side is
    ever extrapolated, and the failure to guard against is a shared range too
    SHORT to be a ladder. So the overlap is measured against the union of the
    two ranges and reported in the unit the ladder is written in -- five rungs
    is four rung-spacings, and a shared range covering three of those four is
    four rungs of five. Counting the points that happen to fall inside instead
    would read an arm whose whole curve sits inside the anchor's as the WORST
    case, when it is the completely covered one."""
    lo, hi = max(min(rr), min(rt)), min(max(rr), max(rt))
    ulo, uhi = min(min(rr), min(rt)), max(max(rr), max(rt))
    span = math.log(uhi) - math.log(ulo)
    if hi <= lo or span <= 0:
        return 0, 0.0
    frac = (math.log(hi) - math.log(lo)) / span
    n = max(0, min(len(rr), round(frac * (len(rr) - 1)) + 1))
    return n, frac


def q_overlap(mr, mt):
    """The quality overlap, which is the interval the BD integral runs over."""
    lo, hi = max(min(mr), min(mt)), min(max(mr), max(mt))
    span = max(mr) - min(mr)
    if hi <= lo or span <= 0:
        return 0.0
    return (hi - lo) / span


# ------------------------------------------------------------- fix A ---------
def ladder(clip, src, work, ex):
    """Five rungs moved to the middle of the steps they land in.

    The anchor's CRF-to-bytes curve is a staircase, so a rung on a step edge is
    a rung whose whole neighbourhood is edge. Each nominal rung is probed once
    on each side; it moves into whichever half changes the bytes less, and
    stays put when both halves are flat, because it is then already interior to
    a flat wider than the probe. Eleven encodes a clip against the forty-five
    the band spends there, and it is a property of the CLIP and the PRESET and
    not of any arm, so it is cached and paid once."""
    key = (f"{LIB_ID}:{PRESET}:{FRAMES}:{clip}:"
           f"{','.join(f'{q:g}' for q in RUNGS)}:d{PROBE_D}")
    with _lock:
        hit = _ladders.get(key)
    if hit:
        return [float(x) for x in hit], True
    probe = {(q0, d): ex.submit(size_at, "", clip, src, q0 + d, work)
             for q0 in RUNGS for d in (0.0, -PROBE_D, PROBE_D)}
    out = []
    for q0 in RUNGS:
        s0 = probe[(q0, 0.0)].result()
        slo = probe[(q0, -PROBE_D)].result()
        shi = probe[(q0, PROBE_D)].result()
        if not s0 or not slo or not shi:
            out.append(q0)
            continue
        if slo == s0 and shi == s0:
            out.append(q0)                          # already mid-flat
            continue
        dlo, dhi = abs(slo - s0) / s0, abs(shi - s0) / s0
        out.append(q0 - PROBE_D / 2 if dlo < dhi else q0 + PROBE_D / 2)
    with _lock:
        _ladders[key] = out
        _save(LADDER_PATH, _ladders)
    return out, False


# ------------------------------------------------------------- the two modes -
def solve(env, src, target, out, centre):
    """The arm's CRF landing nearest `target` bytes, inside a window around the
    anchor. Returns (crf, bracketed). SOLVE=1 only.

    The window is not probed at its edges first. Bisecting it directly costs
    two encodes less, and a solve that ran out of window announces itself
    anyway: it converges onto an edge, so a result within one step of either
    end is reported UNBRACKETED and dropped rather than scored."""
    lo, hi = centre - WIN, centre + WIN
    step = (hi - lo) / (1 << ITERS)
    for _ in range(ITERS):
        mid = (lo + hi) / 2.0
        if enc(env, mid, src, out) > target:
            lo = mid
        else:
            hi = mid
    q = (lo + hi) / 2.0
    return q, (q > centre - WIN + 2 * step and q < centre + WIN - 2 * step)


def run_solve(clip):
    """The matched-byte ladder, as it was before interp mode. SOLVE=1."""
    src = os.path.join(ROOT, "tests", "corpus", clip + ".y4m")
    work = scratch.mkdtemp("hdband")
    out = os.path.join(work, "s.264")
    lines, anchor = [], []
    for q in RUNGS:
        sz = enc("", q, src, out)
        v = bd.vmaf_of(out, src, work)
        if not sz or not v or "VMAF-NEG" not in v:
            return clip, [f"{clip}: default rung {q} failed"], {}, {}
        anchor.append((q, sz, v["VMAF-NEG"]))
        lines.append(f"    {clip:<20} default crf {q:5.2f}  {sz:>9} B  NEG {v['VMAF-NEG']:6.2f}")
    res = {}
    for name, env in allarms:
        pts, ok = [], True
        for q0, target, _ in anchor:
            q, bracketed = solve(env, src, target, out, q0)
            sz = enc(env, q, src, out)
            v = bd.vmaf_of(out, src, work)
            if not v or "VMAF-NEG" not in v:
                ok = False
                break
            pts.append((sz, v["VMAF-NEG"]))
            lines.append(f"    {clip:<20} {name:<8} crf {q:5.2f}  {sz:>9} B  "
                         f"NEG {v['VMAF-NEG']:6.2f}  ({100.0 * sz / target - 100:+.2f}% of target)"
                         + ("" if bracketed else "  UNBRACKETED"))
            if not bracketed:
                ok = False
        if not ok:
            lines.append(f"  {clip:<20} {name}: not scored")
            continue
        r = bd.bd_rate([a[1] for a in anchor], [a[2] for a in anchor],
                       [p[0] for p in pts], [p[1] for p in pts])
        if isinstance(r, float):
            res[name] = r
            lines.append(f"  {clip:<20} {name:<8} BD-VMAF-NEG {r:+.2f}%")
    scratch.release(work)
    return clip, lines, res, {"mode": "solve"}


def curves(clip, src, rungs, work, lines, ex):
    """Every (clip, arm, rung) cell for one clip, submitted at once.

    Under interp there is no bisection, so the whole clip's work is known
    before any of it runs and the shared pool can be filled with it. The pool
    is the band's core budget either way: JOBS encodes, never JOBS x clips."""
    jobs = {}
    for name, env in [("default", "")] + allarms:
        for q in rungs:
            jobs[(name, q)] = ex.submit(point, env, clip, src, q, work, name)
    out = {}
    for name, _ in [("default", "")] + allarms:
        pts = []
        for q in rungs:
            sz, neg = jobs[(name, q)].result()
            if sz is None:
                lines.append(f"    {clip:<20} {name:<8} crf {q:5.2f}  FAILED")
                pts = None
                break
            pts.append((q, sz, neg))
        out[name] = pts
    return out


def score(clip, pts, lines, tag):
    """BD per arm against the anchor, with the overlap printed beside it."""
    anchor = pts["default"]
    res, guard = {}, {}
    if not anchor:
        lines.append(f"  {clip:<20} anchor failed: nothing scored")
        return res, guard
    for q, sz, neg in anchor:
        lines.append(f"    {clip:<20} default  crf {q:5.2f}  {sz:>9} B  NEG {neg:6.2f}")
    rr = [a[1] for a in anchor]
    mr = [a[2] for a in anchor]
    for name, _ in allarms:
        p = pts[name]
        if not p:
            lines.append(f"  {clip:<20} {name}: not scored (encode or VMAF failed)")
            continue
        rt = [x[1] for x in p]
        mt = [x[2] for x in p]
        for (q, sz, neg), a in zip(p, anchor):
            lines.append(f"    {clip:<20} {name:<8} crf {q:5.2f}  {sz:>9} B  "
                         f"NEG {neg:6.2f}  ({100.0 * sz / a[1] - 100:+.2f}% of anchor)")
        n, frac = overlap(rr, rt)
        qf = q_overlap(mr, mt)
        guard[name] = (n, frac, qf)
        tail = (f"  rate ovl {100 * frac:5.1f}%  vmaf ovl {100 * qf:5.1f}%  "
                f"rungs {n}/{len(rr)}{tag}")
        if n < MIN_RUNGS:
            lines.append(f"  {clip:<20} {name:<8} NOT SCORED: overlap under "
                         f"{MIN_RUNGS} rungs{tail}")
            continue
        r = bd.bd_rate(rr, mr, rt, mt)
        if isinstance(r, float):
            res[name] = r
            lines.append(f"  {clip:<20} {name:<8} BD-VMAF-NEG {r:+.2f}%{tail}")
        else:
            lines.append(f"  {clip:<20} {name:<8} NOT SCORED: no quality overlap{tail}")
    return res, guard


def run_interp(clip, ex):
    src = os.path.join(ROOT, "tests", "corpus", clip + ".y4m")
    work = scratch.mkdtemp("hdband")
    lines = []
    h0 = dict(_hits)
    pts = curves(clip, src, RUNGS, work, lines, ex)
    res, guard = score(clip, pts, lines, "")
    used = {"mode": "B", "rungs": list(RUNGS), "guard": guard}
    short = [n for n, g in guard.items() if g[0] < MIN_RUNGS]
    if short and FIXA:
        # The guard refused a cell on this clip, so the WHOLE clip is re-read on
        # mid-step rungs: one row of the table has to be one measurement.
        lad, cached = ladder(clip, src, work, ex)
        lines.append(f"  {clip:<20} FIX A: {', '.join(short)} under {MIN_RUNGS} rungs; "
                     f"rungs -> [{', '.join(f'{q:.3f}' for q in lad)}]"
                     f" ({'cached ladder' if cached else 'probed'})")
        pts = curves(clip, src, lad, work, lines, ex)
        res, guard = score(clip, pts, lines, "  [A]")
        used = {"mode": "A", "rungs": lad, "guard": guard}
    hit, miss = _hits["hit"] - h0["hit"], _hits["miss"] - h0["miss"]
    lines.append(f"  {clip:<20} cells: {hit} cache hit, {miss} measured")
    scratch.release(work)
    return clip, lines, res, used


# ------------------------------------------------------------- reporting -----
def verdict(name, raw, ctl, spread):
    """PASS / REFUSED / UNRESOLVED, and the clip it turns on.

    An arm passes only where BOTH readings pass, raw and control-referenced,
    and it is refused where either fails: the rule the preset ladder was read
    with. Where the per-clip bar is BELOW the controls' own spread on a clip,
    the band has not resolved that clip -- two changes that both intend nothing
    differ there by more than the thing being measured -- so the answer is
    UNRESOLVED, and PASS is not available."""
    if not raw:
        return "nothing scored"
    worst_clip = max(raw, key=lambda c: raw[c])
    med = statistics.median(raw.values())
    over = [c for c in raw if raw[c] > BAR]
    # The most favourable referenced reading: if even that is over the bar, the
    # arm is refused on the referenced reading too.
    for c in raw:
        if c not in over and ctl.get(c) and min(raw[c] - v for v in ctl[c]) > BAR:
            over.append(c)
    if over:
        c = max(over, key=lambda c: raw[c])
        return f"REFUSED on {c} {raw[c]:+.2f}% (bar {BAR:+.2f}% per clip)"
    if med > MEDIAN_BAR:
        return f"REFUSED on the median {med:+.2f}% (bar {MEDIAN_BAR:+.2f}%)"
    blind = [c for c in raw if spread.get(c, 0.0) > BAR]
    if blind:
        c = max(blind, key=lambda c: spread[c])
        return (f"UNRESOLVED: the controls differ by {spread[c]:.2f} on {c}, over "
                f"the {BAR:.2f} bar, so that clip is not read")
    return f"PASS  worst {worst_clip} {raw[worst_clip]:+.2f}%  median {med:+.2f}%"


def main():
    mode = "solve (matched bytes)" if SOLVE else "interp (one ladder, BD over the overlap)"
    print(f"band:  {len(clips)} clips, rungs {RUNGS}, {FRAMES} frames, preset {PRESET}")
    print(f"mode:  {mode}   binary {os.path.basename(BIN)} {LIB_ID}"
          + ("" if SOLVE else f"   guard {MIN_RUNGS}/{len(RUNGS)} rungs, "
                              f"fix A {'on' if FIXA else 'off'}"))
    for name, env in arms:
        print(f"arm {name}: {env}")
    for name, env in controls:
        print(f"control {name}: {env}")
    print(flush=True)
    allres = {name: {} for name, _ in allarms}
    used, drv = {}, None
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        if SOLVE:
            futs = [ex.submit(run_solve, c) for c in clips]
        else:
            # The clip drivers do not encode. They submit into the pool above
            # and wait on it, so they need threads of their own or the pool
            # would deadlock on its own work.
            drv = ThreadPoolExecutor(max_workers=len(clips))
            futs = [drv.submit(run_interp, c, ex) for c in clips]
        for fut in as_completed(futs):
            clip, lines, res, u = fut.result()
            print("\n".join(lines), flush=True)
            used[clip] = u
            for name, v in res.items():
                allres[name][clip] = v
    if drv:
        drv.shutdown()

    # The controls' spread per clip: the band's own floor on that clip, and the
    # thing a per-clip bar has to clear before it is a bar at all.
    spread = {}
    if len(ctl_names) >= 2:
        for c in clips:
            vals = [allres[n][c] for n in ctl_names if c in allres[n]]
            if len(vals) >= 2:
                spread[c] = max(vals) - min(vals)

    print("\n=== BD-VMAF-NEG" + (" at matched achieved bitrate" if SOLVE
                                 else ", one CRF ladder, BD over the overlap")
          + " (negative = arm better) ===")
    for name, _ in allarms:
        r = allres[name]
        kind = "control" if name in ctl_names else "arm"
        if not r:
            print(f"\n  {kind} {name}: nothing scored")
            continue
        print(f"\n  {kind} {name}")
        head = f"    {'clip':<20} {'raw':>7}"
        refd = bool(ctl_names) and name not in ctl_names
        if refd:
            head += "".join(f" {'vs ' + n:>8}" for n in ctl_names)
        print(head + f" {'spread':>7}  mode")
        for clip, v in sorted(r.items(), key=lambda kv: kv[1]):
            row = f"    {clip:<20} {v:+7.2f}"
            if refd:
                for n in ctl_names:
                    cv = allres[n].get(clip)
                    row += f" {v - cv:+8.2f}" if cv is not None else f" {'--':>8}"
            row += f" {spread[clip]:7.2f}" if clip in spread else f" {'--':>7}"
            print(row + f"  {used.get(clip, {}).get('mode', '?')}")
        vals = list(r.values())
        worst = max(r.items(), key=lambda kv: kv[1])
        print(f"    median {statistics.median(vals):+.2f}%   mean {statistics.mean(vals):+.2f}%   "
              f"worst {worst[0]} {worst[1]:+.2f}%   scored {len(vals)}/{len(clips)}")
        if refd:
            ctl = {c: [allres[n][c] for n in ctl_names if c in allres[n]] for c in r}
            print(f"    verdict: {verdict(name, r, ctl, spread)}")

    if not SOLVE:
        a = sorted(c for c, u in used.items() if u.get("mode") == "A")
        print("\n  read under fix A (mid-step rungs): " + (", ".join(a) if a else "none"))
        print(f"  cells: {_hits['hit']} cache hit, {_hits['miss']} measured"
              + ("  [cache off]" if NO_CACHE else ""))


main()
