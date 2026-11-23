# Measuring quality with VMAF v1

yah264 scores quality with VMAF v1. That means libvmaf 3.x and Netflix's
v1.0.16 models. Every quality gate uses it: the BD bands, the long-form ladders and
the banding check. `VMAF_GATE=VMAF-NEG` remains only to reproduce older numbers.

v1 sees three things v0.6.1 couldn't: banding through CAMBI, chroma, and
blockiness through an additive-impairment term. Its detail feature keeps
VMAF-NEG's enhancement-gain cap of 1.0. Because sharpening past the source
earns nothing under that cap, a separate NEG score would add nothing.

## Scoring settings

| | what we do |
|---|---|
| model | `vmaf_v1.0.16_3d0h` up to a height of 1080, `vmaf_v1.0.16_1d5h_2160` above it, and the `_hfr` twin at 50 fps and up |
| bit depth | both sides converted to 10-bit, with `cambi.enc_bitdepth` set to the stream's coded depth |
| resolution | display resolution. We scale a picture smaller than 1920x1080 in both dimensions up to fit it, bicubic with the aspect kept, on the reference and the distorted side. CAMBI is told the coded size. PSNR is read at the coded size |
| frames | every frame, mean pooled |
| cache | v1 cache keys name the scoring rule. Long-form records it only for the pictures it scales, because it leaves larger ones as they were. bdcompare's keys also name the model, the encode bit depth and the libvmaf build |
| comparison | BD-rate against the reference encoder, on the same clip and resolution |

## Limits

- v1's CAMBI ignores very dark areas. `band_check.py` gates on CAMBI without
  that threshold and adds its own dark-area statistics.
- BD loses resolving power near the top of the scale, above about 98.
  bdcompare flags those clips.
- Scores from different resolutions aren't comparable. Each comparison is
  on one clip at one size.

## Where it lives

`scripts/bdcompare.py` (`_v1_of`) is the scorer behind every instrument.
[instruments.md](instruments.md) lists `VMAF_V1_BIN`, `VMAF_V1_MODELS` and
`VMAF_GATE`.
