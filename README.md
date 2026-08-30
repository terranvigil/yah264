# yah264

An H.264/AVC encoder, written from scratch.

The goal is to build a fast H.264 encoder that I will use and adapt for
experimental encoding optimization projects. x264 is the performance and
quality baseline.

## 📖 Documentation: <https://terranvigil.github.io/yah264/>

Start there. It covers how video encoding works, how H.264 works tool by tool,
building and running it, the design, the measured results and how they were
taken, and how the project was built — including the dead ends. The pages live
in `site/` and are rendered by `scripts/build_site.py`.

Where it stands, in short: compared to x264 we lead with pure C, and the shipped
NEON build ties it on small clips but runs noticeably slower at 1080p, because
our SIMD still loses to x264's hand-written assembly.

## Build

Requires a C11 compiler, Meson >= 1.1, and Ninja.

```sh
meson setup build && ninja -C build
meson test -C build
```

[Encoding something with it](https://terranvigil.github.io/yah264/start.html),
including as a library inside ffmpeg with `-c:v libyah264`.

## License

BSD-2-Clause, stated per file as well as in `LICENSE`.
