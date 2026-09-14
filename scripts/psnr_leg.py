#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""psnr_leg.py -- the PSNR-Y floor, aggregated and adjudicated in one place.

THE LEG (2026-09-14, owner). PSNR-Y at equal bytes against x264 is an
additional READING on the goal legs. It is a FLOOR, not a decision metric:

    fail   more than 1.0 dB below x264 on any clip
    debt   0.5 dB or more below x264 -- listed, never gated
    pass   everything else

WHY IT EXISTS, and why it is a floor rather than a fifth thing to optimise.
The sibling H.265 campaign scored VMAF-NEG and PSNR on the same fifteen clips
and they disagreed on nine of them, while the level gap at equal bytes stayed
small -- median -0.08 dB, worst -0.87. Small gaps and frequent disagreement is
exactly the shape in which an item can buy VMAF by spending pixel accuracy and
have no column notice. The floor notices. It is deliberately loose (1.0 dB is
far outside anything the campaign measured) because its job is to catch a
direction of travel, not to arbitrate tenths: the quality leg stays dVMAF, and
nothing is decided on PSNR.

WHY PER CLIP AND NOT ON THE MEDIAN. A median floor is not a floor -- half the
corpus can sit under it and the row still passes. The worst-clip speed leg is
already read this way for the same reason, so the floor follows the form the
board already has: the median is REPORTED, the worst clip is what is JUDGED.

Usage: psnr_leg.py <label-width> <clip>=<dpsnr_dB> ...
Prints the dPSNR-Y / PSNR-FLOOR / PSNR-DEBT rows; the set scripts feed it and
scripts/parity-status.sh greps those three labels back out.
"""
import statistics
import sys

FAIL_DB = 1.0
DEBT_DB = 0.5


def main(argv):
    w = int(argv[1])
    pairs = []
    for a in argv[2:]:
        clip, _, v = a.rpartition("=")
        if v in ("", "n/a"):
            continue
        pairs.append((clip, float(v)))
    if not pairs:
        print(f"{'dPSNR-Y':<{w}} {'n/a':>9}    (no clip scored PSNR-Y)")
        return 0

    vals = [v for _, v in pairs]
    worst_clip, worst = min(pairs, key=lambda cv: cv[1])
    print(f"{'dPSNR-Y':<{w}} {statistics.median(vals):>+9.2f} dB "
          f"(median; worst {worst_clip} {worst:+.2f})")

    bad = sorted([cv for cv in pairs if cv[1] < -FAIL_DB], key=lambda cv: cv[1])
    verdict = "PASS" if not bad else "FAIL"
    detail = (f"worst {worst:+.2f} dB; bar: more than {FAIL_DB:.1f} dB below x264 fails"
              if not bad else
              "under the floor: " + ", ".join(f"{c} {v:+.2f}" for c, v in bad))
    print(f"{'PSNR-FLOOR':<{w}} {verdict:>9}    ({detail})")

    debt = sorted([cv for cv in pairs if -FAIL_DB <= cv[1] <= -DEBT_DB],
                  key=lambda cv: cv[1])
    print(f"{'PSNR-DEBT':<{w}} "
          + ("none".rjust(9) + f"    (nothing at or past {DEBT_DB:.1f} dB below x264)"
             if not debt else
             " " * 9 + "    " + ", ".join(f"{c} {v:+.2f}" for c, v in debt)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
