# VMAF pipeline audit

Before we count a residual quality gap as real coding efficiency, we check
that nothing about our OUTPUT makes our VMAF read lower than it should:
resolution, frame rate, color space and the other stream properties.

The audit encodes matched pairs with both encoders on 300 frames of
foreman_cif and 60 frames of samsung_720p. Each pair goes through the exact
pipeline the harness uses: `perf-comp.sh`'s trim and encode, then an
`ffmpeg -pix_fmt yuv420p` decode, then the `vmaf` CLI. We compare each
metadata axis with ffprobe and the y4m headers, and signal-level checks cover
what the metadata can't show. Every axis came out either identical between
the two encoders or measurably score-neutral.

This audit covers the VMAF v0.6.1 pipeline. The v1 pipeline ([vmaf.md](vmaf.md))
converts both sides to 10-bit, scales pictures smaller than 1080p up to the
display, and reads chroma. Both encoders site chroma `left`.

| axis | finding |
|---|---|
| resolution / crop | width and height exact on both. The table clips are all mod-16, so no crop is in play |
| frame count / dup-drop | decoded y4m is 300/300 frames, file sizes byte-identical apart from the SAR header tag; no ffmpeg dup/drop warnings |
| frame rate / timing | both decoded y4m headers read F30000:1001. The vmaf CLI pairs frames by ORDER, so timing metadata can't resample either side |
| color range / space / primaries / transfer | both streams are unsignaled ("unknown"), so ffmpeg treats them the same way, with no range conversion on either side. A full-range mismatch would be catastrophic, far beyond -0.5 |
| chroma siting | `left` on both |
| pix_fmt / depth | yuv420p 8-bit on both; `-pix_fmt yuv420p` is a no-op |
| reference | ONE trimmed ref.y4m feeds both encoders AND the scorer |
| subsample | 0, every frame, which closes the aliasing trap of scoring at about 5 fps |
| decode fidelity | conformance recon-match: ffmpeg's decode of our stream IS our encoder's recon |
| DC luma bias | ours is +0.22 mean, swinging +-0.6 per frame, against x264's +0.09. The bias is real but bounded. We measured a full +1.0 luma shift at only 0.08 VMAF, so our bias is worth under 0.05 |
| plain VMAF vs NEG "gaming" | if one encoder's psy tuning gamed plain v0.6.1 harder, a plain-VMAF table would be biased. The measured enhancement gain (plain minus NEG) is 1.39 for ours and 1.34 for x264. That's equal, and if anything we benefit slightly more |
| rate residual at the "matched" point | dSIZE reads +0.6% in OUR favor on most table rows. We score with about 0.6% MORE bits than x264 at the "matched" point, so the reported dVMAF is slightly flattered, if anything |
| psy-rd | 1.0 on both sides |

So a dVMAF gap measured this way is real coding efficiency. The measurement
doesn't depress it.

## What the output signals

Neither of these moves VMAF. Both matter to what a real player or decoder
sees.

**SAR.** The CLI parses the Y4M `A` tag and writes it into the VUI. An
explicit `--sar` wins, and `A0:0` stays unspecified. When the reduced ratio
matches a Table E-1 `aspect_ratio_idc`, the SPS writer uses that index in place
of Extended_SAR. The meaning is the same and the SPS is two bytes shorter.
foreman signals 128:117. The square-pixel 720p clips signal 1:1. The vmaf CLI
ignores aspect, but a player without it shows foreman with the wrong
proportions.

**Level.** The level derivation counts the DPB the stream really needs. The
`max_dec_frame_buffering` term doesn't include the current picture, because
A.3.1 bounds it without it. The sliding window charges the pyramid's MARKED
reference Bs instead of `bframes`. `stair_plan_nrefb` reads them off the coding
plan, and `dpbp_open` shares it:

| bframes | marked reference Bs |
|---|---|
| 2-3 | 1 |
| 4 | 2 |
| 5-7 | 3 |

At medium the window is 5 frames. The 720p cells sign level 3.1, the same as
x264, and CIF signs 1.3. The extra references a wider window would hold buy
nothing. Against the 5-frame window, the CRF band BD of a 7-frame window is
neutral on all twelve clips. The worst is |0.08%| and nine sit at +-0.01%.

**We don't apply the tight window where the wide staircase can engage**, which
is the ref <= 1 shapes. The wide staircase recycles DPB slots in a way that
depends on the slack in the wider window. With the tight window there, `determ_repeat`
under six spinners reads 4/16 configs nondeterministic against 16/16 clean
without it. All four failures are ref1. Even though a determinism test caught
it, this guards correctness. ref1 still reaches level 3.1 because the DPB term
alone covers it.

## Trap for the next VUI change

Any VUI byte changes every bitstream md5, which invalidates every byte-identity
baseline at once. Batch a VUI change with another output-changing ship, or flip
it alone with the full gate battery and a note that all canaries re-baseline.
