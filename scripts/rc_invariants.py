#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""rc_invariants.py - the rate controller's invariants, checked in seconds
through the record/replay harness (src/encoder/rcreplay.h).

    python3 scripts/rc_invariants.py                       # REF = CAND = build-rel
    REF=/old/build-rel/cli/yah264 python3 scripts/rc_invariants.py
    ONLY='a6|stefan' python3 scripts/rc_invariants.py
    CAND_ENV='Y264_VBV_PLAN=0' python3 scripts/rc_invariants.py

Each case is recorded ONCE with the reference binary (REF), --threads 1, and
cached under $Y264_CACHE/rcreplay keyed on the binary's md5 and the command.
Then the candidate (CAND, default REF, with CAND_ENV knobs) replays it. Every
replay of a restructuring step must read EXACT: same QP on every slice. That
is the identity column, and it is what gates the byte-identical steps of the
rate-control rebuild. The invariant columns are computed on the replayed
sizes, so when the candidate decides differently they are estimates (the
"modelled" count says how many slices).

The cases are the two compliance sets the tree already uses, at one thread
over 6-second windows:
  a6    --bitrate T with --vbv-maxrate = --vbv-bufsize = 1.0x (CBR, nal-hrd cbr)
        and 1.5x T, on the A6 clips and targets
        (local/records/a6-cbr-2026-09-03.md). Invariants: 0 VBV underflows,
        rate error inside [-5%, +3.4%] at CBR (A6's +3.4% bar; the -5% floor
        is the undershoot side the 10-04 regression broke) and inside
        [-6%, +6%] at the 1.5x cap (owner decision 2026-10-05).
  cvbr  --crf N with --vbv-maxrate = --vbv-bufsize = 0.8/0.6/0.4 x T,
        scripts/cvbr_compliance.sh's clips and CRF points. Invariant: 0 VBV
        underflows.
The VBV column is the harness's leaky bucket over the slice RBSP sizes, final
attempts only. It is a fast proxy; the stream-level gates stay
scripts/vbv_check.py and scripts/hrd_check.py on real encodes.

Exit status: 1 when a case fails to run, when a replay is not EXACT and
EXACT is required (CAND == REF with no CAND_ENV, or EXPECT_EXACT=1 -- set it
when gating a restructuring step against its parent), or when an invariant
fails; 0 otherwise.
INFORM=1 reports invariant failures without failing (for a candidate whose
point is to change behaviour).
"""
import hashlib, os, re, shlex, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.environ.get("REF", os.path.join(ROOT, "build-rel/cli/yah264"))
CAND = os.environ.get("CAND", REF)
CAND_ENV = os.environ.get("CAND_ENV", "")
CORPUS = os.environ.get("CORPUS", os.path.join(ROOT, "tests/corpus"))
ONLY = os.environ.get("ONLY", "")
SECONDS = float(os.environ.get("SECONDS_PER", "6"))
INFORM = os.environ.get("INFORM", "0") == "1"
EXPECT_EXACT = os.environ.get("EXPECT_EXACT", "0") == "1"
CACHE = os.path.join(os.environ.get("Y264_CACHE", os.path.expanduser("~/.cache/yah264")),
                     "rcreplay")

A6 = [("foreman_cif", 400), ("bus_cif", 400), ("stefan_cif", 400),
      ("samsung_720p", 1200), ("park_joy_720p", 12000), ("ducks_720p", 25000)]
CVBR = [("foreman_cif", 400, 21.4), ("bus_cif", 400, 28.6), ("stefan_cif", 400, 26.2),
        ("samsung_720p", 1200, 21.4), ("park_joy_720p", 12000, 26.2),
        ("ducks_720p", 25000, 22.6)]


def fps_of(path):
    with open(path, "rb") as f:
        hdr = f.readline().decode("ascii", "replace")
    m = re.search(r" F(\d+):(\d+)", hdr)
    return int(m.group(1)) / int(m.group(2)) if m else 25.0


def cases():
    out = []
    for clip, t in A6:
        for cap, extra in ((1.0, " --nal-hrd cbr"), (1.5, "")):
            mr = int(round(t * cap))
            out.append((f"a6_{clip}_{cap}", clip, t,
                        f"--bitrate {t} --vbv-maxrate {mr} --vbv-bufsize {mr}{extra}"))
    for clip, t, crf in CVBR:
        for frac in (0.8, 0.6, 0.4):
            cap = int(t * frac)
            out.append((f"cvbr_{clip}_{frac}", clip, None,
                        f"--crf {crf} --vbv-maxrate {cap} --vbv-bufsize {cap}"))
    return out


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run(cmd, extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("Y264_RC_")}
    env.update(extra)
    return subprocess.run(cmd, shell=True, env=env, capture_output=True, text=True)


def main():
    for b in (REF, CAND):
        if not os.access(b, os.X_OK):
            sys.exit(f"rc_invariants: no binary {b}")
    cenv = {}
    for w in CAND_ENV.split():
        k, _, v = w.partition("=")
        if not v and "=" not in w:
            sys.exit(f"CAND_ENV word {w!r} is not NAME=VALUE")
        cenv[k] = v
    selftest = CAND == REF and not cenv
    want_exact = selftest or EXPECT_EXACT
    ndiff = 0
    rid = md5(REF)[:12]
    cdir = os.path.join(CACHE, rid)
    os.makedirs(cdir, exist_ok=True)
    print(f"rc_invariants: REF {REF} ({rid}), CAND {'= REF' if CAND == REF else CAND} {CAND_ENV}")
    print(f"{'case':28} {'identity':9} {'modelled':>8} {'kbps':>9} {'err%':>7} {'under':>5} "
          f"{'minfill%':>8}  verdict")
    nbad = nfail = 0
    t0 = time.perf_counter()
    for name, clip, target, args in cases():
        if ONLY and not re.search(ONLY, name):
            continue
        src = os.path.join(CORPUS, clip + ".y4m")
        if not os.path.exists(src):
            print(f"{name:28} (no clip)"); continue
        fps = fps_of(src)
        nf = int(round(fps * SECONDS))
        base = (f"{{b}} --input-y4m {shlex.quote(src)} --frames {nf} --threads 1 "
                f"--no-progress {args} -o /dev/null")
        key = hashlib.md5((rid + base).encode()).hexdigest()[:16]
        log = os.path.join(cdir, f"{name}.{key}.rcr")
        if not os.path.exists(log):
            p = run(base.format(b=shlex.quote(REF)), {"Y264_RC_REC": log + ".tmp"})
            if p.returncode or not os.path.exists(log + ".tmp"):
                print(f"{name:28} record failed: {p.stderr.strip()[-120:]}"); nbad += 1; continue
            os.replace(log + ".tmp", log)
        q = run(base.format(b=shlex.quote(CAND)), dict(cenv, Y264_RC_PLAY=log))
        m = re.search(r"RCREPLAY slices=(\d+) qp_match=(\d+) qp_diff=(\d+) modelled=(\d+) "
                      r"order_errors=(\d+) unread=(\d+) bits_rec=(\d+) bits_play=(\d+) (\w+)",
                      q.stderr)
        v = re.search(r"RCREPLAY vbv: underflows=(\d+) min_fill=([-\d.]+) of ([\d.]+) bits "
                      r"over (\d+) pictures", q.stderr)
        if q.returncode or not m or not v:
            print(f"{name:28} replay failed: {q.stderr.strip()[-120:]}"); nbad += 1; continue
        ident, mod, bits = m.group(9), int(m.group(4)), int(m.group(8))
        under, minfill, buf, pics = int(v.group(1)), float(v.group(2)), float(v.group(3)), int(v.group(4))
        kbps = bits / (pics / fps) / 1000.0 if pics else 0.0
        err = 100.0 * (kbps / target - 1.0) if target else None
        fails = []
        if under:
            fails.append("underflow")
        # Capped VBR (a6 cases at 1.5x) tracks to about +-6% over the 6 s
        # window: owner decision 2026-10-05, still inside the reference
        # encoder's own spread there. CBR (1.0x) keeps [-5%, +3.4%].
        lo, hi = (-6.0, 6.0) if name.startswith("a6_") and name.endswith("_1.5") else (-5.0, 3.4)
        if err is not None and not (lo <= err <= hi):
            fails.append("rate")
        if ident != "EXACT":
            ndiff += 1
        if fails:
            nfail += 1
        print(f"{name:28} {ident:9} {mod:8d} {kbps:9.1f} "
              f"{(f'{err:+7.2f}' if err is not None else '      -')} {under:5d} "
              f"{100.0 * minfill / buf:8.1f}  {'FAIL ' + '+'.join(fails) if fails else 'ok'}")
    print(f"rc_invariants: {time.perf_counter() - t0:.1f} s; "
          f"identity: {'EXACT everywhere' if not ndiff else str(ndiff) + ' case(s) not EXACT'}"
          f"{' (required)' if want_exact else ''}; {nbad} case(s) failed to run; "
          f"{nfail} case(s) outside an invariant")
    sys.exit(1 if nbad or (ndiff and want_exact) or (nfail and not INFORM) else 0)


main()
