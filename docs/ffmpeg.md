# ffmpeg integration

`libyah264` is an encoder wrapper in a fork of ffmpeg
(`github.com/terranvigil/FFmpeg`, branch `yah264`) that makes
`-c:v libyah264` work like `-c:v libx264`, with decode, encode and mux in one
process instead of three piped together. It's LGPL code inside libavcodec,
so it lives in the fork. This repo supplies the installable library: a
shared object, the public header and pkg-config files. The speed board
(`scripts/ffboard.py`) uses it to run both encoders in one ffmpeg process.

## How it works

The wrapper uses ffmpeg's receive-packet encoder API and declares
`AV_CODEC_CAP_OTHER_THREADS`, so ffmpeg adds no frame threads of its own. At
init it builds a parameter struct with `yah264_param_default`, applies the
preset, maps the options below and opens the encoder.

| ffmpeg | yah264 | note |
|---|---|---|
| `-preset` | preset | default `medium` |
| `-crf` | CRF | default 23; used when no bitrate is set |
| `-b:v` | ABR, kbit/s | any positive bitrate selects ABR |
| `-maxrate`, `-bufsize` | VBV max rate and buffer | |
| `-g` | keyint | left to the preset when unset |
| `-bf` | B-frame count | left to the preset when unset |
| `-refs` | reference count | only when explicitly set (see below). At medium on a picture 3840 or more on its longer side, `-refs 3` becomes 2, because the wrapper doesn't mark it as explicit |
| `-threads` | thread budget | 0 passes through as "encoder decides" |
| sample aspect ratio | SAR | |
| frame rate, or the inverse time base | timebase | |
| color range, primaries, transfer, matrix, chroma location | VUI video signal | only what's set; untagged input signals nothing |
| mastering display and content light level side data on the codec context | the HDR10 SEI | ffmpeg copies a tagged input's side data there from its first frame |
| `-mastering-display`, `-cll` | the CLI options of the same name and syntax | private options for inputs with no side data; they override it |
| `-subme`, `-trellis`, `-aq-strength`, `-psy-rd` | the same settings | private options; -1 keeps the preset's value |

The wrapper has no `-tune`, no free-form parameter string and no way to ask
for the alternative transfer SEI yet.

ffmpeg takes primaries and transfer from the input frames, and Y4M can't
carry them, so tag an untagged input on the frames:

```
ffmpeg -i in.y4m -vf setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv \
  -c:v libyah264 -mastering-display 'G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)' \
  -cll 1000,400 out.mp4
```

`-color_primaries` and `-color_trc` alone don't reach the encoder.
`-colorspace` and `-color_range` do, because ffmpeg negotiates those.

An 8-bit build takes yuv420p, yuv422p and yuv444p, and a 10-bit build their
10-bit forms. The wrapper is compiled for one depth and refuses a library of
the other at init (`yah264_bit_depth()`).

### Packets and timestamps

One encode call can return several coded frames' NAL units in coding
order. The wrapper splits them on slice boundaries into one packet per
frame, with parameter sets and SEI attached to the slice they precede. Each
packet's pts is the display index the encoder reports
(`yah264_encoder_frame_order`).

dts trails pts by the stream's reorder depth only:

| mode | reorder depth |
|---|---|
| B pyramid | the pyramid depth |
| flat B | 1 |
| no B frames | 0 |

ABR and VBV keep a wider margin, the lookahead delay plus the B count,
because those modes can emit anchors early.

### Headers

| case | where the headers go |
|---|---|
| `AV_CODEC_FLAG_GLOBAL_HEADER` set | SPS and PPS to extradata; any SEI from the headers in-band ahead of the first packet |
| flag not set | all of them in-band ahead of the first packet, which lets raw Annex-B output decode |
| every later IDR | SPS and PPS repeated, letting a transport stream be joined or seeked there |
| HDR SEI | written in-band by the encoder, ahead of every IDR and recovery point |

## Building it

The CLI and in-tree tools link the static library. The shared library is
installed as one copy per bit depth. Each copy has its own pkg-config file
(`yah264` or `yah264_10`) whose Cflags set `-DY264_BIT_DEPTH`. Its soname is
the ABI version, currently 4.

```
FFB=$(cd .. && pwd)/build/ffboard
meson setup build -Dbuildtype=release -Dprefix="$FFB/y264inst"
ninja -C build install

git clone -b yah264 git@github.com:terranvigil/FFmpeg.git "$FFB/ffmpeg-yah264"
cd "$FFB/ffmpeg-yah264"
PKG_CONFIG_PATH="$FFB/y264inst/lib/pkgconfig" \
  ./configure --enable-gpl --enable-libyah264
rm -f libavcodec/libyah264.o libavcodec/libyah264.d ffmpeg ffmpeg_g
make -j6
git -C /path/to/yah264 hash-object include/yah264.h > yah264-header.sha
```

`--enable-gpl` is required because the library is GPL-2.0-or-later. Products
that can't ship a GPL ffmpeg take the commercial license. For `ffboard.py`,
also configure `--enable-libx264` against the reference builds
([fair-build recipe](instruments.md)).

**Rebuild the fork whenever `include/yah264.h` changes**, and rewrite
`yah264-header.sha`. The wrapper reads the parameter struct by offset. A
moved field means wrong settings.

## What we chose and why

| choice | why |
|---|---|
| the CLI links the static library; the shared one is installed | linking the CLI against the dylib lost cross-unit inlining and cost 12-14% wall time on every board |
| hidden symbol visibility in the shared library | because internal calls stay direct, an outside caller doesn't pay for the export table on every kernel dispatch |
| from ABI 3 on, the parameter struct records its own size and open refuses a mismatch | a wrapper one field behind the library compiled, loaded and encoded garbage settings with no error |
| soname = ABI version | an ffmpeg built against an older ABI can't load a newer install at all |
| depth in pkg-config Cflags, and `yah264_bit_depth()` checked at init | a consumer compiled for the wrong depth links fine and reads every plane at the wrong stride |
| `-refs` applied only when explicitly set | ffmpeg's default of 1 looks like a real value. Treating it as a choice ran every encode at one reference. In exchange, an explicit `-refs 1` reads as unset, and at medium with a longer side of 3840 or more an explicit `-refs 3` becomes 2 |
| extradata holds SPS and PPS only | MP4 and Matroska rebuild extradata as a decoder configuration record with no room for SEI. Mastering display and light level SEI written there never reached the file |
| HDR10 metadata read from the codec context at init, not from frames | the encoder takes it at open, before the first frame. Side data that only appears on a later frame can't reach the stream |
| threads passed through unchanged | core counting and clamping are the encoder's policy |
| input frames submitted one at a time | the encoder parallelizes on its row wavefront, which the thread budget drives. Queuing 1 to 256 frames ahead made no measurable difference |
| written from ffmpeg's public headers and encoder-API docs only | the wrapper must not copy another encoder's wrapper. Resembling one where the AVCodec interface dictates the structure is fine |

## What didn't work

| idea | why not |
|---|---|
| counting the encoder's lookahead delay in dts for CRF | put the first dts seven frames early at the defaults; players that honor the offset stall at the start |
| SPS/PPS only in extradata | raw Annex-B output started at an IDR slice and nothing would decode it. MP4 hid the bug |
| `Y264LIB` as proof of which library loaded | dyld looks up the leaf name. It skips an install with a different soname and loads the link-time path instead. `otool -L` on the binary is the proof |

## Open questions

| question | detail |
|---|---|
| mapping ffmpeg's mid-stream changes onto [live control](live-control.md) | not done yet. A forced I frame would become a keyframe event on that frame. A bitrate, maxrate or buffer-size change would become a rate event on the next frame |
| which ffmpeg release to pin, and when to go upstream | not decided |
| exposing the recon callback (`yah264_encoder_set_recon_cb`) | would let the recon-match gate run in-process |
| a 10-bit wrapper | the fork's configure asks pkg-config for `yah264`, the 8-bit library, and can't yet point at `yah264_10`. Until it can, put first on `PKG_CONFIG_PATH` a directory holding a copy of `yah264_10.pc` renamed `yah264.pc`, with its `Name:` line changed to match |

## Measuring it

The ffmpeg board is a control for the CLI speed board, whose numbers we
report. At the same operating points the two agree within noise on every
clip, so speed gaps don't live in the CLI's Y4M input path.

| setting | meaning |
|---|---|
| `FF` | the ffmpeg to run; default `../build/ffboard/ffmpeg-yah264/ffmpeg` |
| `Y264_FFBOARD_ROOT` | moves that default set |
| `X264LIB`, `Y264LIB` | the libraries to load; no defaults, on purpose |

The board refuses to run when `$Y264LIB/include/yah264.h` doesn't match
`yah264-header.sha`, and prints INVALID when the two encoders' sizes differ.

| trap | what to do |
|---|---|
| after installing to a new prefix, the wrapper's `.d` still names the old header, so make exits 0 and keeps the old wrapper | delete the wrapper's `.o` and `.d` before `make` |
| a stray pkg-config entry for another architecture resolves, then fails to link | set `PKG_CONFIG_PATH` explicitly; `otool -L ffmpeg` should name only the prefixes under `../build/ffboard` |
| an ABI bump | expect a small wrapper fix; a clean build of the fork shows the wrapper is current |
