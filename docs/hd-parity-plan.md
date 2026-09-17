# HD parity plan

## The problem

The site's ten-clip board reads 0.96x against x264. The board a reader can
run, `make review`, reads 1.33x to 1.40x slower on four HD clips at CRF 26.
Engineers test with clips like those four. A parity claim that does not
survive them is not a parity claim.

This plan gets yah264 to parity on the clips engineers care about, and it
changes what we measure so the published number is the one a reader gets.

## Goal

On an HD board of 720p and 1080p clips at the bitrates people use, the
shipped build reads at or under x264's wall time at matched quality.

| leg | bar |
|---|---|
| median wall ratio, shipped build, auto threads | 1.00x or better |
| worst clip | 1.10x or better |
| VMAF at matched bitrate | at or above x264 on the median clip |
| the runnable board | prints the same ratios as the in-process one, within the run-to-run spread |

The goal is met when both boards say so on the same clips.

## What we know

The ten-clip board's slow cells are the low-bitrate HD ones. Its fast cells
are high-bitrate 1080p. The four review clips are HD at low-to-mid bitrate,
so they are the slow regime.

With assembly off on both sides we are 16% faster than x264 multi-threaded.
With assembly on both sides we are 4% slower. x264's hand-written assembly
buys it about half again what our NEON buys us. That is the whole deficit.
It costs the most where the SIMD-heavy paths dominate, which is low-bitrate
HD: most blocks are skips or cheap modes there, and the work that remains is
the block-matching and filtering that assembly accelerates.

Two things in the runnable board are not encoder slowness. It times two
separate CLIs with process startup inside the measurement. The board script
records that at about 0.19x. It runs both encoders at CRF 26 without
matching the rates, and on three of the four clips we write smaller files at
equal or better quality, so we are working at a higher quality point than
x264 in those cells.

The NEON round already built the kernels the profile priced. What is left
on that list is small. The remaining gap is per-frame fixed cost and decision
work at low rates, plus the quality of the kernels we have against x264's.

## The stages

Each stage is one brief with a named gate. Heavy legs run on a quiet box.

### 0. Measure the gap

The board-reconciliation item splits the review board's ratio into the
harness, the CRF mismatch and the clip regime, each measured. It profiles
the slowest HD cells at one thread by stage, so stages 2 and 3 start from a
measured number. Its output is a table beside the review board's
sample output and the briefs for the next two stages.

### 1. The HD board

A board of eight to twelve 720p and 1080p clips at three rates each, low,
mid and high, weighted toward live action. Windows from the long-form films
we hold, Meridian, Chimera, Sparks and Tears of Steel among them, belong on
it, because those are the clips engineers reach for. It replaces the
ten-clip board as the headline on the site. `make review` runs the same board at matched
achieved rate with process setup outside the timer, so the runnable number
and the published number are the same measurement. This stage removes the
part of the gap that was never encoder speed.

### 2. Decision cost at low rates

At low bitrate the encoder's job is mostly to confirm that a block is a skip
and move on. Ours runs more of the tournament than that needs. Each candidate below must hold the BD change under 0.2% on the HD band:

- an early skip verdict for P and B blocks before the full mode search
- pruning trellis, psychovisual weighting and subpixel refinement with the
  quantizer, so the expensive tools run where they pay
- reference and partition pruning where the block above and to the left
  already settled on a skip

The target is the low-rate HD cells at one thread.

### 3. Fixed cost per frame

The lookahead, the macroblock tree and the deblocking filter cost the same
whether the residual is cheap or not. Stage 0's profile says how much of a
low-rate HD frame that is. The work is to shrink it: the tree's deposit and
propagation on the shrunken picture, the lowres analysis, and the filter's
per-edge overhead.

### 4. Kernel quality

Run our kernels and x264's through the same benchmark block sizes and compare
the speedup each gets over its own C. Where x264's multiple beats ours by
more than 15% on a family, that family gets a second pass: block matching,
transforms, motion compensation and the deblocking filter first. DotProd
and the newer ARM matrix instructions are in scope where they are bit-exact.

### 5. Threading at HD

Only if stage 0 shows the shipped build burning more CPU than x264 for the
same wall time at 720p and 1080p. The places to look are the auto thread cap, the wavefront's row dependency
and frame parallelism at HD sizes.

### 6. Publish

The HD board goes on the results page as the headline. The review page
prints the same numbers. The ten-clip board stays as a second table.

## Order

Stage 0 runs in the next quiet window. Stages 1 to 3 are the engineering
priority after wave 4 of the x264 feature-parity programme, ahead of wave 5.
Stage 4 runs beside the x86-64 kernel waves, which measure the same
multiples on the other architecture. The same rule applies to the sibling
encoders: a parity number is checked on an HD-weighted board at typical
rates before it goes on a page.
