# FFmpeg integration: libyah264 as an encoder ffmpeg can call

## Why

Two reasons, one of them measured.

**Adoption.** Nobody adopts an encoder by piping Y4M into a CLI. They call it
through ffmpeg, GStreamer or libavcodec directly, the way they call libx264
today. Until `-c:v yah264` works, the encoder is a benchmark rather than
something anyone can put in a pipeline.

**A real speed cost we currently pay.** Encoding a file with yah264 today means
running ffmpeg to decode, piping raw Y4M into yah264, and piping its output
back into ffmpeg to mux. That is three processes sharing one CPU, and the
decode alone runs at 481 fps on 1080p while yah264 encodes at 242 fps, so the
decode is stealing a third of the machine. Measured end to end on 1800 frames of
1080p: 222 fps through the pipeline against 242 fps from an already-decoded
file. libx264 inside ffmpeg pays none of this, because decode and encode share
one process and one thread pool. That gap is a packaging artifact, not an
encoder difference, and integration removes it.

## What already exists

More than half the work. `libyah264` builds today and the public API is
already the shape a wrapper needs:

| ffmpeg needs | yah264 has |
|---|---|
| opaque handle | `yah264_encoder_t` |
| param struct + presets | `yah264_param_default`, `yah264_param_apply_preset` |
| open / close | `yah264_encoder_open`, `yah264_encoder_close` |
| global header (SPS/PPS) for containers | `yah264_encoder_headers` |
| frame in, packets out | `yah264_encoder_encode` with `yah264_nal_t` |
| delay signalling for pts/dts | `yah264_lookahead_delay` |
| a picture type | `yah264_picture_t` |

## What is missing

1. **A shared library.** `meson.build` builds `static_library('yah264', ...)`
   with `install : false`. It needs `library()`, an install rule, a SONAME and a
   version.
2. **Installed headers.** `include/yah264.h` is not installed.
3. **A pkg-config file.** ffmpeg's configure finds encoders through
   `pkg-config`; without `yah264.pc` there is nothing to detect.
4. **The wrapper.** An `AVCodec` implementation: option table, `init`,
   `receive_packet` or `encode2`, `close`, and the flush path.
5. **ffmpeg build glue.** A `configure` fragment and a `libavcodec/Makefile`
   entry, plus registration in the encoder list.

Items 1 to 3 are ours and are an afternoon. Items 4 and 5 live in ffmpeg's tree
and are the real work.

## The contract the wrapper has to satisfy

None of this is exotic, but all of it has to be right or the encoder looks
broken in ways that get blamed on the encoder rather than the wrapper:

- **Delay and flush.** ffmpeg sends frames until EOF then sends NULL and expects
  buffered packets to drain. `yah264_lookahead_delay` gives the frame count
  held; the wrapper reports it so timestamps are not shifted.
- **pts/dts with B frames.** Reordering means dts trails pts. Getting this wrong
  produces files that play but seek wrongly, which is the classic wrapper bug.
- **Global header.** With `AV_CODEC_FLAG_GLOBAL_HEADER`, SPS/PPS go in
  `extradata` and out of the stream. `yah264_encoder_headers` supplies them.
- **Option mapping.** `-crf`, `-preset`, `-tune`, `-b:v`, `-g`, `-bf`,
  `-threads`, `-x264-params`-equivalent. Our CLI already uses these names.
- **Pixel formats.** yuv420p, yuv422p, yuv444p, and their 10-bit forms, matching
  what the encoder is built for.
- **Threading.** ffmpeg passes a thread count; the encoder does its own
  threading, so the wrapper declares `AV_CODEC_CAP_OTHER_THREADS` rather than
  letting ffmpeg wrap it in frame threads.

## The provenance hazard, stated before anyone starts

The obvious way to write this is to open `libavcodec/libx264.c` and follow it:
same struct, same callbacks, same option table, same flush logic. **Do not.**
That file is LGPL and structurally copying it produces a derivative work, which
is exactly what `CONTRIBUTING.md` rule 2 forbids and exactly the class of
problem this project has just spent a week clearing out of its own tree.

The rule for this work specifically:

- Write from ffmpeg's **public documentation and headers** only: `avcodec.h`,
  `codec.h`, the encoder-API docs, and the published `AVCodec` contract.
- Whoever writes it should not have read `libx264.c` recently. If they have,
  they should do a different part of this plan.
- The wrapper will still resemble every other encoder wrapper, because the
  `AVCodec` interface dictates the shape. That is interface conformance and it
  is fine. What is not fine is reproducing their expression, their option-table
  ordering, or their internal helpers.

## Licensing and where the code lives

`libyah264` is BSD-2-Clause, so it is linkable into ffmpeg's default LGPL
build. That is a genuine advantage over x264: an ffmpeg built with yah264 does
not become GPL, where `--enable-libx264` forces `--enable-gpl`.

The wrapper itself sits inside `libavcodec` and would be LGPL. It therefore
cannot live in this repository: it is a patch against ffmpeg, and both
`CONTRIBUTING.md` rule 6 and `scripts/hygiene_check.sh` refuse checked-in
patches. It gets its own repository, the way `nextgpu` does.

Two destinations, and they are not exclusive:

- **A patch repo** (`yah264-ffmpeg`) that applies to a pinned ffmpeg release.
  Fast, no negotiation, and enough to publish comparable numbers.
- **Upstream submission** to ffmpeg-devel. Slower and requires the code to meet
  their review standards, but it is the only route that makes `-c:v yah264`
  work in a distribution build. Worth doing once the patch repo has proved
  itself.

## Stages

**S1. Make the library consumable.** `library()` with SONAME and install rules,
installed headers, `yah264.pc`. Gate: a hello-world C file outside the tree
links `-lyah264` via pkg-config, opens an encoder, encodes 10 frames, and the
result decodes.

**S2. The wrapper, minimum viable.** yuv420p 8-bit, CRF and bitrate, preset,
keyint, bframes, threads. Gate: `ffmpeg -i in.mp4 -c:v yah264 -crf 25 out.mp4`
produces a file that ffprobe reads and that decodes bit-exactly against the
encoder's own reconstruction, which is the same recon-match gate the CLI uses.

**S3. Correctness the container cares about.** Global header, pts/dts with B
frames, flush at EOF, `-g`/`-bf` honoured. Gate: seek to ten random points in
the output and land on the right frames; compare timestamps against libx264 on
the same source.

**S4. Formats and depth.** DONE, 2026-08-30. All six combinations encode through
`-c:v libyah264` and come back with the profile they should: 8-bit 4:2:0/4:2:2/
4:4:4 as High / High 4:2:2 / High 4:4:4 Predictive, and the same three at
`-Dbit_depth=10` as High 10 / High 4:2:2 / High 4:4:4 Predictive with
`yuv420p10le` and friends.

Getting there turned up a defect on our side that had nothing to do with the
wrapper. **Both depths install under the same soname and `yah264.pc` carried no
depth**, so a consumer of a 10-bit install compiled against a header whose
`Y264_BIT_DEPTH` had quietly defaulted to 8, typed `pixel` as `uint8_t`, and
handed the library planes at half the stride it expected. It linked, it loaded,
and it produced garbage with no diagnostic anywhere. Two halves to the fix:

- `yah264.pc` now carries `-DY264_BIT_DEPTH=N` in `Cflags` and an `N` in a
  `bit_depth` variable, so anything built through pkg-config is right by
  construction. ffmpeg's `<reference-internal>` picks this up, which is what makes
  the wrapper compile for whichever library configure found.
- `yah264_bit_depth()` reports the built depth at run time, for the caller that
  linked by hand or had the dylib swapped underneath it. The wrapper compares it
  against its own `Y264_BIT_DEPTH` at `init` and refuses the mismatch.

Verified both ways: a 10-bit ffmpeg pointed at the 8-bit library now fails with
"libyah264 is a 8-bit build, this wrapper was compiled for 10-bit" instead of
encoding nonsense.

The S1 gate was also run for the first time as written -- a consumer outside the
tree, pkg-config only, ten frames encoded and decoded -- at both depths.

**S5. Re-measure the table through ffmpeg.** DONE, `scripts/ffboard.py`.
Matched-point CRF, six clips, quiet box, both encoders in one process:

| goal | CLI table | ffmpeg in-process |
|---|--:|--:|
| 1 pure-C, 1 thread | 0.98x | 0.96x |
| 2 pure-C, 12 threads | 0.99x | 0.94x |
| 3 as-shipped SIMD, 12 threads | 1.15x | 1.06x |

The two agree at goal 1 and diverge at goal 3, and the shape of the divergence
is the answer to the question above. It concentrates in the short clips --
the short clips move most, while foreman and park_joy land close to the same
number in both harnesses. That is what a fixed per-process
input cost looks like: its share grows as the encode gets faster, so it is worth
about 0.11 at goal 3's operating point and nothing at goal 1's.

So the input path is a real term in the as-shipped comparison, though a smaller
one than the 1080p pipeline measurement above suggests, and it does not rescue
goal 3's median on its own.

Two wrapper defects surfaced only because this table scored its own output
rather than trusting it, and both are worth stating as the class of bug to
expect here. SPS/PPS went only to `extradata`, so raw Annex-B output began at an
IDR slice and no decoder would open it -- MP4 hid it completely. And
`avctx->refs` defaults to 1, which unlike `gop_size` and `max_b_frames` is a
plausible caller value rather than a sentinel, so the guard could not tell a
choice from a default and every ffmpeg encode ran at ref=1 instead of the
preset's 3.

A third defect was ours, not the wrapper's: making the library installable moved
the CLI onto the shared object, and the resulting loss of cross-TU inlining cost
12-14% of wall time on every table.

**S6. Upstream, if S1 to S5 hold.**

## Rebuilding the measurement setup

Everything `scripts/ffboard.py` needs lives under `/tmp` and does not survive a
reboot or a `/tmp` sweep. The fork is on GitHub, so only the local builds have to
be reconstructed. Recorded here because a reboot is one of the things worth
trying against the table's cross-day spread, and losing the harness to test the
harness would be a poor trade.

```
git clone -b yah264 git@github.com:terranvigil/FFmpeg.git /tmp/ffmpeg-yah264
meson configure build -Dprefix=/tmp/y264inst && ninja -C build install

# x264, from source, TWICE. Same tree, two prefixes.
git -C ../x264 archive HEAD | tar -x -C /tmp/x264src
#   asm:    configure --prefix=/tmp/x264asm   --enable-shared --disable-cli
#   pure-C: configure --prefix=/tmp/x264noasm --enable-shared --disable-cli \
#           --disable-asm, THEN strip -fno-tree-vectorize from config.mak

cd /tmp/ffmpeg-yah264 && PKG_CONFIG_PATH=/tmp/x264asm/lib/pkgconfig:/tmp/y264inst/lib/pkgconfig \
  ./configure --enable-libyah264 --enable-libx264 --enable-gpl
```

`--enable-gpl` is not optional: libx264 is GPL and configure refuses it without.

**The fork branch is `yah264`, and the encoder is `libyah264`.** Both were
`next264` until the library was renamed. That branch was the fork's default for
a while after the rename, so a plain clone handed you an ffmpeg whose encoder
was called `libnext264`; it was deleted 08-30 and `yah264` is the default now,
which is why the recipe above clones by name and can stop having to.
`scripts/ffboard.py` answers to either name through `ENC`, but nothing else
does.

The table run picks the x264 arm at RUN time through `X264LIB`, not at configure
time: both builds carry the same soname, so `DYLD_LIBRARY_PATH` selects one and
the ffmpeg binary never needs rebuilding between goals. Verified bit-exact --
the asm and pure-C libraries produce identical output, differing only in speed.

**Set `PKG_CONFIG_PATH` explicitly.** The default `pkg-config` x264 on this
machine is an **x86_64** Intel-brew leftover in `/usr/local/Cellar`. It resolves
happily and then fails to link on arm64. `lipo -archs` on every library the
binary ends up loading is the check, and `otool -L` on the built ffmpeg is the
proof: it should name `/tmp/x264*/lib` and `/tmp/y264inst/lib`, nothing under
`/usr/local`.

## What this does not fix

Goal 3's median. The in-process table reads it 0.11 better than the CLI table,
which is the input path being removed rather than the encoder getting faster,
and 1.06x is still above the 1.00x bar. The gap is smaller than it looked, not
closed.

## Open questions

- Does the encoder need a reconfiguration path? ffmpeg can change bitrate
  mid-stream; today `yah264_encoder_open` takes params once. Deciding this
  early is cheaper than retrofitting it.
- Which ffmpeg release to pin the patch repo to, and how often to rebase.
- Whether to expose the recon callback (`yah264_encoder_set_recon_cb`) through
  the wrapper. It is what makes the recon-match gate possible in-process, and no
  other encoder offers it.
