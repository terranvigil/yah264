# ffmpeg integration

`libyah264` is an encoder wrapper in a fork of ffmpeg. With it, `-c:v libyah264`
works the way `-c:v libx264` does. Decode, encode and mux share one process
instead of three piped together. The fork lives at
`github.com/terranvigil/FFmpeg` on the `yah264` branch. Because the wrapper is part of
libavcodec and is LGPL, it can't live in this repository. This repo's side
is the installable library: a shared object, the public header and pkg-config
files.

The speed board in `scripts/ffboard.py` runs it, calling both encoders as
libraries inside one ffmpeg process.

## How it works

The wrapper uses ffmpeg's receive-packet encoder API and declares
`AV_CODEC_CAP_OTHER_THREADS`. That flag stops ffmpeg from wrapping it in its
own frame threads. At init it builds a parameter struct with `yah264_param_default`,
applies the preset, maps the options below and opens the encoder.

| ffmpeg | yah264 | note |
|---|---|---|
| `-preset` | preset | default `medium` |
| `-crf` | CRF | default 23; used when no bitrate is set |
| `-b:v` | ABR, kbit/s | any positive bitrate selects ABR |
| `-maxrate`, `-bufsize` | VBV max rate and buffer | |
| `-g` | keyint | left to the preset when unset |
| `-bf` | B-frame count | left to the preset when unset |
| `-refs` | reference count | only when explicitly set (see below) |
| `-threads` | thread budget | 0 passes through as "encoder decides" |
| sample aspect ratio | SAR | |
| frame rate, or the inverse time base | timebase | |
| color range, primaries, transfer, matrix, chroma location | VUI video signal | only what's set; untagged input signals nothing |
| mastering display and content light level side data on the codec context | the HDR10 SEI | ffmpeg puts a tagged input's side data there from its first frame |
| `-mastering-display`, `-cll` | the same as the CLI's options, same syntax | private options for inputs that have no side data. They override it |
| `-subme`, `-trellis`, `-aq-strength`, `-psy-rd` | the same settings | private options; -1 keeps the preset's value |

The wrapper has no `-tune` and no free-form parameter string yet. It also has
no way to ask for the alternative transfer SEI.

Because ffmpeg takes an encoder's primaries and transfer from its input frames
and Y4M has no field for them, tag an untagged input on the frames:

```
ffmpeg -i in.y4m -vf setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv \
  -c:v libyah264 -mastering-display 'G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)' \
  -cll 1000,400 out.mp4
```

`-color_primaries` and `-color_trc` on their own don't reach the encoder.
`-colorspace` and `-color_range` do, because ffmpeg negotiates those.

Pixel formats are yuv420p, yuv422p and yuv444p from an 8-bit build. A 10-bit
build takes their 10-bit forms. The wrapper is compiled for one depth and
refuses a library of the other at init by checking `yah264_bit_depth()`.

The encoder returns NAL units in coding order. One call can return several
coded frames. The wrapper splits them into one packet per frame on slice
boundaries, attaching parameter sets and SEI to the slice they precede. It
labels each packet's pts from the display index the encoder reports
(`yah264_encoder_frame_order`).

dts trails pts by the stream's reorder depth only: the pyramid depth, 1 for
flat B, 0 without B. ABR and VBV keep a wider margin, the lookahead delay plus
the B count, because those modes can emit anchors early.

With `AV_CODEC_FLAG_GLOBAL_HEADER` the SPS and PPS go to extradata while any
SEI from the headers goes in-band ahead of the first packet. Without the flag,
all of them are written in-band ahead of the first packet so that raw Annex-B
output decodes. The SPS and PPS are also repeated ahead of every later IDR so that a
transport stream can be joined or seeked there. The encoder writes its
HDR SEI in-band itself, ahead of every IDR and recovery point.

## Building it

The library builds in two forms. The CLI and the in-tree tools link the static
one. The shared one is what gets installed. Both depths install side by
side under different names. Each has its own pkg-config file, `yah264` or
`yah264_10`, whose Cflags set `-DY264_BIT_DEPTH`. The soname is the ABI
version, currently 3.

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
also configure `--enable-libx264` against the reference builds;
[instruments.md](instruments.md) has the fair-build recipe.

**Rebuild the fork whenever `include/yah264.h` changes**, and rewrite
`yah264-header.sha`. The wrapper reads the parameter struct by offset, and after a
field moves the library reads the wrong settings.

## What we chose and why

| choice | why |
|---|---|
| the CLI links the static library, the shared one is installed | linking the CLI against the dylib lost cross-unit inlining and cost 12-14% wall time on every board |
| hidden symbol visibility in the shared library | an outside caller doesn't pay for the export table on every kernel dispatch because internal calls stay direct |
| the parameter struct records its own size, as of ABI 3, and open refuses a mismatch | a wrapper one field behind the library compiled, loaded and encoded garbage settings with no error |
| soname = ABI version | an ffmpeg built against an older ABI can't load a newer install at all |
| depth in pkg-config Cflags, and `yah264_bit_depth()` checked at init | a consumer compiled for the wrong depth links fine and reads every plane at the wrong stride |
| `-refs` applied only when explicitly set | ffmpeg's default of 1 looks like a real value. Treating it as a choice ran every encode at one reference. In exchange, an explicit `-refs 1` reads as unset |
| extradata holds SPS and PPS only | because MP4 and Matroska rebuild extradata as a decoder configuration record with no room for SEI, the mastering display and light level SEI written there never reached the file |
| HDR10 metadata read from the codec context at init, not from frames | the encoder takes it at open, before the first frame. Side data that only appears on a later frame can't reach the stream |
| threads passed through unchanged | core counting and clamping are the encoder's policy |
| input frames submitted one at a time | the encoder parallelizes on its row wavefront, which the thread budget drives; queuing 1 to 256 frames ahead measured no difference |
| written from ffmpeg's public headers and encoder-API docs only | the wrapper must not copy another encoder's wrapper. Where the AVCodec interface dictates the structure, resembling one is fine |

## What didn't work

| idea | why not |
|---|---|
| counting the encoder's lookahead delay in dts for CRF | put the first dts seven frames early at the defaults; players that honor the offset stall at the start |
| SPS/PPS only in extradata | raw Annex-B output started at an IDR slice and nothing would decode it. MP4 hid the bug |
| `Y264LIB` as proof of which library loaded | because dyld looks up the leaf name, it skips an install with a different soname and loads the link-time path instead. `otool -L` on the binary is the proof |

## Open questions

- Does the encoder need a reconfiguration path? ffmpeg can change bitrate
  mid-stream, and `yah264_encoder_open` takes parameters once. See
  [live-control.md](live-control.md).
- Which ffmpeg release to pin the fork to, and when to submit upstream.
- Whether to expose the recon callback (`yah264_encoder_set_recon_cb`) through
  the wrapper. That would let the recon-match gate run in-process.
- The fork's configure asks pkg-config for `yah264`, the 8-bit library. A
  10-bit wrapper needs it pointed at `yah264_10`, and configure doesn't do that
  yet. Until it does, a directory holding a copy of `yah264_10.pc` renamed
  `yah264.pc`, with its `Name:` line changed to match, put first on
  `PKG_CONFIG_PATH`, builds the 10-bit wrapper.

## Measuring it

The ffmpeg board is a control for the CLI speed board, which gives the
headline numbers. When both run at the same operating points they agree within
noise on every clip, which tells us the CLI's Y4M input path isn't where speed
gaps live. `FF` names the ffmpeg
(default `../build/ffboard/ffmpeg-yah264/ffmpeg`; `Y264_FFBOARD_ROOT` moves the
set). `X264LIB` and `Y264LIB` have no defaults on purpose. The board refuses to
run when `$Y264LIB/include/yah264.h` doesn't match `yah264-header.sha`, and
prints INVALID when the two encoders' sizes don't match.

Traps:

- Delete the wrapper's `.o` and `.d` before `make`. After installing to a new
  prefix, the `.d` still names the old header, make sees nothing to do, exits
  0 and leaves the old wrapper in the binary.
- Set `PKG_CONFIG_PATH` explicitly. A stray pkg-config entry for another
  architecture resolves and then fails to link. Check `otool -L ffmpeg`: it
  should name only the prefixes under `../build/ffboard`.
- A clean build of the fork after an ABI bump is evidence the wrapper is
  current. Expect a small wrapper fix per bump.
