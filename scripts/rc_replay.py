#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""rc_replay.py - record a one-thread encode's rate-control inputs, replay the
rate controller alone, and check the replay against the recording.

    python3 scripts/rc_replay.py                      # self-test matrix, foreman_cif
    CLIP=tests/corpus/pedestrian_1080p.y4m FRAMES=60 python3 scripts/rc_replay.py
    ONLY='abr|vbv' python3 scripts/rc_replay.py
    # a controller change, replayed against the default's recording:
    PLAY_BIN=../other/build-rel/cli/yah264 python3 scripts/rc_replay.py
    PLAY_ENV='Y264_ABR_VBVOV=0.5' ONLY=abr_vbv python3 scripts/rc_replay.py
    KEEP=dir python3 scripts/rc_replay.py             # keep the recordings in dir

How it works (src/encoder/rcreplay.h): Y264_RC_REC=<file> makes a --threads 1
encode write every pixel-derived value its rate controller reads, in the
order it reads them, with each slice's QP and coded size. Y264_RC_PLAY=<file>
runs the same command with the pixel work switched off and those values fed
back, so only the frame flow and the controller run.

The first test is equivalence: replaying with the binary that recorded must
reproduce every slice's QP ("EXACT"). That is the default run here, and any
DIFFERS fails it (exit 1). With PLAY_BIN or PLAY_ENV the replay runs another
controller over the same inputs: a slice whose QP moved gets its size scaled
from the recorded one by the 1/qscale law (Y264_RC_PLAY_K scales the
exponent), so its bits are an estimate, and the row says how many slices were
modelled. The VBV columns are a leaky-bucket estimate over the slice sizes,
not the Annex C gate; run scripts/hrd_check.py on a real encode for that.

Environment: BIN (default build-rel/cli/yah264), CLIP, FRAMES (default 120),
ONLY (regex on config names), PLAY_BIN, PLAY_ENV (NAME=VALUE words),
KEEP (directory to keep recordings in).
"""
import os, re, shlex, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("BIN", os.path.join(ROOT, "build-rel/cli/yah264"))
PLAY_BIN = os.environ.get("PLAY_BIN", BIN)
PLAY_ENV = os.environ.get("PLAY_ENV", "")
CLIP = os.environ.get("CLIP", os.path.join(ROOT, "tests/corpus/foreman_cif.y4m"))
FRAMES = int(os.environ.get("FRAMES", "120"))
ONLY = os.environ.get("ONLY", "")
KEEP = os.environ.get("KEEP", "")

# name, args. PASS2 runs a real pass 1 first, then records and replays pass 2.
CONFIGS = [
    ("cqp",        "--qp 28"),
    ("crf",        "--crf 23"),
    ("crf_hi",     "--crf 38"),
    ("abr",        "--bitrate 500"),
    ("abr_vbv",    "--bitrate 500 --vbv-maxrate 750 --vbv-bufsize 750"),
    ("cbr",        "--bitrate 500 --vbv-maxrate 500 --vbv-bufsize 500 --nal-hrd cbr"),
    ("crf_vbv",    "--crf 21 --vbv-maxrate 600 --vbv-bufsize 600"),
    ("vbr_hrd",    "--bitrate 500 --vbv-maxrate 750 --vbv-bufsize 1500 --nal-hrd vbr --vbv-init 0.5"),
    ("twopass",    "--bitrate 500 PASS2"),
    ("abr_qpstep", "--bitrate 500 --qpstep 2 --qpmin 18 --qpmax 42"),
    ("no_bframes", "--crf 23 --bframes 0"),
    ("no_pyramid", "--bitrate 500 --b-pyramid none"),
    ("no_la",      "--bitrate 500 --rc-lookahead 0"),
    ("keyint",     "--bitrate 500 --keyint 24 --open-gop"),
    ("slices",     "--crf 23 --slices 3"),
    ("paff",       "--crf 23 --tff"),
    ("cavlc",      "--bitrate 500 --cavlc"),
    ("fast",       "--preset fast --bitrate 500 --vbv-maxrate 750 --vbv-bufsize 750"),
]


def run(cmd, env_extra):
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("Y264_RC_"):
            del env[k]
    env.update(env_extra)
    t = time.perf_counter()
    p = subprocess.run(cmd, shell=True, env=env, capture_output=True, text=True)
    return time.perf_counter() - t, p


def env_words(s):
    out = {}
    for w in s.split():
        if "=" not in w or w.startswith("-"):
            sys.exit(f"PLAY_ENV word {w!r} is not NAME=VALUE")
        k, v = w.split("=", 1)
        out[k] = v
    return out


def main():
    for b in (BIN, PLAY_BIN):
        if not os.access(b, os.X_OK):
            sys.exit(f"rc_replay: no binary {b}")
    if not os.path.exists(CLIP):
        sys.exit(f"rc_replay: no clip {CLIP}")
    selftest = PLAY_BIN == BIN and not PLAY_ENV
    work = KEEP or tempfile.mkdtemp(prefix="yah264-rcreplay.")
    os.makedirs(work, exist_ok=True)
    penv = env_words(PLAY_ENV)
    print(f"rc_replay: {os.path.basename(CLIP)} {FRAMES} frames, "
          f"{'self-test' if selftest else 'replay with ' + (PLAY_BIN if PLAY_BIN != BIN else '') + ' ' + PLAY_ENV}")
    print(f"{'config':12} {'rec s':>7} {'play s':>7} {'x':>6}  {'result':8} {'slices':>6} "
          f"{'modelled':>8} {'bits play/rec':>13} {'vbv under rec/play':>18}")
    bad = failed = 0
    for name, args in CONFIGS:
        if ONLY and not re.search(ONLY, name):
            continue
        log = os.path.join(work, f"{name}.rcr")
        stats = os.path.join(work, f"{name}.stats")
        base = f"{{bin}} --input-y4m {shlex.quote(CLIP)} --frames {FRAMES} --threads 1 --no-progress"
        a = args.replace("PASS2", "").strip()
        if "PASS2" in args:
            _, p = run(base.format(bin=shlex.quote(BIN)) + f" {a} --pass 1 --stats {stats} -o /dev/null", {})
            if p.returncode:
                print(f"{name:12} pass 1 failed: {p.stderr.strip()[-120:]}"); failed += 1; continue
            a += f" --pass 2 --stats {stats}"
        trec, p = run(base.format(bin=shlex.quote(BIN)) + f" {a} -o /dev/null", {"Y264_RC_REC": log})
        if p.returncode:
            print(f"{name:12} record failed: {p.stderr.strip()[-160:]}"); failed += 1; continue
        rec = p.stderr
        tplay, q = run(base.format(bin=shlex.quote(PLAY_BIN)) + f" {a} -o /dev/null",
                       dict(penv, Y264_RC_PLAY=log))
        out = q.stderr
        m = re.search(r"RCREPLAY slices=(\d+) qp_match=(\d+) qp_diff=(\d+) modelled=(\d+) "
                      r"order_errors=(\d+) unread=(\d+) bits_rec=(\d+) bits_play=(\d+) (\w+)", out)
        if q.returncode or not m:
            print(f"{name:12} replay failed: {(out or '').strip()[-160:]}"); failed += 1; continue
        sl, qm, qd, mo, oe, un, br, bp, verdict = m.groups()
        ur = re.search(r"RCREPLAY vbv: underflows=(\d+)", rec)
        up = re.search(r"RCREPLAY vbv: underflows=(\d+)", out)
        vbv = f"{ur.group(1)}/{up.group(1)}" if ur and up else "-"
        ratio = int(bp) / int(br) if int(br) else 0.0
        print(f"{name:12} {trec:7.2f} {tplay:7.2f} {trec / max(tplay, 1e-3):6.1f}  {verdict:8} "
              f"{sl:>6} {mo:>8} {ratio:13.4f} {vbv:>18}")
        if verdict != "EXACT":
            fd = re.search(r"RCREPLAY first difference: (.*)", out)
            if fd:
                print(f"{'':12} first difference: {fd.group(1)}")
            if selftest:
                bad += 1
    if not KEEP:
        for f in os.listdir(work):
            os.remove(os.path.join(work, f))
        os.rmdir(work)
    if failed:
        print(f"rc_replay: FAIL ({failed} config(s) failed to run)")
    elif selftest:
        print("rc_replay: " + ("PASS (every replay reproduced its recording)" if not bad
                                else f"FAIL ({bad} config(s) did not reproduce)"))
    sys.exit(1 if failed or (bad and selftest) else 0)


main()
