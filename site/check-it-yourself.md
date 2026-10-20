---
title: Check it yourself - yah264
description: Run yah264 and x264 on the same clip and compare speed, file size and quality with one command.
---

# Check it yourself

One command runs both encoders on the same clips. It prints how fast each
was, how big the files came out and how good they look. This page explains
those three numbers.

## Run it

```
scripts/fetch_corpus.sh --review
X264_ASM=$(command -v x264) make review
```

The first line fetches the four test clips (about 250 MB). The second encodes them twice, once with yah264 and once with x264. Both
aim at the same bitrate. Then it prints how long each took, how big each file
came out and how good each looked. `make review-720` and
`make review-1080` run half of it.

You need `x264`, `ffmpeg` and `vmaf` on your path. [Getting started](start.md)
covers the build.

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

These numbers come from one real run on an idle 18-core Apple M5 Max. Each
clip ran at its own bitrate over a six-second window. Both encoders got twelve
threads, and x264 ran at its defaults. Your run will differ by a few
hundredths. Ignore the second decimal.

**`time`** is our encode time over x264's. Below 1.00 we're faster: 0.82x
means we took about four fifths of x264's time.

**`VMAF`** is quality: our score minus x264's. It uses VMAF's NEG model, which
doesn't reward free sharpening and is the one used everywhere on this site.
Positive means ours looks better.

**`size`** is how much bigger our file is than x264's. Negative means ours is
smaller.

**`PSNR-Y`** is a second, simpler quality score in decibels. The site's floor
is that no clip may fall more than 1 dB behind x264.

`perseverance_1080p` can dip just past it and print `PSNR-FLOOR FAIL`. Our file
there is about 6% smaller at equal VMAF, so the gap is mostly bits we didn't
spend.

## Reading size and quality together

You can always make a smaller file by accepting a worse picture, so neither
number means much on its own.

When both point the same way, the answer is clear. `perseverance_1080p` came
out 3.1% smaller and scored slightly better. That's a real win.

When they point opposite ways, one run can't tell you. `bbb_720p` came out 7.1%
smaller and scored half a point worse. That might be better or worse value.
Answering it needs the whole size-against-quality curve for both encoders.
A BD-rate measures that curve. The [results](results.md) page uses BD-rate
for exactly this reason.

## This page and the results page

This board is the quick check. It runs four HD clips through the two
encoders as separate programs at twelve threads each, and asks both for a
bitrate. The results page is the full test. It covers clips from CIF to 4K and
real titles up to ten minutes, runs each encoder at its own default thread
count, and compares them at the same file size across a range of rates.

## Why these four clips

```
             720p                    1080p
CGI          bbb_720p                bbb10s_1080p_o120
camera       perseverance_720p       perseverance_1080p
```

Resolution and how synthetic the content is move a speed comparison more than
anything else. Computer-generated frames are clean and compress unusually
well. An encoder measured only on them looks better than it is. Real camera
footage has sensor noise and real motion.

Big Buck Bunny is an open-source animated film. Perseverance is footage from
NASA's Mars rover. Each appears at both resolutions, so the 720p and 1080p rows
differ by resolution and nothing else. Four clips keeps the run to a few
minutes. The long titles are on the [results](results.md) page.

Don't settle anything on one run. A speed ratio moves by up to about 0.10
between machines and between days.
