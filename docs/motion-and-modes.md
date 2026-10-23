# Motion search and mode decision

This is the per-macroblock inner loop: find motion vectors, pick a partition
shape and a mode (skip, inter or intra), quantize the residual and decide
which coefficients survive. Most of the encoder's operation count lives here,
mainly as SAD and SATD probes during motion search, so most speed work starts
here too.

The preset sets the effort. Medium is `--subme 7` and `--subpel 2` with three
reference frames, and everything below `--subme 9` takes the fast path: SATD
picks the candidates and full RD runs only on survivors. `--subme 9` and up
run full RD on every partition and keep RDOQ in every trial. The rest of this
page describes medium unless it says otherwise. B-frame specifics (direct
mode, B_8x8, the B skip decision) are in [b-frames.md](b-frames.md).

## How it works

### Integer search

Each search starts from a batch of candidates: zero, the median predictor,
the spatial neighbors' vectors and, at medium, the lowres motion field from
the lookahead. They're scored together and the best one seeds the pattern.

Below `--subme 8`, the pattern is a hexagon iterated to convergence, then a
terminal 8-point square. The square catches minima that sit diagonally off a
hex vertex. We call this set (rich seeds, the lowres field, the square, and
rounding seeds to the nearest whole pel) the hex-parity features. They're on
exactly when hex is the primary search: `--me hex` or the auto tiers at medium
and faster. On that path medium reads -0.48% BD-rate VMAF-NEG against x264
medium.

At `--subme 8` and up, or with `--me umh`, the uneven-cross and multi-hexagon
grid (UMH) runs after the hex, out to `--merange` (default 16). It never runs
on 8x8 or smaller blocks. Those shapes are seeded from their parent's refined
vector, so the wide grid has little left to find there. With no `--me`,
`--subme` picks the method, so `--subme 8` changes the search algorithm and
not just the effort. Pass `--me` to pin it.

Three early-outs trim the integer stage without touching sub-pel:

- **Seed early-out**: when the best seed
  already costs at most K per pixel (SAD plus vector rate), the hex, UMH and
  finish stages are skipped. K is 48 for sub-16x16 shapes and 4 for 16x16.
  A 16x16 starts from a median predictor and needs a near-perfect seed before
  it's safe to stop.
- **Neighbor agreement** (`Y264_ME_AGREE`, pictures 1920 wide and up): when
  every seed collapses onto the predictor, the search takes one hex step and
  one finish step.
- **Small-block UMH skip**, described above.

### Sub-pel refinement

Medium runs the capped diamond (`--subpel 2`): a 4-point diamond, two
iterations per level, half-pel scored in SAD and quarter-pel in SATD. Slow
and up run the 8-neighbor square to convergence in SATD at both levels.

Two rules decide who gets quarter-pel at all. At `--subme 7` and below, a
candidate whose half-pel cost times 7/8 already exceeds the best cost seen in
this macroblock skips it. Only near-winners pay. And when the half-pel winner
is still the integer vector and costs at most 96 x lambda x area / 256
(`Y264_QPEL_SKIPQ`), quarter-pel is skipped because the quantizer
would discard the residual it could improve. On pictures 1920 wide and up,
the B list-0 reference loop searches each reference to half-pel and refines
only the winning reference at quarter-pel.

### P-macroblock decision

In order, at `--subme 8` and below:

1. **Fast P-skip probe.** If the skip prediction's residual quantizes to
   nothing, commit skip before any search. `--no-fast-pskip` turns it off.
2. **16x16 search** over the active references.
3. **Ref-0 exit** (pictures of at least 1200
   macroblocks). If the ref-0 16x16 vector lands within one quarter-pel of
   the skip vector and its raw SATD is under 1200 lambdas, the other
   references and every split are skipped. The 16x16 still goes through the
   RD compare against skip and intra.
4. **`--p-part-gate 400`.** If the 16x16 costs under 400 lambdas, no split is
   searched, unless the 3x3 lowres-cost neighborhood is dispersed (a motion or
   texture edge) or the mb-tree offset says other frames lean on this block.
   The gate saturates against those interlocks, so raising the threshold to
   1200 buys almost nothing.
5. **Partition order with early termination** (`Y264_PART_EARLYTERM=4`).
   The 8x8 split is searched next. 16x8 and 8x16 are searched only when the
   8x8's raw SATD beats the 16x16's by less than the vector rate of the two
   8x8 halves a rectangle would merge. That margin is per macroblock: static
   blocks prune hard and real motion keeps its rectangles. Blocks mb-tree
   marks as important keep the rectangles regardless.
6. **SATD picks the shape.** Full RD scores the winner and, if the winner
   isn't 16x16, the 16x16 as insurance against SATD over-splitting at low
   rates. That's one or two RD encodes instead of four.
7. **Intra screen.** Each I_16x16 mode is predicted and scored by SATD with
   no transform. Intra gets a full encode and an RD trial only when that cost
   is under 1.5x the inter winner's SATD (0.75x on B macroblocks).

The transform size is also pre-decided at medium (SA8D against SATD), so only
one size is encoded. `--subme 9` and up encode both and let RD pick.

### RDOQ and trellis placement

`--trellis 1` is the default. At medium, RD trials quantize with the plain
deadzone and only the winning mode is re-encoded with RDOQ before commit.
`--trellis 2` runs RDOQ in every trial, and `--subme 9` and up do that anyway.
Under CABAC, medium's RDOQ is a single reverse Viterbi pass with
transform-domain distortion and running context rate. CAVLC and psy-trellis
blocks use a greedy search instead: single-pass toward-zero at medium,
iterated to convergence at `--subme 9` and up.

RDOQ seeds each coefficient from round-to-nearest, so a marginal coefficient
always enters the candidate set as {q-1, q} and RD decides keep or drop.

Intra blocks on very smooth source get a smaller RDOQ lambda. A gentle
gradient on a flat wall or sky is carried by levels of one or two, and at the
full lambda RDOQ drops them because they cost bits and buy little squared
error. The block then codes to a flat step, which is banding, and in an I
frame over a static background that step stays for the whole GOP. The lambda
falls linearly from full at a source variance of 8 (on the 8-bit scale) to
zero at 2, where RDOQ keeps the nearest levels. `Y264_TREL_SMOOTH=0` turns
it off.

### Coefficient decimation

After quantization, a 4x4 inter block whose levels are all +-1 gets a score:
each nonzero coefficient adds a weight looked up by the run of zeros before
it. Below the threshold the whole block is zeroed. The 4x4 table is
`3,2,2,1,1,1,1,1,0...` with threshold 3. The 8x8 table is
`3,3,3,3,2,2,2,2,1,...` (eight ones, then zeros) with threshold 2. Any level
of magnitude 2 or more forces keep. `--no-dct-decimate` turns it off.

## What we chose and why

| choice | why |
|---|---|
| Hex with the parity features at medium, UMH from `--subme 8` | -0.48% VMAF-NEG against x264 medium without the wide grid's cost |
| No UMH on 8x8 and smaller | about 6-7% faster single-threaded and BD-neutral or better on the motion clips (stefan -0.75%, bus +0.21%) |
| Seed early-out, K 48 sub-16x16 and 4 at 16x16 | 2-3% of wall at 12 threads for a CRF-band median of +0.12% |
| Capped diamond sub-pel at medium | a speed trade. Slower presets keep the square |
| Adaptive partition margin plus importance rescue (mode 4) | the rescue buys back 0.63 points of the margin's 1.32% band cost for 1-3 points of its 4.33% wall saving |
| `--p-part-gate 400` | holds quality on every clip of the 12-clip HD band. It deletes the largest search count in the P tournament |
| Ref-0 exit at K=1200 | against K=600, shields (the worst clip on wall) is 8.4% faster at one thread. The 14-clip CRF band median is +0.16 to +0.26% |
| Intra admission at 1.5x (P) and 0.75x (B) | 1.5x is BD-neutral for 1.06x CIF and 1.11x 720p in pure C. B intra wins a few dozen macroblocks in 300,000, so its margin is tighter |
| RDOQ on the winner only at medium (`--trellis 1`) | 1.18x faster at CIF and 1.29x at 720p in pure C, for +1.42% VMAF-NEG |
| Viterbi RDOQ under CABAC at medium | -0.24% VMAF-NEG on seven clips and 1.1-1.3x faster than the greedy search |
| Intra RDOQ lambda ramped to zero below a source variance of 8 | akiyo's I frame banded (CAMBI 3.1, the reference 0.2) and its static background kept the bands all clip. VMAF v1 BD against x264 medium: akiyo +7.9% to -3.6%, tempete +2.8% to +1.7%. HD band median +0.02%. A limit of 16 cost sunflower and perseverance 0.55%, and I frames only did no better |
| Round-to-nearest RDOQ seed | a deadzone seed drops would-be level-1 coefficients with no RD look. Round-to-nearest measured -2.8% corpus mean, park_joy -6.7%, no clip worse |
| Decimate head weight 3, thresholds 3 and 2 | a head weight below the threshold makes an adjacent pair of +-1s droppable, which is a different tool. Above 3 the surface is flat (all eight shapes within 0.13%) |
| 4x4 tail `3,2,2,1,1,1,1,1` | at or slightly ahead of every alternative on both bands (CRF -0.07%, deep +0.01%), no speed or size cost, 254/254 conformance |

## What didn't work

| idea | why not |
|---|---|
| Gate UMH on the lowres oracle (hex cost and distance against the lookahead's prediction) | UMH improves 40-58% of motion-clip searches and the gains aren't concentrated where the oracle flags trouble. No skip region is BD-safe |
| An 8-point square refine in place of UMH, without richer seeds | captures most of UMH's small near-hex gains and still costs +15.9% on bus, +8.2% on stefan. The rare distant finds carry the quality |
| Cap sub-pel iterations on the square path | hpel and qpel converge in two iterations 96-99% of the time. A cap saves about 0.3% |
| 4-point diamond to convergence in place of the square (`--subpel 1`) | foreman +1.67%, mobile +0.51% VMAF-NEG |
| Constant-threshold partition early termination | +0.38% mean, mobile +0.54%, for 1-2% speed. Retuning the slack trades BD for speed one to one |
| Constant threshold plus heterogeneity and importance guards (mode 2) | still +0.60% mean, and the guards protect so many blocks that only 2-4% speed is left |
| Seed partition searches from the 16x16 and sibling winners | no speed (seeds are already saturated) and bus +0.96% |
| Hex iteration cap | near zero: the hex already exits on convergence |
| `--me esa` / `tesa` as a quality setting | 3-8x the CPU, and worse on HD (pedestrian +3.67% / +4.30%). The SAD-cheapest vector isn't the one that codes best |
| A 9/8 intra admission margin | +0.48% BD. 18/16 costs tempete +2.44% on its own |
| Decimate head weight 2 | +2.29%: lets an adjacent nonzero pair be dropped |
| Decimate threshold 4 with the shipped table, or thresholds 6/4 | +2.37% at 4. RDOQ already drops marginal coefficients, so 6/4 over-decimates |
| A top-1 SATD gate between 16x16 Bi and uni | +3.2% on bus. Co-clustered modes need to both reach RD |

## Open questions

- In-search dead-ring termination: stop the UMH grid after k gainless rings.
  42-56% of UMH runs gain nothing from all of its roughly 120 probes. Every other UMH cut
  has paid BD out of proportion, so build one bounded version and expect to
  park it.

## Measuring it

- `Y264_ME_STATS=1` prints per-search counters: stage reached, probes, UMH
  improvement and distance, sub-pel iterations, and the oracle histogram. Run
  it at `--threads 1`, since the accumulators are plain globals.
- `Y264_ME_ETSTAT=1` buckets searches by seed cost and reports how far the
  integer stages moved the vector. Read the distance column, not the SATD
  gain. Gain volume is mostly 1-pel moves and doesn't predict BD.
- Profile and BD at `--crf`, never `--qp`. The `--qp` path shifts mb-tree.
  Score BD against x264 medium with `scripts/bdcompare.py --vmaf --no-cache`
  or at matched rate with `scripts/bd_at_rate.py`, not against yah264 alone.
- Measure several arms in one interleaved batch. Arms run one invocation at a
  time read non-monotonic in the gate's own strength.
