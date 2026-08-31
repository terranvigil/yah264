---
title: Getting started - yah264
description: Build yah264, run it, and read the options that matter.
---

# Getting started

## Build

yah264 builds with Meson and Ninja and has no required dependencies beyond a
C11 compiler. The Makefile wraps the usual invocation, so from a clean checkout:

```
make build
```

That configures into `build/` and compiles. `make test` runs the unit tests,
`make conformance` runs the recon-match gate described below, and `make help`
lists the rest.

Two build options are worth knowing about. `-Dbit_depth=10` selects the High 10
code path and changes the pixel and coefficient types throughout, so it is a
separate build and not a runtime switch. `-Dgpu=enabled` links the Metal
compute library, and it is off by default.

## Encode something

The shortest useful command reads Y4M and writes an Annex-B stream:

```
yah264 --input-y4m in.y4m -o out.264
```

Input is a flag rather than a positional argument, and both ends accept `-` for
stdin and stdout, so yah264 can be used with pipes.

The defaults are aimed at x264's medium preset: `--preset medium --cabac --ref 3
--bframes 3 --transform-8x8 --aq-strength 0.4`. I did that so the two encoders
can be compared with no tuning arguments in between, which is how most of the
measurement on this site is done.

## Choosing a rate control mode

Three flags each select a mode.

| flag | mode | use it when |
|---|---|---|
| `--crf N` | constant quality | file size is negotiable |
| `--crf N --vbv-maxrate M --vbv-bufsize B` | capped CRF | streaming VOD and ABR ladder rungs |
| `--bitrate N` | single-pass average bitrate | you have a budget for the whole file |
| `--qp N` | constant QP | you are measuring a coding change and want the rate controller out of the way |

Capped CRF is the one most people actually want, and it is the default choice
for a VOD library or the rungs of an adaptive ladder:

```
yah264 --input-y4m in.y4m --crf 21 --vbv-maxrate 6000 --vbv-bufsize 12000 -o out.264
```

The encoder codes to the quality target and the buffer sets a ceiling. Easy
titles code at CRF 21 and come out small. Hard titles run into the ceiling and get bounded there
instead of blowing the buffer, so nothing in the library is undeliverable.

Two things to plan around. Every GOP after the first assumes a half-full
buffer, which keeps concatenated segments safe and costs a few bits. Short
keyints also run hot, so aim the cap low on two-second segments.

Broadcast and live are different. The target there is a rate rather than a
quality, so pair the VBV flags with `--bitrate`. A cap equal to the target gives
CBR, a cap above it gives capped VBR.

Two-pass belongs to bitrate targets only. `--pass 1` then `--pass 2` runs
against `--bitrate`, two-pass CRF is not implemented, and on the threaded path
two-pass needs a seekable input.

## Presets

`--preset` runs from `ultrafast` through `medium` to `veryslow` and `placebo`,
and sets the subpel and mode-decision tier. Medium is the default because the
whole comparison story on this site is against x264 medium. The motion search
follows from it too, with hex at medium and faster, and umh from slow upward,
unless `--me` overrides it.

`--tune` adjusts for content: `grain`, `film`, `animation`, `psnr`, `ssim`, and
`zerolatency`. The last one turns off the sync lookahead, which is the only
option here that buys latency with quality.

## Threading and the memory it costs

`--threads` defaults to auto, which picks the smaller of your core count and
16, then caps it by what the picture can absorb. Past 16 the coordination costs
more than the extra workers return.

The threaded path reads the whole clip into memory first, so an hour of 720p
needs around 111 GiB. yah264 prices the job up front and refuses anything
needing more than half your RAM, so it fails immediately instead of being killed
an hour in.

`--frames N` encodes a segment, and splitting the input is the other way out.
Only the serial path streams, and `--dump-recon` is what forces it.

## Using it from ffmpeg

Piping Y4M into the CLI works, but it runs three processes over one machine and
the decode alone can take a third of it. Calling the encoder as a library inside
ffmpeg removes that, and it is how you would use it in a real pipeline:

```
ffmpeg -i in.mp4 -c:v libyah264 -preset medium -crf 23 out.mp4
```

Getting there takes one extra step today, because the ffmpeg side of the
integration is a wrapper inside `libavcodec` and therefore LGPL. It cannot live
in this BSD-2 repository, so it is on the `yah264` branch of an ffmpeg fork
and you build that fork yourself. It is not upstream yet.

```
# 1. install the library, headers and yah264.pc
meson setup build -Dprefix=$HOME/.local && ninja -C build install

# 2. build the fork against it
git clone -b yah264 https://github.com/terranvigil/FFmpeg.git ffmpeg-yah264
cd ffmpeg-yah264
PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig ./configure --enable-libyah264
make -j
```

`--enable-libyah264` on its own leaves the build LGPL. That is worth stating
because `--enable-libx264` does not: x264 is GPL and ffmpeg's configure refuses
it without `--enable-gpl`, which relicenses the whole binary. yah264 is
BSD-2-Clause, so an ffmpeg that encodes with it stays under the licence it
started with.

The encoder takes the ffmpeg options you would expect -- `-b:v`, `-g`, `-bf`,
`-threads`, `-crf` -- plus `-preset`, and a few of the knobs worth reaching for
directly: `-subme`, `-trellis`, `-aq-strength`, `-psy-rd`. Each defaults to -1
meaning "whatever the preset chose", so setting one overrides just that.
`ffmpeg -h encoder=libyah264` prints the current list.

Threading is the encoder's own. The wrapper declares
`AV_CODEC_CAP_OTHER_THREADS`, so ffmpeg passes the thread count through and does
not wrap the encoder in frame threads of its own. `-threads 0` means auto, the
same as everywhere else here.

### Bit depth

The library is compiled for one bit depth, so an 8-bit build encodes the 8-bit
formats and a 10-bit build encodes the 10-bit ones. There is no runtime switch,
and that reaches all the way out to ffmpeg: a 10-bit pipeline needs
`-Dbit_depth=10` on the library, its own prefix, and its own ffmpeg built
against it.

| library build | pixel formats ffmpeg will offer |
|---|---|
| default (8-bit) | `yuv420p`, `yuv422p`, `yuv444p` |
| `-Dbit_depth=10` | `yuv420p10le`, `yuv422p10le`, `yuv444p10le` |

`yah264.pc` sets the depth in its `Cflags`, so configure picks it up and the
wrapper compiles for whichever library it found.

## Checking that it decoded

The gate the whole project depends on is available to you as well:

```
make conformance
```

It encodes each clip across a range of QPs, decodes the result with ffmpeg, and
asserts that the decoder's output equals the encoder's own reconstruction bit for
bit. A mismatch anywhere is a hard failure.

The corpus itself is not in the repository. `scripts/fetch_corpus.sh` pulls the
clips, and without them the gate has nothing to run.

## The full option list

`yah264 --help` prints every flag with its default. Beyond that there are 286 `Y264_*`
environment knobs, and they are research instruments. They live in
`docs/knobs.md`, which is generated by `scripts/knob_census.py`, and they change
between commits without notice.
