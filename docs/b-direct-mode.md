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

## 2026-08-31, third round: the guard was at the wrong granularity

The section above ends on the wrong conclusion, and it is left standing so the
reasoning is visible. "The guard is not ours to relax" is true of the spec
requirement and false of the way we enforced it. Reference-list construction is
not the lever. Granularity is.

### What the frame gate actually costs

`Y264_DIRECT_WHY=1` prints each B slice's list 0 beside the distinct co-located
reference POCs, starring the ones that do not resolve. blue_sky, CRF 26:

```
poc=12 legal=1 list0=[8,4,0]  col=[4(10416),8(113908),0(3468)]
poc=10 legal=0 list0=[8,4,0]  col=[16*(19424),8(110048),4(756),0(220)]
poc=14 legal=0 list0=[12,8,4] col=[4(10416),8(113908),0*(3468)]
```

Two failure shapes, and both are ordinary. A leaf B whose co-located picture is
a reference B inherits that picture's list-1 blocks, whose POCs are in the
future and cannot appear in a past-only list 0: 19,424 blocks of 131,000, about
15%. A leaf B whose co-located picture is an anchor inherits references to
anchors that our own three-entry list 0 has already dropped: 3,468 blocks, or
2.6%.

**2.6% of the blocks demoted 100% of the frame.** Clause 8.4.1.2.3's
conformance requirement binds where the derivation runs, not where the slice
header points, so a block that does not resolve costs its own macroblock the
direct mode and nothing else. x264 has always done it that way:
`<reference-internal>` returns 0 on exactly this condition, with
the comment "the collocated ref isn't in the current list0", and the caller
drops direct for that macroblock.

Ours now does the same. `temporal_direct` returns 0 when any of the four
sampled corners fails to resolve, and `direct_ok` goes down, which is the path
the staircase MV clamp already used. The per-macroblock cost is 1-10% of
macroblocks, median about 1.7%. The frames recovered are 89% of them.

`Y264_DIRECT_PERMB=0` restores the old frame-wide gate for A/B. Default is on.

### So every earlier measurement of `--direct temporal` was of a mode that
### mostly did not engage

That includes "temporal is corpus-neutral" and the -6.39% on blue_sky. None of
them are evidence about temporal direct. Re-measured, against x264 medium,
VMAF-NEG, CRF, 150-frame windows, points 34-46, all six 1080p clips:

| clip | spatial (default) | temporal, per-MB | delta |
|---|--:|--:|--:|
| station2 | -23.33% | **-46.63%** | **-23.3** |
| blue_sky | +14.24% | **-16.57%** | **-30.8** |
| crowd_run | -7.37% | -6.21% | +1.2 |
| riverbed | -1.81% | +4.58% | +6.4 |
| pedestrian | -3.62% | +3.17% | +6.8 |
| sunflower | -9.77% | **+14.85%** | **+24.6** |

That point set is NOT the one the +14.17% / -13.69% table in
`docs/fable-b-path-brief.md` used, which picked points per clip to stay off
saturation, so four of the six sit in a band that table never measured. Read it
for the delta column only. **The quotable version is the rate-anchored table
below**, which replaces it.

### The rate-anchored table, and it reproduces the published one

`scripts/direct_rate_table.py` solves every arm onto the same achieved byte
targets, taken from the calibrated operating points in `docs/corpus-sources.md`
scaled by the clip's own duration, on a 0.4 to 1.15 ladder around each. That
removes the band question: a shared CRF point set puts clips of different
difficulty in different places, and a documented kbps does not.

| clip | spatial | temporal | delta | published control |
|---|--:|--:|--:|--:|
| station2 | -12.44% | **-31.86%** | **-19.4** | -13.69% |
| blue_sky | +14.40% | **-8.66%** | **-23.1** | +14.17% |
| pedestrian | -0.55% | +7.69% | +8.2 | -0.90% |
| riverbed | +1.94% | +4.60% | +2.7 | +2.40% |
| crowd_run | +2.04% | +4.55% | +2.5 | +1.76% |
| sunflower | -7.16% | **+30.34%** | **+37.5** | -8.39% |

The spatial column lands within 1.3 points of the published control on all six,
which is what makes the temporal column quotable beside it. It also retires the
-46.63% that the shared point set produced for station2: the honest figure is
**-31.86%**, and the -46.63% was the band talking.

**Taking the better mode per clip moves the 1080p median from +0.70% to
-3.86%**, and takes the class from three of six ahead of x264 to four of six.
The worst clip in the corpus goes from +14.40% to -8.66%.

Three instruments now agree on sign and roughly on size: the shared point set,
`scripts/bd_at_rate.py` against our own default (station2 -31.74%, blue_sky
-21.76%, sunflower +24.61%), and the rate-anchored table.

### The mechanism, and it predicts the sign

Spatial direct takes a neighbour median and snaps to zero motion wherever
colZeroFlag fires, so it is right on content that is still or nearly still and
robust when the co-located field is noise. Temporal direct scales the
co-located vector per 8x8, so it is right when the motion field is coherent and
persists frame to frame, and it amplifies noise when it is not.

Order the six clips by how coherent their global motion is and you get the
delta column: station2 is a steady pan, blue_sky a slow rotation, crowd_run
dense but incoherent motion, riverbed water, pedestrian a fixed camera, and
sunflower a near-static close-up. Motion coherence, not motion magnitude:
crowd_run and riverbed both move a great deal and both want spatial.

**The signal that measures this already exists.** `Y264_DIRECT_SCORE=2` is
x264's own: derive both modes per B macroblock, probe each for skippability,
count. Its choice space was 11-24% of frames; now it is all of them. Taking the
per-clip majority of frames where temporal scores higher:

| clip | frames temporal wins | predicted | actual |
|---|--:|---|---|
| station2 | 29/43 (67%) | temporal | temporal, -23.3 |
| blue_sky | 26/43 (60%) | temporal | temporal, -30.8 |
| sunflower | 11/43 (26%) | spatial | spatial, +24.6 |
| crowd_run | 4/43 (9%) | spatial | spatial, +1.2 |
| riverbed | 0/42 | spatial | spatial, +6.4 |
| pedestrian | 0/43 | spatial | spatial, +6.8 |

Six of six, with a 50% threshold that has margin on both sides, and six of six
again against the rate-anchored table rather than the shared point set.

### Out-of-sample: the twelve gate clips

The score is x264's, fitted to nothing of ours, so the whole gate corpus is
out-of-sample. It predicts spatial on all twelve, the closest calls being
sintel at 44% and sita at 0% but a near-unity ratio. Measured at points 22-34
(bus and bbb saturate there and are unusable):

| clip | spatial | temporal | delta |
|---|--:|--:|--:|
| akiyo | -9.08% | -0.08% | +9.00 |
| samsung | -8.16% | +0.26% | +8.42 |
| coastguard | -15.90% | -10.21% | +5.69 |
| foreman | +0.91% | +5.69% | +4.78 |
| stefan | +10.65% | +15.37% | +4.72 |
| park_joy | +1.47% | +5.56% | +4.09 |
| mobile | +1.59% | +5.11% | +3.52 |
| ducks | -6.31% | -3.64% | +2.67 |
| sintel | -20.55% | -20.61% | -0.06 |
| sita | +9.77% | +7.32% | **-2.45** |

Fourteen of sixteen clips right by sign across both corpora, one tie inside
noise (sintel), one miss worth 2.45% (sita, hand-drawn 2D animation, which the
corpus already knows disagrees with the CGI clip about everything).

### What this is worth, and what it is not

Choosing per clip would take the 1080p median from about -5.5% to about -8.6%
and would delete the worst clip in the corpus: blue_sky goes +14.24% to
-16.57%. On the gate corpus the same rule picks spatial everywhere and the
worst case is leaving sita's 2.45% on the table. Nothing regresses.

**Nothing here flips a default.** Bare yah264 is still x264 medium and still
spatial, and every default-path encode is byte-identical to be88345 at threads
1 and 8. What changed is that `--direct temporal` now does what its name says.

### The next item, and its hard part

`--direct auto`, per slice, on x264's rule: score both modes, keep a running
total with 9/10 decay once it passes the macroblock count, and let the next B
slice take the higher. The scoring code exists and is verified inert.

The hard part is not the score, it is determinism. A running total accumulated
across frames is order-dependent, and our GOP-parallel workers do not encode
frames in slice order, so a decayed cross-frame accumulator would make bits
depend on when a chain finished. That is the one thing the threading design
does not allow. Two ways out worth costing: decide in the lookahead, which is
already a deterministic serial stage with a lowres motion field, or scope the
accumulator to a GOP so each worker's chain is self-contained. Neither is
sized yet.

### The staircase exclusion is real, not conservative

`--direct temporal` disables the staircase wide ring (`stair_clamp_on`,
`stair_wide_capable`, `stair_lag_capable`). An earlier note in this session
guessed that was conservative because the per-macroblock `direct_ok` clamp loop
is mode-agnostic. That guess was wrong, and `stair_clamp_on`'s own comment says
why. Two reasons, and the clamp loop answers neither.

The clamp loop bounds `mvL0` only. Temporal direct also derives
`mvL1 = mvL0 - mvCol`, and no closure bounds that: `mvCol` comes from another
picture's motion field and is not one of the already-clamped coded MVs that
spatial direct's median closes over. Covering it needs a second test against
the list-1 clamp, which is small but real work.

The deeper one is that temporal direct READS the co-located motion field of the
list-1 picture, and under the staircase that picture is still being encoded.
Its `colmv` rows are published progressively behind a row gate, the same as its
recon rows, so the data is reachable in principle. What does not currently
exist is a wait on it from the direct derivation.

So the exclusion stands until someone builds both halves. What decides whether
that is worth doing is what the staircase is worth in wall, and it is worth a
lot.

| clip | threads | spatial | temporal | ratio |
|---|---|--:|--:|--:|
| blue_sky, low rate | 1 | 5.88s | 5.75s | 0.977 |
| blue_sky, low rate | 12 | 0.78s | 1.09s | **1.398** |
| riverbed, high rate | 1 | 12.69s | 12.48s | 0.984 |
| riverbed, high rate | 12 | 1.43s | 1.83s | **1.278** |

Best of three, CRF so the staircase's rate-control term passes, both ends of the
rate range because rate orders the speed table about four times as strongly as
resolution.

**At one thread temporal direct is free.** It is very slightly faster, which
makes sense: it derives one vector per 8x8 from a stored field instead of
running a median over neighbours. **At twelve threads it costs 28 to 40%**, and
none of that is the derivation. It is the staircase wide ring, given up.

That reframes the next item. Bounding `mvL1` and adding a colmv row wait is not
housekeeping to do after the mode question is settled, it is the thing that
decides whether the mode question is worth settling at all: a 23-point BD win
on blue_sky that costs 40% of threaded wall is not obviously a win, and the
same arm at one thread costs nothing. Whoever picks this up should price the
staircase work first.
