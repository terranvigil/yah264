---
title: Design - yah264
description: The pipeline, the threading model, rate control, and the conformance gate.
---

# Design

## The conformance gate

Everything else on this page is a choice I made. This one is a rule, and it is
the reason the rest can be trusted.

Every frame yah264 reconstructs must be bit-exact against an independent
decoder. The gate is a script you run, and no push hook runs it for you. The
test CI runs on manual dispatch, because Actions minutes are metered, and the
site build is the one job that runs on push. The discipline is that the gate
runs before a change is called done. The encoder writes its own reconstruction
with `--dump-recon`, ffmpeg's H.264 decoder decodes the same bitstream, and the
two must be identical byte for byte, across every clip in the suite and a range
of QPs. The encode is lossy, so the decode does not match the input. The match
that matters is decoder output against encoder reconstruction.

This is worth the trouble because of what an encoder is. The encoder contains a
whole decoder, and every frame predicts from the reconstruction. A single bit of
drift between the two sides compounds. The next frame predicts from something
the decoder does not have, and the error grows
until the picture falls apart. Drift does not announce itself in a quality
metric either. It looks like a slightly worse encode until it looks like a
broken one.

## The pipeline

yah264 is a staged many-core pipeline, not a macroblock loop with threads bolted
on. Each stage owns one kind of decision, and the stages are where parallelism
is expressed.

The lookahead runs first. It makes a downscaled analysis pass over a window of
future frames to decide frame types, detect scene cuts, and build the
macroblock-tree propagation data that later stages spend. Nothing downstream can
be smarter than what the lookahead saw.

Behind it, per-frame work runs GOP-parallel while rows within a frame run as a
wavefront, with a lock-free pool feeding both. Output is byte-identical across
runs at a fixed thread count. That matters more than it sounds: a determinism
failure and a correctness failure look identical from the outside, and only one
of them is a bug in the coding tools. The thread count is part of the
configuration. `--threads 1` is its own mode, and its output differs from two
threads and up.

## Threading

Asking an encoder for every core can make it slower, because a picture can only
absorb so much concurrency before the coordination costs more than the work. A
wavefront over a CIF frame runs out of independent rows quickly. A 1080p frame
has more to give.

The thread count is chosen in two steps. Auto resolves to the smaller of the
online core count and 16. The resolved number is then capped by what the picture
can absorb: 12 for CIF, 21 for 720p, 32 for 1080p. Only the CIF cap ever bites
under auto, since auto is already at or below 16. The other two matter when a
caller asks for a count explicitly, and we honour that and then clamp it.

The 16 is a conservative default, and nobody measured a knee there. Wavefront
scaling is bounded by the picture's critical path long before it is bounded by
the machine, and on an asymmetric machine the last few workers end up on
efficiency cores and lengthen that path outright. So the ceiling is a number
that is safe on an unknown box. The measurement behind it is CIF-sized: 18
threads there read 6 to 11% worse than 8, because the extra workers contend for
a diagonal that cannot feed them. We have not measured a machine with more than
18 cores.

Occupancy is also why the speed numbers need reading carefully. On foreman_cif
yah264 fills around 8.8 cores where x264 fills 5.8. That is where that clip's
sub-parity row comes from. Held to one thread each, the advantage is gone and
the same clip reads slower. Part of the speed picture is per-unit efficiency and
part is occupancy, and they only separate when both are measured.

The [interactive threading page](threading.html) walks the model with live
diagrams.

## Rate control

CQP, CRF, single-pass ABR, CBR and VBV, capped VBR, capped CRF and two-pass all
ship. Capped CRF is the one that gets used most, and it is also the one with the
most interesting mechanism.
[How video encoding works](encoding.md) covers what each mode is for. This page
covers where ours puts the intelligence.
Macroblock-tree is on by default. The lookahead works out which blocks later
frames will predict from and gives them a finer quantizer, because a bit spent
on a block that fifty frames inherit is worth more than one nothing references.
Variance adaptive quantization runs alongside it, moving bits toward flat areas
where quantization shows first. The mode decision's lambda is modulated per
macroblock from the same signal, and that is a good deal of the quality
difference against x264.

Under capped CRF the encoder codes to the quality target, and the buffer can
only take bits away. Where the ceiling never bites anywhere in the stream you
get the CRF encode you asked for, bit for bit, at the same thread count. A cap
that bites once changes everything after it. Every GOP after the first then
opens on a half-full buffer, which costs a few bits on its own. Where the
ceiling is tight, a per-frame budget pulls the buffer back toward half full.
Nothing else under CRF watches the bit count, so without that budget the buffer
would drain until every prediction error became an underflow.

The compliance gate is six clips by three caps by both VBV paths, and it passes
29 of those 36 cells. The reference encoder passes all 18 cells its own feature
set covers, so the two counts don't sit against each other as one ratio. The
seven we fail are tight-cap and mid-stream scene-cut cells, tracked in
`docs/rate-control.md`.

**Across shots.** The base QP under CRF is flat per frame type. Between shots it
moves with the frame mean of the per-macroblock offsets. That mean holds the
mb-tree boost and the adaptive-quantisation energy term. We apply the energy
term's frame mean at full strength on the anchor's base QP, and B frames inherit
it through the cascade. A busy shot then codes a little cheaper and a quiet one
a little better, the way x264's constant-quality mode behaves. The term's
within-frame part keeps the gentler strength that won on single-shot clips. On
file input `--cut-split` puts an IDR on every cut the pre-scan finds, and
`--shot-crf` gives each shot its own CRF from the shot table. Numbers on the
results page.

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

The cost function also has a psychovisual term, on by default at strength 2.0.
It rewards a block for keeping the source's texture energy, on top of minimising
error. I say that out loud because it runs at twice the strength the reference
does, and because it trades pixel accuracy for apparent detail. That is exactly
the trade a VMAF-scored board is least likely to notice, and the PSNR-Y floor on
the [results page](results.html) is what watches for it. `--tune psnr` turns it
off.

## The SIMD tier

The SIMD path is about 2,800 lines of NEON intrinsics across five files. Every
kernel is validated against the C reference and benchmarked through checkasm. We
chose intrinsics over hand-written assembly, a deliberate trade with a known
cost: the compiler schedules instructions and allocates registers, and that is
where x264's assembly still wins.

## Decoder

The tree already contains a decoder, and it exists to verify the encoder. It
decodes our own output for the conformance gate above and has never been
benchmarked as a decoder. There is no standalone decode CLI, and no number on
this site is a decode number. Making it fast is a separate track that has not
started. `docs/decoder-speed-plan.md` is the plan.
