# M4 per-content selection: method, and the feature question

Status 2026-08-30: **step 2 answered, and the answer is negative.** All 239
BVI-AOM sequences are labelled. None of the four candidate features predicts
which arm wins, the best rule buildable from them is a coin flip per sequence,
and the one-shot gate-corpus check on that rule loses. Section 9 has the
numbers. The ceiling is still real; the cheap frame-level features are not the
way to it.

The protocol this obeys is the local training-method note (train/gate separation, gate clips test-only); this
file is the M4-specific instance of it, which the work queue makes mandatory
before any measurement on the training corpus.

## 1. The question

The re-derived arm table (`local/records/m4-rederive-2026-08-29.log`, 12 gate
clips, `band_at_rate` at matched achieved bitrate) shows that **every one of the
six arms is content-split, and none is a mistuned constant.** A perfect
per-clip selector is worth **+1.25% median / +1.33% mean BD-NEG, 10/12 clips
improved by more than 0.3%**.

The two arms that carry most of that envelope pull in opposite directions:

| arm | wins on | loses on |
|---|---|---|
| `Y264_CRF_CPLX=0` | akiyo -2.93, tempete -1.25, samsung -1.09, foreman -0.85 | ducks +4.46, touchdown +1.81, park_joy +1.74 |
| `Y264_MBTREE_STRENGTH=2.0` | akiyo -2.69, stefan -2.56, coastguard -1.93, mobile -1.75, bus -1.24 | touchdown +5.83, ducks +4.76, samsung +2.61 |

**So: what cheap, input-derived feature separates them?** A selector is only
real if the feature is something the lookahead already has before the encode
commits.

## 2. Corpus split, and why it is physical rather than promised

- **Training / fitting: BVI-AOM D tier**, `/Volumes/seagate/media/train-corpus/
  bvi-aom/272p`, 239 sequences, 480x272, 10-bit 4:2:0, 64 frames each, no scene
  cuts. External, published, never used as a gate here.
- **Test: the 12-clip gate corpus, TEST-ONLY, permanently.** Bristol's BVI-DVC
  work measured training-corpus choice moving results by up to 10.3%, so a
  threshold fitted on the gate corpus would be fitted on its own test set. This
  is the single likeliest way for this track to produce a confident wrong
  answer.
- The separation is enforced by the harness, not by intent:
  `scripts/m4_label.py` takes a `--src-dir` of external sources and has no path
  into `tests/corpus`. The gate-corpus read is a separate one-shot, run once,
  at the end, and reported whatever it says.

## 3. Labels

Per sequence, three configurations: the shipped default, `Y264_CRF_CPLX=0`, and
`Y264_MBTREE_STRENGTH=2.0`.

**Matched achieved rate, never matched CRF.** Both arms move the CRF-to-rate
mapping, so a matched-CRF `bdcompare` measures ladder placement instead of the
encoder -- the error that invalidated the entire 08-22/25 M4 table and made
`CRF_PBSCALE=2.0` read "-1.5% wins everywhere" on the same build where matched
rate read +0.15 / ducks +9.98. The baseline's three CRF points (24, 30, 36)
define three byte targets; each arm is binary-searched onto those same targets;
BD-NEG is taken over the resulting (bytes, VMAF-NEG) pairs.

**The label has three classes, not two.** `default` is a real class: an arm
only takes the label when it beats the default by more than 0.30% BD, the same
threshold the ceiling table used. This matters because a forced two-way choice
is destructive -- see section 5.

## 4. Features

The candidate set is deliberately restricted to signals the encoder **already
computes per frame, from the source only**, so a selector costs nothing to run
and stays deterministic at any thread count:

| feature | what it is | site |
|---|---|---|
| `tdiff` | uncompensated lowres per-pixel abs temporal difference, x256, EWMA in arrival order | `lr_tdiff_ewma`, in `encode_frame_core` |
| `flat` | share of 16x16 luma MBs with variance < 25 | psy lattice, the `var16x16` sweep in `build_slice_prep` |
| `tex` | share of textured MBs, the complement class | same |
| `motion` | mean lowres MV magnitude per block, x64 | `frame_motion_score` |

`tdiff` is **per pixel and resolution-normalised by construction**, and its own
comment records why it is uncompensated: MV magnitude reads flat clips as fast
(sintel) and clean pans as slow (mobile), and both misclassify. `flat` already
has a shipped precedent as a content gate -- the psy flat gate fires 120/120 on
sintel and 0/120 on every other corpus clip -- which is the existence proof
that a frame-level source feature can carry a per-content decision here.

Dumped by `Y264_PSY_FLAT_LOG=1` and `Y264_ADME_LOG=1`; medians over the
sequence; collected by `scripts/m4_label.py` in the same pass as the labels.

## 5. The gate-corpus read is a HYPOTHESIS, not a fit

Run once on the 12 gate clips, 120 frames each, crf 26 (medians):

| clip | tdiff | flat | tex | motion | best arm |
|---|--:|--:|--:|--:|---|
| akiyo_cif | 131 | 34 | 50 | 0 | CRF_CPLX=0 |
| tempete_cif | 1166 | 6 | 91 | 25 | CRF_CPLX=0 |
| foreman_cif | 1202 | 18 | 69 | 90 | CRF_CPLX=0 |
| samsung_720p | 1320 | 54 | 32 | 201 | CRF_CPLX=0 |
| coastguard_cif | 1948 | 0 | 92 | 80 | MBTREE_STRENGTH=2.0 |
| mobile_cif | 2302 | 3 | 95 | 21 | MBTREE_STRENGTH=2.0 |
| stefan_cif | 3794 | 18 | 79 | 404 | MBTREE_STRENGTH=2.0 |
| bus_cif | 4508 | 2 | 88 | 661 | MBTREE_STRENGTH=2.0 |
| sintel_720p | 268 | 90 | 6 | 675 | (RDOQ_SEED64) |
| touchdown_420 | 975 | 21 | 28 | 49 | (default) |
| ducks_720p | 1423 | 0 | 95 | 32 | (default) |
| park_joy_720p | 3726 | 6 | 85 | 578 | (default) |

**`tdiff` orders the eight arm-winning clips perfectly**, with a 1.48x gap
between the highest CRF_CPLX clip (samsung 1320) and the lowest
MBTREE_STRENGTH clip (coastguard 1948), and it does so ACROSS resolutions --
samsung is 720p and sits with the CIF clips. None of the other three features
separates: `motion` puts mobile (21) and coastguard (80) inside the
CRF_CPLX range, `tex` puts tempete (91) inside the MBTREE range, and `flat`
ties stefan and foreman at 18.

**But the four abstain clips break a two-way rule.** Applying "low tdiff ->
CRF_CPLX, high tdiff -> MBTREE" to all twelve costs sintel +1.05, touchdown
+1.81, park_joy +1.36 and ducks about +4.5 -- because both arms lose there and
the right answer is the default. `tdiff` says which arm, and says nothing about
whether to deviate at all. **The abstain decision, not the arm choice, is the
open half of the selector, and it is what the +1.25% ceiling assumes.**

Eight labelled points and a hand-read threshold is an overfit waiting to
happen, which is the whole reason the training corpus exists. Nothing above is
a fitted constant, and no threshold from this table may ship.

## 6. What the training run has to answer

1. Does `tdiff` separate the two arms on 239 externally-sourced sequences, or
   was the gate-corpus ordering luck at n=8?
2. **What predicts abstain?** The training rows carry both arms' BD margins
   against the default, so the abstain band is measurable rather than assumed.
3. Where is the threshold, fitted on BVI-AOM only, and how wide is the
   indifference region around it?

Known distribution risk, recorded before the run rather than after: BVI-AOM's
480x272 tier is Lanczos-downsampled from 4K, which smooths exactly the signal
`tdiff` measures. The first two labelled sequences read tdiff 58 and 252
against a gate-corpus range of 131-4508, so **the training distribution may not
span the test range**. If it does not, the honest outcome is a threshold fitted
on a compressed axis with its extrapolation stated, or a switch to the C tier
(960x544) -- not a quiet rescale.

## 7. Provenance header, when anything is fitted

Mandatory next to whatever ships, per the protocol:

```
/* FITTED SELECTOR -- see docs/m4-selector-method.md
 * trained:      <date>, encoder commit <sha>
 * train:        BVI-AOM D tier, <N> sequences (local/records/m4-bviaom-*.jsonl)
 * test:         12-clip gate corpus, one shot, <results doc>
 * labels:       BD-NEG at matched achieved rate, 3 targets, abstain < 0.30%
 * features:     tdiff (+ whatever survives)
 * ceiling:      +1.25% median perfect selector; achieved <x>
 * REFIT IF:     the CRF complexity term, mb-tree strength/consumption, the AQ
 *               default, or the lowres downscale changes
 */
```

## 8. What this doc does NOT decide

The runtime shape -- per-clip vs per-shot, where the decision is taken in the
lookahead, how it interacts with the shot-based plan, and whether it is exposed
as a tune or applied silently -- is differentiator design with no ground-truth
gate, which `CLAUDE.md` reserves for the frontier tier. This doc's job is to
make sure that when that design happens it is standing on measured features and
honest labels.

## 9. Results, 2026-08-30: the features do not predict

239 sequences labelled, `local/records/m4-bviaom-2026-08-30.jsonl`, about 7
seconds each.

**The envelope survives on external content, and it is concentrated.** A
perfect per-sequence choice among the two arms is worth **+1.43% mean**, but
only **+0.16% median**, and the top 10% of sequences hold 62% of the total.
Most content has nothing to win here. 127 of 239 sequences want the shipped
default by the 0.30% rule.

**Deviating blindly is a loss.** Always `CRF_CPLX=0` reads -0.52% mean with a
worst sequence at -19.05%; always `MBTREE_STRENGTH=2.0` reads -0.55% mean,
worst -17.39%. The default is a strong baseline and beats either arm applied
everywhere.

**`tdiff` does not predict the arm.** Rank correlation between `tdiff` and the
arm preference (BD difference between the two arms) is **+0.027**. The two
label groups' ranges sit on top of each other: 64-7294 against 50-6414. The
perfect 8-of-8 ordering on the gate corpus was luck at n=8, which is what a
training corpus is for.

| feature | rank correlation with arm preference |
|---|--:|
| `tex` | +0.361 |
| `flat` | -0.360 |
| `motion` | -0.146 |
| `tdiff` | +0.027 |

**`flat` and `tex` carry a weak signal that does not survive contact with the
per-sequence result.** They are complements, so that is one signal, not two.
Fitting a threshold on half the corpus and scoring it on the other half, the
best rule is `flat < 1 -> MBTREE_STRENGTH=2.0, else default`, collecting
**+0.29% held-out** against the +1.43% ceiling. Its shape is the problem: it
fires on 29 of 239 sequences, and on those it reads **mean +3.16% but median
-0.06%, with 14 wins and 12 losses**. The mean is a few large winners, and per
sequence it is a coin flip. That is a high-variance population, not a
prediction.

**The one-shot gate-corpus check, now spent.** That rule fires on exactly two
of the twelve gate clips, coastguard (+1.93) and ducks (-4.76), for **-0.24%
mean across the corpus**. It loses. Since this result is now known, no
threshold may be re-tuned against it; a later rule needs its own validation
target.

**The distribution risk did not materialise.** BVI-AOM's `tdiff` spans
50-7294 with a median of 959, against the gate corpus's 131-4508, so the
Lanczos downsampling did not compress the axis out of range. The concern is
retired, and the negative result is about prediction rather than coverage.

### What this closes and what it leaves open

Closed: **per-clip selection driven by these four frame-level features.** Do
not re-fit `tdiff`, `flat`, `tex` or `motion` against these two arms.

Open, and unchanged in size: the +1.43% mean envelope on external content.
What the round adds is a shape for it. The prize is concentrated in a minority
of sequences, the majority want the default, and blind deviation is expensive,
so any future selector has to be precise about a small population rather than
broadly right about a large one. Frame-level source statistics are too coarse
for that. The candidates that remain are signals taken after some encoding has
happened, which the lookahead already produces per frame and this round never
looked at, and the per-shot form the shot-based plan implies. Both are design
questions rather than sweeps.
