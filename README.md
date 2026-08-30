# yah264

**An H.264/AVC encoder written from the ground up by looking at the performance
and quality of the top existing H.264 encoders and trying to match or surpass
them.**

Full documentation is at **<https://terranvigil.github.io/yah264/>**: how video
encoding works, how H.264 works tool by tool, getting started, the design, the
measured results, and how the project was built. The pages are in `site/` and
are rendered by `scripts/build_site.py`.

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
schedules it, so yah264's SIMD tier is about 5,500 lines of NEON intrinsics,
each kernel validated against the C reference and benchmarked by a checkasm
harness. Part of goal 3's remaining gap is the price of that choice.

## Where it stands

Three speed goals over a six-clip set of CIF and 720p material at a matched
operating point. Each is scored as a ratio to the reference encoder's wall time,
so the bar reproduces on any machine: 1.00x is parity, lower is faster. A goal
passes or fails on four metrics, all read at the same matched point:

| metric | bar |
|---|---|
| median speed | 1.00x or faster |
| worst-clip speed | under 1.15x |
| quality | within 0.5 VMAF |
| compression | within 1.0% size |

The operating point is CRF, solved per clip onto a matched achieved bitrate.
That choice does most of the work in this table, so the ABR reading follows it
below rather than being left out.

**CRF, matched achieved bitrate**:

| goal | configuration | median | max | VMAF | size | status |
|---|---|--:|--:|--:|--:|---|
| 1 | pure C, single-threaded (both encoders no-asm) | **0.95x** | 1.04x | +0.00 | +0.1% | **all metrics pass** |
| 2 | pure C, multi-threaded | **0.85x** | 0.96x | −0.08 | +0.2% | **all metrics pass** |
| 3 | as-shipped SIMD, multi-threaded | 0.96x | 1.14x | −0.07 | +0.2% | all metrics pass 33% of runs |

Goal 3 is the one to read carefully. It cleared every metric on the run above,
and on one of three runs at this rate tolerance: the other two read 1.00x and
1.02x, and both sat exactly at the 1.15x worst-clip bar rather than under it.
The run-to-run spread on this machine is around 0.07 on a median, which is
wider than goal 3's entire margin, and it is the machine rather than the
encoder. One favourable draw does not close a
goal, so goal 3 stays open until the board repeats across separate sessions.

Goal 3 is also scored on CIF and 720p only. At 1080p the same as-shipped tier
reads 1.28x to 1.47x across four clips.

**ABR, same bitrate on both sides**, for contrast:

| goal | configuration | median | max | VMAF | size |
|---|---|--:|--:|--:|--:|
| 1 | pure C, single-threaded | 1.02x | 1.20x | −0.27 | +2.8% |
| 2 | pure C, multi-threaded | 1.19x | 1.31x | +0.30 | +2.9% |
| 3 | as-shipped SIMD, multi-threaded | 1.40x | 1.52x | +0.31 | +2.9% |

That second table is not a speed measurement, which is why the goals are not
set against it. At a matched bitrate the two encoders are not doing the same
work: the ratio largely reports which one spent fewer bits. stefan_cif misses
its 400 kbps target by 48.8% on our side and 57.4% on x264's, and on that clip
we emit 20.2% more bits and take 1.51x the time. The size column says the
same thing at the summary level, sitting near 2.9% against a 1.0% bar.
Matching the rate is not the same as matching the work.

Both encoders run as libraries inside one ffmpeg process, off the same demuxer,
each choosing its own thread count the way it would for any caller. Neither is
told how many threads to use, because a benchmark that pins one number pins it
for both encoders and they do not want the same one. That matters more than it sounds. Measured through two
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
yah264 does best: at the deep band it leads x264 on 9 of 10 clips, median
BD-rate advantage around 12%.

The six board clips are natural video, three CIF and three 720p. That is what
these numbers cover, and content outside it behaves differently enough that the
honest summary is a range rather than a number. On 3D CGI animation we are 25.2%
BD-rate ahead of x264 and 1.34x slower at the same preset, which is the preset
ladder rather than the encoder: held at equal quality, `veryfast` reaches x264
medium for 1.07x and a fifth fewer bits. On hand-drawn 2D animation we are 7.8%
BD-rate behind. That is a 33-point swing between two clips both fairly called
animation, so we do not claim an animation result as one number.
`docs/animation-content.md` has the measurements.

Resolution is the other axis the board does not cover, and it moves the speed
rows more than the clip mix does. Hand both encoders one thread and the CIF
rows we win stop being wins: foreman_cif reads 1.16x that way, against 1.02x
when each picks its own thread count, because at CIF we keep more cores busy
than x264 manages to. Two costs stack there. Our SIMD trails x264's assembly
on a single thread, and our threading then burns more CPU than theirs to use
the extra cores.

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

| encoder | pure-C 1-thread | pure-C MT | SIMD MT | quality (VMAF) | size | notes |
|---|--:|--:|--:|--:|--:|---|
| yah264 | **0.95x** | **0.85x** | 1.01x | −0.07 | +0.2% | this repo |
| x264 | 1.00x | 1.00x | 1.00x | ref | ref | the reference point |
| openh264 | 0.18x | 0.76x | 0.78x | −9.3 | +0.9% | not a matched point, see below |

The first two speed columns are the same in-process board as the goal table, at
the same matched operating point, so they are the same numbers. The SIMD MT
column is not: it carries an earlier read of that board, taken before its rate
match was tightened, which is why 1.01x is not one of the three runs counted
above. It and the 0.96x are two draws from the same ~0.07 spread rather than a
contradiction.

openh264's row is a third measurement again, and its speed cannot be read
against the other two. It exposes no quality knob through ffmpeg, only a bitrate, so there is
nothing to solve onto a common operating point. Boarded at a matched bitrate it
sits more than 9 VMAF below both other encoders, and that deficit is most of why it looks
fast. Its comparable number is BD-rate, which normalises for quality, and there
it costs +63.7%. It also has no B-frames.

That constraint is worth stating plainly, because the obvious fix makes things
worse. Putting every row on the one mode openh264 does support, ABR, drops
yah264 to 1.49x on the SIMD row, and almost none of that is speed: across the
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
ffmpeg -i input.mp4 -f yuv4mpegpipe - | build/cli/yah264 --input-y4m - --crf 23 -o out.264
ffmpeg -i out.264 -f null -   # verify it decodes
```

That pipe runs three processes over one machine, and the decode alone can take a
third of it. To call the encoder as a library instead, `ninja -C build install`
gives you `libyah264` with headers and a pkg-config file, and an ffmpeg built
against it encodes with `-c:v libyah264`. The `libavcodec` wrapper is LGPL and so
lives on the `yah264` branch of an ffmpeg fork rather than here. It is not
upstream yet, so that fork is the route today; the steps are in
[site/start.md](site/start.md) and the design is in
`docs/ffmpeg-integration-plan.md`. Unlike `--enable-libx264`, enabling it does
not force `--enable-gpl`: yah264 is BSD-2-Clause and the resulting ffmpeg stays
LGPL.

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
- **Threading**: why asking an encoder for every core can make it slower, what a
  picture can actually absorb, and how the thread count is chosen.
- **Animation**: what happens when content leaves the corpus, and why a
  preset-for-preset speed comparison stops meaning what it says.
- **Test corpus**: the clips, their classes, their licences and exactly where
  each came from, plus the training set and which libraries are worth pulling
  from next.
- **Comparisons**: full per-clip boards and the feature matrix.

## Roadmap

- Close goal 3's remaining median. It misses by 0.01 today, which is inside the
  board's own noise, so the honest statement is that it sits at the bar rather
  than under it.
- x86-64 SIMD. The current SIMD tier is AArch64 NEON only.
- Decoder speed. The built-in decoder exists for conformance today, and
  bringing it to parity with the fastest decoders is a planned track.
- Add more software encoders to the comparison table. openh264 is done, and
  measuring it in-process would let that table share a harness with the goal
  table above.
- Shot-based single-pass encoding. Stage 1 is built.

## Provenance

yah264 is written from scratch. Other encoders were used as measurement
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
