#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""How fast a live bitrate step lands: one encode with a --control log of
RATE steps, read back from --frame-stats.

usage: scripts/control_step.py --bin build/cli/y264 --clip CLIP.y4m
           --steps 3000@0,1000@150,3000@300 [--vbv cbr|capped|none]
           [--frames N] [--threads 1] [--keep DIR] [--check SECONDS]

The first step is the open-time rate; each later one becomes a control line
"FRAME rate bitrate=K [maxrate=M bufsize=B]". --vbv cbr sets maxrate =
bufsize = the rate (one second of buffer), capped sets maxrate 1.5x and
bufsize 2x, none leaves the buffer off. For each step it prints the rate of
every half second after it, the settle time (the start of the first one-second
window, counted from the landing frame, whose rate is within 10% of the new
target) and the rate over the whole span from the settle to the next step.
With --check S it exits 1 when any step takes longer than S seconds to
settle. One encode, so it obeys the box's one-encode rule.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile


def y4m_fps(path):
    with open(path, 'rb') as f:
        hdr = f.readline().decode('ascii', 'replace').split()
    for t in hdr:
        if t.startswith('F') and ':' in t:
            n, d = t[1:].split(':')
            return int(n) / int(d)
    return 25.0


def rate_args(kbps, vbv):
    if vbv == 'cbr':
        return kbps, kbps
    if vbv == 'capped':
        return int(kbps * 1.5), kbps * 2
    return 0, 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', required=True)
    ap.add_argument('--clip', required=True)
    ap.add_argument('--steps', required=True)
    ap.add_argument('--vbv', default='cbr', choices=['cbr', 'capped', 'none'])
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--threads', type=int, default=1)
    ap.add_argument('--keep')
    ap.add_argument('--check', type=float, default=0.0)
    ap.add_argument('--extra', default='')
    a = ap.parse_args()

    steps = []
    for s in a.steps.split(','):
        k, f = s.split('@')
        steps.append((int(f), int(k)))
    steps.sort()
    if steps[0][0] != 0:
        sys.exit('the first step is the open-time rate and must be @0')
    fps = y4m_fps(a.clip)
    work = a.keep or tempfile.mkdtemp(prefix='ctlstep')
    os.makedirs(work, exist_ok=True)
    log = os.path.join(work, 'steps.ctl')
    with open(log, 'w') as f:
        for fr, k in steps[1:]:
            m, b = rate_args(k, a.vbv)
            f.write(f'{fr} rate bitrate={k}' + (f' maxrate={m} bufsize={b}' if m else '') + '\n')
    fst = os.path.join(work, 'fs.jsonl')
    out = os.path.join(work, 'out.264')
    k0 = steps[0][1]
    m0, b0 = rate_args(k0, a.vbv)
    cmd = [a.bin, '--input-y4m', a.clip, '--bitrate', str(k0), '--threads', str(a.threads),
           '--control', log, '--frame-stats', fst, '-o', out, '--no-progress']
    if m0:
        cmd += ['--vbv-maxrate', str(m0), '--vbv-bufsize', str(b0)]
    if a.frames:
        cmd += ['--frames', str(a.frames)]
    cmd += a.extra.split()
    r = subprocess.run(cmd, stderr=subprocess.PIPE, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr)
        sys.exit(f'encode failed: {r.returncode}')
    rows = [json.loads(l) for l in open(fst)]
    by = {x['frame']: x for x in rows}
    n = max(by) + 1
    bits = [by[i]['bytes'] * 8 if i in by else 0 for i in range(n)]
    landed = {}
    for x in rows:
        for c in x.get('control', []):
            landed[int(c.split()[0])] = x['frame']
    win = max(1, int(round(fps)))
    half = max(1, int(round(fps / 2)))
    worst = 0.0
    print(f'clip {os.path.basename(a.clip)}  {fps:.3f} fps  vbv {a.vbv}  threads {a.threads}  {n} frames')
    for i, (fr, k) in enumerate(steps):
        end = steps[i + 1][0] if i + 1 < len(steps) else n
        at = landed.get(fr, fr) if i else 0
        halves = []
        for s in range(at, end, half):
            e = min(end, s + half)
            halves.append(sum(bits[s:e]) * fps / (e - s) / 1000.0)
        settle = None
        for s in range(at, end - win + 1):
            got = sum(bits[s:s + win]) * fps / win / 1000.0
            if abs(got - k) <= 0.1 * k:
                settle = (s - at) / fps
                break
        rest = None
        if settle is not None:
            s0 = at + int(settle * fps)
            rest = sum(bits[s0:end]) * fps / max(1, end - s0) / 1000.0
        tag = 'open' if i == 0 else f'step at {fr} (landed {at})'
        st = f'{settle:.2f} s' if settle is not None else 'never'
        rs = f'{rest:.0f} kbps ({(rest / k - 1) * 100:+.1f}%)' if rest is not None else '-'
        print(f'{tag}: target {k} kbps, settle {st}, after settle {rs}')
        print('  half-seconds: ' + ' '.join(f'{h:.0f}' for h in halves))
        if i:
            worst = max(worst, settle if settle is not None else float('inf'))
    if not a.keep:
        for p in (log, fst, out):
            os.remove(p)
        os.rmdir(work)
    if a.check and worst > a.check:
        print(f'FAIL: a step took {worst:.2f} s to settle (limit {a.check} s)')
        sys.exit(1)


if __name__ == '__main__':
    main()
