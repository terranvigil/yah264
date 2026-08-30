/*
 * gpu.h - optional nextgpu backend for the lookahead's lowres motion search.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Every entry point here is a no-op that reports failure when the library is
 * not linked (-Dgpu=disabled), when Metal is unavailable, or when the runtime
 * knob is not armed -- so call sites need no #ifdef and the CPU path stays
 * complete and default. That is the contract in
 * docs/gpu-shared-library-design.md, and the conformance gate rests on it.
 *
 * Scope, deliberately narrow: this replaces the WIDE INTEGER half of the lowres
 * search and nothing else. Our lowres is quarter-pel and the library's search is
 * integer-pel, and that precision is load-bearing (it is why blk8_inter_coh
 * exists), so the GPU supplies a starting MV from an exhaustive integer window
 * and the CPU keeps its subpel refinement around it.
 *
 * THE API IS SPLIT SUBMIT/WAIT, AND THAT IS THE WHOLE POINT. The first version
 * of this was synchronous and measured 1.86x SLOWER at t12 -- at a search range
 * where the GPU did essentially no work it was still 1.33x, because blocking the
 * driver before the Phase-A parallel_for converts work that ran across twelve
 * pool threads into serial waiting. A GPU offload that does not overlap the CPU
 * cannot pay however fast its kernels are. docs/archive/goal3-routes-3456.md.
 */
#ifndef YAH264_GPU_H
#define YAH264_GPU_H

#include "../dsp/pixel.h"
#include <stdint.h>

typedef struct y264_gpu y264_gpu;

/* NULL when the library is not linked or the mode is not armed. Metal itself
 * is probed LAZILY at the first begin (the device + pipeline build is tens
 * of ms once per process, which an armed encode that never submits must not
 * pay), so a non-NULL handle means "armed", and a Metal-less box is a begin
 * that returns 0 -- the CPU path, like every other failure here. */
y264_gpu *y264_gpu_open(int max_lw, int max_lh, int max_planes, int max_legs);
void      y264_gpu_close(y264_gpu *g);

/* Y264_GPU_LOWRES: 0 = off (default), 1 = on.
 *
 * There was a mode 2 (two passes, the first field feeding the second as a
 * neighbour predictor, to recover the coherence a sequential CPU search gets
 * from its left neighbour). It is GONE, on measurement: its BD was
 * indistinguishable from mode 1 (median +0.03% against +0.04% on four clips),
 * and it costs a second round-trip that would force a wait between the passes --
 * which is exactly the serialization this file now exists to avoid. */
int y264_gpu_lowres_mode(void);

/* Batch building. begin resets; plane uploads a distinct lowres plane and
 * returns its slot, DEDUPED BY POINTER so a reference shared by many legs is
 * copied once (the walk's legs share anchors heavily, and the copies were real
 * overhead in the synchronous version); leg records one (cur, ref) search
 * writing 2*nblk int16 to `out`. All return <0 / 0 on overflow, and the caller
 * then simply runs the CPU search. */
int y264_gpu_lowres_begin(y264_gpu *g, int lw, int lh);
int y264_gpu_lowres_plane(y264_gpu *g, const pixel *p);
int y264_gpu_lowres_leg(y264_gpu *g, int cur_slot, int ref_slot, int16_t *out);

/* Submit every recorded leg as ONE batch and return WITHOUT waiting, so the
 * caller can do CPU work before wait collects. Returns 1 if a batch is in
 * flight. wait blocks, reads back, and returns 1 on success; on any failure
 * the caller must fall back to the CPU search, which it must always be able
 * to do. Calling wait without a submit in flight is a no-op returning 0. */
int y264_gpu_lowres_submit(y264_gpu *g, int range);
int y264_gpu_lowres_wait(y264_gpu *g);

/* --- gpq: per-push quarter-pel leg batching for mb-tree Phase A ------------
 *
 * The eighth attempt's shape, and the one the seven refusals point at
 * (docs/archive/goal3-gpu-reattempt-2.md "do not reattempt without..."): submit at
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
