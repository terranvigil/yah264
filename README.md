# next264

**An H.264/AVC encoder written from the ground up by looking at the performance
and quality of the top existing H.264 encoders and trying to match or surpass
them.**

## Why do this

There are already strong H.264 encoders and replacing them was never the point.
I wanted a deeper understanding of the coding tools H.264 provides, and of how
each one trades speed against quality.

The second reason is the one I find more interesting. H.264 is a decoder
specification: it fixes what a conformant bitstream decodes to, and says nothing
about how the encoder chose it. Every decision on the encoder side is open to a
model while every decoder already deployed keeps working. Four of those are on
the list, and one of the four is already closed:

- Fit the encoder's hand-chosen constants jointly, instead of one sweep at a
  time. Most were measured as optimal before the encoder changed around them.
- Pick parameters per clip rather than per preset. The measured ceiling is
  around 0.6% BD-rate, and it holds on every clip in the corpus.
- Repay the calibration debt in the bit estimate that the rate-distortion
  search runs on.
- A learned transform-size classifier was the fourth, and it's been measured
  and dropped. It came in worse than the cheap screen it would have replaced.

H.264 is the most deployed video format in history, and x264 is the best of
breed, so that's the baseline every target here is set against.

One asymmetry runs through goal 3. Hand-optimised assembly means scheduling
instructions and allocating registers yourself, across thousands of lines,
against one processor's timing. Current AI does that badly. It handles SIMD
intrinsics well, where the same parallelism goes into C and the compiler
schedules it, so next264's SIMD tier is about 5,500 lines of NEON intrinsics,
each kernel validated against the C reference and benchmarked by a checkasm
harness. Part of goal 3's remaining gap is the price of that choice.

## Where it stands

Three speed goals over a six-clip set of CIF and 720p material at a matched
operating point. Each is scored as a ratio to the reference encoder's wall time,
so the bar reproduces on any machine: 1.00x is parity, lower is faster. A goal
passes or fails on four criteria, all read at the same matched point:

| leg | bar |
|---|---|
| median speed | 1.00x or faster |
| worst-clip speed | under 1.15x |
| quality | within 0.5 dVMAF |
| compression | within 1.0% dSIZE |

| goal | configuration | median | max | dVMAF | dSIZE | status |
|---|---|--:|--:|--:|--:|---|
| 1 | pure C, single-threaded (both encoders no-asm) | **0.96x** | 1.06x | −0.16 | +0.1% | **all four legs met** |
| 2 | pure C, multi-threaded (12 threads) | **0.94x** | 1.04x | −0.18 | +0.1% | **all four legs met** |
| 3 | as-shipped SIMD, multi-threaded | 1.06x | 1.18x | −0.19 | +0.1% | two of four; median and worst clip open |

Both encoders run as libraries inside one ffmpeg process, off the same demuxer
and the same thread pool. That matters more than it sounds. Measured through two
CLIs instead, each encoder's own Y4M reader is inside the timing, and goal 3's
median rises by roughly a tenth. That difference is almost entirely in the short
clips, which is what a fixed per-process cost looks like: its share grows as the
encode gets faster. Reproducing these needs the wrapper, which lives in an
ffmpeg fork rather than here because it would be LGPL. See
`docs/ffmpeg-integration-plan.md` and `scripts/ffboard.py`. For a number that
needs no fork, `make parity-status-crf` runs the two-CLI version.

The pure-C rows compare auto-vectorized C on both sides. x264 suppresses
vectorization in its own build, since its C sits behind hand-written assembly
and exists as a fallback, so the reference for those two rows is built with that
flag stripped. Leaving it in reads goal 2 as 0.73x, which measures a compiler
flag rather than an encoder.

Quality is full-frame VMAF (NEG mode) at matched bitrates. Low bitrate is where
next264 does best: at the deep band it leads x264 on 9 of 10 clips, median
BD-rate advantage around 12%.

The numbers are a snapshot from August 2026 on Apple Silicon, and there's no
x86-64 SIMD yet. Speed ratios move a few points between machines and between
runs on the same machine, so treat the third decimal as noise.

## What's inside

The coding tools cover H.264 Main and High, minus interlacing.

| | |
|---|---|
| entropy coding | CABAC and CAVLC |
| transforms | 4x4 and 8x8, chosen per macroblock by RD |
| intra | 4x4, 8x8, 16x16 |
| inter | up to 16 references, B-frames with pyramid, B_8x8 partitions, spatial and temporal direct |
| frame decisions | scene-cut detection, adaptive B-placement |
| rate control | CQP, CRF, single-pass ABR, CBR/VBV, capped VBR, two-pass |
| formats | 8- and 10-bit, in 4:2:0, 4:2:2 and 4:4:4 |

Macroblock-tree, variance AQ, psychovisual RD and full-trellis RDOQ over both
transform sizes are on by default at the medium-equivalent settings.

Threading is a hybrid. Frames run GOP-parallel, rows run wavefront within a
frame, and a lock-free work pool feeds both. Output stays deterministic for a
given configuration, and on wide machines it scales further than x264's own
threading does.

Memory stays bounded on arbitrarily long clips. The encoder prices its window
up front and refuses a configuration that won't fit in RAM, rather than
discovering the problem an hour into a file.

Every change has to clear the recon-match suite, where the encoder's own
reconstruction must equal an independent decoder's output bit-for-bit, across
every format and bit depth. Determinism and byte-identity tests run alongside
it.

## How it compares

Software encoders, measured on this repo's own harness with full-frame VMAF.
Two of the three sit at a matched operating point; the third cannot, for a
reason worth reading before quoting its numbers.

| encoder | pure-C 1-thread | pure-C MT | SIMD MT | quality (dVMAF) | size | notes |
|---|--:|--:|--:|--:|--:|---|
| next264 | **0.96x** | **0.94x** | 1.06x | −0.19 | +0.1% | this repo |
| x264 | 1.00x | 1.00x | 1.00x | ref | ref | the reference point |
| openh264 | 0.09x | 0.50x | 0.76x | −9.9 | +1.4% | not a matched point, see below |

The first two rows are the same in-process board as the goal table, at the same
matched operating point, so they are the same numbers.

openh264's row is not, and its speed cannot be read against the other two. It
exposes no quality knob through ffmpeg, only a bitrate, so there is nothing to
solve onto a common operating point. Boarded at a matched bitrate it sits about
10 VMAF below both other encoders, and that deficit is most of why it looks
fast. Its comparable number is BD-rate, which normalises for quality, and there
it costs +63.7%. It also has no B-frames.

That constraint is worth stating plainly, because the obvious fix makes things
worse. Putting every row on the one mode openh264 does support, ABR, drops
next264 to 1.49x on the SIMD row, and almost none of that is speed: across the
six clips the ABR speed ratio correlates 0.87 with the bits each encoder spent.
x264's rate control undershoots high-motion CIF and overshoots ducks, so a
matched-bitrate ratio scores whichever encoder happened to spend less. Matching
the rate is not the same as matching the work.

GPU-vendor encoders (NVENC, AMD VCN/AMF, Intel QSV, Apple VideoToolbox) are
fixed-function silicon driven through vendor APIs, with different
quality/latency trade-offs. They stay out of scope for the software boards. The
comparison pages document them for context.

## Building and trying it

Requires a C11 compiler, Meson >= 1.1, and Ninja.

```sh
meson setup build && ninja -C build
meson test -C build

# encode a Y4M stream to Annex-B .264
ffmpeg -i input.mp4 -f yuv4mpegpipe - | build/cli/next264 --input-y4m - --crf 23 -o out.264
ffmpeg -i out.264 -f null -   # verify it decodes
```

The default build is self-contained. One optional Meson flag, `-Dgpu=enabled`,
fetches and links [nextgpu](https://github.com/terranvigil/nextgpu), a Metal
compute library shared with other encoders. It's off by default and nothing here
needs it: the CPU implementation is complete, and every number published here
was measured with it. The GPU path has passed its own convention checks but not
the encoder-side quality gate, so treat it as experimental.

## What's in docs/

The full record is there today, and a docs site is being assembled from it:

- **Methodology**: how the AI loop worked, from the measurement discipline and
  the instrument catalog to the refusal records and the traps that each cost a
  day.
- **How H.264 works**: an explainer of the format and each coding tool, written
  alongside the implementation.
- **Test corpus**: the clips, their classes, and where they come from (Xiph
  "derf" sequences plus modern 720p/1080p material).
- **Comparisons**: full per-clip boards and the feature matrix.

## Roadmap

- Close goal 3's remaining median, the last 6%. Whether it closes without an
  assembly tier is still an open question.
- x86-64 SIMD. The current SIMD tier is AArch64 NEON only.
- Decoder speed. The built-in decoder exists for conformance today, and
  bringing it to parity with the fastest decoders is a planned track.
- Add more software encoders to the comparison table. openh264 is done, and
  measuring it in-process would let that table share a harness with the goal
  table above.
- Shot-based single-pass encoding. Stage 1 is built.

## Provenance

next264 is written from scratch. Other encoders were used as measurement
baselines and nothing else: they were built, run and timed so that every goal
here had a real number to match or beat rather than one I invented for myself.
No source was copied, transliterated or ported from any of them.

Keeping it that way took real discipline. `CONTRIBUTING.md` sets the clean-room
rules the project is written under, and two of them carry most of the weight:
transliterating another implementation into a different style is still copying,
and anyone who has recently read another encoder's source should not be the one
to author mode decision, entropy coding, rate control or motion estimation here.

Where the standard dictates a number, I use that number and cite the table it
comes from. Where it dictates an algorithm, two correct implementations will
resemble each other, and paraphrase won't change that. Everything above the
standard is mine, and `docs/` records how each piece was measured into place,
dead ends included.

## License

BSD-2-Clause, stated per file as well as in `LICENSE`.
