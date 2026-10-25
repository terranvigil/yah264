# Measuring quality with VMAF v1

yah264 scores quality with VMAF v1: libvmaf 3.x and Netflix's v1.0.16 models.
Every quality gate uses it: the BD bands, the long-form ladders and the
banding check. `VMAF_GATE=VMAF-NEG` remains only to reproduce older numbers.

v1 sees what v0.6.1 couldn't: banding (CAMBI), chroma, and blockiness through
an additive-impairment term. Its detail feature keeps VMAF-NEG's
enhancement-gain cap of 1.0, so sharpening past the source earns nothing and a
separate NEG score adds nothing.

## How we score

| | what we do |
|---|---|
| model | `vmaf_v1.0.16_3d0h` below 1440 lines, `vmaf_v1.0.16_1d5h_2160` at 1440 and up, and the `_hfr` twin at 50 fps and up |
| bit depth | both sides converted to 10-bit, with `cambi.enc_bitdepth` set to the stream's coded depth |
| resolution | at display resolution: a picture smaller than 1920x1080 in both dimensions is scaled up to fit it (bicubic, aspect kept), both sides, with CAMBI told the coded size. PSNR is read at the coded size |
| frames | every frame, mean pooled |
| cache | every v1 cache and record key names the scoring rule; bdcompare's also names the model, the encode bit depth and the libvmaf build |
| comparison | BD-rate against the reference encoder, on the same clip and resolution |

## Limits

- v1's CAMBI ignores very dark areas. `band_check.py` gates on CAMBI without
  that threshold, plus its own dark-area statistics.
- Near the top of the scale (above about 98) BD loses resolving power.
  bdcompare flags those clips.
- Scores from different resolutions aren't comparable; every comparison is
  on one clip at one size.

## Where it lives

`scripts/bdcompare.py` (`_v1_of`) is the scorer behind every instrument.
[instruments.md](instruments.md) lists `VMAF_V1_BIN`, `VMAF_V1_MODELS` and
`VMAF_GATE`.
