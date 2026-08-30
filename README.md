# yah264

An H.264/AVC encoder.

The goal is to build a fast H.264 encoder that I will use and adapt for
experimental encoding optimization projects.

We are using x264 as a performance and quality baseline.

Where it stands, in short: compared to x264 we lead with pure C, and the shipped
NEON build ties it on small clips but runs noticeably slower at 1080p, because
our SIMD still loses to x264's hand-written assembly.

## Documentation

- [Introduction](https://terranvigil.github.io/yah264/)
- [How video encoding works](https://terranvigil.github.io/yah264/encoding.html)
- [How H.264 works](https://terranvigil.github.io/yah264/how-h264-works.html)
- [Getting Started](https://terranvigil.github.io/yah264/start.html)
- [Design](https://terranvigil.github.io/yah264/design.html)
- [Threading](https://terranvigil.github.io/yah264/threading.html)
- [Results](https://terranvigil.github.io/yah264/results.html)
- [Methodology](https://terranvigil.github.io/yah264/methodology.html)
- [Story](https://terranvigil.github.io/yah264/story.html)

## Build

Requires a C11 compiler, Meson >= 1.1, and Ninja.

```sh
meson setup build && ninja -C build
meson test -C build
```

Read about using yah264 to encode [here](https://terranvigil.github.io/yah264/start.html)
including as a library inside ffmpeg with `-c:v libyah264`.

## Contributing

Issues and pull requests are welcome. `CONTRIBUTING.md` has the ground rules,
and one of them is unusual enough to read before writing any encoder code: this
is a clean-room project. No source is copied, transliterated or ported from
another encoder, and anyone who has recently read another encoder's source
should not be the one to author mode decision, entropy coding, rate control or
motion estimation here.

Every change has to clear the recon-match gate, where the encoder's own
reconstruction must equal an independent decoder's output bit-for-bit. `make
test` runs the unit tests and `make conformance` runs the gate.

## License

BSD-2-Clause, stated per file as well as in `LICENSE`.
