<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/yah264-dark.gif">
  <img alt="yah264" src="assets/yah264-light.gif" width="480">
</picture>

An H.264/AVC encoder.

The goal is to build a fast H.264 encoder that I plan to adapt and use for
experimental encoding optimization projects.

I am using x264 as a performance and quality baseline.

Development is macOS/arm64 first with NEON SIMD. I plan to follow up with x86-64 (SSE4.2 through AVX2) and others. See [docs/plan.md](docs/plan.md).

Where it stands (ten clips from CIF to 1080p, CRF at matched bitrate): multi-threaded pure C runs at 0.84x of x264's time, the shipped NEON
build at 0.96x, single-threaded pure C at 0.92x, with quality 0.2 to 0.3 VMAF
ahead at the same size. The multi-threaded pure C row meets all four goal
metrics; the single-threaded pure C row and the shipped build have their
worst clip, low-bitrate 1080p, at or a hundredth past the 1.15x bar.

Under those four there is a floor, added 2026-09-14: PSNR-Y at the same bytes
has to stay within 1.0 dB of x264 on every clip, so a change can't buy VMAF by
spending pixel accuracy. It reads a median of -0.07 dB with the worst clip at
-0.47 dB, and nothing is on the debt list. It is a floor and not a target;
quality is still decided on VMAF.

There is also a hardware mode: `--hw videotoolbox` drives the Mac's H.264
engine with our options and our scene-cut, at 13 to 70 times less CPU for 1
to 8 VMAF points at the same bitrate (the results page has the row).

Beyond parity, the first shot-aware pieces are in: the CRF path
now moves bits between shots the way x264's constant-quality mode does (a
multi-shot sequence went from 5 to 13% behind x264 to level, single-shot
clips unchanged), and on file input `--cut-split` and `--shot-crf` put an IDR
on every cut and give each shot its own CRF from the pre-scan's shot table.
Single pass, no trial encodes; the convex-hull stages are still planned
(docs/innovations.md, docs/shot-based-plan.md).

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
