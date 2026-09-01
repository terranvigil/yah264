---
title: Methodology - yah264
description: How an AI agent built an encoder under measurement gates, dead ends included.
---

# Methodology

An encoder is a good test of whether an AI agent can do sustained engineering
work, because almost nothing about it can be faked. A coding tool either
reconstructs bit-exactly or it does not. A speed claim either survives a
repeated table run or it does not. This page is about the loop that produced the
rest of the site, including the parts that failed.

## The loop

A session picks one idea, prices it, gates it, and then ships or refuses it. I
wrote almost none of this loop in advance. It is what survived after the first
few months of doing it badly.

Pricing comes first because most ideas die there. Before writing an
optimization, build the oracle that says what a perfect version of it would be
worth. If a perfect early-skip decision is worth eleven percent and the
realistic version captures a third of that, the idea is worth a day. If the
ceiling comes back at half a percent, it is not, and no amount of implementation
skill changes that. Several of the ideas below were killed by their own ceiling
before a line of encoder code was written.

## What decides whether work ships

Three gates decide whether work ships, and none of them takes an argument.

Recon-match decides correctness. The encoder's reconstruction must equal
ffmpeg's decoder output bit for bit, and it runs before a change counts as
done.

BD-rate over a solved rate range decides quality, measured with full-frame [VMAF](https://en.wikipedia.org/wiki/Video_Multimethod_Assessment_Fusion)
at matched bitrates. A change that improves one clip and costs two others is a
refusal even when the mechanism is elegant.

The table decides speed, as a ratio to x264's wall time at a matched operating
point, so the bar reproduces on any machine.

Nothing ships on a plausible mechanism. The recurring failure mode of a language
model is a confident explanation of why an idea should work, and I suspect that is
the single most useful thing to know when pointing one at a problem like this.
The gates exist because that explanation is worth nothing on its own.

## The instrument catalog

The most expensive recurring mistake had nothing to do with encoding. A session
would spend its first hour building a profiler, an oracle, or an A/B harness
that already existed under a name nobody remembered. The tree holds around 120 harness
scripts and 286 environment knobs, which is well past what fits in one context
window.

`docs/instruments.md` is the fix. It catalogs the instruments that have
produced results, organized by the question each one answers: where did the time
go, what would a perfect version of X buy, is the idea any good, is it fast, is
it correct. Every name on it is verified present. Reading it is the first step
of any measurement task, and that rule is written into the project instructions
so it survives a fresh session.

## The ideas that were measured and dropped

A record that publishes only the wins is a highlight reel. These were built or
bounded, measured, and dropped:

- A learned transform-size classifier came in worse than the cheap screen it was
  meant to replace.
- GPU lookahead hit a per-process Metal floor of 12 to 17 milliseconds, which
  every table cell would have paid.
- The motion-estimation early-out rescues were tried three times and the fourth
  was refused on the strength of the first three.
- A pre-motion signal that would predict the B-skip verdict does not exist, and
  a kill test established that offline before anything was built around it.

Each of those is a real number, and each closed a direction that would otherwise
be reopened by the next session with the same good idea.

## Traps that each cost a day

- **Shell quoting.** The shell here is zsh, which does not word-split unquoted
  scalars. Building arguments in a variable and expanding it bare passes one
  argument instead of several, so an A/B loop runs both sides with the same
  settings and reports a clean null result. It fails silently, and it invalidated
  a whole measurement round before anyone checked the arguments reaching the
  binary.
- **Ceilings built from two separate runs.** Subtracting one measurement from
  another taken at a different time gives a number that is mostly drift. A
  ceiling has to be head to head.
- **Tolerance wider than the margin.** A rate-matched table cannot decide a
  quality difference smaller than its own rate tolerance. Quoting two decimals
  from it is quoting noise.
- **Cache staleness.** A comparison tool that caches encodes will happily
  compare a fresh binary against a stale result after a rebuild.
- **Presence-based knobs.** An environment knob read for presence and not for
  value means `KNOB=0` and `KNOB=1` both turn it on, so both sides of a
  comparison run identically and the knob looks inert.

## Clean-room rules

yah264 is written from scratch. Other encoders were used as measurement
baselines and nothing else: built, run and timed, so that every goal had a real
number to match.

Transliterating another implementation into a different style is still copying.
Anyone who has recently read another encoder's source should not be the one to
author mode decision, entropy coding, rate control or motion estimation here.
`CONTRIBUTING.md` has the full rules, and a provenance audit measured the
result against a public baseline.
