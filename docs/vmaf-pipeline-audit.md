# VMAF pipeline audit: nothing about our output depresses the score

The question: before accepting that a residual quality deficit is real coding
efficiency, verify nothing about our OUTPUT (resolution, frame rate, color
space, ...) makes our VMAF read lower than it should.

Method: matched pairs on foreman_cif (300f) and samsung_720p (60f), both
encoders, pushed through the exact harness pipeline (`perf-comp.sh`'s trim ->
encode -> `ffmpeg -pix_fmt yuv420p` decode -> `vmaf` CLI), with every metadata
axis compared via ffprobe and the y4m headers, plus signal-level checks the
metadata cannot show. **Every axis is either identical between the two
encoders or measurably score-neutral:**

| axis | finding |
|---|---|
| resolution / crop | W/H exact both; board clips are all mod-16, no crop in play |
| frame count / dup-drop | decoded y4m = 300/300 frames, file sizes byte-identical modulo the SAR header tag; no ffmpeg dup/drop warnings |
| frame rate / timing | both decoded y4m headers read the same F30000:1001; the vmaf CLI pairs frames by ORDER, not timestamps, so timing metadata cannot resample either side |
| color range / space / primaries / transfer | **both** streams unsignaled ("unknown") -> identical ffmpeg treatment, no range conversion on either (a yuvj-range mismatch would have been catastrophic, not -0.5) |
| chroma siting | `left` both; irrelevant anyway -- vmaf_v0.6.1 is luma-only |
| pix_fmt / depth | yuv420p 8-bit both; `-pix_fmt yuv420p` is a no-op |
| reference | ONE trimmed ref.y4m feeds both encoders AND the scorer |
| subsample | 0 = every frame (the ~5fps aliasing trap is closed) |
| decode fidelity | conformance recon-match 254/254: ffmpeg's decode of our stream IS our encoder's recon |
| DC luma bias | ours +0.22 mean (swings ±0.6/frame) vs x264 +0.09: real but bounded -- a full +1.0 luma shift costs only **0.08 VMAF** (measured), so our bias is worth <0.05 |
| plain-VMAF vs NEG "gaming" | the board scores PLAIN v0.6.1; if x264's psy gamed it harder than ours the board would be biased. Measured enhancement gain (plain minus neg): ours **1.39**, theirs **1.34**. Equal; if anything we benefit slightly more |
| rate residual at the "matched" point | dSIZE reads +0.6% in OUR favor on most board rows, i.e. we score with ~0.6% MORE bits than x264 at the "matched" point -- the reported dVMAF is slightly FLATTERED, not depressed |
| psy-rd | 1.0 default both sides |

**Conclusion: a dVMAF deficit measured this way is real coding efficiency, not
measurement.**

## The two output defects the audit found, and their fixes

Neither is a VMAF defect; both were real bugs in what we emit.

**SAR.** x264 copies the y4m aspect ratio into the VUI (`128:117` on foreman,
`1:1` on the square-pixel clips); we wrote no aspect at all
(`sample_aspect_ratio=N/A`). The vmaf CLI ignores aspect, so there was no score
effect, but a real player displayed our foreman encode at the wrong shape. The
CLI now parses the Y4M `A` tag and carries it into the VUI (an explicit `--sar`
still wins; `A0:0` stays unspecified), and the SPS writer prefers a Table E-1
`aspect_ratio_idc` over Extended_SAR when the reduced ratio matches one (same
semantics, two bytes shorter). foreman signals 128:117, the square-pixel 720p
clips 1:1.

**Level.** Our level derivation was over-conservative, reading 40 where x264
picks 31 for the same stream. Two independent over-counts are removed. The DPB
term dropped a spurious `+1` (A.3.1 bounds `max_dec_frame_buffering`; the
current picture is not part of it), and the sliding-window formula charges the
pyramid's MARKED reference-B count read off the coding plan itself (1 at
bframes 2-3, 2 at 4, 3 at 5-7 -- `stair_plan_nrefb`, shared with `dpbp_open`)
instead of `bframes`. At medium that is a 5-frame window, not 7, and the 720p
cells sign **level 3.1 exactly where x264 does** (CIF: 1.3).
`N264_DPB_TIGHT=0` restores the old window and reproduces the old streams'
decoded pixels exactly.

**The tight window is scoped away from the wide staircase** (ref <= 1 shapes):
its DPB slot recycling depends on the old window's slack -- `determ_repeat`
under six spinners reads 4/16 nondeterministic configs with the tight window
there (all ref1) and 16/16 with it scoped. The serial path is proven tight
(recon-sweep 300/300); ref1 still reaches level 3.1 anyway, because the `+1`
removal alone covers it.

Gates: unit 9/9, conformance 254/254, recon-sweep 300/300 (tight arm),
determ_repeat 16/16 under load, CRF band BD old-vs-tight **neutral on all
twelve clips** (worst |0.08%| sintel, nine clips at +/-0.01%) -- the old
window's extra references were buying nothing.

**Trap for the next VUI change:** any VUI byte changes every bitstream md5,
which invalidates every byte-identity baseline at once. Batch a VUI change with
another output-changing ship, or flip it alone with the full gate battery and a
note that all canaries re-baseline.
