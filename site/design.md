---
title: Design - yah264
description: Validating the bitstream, the pipeline, the threading model and rate control.
---

# Design

## Validating the bitstream

Every change has to pass the conformance gate. It checks that an independent
decoder reads our output the same way yah264 does.

An encoder keeps a decoded copy of each frame, called the reconstruction, to
predict later frames from. The test writes that copy out with `--dump-recon`
and decodes the output file with ffmpeg's H.264 decoder. The two must match bit
for bit on every frame of every test clip, across a range of quality settings.
Neither matches the source video. They only have to match each other.

`make conformance` runs it locally. CI runs it on every push that touches code.

The bar is exact because errors compound. If our copy of a frame differs from a
real decoder's by one bit, the next frame is predicted from a picture the
decoder never had. The error grows until the picture breaks. A quality score
misses this early, because drift looks like a slightly worse encode until it
looks like a broken one.

## The pipeline

Most encoders loop over the blocks of a frame and add threads afterward.
Because that design runs out of room quickly, yah264 is a pipeline of stages
instead. Each stage owns one kind of decision. The parallelism lives between
the stages.

The lookahead runs first, on a shrunken copy of each picture over a window of
future frames. It picks frame types and finds scene cuts. It also builds the
macroblock-tree data, which records how much later frames will depend on each
16 by 16 block, or macroblock. Nothing downstream can be smarter than what the lookahead saw.

Behind it, whole frames encode in parallel where the GOP structure allows. A GOP
is the run of frames from one keyframe to the next. Inside a frame the rows move
as a wavefront. A row starts once the row above it is far enough ahead to supply
the neighboring blocks it needs. One lock-free pool feeds both kinds of work.

We test that two runs of the same encode at `--threads 1` produce the same
bytes. With more threads the bytes may differ across runs or across thread
counts. Holding them to it would rule out optimizations. x264 doesn't promise
it either. We do promise that every stream decodes to exactly the picture the
encoder reconstructed.

## Threading

Giving an encoder every core can make it slower. A picture absorbs only so much
concurrency before coordination costs more than the work it buys. At 352 by
288 pixels, a CIF frame's wavefront runs out of independent rows quickly. A
1080p frame has far more to give.

We pick the thread count in two steps. Auto takes the smaller of the core
count and 16, and then the picture caps it:

| picture | thread cap |
|---|--:|
| CIF | 12 |
| 720p | 21 |
| 1080p | 32 |

Under auto only the CIF-and-smaller caps ever bite. The others apply when a
caller asks for a count, which we honor and then clamp.

Nobody measured a knee at 16. It's a safe default on an unknown machine. Wavefront
scaling hits the picture's critical path, the longest chain of work that has to
run in order, well before it runs out of cores. On a chip with two kinds of
core, the last workers go to the slow ones and lengthen that chain. The one
measurement behind the default is at CIF, where 18 threads read 6 to 11% worse
than 8. Runs on rented 32- and 96-vCPU machines are on the
[results page](results.md#by-processor).

The [interactive threading page](threading.html) walks through the model with
live diagrams.

## Rate control

Rate control decides how many bits each frame gets. CQP, CRF, single-pass ABR,
CBR and VBV, capped VBR, capped CRF and two-pass all ship. This page covers
capped CRF, because it's the mode that gets used most.
[How video encoding works](encoding.md) explains what each mode is for.

Macroblock-tree is on by default. The lookahead works out which blocks later
frames will predict from and gives them a finer quantizer, because a bit spent
on a block that fifty frames inherit is worth more than one nothing references.
Variance-adaptive quantization runs alongside it and moves bits toward flat
areas, where quantization shows first. The same signal also adjusts the mode
decision's lambda per macroblock. Lambda is the exchange rate between bits and
error that the encoder uses to choose between two ways of coding a block.
Adjusting it per block accounts for much of our quality lead over x264.

Under capped CRF the encoder codes to the quality target. The buffer can only
take bits away. The buffer models what a decoder could have received by the
time it needs each frame, so respecting it keeps a stream playable. If the cap
never bites, you get exactly the CRF encode you asked for at the same thread
count. Once it bites, everything after it changes. Every later GOP opens on a
half-full buffer, which costs a few bits.

Where the cap is tight, a per-frame budget pulls the buffer back toward half
full. Nothing else under CRF watches the bit count. Without that budget the
buffer would drain until a prediction error became an underflow.

The buffer compliance test runs six clips at three caps through both VBV paths.
We pass 29 of its 36 cells. The seven we fail are tight caps and mid-stream
scene cuts. The
[rate-control notes](https://github.com/terranvigil/yah264/blob/main/docs/rate-control.md)
track them. The reference encoder passes all 18 cells that its own features
cover. The two counts aren't one ratio.

Across shots, the base quantizer under CRF is flat per frame type and moves with
the frame mean of the per-macroblock offsets. That mean holds the
macroblock-tree boost and adaptive quantization's energy term. We put the
energy term's frame mean on the anchor frame's base QP, scaled by 1.3 under
uncapped CRF and 1.04 under a VBV cap or a bitrate target. B frames inherit the
shift through the offsets under the anchor. A busy shot then codes a little
cheaper and a quiet one a little better. The term's within-frame part keeps the
gentler strength that won on single-shot clips.

On file input, `--cut-split` puts an IDR on every cut the pre-scan finds.
Nothing after a cut predicts across it. `--shot-crf` gives each shot its own
CRF from the shot table. The [results page](results.html) has the numbers.

## Mode decision and motion estimation

Mode decision is how the encoder carves up a macroblock and predicts each
piece. Every candidate is costed as distortion plus lambda times rate. The
cheapest wins. Encoders differ in three places:

- which candidates are worth trying
- how well they estimate the rate, since the entropy coder that turns decisions
  into bits hasn't run yet
- where the search may stop early

Motion estimation looks for where a block came from in an earlier picture. The
search starts from a guess built from the neighboring blocks' vectors and walks
a diamond, hexagon or uneven multi-hexagon pattern, depending on the preset.
Subpixel refinement then tests positions between whole pixels, as far as the
preset allows. Full trellis RDOQ runs over both transform sizes and picks each
quantized coefficient knowing what it costs in bits. The transform size is
chosen per macroblock by the same cost, with a cheap screen in front of the
full test.

The cost also has a psychovisual term. It's on by default at strength 2.0,
which is twice the reference encoder's. The term rewards a block for keeping
the source's texture energy and trades some pixel accuracy for visible
detail. The PSNR-Y floor
on the [results page](results.html) watches that trade, since PSNR-Y measures
how far each pixel ends up from the source. `--tune psnr` turns the term off.

## The SIMD tier

SIMD instructions do the same arithmetic on many pixels at once. We write them
as intrinsics, C functions that map one for one onto the instructions, instead
of hand assembly. The compiler then schedules instructions and allocates
registers, which is where hand assembly can still win a kernel. Every kernel is
checked against the plain C version of the same routine and benchmarked through
checkasm.

| tier | where it runs | coverage |
|---|---|---|
| NEON | ARM, including the Apple silicon development machine | about 4,000 lines in five files |
| SSE4.2, AVX2 | x86-64, picked at run time | the same kernel families as NEON |
| AVX-512 | x86-64 chips that have it, included in every x86-64 build | pixel comparisons. They beat AVX2 on both an AMD and an Intel machine. |
| SVE | Linux ARM servers with 256-bit SVE, such as AWS Graviton3 | replaces the widest NEON kernels |

The [SIMD notes](https://github.com/terranvigil/yah264/blob/main/docs/simd.md)
have the plan.

## Decoder

The tree has a decoder. It exists to verify the encoder by decoding our own
output for the bitstream validation above. It has never been benchmarked and
has no standalone command. No number on this site is a decode number. Making it
fast is a separate track in the
[project plan](https://github.com/terranvigil/yah264/blob/main/docs/plan.md)
that hasn't started.
