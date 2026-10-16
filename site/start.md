---
title: Getting started - yah264
description: Build yah264, run it, and read the options that matter.
---

# Getting started

## Build

yah264 builds with Meson and Ninja. It needs nothing beyond a C11 compiler. The
Makefile wraps the usual commands, so from a clean checkout:

```
make build
```

That configures into `build/` and compiles. `make test` runs the unit tests.
`make conformance` runs the recon-match gate described below. `make help` lists
the rest.

One build gives you both sample depths. `make build` produces an 8-bit encoder
library and a 10-bit one. A single `yah264` binary links both. Each library
installs with its own pkg-config file: `yah264.pc` and `yah264_10.pc`.
`-Dbit_depth` doesn't change any of that. It only picks the depth the tests and
the in-tree tools build at.

`-Dgpu=enabled` links the Metal compute library. It's off by default.

## Encode something

The shortest useful command reads Y4M and writes an Annex-B stream:

```
yah264 --input-y4m in.y4m -o out.264
```

Input is a flag. There is no positional input argument. Both ends accept `-` for
stdin and stdout, so yah264 works in pipes.

Raw YUV goes in through `--input-raw`, which needs `--input-res WxH`.
`--input-csp`, `--fps` and `--input-range` supply the rest of what a Y4M header
would hold. `--seek N` drops the first N input frames. `--crop-rect L,T,R,B`
crops the input before encoding, in luma samples.

The defaults are close to x264's medium preset: `--preset medium --cabac --ref 3
--bframes 3 --transform-8x8`. That lets you compare the two encoders without a
pile of tuning arguments, and it's how most of the measurements on this site
were made.

Two defaults differ from x264's on purpose. We swept both against our own
quality band. `--aq-strength` is 0.4 where x264 uses 1.0, and `--psy-rd` is 2.0
where x264 uses 1.0. Psychovisual RD trades pixel accuracy for visible texture.
It's the setting most likely to flatter a VMAF-scored comparison. `--tune psnr`
turns both off if you want a run without it.

## Choosing a rate control mode

Each of these flags selects a mode:

| flag | mode | use it when |
|---|---|---|
| `--crf N` | constant quality | file size is negotiable |
| `--crf N --vbv-maxrate M --vbv-bufsize B` | capped CRF | streaming VOD and ABR ladder rungs |
| `--bitrate N` | single-pass average bitrate | you have a budget for the whole file |
| `--qp N` | constant QP | you are measuring a coding change and want the rate controller out of the way |

Capped CRF is a common choice for VOD libraries and adaptive ladders:

```
yah264 --input-y4m in.y4m --crf 21 --vbv-maxrate 6000 --vbv-bufsize 12000 -o out.264
```

The encoder codes to the quality target. The buffer sets a ceiling. Easy
titles encode at CRF 21 and come out small. Hard titles hit the ceiling and
stay under it. Nothing in the library ends up undeliverable.

Plan around two things. Every GOP after the first assumes a half-full buffer.
That keeps concatenated segments safe but costs a few bits. Short keyints also
run hot, so aim the cap low on two-second segments.

Broadcast and live work differently, because the target there is a rate. Pair
the VBV flags with `--bitrate`. A cap equal to the target gives CBR. A cap above
it gives capped VBR.

`--nal-hrd vbr` or `--nal-hrd cbr` writes the buffer model into the stream, so a
receiver can verify the buffer instead of trusting it. It needs both VBV flags
and a frame rate, and it's refused without them. Under `cbr`, `--filler` is
already on and pads every access unit up to the rate. `--no-filler` removes the
padding but keeps the CBR headers. Use it to measure, never to ship.

Two-pass is for bitrate targets only. `--pass 1` then `--pass 2` runs against
`--bitrate`. `--pass 3` reads the stats and writes them back, so each further
pass refines against a real encode. Two-pass CRF isn't implemented. On the
threaded path, two-pass needs a seekable input.

## Presets

`--preset` runs from `ultrafast` through `medium` to `veryslow` and `placebo`.
It sets the subpel and mode-decision tier. The motion search follows from it:
hex at medium and faster, umh at slow and up, unless `--me` overrides it.
Medium is the default because every comparison on this site is against x264
medium.

`--tune` adjusts for content: `grain`, `film`, `animation`, `psnr`, `ssim`,
`zerolatency`, `stillimage` and `fastdecode`. `zerolatency` turns off the sync
lookahead. It's the only option here that trades quality for latency.

Four tunes come with a caveat:

- `grain` lowers psy-rd to 1.5 from the 2.0 default. Nobody has re-measured that
  direction.
- `film` hasn't been BD-measured at all, because the test set has no film clip.
- `stillimage` borrows x264's constants for that tune. We haven't measured them,
  because there's no still-image clip to measure them on.
- `fastdecode` turns off deblocking, CABAC and weighted biprediction. That's a
  real quality loss, on purpose. Explicit P weighted prediction belongs in that
  set too but can't be turned off yet.

## Other flags

[options.md](https://github.com/terranvigil/yah264/blob/main/docs/options.md)
documents every flag with its default and how it differs from x264. A few are
worth knowing here:

- `--profile` is enforced. Naming a profile narrows any tool the preset chose
  and refuses a tool you asked for that doesn't fit.
- `--slices N` cuts each picture into N independently decodable slices.
- `--tff` and `--bff` code each frame as two field pictures. An interlaced Y4M
  turns field coding on by itself. `--no-interlaced` refuses it.
- `--constrained-intra` keeps intra prediction in P and B slices off
  inter-coded neighbors. It costs bits and buys error resilience.
- `--deblock A:B` moves the in-loop filter offsets.
- `--aud` opens every access unit with a delimiter, and `--pic-struct` adds a
  pic_timing SEI to each picture. Neither changes a sample.

`--cut-split` and `--shot-crf` encode per shot in one pass. The
[shot-aware section](index.html#shot-aware-support) of the home page covers
both.

## Threading and the memory it costs

`--threads` defaults to auto. Auto picks the smaller of your core count and 16,
then caps that by what the picture can absorb. The 16 is a conservative default.
Nobody measured a knee there. Wavefront scaling hits the picture's critical
path before it runs out of cores, and on a machine with two kinds of core the
last workers end up on the slower ones. An explicit `--threads N` is honored,
then clamped by the picture. The [design page](design.md) has the caps and what
has been measured.

The threaded path streams. It reads on its own thread and writes each GOP as it
finishes, so clip length isn't the limit. From a file, frames are read in the
order the GOP workers need them. Only a few frames per worker sit in memory at
a time. From a pipe, the reader holds a window of up to
`(--threads + 1) x --keyint` frames. Neither grows with the length of the clip.

yah264 prices the pipe window up front. It refuses a job that needs more than
half your RAM and quotes the figure and the window it came from, so the job
fails at once instead of being killed an hour in. If that happens, lower
`--threads` or `--keyint`, or set the window directly with
`Y264_STREAM_WINDOW`.

The encoder workers have a budget of their own. `--max-memory` defaults to half
your RAM. When the GOP workers wouldn't fit in it, fewer run at once, each with
a wider wavefront. The thread count stays the same, and yah264 says so on
stderr.

`--frames N` encodes a segment. Splitting the input is the other way out.

## Using it from ffmpeg

Piping Y4M into the CLI works. It runs three processes on one machine, though,
and the decode alone can take a third of it. Calling the encoder as a library inside
ffmpeg removes that overhead. It's also how you'd use yah264 in a real
pipeline:

```
ffmpeg -i in.mp4 -c:v libyah264 -preset medium -crf 23 out.mp4
```

That takes one extra step today. The ffmpeg side of the integration is a
wrapper inside `libavcodec`, which is LGPL, so it can't live in this repository.
It's on the `yah264` branch of an ffmpeg fork that you build yourself. It isn't
upstream yet.

```
# 1. install the libraries, headers and pkg-config files
meson setup build -Dprefix=$HOME/.local && ninja -C build install

# 2. build the fork against them
git clone -b yah264 https://github.com/terranvigil/FFmpeg.git ffmpeg-yah264
cd ffmpeg-yah264
PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig ./configure --enable-gpl --enable-libyah264
make -j
```

`--enable-libyah264` needs `--enable-gpl`, the same as `--enable-libx264`.
yah264 is GPL-2.0-or-later, and ffmpeg's configure refuses a GPL library without
that flag. The flag puts the whole binary under the GPL. A product that can't
ship that way takes yah264's commercial license. x264 and x265 offer the same
arrangement.

The encoder takes the ffmpeg options you'd expect: `-b:v`, `-g`, `-bf`,
`-threads`, `-crf` and `-preset`. Four more are worth setting directly:
`-subme`, `-trellis`, `-aq-strength` and `-psy-rd`. Each defaults to -1, meaning
"whatever the preset chose". Setting one overrides only that.
`ffmpeg -h encoder=libyah264` prints the current list.

The encoder does its own threading. The wrapper declares
`AV_CODEC_CAP_OTHER_THREADS`, so ffmpeg passes the thread count through instead
of wrapping the encoder in frame threads of its own. `-threads 0` means auto,
as it does everywhere else here.

### Bit depth

One binary encodes both depths and picks from the input's own chroma tag. A
`C420` Y4M goes to the 8-bit library. A `C420p10` one goes to High 10.
`--output-depth 10` upshifts every sample of 8-bit input and codes it as High
10. Raw input has no tag, so it declares its depth in the format instead:
`--input-csp i420p10`, and likewise `i422p10` and `i444p10`.

Both libraries install side by side. `yah264.pc` and `yah264_10.pc` each name
their depth in `Cflags`. That's what makes a caller's `yah264_encoder_open()`
resolve to the library it configured against.

The ffmpeg wrapper offers `yuv420p`, `yuv422p` and `yuv444p` today. The wrapper
on the fork's `yah264` branch is being updated to add `yuv420p10le`,
`yuv422p10le` and `yuv444p10le`.

## Checking that it decoded

The gate the whole project depends on is available to you as well:

```
make conformance
```

It encodes each clip across a range of QPs and decodes the result with ffmpeg.
The decoder's output has to equal the encoder's own reconstruction bit for bit.
A mismatch anywhere is a hard failure.

By default ffmpeg is the only decoder it uses.
`YAH264_CONF_DECODERS="ffmpeg openh264 jm"` adds openh264 and the JM reference
decoder, so three independent readings of the specification have to agree.
[fetch_openh264.sh](https://github.com/terranvigil/yah264/blob/main/scripts/fetch_openh264.sh)
and [fetch_jm.sh](https://github.com/terranvigil/yah264/blob/main/scripts/fetch_jm.sh)
build those two.

The test clips aren't in the repository.
[fetch_corpus.sh](https://github.com/terranvigil/yah264/blob/main/scripts/fetch_corpus.sh)
downloads them. Without them the gate has nothing to run.

## What ships

Everything in this table is in the encoder today. `--help` and
[options.md](https://github.com/terranvigil/yah264/blob/main/docs/options.md)
have the defaults and the differences from x264.

| area | what ships |
|---|---|
| profiles | `--profile` takes `baseline`, `main`, `high`, `high10`, `high422` and `high444`. It refuses a tool that doesn't fit the profile you named. |
| bit depth | 8-bit and 10-bit in one binary, picked from the input's Y4M `C` tag. `--output-depth 10` codes 8-bit input as High 10. |
| chroma | 4:2:0, 4:2:2 and 4:4:4. |
| entropy coding | CABAC and CAVLC. Both must pass the conformance gate before a change merges. |
| rate control | `--crf` for constant quality, the same flag under a VBV cap for capped CRF, `--bitrate` for single-pass ABR, `--qp` for constant QP, and `--pass 1/2/3` for two-pass. `--crf-max` caps the QP a buffer may push a CRF encode to. `--ratetol` sets how far ABR may drift. `--qpfile` forces a type and QP per frame. |
| buffer signaling | `--nal-hrd vbr` or `--nal-hrd cbr` writes the buffer model into the stream. Under `cbr`, `--filler` pads every access unit up to the rate. |
| slices | `--slices N` cuts each picture into N independently decodable slices, one NAL each. |
| field coding | `--tff` and `--bff` code each frame as two field pictures. I, P and B fields are all coded, and `--slices` works with them. |
| error resilience | `--constrained-intra` keeps intra prediction in P and B slices off inter-coded neighbors. |
| open GOP | `--open-gop` makes every keyframe after the first a plain I picture with a recovery point instead of an IDR. The B frames before it keep their references. |
| partitions | `--partitions` picks which block shapes the mode decision may try, using x264's names: p8x8, p4x4, b8x8, i8x8, i4x4, none, all. |
| weighted prediction | `--weightp 0`, `1` or `2`. Mode 1 is the default. Mode 0 turns it off and lets a stream declare Baseline profile. Mode 2 adds a weighted copy of a reference so each block can choose. |
| shot-aware | `--cut-split` starts every shot on its own keyframe. `--shot-crf` gives each shot its own quality setting from the same scan. `--scan-out` saves that scan, and `--scan-in` reuses it on a later encode of the same input. |
| intra refresh | `--intra-refresh` replaces keyframes after the first with a column of intra macroblocks that sweeps the picture once every `--keyint` frames. No single picture arrives as a burst. |
| hardware | `--hw videotoolbox` encodes through the Mac's fixed-function H.264 engine, with our options mapped onto it. |
| SIMD | NEON on arm64, plus the dot-product and I8MM extensions where the chip has them. SVE kernels on Linux machines with 256-bit vectors, such as AWS Graviton3. SSE4.2 and AVX2 on x86-64, and AVX-512 as a build option (`-Davx512=true`). The [architectures page](architectures.html) has the details. |

Some things are left out on purpose. Each has a reason written down:

- MBAFF. Field coding covers interlaced sources with a fraction of the code
  through the macroblock, CABAC and deblocking paths.
- OpenCL. The GPU path here is Metal.
- `--sliced-threads`. Threading is GOP-parallel plus a row wavefront, so slices
  aren't needed for threading.
- Containers, muxing and filtering. yah264 takes Y4M, raw YUV or an AVFrame.
  ffmpeg does the rest.
- `--psnr` and `--ssim` inside the encoder. The test harness scores every encode
  from outside.

[what-we-dont-do.md](https://github.com/terranvigil/yah264/blob/main/docs/what-we-dont-do.md)
is the full list, with the reason for each line.

## The full option list

`yah264 --help` prints every flag with its default. Beyond that there are about
350 `Y264_*` environment knobs. They're research instruments and change between
commits without notice.
[knob_census.py](https://github.com/terranvigil/yah264/blob/main/scripts/knob_census.py)
generates the catalog in
[knobs.md](https://github.com/terranvigil/yah264/blob/main/docs/knobs.md),
where the exact count lives.
