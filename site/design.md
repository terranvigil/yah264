---
title: Design - yah264
description: The pipeline, the threading model, rate control, and the conformance gate.
---

# Design

## The conformance gate

Everything else on this page is a choice I made. This one is a rule, and it is
the reason the rest can be trusted.

Every frame yah264 reconstructs must be bit-exact against an independent
decoder. The gate is a script rather than a push hook: CI runs on manual
dispatch, because Actions minutes are metered, so the discipline is that the
gate runs before a change is called done. The encoder writes its own
reconstruction with `--dump-recon`, ffmpeg's H.264 decoder decodes the same
bitstream, and the two must be identical byte for byte, across every clip in the
suite and a range of QPs. The encode is lossy, so the decode does not match the
input. What must match is decoder output against encoder reconstruction.

This is worth the trouble because of what an encoder is. The encoder contains a
whole decoder, and every frame predicts from the reconstruction. A single bit of
drift between the two sides compounds. The next frame predicts from something
the decoder does not have, and the error grows
until the picture falls apart. Drift does not announce itself in a quality
metric either. It looks like a slightly worse encode until it looks like a
broken one.

## The pipeline

yah264 is a many-core pipeline in the style of SVT. Work moves through stages
instead of looping over macroblocks with threads bolted on. Each stage owns one kind of
decision, and the stages are where parallelism is expressed.

The lookahead runs first. It makes a downscaled analysis pass over a window of
future frames to decide frame types, detect scene cuts, and build the
macroblock-tree propagation data that later stages spend. Nothing downstream can
be smarter than what the lookahead saw.

Behind it, per-frame work runs GOP-parallel while rows within a frame run as a
wavefront, with a lock-free pool feeding both. Output stays deterministic for a
given configuration, which matters more than it sounds: a determinism failure
and a correctness failure look identical from the outside, and only one of them
is a bug in the coding tools.

## Threading

Asking an encoder for every core can make it slower, and the reason is that a
picture can only absorb so much concurrency before the coordination costs more
than the work. A wavefront over a CIF frame runs out of independent rows quickly.
A 1080p frame has more to give.

The thread count is chosen in two steps. Auto resolves to the smaller of the
online core count and 16, and that resolved number is then capped by what the
picture can absorb: 12 for CIF, 21 for 720p, 32 for 1080p. The 16 is the more
interesting half. Past it the coordination costs more than the extra workers
return, so asking for 32 threads on a 32-core machine is slower than asking for
16.

Occupancy is also why the speed numbers need reading carefully. On foreman_cif
yah264 fills around 8.8 cores where x264 fills 5.8, which is where that clip's
sub-parity row comes from. Held to one thread each, the advantage is gone and
the same clip reads slower. Part of the speed picture is per-unit efficiency and
part is occupancy, and they only separate when both are measured.

The [interactive threading page](threading.html) walks the model with live
diagrams.

## Rate control

CQP, CRF, single-pass ABR, CBR and VBV, capped VBR, capped CRF and two-pass all
ship. Capped CRF is the one that gets used most, and it is also the one with the
most interesting mechanism.
[How video encoding works](encoding.md) covers what each mode is for. What is
specific to this encoder is where the intelligence sits.
Macroblock-tree is on by default. The lookahead works out which blocks later
frames will predict from and gives them a finer quantizer, because a bit spent
on a block that fifty frames inherit is worth more than one nothing references.
Variance adaptive quantization runs alongside it, moving bits toward flat areas
where quantization shows first. The mode decision's lambda is modulated per
macroblock from the same signal, and that is a good deal of the quality
difference against x264.

Under capped CRF the encoder codes to the quality target, and the buffer can
only take bits away. Where the ceiling is slack you get the CRF encode you asked
for, bit for bit. Where it
is tight, a per-frame budget pulls the buffer back toward half full. Nothing
else under CRF watches the bit count, so without that budget the buffer would
drain until every prediction error became an underflow. The compliance gate
passes 29 of its 36 cells.

## Mode decision and motion estimation

Every candidate partitioning of a macroblock is costed as distortion plus
lambda times rate, and the cheapest wins. Three things separate encoders.

- Which candidates are worth trying.
- How accurately the rate term is estimated before the entropy coder has run.
- Where the search is allowed to stop early.

Motion search runs diamond, hexagon or uneven multi-hexagon depending on the
preset, seeded from the vectors of neighboring blocks. Subpel refinement
follows at the level the preset sets. Full trellis RDOQ runs over both transform
sizes, and the transform size itself is chosen per macroblock by RD with a cheap
screen in front of it.

## The SIMD tier

The SIMD path is about 2,800 lines of NEON intrinsics across five files, with
every kernel validated against the C reference and benchmarked through a
checkasm harness. Choosing intrinsics over hand-written assembly is a deliberate
trade with a known cost: the compiler schedules instructions and allocates
registers, which is where x264's assembly still wins.

## Decoder

This is in progress.
