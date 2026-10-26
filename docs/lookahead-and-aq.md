# Lookahead, mb-tree and adaptive quantization

The lookahead reads frames before they're coded. It builds a half-resolution
copy of each one and estimates what every macroblock would cost as intra and as
inter. Those costs decide where the scene cuts go and which frames become B
frames. They also say how much each macroblock matters to the frames that will
predict from it, and acting on that is mb-tree. Adaptive
quantization (AQ) works beside it and moves QP per macroblock by texture, so
flat areas, where blocking shows, get more bits than busy ones, where it
hides. Psy-RD and psy-trellis then bias the mode and coefficient decisions
toward keeping texture instead of smoothing it away.

How the frame-level AQ strength differs between CRF, capped CRF, ABR and
two-pass is covered in [rate-control.md](rate-control.md). This page covers
the mechanisms.

## How it works

**The lookahead window.** `--rc-lookahead` sets the depth: 40 frames at
medium, capped at 64. The preset table in [options.md](options.md) gives the
other presets' depths, and superfast and ultrafast run no window. Each frame is downscaled 2x2 and every 8x8 lowres
block (one macroblock) gets an intra estimate and a motion-searched inter
estimate against its neighbors in the window. The lowres search builds its half-pel
phases from the source, not by interpolating the lowres plane. It starts from
its neighbors' exact subpel vector and prices motion-vector rate, so coherent
motion stays cheap. The intra estimate scores four modes, plus the
six directional 8x8 modes on blocks where motion leaves inter close to intra.
The window runs on its own thread when there's a pool,
`bframes+1` frames ahead of the encode (`--sync-lookahead`, which costs latency
and never changes a bit; see [threading.md](threading.md)).

**mb-tree.** Walking back from the end of the window, each block passes the
share of its cost that prediction saves, `1 - inter/intra`, onto the blocks it
predicts from, splatted along its lowres motion vector. B frames split what
they pass between their two references by temporal distance. The pyramid's
reference B is a propagation target of its own and gets its own offset field
(`Y264_MBT_BREF`). The walk stops at IDRs. A block's offset is
`-strength * log2(1 + propagated / (intra * AQ weight))`, so a block that
much of the future leans on codes at a lower QP. The strength is
`5 * (1 - qcomp) * 0.7`, which is 1.4 at `--qcomp 0.6`. An AC gain of 1.7 then
widens the boost around its frame mean. That spreads the allocation without
moving the anchor-versus-B balance. The AQ offset is folded into the same
per-macroblock value. Under CRF and ABR the boost isn't centered on zero. mb-tree runs whenever `--bframes` is above 0, or at
`--bframes 0` when the lookahead window is on. Constant QP turns it off along
with AQ, so `--qp N` means one QP for every macroblock.

**Variance AQ.** Each macroblock's energy is `l = log2(variance + floor)` on
luma. The offset is `strength * gain * (l - center)`. `--aq-strength` defaults
to 0.4 in every rate-controlled mode and 0 at constant QP. `--aq-mode 2`, the
default, adds a per-frame gain from the spread of `l` across the frame: a
frame whose blocks all sit near one energy (a dark scene, a fade, a flat
cartoon background) has little masking to exploit, so its gradient eases,
down to half. `--aq-mode 1` is plain log-variance AQ with gain 1. There's no
mode 0. AQ off is `--aq-strength 0`.

Where the center sits depends on the frame:

| frames | center | variance floor |
|---|---|---|
| anchors and reference B (they carry an mb-tree field) | a fixed level, 4.5 at 8 bits, 8.5 at 10 bits | 2 |
| non-reference B | the frame mean, pulled 1.3x toward a fixed level of 6.0 | 16 (1 under two-pass) |

**The frame-level DC term.** Centering the anchors' field on a constant
instead of the frame mean leaves a DC in it: a busy frame's field averages
above zero and a flat frame's below. Under CRF that DC is the encoder's
across-frame and across-shot complexity term. The part the per-macroblock
field doesn't carry goes onto the anchor's base QP at its own strength
(`Y264_AQ_DC`), and the B frames inherit it through the QP cascade. The DC is
weighted by a per-macroblock rate proxy at 0.25, so a frame
that's mostly black or flat doesn't drop its base QP further than its field's
bits justify. Non-reference B frames get their frame-level term from the
center pull in the table instead. The DC strength is 1.3 under uncapped CRF
and 1.04 elsewhere; [rate-control.md](rate-control.md) has why.

**Psy-RD.** `--psy-rd 2.0`. Mode decision scores each candidate by SSD plus a
penalty for the reconstruction's texture energy departing from the
source's. The energy is the average of a 4x4 SATD measure and an 8x8 Hadamard
measure, so a skip or flat mode that averages texture away no longer wins on
SSD alone. It only steers decisions, and the reconstruction stays exact. On
2160p frames at medium, the pre-search skip bound keeps half the psy term,
which saves about 4% of instructions on meridian 2160p.

**Dark, smooth blocks.** Banding shows first in dark gradients and faint
dark grain, and VMAF-NEG doesn't see it, so two terms guard those blocks
directly. A macroblock counts as dark and smooth by two linear ramps: its luma
mean (full weight at 32 or below, none at 64, on the 8-bit scale) and its
variance (full at 4 or below, none at 16). On those blocks the psy-RD weight
rises to 5x, because at one QP the RD decision would otherwise code faint
grain to a flat block. Those blocks also get up to 6 QP below the variance
offset in two kinds of frame. In frames that also hold bright areas, the
field's center sits high and a low `--aq-strength` leaves the dark side coarse;
there the cut is scaled by the share of macroblocks with a luma mean of 170 or
more, none at 8% and full at 20%. In frames where much of the picture is dark
grain or gradient, it is scaled by the share of dark, smooth macroblocks that
aren't flat, counted over the picture leaving out any letterbox or pillarbox
bars, none at 35% and full at 50%. A dark frame with neither is left to the
variance offset.

**Letterbox and pillarbox bars.** The grain share leaves out bars of any matte
color: matched pairs of flat edge rows (or columns) of one color, of about the
same depth, that don't drift line to line, up to three eighths of the height or
width a side. Only flat blocks count, so a grainy sky is never a bar; grainy
film-scan bars count as picture. Without this, the bars of a letterboxed night
scene would dilute its share and keep the gate shut.

**Psy-trellis and the flat-content lattice.** `--psy-trellis` defaults to 0.
When on, the trellis rewards keeping the AC energy of the reconstructed
coefficients, `J -= psy * sum_AC |fdct(pred + res)|`. On its own it's a
tuning knob (`--tune grain` sets 1.0, film and animation 0.5). It also turns
on automatically per frame: when at least 75% of a frame's macroblocks have
luma variance under 25, that frame's psy-trellis runs at no less than 1.2,
inside the Viterbi trellis. The gate reads
only the source frame, so it's deterministic at any thread count. On the
corpus it fires on sintel and nothing else.

**Scene cuts.** A frame is a cut candidate when its lowres
`min(intra, inter)` cost against the previous frame reaches
`(1 - bias) * intra`. The bias rises from a quarter of `--scenecut / 100`
(default 40) toward the full value as the GOP ages. Two more tests then
apply. The frame's ratio has to be at least 0.90, unless its
intra cost more than doubled, which is a scene rising out of black. And it has
to jump by 0.10 over the largest of the three frames before it,
or halve their remaining headroom. Last, a flash test clears
the cut if the next frame predicts well from the frame before the cut. A cut
inside `--min-keyint` (auto: `keyint/10`) still becomes an I frame, only not
an IDR.

**B placement.** `--b-adapt 1` keeps each candidate a B until its lowres
cost reaches 0.77 of its intra cost. Then it's demoted to
an anchor. The cost takes, per block, the cheapest of the forward leg against
the previous frame, the backward leg against the next one, and intra, summed
over the frame. The backward leg reuses the next frame's own search of the
same pair. On
most content every run fills to `--bframes`. `--b-bias` moves the 0.77
linearly, to 1.0 at +100 and to 0 at -100. `--b-adapt 0` codes a fixed
cadence. [b-frames.md](b-frames.md) covers the pyramid and the B coding
itself.

## What we chose and why

All BD figures are BD-VMAF-NEG at matched achieved bitrate; negative is better.

| choice | why |
|---|---|
| `--rc-lookahead 40` | Swept at 30/40/50 on 11 real-title cells at CRF and VBR, against x264 medium at its own 40. 50 beats 40 by 0.1 to 0.4 points on bbb (both sizes) and meridian 1080p and is inside the 0.4-point run-to-run spread everywhere else. 30 loses 1.9 points on chimera 2160p VBR. Each frame of depth costs about 22 MiB per instance at 2160p. |
| `--aq-strength 0.4` | A seven-clip sweep put the optimum near 0.3. At 1.0, AQ loses about 5% everywhere. |
| `--aq-mode 2` gain constants | Fitted at matched rate. A spread exponent at or below 0.5 loses 1.3 to 4.0% on sintel. A gain cap above 1.0 costs perseverance and akiyo 0.3 to 1.3%. |
| floor 2 on mb-tree frames, 16 on non-reference B | Floor 1 costs perseverance 1.3%. The B floor at 16 reads -0.4% on bbb against +1.0% at floor 1. |
| absolute anchor 4.5 | -5.17% median on 12 clips, 11 better. 5.5 has a worse mean and worst case. Above 5.5 it degrades fast (samsung +9.85% at 7.5). |
| DC strength 1.3 under uncapped CRF | Lowest worst clip on the CRF band. It recovers long multi-shot titles: a 1080p animated feature reads -5.6% against x264 medium at 1.4 and -1.8% at 1.04. 1.4 starts to cost a static clip. |
| DC rate weighting 0.25 | Multi-shot -0.92 / -3.16 / -0.38%, CRF band median -0.20%. 0.5 overshoots. |
| B center pull 1.3, level 6.0 | A pull of 0 loses 1 to 2.7% on stefan, sintel and mobile. 1.6 costs akiyo under ABR. |
| mb-tree strength 1.4 | Restoring the full 2.0 closes the bitrate gap at equal CRF but costs 10 of 12 clips (ducks +7.0%). |
| mb-tree AC gain 1.7 | CRF band median -0.70%, 10 of 12 better. 1.7 is the sweep optimum. |
| reference-B mb-tree field | CRF band median -2.09%, 10 of 12 better; ABR median -0.84%. Costs about 3% of wall. |
| motion-rate lambda 8 in propagation | Without it a well-tracked block passes nearly all its cost on, and importance piles up along long chains until the offsets saturate. |
| corrected lowres costs (source half-pel phases, subpel start, the six directional modes where needed) | t1 band median -0.43%, multi-shot -0.65 / -2.46 / -0.41%, for +0.7% instructions at 1080p. |
| `--psy-rd 2.0` | Basin from 2.0 to 3.0. 2.0 is the most uniform: median -0.17%, 7 of 12 better, worst +0.74%. |
| psy-RD 5x on dark, smooth blocks | Five dark windows from three titles, at the reference's size: the share of dark smooth blocks coded flat falls from 66% to 23% on a mostly dark frame (the reference: 32%). HD band median +0.03%, worst +0.46%. 3x keeps less grain; 8x keeps no more. |
| a gated 6-point dark cut | It keeps dark grain and gradients from coding flat, within the banding check's margins of the reference on every window. Applied to every dark frame it spends bits where nothing can band, so the gate limits it to frames with bright areas or with much dark grain or gradient. |
| flat-content psy-trellis gate | sintel -5.6% (high band) and -9.0% (low band) for +0.8% wall on sintel. Every other corpus clip is byte-identical. The class that gains is flat and dark content, not grain. |
| scene-cut floor 0.90 | Real cuts read 0.93 or more. False cuts on continuous content read 0.62 to 0.89. Sunflower -3.66%, parkrun -1.42%. |
| scene-cut jump 0.10 | Removes false cuts where the falling threshold walks onto a high, steady ratio (fog, water, a tilt): coastguard -2.66%, perseverance 720p -3.01%. Every real shot boundary is kept. |
| demote threshold 0.77 | At 0.90 riverbed's water read 0.79 to 0.81, so it coded B frames from anchors four frames apart and fell behind. At 0.77 it codes P. No other clip reads above 0.74, and the CRF band is byte-identical. |

## What didn't work

| idea | why not |
|---|---|
| Chroma in the AQ energy | +0.35 to -0.29%, neutral, for 9% of the mb-tree bucket. |
| A gain that grows with frame energy above 1 | stefan +2.5%, and the HD clips lost too. |
| The spread gain on non-reference B under a bitrate target | Cost mobile's ABR band 1.2 to 2.4%. That path runs gain 1 there. |
| Pulling only B frames busier than the level | Nothing on perseverance, sintel +0.3 to 0.5%. |
| The DC term as a per-macroblock offset instead of on the base QP | mobile +2.4%: consumers keyed on the base QP (lambda, cascade, psy, trellis) don't see it. |
| A CRF base term from lowres complexity under mb-tree | Either sign hurts some clip: +0.6 costs sintel 10%, -0.6 costs akiyo 1.6%. |
| Centering the mb-tree boost under CRF | Destroys the cross-frame gain on static content (akiyo). |
| A magnitude clamp on mb-tree offsets | Refused. |
| Content-adaptive mb-tree strength from a motion ratio | Belongs to the older per-anchor path. The whole-window walk replaced it and doesn't use it. |
| A QP-ramped psy-RD weight, or one scaled by the ME lambda | Deep band median +0.24 / +0.44%. The lambda-scaled 4x4-only form also lost. |
| Psy energy inside the intra-mode SATD loops | BD loss. |
| Psy-trellis as a residual-energy or dequantized-coefficient reward | Akiyo gains, foreman and mobile lose 1 to 3% at every strength. Only the reconstruction-spectrum form is clean. |
| The flat gate at 50% on the greedy trellis | Quality clean, but samsung wall +30%. |
| Cost-driven B placement rules (pairwise, path-cost, path-cost on half-pel lowres) | +8 to +16% each. The fixed full cadence the conservative rule keeps is the optimum on most content. |
| Whole-run B placement | Cosmos +0.4 to +0.85 VMAF-NEG, but coastguard and akiyo lose and the band median is 0. Costs +3.5% instructions at best. Off. |
| `--b-bias` below 0 | -50 costs 11 to 54% on four clips. Above 0 does nothing on that content, since every run is already full. |
| The field-ME previous-frame leg for cut sums | Finds better matches, so the sums fall below the calibrated cut threshold: chimera 2160p lost two real cuts. |
| A cheaper four-mode lowres intra on 2160p | chimera +1.45%. |
| Suppressing a cut inside `--min-keyint` entirely | sintel's early cut coded straight through as a B cadence and spent 76% of the clip's bits on the 8 frames after it. |

## Open questions

- Why the best AQ strength (0.3 to 0.4) sits so far below x264 medium's 1.0.
  One lead: the field also weights mb-tree's propagation, and the mb-tree
  term is worth far less with those weights at 0.4 than at 1.0 (bus -4.6%
  against -12.4%).
- Depth 50 is better than 40 on bbb and meridian 1080p and never worse
  beyond the spread. Whether that pays its memory and lookahead time at 2160p is
  unmeasured.
- sintel loses 6.8% under CRF with the reference-B mb-tree field on. It's
  reproducible and unexplained.
- `--tune grain` still sets psy-rd 1.5, a value measured against an older
  default of 1.0, and hasn't been re-measured against 2.0.

## Measuring it

Judge AQ, the DC term and mb-tree with `scripts/bd_at_rate.py` or
`scripts/band_at_rate.py` at matched achieved bitrate. A matched-CRF-number
comparison reads the absolute anchor as a large loss (samsung +9.3%), because
most of what it does is move the CRF scale, and that's ladder placement, not
quality. The 12-clip CRF band can't see the across-shot term at all, so run
the multi-shot gate too. mb-tree's constants were fitted together, so don't
re-sweep one alone. `Y264_PSY_FLAT_LOG=1` prints each frame's flat share and
whether the psy gate fired.
`Y264_TYPE_ORACLE=<file>` replays a frame-type sequence through the encoder,
which prices a placement policy apart from everything downstream of it.

Any change to AQ, psy or the RD decision also runs `scripts/band_check.py`.
VMAF-NEG doesn't see banding and CAMBI misses much of it, so a change can
pass every BD band and still flatten dark gradients. The script compares
yah264 with x264 at the same size on the dark windows the banding was found
on, and fails a window that flattens more dark blocks or scores worse on
CAMBI. `tests/test_darkaq.c` fails if either dark-block term is tuned off.
