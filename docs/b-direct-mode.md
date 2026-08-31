# The B direct mode, and what blue_sky says about it

Opened 2026-08-31 by the worst clip in the new corpus. Nothing here is shipped;
the closing recommendation is deliberately not a flip.

## Where it started

`blue_sky_1080p` reads **+14.17% BD-rate against x264 medium** (VMAF-NEG, CRF,
150-frame window, points 34-46). That is the largest single-clip deficit we
have, and it sits on smooth, low-texture content under a slowly rotating
camera, which is a class the old twelve-clip corpus had nothing like.

The deficit is uniform: we spend 12-20% more bits at every quality level from
VMAF-NEG 50 to 88. Nothing about it is a deep-rate or saturation artefact.

## What it is not

| ruled out | reading |
|---|--:|
| bit allocation, seven knobs | best of them, AC_GAIN=1.0, is -0.84%; aq-strength 0 is +23.15%; mb-tree OFF is +5.66%, so mb-tree is helping |
| the fine-intra gate | -0.69%, and 24 / 32 / always-on are identical because a 1.5x fine margin is vacuous against the 1.5x admission margin |
| multiref | `--ref 1` still reads +10.40% |
| the B skip exits | `Y264_B_SKIP_EXIT=0`, the full tournament, reads +0.39% |

The P-frame census is a red herring worth recording so nobody re-chases it. At
matched rate we code 7.50% intra against x264's 4.70%, and our intra split is
inverted -- 94% I_16x16 against their 26%. Striking, and worth 0.7%.

## What it is

**The whole deficit is the B-frame path.** With `--bframes 0` it collapses to
**-0.94%**, ie parity.

| | blue_sky BD, B-frames against no B-frames |
|---|--:|
| x264 | **-10.49%** (B-frames pay them) |
| us | **+3.15%** (B-frames cost us) |

That 13.6-point swing is the entire gap.

**The mechanism is direct over-selection.** B macroblock census at matched
bytes:

| B mode | ours | x264 |
|---|--:|--:|
| direct, including B_Skip | **86.41%** | **75.54%** |
| L0 explicit | 11.2% | 16.7% |
| L1 explicit | 2.2% | 7.4% |
| Bi | 0.07% | 0.38% |

Eleven points, about 97,000 macroblocks, where x264 searches an explicit vector
and we accept the direct prediction. Spatial direct derives its vector from
neighbours, which predicts badly under smooth global rotation, so accepting it
is exactly the wrong trade on this content. Both encoders run spatial direct
here, so this is our implementation of it rather than the mode.

`--direct temporal` recovers **-6.39%** on blue_sky, +0.39% on riverbed and
+0.04% on crowd_run, so it is specific to this content rather than a general
1080p fix.

## Corpus reading for `--direct temporal`

`band_at_rate`, BANDS=all, twelve clips, matched achieved rate:

```
median +0.01%   mean -0.10%   negative 6/12   worst coastguard +1.08%
```

Winners are touchdown -1.74, samsung -0.84, sintel -0.60; the payers are
coastguard +1.08 and tempete +0.85.

## Recommendation: do not flip the default

The shape passes the portfolio rule -- narrow-positive, elsewhere-neutral --
but a default flip here spends something the corpus median cannot see. Bare
`yah264` is deliberately x264 medium, and x264 medium is spatial. Flipping the
mode buys a corpus-neutral median and breaks that correspondence, which is the
one property that makes our comparisons legible.

The gain is real, so the item worth opening is the one that collects it without
the switch: **why our spatial direct is weak under smooth global motion when
x264's is not.** Temporal recovers less than half the B-frame swing (-6.39 of
13.6), so even flipping would leave blue_sky around +8% behind. The remaining
half is in the direct vector itself or in what the tournament compares it
against.

Kept as a measured arm, not a default. `--direct temporal` is the escape for
anyone encoding this content class today.
