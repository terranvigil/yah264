<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/yah264-logo-dark-v2.png">
  <img alt="yah264" src="assets/yah264-logo-light-v2.png" width="454">
</picture>

[Docs](#documentation) · [What](#what) · [Plan](#plan) · [Status](#status) · [Details](#details) · [Verify](#verify) · [Hardware](#hardware-option) · [Shot-aware](#shot-aware-support) · [Up next](#up-next) · [Build](#build) · [Contributing](#contributing) · [License](#license)

## What

A super-fast, highly efficient H.264/AVC FLOSS encoder. 

## YAH264?

A fast open-source H.264 encoder exists. For 20 years x264 has been the leading software implementation for speed and quality. Beyond highly optimized coding tool algorithms, its hottest code (motion search, deblocking, CABAC) is hand-written assembly. Tens of thousands of lines of it.

In the past, when working on formats and codecs, I'd start by pulling up the specs and spend weeks working features for small gains. Today, you can use coding agents to accelerate the iteration over a large number of algorithmic optimizations. Prompt with a long list of ideas to try out and in minutes or hours, you have an answer.

A fun example of this was when we were trying to optimize the skip check for prediction reuse. Beyond brute force, there are some simple methods for this. But after iterating through a number of options, the Cauchy–Schwarz inequality turned out to be a clear winner. We can bound every transform coefficient by the energy of the residual with its mean removed. If every bound is under the quantizer’s threshold, the block must be zero, and we skip the transform. On our “sunflower” test clip, 79% of the skip check’s transforms disappeared.

## Plan

We make use of coding to assist with design-space exploration. We treat each coding tool as a slot with several candidate algorithms. We then generate, implement and benchmark the alternatives against an objective metric until the best combination is found:

1. Build a standard H.264 encoder with the same feature set and options as x264.
2. Capture x264's performance and quality baseline on a fixed test set of clips.
3. Measure the speed and compression gaps between the two.
4. Iterate over each H.264 coding tool, trying different techniques to determine how much of the gap can be closed.
5. Stop when iterations no longer produce results.
6. Progress through three stages: single-threaded C, then multi-threaded, then multi-threaded with SIMD optimization.

## Status

yah264 has met and surpassed speed, compression and quality parity with x264. The test set runs from CIF to 4K at three lengths: short test clips, 2-minute segments of real titles and 10-minute excerpts of open-licensed films. In every one of those groups yah264 is faster than x264 at the same file size, and its quality is the same or better. Each figure below is how many times faster yah264 finished than x264, in wall time.

| goal | what runs on each side | how much faster than x264 (median, across the groups) | slowest cell |
|---|---|--:|--:|
| 1 | plain C, one thread | 1.39x to 2.13x | 1.33x |
| 2 | plain C, all cores | 1.41x to 2.04x | 1.32x |
| 3 | fastest build (yah264 NEON vs x264's assembly), all cores | 1.22x to 1.89x | 1.14x |

NOTE: An Apple M5 is my primary development platform so most of the work was done for arm64. Once we reached parity, I also further optimized for additional platforms and instruction sets.

## Details

On 52 rate ladders over real titles, yah264 uses a median 13% fewer bits than x264 for the same quality. Every cell is on the [results page](https://terranvigil.github.io/yah264/results.html).

On rented cloud machines:

| machine | measured | yah264 against x264 |
|---|---|---|
| AWS Graviton4 (arm64) | goals 1 to 3 | 1.75x, 2.27x and 1.43x faster |
| AWS Graviton3 (arm64, SVE) | one thread, CPU time | 26% to 35% less |
| AMD EPYC (x86-64, AVX2) | one thread, CPU time | 4% to 14% less |
| AMD EPYC (x86-64, AVX-512, on by default where the chip has it) | one thread, CPU time | 7% to 16% less |
| Intel Xeon (x86-64, AVX2) | one thread, CPU time | 1% to 12% less |

With every core busy on the Intel machine, yah264 takes 4% longer than x264 at 1080p and 10% less time at 4K. Threading on x86-64 is the next fix.

## Verify

`make review` runs a smaller HD board from the repository. [Check it yourself](https://terranvigil.github.io/yah264/check-it-yourself.html) explains it.

## Hardware option?

On Macs, `--hw videotoolbox` uses Apple's hardware encoder instead of our SIMD build. Median over the ten clips of the [results page's hardware table](https://terranvigil.github.io/yah264/results.html#the-hardware-mode), same bitrate:

| mode | wall time | CPU used | VMAF |
|---|--:|--:|--:|
| `--hw videotoolbox` vs yah264 SIMD | about the same | 1/21 as much | 4 points lower |

## Shot-aware support

Most video is a sequence of shots. Each shot wants different settings. yah264 can adapt per shot in a single pass. It also exposes what an external tool needs to run a full per-shot search.

`--cut-split` scans the source for scene cuts first and starts every shot on its own keyframe, on top of the regular keyframe interval. Each shot can then be seeked to, packaged or replaced on its own. It costs no compression, because predicting across a cut gains nothing. The default scene-cut detection is different. It only turns a sudden change into an intra frame inside the lookahead and knows nothing about shots.

`--shot-crf` uses the same scan to give each shot its own quality setting. Hard shots get more bits and easy ones fewer. It's still one encode with no trial encodes. On ten real titles at 1080p it needs a median 1% fewer bits for the same VMAF-NEG score, and 5% fewer for the same PSNR-Y.

Neither option re-encodes anything: the scan reads the source once before the encode starts. It needs a file, not a pipe.

For a full per-shot search, an external tool encodes each shot several ways in parallel, keeps the best and joins them. Each shot starts on its own keyframe, so a shot encoded alone matches the full encode exactly. That's guaranteed at one thread and usual with more. Trials are exact previews. Any shot can be redone later. Both options are off by default. [engine-interface.md](docs/engine-interface.md) has the details.

## Up next

With parity reached on Apple silicon, the work moves to speed on cloud servers. On x86-64 the kernels are written, verified and tuned twice. The open item is speed with every core busy. There we still trail x264 on some clips ([x86-plan.md](docs/x86-plan.md)).
Next come the convex-hull stages, in [beyond-parity.md](docs/beyond-parity.md) and [shot-based-plan.md](docs/shot-based-plan.md).

## Documentation

- [How video encoding works](https://terranvigil.github.io/yah264/encoding.html): the concepts every codec shares.
- [How H.264 works](https://terranvigil.github.io/yah264/how-h264-works.html): the standard's tools, one by one.
- [Getting started](https://terranvigil.github.io/yah264/start.html): how to build and run yah264 and what the presets do.
- [Design](https://terranvigil.github.io/yah264/design.html): how the encoder is put together.
- [Threading](https://terranvigil.github.io/yah264/threading.html): the many-core pipeline.
- [Results](https://terranvigil.github.io/yah264/results.html): the goal tables and the quality maps.
- [Check it yourself](https://terranvigil.github.io/yah264/check-it-yourself.html): reproduce every number here.

## Build

Requires a C11 compiler, Meson >= 1.1, and Ninja.

```sh
meson setup build && ninja -C build
meson test -C build
```

Read about using yah264 to encode [here](https://terranvigil.github.io/yah264/start.html)
including as a library inside ffmpeg with `-c:v libyah264`.

## Contributing

Issues and pull requests are welcome. `CONTRIBUTING.md` has the ground rules.

Every change has to clear the recon-match gate. The encoder's internal copy of
each decoded frame, the one it predicts the next frames from, must match what an
independent decoder produces from the bitstream, bit for bit. Output is free to
change; the encoder's picture of its own output is not. `make test` runs the
unit tests and `make conformance` runs the gate.

## License

GPL-2.0-or-later, stated per file as well as in `LICENSE`. A commercial license
is available for products that cannot comply with the GPL. x264 and x265 use the
same arrangement. Linking yah264 into a product puts the product under the GPL
unless it holds the commercial license.
Commercial terms: terranvigil@gmail.com.
