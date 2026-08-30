#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
"""Cross-encoder per-MB join on hf_probe bitstreams: the contingency table of
our MB class against x264's (skip/inter/intra from `ffmpeg -debug mb_type`),
with each side's recon MSE and AC-energy retention on every cell.

This is the attribution half of the HF instrument: hf_probe.py says WHETHER the
recon sheds energy; this says on WHICH decision disagreements, and what the
other encoder's choice bought on the same blocks. The skip/skip cell is the
reference-chain control: neither side coded anything there, so any MSE or
retention difference on it is inherited entirely from reference quality.

Usage (encodes if the hf_probe bitstreams are not already in HF_SCRATCH):
  python3 scripts/hf_join.py foreman_cif 100 48 [next-vs x264-med]
"""
import os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hf_probe as hf


def decode(bs, w, h, frames):
    yuv = bs + ".j.yuv"
    hf.run([hf.FFMPEG, "-y", "-hide_banner", "-i", bs, "-pix_fmt", "yuv420p",
            "-f", "rawvideo", yuv])
    y = hf.read_yuv(yuv, w, h, frames)
    os.unlink(yuv)
    return y


def main():
    clip, frames, qp = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    arm_a = sys.argv[4] if len(sys.argv) > 4 else "next-vs"
    arm_b = sys.argv[5] if len(sys.argv) > 5 else "x264-med"
    os.makedirs(hf.SCRATCH, exist_ok=True)
    src = os.path.join(hf.REPO, "tests/corpus", clip + ".y4m")
    w, h, sy = hf.read_y4m(src, frames)
    mbw, mbh = w // 16, h // 16
    cls, dec = [], []
    for arm in (arm_a, arm_b):
        bs = os.path.join(hf.SCRATCH, f"{clip}_{arm}_{qp}.264")
        if not os.path.exists(bs):
            hf.encode(arm, clip, frames, qp, bs)
        dec.append(decode(bs, w, h, frames))
        cls.append(hf.mb_classes(bs, mbw, mbh, frames)[1:])
    ac, bc = cls

    def mb_view(a):
        return a.reshape(a.shape[0], mbh, 16, mbw, 16)

    sv = mb_view(sy[1:])
    mses, acs = [], []
    for d in dec:
        dv = mb_view(d[1:])
        mses.append(((dv - sv) ** 2).mean(axis=(2, 4)))
        acs.append(dv.var(axis=(2, 4)))
    sac = sv.var(axis=(2, 4))
    names = {0: "skip", 1: "inter", 2: "intra"}
    tot = ac.size
    print(f"# {clip} qp{qp} rows={arm_a} cols={arm_b}: share% | MSE {arm_a}/{arm_b} | "
          f"AC-retention {arm_a}/{arm_b} (recon var / src var)")
    for a in (0, 1, 2):
        for b in (0, 1, 2):
            m = (ac == a) & (bc == b)
            n = m.sum()
            if n == 0:
                continue
            den = max(sac[m].sum(), 1e-9)
            print(f"{names[a]:5s}/{names[b]:5s} {100*n/tot:5.1f}% | "
                  f"{mses[0][m].mean():7.1f} {mses[1][m].mean():7.1f} | "
                  f"{acs[0][m].sum()/den:.3f} {acs[1][m].sum()/den:.3f}")


if __name__ == "__main__":
    main()
