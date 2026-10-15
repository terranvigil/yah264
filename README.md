<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/yah264-logo-dark.png">
  <img alt="yah264" src="assets/yah264-logo-light.png" width="458">
</picture>

## What

A super-fast, highly efficient H.264/AVC encoder. 

## Do we need another?

Fast open-source H.264 encoders exist today. x264 has always been the leading software implementations for speed. On top of the algorithmic work, its hot paths (motion estimation, deblocking, CABAC, etc.) have been further tuned with tens of thousands of lines of hand-written assembly.

New tools now exist that can accelerate the iteration over a large number of algorithmic optimizations. Now is a good time to take advantage of this.

## Plan

We are making use of agentic tools to assist with design-space exploration. We treat each coding tool as a slot with several candidate algorithms. We then generate, implement and benchmark the alternatives against an objective metric until the best combination emerges:

1. Build a standard H.264 encoder with roughly the same feature set and options as x264.
2. Capture x264's performance and quality baseline on a fixed test set of clips.
3. Measure the gap between the two.
4. Iterate over each H.264 coding tool, trying different techniques to determine how much of the gap can be closed.
5. Stop when iterations no longer produce results.
6. Progress through three stages: single-threaded C, then multi-threaded, then multi-threaded with SIMD optimization.

## Status

yah264 has reached full speed, compression and quality parity with x264. This is measured on an Apple M5 Max. The test set runs from CIF to 4K at three lengths: short test clips, 2-minute segments of real titles and 10-minute excerpts of open-licensed films. In every one of those groups yah264 is faster than x264 at the same file size, and its quality is the same or better. Each figure below is how many times faster yah264 finished than x264, in wall time.

| goal | what runs on each side | how much faster than x264 (median, across the groups) | slowest cell |
|---|---|--:|--:|
| 1 | plain C, one thread | 1.33x to 2.04x | 1.30x |
| 2 | plain C, all cores | 1.41x to 1.96x | 1.27x |
| 3 | fastest build (yah264 NEON vs x264's assembly), all cores | 1.20x to 1.79x | 1.06x |

On 52 rate ladders over real titles, yah264 needs a median 12% fewer bits than x264 for the same quality. Every cell is on the [results page](https://terranvigil.github.io/yah264/results.html).

On cloud machines, arm64 runs 1.75x, 2.27x and 1.43x faster than x264 on goals 1 to 3. On x86-64 (amd64), one thread with SIMD runs from 2% slower to 12% faster than x264. More x86 tuning is under way.

`make review` runs a smaller HD board from the repository. [Check it yourself](https://terranvigil.github.io/yah264/check-it-yourself.html) explains it.

On Macs, `--hw videotoolbox` uses Apple's hardware encoder instead of our SIMD build. Median over the ten clips of the [results page's hardware table](https://terranvigil.github.io/yah264/results.html#the-hardware-mode), same bitrate:

| mode | wall time | CPU used | VMAF |
|---|--:|--:|--:|
| `--hw videotoolbox` vs yah264 SIMD | 1.7x faster | 1/28 as much | 5 points lower |

## Shot-aware support

Most video is a sequence of shots, and each shot wants different settings. yah264 can adapt per shot in a single pass, and it exposes what an external tool needs to run a full per-shot search.

`--cut-split` scans the source for scene cuts first and starts every shot on its own keyframe, on top of the regular keyframe interval. Each shot can then be seeked to, packaged or replaced on its own. It costs no compression, because predicting across a cut gains nothing. This is different from the default scene-cut detection, which only turns a sudden change into an intra frame inside the lookahead and knows nothing about shots.

`--shot-crf` uses the same scan to give each shot its own quality setting, so hard shots get more bits and easy ones fewer. It's still one encode with no trial encodes. On ten real titles at 1080p it needs a median 1% fewer bits for the same VMAF-NEG score, and 5% fewer for the same PSNR-Y.

Neither option re-encodes anything: the scan reads the source once before the encode starts. It needs a file, not a pipe.

For a full per-shot search, an external tool encodes each shot several ways in parallel, keeps the best and joins them. Each shot starts on its own keyframe, so a shot encoded alone matches the full encode exactly. That's guaranteed at one thread and usual with more. Trials are exact previews, and any shot can be redone later. Both options are off by default. [engine-interface.md](docs/engine-interface.md) has the details.

## Up next

With parity reached on Apple silicon, most of the work moves to x86-64. The x86 kernels are written, verified and timed on a cloud machine. One thread runs from 2% slower to 12% faster than x264, and tuning continues ([x86-plan.md](docs/x86-plan.md)).

A few x264 options are still to build, such as intra refresh and custom quant matrices ([x264-parity-plan.md](docs/x264-parity-plan.md)). After that come the convex-hull stages, in [beyond-parity.md](docs/beyond-parity.md) and [shot-based-plan.md](docs/shot-based-plan.md).

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

GPL-2.0-or-later, stated per file as well as in `LICENSE`. A commercial licence
is available for products that cannot comply with the GPL, the same arrangement
x264 and x265 use. Linking yah264 into a product puts the product under the GPL
unless it holds the commercial licence.
Commercial terms: terranvigil@gmail.com.
