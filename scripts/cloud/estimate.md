<!--
Copyright (c) 2026, the yah264 authors
SPDX-License-Identifier: GPL-2.0-or-later
-->

# The cloud campaign: estimate against actual

The plan's session table (docs/x86-plan.md, "The cloud campaign") with a column
for what each session really cost. Fill the actual column in after the session,
from the instance's own billing page, and commit it. An estimate nobody checks
against a bill is a guess that gets quoted as a budget.

Prices were read on 2026-09-17 and are per-second after a one-minute minimum.
No account, project, bucket or key is named here or anywhere else in this tree:
the operator supplies `BUCKET_URL` in the environment at run time.

## Sessions

| session | boxes | mode | est. hours | est. $ (both) | actual hours | actual $ | notes |
|---|---|---|---|---|---|---|---|
| A | c3-highcpu-8 (Sapphire Rapids) + c3d-highcpu-16 (Genoa) | spot | ~4 each | ~$1.75 (on-demand ~$3.80) | | | bootstrap, checkasm including AVX-512 on real silicon, SDE, conformance, identity |
| B | c3-highcpu-22 + c3d-highcpu-16 | on-demand | up to 3x8 each | ~$37 ceiling | | | the board: perf-comp t1 pinned and tN against both x264 arms, ffboard, bd_at_rate, the AVX-512 vs AVX2 A/B |
| **total** | | | | **~$40 on-demand ceiling** | | | $0 if the trial credit's vCPU quota admits these shapes |

B runs only after A is clean on both vendors. Never spot a timing series: a
preemption mid-table does not announce itself in the numbers.

## What each session's wall budget should be

`WALL_BUDGET_H` is the guard, not the plan: it pushes partial results and stops
before the budget runs out, holding back five minutes for the push. Set it
below the hours you are willing to pay for, never at them.

| session | box | suggested WALL_BUDGET_H | why |
|---|---|---|---|
| A | c3-highcpu-8 | 3.5 | bootstrap is the long pole; the ffmpeg fork is most of it |
| A | c3d-highcpu-16 | 3.0 | same work, more cores for the builds |
| B | c3-highcpu-22 | 7.5 | the boards at two thread counts against two x264 arms |
| B | c3d-highcpu-16 | 7.5 | as above |

## The one-time staging cost

The bucket holds the corpus (~12 GB), the JM and openh264 archives and the SDE
tarball, in the campaign's own region. Ingress is free; the storage is pennies
a month; the instances read it over the region's internal network, which is
both free and much faster than pulling the corpus from its origin each session.

| item | size | cost |
|---|---|---|
| corpus tarball | ~12 GB | storage only |
| JM + openh264 archives | ~20 MB | storage only |
| SDE tarball | ~100 MB | storage only, staged by hand (Intel's download is a click-through) |
| results tarballs out | tens of MB | egress $0.12/GB after the first 1 GiB |

## Actual spend log

One line per instance, added after it is terminated.

| date | session | sku | wall | $ | outcome |
|---|---|---|---|---|---|
| | | | | | |
