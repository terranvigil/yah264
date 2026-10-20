# Animation

Animation isn't one class for yah264. Rendered 3D (Big Buck Bunny, Sintel) and
hand-drawn 2D (Sita Sings the Blues) behave differently, so every animation
change is measured on both kinds. Against x264 medium we're far ahead on CGI
and a few points ahead on hand-drawn, and a setting that helps one kind often
costs the other.

All numbers here are BD-rate on VMAF-NEG (negative is better). Self-A/Bs are
against yah264's shipped default on the same binary; cross-encoder numbers are
against x264 medium.

## What yah264 does

| setting | what it does | why |
|---|---|---|
| `--tune animation` | adds four B frames and changes nothing else: seven at every preset with B frames, four at ultrafast. An explicit `--bframes` wins over it | B frames are the one tune element that helps both CGI clips: bbb -6.8%, sintel -1.0%, at neutral wall on bbb and 0.945x on sintel. Dropping back to five costs bbb 5.7% |
| adaptive quantization | stays at the 0.4 default | on bbb and sita the default is best or within noise (sita's best alternative is -0.25%). Sintel would gain 6.9% from AQ off, but bbb loses 6.8% at the same setting, so no fixed value suits the class |
| mode-decision lambda | follows the mb-tree part of each macroblock's QP, on non-flat macroblocks only | at the frame QP, mb-tree's finer quantizer on static line art got priced into skip. Per-MB lambda takes about 45% off the hand-drawn gap to x264 and reads -2 to -3% median on the natural-video bands. bbb pays +3.1%, the one clip that does |
| psy-trellis on flat frames | the psy flat gate raises psy-trellis on frames where at least 75% of macroblocks are near-flat | it's a per-frame content gate, not an animation switch. It fires on every frame of sintel and on no frame of the other corpus clips: sintel -5.6% to -9.0% at matched rate |
| repeated frames | no special handling | held frames are already cheap. A bbb cut with half its frames repeated encodes 18% faster than the unique-frame cut |

## Where it stands against x264 medium

| clip | kind | BD-rate vs x264 medium |
|---|---|--:|
| bbb_720p | 3D CGI | -38.6% |
| sita_720p | hand-drawn 2D | -3.9% |

Both are short windows of 180 and 140 frames, read at CRF 30 to 42 with both
encoders at two threads. A median over the two would read about -21%, which describes neither,
so animation results are always quoted per clip. Short windows also exaggerate:
over the 10-minute Big Buck Bunny excerpt in the parity tables the lead is about
3%.

## What doesn't help

| idea | why not |
|---|---|
| lower psy-rd (0.4 or 1.0) | costs bbb 0.7-1.1% and sintel 1.4%; sita +0.4% |
| more reference frames (`--ref 6`, `--ref 9`) | costs bbb 0.3-0.7% and sintel 1.4%; sita -0.4%, inside noise |
| `--aq-strength 0.6` | costs every animation clip: bbb +0.7-1.0%, sita +2.6%, sintel +5.9-7.3% |
| psy-trellis 0.5 in the tune | +0.6% on bbb, byte-identical on sintel, where the flat gate already sets more |
| stronger P and mid-frame skip exits | they misjudge flat content: +13% to +32% on sintel, +2.8% to +5.7% on bbb, for at most 1.2% of threaded wall |
| `Y264_B_SKIP_EXIT=3` | nearly free on quality but buys only 0.35% of wall at 12 threads |
| gating the skip exits on flat content | every exit costs bits on both animation clips, so there's nothing for a gate to keep |
| stronger mb-tree to fix sita | strength 2.8 and 4.0 read -7.6% and -22.9% worse |
| more intra blocks in P frames | +0.7% and +0.2% on sita: no gain |
| per-MB lambda on flat macroblocks only, or on negative offsets only | the hand-drawn win lives on the line-art minority; gating the flat majority kills it |
| anchor-grade lambda on non-reference B frames | +2.2% |
| a larger B-frame QP step (2.0) | sita -2.6%, but ducks +10.0% and touchdown +6.1%. It's a content split, not a constant |
| frame dedup or VFR machinery | repeats are already near-free in-codec |
| screen-content tools (intra block copy, palette) | not in H.264 |

## Open questions

- **Hand-drawn leads by less than CGI.** On sita we're about 4% ahead, against
  4.75% ahead on all-intra (`--keyint 1`), so inter coding still gives back a
  little. The B-frame share has no lever yet, and the obvious ones are in the
  table above.
- **The tune doesn't carry hand-drawn content.** With both encoders on
  `--tune animation`, sita trails by about 14%. Adding `--ref 6`,
  `--psy-rd 0.4` or `--aq-strength 0.6` on top of our tune doesn't close it.
- **Deblock offsets on animation.** `--deblock` exists, but no offset has been
  swept on the animation clips. Blocking on flat fills is unmasked, so sweep it
  at the deep band rather than copying a value.
- **Why does per-MB lambda cost CGI at all?** bbb's loss doesn't split by
  flatness.
- **A cheaper mode decision for blocks that end up skipped.** On bbb about half
  of the eventual-skip macroblocks run the full search, against about a fifth
  on natural video. That's where animation's extra time goes. Most of it
  overlaps with other work at 12 threads, so price any fix at 12 threads, not
  at 1.
- **Banding.** VMAF doesn't see it. `vmaf --feature cambi` runs locally and
  hasn't been read on animation yet.

## Measuring it

Run bbb_720p and sita_720p on every change, and sintel_720p when AQ or psy
moves. bbb saturates above VMAF-NEG ~94, so read it at CRF 34-46 as well as the
standard 30-42 points. sita_720p is 140 frames calibrated at 900 kbps.
bbb_720p is in the HD band (`scripts/hd_band.py`) but not in the standing CRF
ladders.

    scripts/bdcompare.py --a '...yah264...' --b '...x264...' \
        --clips bbb_720p,sita_720p --frames 180 --vmaf --points 30,34,38,42

Never ship an animation change measured on one clip. bbb and sita disagree
about almost everything.
