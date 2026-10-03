/*
 * iprof.h - retired instructions by encoder stage, exact, from the thread's
 * own instruction counter. A measurement build only: compile with
 * -DY264_IPROF (meson: -Dc_args=-DY264_IPROF in a dedicated build dir);
 * without it every macro is empty.
 *
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Model: nested scopes, the innermost open scope owns the instructions
 * (a parent pauses while a child runs), per thread. Each counter read costs
 * the calling thread a fixed number of instructions, measured at start-up
 * and taken back off the scope that paid it. Totals print at exit.
 */
#ifndef Y264_IPROF_H
#define Y264_IPROF_H

enum {
    IP_ME,          /* integer motion search (y264_me_search minus sub-pel) */
    IP_SUBPEL,      /* sub-pel refinement inside the motion search */
    IP_RDTRIAL,     /* an RD trial: residual coding, reconstruction, distortion */
    IP_RDBITS,      /* the trial's bit estimate */
    IP_COMMIT,      /* the winner's re-encode with the trellis */
    IP_INTRA,       /* intra analysis and trials */
    IP_EMIT,        /* the real entropy coding */
    IP_DEBLOCK,
    IP_LA,          /* lookahead (lowres build, costs, frame types) */
    IP_MBTREE,
    IP_MBDEC,       /* the rest of a macroblock's analysis: skip and direct
                     * candidates, prediction builds, screens, bookkeeping */
    IP_HPEL,        /* half-pel planes and reference borders */
    IP_N
};

#ifdef Y264_IPROF
void y264_ip_beg(int bucket);
void y264_ip_end(void);
void y264_ip_scope_end(int *unused);
#define IP_SCOPE(b) __attribute__((cleanup(y264_ip_scope_end))) int ip_scope_##b = (y264_ip_beg(b), 0)
#define IP_SCOPE_B(e) __attribute__((cleanup(y264_ip_scope_end))) int ip_scope_b_ = (y264_ip_beg(e), 0)
#define IP_BEG(b) y264_ip_beg(b)
#define IP_END() y264_ip_end()
#else
#define IP_SCOPE(b) ((void)0)
#define IP_SCOPE_B(e) ((void)0)
#define IP_BEG(b) ((void)0)
#define IP_END() ((void)0)
#endif

#endif
