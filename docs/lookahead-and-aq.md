# Lookahead, mb-tree and adaptive quantization

The lookahead reads frames before they're coded. It builds a half-resolution
copy of each one and estimates what every macroblock would cost as intra and as
inter. Those costs decide where the scene cuts go and which frames become B
frames. They also say how much each macroblock matters to the frames that will
predict from it, and mb-tree is the part that acts on that. Adaptive
quantization (AQ) works beside mb-tree and moves QP per macroblock by texture.
Flat areas, where blocking shows, get more bits than busy ones, where it
hides. Psy-RD and psy-trellis then bias the mode and coefficient decisions
toward keeping texture instead of smoothing it away.

[Rate control](rate-control.md) covers how the frame-level AQ strength differs
between CRF, capped CRF, ABR and two-pass. This page covers the mechanisms.

## How it works

**The lookahead window.** `--rc-lookahead` sets the depth: 40 frames at
medium, capped at 64. The preset table in [the options reference](options.md)
gives the other presets' depths. Superfast and ultrafast run no window. After
each frame is downscaled 2x2, every 8x8 lowres block (one macroblock) gets an
intra estimate and a motion-searched inter estimate against its neighbors in
the window. The lowres search builds its half-pel phases from the source
instead of interpolating the lowres plane. Because it starts from its
neighbors' exact subpel vector and prices motion-vector rate, coherent motion
stays cheap. The intra estimate scores four modes, plus the six directional
8x8 modes on blocks where motion leaves inter close to intra. When there's a
thread pool, the window runs on its own thread `bframes+1` frames ahead of the
encode. That's `--sync-lookahead`, which costs latency and never changes a
bit, as [threading](threading.md) explains.

**mb-tree.** The walk starts at the end of the window and works back. Each
block passes the share of its cost that prediction saves, `1 - inter/intra`,
onto the blocks it predicts from, splatted along its lowres motion vector.
B frames split what they pass between their two references by temporal
distance. The pyramid's reference B is a propagation target of its own and
gets its own offset field (`Y264_MBT_BREF`). The walk stops at IDRs.

A block's offset is `-strength * log2(1 + propagated / (intra * AQ weight))`.
With that offset, a block that much of the future depends on codes at a lower
QP. The strength is `5 * (1 - qcomp) * 0.7`, which is 1.4 at `--qcomp 0.6`. An
AC gain of 1.7 then widens the boost around its frame mean. That spreads the
allocation without moving the anchor-versus-B balance. The AQ offset is folded
into the same per-macroblock value. Under CRF and ABR the boost isn't centered
on zero. mb-tree runs whenever `--bframes` is above 0, or at `--bframes 0` when
the lookahead window is on. Because constant QP turns it off along with AQ,
`--qp N` means one QP for every macroblock.

**Variance AQ.** Each macroblock's energy is `l = log2(variance + floor)` on
luma. The variance is on the 8-bit scale at every depth. The offset is
`strength * gain * (l - center)`. `--aq-strength` defaults to 0.4 in every
rate-controlled mode and 0 at constant QP. The default, `--aq-mode 2`, adds a
per-frame gain from the spread of `l` across the frame. When a frame's blocks
all sit near one energy (a dark scene, a fade, a flat cartoon background), its
gradient eases down to half, because there's little masking to exploit.
`--aq-mode 1` is plain log-variance AQ with gain 1. AQ off is
`--aq-strength 0`, since there's no mode 0.

Where the center sits depends on the frame:

| frames | center | variance floor |
|---|---|---|
| anchors and reference B, which have an mb-tree field | a fixed level, 4.5 | 2 |
| non-reference B | the frame mean, pulled 1.3x toward a fixed level of 6.0 | 16 (1 under two-pass) |

**The frame-level DC term.** Centering the anchors' field on a constant
instead of the frame mean leaves a DC in it. A busy frame's field averages
above zero and a flat frame's below. Under CRF that DC is the encoder's
across-frame and across-shot complexity term. The part the per-macroblock
field doesn't carry goes onto the anchor's base QP at its own strength
(`Y264_AQ_DC`). The B frames inherit it through the QP cascade. Weighting the
DC by a per-macroblock rate proxy at 0.25 keeps a frame that's mostly black or
flat from dropping its base QP further than its field's bits justify.
Non-reference B frames get their frame-level term from the center pull in the
table instead. The DC strength is 1.3 under uncapped CRF and 1.04 elsewhere.
[Rate control](rate-control.md) explains why.

**Psy-RD.** `--psy-rd 2.0`. Mode decision scores each candidate by SSD plus a
penalty for the reconstruction's texture energy departing from the source's.
The energy is the average of a 4x4 SATD measure and an 8x8 Hadamard measure.
With that penalty, a skip or flat mode that averages texture away no longer
wins on SSD alone. Because psy-RD only steers decisions, the reconstruction
stays exact. On 2160p frames at medium, the pre-search skip bound keeps half
the psy term. That saves about 4% of instructions on meridian 2160p.

**Dark, smooth blocks.** Banding shows first in dark gradients and faint dark
grain. VMAF scores miss most of it, because v1's banding term ignores very dark
areas. Two terms guard those blocks directly. A macroblock counts as dark and
smooth by two linear ramps. The first is its luma mean, at full weight at 32 or
below and none at 64 on the 8-bit scale. The second is its variance, full at 4
or below and none at 16. On those blocks the psy-RD weight rises to 5x, because
at one QP the RD decision would otherwise code faint grain to a flat block.

Those blocks also get a dark cut of up to 6 QP below the variance offset in two
kinds of frame. The first kind holds bright areas as well. In such a frame the
field's center sits high. A low `--aq-strength` then leaves the dark side
coarse. The dark cut there is scaled by the share of macroblocks with a luma
mean of 170 or more: none at 8% and full at 20%. In the second kind, much of
the picture is dark grain or gradient. There the dark cut is scaled by the
share of dark, smooth macroblocks that aren't flat: none at 35% and full at
50%. That share is counted over the picture without any letterbox or pillarbox
bars.

On a frame that's mostly black stage, a lit subject in a flat black field, the
share also leaves out the flat blocks within four levels of the coded black.
The stage would otherwise dilute it as bars do. The bar walk also takes such a
stage for a pillarbox on some frames and not others. This exclusion blends in
as flat black grows from 60% of the whole frame to 70%, black bars included.
A letterboxed or pillarboxed dark frame therefore reaches it with less black
inside its bars. It blends back out as what's left of the picture shrinks from
15% of the frame to 5%, where those few blocks can't speak for the frame.

A picture coming up out of black looks like a stage at first. So a frame
whose mean luma has moved the same way by a quarter of the fade step (below)
on each of its last six frames keeps its black in the count. So do the six
frames after such a run ends, unless a detected scene cut ends it first. A
forced keyframe doesn't end it. Like the fade test, this needs a lookahead
window. A GOP coded on its own starts it fresh.

A dark frame with neither bright areas nor much dark grain or gradient is left
to the variance offset. A frame inside a fade gets the bright-area scaling
only. A fade from black runs through dark, smooth frames that count as grain,
where the dark cut spends bits without keeping visible banding out. The
lookahead counts a frame as fading when the picture's mean luma has moved the
same way, by at least 0.2 levels, on each of its last six frames. Since it
looks back only over frames the same encoder has seen, a GOP coded on its own
starts fresh. A step of more than 16 levels is a cut and breaks the run.
Without a lookahead window (superfast and ultrafast) no frame counts as
fading.

The grain count and the bar test read a block's variance the way the picture's
8-bit copy would, 8-bit rounding included. Because a 10-bit master has only a
sixteenth of that rounding, its blocks get the rest added. Blocks of 8-bit
samples shifted or bit-replicated up to 10 bits already have it. A master and
its 8-bit copy then count nearly the same blocks.

**Letterbox and pillarbox bars.** The grain share leaves out bars of any matte
color. A bar is one of a matched pair of flat edge rows (or columns) of one
color and about the same depth that don't drift line to line. Each side can
take up to three eighths of the height or width. Without this, the bars of a
letterboxed night scene would dilute its share and keep the gate shut.

Film-scan or analog noise over a matte makes its blocks non-flat. Counted as
grain, those blocks would open the gate on their own. One frame can't tell such
a matte from a grainy dark wall or night sky. So on each axis where it found no
flat bars, the lookahead looks for bars a second time under looser terms. That
walk lets in blocks up to twice the grain ceiling and needs every line to be
nearly all matte. It keeps the same pair rule of one color, matched depths and
no drift. The bars must sit within four levels of the coded black (16, or 0 when
the stream signals full range). They must also frame a picture that isn't a
quarter or more of their own level, as grain behind credits is. A frame leaves
them out only when the frame before and the frame after it found the same depths
at the same level. With a one-frame window (`--rc-lookahead 1`) a frame can be
coded before its successor arrives. There it compares the two frames before it
instead, whatever `--sync-lookahead` is. Grain beside a clean matte still
counts, because an axis that found flat bars keeps them. Without a lookahead
window only flat bars are left out. A grainy band of near-black picture at both
edges, at one level and holding still, still reads as a matte. So do the dark
bands of full-range input that isn't tagged as full range.

**Psy-trellis and the flat-content lattice.** `--psy-trellis` defaults to 0.
When on, the trellis rewards keeping the AC energy of the reconstructed
coefficients, `J -= psy * sum_AC |fdct(pred + res)|`. As a tuning knob it's
set to 1.0 by `--tune grain` and to 0.5 by the film and animation tunes. It
also turns on automatically per frame: when at least 75% of a frame's
macroblocks have luma variance under 25, that frame's psy-trellis runs at no
less than 1.2 inside the Viterbi trellis. Because the gate reads only the
source frame, it's deterministic at any thread count. On the corpus it fires
on sintel and nothing else.

**Scene cuts.** A frame is a cut candidate when its lowres
`min(intra, inter)` cost against the previous frame reaches
`(1 - bias) * intra`. The bias rises from a quarter of `--scenecut / 100`
(default 40) toward the full value as the GOP ages. The frame's ratio then has
to be at least 0.90, unless its intra cost more than doubled, which is a scene
rising out of black. The ratio also has to jump by 0.10 over the largest of
the three frames before it, or halve their remaining headroom. Last, a flash
test clears the cut if the next frame predicts well from the frame before the
cut. A cut inside `--min-keyint` (auto: `keyint/10`) still becomes an I frame,
only not an IDR.

**B placement.** `--b-adapt 1` demotes a B candidate to an anchor once its
lowres cost reaches 0.77 of its intra cost. For each block that cost takes the
cheapest of three: the forward leg against the previous frame, the backward
leg against the next one, and intra. The frame's cost is the sum over its
blocks. The backward leg reuses the next frame's own search of the same pair.
On most content every run fills to `--bframes`. `--b-bias` moves the 0.77
linearly, to 1.0 at +100 and to 0 at -100. `--b-adapt 0` codes a fixed
cadence. [B frames](b-frames.md) covers the pyramid and the B coding itself.

## What we chose and why

BD figures on this page are BD-VMAF v1 at matched achieved bitrate. The few
that are still on VMAF-NEG (earlier metric) say so. Negative is better.
Rows marked "older build" were read on a build that still had the arm's code,
before later default changes.

| choice | why |
|---|---|
| `--rc-lookahead 40` | On v1, we swept 30, 40 and 50 at CRF and VBR on seven of the 720p and 1080p real-title cells: the three 1080p segments, tears, bbb and the 10-minute meridian excerpt at 1080p, and sintel at 720p. x264 medium ran at its own 40. Both encoders ran on three threads. 50 beats 40 by 0.7 points on bbb 1080p CRF and 0.4 on bbb VBR. Sintel 720p CRF and the meridian excerpt in VBR also read best at 50. They're only 0.2 ahead of 40, inside the spread. On those two, 30 is what falls behind. Everywhere else the three depths are within 0.4 points. That spread is the most a near-null control or a second VBR run moved one cell's BD. The 2160p cells aren't swept on v1. Each frame of depth costs about 22 MiB per instance at 2160p. |
| `--aq-strength 0.4` | 0.3 reads a median -0.35% against 0.4 on the 13-clip CRF band, with 8 clips better. At 1.0, AQ loses 3% to 13% on the four clips read. |
| `--aq-mode 2` gain constants | A spread exponent of 0.5 loses 2.1% on sintel. A gain cap of 1.2 costs the HD band a median 0.38%, with perseverance 720p the worst at +1.5%. |
| floor 2 on mb-tree frames, 16 on non-reference B | On the HD band, floor 1 on mb-tree frames reads a median +0.16% and costs bbb 720p 0.8%, though perseverance 720p gains 1.9%. A B floor of 1 reads a median -0.03%, inside the band's noise. |
| absolute anchor 4.5 | Against no anchor the 13-clip CRF band reads a mean of -1.07% but a flat median (+0.04%), with only 6 clips better. The gain sits on sintel (-7.1%) and park_joy (-5.0%). On the earlier metric it was a median -5.17% with 11 of 12 better. 7.5 costs samsung 2.3%. |
| DC strength 1.3 under uncapped CRF | On the earlier metric it had the lowest worst clip on the CRF band and recovered long multi-shot titles. On v1, a 2-minute window of a 1080p animated feature puts 1.04, 1.3 and 1.4 within 0.25% of each other against x264 medium. On the deep band 1.4 costs the static clips +0.3% to +0.7%, inside the band's noise. |
| DC rate weighting 0.25 | It loses to no weighting on all three multi-shot sequences: +0.9%, +2.0% and +1.9%. On the earlier metric it won on all three. Against 0.5 it's mixed. |
| B center pull 1.3, level 6.0 | A pull of 0 costs mobile 1.4% and sintel 0.6%, and gains stefan 0.4%. 1.6 costs akiyo 1.2% under ABR. |
| mb-tree strength 1.4 | Restoring the full 2.0 closes the bitrate gap at equal CRF but costs 12 of 13 clips, median +4.0%. fourpeople reads +14%. |
| mb-tree AC gain 1.7 | It was the sweep optimum on the earlier metric. On v1 it isn't: 1.0 reads a CRF band median of -0.08% against it, 7 of 13 better, and fourpeople -9.1%. |
| reference-B mb-tree field | It loses on v1. With the field off the CRF band reads a median -0.52%, 8 of 13 better. Under ABR it's flat (median -0.09%). It costs about 3% of wall. On the earlier metric it read -2.09% on the CRF band. |
| motion-rate lambda 8 in propagation | Without it a well-tracked block passes nearly all its cost on, and importance piles up along long chains until the offsets saturate. |
| corrected lowres costs (source half-pel phases, subpel start, the six directional modes where needed) | Older build, against the uncorrected costs: t1 CRF band median -0.34%, multi-shot -1.7%, -3.8% and -0.7%. +0.7% instructions at 1080p. |
| `--psy-rd 2.0` | On VMAF-NEG (earlier metric) the basin ran from 2.0 to 3.0, and 2.0 was the most uniform: median -0.17%, 7 of 12 better, worst +0.74%. |
| psy-RD 5x on dark, smooth blocks | On five dark windows from three titles at the reference's size, the share of dark smooth blocks coded flat falls from 66% to 23% on a mostly dark frame. The reference codes 32% of them flat. Against 1x the HD band reads a median +0.05% and a worst clip of +0.34%. 3x keeps less grain. 8x keeps no more. |
| a gated 6-point dark cut | It keeps dark grain and gradients from coding flat, within the banding check's margins of the reference on every window. Applied to every dark frame, it spends bits where nothing can band. The gate limits it to frames with bright areas or with much dark grain or gradient. |
| flat black left out of the grain share on mostly black frames | A black stage around a lit subject kept the gate shut and left its dark gradient banded. Leaving flat black out on every frame also opened the gate on dark CG shadow and on slow fades from black, where it spent bits for no banding gain. We hold the rule to mostly black frames that aren't fading. |
| no grain scaling inside a fade | A fade from black opened the grain gate on a third of sintel's frames for no visible gain. It cost sintel about 4% BD-VMAF v1 (not NEG) against the reference. Without it sintel moves from behind the reference to ahead of it. The banding windows all keep their verdicts. The rest of the corpus codes byte-identically or within noise. |
| flat-content psy-trellis gate | sintel reads -0.6% on the CRF band and -0.9% on the deep band, the second inside that band's noise. It costs +0.8% wall on sintel. The other corpus clips are byte-identical. The content that gains is flat and dark, not grainy. |
| scene-cut floor 0.90 | Real cuts read 0.93 or more. False cuts on continuous content read 0.62 to 0.89. Over the full clips, parkrun reads -2.6% and perseverance 720p -1.0%. |
| scene-cut jump 0.10 | Removes false cuts where the falling threshold walks onto a high, steady ratio, as on fog, water or a tilt. Real shot boundaries are all kept. With the floor in place it changes no cut on coastguard, parkrun, perseverance 720p or sunflower. |
| demote threshold 0.77 | At 0.90 riverbed's water read 0.79 to 0.81. It coded B frames from anchors four frames apart and fell behind. At 0.77 it codes P. No other clip reads above 0.74. The CRF band is byte-identical. |

## What didn't work

| idea | why not |
|---|---|
| Chroma in the AQ energy | Older build, equal CRF: -1.8% to +0.8% on six CIF clips, better on four, for 9% of the mb-tree bucket. On the earlier metric it was neutral. |
| A gain that grows with frame energy above 1 | It lost on stefan and on the HD clips. |
| The spread gain on non-reference B under a bitrate target | It costs mobile's ABR band 0.75%. That path runs gain 1 there. |
| Pulling only B frames busier than the level | Nothing on perseverance, a small loss on sintel. |
| The DC term as a per-macroblock offset instead of on the base QP | mobile loses: consumers keyed on the base QP (lambda, cascade, psy, trellis) don't see it. |
| A CRF base term from lowres complexity under mb-tree | Older build. Either sign hurts some clip: +0.6 costs sintel 10.8%. -0.6 gains sintel 9.3% but costs akiyo 1.0%. |
| Centering the mb-tree boost under CRF | Destroys the cross-frame gain on static content (akiyo). |
| A magnitude clamp on mb-tree offsets | Refused. |
| Content-adaptive mb-tree strength from a motion ratio | It belongs to the per-anchor walk, which runs only as a fallback. The whole-window walk doesn't use it. |
| A QP-ramped psy-RD weight, or one scaled by the ME lambda | Both lost on the deep band. The lambda-scaled 4x4-only form also lost. |
| Psy energy inside the intra-mode SATD loops | BD loss. |
| Psy-trellis as a residual-energy or dequantized-coefficient reward | Akiyo gains. Foreman and mobile lose at every strength. Only the reconstruction-spectrum form is clean. |
| The flat gate at 50% on the greedy trellis | Quality clean, but samsung wall +30%. |
| Cost-driven B placement rules (pairwise, path-cost, path-cost on half-pel lowres) | Each loses clearly. The fixed full cadence the conservative rule keeps is the optimum on most content. |
| Whole-run B placement | Older build. A 2-minute Cosmos window gains 1.1%. On the CRF band the median is 0, coastguard loses 2.8% and samsung gains 1.7%. It costs +3.5% instructions at best and stays off. |
| `--b-bias` below 0 | -50 costs 2.7% to 24% on four CIF clips. Above 0 does nothing on that content, since every run is already full. |
| The field-ME previous-frame leg for cut sums | Because it finds better matches, the sums fall below the calibrated cut threshold. chimera 2160p lost two real cuts. |
| A cheaper four-mode lowres intra on 2160p | It lost on chimera. |
| Suppressing a cut inside `--min-keyint` entirely | sintel's early cut coded straight through as a B cadence and spent 76% of the clip's bits on the 8 frames after it. |

## Open questions

- Why the best AQ strength sits at 0.3 to 0.4, so far below x264 medium's 1.0.
  The old lead was that the field also weights mb-tree's propagation and
  starves it at 0.4. On v1 that doesn't hold on bus: mb-tree is worth -6.2% at
  0.4 and -3.8% at 1.0.
- On the seven cells swept, depth 50 is better than 40 on bbb 1080p and never
  worse beyond the spread. Whether it helps at 2160p, and whether that pays its
  memory and lookahead time there, is unmeasured.
- The reference-B mb-tree field loses on the v1 CRF band but still helps sintel
  a little. Which content it hurts, and why, is open.
- `--tune grain` still sets psy-rd 1.5. That value was measured against an
  older default of 1.0 and hasn't been re-measured against 2.0.

## Measuring it

Judge AQ, the DC term and mb-tree at matched achieved bitrate with
`scripts/bd_at_rate.py` or `scripts/band_at_rate.py`. A comparison at the same
CRF number reads the absolute anchor as a large loss.
Most of what the anchor does is move the CRF scale, and that's ladder
placement, not quality. The 12-clip CRF band can't see the across-shot term at
all, so run the multi-shot gate too. Don't re-sweep one of mb-tree's constants
alone, because they were fitted together. `Y264_PSY_FLAT_LOG=1` prints each
frame's flat share, whether the psy gate fired and whether the fade test held.
`Y264_TYPE_ORACLE=<file>` replays a frame-type sequence through the encoder.
That prices a placement policy apart from everything downstream of it. Both
variables are read only in a build configured with `-Ddev_knobs=true`.

Any change to AQ, psy or the RD decision also runs `scripts/band_check.py`.
Because CAMBI misses much of the banding in dark areas, a change can pass every
BD band and still flatten dark gradients. The script compares
yah264 with x264 at the same size on the dark windows where the banding was
found. It fails a window that flattens more dark blocks or scores worse on
CAMBI. `tests/test_darkaq.c` fails if either dark-block term is tuned off.
