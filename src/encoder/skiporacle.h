/*
 * skiporacle.h - measurement-only bound on the late-skip cost.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * At a low-bitrate operating point 42.6% of P and 62.0% of B macroblocks end up
 * coded as skip, but our early probe catches only ~16% of each; the rest run a
 * full ME + intra + RD tournament and are then discarded. x264 catches its skips
 * up front (a lambda-scaled gate plus a decimate-tolerant probe) and that is why
 * its satd volume falls 32% into low bitrate while ours does not move.
 *
 * "Write a better early test" is a quality question and needs a BD round. But
 * "what is a better early test WORTH" is answerable exactly, with no quality
 * argument at all: record each MB's final verdict, replay it as an early exit,
 * and time that. Where the verdict is skip either way the reconstruction is the
 * same P_Skip/B_Skip, so a correct oracle run is BYTE-IDENTICAL to a normal one
 * -- which is also the check that the oracle is wired up honestly. Its wall time
 * is the ceiling any early-skip predictor can reach.
 *
 * Y264_SKIP_ORACLE=<path> pass 1 (file absent) records, pass 2 (present) replays.
 * Measurement only; never on in a shipping path.
 *
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef Y264_SKIPORACLE_H
#define Y264_SKIPORACLE_H

/* mode: 0 = disabled, 1 = recording, 2 = replaying */
int  y264_skor_mode(void);
/* Replay: 1 = this MB's final verdict was skip. Recording/disabled: 0. */
int  y264_skor_ask(int poc, int is_b, int mbx, int mby, int wmb);
/* Record this MB's final verdict. */
void y264_skor_put(int poc, int is_b, int mbx, int mby, int wmb, int skip);

/* Which side replays, and WHERE the replayed exit is taken. Both exist because
 * the top-of-analysis exit is a bound no real predictor can reach: x264's B gate
 * commits only after list0 ref0 and list1 ref0 16x16 ME have landed, so the
 * reachable prize is the "post" number and the pre/post gap is the share that
 * lives in the ME itself.
 *
 * Y264_SKIP_ORACLE_SIDE = b | p | both (default both)
 * Y264_SKIP_ORACLE_AT = pre | post (default pre; B side only)
 */
int  y264_skor_side_b(void);
int  y264_skor_side_p(void);
int  y264_skor_at_post(void);

#endif
