---
title: Results - yah264
description: The goal tables, the quality maps, the corpus, and how each number reproduces.
---

# Results

Both encoders code the same clip at the same quality. Every ratio below is
yah264's wall time over x264's. 1.0 is a tie. Lower is faster.

**CRF, matched achieved bitrate**, ten clips. Three CIF, four 720p, three 1080p.
The configuration column names both sides. Rows 1 and 2 run our plain C
against x264 built with its assembly off. Row 3 is what each encoder ships:
our NEON build against x264 with its hand-written assembly on.

| goal | configuration | median | max | VMAF | size | status |
|---|---|--:|--:|--:|--:|---|
| 1 | our C vs x264 C, one thread each | **0.92x** | 1.15x | +0.26 | −0.1% | worst clip on the bar, so the leg is open |
| 2 | our C vs x264 C, all threads | **0.84x** | 1.06x | +0.20 | +0.1% | speed and quality legs pass |
| 3 | our NEON vs x264 assembly, all threads | **0.96x** | 1.16x | +0.22 | +0.0% | worst clip past the bar by 0.01 |

<div class="aside">
<p class="aside-title">Four words this page uses a lot</p>
<p><b>Board.</b> One timed run of both encoders over a fixed set of clips. This page reads the ten-clip board.</p>
<p><b>Band.</b> The clips and CRF rungs a quality sweep is read over, chosen to fall inside VMAF-NEG 55 to 95.</p>
<p><b>Leg.</b> One metric of a goal, read on its own.</p>
<p><b>Bar.</b> The number a leg has to beat.</p>
</div>

The worst clip on every row is the same low-bitrate 1080p one, sunflower at
1.5 Mbit/s, with shields at 2.3 Mbit/s next. The high-bitrate 1080p rows are
the fastest cells on the board.

Both open legs are inside the board's own run-to-run spread. The
single-threaded pure C row's worst clip is exactly on the bar and passes. The
shipped build's is a hundredth over, where two reads of the previous board had
it on the bar. Neither that row nor the gap between the two medians is settled
in either direction. The clip swap is not behind them either: fourpeople reads
1.04x in samsung's slot, the same as samsung did.

fourpeople_720p holds the slot samsung_720p used to. samsung was vendor
material with no licence. fourpeople is Xiph derf and anyone can fetch it. We
gave it samsung's operating point of 1200 kbit/s instead of its own calibrated
1600, so the row stays comparable with the pre-swap board.

The wall ratio is only half of a speed reading. The board prints a CPU-seconds
ratio beside it. That column says whether a wall ratio near 1.00 is efficiency
or occupancy. Being level while burning half again the CPU is a lead that goes
the moment the reference threads better. We have not published that column
beside these medians yet. The one occupancy figure we have published is
foreman_cif's, on the [design page](design.html).

Before the 1080p clips joined, this table was taken on six clips with no 1080p
in it and read 0.95x / 0.85x / 0.96x. The full per-clip tables are kept in our
local board notes.

## Reading the tables

Every figure on this page was taken on one machine, an Apple M5 Max running
macOS 26, Darwin 25.6. It has 18 cores, 6 performance and 12 efficiency. The
multi-threaded rows are wall-clock ratios at that core count. We will add
rows for other CPUs and instruction sets as we support them.

Each cell is the median of three interleaved samples. A cell whose own samples
spread by more than 1.15x prints a warning and is not read. Repeating a whole
board on the same machine still moves a ratio by up to about 0.10, and another
machine will probably move it further. So differences under 0.05 in the tables
here are not readings. That is why the shipped row's worst-clip leg stays open
on a hundredth.

The goal is made up of four metrics, with a fifth reading underneath them as a
floor:

| metric | bar |
|---|---|
| median speed | 1.00x or faster |
| worst-clip speed | 1.15x or better |
| quality | within 0.5 [VMAF](https://en.wikipedia.org/wiki/Video_Multimethod_Assessment_Fusion) |
| compression | within 1.0% size |
| pixel accuracy (floor) | no clip more than 1.0 dB below x264 in PSNR-Y at the same bytes |

Don't read the compression row as a target. Every row of the tables on this
page is solved onto a matched achieved bitrate before either encoder is timed,
so the size column reads the solve's residual, inside about 0.3% by
construction. Read it as a check that the solve converged. The compression
result proper is
[BD-rate](encoding.html#bd-rate), further down the page.

The last row is a floor. VMAF models what a viewer notices. PSNR measures how
far each pixel actually moved, and the two can point in opposite directions.
The sibling H.265 encoder ran both over fifteen clips and they disagreed on
nine of them, while the level gap at the same bytes stayed small. That is how a
change buys VMAF by quietly spending pixel accuracy with no column to notice.
So PSNR-Y gets a floor. The bar is well outside anything measured so far, and a
clip 0.5 dB below x264 goes on a debt list while the build still passes.
Quality decisions are still made on VMAF.

The current read is CRF at matched bitrate, ten clips, pure C: median
-0.07 dB, worst clip bus_cif at -0.47 dB. The floor passes and the debt list is
empty.

The rates are matched to within 0.5% of each other, not exactly. Quality
differences of a hundredth of a VMAF point are therefore smaller than the test
can measure, and the gaps between the three rows' VMAF columns are inside that.

## Why the rate control mode changes the answer

You can ask an encoder for a quality level and take whatever bitrate comes out.
That's CRF, and it's the table at the top of this page, solved per clip so that
it hits the same bitrate as x264. Or you can ask for a bitrate and take
whatever quality comes out. That's ABR, below. The two answer different
questions, so both are here.

**ABR, matched achieved bitrate. Superseded, kept for the record.** This table
was taken after the rate controller's opening was refitted, described below. It
still has samsung in the 720p slot, so it is not the current board. We re-read
the multi-threaded tiers three times with fourpeople, and every read came back
with the board's own "box loaded" warning on several cells, so those re-reads
are not quoted. The single-threaded tier did reproduce. Its median came back
unchanged and its worst clip a hundredth higher, inside the spread.
fourpeople's cell read 0.85x there. Nothing below is decided on this table. A
quiet re-read will replace it.

| goal | configuration | median | max | VMAF | size |
|---|---|--:|--:|--:|--:|
| 1 | pure C, single-threaded | 0.93x | 1.14x | +0.34 | −0.1% |
| 2 | pure C, multi-threaded | 0.90x | 1.08x | +1.23 | +0.1% |
| 3 | as-shipped SIMD, multi-threaded | 1.07x | 1.23x | +1.25 | +0.1% |

Here x264's target is solved so that it hits the bitrate we achieved. That is
what the size column shows. No goal is set against this table. It does read as
a speed number now: before the two changes below, it handed both encoders the
same target and let the sizes differ by about 3%, which made it unreadable as
one. Single-threaded, ABR costs us nothing over CRF. Multi-threaded, read
against the CRF table at the top of the page, it costs about a tenth of a ratio
point, down from 0.3 to 0.4 before the two changes below. We let the
rate-control decide run one burst ahead. Under that
lag we then allowed a staircase device that lets the next frame start against a
reference still being coded, which rate control had been refusing. The
staircase alone took the multi-threaded rows from 1.06x and 1.26x to 0.94x and
1.11x.

We refitted the rate controller's opening at the same time. Its cumulative rate
factor never forgets the first second, and the old resolution-only seed opened
the high-bitrate cells near QP 6 to 17 against operating points of 27 to 34.
samsung, then still on the board, opened five QP too high. That is where the
earlier −14% / +18% rate misses came from. The opening is now fitted at the
first decide on the lookahead window's inter cost and the target bits per
macroblock. We fitted it on seventeen non-board cells and held the board out.
On the ten board clips at their ABR rates the median absolute rate error went
from 12.5% to 6.6% multi-threaded, against x264's 5.6%, and from 7.5% to 3.8%
single-threaded, against x264's 4.0%. Matched-rate BD-rate gained 2% at the
median. The table above is the first board after that change, and it moved the
ratios by 0.01 to 0.04.

**By resolution class**, median ratio on the same two boards of three CIF, four
720p and three 1080p clips. The ABR columns come from the superseded table
above, so its caveat applies to them:

| goal | CRF CIF | CRF 720p | CRF 1080p | ABR CIF | ABR 720p | ABR 1080p |
|---|--:|--:|--:|--:|--:|--:|
| 1 | 0.93x | 0.90x | 1.07x | 0.90x | 0.90x | 1.09x |
| 2 | 0.78x | 0.86x | 1.00x | 0.90x | 0.87x | 1.04x |
| 3 | 0.92x | 0.99x | 1.10x | 1.07x | 1.02x | 1.18x |

Bitrate orders these rows. Resolution doesn't. The slow cells are the
low-bitrate HD ones, and the high-bitrate 1080p cells are the fastest on the
board. The 1080p column reads high because two of its three clips are
low-bitrate.

## The three speed goals

Goal 2 passes its speed and quality legs. Goals 1 and 3 have their worst clips
on and just past the bar. Both readings are inside the board's own spread, so
both stay open.

## How to reproduce them

Both encoders run inside a single ffmpeg process as libraries, and every encode
hands the same thread count to both. Goal 1 fixes that count at 1 on both
sides. Goals 2 and 3 pass 0 for auto, and each encoder then resolves its own
number: yah264 takes the smaller of the online core count and 16, and x264 uses
its own rule. That is deliberate, because auto is how each one ships. It does
mean the two rows are not running the same number of workers, and the
CPU-seconds ratio is what tells you when that matters.

The pure-C rows need one adjustment to be fair, so they do not run against a
stock x264. Its configure adds `-fno-tree-vectorize` unconditionally, so a
stock build's C is not auto-vectorised while ours is. We build x264 with
`--disable-asm`, strip that flag from its `config.mak`, and rebuild, so the
compiler treats both sides the same. Leave the flag in and goal 2 reads about
0.73x. What moved there is the compiler flag. The runtime switch won't do it
instead, because the flag was applied when the binary was compiled.

Three instruments here reach for x264, and they don't all reach for the same
build. The in-process board loads whichever libx264 it is pointed at. The CLI
board builds from a source checkout with `--disable-lavf --disable-ffms
--disable-avs --disable-swscale`, and the multi-shot gate uses that checkout's
CLI. Any number quoted from this page should say which board it came from.
[instruments.md](https://github.com/terranvigil/yah264/blob/main/docs/instruments.md) records the binaries.

For licence reasons, the wrapper that runs both encoders in one process lives
in an ffmpeg fork outside this repository. `make parity-status-crf` runs the
two-CLI version instead and needs no fork.

## Read at equal quality instead

Every table above is read at equal bytes with both encoders fixed at `medium`,
which leaves our VMAF margin unspent. The fair counter-question is what happens
if the reference gets a slower preset until it reaches our quality, or if we
get a faster one until we reach its. We have not published that comparison for
the board corpus. The margin is quoted at `--preset medium` only. The one
preset-ladder reading we do have is on animation, below, and it moves the
answer a long way: on 3D CGI our `veryfast` reaches x264 medium's quality at
1.07x the time for a fifth fewer bits, where the same clip preset-for-preset
reads 1.34x. Until the same ladder runs on the board corpus, read the tables
above at equal bytes and don't take them for an equal-quality claim.

## Quality across the rate range

Quality is scored with VMAF. It predicts how good video subjectively looks to
people, and we compare both encoders at the same file size.

yah264 does best at low bitrates. In the deep band, VMAF-NEG 55 to 83, it beats
x264 on 9 of the 10 clips that reach that band, by a median of about 12%
[BD-rate](encoding.html#bd-rate) on VMAF-NEG. BD-rate averages across a range
of bitrates. The lead fades as bitrate rises and is gone at the top.

Two cautions on that number. Those ten clips come out of the twelve-clip
quality band, the ones that span the deep band. They are not the ten-clip speed
board used everywhere else on this page: same count, different clips. The lead
also depends on resolution as much as on rate. On the twelve-clip HD gate set,
which nothing here was ever tuned against, the 720p median is -13.9% with all
six ahead, while the 1080p median is +0.4% with three ahead and three behind.
Any compression claim from this project should say which bitrate range, which
corpus and which resolution it came from.

File size is half the story. It says nothing about encoding time, so every
quality number here sits next to a speed number.

## The hardware mode

`--hw videotoolbox` encodes through the Mac's fixed-function H.264 engine, with
yah264's options mapped onto it and our scene-cut deciding its keyframes. The
stream is the hardware's, not ours, so nothing above applies to it. This is its
own row, measured on the same ten clips at the same bitrates on an Apple
M-series machine. VMAF is the NEG variant.

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

Six-second windows, our encoder at auto threads. The headline is the CPU
column: on HD the hardware spends 13 to 72 times less of it than we do. It is
faster in wall time there too. On CIF it is slower than we are, because opening
the session is most of the run. Quality is what pays for all of that. The
hardware scores below us at the same bitrate on nine of the ten clips, by as
much as 9.5 VMAF points on riverbed, and it is level on foreman.

The quality columns are a second read on the current build, taken after the
rate-control opening was refitted. That refit is what lifted riverbed off 82.8.
The timing columns are the earlier read. Our own columns here are a different
read again from the openh264 table further down, which covers the same clips at
the same rates. Set the two side by side and a cell moves by up to about 11%.
That is the run-to-run variation this page opens with. The hardware is not
byte-stable run to run and its quality moves with it: riverbed read 83.0 on one
run and 78.7 on the next at the same size. Sizes are within a few percent of
target on both sides, and the full per-clip figures are in our local records.

VMAF is the v0.6.1 NEG model throughout this page. x264 is 0.165.3222 (b35605a,
Homebrew) at `--preset medium`: its stock build for the SIMD rows, and a build
with assembly off and the compiler's vectoriser left on for the pure C rows.

## Against other encoders

| encoder | pure-C 1-thread | pure-C MT | SIMD MT | quality (VMAF) | size | notes |
|---|--:|--:|--:|--:|--:|---|
| yah264 | 0.92x | **0.84x** | 0.96x | +0.22 | +0.0% | this repo, ten-clip board |
| x264 | 1.00x | 1.00x | 1.00x | ref | ref | the reference point |
| openh264 | 0.15x | 0.98x | withdrawn | -14.2 | +4.1% | same ten clips and bitrates; a different design point |

The yah264 row is the goal table above, CRF at matched achieved bitrate. The
openh264 row is a different measurement. Cisco's encoder runs through a thin
adapter over the same ten clips and bitrates as the hardware row, in
six-second windows. Each encoder runs at its own defaults. We turn frame
skipping off, which is openh264's default under rate pressure and would leave
us comparing videos of different lengths. The speed columns are wall time against x264 in the same
three configurations. The quality and size columns are against x264 at the same
target.

openh264's SIMD-MT cell is withdrawn, because it did not survive a check
against its own neighbour. openh264 has no scalar build here and its wall
barely moves with thread count, so across those three columns it is close to
one constant divided by three different x264 times. Turning x264's assembly on
makes the denominator smaller, so that cell has to read *higher* than the
pure-C MT cell beside it. It read 0.47x against 0.98x. That would put x264 with
assembly slower than x264 without it at the same thread count, and that cannot
be right. The cell is out until we re-measure it.

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

openh264 aims at a different design point, real-time and conferencing: no
B-frames, no lookahead, a light analysis. Read the row as one. It uses about a
tenth of our CPU and comes in 15.2 VMAF-NEG points below us at the same
bitrate. Its gap against x264 comes from a different measurement again, so the
two do not subtract to the +0.22 in the board table at the top of this page.

openh264 threads per slice and these streams are single-slice, so its wall does
not fall with thread count while x264's and ours do. That is what its three
speed columns are reading. Against our encoder at auto threads its wall reads
1.09x. The quality-normalised number is [BD-rate](encoding.html#bd-rate), where
an earlier five-clip measurement with B-frames off on x264 read +63.7%. That
figure is in our local records and we have not re-measured it here.

GPU-vendor encoders are fixed-function silicon with different quality and
latency trade-offs, so they stay out of scope here.

## Across shots

The board clips are single shots. Real content is a chain of them, and how an
encoder moves bits between an easy shot and a hard one is invisible above.
Three sequences built from the board clips measure it. Each has five shots with
hard cuts at known frames and 150 frames per shot, except the CIF sequence's
third shot, which is 90. CIF runs at 30 fps, 720p at 50, 1080p at 25, and
`scripts/make_multishot.py` rebuilds all three byte for byte. The measurement
is BD-rate on VMAF-NEG over four CRF points, 22 to 34, at auto threads, with
x264 medium as the control. Four points fitted with a cubic is an exact
interpolation, thinner than the five-point sweeps this project's own rule asks
for, so read these for the shape of the effect and not to the decimal.

| sequence | yah264 before the fix vs x264 | yah264 now vs x264 | `--shot-crf` vs x264 |
|---|--:|--:|--:|
| five CIF shots (690 frames) | +13.4% | +4.4% | +0.2% |
| five 720p shots (750 frames) | +4.9% | +0.3% | -2.9% |
| five 1080p shots (750 frames) | +7.3% | +0.7% | +0.2% |

Positive means yah264 spends more bits than x264 at equal quality. The same
clips taken one at a time read -5.7% on CIF, -14% on 720p and +0.4% on 1080p,
so the whole swing was allocation across shots and not coding efficiency. The
fix puts the frame-mean part of adaptive quantisation on the frame's base QP at
full strength, for every input path including streaming. The single-shot tables
above did not move: band median +0.3%, worst +0.9%. `--shot-crf` goes further
on file input by sizing each shot against the whole title. Reproduce it with
`scripts/multishot_bd.py --arms flat=env:Y264_AQ_DC=0.4 new=env: x264`.

## The corpus

The ten clips in the goal tables are natural video. All of them come from
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
with its source and licence in
[corpus-sources.md](https://github.com/terranvigil/yah264/blob/main/docs/corpus-sources.md).

The training set and the gate set are separate, and the gate set is test-only.
Any fitted coefficient is calibrated on the training half and reported on the
gate half, because a number fitted and reported on the same clips is not a
measurement.

## Off-corpus content

Content outside that set behaves differently enough that the only summary that
holds is a range.

Two clips both fairly called animation are 33 BD-rate points apart. On 3D CGI
yah264 runs 25% ahead of x264 at 1.34x the time. Held at equal quality,
`veryfast` reaches x264 medium at 1.07x for a fifth fewer bits. On hand-drawn 2D
it runs 8% behind.

So no animation result here is claimed as a single number, and
[animation-content.md](https://github.com/terranvigil/yah264/blob/main/docs/animation-content.md) has the measurements and the preset-ladder rows.
That spread is also why a preset-for-preset speed comparison stops meaning much
once content leaves the set the presets were tuned on.
