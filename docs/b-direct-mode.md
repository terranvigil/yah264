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

Per B macroblock it derives BOTH direct modes and runs the B skip probe on each,
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
tests something narrower, that the list-1 reference's own first list-0 picture
is this slice's first list-0 picture, which holds for it because of how it
constructs its lists.

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
direct mode and nothing else. x264 has always done it that way: its
temporal-direct derivation refuses the mode on exactly this condition, a
co-located reference that is not in the current list 0, and drops direct for
that macroblock alone.

Ours now does the same. `temporal_direct` returns 0 when any of the four
sampled corners fails to resolve, and `direct_ok` goes down, which is the path
the staircase MV clamp already used. The per-macroblock cost is 1-10% of
macroblocks, median about 1.7%. The frames recovered are 89% of them.

**It defaults on only at THREADS 1**, because with it on above one thread the
output is not reproducible run to run. `scripts/stair_determ.sh` reads 0/32 with
per-macroblock gating on and 32/32 with it off, and three runs of the same
ducks_720p command at t18 produce three different bitstreams. With the gate off,
or with spatial direct, the same command is bit-exact across runs.

**Where it is NOT, corrected 2026-09-01.** An earlier revision of this section
blamed the co-located motion field and cited a macroblock count that moved
between runs. Both halves were wrong.

The count came from `y264_tdir_mb`, a non-atomic global incremented from the
macroblock loop, so above one thread it loses counts and disagrees with itself.
It was measuring its own race, not the encoder's.

And the co-located field is provably stable. `Y264_DIRECT_WHY=1` now prints a
`colhash` over the whole `colpoc`/`colmvx`/`colmvy` grid beside each slice's
list 0. Three runs at t18 produce **byte-identical dwhy output** -- same list 0,
same co-located POC sets, same legality verdict, same colhash -- and three
different bitstreams. So every input to the direct decision is deterministic and
the instability is downstream of it.

Five candidates are eliminated, each with a test that can be re-run:

| candidate | test | verdict |
|---|---|---|
| the co-located motion field | `colhash` identical across three t18 runs | not it |
| the staircase | reproduces with `Y264_STAIR=0` | not it |
| uninitialised `dp`/`dcp` | fill them with a fixed pattern: no change | not it |
| the `direct_ok == 0` path itself | `Y264_DIAG_DIRECTOK=7` forces it on a fixed macroblock set under SPATIAL direct: bit-exact over three t18 runs | not it |
| hpel fast path vs the `mc_luma` fallback | `Y264_DIAG_NOHPEL=1` forces the fallback everywhere: still nondeterministic | not it |
| an out-of-range derived vector | `Y264_DIAG_TDIRLIM=64` refuses direct whenever the derived mvL0/mvL1 exceeds +/-64 pel: still nondeterministic | not it |

The fourth is the useful one. Forcing `direct_ok = 0` on a fixed, content
independent set of macroblocks is deterministic, so the path per-macroblock
gating newly makes common is not itself the problem. What distinguishes the
failing configuration is that temporal direct is USED on nearly every B frame
rather than 11-24% of them.

So the next question is not "what does the per-macroblock gate break" but
"**is temporal direct nondeterministic under threads even when it is fully
legal, and did the frame-wide gate simply make it rare?**"

x264 carries a guard here that we have no equivalent for: above one thread its
temporal-direct derivation refuses direct for a macroblock whose scaled vertical
vector runs past the reference area a thread is allowed to read, and we apply no
range test to the derived vector at all. That looked like the answer and it is
not: `Y264_DIAG_TDIRLIM=64` refuses direct on
any derived vector beyond +/-64 pel and the output still moves between runs. The
guard is worth having for its own sake; it is not this bug.

### Localized: two macroblocks, bi against L0

Six failed guesses was enough. Decoding both runs with `ffmpeg -debug mb_type`
and diffing the macroblock grids frame by frame names it exactly.

Of 48 coded frames, **47 are identical**. The one that differs is coded frame 3,
a B frame, and within it **two macroblocks** differ: row 37, macroblocks 21 and
23. One run codes them `X`, bi-predicted; the other codes them `>`, forward
only. Nothing else in the picture moves, and the frames coded before it --
including the mini-GOP's reference B -- are bit-identical.

That shape is a decision flipping, not data being corrupted wholesale. Two
macroblocks out of about 3900, choosing between two inter modes, on a frame
whose every documented input is identical between the runs.

**Note which modes.** `X` against `>` is bi-prediction against list-0 only.
Direct is not the mode that changes. Temporal direct is what makes the frame
nondeterministic, but the decision that actually moves is an ordinary
list-0-versus-bi tournament outcome, and bi is the candidate that reads list 1.

### The mechanism: an uninitialised `struct direct_mv` seeding the searches

**This section replaces an earlier one that concluded the cause was the
in-flight reference B's pixels. That was wrong, and it was the third wrong
diagnosis of this bug.** The elimination table above stands as a record of what
was tested; the conclusion drawn from it did not. Note its third row in
particular -- "uninitialised `dp`/`dcp`, fill them with a fixed pattern, no
change". That is the right class, tested one variable away: the prediction
buffers rather than the MV struct sitting beside them.

Two splits narrow it before any code is read.

| shape | pair formed? | three runs |
|---|---|---|
| `--bframes 1` | no | **bit-identical** |
| `--bframes 2` | no | **bit-identical** |
| `--bframes 3` | yes | three different bitstreams |
| `--bframes 3`, `Y264_FPIPE=0` | pair disabled | **bit-identical** |
| `--bframes 3`, `Y264_W2=0` | pair still formed | three different bitstreams |

`Y264_FPIPE=0` alone makes the reproducer reproducible, and `Y264_W2=0` does
not. So the vector is `code_b_pair` -- the two sibling non-reference leaves
encoded concurrently -- and not the parent's trailing entropy emit. That also
retires the pixels theory on its own: the mini-GOP's reference B has been
analysed, deblocked, border-extended and DPB-stored before `code_b_pair` is
called. Nothing writes it while the leaves read it.

Splitting the Annex B stream per NAL says the same thing from the other end.
Across two runs the SPS, PPS, IDR, every P and every reference B are byte-equal;
the only NALs that ever differ are non-reference leaves, and always the SECOND
of each pair -- the one submitted to `fp_bg` while the first runs on the caller.

A per-macroblock dump of the analysis records, taken after the wavefront has
joined so the print order is deterministic, then lands it. Between 270 and 410
macroblocks per differing frame show a different `dmv.mvL1[0]` -- and every one
of them is a macroblock where `temporal_direct` returned 0. Strip that field and
exactly **two** macroblocks per frame actually differ, which is the count the
`mb_type` diff found. The mode flips are the symptom; the varying `dmv` is the
cause.

```c
struct direct_mv dmv;               /* uninitialised stack local */
int tdir_ok = 1;
if (f->direct_temporal)
    tdir_ok = temporal_direct(f, mbx, mby, &dmv);   /* may return 0 MID-LOOP */
else
    spatial_direct(f, mbx, mby, &dmv);              /* always fills */
...
seed0[2*ns0] = dmv.mvL0[0][0]; ... ns0++;           /* unconditional */
seed1[2*ns1] = dmv.mvL1[0][0]; ... ns1++;           /* unconditional */
```

`temporal_direct` returns 0 from inside its four-block loop, on the block whose
co-located reference does not resolve in this slice's list 0. A refusal
therefore leaves `dmv` PARTLY written -- which its own doc comment already said
("Returns 0 when any of the four sampled corners fails to resolve, and d is then
not usable"). The contract was written down; the caller did not honour it. The motion search's seed list then reads
block 0 whether the derivation succeeded or not, so a refused macroblock seeds
both list searches with **stack garbage**.

That is deterministic for exactly as long as the worker's call history is.
One frame in flight: every pool worker walks the same sequence of frames and
cells, the residue under `dmv` is the same every run, and the encode reproduces.
Two sibling leaves sharing the same workers: which frame a worker served last
decides what is on its stack, and the seed changes run to run. Hence
`--bframes 1`/`2` clean, `Y264_FPIPE=0` clean, the second leaf of each pair
dirty.

It also explains why per-macroblock gating is what exposed it, and the reason is
sharper than "the gate made it rare". The frame-wide gate is immune BY
CONSTRUCTION: it scans the whole `colpoc` grid in the slice header and demotes
the frame to spatial if any block is unresolvable, so inside a frame that does
code temporal direct, `temporal_direct` can never return 0 and `dmv` is always
fully written. Per-macroblock gating is exactly the change that lets a refusal
happen inside a temporal frame -- which is what makes the partial fill
reachable. Measured: with `Y264_DIRECT_PERMB=0` the reproducer is byte-identical
between `77c8b46` and the fix, and deterministic on both.

And it explains every negative result above. Recon-match passes because the
garbage reaches a SEARCH SEED, never the reconstruction: the encoder builds
exactly what it signals, it just chose differently. ThreadSanitizer is silent
because reading your own thread's stale stack is not a data race. AddressSanitizer
is silent because the memory is validly allocated. The one tool that would have
named it in a line is MemorySanitizer, which does not exist for this target.

**The fix**, in `analyze_b_mb`:

- zero `dmv` before the derivation, so no reader can see a partly-derived MV
  (the staircase's own range checks at `stair_clamp0_poc` read `refL0` the same
  unconditional way);
- add the direct MV to the seed lists only when `tdir_ok`, because a direct
  mode that does not exist has no predictor to offer.

The second is NOT free, and the guess that it would be was wrong: it changes
bits on 12 of a 24-cell matrix (four clips x `--bframes 3/7` x t1/t8, all at
`--direct temporal`). A seed is not only a start candidate -- the seed list's
length feeds the search's dedup and tie-breaking -- so dropping the placeholder
moves results. That is not a bundled unmeasured change, though, because there is
no prior behaviour to preserve here: before the fix this seed was undefined, so
(0, 0) and "no seed" are both new, and the rate-anchored table below is measured
with the gate on.

Measured: the reproducer becomes identical across runs AND across
`--threads 2/4/8/18`. Under six busy-loop spinners, `scripts/stair_determ.sh`
reads 48/48 at t4, t8 and t18 with `--direct temporal` (0/32 for the same
command on `77c8b46`), and `scripts/determ_repeat.sh` with
`ARGS='--direct temporal'` reads 8/8 configs over 8 runs each where `77c8b46`
with the gate forced on reads 0/8 -- seven of those eight emitting five to eight
distinct bitstreams in eight runs.

`Y264_DIRECT_PERMB` therefore **defaults on at every thread count** as of
2026-09-01. Byte-identity against `77c8b46` holds 32/32 over a CIF/720p/1080p x
CABAC/CAVLC x CQP/CRF/ABR x t1/t2/t8/t18 matrix on the shipped default, because
`--direct spatial` always fills `dmv` and never touched this. Only
`--direct temporal` moves.

**Method note.** The three wrong diagnoses all came from reasoning about which
shared object two threads could be fighting over. What actually found it was
narrowing the concurrency (which knob makes it stop?), then narrowing the output
(which NAL differs?), then dumping the decision inputs per macroblock and
diffing. Each step is cheap and each one halves the search space; none of them
requires a theory.

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

**Re-measured after the `dmv` fix (2026-09-01).** Every number in this table was
produced by an encoder that was seeding some of its searches with stack garbage,
so it needed re-running on a reproducible one before it could be quoted without
a caveat. The two clips the win rests on come back within a tenth of a point:

| clip | spatial | temporal | delta | table above |
|---|--:|--:|--:|--:|
| station2 | -12.44% | **-31.72%** | **-19.3** | -19.4 |
| blue_sky | +14.40% | **-8.62%** | **-23.0** | -23.1 |
| sunflower | -7.16% | **+30.11%** | **+37.3** | +37.5 |

The spatial column reproduces to the second decimal, as it must -- `--direct
spatial` is byte-identical across the fix. Every delta lands within 0.2 points
of the pre-fix table, the LOSS on sunflower included, so the fix moved the
reproducibility and not the ranking. The 19-to-23-point figure stands, and it is
now a number that repeats.

Noted 2026-09-03: only these three clips were re-run. The pedestrian, riverbed
and crowd_run rows of the rate-anchored table are still pre-fix readings, so
read them as the pre-fix encoder's, not as re-measured.

Which also settles what a flip would be worth: **nothing, as a blanket
default.** The same change that is worth -19 and -23 on two clips is worth +37
on a third, so `--direct temporal` is a per-content decision or it is not a
decision at all. See "Does a per-slice choice beat a per-clip one" below, and
the per-shot form it points at.

### The mechanism, and it predicts the sign

**RETRACTED, noted 2026-09-03.** Four more clips falsified this narrative, and
the retraction is in "the per-shot selector, built and CLOSED NEGATIVE" below:
three of the four steady pans it added want spatial, two of them by more than
nine points. The ordering below was fitted to six clips. Kept as written because
the retraction argues against it.

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

**VOID, noted 2026-09-03.** This table and the twelve-gate-clip one below were
taken at a different band and on the pre-`dmv`-fix encoder, and at the
out-of-sample operating point the same score calls spatial for station2 and
temporal for stockholm, wrong in both directions. Neither is quotable until it
is re-measured. See "the per-shot selector, built and CLOSED NEGATIVE" below.

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

Corrected 2026-09-03: that pair of medians, and the blue_sky numbers with them,
come from the shared point set the rate-anchored section above retired. The
quotable pair is **+0.70% to -3.86%**, with blue_sky +14.40% to -8.66%. Same
sign, same conclusion, smaller numbers.

**Nothing here flips a default.** Bare yah264 is still x264 medium and still
spatial, and every default-path encode is byte-identical to be88345 at threads
1 and 8. What changed is that `--direct temporal` now does what its name says.

### Does a per-slice choice beat a per-clip one? Mostly no, and that is the useful answer

`Y264_DIRECT_AUTO=1`, threads 1, BD against our own spatial default rather than
x264 (which is the comparison the question asks, and x264 was wedged on this
machine that night, see below).

| clip | frames voting temporal | temporal fixed | auto per-slice | best fixed | auto against it |
|---|--:|--:|--:|--:|--:|
| station2 | 29/43 | -32.91% | -30.91% | -32.91% | +2.00 worse |
| blue_sky | 26/43 | -26.54% | -24.45% | -26.54% | +2.09 worse |
| sintel | 19/43 | -2.06% | **-5.78%** | -2.06% | **-3.72 BETTER** |
| sunflower | 11/43 | +26.25% | **+2.66%** | 0.00% | +2.66 worse |

Three things, and they point the same way.

**Auto is sound as a safety mechanism.** On sunflower, where a fixed temporal
costs 26 points, auto costs 2.7. It does not need to be told which clip it is
looking at.

**On clips whose vote is decided it LOSES about two points to the fixed
choice.** That is the price of learning online: the running total takes several
slices to settle and mis-picks the ones before it does, on a clip where the
answer never changes.

**On the one clip whose vote is genuinely mixed it beats both fixed modes by
3.72 points.** sintel is the 19/43 clip. Per-slice switching is worth something
real, and it is worth it exactly where the content is mixed.

So the deterministic design is not a compromise. **A decision made per shot in
the lookahead should beat this per-slice version on the decided clips**, since
it knows the answer before the first slice instead of paying to learn it, while
keeping most of what sintel shows. Build that, not the order-dependent
accumulator. Evaluate it against sintel's -5.78% rather than assuming a shot
decision equals a clip decision.

### 2026-09-01: the per-shot selector, built and CLOSED NEGATIVE

The section above says to build a shot decision in the lookahead. It was built,
as far as a signal goes, and **it does not work.** Recording it properly,
because the failure retires more than the one arm.

**The signal.** `Y264_DIRECT_LRVOTE` (measurement only, verified md5-identical
on/off) simulates both derivations on the lowres field in mb-tree's Phase B --
which is inside the lookahead, so a decision there would be a pure function of
pre-dispatch state and deterministic at any thread count, the property the
per-slice accumulator cannot have. Two forms:

| mode | scores each derivation by | Spearman rho vs the 16 labels |
|---|---|--:|
| 1 | distance from the lowres search result | **-0.06** |
| 2 | lowres SATD of its BI-prediction | **-0.76** |

Mode 1 fails for a reason worth keeping: its spatial arm predicts from the TRUE
neighbouring MVs, so it is strongest exactly where the motion field is smooth,
which is the content temporal direct suits. The bias is anti-correlated with the
label, not merely additive. Mode 2 scores pixels and has no such advantage.

**Mode 2 looked strong, in-sample.** Over the 16 clips this document already
labels, the two clips temporal wins on ranked **1 and 2 of 16**, with a 0.058
gap below them -- the widest in the relevant region. A threshold in that gap
captures 94% of the available BD, and leave-one-out with the threshold placed at
the midpoint of the gap holds both clips in every fold.

**It fails out-of-sample, 0 for 3.** Predictions were recorded on twelve
unlabelled clips BEFORE measuring. Three came back temporal -- and they were the
three highest vote shares of any of the 28 clips ever run, above station2 and
blue_sky themselves:

| clip | predicted | share | measured (temporal vs spatial) |
|---|---|--:|--:|
| stockholm | TEMPORAL | 0.5838 | **+9.02%** |
| shields | TEMPORAL | 0.5585 | **+9.53%** |
| parkrun | TEMPORAL | 0.5629 | **+4.20%** |

Measured with `scripts/bd_at_rate.py`, self-anchored ladder at 0.5/0.75/1.0/1.4x
the CRF-26 size, 48 frames, VMAF-NEG 83-92 so the band discriminates. The two
calibrators in the same run come out right (station2 -13.71%, sunflower
+21.60%), so the instrument is sound and the predictions are wrong.

Scored inside that one instrument, with no band or cross-instrument confound:

| clip | share | measured |
|---|--:|--:|
| stockholm | 0.5838 | +9.02% |
| parkrun | 0.5629 | +4.20% |
| shields | 0.5585 | +9.53% |
| **station2** | 0.5081 | **-13.71%** |
| uneven | 0.4477 | +2.21% |
| old_town | 0.3906 | +2.89% |
| sunflower | 0.2588 | +21.60% |

**Spearman rho = +0.000.** The one clip that wants temporal ranks fourth of
seven. The rule's temporal calls cost **+9.04 BD points** where a working rule
would have gained.

**x264's own signal fails the same way.** `Y264_DIRECT_SCORE=2` at this
operating point calls SPATIAL for station2 (13 of 34 frames vote temporal) and
TEMPORAL for stockholm (26 of 34) -- wrong on both, in opposite directions. The
"six of six" and "fourteen of sixteen" tables above were taken at a different
band and on the pre-`dmv`-fix encoder, so they need re-measuring before either
is quoted again.

**And the mechanism story is falsified.** station2, stockholm, shields and
parkrun are all steady pans -- the content "motion coherence, not motion
magnitude" says wants temporal. Three of the four want spatial, two of them by
more than nine points. Ordering six clips by coherence and finding the delta
column was a narrative fitted to six clips; it does not survive four more.

**What this costs.** The -19 and -23 on station2 and blue_sky are real and now
reproducible, but nothing here can identify such a clip in advance, so the win
cannot currently be banked automatically. `--direct temporal` stays a flag for
someone who has measured their own content.

**The method lesson, and it is the M4 lesson again.** The in-sample fit rested
on TWO positive examples out of sixteen. Leave-one-out could not see that: with
two positives, holding one out just re-finds the same threshold from the other,
so LOO reported 94% for a rule worth nothing. **With a rare positive class,
resampling is not validation -- only new positives are.** Three of them cost one
afternoon and would have cost a lookahead plumbing change and a default flip.

The instrument stays in the tree, both modes, so the next attempt starts from a
measured baseline instead of a theory.

**And the instrument itself had the bug this whole page is about.** A code
review caught it before the work merged: the co-located motion was read guarded
only by the B's `lists_used`, never by the anchor's, and `mbt_pa_source` writes
`pmv[]` only on the path that also sets `plu != 0` -- so a co-located block that
went intra left `mvCol` reading uninitialised heap or a previous frame's motion
out of a malloc'd memo array that persists across walks. An uninitialised read,
in the instrument written to study an uninitialised read.

It is fixed (`!(pluc[i] & 1)` skips those blocks), and **the conclusion above
was re-derived on the fixed instrument rather than assumed to survive**:

| clip | before the fix | after |
|---|--:|--:|
| station2 | 0.5081 | 0.5082 |
| blue_sky | 0.5053 | 0.5055 |
| sunflower | 0.2588 | 0.2588 |
| stockholm | 0.5838 | 0.5841 |
| shields | 0.5585 | 0.5588 |
| parkrun | 0.5629 | 0.5630 |

Every share moves by at most 0.0003, no ordering changes, and the three
out-of-sample calls are still wrong -- co-located intra blocks are rare on this
content, so the defect had little to bite on here. The share is now identical
across repeated runs, which is what it had no right to be before. **The
conclusion is unchanged; it is now also entitled to be believed.**

The same review noted that the spatial arm approximates 8.4.1.2.2 rather than
implementing it -- a neighbour counts only when both its legs were measured, and
fewer than three survivors fall back to the first rather than to a median
against zero. That is now stated at the site. It is a reason to read the T-vs-S
share as an ordinal signal rather than as a calibrated error estimate, and it
does not rescue a rho of +0.000.

### The next item, and its hard part

`--direct auto`, per slice, on x264's rule: score both modes, keep a running
total with 9/10 decay once it passes the macroblock count, and let the next B
slice take the higher. The scoring code exists and is verified inert.

The hard part is not the score, it is determinism. A running total accumulated
across frames is order-dependent, and our GOP-parallel workers do not encode
frames in slice order, so a decayed cross-frame accumulator would make bits
depend on when a chain finished. That is the one thing the threading design
does not allow, which is why the knob refuses at threads > 1.

The measurement above says decide in the lookahead, per shot. It is already a
deterministic serial stage with a lowres motion field, and deciding before the
first slice is what removes the two-point online-learning loss. Scoping the
accumulator to a GOP is the other option and looks strictly worse: it keeps the
learning cost and adds a warm-up at every GOP boundary.

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

### Both halves were tried, and the exclusion survived

**Step one: bound `mvL1` and lift the exclusion.** `analyze_b_mb` now tests the
derived list-1 vector against the same `stair_mvy_max` the list-1 searches take,
which is the first half. The threaded recon gate then bus-errors on
blue_sky_1080p at t4, t12 and t18, deterministically, and passes with the
exclusion in place. Narrowing by stage named the culprit:

| stage disabled | result |
|---|---|
| `Y264_STAIR_BDEPTH=0` | passes |
| `Y264_STAIR_DEPTH=0` | passes |
| `Y264_STAIR_WIDE=0` | still faults |

So it is v5 reference-B pipelining specifically: the co-located picture is then
a still-streaming reference B whose colmv field is read before
`stair_dpb_commit_content` lands.

**Step two: exclude only BDEPTH.** Recon-match passes at t4/t12/t18 on three
clips including blue_sky_1080p, and the wall price falls from 1.40x to 1.05x.
That looked like the answer and it is not. `scripts/stair_determ.sh` reads
**14/32** with the staircase on against 32/32 with it off. The bitstream is not
reproducible run to run at a fixed thread count, which recon-match structurally
cannot see, because each run decodes to whatever that run built. **The 1.05x was
measured on an encoder that was a different encoder each time.**

The exclusion is therefore not narrowable by stage, and `Y264_STAIR_TDIR` is an
experiment switch over a known-nondeterministic path rather than a feature. What
it waits on is a leaf-side wait on the co-located field's commit, which
`refb_done` already sequences for the next anchor and nothing sequences for the
leaves.

That is the same root cause as the per-macroblock gate's thread restriction
above, reached from the other direction, and fixing it once fixes both.

### What the staircase is worth in wall, which is why any of this matters

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

## 2026-09-01: the base-path thread, first screen

The brief's second, untouched thread was that riverbed and crowd_run sit 3-4%
behind x264 **without** B-frames, which would make it a base-path question. A
first screen says the premise is band-specific and does not survive as stated.

Both encoders at `--bframes 0`, CRF points 34-46, 150 frames, VMAF-NEG:

| arm (ours; x264 fixed at medium) | riverbed | crowd_run |
|---|--:|--:|
| control | +1.02% | -2.20% |
| `--ref 1` | +1.97% | **-4.06%** |
| `aq-strength 0` | **-2.35%** | +0.10% |
| mb-tree off | +1.86% | +2.56% |
| psy-rd off | +0.98% | -1.93% |
| `--preset veryslow` | +8.66% | +12.94% |

**The control does not reproduce the +3.09% / +3.79% the brief records.** It
reads +1.02% and **-2.20%**, ie we are ahead on crowd_run. Points 34-46 sit far
below these clips' calibrated operating points (12500 and 22000 kbit/s), so this
is a low-rate band for them and the brief's figures are a different band. That
is the third time in two days a 1080p claim has turned out to be band-specific,
and it is the same lesson each time: for these clips, anchor to the calibrated
rate or say which band.

**This is a SCREEN, not a result.** Every arm here changes only our side, so at
a matched CRF number it also moves where our ladder sits, and mb-tree and
aq-strength are both known to translate the CRF axis. Anything below has to be
re-read with `scripts/bd_at_rate.py` before it is believed. In particular the
`--preset veryslow` row is almost certainly ladder placement rather than a real
regression: a slower preset producing 8-13% worse BD is not a plausible
encoder result, and it is exactly the shape a shifted operating point makes.

What survives as worth pursuing:

**aq-strength splits two clips by content.** Turning AQ off is worth 3.4
points on riverbed and costs 2.3 on crowd_run. Water is textured everywhere, so
there is nothing for a variance-based quantizer to redistribute toward and it
mostly misallocates; a dense crowd has real flat regions. That is a content
signal of the same shape as the direct-mode one, and it should be priced at
matched rate before anything is built on it.

**`--ref 1` is worth 1.9 points on crowd_run**, ie fewer references beat more on
dense incoherent motion, which is the opposite of the usual direction and worth
understanding rather than exploiting immediately.

`scripts/direct_rate_table.py` now takes `DRT_EXTRA` and `DRT_ARMS` so the
rate-anchored version of this table can be run without a second copy of the
solve.
