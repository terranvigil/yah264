# GPU offload

yah264 encodes on the CPU. There's an optional Metal offload for part of the
lookahead's lowres motion search, built on a separate compute library called
nextgpu. It's off by default at build time and at run time, and it doesn't
pay: any encode that arms it pays a 12 to 17 ms per-process Metal bring-up
cost that the work it offloads can't win back. The code stays in the tree as
an experiment you can re-run, not a feature.

This page covers GPU compute inside the software encoder. Apple's
fixed-function encoder is a separate backend that replaces the core instead of
offloading part of it; see [hardware-mode.md](hardware-mode.md).

## How it works

### What the GPU can and can't touch

An H.264 encoder splits into an analysis half and a serial half. The analysis
half produces estimates: lowres motion vectors, cost maps, scene-cut inputs,
complexity. None of it enters reconstruction, so GPU rounding is harmless,
and the CPU can refine or recompute anything it gets back. The serial half
(intra prediction from reconstructed neighbors, RD mode decision with
predicted motion vectors, CABAC) is one dependency chain per frame and stays
on the CPU. That bounds any GPU offload to the lookahead and motion-search
side.

### The build

| option | values | default | effect |
|---|---|---|---|
| `gpu` | `auto`, `enabled`, `disabled` | `disabled` | Link the nextgpu library. `auto` means on for macOS on aarch64. `disabled` builds with no reference to the subproject at all. |

Linking the library never turns the offload on. That takes a runtime knob.
nextgpu comes in as a meson subproject pinned to one revision by
`subprojects/nextgpu.wrap`, so the encoder records exactly which library
version it was validated against. With `-Dgpu=disabled`, a clone that can't
reach the library's repository still builds. With the library linked,
meson defines `Y264_HAVE_GPU`.

The offload is 8-bit only. The library takes 8-bit samples and the 10-bit
library's `pixel` is 16 bits, so `src/encoder/gpu.c` compiles to stubs in the
10-bit build.

### The offload in `src/encoder/gpu.c`

It's a runtime knob, default 0, and it falls back to the CPU search on any
failure.

| knob | default | what it does |
|---|---|---|
| `Y264_GPU_PHASEA` | 0 | Offloads the mb-tree's per-frame lowres legs. Each frame push submits a round of quarter-pel searches against the previous `bframes + 1` frames. The kernel returns, per 8x8 block, the best quarter-pel vector with its SATD plus the zero-vector SATD, and the CPU prices rate between those two. Covered legs skip the CPU search entirely. Each round searches a range of 8 and carries 4 pushes. |
| `Y264_GPQ_WARMUP` | 24 | Pushes that get no GPU coverage, so the background Metal init runs behind the first frames instead of stalling the chain. |

The API is split into submit and wait. The CPU builds a batch of legs, submits
it, keeps working, and collects later. A synchronous call can't pay however
fast the kernels are: it turns work that ran across the thread pool into
serial waiting.

There's one Metal context per process, shared by every encoder instance and
GOP worker. Each handle owns its own stream (command queue) and buffers.
Metal is probed lazily at the first submit, so an armed encode that never
submits doesn't pay for device init.

Determinism holds with Phase A armed. Whether a leg is covered depends only on
arithmetic (push distance, round residency, submit health), never on timing.
Any GPU failure turns the handle off for the rest of the encode with a one-time
warning, and that's the only case where output can differ from a healthy run.

### The library boundary

nextgpu is its own repository with the `ngc_` prefix, and it's meant to be
shared by any block-based encoder. The design rules that matter to a
developer here:

- **Pixels in, numbers out.** The library sees raw planes and returns
  advisory costs, vectors and scores. It never sees reconstruction buffers,
  syntax, rate models or reference lists. Every job names its plane pairs
  explicitly.
- **Pure C11 header.** `ngc.h` has opaque handles and no Metal or
  Objective-C types. The implementation is Objective-C over Metal.
- **Handles.** `ngc_ctx` per process (device, pipelines). `ngc_stream` per
  encoder or worker, single-threaded by contract. `ngc_buf` is a
  library-allocated shared-storage buffer, and `ngc_fence` marks a submitted
  batch. Inputs are immutable from submit until the fence signals, and outputs
  are unreadable until it does.
- **`ngc_open` returning NULL is the presence test.** Call sites need no
  `#ifdef`, and the CPU path stays complete and default.
- **The library carries its own C reference** for every kernel, and its own
  harness (`gpucheck`) compares GPU output against it. Integer kernels (SAD,
  SATD, variance, box downscale) must match exactly. Searches are checked by
  cost regret, not vector equality, since ties make vector equality
  meaningless.
- **Run-to-run determinism is a kernel-authoring rule.** No non-associative
  atomic accumulation in reductions.

## What we chose and why

| choice | why |
|---|---|
| `-Dgpu=disabled` by default | The encoder-side quality and speed gate never passed. Non-Apple builds and the conformance gate never depend on Metal. |
| Runtime knobs default to 0 even when linked | Linking decides what's in the binary. Arming it is a measurement choice. |
| Submit/wait API | The first synchronous version measured 1.86x slower at 12 threads, and still 1.33x slower at a search range where the GPU did almost no work. |
| One Metal context per process | Per-handle contexts made the armed build 21% to 171% slower on short board cells, and the all-intra cells (no GPU work, identical output) paid the most. That was pure init. |
| Library in its own repository, pinned by a wrap | Vendored copies drift. An unpinned include lets one consumer's change move another's validated output. |
| Metal only, no OpenCL | The machines this targets are Apple Silicon. OpenCL is on the refusal list. |

## What didn't work

| idea | why not |
|---|---|
| GPU lookahead as a shipped path | Any Metal-armed encode pays a 12 to 17 ms per-process bring-up floor, on every cell of a board. It's device init, not shaders, and a precompiled metallib doesn't move it. |
| Hiding the floor with faster kernels | Not the bottleneck. Scale and costmap on a 720p frame take 0.24 to 0.26 ms end to end against 3.5 ms of lowres and mb-tree work per frame. What kills it is the idle gap between rounds and the GPU's clock ramp. |
| Warming the GPU with a sleeping keep-alive | It read flat once and failed to reproduce twice. Hot-clock numbers don't describe a workload that submits on the anchor cadence. |
| Two-pass lowres search, first pass feeding the second as a neighbor predictor | BD indistinguishable from one pass (median +0.03% against +0.04% on four clips), and it forces a wait between passes. |
| Synchronous Phase A offload, and pipelined warm-up variants | Seven attempts were refused before the per-push batched shape, either by serial waiting or because a leg's inputs weren't present yet at submit time. |
| SME/SME2 instead of the GPU for transforms and SATD | Closed on every form. See [simd.md](simd.md). |

Never built: full-frame motion estimation on the GPU, GPU VMAF, bulk
transform and SATD on the GPU, and GPU sub-pel interpolation for search.
The 10-bit pipeline the library design reserves isn't built either.

## Measuring it

- **Build it:** `meson setup build-gpu -Dgpu=enabled -Ddev_knobs=true` on an Apple Silicon Mac.
- **Arm it per run:** `Y264_GPU_PHASEA=1`.
- **Isolate the floor:** `Y264_GPU_PHASEA=1 Y264_GPQ_WARMUP=10000` arms the
  offload but never submits, so only the background `ngc_open` runs. That
  reads the per-process floor (about +16 ms on bus and +17 ms on stefan).
  `Y264_GPQ_CONSUME=0` submits rounds but never reads them, which splits the
  chain-side cost from the walk-side cost. Both are listed in
  [instruments.md](instruments.md).
- **Watch the clock ramp.** A GPU that idles between rounds drops its clock,
  so a microbenchmark that keeps it busy overstates what the encoder sees.
