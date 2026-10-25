#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The HD band: BD-VMAF-NEG for several arms at once, on one CRF ladder.

`band_at_rate.py` answers the same question for the standing twelve-clip
ladder, which is seven CIF clips and five HD ones. The clips engineers test
with are 720p and 1080p at the bitrates people use, and docs/plan.md
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
- THE NULL. Each arm's cell prints raw and referenced to the clip's null: the
  stored spread scripts/hd_null.py fits from the point cache, or one live
  control where the defaults changed since that fit. A per-clip bar below the
  null's own spread on that clip is not a reading, and the band prints
  UNRESOLVED rather than PASS there. CONTROLS=... restores live controls.

`SOLVE=1` restores the matched-byte ladder, unchanged, for the pages that
document it.

    ARMS='c1=--b-preme-skip 1;c3=--rd-surv-rank 1' python3 scripts/hd_band.py
    ARMS='c1=--b-preme-skip 1' CLIPS=sunflower_1080p JOBS=4 python3 scripts/hd_band.py
    PRESET=fast ARMS='c1=--b-preme-skip 1' python3 scripts/hd_band.py
    SOLVE=1 ARMS='c1=--b-preme-skip 1' python3 scripts/hd_band.py
    ARMS='q6=Y264_B8_QGATE=6 --psy-rd 1.5' python3 scripts/hd_band.py
    YAH264=/base/build-rel/cli/yah264 ARMS='int=@/int/build-rel/cli/yah264' EARLY_STOP=0 python3 scripts/hd_band.py
"""
import hashlib, json, math, os, shlex, statistics, subprocess, sys, threading
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import scratch                                                  # noqa: E402
import bdcompare as bd

bd.SUBSAMPLE = 1
# The band reads NEG and nothing else, so it scores NEG and nothing else:
# VMAF_MODELS=both restores the two-model pass. NEG's value is the same either
# way, so every cached point stays comparable.
bd.MODELS = os.environ.get("VMAF_MODELS", "neg")

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

# THE NULL. A per-clip bar needs the band's own per-clip floor before it can be
# read. That floor used to be measured live in every band, by two near-null
# control arms (Y264_MBT_DEPFLOOR=16 and =8), which cost two curves of every
# seven. It is now a STORED per-clip spread, fitted by scripts/hd_null.py from
# the 10-20 control reads per clip the point cache already holds, and dated and
# tied to a reference binary. Each arm prints its reading referenced to the
# stored null's centre and "inside null spread" where it lies within two
# standard deviations of it. A clip whose anchor bytes differ from the
# reference binary's -- the defaults changed since the fit -- runs ONE live
# control (LIVE_CONTROL) and is referenced to that read instead. A clip with no
# stored fit does the same.
#
# CONTROLS='name=ENV;...' restores live controls on every clip, with no stored
# null: the old behaviour, and the way to read the controls themselves.
# CONTROLS=none drops every control-referenced column.
CONTROLS = os.environ.get("CONTROLS", "auto")
LIVE_CONTROL = os.environ.get("LIVE_CONTROL", "ctl=Y264_MBT_DEPFLOOR=16")
NULL_SD = float(os.environ.get("NULL_SD", "2"))      # half-width, in sd

# EARLY STOP. An arm is refused, and its cells still queued are skipped, once
# one clip reads worse than EARLY_ONE or EARLY_N clips read worse than
# EARLY_SOME (raw BD-VMAF-NEG, percent). Either already fails the per-clip bar,
# so the clips not read cannot change the verdict. Replayed in band order over
# the 37 arms in the point cache with 11+ clips on the nominal rungs, it skips
# 14% of their clips. Controls never stop early. EARLY_STOP=0 reads every clip.
EARLY_STOP = os.environ.get("EARLY_STOP", "1") == "1"
EARLY_ONE = float(os.environ.get("EARLY_ONE", "1.0"))
EARLY_SOME = float(os.environ.get("EARLY_SOME", "0.5"))
EARLY_N = int(os.environ.get("EARLY_N", "3"))

# PRESET names the band's baseline, and BOTH sides of every A/B move with it:
# an arm is read against the SAME preset with the arm off, never against
# medium. The two tool flags spell out medium's own row, so they are inert
# everywhere except ultrafast, whose row is CAVLC and no 8x8; there they are
# dropped and the preset's tool-set stands, or the band would be measuring
# superfast under ultrafast's name.
PRESET = os.environ.get("PRESET", "medium")
# Encoder threads for every encode in the band (both sides). 1 is the
# standing band; any other count is a separate cache namespace.
THREADS = int(os.environ.get("THREADS", "1"))
TOOLS = "" if PRESET == "ultrafast" else "--cabac --transform-8x8 "
BASE = (f'{N} --input-y4m {{src}} --frames {FRAMES} --preset {PRESET} {TOOLS}'
        f'--crf {{q}} --threads {THREADS} -o {{out}}')
# the threads count joins the cache keys only when it is not the standing 1,
# so every cached t1 point stays addressable
PKEY = PRESET if THREADS == 1 else f"{PRESET}@t{THREADS}"

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
bd.require_dev_knobs(N if os.path.isabs(N) else os.path.join(ROOT, N),
                     os.environ.get("ARMS", ""), CONTROLS, LIVE_CONTROL)
if not arms:
    sys.exit("set ARMS='name=ENV=VAL[ ENV2=VAL2][ --flag v];name2=...' : this is a self-A/B gate")
AUTO = CONTROLS.lower() == "auto" and not SOLVE
if AUTO:
    controls = []
elif CONTROLS.lower() in ("none", "0", "", "auto"):
    controls = [] if CONTROLS.lower() != "auto" else parse_arms(
        "ctl=Y264_MBT_DEPFLOOR=16;ctl2=Y264_MBT_DEPFLOOR=8")   # SOLVE=1 keeps both
else:
    controls = parse_arms(CONTROLS)
# An arm the caller named explicitly wins over the same env arriving as a
# control, so a band that WANTS to read a control as an arm still gets one row.
_seen = {e for _, e in arms}
controls = [(n, e) for n, e in controls if e not in _seen]
ctl_names = [n for n, _ in controls]
allarms = arms + controls
live = parse_arms(LIVE_CONTROL)[:1] if AUTO else []
if live and live[0][1] in _seen:
    live = []


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
# Per CLIP, because twelve clips run at once and a global counter read at the
# top and bottom of one clip's driver counts every other clip's cells too. The
# first band run this way printed 157 cells for a 45-cell clip.
_hits = {}


def _count(clip, field):
    with _lock:
        _hits.setdefault(clip, {"hit": 0, "miss": 0, "probe": 0, "skip": 0})[field] += 1


def _load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return {}


_points = {} if NO_CACHE else _load(POINTS_PATH)
_ladders = {} if NO_CACHE else _load(LADDER_PATH)

# The stored null for this preset, frame count and ladder (scripts/hd_null.py).
NULL_KEY = f"{PKEY}:{FRAMES}:{','.join(f'{q:g}' for q in RUNGS)}"
NULL_FIT = (_load(os.environ.get("NULL_FIT", os.path.join(
    CACHE, "null.json" if bd.GATE == "VMAF-NEG" else f"null-{bd.GATE}-{bd.V1_RULE}.json")))
            .get(NULL_KEY) if AUTO else None)
if AUTO and not NULL_FIT:
    print(f"no stored null for {NULL_KEY}: every clip runs one live control "
          f"(scripts/hd_null.py fits one)", flush=True)

# Early-stop state: arm -> {clip: raw BD}, and the arms already refused.
_reads, _refused = {}, {}


def _save(path, obj):
    if NO_CACHE:
        return
    tmp = f"{path}.{os.getpid()}.tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f)
    os.replace(tmp, path)


# ---------------------------------------------------------------- encoding ---
# An arm whose env starts with "@<path>" runs ANOTHER BINARY, with whatever env
# follows: ARMS='int=@/path/to/new/yah264' reads a whole build against YAH264
# as the anchor, which is how an integration build is banded against its base.
# Its cells are keyed on that binary's sha, exactly as a band run with
# YAH264=<path> would key its default cells, so the two share the cache.
_arm_bins = {}


def split_bin(env):
    """(binary path, binary id, env) for an arm's env string."""
    if not env.startswith("@"):
        return N, LIB_ID, env
    path, _, rest = env[1:].partition(" ")
    with _lock:
        if path not in _arm_bins:
            full = path if os.path.isabs(path) else os.path.join(ROOT, path)
            _arm_bins[path] = _sha12(full)
    return path, _arm_bins[path], rest.strip()


def split_flags(env):
    """An arm's env string may carry encoder flags as well: everything from
    the first word starting with "--" goes after the command, the
    NAME=VALUE words before it stay env. Param overrides have no env knob
    (removed 2026-10-04), so their flags are how an arm sets them."""
    words = env.split()
    for i, w in enumerate(words):
        if w.startswith("--"):
            return " ".join(words[:i]), " ".join(words[i:])
    return env, ""


def enc(env, q, src, out):
    binp, _, env = split_bin(env)
    env, flags = split_flags(env)
    cmd = BASE.format(q=f"{q:.4f}", src=src, out=out)
    if binp != N:
        cmd = shlex.quote(binp) + cmd[len(N):]
    if flags:
        cmd = cmd + " " + flags
    if env:
        cmd = env + " " + cmd
    subprocess.run(cmd, shell=True, capture_output=True, cwd=ROOT)
    return os.path.getsize(out) if os.path.exists(out) else 0


def _key(clip, env, q):
    _, lib, env = split_bin(env)
    # a v1 point is its own cell; NEG keys stay as they were so the old cache serves them
    gate = "" if bd.GATE == "VMAF-NEG" else f":{bd.GATE}-{bd.V1_RULE}"
    return f"{lib}:{PKEY}:{FRAMES}:{clip}:{env}:{q:.4f}{gate}"


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
    _count(clip, "probe")
    out = os.path.join(work, f"sz{q:.4f}.264")
    sz = enc(env, q, src, out)
    try:
        os.unlink(out)
    except OSError:
        pass
    return sz


def point(env, clip, src, q, work, tag):
    """(bytes, VMAF-NEG) at this CRF, cached across runs; (-1, None) for a cell
    skipped because its arm was refused early.

    A band that reused a whole table of cached cells used to look exactly like
    a band that measured them, so the hit and miss counts are printed per clip
    rather than left to be inferred."""
    k = _key(clip, env, q)
    with _lock:
        hit = _points.get(k)
        refused = tag in _refused
    if hit:
        _count(clip, "hit")
        return hit[0], hit[1]
    if refused:
        _count(clip, "skip")
        return -1, None
    out = os.path.join(work, f"{tag}_{q:.4f}.264")
    sz = enc(env, q, src, out)
    v = bd.vmaf_of(out, src, work) if sz else None
    try:
        os.unlink(out)
    except OSError:
        pass
    if not sz or not v or bd.GATE not in v:
        return None, None
    with _lock:
        _points[k] = [sz, v[bd.GATE]]
        _save(POINTS_PATH, _points)
    _count(clip, "miss")
    return sz, v[bd.GATE]


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
    key = (f"{LIB_ID}:{PKEY}:{FRAMES}:{clip}:"
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
        if not sz or not v or bd.GATE not in v:
            return clip, [f"{clip}: default rung {q} failed"], {}, {}
        anchor.append((q, sz, v[bd.GATE]))
        lines.append(f"    {clip:<20} default crf {q:5.2f}  {sz:>9} B  {bd.GATE_SHORT} {v[bd.GATE]:6.2f}")
    res = {}
    for name, env in allarms:
        pts, ok = [], True
        for q0, target, _ in anchor:
            q, bracketed = solve(env, src, target, out, q0)
            sz = enc(env, q, src, out)
            v = bd.vmaf_of(out, src, work)
            if not v or bd.GATE not in v:
                ok = False
                break
            pts.append((sz, v[bd.GATE]))
            lines.append(f"    {clip:<20} {name:<8} crf {q:5.2f}  {sz:>9} B  "
                         f"{bd.GATE_SHORT} {v[bd.GATE]:6.2f}  ({100.0 * sz / target - 100:+.2f}% of target)"
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
            lines.append(f"  {clip:<20} {name:<8} BD-{bd.GATE} {r:+.2f}%")
    scratch.release(work)
    return clip, lines, res, {"mode": "solve"}


_submit_lock = threading.Lock()


def curves(clip, src, rungs, work, lines, ex, which):
    """Every (clip, arm, rung) cell for one clip, submitted at once.

    Under interp there is no bisection, so the whole clip's work is known
    before any of it runs and the shared pool can be filled with it. The pool
    is the band's core budget either way: JOBS encodes, never JOBS x clips.
    One clip's cells enter the queue together, so the queue runs roughly clip
    by clip, which is what lets an early stop skip whole clips of an arm."""
    jobs = {}
    with _submit_lock:
        for name, env in which:
            for q in rungs:
                jobs[(name, q)] = ex.submit(point, env, clip, src, q, work, name)
    out = {}
    for name, _ in which:
        pts = []
        for q in rungs:
            sz, neg = jobs[(name, q)].result()
            if sz == -1:
                pts = "skipped"
                continue
            if sz is None:
                lines.append(f"    {clip:<20} {name:<8} crf {q:5.2f}  FAILED")
                pts = None
                break
            if pts != "skipped":
                pts.append((q, sz, neg))
        out[name] = pts
    return out


def null_for(clip, anchor):
    """(centre, half-width, source) of the null this clip is referenced to, or
    None when the clip needs a live control: no stored fit for it, or its
    anchor bytes differ from the fit's reference binary's (defaults changed)."""
    if not NULL_FIT:
        return None
    c = NULL_FIT["clips"].get(clip)
    ref = NULL_FIT.get("anchor", {}).get(clip)
    if not c or not ref:
        return None
    if [a[1] for a in anchor] != ref:
        return None
    return c["centre"], NULL_SD * c["rsd"], "stored"


def note_read(name, clip, v):
    """Record an arm's raw reading and refuse the arm once it is over either
    early-stop rule. Returns the refusal reason the first time it trips."""
    if not EARLY_STOP or name in ctl_names or name in [n for n, _ in live]:
        return None
    with _lock:
        if name in _refused:
            return None
        r = _reads.setdefault(name, {})
        r[clip] = v
        some = [c for c, x in r.items() if x > EARLY_SOME]
        if v > EARLY_ONE:
            _refused[name] = f"{clip} {v:+.2f}% > {EARLY_ONE:+.2f}%"
        elif len(some) >= EARLY_N:
            _refused[name] = (f"{len(some)} clips > {EARLY_SOME:+.2f}% "
                              f"({', '.join(sorted(some))})")
        else:
            return None
        return _refused[name]


def score(clip, pts, lines, tag):
    """BD per arm against the anchor, with the overlap printed beside it."""
    anchor = pts["default"]
    res, guard = {}, {}
    if not anchor:
        lines.append(f"  {clip:<20} anchor failed: nothing scored")
        return res, guard
    for q, sz, neg in anchor:
        lines.append(f"    {clip:<20} default  crf {q:5.2f}  {sz:>9} B  {bd.GATE_SHORT} {neg:6.2f}")
    rr = [a[1] for a in anchor]
    mr = [a[2] for a in anchor]
    for name, _ in allarms + [l for l in live if l[0] in pts]:
        p = pts[name]
        if p == "skipped":
            lines.append(f"  {clip:<20} {name:<8} not read: arm refused early")
            continue
        if not p:
            lines.append(f"  {clip:<20} {name}: not scored (encode or VMAF failed)")
            continue
        rt = [x[1] for x in p]
        mt = [x[2] for x in p]
        for (q, sz, neg), a in zip(p, anchor):
            lines.append(f"    {clip:<20} {name:<8} crf {q:5.2f}  {sz:>9} B  "
                         f"{bd.GATE_SHORT} {neg:6.2f}  ({100.0 * sz / a[1] - 100:+.2f}% of anchor)")
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
            lines.append(f"  {clip:<20} {name:<8} BD-{bd.GATE} {r:+.2f}%{tail}")
        else:
            lines.append(f"  {clip:<20} {name:<8} NOT SCORED: no quality overlap{tail}")
    return res, guard


def run_interp(clip, ex):
    src = os.path.join(ROOT, "tests", "corpus", clip + ".y4m")
    work = scratch.mkdtemp("hdband")
    lines = []
    if AUTO:
        # The anchor first: its bytes decide whether this clip can use the
        # stored null or needs a live control.
        anchor = curves(clip, src, RUNGS, work, lines, ex, [("default", "")])["default"]
        nl = null_for(clip, anchor) if anchor else None
        which = arms + ([] if nl else live)
        pts = curves(clip, src, RUNGS, work, lines, ex, which)
        pts["default"] = anchor
    else:
        nl, which = None, allarms
        pts = curves(clip, src, RUNGS, work, lines, ex, [("default", "")] + which)
    res, guard = score(clip, pts, lines, "")
    used = {"mode": "B", "rungs": list(RUNGS), "guard": guard, "null": nl}
    short = [n for n, g in guard.items() if g[0] < MIN_RUNGS]
    if short and FIXA:
        # The guard refused a cell on this clip, so the WHOLE clip is re-read on
        # mid-step rungs: one row of the table has to be one measurement.
        lad, cached = ladder(clip, src, work, ex)
        lines.append(f"  {clip:<20} FIX A: {', '.join(short)} under {MIN_RUNGS} rungs; "
                     f"rungs -> [{', '.join(f'{q:.3f}' for q in lad)}]"
                     f" ({'cached ladder' if cached else 'probed'})")
        # the stored null was fitted on the nominal rungs, so a clip moved to
        # mid-step rungs is referenced to a live control instead
        which = arms + live if AUTO else allarms
        pts = curves(clip, src, lad, work, lines, ex, [("default", "")] + which)
        res, guard = score(clip, pts, lines, "  [A]")
        used = {"mode": "A", "rungs": lad, "guard": guard, "null": None}
        nl = None
    if AUTO and not nl:
        why = ("mid-step rungs" if used["mode"] == "A" else
               "defaults changed since the stored fit"
               if NULL_FIT and clip in NULL_FIT["clips"] else "no stored fit for this clip")
        lines.append(f"  {clip:<20} null: one live control ({why})")
    elif nl:
        lines.append(f"  {clip:<20} null: stored, centre {nl[0]:+.2f}  +/- {nl[1]:.2f}")
    for name, v in res.items():
        why = note_read(name, clip, v)
        if why:
            lines.append(f"  EARLY STOP: arm {name} refused on {why}; its queued cells are skipped")
    c = _hits.get(clip, {"hit": 0, "miss": 0, "probe": 0, "skip": 0})
    lines.append(f"  {clip:<20} cells: {c['miss']} measured, {c['hit']} from cache"
                 + (f", {c['probe']} ladder probes" if c["probe"] else "")
                 + (f", {c['skip']} skipped (early stop)" if c["skip"] else ""))
    scratch.release(work)
    return clip, lines, res, used


# ------------------------------------------------------------- reporting -----
def verdict(name, raw, ctl, spread, what="the controls differ by"):
    """PASS / REFUSED / UNRESOLVED, and the clip it turns on.

    An arm passes only where BOTH readings pass, raw and control-referenced,
    and it is refused where either fails: the rule the preset ladder was read
    with. Where the per-clip bar is BELOW the controls' own spread on a clip,
    the band has not resolved that clip -- two changes that both intend nothing
    differ there by more than the thing being measured -- so the answer is
    UNRESOLVED, and PASS is not available."""
    if not raw:
        return "nothing scored"
    if name in _refused:
        return f"REFUSED by early stop on {_refused[name]} (bar {BAR:+.2f}% per clip)"
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
        return (f"UNRESOLVED: {what} {spread[c]:.2f} on {c}, over "
                f"the {BAR:.2f} bar, so that clip is not read")
    return f"PASS  worst {worst_clip} {raw[worst_clip]:+.2f}%  median {med:+.2f}%"


def main():
    mode = "solve (matched bytes)" if SOLVE else "interp (one ladder, BD over the overlap)"
    print(f"band:  {len(clips)} clips, rungs {RUNGS}, {FRAMES} frames, preset {PRESET}, threads {THREADS}")
    print(f"mode:  {mode}   binary {os.path.basename(BIN)} {LIB_ID}"
          + ("" if SOLVE else f"   guard {MIN_RUNGS}/{len(RUNGS)} rungs, "
                              f"fix A {'on' if FIXA else 'off'}"))
    for name, env in arms:
        print(f"arm {name}: {env}")
    for name, env in controls:
        print(f"control {name}: {env}")
    if AUTO:
        print(f"null:  stored ({NULL_KEY}, fitted {NULL_FIT['date']} on {len(NULL_FIT['libs'])} "
              f"binaries, ref {NULL_FIT['ref_lib']}); live control where needed: "
              f"{live[0][0]}={live[0][1]}" if NULL_FIT and live else
              "null:  none stored; one live control per clip" if live else
              "null:  stored, no live control")
    print(f"vmaf:  models {bd.MODELS}, score cache "
          f"{'on' if bd.SCORE_CACHE else 'off'}; early stop "
          + (f"on (one clip > {EARLY_ONE:+.2f}% or {EARLY_N} > {EARLY_SOME:+.2f}%)"
             if EARLY_STOP else "off"))
    print(flush=True)
    allres = {name: {} for name, _ in allarms + live}
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
    # Under the stored null the same question is asked of the fit: the spread
    # of the difference between two null reads, sqrt(2) x the clip's robust sd,
    # which is the deterministic version of what two live controls sampled once.
    # The centre is the stored one, or the clip's live control read where the
    # defaults changed; the half-width is NULL_SD x the stored sd either way.
    centre, halfw = {}, {}
    if AUTO:
        for c in clips:
            fitc = (NULL_FIT or {}).get("clips", {}).get(c)
            nl = used.get(c, {}).get("null")
            if nl:
                centre[c], halfw[c] = nl[0], nl[1]
            elif live and c in allres[live[0][0]]:
                centre[c] = allres[live[0][0]][c]
                halfw[c] = NULL_SD * fitc["rsd"] if fitc else None
            if fitc:
                spread[c] = math.sqrt(2.0) * fitc["rsd"]

    print(f"\n=== BD-{bd.GATE}" + (" at matched achieved bitrate" if SOLVE
                                 else ", one CRF ladder, BD over the overlap")
          + " (negative = arm better) ===")
    for name, _ in allarms + live:
        r = allres[name]
        kind = "control" if name in ctl_names or name in [n for n, _ in live] else "arm"
        if not r and name in [n for n, _ in live]:
            continue                       # no clip needed the live control
        if not r:
            print(f"\n  {kind} {name}: nothing scored")
            continue
        print(f"\n  {kind} {name}")
        head = f"    {'clip':<20} {'raw':>7}"
        refd = bool(ctl_names) and name not in ctl_names
        nulld = AUTO and kind == "arm"
        if refd:
            head += "".join(f" {'vs ' + n:>8}" for n in ctl_names)
        if nulld:
            head += f" {'vs null':>8} {'+/-':>5}  {'':<12}"
        print(head + f" {'spread':>7}  mode")
        for clip, v in sorted(r.items(), key=lambda kv: kv[1]):
            row = f"    {clip:<20} {v:+7.2f}"
            if refd:
                for n in ctl_names:
                    cv = allres[n].get(clip)
                    row += f" {v - cv:+8.2f}" if cv is not None else f" {'--':>8}"
            if nulld:
                if clip in centre:
                    d, hw = v - centre[clip], halfw.get(clip)
                    tag = ("inside null" if hw is not None and abs(d) <= hw else
                           "outside" if hw is not None else "no fit")
                    src = "" if used.get(clip, {}).get("null") else " [live]"
                    row += (f" {d:+8.2f} {hw:5.2f}" if hw is not None
                            else f" {d:+8.2f} {'--':>5}") + f"  {tag + src:<12}"
                else:
                    row += f" {'--':>8} {'--':>5}  {'':<12}"
            row += f" {spread[clip]:7.2f}" if clip in spread else f" {'--':>7}"
            print(row + f"  {used.get(clip, {}).get('mode', '?')}")
        vals = list(r.values())
        worst = max(r.items(), key=lambda kv: kv[1])
        print(f"    median {statistics.median(vals):+.2f}%   mean {statistics.mean(vals):+.2f}%   "
              f"worst {worst[0]} {worst[1]:+.2f}%   scored {len(vals)}/{len(clips)}")
        if nulld:
            inside = [c for c in r if c in centre and halfw.get(c) is not None
                      and abs(r[c] - centre[c]) <= halfw[c]]
            print(f"    inside null spread on {len(inside)}/{len(r)} clips")
        if name in _refused:
            print(f"    early stop: refused on {_refused[name]}; "
                  f"{len(clips) - len(vals)} clips not read")
        if refd:
            ctl = {c: [allres[n][c] for n in ctl_names if c in allres[n]] for c in r}
            print(f"    verdict: {verdict(name, r, ctl, spread)}")
        elif nulld:
            # the most favourable plausible null read, centre + half-width:
            # the stored analogue of the old rule's "the more favourable of
            # the two live controls", so a referenced refusal still means the
            # arm is over the bar against every null the clip could produce
            ctl = {c: [centre[c] + (halfw.get(c) or 0.0)] for c in r if c in centre}
            print(f"    verdict: {verdict(name, r, ctl, spread, 'the null read-to-read sd is')}")

    if not SOLVE:
        a = sorted(c for c, u in used.items() if u.get("mode") == "A")
        print("\n  read under fix A (mid-step rungs): " + (", ".join(a) if a else "none"))
        tot = {f: sum(c[f] for c in _hits.values()) for f in ("hit", "miss", "probe")}
        n = max(1, len(clips))
        tot["skip"] = sum(c.get("skip", 0) for c in _hits.values())
        print(f"  cells: {tot['miss']} measured, {tot['hit']} from cache, "
              f"{tot['probe']} ladder probes, {tot['skip']} skipped by early stop; "
              f"{(tot['miss'] + tot['hit'] + tot['probe']) / n:.0f} encodes a clip"
              + ("  [cache off]" if NO_CACHE else ""))
        print(f"  vmaf scores: {bd.SCORE_STATS['miss']} computed, "
              f"{bd.SCORE_STATS['hit']} from the score cache (models {bd.MODELS})")


main()
