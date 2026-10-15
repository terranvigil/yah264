---
title: Check it yourself - yah264
description: Run yah264 and x264 on the same clip and compare speed, file size and quality with one command.
---

# Check it yourself

One command runs both encoders on the same clips and prints how fast each
was, how big the files came out, and how good they look. This page explains
those three numbers.

## The one command

```
make review
```

It encodes four HD clips twice, once with yah264 and once with x264, with both
aiming at the same bitrate. Then it prints how long each took, how big each
file came out, and how good each looked. `make review-720` and
`make review-1080` run half of it.

You need x264 built alongside this repository and `ffmpeg` on your path.
[Getting started](start.md) covers the build.

## What comes back

```
clip                         time       VMAF     size    PSNR-Y
                        ours/x264  ours-x264  vs x264 ours-x264
bbb_720p                    0.88x      -0.51    -7.1%     +1.32
perseverance_720p           0.86x      +0.10    -2.5%     -0.09
bbb10s_1080p_o120           0.74x      -0.15    -1.8%     -0.88
perseverance_1080p          0.78x      +0.12    -3.1%     -0.81
MEDIAN                      0.82x
```

These numbers are from one real run on an idle Apple M5 Max (18 cores): each
clip at its own bitrate, six-second windows, both encoders given twelve
threads, x264 at its defaults. Your run will differ by a few hundredths, so
ignore the second decimal.

**`time`** is our encode time over x264's. Below 1.00 we're faster: 0.82x
means we took about four fifths of x264's time.

**`VMAF`** is quality: our score minus x264's, with VMAF's NEG model (used
everywhere on this site), which doesn't reward free sharpening. Positive means
ours looks better.

**`size`** is how much bigger our file is than x264's. Negative means ours is
smaller.

**`PSNR-Y`** is a second, simpler quality score in decibels. The site's floor
is that no clip may fall more than 1 dB behind x264.

## Reading size and quality together

You can always make a smaller file by accepting a worse picture, so neither
number means much on its own.

When both point the same way, the answer is clear. `perseverance_1080p` came
out 3.1% smaller and scored slightly better. That's a real win.

When they point opposite ways, one run can't tell you. `bbb_720p` came out 7.1%
smaller and scored half a point worse. That might be better or worse value.
Answering it needs the whole size-against-quality curve for both encoders,
which is what a BD-rate measures. The [results](results.md) page uses BD-rate
for exactly this reason.

## This page and the results page

This board is the quick check: four HD clips, the two encoders run as separate
programs at twelve threads each, and both asked for a bitrate. The results
page is the full test: clips from CIF to 4K, real titles up to ten minutes,
each encoder at its own default thread count, compared at the same file size
and across a range of rates. The two now agree that yah264 is faster. In
September, before parity, this board read 1.04x.

## Why these four clips

```
             720p                    1080p
CGI          bbb_720p                bbb10s_1080p_o120
camera       perseverance_720p       perseverance_1080p
```

Resolution and how synthetic the content is move a speed comparison more than
anything else. Computer-generated frames are clean and compress unusually
well, so an encoder measured only on them looks better than it is. Real camera
footage has sensor noise and real motion.

Big Buck Bunny is an open-source animated film. Perseverance is footage from
NASA's Mars rover. Each appears at both resolutions, so the 720p and 1080p rows
differ by resolution and nothing else. Four clips keeps the run to a few
minutes. The long titles are on the [results](results.md) page.

Don't settle anything on one run. A speed ratio moves by up to about 0.10
between machines and between days.
