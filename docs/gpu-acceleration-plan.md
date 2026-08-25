# GPU and matrix-unit acceleration plan

Plan for offloading parts of next264 to the M5 Max GPU (Metal) and matrix units
(SME/SME2), with a comparison protocol that measures each option's speed and
quality effect so the ones that pay can be kept.

## When to start

Gate: full x264 feature parity. This is a performance and hardware track, not a
compression-tool track, so it does not compete for time with the coding tools
that close the parity gap. Starting earlier risks tuning a GPU path against an
encoder whose hot spots are still moving. Once the CPU profile settles, the
offload targets stop shifting and the measurements mean something.

The one exception worth pulling forward: the lookahead module (G1 below) is also
a prerequisite for shot-based encoding (`docs/shot-based-plan.md`) and adaptive
quantization. Build the lookahead CPU-first, then add the Metal path here. Design
the lookahead so its motion and complexity kernels have a clean dispatch
boundary, so the GPU version is a drop-in backend rather than a rewrite.

## The dividing line

Block-based encoding splits cleanly into a parallel analysis half and a serial
bitstream half, and the split decides what the GPU can touch.

The GPU can accelerate anything that produces decisions or estimates and does
not feed the bit-exact reconstruction loop: motion vectors, cost maps, scene
boundaries, complexity scores, quality metrics. Floating-point rounding on the
GPU is fine here, because the CPU redoes anything that enters reconstruction
with exact integer math. A wrong-by-a-little motion vector from the GPU just
seeds a CPU refinement; it never desyncs a decoder.

The GPU cannot help the serial core. Intra prediction reads the reconstructed
block above and to the left, so blocks resolve in raster order, one dependency
chain. CABAC is strictly serial: each bin's arithmetic state depends on the
previous one, the same property that makes it hard in hardware decoders. RD mode
decision threads predicted motion vectors from neighbors. All of this forms the
bitstream and stays on the CPU with NEON and SME.

Two facts about the M5 Max shape everything below. Unified memory means the
CPU and GPU share physical RAM, so a handoff is a shared buffer and cache
coherency, not a PCIe copy; this is why GPU-assisted software encoding is worth
trying here and mostly was not on desktop. And the chip has both a Metal GPU and
the scalable matrix extension, so for several kernels the real question is not
"CPU or GPU" but "GPU, SME, or NEON," decided by batch size and latency
tolerance.

Amdahl bounds the whole effort. Motion estimation and lookahead are a large
slice of encode time, maybe 40 to 60% depending on preset, and offloading them
also frees CPU cores for more GOP-parallel work. But nothing here speeds up
CABAC or reconstruction, so the ceiling is a solid multiplier, not an order of
magnitude. Measure against that expectation, not against a fantasy.

Keep the fixed-function media engine out of scope. The M5 Max has a hardware
H.264/HEVC encode block that VideoToolbox drives. Using it means not running
next264 at all, which defeats the project, since next264 exists to make better
encoder decisions than that ASIC does. This plan is about using GPU compute and
matrix units to make the software encoder faster, not replacing it.

## Options catalog

| ID | Option | Accelerates | Benefit | Effort | Depends on |
|---|---|---|---|---|---|
| G1 | Lookahead + analysis on Metal | scene cut, complexity, low-res ME | speed L | L | new lookahead module, scaler |
| G2 | Full-frame motion estimation on Metal | integer + sub-pel ME | speed L | L | Metal compute path, ME refactor |
| G3 | GPU VMAF | VMAF-targeted RC, bench harness | speed M | M | Metal, VMAF port |
| G4 | Bulk transform/SATD on Metal | forward DCT, Hadamard SATD | speed S? | M | Metal; likely rejected, test it |
| G5 | Sub-pel interpolation on Metal | 6-tap MC for ME candidates | speed S-M | M | G2 |
| C1 | SME/SME2 transforms + SATD | 4x4/8x8 transform, Hadamard | speed M | L | SME kernels, dispatch |
| C2 | i8mm / dotprod SAD | motion-search SAD | speed M | S | UDOT kernels |
| C3 | NEON gap-fill | deblock, DCT butterflies, MC | speed M | M | none (extends existing NEON) |

Benefit and effort are S/M/L. "speed L" means a large expected throughput win on
the offloaded stage, not on the whole encode. The C-series are CPU vector/matrix
options included because the experiment is a head-to-head: for a kernel that both
a GPU and an SME version can compute, we want the measured winner, not a guess.

## GPU offload options

### G1. Lookahead and analysis on Metal

The highest-value target and the one to build first. A lookahead pass runs ahead
of the encode on downscaled frames and produces, per frame: low-resolution
motion vectors, an intra-versus-inter cost split, a scene-cut flag, and per-block
complexity and variance for adaptive quantization and shot analysis. None of this
enters reconstruction, so it is safe to compute in GPU floating point, and it is
latency-tolerant because it runs several frames ahead of the encode, so a whole
batch of frames dispatches at once and the round-trip cost amortizes.

Mechanism: a Metal compute pipeline that takes the luma plane (or a half-res
downscale of it), runs a small-window motion search against the previous frame
on 8x8 blocks, and reduces per-block SAD/SATD with SIMD-group operations. The
downscale itself runs on the GPU. Output is a per-frame struct of cost maps and
flags read back into the shared buffer.

Hook points: this is a new `src/encoder/lookahead.c` (see the shot-based plan,
which specs the CPU version and the scene-cut math). The GPU path is a second
backend behind the same interface, selected at runtime when Metal is available.
The scaler is new; no downscaler exists in the tree today, so G1 brings one
(`src/dsp/scale.c`, CPU and Metal versions).

Benefit: large on the lookahead stage, and it composes with shot-based encoding
and AQ, so the same pass feeds three features. Effort: large, because it is the
first Metal code in the project and drags in the compute pipeline, buffer
management, and a correctness harness.

Correctness: the lookahead is heuristic, so "correct" means the GPU cost maps and
scene cuts agree with the CPU reference within tolerance, not bit-exactly. Add a
checkasm-style comparison that runs both backends on the same frames and asserts
the vectors and flags match closely enough that mode decision picks the same
path most of the time.

### G2. Full-frame motion estimation on Metal

Motion estimation is the single largest CPU cost, and integer motion search is
close to embarrassingly parallel: independent per block, independent per
candidate vector, and the inner loop is SAD or SATD over a window, which the GPU
computes densely in fp16. The win comes from batching the search for a whole
frame at once rather than per-macroblock, which fits a lookahead-driven design
and avoids paying dispatch latency inside the tight per-MB loop.

Mechanism: for each block in the frame, evaluate a search pattern (or a dense
window in a quality mode) against the reference, reduce SAD per candidate with
SIMD-group reductions, and return the best vector and cost. Seed from the
lookahead vectors (G1) and from predicted neighbor vectors to shrink the search.
The CPU then refines around the GPU's best vector with exact integer SAD and
takes over for mode decision.

Hook points: today motion search is `n264_me_search` in `src/encoder/me.c`,
which returns a SAD-based cost with an MV-rate lambda bias. The GPU path replaces
the search-and-return with "GPU proposes, CPU refines and finalizes." Keep the
CPU search as the fallback and as the refinement stage; the GPU produces
candidates, not final vectors.

Benefit: large on the ME stage. Effort: large, dominated by writing and tuning
the Metal search kernels and the CPU/GPU overlap. Risk: for low-latency or live
encoding the round-trip may cost more than it saves, so gate the GPU ME behind an
offline/VOD path and keep CPU ME for low-latency.

### G3. GPU VMAF

VMAF is expensive and parallel: several per-pixel and per-block feature maps
(visual information fidelity, detail loss, a motion term) reduced and fed to a
trained model. Computing it on the GPU serves two ends. It unblocks a
VMAF-targeted rate-control mode that steers per-frame QP to hit a quality target
without stalling the CPU encode, and it speeds up the bench harness itself, which
runs VMAF on every comparison encode.

Mechanism: port the VMAF feature extractors to Metal compute kernels, run the
small model on the reduced features, and read back the score. The upsample step
for cross-resolution comparison (decode, scale to 1080p, score) also runs on the
GPU, which matters for the shot-based convex-hull work that scores many
resolutions.

Hook points: `bench/bench.py` already measures VMAF (v1 if available, else
v0.6.1 plus NEG). The GPU VMAF is a new compute path used both by the bench
harness and, later, by an in-encoder VMAF-targeted RC mode. It touches no
contended files.

Benefit: medium; it is an enabler for VMAF-targeted RC more than a raw encode
speedup. Effort: medium, mostly the feature-extractor port and matching the
reference VMAF within a small tolerance so the numbers stay comparable to
published results.

### G4. Bulk transform and SATD on Metal

List it, test it, expect to reject it. The 4x4 and 8x8 integer transforms and the
Hadamard SATD are parallel across blocks, so they can run on the GPU. But they
are small kernels in the middle of the sequential loop, so dispatching them to
the GPU pays round-trip latency on work that a CPU vector unit finishes in a
handful of cycles. The transforms are also integer by design, and the GPU's
advantage is in float, so there's no precision headroom to exploit. The likely
outcome is that SME (C1) wins these decisively. Run the measurement anyway so the
rejection is evidence-based, and because a future batched-transform design (all
blocks of a frame at once, outside the per-MB loop) might change the answer.

### G5. Sub-pel interpolation on Metal

Half and quarter-pel motion compensation runs a 6-tap filter to build sub-pixel
reference samples, and it's a real MC hot path. On the GPU it pairs with G2:
when the search evaluates sub-pel candidates, interpolate them on the GPU in the
same pass instead of bouncing back to the CPU. Standalone it has the same
latency problem as G4, so only pursue it as part of the GPU ME pipeline, not as
an isolated kernel. Today the 6-tap luma interpolation and eighth-pel chroma live
in `src/dsp/mc.c` as the reference; the Metal version mirrors it for the search
side only, while the CPU keeps the exact version for final reconstruction.

## CPU matrix and SIMD options

These compete with the GPU for specific kernels. Include them in the comparison
so each kernel goes to its measured-best unit.

### C1. SME/SME2 transforms and SATD

The scalable matrix extension suits the 4x4/8x8 transform butterflies and the
Hadamard SATD, and it runs in the CPU's execution stream with no dispatch
latency, which is exactly the property G4 lacks. Runtime detection already
reports SME on this hardware; only NEON is used today. Build SME kernels for the
transforms and SATD and add a dispatch path in `n264_dsp_init`. Expect C1 to beat
G4 for the in-loop transforms and to be the right home for them.

### C2. i8mm / dotprod SAD

The M-series has i8mm and dot-product instructions, and motion-search SAD maps
onto UDOT well. This is the CPU-side counterpart to G2: for the CPU refinement
stage and for low-latency mode where GPU ME is off, UDOT-based SAD speeds the
search without leaving the CPU. Small effort, since it's a focused kernel swap in
the existing `sad[]` dispatch table. Measure C2 against G2 to find the crossover
window size where the GPU starts winning.

### C3. NEON gap-fill

Several kernels are still scalar C: the deblock filter (`filter_line`), the DCT
butterflies (quant/dequant are NEON, the transform is not), and parts of MC.
Vectorizing them with NEON is independent of the GPU work and lifts the CPU
baseline that everything else is measured against. Do this regardless of the GPU
outcome. A GPU win measured against a slow scalar baseline isn't a real win, so
lift the CPU baseline first.

## Hybrid pipeline design

The target architecture runs the GPU and CPU as an overlapped pipeline, not a
call-and-wait. The GPU stays a few frames ahead running lookahead (G1) and, in
VOD mode, full-frame motion estimation (G2), writing cost maps and candidate
vectors into shared buffers. The CPU consumes those buffers, refines with exact
integer math (C2), runs RD mode decision, reconstruction, and CABAC, and
produces the bitstream. The two never block on each other except at the shared
buffer boundary, and unified memory keeps that boundary cheap.

This composes with the existing GOP-parallel threading in the CLI: each GOP
worker can share one GPU analysis stream, or the GPU can run analysis for the
whole title once and feed all workers. Decide that by measuring GPU contention
under the GOP-parallel load.

## How to run the comparison

The point of this plan is evidence, so every option ships behind a flag and gets
measured the same way.

Metrics per option, per preset, per content class:

- Throughput: encoded frames per second, wall-clock.
- Latency: per-frame encode latency, which matters for the live-mode verdict.
- Quality: BD-rate on VMAF and PSNR against the CPU baseline at matched settings.
 Offloaded analysis should be quality-neutral or close, since the CPU finalizes
 everything; a quality drop means the GPU seeds are steering mode decision wrong.
- Power: watts and joules-per-encode via powermetrics, since the M-series
 efficiency story is part of the pitch and the GPU may win or lose on energy
 independently of speed.
- Freed CPU: utilization headroom the offload opens for more GOP-parallel work.

Test matrix: each option on and off, plus the combinations that make sense
(G1+G2, G1+G2+C2), across the bench corpus resolutions and a spread of content
(low motion, high motion, grain, screen content). The bench harness
(`bench/bench.py`) already records VMAF/PSNR/size/wall-time to history.csv and
renders progression; extend it to log the new dimensions (power, latency, freed
CPU) and the active option set per row.

Correctness harness: GPU kernels need a validation path like checkasm has for CPU
kernels. For bit-exact kernels (any that could enter reconstruction) assert exact
match against the C reference. For heuristic kernels (ME, lookahead, VMAF) assert
agreement within tolerance and, more importantly, assert that the final encode
decisions and BD-rate stay within a set bound of the CPU-only encode.

Report format: one comparison table per encoder generation showing each option's
throughput delta, BD-rate delta, and power delta against the CPU/NEON baseline,
so the keep/drop decision per option is one row.

## A shared compute library

Build the Metal analysis kernels (downscale, motion search, SATD reduction,
VMAF) as a shared compute library, not encoder-private code. The math is nearly
identical across block-based codecs; only the block sizes and search patterns
differ, and those are parameters. A shared library means a second consumer
inherits the GPU path for the cost of parameter tuning rather than a rewrite, and
the comparison harness runs unchanged. Design G1 and G2 with that reuse in mind
from the start. The library boundary is specified in
`docs/gpu-shared-library-design.md`.

The parallel-versus-serial divide has the same shape in every block-based codec,
so the plan structure transfers: analysis and metrics go to the GPU or SME, the
entropy coder and reconstruction stay serial on the CPU. What changes per codec
is how much there is to offload. Larger coding units and deeper partition trees
give the GPU analysis more to chew on; the serial floor does not move, because
every modern entropy coder is serial the way CABAC is.

## Open questions and risks

- Live-mode latency: GPU round-trip may sink G2 for low-latency encoding. Keep
 CPU ME (C2) as the low-latency path and gate GPU ME to VOD until measured.
- GPU contention: the GPU is also wanted for VMAF (G3) and the display. Under
 GOP-parallel encoding, several workers plus VMAF may oversubscribe it. Measure.
- fp16 precision in ME: half-precision SAD is fine for a coarse search but may
 mislead sub-pel refinement; the CPU refine stage should absorb this, but
 confirm it doesn't cost BD-rate.
- Effort front-loading: G1 drags in the entire Metal toolchain (compute
 pipelines, shared buffers, validation). Budget that once; G2, G3, G5 are
 cheaper after it lands.
- Portability: this is Apple-Silicon-specific. Keep the CPU/NEON path complete
 and default so non-Apple builds and the correctness baseline never depend on
 Metal or SME.
