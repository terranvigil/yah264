---
title: Methodology - yah264
description: How an AI agent built an encoder under measurement gates, dead ends included.
---

# Methodology

An encoder is a good test of sustained engineering by an AI agent, because
almost nothing about it can be faked. A coding tool reconstructs bit-exactly or
it doesn't. A speed claim survives a repeated table run or it doesn't. This
page covers the loop that produced the site, failures included.

## The loop

A session picks one idea, prices it, gates it, then ships or refuses it. I
wrote almost none of this loop in advance. It survived months of doing it badly.

Most ideas die at pricing. Before writing an optimization, build the oracle
that says what a perfect version would be worth. If a perfect early-skip is
worth eleven percent and a realistic one gets a third of that, it's worth a
day. If the ceiling is half a percent, no implementation skill saves it.
Several ideas below died on their ceiling before any encoder code existed.

## What decides whether work ships

Three gates decide, none of them open to argument. Recon-match decides
correctness: the encoder's reconstruction must equal ffmpeg's decode bit for
bit before a change counts as done. BD-rate over a solved rate range, on
full-frame [VMAF](https://en.wikipedia.org/wiki/Video_Multimethod_Assessment_Fusion)
v1 at matched achieved bitrates, decides quality. A change that helps one clip
and costs two is refused, however elegant.

The table decides speed, as our wall time over x264's at a matched operating
point. Even a ratio is tied to its setup. The [results](results.md) page names the
machine and builds because:

- A repeated board on the same machine moves a ratio by up to about 0.10.
- Multi-threaded rows depend on the core count.
- Pure-C rows need a specially built x264.
- The headline board needs an ffmpeg that isn't in this repository.

Nothing ships on a plausible mechanism. A language model's recurring failure is
a confident explanation of why an idea should work. I suspect that's the most
useful thing to know when pointing one at a problem like this. The gates
exist because the explanation is worth nothing alone.

## The instrument catalog

The costliest recurring mistake wasn't about encoding. A session would spend
its first hour building a profiler, oracle or A/B harness that already existed
under a forgotten name. About eighty harness scripts and 140 environment knobs
won't fit in one context window.

The fix is an [instrument catalog](https://github.com/terranvigil/yah264/blob/main/docs/instruments.md),
organized by question: where did the time go, what would a perfect X buy, is
the idea any good, is it fast, is it correct. A census fails the build when a
listed knob goes missing from the source. Instrument names are checked by hand
on edit. Because the project instructions make reading it the first step of
any measurement, the rule survives a fresh session.

## The ideas that were measured and dropped

A record of only the wins is a highlight reel. These were measured and dropped:

- A learned transform-size classifier lost to the cheap screen it was to replace.
- GPU lookahead hit a per-process Metal floor of 12 to 17 milliseconds per cell.
- Motion-estimation early-out rescues failed three times. A fourth was refused
  on that record.
- No pre-motion signal predicts the B-skip verdict, as an offline kill test
  showed before anything was built on it.

Each closed a direction the next session would otherwise reopen.

## Traps that each cost a day

- **Shell quoting.** zsh doesn't word-split unquoted scalars, so arguments
  built in a variable arrive as one. An A/B loop then runs both sides alike
  and silently reports a clean null. It invalidated a whole measurement round.
- **Ceilings from two runs.** Subtracting measurements taken at different
  times gives mostly drift. A ceiling has to be head to head.
- **Tolerance wider than the margin.** A rate-matched table can't decide a
  quality difference smaller than its rate tolerance.
- **Cache staleness.** A tool that caches encodes will compare a fresh binary
  against a stale result.
- **Presence-based knobs.** If a knob is read for presence, `KNOB=0` and
  `KNOB=1` both turn it on. Both arms match and the knob looks inert.

## Clean-room rules

yah264 is written from scratch. Other encoders were only baselines we built and
timed so every goal had a real number. Transliterating another implementation,
or carrying over its structure, naming or statement order, is still copying.
`CONTRIBUTING.md` has the full rules.
