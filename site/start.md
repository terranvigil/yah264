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

One build gives you both sample depths. `make build` produces an 8-bit encoder
library and a 10-bit one. A single `yah264` binary links both. Each library
installs with its own pkg-config file, `yah264.pc` and `yah264_10.pc`.
`-Dbit_depth` does not change any of that. It only selects the depth the tests
and the in-tree tools build at.

`-Dgpu=enabled` links the Metal compute library. It is off by default.

## Encode something

The shortest useful command reads Y4M and writes an Annex-B stream:

```
yah264 --input-y4m in.y4m -o out.264
```

Input is a flag. There is no positional input argument. Both ends accept `-` for
stdin and stdout, so yah264 works with pipes.

Headerless planar YUV goes in through `--input-raw`. That one needs
`--input-res WxH`. `--input-csp`, `--fps` and `--input-range` supply the rest of
what a Y4M header would have carried. `--seek N` drops the first N input
frames. `--crop-rect L,T,R,B` crops the input before encoding, in luma samples.

The defaults are near x264's medium preset: `--preset medium --cabac --ref 3 --bframes 3 --transform-8x8`.
That way the two encoders can be compared without a pile of tuning arguments in
between. Most of the measurement on this site is done that way.

Two of them are deliberately not the reference's. We swept both against our own
quality band, so a comparison has to account for them: `--aq-strength 0.4` where
the reference runs 1.0, and `--psy-rd 2.0` where it runs 1.0. Psychovisual RD
trades pixel accuracy for apparent texture, so it is the one most likely to
flatter a VMAF-scored comparison. `--tune psnr` turns both off if you want a
psy-free run.

## Choosing a rate control mode

Three flags each select a mode.

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

The encoder codes to the quality target and the buffer sets a ceiling. Easy
titles code at CRF 21 and come out small. Hard titles run into the ceiling and
get bounded there. Nothing in the library is undeliverable.

Two things to plan around. Every GOP after the first assumes a half-full buffer.
That keeps concatenated segments safe and costs a few bits. Short keyints also
run hot, so aim the cap low on two-second segments.

Broadcast and live are different. The target there is a rate, so pair the VBV
flags with `--bitrate`. A cap equal to the target gives CBR. A cap above it
gives capped VBR.

`--nal-hrd vbr` or `--nal-hrd cbr` writes the buffer model into the stream
itself, so a receiver can verify the buffer instead of trusting it. It needs
both VBV flags and a frame rate. Without them it is refused. Under `cbr`,
`--filler` is already on and pads every access unit up to the rate.
`--no-filler` takes the padding away and leaves the CBR headers in place. Use
that one to measure, never to ship.

Two-pass belongs to bitrate targets only. `--pass 1` then `--pass 2` runs
against `--bitrate`. `--pass 3` reads the stats and writes them back, so a
further pass refines against a real encode. Two-pass CRF is not implemented. On
the threaded path two-pass needs a seekable input.

## Presets

`--preset` runs from `ultrafast` through `medium` to `veryslow` and `placebo`.
It sets the subpel and mode-decision tier. Medium is the default because the
whole comparison story on this site is against x264 medium. The motion search
follows from it too. Medium and faster get hex, slow and up get umh, unless
`--me` overrides it.

`--tune` adjusts for content: `grain`, `film`, `animation`, `psnr`, `ssim`,
`zerolatency`, `stillimage` and `fastdecode`. `zerolatency` turns off the sync
lookahead. It is the only option here that buys latency with quality.

Four of the tunes come with a caveat. `grain` sets psy-rd to 1.5, down from the
2.0 default. Nobody has re-measured it in that direction. `film`
has not been BD-measured at all, for want of a film clip among the test clips.
`stillimage` borrows the reference tune's constants and nothing here has
measured them, because there is no still-image clip to measure them on.
`fastdecode` turns off deblocking, CABAC and weighted biprediction. That is a
real quality loss on purpose. Explicit P weighted prediction belongs in that set
too and cannot be turned off yet.

## Other flags

[options.md](https://github.com/terranvigil/yah264/blob/main/docs/options.md) documents every flag with its default and its differences from
the reference encoder. A few are worth knowing about here. `--profile` is
enforced. Naming one narrows a tool the preset chose and refuses a tool you
asked for that does not fit. `--slices N` cuts each picture into N independently
decodable slices. `--tff` and `--bff` code each frame as two field pictures. An
interlaced Y4M turns field coding on by itself, and `--no-interlaced` refuses
it. `--constrained-intra` keeps intra prediction in P and B slices off
inter-coded neighbours. That costs bits and buys error resilience.
`--deblock A:B` moves the in-loop filter offsets. `--aud` opens every access
unit with a delimiter. `--pic-struct` adds a pic_timing SEI per picture. Neither of those two moves a
sample.

`--cut-split` and `--shot-crf` encode per shot in one pass. The
[shot-aware section](index.html#shot-aware-support) of the home page covers
both.

## Threading and the memory it costs

`--threads` defaults to auto, which picks the smaller of your core count and
16, then caps it by what the picture can absorb. The 16 is a conservative
default and nobody measured a knee there. Wavefront scaling runs out on the
picture's critical path before it runs out of machine. On an asymmetric machine
the last workers end up on efficiency cores. An explicit `--threads N`
is honoured, then clamped by the picture. The [design page](design.md) has the
caps and what has actually been measured.

The threaded path streams. It reads on its own thread through a bounded window
and writes each GOP as it finishes, so clip length is not the ceiling. The
window is what has to fit, at worst `(--threads + 1) x --keyint` frames. That
does not grow with the length of the clip. yah264 prices it up front and refuses
a job needing more than half your RAM. The refusal quotes the figure and the
window it came from, so the job fails immediately instead of being killed an
hour in. Lower `--threads` or `--keyint` if it does, or set the window directly
with `Y264_STREAM_WINDOW`.

`--frames N` encodes a segment. Splitting the input is the other way out.

## Using it from ffmpeg

Piping Y4M into the CLI works, but it runs three processes over one machine.
The decode alone can take a third of it. Calling the encoder as a library inside
ffmpeg removes that. It is also how you would use it in a real pipeline:

```
ffmpeg -i in.mp4 -c:v libyah264 -preset medium -crf 23 out.mp4
```

Getting there takes one extra step today, because the ffmpeg side of the
integration is a wrapper inside `libavcodec` and therefore LGPL. It cannot live
in this repository. It is on the `yah264` branch of an ffmpeg fork. You
build that fork yourself. It is not upstream yet.

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
yah264 is GPL-2.0-or-later. ffmpeg's configure refuses a GPL library without
that flag, and the flag puts the whole binary under the GPL. A product that
cannot ship that way takes yah264's commercial licence. That is the arrangement
x264 and x265 offer.

The encoder takes the ffmpeg options you would expect: `-b:v`, `-g`, `-bf`,
`-threads`, `-crf` and `-preset`. Four more are worth reaching for directly,
`-subme`, `-trellis`, `-aq-strength` and `-psy-rd`. Each defaults to -1. That
means "whatever the preset chose", so setting one overrides just that.
`ffmpeg -h encoder=libyah264` prints the current list.

Threading is the encoder's own. The wrapper declares
`AV_CODEC_CAP_OTHER_THREADS`, so ffmpeg passes the thread count through and does
not wrap the encoder in frame threads of its own. `-threads 0` means auto, the
same as everywhere else here.

### Bit depth

One binary encodes both depths and picks from the input's own chroma tag. A
`C420` Y4M goes to the 8-bit library and a `C420p10` one goes to High 10.
`--output-depth 10` puts 8-bit input through High 10 after an explicit upshift
of every sample. Raw input has no tag, so it declares its
depth in the format instead: `--input-csp i420p10`, and likewise `i422p10` and
`i444p10`.

Both libraries install side by side. `yah264.pc` and `yah264_10.pc` each name
their depth in `Cflags`. That is what makes a caller's `yah264_encoder_open()`
resolve to the one it configured against.

The ffmpeg wrapper offers `yuv420p`, `yuv422p` and `yuv444p` today. The wrapper
on the fork's `yah264` branch is being updated to offer both format sets. That
adds `yuv420p10le`, `yuv422p10le` and `yuv444p10le`.

## Checking that it decoded

The gate the whole project depends on is available to you as well:

```
make conformance
```

It encodes each clip across a range of QPs and decodes the result with ffmpeg.
The decoder's output has to equal the encoder's own reconstruction bit for bit.
A mismatch anywhere is a hard failure.

ffmpeg is the only decoder it uses by default.
`YAH264_CONF_DECODERS="ffmpeg openh264 jm"` runs openh264 and the JM reference
decoder alongside it, so three independent readings of the specification have to
agree. [fetch_openh264.sh](https://github.com/terranvigil/yah264/blob/main/scripts/fetch_openh264.sh) and [fetch_jm.sh](https://github.com/terranvigil/yah264/blob/main/scripts/fetch_jm.sh) build those two.

The test clips are not in the repository. [fetch_corpus.sh](https://github.com/terranvigil/yah264/blob/main/scripts/fetch_corpus.sh) pulls them.
Without them the gate has nothing to run.

## What ships

Everything in this table is in the encoder today. `--help` and
[options.md](https://github.com/terranvigil/yah264/blob/main/docs/options.md)
have the defaults and the differences from the reference encoder.

| area | what ships |
|---|---|
| profiles | `--profile` takes `baseline`, `main`, `high`, `high10`, `high422` and `high444`. It refuses a tool that does not fit the profile you named. |
| bit depth | 8-bit and 10-bit in one binary, picked from the input's Y4M `C` tag. `--output-depth 10` codes 8-bit input as High 10. |
| chroma | 4:2:0, 4:2:2 and 4:4:4. |
| entropy coding | CABAC and CAVLC. Both must pass the conformance gate before a change merges. |
| rate control | `--crf` for constant quality, the same flag under a VBV cap for capped CRF, `--bitrate` for single-pass ABR, `--qp` for constant QP, and `--pass 1/2/3` for two-pass. |
| buffer signalling | `--nal-hrd vbr` or `--nal-hrd cbr` writes the buffer model into the stream itself. Under `cbr`, `--filler` pads every access unit up to the rate. |
| slices | `--slices N` cuts each picture into N independently decodable slices, one NAL each. |
| field coding | `--tff` and `--bff` code each frame as two field pictures. I, P and B fields are all coded, and `--slices` composes with them. |
| error resilience | `--constrained-intra` keeps intra prediction in P and B slices off inter-coded neighbours. |
| open GOP | `--open-gop` makes every keyframe after the first a plain I picture with a recovery point instead of an IDR, so the B frames before it keep their references. |
| shot-aware | `--cut-split` starts every shot on its own keyframe. `--shot-crf` gives each shot its own quality setting from the same scan. |
| hardware | `--hw videotoolbox` encodes through the Mac's fixed-function H.264 engine with our options mapped onto it. |
| SIMD | NEON kernels ship on arm64. The x86-64 dispatch and build shape ship with the kernels still to come. |

Some things are left out on purpose. Each one has a reason written down.

- MBAFF. Field coding covers interlaced sources for a fraction of the code through the macroblock, CABAC and deblocking paths.
- OpenCL. The GPU path here is Metal.
- `--sliced-threads`. Threading is GOP-parallel plus a row wavefront, so slices are not needed as a threading vehicle.
- Containers, muxing and filtering. yah264 takes Y4M, raw YUV or an AVFrame, and ffmpeg does the rest.
- `--psnr` and `--ssim` inside the encoder. The harness scores every encode from outside it.
- `--weightp` has no equivalent yet. Explicit P weighted prediction is always on and cannot be turned off.

[what-we-dont-do.md](https://github.com/terranvigil/yah264/blob/main/docs/what-we-dont-do.md)
is the full ledger, with the reason against each line.

## The full option list

`yah264 --help` prints every flag with its default. Beyond that there are around
350 `Y264_*` environment knobs. They are research instruments and they change
between commits without notice. [knob_census.py](https://github.com/terranvigil/yah264/blob/main/scripts/knob_census.py) generates the catalogue
in [knobs.md](https://github.com/terranvigil/yah264/blob/main/docs/knobs.md), where the exact count lives.
