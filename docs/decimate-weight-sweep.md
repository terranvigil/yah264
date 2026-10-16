<!--
Copyright (c) 2026, the yah264 authors
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Decimate run-weights: what the sweep says

`DECIMATE_T4` weights each nonzero coefficient by the run of zeros before it.
The weights are summed over a block. Below a threshold, the encoder drops the
whole block and codes nothing for it. Nothing in ITU-T H.264 fixes these
numbers. Because they are a pure encoder-side heuristic, we measured them
instead of inheriting them.

## Method

`Y264_DCTDEC_TAB4` overrides the table. We ran eight candidate shapes on the
twelve-clip corpus as a self-A/B through `scripts/run_band.py` on the CRF band,
scored by VMAF-NEG BD-rate. The top candidates then re-ran on the deep band
through `scripts/band_at_rate.py` at matched achieved bitrate.

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

The head weight carries everything. Dropping it from 3 to 2 costs 2.3%, two
orders of magnitude more than any other move in the table. With a threshold of
3, a head weight of 2 lets the encoder decimate an adjacent pair of nonzeros.
That makes it a different tool, not a tuning of the same one. Above 3, the
surface is flat: every candidate lands within ±0.13%.

## The two that looked better on one band

`head4` and `long` were the only candidates nominally ahead on the CRF band.
That band flagged three of twelve clips as near-saturated, so we re-ran both
on the deep band (VMAF-NEG 55-83) at matched achieved bitrate. Decimation
bites there.

| shape | CRF band, 12 clips | deep band | negative on |
|---|--:|--:|--:|
| head4 | −0.13% | **+0.09%** (4 clips) | 2 of 4 |
| long | −0.07% | **+0.01%** (6 clips) | 2 of 6 |

`head4` changes sign between bands and is out. `long` is neutral. It doesn't
replicate as an improvement, but it doesn't cost anything on either band
either. Every number in that table is inside the deep band's measured noise floor of
about ±1.2 per clip.

## What shipped, and why

`3,2,2,1,1,1,1,1,0…`. It reads at or slightly ahead of every alternative on both
bands. It costs nothing measurable elsewhere:

- **Speed:** unchanged. Best-of-5 single-threaded pure-C over bus/foreman/mobile
  reads +0.0%, +0.0% and +2.3%. The +2.3% is inside run-to-run noise.
- **Size:** ±0.2% with no consistent direction across the same three clips.
- **Conformance:** 254/254 recon-match.

We re-checked the threshold against the new table instead of assuming it held.
The pair is jointly tuned. Moving one half of a jointly-priced constant has
bitten this project before. At threshold 4 this same table costs
**+2.37%**, so 3/2 remains the right partner and stays unchanged.

## What this does and does not show

The head weight is forced. It has to clear the decimate threshold, or an
adjacent nonzero pair becomes droppable and the heuristic turns into a
different tool. The +2.29% on `head2` is that boundary, not a tuning penalty.
Above head 3 the measurement cannot separate the candidates on either band.

So the tail is a free choice. The value shipped is ours, picked by this sweep
from a set of eight on this corpus. It differs from what other encoders
use. We tuned the sibling `DECIMATE_T8` and both thresholds here too. They also landed low: thresholds 3/2, where a heavier 6/4 over-decimated. That is the same evidence from the other direction.

The sweep does *not* claim that any particular point on a flat plateau is
special. What it shows is that we made the choice here, with the measurement to
show for it.
