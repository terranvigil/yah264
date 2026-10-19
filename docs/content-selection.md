# Content selection

Many of yah264's settings are content-split. The same constant helps one clip
and hurts another by several points. Per-content selection means picking those
settings from what the encoder can see of the source, before it commits. We
ship it where a cheap feature separates the winners cleanly and nowhere else.
A general per-clip selector over the content-split constants has been tried
and doesn't work with frame-level features; the prize is real but sits in a
small minority of sequences.

All numbers are BD-rate on VMAF-NEG (negative is better), against the shipped
default at matched achieved bitrate unless noted.

## What ships

Every shipped selection is a gate on a source feature the encoder already
computes, decided per frame or per slice, and deterministic at any thread
count. None is a per-clip classifier.

| gate | feature | what it switches | default |
|---|---|---|---|
| `--direct auto` | per B slice, the running count of macroblocks each direct derivation would have made skippable | spatial or temporal direct | on. CRF band median 0.0%, station2 -30%, stockholm -7%, bus -4%; worst sunflower +0.9 to +1.4%; wall 1.01-1.05x at 12 threads |
| psy flat gate | share of 16x16 luma macroblocks with variance under 25 | raises the psy-trellis floor on frames that are at least 75% flat | on. Fires on every sintel frame and on no frame of any other corpus clip: sintel -5.6% to -9.0% |
| per-MB lambda | the same variance test, per macroblock | mode decision follows mb-tree's QP on non-flat macroblocks only | on. See [animation.md](animation.md) |
| `--nr` noise gate | spatial Laplacian percentile | coefficient noise reduction on or off per picture | gate armed whenever `--nr` is on |
| `--mcstf` noise gate | temporal match-error percentile | temporal pre-filter on or off per picture | gate armed whenever `--mcstf` is on; see [denoise.md](denoise.md) |

## What the evidence says

**The ceiling is real but concentrated.** Over two content-split arms that hold
most of the envelope, `Y264_CRF_CPLX=0` (no complexity term in CRF) and
`Y264_MBTREE_STRENGTH=2.0` (stronger mb-tree than the shipped 1.4), a perfect
per-sequence choice is worth:

| corpus | mean | median |
|---|--:|--:|
| 12-clip gate corpus | -1.33% | -1.25% |
| 239 BVI-AOM sequences (external, 480x272) | -1.43% | -0.16% |

On external content the top 10% of sequences hold 62% of the gain, and 127 of
239 want the default.

**Deviating blindly is a loss.** Applying either arm everywhere reads +0.52%
(`CRF_CPLX=0`) and +0.55% (`MBTREE_STRENGTH=2.0`) mean, with single sequences
losing 17-19%. The default beats both.

**Frame-level source features don't predict the winner.** Rank correlation with
arm preference over the 239 labeled sequences:

| feature | what it is | correlation |
|---|---|--:|
| `tex` | share of textured macroblocks | +0.361 |
| `flat` | share of near-flat macroblocks | -0.360 |
| `motion` | mean lowres MV magnitude | -0.146 |
| `tdiff` | uncompensated lowres temporal difference per pixel | +0.027 |

`tdiff` ordered the eight arm-winning gate clips perfectly, and that was luck at
n=8. The best rule from `flat`/`tex` (`flat < 1` picks the stronger mb-tree)
scores -0.29% held out against a -1.43% ceiling. On the 29 sequences it fires
on it wins 14 and loses 12, so it's a coin flip with a few big winners. On the
gate corpus it fires on coastguard and ducks and loses overall (+0.24% mean).

**Other content-split constants** that point the same way, each refused as a
global default because it's narrow:

| constant | split |
|---|---|
| a larger B-frame QP step (2.0) | sita -2.6%, ducks +10.0%, touchdown +6.1% |
| `--aq-strength 0` | sintel -6.9%, bbb +6.8% |
| `--aq-strength 0.2` / `0.6` | riverbed -0.84 at 0.2, blue_sky -0.63 at 0.6: opposite directions |

## What didn't work

| idea | why not |
|---|---|
| per-clip selection from `tdiff`, `flat`, `tex` or `motion` against the two arms above | no predictive signal on 239 external sequences; don't refit these four |
| a two-way rule (always pick one arm) | the four clips that want the default lose 1-4.5% each; the abstain decision is most of the problem |
| fitting a threshold on the gate corpus | it's the test set. The one-shot gate check on the BVI-AOM rule is already spent, so no threshold may be tuned against it |
| a QP-gated escape for the B-step split | coastguard loses at its own high-QP rungs: the split is content, not rate regime |
| per-class psy-rd (1.0 or 3.0) | within ±0.3 everywhere; the default holds |

## Open questions

- **Which signals do predict?** Two candidates are untested. One is signals
  taken after some encoding has happened, which the lookahead already produces
  per frame. The other is the per-shot form, decided at each shot instead of
  per clip (see [shot-based.md](shot-based.md)). A future selector has to be
  precise about a small population; being broadly right about a large one
  won't pay.
- **The runtime shape.** Per clip or per shot, where in the lookahead the
  decision is taken, and whether it's a tune or applied silently. None of
  that is decided.
- **The model form, if one is fitted.** A small decision tree compiled to an
  integer C table: no runtime dependency, deterministic at any thread count.
  Per-macroblock neural nets are out on cost (the budget is 100-300 ns per
  macroblock).
- **Related fitted screens** with better-posed labels than content selection:
  the 4x4/8x8 transform choice (an exact oracle reads -1.58%
  median against the shipped heuristic's -1.30%), and calibrating the bit
  estimate against real CABAC bits (P_Skip is priced at a flat 1 bit and the
  intra mode bonus at a flat 4, both lambda-blind).
- **Re-sweeping stale constants** comes before any fit, since a model fitted on
  top of a stale constant inherits it. Candidates include the RDOQ seed, the
  intra admission margins and the 0.7 mb-tree strength scale, which was fitted
  against a coarser lowres search than the one that ships.

## Measuring it

- **Training data never comes from the gate corpus.** Fit on an external set
  (BVI-AOM's 480x272 tier, `/Volumes/seagate/media/train-corpus/bvi-aom/272p`,
  239 sequences, 64 frames, no cuts) and read the 12 gate clips once, at the
  end. `scripts/m4_label.py` takes only a `--src-dir` of external sources and
  has no path into `tests/corpus`.
- **Label at matched achieved rate, never matched CRF.** Both arms move the
  CRF-to-rate mapping, so a matched-CRF comparison measures ladder placement.
  The labeler binary-searches each arm onto the default's byte targets at CRF
  24, 30 and 36.
- **Three classes, not two.** An arm earns the label only when it beats the
  default by more than 0.30%; otherwise the label is `default`.
- `Y264_PSY_FLAT_LOG=1` and `Y264_ADME_LOG=1` dump the frame features.
- Any fitted table carries a header naming its training set, labels, features,
  the ceiling and what it achieved, and a `REFIT IF:` line listing the encoder
  changes that invalidate it.
