<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/yah264-dark.gif">
  <img alt="yah264" src="assets/yah264-light.gif" width="480">
</picture>

An H.264/AVC encoder.

Why do this? x264 is widely regarded as the fastest high-quality software AVC encoder. For quality-per-bit it is effectively unbeatable. It has been under continuous open-source development since 2003, with significant talent behind it. Laurent Aimar (fenrir) wrote it, and Loren Merritt (pengvado) and Fiona Glaser (Dark Shikari) developed it for most of its life. On top of the algorithmic work, its hot paths (motion estimation, deblocking, CABAC, etc.) have been further tuned with tens of thousands of lines of hand-written assembly.

But we have new tools at our disposal now. So is there any juice left to squeeze? The plan:

1. Build a standard H.264 encoder with roughly the same feature set and options as x264
2. Capture x264's performance and quality baseline on a fixed test set of clips
3. Measure the gap between the two
4. Iterate over each H.264 coding tool, trying different techniques to determine how much of the gap can be closed
5. Stop when iterations no longer produce results
6. Progress through three stages: single-threaded C, then multi-threaded, then multi-threaded with SIMD optimization

Once every speed and quality path has been exhausted, I will use yah264 as a testbed for experimental encoding optimization projects.

Development is macOS/arm64 first with NEON SIMD. I plan to follow up with x86-64 from SSE4.2 through AVX2, and others after that. See [plan.md](docs/plan.md).

## Where it stands

On ten clips from CIF to 1080p, yah264 is at parity with x264 medium: 4% faster on the median clip, 16% slower on the worst, equal on VMAF at the same file size. With assembly off on both sides, our C code is 19% faster. Details, including the PSNR floor and the exact x264 build, are on the [results page](https://terranvigil.github.io/yah264/results.html).

For Macs, there's a hardware option as well. `--hw videotoolbox` offloads the encode to Apple's built-in H.264 hardware encoder while keeping our options and scene-cut detection. It costs a few VMAF points. In exchange it uses a tiny fraction of the CPU.

## Shot-aware support

Parity was the first milestone. An initial shot-aware implementation is now done.

Typical videos are made of many shots, and yah264 supports two ways to treat them that way. On its own, `--cut-split` pre-scans the file and puts a keyframe on every scene cut. Add `--shot-crf` and each shot gets its own quality setting out of that same scan. The whole job is one encode, with no trial encodes. The pre-scan seeks through the file, so it won't run on a pipe.

For a proper per-shot optimization, the encoder exposes the hooks an orchestrator needs: a shot table, a plan of keyframes and per-shot quality offsets, deterministic per-shot output (byte-identical at a fixed thread count), per-frame stats, and per-shot segment files. An external tool can then probe every shot at several quality points in parallel, pick the best point per shot, and assemble the result without re-encoding. Both are opt-in and off by default. [engine-interface.md](docs/engine-interface.md) has the details.

## Up next

The convex-hull stages come next; see [innovations.md](docs/innovations.md) and [shot-based-plan.md](docs/shot-based-plan.md).

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

Every change has to clear the recon-match gate, where the encoder's own
reconstruction must equal an independent decoder's output bit-for-bit. `make
test` runs the unit tests and `make conformance` runs the gate.

## License

GPL-2.0-or-later, stated per file as well as in `LICENSE`. A commercial licence
is available for products that cannot comply with the GPL, the same arrangement
x264 and x265 use. Linking yah264 into a product puts the product under the GPL
unless it holds the commercial licence.
Commercial terms: terranvigil@gmail.com.
