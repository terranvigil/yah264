# Animation: where the preset ladder stops meaning what it says

next264 encodes Big Buck Bunny about 1.34x slower than x264 at the same preset,
against 0.82x to 1.16x on the six board clips. That looked like a defect. It is
not, and the reason is worth writing down, because the same reasoning applies to
any content class the corpus does not contain.

## The gap is content, not size or rate

Three explanations were available and two are wrong. Measured on the same clip:

| test | result | verdict |
|---|---|---|
| matched bitrate vs equal CRF | 1.51x vs 1.48x | not the operating point |
| CIF vs 720p vs 1080p, same content | 1.40x / 1.33x / 1.34x | not the resolution |
| animation vs the board's natural video | 1.34x vs 0.82-1.16x | the content |

The board is three CIF clips and three 720p clips, all natural video. It has no
animation, so nothing in the published numbers covers this.

## What the extra time buys

At a matched bitrate next264 scores **+5.07 dVMAF** on this clip, where every
board clip sits between -0.99 and +0.80. Matching quality instead of rate, x264
needs **41.3% more bits** to reach the same VMAF. Over a proper rate ladder the
BD-rate reads **-29.76% (VMAF-NEG)**, against a corpus median of -0.85%.

So the encoder is not slow on animation. It is spending time on compression that
this content rewards unusually well, and a rate-matched speed ratio charges it
for quality it is giving away.

## The target was already reachable

Holding quality at x264 medium's VMAF-NEG of 94.51 and letting next264 pick its
own preset:

| next264 preset | wall vs x264 medium | bits vs x264 medium |
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

Everything above was measured on 3D CGI. Both clips available at the time,
bbb_720p and sintel_720p, are rendered animation. Adding a hand-drawn 2D clip
changed the answer completely.

`sita_720p` is 140 frames from Sita Sings the Blues (Public Domain Mark 1.0),
the longest cut-free run in a flat-colour sequence 35 minutes in: 72% flat
blocks, no scene cuts. Against x264 medium on the same ladder:

| clip | kind | BD-rate (VMAF-NEG) |
|---|---|--:|
| bbb_720p | 3D CGI | **-29.76%** |
| sita_720p | hand-drawn 2D | **+10.73%** |

A 40-point swing between two clips both fairly called animation. On the content
the anime literature is actually about, we lose by about 11%. The calibration
ladder shows the same thing directly: at 800 kbps next264 reads VMAF 85.87
against x264's 91.98.

So the 29.7% lead is a CGI result, not an animation result, and reporting it as
an animation result would have been wrong. This is the clearest case yet of the
standing rule that a global median hides content-dependent behaviour: here the
median of two clips would have been about -9%, which describes neither.

## The finding

**Animation is not one class for this encoder**, and the preset observation below
holds only for the CGI half.

**Our preset ladder is calibrated on natural video, and preset-for-preset is not
a quality-matched comparison on content that departs from it.** On animation our
cheaper presets already beat x264 medium on quality, so comparing medium to
medium spends work no one asked for. The same is likely true in the other
direction for content harder than the corpus.

Two things follow. Any cross-encoder speed claim should say what content class
it covers, since ours covers natural video and reads 0.82-1.16x there. And
choosing a preset from content rather than from a fixed default is worth real
speed at no quality cost, which is the same argument the per-content selector
track is making from a different direction.

## Reproducing

Two animation clips now exist and they disagree, so quote both.
`tests/corpus/sita_720p.y4m` is the hand-drawn one, 140 frames, calibrated at 900
kbps. `tests/corpus/bbb_720p.y4m` is 450 frames of 720p from 9m45s of Big Buck Bunny,
the most sustained-motion window that is not also a night scene. Its calibrated
board point is 850 kbps, in `CLIPS_CALIB` rather than `CLIPS`: adding a seventh
clip re-medians every published number, which is an owner call.

    scripts/bdcompare.py --a '...next264...' --b '...x264...' \
        --clips bbb_720p --frames 180 --vmaf --points 30,34,38,42
