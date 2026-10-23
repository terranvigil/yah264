# Measuring quality with VMAF v1

yah264 scores quality with VMAF v1 alone: libvmaf 3.x with Netflix's v1.0.16
models. Every quality gate scores with it: BD bands, long-form ladders and
the banding check. The older VMAF v0.6.1 and its VMAF-NEG variant aren't
computed anywhere in the harness. The one exception is
`VMAF_GATE=VMAF-NEG`, kept so a number published on NEG can be reproduced.
The tables on the site were measured with VMAF-NEG, and their re-measure on
v1 is under way.

## Why v1

v0 scores luma detail and motion and nothing else, so it can't see the
artifacts viewers complain about most:

- **Banding.** v0 has no banding term. Our dark-banding bug passed every
  VMAF-NEG band. v1 carries CAMBI, Netflix's banding detector, as a model
  feature.
- **Chroma.** v0 reads luma only. v1 adds a chroma feature (SpEED-QA on the
  U and V planes). It found our biggest v1 gap: inter chroma DC levels
  that the quantizer's deadzone dropped. Choosing those levels by
  rate-distortion took park_joy's v1 BD-rate against x264 from +15.7% to
  +0.2%, while VMAF-NEG read the same change as a small loss.
- **Blockiness.** v1 computes an additive-impairment measure (AIM), which
  picks up blocking, alongside its detail loss. v0 used the detail loss alone.
- **Viewing conditions and frame rate.** v1 ships models per display and
  viewing distance, and high-frame-rate variants for 50 and 60 fps content.

Netflix also dropped VIF from the core feature set. In their words it was
costly and didn't improve accuracy, and the v1 models run faster than v0.

## How v1 covers VMAF-NEG

VMAF-NEG is v0.6.1 with an enhancement-gain limit of 1.0 on its VIF and
ADM features. An encoder can't raise its score by sharpening or boosting
contrast past the source, because detail above the source's no longer
counts. That was the whole reason we scored on NEG.

v1 keeps that protection. Its detail feature (ADM) runs with
`adm_enhn_gain_limit: 1.0`, the same cap. The feature keys carry it as
`egl_1`. VIF, the other feature NEG capped, isn't in v1 at all. The v1.0.16
1080p model fuses four features: CAMBI, chroma (`speed_chroma_uv`), capped
ADM (`adm3`) and capped motion (`motion3`). None of them rewards
enhancement, so a separate NEG score adds nothing.

## What Netflix recommends and what we do

These come from the libvmaf documentation for the v1 models
(`resource/doc/models_v1.md` and `cambi.md` in the Netflix/vmaf repository).

| recommendation | what we do |
|---|---|
| Pick the model for the display and viewing distance: 3H for 1080p, the 1.5H 4K model as the default for 2160p | `vmaf_v1.0.16_3d0h` below 1440 lines and `vmaf_v1.0.16_1d5h_2160` at 1440 and up. The model follows the source's height, and its file name is stored with every score. We don't use the phone (5H) or 4K consumer-TV (3H, 0-110 range) models |
| Use the `_hfr` twin for content around 50/60 fps | Sources at 50 fps and up get the `vmaf_v1.0.16_hfr` model of the same class. That covers the 60p titles (nocturne, meridian) |
| Score SDR at 10-bit precision, converting both inputs even when the encode is 8-bit | Reference and decode both go through `yuv420p10le` before scoring. They stream through FIFOs, so no full-size copy is written to disk |
| Pass CAMBI the encode-side width, height and bit depth when they differ from the scoring input | Every score passes `cambi.enc_bitdepth` with the stream's coded depth (8 for our builds and x264, read from the stream). Width and height need no override, because we never rescale: the encode, the decode and the reference all share one resolution |
| Don't compare absolute scores across resolutions | Comparisons are always ours against the reference encoder on the same clip, at the same resolution, read as BD-rate |
| A float-enabled libvmaf (the chroma feature needs it) | A local float build at `~/.local/opt/vmaf-v1` (`VMAF_V1_BIN`). Homebrew's libvmaf can't load the v1 models |

We add two practices of our own:

- **Every frame is scored.** Bands run at subsample 1. Long-form cells score
  every frame, and the legacy strided rows aren't used.
- **The model version is part of the cache key.** Scores are cached under the
  model file and the encode bit depth, so a score from another model or
  setting is never reused.

## What v1 doesn't settle

CAMBI misses much of the banding in very dark gradients. `band_check.py`
keeps its own block statistics on dark smooth areas (flat share, detail
kept, dark PSNR) beside v1's CAMBI, and a window fails on either.

v1 also compresses near the top of its scale. A clip whose upper rungs score
above about 98 gives a BD number with little resolving power, and bdcompare
flags it as near-saturation.

The band's rung placement and the HD band's noise baselines were calibrated
on VMAF-NEG. `calibrate_band.py` and `hd_null.py` now read the v1 gate, and
re-running both on it is next. Until then the HD band runs a live control on
every clip.

## Where it lives

- `scripts/bdcompare.py`: `_v1_of` is the scorer every Python instrument calls
  (`run_band.py`, `hd_band.py`, `ladder.py`, `band_check.py`,
  `calibrate_band.py`). `VMAF_GATE` picks the gate score, VMAF v1 by default.
- [instruments.md](instruments.md) lists the knobs: `VMAF_V1_BIN`,
  `VMAF_V1_MODELS` and `VMAF_GATE`.
