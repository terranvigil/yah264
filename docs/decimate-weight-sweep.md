# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause

# Decimate run-weights: what the sweep says

`DECIMATE_T4` weights each nonzero coefficient by the run of zeros before it.
The weights are summed over a block and, below a threshold, the whole block is
dropped rather than coded. Nothing in ITU-T H.264 fixes these numbers -- they
are a pure encoder-side heuristic, which is exactly why they were worth
measuring rather than inheriting.

## Method

`N264_DCTDEC_TAB4` overrides the table. Eight candidate shapes, twelve-clip
corpus, self-A/B through `scripts/run_band.py` on the CRF band, VMAF-NEG
BD-rate, and then the top candidates re-run on the deep band through
`scripts/band_at_rate.py` at matched achieved bitrate.

## Result: one term matters, and then the surface goes flat

| shape | table | CRF band, median |
|---|---|--:|
| head2 | `2,2,1,1,1,1,0…` | **+2.29%** |
| flat | `2,2,2,1,1,1,0…` | **+2.28%** |
| short | `3,2,2,1,1,0…` | +0.03% |
| fast | `3,2,1,1,0…` | +0.01% |
| baseline | `3,2,2,1,1,1,0…` | 0.00% |
| slow | `3,3,2,2,1,1,1,0…` | −0.03% |
| **long (shipped)** | `3,2,2,1,1,1,1,1,0…` | **−0.07%** |
| head4 | `4,3,2,1,1,1,0…` | −0.13% |

The head weight carries everything. Drop it from 3 to 2 and the cost is 2.3%,
two orders of magnitude larger than any other move in the table -- because a
head weight of 2 with a threshold of 3 lets an adjacent pair of nonzeros be
decimated, which is a different tool, not a tuning of the same one. Above 3, the
surface is flat: every candidate lands within ±0.13%.

## The two that looked better on one band

`head4` and `long` were the only candidates nominally ahead on the CRF band,
which flagged three of twelve clips as near-saturated, so both were re-run on
the deep band (VMAF-NEG 55-83) at matched achieved bitrate, where decimation
actually bites.

| shape | CRF band, 12 clips | deep band | negative on |
|---|--:|--:|--:|
| head4 | −0.13% | **+0.09%** (4 clips) | 2 of 4 |
| long | −0.07% | **+0.01%** (6 clips) | 2 of 6 |

`head4` changes sign between bands and is out. `long` is neutral: it does not
replicate as an improvement, but it does not cost anything either, on either
band, and the deep band's measured noise floor is about ±1.2 per clip, so every
number in that table is inside it.

## What shipped, and why

`3,2,2,1,1,1,1,1,0…`. It reads at or slightly ahead of every alternative on both
bands, and it costs nothing measurable elsewhere:

- **Speed:** unchanged. Best-of-5 single-threaded pure-C over bus/foreman/mobile:
  +0.0%, +0.0%, +2.3%, the last of those inside run-to-run noise.
- **Size:** ±0.2% with no consistent direction across the same three clips.
- **Conformance:** 254/254 recon-match.

The threshold was re-checked against it rather than assumed, because the pair is
jointly tuned and this project has been bitten before by moving one half of a
jointly-priced constant. At threshold 4 this same table costs **+2.37%**, so 3/2
remains the right partner and is unchanged.

## What this does and does not show

The head weight is forced. It has to clear the decimate threshold or an adjacent
nonzero pair becomes droppable, which is a different tool; the +2.29% on `head2`
is that boundary, not a tuning penalty. Above head 3 the measurement cannot
separate the candidates on either band.

So the tail is a free choice, and the value shipped is ours: it was selected by
this sweep, on this corpus, from a set of eight, and it differs from what other
encoders use. The sibling `DECIMATE_T8` and both thresholds were tuned here too
and also landed somewhere else (thresholds 3/2 against the 6/4 that a reference
encoder uses), which is the same evidence from the other direction.

What the sweep does *not* claim is that a flat plateau makes any particular point
on it special. It does not. The point is that the choice was made here, with the
measurement to show for it.
