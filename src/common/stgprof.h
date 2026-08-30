/*
 * stgprof.h - fine-grained stage profiler for the pure-C speed-parity work
 * (docs/pure-c-speed-parity.md). Attributes the per-MB analyze "orchestration"
 * that the frame-level Y264_THREAD_PROF buckets lump into TP_ANALYZE.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Compile-time gated: build with -DY264_STAGE_PROF (meson:
 * -Dc_args=-DY264_STAGE_PROF in a dedicated build dir), mirroring ledger.{h,c}.
 * ZERO cost when compiled out (the macros expand to nothing).
 *
 * Model: a nesting stack of scoped timers. STG_BEG(b) opens a region billed to
 * bucket b; STG_END closes it. Time is always attributed to the INNERMOST
 * open region, so a save_mb_rec timed inside an ME region does not double-count
 * -- the parent's clock pauses for the duration of the child. Single-threaded
 * measurement only (plain accumulation, no atomics); run with --threads 1.
 *
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef Y264_STGPROF_H
#define Y264_STGPROF_H

#ifdef Y264_STAGE_PROF

enum {
    STG_SKIP,       /* P/B skip candidate: MC + dist_mb + snap */
    STG_PROBE,      /* probe_pskip / probe_skip early-skip test */
    STG_ME,         /* eval_inter_part: motion search + SATD partition rank */
    STG_INTERRD,    /* inter_rd_score + qpel_rd_nudge + insurance RD (residual) */
    STG_INTRA,      /* intra admit + analyze_intra_g + rate */
    STG_SNAP,       /* save_mb_rec / load_mb_rec reconstruction snapshots */
    STG_DECIDE,     /* per-MB mode decision + bookkeeping (the remainder) */
    STG_DEBLOCK,    /* y264_deblock_frame */
    STG_PREP,       /* build_slice_prep (hpel plane build) */
    STG_EMIT,       /* entropy emit authoring */
    STG_BSKIP,      /* B skip/direct build */
    STG_BME,        /* B inter mode build+search */
    STG_BRD,        /* B RD scoring */
    STG_OTHER,      /* anything explicitly parked */
    STG_N
};

void y264_stg_beg(int bucket);
void y264_stg_end(void);
void y264_stg_count(int bucket);

#define STG_BEG(b)   y264_stg_beg(b)
#define STG_END()    y264_stg_end()
#define STG_COUNT(b) y264_stg_count(b)   /* bump a call counter without timing */

#else

#define STG_BEG(b)   ((void)0)
#define STG_END()    ((void)0)
#define STG_COUNT(b) ((void)0)

#endif

#endif
