# Animation

Against x264 medium we're far ahead on rendered 3D (Big Buck Bunny, Sintel)
and slightly behind on hand-drawn 2D (Sita Sings the Blues). A setting that
helps one kind often costs the other. We measure every change on both.

Numbers are BD-rate on VMAF v1, negative is better. Self-A/Bs are against the
shipped default on the same binary.

## What yah264 does

| setting | what it does | why |
|---|---|---|
| `--tune animation` | adds four B frames and changes nothing else: seven at every preset with B frames, four at ultrafast. An explicit `--bframes` wins over it | B frames are the one tune element that helps both CGI clips. Wall is neutral on bbb and 0.945x on sintel. The tune costs sita |
| adaptive quantization | stays at the 0.4 default | a strength of 0.2 reads better on all three clips. We haven't moved the default on animation evidence alone. AQ off splits the class |
| mode-decision lambda | follows the mb-tree part of each macroblock's QP, on non-flat macroblocks only | at the frame QP, mb-tree's finer quantizer on static line art got priced into skip. Per-MB lambda takes about two thirds off sita's gap to x264 and reads -0.75% median on the CRF band. It also helps the CGI clips |
| psy-trellis on flat frames | the psy flat gate raises psy-trellis on frames where at least 75% of macroblocks are near-flat | the gate fires on every frame of sintel and on no frame of the other corpus clips. On VMAF v1 it reads neutral on sintel: within 0.3% at matched rate on the standing band and within 0.1% at CRF 30 to 42 |
| repeated frames | no special handling | held frames are already cheap. A bbb cut with half its frames repeated encodes 18% faster than the unique-frame cut |

Per clip, against the shipped default:

| change | bbb | sita | sintel |
|---|--:|--:|--:|
| `--tune animation` | -22.9% | +3.0% | -11.2% |
| five B frames in place of the tune's seven | +17.8% | | |
| per-MB lambda, on against off | -4.6% | gap to x264 from +5.8% to +1.9% | -0.7% |
| `--aq-strength 0.2` | -1.4% | -2.7% | -3.6% |
| AQ off | +10.6% | | -3.4% |
| `--aq-strength 0.6` | +1.6% | +1.3% | +3.1% |
| `--psy-rd` 0.4 or 1.0 | +0.5% to +0.9% | within 0.2% | -1.0% to -1.4% |
| `--ref 6` or `--ref 9` | within 0.4% | -1.6% to -2.0% | +0.6% |
| psy-trellis 0.5 in the tune | +0.2% | | byte-identical |
| `Y264_B_SKIP_EXIT=3` | within 0.4% | within 0.4% | within 0.4% |
| mb-tree strength 2.8 | | +1.0% | |
| mb-tree strength 4.0 | | +16.6% | |

## Where it stands against x264 medium

| clip | kind | BD-rate on VMAF v1 vs x264 medium |
|---|---|--:|
| bbb_720p | 3D CGI | -37.4% |
| sita_720p | hand-drawn 2D | +1.9% |

Quote animation per clip. The median of the two, about -18%, describes
neither.

On the 10-minute Big Buck Bunny excerpt, yah264 needs 1% to 2% fewer bits than
x264 at CRF at 720p, 1080p and 4K. At capped VBR the two are within 1%.

Sita trails in every configuration but all-intra:

| sita_720p configuration | BD-rate vs x264 |
|---|--:|
| default settings on both sides | +1.9% |
| all-intra, `--keyint 1` on both sides | -11.4% |
| `--tune animation` on both sides | +17.5% |
| our tune plus `--ref 6`, `--psy-rd 0.4` or `--aq-strength 0.6` | +16% to +19% |

## What doesn't help

| idea | why not |
|---|---|
| lower psy-rd, at 0.4 or 1.0 | costs bbb and leaves sita flat. Sintel gains, so it splits the class |
| more reference frames (`--ref 6`, `--ref 9`) | sita gains, but sintel loses and bbb barely moves. It doesn't pay for the extra search on CGI |
| `--aq-strength 0.6` | costs every animation clip |
| psy-trellis 0.5 in the tune | costs bbb a little and changes nothing on sintel, where the flat gate already sets more |
| stronger P and mid-frame skip exits | they misjudge flat content and cost bits on sintel and bbb, for at most 1.2% of threaded wall |
| `Y264_B_SKIP_EXIT=3` | free on quality but buys only 0.35% of wall at 12 threads |
| limiting the skip exits to flat content | every exit costs bits on both animation clips. A gate would have nothing to keep |
| stronger mb-tree to fix sita | at sita's 900 kbps point, strength 2.8 loses 0.5 VMAF v1 points and 4.0 loses 3.1 |
| more intra blocks in P frames | no gain on sita |
| per-MB lambda on flat macroblocks only, or on negative offsets only | the hand-drawn win comes from the line-art minority. Restricting it to the flat majority kills it |
| anchor-grade lambda on non-reference B frames | it loses |
| a larger B-frame QP step of 2.0 | sita gains, but ducks and touchdown lose more. The best step depends on content |
| frame dedup or VFR machinery | repeats are already near-free in-codec |
| screen-content tools (intra block copy, palette) | not in H.264 |

## Open questions

- Inter coding loses sita's all-intra lead. Nothing we've tried moves the
  B-frame share.
- No `--deblock` offset has been swept on animation. Sweep it at the deep
  band.
- On bbb about half the eventual skips run the full search, against a fifth on
  natural video. That's animation's extra time. Price any fix at 12 threads,
  where most of it overlaps other work.
- CAMBI hasn't been read on the animation clips, apart from sintel's dark
  frames.

## Measuring it

Run bbb_720p and sita_720p on every change, plus sintel_720p when AQ or psy
moves. bbb tops out near VMAF 90 at CRF 30, so the standard points read it.
bbb_720p is in the HD band (`scripts/hd_band.py`) but not the standing
CRF ladders.

- bbb_720p is a 180-frame window.
- sita_720p is a 140-frame window, calibrated at 900 kbps.
- Both encoders run at two threads and are read at CRF 30 to 42.

```
scripts/bdcompare.py --a '...yah264...' --b '...x264...' \
    --clips bbb_720p,sita_720p --frames 180 --vmaf --points 30,33,36,39,42
```

Never ship an animation change measured on one clip.
