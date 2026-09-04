# The shared Metal compute library boundary

Design decision record. This refines the shared-compute-library note in
`docs/gpu-acceleration-plan.md` into a concrete library boundary: which kernels
are shared, what the ABI looks like, and where the code lives. It covers G1
(lookahead/analysis), G2 (full-frame ME), G3 (GPU VMAF), and G5 (sub-pel
interpolation for search). It is an architecture doc and no code ships with it.
The implementer starts from the "what this locks" paragraph at the end.

C prefix **`ngc_`** (next GPU compute). Consumers keep
their own prefixes (yah264's is `y264_`). The library gets its own because it
belongs to no encoder.

## The scope line

One rule draws it: **the library computes pixels-in, numbers-out on raw planes.
It never sees codec state.** No reconstruction buffers, no syntax elements, no
rate models, no reference-list semantics. Everything it returns is advisory
(costs, vectors, scores) and the consuming encoder either refines it with exact
CPU math or could recompute it entirely. Nothing that can reach reconstruction
lives in the library, which is also why nothing in it ever needs to match a
decoder bit for bit.

In scope:

| Kernel family | Feeds | How shared is the math? |
|---|---|---|
| Downscale (2x2 box; Lanczos-3) | G1 lookahead; hull cross-res scoring | Identical across codecs |
| Block cost maps: SAD, SATD (4x4/8x8 Hadamard), variance + mean per block | G1, AQ, shot features, scene cut inputs | Identical |
| Low-res block motion search (best MV + cost per block vs a reference plane) | G1; seeds G2 | Identical given parameters |
| Candidate evaluation: score a caller-supplied MV candidate list per block, with sub-pel samples built by a separable filter bank | G2, G5 | Shared kernel skeleton, per-codec filter tables |
| VMAF feature extraction (VIF, ADM, motion) + model evaluation | G3, VMAF-targeted RC, bench | Codec-free entirely |

Out of scope, permanently per-encoder: exact MC/transform/quant/recon, which the
parent plan's dividing line already keeps on the CPU. Search orchestration policy
too: MVP/merge seeding, early-exit heuristics, and mode decision are codec
judgment, not plane math. MV rate models stay out beyond the data table described
below. The scene-cut, AQ, and mb-tree decision math stays in each codec because
it is cheap scalar arithmetic over the returned maps and it is exactly the tuning
each codec wants to own. yah264's version is `la_chain_prop` and the cut logic
in `src/encoder/encoder.c`, and it stays there. Compound prediction (OBMC, warped
motion, wedge) and codec-specific bi-prediction rounding stay out. So do G4
batched transforms, which the parent plan expects SME to win and which are
codec-specific integer math anyway.

To state the "shared" claim precisely: downscale, the cost maps, and VMAF are
literally the same math in every block-based encoder. The low-res search is the
same given parameters. Sub-pel interpolation only *looks* shared: the separable
N-tap filter skeleton is common, but the filter banks, phase counts, and rounding
rules differ per codec, and non-translational modes do not fit the skeleton at
all. That is handled in the next section.

## The parameterization seam

Pressure-tested against H.264, HEVC and AV1, the axes that actually vary are:

- **Analysis block size.** Lookaheads work on 8x8 blocks of half-res luma, the
  x264 lineage yah264 implements in `lowres_analyse`. A CTU- or
  superblock-based consumer aggregates those maps up to its own grid, but
  aggregation is caller-side summing, not a kernel concern. The parameter is
  block dims from {4, 8, 16}, and output grid geometry derives from them.
- **SATD transform size.** 4x4 vs 8x8 Hadamard. Two kernel variants behind one
  job field.
- **Search shape.** Pattern (diamond, hex, exhaustive window), range, iteration
  cap. Plain data.
- **Scoring metric.** SAD vs SATD per job. A field.
- **Sub-pel filters.** H.264 uses a 6-tap half-pel filter plus bilinear
  quarter-pel, HEVC an 8-tap quarter-pel, and AV1 an 8-tap with per-block
  selectable filter sets and eighth-pel chroma. Parameterized as coefficient
  tables registered at session open, plus a per-block filter index in the job.
  Still data.
- **MV cost.** The rate model differs per codec and evolves. The library takes an
  optional caller-filled cost table indexed by component distance from the
  predictor, plus a lambda, and only does lookup-and-add. Table contents are
  codec semantics; table lookup is shared math. A codec whose rate model is not
  component-separable approximates, and the mainstream ones are separable-ish in
  practice.
- **Bit depth.** 8-bit today; 10-bit consumers will want more. This is *not* a
  job parameter. It is pipeline specialization at session creation via Metal
  function constants, so a session opens at a fixed depth and the library ships
  an 8-bit and a 16-bit compiled variant of each kernel family.
- **Reference structure.** Never enters the library, by design. Every job names
  explicit plane pairs, while list construction, reference ordering, and temporal
  distances stay in the codec. This one decision kills the largest class of
  would-be parameters.

The descriptor set that falls out, described here and pinned in `ngc.h` by the
implementer:

- `ngc_config` (per session): max frame dims, bit depth, filter banks to
  register, VMAF model(s) to load, which pipeline families to build.
- `ngc_plane`: buffer handle + byte offset, width, height, stride. Planes carry
  no frame semantics (no POC, no type).
- `ngc_job`: a kind tag (scale, costmap, search, candeval, vmaf) plus a per-kind
  parameter block as above. Every job names its input planes and one output
  buffer with a documented, versioned layout.

**Where the "it's all parameters" claim leaks**, ranked by cost if ignored:

1. **Sub-pel and compound prediction.** The library does translational separable
   filtering only. OBMC, warp, and wedge candidates, and each codec's exact
   bi-pred rounding, are different algorithms, not parameter values. Search-side
   approximation with translational filters is standard practice, which is what
   production lookaheads do, so G1/G2 survive fine. But a full G2 for a codec
   with compound modes will eventually want those candidates scored on the GPU,
   and no parameter struct gets there. Escape hatch: the library exposes kernel
   registration, so a codec can supply a private `.metal` function and a job-kind
   descriptor and ride the library's buffers, batching, and harness. The leak
   becomes a plug-in, not a fork. Resist using the hatch until a measured need
   forces it.
2. **The intra-cost proxy.** yah264 uses DC-per-quadrant SATD (`blk8_intra`),
   and a richer predictor set is arguable for other codecs. Fix one shared proxy
   anyway: DC plus horizontal, vertical, and a planar term on the 8x8, declared
   heuristic, calibrated per codec with a scalar. Lookahead-grade intra estimates
   are not sensitive to predictor count, and if one codec proves otherwise, that
   is a custom kernel via the hatch.
3. **Bit depth** is a build-time specialization, not a runtime knob, which is
   fine but is not "just a parameter" in the descriptor-struct sense. Budget the
   second pipeline family when a consumer needs it, not before.
4. **The MV cost table shape** is committed by the library as
   component-separable. That is a mild approximation for a reference-MV stack,
   and the CPU refinement stage absorbs it.

## Interface, ownership, and the language boundary

The header `ngc.h` is pure C11: opaque handles, no Metal or Objective-C types
leak through it. The implementation is Objective-C (`.m` files) wrapping the
Metal API, compiled only on Apple platforms. Plain ObjC over ObjC++ keeps C++ out
of the toolchain entirely, matching the encoder's C11-plus-asm convention. C
callers link the static library like any other.

Handles and lifetimes:

- **`ngc_ctx`**, one per process. Owns the `MTLDevice`, the compiled pipeline
  states, registered filter banks, and loaded VMAF models. `ngc_open(&config)`
  returns NULL when Metal is unavailable, and that NULL *is* the presence test
  (see dispatch below). Thread-safe for creating streams and buffers.
- **`ngc_stream`**, one per encoder instance or GOP worker. Owns an
  `MTLCommandQueue`. Batches build and submit on a stream, and a stream is
  single-threaded by contract. Streams let GOP-parallel workers submit
  independently without a shared lock. Whether many streams beat one whole-title
  analysis pass is a contention measurement, and the API supports both answers so
  the measurement can decide.
- **`ngc_buf`**, a library-allocated `MTLBuffer` with shared storage.
  `ngc_buf_ptr` returns the CPU pointer, valid for the buffer's life. The library
  allocates because no-copy wrapping of caller memory requires page-aligned
  allocations the encoders' allocators do not guarantee, and one rule ("pixels
  the GPU will read live in ngc buffers") is cheaper than a staging copy.
  Concretely in yah264, the lookahead push already memcpys the padded plane and
  lowres into `struct la_entry` (`src/encoder/encoder.c`), so redirecting those
  copies' destinations into ngc buffers costs nothing extra.
- **Batch and fence.** A batch accumulates jobs on a stream; submit returns an
  `ngc_fence` (wrapping command-buffer completion). Callers wait or poll. The
  synchronization contract is one sentence: **inputs are immutable from submit
  until the fence signals, and outputs are unreadable until it does.** On unified
  memory with shared storage, fence completion is the coherency point; no blit,
  no copy-back. Ship a debug mode that poisons buffers on submit to catch
  violations.

The intended pipelining is simple: the codec submits frame N+k's analysis batch
at lookahead-push time and waits on frame N's fence at consume time, giving k
frames of slack. That maps directly onto yah264's existing lookahead ring
(`la[64]` / `la_depth` in `src/encoder/encoder.h`), so the GPU stays ahead of the
CPU without any new scheduling machinery.

Results come back as flat arrays in the job's output buffer (per-block cost maps,
MV fields, per-frame scalars), with layouts documented per job kind and versioned
so the harness and every consumer read the same bytes the same way.

## Dispatch and fallback

The GPU does not join the CPU kernel dispatch tables, and that is deliberate.
`y264_cpu_detect` flags (NEON, DOTPROD, I8MM in `src/common/cpu.h`) select
synchronous per-block function pointers via `y264_pixel_init` into the `y264_dsp`
table. The GPU is the wrong shape for that table: it is a batch-async,
module-level backend, not a drop-in `sad[]` entry. So "the library is present"
means one thing to a caller: `ngc_open` succeeded at encoder open, and the
encoder holds the context (e.g. an `e->gpu` handle). A param flag exposes it (off
/ on / auto), defaulting to off until the parent plan's measurements flip it to
auto.

Build contract: a meson option (`-Dgpu=auto|enabled|disabled`), auto meaning
enabled on darwin/aarch64 and disabled elsewhere. When disabled, `ngc.h` still
compiles and the library provides stubs where `ngc_open` returns NULL, so call
sites need no `#ifdef`s. The conformance gate, CI baseline, and non-Apple builds
never depend on Metal, and every consumer keeps its CPU implementation complete
and default. In yah264 that means the `lowres_analyse` path in `encoder.c` stays
as-is, and the GPU path fills the same per-MB arrays (`lr_intra`, `lr_inter`,
`lr_mvx`, `lr_mvy`, or their `la_entry` equivalents) from readback, so everything
downstream (`la_chain_prop`, scene cut, mb-tree) is backend-blind.

Runtime fallback: a batch can fail (device lost, timeout, allocation). The
library reports per-batch status and never aborts, so the codec recomputes the
affected frames on CPU and keeps going. GPU-on must degrade to GPU-off, never to
a broken encode.

## The correctness-harness contract

The library includes **its own portable C reference implementation of every
kernel**, mirroring the `y264_pixel_init_c` convention the encoder already uses
for checkasm. That single decision is what lets one harness validate every
consumer: the harness (living in the library repo, checkasm-style like
`tools/checkasm/checkasm.c`) compares GPU output against the library's C
reference on randomized and corpus inputs, with no codec involved. Each codec
then only has to establish once, at integration, that its CPU analysis path
agrees with the library's C reference. Better: where they compute the same thing,
adopt the library's C reference *as* the codec's CPU fallback and delete the
duplicate. yah264's `blk8_intra`/`blk8_inter`/`downscale` predate the library,
so first integration asserts equivalence, then collapses them.

Tolerance tiers, per kernel kind:

| Tier | Kernels | Assertion |
|---|---|---|
| Exact | SAD, SATD, variance/mean sums, box downscale | Bit-identical to the C reference. Integer accumulation on the GPU is exact; these kernels are *required* to accumulate in integer regardless of any fp16 experiments elsewhere. |
| Bounded fp | VMAF features and score, Lanczos scaler, any fp cost | Per-kernel absolute tolerance, documented in the header. VMAF score bound set against libvmaf on the bench corpus (start at ±0.05 per frame, tighten from measurement). |
| Cost regret | Motion search, candidate eval | Never assert MV equality (ties and traversal order make it meaningless). Assert cost(GPU winner) <= cost(C-reference winner) x (1 + eps), evaluated with the exact C metric. Start eps at 1%. |
| End-to-end | Whole encoder | GPU-on vs GPU-off BD-rate on VMAF within a bound (start at 0.25%) per codec, in each codec's bench harness (`bench/bench.py` pattern). The library enables this by making GPU purely a runtime switch. |

What the library must expose to be testable: deterministic output for identical
input and config (fixed traversal order, deterministic reduction trees, no
non-associative atomics), the documented and versioned output layouts, a kernel
enumeration plus a version string for bench logging, and the C reference itself
as linkable functions.

**NOTE:** determinism here matters beyond testing. yah264's GOP-parallel
determinism story (the reason `la_chain_prop` stops at IDRs) has to survive
GPU-on. Run-to-run identical GPU output is achievable in Metal if reductions
avoid non-associative atomic accumulation; commit to it as a hard
kernel-authoring rule, not a nice-to-have.

## Where the library lives

The library is its own repository, with its own git history, its own
harness, and its own CI. Each consumer takes it as a meson subproject through a
`subprojects/<library>.wrap` pinning a revision. The pinned SHA is the point: it
records exactly which library version each encoder validated against, each repo
bumps when it chooses to, and the repos keep evolving independently. On the dev
machine, iteration can point the wrap at the sibling checkout directly.

Rejected alternatives. Vendored copies are the failure mode this whole doc exists
to prevent: silently diverging copies of the GPU kernels. An unpinned sibling
include has no version pinning, so a kernel change motivated by one consumer
silently changes another's validated behavior. A git submodule would work and
pins the same way, but every consumer already builds with meson, and a wrap does
the identical pinning with less checkout friction.

Inside the library repo: `include/ngc.h`, `src/*.m`, `kernels/*.metal`, `ref/*.c`
for the C reference, and `tools/gpucheck` for the harness. The `.metal` sources
compile at build time with `xcrun metal` into a metallib embedded as a byte array
in the static library, so encoders do not ship a loose file next to their
binaries. Runtime compilation from source stays available as a dev mode. License
and style follow the encoder: BSD-2-Clause, C11 conventions.

## Left open, on purpose

These are measurement questions or implementer calls. The boundary above does not
move whichever way they go.

- **fp16 vs integer/fp32 in search scoring.** Integer SAD accumulation is exact
  and comparable to the NEON path, but fp16 may be faster via simdgroup
  operations. Measure both. The cost-regret and BD-rate checks absorb either
  answer. The parent plan flags fp16-misleading-sub-pel as a risk.
- **GPU contention under GOP-parallel load.** Streams-per-worker vs one
  whole-title analysis pass. The API supports both. Decide from throughput and
  powermetrics under the real GOP-parallel CLI.
- **Live-mode latency.** Batch depth k, and whether GPU ME stays VOD-only.
  Nothing in this boundary assumes an answer.
- **10-bit timing.** The 16-bit pipeline family is designed in (session-level
  depth specialization) but should not be built until a consumer needs it.
- **GPU VMAF's reporting role.** Whether GPU VMAF replaces libvmaf in bench
  reporting or only feeds in-loop RC. Keep libvmaf for published cross-encoder
  numbers until the GPU port proves inside its tolerance on the corpus; the v1
  model situation is already delicate.
- **The custom-kernel hatch's first use.** Registration is designed in. Do not
  use it until a measured leak, most likely compound candidate eval, forces it.
- **The Lanczos-3 scaler's home for hull scoring.** It is off the encode critical
  path, so only port it to Metal if hull scoring dominates ladder-mode wall time.

## What this locks and what it deliberately leaves open

Locked: the scope line (pixels-in/numbers-out on raw planes, five kernel
families, codec state never crosses), the descriptor seam including its four
named leaks and the custom-kernel escape hatch, the `ngc_` C11 ABI with
ctx/stream/buf/fence ownership exactly as above (library-allocated shared
buffers, submit-to-fence immutability), presence-equals-`ngc_open`-succeeding
with stubbed non-Apple builds and a complete default CPU path, the
library-owns-its-own-C-reference testing model with the four tolerance tiers and
hard run-to-run determinism, and the separate-repo-plus-meson-wrap layout. Open:
every numeric threshold (the tolerance and BD-rate bounds are starting values,
tightened by measurement), fp16 usage, stream topology under contention,
live-mode gating, 10-bit build timing, GPU VMAF's reporting role, and all
kernel-internal design (threadgroup shapes, tiling, reduction strategy), which
the implementer owns entirely so long as determinism and the output layouts hold.
