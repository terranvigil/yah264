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
whole decoder, and every frame predicts from the reconstruction, never from
the source. A single bit of drift between the two sides compounds: the next
frame predicts from something the decoder does not have, and the error grows
until the picture falls apart. Drift does not announce itself in a quality
metric either. It looks like a slightly worse encode until it looks like a
broken one.

The gate had a hole I did not spot for a long time. `conformance.sh` pins its encodes to a single
thread, so the job pool owns the parallelism. That kept
the check deterministic and left threaded reconstruction untested, which is
exactly where a race would live. Two further instruments close it: a recon
comparator that runs at real thread counts, and a threaded recon gate that runs
the same assertion with the pool wide open.

## The pipeline

yah264 is a many-core pipeline in the style of SVT. Work moves through stages
instead of looping over macroblocks with threads bolted on. Each stage owns one kind of
decision, and the stages are where parallelism is expressed.

The lookahead sits in front. It runs a downscaled analysis pass over a window of
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
yah264 fills around 8.5 cores where x264 fills 5.9, which is where that clip's
sub-parity row comes from. Held to one thread each, the advantage is gone and
the same clip reads slower. Part of the speed picture is per-unit efficiency and
part is occupancy, and they only separate when both are measured.

The [interactive threading page](threading.html) walks the model with live
diagrams.

## Rate control

CQP, CRF, single-pass ABR, CBR and VBV, capped VBR, capped CRF and two-pass all
ship. Capped CRF is the one that gets used most, and it is also the one with the
most interesting mechanism, so it gets the room here.
[How video encoding works](encoding.md) covers what each mode is for. What is
specific to this encoder is where the intelligence sits.

Macroblock-tree is on by default. It uses the lookahead's propagation data to
work out which blocks later frames will predict from, and protects those blocks
by lowering their quantizer, because a bit spent on a block that fifty frames
inherit is worth more than a bit spent on one that nothing references. Variance
adaptive quantization runs alongside it, moving bits toward flat areas where
quantization shows first.

Under capped CRF the quality target drives the encode and the buffer only ever
subtracts. Where the ceiling is slack the output is bit-for-bit the CRF encode
that was asked for. Where it is tight, a per-frame budget derived from x264's
target-fill goal pulls the buffer back toward half full: generous above the
halfway mark, and collapsing to half of one frame's arrival below it, so the
buffer climbs back at a bounded rate. That budget is what makes the mode work at
all, because nothing else under CRF looks at bits, and a rate factor whose
natural bitrate sits above the cap would otherwise drain the buffer unopposed
and then run pinned at the boundary where every prediction error is an
underflow. With the budget in place the compliance gate passes 34 of its 36
cells.

The lambda that the mode decision runs on is modulated per macroblock from that
same mb-tree signal. That modulation is where a good deal of the quality
difference against x264 comes from, and it is documented with the
measurements that decided it.

## Mode decision and motion estimation

The search is a tournament. Each candidate partitioning of a macroblock is
costed as distortion plus lambda times rate, and the cheapest wins. What
separates encoders is which candidates are worth trying, how honestly the rate
term is estimated before the entropy coder has run, and where the search is
allowed to stop early.

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

The C path is never allowed to rot. SIMD is enabled on runtime detection, the
pure-C tier is boarded separately as its own goal, and a C-only win is not
reverted when a SIMD version of the same kernel lands.

## There is no decoder yet

Worth stating plainly, because an encoder project is often assumed to carry one.
The independent second opinion in the test suite is ffmpeg's decoder, not ours.
What exists in this tree is a CABAC decoder and the recon-match path, and they
are there to verify reconstruction rather than to decode a stream: no standalone
decode mode, no CLI, never benchmarked as a decoder.

A real decoder would be a separate binary, and it is planned rather than
started. Nothing on the encoder side waits on it.
