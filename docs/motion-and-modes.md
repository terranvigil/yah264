# Motion search and mode decision

This page covers the per-macroblock inner loop. It finds motion vectors,
picks a partition shape and a skip, inter or intra mode, quantizes the
residual and decides which coefficients survive. Because most of the
encoder's operation count is spent here, mainly on SAD and SATD probes during
motion search, most speed work starts here too.

The preset sets the effort. Medium is `--subme 7` and `--subpel 2` with three
reference frames. Everything below `--subme 9` takes the fast path, where
SATD picks the candidates and full RD runs only on the survivors. `--subme 9`
and up run full RD on every partition and keep RDOQ in every trial. The rest
of this page describes medium unless it says otherwise. [B-frames](b-frames.md)
covers direct mode, B_8x8 and the B skip decision.

Quality figures on this page are BD-rate on VMAF v1, self-A/Bs on the CRF
band at one thread unless a row says otherwise.

## How it works

### Integer search

Each search starts from a batch of candidates: zero, the median predictor and
the spatial neighbors' vectors. When hex is the search, the lowres motion
field from the lookahead joins them. Hex is the search at medium and faster,
or with `--me hex` at any preset. All the candidates are scored together and
the best one seeds the pattern.

Below `--subme 8`, the pattern is a hexagon iterated to convergence and then
a terminal 8-point square. The square catches minima that sit diagonally off
a hex vertex. We call four things together the hex-parity features: the
richer seeds, the lowres field, the square, and rounding seeds to the nearest
whole pel. They're on exactly when hex is the primary search: with
`--me hex`, or in the auto tiers at medium and faster. On that path medium
reads -4.4% median against x264 medium on the CRF band's seven CIF clips.

The uneven-cross and multi-hexagon grid (UMH) runs after the hex at
`--subme 8` and up or with `--me umh`. It reaches out to `--merange`, which
defaults to 16. UMH never runs on 8x8 or smaller blocks. The wide grid has
little left to find there, because those blocks are seeded from their
parent's refined vector. When `--me` isn't given, `--subme` picks the method.
That makes `--subme 8` a change of search algorithm as well as of effort.
Pass `--me` to fix the method.

Three early-outs trim the integer stage without touching sub-pel:

- The seed early-out skips the hex, UMH and finish stages when the best seed
  already costs at most K per pixel in SAD plus vector rate. K is 48 for
  sub-16x16 shapes and 4 for 16x16, because a 16x16 starts from a median
  predictor and needs a near-perfect seed before it's safe to stop.
- Neighbor agreement (`Y264_ME_AGREE`) applies to pictures 1920 wide and up.
  When every seed collapses onto the predictor, the search takes one hex step
  and one finish step.
- The small-block UMH skip, described above.

### Sub-pel refinement

Medium runs the capped diamond (`--subpel 2`). It's a 4-point diamond with two
iterations per level. Half-pel is scored in SAD and quarter-pel in SATD. Slow
and up run the 8-neighbor square to convergence in SATD at both levels.

Two rules decide which candidates get quarter-pel at all. At `--subme 7` and
below, a candidate skips quarter-pel when its half-pel cost times 7/8 already
exceeds the best cost seen in this macroblock. Only near-winners pay. The
second rule (`Y264_QPEL_SKIPQ`) skips quarter-pel when the half-pel winner is
still the integer vector and costs at most 96 x lambda x area / 256, because
the quantizer would discard the residual quarter-pel could improve. On
pictures 1920 wide and up, the B list-0 reference loop searches each
reference to half-pel and refines only the winning reference at quarter-pel.

### P-macroblock decision

At `--subme 8` and below, a P macroblock goes through these steps in order:

1. **Fast P-skip probe.** If the skip prediction's residual quantizes to
   nothing under the seed quantizer, the macroblock commits skip before any
   search. `--no-fast-pskip` turns the probe off.

   With `--weightp 1` or `2`, the probe also refuses some skips.
   Because the Intra16x16 DC transform sums the whole macroblock, it can code
   a small even step that every 4x4 deadzone drops, such as one frame of a
   fade. In a picture whose mean moved against reference 0 by a quarter code
   or more, the probe refuses a skip on a flat macroblock whose residual
   would keep a level in that DC transform. Flat means variance under the AQ
   field's floor. The probe still leaves the skip alone in four cases:

   - the step goes against the picture's shift
   - the step is smaller than the block's own noise
   - the step buys less distortion than 8 bits cost at the mode lambda
   - the block is dark but not black, where intra plateaus over a smooth
     gradient band worse than the skip

   `Y264_FLAT_SKIP_DC=0` turns the check off in a dev_knobs build.
2. **16x16 search** over the active references.
3. **Ref-0 exit**, on pictures of at least 1200 macroblocks. When the ref-0
   16x16 vector falls within one quarter-pel of the skip vector and its raw
   SATD is under 1200 lambdas, the encoder skips the other references and all
   splits. The 16x16 still goes through the RD compare against skip and
   intra.
4. **`--p-part-gate 400`.** If the 16x16 costs under 400 lambdas, no split is
   searched. Two interlocks keep the splits anyway. One is a dispersed 3x3
   lowres-cost neighborhood, which marks a motion or texture edge. The other
   is an mb-tree offset saying other frames depend on this block. The gate
   saturates against those interlocks. Raising the threshold to 1200 buys
   almost nothing.
5. **Partition order with early termination** (`Y264_PART_EARLYTERM=4`).
   The 8x8 split is searched next. 16x8 and 8x16 are searched only when the
   8x8's raw SATD beats the 16x16's by less than the vector rate of the two
   8x8 halves a rectangle would merge. That margin is per macroblock: static
   blocks prune hard and real motion keeps its rectangles. Blocks mb-tree
   marks as important keep the rectangles regardless.
6. **SATD picks the shape.** Full RD scores the SATD winner. When the winner
   isn't 16x16, RD also scores the 16x16 as insurance against SATD
   over-splitting at low rates. That's one or two RD encodes instead of four.
7. **Intra screen.** Each I_16x16 mode is predicted and scored by SATD with
   no transform. Intra gets a full encode and an RD trial only when that cost
   is under 1.5x the inter winner's SATD. On B macroblocks the margin is
   0.75x.

A slice can hold reference 0 a second time as a plain copy for letterbox bars
(see `--weightp` on the [options page](options.md)). In such a slice, at any
`--subme`, a macroblock that reaches into the bars doesn't take skip when the
plain copy predicts its bar samples better than the weighted one, because
skip would predict through the weighted copy. It searches reference 0 alone
and gives the plain copy to each partition with at least half its rows or
columns in the bars. A partition that only clips a bar's edge keeps
whichever copy predicts it better. A bar that fades with the picture keeps
the weighted copy and its skip.

At any `--subme`, a CABAC P macroblock prices its intra trial with the CABAC
rate estimate, as it does its inter candidates.

Medium also picks the transform size up front by comparing SA8D against
SATD. Only that size is encoded. `--subme 9` and up encode both and let RD
pick.

### RDOQ and trellis placement

`--trellis 1` is the default. At medium, RD trials quantize with the plain
deadzone and only the winning mode is re-encoded with RDOQ before commit.
`--trellis 2` runs RDOQ in every trial, which `--subme 9` and up do anyway.
Under CABAC, medium's RDOQ is a single reverse Viterbi pass with
transform-domain distortion and running context rate. CAVLC and psy-trellis
blocks use a greedy search instead. That search makes one toward-zero pass at
medium and iterates to convergence at `--subme 9` and up.

Because RDOQ seeds each coefficient from round-to-nearest, a marginal
coefficient always enters the candidate set as {q-1, q}. RD then decides
whether to keep or drop it.

On very smooth intra blocks the RDOQ lambda ramps from full at a source
variance of 8 down to zero at 2. That keeps the small levels that make up a
gentle gradient. In a dev_knobs build, `Y264_TREL_SMOOTH=0` turns the ramp
off.

### Coefficient decimation

After quantization, a 4x4 inter block whose levels are all +-1 gets a score.
Each nonzero coefficient adds a weight looked up by the run of zeros before
it. Below the threshold the whole block is zeroed. Any level of magnitude 2 or
more forces a keep. `--no-dct-decimate` turns decimation off.

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
| Adaptive partition margin plus importance rescue (mode 4) | the lowest BD cost of the early-termination variants in the table below. The rescue buys back 0.09 of the margin's loss for part of its wall saving |
| `--p-part-gate 400` | holds quality on every clip of the 12-clip HD band: no clip moves by more than 0.32%. It removes the P tournament's most numerous searches |
| Ref-0 exit at K=1200 | against K=600, shields, the worst clip on wall, is 8.4% faster at one thread. fourpeople pays +1.90%. The CRF band median reads 0.00% because the exit doesn't run at CIF |
| Intra admission at 1.5x (P) and 0.75x (B) | 1.5x is BD-neutral for 1.06x CIF and 1.11x 720p in pure C. Because B intra wins a few dozen macroblocks in 300,000, its margin is tighter |
| RDOQ on the winner only at medium (`--trellis 1`) | 1.18x faster at CIF and 1.29x at 720p in pure C, for a +0.36% CRF-band median (sintel +3.87% is the worst clip) |
| Viterbi RDOQ under CABAC at medium | -1.94% median against the greedy search on the CRF band, and 1.1-1.3x faster |
| Intra RDOQ lambda ramped to zero below a source variance of 8 | without it RDOQ drops the small levels in smooth gradients. The I frame then bands |
| Round-to-nearest RDOQ seed | a deadzone seed drops would-be level-1 coefficients with no RD look. Round-to-nearest gained on the corpus mean, most on park_joy, with no clip worse |
| Decimate head weight 3, thresholds 3 and 2 | a head weight below the threshold makes an adjacent pair of +-1s droppable. That's a different tool. Above 3 the results are flat: the shapes measured are within 0.2% of each other on the median |
| 4x4 tail `3,2,2,1,1,1,1,1` | a free choice on a flat plateau, because neither band separates it from the shorter tail `3,2,2,1,1,1`. The shorter tail reads 0.19% better on the CRF band median. On the deep band it reads a 0.00% median, with every clip inside the v1 deep band's measured null of about ±1.0 per clip. It has no speed or size cost and passes 254/254 conformance |

Partition early termination on the CRF band, against none:

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
| Constant-threshold partition early termination | costs more than the shipped margin for less speed. Retuning the slack trades BD for speed one to one |
| Constant threshold plus heterogeneity and importance guards (mode 2) | the guards protect so many blocks that little speed is left |
| Seed partition searches from the 16x16 and sibling winners | no speed gain, since the seeds are already saturated, and bus loses |
| Hex iteration cap | near zero: the hex already exits on convergence |
| `--me esa` / `tesa` as a quality setting | 3-8x the CPU and worse on HD: against the hex default, pedestrian reads +11.0% / +11.3%, where `--me umh` reads +5.4%. The SAD-cheapest vector isn't the one that codes best |
| A 9/8 intra admission margin | no quality gain: it reads neutral against 1.5x (median 0.00%, worst sintel +0.29%). Whether its speed is worth taking is open |
| Decimate head weight 2 | +1.09% mean, sintel +9.6%: lets an adjacent nonzero pair be dropped |
| Decimate threshold 4 with the shipped table, or thresholds 6/4 | +0.92% mean at 4, sintel +9.1%. RDOQ already drops marginal coefficients, so 6/4 over-decimates |
| A top-1 SATD gate between 16x16 Bi and uni | bus loses. Co-clustered modes need to both reach RD |

## Open questions

- In-search dead-ring termination would stop the UMH grid after k gainless
  rings. 42-56% of UMH runs gain nothing from all of its roughly 120 probes.
  Every other way of trimming UMH has paid BD out of proportion, so build one
  bounded version and expect to park it.

## Measuring it

The `Y264_*` variables here are read only in a build configured with
`-Ddev_knobs=true`.

- `Y264_ME_ETSTAT=1` buckets searches by seed cost and reports how far the
  integer stages moved the vector. Read the distance column. The SATD gain is
  mostly 1-pel moves and doesn't predict BD.
- Profile and measure BD at `--crf`, never `--qp`, because the `--qp` path
  shifts mb-tree. Score BD against x264 medium with
  `scripts/bdcompare.py --vmaf --no-cache`, or at matched rate with
  `scripts/bd_at_rate.py`. Don't score it against yah264 alone.
- At `--threads 1` the seed early-out and the partition early termination are
  off, because single-thread encodes take the quality order. A one-thread
  A/B of either reads byte-identical. Set `Y264_STQ=0` on both sides, as the
  rows above were read, or run at two or three threads.
- Measure several arms in one interleaved batch. Arms run one invocation at a
  time come out non-monotonic in the gate's own strength.
