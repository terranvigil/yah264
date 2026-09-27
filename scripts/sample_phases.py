#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""sample_phases.py -- CPU samples by encoder stage from a macOS `sample` report.

    sample <pid> 15 1 -mayDie -file enc.sample     # while a threaded encode runs
    python3 scripts/sample_phases.py enc.sample [--top N] [--rules FILE]

`sample` records every thread every millisecond, blocked or not. This drops
the samples whose leaf is a blocking wait (condition, mutex, ulock, workq,
read of an empty pipe is kept), keeps a yield spin as CPU, rebuilds each
sample's stack from the call-graph indentation, and assigns it to the first
stage rule that matches walking from the leaf toward the thread root. The
result is a share of RUNNING samples per stage; multiply by the process's
CPU-s from `/usr/bin/time -l` for seconds. Kernel time lands on the user
frame that trapped (a page fault on the function that touched the page),
which is what made the memory-pressure cost visible by stage.

--rules FILE takes a JSON list of [stage, regex] pairs, tried in order, plus
an optional "leaf" list applied to the leaf symbol alone when no stack frame
matched -- for a binary whose stacks break at frame-pointer-less leaves. The
built-in rules are this encoder's; rules for another encoder stay in local
notes, not in the tree. Unsymbolised frames are resolved against the
binary's own symbol table with nm, nearest preceding symbol; `sample` masks
the user's home in the report's paths, so name the image with --bin NAME=PATH.
"""
import argparse, bisect, collections, json, os, re, subprocess, sys

WAIT = {'__psynch_cvwait', '__psynch_mutexwait', '__ulock_wait', '__ulock_wait2',
        '__semwait_signal', '__workq_kernreturn', '__sigsuspend', 'mach_msg2_trap',
        'kevent', 'kevent_id', '__select', '__wait4', '__psynch_rw_rdlock'}
OURS = [
    ('spin/yield', r'^(swtch_pri|cthread_yield|sched_yield)$'),
    ('input read+copy', r'^(y4m_reader|read_plane|__read_nocancel)$'),
    ('lookahead (lowres, lowres ME)', r'(lowres|^lr_|^blk8_|build_lr_subpel|^la_|lookahead|frame_complexity|^lr)'),
    ('mb-tree', r'(mbtree|^mbt_)'),
    ('deblock', r'deblock'),
    ('entropy emit (pass 2)', r'^(emit_|y264_frame_emit|y264_cabac_encode|cabac_write|write_|y264_cavlc)'),
    ('hpel + ref store + borders', r'(hpel|dpb_store|extend|pad_input|copy_planes|border)'),
    ('motion search', r'^(y264_me_|probe_|me_search|subpel|qpel|eval_inter_part)'),
    ('macroblock analysis (RD, transform)', r'^(analyze_|frame_analyze|y264_frame_analyze|mb_|encode_|rdoq|trellis|est_)'),
    ('pool/staircase plumbing', r'^(ntp_|stair_|run_row|wf_|pool_)'),
    ('macroblock analysis (RD, transform)', r'(_wf_cell|^rd_|dist_mb|ssd_mb|inter_rd|build_direct|_mode$)'),
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('report'); ap.add_argument('--top', type=int, default=0)
    ap.add_argument('--rules')
    ap.add_argument('--bin', action='append', default=[],
                    help='NAME=PATH: where image NAME lives, for nm (the report masks the user path)')
    a = ap.parse_args()
    bins = dict(b.split('=', 1) for b in a.bin)
    rules, leaf_rules = OURS, []
    if a.rules:
        j = json.load(open(a.rules))
        rules = j['stack'] if isinstance(j, dict) else j
        leaf_rules = j.get('leaf', []) if isinstance(j, dict) else []
    rules = [(n, re.compile(r)) for n, r in rules]
    leaf_rules = [(n, re.compile(r)) for n, r in leaf_rules]
    txt = open(a.report).read()
    m = re.search(r'^Path:\s+(\S+)', txt, re.M)
    nmcache = {}
    def symfor(binname, off):
        if binname not in nmcache:
            cands = [p for p in (bins.get(binname, ''), binname, m.group(1) if m else '')
                     if p and os.path.exists(p)]
            tab = []
            if cands:
                for l in subprocess.run(['nm', '-n', cands[0]], capture_output=True, text=True).stdout.splitlines():
                    f = l.split()
                    if len(f) == 3 and f[1] in 'tT':
                        tab.append((int(f[0], 16) - 0x100000000, f[2].lstrip('_')))
            nmcache[binname] = tab
        t = nmcache[binname]
        i = bisect.bisect_right([x for x, _ in t], off) - 1
        return t[i][1] if i >= 0 else '???'
    nodes, stack = [], []
    for ln in txt.split('Sort by top of stack')[0].splitlines():
        mm = re.match(r'^([\s+!:|]*)(\d+)\s+(\S+)', ln)
        if not mm:
            continue
        fn = mm.group(3)
        if fn == '???':
            q = re.search(r'\(in ([^)]+)\)\s+load address 0x[0-9a-f]+ \+ (0x[0-9a-f]+)', ln)
            if q:
                fn = symfor(q.group(1), int(q.group(2), 16))
        ind, n = len(mm.group(1)), int(mm.group(2))
        while stack and stack[-1][0] >= ind:
            stack.pop()
        node = {'fn': fn, 'n': n, 'kids': 0, 'path': [s[1]['fn'] for s in stack] + [fn]}
        if stack:
            stack[-1][1]['kids'] += n
        stack.append((ind, node)); nodes.append(node)
    ph, leaf = collections.Counter(), collections.Counter()
    for nd in nodes:
        ex = nd['n'] - nd['kids']
        if ex <= 0 or nd['fn'].startswith('Thread_') or nd['fn'] in WAIT:
            continue
        got = 'other'
        for fn in reversed(nd['path']):
            hit = next((name for name, rx in rules if rx.search(fn)), None)
            if hit:
                got = hit; break
        if got == 'other':
            got = next((name for name, rx in leaf_rules if rx.search(nd['fn'])), 'other')
        ph[got] += ex; leaf[(got, nd['fn'])] += ex
    tot = sum(ph.values()) or 1
    print(f'running samples {tot}')
    for k, v in ph.most_common():
        print(f'{100 * v / tot:6.1f}%  {k}')
    for (g, f), v in leaf.most_common(a.top):
        print(f'   {100 * v / tot:5.2f}% {g[:24]:24s} {f}')

if __name__ == '__main__':
    main()
