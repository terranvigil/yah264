# Content selection

Many of yah264's settings are content-split. The same constant helps one clip
and hurts another by several points. Per-content selection means picking those
settings from what the encoder can see of the source, before it commits. We
ship it where a cheap feature separates the winners cleanly and nowhere else.
A general per-clip selector over the content-split constants doesn't work with
frame-level features. The prize is real, although a small minority of
sequences holds it.

All numbers are BD-rate on VMAF v1 against the shipped default, where
negative is better. The BVI-AOM labels and everything derived from them are on
VMAF-NEG (earlier metric), and the page says so where they appear. Rows differ
in how the two sides are matched:

- At matched achieved bitrate: the gate-corpus ceiling, the `flat < 1` read
  and the psy flat gate.
- At shared CRF points: `--direct auto` at per-clip CRF sets and two threads,
  and the AQ and psy-rd rows on CRF ladders.

## What ships

Each shipped selection is a gate on a source feature the encoder already
computes. The gate decides per frame or per slice and is deterministic at any
thread count. None is a per-clip classifier.

| gate | feature | what it switches | default |
|---|---|---|---|
| `--direct auto` | per B slice, the running count of macroblocks each direct derivation would have made skippable | spatial or temporal direct | on. Median 0.0% against spatial on 14 clips, station2 -32%, stockholm -12%, bus -1.7%; worst sunflower +3.3%; wall 1.01-1.05x at 12 threads |
| psy flat gate | share of 16x16 luma macroblocks with variance under 25 | raises the psy-trellis floor on frames that are at least 75% flat | on. Fires on every sintel frame and on no frame of any other corpus clip. On VMAF v1 it reads neutral on sintel, within 0.3% |
| per-MB lambda | the same variance test, per macroblock | mode decision follows mb-tree's QP on non-flat macroblocks only | on. See [animation.md](animation.md) |
| `--nr` noise gate | spatial Laplacian percentile | coefficient noise reduction on or off per picture | gate armed whenever `--nr` is on |
| `--mcstf` noise gate | temporal match-error percentile | temporal pre-filter on or off per picture | gate armed whenever `--mcstf` is on; see [denoise.md](denoise.md) |

## What the evidence says

The ceiling is concentrated, and on VMAF v1 it's small on our own corpus. Two
content-split arms hold most of the envelope. `Y264_CRF_CPLX=0` drops the
complexity term from CRF. `Y264_MBTREE_STRENGTH=2.0` runs a stronger mb-tree
than the shipped 1.4. Over those two arms, a perfect per-sequence choice is
worth:

| corpus | mean | median | sequences that want the default | gain held by the top 10% |
|---|--:|--:|--:|--:|
| 12-clip gate corpus | -0.29% | -0.02% | 6 of 12 | |
| 239 BVI-AOM sequences (external, 480x272; VMAF-NEG) | -1.43% | -0.16% | 127 of 239 | 62% |

Deviating blindly is a loss. The default beats both arms:

| arm applied to every clip | gate corpus mean |
|---|--:|
| `CRF_CPLX=0` | +1.20% |
| `MBTREE_STRENGTH=2.0` | +3.85% |

Single gate clips lose up to 8.6%. The BVI-AOM labels on the earlier metric
say the same, with single sequences losing 17-19%.

Frame-level source features don't predict the winner. Here is the rank
correlation with arm preference over the 239 labeled sequences, on VMAF-NEG:

| feature | what it is | correlation |
|---|---|--:|
| `tex` | share of textured macroblocks | +0.361 |
| `flat` | share of near-flat macroblocks | -0.360 |
| `motion` | mean lowres MV magnitude | -0.146 |
| `tdiff` | uncompensated lowres temporal difference per pixel | +0.027 |

`tdiff` ordered the eight arm-winning gate clips perfectly, which at that
sample size was luck. The best rule from `flat` and `tex` picks the stronger
mb-tree when `flat < 1`. It's a coin flip with a few big winners:

| where the `flat < 1` rule was read | result |
|---|---|
| held out on the BVI-AOM labels (VMAF-NEG) | -0.29% against a -1.43% ceiling |
| the 29 BVI-AOM sequences it fires on | wins 14, loses 12 |
| the gate corpus (VMAF v1) | fires on coastguard and ducks. +0.25% mean over the twelve clips, because ducks loses more than coastguard gains |

Other content-split constants point the same way. We refused each as a global
default because it's narrow:

| constant | split |
|---|---|
| a larger B-frame QP step of 2.0 | sita gains, ducks and touchdown lose more |
| `--aq-strength 0` | sintel -3.4%, bbb +10.6% |
| `--aq-strength 0.2` / `0.6` | riverbed -1.47 at 0.2, blue_sky -7.09 at 0.6, where 0.2 costs blue_sky +6.72: opposite directions |

## What didn't work

| idea | why not |
|---|---|
| per-clip selection from `tdiff`, `flat`, `tex` or `motion` against the two arms above | no predictive signal on 239 external sequences; don't refit these four |
| a two-way rule (always pick one arm) | the six gate clips that want the default lose 1.2-4.8% each, even on their better arm; the abstain decision is most of the problem |
| fitting a threshold on the gate corpus | it's the test set. The one-shot gate check on the BVI-AOM rule is already spent, so no threshold may be tuned against it |
| a QP-gated escape for the B-step split | coastguard loses at its own high-QP rungs. The split follows content, not rate regime |
| per-class psy-rd at 1.0 or 3.0 | 1.0 is within ±0.2 on sita, riverbed, crowd_run and blue_sky; 3.0 costs blue_sky 2.1%. The default holds |

## Open questions

We don't know which signals do predict. Two candidates are untested. One is
signals taken after some encoding has happened, which the lookahead already
produces per frame. The other is the per-shot form, decided at each shot
instead of per clip, which [shot-based encoding](shot-based.md) covers. A
future selector has to be precise about the small population that holds the
gain.

The runtime design isn't decided either: per clip or per shot, where in the
lookahead the decision is taken, and whether it's a tune or applied silently.

If we fit a model, it would be a small decision tree compiled to an integer C
table. That has no runtime dependency and is deterministic at any thread
count. Per-macroblock neural nets are out on cost, since the budget is 100-300
ns per macroblock.

Two related fitted screens have better-posed labels than content selection.
One is the 4x4/8x8 transform choice, where an exact oracle beat the shipped
heuristic on the earlier metric. The other calibrates the bit estimate
against real CABAC bits. P_Skip is priced at a flat 1 bit and the intra mode
bonus at a flat 4. Neither looks at lambda.

Re-sweeping stale constants comes before any fit, because a model fitted on
top of a stale constant inherits it. Candidates include the RDOQ seed, the
intra admission margins and the 0.7 mb-tree strength scale. That scale was
fitted against a coarser lowres search than the one that ships.

## Measuring it

- **Training data never comes from the gate corpus.** Fit on an external set
  and read the 12 gate clips once, at the end. We use BVI-AOM's 480x272 tier
  in `/Volumes/seagate/media/train-corpus/bvi-aom/272p`: 239 sequences of 64
  frames with no scene changes. `scripts/m4_label.py` takes only a `--src-dir` of external sources and
  has no path into `tests/corpus`.
- **Label at matched achieved rate, never matched CRF.** Both arms move the
  CRF-to-rate mapping, so a matched-CRF comparison would measure ladder placement.
  The labeler binary-searches each arm onto the default's byte targets at CRF
  24, 30 and 36.
- **Label three classes.** An arm earns the label only when it beats the
  default by more than 0.30%; otherwise the label is `default`.
- `Y264_PSY_FLAT_LOG=1` and `Y264_ADME_LOG=1` dump the frame features in a
  dev_knobs build.
- Any fitted table has a header naming its training set, labels, features,
  the ceiling and what it achieved, and a `REFIT IF:` line that lists the
  encoder changes that invalidate it.
