# Motion search and mode decision

The per-macroblock inner loop finds motion vectors, picks partition and mode,
and quantizes the residual. Most of the encoder's operations happen here,
mainly as motion-search SAD and SATD probes.

Medium is `--subme 7` and `--subpel 2` with three reference frames. Below
`--subme 9`, SATD picks candidates and full RD runs only on the survivors.
`--subme 9` and up run full RD on every partition with RDOQ in every trial.
This page describes medium unless it says otherwise. See [B-frames](b-frames.md)
for direct mode, B_8x8 and B skips.

Quality figures are BD-rate on VMAF v1, self-A/Bs on the CRF band at one
thread unless noted.

## How it works

### Integer search

Each search scores a batch of seeds and starts from the best: zero, the
median predictor, the spatial neighbors' vectors and, for hex, the
lookahead's lowres motion field. Hex is the search at medium and
faster, or with `--me hex`.

Below `--subme 8`, hex iterates to convergence and a terminal 8-point square
catches minima diagonally off a vertex. The hex-parity features are on exactly
when hex is the primary search: richer seeds, the lowres field, the square and
whole-pel seed rounding. Medium on that path reads -4.4% median against x264
medium on the CRF band's seven CIF clips.

The uneven-cross multi-hexagon grid (UMH) runs after hex at `--subme 8` and
up, or with `--me umh`, out to `--merange` (default 16). It skips 8x8 and
smaller blocks, which start from their parent's refined vector. Pass `--me`
to stop `--subme 8` from also changing the algorithm.

Three early-outs trim the integer stage without touching sub-pel:

- The seed early-out skips the hex, UMH and finish stages when the best seed
  costs at most K per pixel in SAD plus vector rate. K is 48 below 16x16 and 4
  at 16x16, where the median-predictor start needs a near-perfect seed.
- Neighbor agreement (`Y264_ME_AGREE`) takes one hex step and one finish
  step when every seed collapses onto the predictor, on pictures 1920 wide
  and up.
- The small-block UMH skip above.

### Sub-pel refinement

Medium runs the capped diamond (`--subpel 2`): a 4-point diamond, two
iterations per level, half-pel in SAD and quarter-pel in SATD. Slow and up run
the 8-neighbor square to convergence in SATD.

Two rules keep candidates out of quarter-pel. At `--subme 7` and below, a
candidate skips it when its half-pel cost times 7/8 exceeds the macroblock's
best. `Y264_QPEL_SKIPQ` skips it when the half-pel winner is the integer
vector and costs at most 96 x lambda x area / 256, because the quantizer
would discard the residual quarter-pel could improve. On pictures 1920 wide
and up, B list 0 refines only the winning reference at quarter-pel.

### P-macroblock decision

At `--subme 8` and below, a P macroblock goes through these steps:

1. **Fast P-skip probe.** If the skip residual quantizes to nothing under the
   seed quantizer, the macroblock commits skip before any search.
   `--no-fast-pskip` turns it off.

   With `--weightp 1` or `2`, the probe refuses skip on a flat macroblock
   whose residual keeps an Intra16x16 DC level, once the picture's mean has
   moved a quarter code or more against reference 0. Because that transform
   sums the whole macroblock, it keeps the small even steps of a fade that
   4x4 deadzones drop. Flat means variance under the AQ floor. Skip is still
   allowed when:

   - the step goes against the picture's shift
   - the step is smaller than the block's own noise
   - the step buys less distortion than 8 bits cost at the mode lambda
   - the block is dark but not black, where intra plateaus over a smooth
     gradient band worse than the skip

   `Y264_FLAT_SKIP_DC=0` turns the check off in a dev_knobs build.
2. **16x16 search** over the active references.
3. **Ref-0 exit**, on pictures of at least 1200 macroblocks. A ref-0 16x16
   vector within a quarter-pel of the skip vector, with raw SATD under 1200
   lambdas, ends the search before other references and splits. RD still
   compares it against skip and intra.
4. **`--p-part-gate 400`.** Under 400 lambdas, no split is searched, unless
   the 3x3 lowres-cost neighborhood is dispersed (a motion or texture edge)
   or mb-tree says other frames depend on the block. Raising it to 1200
   buys almost nothing because the gate saturates.
5. **Partition early termination** (`Y264_PART_EARLYTERM=4`). 8x8 is searched
   next. 16x8 and 8x16 follow only when the 8x8's raw SATD beats the 16x16's
   by less than the vector rate of the two halves a rectangle would merge.
   The margin is per macroblock, so static blocks prune hard and real motion
   keeps its rectangles. Blocks mb-tree marks important always keep them.
6. **SATD picks the shape.** RD scores the SATD winner and, as insurance
   against over-splitting at low rates, the 16x16. That's one or two RD
   encodes instead of four.
7. **Intra screen.** I_16x16 modes are scored by SATD without a transform.
   Intra gets an RD trial only under 1.5x the inter winner's SATD, or 0.75x
   on B macroblocks.

With [`--weightp`](options.md), a slice can hold a plain copy of reference 0
for letterbox bars. At any `--subme`, a macroblock that reaches into the bars
refuses skip when the plain copy predicts its bar samples better, because
skip would predict through the weighted copy. It searches reference 0 alone.
Partitions at least half in the bars get the plain copy. One that only clips
a bar's edge takes the better copy. Bars that fade with the picture keep the
weighted copy and skip.

CABAC P macroblocks price intra trials with the CABAC rate estimate at any
`--subme`.

Medium picks the transform size up front from SA8D against SATD. `--subme 9`
and up encode both and let RD pick.

### RDOQ and trellis placement

With the default `--trellis 1`, medium's RD trials use the plain deadzone.
Only the winner is re-encoded with RDOQ. `--trellis 2`, and `--subme 9` and
up, run RDOQ in every trial. Under CABAC, medium's RDOQ is one reverse Viterbi
pass with transform-domain distortion and running context rate. CAVLC and
psy-trellis blocks use a greedy search: one toward-zero pass at medium, to
convergence at `--subme 9` and up.

Because RDOQ seeds from round-to-nearest, a marginal coefficient always
enters as {q-1, q}. RD decides whether it stays.

On very smooth intra blocks the RDOQ lambda ramps from full at source variance
8 to zero at 2, keeping the small levels of a gentle gradient.
`Y264_TREL_SMOOTH=0` turns the ramp off in a dev_knobs build.

### Coefficient decimation

A 4x4 inter block with only +-1 levels scores a weight per nonzero
coefficient, by the zero run before it. Below the threshold it's zeroed. Any
level of 2 or more forces a keep. `--no-dct-decimate` disables it.

| block | weight by preceding zero run | threshold |
|---|---|--:|
| 4x4 | `3,2,2,1,1,1,1,1`, then zeros | 3 |
| 8x8 | `3,3,3,3,2,2,2,2`, then eight ones, then zeros | 2 |

## What we chose and why

| choice | why |
|---|---|
| Hex with the parity features at medium, UMH from `--subme 8` | -4.4% median against x264 medium on the CIF clips without the wide grid's cost |
| No UMH on 8x8 and smaller | about 6-7% faster single-threaded. Under `--me umh` it costs 0.2-0.7% on the four CIF motion clips: foreman +0.66%, bus +0.46%, stefan +0.41% |
| Seed early-out, K 48 sub-16x16 and 4 at 16x16 | 2-3% of wall at 12 threads for a CRF-band median of +0.15% |
| Capped diamond sub-pel at medium | a speed trade. Slower presets keep the square |
| Adaptive partition margin plus importance rescue (mode 4) | against no early termination the margin alone costs a 0.46% CRF-band median and mode 4 costs 0.37%. The rescue buys back that 0.09 for 1-3 points of the margin's 4.33% wall saving |
| `--p-part-gate 400` | holds quality on every clip of the 12-clip HD band: no clip moves by more than 0.32%. It removes the P tournament's most numerous searches |
| Ref-0 exit at K=1200 | against K=600, shields, the worst clip on wall, is 8.4% faster at one thread. fourpeople pays +1.90%. The CRF band median reads 0.00% because the exit doesn't run at CIF |
| Intra admission at 1.5x (P) and 0.75x (B) | 1.5x is BD-neutral for 1.06x CIF and 1.11x 720p in pure C. Because B intra wins a few dozen macroblocks in 300,000, its margin is tighter |
| RDOQ on the winner only at medium (`--trellis 1`) | 1.18x faster at CIF and 1.29x at 720p in pure C, for a +0.36% CRF-band median (sintel +3.87% is the worst clip) |
| Viterbi RDOQ under CABAC at medium | -1.94% median against the greedy search on the CRF band, and 1.1-1.3x faster |
| Intra RDOQ lambda ramped to zero below a source variance of 8 | without it RDOQ drops the small levels in smooth gradients. The I frame then bands |
| Round-to-nearest RDOQ seed | a deadzone seed drops would-be level-1 coefficients with no RD look. Round-to-nearest gained on the corpus mean, most on park_joy, with no clip worse |
| Decimate head weight 3, thresholds 3 and 2 | a head weight below the threshold makes an adjacent pair of +-1s droppable. That's a different tool. Above 3 the results are flat: the shapes measured are within 0.2% of each other on the median |
| 4x4 tail `3,2,2,1,1,1,1,1` | a free choice on a flat plateau, because neither band separates it from the shorter tail `3,2,2,1,1,1`. The shorter tail reads 0.19% better on the CRF band median. On the deep band it reads a 0.00% median, with every clip inside the v1 deep band's measured null of about ±1.0 per clip. It has no speed or size cost and passes 254/254 conformance |

Partition early termination on the CRF band, against none. The
constant-threshold rows are means and the margin rows are medians, so read
each row on its own:

| variant | BD cost | speed |
|---|--:|--:|
| constant threshold | +0.53% mean, mobile +1.57% | 1-2% |
| constant threshold plus guards (mode 2) | +0.42% mean | 2-4% |
| adaptive margin alone | +0.46% median | 4.33% of wall |
| adaptive margin plus importance rescue (mode 4, shipped) | +0.37% median | 1-3 points under the margin alone |

## What didn't work

| idea | why not |
|---|---|
| Gate UMH on the lowres oracle (hex cost and distance against the lookahead's prediction) | UMH improves 40-58% of motion-clip searches. Its gains aren't concentrated where the oracle flags trouble. No skip region is BD-safe |
| An 8-point square refine in place of UMH, without richer seeds | captures most of UMH's small near-hex gains and still loses clearly on bus and stefan. The quality comes from the rare distant finds |
| Cap sub-pel iterations on the square path | hpel and qpel converge in two iterations 96-99% of the time. A cap saves about 0.3% |
| 4-point diamond to convergence in place of the square (`--subpel 1`) | foreman +0.92%, mobile +0.76% |
| Constant-threshold partition early termination | +0.53% mean, mobile +1.57%, for 1-2% speed. Retuning the slack trades BD for speed one to one |
| Constant threshold plus heterogeneity and importance guards (mode 2) | still +0.42% mean. The guards protect so many blocks that only 2-4% speed is left |
| Seed partition searches from the 16x16 and sibling winners | no speed gain, since the seeds are already saturated, and bus loses |
| Hex iteration cap | near zero: the hex already exits on convergence |
| `--me esa` / `tesa` as a quality setting | 3-8x the CPU and worse on HD: against the hex default, pedestrian reads +11.0% / +11.3%, where `--me umh` reads +5.4%. The SAD-cheapest vector isn't the one that codes best |
| A 9/8 intra admission margin | no quality gain: it reads neutral against 1.5x (median 0.00%, worst sintel +0.29%). Whether its speed is worth taking is open |
| Decimate head weight 2 | +1.09% mean, sintel +9.6%: lets an adjacent nonzero pair be dropped |
| Decimate threshold 4 with the shipped table, or thresholds 6/4 | +0.92% mean at 4, sintel +9.1%. RDOQ already drops marginal coefficients, so 6/4 over-decimates |
| A top-1 SATD gate between 16x16 Bi and uni | bus loses. Co-clustered modes need to both reach RD |

## Open questions

- Dead-ring termination would stop UMH after k gainless rings. 42-56% of UMH
  runs gain nothing from their roughly 120 probes. Every other UMH trim cost
  BD out of proportion, so build one bounded version and expect to park it.

## Measuring it

The `Y264_*` variables need a `-Ddev_knobs=true` build.

- `Y264_ME_ETSTAT=1` buckets searches by seed cost and reports how far the
  integer stages moved the vector. Read the distance column. The SATD gain,
  mostly 1-pel moves, doesn't predict BD.
- Measure at `--crf`, never `--qp`, which shifts mb-tree. Score against x264
  medium, never yah264 alone, with `scripts/bdcompare.py --vmaf --no-cache`
  or at matched rate with `scripts/bd_at_rate.py`.
- `--threads 1` takes the quality order, which turns off the seed early-out
  and partition early termination. A one-thread A/B of either is
  byte-identical. Set `Y264_STQ=0` on both sides, as the rows were read, or
  use two or three threads.
- Run several arms in one interleaved batch. Arms run one invocation at a
  time come out non-monotonic in the gate's own strength.
