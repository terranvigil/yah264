---
title: Results - yah264
description: The goal tables, quality across the rate range, the corpus, and how each number reproduces.
---

# Results

yah264 reached full parity with x264. Both encoders code the
same source at the same file size. Every speed figure here is yah264's wall
time over x264's, so anything under 1.00 means yah264 finished first. x264 is
version 0.165.3222 at `--preset medium`. Goals 1 and 2 run plain
C on both sides. Our C runs against the same x264 version built with its assembly
off and with `-fno-tree-vectorize` removed, so the compiler vectorizes its C the
way it vectorizes ours. Goal 3 runs what each encoder ships: our NEON against
x264's hand-written assembly. Quality is scored with the VMAF NEG model, which predicts
how good video looks to people.

Every figure in this section comes from the current parity measurement, timed on
one Apple M5 Max with 18 cores. A group passes when its median ratio is at or
under 1.00 and its worst cell at or under 1.15, with its quality holding. All 27 group-and-goal readings that count toward parity pass.

## The test set

| group | what it is |
|---|---|
| S, short clips | twelve standard test clips from CIF to 4K |
| M, 2-minute segments | Chimera, Sol Levante and Meridian at 1080p and 4K, each starting at its first non-black frame |
| L, 10-minute excerpts | seven open-licensed films at 720p, 1080p and 4K: Tears of Steel, Meridian, StEM2, Sintel, Big Buck Bunny, Cosmos Laundromat and Nocturne, with end credits cut off |
| C, content types | film grain, sports and screen content, where quality counts toward parity and speed is shown but doesn't |

Both encoders read the same 8-bit 4:2:0 frames on stdin.

## Speed

Median and worst ratio in each group. A ≤ marks a reading where the source
couldn't be fed as fast as yah264 encoded, so it's an upper bound on our ratio.

| class | group | goal 1, C, one thread | goal 2, C, all cores | goal 3, SIMD, all cores |
|---|---|--:|--:|--:|
| CIF | S | 0.72 / 0.75 | 0.71 / 0.73 | 0.82 / 0.88 |
| 720p | S | 0.66 / 0.70 | 0.60 / 0.64 | 0.70 / 0.84 |
| 720p | L | 0.68 / 0.69 | 0.62 / 0.76 | 0.56 / 0.62 |
| 720p | C | 0.60 / 0.60 | 0.64 / 0.64 | 0.63 / 0.63 |
| 1080p | S | 0.60 / 0.62 | 0.64 / 0.69 | 0.75 / 0.88 |
| 1080p | M | 0.51 / 0.56 | 0.59 / 0.70 | 0.69 / 0.71 |
| 1080p | L | 0.54 / 0.58 | 0.61 / 0.72 | 0.59 / 0.61 |
| 1080p | C | 0.62 / 0.62 | 0.66 / 0.66 | 0.65 / 0.65 |
| 2160p | S | 0.50 / 0.52 | 0.55 / 0.60 | 0.67 / 0.73 |
| 2160p | M | 0.47 / 0.58 | 0.49 / 0.60 | 0.53 / 0.57 |
| 2160p | L | 0.52 / 0.55 | ≤0.58 / 0.62 | ≤0.57 / 0.59 |
| 2160p | C | not measured | not measured | void |

The slowest cell anywhere is 0.88, a short CIF clip with SIMD on. The 4K
content-type clips are the one gap. Goals 1 and 2 haven't been timed there, and
the goal 3 run failed a validity rule. Those cells sit outside the parity count.

## Quality at the same size

The table gives the VMAF-NEG difference, ours minus x264's (median / worst),
and the worst PSNR-Y difference in dB. Positive means yah264 looks better.

| class | group | VMAF-NEG | PSNR-Y worst | worst cell |
|---|---|--:|--:|---|
| CIF | S | -0.05 / -0.28 | -0.29 | stefan_cif |
| 720p | S | +0.19 / +0.06 | +0.02 | park_joy_720p |
| 720p | L | +0.85 / +0.32 | -0.21 | bbb_720p |
| 720p | C | +2.06 / +2.06 | +0.31 | lash1924_720p |
| 1080p | S | +0.48 / +0.12 | +0.06 | riverbed_1080p |
| 1080p | M | +0.72 / +0.71 | +0.24 | sollevante_1080p |
| 1080p | L | +0.98 / -0.06 | -0.85 | cosmos_1080p |
| 1080p | C | +1.13 / +0.04 | -0.10 | worldcup_far_1080p |
| 2160p | S | +1.59 / +0.60 | -0.74 | bbb_2160p |
| 2160p | M | +1.20 / +0.53 | +0.07 | sollevante_2160p |
| 2160p | L | +1.79 / +0.27 | -0.72 | bbb_2160p |
| 2160p | C | +2.18 / +2.18 | +0.39 | boxing_2160p |

## Across the rate range

Each real title was also coded at five rates, at CRF and at capped VBR. Both
encoders ran on six threads with a 40-frame lookahead. The bar is a
[BD-rate](encoding.html#bd-rate) on VMAF-NEG no worse than +1.0%. All 52 ladders
pass. At the median yah264 needs 13% fewer bits than x264 for the same score. On
Meridian at 4K it needs 55% fewer. One ladder came out behind: Big Buck Bunny at
720p VBR by 0.27%.

Of those ladders, 49 have some rates measured on rented Linux arm64 instances.
An instance only counted after it reproduced this machine's output. Their source
frames were decoded in software. On 27 ladders the instance coded its own decode
of the title instead of this machine's frames.

We also swept our lookahead at 30, 40 and 50 frames against x264's default of
40. Most ladders moved less than the 0.4-point run-to-run spread. Big Buck Bunny
at 1080p and 4K, Chimera at 4K, Meridian at 1080p and 4K and Sol Levante at 4K
did best at 50 in at least one mode. Sintel at 4K and Sol Levante at 1080p did
best at 40 in at least one mode. Every other figure on this page uses 40.

In capped VBR, with the target set to x264's CRF 23 rate, every encode came
within 4% of its target. Goal 3 read between 0.49 and 0.67 on every VBR cell. On
two of them the source couldn't be fed as fast as yah264 encoded, so those
readings are upper bounds on our ratio.

## By processor

The three goals are read per processor and instruction set. The Apple row is the
parity table above. The other rows come from rented cloud machines and use
fewer clips. They're reported apart from it. The
[architectures](architectures.md) page says what each tier is.

| processor | SIMD tier | goal 1 | goal 2 | goal 3 | what exists |
|---|---|--:|--:|--:|---|
| arm64: Apple M5 Max, 18 cores | NEON + dot product | 0.47-0.72 | 0.49-0.71 | 0.53-0.82 | full parity, the tables above |
| arm64: AWS Graviton4, 96 vCPU, Linux | NEON | 0.57 | 0.44 | 0.70 | seven-clip check, median of 5 to 7 valid cells |
| arm64: AWS Graviton3, 32 vCPU, Linux | SVE (256-bit) + NEON | | | 0.65-0.74 CPU time, one thread | two-clip profile; not goal 3, which is read at the default thread count |
| x86-64: AMD EPYC, 32 vCPU, rented | AVX2 | | | 0.86-0.96 CPU time, one thread | two-clip profile after the second x86 tuning round; not goal 3; slower than x264 at the default thread count on one clip, see below |
| x86-64: GitHub runner | AVX2 | | | | output byte-identical to C on every push |
| x86-64: AMD EPYC, 32 vCPU, rented | AVX-512 | | | 0.84-0.93 CPU time, one thread | on by default where the chip has it; pixel comparison kernels |
| x86-64: Intel Xeon, 32 vCPU, rented | AVX2 | | | 0.88-0.99 CPU time, one thread | two-clip profile, same binary as the AMD rows; not goal 3; full-thread profile below |
| x86-64: Intel Xeon, 32 vCPU, rented | AVX-512 | | | 0.84-0.97 CPU time, one thread | on by default where the chip has it; pixel comparison kernels |
| x86-64 | SSE4.2 | | | | checked under emulation, no timing |

The Apple row gives each goal's range of group medians.

On the Graviton4 the worst valid cells read 0.66, 0.99 and 0.75. x264's plain-C
build there was compiled with gcc. Its clang build turned out 12% to 18% faster
on the three cells checked, so goals 1 and 2 flatter us by about that much on
that machine.

The x86-64 and Graviton3 figures come from a profiling run instead of the
parity method. Each is one encode at one thread with SIMD on and CRF 23 on both
encoders, over 60 frames of sunflower at 1080p and 20 of crowd_run at 4K. The
number is CPU time against x264's. On the AMD machine, yah264 retires 0.88 and
0.83 times x264's instructions with AVX2. The AVX-512 kernels bring that to 0.82
and 0.78. The Intel machine is a Xeon Platinum 8488C with 16 cores and two threads
per core. It ran the same binary, so its counts are the same. At the same bitrate on
sunflower the AMD machine retires 0.94 times x264's instructions but uses 1.02
times its CPU time. On the Graviton3 it retires 0.67 and 0.62 times x264's
instructions. Output is byte-identical to the Mac's on all three machines, and
conformance passes.

At the default thread count the AMD machine tells a different story. On a single
60-frame run, sunflower took 0.76 s against x264's 0.53 s and used more CPU time.
crowd_run took 1.17 s against 1.14 s. That run is short and includes tool
start-up.

The Intel machine gave the first clean all-thread reading on x86-64. Each
encoder ran at its default thread count over the full 500-frame clips while both
read one piped feed. The table gives the median of five runs. It's still a
profile and not goal 3, because both encoders ran at CRF 23 instead of at the
same file size. yah264's files came out smaller, at 0.77 and 0.94 times x264's
bytes.

| clip | wall vs x264 | CPU time vs x264 | yah264 CPU ms per frame | x264 CPU ms per frame |
|---|--:|--:|--:|--:|
| sunflower 1080p, AVX2 | 1.04 | 1.01 | 102.4 | 101.3 |
| sunflower 1080p, AVX-512 | 1.04 | 0.98 | | |
| crowd_run 4K, AVX2 | 0.90 | 0.76 | 541 | 714 |
| crowd_run 4K, AVX-512 | 0.87 | 0.72 | | |

On sunflower we're level on CPU time and 4% behind on wall time. At 4K we're
ahead on both. Threading at high core counts is the open x86 item, and Intel is
where it's now sized. None of the three goals has been read on x86-64 yet. A
goal board run on a rented AMD machine, using the same method as the Apple row,
will fill them in.

### Estimated cost per frame

Which rented machine encodes a frame most cheaply? Multiplying yah264's CPU time
per frame by the price of a vCPU-hour gives a rough answer. On sunflower 1080p
with every thread busy, Graviton4 comes out cheapest on both price bases:

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

Treat it as an estimate, because the three CPU times don't come from one run.
Graviton4's comes from the parity run's full-thread rows on a c8g.24xlarge. AMD's comes from the short 60-frame run, which is why its
ratios say "about". Only Intel's is the reading above. The on-demand prices are AWS's published rates,
not fetched live. Intel's vCPUs are hyperthreads, and a measure per unit of CPU
time absorbs that. For x264 the order differs: at spot, AMD is cheapest.

## The earlier ten-clip board

Before parity this page read a ten-clip board of Xiph clips from CIF to 1080p,
timed on the same machine. The sections below still come from that board. They
show how the encoder got here. The hardware mode, the other encoders and the
multi-shot sequences have no parity-report reading yet.

<div class="aside">
<p class="aside-title">Four words this page uses a lot</p>
<p><b>Board.</b> One timed run of both encoders over a fixed set of clips. This section reads the ten-clip board.</p>
<p><b>Band.</b> The clips and CRF rungs a quality sweep is read over, chosen to fall inside VMAF-NEG 55 to 95.</p>
<p><b>Leg.</b> One metric of a goal, read on its own.</p>
<p><b>Bar.</b> The number a leg has to beat.</p>
</div>

### Reading the tables

Every figure here comes from one Apple M5 Max with 18 cores. Each cell is the median of three
interleaved samples. A board repeated on the same machine still moves its
ratios, so small differences in these tables are not readings. A goal
is four metrics. A fifth reads underneath them as a floor.

| metric | bar |
|---|---|
| median speed | 1.00x or faster |
| worst-clip speed | 1.15x or better |
| quality | within 0.5 [VMAF](https://en.wikipedia.org/wiki/Video_Multimethod_Assessment_Fusion) |
| compression | within 1.0% size |
| pixel accuracy (floor) | no clip more than 1.0 dB below x264 in PSNR-Y at the same bytes |

The size column reads what the bitrate solve left over. Read it as a check
that the solve converged. The real compression number is
[BD-rate](encoding.html#bd-rate), further down the page. A clip 0.5 dB below
x264 joins the debt list while the build still passes. Quality decisions are
made on VMAF.

The numbers: a cell is dropped when its own samples spread over 1.15x. A
repeated board moves a ratio by up to about 0.10, so differences under 0.05 are
not readings. The solve leaves about 0.3% residual. The two rates are matched
within 0.5%, wider than any VMAF gap in the goal table. PSNR-Y over ten clips
in pure C at CRF medians -0.07 dB against x264, worst on bus_cif at -0.47 dB.
The floor passes and the debt list is empty.

## CRF versus ABR

Ask an encoder for a quality level. It gives you whatever bitrate that takes.
That is CRF. Ask for a bitrate
instead. You get whatever quality that buys. That is ABR. No goal is set
against the ABR table.

**ABR, matched achieved bitrate**, on the earlier ten-clip board, before
parity. The parity tables at the top of this page cover capped VBR instead.

| goal | configuration | median | max | VMAF | size |
|---|---|--:|--:|--:|--:|
| 1 | pure C, single-threaded | 0.93x | 1.14x | +0.34 | −0.1% |
| 2 | pure C, multi-threaded | 0.90x | 1.08x | +1.23 | +0.1% |
| 3 | as-shipped SIMD, multi-threaded | 1.07x | 1.23x | +1.25 | +0.1% |

Here x264's target is solved to hit the bitrate we achieved. That is what the
size column shows. We refitted the rate controller's opening and let a frame
start against a reference still being coded. Single-threaded, ABR now costs us
nothing over CRF, and multi-threaded it costs about a tenth of a ratio point.
[rate-control.md](https://github.com/terranvigil/yah264/blob/main/docs/rate-control.md)
has how both changes work.

**By resolution class**, median ratio over three CIF, four 720p and three 1080p
clips. The ABR columns come from the superseded table above.

| goal | CRF CIF | CRF 720p | CRF 1080p | ABR CIF | ABR 720p | ABR 1080p |
|---|--:|--:|--:|--:|--:|--:|
| 1 | 0.93x | 0.90x | 1.07x | 0.90x | 0.90x | 1.09x |
| 2 | 0.78x | 0.86x | 1.00x | 0.90x | 0.87x | 1.04x |
| 3 | 0.92x | 0.99x | 1.10x | 1.07x | 1.02x | 1.18x |

Bitrate orders these rows. Resolution doesn't. On that board the slow cells were
the low-bitrate HD ones, and the fastest were the high-bitrate 1080p
ones. The 1080p column reads high because two of its three clips are
low-bitrate.

The numbers: our median absolute rate error over the ten board clips went from
12.5% to 6.6% multi-threaded and from 7.5% to 3.8% single-threaded, against
x264's 5.6% and 4.0%. We fitted the opening on seventeen non-board cells and
held the board out. Matched-rate BD-rate gained 2% at the median. That board
moved the ratios by 0.01 to 0.04. The old resolution-only seed opened the
high-bitrate cells near QP 6 to 17 against operating points of 27 to 34.
One high-rate demo clip opened five QP too high. Those openings caused the earlier −14% and
+18% rate misses. The staircase alone took the multi-threaded rows from 1.06x
and 1.26x to 0.94x and 1.11x. Before it, multi-threaded ABR had cost 0.3 to 0.4
of a ratio point over CRF. Earlier still, both encoders got the same target and the
sizes differed by about 3%. The single-threaded tier re-read with its median
unchanged. Its worst clip came out a hundredth higher, with fourpeople at
0.85x.

## How to reproduce them

Both encoders run as libraries inside a single ffmpeg process. Every encode
hands the same thread count to both. Goal 1 fixes that count at one on each
side. Goals 2 and 3 pass 0 for auto and let each encoder resolve its own
number, because auto is how each one ships. The pure C rows need one adjustment
to be fair. x264's configure adds `-fno-tree-vectorize` unconditionally, so a
stock build's C is not auto-vectorized while ours is. We build x264 with
`--disable-asm`, strip that flag from its `config.mak` and rebuild, so the
compiler treats both sides the same. The runtime switch can't do it instead,
because the flag was applied when the binary was compiled. Each number on this
page says which board it came from, and
[instruments.md](https://github.com/terranvigil/yah264/blob/main/docs/instruments.md)
records the binaries.

For license reasons the wrapper that runs both encoders in one process lives in
an [ffmpeg fork](https://github.com/terranvigil/FFmpeg) outside this
repository. The command below runs the two-CLI version instead and needs no fork:

```
make parity-status-crf
```

The numbers: our auto count is the smaller of the online core count and 16.
Leave `-fno-tree-vectorize` in and goal 2 reads about 0.73x, which is the
compiler flag talking.

## Read at equal quality instead

Every table above is read at equal bytes. Both encoders stay at `medium`, so
our VMAF margin goes unspent. The fair counter-question is what happens when
the reference gets a slower preset until it reaches our quality, or when we get
a faster one until we reach its. We have not published that comparison for the
board corpus. The one preset-ladder reading we do have is on animation (below).
It moves the answer a long way. Until the same ladder runs on the board
corpus, read the tables above at equal bytes.

## Quality across the rate range

yah264 does best at low bitrates. In the deep band, VMAF-NEG 55 to 83, it beats
x264 on 9 of the 10 clips that reach that band. The median gain there is about
12% [BD-rate](encoding.html#bd-rate), which averages the comparison across a
range of bitrates. The lead fades as bitrate rises and is gone at the top.
Those ten clips come from the twelve-clip quality band, so they are not the ten
of the speed board. The lead depends on resolution as much as on rate. The
twelve-clip HD gate set shows it. Nothing here was ever tuned against that set.

| HD gate set | BD-rate on VMAF-NEG | clips ahead |
|---|--:|--:|
| 720p | -13.9% | 6 of 6 |
| 1080p | +0.4% | 3 of 6 |

## The hardware mode

`--hw videotoolbox` encodes through the Mac's built-in H.264 engine with
yah264's options mapped onto it. Our scene-cut decides its keyframes. The
stream is the hardware's, so nothing above applies to it. It runs the same ten
clips at the same bitrates.

| clip | kbit/s target | wall, hardware ÷ ours | CPU, ours ÷ hardware | VMAF, hardware − ours | our VMAF | hardware VMAF |
|---|--:|--:|--:|--:|--:|--:|
| foreman_cif | 400 | 1.85 | 14 | −0.6 | 93.2 | 92.6 |
| bus_cif | 400 | 1.82 | 12 | −5.2 | 91.5 | 86.4 |
| stefan_cif | 400 | 2.26 | 9 | −3.0 | 88.2 | 85.2 |
| ducks_720p | 25000 | 0.62 | 61 | −8.2 | 89.5 | 81.4 |
| park_joy_720p | 12000 | 0.65 | 36 | −7.2 | 89.9 | 82.7 |
| fourpeople_720p | 1200 | 1.42 | 20 | −3.8 | 89.2 | 85.4 |
| shields_720p | 2200 | 0.99 | 22 | −4.2 | 92.8 | 88.5 |
| sunflower_1080p | 1500 | 0.93 | 20 | −3.0 | 89.9 | 86.9 |
| pedestrian_1080p | 2800 | 0.80 | 35 | −3.8 | 88.0 | 84.2 |
| riverbed_1080p | 12500 | 0.33 | 55 | −13.6 | 92.1 | 78.5 |

Six-second windows, or the whole clip where it is shorter. Our encoder runs at
auto threads. VMAF is the NEG model.

The hardware takes about the same wall time as we do at the median. It burns
about a twentieth of our CPU. It finishes sooner on the high-rate clips and on
most of HD. On CIF and on fourpeople it is slower, because opening the session
is a large part of so short a run. Quality is what pays for the CPU. The
hardware scores below us at the same bitrate on all ten clips, by 4 points at
the median.

The numbers: each cell is the median of five runs per side, taken in
interleaved order on a quiet machine. Wall times spread under 6% within a cell,
except our foreman cell at 13%. CPU spread under 5% on our side and up to 25%
on the hardware's. CPU counts the encoder process only, so it leaves out work
the hardware does elsewhere. The hardware is not byte-stable run to run, but
its VMAF moved by at most 0.6 points across the five runs. Ours repeated to
within about 0.2. The hardware lands within 5% of target on every clip. Ours
runs from 13% under to 10% over, and under target on seven clips. The hardware
spends more bits than we do on eight of the ten, which favors it in the quality
column. Ducks and riverbed are the exceptions.

## Against other encoders

| encoder | quality (VMAF) | pure-C 1-thread | pure-C MT | SIMD MT | size | notes |
|---|--:|--:|--:|--:|--:|---|
| yah264 | +0.22 | 0.92x | **0.84x** | 0.96x | +0.0% | this repo, earlier ten-clip board, before parity |
| x264 | ref | 1.00x | 1.00x | 1.00x | ref | the reference point |
| openh264 | -14.2 | 0.15x | 0.98x | withdrawn | +4.1% | a different design point |

openh264 beats x264 single-threaded because it does far less work per frame.
It pays about 14 VMAF points for that at the same bitrate. Read the speed
and the quality columns as one row. The yah264 row is the earlier ten-clip board. The
openh264 row runs Cisco's encoder over the same ten clips and bitrates at its
own defaults, with frame skipping off so both encoders code every frame. Speed
is wall time against x264 in the same three builds. Quality and size are
against x264 at the same target. Its SIMD MT cell is withdrawn. It read faster than
the column beside it, which cannot be right.

| clip | kbit/s target | wall, openh264 ÷ ours | CPU, ours ÷ openh264 | VMAF, openh264 − ours | our VMAF | openh264 VMAF |
|---|--:|--:|--:|--:|--:|--:|
| foreman_cif | 400 | 0.8 | 13 | −9.2 | 93.9 | 84.7 |
| bus_cif | 400 | 0.8 | 13 | −21.7 | 93.3 | 71.6 |
| stefan_cif | 400 | 0.7 | 12 | −19.1 | 90.1 | 71.0 |
| ducks_720p | 25000 | 1.2 | 10 | −15.2 | 91.9 | 76.7 |
| park_joy_720p | 12000 | 1.1 | 10 | −16.1 | 90.4 | 74.3 |
| fourpeople_720p | 1200 | 0.7 | 16 | −12.2 | 89.8 | 77.6 |
| shields_720p | 2200 | 1.1 | 10 | −15.2 | 94.7 | 79.5 |
| sunflower_1080p | 1500 | 1.1 | 12 | −18.6 | 90.8 | 72.2 |
| pedestrian_1080p | 2800 | 1.3 | 11 | −12.9 | 87.4 | 74.5 |
| riverbed_1080p | 12500 | 1.8 | 7.7 | −14.5 | 88.2 | 73.7 |

Our encoder at auto threads against openh264 with SIMD at twelve threads.

openh264 is built for real-time conferencing: no B-frames, no lookahead, a
light analysis pass. It uses about a tenth of our CPU and scores far below us
at the same bitrate. Its wall time doesn't fall with thread count, because it
threads per slice and these streams have one slice. GPU encoders are
fixed-function silicon with their own trade-offs. They stay out of scope here.

The numbers: openh264 comes in 15.2 VMAF-NEG points below us at the median. Its
wall against ours at auto threads reads 1.09x. The withdrawn cell read 0.47x
against the 0.98x beside it. An earlier five-clip BD-rate with B-frames off on
x264 read +63.7%. It wasn't re-measured for this page. Its
gap against x264 comes from a different measurement again, so the two do not
subtract to the +0.22 in the yah264 row above.

## Across shots

The board clips are single shots. Real content is a chain of them. How an
encoder moves bits between an easy shot and a hard one is invisible above.
Three sequences built from the board clips measure it. Each has five shots with
hard cuts at known frames.
[make_multishot.py](https://github.com/terranvigil/yah264/blob/main/scripts/make_multishot.py)
rebuilds all three byte for byte.

| sequence | yah264 before the fix vs x264 | yah264 now vs x264 | `--shot-crf` vs x264 |
|---|--:|--:|--:|
| five CIF shots (690 frames) | +13.4% | +4.4% | +0.2% |
| five 720p shots (750 frames) | +4.9% | +0.3% | -2.9% |
| five 1080p shots (750 frames) | +7.3% | +0.7% | +0.2% |

BD-rate on VMAF-NEG at auto threads, against x264 medium. Positive means
yah264 spends more bits at equal quality.

Taken one at a time, the same clips are ahead of x264 on two of the three
sizes. So the whole swing came from allocation across shots. Coding efficiency
wasn't the cause. The fix puts the frame-mean part of adaptive quantization on
the frame's base QP, on every input path including streaming. Its strength is
1.3 under uncapped CRF and 1.04 under a VBV cap or a bitrate target. The single-shot board did not move. `--shot-crf` goes further on file input
by sizing each shot against the whole title. Four CRF points fitted with a
cubic is thinner than the five-point sweeps our own rule asks for. Read the
table for the direction and rough size of the effect. Reproduce it with:

```
scripts/multishot_bd.py --arms flat=env:Y264_AQ_DC=0.4 new=env: x264
```

The numbers: each sequence runs 150 frames per shot, except the CIF sequence's
third shot at 90. CIF runs at 30 fps, 720p at 50 and 1080p at 25. The four CRF
points are 22 to 34. One clip at a time the same content reads -5.7% on CIF,
-14% on 720p and +0.4% on 1080p. The single-shot band moved by +0.3% at the
median and +0.9% at worst.

## The corpus

The ten clips of the earlier board are natural video. All of them come from
Xiph's public test collection, 8-bit 4:2:0 at native resolution. The rate is
the ABR target each clip is coded at on the board.

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

The wider corpus adds animation and high-motion sport. Every clip is recorded
with its source and license in
[corpus-sources.md](https://github.com/terranvigil/yah264/blob/main/docs/corpus-sources.md).

The training set and the gate set are separate. The gate set is test-only. Any
fitted coefficient is calibrated on the training half and reported on the gate
half. A number fitted and reported on the same clips is not a measurement.

## Off-corpus content

Content outside that set behaves differently enough that the only summary that
holds is a range. Two clips both fairly called animation are 33 BD-rate points
apart. yah264 is well ahead on 3D CGI and behind on hand-drawn 2D. So we claim no
animation result here as a single number.
[animation-content.md](https://github.com/terranvigil/yah264/blob/main/docs/animation-content.md)
has the measurements and the preset-ladder rows. That spread is also why a
preset-for-preset speed comparison stops meaning much once content leaves the
set the presets were tuned on.

The numbers, read before parity: on 3D CGI we ran 25% ahead of x264 at 1.34x the time. Held at
equal quality there, our `veryfast` reached x264 medium at 1.07x for a fifth
fewer bits. On hand-drawn 2D we ran 8% behind.
