<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/yah264-dark.gif">
  <img alt="yah264" src="assets/yah264-light.gif" width="480">
</picture>

An H.264/AVC encoder.

## Where it stands

On ten clips from CIF to 1080p, yah264 is at parity with x264 medium on the median clip, which runs 4% faster. The worst clip is 16% slower and still just over the bar. Both encoders reach the same VMAF at the same file size. With assembly off on both sides, our C code is 16% faster than x264's. Details, including the PSNR floor and the exact x264 build, are on the [results page](https://terranvigil.github.io/yah264/results.html).

A second board is runnable straight from the repository with `make review`. Its four clips are HD at low to mid bitrate, the kind of clip video engineers commonly test with. On those, rate-matched at twelve threads, we read 4% slower than x264 on the median clip and 8% on the worst. Low-bitrate HD is where we are slowest. [Check it yourself](https://terranvigil.github.io/yah264/check-it-yourself.html) has the run and how to read it. The [HD parity plan](docs/hd-parity-plan.md) is the work to close that gap.

For Macs, there's a hardware option as well. `--hw videotoolbox` offloads the encode to Apple's built-in H.264 hardware encoder while keeping our options and scene-cut detection. It costs a few VMAF points. In exchange it uses a tiny fraction of the CPU.

## Why do this?

x264 is widely regarded as the fastest high-quality software AVC encoder. For quality-per-bit it is effectively unbeatable. It has been under continuous open-source development since 2003, with significant talent behind it. Laurent Aimar (fenrir) wrote it, and Loren Merritt (pengvado) and Fiona Glaser (Dark Shikari) developed it for most of its life. On top of the algorithmic work, its hot paths (motion estimation, deblocking, CABAC, etc.) have been further tuned with tens of thousands of lines of hand-written assembly.

But we have new tools at our disposal now. So is there any juice left to squeeze? The plan:

1. Build a standard H.264 encoder with roughly the same feature set and options as x264
2. Capture x264's performance and quality baseline on a fixed test set of clips
3. Measure the gap between the two
4. Iterate over each H.264 coding tool, trying different techniques to determine how much of the gap can be closed
5. Stop when iterations no longer produce results
6. Progress through three stages: single-threaded C, then multi-threaded, then multi-threaded with SIMD optimization

Once every speed and quality path has been exhausted, I will use yah264 as a testbed for experimental encoding optimization projects.

Development is macOS/arm64 first with NEON SIMD. The plan is to follow up with x86-64 from SSE4.2 through AVX2, and others after that. See [plan.md](docs/plan.md).

## Shot-aware support

Most encoded videos are a sequence of shots. The right settings differ from shot to shot. yah264 can encode per shot in a single pass, and it exposes what an external tool needs to do the full per-shot search.

`--cut-split` pre-scans the source for scene cuts and starts every shot on its own keyframe, on top of the regular keyframe interval. Each shot becomes a clean unit: seekable, packageable, and replaceable on its own. It costs nothing in compression. Predicting across a cut buys nothing. NOTE: This is not the scene-cut detection x264 and yah264 both run by default. That marks a sudden change as an intra frame inside the lookahead and knows nothing about shots. Cut-split maps every shot in the file first and builds the stream around them.

`--shot-crf` gives each shot its own quality setting from the same scan. Hard shots get more bits and easy shots get fewer. The whole job is still one encode with no trial encodes. This is the simple version of shot-aware encoding. Its size gain on long-form films is being measured and will be stated here when it is.

Neither option re-encodes anything. The scan reads the uncompressed source before the encode starts, so every keyframe is placed on the first and only pass. There is no generation loss. Note that the scan seeks through the file, so it needs a file rather than a pipe.

The full version tries each shot a few ways and keeps the best. A separate tool encodes every shot at several settings in parallel, compares them, and stitches the winners into one file. This works because a shot encoded alone comes out byte-for-byte the same as it does inside a full encode: every shot starts fresh at its keyframe, and the encoder is deterministic at a fixed thread count. So a trial encode of one shot is an exact preview of the final file, and a single shot can be redone later without touching the rest. Both modes are off unless you ask for them. [engine-interface.md](docs/engine-interface.md) has the details.

## Up next

Three lines of work are open. The x264 parity programme is closing the option gaps left against the reference encoder, wave by wave, and [x264-parity-plan.md](docs/x264-parity-plan.md) lists them. The x86-64 kernels come next behind the dispatch that already ships. [x86-plan.md](docs/x86-plan.md) names the first three waves of those. The convex-hull stages are the third line, in [beyond-parity.md](docs/beyond-parity.md) and [shot-based-plan.md](docs/shot-based-plan.md).

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
