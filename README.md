<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/yah264-dark.gif">
  <img alt="yah264" src="assets/yah264-light.gif" width="480">
</picture>

An H.264/AVC encoder.

Why do this? x264 is widely regarded as the fastest software AVC encoder, and for quality-per-bit it is effectively unbeatable. It has been under continuous open-source development since 2003, with remarkable talent behind it: it was originally written by Laurent Aimar (fenrir), taken over by Loren Merritt (pengvado) and Fiona Glaser (Dark Shikari) in 2008. On top of the algorithmic work, its hot paths (motion estimation, deblocking, CABAC, etc) have been further tuned with tens of thousands of lines of hand-written assembly.

But we have new tools at our disposal now. So is there any juice left to squeeze? The plan:

1. Build a standard H.264 encoder with roughly the same feature set and options as x264
2. Capture x264's performance and quality baseline on a fixed test set of clips
3. Measure the gap between the two
4. Iterate over each H.264 coding tool, trying different techniques to determine how much of the gap can be closed
5. Stop when iterations no longer produce results
6. Progress through three stages: single-threaded C, then multi-threaded, then multi-threaded with SIMD optimization

Once every speed and quality path has been exhausted, I will use yah264 as a testbed for experimental encoding optimization projects.

Development is macOS/arm64 first with NEON SIMD. I plan to follow up with x86-64 (SSE4.2 through AVX2) and others. See [plan.md](docs/plan.md).

## Where it stands

On a test set of ten clips: small CIF sources up to 1080p, yah264 now matches or beats x264 on speed and quality. The multi-threaded C build is x% median faster than x264 while landing slightly ahead on VMAF at the same file size. The single-threaded build and the shipped NEON build are close behind - we still have work to do on some specific content types.

On a ten-clip test set, from CIF up to 1080p, yah264 matches or beats x264 on both speed and quality. The multi-threaded C build encodes ~ 17% faster than x264 (median) and is slightly ahead on VMAF at equal file size. The single-threaded and NEON builds are close behind, while a handful content types still need work.

For Macs, there's a hardware option as well. `--hw videotoolbox` offloads the encode to Apple's built-in H.264 hardware encoder while keeping our options and scene-cut detection. It costs a few VMAF points, but it uses a tiny fraction of the CPU.

## Shot-aware support

Parity was the first milestone. An initial shot-aware implementation is now done.

Typical videos are made of many shots, and yah264 supports two ways to treat them that way. On its own, --cut-split pre-scans the file, puts a keyframe on every scene cut, and with --shot-crf gives each shot its own quality setting from that scan: one encode, no trial encodes.

For a proper per-shot optimization, the encoder exposes the hooks an orchestrator needs: a shot table, a plan of keyframes and per-shot quality offsets, deterministic per-shot output (a shot encoded alone is byte-identical to the same shot in the full encode), per-frame stats, and per-shot segment files. That lets an external tool probe every shot at several quality points in parallel, pick the best point per shot, and assemble the result without re-encoding. Both are opt-in and off by default; details in [engine-interface](docs/engine-interface.md)

## Up next

The convex-hull stages come next; see innovations.md and shot-based-plan.md.

## Documentation

- [Introduction](https://terranvigil.github.io/yah264/)
- [How video encoding works](https://terranvigil.github.io/yah264/encoding.html)
- [How H.264 works](https://terranvigil.github.io/yah264/how-h264-works.html)
- [Getting Started](https://terranvigil.github.io/yah264/start.html)
- [Design](https://terranvigil.github.io/yah264/design.html)
- [Threading](https://terranvigil.github.io/yah264/threading.html)
- [Results](https://terranvigil.github.io/yah264/results.html)
- [Check it yourself](https://terranvigil.github.io/yah264/check-it-yourself.html)

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

GPL-2.0-or-later, stated per file as well as in `LICENSE`, with a commercial
licence available for products that cannot comply with the GPL: the same
arrangement x264 and x265 use. Linking yah264 into a product puts the product
under the GPL unless it holds the commercial licence. Running the standalone
binary from another program is not linking and needs nothing beyond the GPL.
Contact the author for commercial terms.
