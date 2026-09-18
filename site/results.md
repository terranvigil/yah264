---
title: Results - yah264
description: The goal tables, the quality maps, the corpus, and how each number reproduces.
---

# Results

Both encoders code the same clip at the same quality. Every ratio below is
yah264's wall time over x264's. 1.0 is a tie. Lower is faster. x264 runs at
`--preset medium`. The SIMD rows use its stock build. The pure C rows use a
build with its assembly off. Quality is scored with the VMAF NEG model, which
predicts how good video looks to people.

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

The worst clip on every row is sunflower, the low-bitrate 1080p clip. The
high-bitrate 1080p clips are the fastest cells on the board. Goal 2 passes its
speed and quality legs. Goals 1 and 3 have their worst clips on and just past
the bar. Both of those readings are inside the board's own run-to-run spread,
so both legs stay open.

fourpeople_720p replaced samsung_720p. samsung was vendor material with no
licence. fourpeople keeps samsung's operating point, so the row stays
comparable.

The numbers: fourpeople reads 1.04x at samsung's 1200 kbit/s, the same as
samsung did. shields at 2.3 Mbit/s is the second-slowest clip. Before the 1080p
clips joined, the six-clip board read 0.95x, 0.85x and 0.96x on the three rows.
Two reads of the previous board had the shipped row's worst clip on the bar.
The full per-clip tables are in our local board notes.

## Reading the tables

Every figure here comes from one Apple M5 Max with 18 cores. We will add other
CPUs and instruction sets as we support them. Each cell is the median of three
interleaved samples. A board repeated on the same machine still
moves its ratios, so small differences in these tables are not readings. A goal
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
That is CRF. The table at the top of this page reads it. Ask for a bitrate
instead. You get whatever quality that buys. That is ABR. No goal is set
against the ABR table.

**ABR, matched achieved bitrate.** Superseded, because samsung still holds the
720p slot. A quiet re-read will replace it.

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

Bitrate orders these rows. Resolution doesn't. The slow cells are the
low-bitrate HD ones. The fastest on the board are the high-bitrate 1080p ones. The 1080p column reads high because two of its three clips are
low-bitrate.

The numbers: our median absolute rate error over the ten board clips went from
12.5% to 6.6% multi-threaded and from 7.5% to 3.8% single-threaded, against
x264's 5.6% and 4.0%. We fitted the opening on seventeen non-board cells and
held the board out. Matched-rate BD-rate gained 2% at the median. That board
moved the ratios by 0.01 to 0.04. The old resolution-only seed opened the
high-bitrate cells near QP 6 to 17 against operating points of 27 to 34, and
samsung opened five QP too high, which is where the earlier −14% and +18% rate
misses came from. The staircase alone took the multi-threaded rows from 1.06x
and 1.26x to 0.94x and 1.11x, where multi-threaded ABR had cost 0.3 to 0.4 of a
ratio point over CRF. Earlier still, both encoders got the same target and the
sizes differed by about 3%. The single-threaded tier re-read with its median
unchanged and its worst clip a hundredth higher, where fourpeople read 0.85x.

## How to reproduce them

Both encoders run as libraries inside a single ffmpeg process. Every encode
hands the same thread count to both. Goal 1 fixes that count at one on each
side. Goals 2 and 3 pass 0 for auto and let each encoder resolve its own
number, because auto is how each one ships. The pure C rows need one adjustment
to be fair. x264's configure adds `-fno-tree-vectorize` unconditionally, so a
stock build's C is not auto-vectorised while ours is. We build x264 with
`--disable-asm`, strip that flag from its `config.mak` and rebuild, so the
compiler treats both sides the same. The runtime switch can't do it instead,
because the flag was applied when the binary was compiled. Each number on this
page says which board it came from, and
[instruments.md](https://github.com/terranvigil/yah264/blob/main/docs/instruments.md)
records the binaries.

For licence reasons the wrapper that runs both encoders in one process lives in
an [ffmpeg fork](https://github.com/terranvigil/FFmpeg) outside this
repository. This runs the two-CLI version instead and needs no fork:

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
board corpus. The one preset-ladder reading we do have is on animation, below,
and it moves the answer a long way. Until the same ladder runs on the board
corpus, read the tables above at equal bytes.

## Quality across the rate range

yah264 does best at low bitrates. In the deep band, VMAF-NEG 55 to 83, it beats
x264 on 9 of the 10 clips that reach that band. The median gain there is about
12% [BD-rate](encoding.html#bd-rate), which averages the comparison across a
range of bitrates. The lead fades as bitrate rises and is gone at the top.
Those ten clips come from the twelve-clip quality band, so they are not the ten
of the speed board. The lead depends on resolution as much as on rate, and the
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
| foreman_cif | 400 | 1.5 | 20 | −0.6 | 93.9 | 93.3 |
| bus_cif | 400 | 1.4 | 17 | −6.8 | 93.3 | 86.5 |
| stefan_cif | 400 | 1.7 | 13 | −4.1 | 90.1 | 86.0 |
| ducks_720p | 25000 | 0.4 | 61 | −7.0 | 91.9 | 84.9 |
| park_joy_720p | 12000 | 0.4 | 44 | −7.8 | 90.4 | 82.6 |
| fourpeople_720p | 1200 | 0.9 | 24 | −4.2 | 89.8 | 85.6 |
| shields_720p | 2200 | 0.6 | 27 | −6.1 | 94.7 | 88.6 |
| sunflower_1080p | 1500 | 0.6 | 30 | −3.3 | 90.8 | 87.5 |
| pedestrian_1080p | 2800 | 0.5 | 48 | −2.3 | 87.4 | 85.1 |
| riverbed_1080p | 12500 | 0.3 | 71 | −9.5 | 88.2 | 78.7 |

Six-second windows. Our encoder runs at auto threads.

On HD the hardware burns far less CPU than we do. It finishes sooner there in
wall time too. On CIF it is slower than we are, because opening the session is
most of the run. Quality is what pays for all of that. The hardware scores
below us at the same bitrate on nine of the ten clips.

The numbers: the quality columns are a second read taken after the rate-control
refit, which lifted riverbed off 82.8. The timing columns are the earlier read. Our own columns here differ from the openh264 table below by up to about
11% on a cell. The hardware is not byte-stable run to run, so riverbed read 83.0 on one run
against 78.7 on the next at the same size. Sizes come within a few percent of
target on both sides. The full per-clip figures are in our local
records.

## Against other encoders

| encoder | quality (VMAF) | pure-C 1-thread | pure-C MT | SIMD MT | size | notes |
|---|--:|--:|--:|--:|--:|---|
| yah264 | +0.22 | 0.92x | **0.84x** | 0.96x | +0.0% | this repo, ten-clip board |
| x264 | ref | 1.00x | 1.00x | 1.00x | ref | the reference point |
| openh264 | -14.2 | 0.15x | 0.98x | withdrawn | +4.1% | a different design point |

openh264 beats x264 single-threaded because it does far less work per frame,
and it pays about 14 VMAF points for that at the same bitrate. Read the speed
and the quality columns as one row. The yah264 row is the goal table above. The
openh264 row runs Cisco's encoder over the same ten clips and bitrates at its
own defaults, with frame skipping off so both encoders code every frame. Speed
is wall time against x264 in the same three builds. Quality and size are
against x264 at the same target. Its SIMD MT cell is withdrawn, because it read
faster than the column beside it, which cannot be right.

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
x264 read +63.7%, which is in our local records and not re-measured here. Its
gap against x264 comes from a different measurement again, so the two do not
subtract to the +0.22 in the board table at the top of this page.

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
sizes, so the whole swing was allocation across shots and not coding
efficiency. The fix puts the frame-mean part of adaptive quantisation on the
frame's base QP at full strength, on every input path including streaming. The
single-shot tables above did not move. `--shot-crf` goes further on file input
by sizing each shot against the whole title. Four CRF points fitted with a
cubic is thinner than the five-point sweeps our own rule asks for, so read the
table for the shape of the effect. Reproduce it with:

```
scripts/multishot_bd.py --arms flat=env:Y264_AQ_DC=0.4 new=env: x264
```

The numbers: each sequence runs 150 frames per shot, except the CIF sequence's
third shot at 90. CIF runs at 30 fps, 720p at 50 and 1080p at 25. The four CRF
points are 22 to 34. One clip at a time the same content reads -5.7% on CIF,
-14% on 720p and +0.4% on 1080p. The single-shot band moved by +0.3% at the
median and +0.9% at worst.

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

The training set and the gate set are separate. The gate set is test-only. Any
fitted coefficient is calibrated on the training half and reported on the gate
half. A number fitted and reported on the same clips is not a measurement.

## Off-corpus content

Content outside that set behaves differently enough that the only summary that
holds is a range. Two clips both fairly called animation are 33 BD-rate points
apart. yah264 is well ahead on 3D CGI and behind on hand-drawn 2D. So no
animation result here is claimed as a single number, and
[animation-content.md](https://github.com/terranvigil/yah264/blob/main/docs/animation-content.md)
has the measurements and the preset-ladder rows. That spread is also why a
preset-for-preset speed comparison stops meaning much once content leaves the
set the presets were tuned on.

The numbers: on 3D CGI we run 25% ahead of x264 at 1.34x the time. Held at
equal quality there, our `veryfast` reaches x264 medium at 1.07x for a fifth
fewer bits. On hand-drawn 2D we run 8% behind.
