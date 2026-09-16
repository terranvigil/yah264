---
title: Results - yah264
description: The goal tables, the quality maps, the corpus, and how each number reproduces.
---

# Results

Every figure on this page was taken on one machine: an Apple M5 Max, 18 cores
(6 performance and 12 efficiency), macOS 26 (Darwin 25.6). The multi-threaded
rows are wall-clock ratios at that core count and do not carry unchanged to a
different one.

## Reading the tables

The tables show how fast yah264 is compared to x264. Both encode the same clip
at the same quality. 1.0 means a tie. Lower is faster.

Each cell is the median of three interleaved samples, and a cell whose own
samples spread by more than 1.15x prints a warning and is not read. Repeating a
whole board on the same machine still moves a ratio by up to about 0.10, and
another machine moves it further. So differences under 0.05 in the tables below
are not readings. That is also why the two open worst-clip legs, 1.15x and
1.16x against a bar of under 1.15x, are described as open rather than as
decided either way.

The goal is made up of four metrics, with a fifth reading underneath them as a
floor:

| metric | bar |
|---|---|
| median speed | 1.00x or faster |
| worst-clip speed | under 1.15x |
| quality | within 0.5 [VMAF](https://en.wikipedia.org/wiki/Video_Multimethod_Assessment_Fusion) |
| compression | within 1.0% size |
| pixel accuracy (floor) | no clip more than 1.0 dB below x264 in PSNR-Y at the same bytes |

The compression row is not a thing to optimise, and not really a measurement
either. Every row of the tables below is solved onto a matched achieved
bitrate before either encoder is timed, so the size column reads the solve's
residual, inside about 0.3% by construction, rather than a compression
result. Read it as a check that the solve converged. The compression result is
BD-rate, further down the page.

The last row was added on 2026-09-14 and it is not a fifth thing to optimise.
VMAF is a model of what a viewer notices; PSNR measures how far each pixel
actually moved. The two can point in opposite directions. The sibling H.265
encoder ran both over fifteen clips and they disagreed on nine of them, while
the level gap at the same bytes stayed small. That is the shape in which a
change buys VMAF by quietly spending pixel accuracy and no column notices. So
PSNR-Y gets a floor instead of a target. The bar sits well outside anything
measured so far, and a clip 0.5 dB below x264 is written down as debt rather
than gated. Quality decisions are still made on VMAF.

Current read (CRF at matched bitrate, ten clips, pure C, 2026-09-14): median
-0.07 dB, worst clip bus_cif at -0.47 dB. The floor passes and the debt list is
empty.

One more thing to keep in mind. The rates are matched to within 0.5% of each
other rather than exactly, so tiny quality differences - hundredths of a VMAF
point - are smaller than the test can measure, and the gaps between the three
rows' VMAF columns are inside that.

## Why the rate control mode changes the answer

You can ask an encoder for a quality level and let the bitrate land where it
lands. That's CRF, and it's the main table. Or you can ask for a bitrate and let
quality land where it lands. That's ABR, in the second table. The two answer
different questions so both are here.

The main table is CRF. It's solved per clip so it lands on the same bitrate
as x264.

**CRF, matched achieved bitrate**, ten clips (three CIF, four 720p, three
1080p). fourpeople_720p holds the slot samsung_720p used to: samsung was vendor
material with no licence, fourpeople is Xiph derf and fetchable, and it reads
the same ratio in that slot. It took samsung's operating point of 1200 kbit/s
rather than its own calibrated 1600, so the row stays comparable with the
pre-swap board.

| goal | configuration | median | max | VMAF | size | status |
|---|---|--:|--:|--:|--:|---|
| 1 | pure C, single-threaded | **0.92x** | 1.15x | +0.26 | −0.1% | worst clip on the bar, so the leg is open |
| 2 | pure C, multi-threaded | **0.84x** | 1.06x | +0.20 | +0.1% | speed and quality legs pass |
| 3 | as-shipped SIMD, multi-threaded | **0.96x** | 1.16x | +0.22 | +0.0% | worst clip past the bar by 0.01 |

The worst clip on every row is the same one, low-bitrate 1080p (sunflower at
1.5 Mbit/s), with shields at 2.3 Mbit/s next; the high-bitrate 1080p rows are
the fastest cells on the board.

Both worst-clip legs are open. The single-threaded pure C row reads 1.15x,
which a bar written as "under 1.15x" does not admit, and the shipped build's
reads 1.16x on this board (1.15x on two reads of the previous board). Those
hundredths are inside the board's own run-to-run spread, so neither row is
settled in either direction, and neither is the gap between 0.92x and 0.96x.
The hundredths are also not the clip swap: fourpeople reads 1.04x in samsung's
slot, the same as samsung did.

The wall ratio is only half of a speed reading. The board prints a
CPU-seconds ratio beside it, and that column is what says whether a wall ratio
near 1.00 is efficiency or occupancy: being level while burning half again
the CPU is a lead that goes the moment the reference threads better. We have
not published that column beside these medians yet; the one occupancy figure
that is published is foreman_cif's, on the [design page](design.html).

Before the 1080p clips joined, this table was taken on six clips with no 1080p
in it and read 0.95x / 0.85x / 0.96x. The full per-clip tables are kept in our
local board notes.

**ABR, matched achieved bitrate. Superseded, kept for the record.** This table
was taken after the rate controller's opening was refitted (see below), and it
still has samsung in the 720p slot, so it is not the current board. The
multi-threaded tiers were re-read three times with fourpeople and every read
came back with the board's own "box loaded" warning on several cells, so those
re-reads are not quoted. The single-threaded tier did reproduce: median 0.93x,
worst clip 1.15x against this table's 1.14x, which is a hundredth and inside
the spread; fourpeople's cell read 0.85x there. Nothing below is decided on
this table, and a quiet re-read replaces it.

| goal | configuration | median | max | VMAF | size |
|---|---|--:|--:|--:|--:|
| 1 | pure C, single-threaded | 0.93x | 1.14x | +0.34 | −0.1% |
| 2 | pure C, multi-threaded | 0.90x | 1.08x | +1.23 | +0.1% |
| 3 | as-shipped SIMD, multi-threaded | 1.07x | 1.23x | +1.25 | +0.1% |

Here x264's target is solved so it lands on the bitrate we achieved, which is
what the size column shows. No goal is set against this table, but it is now a
speed reading rather than a bit-spending contest. Single-threaded, ABR costs us
nothing over CRF. Multi-threaded, reading it against the CRF table above, it
costs 0.06 on goal 2 and 0.11 on goal 3, down from 0.3 to 0.4: the rate-control
decide was allowed to run one burst ahead, and then a staircase device that
lets the next frame start against a reference still being coded, which rate
control had been refusing, was allowed under that lag. That second change alone
took the multi-threaded rows from 1.06x and 1.26x to 0.94x and 1.11x. Before
those changes this table handed both encoders the same target and let the sizes
differ by about 3%, which made it unreadable as a speed number.

The rate controller's opening was refitted at the same time. Its cumulative rate
factor never forgets the first second, and the old resolution-only seed opened
the high-bitrate cells near QP 6 to 17 against operating points of 27 to 34
(and samsung, then still on the board, five QP too high), which is where the earlier −14% / +18% rate
misses came from. The opening is now fitted at the first decide on the
lookahead window's inter cost and the target bits per macroblock (fit on
seventeen non-board cells, the board held out). On the ten board clips at
their ABR rates the median absolute rate error went from 12.5% to 6.6%
multi-threaded (x264: 5.6%) and from 7.5% to 3.8% single-threaded (x264:
4.0%), with a 2% median BD-rate gain at matched rate. The table above is the
first board after that change; it moved the ratios by 0.01 to 0.04.

**By resolution class**, median ratio on the same two boards (three CIF, four
720p, three 1080p clips). The ABR columns come from the superseded table above
and carry its caveat:

| goal | CRF CIF | CRF 720p | CRF 1080p | ABR CIF | ABR 720p | ABR 1080p |
|---|--:|--:|--:|--:|--:|--:|
| 1 | 0.93x | 0.90x | 1.07x | 0.90x | 0.90x | 1.09x |
| 2 | 0.78x | 0.86x | 1.00x | 0.90x | 0.87x | 1.04x |
| 3 | 0.92x | 0.99x | 1.10x | 1.07x | 1.02x | 1.18x |

Resolution is not what orders these rows. Bitrate is: the slow cells are the
low-bitrate HD ones and the high-bitrate 1080p cells are the fastest on the
board. The 1080p column reads high because two of its three clips are
low-bitrate.

## The three speed goals

Goal 2 passes its speed and quality legs. Goals 1 and 3 have their worst clips
on and just past the 1.15x bar, and both readings are inside the board's own
spread, so both are left open.

## How to reproduce them

Both encoders run inside a single ffmpeg process, as libraries, and the same
thread count is handed to both on every encode. Goal 1 pins that count to 1 on
both sides. Goals 2 and 3 pass 0, which means auto, and each encoder then
resolves its own number: yah264 takes the smaller of the online core count and
16, x264 uses its own rule. That is deliberate, auto is how each ships, but
it does mean the two rows are not running the same number of workers, and the
CPU-seconds ratio rather than the wall ratio is what tells you when that
matters.

The pure-C rows need one adjustment to be fair, and it means those rows are not
run against a stock x264. Its configure adds `-fno-tree-vectorize`
unconditionally, so a stock build's C is not auto-vectorised while ours is. We
build x264 with `--disable-asm`, strip that flag from its `config.mak`, and
rebuild, so the compiler treats both sides the same. Leave the flag in and goal
2 reads about 0.73x, which measures a compiler flag rather than an encoder. The
runtime switch is not a substitute: the flag was applied when the binary was
compiled.

Three harnesses here reach for x264 and they do not all reach for the same
build: the in-process board loads whichever libx264 it is pointed at, the CLI
board builds from a source checkout with `--disable-lavf --disable-ffms
--disable-avs --disable-swscale`, and the multi-shot gate uses that checkout's
CLI. Any number quoted from this page should say which board it came from;
`docs/instruments.md` records the binaries.

The wrapper that runs both encoders in one process lives in an ffmpeg fork, not
in this repository, for licence reasons. `make parity-status-crf` runs the
two-CLI version instead and needs no fork.

## Read at equal quality instead

Every table above is read at equal bytes, with both encoders pinned to
`medium`, which leaves our +0.22 VMAF unspent. The fair counter-question is
what happens if the reference is given a slower preset until it reaches our
quality, or if we are given a faster one until we reach its. We have not
published that comparison for the board corpus; the margin is quoted at
`--preset medium` only. The one preset-ladder reading we do have is on
animation, below, and it moves the answer a long way: on 3D CGI our `veryfast`
reaches x264 medium's quality at 1.07x the time for a fifth fewer bits, where
the same clip preset-for-preset reads 1.34x. Until the same ladder is run on
the board corpus, read the tables above as equal-bytes readings and not as an
equal-quality claim.

## Quality across the rate range

Quality is scored with VMAF, which predicts how good video subjectively looks to
people. We compare both encoders at the same file size.

yah264 does best at low bitrates: in the deep band, VMAF-NEG 55 to 83, it beats
x264 on 9 of the 10 clips that reach that band, by a median of about 12%
BD-rate on VMAF-NEG. BD-rate is an average across a range of bitrates rather
than a single one. The lead fades as bitrate rises and is gone at the top.

Two things that number is not. The ten clips there are the ones from the
twelve-clip quality band that span the deep band, not the ten-clip speed board
used everywhere else on this page: same count, different clips. And the lead
is resolution-dependent as well as rate-dependent: on the twelve HD clips that
nothing here was ever tuned against, the 720p median is -13.9% with all six
ahead, while the 1080p median is +0.4% with three ahead and three behind. Any
compression claim from this project should say which bitrate range, which
corpus and which resolution it came from.

And file size is half the story. It says nothing about encoding time. Every
quality number here sits next to a speed number.

## The hardware mode

`--hw videotoolbox` encodes through the Mac's fixed-function H.264 engine
with yah264's options mapped onto it and our scene-cut driving its keyframes.
The stream is the hardware's, not ours, so nothing above applies to it; this
is its own row, measured on the same ten clips at the same bitrates
on an Apple M-series machine. VMAF is the NEG variant.

| clip | kbit/s target | yah264 wall | yah264 CPU | hardware wall | hardware CPU | yah264 VMAF | hardware VMAF |
|---|--:|--:|--:|--:|--:|--:|--:|
| foreman_cif | 400 | 0.11 s | 0.81 s | 0.16 s | 0.04 s | 93.9 | 93.3 |
| bus_cif | 400 | 0.11 | 0.68 | 0.15 | 0.04 | 93.3 | 86.5 |
| stefan_cif | 400 | 0.07 | 0.39 | 0.12 | 0.03 | 90.1 | 86.0 |
| ducks_720p | 25000 | 1.46 | 15.25 | 0.53 | 0.25 | 91.9 | 84.9 |
| park_joy_720p | 12000 | 1.16 | 11.85 | 0.43 | 0.27 | 90.4 | 82.6 |
| fourpeople_720p | 1200 | 0.57 | 6.04 | 0.50 | 0.25 | 89.8 | 85.6 |
| shields_720p | 2200 | 0.69 | 7.25 | 0.42 | 0.27 | 94.7 | 88.6 |
| sunflower_1080p | 1500 | 0.68 | 8.41 | 0.42 | 0.28 | 90.8 | 87.5 |
| pedestrian_1080p | 2800 | 0.79 | 10.45 | 0.41 | 0.22 | 87.4 | 85.1 |
| riverbed_1080p | 12500 | 1.34 | 16.44 | 0.42 | 0.23 | 88.2 | 78.7 |

Six-second windows, our encoder at auto threads. The hardware uses 13 to 72
times less CPU and is 1.1 to 3.2 times faster in wall time on HD, reaching 2x
or better on three of the seven HD clips (it is slower on CIF, where the
session's setup is most of the run). It lands 2.3 to 9.5 VMAF points below our
encoder at the same bitrate on nine of the ten clips and is level on foreman;
the largest gap is riverbed's 9.5. The quality columns are a second read on the current
build after the rate-control opening was refitted (our riverbed figure rose
from 82.8 to 88.2 with it); the timing columns are the earlier read. Our own
columns here are a different read from the openh264 table further down, which
covers the same clips at the same rates: ducks reads 1.46 s of wall here and
1.31 s there, riverbed 1.34 s against 1.22 s. That spread, up to about 11% on a
cell, is the run-to-run figure this page opens with, shown rather than
described. The hardware is not byte-stable run to run and its quality
moves with it: riverbed read 83.0 on one run and 78.7 on the next at
the same size. Sizes are within a few percent of target on both sides; the
full per-clip figures are in our local records. VMAF is the v0.6.1 NEG model
throughout this page; x264 is 0.165.3222 (b35605a, Homebrew) at `--preset medium`, its stock build for
the SIMD rows and a build with assembly off and the compiler's vectoriser
left on for the pure C rows.

## Against other encoders

| encoder | pure-C 1-thread | pure-C MT | SIMD MT | quality (VMAF) | size | notes |
|---|--:|--:|--:|--:|--:|---|
| yah264 | 0.92x | **0.84x** | 0.96x | +0.22 | +0.0% | this repo, ten-clip board |
| x264 | 1.00x | 1.00x | 1.00x | ref | ref | the reference point |
| openh264 | 0.15x | 0.98x | withdrawn | -14.2 | +4.1% | same ten clips and bitrates; a different design point |

The yah264 row is the goal table above (CRF, matched achieved bitrate). The
openh264 row is a different measurement: Cisco's encoder driven through a thin
adapter at the same ten clips and bitrates as the hardware row, six-second
windows, frame skipping off (its default under rate pressure, which would
compare videos of different lengths), each encoder at its own defaults. The
speed columns are wall time against x264 in the same three configurations; the
quality and size columns are against x264 at the same target.

openh264's SIMD-MT cell is withdrawn rather than quoted, because it did not
survive a check against its own neighbour. openh264 has no scalar build here
and its wall barely moves with thread count, so across those three columns it
is close to one constant divided by three different x264 times. Turning x264's
assembly on makes the denominator smaller, so that cell has to read *higher*
than the pure-C MT cell beside it, not lower. It read 0.47x against 0.98x,
which would put x264 with assembly slower than x264 without it at the same
thread count, and that cannot be right. The cell is out until it is re-measured.

Per clip, our encoder at auto threads against openh264 with SIMD at twelve
threads, VMAF-NEG:

| clip | kbit/s target | yah264 wall | yah264 CPU | openh264 wall | openh264 CPU | yah264 VMAF | openh264 VMAF |
|---|--:|--:|--:|--:|--:|--:|--:|
| foreman_cif | 400 | 0.10 s | 0.78 s | 0.08 s | 0.06 s | 93.9 | 84.7 |
| bus_cif | 400 | 0.09 s | 0.66 s | 0.07 s | 0.05 s | 93.3 | 71.6 |
| stefan_cif | 400 | 0.06 s | 0.37 s | 0.04 s | 0.03 s | 90.1 | 71.0 |
| ducks_720p | 25000 | 1.31 s | 15.35 s | 1.55 s | 1.53 s | 91.9 | 76.7 |
| park_joy_720p | 12000 | 1.07 s | 12.01 s | 1.19 s | 1.17 s | 90.4 | 74.3 |
| fourpeople_720p | 1200 | 0.57 s | 6.02 s | 0.39 s | 0.38 s | 89.8 | 77.6 |
| shields_720p | 2200 | 0.65 s | 7.24 s | 0.71 s | 0.69 s | 94.7 | 79.5 |
| sunflower_1080p | 1500 | 0.64 s | 8.30 s | 0.69 s | 0.67 s | 90.8 | 72.2 |
| pedestrian_1080p | 2800 | 0.75 s | 10.46 s | 0.97 s | 0.95 s | 87.4 | 74.5 |
| riverbed_1080p | 12500 | 1.22 s | 16.64 s | 2.18 s | 2.16 s | 88.2 | 73.7 |

openh264 is a different design point (real-time and conferencing: no B-frames,
no lookahead, a light analysis) and the row has to be read as one. It uses
about a tenth of our CPU (median 0.09x) and lands 15.2 VMAF-NEG points
below us at the same bitrate (9 to 22 per clip), and 14.2 below x264. Those
last two are from different measurements and do not subtract to the +0.22 in
the board table above, which is a matched-bytes reading on a different run. Its
wall time is 0.15x of x264's single-threaded and level with x264 multi-threaded
in pure C: its threading is per slice and these streams are single-slice, so
its wall does not fall with threads here while x264's and ours do; against our
encoder at auto threads its wall reads 1.09x. The
quality-normalised number is BD-rate, where an earlier five-clip measurement
with B-frames off on x264 read +63.7%; that figure is in our local records
and is not re-measured here.

GPU-vendor encoders are fixed-function silicon with different quality and
latency trade-offs, so they stay out of scope here.

## Across shots

The board clips are single shots. Real content is a chain of them, and how an
encoder moves bits between an easy shot and a hard one is invisible above.
Three sequences built from the board clips with hard cuts at known frames (five
shots each, 150 frames per shot except the CIF sequence's third, which is 90;
CIF at 30 fps, 720p at 50, 1080p at 25; `scripts/make_multishot.py` rebuilds
them byte for byte) measure it: BD-rate on VMAF-NEG over four CRF points, 22 to
34, at auto threads, with x264 medium as the control. Four points fitted with a
cubic is an exact interpolation, which is thinner than the five-point sweeps
this project's own gate rule asks for, so read these as the shape of the effect
rather than to the decimal.

| sequence | yah264 before the fix vs x264 | yah264 now vs x264 | `--shot-crf` vs x264 |
|---|--:|--:|--:|
| five CIF shots (690 frames) | +13.4% | +4.4% | +0.2% |
| five 720p shots (750 frames) | +4.9% | +0.3% | -2.9% |
| five 1080p shots (750 frames) | +7.3% | +0.7% | +0.2% |

Positive means yah264 spends more bits than x264 at equal quality. The same
clips taken one at a time read -5.7% (CIF), -14% (720p) and +0.4% (1080p), so
the whole swing was allocation across shots, not coding efficiency. The fix
puts the frame-mean part of adaptive quantisation on the frame's base QP at
full strength, for every input path including streaming; the single-shot
tables above did not move (band median +0.3%, worst +0.9%). `--shot-crf`
goes further on file input by sizing each shot against the whole title.
Reproduce: `scripts/multishot_bd.py --arms flat=env:Y264_AQ_DC=0.4 new=env: x264`.

## The corpus

The clips in the goal tables are natural video, three CIF, four 720p and three 1080p. The
wider corpus adds animation and high-motion sport, and every clip is recorded with its source
and licence.

The training set and the gate set are separate, and the gate set is test-only.
Any fitted coefficient is calibrated on the training half and reported on the
gate half, because a number fitted and reported on the same clips is not a
measurement.

## Off-corpus content

Content outside that set behaves differently enough that the only summary that
holds is a range.

Two clips both fairly called animation sit 33 BD-rate points apart. On 3D CGI
yah264 runs 25% ahead of x264 at 1.34x the time. Held at equal quality instead,
`veryfast` reaches x264 medium at 1.07x for a fifth fewer bits. On hand-drawn 2D
it runs 8% behind.

No animation result here is claimed as a single number.
`docs/animation-content.md` has the measurements and the preset-ladder rows. It is also why a preset-for-preset speed comparison stops
meaning much once content leaves the set the presets were tuned on.
