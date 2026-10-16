# GPU and matrix-unit acceleration plan

Status: several premises here have moved. The GPU lookahead (G1) was measured
and closed: any Metal-armed path pays a 12-17 ms per-process bring-up floor,
which is device init and not shaders (`docs/what-shipped.md`, the "Refused"
row, and the `Y264_GPU_PHASEA` row in `docs/instruments.md`). SME/ZA was
closed on every form too. The lookahead was built inside
`src/encoder/encoder.c` with its own half-resolution downscale, so the
`src/encoder/lookahead.c` and `src/dsp/scale.c` named below were never
created. C3's scalar gaps are filled: deblock and the forward and inverse
transforms have NEON kernels (`src/dsp/loopfilter_neon.c`,
`src/dsp/transform_neon.c`). NEON is no longer the only SIMD in use: there
are SVE kernels in `src/dsp/arm/` and SSE4.2, AVX2 and AVX-512 kernels in
`src/dsp/x86/`.

This plan covers offloading parts of yah264 to the M5 Max GPU (Metal) and
matrix units (SME/SME2). Its comparison protocol measures each option's speed
and quality effect, so we keep the ones that pay.

## When to start

Gate: full x264 feature parity. This is a performance and hardware track. It
does not compete for time with the coding tools that close the parity gap.
Starting earlier risks tuning a GPU path against an encoder whose hot spots
are still moving. Once the CPU profile settles, the offload targets stop
shifting and the measurements mean something.

One piece is worth pulling forward. The lookahead module (G1 below) is also a
prerequisite for shot-based encoding (`docs/shot-based-plan.md`) and adaptive
quantization. Build the lookahead CPU-first, then add the Metal path here.
Give its motion and complexity kernels a clean dispatch boundary, so the GPU
version drops in as a backend and needs no rewrite.

## The dividing line

Block-based encoding splits cleanly into a parallel analysis half and a serial
bitstream half. The split decides what the GPU can touch.

The GPU can accelerate anything that produces decisions or estimates and does
not feed the bit-exact reconstruction loop: motion vectors, cost maps, scene
boundaries, complexity scores, quality metrics. Floating-point rounding on the
GPU is fine here, because the CPU redoes anything that enters reconstruction
with exact integer math. A wrong-by-a-little motion vector from the GPU just
seeds a CPU refinement. It never desyncs a decoder.

The GPU cannot help the serial core. Intra prediction reads the reconstructed
block above and to the left, so blocks resolve in raster order as one
dependency chain. CABAC is strictly serial. Each bin's arithmetic state
depends on the previous one, which is also what makes it hard in hardware
decoders. RD mode decision threads predicted motion vectors from neighbors. All of this forms the
bitstream and stays on the CPU with NEON and SME.

Two facts about the M5 Max shape everything below. First, unified memory
means the CPU and GPU share physical RAM. A handoff is a shared buffer and
cache coherency instead of a PCIe copy. That is why GPU-assisted software
encoding is worth trying here when it mostly was not on desktop. Second, the
chip has both a Metal GPU and the scalable matrix extension. For several
kernels the real question is not "CPU or GPU" but "GPU, SME, or NEON," decided
by batch size and latency tolerance.

Amdahl bounds the whole effort. Motion estimation and lookahead are a large
slice of encode time, maybe 40 to 60% depending on preset. Offloading them
also frees CPU cores for more GOP-parallel work. Nothing here speeds up CABAC
or reconstruction, though, so the ceiling is a solid multiplier and no order
of magnitude. Measure against that expectation.

Keep the fixed-function media engine out of scope. The M5 Max has a hardware
H.264/HEVC encode block that VideoToolbox drives. Using it means not running
yah264 at all, which defeats the project, since yah264 exists to make better
encoder decisions than that ASIC does. This plan uses GPU compute and matrix units to
make the software encoder faster. It does not replace the encoder.

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
the offloaded stage, not on the whole encode. The C-series are CPU vector and
matrix options. They are included because the experiment is a head-to-head.
For a kernel that both a GPU and an SME version can compute, we want the
measured winner.

## GPU offload options

### G1. Lookahead and analysis on Metal

This is the highest-value target and the one to build first. A lookahead pass
runs ahead of the encode on downscaled frames. Per frame it produces
low-resolution motion vectors, an intra-versus-inter cost split, a scene-cut
flag, and per-block complexity and variance for adaptive quantization and shot
analysis. None of this enters reconstruction, so it is safe to compute in GPU
floating point. It also tolerates latency. It runs several frames ahead of the
encode, so a whole batch of frames dispatches at once and the round-trip cost
amortizes.

Mechanism: a Metal compute pipeline takes the luma plane (or a half-res
downscale of it). It runs a small-window motion search against the previous
frame on 8x8 blocks and reduces per-block SAD/SATD with SIMD-group operations. The
downscale itself runs on the GPU. Output is a per-frame struct of cost maps and
flags read back into the shared buffer.

Hook points: this is a new `src/encoder/lookahead.c` (see the shot-based plan,
which specs the CPU version and the scene-cut math). The GPU path is a second
backend behind the same interface, selected at runtime when Metal is available.
The scaler is new. No downscaler exists in the tree today, so G1 brings one
(`src/dsp/scale.c`, CPU and Metal versions).

Benefit: large on the lookahead stage. It composes with shot-based encoding
and AQ, so the same pass feeds three features. Effort: large. It is the first
Metal code in the project and drags in the compute pipeline, buffer
management, and a correctness harness.

Correctness: the lookahead is heuristic, so "correct" means the GPU cost maps and
scene cuts agree with the CPU reference within tolerance, not bit-exactly. Add
a checkasm-style comparison that runs both backends on the same frames. It
asserts the vectors and flags match closely enough that mode decision picks
the same path most of the time.

### G2. Full-frame motion estimation on Metal

Motion estimation is the single largest CPU cost. Integer motion search is
close to embarrassingly parallel. Each block and each candidate vector is
independent, and the inner loop is SAD or SATD over a window, which the GPU
computes densely in fp16. The win comes from batching the search for a whole
frame at once instead of per macroblock. That fits a lookahead-driven design
and avoids paying dispatch latency inside the tight per-MB loop.

Mechanism: for each block in the frame, evaluate a search pattern (or a dense
window in a quality mode) against the reference. Reduce SAD per candidate with
SIMD-group reductions and return the best vector and cost. Seed from the
lookahead vectors (G1) and from predicted neighbor vectors to shrink the search.
The CPU then refines around the GPU's best vector with exact integer SAD and
takes over for mode decision.

Hook points: today motion search is `y264_me_search` in `src/encoder/motion.c`,
which returns a SAD-based cost with an MV-rate lambda bias. The GPU path replaces
the search-and-return with "GPU proposes, CPU refines and finalizes." Keep the
CPU search as the fallback and as the refinement stage. The GPU produces
candidates, not final vectors.

Benefit: large on the ME stage. Effort: large, mostly in writing and tuning
the Metal search kernels and the CPU/GPU overlap. Risk: for low-latency or
live encoding the round-trip may cost more than it saves. Gate the GPU ME
behind an offline/VOD path and keep CPU ME for low-latency.

### G3. GPU VMAF

VMAF is expensive and parallel. Several per-pixel and per-block feature maps
(visual information fidelity, detail loss, a motion term) are reduced and fed
to a trained model. Computing it on the GPU serves two ends. It unblocks a
VMAF-targeted rate-control mode that steers per-frame QP to hit a quality
target without stalling the CPU encode. It also speeds up the bench harness,
which runs VMAF on every comparison encode.

Mechanism: port the VMAF feature extractors to Metal compute kernels, run the
small model on the reduced features, and read back the score. The upsample
step for cross-resolution comparison (decode, scale to 1080p, score) also runs
on the GPU. That matters for the shot-based convex-hull work, which scores
many resolutions.

Hook points: `bench/bench.py` already measures VMAF (v1 if available, else
v0.6.1 plus NEG). The GPU VMAF is a new compute path used both by the bench
harness and, later, by an in-encoder VMAF-targeted RC mode. It touches no
contended files.

Benefit: medium. It enables VMAF-targeted RC more than it speeds up the raw
encode. Effort: medium, mostly the feature-extractor port and matching the
reference VMAF within a small tolerance so the numbers stay comparable to
published results.

### G4. Bulk transform and SATD on Metal

List it, test it, expect to reject it. The 4x4 and 8x8 integer transforms and the
Hadamard SATD are parallel across blocks, so they can run on the GPU. They are
small kernels in the middle of the sequential loop, though. Dispatching them
to the GPU pays round-trip latency on work that a CPU vector unit finishes in
a handful of cycles. The transforms are also integer by design, while the
GPU's advantage is in float, so there's no precision headroom to exploit. SME
(C1) will probably win these decisively. Run the measurement anyway, so the
rejection rests on evidence. A future batched-transform design (all blocks of
a frame at once, outside the per-MB loop) might also change the answer.

### G5. Sub-pel interpolation on Metal

Half and quarter-pel motion compensation runs a 6-tap filter to build sub-pixel
reference samples. It's a real MC hot path. On the GPU it pairs with G2. When
the search evaluates sub-pel candidates, interpolate them on the GPU in the
same pass instead of bouncing back to the CPU. Standalone it has the same
latency problem as G4, so pursue it only as part of the GPU ME pipeline. Today
the 6-tap luma interpolation and eighth-pel chroma live in `src/dsp/interp.c` as
the reference. The Metal version mirrors it for the search side only. The CPU
keeps the exact version for final reconstruction.

## CPU matrix and SIMD options

These compete with the GPU for specific kernels. Include them in the comparison
so each kernel goes to its measured-best unit.

### C1. SME/SME2 transforms and SATD

The scalable matrix extension suits the 4x4/8x8 transform butterflies and the
Hadamard SATD. It runs in the CPU's execution stream with no dispatch latency,
the property G4 lacks. Runtime detection already reports SME on this
hardware. Only NEON is used today. Build SME kernels for the
transforms and SATD and add a dispatch path in `y264_dsp_init`. Expect C1 to beat
G4 for the in-loop transforms and to be the right home for them.

### C2. i8mm / dotprod SAD

The M-series has i8mm and dot-product instructions, and motion-search SAD maps
onto UDOT well. This is the CPU-side counterpart to G2. In the CPU refinement
stage, and in low-latency mode where GPU ME is off, UDOT-based SAD speeds the
search without leaving the CPU. The effort is small, a focused kernel swap in
the existing `sad[]` dispatch table. Measure C2 against G2 to find the crossover
window size where the GPU starts winning.

### C3. NEON gap-fill

Several kernels are still scalar C: the deblock filter (`filter_line`), the DCT
butterflies (quant/dequant are NEON, the transform is not), and parts of MC.
Vectorizing them with NEON is independent of the GPU work. It lifts the CPU
baseline that everything else is measured against, so do it whatever the GPU
outcome. A GPU win measured against a slow scalar baseline isn't a real win.

## Hybrid pipeline design

The target architecture runs the GPU and CPU as an overlapped pipeline, not a
call-and-wait. The GPU stays a few frames ahead. It runs lookahead (G1) and, in
VOD mode, full-frame motion estimation (G2), and writes cost maps and
candidate vectors into shared buffers. The CPU consumes those buffers and
refines with exact integer math (C2). It then runs RD mode decision,
reconstruction and CABAC, and produces the bitstream. The two block on each
other only at the shared buffer boundary. Unified memory keeps that boundary
cheap.

This composes with the existing GOP-parallel threading in the CLI. Each GOP
worker can share one GPU analysis stream, or the GPU can run analysis for the
whole title once and feed all workers. Measure GPU contention under the
GOP-parallel load to decide.

## How to run the comparison

This plan exists to gather evidence. Every option ships behind a flag and is
measured the same way.

Metrics per option, per preset, per content class:

- Throughput: encoded frames per second, wall-clock.
- Latency: per-frame encode latency, which matters for the live-mode verdict.
- Quality: BD-rate on VMAF and PSNR against the CPU baseline at matched settings.
 Offloaded analysis should be quality-neutral or close, since the CPU finalizes
 everything. A quality drop means the GPU seeds are steering mode decision
 wrong.
- Power: watts and joules-per-encode via powermetrics. The M-series efficiency
 story is part of the pitch, and the GPU may win or lose on energy
 independently of speed.
- Freed CPU: utilization headroom the offload opens for more GOP-parallel work.

Test matrix: each option on and off, plus the combinations that make sense
(G1+G2, G1+G2+C2), across the bench corpus resolutions and a spread of content
(low motion, high motion, grain, screen content). The bench harness
(`bench/bench.py`) already records VMAF/PSNR/size/wall-time to history.csv and
renders progression. Extend it to log the new dimensions (power, latency, freed
CPU) and the active option set per row.

Correctness harness: GPU kernels need a validation path like checkasm has for CPU
kernels. For bit-exact kernels (any that could enter reconstruction) assert exact
match against the C reference. For heuristic kernels (ME, lookahead, VMAF) assert
agreement within tolerance. More important, assert that the final encode
decisions and BD-rate stay within a set bound of the CPU-only encode.

Report format: one comparison table per encoder generation. It shows each
option's throughput, BD-rate and power deltas against the CPU/NEON baseline,
so the keep/drop decision per option is one row.

## A shared compute library

Build the Metal analysis kernels (downscale, motion search, SATD reduction,
VMAF) as a shared compute library instead of encoder-private code. The math is
nearly identical across block-based codecs. Only the block sizes and search
patterns differ, and those are parameters. With a shared library, a second
consumer inherits the GPU path for the cost of parameter tuning. The
comparison harness runs unchanged. Design G1 and G2 with that reuse in mind
from the start. The library boundary is specified in
`docs/gpu-shared-library-design.md`.

The parallel-versus-serial divide has the same shape in every block-based codec,
so the plan's structure transfers. Analysis and metrics go to the GPU or SME.
The entropy coder and reconstruction stay serial on the CPU. What changes per
codec is how much there is to offload. Larger coding units and deeper
partition trees give the GPU analysis more to chew on. The serial floor does
not move, because every modern entropy coder is serial the way CABAC is.

## Open questions and risks

- Live-mode latency: GPU round-trip may sink G2 for low-latency encoding. Keep
 CPU ME (C2) as the low-latency path and gate GPU ME to VOD until measured.
- GPU contention: the GPU is also wanted for VMAF (G3) and the display. Under
 GOP-parallel encoding, several workers plus VMAF may oversubscribe it. Measure.
- fp16 precision in ME: half-precision SAD is fine for a coarse search but may
 mislead sub-pel refinement. The CPU refine stage should absorb this. Confirm
 it doesn't cost BD-rate.
- Effort front-loading: G1 drags in the entire Metal toolchain (compute
 pipelines, shared buffers, validation). Budget that once. G2, G3 and G5 are
 cheaper after it lands.
- Portability: this is Apple-Silicon-specific. Keep the CPU/NEON path complete
 and default so non-Apple builds and the correctness baseline never depend on
 Metal or SME.
