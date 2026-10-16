# VMAF pipeline audit: nothing about our output depresses the score

Before accepting that a residual quality deficit is real coding efficiency, we
checked that nothing about our OUTPUT makes our VMAF read lower than it should.
That covers resolution, frame rate, color space and the other stream
properties.

We encoded matched pairs with both encoders on foreman_cif (300f) and
samsung_720p (60f). Each pair went through the exact harness pipeline:
`perf-comp.sh`'s trim -> encode -> `ffmpeg -pix_fmt yuv420p` decode -> `vmaf`
CLI. We compared every metadata axis with ffprobe and the y4m headers, then ran
signal-level checks for what the metadata cannot show. Every axis is either
identical between the two encoders or measurably score-neutral:

| axis | finding |
|---|---|
| resolution / crop | W/H exact both; table clips are all mod-16, no crop in play |
| frame count / dup-drop | decoded y4m = 300/300 frames, file sizes byte-identical modulo the SAR header tag; no ffmpeg dup/drop warnings |
| frame rate / timing | both decoded y4m headers read the same F30000:1001; the vmaf CLI pairs frames by ORDER, not timestamps, so timing metadata cannot resample either side |
| color range / space / primaries / transfer | **both** streams unsignaled ("unknown") -> identical ffmpeg treatment, no range conversion on either (a yuvj-range mismatch would have been catastrophic, not -0.5) |
| chroma siting | `left` both; irrelevant anyway -- vmaf_v0.6.1 is luma-only |
| pix_fmt / depth | yuv420p 8-bit both; `-pix_fmt yuv420p` is a no-op |
| reference | ONE trimmed ref.y4m feeds both encoders AND the scorer |
| subsample | 0 = every frame (the ~5fps aliasing trap is closed) |
| decode fidelity | conformance recon-match 254/254: ffmpeg's decode of our stream IS our encoder's recon |
| DC luma bias | ours +0.22 mean (swings ±0.6/frame) vs x264 +0.09: real but bounded -- a full +1.0 luma shift costs only **0.08 VMAF** (measured), so our bias is worth <0.05 |
| plain-VMAF vs NEG "gaming" | the table scores PLAIN v0.6.1; if x264's psy gamed it harder than ours the table would be biased. Measured enhancement gain (plain minus neg): ours **1.39**, theirs **1.34**. Equal; if anything we benefit slightly more |
| rate residual at the "matched" point | dSIZE reads +0.6% in OUR favor on most table rows, i.e. we score with ~0.6% MORE bits than x264 at the "matched" point -- the reported dVMAF is slightly FLATTERED, not depressed |
| psy-rd | 1.0 default both sides |

**Conclusion: a dVMAF deficit measured this way is real coding efficiency, not
measurement.**

## The two output defects the audit found, and their fixes

Neither is a VMAF defect. Both were real bugs in what we emit.

**SAR.** x264 copies the y4m aspect ratio into the VUI: `128:117` on foreman
and `1:1` on the square-pixel clips. We wrote no aspect at all
(`sample_aspect_ratio=N/A`). The vmaf CLI ignores aspect, so the score didn't
move. A real player still displayed our foreman encode at the wrong shape.

The CLI now parses the Y4M `A` tag and carries it into the VUI. An explicit
`--sar` still wins, and `A0:0` stays unspecified. When the reduced ratio
matches a Table E-1 `aspect_ratio_idc`, the SPS writer uses that index in place
of Extended_SAR. The semantics are the same and the SPS is two bytes shorter.
foreman now signals 128:117, and the square-pixel 720p clips signal 1:1.

**Level.** Our level derivation was over-conservative. It read 40 where x264
picks 31 for the same stream. We removed two independent over-counts.

The first was a spurious `+1` in the DPB term. A.3.1 bounds
`max_dec_frame_buffering`, and the current picture is not part of it. The
second was in the sliding-window formula, which charged `bframes`. It now
charges the pyramid's MARKED reference-B count, read off the coding plan
itself by `stair_plan_nrefb` (shared with `dpbp_open`):

| bframes | marked reference Bs |
|---|---|
| 2-3 | 1 |
| 4 | 2 |
| 5-7 | 3 |

At medium the window drops from 7 frames to 5. The 720p cells now sign
**level 3.1 exactly where x264 does**, and CIF signs 1.3.
`Y264_DPB_TIGHT=0` restores the old window and reproduces the old streams'
decoded pixels exactly.

**The tight window is scoped away from the wide staircase**, the ref <= 1
shapes. The wide staircase's DPB slot recycling depends on the old window's
slack. Under six spinners, `determ_repeat` reads 4/16 nondeterministic configs
with the tight window there, all of them ref1. With the window scoped away it
reads 16/16. The serial path is proven tight by recon-sweep 300/300. ref1 still
reaches level 3.1 anyway, because the `+1` removal alone covers it.

The CRF band BD of old against tight is **neutral on all twelve clips**. The
worst is sintel at |0.08%|, and nine clips sit at +/-0.01%. The old window's
extra references were buying nothing. The other gates:

| gate | result |
|---|---|
| unit | 9/9 |
| conformance | 254/254 |
| recon-sweep (tight arm) | 300/300 |
| determ_repeat under load | 16/16 |

**Trap for the next VUI change:** any VUI byte changes every bitstream md5.
That invalidates every byte-identity baseline at once. Batch a VUI change with
another output-changing ship, or flip it alone with the full gate battery and a
note that all canaries re-baseline.
