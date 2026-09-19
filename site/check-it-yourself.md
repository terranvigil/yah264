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

It encodes four clips twice, once with yah264 and once with x264, at the same
quality setting on both. Then it prints how long each took, how big each file
came out, and how good each looked.

`make review-720` and `make review-1080` run half of it if you only care about
one resolution. `make review REVIEW_CRF=23` asks for higher quality.

You need x264 built alongside this repository and `ffmpeg` on your path.
[Getting started](start.md) covers the build.

## What comes back

```
clip                         time       VMAF     size
                        ours/x264  ours-x264  vs x264
bbb_720p                    1.05x      -0.38    -6.0%
perseverance_720p           1.03x      -0.05    -1.5%
bbb10s_1080p_o120           0.93x      -1.04    -4.4%
perseverance_1080p          1.08x      -0.00    -2.6%
```

Those are numbers from a real run: each clip at its own target bitrate on
both encoders, six-second windows, on an Apple M5 Max with 18 cores, against
a locally built x264 at its own defaults, with twelve threads handed to both
encoders so that other processes on the machine stay out of the ratio. The
machine was otherwise idle. Your numbers will not match it exactly. The speed
ratio moves by a few hundredths between runs on the same machine, and more
than that between machines, so treat the second decimal as noise.

There are three columns. You need all three.

**`time` is speed.** It is our encode time divided by x264's. 1.38x means x264
finished in about three quarters of the time we did. Above 1.00 we are slower.
We are behind in this column today.

**`VMAF` is quality.** VMAF is a score that tries to predict how good a video
looks to a person. This board uses its NEG variant. Every other quality
number on this site uses the same model. The default model rewards sharpening that
an encoder can add for free. NEG does not. The column is our score minus x264's. Positive means our
picture scored better.

**`size` is file size.** It is how much bigger our file is than x264's.
Negative means ours is smaller, which is the direction you want.

## Why this page and the results page disagree

The results page reads 0.96x on its ten clips. This page reads 1.04x on these
four. Both are real measurements. On the HD clips video engineers commonly
test with, we are a few percent behind today. The
[HD parity plan](https://github.com/terranvigil/yah264/blob/main/docs/hd-parity-plan.md)
is the work to close that.

The two boards differ in three ways. This page runs the two encoders as two
separate programs at twelve threads each. The results page runs both inside
one program, each at its own default thread count. This page asks both
encoders for a bitrate. The results page asks for a quality and matches the
file size afterwards. And these four clips are HD at low to mid bitrate. That
is the regime we are slowest in. The ten-clip board also has small CIF clips
and high-bitrate HD, where we are fastest.

An earlier version of this page read 1.33x to 1.40x. That run handed every
core to both encoders, compared them at the same CRF number although the two
CRF scales differ, and ran on a busy machine. All three are fixed above.

## How to read them together

This is the part that trips people up.

A video encoder trades size against quality continuously. You can always make a
smaller file by accepting a worse picture. So a size number on its own means
nothing. The same goes for a quality number on its own.

**When both point the same way, the answer is unambiguous.** In the table above,
`perseverance_1080p` came out 10.5% smaller *and* scored better. That is a real
win, no interpretation required. `bbb10s_1080p_o120` came out 9.3% bigger *and*
scored worse. That is a real loss. It stays in the table because three of the
four rows favour us and one does not. We print the losing row.

**When they point opposite ways, one run cannot tell you.** A file that is 10%
smaller and scores slightly worse might be better or worse value than the
alternative. Answering that needs the whole curve. That is what a BD-rate
is: encode at several qualities, plot size against score for both encoders, and
measure the gap between the curves. The
[results](results.md) page uses BD-rate for exactly this reason.

## Why the same setting gives different sizes

Both encoders are given the same CRF number. CRF means "constant quality" --
you ask for a quality level and the encoder spends whatever bits that takes.

The catch is that CRF 26 does not mean the same thing to both encoders. The two
scales were calibrated independently and are simply different, by an amount that
depends on the clip. So handing both encoders CRF 26 does not put them at the
same operating point. The different sizes come from the scales, not from one
encoder being better.

That is fine for a speed question. That is what this board asks. Both
encoders did a real, complete encode of the same source. It is not fine for a
quality verdict. Quality claims on this site come from BD-rate at matched
bitrates for that reason.

## Why these four clips

```
             720p                    1080p
CGI          bbb_720p                bbb10s_1080p_o120
camera       perseverance_720p       perseverance_1080p
```

Two things move an encoder speed comparison more than anything else. One is
the resolution. The other is how synthetic the content is. Computer-generated frames are
clean and compress unusually well, so an encoder measured only on them looks
better than it is. Real camera footage has sensor noise and real motion.

Big Buck Bunny is an open-source animated film. Perseverance is footage from
NASA's Mars rover. Each appears at both resolutions as the same content, so the
720p and 1080p rows differ by resolution and nothing else.

Four clips is a small sample on purpose. It runs in a few minutes. We hold
longer films too, Meridian, Chimera, Sparks and Tears of Steel among them.
Windows from those are being measured now and will join this board.

## The longer numbers

The [results](results.md) page measures the same thing more carefully. Both
encoders run as libraries in one process, so startup is not timed. Each is
solved onto the same bitrate, so neither gets credit for spending fewer bits.
That board needs a patched ffmpeg. This page exists so you can check without
one.

Two habits if you re-run any of this. Say which board a number came from,
because the two differ by a steady margin. Don't settle anything on one run.
The spread on a speed ratio between machines and between days is about 0.10.
