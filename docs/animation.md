# Animation

yah264 treats animation as two classes. Rendered 3D, such as Big Buck Bunny
and Sintel, behaves differently from hand-drawn 2D, such as Sita Sings the
Blues. We measure every animation change on both kinds, because a setting that
helps one kind often costs the other. Against x264 medium we're far ahead on
CGI and slightly behind on hand-drawn.

All numbers here are BD-rate on VMAF v1, where negative is better. Self-A/Bs
are against yah264's shipped default on the same binary, read at CRF 30 to 42
on 180-frame windows with both sides at two threads. Cross-encoder numbers are
against x264 medium.

## What yah264 does

| setting | what it does | why |
|---|---|---|
| `--tune animation` | adds four B frames and changes nothing else: seven at every preset with B frames, four at ultrafast. An explicit `--bframes` wins over it | B frames are the one tune element that helps both CGI clips: bbb -22.9%, sintel -11.2%, at neutral wall on bbb and 0.945x on sintel. Dropping back to five costs bbb 17.8%. The tune costs sita 3.0% |
| adaptive quantization | stays at the 0.4 default | a lower strength reads better on VMAF v1: 0.2 gains on all three clips (bbb -1.4%, sita -2.7%, sintel -3.6%). We haven't moved the default on animation evidence alone. AQ off splits the class, because sintel gains 3.4% while bbb loses 10.6% |
| mode-decision lambda | follows the mb-tree part of each macroblock's QP, on non-flat macroblocks only | at the frame QP, mb-tree's finer quantizer on static line art got priced into skip. Per-MB lambda takes about two thirds off sita's gap to x264 (+5.8% without it, +1.9% with it) and reads -0.75% median on the CRF band. It also helps the CGI clips: bbb -4.6%, sintel -0.7% |
| psy-trellis on flat frames | the psy flat gate raises psy-trellis on frames where at least 75% of macroblocks are near-flat | it's a per-frame content gate. It fires on every frame of sintel and on no frame of the other corpus clips. On VMAF v1 it reads neutral on sintel: within 0.3% at matched rate on the standing band and within 0.1% at CRF 30 to 42 |
| repeated frames | no special handling | held frames are already cheap. A bbb cut with half its frames repeated encodes 18% faster than the unique-frame cut |

## Where it stands against x264 medium

| clip | kind | BD-rate on VMAF v1 vs x264 medium |
|---|---|--:|
| bbb_720p | 3D CGI | -37.4% |
| sita_720p | hand-drawn 2D | +1.9% |

Both are short windows of 180 and 140 frames, read at CRF 30 to 42 with both
encoders at two threads. Since a median over the two would read about -18%
and describe neither clip, we always quote animation results per clip.

On the 10-minute Big Buck Bunny excerpt in the parity tables, scored on VMAF
v1, yah264 needs 1% to 2% fewer bits than x264 at CRF at 720p, 1080p and 4K. At
capped VBR the two encoders are within 1% of each other.

## What doesn't help

| idea | why not |
|---|---|
| lower psy-rd, at 0.4 or 1.0 | costs bbb 0.5-0.9% and leaves sita within 0.2%. Sintel gains 1.0-1.4%, so it splits the class |
| more reference frames (`--ref 6`, `--ref 9`) | sita gains 1.6-2.0%, but sintel loses 0.6% and bbb moves less than 0.4%. It doesn't pay for the extra search on CGI |
| `--aq-strength 0.6` | costs every animation clip: bbb +1.6%, sita +1.3%, sintel +3.1% |
| psy-trellis 0.5 in the tune | +0.2% on bbb, byte-identical on sintel, where the flat gate already sets more |
| stronger P and mid-frame skip exits | they misjudge flat content and cost bits on sintel and bbb, for at most 1.2% of threaded wall |
| `Y264_B_SKIP_EXIT=3` | free on quality (within 0.4% on all three clips) but buys only 0.35% of wall at 12 threads |
| limiting the skip exits to flat content | every exit costs bits on both animation clips. A gate would have nothing to keep |
| stronger mb-tree to fix sita | at sita's 900 kbps point, strength 2.8 loses 0.5 VMAF v1 points and 4.0 loses 3.1. Over the ladder that's +1.0% and +16.6% at matched rate |
| more intra blocks in P frames | no gain on sita |
| per-MB lambda on flat macroblocks only, or on negative offsets only | the hand-drawn win comes from the line-art minority. Restricting it to the flat majority kills it |
| anchor-grade lambda on non-reference B frames | it loses |
| a larger B-frame QP step of 2.0 | sita gains, but ducks and touchdown lose more. The best step depends on content |
| frame dedup or VFR machinery | repeats are already near-free in-codec |
| screen-content tools (intra block copy, palette) | not in H.264 |

## Open questions

- **Hand-drawn trails while CGI leads.** On sita we're about 2% behind x264 on
  VMAF v1, yet all-intra (`--keyint 1` on both sides) reads 11.4% ahead. Inter
  coding gives all of that back and more. Nothing we've found moves the B-frame
  share yet, and the obvious candidates are in the table above.
- **The tune doesn't win on hand-drawn content.** With both encoders on
  `--tune animation`, sita trails by 17.5%. Adding `--ref 6`, `--psy-rd 0.4`
  or `--aq-strength 0.6` on top of our tune leaves it 16% to 19% behind.
- **Deblock offsets on animation.** `--deblock` exists. Nobody has swept an
  offset on the animation clips yet. Nothing masks blocking on flat fills, so
  sweep it at the deep band instead of copying a value.
- **A cheaper mode decision for blocks that end up skipped.** On bbb about half
  of the eventual-skip macroblocks run the full search, against about a fifth
  on natural video. That's where animation's extra time goes. Most of it
  overlaps with other work at 12 threads, so price any fix at 12 threads, not
  at 1.
- **Banding.** VMAF v1 scores it through CAMBI. Apart from sintel's dark
  frames, CAMBI hasn't been read on the animation clips.

## Measuring it

Run bbb_720p and sita_720p on every change, and add sintel_720p when AQ or psy
moves. bbb doesn't saturate on VMAF v1 at these points: it tops out near 90
at CRF 30, so the standard points read it. sita_720p is 140 frames
calibrated at 900 kbps. bbb_720p is in the HD band (`scripts/hd_band.py`) but
not in the standing CRF ladders.

    scripts/bdcompare.py --a '...yah264...' --b '...x264...' \
        --clips bbb_720p,sita_720p --frames 180 --vmaf --points 30,33,36,39,42

Never ship an animation change measured on one clip. bbb and sita disagree
about almost everything.
