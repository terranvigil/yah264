# GPU offload

yah264 encodes on the CPU. An optional Metal offload can take part of the
lookahead's lowres motion search, using a separate compute library called
nextgpu. It's off by default at build time and at run time because it doesn't
pay. Any encode that arms it pays a 12 to 17 ms Metal bring-up per process
that the offloaded work can't win back. The code stays in the tree as an
experiment you can re-run.

This page covers GPU compute inside the software encoder. Apple's
fixed-function encoder replaces the core instead and is covered in
[Hardware mode](hardware-mode.md).

## How it works

### What the GPU can and can't touch

Only the lookahead and motion-search side can move to the GPU. An H.264
encoder splits into two halves:

- The analysis half produces estimates: lowres motion vectors, cost maps,
  scene-cut inputs and complexity. None of it enters reconstruction, so GPU
  rounding is harmless. The CPU can refine or recompute anything it gets back.
- The serial half is one dependency chain per frame and stays on the CPU. It
  covers intra prediction from reconstructed neighbors, RD mode decision with
  predicted motion vectors, and CABAC.

### The build

| option | values | default | effect |
|---|---|---|---|
| `gpu` | `auto`, `enabled`, `disabled` | `disabled` | Link the nextgpu library. `auto` means on for macOS on aarch64. `disabled` builds with no reference to the subproject at all. |

Linking the library never turns the offload on. Only a runtime knob does.
nextgpu is a meson subproject held to one revision by
`subprojects/nextgpu.wrap`. The encoder therefore records exactly which
library version it was validated against. A clone that can't reach the
library's repository still builds with `-Dgpu=disabled`. With the library
linked, meson defines `Y264_HAVE_GPU`.

The offload is 8-bit only. Because the library takes 8-bit samples and the
10-bit library's `pixel` is 16 bits, the offload compiles to stubs in the
10-bit build.

### The offload in `src/encoder/gpu.c`

The offload stays off until its runtime knob arms it. On any failure it falls
back to the CPU search.

| knob | default | what it does |
|---|---|---|
| `Y264_GPU_PHASEA` | 0 | Offloads the mb-tree's per-frame lowres legs. Each frame push submits a round of quarter-pel searches against the previous `bframes + 1` frames. A round searches a range of 8 and covers 4 pushes. Per 8x8 block the kernel returns the best quarter-pel vector and its SATD, plus the zero-vector SATD. The CPU prices rate between those two. Covered legs skip the CPU search entirely. |
| `Y264_GPQ_WARMUP` | 24 | Pushes that get no GPU coverage. The background Metal init runs behind these first frames instead of stalling the chain. |

The API is split into submit and wait. The CPU builds a batch of legs,
submits it, keeps working and collects later. A synchronous call can't pay
however fast the kernels are, because it turns work that ran across the
thread pool into serial waiting.

Every encoder instance and GOP worker in a process shares one Metal context.
Each handle owns its own buffers and its own stream (a command queue). Metal
is probed lazily at the first submit. An armed encode that never submits
doesn't pay for device init.

Determinism holds with Phase A armed. Whether a leg is covered depends only on
push distance, round residency and submit health, never on timing. A GPU
failure turns the handle off for the rest of the encode with a one-time
warning. That's the only case where output can differ from a healthy run.

### The library boundary

nextgpu is its own repository with the `ngc_` prefix, meant for any
block-based encoder. Its design rules:

- **Pixels in, numbers out.** The library sees raw planes and returns
  advisory costs, vectors and scores. It never sees reconstruction buffers,
  syntax, rate models or reference lists. Every job names its plane pairs
  explicitly.
- **Pure C11 header.** `ngc.h` has opaque handles and no Metal or
  Objective-C types. The implementation is Objective-C over Metal.
- **Handles.** Inputs are immutable from submit until the fence signals.
  Outputs are unreadable until it does.
  - `ngc_ctx`: one per process (device, pipelines).
  - `ngc_stream`: one per encoder or worker, single-threaded by contract.
  - `ngc_buf`: a library-allocated shared-storage buffer.
  - `ngc_fence`: marks a submitted batch.
- **`ngc_open` returning NULL is the presence test.** Call sites need no
  `#ifdef`. The CPU path stays complete and the default.
- **The library has its own C reference** for every kernel. Its test tool,
  `gpucheck`, compares GPU output against that reference. Integer kernels
  such as SAD, SATD, variance and box downscale must match exactly. Searches
  are checked by cost regret, because ties make vector equality meaningless.
- **Run-to-run determinism is a kernel-authoring rule.** Reductions use no
  non-associative atomic accumulation.

## What we chose and why

| choice | why |
|---|---|
| `-Dgpu=disabled` by default | The encoder-side quality and speed gate never passed. Non-Apple builds and the conformance gate never depend on Metal. |
| Runtime knobs default to 0 even when linked | Linking decides what's in the binary. Arming it is a measurement choice. |
| Submit/wait API | A synchronous version was 1.86x slower at 12 threads. At a search range where the GPU did almost no work it was still 1.33x slower. |
| One Metal context per process | Per-handle contexts made the armed build 21% to 171% slower on short board cells. That was pure init: the all-intra cells, with no GPU work and identical output, paid the most. |
| Library in its own repository, pinned by a wrap | Vendored copies drift. An unpinned include lets one consumer's change move another's validated output. |
| Metal only, no OpenCL | The machines this targets are Apple Silicon. OpenCL is on the refusal list. |

## What didn't work

| idea | why not |
|---|---|
| GPU lookahead as a shipped path | Every Metal-armed encode pays a 12 to 17 ms bring-up floor per process, on every cell of a board. The floor is device init, not shaders. A precompiled metallib doesn't move it. |
| Hiding the floor with faster kernels | Kernels aren't the bottleneck. On a 720p frame, scale and costmap take 0.24 to 0.26 ms end to end. The lowres and mb-tree work takes 3.5 ms per frame. The idle gap between rounds and the GPU's clock ramp are what kill it. |
| Warming the GPU with a sleeping keep-alive | It read flat once and failed to reproduce twice. Hot-clock numbers don't describe a workload that submits on the anchor cadence. |
| Two-pass lowres search, first pass feeding the second as a neighbor predictor | No measurable gain: median BD over four clips is +0.03% for two passes against +0.04% for one. It also forces a wait between passes. |
| Synchronous Phase A offload, and pipelined warm-up variants | We rejected seven attempts before the per-push batched design. Each lost to serial waiting or found a leg's inputs not yet present at submit time. |
| SME/SME2 instead of the GPU for transforms and SATD | Closed on every form. See [simd.md](simd.md). |

Never built: full-frame motion estimation on the GPU, GPU VMAF, bulk
transform and SATD on the GPU, and GPU sub-pel interpolation for search.
The 10-bit pipeline the library design reserves isn't built either.

## Measuring it

Build it on an Apple Silicon Mac with
`meson setup build-gpu -Dgpu=enabled -Ddev_knobs=true`, then arm it per run
with `Y264_GPU_PHASEA=1`.

Two knob settings from [the instruments catalog](instruments.md) split the
cost.

| setting | what runs | what it reads |
|---|---|---|
| `Y264_GPU_PHASEA=1 Y264_GPQ_WARMUP=10000` | arms the offload but never submits. Only the background `ngc_open` runs | the per-process floor: about +16 ms on bus, +17 ms on stefan |
| `Y264_GPQ_CONSUME=0` | submits rounds but never reads them | the chain-side cost apart from the walk-side cost |

Watch the clock ramp. A microbenchmark that keeps the GPU busy overstates
what the encoder sees, because a GPU that idles between rounds drops its
clock.
