---
title: Results - yah264
description: The goal tables, quality across the rate range, the corpus and how each number reproduces.
---

# Results

yah264 is at full parity with x264. Both encoders code the same source at the
same file size. Every speed figure here is yah264's wall time divided by
x264's. Anything under 1.00 means yah264 finished first. x264 is version
0.165.3222 at `--preset medium`.

Goals 1 and 2 run plain C on both sides. We run our C against the same x264
version built with its assembly off and with `-fno-tree-vectorize` removed.
The compiler then vectorizes x264's C the way it vectorizes ours. Goal 3 runs
what each encoder ships: our NEON against x264's hand-written assembly.

VMAF v1 predicts how good video looks to people and also sees banding, chroma
loss and blocking. We score quality with it at display resolution. On both
sides, a picture smaller than 1920x1080 in both dimensions is scaled up to fit
before scoring. That way a small encode is judged the way it would be watched.
The [VMAF notes](https://github.com/terranvigil/yah264/blob/main/docs/vmaf.md)
have the models and settings.

Every figure in this section comes from the current parity measurement. It was
timed on one Apple M5 Max with 18 cores. A group passes when its median ratio is
at or under 1.00 and its worst cell is at or under 1.15, as long as its quality
holds. All 27 group-and-goal readings that count toward parity pass.

## The test set

| group | what it is |
|---|---|
| S, short clips | twelve standard test clips from CIF to 4K |
| M, 2-minute segments | Chimera, Sol Levante and Meridian at 1080p and 4K. Each starts at its first non-black frame. |
| L, 10-minute excerpts | seven open-licensed films at 720p, 1080p and 4K with end credits cut off: Tears of Steel, Meridian, StEM2, Sintel, Big Buck Bunny, Cosmos Laundromat and Nocturne |
| C, content types | film grain, sports and screen content. Quality counts toward parity. Speed is shown but doesn't count. |

In this section's tables both encoders run as command-line programs. They read
the same 8-bit 4:2:0 frames on stdin from one feeder.

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

A short 1080p clip with SIMD on is the slowest cell anywhere at 0.89.
Content-type cells are shown but sit outside the parity count. The 4K
content-type cell is a single four-second clip. Treat it as a thin reading.

## Quality at the same size

The table gives the VMAF v1 difference as ours minus x264's (median / worst).
It also gives the worst PSNR-Y difference in dB. Positive means yah264 looks
better.

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

We also coded each real title at five rates, at CRF and at capped VBR. Both
encoders ran on six threads with a 40-frame lookahead. The bar is a
[BD-rate](encoding.html#bd-rate) on VMAF v1 no worse than +1.0%. All 52 ladders
pass it. At the median yah264 needs 10% fewer bits than x264 for the same score.
On Meridian at 4K it needs 33% fewer. Four ladders came out behind:

- Big Buck Bunny at 4K VBR, by 0.88%
- Cosmos Laundromat at 1080p CRF, by 0.32%
- Cosmos Laundromat at 1080p VBR, by 0.28%
- Big Buck Bunny at 1080p VBR, by 0.17%

We coded and scored all 52 ladders on a rented Linux arm64 instance. Its
one-thread encodes match this machine's byte for byte. A CRF ladder run on both
machines reads the same. Because a capped-VBR ladder run on both reads about
a point worse on the instance, the VBR figures here are, if anything, on the low
side for yah264. The instance decoded its source frames in software. On 30 ladders it
coded its own decode of the title instead of this machine's frames.

We also swept our lookahead over 30, 40 and 50 frames against x264's default of
40. That sweep was scored with VMAF-NEG (earlier metric). Most ladders moved
less than the 0.4-point run-to-run spread. Some did best at 50 in at least one
mode: Big Buck Bunny at 1080p and 4K, Chimera at 4K, Meridian at 1080p and 4K,
and Sol Levante at 4K. Sintel at 4K and Sol Levante at 1080p did best at 40 in
at least one mode. Every other figure on this page uses 40.

In capped VBR every rung of every ladder came within 5% of its target.
Goal 3 read between 0.45 and 0.67 on every VBR cell.

## By processor

We read the three goals per processor and instruction set. The Apple row is the
parity table above. The other rows come from rented cloud machines and use
fewer clips. We report them apart from the Apple row. The
[architectures](architectures.md) page says what each tier is.

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

The Apple row gives each goal's range of group medians.

On the Graviton4 the worst valid cells read 0.66, 0.99 and 0.75. We compiled
x264's plain-C build there with gcc. Because its clang build ran 12% to 18% faster
on the three cells we checked, goals 1 and 2 flatter us by about that much on
that machine.

The x86-64 and Graviton3 figures come from a profiling run instead of the
parity method. Each is one encode at one thread with SIMD on and CRF 23 on both
encoders. The clips are 60 frames of sunflower at 1080p and 20 of crowd_run at
4K. The number is CPU time against x264's.

On the AMD machine yah264 retires 0.88 and 0.83 times x264's instructions with
AVX2. The AVX-512 kernels bring that to 0.82 and 0.78. The Intel machine is a
Xeon Platinum 8488C with 16 cores and two threads per core. Because it ran the
same binary, its instruction counts are the same. At the same bitrate on
sunflower the AMD machine retires 0.94 times x264's instructions but uses 1.02
times its CPU time. On the Graviton3 yah264 retires 0.67 and 0.62 times x264's
instructions. Output is byte-identical to the Mac's and conformance passes on all three
machines.

At the default thread count the AMD machine reads differently. On a
single 60-frame run, sunflower took 0.76 s against x264's 0.53 s and used more
CPU time. crowd_run took 1.17 s against 1.14 s. That run is short and includes
tool start-up.

The Intel machine gave the first clean all-thread reading on x86-64. Each
encoder ran at its default thread count over the full 500-frame clips while both
read one piped feed. The table gives the median of five runs. It's a profile and
not goal 3, because both encoders ran at CRF 23 instead of at the same file
size. yah264's files were smaller: 0.77 and 0.94 times x264's bytes.

| clip | wall vs x264 | CPU time vs x264 | yah264 CPU ms per frame | x264 CPU ms per frame |
|---|--:|--:|--:|--:|
| sunflower 1080p, AVX2 | 1.04 | 1.01 | 102.4 | 101.3 |
| sunflower 1080p, AVX-512 | 1.04 | 0.98 | | |
| crowd_run 4K, AVX2 | 0.90 | 0.76 | 541 | 714 |
| crowd_run 4K, AVX-512 | 0.87 | 0.72 | | |

On sunflower we're level on CPU time and 4% behind on wall time. At 4K we're
ahead on both. Threading at high core counts is the open x86 item. We're
sizing it on Intel now. None of the three goals has been read on x86-64 yet. A
goal board on a rented AMD machine will fill them in with the same method as the
Apple row.

### Estimated cost per frame

yah264's CPU time per frame times the price of a vCPU-hour gives a rough cost
per frame on each rented machine. On sunflower 1080p with every thread busy,
Graviton4 comes out cheapest on both price bases:

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
Graviton4's comes from the parity run's full-thread rows on a c8g.24xlarge.
AMD's ratios say "about" because its CPU time comes from the short 60-frame run.
Only Intel's is the reading above. The on-demand prices are AWS's published
rates and weren't fetched live. A measure per unit of CPU time absorbs the fact
that Intel's vCPUs are hyperthreads. For x264 the order differs: at spot, AMD is
cheapest.

## The ten-clip board

The sections below use a ten-clip board of Xiph clips from CIF to 1080p that was
timed on the same machine.

<div class="aside">
<p class="aside-title">Four words this page uses a lot</p>
<p><b>Board.</b> One timed run of both encoders over a fixed set of clips. This section reads the ten-clip board.</p>
<p><b>Band.</b> The clips and rates a quality sweep is read over, chosen so that x264 scores between VMAF v1 88 and 94 on each clip.</p>
<p><b>Leg.</b> One metric of a goal, read on its own.</p>
<p><b>Bar.</b> The number a leg has to beat.</p>
</div>

### Reading the tables

Every figure here comes from one Apple M5 Max with 18 cores. Each cell is the median of three
interleaved samples. A board repeated on the same machine still moves its
ratios. Small differences in these tables aren't readings. A goal is four
metrics. A fifth sits underneath them as a floor.

| metric | bar |
|---|---|
| median speed | 1.00x or faster |
| worst-clip speed | 1.15x or better |
| quality | within 0.5 [VMAF v1](https://en.wikipedia.org/wiki/Video_Multimethod_Assessment_Fusion) |
| compression | within 1.0% size |
| pixel accuracy (floor) | no clip more than 1.0 dB below x264 in PSNR-Y at the same bytes |

The size column shows what the bitrate solve left over, as a check that the
solve converged. The real compression number is the
[BD-rate](encoding.html#bd-rate) further down the page. A clip 0.5 dB below
x264 joins the debt list, though the build still passes. We make quality
decisions on VMAF.

A cell is dropped when its own samples spread over 1.15x. A repeated board moves
a ratio by up to about 0.10. Differences under 0.05 aren't readings. The solve
leaves about 0.3% residual. The two rates are matched within 0.5%.

At CRF and the same size, yah264 scores a median 1.3 VMAF v1 points above x264
on the ten clips at one thread, and 1.2 at three threads. Every clip is ahead,
by at least 0.75. At one thread PSNR-Y has a median of +0.03 dB against x264.
The worst clip is stefan_cif at -0.51 dB, which puts it on the debt list. The
floor passes.

## CRF versus ABR

CRF asks an encoder for a quality level and takes whatever bitrate that needs.
ABR asks for a bitrate and takes whatever quality that buys. No goal is set
against the ABR table.

The table below is ABR at matched achieved bitrate on the ten-clip board. The
tables at the top of this page cover capped VBR.

| goal | configuration | median | max | VMAF v1 | size |
|---|---|--:|--:|--:|--:|
| 1 | pure C, single-threaded | 0.66x | 0.74x | +1.73 | +0.2% |
| 2 | pure C, multi-threaded | 0.69x | 0.85x | +1.93 | -0.1% |
| 3 | as-shipped SIMD, multi-threaded | 0.86x | 0.99x | +1.93 | -0.1% |

The VMAF v1 and size columns are medians over the ten clips. Goals 2 and 3 code
the same stream, read at three threads on both encoders. The speed columns come
from the timed board, at one thread for goal 1 and auto threads for goals 2
and 3.

x264's target is solved to hit the bitrate we achieved, as the size column
shows. The rate controller's opening is fitted. A frame can also start against
a reference that's still being coded. Single-threaded, ABR costs us nothing over
CRF. Multi-threaded it costs up to about a tenth of a ratio point. The
[rate-control notes](https://github.com/terranvigil/yah264/blob/main/docs/rate-control.md)
explain how both changes work.

By resolution class, the median ratio over three CIF, four 720p and three 1080p
clips:

| goal | CRF CIF | CRF 720p | CRF 1080p | ABR CIF | ABR 720p | ABR 1080p |
|---|--:|--:|--:|--:|--:|--:|
| 1 | 0.68x | 0.65x | 0.58x | 0.71x | 0.66x | 0.59x |
| 2 | 0.66x | 0.64x | 0.63x | 0.82x | 0.65x | 0.69x |
| 3 | 0.77x | 0.79x | 0.77x | 0.94x | 0.83x | 0.83x |

The slowest cells are the CIF clips and fourpeople, the low-bitrate 720p clip.
Under ABR with SIMD, riverbed joins them.

Over the ten board clips our median absolute rate error is 6.6% multi-threaded
and 3.8% single-threaded. x264's is 5.6% and 4.0%. We fit the opening on
seventeen non-board cells and hold the board out.

## Reproducing the tables

The parity tables at the top come from `scripts/longform_throughput.py`. It
runs both command-line encoders on stdin from one feeder. The ten-clip board
and the sections built on it run both encoders as libraries inside a single
ffmpeg process instead.

Each encode hands the same thread count to both. Goal 1 fixes that count at
one on each side. Goals 2 and 3 pass 0 for auto and let each encoder resolve its
own number, because auto is how each one ships.

The pure C rows need one adjustment to be fair. x264's configure adds
`-fno-tree-vectorize` unconditionally. That leaves a stock build's C without
the auto-vectorization ours gets. We build x264 with `--disable-asm`, strip that
flag from its `config.mak` and rebuild. The compiler then treats both sides the
same. The runtime switch can't do it instead, because the flag was applied when
the binary was compiled. Each number on this page says which board it came
from. The
[instruments catalog](https://github.com/terranvigil/yah264/blob/main/docs/instruments.md)
records the binaries.

For license reasons the wrapper that runs both encoders in one process lives in
an [ffmpeg fork](https://github.com/terranvigil/FFmpeg) outside this
repository. The command below runs the two-CLI version instead and needs no fork:

```
make parity-status-crf
```

Our auto count is the smaller of the online core count and 16. Leave
`-fno-tree-vectorize` in and the compiler flag alone pushes goal 2 to about
0.73x.

## Read at equal quality instead

All the tables above are read at equal bytes. Because both encoders stay at
`medium`, our VMAF margin goes unspent. The fair counter-question is what
happens when the reference gets a slower preset until it reaches our quality,
or when we get a faster one until we reach its quality. We haven't published
that comparison.

## Quality across the rate range

In the deep band, from VMAF v1 37 to 66, twelve of the thirteen quality-band
clips reach the band, and yah264 beats x264 on all twelve. The median gain there
is about 17% in [BD-rate](encoding.html#bd-rate). BD-rate averages the
comparison across a range of bitrates. Each clip runs 120 frames at one thread,
with both encoders solved onto the same three or four rates per clip. The quality
band overlaps the speed board on only six clips.

yah264 is also ahead on every clip of the twelve-clip HD gate set, more so at
720p. Nothing here was ever tuned against that set.

| HD gate set | median BD-rate on VMAF v1 | clips ahead |
|---|--:|--:|
| 720p | -19.3% | 6 of 6 |
| 1080p | -12.5% | 6 of 6 |

Each clip runs 150 frames with both encoders at two threads, over five CRF
points three apart. The lowest CRF is where x264 scores VMAF v1 93 on that clip.
crowd_run has the smallest lead, at -6.1%.

## The hardware mode

`--hw videotoolbox` encodes through the Mac's built-in H.264 engine with
yah264's options mapped onto it. Our scene-cut decides its keyframes. Nothing
above applies to it, because the stream is the hardware's. The table runs it on
the same ten clips at the same bitrates.

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

Each run covers a six-second window, or the whole clip where it's shorter. The
wall and CPU columns come from timed runs with our encoder at auto threads. The
quality columns are VMAF v1 with our encoder at three threads.

At the median the hardware takes about the same wall time as we do and uses
about an eighteenth of our CPU. It finishes sooner on the high-rate clips and
on most of HD. On CIF and on fourpeople it's slower, because opening the session
is a large part of so short a run. The CPU saving costs quality. The hardware
scores below us at the same bitrate on all ten clips, by 7.8 points at the
median.

Each cell is the median of five runs per side in interleaved order on a quiet
machine. Wall times spread under 10% within a cell, except foreman at 17% on
both sides. Because CPU counts the encoder process only, it leaves out
work the hardware does elsewhere. Both encoders come within about 10% of target on eight
clips. On bus and stefan every encoder on this page undershoots the 400 kbit/s
target by a wide margin. On seven of the ten clips the hardware spends more
bits than we do. Foreman, ducks and riverbed are the exceptions. The extra bits
favor the hardware in the quality column.

## Against other encoders

| encoder | quality (VMAF v1) | pure-C 1-thread | pure-C MT | SIMD MT | size | notes |
|---|--:|--:|--:|--:|--:|---|
| yah264 | +1.2 | 0.65x | **0.66x** | **0.77x** | +0.1% | this repo, ten-clip board at CRF |
| x264 | ref | 1.00x | 1.00x | 1.00x | ref | the reference point |
| openh264 | -12.3 | **0.15x** | 1.01x | 0.94x | +3.3% | a different design point |

openh264 beats x264 single-threaded because it does far less work per frame.
It pays about 12 VMAF v1 points for that at the same bitrate. Read the speed
and the quality columns as one row. The yah264 row is the ten-clip board. The
openh264 row runs Cisco's encoder over the same ten clips and bitrates at its
own defaults. Frame skipping is off so both encoders code every frame. Because
the row is timed with the command-line encoders, its speed is wall time against
the x264 command-line tool in the same three builds. Quality and size are
medians against x264 at the same target, with all three encoders at three
threads.

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

The wall and CPU columns ran our encoder at auto threads against openh264 with
SIMD at twelve threads. The quality columns ran both at three threads.

openh264 is built for real-time conferencing: no B-frames, no lookahead, a
light analysis pass. It uses about a seventh of our CPU and scores far below us
at the same bitrate. Its wall time doesn't fall with thread count, because it
threads per slice and these streams have one slice. We leave GPU encoders out of
scope here. They're fixed-function silicon with their own trade-offs.

openh264 comes in 14.6 VMAF v1 points below us at the median. Its
wall against ours at auto threads reads 1.68x.

## Across shots

The board clips are single shots, while real content is a chain of them. The
tables above can't show how an encoder moves bits between an easy shot and a
hard one. Three sequences built from the board clips measure that. Each has
five shots with hard cuts at known frames.
[make_multishot.py](https://github.com/terranvigil/yah264/blob/main/scripts/make_multishot.py)
rebuilds all three byte for byte.

| sequence | yah264 vs x264 | `--shot-crf` vs x264 |
|---|--:|--:|
| five CIF shots over 690 frames | -12.3% | -22.8% |
| five 720p shots over 750 frames | -12.8% | -19.5% |
| five 1080p shots over 750 frames | -8.4% | -16.7% |

The figures are BD-rate on VMAF v1 against x264 medium at two threads on both
encoders. Negative means yah264 spends fewer bits at equal quality.

On every input path, streaming included, yah264 puts the frame-mean part of
adaptive quantization on the frame's base QP. Its strength is 1.3 under
uncapped CRF and 1.04 under a VBV cap or a bitrate target. `--shot-crf` goes
further on file input by sizing each shot against the whole title. The BD-rate
fits a cubic to five CRF points. Reproduce the CIF and 720p rows with:

```
scripts/multishot_bd.py --threads 2 --points 22,25,28,31,34 --seqs ms_cif_30,ms_720p_50 \
  --arms new=env: \
  'shot=build/cli/y264 --input-y4m {src} --crf {q} --shot-crf --threads 2 -o {out}' \
  'x264=x264 --preset medium --crf {q} --threads 2 --demuxer y4m -o {out} {src}'
```

The 1080p row is the same command with `--seqs ms_1080p_25 --points 26,29,32,35,38`.

Each sequence runs 150 frames per shot, except the CIF sequence's third shot at
90. CIF runs at 30 fps, 720p at 50 and 1080p at 25. The CRF points are 22 to 34,
and 26 to 38 at 1080p.

Coded one clip at a time, the same content reads a median -14% on CIF, -14% on
720p and -12% on 1080p.

## The corpus

The ten board clips are natural video from Xiph's public test collection. They're
8-bit 4:2:0 at native resolution. The rate column is the ABR target each clip is
coded at on the board. The rates were chosen on the earlier metric and are kept
so the speed columns stay comparable.

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

The wider corpus adds animation and high-motion sport. The
[corpus sources](https://github.com/terranvigil/yah264/blob/main/docs/corpus-sources.md)
list every clip with its source and license.

The gate set is test-only and separate from the training set. We calibrate any
fitted coefficient on the training half and report it on the gate half. A
number fitted and reported on the same clips isn't a measurement.

## Off-corpus content

Content outside that set behaves differently enough that the only summary that
holds is a range. Because two clips both fairly called animation are 39 BD-rate
points apart, we claim no animation result here as a single number. The
[animation notes](https://github.com/terranvigil/yah264/blob/main/docs/animation.md)
have the measurements. That spread is also why a preset-for-preset speed
comparison stops meaning much once content leaves the set the presets were
tuned on.

On a 180-frame window of the 3D CGI Big Buck Bunny, yah264 needs 37% fewer bits
than x264 medium on VMAF v1. On 140 frames of the hand-drawn Sita Sings the Blues
it needs 2% more.

On the 10-minute Big Buck Bunny excerpt in the parity tables, yah264 needs 1% to
2% fewer bits than x264 at CRF at 720p, 1080p and 4K. At capped VBR the two
encoders are within 1% of each other.
