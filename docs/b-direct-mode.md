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
and we accept the direct prediction.

**But the share is NOT what costs us, and the control says so.** Re-run with
`--direct temporal`, the direct share is **86.74%** -- unchanged from spatial's
86.41% -- while the same encode drops 3.6% of its bytes at the same CRF and
gains **-6.39% BD**. The same macroblocks take direct either way. What changes
is the VECTOR they get.

So there are two independent facts here, and an early draft of this doc
conflated them into one causal story:

1. Our direct share sits 11 points above x264's. That is a decision-regime
   difference and it is not what the -6.39% comes from.
2. Our spatial direct VECTOR is poor under smooth global rotation. Spatial
   derives from the neighbour predictor; temporal scales the co-located vector,
   which tracks a rotating field far better. With 86% of B macroblocks taking
   direct on this content, that vector's quality dominates the whole B result.

`--direct temporal` gains -6.39% on blue_sky, +0.39% on riverbed and +0.04% on
crowd_run, so it is specific to this content rather than a general 1080p fix.

Partitioning is not the lever either, which is worth recording because the
prior B-direction work pointed there: `Y264_B_RECT=1` reads +0.61% and
`Y264_B_8X8=0` reads +0.11%, so B_8x8 buys nothing on this clip.

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
the switch: **the spatial direct vector under smooth global motion.** With 86%
of B macroblocks taking direct here, that one vector decides most of the B
result, and temporal proves a better vector is worth 6.4% on the same block
set. Temporal still recovers less than half the B-frame swing (-6.39 of 13.6),
so even flipping would leave blue_sky around +8% behind; the rest is in what
the tournament compares direct against, which is the 11-point share difference
and a separate question.

Kept as a measured arm, not a default. `--direct temporal` is the escape for
anyone encoding this content class today.

## 2026-08-31, second round: what is actually changeable

**The spatial direct vector is normative and the encoder cannot improve it.**
Clause 8.4.1.2.2 derives it from the neighbours and the co-located block, and
the decoder derives the same one. Our implementation reads spec-shaped:
MinPositive over the three predictor neighbours, colZeroFlag on refIdx 0 with
both components in [-1,1], 8x8 inference sampling each 8x8's outer corner.
"Fix our spatial direct" was never an available move, so this doc's earlier
open item was mis-stated.

What the encoder does choose is the SLICE-level mode flag, and whether a given
macroblock uses direct at all. So the only legal lever for the blue_sky gain is
a per-slice decision, ie an `auto` mode. x264 has one at `--preset slow` and
above; medium, which is what both encoders ran here, is fixed spatial.

### x264's auto, and why its signal is shaped the way it is

Per B macroblock it derives BOTH direct modes and runs `probe_bskip` on each,
adding the boolean to `i_direct_score[mode]` -- it counts how many macroblocks
each mode would make SKIPPABLE. The next B slice takes the higher score, with a
9/10 decay once the total passes the macroblock count. That is two direct
derivations and two skip probes per B macroblock, which is why it is a
slow-preset feature.

### The cheap signal does not work, measured

`Y264_DIRECT_SCORE=1` (new, measurement only, t1, verified md5-identical on and
off) logs per B frame the direct prediction's SSD against the source and the
macroblock count. Run the same encode in each mode and diff:

| clip | temporal better on | median per-MB SSD | actual BD |
|---|--:|--:|--:|
| blue_sky_1080p | 65% of frames | -3.6% | -6.39% |
| coastguard_cif | 63% of frames | -0.6% | **+1.08%** |
| samsung_720p | 58% of frames | +1.5% | **-0.84%** |

**Prediction error does not predict the outcome.** coastguard predicts better
under temporal and codes worse; samsung predicts worse and codes better. Nor
does the direct SHARE: it moves 86.41% to 86.74% while BD moves 6.4 points. So
an auto mode built on either cheap proxy would misfire on two of three clips
here, and x264's threshold-shaped skippability count is load-bearing rather
than incidental.

### The skippability score, built -- and what it found instead

`Y264_DIRECT_SCORE=2` derives BOTH direct modes per B macroblock and runs the
B-skip probe on each, counting skippable macroblocks per mode per frame. The
probe reads its prediction out of `rec`, so each arm writes `rec` and the
caller's content is restored; verified byte-identical with the scorer armed.
Where both modes score, the signal behaves: on blue_sky temporal makes
consistently more macroblocks skippable (6543 against 6143, 7300 against 6784).

**But most frames score nothing, because temporal direct is not LEGAL on them.**

| clip | B frames | temporal legal on |
|---|--:|--:|
| blue_sky_1080p | 28 | 3 (11%) |
| coastguard_cif | 28 | 3 (11%) |
| samsung_720p | 25 | 6 (24%) |

The guard is not ours to relax. Clause 8.4.1.2.3 makes it a requirement of
bitstream conformance that the picture referred to by `refIdxCol` be present in
RefPicList0, and `build_slice_prep` enforces exactly that, frame-wide, falling
back to spatial when any co-located block's reference does not resolve. x264
tests something narrower -- `fref[1][0]->i_poc_l0ref0 == fref[0][0]->i_poc` --
which holds for it because of how it constructs its lists.

**So `--direct temporal` collects -6.39% on blue_sky while engaging on about a
ninth of its B frames.** Those frames are carrying a disproportionate amount,
which points at the reference B frames in the pyramid, whose quality propagates
into every leaf that references them.

### Where this leaves the item

An `auto` mode is worth much less than it looked: its choice space is the 11-24%
of frames where temporal is legal at all, and on the rest there is nothing to
choose. The scoring signal is built and sound, and it is not the bottleneck.

**The lever is reference-list construction** -- how often the co-located
picture's references land in the current slice's list 0. That decides how often
temporal is available, and the blue_sky evidence says availability is worth far
more per frame than the mode choice is. That is an architectural question about
list construction and the B pyramid, not a knob, and it should be costed before
anything is built on top of it.
