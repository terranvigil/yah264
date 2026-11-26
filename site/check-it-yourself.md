---
title: Check it yourself - yah264
description: Run yah264 and x264 on the same clip and compare speed, file size and quality with one command.
---

# Check it yourself

One command runs both encoders on the same clips and prints speed, file size
and quality. This page explains those numbers.

## Run it

```
scripts/fetch_corpus.sh --review
X264_ASM=$(command -v x264) make review
```

The first line downloads two public videos into `tests/corpus/.src/` and cuts
six Y4M windows. The second encodes four of them with each encoder at the same
bitrate. `make review-720` and `make review-1080` run half.

| download and disk | size |
|---|--:|
| the two source videos | about 530 MB |
| all six Y4M windows | about 7.8 GB |
| the four that `make review` reads | 3.6 GB |

You need `x264` and `ffmpeg` on your path, plus a float-enabled libvmaf 3.x
`vmaf` CLI with Netflix's VMAF v1.0.16 models, at
`~/.local/opt/vmaf-v1/bin/vmaf` and `~/.local/share/vmaf/model` or wherever
`VMAF_V1_BIN` and `VMAF_V1_MODELS` point. See [getting started](start.md) for
the build.

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

That's one run on an idle 18-core Apple M5 Max: each clip at its own bitrate
over six seconds, twelve threads each, x264 at its defaults. Yours will differ
by a few hundredths, so ignore the second decimal.

| column | what it is |
|---|---|
| `time` | our encode time over x264's. Below 1.00 we're faster: 0.82x means about four fifths of x264's time. |
| `VMAF` | our VMAF v1 score minus x264's, at display resolution. v1 doesn't reward free sharpening and sees banding and chroma. |
| `size` | our file against x264's. Negative means ours is smaller. |
| `PSNR-Y` | a simpler score in decibels. The site's floor is no clip more than 1 dB behind x264. |

`perseverance_1080p` can dip just past that and print `PSNR-FLOOR FAIL`. Our
file there is about 6% smaller at equal VMAF. Most of the PSNR gap is bits we didn't
spend.

## Reading size and quality together

A worse picture always buys a smaller file, so neither number stands alone.
When both point the same way the answer is clear: `perseverance_1080p` came out
3.1% smaller and scored slightly better. When they disagree, one run can't
tell. `bbb_720p` came out 7.1% smaller and half a point worse, and judging that
needs the whole size-quality curve for both encoders. That curve is the
BD-rate the [results](results.md) page uses.

## This page and the results page

This board is the quick check. The results page is the full test:

| | this board | the results page |
|---|---|---|
| clips | four HD clips | CIF to 4K, and real titles up to ten minutes |
| encoders run as | separate programs | command-line programs for the parity tables, libraries in one ffmpeg for the ten-clip board |
| threads | twelve each | each encoder's own default |
| compared at | the same requested bitrate | the same file size, across a range of rates |

## Why these four clips

```
             720p                    1080p
CGI          bbb_720p                bbb10s_1080p_o120
camera       perseverance_720p       perseverance_1080p
```

Resolution and how synthetic the content is move a speed comparison most.
Clean computer-generated frames compress unusually well and flatter an
encoder. Camera footage has sensor noise and real motion. Big Buck Bunny is an
open-source animated film, Perseverance footage from NASA's Mars rover. Because each
appears at both resolutions, the rows differ only by resolution. Four clips
keep the run to a few minutes.

Don't settle anything on one run. A speed ratio moves by up to about 0.10
between machines and days.
