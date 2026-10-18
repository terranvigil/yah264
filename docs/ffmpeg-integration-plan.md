# FFmpeg integration: libyah264 as an encoder ffmpeg can call

## Why

Two reasons, one of them measured.

**Adoption.** Nobody adopts an encoder by piping Y4M into a CLI. They call it
through ffmpeg, GStreamer or libavcodec directly, the way they call libx264
today. Until `-c:v libyah264` works, the encoder is only a benchmark. Nobody can
put it in a pipeline.

**A real speed cost we currently pay.** Encoding a file with yah264 today means
running ffmpeg to decode, piping raw Y4M into yah264, and piping its output
back into ffmpeg to mux. That is three processes sharing one CPU. On 1080p the
decode alone runs at 481 fps while yah264 encodes at 242 fps, so the decode
steals a third of the machine. End to end on 1800 frames of 1080p we measured
222 fps through the pipeline against 242 fps from an already-decoded file.
libx264 inside ffmpeg pays none of this, because decode and encode share one
process and one thread pool. The gap comes from packaging, and integration
removes it.

## What already exists

More than half the work. `libyah264` builds today, and the public API already
has the shape a wrapper needs:

| ffmpeg needs | yah264 has |
|---|---|
| opaque handle | `yah264_encoder_t` |
| param struct + presets | `yah264_param_default`, `yah264_param_apply_preset` |
| open / close | `yah264_encoder_open`, `yah264_encoder_close` |
| global header (SPS/PPS) for containers | `yah264_encoder_headers` |
| frame in, packets out | `yah264_encoder_encode` with `yah264_nal_t` |
| delay signaling for pts/dts | `yah264_lookahead_delay` |
| a picture type | `yah264_picture_t` |

## What is missing

1. **A shared library.** `meson.build` builds `static_library('yah264', ...)`
   with `install : false`. It needs `library()`, an install rule, a SONAME and a
   version.
2. **Installed headers.** `include/yah264.h` is not installed.
3. **A pkg-config file.** ffmpeg's configure finds encoders through
   `pkg-config`. Without `yah264.pc` there is nothing to detect.
4. **The wrapper.** An `AVCodec` implementation: option table, `init`,
   `receive_packet` or `encode2`, `close`, and the flush path.
5. **ffmpeg build glue.** A `configure` fragment and a `libavcodec/Makefile`
   entry, plus registration in the encoder list.

Items 1 to 3 are ours and take an afternoon. Items 4 and 5 live in ffmpeg's
tree and are the real work.

## The contract the wrapper has to satisfy

None of this is exotic. All of it has to be right, though. Otherwise the
encoder looks broken, and the encoder takes the blame for the wrapper's bug:

- **Delay and flush.** ffmpeg sends frames until EOF, then sends NULL and
  expects buffered packets to drain. `yah264_lookahead_delay` gives the frame
  count held. The wrapper reports it so timestamps are not shifted.
- **pts/dts with B frames.** Reordering means dts trails pts. Getting this wrong
  produces files that play but seek wrongly. That is the classic wrapper bug.
- **Global header.** With `AV_CODEC_FLAG_GLOBAL_HEADER`, SPS/PPS go in
  `extradata` and out of the stream. `yah264_encoder_headers` supplies them.
- **Option mapping.** `-crf`, `-preset`, `-tune`, `-b:v`, `-g`, `-bf`,
  `-threads`, `-x264-params`-equivalent. Our CLI already uses these names.
- **Pixel formats.** yuv420p, yuv422p, yuv444p, and their 10-bit forms, matching
  what the encoder is built for.
- **Threading.** ffmpeg passes a thread count. The encoder does its own
  threading, so the wrapper declares `AV_CODEC_CAP_OTHER_THREADS` and does not
  let ffmpeg wrap it in frame threads.

## The provenance hazard, stated before anyone starts

The obvious way to write this is to open `libavcodec/libx264.c` and follow it:
same struct, same callbacks, same option table, same flush logic. **Do not.**
That file is LGPL, and copying its structure produces a derivative work.
`CONTRIBUTING.md` rule 2 forbids exactly that. It is also the class of problem
this project has just spent a week clearing out of its own tree.

The rule for this work specifically:

- Write from ffmpeg's **public documentation and headers** only: `avcodec.h`,
  `codec.h`, the encoder-API docs, and the published `AVCodec` contract.
- Whoever writes it should not have read `libx264.c` recently. If they have,
  they should do a different part of this plan.
- The wrapper will still resemble every other encoder wrapper, because the
  `AVCodec` interface dictates the shape. That is interface conformance, and
  it is fine. Reproducing their expression, their option-table ordering or
  their internal helpers is not.

## Licensing and where the code lives

`libyah264` is GPL-2.0-or-later, so ffmpeg's configure lists it with the GPL
libraries and `--enable-libyah264` needs `--enable-gpl`, exactly as
`--enable-libx264` does. Products that cannot ship an ffmpeg under the GPL
take the commercial license instead.

The wrapper itself sits inside `libavcodec` and would be LGPL. It therefore
cannot live in this repository. It is a patch against ffmpeg, and both
`scripts/hygiene_check.sh` refuses checked-in
patches. It gets its own repository, the way the shared GPU library does.

There are two destinations. They are not exclusive:

- **A patch repo** (`yah264-ffmpeg`) that applies to a pinned ffmpeg release.
  Fast, no negotiation, and enough to publish comparable numbers.
- **Upstream submission** to ffmpeg-devel. Slower, and the code has to meet
  their review standards. It is the only route that makes `-c:v libyah264` work
  in a distribution build. Worth doing once the patch repo has proved itself.

## Stages

**S1. Make the library consumable.** `library()` with SONAME and install rules,
installed headers, `yah264.pc`. Gate: a hello-world C file outside the tree
links `-lyah264` via pkg-config, opens an encoder and encodes 10 frames. The
result decodes.

**S2. The wrapper, minimum viable.** yuv420p 8-bit, CRF and bitrate, preset,
keyint, bframes, threads. Gate: `ffmpeg -i in.mp4 -c:v libyah264 -crf 25 out.mp4`
produces a file that ffprobe reads. It decodes bit-exactly against the
encoder's own reconstruction, the same recon-match gate the CLI uses.

**S3. Correctness the container cares about.** Global header, pts/dts with B
frames, flush at EOF, `-g`/`-bf` honored. Gate: seek to ten random points in
the output and land on the right frames. Compare timestamps against libx264 on
the same source.

**S4. Formats and depth.** DONE, 2026-08-30. All six combinations encode through
`-c:v libyah264` and come back with the profile they should: 8-bit 4:2:0/4:2:2/
4:4:4 as High / High 4:2:2 / High 4:4:4 Predictive, and the same three at
`-Dbit_depth=10` as High 10 / High 4:2:2 / High 4:4:4 Predictive with
`yuv420p10le` and friends.

Getting there turned up a defect on our side that had nothing to do with the
wrapper. **Both depths install under the same soname and `yah264.pc` carried no
depth.** A consumer of a 10-bit install compiled against a header whose
`Y264_BIT_DEPTH` had quietly defaulted to 8. That header typed `pixel` as
`uint8_t`, so the consumer handed the library planes at half the stride it
expected. It linked, it loaded, and it produced garbage with no diagnostic
anywhere. The fix has two halves:

- `yah264.pc` now has `-DY264_BIT_DEPTH=N` in `Cflags` and an `N` in a
  `bit_depth` variable. Anything built through pkg-config is right by
  construction. ffmpeg's configure picks this up through pkg-config, so the
  wrapper compiles for whichever library configure found.
- `yah264_bit_depth()` reports the built depth at run time. It serves the
  caller that linked by hand or had the dylib swapped underneath it. The
  wrapper compares it against its own `Y264_BIT_DEPTH` at `init` and refuses
  the mismatch.

Verified both ways: a 10-bit ffmpeg pointed at the 8-bit library now fails with
"libyah264 is a 8-bit build, this wrapper was compiled for 10-bit" instead of
encoding nonsense.

The S1 gate also ran for the first time as written, at both depths: a
consumer outside the tree, pkg-config only, ten frames encoded and decoded.

**S5. Re-measure the table through ffmpeg.** DONE, `scripts/ffboard.py`.
Matched-point CRF, six clips, quiet box, both encoders in one process:

| goal | CLI table | ffmpeg in-process |
|---|--:|--:|
| 1 pure-C, 1 thread | 0.98x | 0.96x |
| 2 pure-C, 12 threads | 0.99x | 0.94x |
| 3 as-shipped SIMD, 12 threads | 1.15x | 1.06x |

The two agree at goal 1 and diverge at goal 3. The shape of the divergence
answers the question above. It concentrates in the short clips. Those move
most, while foreman and park_joy land close to the same number in both
harnesses. A fixed per-process input cost looks like that. Its share grows as
the encode gets faster, so it is worth about 0.11 at goal 3's operating point
and nothing at goal 1's.

The input path is therefore a real term in the as-shipped comparison. It is
smaller than the 1080p pipeline measurement above suggests, and on its own it
does not rescue goal 3's median.

Two wrapper defects surfaced only because this table scored its own output
instead of trusting it. Both show the class of bug to expect here. SPS/PPS
went only to `extradata`, so raw Annex-B output began at an IDR slice and no
decoder would open it. MP4 hid that completely. And `avctx->refs` defaults to
1. Unlike `gop_size` and `max_b_frames`, 1 is a plausible caller value and
no sentinel. The guard could not tell a choice from a default, so every
ffmpeg encode ran at ref=1 instead of the preset's 3.

A third defect was ours. Making the library installable moved the CLI onto
the shared object. The resulting loss of cross-TU inlining cost 12-14% of
wall time on every table.

**S6. Upstream, if S1 to S5 hold.**

## Rebuilding the measurement setup

Everything `scripts/ffboard.py` needs used to live under `/tmp`. On 2026-09-17
macOS swept it. ffmpeg, the installed libyah264 and both libx264 builds were
gone, and that day's board could not run its in-process arm at all. The set
now lives at `../build/ffboard` beside the checkout, which survives a sweep
and a reboot. The fork is on GitHub, so only the local builds have to be
reconstructed.

```
FFB=$(cd .. && pwd)/build/ffboard          # ../build/ffboard, a sibling of the checkout
mkdir -p "$FFB"

git clone -b yah264 git@github.com:terranvigil/FFmpeg.git "$FFB/ffmpeg-yah264"
meson setup build -Dbuildtype=release -Dprefix="$FFB/y264inst" && ninja -j6 -C build install

# x264, from source, TWICE. Same source, two trees, two prefixes: x264 builds
# in-tree, so one export cannot serve both configures.
git -C ../x264 archive HEAD | tar -x -C "$FFB/x264src-asm"
#   asm:    configure --prefix="$FFB/x264asm"   --enable-shared --disable-cli
#   pure-C: configure --prefix="$FFB/x264noasm" --enable-shared --disable-cli \
#           --disable-asm, THEN strip -fno-tree-vectorize from config.mak

cd "$FFB/ffmpeg-yah264" && PKG_CONFIG_PATH="$FFB/x264asm/lib/pkgconfig:$FFB/y264inst/lib/pkgconfig" \
  ./configure --enable-libyah264 --enable-libx264 --enable-gpl

# DELETE THE WRAPPER'S OBJECT AND ITS DEPENDENCY FILE BEFORE MAKE. This is not
# tidiness. It is the difference between rebuilding the wrapper and appearing
# to. Install the library to a NEW prefix -- which is what you do whenever the
# header has moved, since the prefix is named for the header -- and
# libavcodec/libyah264.d still names the OLD prefix's yah264.h. That file has
# not changed, so make has nothing to do: it prints its config steps, exits 0,
# and leaves the binary with the wrapper it already had. The line below then
# stamps the NEW header's blob beside it, and the gate that exists to catch
# exactly this mismatch passes on a pair that does not match. Found 2026-09-22,
# one step on from the 2026-09-19 failure the gate was written for.
rm -f libavcodec/libyah264.o libavcodec/libyah264.d ffmpeg ffmpeg_g
make -j6

# Prove the wrapper really was rebuilt: its dependency file must name the
# prefix you just installed to, and the binary must load the dylib from there.
grep yah264.h libavcodec/libyah264.d
otool -L ffmpeg | grep yah264

# Record which public header this ffmpeg's wrapper was compiled against. Run it
# from the yah264 checkout that produced the install above, every time the
# ffmpeg is rebuilt. scripts/ffboard.py reads it and refuses a Y264LIB whose
# installed header is a different blob.
git hash-object include/yah264.h > "$FFB/ffmpeg-yah264/yah264-header.sha"
```

`--enable-gpl` is not optional: libx264 is GPL and configure refuses it without.

**Rebuild the ffmpeg whenever `include/yah264.h` changes**, and rewrite
`yah264-header.sha` with it. The wrapper reads `yah264_param_t` by offset. If
the header moved a field, the wrapper writes the wrong fields, and until ABI 3
nothing anywhere said so (see the ABI note below). `scripts/ffboard.py` will
not run a board across that gap. It compares the recorded blob against
`$Y264LIB/include/yah264.h` and names both when they differ.

`scripts/ffboard.py` looks for the binary at `../build/ffboard/ffmpeg-yah264/ffmpeg`
first and falls back to the old `/tmp/ffmpeg-yah264/ffmpeg` for a box that still
has one. `Y264_FFBOARD_ROOT` moves the whole set somewhere else. `FF` still
names one binary directly. `X264LIB` and `Y264LIB` have no defaults on
purpose, because a stale install that loads is worse than one that is missing.

**The fork branch is `yah264`, and the encoder is `libyah264`.** Both were
`next264` until the library was renamed. That branch stayed the fork's default
for a while after the rename, so a plain clone handed you an ffmpeg whose
encoder was called `libnext264`. It was deleted 08-30, and `yah264` is the
default now. The recipe above still clones by name, though it no longer has
to. `scripts/ffboard.py` answers to either name through `ENC`. Nothing else
does.

**The wrapper trails the library's ABI, and the build is where you find out.**
At the 2026-09-17 rebuild `libavcodec/libyah264.c` would not compile. It still
cast picture planes to `pixel` and divided the stride by `sizeof(pixel)`, and
ABI 2 had dropped that typedef. Planes are `void *` now and stride is in
samples. The fix assigns `f->data[i]` straight across and divides the
linesize by 2 only at `Y264_BIT_DEPTH > 8`. Nothing else in the fork needed
touching. The rest of the branch built as it stood. That fix is local to the
clone until someone pushes it. Expect one of these per ABI bump. A clean build
of the fork is evidence that the wrapper is current, and worth reading as
such.

**A clean build is still not enough on its own. Learning that cost a board on
2026-09-19.** B-partitions added a `partitions` field to `yah264_param_t` after
that ffmpeg was built. The wrapper still compiled, linked and loaded, because
the soname had not moved and every symbol resolved. It handed the library a
struct in the old layout. The library read every field past `partitions` as
some other field's bytes. park_joy solved to CRF 42.7 at 55 Mbps and the
encode timed 32x slower than x264. Nothing in the stack said a word. Three
checks now stand between that and a published number:

- `yah264_param_t` has its own `size` as its first field (ABI 3).
  `yah264_param_default()` fills it. `yah264_encoder_open()` refuses a value
  that is not the library's own sizeof, and the message names both numbers.
  The wrapper already calls `yah264_param_default`, so it needs nothing but a
  rebuild.
- The soname is the ABI version, so an ffmpeg built against ABI 2 cannot load
  an ABI-3 install at all. Note what that does NOT do. dyld looks up the LEAF
  NAME the binary asked for. Pointing `Y264LIB` at an install whose soname
  differs does not load it. dyld silently falls back to the absolute path
  recorded at link time. `Y264LIB` alone never proved which library ran.
- `scripts/ffboard.py` compares `yah264-header.sha` against the header
  installed beside the dylib. That catches a field moved WITHIN one ABI, which
  was the 2026-09-19 case: the soname was right and the layout was not.

The table run picks the x264 arm at RUN time through `X264LIB`, not at configure
time. Both builds have the same soname, so `DYLD_LIBRARY_PATH` selects one and
the ffmpeg binary never needs rebuilding between goals. We verified it
bit-exact. The asm and pure-C libraries produce identical output and differ
only in speed.

**Set `PKG_CONFIG_PATH` explicitly.** The default `pkg-config` x264 on this
machine is an **x86_64** Intel-brew leftover in `/usr/local/Cellar`. It resolves
happily and then fails to link on arm64. Check with `lipo -archs` on every
library the binary ends up loading. `otool -L` on the built ffmpeg is the
proof. It should name the `x264asm` and `y264inst` prefixes under
`../build/ffboard` and nothing under `/usr/local`.

## What this does not fix

Goal 3's median. The in-process table reads it 0.11 better than the CLI table.
That gain is the input path going away. The encoder did not get faster, and
1.06x is still above the 1.00x bar. The gap is smaller than it looked, but it
is still open.

Since then goal 3 has closed with the rest of the parity work: yah264 reached
parity with x264 on 2026-10-12 (`site/results.md`).

## Open questions

- Does the encoder need a reconfiguration path? ffmpeg can change bitrate
  mid-stream. Today `yah264_encoder_open` takes params once. Deciding this
  early is cheaper than retrofitting it.
- Which ffmpeg release to pin the patch repo to, and how often to rebase.
- Whether to expose the recon callback (`yah264_encoder_set_recon_cb`) through
  the wrapper. It is what makes the recon-match gate possible in-process, and no
  other encoder offers it.
