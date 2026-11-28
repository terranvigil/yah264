---
title: Getting started - yah264
description: How to build yah264, run it and pick the options that matter.
---

# Getting started

## Build

yah264 builds with Meson and Ninja and needs only a C11 compiler. The Makefile
wraps the usual commands:

```
make build
```

That builds into `build/`. `make test` runs the unit tests, `make conformance`
the recon-match gate below, and `make help` lists the rest.

One build makes an 8-bit and a 10-bit library. The `y264` command links both,
and `yah264` is an alias for it. `-Dbit_depth` only picks the depth the tests and
in-tree tools build at. `-Dgpu=enabled`, off by default, links the Metal compute
library.

## Encode something

The shortest useful command reads Y4M and writes an Annex-B stream:

```
y264 --input-y4m in.y4m -o out.264
```

Input is a flag. Either end accepts `-` for a pipe.
Raw YUV goes through `--input-raw` with `--input-res WxH`, plus `--input-csp`,
`--fps` and `--input-range` for what a Y4M header would hold. `--seek N` skips N
input frames, and `--crop-rect L,T,R,B` crops in luma samples.

The defaults are close to x264's medium preset:

```
--preset medium --cabac --ref 3 --bframes 3 --transform-8x8
```

Most measurements on this site use them. Two defaults differ from x264's on
purpose, swept against our own quality band:

| option | yah264 | x264 |
|---|--:|--:|
| `--aq-strength` | 0.25 | 1.0 |
| `--psy-rd` | 2.0 | 1.0 |

Psychovisual RD trades pixel accuracy for visible texture, the setting most
likely to flatter a VMAF comparison. `--tune psnr` turns both off.

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
y264 --input-y4m in.y4m --crf 21 --vbv-maxrate 6000 --vbv-bufsize 12000 -o out.264
```

Easy titles encode at CRF 21 and come out small. Because hard titles stay under
the ceiling, nothing ends up undeliverable. Each GOP after the first assumes a
half-full buffer. That keeps concatenated segments safe at the cost of a few bits. Short
keyints run hot, so aim the cap low on two-second segments.

Broadcast and live target a rate: pair the VBV flags with `--bitrate`. A cap
equal to the target gives CBR. A higher cap gives capped VBR.

`--nal-hrd vbr` or `--nal-hrd cbr` writes the buffer model into the stream so a
receiver can verify it. It needs both VBV flags and a frame rate. Under `cbr`,
`--filler` is already on and pads each access unit up to the rate.
`--no-filler` removes the padding but keeps the CBR headers. Use it to measure,
never to ship.

Two-pass is for bitrate targets: `--pass 1` then `--pass 2` with `--bitrate`.
`--pass 3` reads and rewrites the stats, which lets each further pass refine
against a real encode. There's no two-pass CRF. On the threaded path two-pass
needs a seekable input.

## Presets

`--preset` runs from `ultrafast` through `medium` to `veryslow` and `placebo`,
setting the subpel and mode-decision tier. Motion search follows it unless
`--me` overrides: hex up to medium, umh from slow. Medium is the default because
every comparison here is against x264 medium.

`--tune` takes `grain`, `film`, `animation`, `psnr`, `ssim`, `zerolatency`,
`stillimage` and `fastdecode`. Only `zerolatency` trades quality for latency,
turning off B frames and both lookaheads (rate control and sync).

Four tunes come with a caveat:

- `grain` lowers psy-rd from 2.0 to 1.5, a direction nobody has re-measured.
- `film` isn't BD-measured, because the test set has no film clip.
- `stillimage` borrows x264's constants, unmeasured for want of a still clip.
- `fastdecode` turns off deblocking, CABAC, weighted biprediction and explicit
  P weighted prediction (`--weightp 0`), a deliberate quality loss.

## Other flags

[options.md](https://github.com/terranvigil/yah264/blob/main/docs/options.md)
documents every flag, its default and how it differs from x264. Beyond the
[table below](#what-ships):

- `--profile` also narrows any tool the preset chose.
- An interlaced Y4M turns field coding on by itself. `--no-interlaced` refuses
  it.
- `--constrained-intra` costs bits for error resilience.
- `--deblock A:B` moves the in-loop filter offsets.
- `--aud` adds an access unit delimiter and `--pic-struct` a pic_timing SEI.
  Neither changes a sample.
- `--cut-split` and `--shot-crf` encode per shot in one pass. See the
  [shot-aware section](index.html#shot-aware-support) of the home page.

## Threading and the memory it costs

`--threads` defaults to auto: the smaller of your core count and 16, capped by
what the picture can absorb. An explicit `--threads N` is clamped the same way.
The [design page](design.md) explains the conservative 16 and the caps.

The threaded path streams, writing each GOP as it finishes, so clip length
isn't the limit. From a file, only a few frames per worker are in memory. From a
pipe, the reader holds up to `(--threads + 1) x --keyint` frames. Neither grows
with the length of the clip.

yah264 prices that window up front. A job needing more than half your RAM fails
at once, quoting the figure, instead of being killed an hour in. Lower
`--threads` or `--keyint`, set `Y264_STREAM_WINDOW`, encode a segment with
`--frames N`, or split the input.

`--max-memory` (default half your RAM) budgets the GOP workers. If they don't
fit, fewer run at once with wider wavefronts, at the same thread count, and
stderr says so.

## Using it from ffmpeg

Piping Y4M into the CLI runs three processes on one machine, and the decode
alone can take a third of it. Calling the encoder as a library inside ffmpeg
removes that overhead, and it's how you'd use yah264 in a real pipeline:

```
ffmpeg -i in.mp4 -c:v libyah264 -preset medium -crf 23 out.mp4
```

Because `libavcodec` is LGPL, the wrapper inside it can't live in this
repository. Until it's upstream, build it from the `yah264` branch of an ffmpeg
fork:

```
# 1. install the libraries, headers and pkg-config files
meson setup build -Dprefix=$HOME/.local && ninja -C build install

# 2. build the fork against them
git clone -b yah264 https://github.com/terranvigil/FFmpeg.git ffmpeg-yah264
cd ffmpeg-yah264
PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig ./configure --enable-gpl --enable-libyah264
make -j
```

`--enable-libyah264` needs `--enable-gpl`, as `--enable-libx264` does, because
yah264 is GPL-2.0-or-later. That puts the whole binary under the GPL. Products
that can't ship that way take yah264's commercial license, the same arrangement
x264 and x265 offer.

The encoder takes the ffmpeg options you'd expect: `-b:v`, `-g`, `-bf`,
`-threads`, `-crf` and `-preset`. Four more are worth setting directly:
`-subme`, `-trellis`, `-aq-strength` and `-psy-rd`. Each defaults to -1,
"whatever the preset chose". `ffmpeg -h encoder=libyah264` prints the list.

The wrapper declares `AV_CODEC_CAP_OTHER_THREADS`, so ffmpeg passes the thread
count through instead of adding frame threads. `-threads 0` means auto.

### Bit depth

One binary picks the depth from the input's chroma tag: `C420` goes to the
8-bit library, `C420p10` to High 10. `--output-depth 10` upshifts 8-bit input
and codes it as High 10. Because raw input has no tag, it declares its depth in the format instead:
`--input-csp i420p10`, and likewise `i422p10` and `i444p10`. HDR10 and HLG
need a few more options, covered on the [HDR](hdr.md) page.

The libraries install side by side. `yah264.pc` and `yah264_10.pc` name their
depth in `Cflags`, so a caller's `yah264_encoder_open()` resolves to the library
it configured against. The ffmpeg wrapper takes `yuv420p`, `yuv422p` and
`yuv444p` against the 8-bit library, and `yuv420p10le`, `yuv422p10le` and
`yuv444p10le` against the 10-bit one.

## Checking that it decoded

The gate the whole project depends on is available to you as well:

```
make conformance
```

It encodes each clip across a range of QPs and requires ffmpeg's decode to equal
the encoder's reconstruction bit for bit. Any mismatch fails.
`YAH264_CONF_DECODERS="ffmpeg openh264 jm"` adds openh264 and the JM reference
decoder, built by
[fetch_openh264.sh](https://github.com/terranvigil/yah264/blob/main/scripts/fetch_openh264.sh)
and [fetch_jm.sh](https://github.com/terranvigil/yah264/blob/main/scripts/fetch_jm.sh),
so three readings of the specification must agree. The test clips aren't in the
repository: run
[fetch_corpus.sh](https://github.com/terranvigil/yah264/blob/main/scripts/fetch_corpus.sh)
first.

## What ships

Everything here is in the encoder today.

| area | what ships |
|---|---|
| profiles | `--profile` takes `baseline`, `main`, `high`, `high10`, `high422` and `high444`. It refuses a tool that doesn't fit the profile you named. |
| bit depth | 8-bit and 10-bit in one binary, picked from the input's Y4M `C` tag. `--output-depth 10` codes 8-bit input as High 10. |
| chroma | 4:2:0, 4:2:2 and 4:4:4. |
| entropy coding | CABAC and CAVLC. Both must pass the conformance gate before a change merges. |
| rate control | `--crf` for constant quality, the same flag under a VBV cap for capped CRF, `--bitrate` for single-pass ABR, `--qp` for constant QP, and `--pass 1/2/3` for two-pass. `--crf-max` caps the QP a buffer may push a CRF encode to. `--ratetol` sets how far ABR may drift. `--qpfile` forces a type and QP per frame. |
| buffer signaling | `--nal-hrd vbr` or `--nal-hrd cbr` writes the buffer model into the stream. Under `cbr`, `--filler` pads every access unit up to the rate. |
| slices | `--slices N` splits each picture into N independently decodable slices, one NAL each. |
| field coding | `--tff` and `--bff` code each frame as two field pictures. I, P and B fields are all coded, and `--slices` works with them. |
| error resilience | `--constrained-intra` keeps intra prediction in P and B slices off inter-coded neighbors. |
| open GOP | `--open-gop` makes every keyframe after the first a plain I picture with a recovery point instead of an IDR. The B frames before it keep their references. |
| partitions | `--partitions` picks which block shapes the mode decision may try, using x264's names: p8x8, p4x4, b8x8, i8x8, i4x4, none, all. |
| weighted prediction | `--weightp 0`, `1` or `2`. Mode 1 is the default. Mode 0 turns it off and lets a stream declare Baseline profile. Mode 2 adds a weighted copy of a reference so each block can choose. |
| shot-aware | `--cut-split` starts every shot on its own keyframe. `--shot-crf` gives each shot its own quality setting from the same scan. `--scan-out` saves that scan, and `--scan-in` reuses it on a later encode of the same input. |
| intra refresh | `--intra-refresh` replaces keyframes after the first with a column of intra macroblocks that sweeps the picture once every `--keyint` frames. No single picture arrives as a burst. |
| hardware | `--hw videotoolbox` encodes through the Mac's fixed-function H.264 engine, with our options mapped onto it. |
| SIMD | NEON on arm64, plus the dot-product and I8MM extensions where the chip has them. SVE kernels on Linux machines with 256-bit vectors, such as AWS Graviton3. SSE4.2, AVX2 and AVX-512 on x86-64, each used where the chip has it. `-Davx512=false` builds without AVX-512. The [architectures page](architectures.html) has the details. |

Left out on purpose:

- MBAFF. Field coding covers interlaced sources with a fraction of the code
  through the macroblock, CABAC and deblocking paths.
- OpenCL. The GPU path here is Metal.
- `--sliced-threads`. Threading is GOP-parallel plus a row wavefront, so slices
  aren't needed for threading.
- Containers, muxing and filtering. yah264 takes Y4M, raw YUV or an AVFrame.
  ffmpeg does the rest.
- `--psnr` and `--ssim` inside the encoder. The test scripts score each encode
  from outside.

[What we don't do](https://github.com/terranvigil/yah264/blob/main/docs/what-we-dont-do.md)
gives the reason for each.

## The full option list

`y264 --help` prints every flag with its default. There are also about 140
`Y264_*` environment knobs, research instruments that change without notice.
Most are read only in a `-Ddev_knobs=true` build. The few the CLI passes its own
options through are read in every build.
[knob_census.py](https://github.com/terranvigil/yah264/blob/main/scripts/knob_census.py)
generates the catalog in
[knobs.md](https://github.com/terranvigil/yah264/blob/main/docs/knobs.md),
where the exact count lives.
