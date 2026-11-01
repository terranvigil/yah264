---
title: HDR - yah264
description: Encode HDR10 and HLG with yah264, see what players read from the stream, and how it scores on HDR-VMAF.
---

# HDR

yah264 encodes HDR10 and HLG as 10-bit H.264. It writes the color description
and the HDR metadata that players and probes look for, and ffprobe and
MediaInfo both read its output as HDR10. We scored the pictures with Netflix's
HDR models for VMAF v1 on two open HDR10 masters, and the results are below.

## Supported formats

| | how the stream says it | options |
|---|---|---|
| HDR10 | PQ transfer with BT.2020 primaries and matrix in the VUI, plus mastering display (SMPTE ST 2086) and content light level (CTA-861.3) SEI | `--transfer smpte2084 --colorprim bt2020 --colormatrix bt2020 --mastering-display ... --cll ...` |
| HLG | HLG transfer in the VUI | `--transfer arib-std-b67 --colorprim bt2020 --colormatrix bt2020` |
| HLG, SDR-compatible | BT.2020 transfer in the VUI and an alternative transfer SEI that names HLG, so an SDR-only decoder still shows a picture | `--transfer bt2020 --alternative-transfer arib-std-b67` |
| 10-bit 4:2:0, 4:2:2, 4:4:4 | High 10, High 4:2:2, High 4:4:4 Predictive | picked from the input, see [Getting started](start.md#bit-depth) |

Dynamic metadata isn't supported, so there's no HDR10+ (SMPTE ST 2094-40) and
no Dolby Vision.

These options label the stream and nothing more. Rate control, adaptive
quantization and mode decision run the same way on a PQ or HLG picture as on an
SDR one.

## Encoding HDR10

The command-line encoder reads 10-bit Y4M, so ffmpeg decodes the master and
pipes it in:

```
ffmpeg -i master.mov -pix_fmt yuv420p10le -strict -1 -f yuv4mpegpipe - |
  yah264 --input-y4m - -o out.264 --crf 22 \
    --colorprim bt2020 --transfer smpte2084 --colormatrix bt2020 --range limited \
    --mastering-display 'G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)' \
    --cll 1000,400
```

Y4M has no field for the transfer or the primaries, so they go on the command
line. `--colormatrix bt2020` is the non-constant-luminance matrix, H.273 code
9, which ffmpeg calls `bt2020nc`.

`--mastering-display` lists green, blue, red, then the white point, which is the
order the H.264 spec uses. Chromaticities are in units of 0.00002 and luminance
in units of 0.0001 cd/m². The example is a P3 D65 display with a 1000-nit peak.
`--cll` takes the title's MaxCLL and MaxFALL in cd/m². When the master carries
both, `ffprobe -show_frames` prints them.

The ffmpeg wrapper doesn't pass the color description or the HDR metadata to
the encoder yet. Until it does, encode HDR with the command-line encoder.

## What a player reads

ffprobe on one of the encodes below:

```
profile=High 10
pix_fmt=yuv420p10le
color_range=tv
color_space=bt2020nc
color_transfer=smpte2084
color_primaries=bt2020
side_data_type=Mastering display metadata
...
side_data_type=Content light level metadata
max_content=1535
max_average=1024
```

MediaInfo on the same file:

```
Format profile                           : High 10@L5
HDR format                               : SMPTE ST 2086, HDR10 compatible
Bit depth                                : 10 bits
Color primaries                          : BT.2020
Transfer characteristics                 : PQ
Matrix coefficients                      : BT.2020 non-constant
Mastering display color primaries        : Display P3
Mastering display luminance              : min: 0.0001 cd/m2, max: 1000 cd/m2
Maximum Content Light Level              : 1535 cd/m2
Maximum Frame-Average Light Level        : 1024 cd/m2
```

An HLG encode reads `Transfer characteristics : HLG`. The SDR-compatible form
reads `HLG / BT.2020 (10-bit)`, and ffmpeg's decoder takes the HLG transfer from
the SEI.

## Quality on HDR-VMAF

VMAF v1 has HDR models (HDR-VMAF v1.1.8). They convert both pictures to PQ in
the ICtCp color space before scoring, so the score is about the HDR picture
itself and not about an SDR rendering of it. A 4K encode is scored with the 4K
model at 1.5 picture heights, a 1080p encode with the 1080p model at 3 picture
heights, and Sparks, at 59.94 fps, with the high-frame-rate versions of both.
Each encode is scored against its source at the same size. The run also
reports CAMBI in its PQ mode, which measures banding.

| clip | content | frames | source |
|---|---|---|---|
| Sol Levante | animation, a sunset sky over the sea | 240 at 24 fps, from 2:40 | ProRes 4444 XQ, 12-bit PQ, BT.2020, 1000 nits |
| Sparks | live action, a grinder throwing sparks | 300 at 59.94 fps, frames 5880 to 6179 | 16-bit PQ BT.2020 TIFF frames, 1000 nits, center-cropped to 3840x2160 |

Both titles are [Netflix Open Content](https://opencontent.netflix.com/) under
CC BY 4.0. Each was converted to 10-bit 4:2:0 PQ once, at 3840x2160 and again
at 1920x1080, and every encode starts from those files.

<figure class="chart"><img src="assets/hdr/hdr-quality.svg" alt="HDR-VMAF against bitrate for Sol Levante and Sparks at 1080p and 4K."></figure>

Sol Levante at 4K reaches HDR-VMAF 87 at 13 Mbit/s, CRF 26. Sparks is a much
harder picture, with film grain under thousands of moving sparks, and at 4K it
needs about 100 Mbit/s for 84. CAMBI starts at 0 for no banding and climbs as
banding gets worse. No rung of either clip goes above 1.2.

| clip | CRF | Mbit/s | HDR-VMAF | CAMBI |
|---|--:|--:|--:|--:|
| Sol Levante 1080p | 22 | 6.7 | 89.7 | 0.45 |
| Sol Levante 1080p | 26 | 3.9 | 84.1 | 0.85 |
| Sol Levante 4K | 22 | 23.4 | 92.6 | 0.47 |
| Sol Levante 4K | 26 | 13.4 | 87.4 | 0.98 |
| Sparks 1080p | 22 | 50.0 | 85.8 | 0.09 |
| Sparks 1080p | 26 | 31.4 | 76.6 | 0.19 |
| Sparks 4K | 22 | 167.8 | 92.3 | 0.14 |
| Sparks 4K | 26 | 101.5 | 84.5 | 0.20 |

## The pictures

Each image is frame 120 of a 4K encode, cropped to 960x540 at full
resolution, with the source on the left and yah264 at CRF 26 on the right. A
web page can't show PQ on every screen, so both halves go through the same tone
map to SDR (Hable, 1000-nit peak).

<figure class="chart"><img src="assets/hdr/hdr-sol.png" alt="Sol Levante, a sunset sky over the sea: the source on the left and yah264 at CRF 26 on the right."></figure>

The sky and the sun's edge come through intact. Sparks is the harder case:

<figure class="chart"><img src="assets/hdr/hdr-sparks.png" alt="Sparks, a grinder's sparks over a worker's jacket: the source on the left and yah264 at CRF 26 on the right."></figure>

The sparks and the clasp keep their edges, and the encode smooths some of the
fine grain on the jacket.

## Run it yourself

`scripts/hdr_check.sh` runs the whole check: it fetches the two clips, prepares
the sources, encodes the CRF ladder and scores each encode with HDR-VMAF. It
needs a libvmaf built with zimg and the `hdr_vmaf_v1.1.8` models from the
[VMAF repository](https://github.com/Netflix/vmaf).
