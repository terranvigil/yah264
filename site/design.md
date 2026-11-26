---
title: Design - yah264
description: Validating the bitstream, the pipeline, the threading model and rate control.
---

# Design

## Validating the bitstream

Every change must pass the conformance gate: an independent decoder has to read
our output the way yah264 does. An encoder predicts from its own decoded copy of
each frame, the reconstruction. The test writes that copy out with
`--dump-recon`, decodes the stream with ffmpeg's H.264 decoder, and requires a
bit-exact match on every frame of every test clip across a range of quality
settings. Neither has to match the source, only each other. `make conformance`
runs it locally, and CI runs it on every push that touches code.

The bar is exact because errors compound. One wrong bit means the next frame is
predicted from a picture the decoder never had. The error grows until the
picture breaks. A quality score misses this early, because drift looks like a
slightly worse encode until it looks like a broken one.

## The pipeline

Most encoders loop over a frame's blocks and add threads afterward, which runs
out of room quickly. yah264 is a pipeline of stages instead. Each owns one kind
of decision. The parallelism lives between them.

The lookahead runs first, on shrunken copies of a window of future frames. It
picks frame types, finds scene cuts and builds the macroblock-tree data: how
much later frames depend on each 16 by 16 macroblock. Nothing downstream can be
smarter than what it saw.

Behind it, whole frames encode in parallel where the GOP (the frames from one
keyframe to the next) allows. Inside a frame, rows move as a wavefront. A row
starts once the row above is far enough ahead. One lock-free pool feeds both.

Two runs at `--threads 1` must produce the same bytes. With more threads the
bytes may differ, because holding them would rule out optimizations, and x264
doesn't promise it either. Every stream does decode to exactly the picture the
encoder reconstructed.

## Threading

Every core can make an encoder slower. A picture absorbs only so much
concurrency before coordination costs more than it buys: a 352 by 288 CIF
frame runs out of independent rows quickly, a 1080p frame much later. Auto
takes the smaller of the core count and 16, then the picture caps it:

| picture | thread cap |
|---|--:|
| CIF | 12 |
| 720p | 21 |
| 1080p | 32 |

Under auto only the CIF-and-smaller cap bites. The others clamp an explicit
count.

Nobody measured a knee at 16. It's a safe default. Wavefront scaling hits the
picture's critical path, the longest chain that must run in order, well before
it runs out of cores. On chips with two kinds of core, the last workers go to
the slow ones and lengthen it. The one measurement behind the default is at
CIF, where 18 threads read 6 to 11% worse than 8. See the [results page](results.md#by-processor)
for rented 32- and 96-vCPU runs and the [threading page](threading.html) for the
model.

## Rate control

CQP, CRF, single-pass ABR, CBR and VBV, capped VBR, capped CRF and two-pass all
ship. This page covers capped CRF, the most used. See
[how video encoding works](encoding.md) for what each mode is for.

Macroblock-tree is on by default. It gives a finer quantizer to blocks that
later frames predict from, since a bit that fifty frames inherit is worth more.
Variance-adaptive quantization moves bits toward flat areas, where quantization
shows first. The same signal adjusts the mode decision's lambda, the bits-for-
error exchange rate, per macroblock, which accounts for much of our quality
lead over x264.

Under capped CRF the encoder codes to the quality target and the buffer only
takes bits away. Because the buffer models what a decoder could have
received by each frame's turn, respecting it keeps the stream playable. If the cap never bites you get
exactly the CRF encode at the same thread count. Once it bites, everything after
changes. Every later GOP opens on a half-full buffer, which costs a few bits. Where the cap is tight, a per-frame budget pulls the buffer back toward
half full. Nothing else under CRF watches the bit count, so without it a
prediction error could drain the buffer into an underflow.

The buffer compliance test runs six clips at three caps through both VBV paths.
We pass 29 of 36 cells and fail on tight caps and mid-stream scene cuts, as the
[rate-control notes](https://github.com/terranvigil/yah264/blob/main/docs/rate-control.md)
track. The reference encoder passes all 18 cells its features cover. The two
counts aren't one ratio.

Across shots, the CRF base quantizer is flat per frame type and moves with the
frame mean of the per-macroblock offsets: the macroblock-tree boost plus
adaptive quantization's energy term. The energy term's frame mean goes on the
anchor frame's base QP, scaled by 1.3 under uncapped CRF and 1.04 under a VBV
cap or bitrate target, and B frames inherit it through the anchor's offsets. A
busy shot codes a little cheaper and a quiet one a little better. Within a
frame the term keeps the gentler strength that won on single-shot clips.

On file input, `--cut-split` puts an IDR on every cut the pre-scan finds.
Nothing predicts across a cut. `--shot-crf` gives each shot its own CRF from the
shot table. The [results page](results.html) has the numbers.

## Mode decision and motion estimation

Mode decision carves up a macroblock and predicts each piece. The candidate with
the lowest distortion plus lambda times rate wins. Encoders differ in three places:

- which candidates they try
- how well they estimate rate before the entropy coder has run
- where the search may stop early

Motion search starts from a guess built from neighboring vectors and walks a
diamond, hexagon or uneven multi-hexagon pattern by preset. Subpixel refinement
follows, as far as the preset allows. Full trellis RDOQ over both transform
sizes picks each quantized coefficient knowing its bit cost. The same cost picks
the transform size per macroblock, behind a cheap screen.

A psychovisual term, on by default at 2.0 (twice the reference encoder's),
rewards keeping the source's texture energy at some cost in pixel accuracy. The
PSNR-Y floor on the [results page](results.html) watches that trade.
`--tune psnr` turns the term off.

## The SIMD tier

SIMD instructions do the same arithmetic on many pixels at once. We write them
as intrinsics, C functions that map one for one onto instructions. The compiler
schedules and allocates registers, which is where hand assembly can still win.
checkasm tests every kernel against its plain C version and benchmarks it.

| tier | where it runs | coverage |
|---|---|---|
| NEON | ARM, including the Apple silicon development machine | about 4,000 lines in five files |
| SSE4.2, AVX2 | x86-64, picked at run time | the same kernel families as NEON |
| AVX-512 | x86-64 chips that have it, included in every x86-64 build | pixel comparisons. They beat AVX2 on both an AMD and an Intel machine. |
| SVE | Linux ARM servers with 256-bit SVE, such as AWS Graviton3 | replaces the widest NEON kernels |

The [SIMD notes](https://github.com/terranvigil/yah264/blob/main/docs/simd.md)
have the plan.

## Decoder

The tree's decoder exists to verify our output for the validation above. It
has no standalone command and has never been benchmarked. No number on this
site is a decode number. Making it fast is a separate track in the
[project plan](https://github.com/terranvigil/yah264/blob/main/docs/plan.md)
that hasn't started.
