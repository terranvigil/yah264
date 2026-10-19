# ffmpeg integration

`libyah264` is an encoder wrapper in a fork of ffmpeg, so `-c:v libyah264`
works the way `-c:v libx264` does. Decode, encode and mux share one process
instead of three piped together. The fork lives at
`github.com/terranvigil/FFmpeg` on the `yah264` branch. The wrapper sits inside
libavcodec and is LGPL, so it can't live in this repository. This repo's side
is the installable library: a shared object, the public header and pkg-config
files.

It's what `scripts/ffboard.py` runs: the speed board with both encoders called
as libraries inside one ffmpeg process.

## How it works

The wrapper uses ffmpeg's receive-packet encoder API and declares
`AV_CODEC_CAP_OTHER_THREADS`, so ffmpeg doesn't wrap it in its own frame
threads. At init it builds a parameter struct with `yah264_param_default`,
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
| `-subme`, `-trellis`, `-aq-strength`, `-psy-rd` | the same settings | private options; -1 keeps the preset's value |

There's no `-tune` and no free-form parameter string yet.

Pixel formats are yuv420p, yuv422p and yuv444p from an 8-bit build, or their
10-bit forms from a 10-bit build. The wrapper is compiled for one depth and
refuses a library of the other at init by checking `yah264_bit_depth()`.

The encoder returns NAL units in coding order, and one call can carry several
coded frames. The wrapper splits them into one packet per frame on slice
boundaries, attaching parameter sets and SEI to the slice they precede, and
labels each packet's pts from the display index the encoder reports
(`yah264_encoder_frame_order`). dts trails pts by the stream's reorder depth
only: the pyramid depth, 1 for flat B, 0 without B. ABR and VBV keep a wider
margin, the lookahead delay plus the B count, because those modes can emit
anchors early. With `AV_CODEC_FLAG_GLOBAL_HEADER` the SPS and PPS go to
extradata. Without it they're written in-band ahead of the first packet, so raw
Annex-B output decodes.

## Building it

The library builds in two forms. The CLI and the in-tree tools link the static
one; the shared one is what gets installed. Both depths install side by side
under different names, each with its own pkg-config file (`yah264` and
`yah264_10`) whose Cflags carry `-DY264_BIT_DEPTH`. The soname is the ABI
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
`yah264-header.sha`. The wrapper reads the parameter struct by offset, so a
moved field means the library reads the wrong settings.

## What we chose and why

| choice | why |
|---|---|
| the CLI links the static library, the shared one is installed | linking the CLI against the dylib lost cross-unit inlining and cost 12-14% wall time on every board |
| hidden symbol visibility in the shared library | internal calls stay direct, so an outside caller doesn't pay for the export table on every kernel dispatch |
| the parameter struct carries its own size (ABI 3); open refuses a mismatch | a wrapper one field behind the library compiled, loaded and encoded garbage settings with no error |
| soname = ABI version | an ffmpeg built against an older ABI can't load a newer install at all |
| depth in pkg-config Cflags, and `yah264_bit_depth()` checked at init | a consumer compiled for the wrong depth links fine and reads every plane at the wrong stride |
| `-refs` applied only when explicitly set | ffmpeg's default of 1 is a plausible value, not a sentinel; treating it as a choice ran every encode at one reference. The cost: an explicit `-refs 1` reads as unset |
| threads passed through unchanged | core counting and clamping are the encoder's policy, not the wrapper's |
| input frames submitted one at a time | the encoder parallelizes on its row wavefront, which the thread budget drives; queuing 1 to 256 frames ahead measured no difference |
| written from ffmpeg's public headers and encoder-API docs only | the wrapper must not copy another encoder's wrapper; resembling it where the AVCodec interface dictates the shape is fine |

## What didn't work

| idea | why not |
|---|---|
| counting the encoder's lookahead delay in dts for CRF | put the first dts seven frames early at the defaults; players that honor the offset stall at the start |
| SPS/PPS only in extradata | raw Annex-B output started at an IDR slice and nothing would decode it; MP4 hid the bug |
| `Y264LIB` as proof of which library loaded | dyld looks up the leaf name; an install with a different soname is skipped and the link-time path loads instead. `otool -L` on the binary is the proof |

## Open questions

- Does the encoder need a reconfiguration path? ffmpeg can change bitrate
  mid-stream, and `yah264_encoder_open` takes parameters once. See
  [live-control.md](live-control.md).
- Which ffmpeg release to pin the fork to, and when to submit upstream.
- Whether to expose the recon callback (`yah264_encoder_set_recon_cb`) through
  the wrapper, which would let the recon-match gate run in-process.
- The fork's configure asks pkg-config for `yah264`, the 8-bit library. A
  10-bit wrapper needs it pointed at `yah264_10`, and configure doesn't do that
  yet.

## Measuring it

`scripts/ffboard.py` is the control for the CLI speed board, not the headline.
At the same operating points the two agree within noise on every clip, so the
CLI's Y4M input path isn't where speed gaps live. `FF` names the ffmpeg
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
  current; expect a small wrapper fix per bump.
