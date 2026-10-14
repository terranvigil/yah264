<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/yah264-logo-dark.png">
  <img alt="yah264" src="assets/yah264-logo-light.png" width="458">
</picture>

## What

A very fast and efficient H.264/AVC encoder. 

## Do we need another?

Fast open-source H.264 encoders exist today. For years, x264 has led as the fastest software implementation. On top of the algorithmic work, its hot paths (motion estimation, deblocking, CABAC, etc.) have been further tuned with tens of thousands of lines of hand-written assembly.

New tools now exist that can accelerate iteration over a large number of algorithmic optimizations. Now is a good time to take advantage of that.

## Plan

We are making use of agentic tools to assist with design-space exploration. We treat each coding tool as a slot with several candidate algorithms. We then generate, implement and benchmark the alternatives against an objective metric until the best combination emerges:

1. Build a standard H.264 encoder with roughly the same feature set and options as x264. **Mostly done.** Four of the six option waves have shipped, and [x264-parity-plan.md](docs/x264-parity-plan.md) lists what's left.
2. Capture x264's performance and quality baseline on a fixed test set of clips. **Done.**
3. Measure the gap between the two. **Done.**
4. Iterate over each H.264 coding tool, trying different techniques to determine how much of the gap can be closed. **Done on Apple silicon.** The gap closed there on 12 October 2026, as Status below shows.
5. Stop when iterations no longer produce results. **Not yet.** They still do.
6. Progress through three stages: single-threaded C, then multi-threaded, then multi-threaded with SIMD optimization. **Done on arm64 with NEON.** On x86-64 the SIMD kernels are written and the speed work is next.

Once every speed and quality path has been exhausted, I will use yah264 as a testbed for experimental encoding optimization projects.

Development started on macOS/arm64. That build is finished, with its C, its threading and its NEON SIMD all at parity. The x86-64 build came next. It has SSE4.2 and AVX2 versions of the same kernel families as NEON, and checkasm holds every one of them bit-exact against its C reference. On a rented AMD EPYC machine its one-thread output matches the Mac's byte for byte and passes the conformance suite. It hasn't been timed against x264 on x86 yet. AVX-512 is a build option with no kernels written. See [plan.md](docs/plan.md).

## Status

yah264 reached full parity with x264 `--preset medium` on 12 October 2026, measured on an Apple M5 Max. The test set runs from CIF to 4K at three lengths: short test clips, 2-minute segments of real titles and 10-minute excerpts of open-licensed films. In every one of those groups yah264 is faster than x264 at the same file size, and its quality is the same or better. Each figure below is our wall time over x264's, so anything under 1.00 means yah264 finished first.

| goal | what runs on each side | median, across the groups | worst cell |
|---|---|--:|--:|
| 1 | plain C, one thread | 0.49 to 0.75 | 0.77 |
| 2 | plain C, all cores | 0.51 to 0.71 | 0.79 |
| 3 | each encoder's fastest build (our NEON, x264's assembly), all cores | 0.56 to 0.83 | 0.94 |

Compression holds up across the whole rate range too. On 52 rate ladders over the real titles, at CRF and at capped VBR, yah264 needs a median 12% fewer bits than x264 for the same VMAF-NEG score. Two ladders came out behind by less than half a percent, which is inside the 1% the bar allows. In VBR no encode missed its target bitrate by more than 5%. One corner isn't measured yet: speed on the 4K sports and screen-content clips, whose quality is in. Details, including the exact x264 build and every cell, are on the [results page](https://terranvigil.github.io/yah264/results.html).

Other processors are read separately from that table, on rented cloud machines. On an AWS Graviton4 (Linux arm64, NEON) a seven-clip check gives medians of 0.57, 0.44 and 0.70 for goals 1 to 3. On an AMD EPYC 9R14 (x86-64, AVX2) the x86 kernels aren't tuned yet. At one thread with SIMD on, yah264 takes 1.19 times x264's time on a 1080p clip at the same bitrate and 1.01 times on a 4K clip. Closing that gap is the current work.

`make review` runs a smaller HD board straight from the repository. [Check it yourself](https://terranvigil.github.io/yah264/check-it-yourself.html) has the run and how to read it.

For Macs, there's a hardware option as well. `--hw videotoolbox` offloads the encode to Apple's built-in H.264 hardware encoder while keeping our options and scene-cut detection. It costs a few VMAF points. In exchange it uses a tiny fraction of the CPU.

## Shot-aware support

Most encoded videos are a sequence of shots. The right settings differ from shot to shot. yah264 can encode per shot in a single pass. It also exposes what an external tool needs to do the full per-shot search.

`--cut-split` pre-scans the source for scene cuts and starts every shot on its own keyframe, on top of the regular keyframe interval. Each shot becomes a clean unit: seekable, packageable, and replaceable on its own. It costs nothing in compression. Predicting across a cut buys nothing. NOTE: This is not the scene-cut detection x264 and yah264 both run by default. That marks a sudden change as an intra frame inside the lookahead and knows nothing about shots. Cut-split maps every shot in the file first and builds the stream around them.

`--shot-crf` gives each shot its own quality setting from the same scan. Hard shots get more bits and easy shots get fewer. The whole job is still one encode with no trial encodes. This is the simple version of shot-aware encoding. Its size gain on long-form films is being measured and will be stated here when it is.

Neither option re-encodes anything. The scan reads the uncompressed source before the encode starts, so every keyframe is placed on the first and only pass. There is no generation loss. Note that the scan seeks through the file, so it needs a file rather than a pipe.

The full version tries each shot a few ways and keeps the best. A separate tool encodes every shot at several settings in parallel, compares them, and stitches the winners into one file. This works because every shot starts fresh at its keyframe, so at one thread a shot encoded alone comes out byte-for-byte the same as it does inside a full encode, and a trial encode of one shot is an exact preview of the final file. With more threads it usually matches too, but that isn't guaranteed. A single shot can be redone later without touching the rest. Both modes are off unless you ask for them. [engine-interface.md](docs/engine-interface.md) has the details.

## Up next

With parity reached on Apple silicon, most of the work moves to x86-64. Its kernels are written and verified, so the next step is to time them against x264 on x86 and make them faster. [x86-plan.md](docs/x86-plan.md) has that plan. The x264 option programme has two waves left, listed in [x264-parity-plan.md](docs/x264-parity-plan.md). The convex-hull stages come after that, in [beyond-parity.md](docs/beyond-parity.md) and [shot-based-plan.md](docs/shot-based-plan.md).

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
