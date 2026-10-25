/*
 * gpu.h - optional nextgpu backend for the mb-tree's lowres Phase A legs.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Every entry point here is a no-op that reports failure when the library is
 * not linked (-Dgpu=disabled), when Metal is unavailable, or when the runtime
 * knob is not armed -- so call sites need no #ifdef and the CPU path stays
 * complete and default. That is the contract in docs/gpu.md, and the
 * conformance gate rests on it.
 *
 * A GPU offload has to overlap the CPU to pay: blocking the driver before
 * the Phase-A parallel_for turns work that runs across the pool threads into
 * serial waiting, however fast the kernels are. Hence submit at push and
 * collect frames later, below.
 */
#ifndef YAH264_GPU_H
#define YAH264_GPU_H

#include "../dsp/metrics.h"
#include <stdint.h>

/* --- gpq: per-push quarter-pel leg batching for mb-tree Phase A ------------
 *
 * The eighth attempt's shape, and the one the seven refusals point at
 * (the local measurement records "do not reattempt without..."): submit at
 * PUSH time, when every leg pairing the new frame with the previous
 * bframes+1 frames has both inputs present (the pipelined warm's
 * input-availability refusal does not apply), and collect at WALK time,
 * frames later, so the ~2 ms DVFS round cost is hidden behind the slack
 * instead of being beaten (the synchronous refusals do not apply). Each push
 * submits one round of 2*reach SEARCHQ legs -- the superset of every leg any
 * walk can later want, since push order == display order and a walk leg's
 * display distance is at most bframes+1.
 *
 * The kernel returns, per 8x8 lowres block, the distortion-argmin QUARTER-pel
 * MV with its SATD plus the zero-MV SATD (NGC_JOB_SEARCHQ); the consumer
 * prices rate between those two candidates with its own chained predictor.
 * So the CPU-side Phase A search -- diamonds, satds, and the subpel phase
 * plane builds -- is DELETED for covered legs, not seeded.
 *
 * DETERMINISM CONTRACT. Whether a leg is covered must not depend on timing,
 * because the warm and the walk memoize each other's results: coverage is a
 * pure function of (push distance <= reach, round still resident, submits
 * healthy). The round ring holds nslots + 2*reach rounds -- strictly more
 * than any consumer can reach back -- and any GPU failure flips the handle
 * STICKY-dead (a loud one-time warning; output can differ from a healthy run
 * only across a hardware failure, the same caveat the CPU fallback always
 * carries).
 *
 * Threading: push is called by the lookahead chain only (single-threaded,
 * in push order); field by anyone (driver, pool workers) -- it fence-waits
 * the round on first touch under the handle's mutex, which is a no-op frames
 * after the submit. */
typedef struct y264_gpq y264_gpq;

/* One block's result; byte-layout of ngc_searchq_out, mirrored so encoder.c
 * does not include ngc.h. */
typedef struct { int16_t mvx, mvy; uint32_t satd, satd0; } y264_gpq_blk;

int       y264_gpq_mode(void);          /* Y264_GPU_PHASEA, default 0 */
y264_gpq *y264_gpq_open(int lw, int lh, int nslots, int reach);
void      y264_gpq_close(y264_gpq *g);
void      y264_gpq_push(y264_gpq *g, long push, int slot, const pixel *lowres);
/* max_push: the highest push index the CALLER's chain wait already guarantees
 * (derive it from the entries actually enumerated, never from a progress
 * counter). Coverage is decided against it by arithmetic -- a leg whose batch
 * boundary lies beyond it reads as uncovered on every run, so batching cannot
 * introduce timing-dependent coverage. Pass 0 for "no bound" (single-batch
 * probes only). */
const y264_gpq_blk *y264_gpq_field(y264_gpq *g, long src_push, long ref_push,
                                   long max_push);

/* Resolve this TU's env gates on the main thread (warm_lr_statics). */
void      y264_gpu_warm_statics(void);

#endif /* YAH264_GPU_H */
