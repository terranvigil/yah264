---
title: Check it yourself - yah264
description: Run yah264 and x264 on the same clip and compare speed, file size and quality with one command.
---

# Check it yourself

Every number on this site comes from a command you can run. This page is the
short version: one command, three numbers, and what they mean.

## The one command

```
make review
```

It encodes four clips twice, once with yah264 and once with x264, using the
same quality setting on both, and prints how long each took, how big each file
came out, and how good each looked.

`make review-720` and `make review-1080` run half of it if you only care about
one resolution. `make review REVIEW_CRF=23` asks for higher quality.

You need x264 built alongside this repository and `ffmpeg` on your path.
[Getting started](start.md) covers the build.

## What comes back

```
clip                       x264 x      dVMAF    dsize
bbb_720p                    1.40x      +0.18   -13.5%
perseverance_720p           1.34x      +0.11    -2.8%
bbb10s_1080p_o120           1.38x      -3.10    +9.3%
perseverance_1080p          1.33x      +0.49   -10.5%
```

That is a real run, not an illustration. Your numbers will not match it exactly:
the speed ratio moves by a few hundredths between runs on the same machine, and
more than that between machines, so treat the second decimal as noise.

Three columns, and you need all three.

**`x264 x` is speed.** 1.37x means x264 finished in about three quarters of the
time we did. Above 1.00 we are slower. This is the column where we are behind
today, and the site does not hide it.

**`dVMAF` is quality.** VMAF is a score that tries to predict how good a video
looks to a person, rather than counting how many pixels changed. Positive means
our picture scored better.

**`dsize` is file size.** Negative means our file is smaller, which is the
direction you want.

## How to read them together

This is the part that trips people up, so here is the rule.

A video encoder trades size against quality continuously. You can always make a
smaller file by accepting a worse picture. So a size number on its own means
nothing, and a quality number on its own means nothing.

**When both point the same way, the answer is unambiguous.** In the table above,
`perseverance_1080p` came out 10.5% smaller *and* scored better. That is a real
win, no interpretation required. `bbb10s_1080p_o120` came out 9.3% bigger *and*
scored worse. That is a real loss, and it stays in the table for that reason:
three of the four rows favour us and one does not, which is what an honest
four-row sample looks like.

**When they point opposite ways, one run cannot tell you.** A file that is 10%
smaller and scores slightly worse might be better or worse value than the
alternative. Answering that needs the whole curve rather than one point, which
is what a BD-rate is: encode at several qualities, plot size against score for
both encoders, and measure the gap between the curves. The
[results](results.md) page uses BD-rate for exactly this reason.

## Why the same setting gives different sizes

Both encoders are given the same CRF number. CRF means "constant quality" --
you ask for a quality level and the encoder spends whatever bits that takes.

The catch is that CRF 26 does not mean the same thing to both encoders. The two
scales were calibrated independently and are simply different, by an amount that
depends on the clip. So handing both encoders CRF 26 does not put them at the
same operating point, and that is why the sizes come out different rather than
because one encoder is better.

That is fine for a speed question, which is what this board is for: both
encoders did a real, complete encode of the same source. It is not fine for a
quality verdict, which is why quality claims on this site come from BD-rate at
matched bitrates instead.

## Why these four clips

```
             720p                    1080p
CGI          bbb_720p                bbb10s_1080p_o120
camera       perseverance_720p       perseverance_1080p
```

Two things move an encoder speed comparison more than anything else: the
resolution, and how synthetic the content is. Computer-generated frames are
clean and compress unusually well, so an encoder measured only on them looks
better than it is. Real camera footage carries sensor noise and real motion.

Big Buck Bunny is an open-source animated film. Perseverance is footage from
NASA's Mars rover. Each appears at both resolutions **as the same content**, so
the 720p and 1080p rows differ by resolution and nothing else.

Four clips is not a large sample, and it is not meant to be the last word. It is
meant to be small enough that a person can run it in a few minutes and see for
themselves.

## The longer numbers

The headline goals on the [results](results.md) page are measured differently
and more carefully: both encoders run as libraries inside one process, so no
part of the measurement is process startup, and each encoder is solved onto the
same achieved bitrate so neither gets credit for spending fewer bits. That board
needs a patched ffmpeg and is not a one-command run, which is why this page
exists alongside it.

Two habits worth borrowing if you re-run any of this. Say which board a number
came from, because the two disagree by a consistent margin and comparing across
them looks like a regression that is not there. And do not settle anything on a
single run: the machine-to-machine and day-to-day spread on a speed ratio here
reaches about 0.10, which is wider than some of the margins being argued about.
