---
title: Design - yah264
description: The pipeline, the threading model, rate control, and the conformance gate.
---

# Design

## The conformance gate

Every frame yah264 produces has to come back out of somebody else's decoder
identical, byte for byte. That is the one hard rule in the project, and it is
why the choices further down this page can be trusted.

An encoder keeps its own copy of the decoded picture, called the
reconstruction, because later frames are predicted from it. `--dump-recon`
writes ours out. ffmpeg's H.264 decoder then decodes the same bitstream. The
two have to match exactly, on every clip in the suite, over a range of
quantizer settings. The quantizer decides how much detail gets rounded away.
Encoding is lossy, so neither file matches the original video, and the
comparison that matters is decoder output against encoder reconstruction.

The gate is a script you run, and no push hook runs it for you. The test CI
runs on manual dispatch, because Actions minutes are metered, and the site
build is the one job that fires on a push. So the discipline is human, and the
rule is that the gate runs before a change is called done.

That trouble is worth taking because of what an encoder is. An encoder
contains a whole decoder inside it, and every frame it codes is predicted from
the reconstruction that decoder produced. One wrong bit does not stay one
wrong bit. The next frame predicts from something the real decoder never had,
and the error grows from there until the picture falls apart. Drift will not
show up in a quality score either, since it looks like a slightly worse encode
right up until it looks like a broken one.

## The pipeline

yah264 is a staged many-core pipeline. The more common design is a single loop
over the blocks of a frame with threads bolted on afterwards, and that one runs
out of room quickly. Here each stage owns one kind of decision, and the stages
are where the parallelism is expressed.

The lookahead runs first. It makes a cheap analysis pass over a window of
future frames, working on a shrunken copy of the picture. It picks each frame's
type and finds the scene cuts. It also builds the macroblock-tree data that
later stages spend. That data records how much later frames will depend on each
block. A macroblock is a 16 by 16 square of pixels that the encoder decides
about as a unit. Nothing downstream can be smarter than what the lookahead saw.

Behind the lookahead, whole frames encode in parallel where the GOP structure
allows it. A GOP is the repeating run of frames between one key frame and the
next. Inside a single frame the rows advance as a wavefront, so a row can only
start once the row above it is far enough ahead to have given it the
neighbouring blocks it needs. One lock-free pool feeds both kinds of work.

Output is byte-identical across runs at a fixed thread count. That matters more
than it sounds. A determinism failure and a correctness failure look identical
from the outside, and only one of them is a bug in the coding tools. The thread
count is part of the configuration. `--threads 1` is its own mode. Its output
differs from what two threads and up produce.

## Threading

Asking an encoder for every core on the box can make it slower, because a
picture can only absorb so much concurrency before the coordination costs more
than the work it buys. A wavefront over a CIF frame runs out of independent
rows quickly, since CIF is only 352 by 288 pixels. A 1080p frame has more to
give.

The thread count is chosen in two steps. Auto resolves to the smaller of the
online core count and 16. The resolved number is then capped by what the
picture can absorb: 12 for CIF, 21 for 720p, 32 for 1080p. Only the CIF cap
ever bites under auto, since auto is already at or below 16. The other two
matter when a caller asks for a count explicitly. We honour the request and
then clamp it.

The 16 is a conservative default, and nobody measured a knee there. Wavefront
scaling is bounded by the picture's critical path long before it is bounded by
the machine. The critical path is the longest chain of work that has to happen
in order. On a machine with two kinds of core, the last few workers end up on
the slow ones and lengthen that chain outright. So the ceiling is a number that
is safe on an unknown box. The measurement behind it is CIF-sized. 18 threads
there read 6 to 11% worse than 8, because the extra workers contend for a
diagonal that cannot feed them. We have not measured a machine with more than
18 cores.

The two encoders also differ in how many cores they keep busy, and on
foreman_cif yah264 fills around 8.8 cores where x264 fills 5.8. That is where
that clip's sub-parity row comes from. Held to one thread each, the advantage
is gone and the same clip reads slower.

The [interactive threading page](threading.html) walks the model with live
diagrams.

## Rate control

Rate control decides how many bits each frame gets. CQP, CRF, single-pass ABR,
CBR and VBV, capped VBR, capped CRF and two-pass all ship. Capped CRF is the
one that gets used most, and it is also the one with the most interesting
mechanism. [How video encoding works](encoding.md) covers what each
mode is for. This page covers where ours puts the intelligence.

Macroblock-tree is on by default. The lookahead works out which blocks later
frames will predict from and gives them a finer quantizer. A bit spent on a
block that fifty frames inherit is worth more than one nothing references.
Variance adaptive quantization runs alongside it and moves bits toward flat
areas of the picture. Quantization shows up there first. The mode decision's
lambda is modulated per macroblock from the same signal. Lambda is the exchange
rate between bits and error that the encoder uses when it picks between two
ways of coding a block. That per-block modulation is a good deal of the quality
difference against x264.

Under capped CRF the encoder codes to the quality target, and the buffer can
only take bits away. The buffer is a model of what a decoder could have
received by the time it needs each frame. Respecting it is what keeps a stream
playable on a real player. Where the ceiling never bites anywhere in the stream
you get the CRF encode you asked for, bit for bit, at the same thread count. A
cap that bites once changes everything after it. Every GOP after the first then
opens on a half-full buffer, which costs a few bits on its own. Where the
ceiling is tight, a per-frame budget pulls the buffer back toward half full.
Nothing else under CRF watches the bit count. Without that budget the buffer
would drain until every prediction error became an underflow.

The compliance gate is six clips by three caps by both VBV paths. It passes 29
of those 36 cells. The reference encoder passes all 18 cells its own feature
set covers, so the two counts don't sit against each other as one ratio. The
seven we fail are tight-cap and mid-stream scene-cut cells. They are tracked in
[rate-control.md](https://github.com/terranvigil/yah264/blob/main/docs/rate-control.md).

**Across shots.** The base quantizer under CRF is flat per frame type. Between
shots it moves with the frame mean of the per-macroblock offsets. That mean
holds the macroblock-tree boost and the adaptive quantization energy term. We
apply the energy term's frame mean at full strength on the anchor frame's base
QP. B frames are predicted from both an earlier and a later picture, and they
inherit the shift through the cascade of offsets under the anchor. A busy shot
then codes a little cheaper and a quiet one a little better, the way x264's
constant-quality mode behaves. The term's within-frame part keeps the gentler
strength that won on single-shot clips. On file input `--cut-split` puts an IDR
on every cut the pre-scan finds, so nothing after a cut predicts across it.
`--shot-crf` gives each shot its own CRF from the shot table. Numbers on the
[results page](results.html).

## Mode decision and motion estimation

Mode decision is the encoder choosing how to carve up a macroblock and how to
predict each piece. Every candidate partitioning is costed as distortion plus
lambda times rate, and the cheapest wins. Three things separate encoders.

- Which candidates are worth trying.
- How accurately the rate term is estimated. The entropy coder that turns
  decisions into real bits has not run yet at that point.
- Where the search is allowed to stop early.

Motion estimation is the hunt for where a block moved from in an earlier
picture. The search runs a diamond, hexagon or uneven multi-hexagon pattern
depending on the preset, and each of those walks a different arrangement of
test points around a starting guess. The guess comes from the vectors of
neighboring blocks. Subpel refinement follows at the level the preset sets, and
it tests positions that fall between whole pixels. Full trellis RDOQ runs over
both transform sizes, and it picks each quantized coefficient with the bit cost
of that choice already in hand. The transform size itself is chosen per
macroblock by the same cost, with a cheap screen in front of the full test.

The cost function also has a psychovisual term, on by default at strength 2.0.
It rewards a block for keeping the source's texture energy, on top of
minimising error. I say that out loud because it runs at twice the strength the
reference does, and because it trades pixel accuracy for apparent detail. The
PSNR-Y floor on the [results page](results.html) is what watches for that
trade. PSNR-Y measures how far each pixel ended up from the source.
`--tune psnr` turns the term off.

## The SIMD tier

SIMD instructions do the same arithmetic on many pixels at once, and our SIMD
path is about 2,800 lines of NEON intrinsics across five files. NEON is the ARM
form of those instructions. Intrinsics are C functions that map onto them one
for one. Every kernel is validated against the plain C version of the same
routine and benchmarked through checkasm. We chose intrinsics over hand-written
assembly. That trade has a known cost: the compiler schedules instructions and
allocates registers, and that is where x264's assembly still wins.

## Decoder

The tree already contains a decoder, and it exists to verify the encoder. It
decodes our own output for the conformance gate above and has never been
benchmarked as a decoder. There is no standalone decode CLI, and no number on
this site is a decode number. Making it fast is a separate track that has not
started. [decoder-speed-plan.md](https://github.com/terranvigil/yah264/blob/main/docs/decoder-speed-plan.md) is the plan.
