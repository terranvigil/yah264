---
title: Results - yah264
description: The goal tables, quality across the rate range, the corpus and how each number reproduces.
---

# Results

yah264 is at full parity with x264 0.165.3222 at `--preset medium`, coding the
same source to the same file size. Speed is yah264's wall time over x264's, so
under 1.00 means yah264 finished first.

Goals 1 and 2 run plain C on both sides, with x264 built without its assembly
and without `-fno-tree-vectorize` so the compiler vectorizes both alike. Goal 3
runs what each ships: our NEON against x264's hand-written assembly.

Quality is [VMAF v1](https://github.com/terranvigil/yah264/blob/main/docs/vmaf.md),
which predicts how good video looks to people and also sees banding, chroma
loss and blocking. On both sides, pictures smaller than 1920x1080 in both
dimensions are scaled up to fit before scoring. A small encode is then judged
the way it would be watched.

These figures were timed on one 18-core Apple M5 Max. A group passes when its
median ratio is at or under 1.00 and its worst cell at or under 1.15, with
quality holding. All 27 group-and-goal readings that count toward parity pass.

## The test set

| group | what it is |
|---|---|
| S, short clips | twelve standard test clips from CIF to 4K |
| M, 2-minute segments | Chimera, Sol Levante and Meridian at 1080p and 4K. Each starts at its first non-black frame. |
| L, 10-minute excerpts | seven open-licensed films at 720p, 1080p and 4K with end credits cut off: Tears of Steel, Meridian, StEM2, Sintel, Big Buck Bunny, Cosmos Laundromat and Nocturne |
| C, content types | film grain, sports and screen content. Quality counts toward parity. Speed is shown but doesn't count. |

In the parity tables, both encoders run as command-line programs reading the
same 8-bit 4:2:0 frames on stdin from one feeder.

## Speed

Each cell is the group's median and worst ratio.

| class | group | goal 1, C, one thread | goal 2, C, all cores | goal 3, SIMD, all cores |
|---|---|--:|--:|--:|
| CIF | S | 0.73 / 0.74 | 0.73 / 0.75 | 0.78 / 0.83 |
| 720p | S | 0.66 / 0.70 | 0.59 / 0.64 | 0.68 / 0.84 |
| 720p | L | 0.65 / 0.69 | 0.59 / 0.74 | 0.54 / 0.61 |
| 720p | C | 0.61 / 0.61 | 0.63 / 0.63 | 0.62 / 0.62 |
| 1080p | S | 0.59 / 0.61 | 0.62 / 0.69 | 0.73 / 0.89 |
| 1080p | M | 0.51 / 0.53 | 0.58 / 0.64 | 0.63 / 0.63 |
| 1080p | L | 0.54 / 0.56 | 0.58 / 0.72 | 0.58 / 0.60 |
| 1080p | C | 0.62 / 0.62 | 0.64 / 0.64 | 0.65 / 0.65 |
| 2160p | S | 0.49 / 0.52 | 0.53 / 0.58 | 0.64 / 0.71 |
| 2160p | M | 0.46 / 0.51 | 0.48 / 0.54 | 0.52 / 0.54 |
| 2160p | L | 0.48 / 0.54 | 0.51 / 0.58 | 0.54 / 0.56 |
| 2160p | C | 0.53 / 0.53 | 0.56 / 0.56 | 0.61 / 0.61 |

The slowest cell anywhere is the short 1080p clips with SIMD on, at 0.89. The
4K content-type cell is one four-second clip, so treat it as a thin reading.

## Quality at the same size

Each cell is ours minus x264's. VMAF v1 shows median / worst, and PSNR-Y the
worst in dB. Positive favors yah264.

| class | group | VMAF v1 | PSNR-Y worst | worst cell |
|---|---|--:|--:|---|
| CIF | S | +2.70 / +0.82 | -0.54 | stefan_cif |
| 720p | S | +0.51 / +0.35 | -0.10 | shields_720p |
| 720p | L | +1.17 / +0.10 | -0.63 | bbb_720p |
| 720p | C | +1.42 / +1.42 | -0.11 | lash1924_720p |
| 1080p | S | +0.76 / +0.33 | -0.05 | sunflower_1080p |
| 1080p | M | +0.64 / +0.01 | +0.16 | chimera_1080p |
| 1080p | L | +0.52 / -0.37 | -1.06 | cosmos_1080p |
| 1080p | C | +1.57 / +1.57 | -0.22 | lash1924_1080p |
| 2160p | S | +0.24 / +0.10 | -0.81 | bbb_2160p |
| 2160p | M | +0.57 / +0.05 | -0.31 | chimera_2160p |
| 2160p | L | +1.23 / -0.24 | -1.03 | bbb_2160p |
| 2160p | C | +1.30 / +1.30 | +0.02 | boxing_2160p |

## Across the rate range

Each real title was also coded at five rates, at CRF and capped VBR, on six
threads with a 40-frame lookahead. All 52 ladders pass the bar of a
[BD-rate](encoding.html#bd-rate) on VMAF v1 no worse than +1.0%. yah264 needs a
median 10% fewer bits than x264 for the same score, and 33% fewer on Meridian
at 4K. Four
ladders came out behind:

- Big Buck Bunny at 4K VBR, by 0.88%
- Cosmos Laundromat at 1080p CRF, by 0.32%
- Cosmos Laundromat at 1080p VBR, by 0.28%
- Big Buck Bunny at 1080p VBR, by 0.17%

The ladders ran on a rented Linux arm64 instance whose one-thread encodes
match this machine's byte for byte. A CRF ladder run on both machines reads the
same. A capped-VBR ladder reads about a point worse for yah264 on the instance,
so the VBR figures here are if anything low for yah264. The instance decoded its sources in software, and on 30
ladders coded its own decode instead of this machine's frames.

Every capped-VBR rung came within 5% of its target. Goal 3 speed read 0.45 to
0.67 on every VBR cell.

We also swept our lookahead depth against x264's:

| lookahead sweep | setting |
|---|---|
| our depths | 30, 40 and 50 frames |
| x264's depth | 40, its default |
| threads | three per encoder |
| modes | CRF and capped VBR, on the same rungs |
| cells | seven of the 720p and 1080p real-title cells: the three 1080p segments, Tears of Steel, Big Buck Bunny and Meridian at 1080p, and Sintel at 720p. No 4K titles. |
| Big Buck Bunny 1080p, 50 over 40 | 0.7 points at CRF, 0.4 at VBR |

On most swept ladders the three depths read within 0.4 points of each other,
the most a near-null control or a repeat run moved a ladder. Big Buck Bunny at
1080p did best at 50 in both modes. Every other figure on this page uses 40.

## By processor

The Apple row is the parity table above, as each goal's range of group
medians. The other rows come from rented cloud machines on fewer clips and are
reported apart. See [architectures](architectures.md) for the tiers.

| processor | SIMD tier | goal 1 | goal 2 | goal 3 | what exists |
|---|---|--:|--:|--:|---|
| arm64: Apple M5 Max, 18 cores | NEON + dot product | 0.46-0.73 | 0.48-0.73 | 0.52-0.78 | full parity in the tables above |
| arm64: AWS Graviton4, 96 vCPU, Linux | NEON | 0.57 | 0.44 | 0.70 | median of 5 to 7 valid cells in a seven-clip check |
| arm64: AWS Graviton3, 32 vCPU, Linux | SVE (256-bit) + NEON | | | 0.65-0.74 CPU time, one thread | a two-clip profile and not goal 3, which is read at the default thread count |
| x86-64: AMD EPYC, 32 vCPU, rented | AVX2 | | | 0.86-0.96 CPU time, one thread | a two-clip profile and not goal 3. Slower than x264 at the default thread count on one clip (see below). |
| x86-64: GitHub runner | AVX2 | | | | output byte-identical to C on every push |
| x86-64: AMD EPYC, 32 vCPU, rented | AVX-512 | | | 0.84-0.93 CPU time, one thread | pixel comparison kernels, on by default where the chip has AVX-512 |
| x86-64: Intel Xeon, 32 vCPU, rented | AVX2 | | | 0.88-0.99 CPU time, one thread | a two-clip profile and not goal 3. Same binary as the AMD rows. Full-thread profile below. |
| x86-64: Intel Xeon, 32 vCPU, rented | AVX-512 | | | 0.84-0.97 CPU time, one thread | pixel comparison kernels, on by default where the chip has AVX-512 |
| x86-64 | SSE4.2 | | | | checked under emulation, no timing |

On the Graviton4 the worst valid cells read 0.66, 0.99 and 0.75. x264's plain C
there was built with gcc. Its clang build ran 12% to 18% faster on three cells,
so goals 1 and 2 flatter us by about that much.

The x86-64 and Graviton3 rows come from a profile instead of the parity method.
Each is CPU time against x264's for one encode at one thread with SIMD on and
CRF 23. The clips are 60 frames of sunflower at 1080p and 20 of crowd_run at
4K. Retired instructions:

| machine | yah264 instructions vs x264, two clips |
|---|--:|
| AMD EPYC, AVX2 | 0.88, 0.83 |
| AMD EPYC, AVX-512 kernels | 0.82, 0.78 |
| Intel Xeon | same as AMD (same binary) |
| Graviton3 | 0.67, 0.62 |

The Intel machine is a Xeon Platinum 8488C, 16 cores with two threads each. At
the same bitrate on sunflower, AMD retires 0.94 times x264's instructions but
uses 1.02 times its CPU time. Output matches the Mac's byte for byte, and
conformance passes on all three machines.

At default threads on AMD, one short 60-frame run that includes tool start-up
took 0.76 s against x264's 0.53 s on sunflower, with more CPU time. crowd_run
took 1.17 s against 1.14 s.

The Intel machine gave the first clean all-thread reading on x86-64: default
threads, full 500-frame clips, one piped feed, median of five runs. Because both
ran at CRF 23 instead of the same size, it's a profile and not goal 3. yah264's files
were 0.77 and 0.94 times x264's bytes.

| clip | wall vs x264 | CPU time vs x264 | yah264 CPU ms per frame | x264 CPU ms per frame |
|---|--:|--:|--:|--:|
| sunflower 1080p, AVX2 | 1.04 | 1.01 | 102.4 | 101.3 |
| sunflower 1080p, AVX-512 | 1.04 | 0.98 | | |
| crowd_run 4K, AVX2 | 0.90 | 0.76 | 541 | 714 |
| crowd_run 4K, AVX-512 | 0.87 | 0.72 | | |

On sunflower we're level on CPU time and 4% behind on wall time. At 4K we're
ahead on both. Threading at high core counts is the open x86 item, under study on Intel. No
goal has been read on x86-64 yet. A goal board on a rented AMD machine will fill
them in with the Apple row's method.

### Estimated cost per frame

CPU time per frame times the vCPU-hour price gives a rough cost per frame. On
sunflower 1080p with every thread busy, Graviton4 is cheapest either way:

| price basis | Graviton4 | AMD EPYC | Intel Xeon |
|---|--:|--:|--:|
| on-demand list price | 1.00 | about 1.65 | 1.61 |
| spot, 7-day median, us-east-2 | 1.00 | about 1.11 | 1.34 |

The inputs:

| machine | instance price is for | yah264 CPU ms per frame | on-demand per hour | spot per hour |
|---|---|--:|--:|--:|
| AWS Graviton4 | c8g.8xlarge | 70.9 | $1.276 | $0.384 |
| AMD EPYC | c7a.8xlarge | 90.9 | $1.642 | $0.334 |
| Intel Xeon | c7i.8xlarge | 102.4 | $1.428 | $0.356 |

The CPU times come from different runs: Graviton4's from the parity run's
full-thread rows on a c8g.24xlarge, AMD's from the short 60-frame run (hence
"about"), Intel's from the reading above. On-demand prices are AWS's published
rates, not fetched live. Pricing per CPU time absorbs Intel's vCPUs being
hyperthreads. For x264, AMD is cheapest at spot.

## The ten-clip board

The sections below use ten Xiph clips from CIF to 1080p, timed on the same
machine.

<div class="aside">
<p class="aside-title">Four words this page uses a lot</p>
<p><b>Board.</b> One timed run of both encoders over a fixed set of clips. This section reads the ten-clip board.</p>
<p><b>Band.</b> The clips and rates a quality sweep is read over, chosen so that x264 scores between VMAF v1 88 and 94 on each clip.</p>
<p><b>Leg.</b> One metric of a goal, read on its own.</p>
<p><b>Bar.</b> The number a leg has to beat.</p>
</div>

### Reading the tables

Each cell is the median of three interleaved samples. A goal is four metrics
plus a floor:

| metric | bar |
|---|---|
| median speed | 1.00x or faster |
| worst-clip speed | 1.15x or better |
| quality | within 0.5 [VMAF v1](https://en.wikipedia.org/wiki/Video_Multimethod_Assessment_Fusion) |
| compression | within 1.0% size |
| pixel accuracy (floor) | no clip more than 1.0 dB below x264 in PSNR-Y at the same bytes |

The size column only checks that the bitrate solve converged. The real
compression number is the [BD-rate](encoding.html#bd-rate) further down. A clip
0.5 dB below x264 joins the debt list but still passes. Quality decisions go by
VMAF.

Repeated boards move, so small differences aren't readings:

| noise and tolerance | value |
|---|---|
| a cell is dropped when its own samples spread over | 1.15x |
| a repeated board moves a ratio by | up to about 0.10 |
| differences that aren't readings | under 0.05 |
| bitrate solve residual | about 0.3% |
| the two rates are matched within | 0.5% |

At CRF and the same size every clip is ahead on VMAF v1. The floor passes.

| ten-clip board at CRF | value |
|---|---|
| median VMAF v1 lead, one thread | 1.3 |
| median VMAF v1 lead, three threads | 1.2 |
| smallest VMAF v1 lead on any clip | 0.75 |
| median PSNR-Y difference, one thread | +0.03 dB |
| worst PSNR-Y difference | stefan_cif at -0.51 dB, on the debt list |

## CRF versus ABR

CRF asks for a quality and takes the bitrate it needs. ABR asks for a bitrate
and takes the quality it buys. This table is ABR at matched achieved bitrate on
the ten-clip board. No goal is set against it.

| goal | configuration | median | max | VMAF v1 | size |
|---|---|--:|--:|--:|--:|
| 1 | pure C, single-threaded | 0.66x | 0.74x | +1.73 | +0.2% |
| 2 | pure C, multi-threaded | 0.69x | 0.85x | +1.93 | -0.1% |
| 3 | as-shipped SIMD, multi-threaded | 0.86x | 0.99x | +1.93 | -0.1% |

VMAF v1 and size are medians over the ten clips. Goals 2 and 3 code the same
stream, so their quality is read once, at three threads on both encoders. Speed is from the timed board, one thread for
goal 1 and auto for goals 2 and 3. x264's target is solved to our achieved
bitrate.

ABR costs us nothing over CRF single-threaded, and up to about a tenth of a
ratio point multi-threaded. Two changes keep it there: the rate controller's
opening is fitted, and a frame can start against a reference still being coded.
The [rate-control notes](https://github.com/terranvigil/yah264/blob/main/docs/rate-control.md)
explain both.

Median ratio by class, over three CIF, four 720p and three 1080p clips:

| goal | CRF CIF | CRF 720p | CRF 1080p | ABR CIF | ABR 720p | ABR 1080p |
|---|--:|--:|--:|--:|--:|--:|
| 1 | 0.68x | 0.65x | 0.58x | 0.71x | 0.66x | 0.59x |
| 2 | 0.66x | 0.64x | 0.63x | 0.82x | 0.65x | 0.69x |
| 3 | 0.77x | 0.79x | 0.77x | 0.94x | 0.83x | 0.83x |

The slowest cells are CIF and fourpeople, the low-bitrate 720p clip, plus
riverbed under ABR with SIMD.

| median absolute rate error, ten board clips | yah264 | x264 |
|---|--:|--:|
| multi-threaded | 6.6% | 5.6% |
| single-threaded | 3.8% | 4.0% |

The opening is fitted on seventeen non-board cells, holding the board out.

## Reproducing the tables

The parity tables come from `scripts/longform_throughput.py`, which feeds both
command-line encoders on stdin. The ten-clip board runs both as libraries in one
ffmpeg process instead.

Both get the same thread count: one for goal 1, and 0 (auto, as each ships) for
goals 2 and 3. Our auto is the smaller of the online core count and 16.

x264's configure always adds `-fno-tree-vectorize`, which denies its C the
auto-vectorization ours gets. For the pure C rows we build x264 with
`--disable-asm`, strip that flag from `config.mak` and rebuild. No runtime switch
can undo a compile flag. Left in, it alone pushes goal 2 to about
0.73x. The
[instruments catalog](https://github.com/terranvigil/yah264/blob/main/docs/instruments.md)
records the binaries.

The one-process wrapper lives in an
[ffmpeg fork](https://github.com/terranvigil/FFmpeg) for license reasons. This
runs the two-CLI version and needs no fork:

```
make parity-status-crf
```

## Read at equal quality instead

Everything above is read at equal bytes. Because both encoders stay at
`medium`, our VMAF margin goes unspent. The fair counter-question is a slower preset for the
reference until it matches our quality, or a faster one for us until we match
its. We haven't published that comparison.

## Quality across the rate range

In the deep band, VMAF v1 37 to 66, yah264 beats x264 on all twelve of the
thirteen quality-band clips that reach it, by a median of about 17% in
[BD-rate](encoding.html#bd-rate). Each clip runs 120 frames at one thread on
three or four shared rates. The band shares only six clips with the speed board.

yah264 also leads on every clip of the twelve-clip HD gate set, which we never
tuned against.

| HD gate set | median BD-rate on VMAF v1 | clips ahead |
|---|--:|--:|
| 720p | -19.3% | 6 of 6 |
| 1080p | -12.5% | 6 of 6 |

Each clip runs 150 frames at two threads over five CRF points three apart,
starting where x264 scores VMAF v1 93. crowd_run has the smallest lead, at
-6.1%.

## The hardware mode

`--hw videotoolbox` encodes through the Mac's H.264 engine with our options
mapped onto it and our scene-cut placing keyframes. Because the
stream is the hardware's, nothing above applies. Same ten clips, same bitrates:

| clip | kbit/s target | wall, hardware ÷ ours | CPU, ours ÷ hardware | VMAF v1, hardware − ours | our VMAF v1 | hardware VMAF v1 |
|---|--:|--:|--:|--:|--:|--:|
| foreman_cif | 400 | 1.88 | 12 | −6.1 | 75.8 | 69.7 |
| bus_cif | 400 | 1.78 | 11 | −11.9 | 76.8 | 64.9 |
| stefan_cif | 400 | 2.29 | 8 | −8.7 | 74.3 | 65.7 |
| ducks_720p | 25000 | 0.62 | 48 | −9.9 | 91.0 | 81.1 |
| park_joy_720p | 12000 | 0.65 | 32 | −9.2 | 92.3 | 83.1 |
| fourpeople_720p | 1200 | 1.45 | 16 | −6.4 | 88.5 | 82.1 |
| shields_720p | 2200 | 1.02 | 18 | −7.0 | 95.7 | 88.7 |
| sunflower_1080p | 1500 | 0.93 | 18 | −3.9 | 94.2 | 90.3 |
| pedestrian_1080p | 2800 | 0.80 | 27 | −5.3 | 89.9 | 84.5 |
| riverbed_1080p | 12500 | 0.32 | 46 | −12.6 | 89.5 | 76.9 |

At the median the hardware matches our wall time on about an eighteenth of
our CPU. It's faster on high-rate clips and most of HD, slower on CIF and
fourpeople, where opening the session is a large part of so short a run. It
scores below us at the same bitrate on all ten clips, by 7.8 points at the
median.

- Runs cover six seconds, or the whole clip if shorter.
- Wall and CPU: our encoder at auto threads, median of five interleaved runs
  per side on a quiet machine. Spread is under 10%, except foreman at 17%.
- CPU counts the encoder process only, not work the hardware does elsewhere.
- Quality: our encoder at three threads.
- Both come within about 10% of target on eight clips. On bus and stefan every
  encoder here undershoots 400 kbit/s by a wide margin.
- The hardware spends more bits than we do on seven clips (all but foreman,
  ducks and riverbed), which favors it on quality.

## Against other encoders

| encoder | quality (VMAF v1) | pure-C 1-thread | pure-C MT | SIMD MT | size | notes |
|---|--:|--:|--:|--:|--:|---|
| yah264 | +1.2 | 0.65x | **0.66x** | **0.77x** | +0.1% | this repo, ten-clip board at CRF |
| x264 | ref | 1.00x | 1.00x | 1.00x | ref | the reference point |
| openh264 | -12.3 | **0.15x** | 1.01x | 0.94x | +3.3% | a different design point |

openh264 beats x264 single-threaded by doing far less work per frame, and pays
about 12 VMAF v1 points for it. The yah264 row is the ten-clip board. The
openh264 row runs Cisco's encoder on the same clips and bitrates at its own
defaults, frame skipping off, timed as command-line tools against x264's in the
same three builds. Quality and size are medians at the same target. Every
encoder ran at three threads for them.

| clip | kbit/s target | wall, openh264 ÷ ours | CPU, ours ÷ openh264 | VMAF v1, openh264 − ours | our VMAF v1 | openh264 VMAF v1 |
|---|--:|--:|--:|--:|--:|--:|
| foreman_cif | 400 | 1.0 | 8.2 | −19.5 | 75.8 | 56.3 |
| bus_cif | 400 | 1.0 | 7.9 | −24.3 | 76.8 | 52.5 |
| stefan_cif | 400 | 1.0 | 8.2 | −27.5 | 74.3 | 46.8 |
| ducks_720p | 25000 | 2.0 | 7.4 | −13.6 | 91.0 | 77.4 |
| park_joy_720p | 12000 | 1.9 | 7.5 | −15.6 | 92.3 | 76.7 |
| fourpeople_720p | 1200 | 1.3 | 9.6 | −11.1 | 88.5 | 77.4 |
| shields_720p | 2200 | 1.8 | 7.2 | −11.9 | 95.7 | 83.7 |
| sunflower_1080p | 1500 | 1.6 | 7.3 | −11.6 | 94.2 | 82.6 |
| pedestrian_1080p | 2800 | 1.9 | 6.6 | −12.9 | 89.9 | 77.0 |
| riverbed_1080p | 12500 | 1.8 | 4.8 | −18.8 | 89.5 | 70.7 |

Wall and CPU ran ours at auto threads against openh264 with SIMD at twelve.
Quality ran both at three.

openh264 is built for real-time conferencing: no B-frames, no lookahead, a light
analysis pass. At the median it uses about a seventh of our CPU, scores 14.6
VMAF v1 points lower and takes 1.68x our wall time. Its wall time doesn't fall
with threads, because it threads per slice and these streams have one.

GPU encoders, fixed-function silicon with their own trade-offs, are out of
scope.

## Across shots

Real content is a chain of shots. Three five-shot sequences built from the
board clips, with hard cuts at known frames, show how an encoder moves bits
between easy and hard shots.
[make_multishot.py](https://github.com/terranvigil/yah264/blob/main/scripts/make_multishot.py)
rebuilds them byte for byte.

| sequence | fps | CRF points | yah264 vs x264 | `--shot-crf` vs x264 | same clips coded one at a time |
|---|--:|---|--:|--:|--:|
| five CIF shots over 690 frames | 30 | 22 to 34 | -12.3% | -22.8% | -14% |
| five 720p shots over 750 frames | 50 | 22 to 34 | -12.8% | -19.5% | -14% |
| five 1080p shots over 750 frames | 25 | 26 to 38 | -8.4% | -16.7% | -12% |

Figures are BD-rate on VMAF v1 against x264 medium at two threads, from a
cubic fit over five CRF points. Negative means fewer bits. The last column is a
median. Shots run 150 frames, except the CIF third shot at 90.

On every input path, streaming included, yah264 puts the frame-mean part of adaptive quantization on
the base QP, at strength 1.3 under uncapped CRF and 1.04 under a VBV cap or
bitrate target. `--shot-crf` also sizes each shot against the whole title on
file input. The CIF and 720p rows:

```
scripts/multishot_bd.py --threads 2 --points 22,25,28,31,34 --seqs ms_cif_30,ms_720p_50 \
  --arms new=env: \
  'shot=build/cli/y264 --input-y4m {src} --crf {q} --shot-crf --threads 2 -o {out}' \
  'x264=x264 --preset medium --crf {q} --threads 2 --demuxer y4m -o {out} {src}'
```

The 1080p row is the same command with `--seqs ms_1080p_25 --points 26,29,32,35,38`.

## The corpus

The ten board clips are natural 8-bit 4:2:0 video from Xiph's public
collection, at native resolution. The rate is each clip's ABR target, chosen on
the earlier metric and kept so speed stays comparable.

| clip | size | what it is | rate |
|---|---|---|---|
| foreman | CIF, 352x288 | a talking head, then a pan to a building site | 400 kbps |
| bus | CIF | a bus passing under a tracking camera | 400 kbps |
| stefan | CIF | a tennis player, fast motion over a crowd | 400 kbps |
| ducks | 720p | ducks taking off from water, heavy motion | 25 Mbps |
| park_joy | 720p | runners in a park under trees, detail under motion | 12 Mbps |
| fourpeople | 720p | a video conference, almost static | 1.2 Mbps |
| shields | 720p | a pan over a wall of shields, fine detail | 2.2 Mbps |
| sunflower | 1080p | a smooth close-up of a sunflower and a bee | 1.5 Mbps |
| pedestrian | 1080p | a fixed camera over walking people | 2.8 Mbps |
| riverbed | 1080p | flowing water at the edge of noise, the hardest clip here | 12.5 Mbps |

The wider corpus adds animation and high-motion sport, with sources and
licenses in the
[corpus list](https://github.com/terranvigil/yah264/blob/main/docs/corpus-sources.md).
Fitted coefficients are calibrated on the training half and reported on the
separate, test-only gate set.

## Off-corpus content

Outside that set only a range holds. Two clips fairly called
[animation](https://github.com/terranvigil/yah264/blob/main/docs/animation.md)
sit 39 BD-rate points apart. On 180 frames of 3D CGI Big Buck Bunny, yah264
needs 37% fewer bits than x264 medium on VMAF v1. On 140 frames of hand-drawn Sita Sings
the Blues it needs 2% more. The same spread makes preset-for-preset speed comparisons
weak off the tuning set.

On the 10-minute Big Buck Bunny excerpt yah264 needs 1% to 2% fewer bits than
x264 at CRF, at every resolution. At capped VBR the two are within 1%.
