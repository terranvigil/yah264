#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""cc_split.py -- issue-slot bottleneck split from an Instruments CPU Counters trace.

    xcrun xctrace record --template 'CPU Counters' --attach <pid> \\
        --time-limit 10s --output enc.trace --no-prompt
    python3 scripts/cc_split.py enc.trace <process-name-substring>

Reads the MetricAggregationForProcess table (the template's own useful /
processing / delivery / discarded metrics per 10 ms interval), weights each
interval by its cycles and normalises the four to 100%. "processing" is the
memory and execution stall share, the one that doubles from one thread to
twelve here. Attach rather than --launch: a launched recording hung for
minutes on this box. Threads created after the attach are not counted, so
attach once the encode is in its steady state.
"""
import collections, subprocess, sys, xml.etree.ElementTree as ET

def main():
    tr, want = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else '')
    x = subprocess.run(['xcrun', 'xctrace', 'export', '--input', tr, '--xpath',
                        '/trace-toc/run[@number="1"]/data/table[@schema="MetricAggregationForProcess"]'],
                       capture_output=True, text=True).stdout
    root, ids = ET.fromstring(x), {}
    def val(e):
        if 'ref' in e.attrib:
            return ids[e.attrib['ref']]
        v = (e.text, e.attrib.get('fmt'))
        if 'id' in e.attrib:
            ids[e.attrib['id']] = v
        for c in e:
            val(c)
        return v
    rows, seq, last = collections.defaultdict(dict), 0, None
    for r in root.iter('row'):
        t, d, p, iv, dv, nm, pr = [val(c) for c in list(r)[:7]]
        if pr[0] != '0' or want not in (p[1] or ''):
            continue
        k0 = (t[0], d[0], p[1])
        if k0 != last or nm[0] in rows[(seq,) + k0]:
            seq += 1; last = k0
        rows[(seq,) + k0][nm[0]] = (int(iv[0]), float(dv[0]))
    acc, cyc = collections.Counter(), 0
    for m in rows.values():
        if 'cycle' not in m:
            continue
        c = m['cycle'][0]; cyc += c
        for n in ('useful', 'processing', 'delivery', 'discarded'):
            if n in m:
                acc[n] += c * m[n][1]
    s = sum(acc.values()) or 1
    print(f'{tr}: cycles {cyc / 1e9:.1f}G  ' + '  '.join(
        f'{n} {100 * acc[n] / s:.1f}%' for n in ('useful', 'processing', 'delivery', 'discarded')))

if __name__ == '__main__':
    main()
