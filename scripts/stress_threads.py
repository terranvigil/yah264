#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""
stress_threads.py -- regression stress for the --threads N wavefront/threadpool.

Launches many CONCURRENT `--threads 8` encodes (oversubscribing the cores) and
watches for the 0%-CPU hang signature. This reproduces the rare wavefront lost-wakeup
deadlock (fixed 2026-07-19, threadpool.c run_row): pre-fix it hung ~1 in 50-96
concurrent encodes; post-fix it must be 0. Run it after any change to the threadpool
or the wavefront analysis path.

Usage: scripts/stress_threads.py [--rounds N] [--per-round-timeout S]
Exit 0 = no hang; exit 1 = a hang was caught (a /tmp/stress_hang.sample is written).
"""
import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("YAH264", os.path.join(ROOT, "build", "cli", "yah264"))
CORPUS = os.path.join(ROOT, "tests", "corpus")
CLIPS = ["stefan_cif", "bus_cif", "mobile_cif", "coastguard_cif"]
CRFS = [32, 38, 44]


def cpu(pid):
    try:
        o = subprocess.run(["ps", "-o", "%cpu=", "-p", str(pid)],
                           capture_output=True, text=True).stdout.strip()
        return float(o) if o else -1.0
    except Exception:
        return -1.0


# Y264_STRESS_ABR=1 swaps --crf for --bitrate so the run engages RC_PIPE. The
# CRF configs above cannot: rcp_on is false for them, so a stress that only ever
# ran CRF says nothing about the rate-control pipeline or anything gated behind
# it. Default stays CRF, which is what every earlier round's number means.
ABR = os.environ.get("Y264_STRESS_ABR") == "1"
BITRATES = [1500, 2500, 4000]


def enc(clip, q):
    src = os.path.join(CORPUS, clip + ".y4m")
    rate = ["--bitrate", str(q)] if ABR else ["--crf", str(q)]
    return subprocess.Popen(
        [BIN, "--input-y4m", src, "-o", os.devnull, *rate,
         "--cabac", "--transform-8x8", "--bframes", "2", "--threads", "8"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=30)
    ap.add_argument("--per-round-timeout", type=float, default=90.0)
    args = ap.parse_args()

    clips = [c for c in CLIPS if os.path.exists(os.path.join(CORPUS, c + ".y4m"))]
    if not clips:
        sys.exit("stress_threads: no corpus clips (run scripts/fetch_corpus.sh)")

    hangs = 0
    for r in range(args.rounds):
        procs = [enc(c, q) for c in clips for q in (BITRATES if ABR else CRFS)]
        zero, t0 = {}, time.time()
        while time.time() - t0 < args.per_round_timeout:
            alive = [p for p in procs if p.poll() is None]
            if not alive:
                break
            time.sleep(2)
            hit = None
            for p in alive:
                v = cpu(p.pid)
                zero[p.pid] = zero.get(p.pid, 0) + 1 if 0 <= v < 1.0 else 0
                if zero[p.pid] >= 6:            # ~12s at 0% CPU = hung, not slow
                    hit = p
                    break
            if hit is not None:
                hangs += 1
                print(f"HANG round {r}: pid {hit.pid} (0% CPU, wedged)")
                subprocess.run(["sample", str(hit.pid), "2", "-f", "/tmp/stress_hang.sample"])
                print("  sampled -> /tmp/stress_hang.sample")
                break
        for p in procs:
            if p.poll() is None:
                p.kill(); p.wait()
        if hangs:
            break
        if (r + 1) % 10 == 0:
            print(f"  {r + 1}/{args.rounds} rounds clean")

    n = args.rounds * len(clips) * len(BITRATES if ABR else CRFS)
    print(f"RESULT: {hangs} hang(s) in up to {n} concurrent --threads-8 encodes")
    sys.exit(1 if hangs else 0)


if __name__ == "__main__":
    main()
