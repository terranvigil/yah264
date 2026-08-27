#!/usr/bin/env python3
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""M6 offline kill-test: can pre-ME evidence predict the B tournament's skip
verdict on the early-probe ESCAPEE population?

RESULT 2026-08-27: THE ITEM IS DEAD, and this script is the record. Kill line,
stated before fitting: <95% precision at <30% coverage held-out kills it; the
target that funds an encoder round was >=97% @ >=40%.

  - Leave-one-clip-out (8 clips, 2.3M rows): coverage at >=95% precision is
    ~0% on every held-out clip, and the depth-5 CART transfers WORSE than a
    bdist-only threshold.
  - Within-clip temporal split (the most favourable bound possible): 97% is
    unreachable at any coverage; 95% tops out at ducks 35.5%, bbb 8.9%,
    everything else <=16%.

So the verdict is NO-SIGNAL, not no-transfer: the pre-ME features (skip-recon
distortion, direct SATD, lookahead pair costs, lowres-vs-direct MV
disagreement, mb-tree offset, qp, isref, left/top verdicts) do not separate
the coded minority at commit-grade precision, because the deciding information
is the SEARCHED MV's quality versus direct -- which does not exist before the
search runs. This is also why three generations of hand gates failed: the
thresholds were never the problem.

A revival must add a feature class not in this dump -- the colocated MB's
verdict in the reference frame is the one obvious candidate -- and must beat
the same bar on the same harness. Data recipe: N264_BLATE_STAT=<f> per clip
(t1 only), 120 frames, CRF 25 (+34 for qp spread); rows land one per B MB.
satd16min is post-ME and stays EXCLUDED."""
import numpy as np, glob, os, sys

D = os.path.dirname(os.path.abspath(__file__)) + "/m6"
COLS = "poc mbx mby mode path isref bdist dsatd satd16min c0 c1 ci da0 da1 mbtoff qp".split()

def load(path):
    a = np.loadtxt(path, dtype=np.int64)
    return a

def features(a, wmb):
    """Pre-ME feature matrix + label, escapee population only."""
    poc, mbx, mby = a[:,0], a[:,1], a[:,2]
    mode, path = a[:,3], a[:,4]
    # derived: left/top neighbour final verdict (causal at gate time in raster order)
    skipmap = {}
    left = np.zeros(len(a), dtype=np.int8); top = np.zeros(len(a), dtype=np.int8)
    for i in range(len(a)):
        k = (poc[i], mbx[i]-1, mby[i]);  left[i] = skipmap.get(k, -1)
        k = (poc[i], mbx[i], mby[i]-1);  top[i]  = skipmap.get(k, -1)
        skipmap[(poc[i], mbx[i], mby[i])] = 1 if mode[i] == 0 else 0
    esc = path != 1                      # escaped the early probe
    X = np.column_stack([a[:,6], a[:,7], a[:,9], a[:,10], a[:,11],
                         a[:,12], a[:,13], a[:,14], a[:,15], a[:,5],
                         left, top]).astype(np.float64)
    y = (mode == 0).astype(np.int8)
    return X[esc], y[esc]

FEATS = "bdist dsatd c0 c1 ci da0 da1 mbtoff qp isref leftskip topskip".split()

class Tree:
    def __init__(s, depth=5, minleaf=500): s.d, s.m, s.nodes = depth, minleaf, {}
    def fit(s, X, y, node=0, depth=0):
        n = len(y); p = y.mean() if n else 0.0
        if depth >= s.d or n < 2*s.m or p in (0.0, 1.0):
            s.nodes[node] = ('leaf', p, n); return
        best = None
        for f in range(X.shape[1]):
            v = X[:,f]
            qs = np.unique(np.quantile(v, np.linspace(0.05,0.95,19)))
            for t in qs:
                L = v <= t; nl = L.sum()
                if nl < s.m or n-nl < s.m: continue
                pl, pr = y[L].mean(), y[~L].mean()
                g = nl*pl*(1-pl) + (n-nl)*pr*(1-pr)   # weighted gini
                if best is None or g < best[0]: best = (g, f, t, L)
        if best is None: s.nodes[node] = ('leaf', p, n); return
        _, f, t, L = best
        s.nodes[node] = ('split', f, t)
        s.fit(X[L], y[L], 2*node+1, depth+1)
        s.fit(X[~L], y[~L], 2*node+2, depth+1)
    def prob(s, X):
        out = np.empty(len(X))
        for i in range(len(X)):
            node = 0
            while True:
                nd = s.nodes[node]
                if nd[0] == 'leaf': out[i] = nd[1]; break
                node = 2*node+1 if X[i, nd[1]] <= nd[2] else 2*node+2
        return out

def curve(score, y, label, points=(0.999,0.99,0.98,0.97,0.95,0.90,0.80)):
    order = np.argsort(-score)
    ys = y[order]
    cum = np.cumsum(ys); k = np.arange(1, len(ys)+1)
    prec = cum / k
    out = []
    for target in points:
        ok = np.where(prec >= target)[0]
        cov = 0.0; got = 0.0
        if len(ok):
            i = ok[-1]
            cov = (i+1)/len(ys)*100; got = prec[i]*100
        out.append((target*100, cov))
    return out

clips = sorted(set(os.path.basename(f).split('.')[0] for f in glob.glob(D+"/*.rows")))
data = {}
for c in clips:
    Xs, ys = [], []
    for f in sorted(glob.glob(f"{D}/{c}.*.rows")):
        a = load(f); X, y = features(a, 0); Xs.append(X); ys.append(y)
    data[c] = (np.vstack(Xs), np.concatenate(ys))
    print(f"  {c:<16} escapees={len(data[c][1]):7d}  skip-rate={data[c][1].mean()*100:5.1f}%")

print("\nLeave-one-clip-out: coverage% at each precision target (model | bdist-only)")
hdr = "  ".join(f"@{p}%" for p in (99.9,99,98,97,95,90,80))
print(f"  {'held-out clip':<16} {hdr}")
for held in clips:
    Xtr = np.vstack([data[c][0] for c in clips if c != held])
    ytr = np.concatenate([data[c][1] for c in clips if c != held])
    # subsample train for speed, stratified is unnecessary at this size
    if len(ytr) > 400000:
        idx = np.random.RandomState(7).choice(len(ytr), 400000, replace=False)
        Xtr, ytr = Xtr[idx], ytr[idx]
    T = Tree(depth=5, minleaf=500); T.fit(Xtr, ytr)
    Xte, yte = data[held]
    p = T.prob(Xte)
    mc = curve(p, yte, held)
    bc = curve(-Xte[:,0].astype(float), yte, held)   # bdist-only: lower = more skip-like
    ms = "  ".join(f"{c:5.1f}" for _, c in mc)
    bs = "  ".join(f"{c:5.1f}" for _, c in bc)
    print(f"  {held:<16} {ms}")
    print(f"  {'':<16} {bs}   <- bdist-only baseline")
