# Animation: where the preset ladder stops meaning what it says

yah264 encodes Big Buck Bunny about 1.34x slower than x264 at the same preset,
against 0.82x to 1.16x on the six table clips. That looked like a defect. It is
not. The reason is worth writing down because the same reasoning applies to any
content class the corpus does not contain.

## The gap is content, not size or rate

Three explanations were available, and two of them are wrong. Measured on the same clip:

| test | result | verdict |
|---|---|---|
| matched bitrate vs equal CRF | 1.51x vs 1.48x | not the operating point |
| CIF vs 720p vs 1080p, same content | 1.40x / 1.33x / 1.34x | not the resolution |
| animation vs the table's natural video | 1.34x vs 0.82-1.16x | the content |

The table is three CIF clips and three 720p clips, all natural video. It has no
animation, so nothing in the published numbers covers this.

## What the extra time buys

At a matched bitrate yah264 scores **+5.07 dVMAF** on this clip, where every
table clip sits between -0.99 and +0.80. Matching quality instead of rate, x264
needs **41.3% more bits** to reach the same VMAF. Over a proper rate ladder the
BD-rate reads **-29.76% (VMAF-NEG)**, against a corpus median of -0.85%.

So the encoder is not slow on animation. It spends time on compression that this
content rewards unusually well. A rate-matched speed ratio then charges it for
quality it gives away.

## The target was already reachable

This table holds quality at x264 medium's VMAF-NEG of 94.51 and lets yah264
pick its own preset:

| yah264 preset | wall vs x264 medium | bits vs x264 medium |
|---|--:|--:|
| medium | 1.34x | -29.2% |
| fast | 1.24x | -25.9% |
| faster | 1.21x | -27.1% |
| veryfast | 1.07x | -19.5% |
| superfast | 0.99x | -9.3% |

`veryfast` reaches parity-class speed while still spending a fifth fewer bits
than x264 medium. `superfast` is faster than x264 outright and still ahead on
compression.

## Then a hand-drawn clip inverted it

Everything above was measured on 3D CGI. The only clips available at the
time were bbb_720p and sintel_720p. Both are rendered animation. Adding a hand-drawn 2D clip
changed the answer completely.

`sita_720p` is 140 frames from Sita Sings the Blues (Public Domain Mark 1.0).
They are the longest cut-free run in a flat-color sequence 35 minutes in, with
72% flat blocks and no scene cuts. Against x264 medium on the same ladder:

| clip | kind | BD-rate (VMAF-NEG) |
|---|---|--:|
| bbb_720p | 3D CGI | **-29.76%** |
| sita_720p | hand-drawn 2D | **+10.73%** |

Re-measured 2026-08-27 after the `Y264_MB_LAMBDA=5` default (the mb-tree-lambda
fix this deficit led to), on the same ladder in one run: **bbb -25.24%, sita
+7.76%**. The flip paid about 4.5 points of the CGI lead to take about 3 off
the hand-drawn deficit. That is the accepted trade. Both README numbers now
quote this pair.

Corrected 2026-09-03: **the shipped form reads sita +5.89%**, from +10.66% on
the same 30/34/38/42 ladder. The battery below records that pair.
The +7.76% above is a single run taken before the sentinel fix that improved
every read. Quote +5.89%.

That is a 40-point swing between two clips both fairly called animation. On the
content the anime literature is actually about, we lose by about 11%. The calibration
ladder shows the same thing directly: at 800 kbps yah264 reads VMAF 85.87
against x264's 91.98.

So the 29.7% lead is a CGI result. Reporting it as an animation result would
have been wrong. This is the clearest case yet of the standing rule that a
global median hides content-dependent behavior. Here the median of two clips
would have been about -9%, which describes neither.

## The finding

**Animation is not one class for this encoder.** The preset observation below
holds only for the CGI half.

**Our preset ladder is calibrated on natural video, and preset-for-preset is not
a quality-matched comparison on content that departs from it.** On animation our
cheaper presets already beat x264 medium on quality. Comparing medium to medium
spends work no one asked for. The same is likely true in the other
direction for content harder than the corpus.

Two things follow. Any cross-encoder speed claim should say what content class
it covers. Ours covers natural video and reads 0.82-1.16x there. Second,
choosing a preset from content instead of a fixed default is worth real speed at
no quality cost. The per-content selector track makes the same argument from a
different direction.

## Reproducing

Two animation clips now exist and disagree, so quote both.
`tests/corpus/sita_720p.y4m` is the hand-drawn one, 140 frames, calibrated at 900
kbps. `tests/corpus/bbb_720p.y4m` is 450 frames of 720p from 9m45s of Big Buck Bunny.
It is the most sustained-motion window that is not also a night scene. Corrected
2026-09-03: it is not in `CLIPS_CALIB` and carries no calibrated point. It sits
in `REVIEW_CLIPS` at 4000 kbps, which that set records as an order-of-magnitude
rate and not a calibrated one. It stays out of `CLIPS` for a reason that has not
changed. Adding a clip re-medians every published number, and that is an owner
call.

    scripts/bdcompare.py --a '...yah264...' --b '...x264...' \
        --clips bbb_720p --frames 180 --vmaf --points 30,34,38,42

## Where the hand-drawn deficit lives

Bisected on `sita_720p`, BD-rate VMAF-NEG against x264 medium, same ladder
throughout:

| configuration | BD-rate | delta from the row above |
|---|--:|--:|
| all-intra (`--keyint 1`) | **-4.75%** | |
| P-only (`--bframes 0`) | +4.35% | **+9.1** |
| `--bframes 1` | +5.08% | +0.7 |
| default (`--bframes 3`) | **+10.73%** | **+6.4** |

**Our intra path is not the problem: on all-intra we are 4.75% AHEAD** on exactly
the content the full-GOP number says we lose. Intra prediction, the transform and
RDOQ all handle flat color and hard edges well.

The whole 15-point swing is inter. It arrives in two roughly equal pieces.
About 9 points appear the moment P frames exist, and about 6 more as B frames go
from 1 to 3. So there are two separate problems.

### Exonerated, so nobody re-runs them

- **AQ.** Swept 0.0/0.2/0.6/0.8 against the 0.4 default on both animation kinds.
  The default is near-optimal on both. Sita's best alternative is -0.25%, which
  is noise. The 13.7-point aq-0 swing between the CGI clips is real. The default
  is already on the right side of it.
- **Duplicate frames.** `sita_720p` has ZERO exact consecutive duplicates and 4%
  near-duplicates. Every frame is unique, and the hold-length histogram is 139
  runs of 1. Animation on 2s does not survive a 1080p master being decoded and
  rescaled, so this clip cannot test that hypothesis either way.
- **The intra path**, per the table above.

### What that leaves

Two inter-side questions remain on flat, hard-edged, low-texture content: why
the P path costs 9 points, and why B frames cost 6 more. Motion search on flat
regions is the obvious first suspect for the P half. There is little texture to
lock onto, so a wrong-but-cheap MV is easy to pick. Nothing has measured it.

### The P half, located and explained (08-27)

Per-point matched-rate NEG deltas put the P-only deficit at the starved band
(CRF 35 -0.66, CRF 38 -0.99, ~500-700 kbps; ahead above it, washed out below).
Two levers were refuted there. The first is mb-tree strength from above (2.8
reads -7.61 and 4.0 reads -22.91, catastrophically worse). The second is
intra-in-P refresh from both sides. An RD discount that buys x264's intra-in-P share (read from its bitstream) reads +0.68% BD-NEG, and opening the admission screen entirely reads +0.18%,
which is null.

**The mechanism is the frame-level mode-decision lambda under mb-tree
modulation.** We compute mode/ME lambda once per slice from the frame QP while
mb-tree modulates the quantizer per MB. On static line art, mb-tree hands large
negative offsets to exactly the MBs the frame-level lambda then prices into
SKIP. `Y264_MB_LAMBDA=1` (decide at the modulated QP) reads
**-4.59% P-only** on sita over band ladder 32/35/38/41. The split is decisive.
With AQ off (mb-tree modulation alone) the arm is worth **-12.25%**, while the
AQ half alone (mb-tree off) is +0.83%. This also explains the decoder census:
x264 codes 1.5-1.7x more P MBs at finer effective QP on this content, because
they code what mb-tree paid for and we skip it. It explains the old result that
propagation buys us ~0 where it buys x264 9-14%, too.

### Y264_MB_LAMBDA=5: SHIPPED AS THE DEFAULT (08-27, full battery green)

Two refinements produce the shippable form. The first takes the lambda from the
MB-TREE COMPONENT only (`cur_qp - aq_off`; the AQ half is a small loss). The
second engages it only on NON-FLAT source MBs, with var16x16 above the psy flat
gate's 25 threshold. Gating the flat majority instead KILLS the win, because
sita's whole gain lives on the line-art minority. bbb's loss does not decompose
by flatness at all.
An early build leaked mode-1 behavior onto the mbtree-less non-ref B frames.
The sentinel fix (not-engaged = leave the slice lambdas untouched;
`Y264_MB_LAMBDA=6,99` verified a byte-exact no-op) improved every read.
Clean numbers:

| gate | result |
|---|---|
| standing band (NEG 88-94, matched rate) | **median -2.06% / mean -1.50%, 9/12 neg, worst +0.91** (ducks +0.14, sintel +0.34, touchdown +0.91: all inside noise) |
| deep band (NEG 55-83) | **median -2.81% / mean -2.64%, 9/10 neg, worst akiyo +0.63** (inside the +/-1.2 floor) |
| sita ladder 32/35/38/41 | **-4.58%** |
| sita vs x264 (30/34/38/42) | **+10.66% -> +5.89%**, 45% of the hand-drawn deficit |
| bbb (34/38/42/46) | **+3.14%**, the one real payer |
| wall (t1, best-of-3, foreman/samsung/sita) | 0.98-1.00x, free |
| default path | byte-identical; unit tests 9/9 |

The battery ran all green with the arm engaged: conformance 518/518,
recon_thread_gate, determ_repeat 16/16 configs x 12 runs and abr_decode_gate.
The ABR-mode BD is negative on 5/5 clips (foreman -6.37, coastguard -7.18,
bus -7.27, samsung -0.76, sita -3.25). ABR wins BIGGER than CRF. That fits the
mechanism, since ABR leans harder on mb-tree's allocation. **Default flipped
08-27**, and `Y264_MB_LAMBDA=0` restores the frame-level lambda. The accepted
trade is CGI animation (bbb +3.14 at its band) against -2 to -3% everywhere
else, taken against the -29.76% CGI lead. Modes 3 (flat-only), 4
(negative-offset-only) and 6 (frame-QP floor, `=6,<qp0>`) remain for gating
experiments; 3 and 4 measured non-separating on bbb. Open: why
lambda-following hurts CGI at all,
the untested downward mb-tree-strength direction, and a goal-table re-read
after the reboot control.
