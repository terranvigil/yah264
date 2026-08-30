# Sync-lookahead: extra input buffering, not a mb-tree window fence

Sync-lookahead buys the lookahead thread a head start by growing the input
ring's CAPACITY, leaving the mb-tree window walk's DEPTH untouched. It is an
engineering task gated on byte-identity, not a BD-rate design decision.

It is ON by default at `bframes + 1` wherever the wavefront pool is wide enough
to run a chain against. It is a real param field (`param.sync_lookahead`, CLI
`--sync-lookahead N`), off via `N=0` or `--tune zerolatency`, and both
`Y264_LA_THREAD` and `Y264_LA_INLINE` follow it.

## The problem

`Y264_LA_THREAD` moves the per-push lookahead chain (downscale, lowres ME,
scene-cut and b-adapt typing) to a dedicated thread. Measured effect on its
own: ~1.00-1.02x, far below the >=1.15x hoped for. The reason is that
`yah264_encoder_encode` is synchronous per arriving frame. Each call that pops
a ring entry to encode also calls `compute_mbtree`, whose window walk reads
ring entries `e->la[la_head..la_head+la_n-1]`, and among those is the entry
this SAME call just pushed via `la_push`. The la thread has had zero wall-clock
time to run that entry's chain step, so the arrival-side wait (`la_th_wait`)
collapses to an inline block every call. There is no overlap window to buy
back.

## Why the fix is capacity, not window depth

The tempting fix is to fence the mb-tree window walk at push `N-k` instead of
`N`, having `compute_mbtree` ignore the newest k pushes. That is a VALUE change
to the propagation math (fewer frames of forward lookahead) and would need a
BD-rate sweep.

x264's sync-lookahead knob does not do that. Its delay budget is built in three
additive parts: a base window that is the larger of the B-frame-driven delay
and the configured lookahead depth; plus one frame per additional frame thread;
plus the sync-lookahead value on top of both. The lookahead thread fires its
slicetype decision once the queue is longer than that base window, and the
decision itself still walks a window of depth `i_slicetype_length`.
`i_sync_lookahead` (default `bframes+1`) just means that many MORE frames sit
buffered ahead of the window before the thread is required to have an answer
ready. The window's depth is untouched.

### How that maps onto our ring

`yah264_encoder_encode`'s `la_depth > 0` branch already implements a
fixed-depth ring exactly like x264's `next` buffer: it does not pop (emit) a
frame until `la_n` reaches the ring's capacity. Coupling capacity and window
depth to the SAME number (`e->la_depth`) is the bug: the moment the ring is
full, the newest entry is BOTH "just pushed this call" and "needed by this
call's compute_mbtree window."

Growing ring CAPACITY to `la_depth + k` while capping the WINDOW WALK at the
original `la_depth` decouples them without touching what `compute_mbtree`
computes:

- `compute_mbtree_wholebuf`'s anchor and source walks and the legacy
  `la_chain_prop` walk iterate `e->la` starting at `la_head`, for `i < e->la_n`.
  Right after a pop, `la_n` is `la_depth - 1` (the ring was full at `la_depth`,
  one entry left). With more capacity, post-pop `la_n` becomes
  `la_depth + k - 1`: MORE entries newly visible to an uncapped walk, which
  would change mb-tree's output (a strictly larger propagation window is a real
  algorithmic change, not free). The fix adds an explicit `&& i < la_depth - 2`
  bound to those three walks.

  **It must be `- 2`, not `- 1`, and the byte-identity gate is what caught
  that.** At `k = 0`, the walk's last `la_n`-bounded entry (`i = la_depth - 2`)
  is always the frame pushed in THIS SAME call. It is still untyped, since
  typing lags one push behind its own chain step (`la_finalize` runs when the
  NEXT push lands), so `!en->typed` always breaks there and only `la_depth - 2`
  entries (`i = 0..la_depth-3`) ever actually get folded in. With `k > 0` that
  same relative ring position is no longer the newest push (k more have landed
  behind it by the time it is read), so it IS typed by then, and a `- 1` bound
  would silently admit it as an extra entry: measured as a real divergence at
  every `k >= 1` on `--bframes 0` and `--bframes 3`, passing only by
  coincidence on `--bframes 2`. `- 2` reproduces the k=0 exclusion explicitly
  instead of relying on the untyped-break accident.

  With the bound in place the loop visits exactly the same FIFO-ordered set of
  frames regardless of `k`. Ring order is push order is display order, so the
  first `la_depth - 2` entries after any given anchor's pop are identical
  content whether or not `k` extra frames sit further back in the ring.
  `compute_mbtree(anchor)` is therefore a pure function of the same inputs at
  every `k`.
- The one window-shaped walk that does NOT need a cap is the buffered-B
  "bothlist" list-0 propagation and `lowres_bleg_me`'s backward leg scan. Both
  terminate on a structural condition (the next typed anchor, or `nb` bounded
  by `bframes`) reached well inside the first `la_depth-1` entries at the
  engage gate's own `la_depth >= bframes+3` minimum, so they already return the
  same result regardless of how much MORE sits in the ring behind that point.
- Everywhere the ring's actual array size is used for wraparound (`% e->la_depth`
  in `la_push`, the flush pop, the pad-time slot claim, and `lowres_bleg_me`'s
  backward index) has to change to the new capacity (`e->la_cap`), or indices
  alias into the wrong slot once capacity exceeds the old depth. Same for the
  alloc/free loops that size the ring's per-entry buffers.
- The pad-time slot-reuse fence ("the recycled slot's own analysis is the only
  chain read of `en->plane`") is a statement about ring ROTATION PERIOD, i.e.
  capacity, not window depth: slot `s % capacity` was last written by push
  `s - capacity`. That wait becomes `pushed + 1 - la_cap`.
- The "window still filling" gate that decides whether an arrival call pops
  anything (`la_n < e->la_depth`) becomes `la_n < e->la_cap`. This is the one
  place the change is visible from outside: the encoder withholds the first `k`
  frames' worth of output an extra `k` calls, mirroring x264's ifbuf/next split.
- `la_th`'s own bookkeeping (`struct la_thread.q[64]`, the `% 64` indices in
  `la_th_enqueue`/`la_th_main`) was sized to the assumption that lag between
  "pushed" and "popped" never exceeds 64 (`la_depth`'s hard clamp). With
  capacity now `la_depth + k`, the fixed array needs the same headroom
  (`Y264_LA_CAP_MAX`) or it wraps into a push it hasn't consumed yet.
- The la-thread engage gate (`la_depth >= bframes + 3`, the margin that keeps
  the popped entry's own finalize step, at most `bframes+2` pushes after its
  pop, inside what has already been pushed) does not change: `la_cap >=
  la_depth`, so the gate is if anything MORE slack with `k > 0`, never less.

So every quantity `compute_mbtree` (or `la_chain_prop`, or the buffered-B
propagation) reads is either capped to the ORIGINAL `la_depth` window, and so
provably independent of `k`, or a ring-capacity/rotation detail that must track
the new capacity purely for memory safety. The only externally visible effect
of `k > 0` is added start-of-stream latency (k more `encode` calls return 0
NALs before the first frame is emitted) and more slack for the decoupled
lookahead thread to run ahead of an arrival call before that call's
`compute_mbtree` needs the freshest push, which is the thing `Y264_LA_THREAD`
alone could not buy.

## Implementation

- `Y264_LA_CAP_MAX` (80): the la-ring's compiled array size (`e->la[]` and
  `la_thread.q[]`), replacing the implicit `64` both were sized to match
  `la_depth`'s own clamp.
- `e->la_cap = e->la_depth + e->la_buf`. `e->la_buf` resolves from
  `param.sync_lookahead` (default `bframes + 1`), clamped to
  `[0, Y264_LA_CAP_MAX - la_depth]`, and is warmed in `warm_lr_statics` like
  every other env-gated static.
- `Y264_LA_BUF` overrides the resolved value. **It overrides DOWNWARD as well
  as up**: with the default resolving to `bframes+1`, `Y264_LA_BUF=1` cuts
  frames off the encoder's own lead and reads as a regression that the knob
  itself did not cause (samsung: `0` reads -6.41%, `1` reads -5.01%, `4` is the
  default). Print what the default resolved to before pricing the knob.
- At `k = 0`, `la_cap == la_depth`, every touched loop's added bound is a
  no-op, and the hot paths are the same instruction sequence as without the
  feature (the only diff is one extra int compared in the window walks and a
  field rename at the allocation/free/index sites).
- Gate: 6-config byte-identity matrix at k=0 (the escape hatch, which must be
  literally unchanged); at k in {1, bframes+1, 8}, byte-identity of every
  frame's CODED CONTENT once output starts (first k frames of output delayed,
  nothing else differs) plus determinism t1==t8==t18 x5, conformance --fast
  249/249, TSan 0, stress 0 hangs, and flush-torture. Flush torture is exactly
  where an off-by-one in the new caps surfaces: the tail drain has to walk
  `la_cap` entries instead of `la_depth`.
