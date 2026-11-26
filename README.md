<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/yah264-logo-dark-v2.png">
  <img alt="yah264" src="assets/yah264-logo-light-v2.png" width="454">
</picture>

[Docs](#documentation) · [What](#what) · [Plan](#plan) · [Status](#status) · [Details](#details) · [Verify](#verify) · [Hardware](#hardware-option) · [Shot-aware](#shot-aware-support) · [Up next](#up-next) · [Build](#build) · [Contributing](#contributing) · [License](#license)

## What

A super-fast, highly efficient H.264/AVC FLOSS encoder.

## YAH264?

A fast open-source H.264 encoder exists. For 20 years x264 has led software encoding on speed and quality, with its hottest code (motion search, deblocking, CABAC) in tens of thousands of lines of hand-written assembly.

I used to start a codec feature by pulling up the specs and spending weeks for small gains. Coding agents now let you iterate over many algorithmic optimizations at once: prompt with a long list of ideas and in minutes or hours you have an answer.

One fun example was the skip check for prediction reuse. After trying a number of options, the Cauchy-Schwarz inequality won clearly. It bounds every transform coefficient by the energy of the residual with its mean removed. If every bound is under the quantizer's threshold, the block must be zero and we skip the transform. On our "sunflower" test clip, 79% of the skip check's transforms disappeared.

## Plan

Coding agents explore the design space. Each coding tool is a slot with several candidate algorithms, which we generate, implement and benchmark against an objective metric until the best combination wins:

1. Build a standard H.264 encoder with x264's feature set and options.
2. Capture x264's speed and quality baseline on a fixed test set.
3. Measure the speed and compression gaps.
4. Try techniques on each coding tool to see how much of the gap closes.
5. Stop when iterations stop paying.
6. Go through three stages: single-threaded C, multi-threaded, then multi-threaded with SIMD.

## Status

yah264 has passed x264 on speed, compression and quality. The test set runs from CIF to 4K at three lengths: short test clips, 2-minute segments of real titles and 10-minute excerpts of open-licensed films. In every group yah264 is faster at the same file size, with the same or better quality. Figures are wall-time speedups over x264.

| goal | what runs on each side | how much faster than x264 (median, across the groups) | slowest cell |
|---|---|--:|--:|
| 1 | plain C, one thread | 1.37x to 2.17x | 1.35x |
| 2 | plain C, all cores | 1.37x to 2.08x | 1.33x |
| 3 | fastest build (yah264 NEON vs x264's assembly), all cores | 1.28x to 1.92x | 1.12x |

NOTE: Most of the work was on arm64, my Apple M5 development machine. After parity I optimized for other platforms too.

## Details

On 52 rate ladders over real titles, yah264 uses a median 10% fewer bits than x264 for the same VMAF v1 score. See the [results page](https://terranvigil.github.io/yah264/results.html) for every cell.

On rented cloud machines:

| machine | measured | yah264 against x264 |
|---|---|---|
| AWS Graviton4 (arm64) | goals 1 to 3 | 1.75x, 2.27x and 1.43x faster |
| AWS Graviton3 (arm64, SVE) | one thread, CPU time | 26% to 35% less |
| AMD EPYC (x86-64, AVX2) | one thread, CPU time | 4% to 14% less |
| AMD EPYC (x86-64, AVX-512, on by default where the chip has it) | one thread, CPU time | 7% to 16% less |
| Intel Xeon (x86-64, AVX2) | one thread, CPU time | 1% to 12% less |

With every core busy on the Intel machine, yah264 takes 4% longer than x264 at 1080p and 10% less at 4K. x86-64 threading is the next fix.

## Verify

`make review` runs a smaller HD board, as [check it yourself](https://terranvigil.github.io/yah264/check-it-yourself.html) explains.

## Hardware option?

On Macs, `--hw videotoolbox` uses Apple's hardware encoder instead of our SIMD build. Medians over the [hardware table](https://terranvigil.github.io/yah264/results.html#the-hardware-mode)'s ten clips at the same bitrate:

| mode | wall time | CPU used | VMAF |
|---|--:|--:|--:|
| `--hw videotoolbox` vs yah264 SIMD | about the same | 1/18 as much | 8 points lower (VMAF v1) |

## Shot-aware support

yah264 adapts per shot in a single pass and exposes what an external tool needs for a full per-shot search. Both options are off by default. Neither re-encodes. One scan of the source file (no pipes) runs before encoding.

`--cut-split` starts every shot on its own keyframe, on top of the regular interval. A shot can then be seeked to, packaged or replaced alone. Because predicting across a cut gains nothing, it costs no compression. The default scene-cut detection only makes a sudden change an intra frame inside the lookahead.

`--shot-crf` raises CRF on hard shots and lowers it on easy ones. On ten real titles at 1080p it needs a median 3.6% fewer bits for the same VMAF v1 score and wins on nine. PSNR-Y averaged over frames agrees. Pooled over the clip it reads the other way, because the hardest shots are where quality drops.

A full search encodes each shot several ways in parallel, keeps the best and joins them. A shot encoded alone matches the full encode exactly (guaranteed at one thread, usual with more), so trials are exact previews and any shot can be redone later. See [the engine interface](docs/engine-interface.md).

## Up next

Next is speed on cloud servers. The x86-64 kernels are written, verified and tuned. All-core speed is open: we trail x264 on some clips ([SIMD notes](docs/simd.md)). Then come the convex-hull stages in [the plan](docs/plan.md) and [shot-based encoding](docs/shot-based.md).

## Documentation

- [How video encoding works](https://terranvigil.github.io/yah264/encoding.html): concepts every codec shares
- [How H.264 works](https://terranvigil.github.io/yah264/how-h264-works.html): the standard's tools
- [Getting started](https://terranvigil.github.io/yah264/start.html): build, run, presets
- [Design](https://terranvigil.github.io/yah264/design.html): how the encoder fits together
- [Threading](https://terranvigil.github.io/yah264/threading.html): the many-core pipeline
- [Results](https://terranvigil.github.io/yah264/results.html): goal tables and quality maps
- [Check it yourself](https://terranvigil.github.io/yah264/check-it-yourself.html): reproduce the numbers

## Build

Requires a C11 compiler, Meson >= 1.1, and Ninja.

```sh
meson setup build && ninja -C build
meson test -C build
```

[Getting started](https://terranvigil.github.io/yah264/start.html) covers encoding, including inside ffmpeg with `-c:v libyah264`.

## Contributing

Issues and pull requests are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) has the ground rules.

Every change must clear the recon-match gate: the encoder's copy of each
decoded frame, which it predicts from, must match an independent decoder's
output bit for bit. Output is free to change; the encoder's picture of its own
output is not. `make test` runs the unit tests and `make conformance` the gate.

## License

GPL-2.0-or-later, stated per file and in `LICENSE`. Linking yah264 into a
product puts it under the GPL unless it holds the commercial license, the same
arrangement x264 and x265 use.
Commercial terms: terranvigil@gmail.com.
