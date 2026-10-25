# VMAF pipeline audit: nothing about our output depresses the score

Before a residual quality gap counts as real coding efficiency, we check that
nothing about our OUTPUT makes our VMAF read lower than it should: resolution,
frame rate, color space and the other stream properties.

The audit encodes matched pairs with both encoders on foreman_cif (300 frames)
and samsung_720p (60 frames). Each pair goes through the exact harness
pipeline: `perf-comp.sh`'s trim, encode, `ffmpeg -pix_fmt yuv420p` decode,
then the `vmaf` CLI. Every metadata axis is compared with ffprobe and the y4m
headers, and signal-level checks cover what the metadata can't show. Every
axis is either identical between the two encoders or measurably score-neutral:

| axis | finding |
|---|---|
| resolution / crop | width and height exact on both; the table clips are all mod-16, so no crop is in play |
| frame count / dup-drop | decoded y4m is 300/300 frames, file sizes byte-identical apart from the SAR header tag; no ffmpeg dup/drop warnings |
| frame rate / timing | both decoded y4m headers read F30000:1001; the vmaf CLI pairs frames by ORDER, not timestamps, so timing metadata can't resample either side |
| color range / space / primaries / transfer | both streams unsignaled ("unknown"), so ffmpeg treats them identically with no range conversion on either side. A full-range mismatch would be catastrophic, not -0.5 |
| chroma siting | `left` on both |
| pix_fmt / depth | yuv420p 8-bit on both, scored at 10-bit ([vmaf.md](vmaf.md)) |
| reference | ONE trimmed ref.y4m feeds both encoders AND the scorer |
| subsample | 0, every frame (the ~5 fps aliasing trap is closed) |
| decode fidelity | conformance recon-match: ffmpeg's decode of our stream IS our encoder's recon |
| DC luma bias | ours +0.22 mean (swinging +-0.6 per frame) against x264's +0.09. Real but bounded: a full +1.0 luma shift costs only **0.08 VMAF** (measured), so our bias is worth under 0.05 |
| plain VMAF vs NEG "gaming" | if one encoder's psy tuning gamed plain v0.6.1 harder, a plain-VMAF table would be biased. Measured enhancement gain (plain minus NEG): ours **1.39**, x264's **1.34**. Equal; if anything we benefit slightly more |
| rate residual at the "matched" point | dSIZE reads +0.6% in OUR favor on most table rows: we score with ~0.6% MORE bits than x264 at the "matched" point, so the reported dVMAF is slightly flattered, not depressed |
| psy-rd | 1.0 on both sides |

**A dVMAF gap measured this way is real coding efficiency, not measurement.**

## What the output signals

Neither of these moves VMAF. Both are about what a real player or decoder
sees.

**SAR.** The CLI parses the Y4M `A` tag and carries it into the VUI. An
explicit `--sar` wins, and `A0:0` stays unspecified. When the reduced ratio
matches a Table E-1 `aspect_ratio_idc`, the SPS writer uses that index in place
of Extended_SAR: same semantics, two bytes shorter. foreman signals 128:117 and
the square-pixel 720p clips signal 1:1. The vmaf CLI ignores aspect, but a
player without it shows foreman at the wrong shape.

**Level.** The level derivation counts the DPB the stream really needs. The
`max_dec_frame_buffering` term doesn't include the current picture (A.3.1
bounds it without it). The sliding window charges the pyramid's MARKED
reference Bs, not `bframes`, read off the coding plan by `stair_plan_nrefb`
(shared with `dpbp_open`):

| bframes | marked reference Bs |
|---|---|
| 2-3 | 1 |
| 4 | 2 |
| 5-7 | 3 |

At medium the window is 5 frames. The 720p cells sign level 3.1, the same as
x264, and CIF signs 1.3. The extra references a wider window would hold buy
nothing: the CRF band BD of a 7-frame window against the 5-frame one is
neutral on all twelve clips (worst |0.08%|, nine at +-0.01%).

**The tight window isn't applied where the wide staircase can engage** (the
ref <= 1 shapes). The wide staircase's DPB slot recycling depends on the wider
window's slack: with the tight window there, `determ_repeat` under six spinners
reads 4/16 configs nondeterministic, all ref1, against 16/16 clean without it.
This is a correctness guard, not a determinism one. ref1 still reaches level
3.1, since the DPB term alone covers it.

## Trap for the next VUI change

Any VUI byte changes every bitstream md5, which invalidates every byte-identity
baseline at once. Batch a VUI change with another output-changing ship, or flip
it alone with the full gate battery and a note that all canaries re-baseline.
