# FFmpeg integration: libnext264 as an encoder ffmpeg can call

## Why

Two reasons, one of them measured.

**Adoption.** Nobody adopts an encoder by piping Y4M into a CLI. They call it
through ffmpeg, GStreamer or libavcodec directly, the way they call libx264
today. Until `-c:v next264` works, the encoder is a benchmark rather than
something anyone can put in a pipeline.

**A real speed cost we currently pay.** Encoding a file with next264 today means
running ffmpeg to decode, piping raw Y4M into next264, and piping its output
back into ffmpeg to mux. That is three processes sharing one CPU, and the
decode alone runs at 481 fps on 1080p while next264 encodes at 242 fps, so the
decode is stealing a third of the machine. Measured end to end on 1800 frames of
1080p: 222 fps through the pipeline against 242 fps from an already-decoded
file. libx264 inside ffmpeg pays none of this, because decode and encode share
one process and one thread pool. That gap is a packaging artifact, not an
encoder difference, and integration removes it.

## What already exists

More than half the work. `libnext264` builds today and the public API is
already the shape a wrapper needs:

| ffmpeg needs | next264 has |
|---|---|
| opaque handle | `next264_encoder_t` |
| param struct + presets | `next264_param_default`, `next264_param_apply_preset` |
| open / close | `next264_encoder_open`, `next264_encoder_close` |
| global header (SPS/PPS) for containers | `next264_encoder_headers` |
| frame in, packets out | `next264_encoder_encode` with `next264_nal_t` |
| delay signalling for pts/dts | `next264_lookahead_delay` |
| a picture type | `next264_picture_t` |

## What is missing

1. **A shared library.** `meson.build` builds `static_library('next264', ...)`
   with `install : false`. It needs `library()`, an install rule, a SONAME and a
   version.
2. **Installed headers.** `include/next264.h` is not installed.
3. **A pkg-config file.** ffmpeg's configure finds encoders through
   `pkg-config`; without `next264.pc` there is nothing to detect.
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
  buffered packets to drain. `next264_lookahead_delay` gives the frame count
  held; the wrapper reports it so timestamps are not shifted.
- **pts/dts with B frames.** Reordering means dts trails pts. Getting this wrong
  produces files that play but seek wrongly, which is the classic wrapper bug.
- **Global header.** With `AV_CODEC_FLAG_GLOBAL_HEADER`, SPS/PPS go in
  `extradata` and out of the stream. `next264_encoder_headers` supplies them.
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

`libnext264` is BSD-2-Clause, so it is linkable into ffmpeg's default LGPL
build. That is a genuine advantage over x264: an ffmpeg built with next264 does
not become GPL, where `--enable-libx264` forces `--enable-gpl`.

The wrapper itself sits inside `libavcodec` and would be LGPL. It therefore
cannot live in this repository: it is a patch against ffmpeg, and both
`CONTRIBUTING.md` rule 6 and `scripts/hygiene_check.sh` refuse checked-in
patches. It gets its own repository, the way `nextgpu` does.

Two destinations, and they are not exclusive:

- **A patch repo** (`next264-ffmpeg`) that applies to a pinned ffmpeg release.
  Fast, no negotiation, and enough to publish comparable numbers.
- **Upstream submission** to ffmpeg-devel. Slower and requires the code to meet
  their review standards, but it is the only route that makes `-c:v next264`
  work in a distribution build. Worth doing once the patch repo has proved
  itself.

## Stages

**S1. Make the library consumable.** `library()` with SONAME and install rules,
installed headers, `next264.pc`. Gate: a hello-world C file outside the tree
links `-lnext264` via pkg-config, opens an encoder, encodes 10 frames, and the
result decodes.

**S2. The wrapper, minimum viable.** yuv420p 8-bit, CRF and bitrate, preset,
keyint, bframes, threads. Gate: `ffmpeg -i in.mp4 -c:v next264 -crf 25 out.mp4`
produces a file that ffprobe reads and that decodes bit-exactly against the
encoder's own reconstruction, which is the same recon-match gate the CLI uses.

**S3. Correctness the container cares about.** Global header, pts/dts with B
frames, flush at EOF, `-g`/`-bf` honoured. Gate: seek to ten random points in
the output and land on the right frames; compare timestamps against libx264 on
the same source.

**S4. Formats and depth.** 4:2:2, 4:4:4, 10-bit. Gate: the recon-match matrix
across formats, as the CLI already runs it.

**S5. Re-measure the board through ffmpeg.** Both encoders in-process under the
same harness, same demuxer, same thread pool. This is what makes the parity
numbers a like-for-like comparison instead of a comparison of two CLI I/O paths,
which is a question currently open in the board.

**S6. Upstream, if S1 to S5 hold.**

## What this does not fix

The board's current input-path question is a separate issue with a cheaper
answer: feed both CLIs the same way. `PERF_COMP_FEED` does that today. Do not
wait on ffmpeg integration to settle it.

## Open questions

- Does the encoder need a reconfiguration path? ffmpeg can change bitrate
  mid-stream; today `next264_encoder_open` takes params once. Deciding this
  early is cheaper than retrofitting it.
- Which ffmpeg release to pin the patch repo to, and how often to rebase.
- Whether to expose the recon callback (`next264_encoder_set_recon_cb`) through
  the wrapper. It is what makes the recon-match gate possible in-process, and no
  other encoder offers it.
