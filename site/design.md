---
title: Design - yah264
description: Validating the bitstream, the pipeline, the threading model and rate control.
---

# Design

## Validating the bitstream

Every change has to pass one test, the conformance gate. It checks that an
independent decoder reads our output the same way yah264 does.

An encoder keeps a decoded copy of each frame, the reconstruction, and predicts
later frames from it. The test writes that copy out with `--dump-recon`,
decodes the output file with ffmpeg's H.264 decoder, and requires the two to
match bit for bit, on every frame of every test clip, across a range of quality
settings. Neither matches the source video. They only have to match each other.

`make conformance` runs it locally, and CI runs it on every push that touches
code.

The bar is exact because errors compound. If our copy of a frame differs from a
real decoder's by one bit, the next frame is predicted from a picture the
decoder never had, and the error grows until the picture breaks. A quality score
misses it early: drift looks like a slightly worse encode until it looks like a
broken one.

## The pipeline

Most encoders loop over the blocks of a frame and add threads afterwards. That
design runs out of room quickly. yah264 is a pipeline of stages instead. Each stage
owns one kind of decision, and the parallelism lives between the stages.

The lookahead runs first, on a shrunken copy of each picture over a window of
future frames. It picks frame types and finds scene cuts. It also builds the
macroblock-tree data, which records how much later frames will depend on each
16 by 16 block, or macroblock. Nothing downstream can be smarter than what the lookahead saw.

Behind it, whole frames encode in parallel where the GOP structure allows. A GOP
is the run of frames from one keyframe to the next. Inside a frame the rows move
as a wavefront: a row starts once the row above it is far enough ahead to supply
the neighboring blocks it needs. One lock-free pool feeds both kinds of work.

At `--threads 1`, two runs of the same encode produce the same bytes. We test
that. With more threads they may not, across runs or across thread counts.
Holding it there would rule out optimizations, and x264 doesn't promise it
either. What we do promise is that every stream decodes to exactly the picture
the encoder reconstructed.

## Threading

Giving an encoder every core can make it slower. A picture absorbs only so much
concurrency before coordination costs more than the work it buys. A CIF frame
is 352 by 288 pixels, so its wavefront runs out of independent rows quickly. A
1080p frame has far more to give.

So the thread count is picked in two steps. Auto takes the smaller of the core
count and 16. Then the picture caps it: 12 threads for CIF, 21 for 720p, 32 for
1080p. Under auto only the CIF cap ever bites. The others apply when a caller
asks for a count, which we honor and then clamp.

Nobody measured a knee at 16. It's a default that's safe on an unknown machine,
because wavefront scaling hits the picture's critical path, the longest chain of
work that has to run in order, well before it runs out of cores. On a chip with
two kinds of core, the last workers land on the slow ones and lengthen that
chain. The one measurement behind the default is at CIF, where 18 threads read
6 to 11% worse than 8. We haven't measured a machine with more than 18 cores.

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
take bits away. It models what a decoder could have received by the time it
needs each frame, so respecting it keeps a stream playable. If the cap
never bites, you get exactly the CRF encode you asked for at the same thread
count. Once it bites, everything after it changes: every later GOP opens on a
half-full buffer, which costs a few bits. Where the cap is tight, a per-frame
budget pulls the buffer back toward half full. Nothing else under CRF watches
the bit count. Without that budget the buffer would drain until a prediction
error became an underflow.

The buffer compliance test runs six clips at three caps through both VBV paths.
We pass 29 of its 36 cells. The seven we fail are tight caps and mid-stream
scene cuts, tracked in
[rate-control.md](https://github.com/terranvigil/yah264/blob/main/docs/rate-control.md).
The reference encoder passes all 18 cells its own features cover, so the two
counts aren't one ratio.

Across shots, the base quantizer under CRF is flat per frame type and moves with
the frame mean of the per-macroblock offsets. That mean holds the
macroblock-tree boost and the adaptive quantization's energy term. We apply the
energy term's frame mean at full strength on the anchor frame's base QP. B
frames predict from an earlier and a later picture, so they inherit the shift
through the offsets under the anchor. A busy shot then codes a little cheaper and a quiet one a little better. The
term's within-frame part keeps the gentler strength that won on single-shot
clips.

On file input, `--cut-split` puts an IDR on every cut the pre-scan finds, so
nothing after a cut predicts across it. `--shot-crf` gives each shot its own
CRF from the shot table. The [results page](results.html) has the numbers.

## Mode decision and motion estimation

Mode decision is how the encoder carves up a macroblock and predicts each
piece. Every candidate is costed as distortion plus lambda times rate. The
cheapest wins. Encoders differ in three places:

- which candidates are worth trying;
- how well they estimate the rate, since the entropy coder that turns decisions
  into bits hasn't run yet;
- where the search may stop early.

Motion estimation looks for where a block came from in an earlier picture. The
search starts from a guess built from the neighboring blocks' vectors and walks
a diamond, hexagon or uneven multi-hexagon pattern, depending on the preset.
Subpixel refinement then tests positions between whole pixels, as far as the
preset allows. Full trellis RDOQ runs over both transform sizes and picks each
quantized coefficient knowing what it costs in bits. The transform size is
chosen per macroblock by the same cost, with a cheap screen in front of the
full test.

The cost also has a psychovisual term, on by default at strength 2.0, twice the
reference encoder's. It rewards a block for keeping the source's texture
energy, so it trades some pixel accuracy for visible detail. The PSNR-Y floor
on the [results page](results.html) watches that trade, since PSNR-Y measures
how far each pixel ends up from the source. `--tune psnr` turns the term off.

## The SIMD tier

SIMD instructions do the same arithmetic on many pixels at once. We write them
as intrinsics, C functions that map one for one onto the instructions, rather
than hand assembly. The cost is that the compiler schedules instructions and
allocates registers. That's where x264's assembly still wins. Every kernel
is checked against the plain C version of the same routine and benchmarked
through checkasm.

NEON, the ARM form, came first because Apple silicon is the development
machine. It's about 4,000 lines in five files. The same kernel families now
exist for SSE4.2 and AVX2 on x86-64, picked at run time. AVX-512 kernels cover
the pixel comparisons. They beat AVX2 on both an AMD and an Intel machine, so
every x86-64 build carries them and uses them where the chip has AVX-512. On
Linux ARM servers with 256-bit SVE, such as AWS Graviton3, SVE kernels take
over the widest NEON ones. [x86-plan.md](https://github.com/terranvigil/yah264/blob/main/docs/x86-plan.md)
has the plan and the order the kernel families shipped in.

## Decoder

The tree has a decoder. It exists to verify the encoder, by decoding our own
output for the bitstream validation above. It has never been benchmarked and has no
standalone command, so no number on this site is a decode number. Making it
fast is a separate track that hasn't started.
[decoder-speed-plan.md](https://github.com/terranvigil/yah264/blob/main/docs/decoder-speed-plan.md)
is the plan.
