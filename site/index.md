---
title: yah264
description: An H.264/AVC encoder project.
---

# yah264

An H.264/AVC encoder.

The goal is to build a fast H.264 encoder that I plan to adapt and use for
experimental encoding optimization projects.

I am using x264 as a performance and quality baseline.

Development is macOS/arm64 first with NEON SIMD. I plan to follow up with
x86-64 (SSE4.2 through AVX2) and others. See
[docs/plan.md](https://github.com/terranvigil/yah264/blob/main/docs/plan.md).

## Status

Compared to x264 on a ten-clip board with 1080p in it, multi-threaded pure C leads (0.84x), the shipped NEON build sits at 0.96x, and single-threaded pure C at 0.92x. The open item is the same on every row: low-bitrate HD, where the worst clip sits at 1.15x against a bar of under 1.15x. High-bitrate 1080p is where we are fastest.

Criteria for performance (goal 3 still open):

| metric | bar |
|---|---|
| median speed | 1.00x or faster |
| worst-clip speed | under 1.15x |
| quality | within 0.5 VMAF |
| compression | within 1.0% size |

Current performance (three CIF, four 720p, three 1080p):

| goal | configuration | median | max | VMAF | size | status |
|---|---|--:|--:|--:|--:|---|
| 1 | pure C, single-threaded | **0.92x** | 1.15x | +0.26 | −0.1% | worst clip at the bar |
| 2 | pure C, multi-threaded | **0.84x** | 1.06x | +0.20 | +0.1% | all metrics pass |
| 3 | as-shipped SIMD, multi-threaded | **0.96x** | 1.16x | +0.22 | +0.0% | worst clip past the bar by 0.01 |

Big caveat: the board's resolution mix hides a rate story. Read by class,
the shipped build (row 3) is 0.91x at CIF, 0.99x at 720p and 1.08x at 1080p,
and the slow cells are the low-bitrate HD ones, not 1080p as such: the two
high-bitrate 1080p clips are the fastest on the board. At CIF we keep 8.8 cores
busy where x264 uses 5.8, and that lead goes away once the frame is large enough
for both encoders to consume every core. Give row 3 a single thread and it is
behind at every resolution, because our SIMD loses to x264's hand-written
assembly. Row 1 is the pure C tier, where we are level.

The goal tables, per configuration and per resolution class, are on the
[results page](results.html); the numbers above are the current board.

Quality is measured with [VMAF](https://en.wikipedia.org/wiki/Video_Multimethod_Assessment_Fusion) at matched bitrates (the v0.6.1 NEG model, full-frame sampling). yah264 excels at low bitrates. The lead fades higher up the range. See [Results](results.md) for the details and how to reproduce them.

## The hardware mode

`--hw videotoolbox` drives the Mac's H.264 engine with our options and our
scene-cut, at 13 to 70 times less CPU for 1 to 8 VMAF points at the same
bitrate. It is its own row on the [results page](results.html), never a
parity row.

## Across shots

Beyond parity, the first shot-aware pieces are in: the CRF path moves bits
between shots the way x264's constant-quality mode does (a multi-shot
sequence went from 5 to 13% behind x264 to level, single-shot clips
unchanged), and on file input `--cut-split` and `--shot-crf` put an IDR on
every cut and give each shot its own CRF from the pre-scan's shot table.
Single pass, no trial encodes; the convex-hull stages are still planned
([docs/innovations.md](https://github.com/terranvigil/yah264/blob/main/docs/innovations.md),
[docs/shot-based-plan.md](https://github.com/terranvigil/yah264/blob/main/docs/shot-based-plan.md)).

## Documentation

- **[How video encoding works](encoding.md)**: the concepts every codec shares.
- **[How H.264 works](how-h264-works.html)**: the standard's tools, one by one.
- **[Getting started](start.md)**: how to build and run yah264 and what the
  presets do.
- **[Design](design.md)**: how the encoder is put together.
- **[Threading](threading.html)**: the many-core pipeline.
- **[Results](results.md)**: the goal tables and the quality maps.
- **[Check it yourself](check-it-yourself.md)**: reproduce every number here.
