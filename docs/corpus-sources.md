# Where the test and tuning material comes from

Two corpora, and the separation between them is a rule rather than a convention.

## The rule, first

**The gate corpus is TEST-ONLY, permanently.** Nothing that tunes a constant, and
nothing that trains a model, may see a clip that a gate scores. A model that has
been shown the gate produces a number nobody can interpret, and the failure is
silent: everything still runs, the number just stops meaning what it says.

Our gate clips came largely from Xiph's derf collection, so drawing training
material from derf is exactly where an accidental overlap would come from.
`scripts/fetch_train_corpus.sh` enforces a no-overlap check for that reason.

## Gate corpus, `tests/corpus/`

Sixteen clips. Class is in `tests/corpus/CLASSES`; the six that form the speed
board and their calibrated operating points are in `scripts/parity-clips.sh`.

| clip | res | class | source |
|---|---|---|---|
| akiyo, bus, coastguard, foreman, mobile, stefan | CIF | static / motion / detail | Xiph derf standard sequences |
| ducks_720p, park_joy_720p | 720p | motion | Xiph derf |
| samsung_720p | 720p | | vendor test material |
| uneven_720p | 720p | | mislabeled on disk: its container frame rate does not match its content. Kept, but every script reads the rate off the clip |
| touchdown_1080p | 1080p | | the 4:2:2 ORIGINAL. Never put it in a 4:2:0 sweep. It is `scripts/conformance.sh`'s only 4:2:2 recon-match coverage, so do not delete it |
| touchdown_420 | 1080p | | the converted 4:2:0 version. Usable for speed and conformance, NOT for BD: three in-band ladders of one encode pair read +4.27%, +61.01% and +1.31% |
| sintel_720p | 720p | animation (3D CGI) | Blender open movie, CC-BY 3.0 |
| bbb_720p | 720p | animation (3D CGI) | Big Buck Bunny, Blender, CC-BY 3.0. 450 frames from 9m45s, the most sustained-motion window that is not also a night scene |
| sita_720p | 720p | animation (hand-drawn 2D) | Sita Sings the Blues, **Public Domain Mark 1.0**, archive.org item `sita-sings-the-blues_202403`. 140 frames from 35m, the longest cut-free run in a flat-colour sequence |

The two animation kinds are deliberately both present and they disagree
violently: we are 29.76% BD-rate ahead of x264 on the CGI clip and 10.73% behind
on the hand-drawn one. Quote which kind, never "animation".

### Pulling a clip out of a long source

`sita_720p` came from a 16 GB master by HTTP range request rather than
downloading the file, which is worth knowing because most good masters are large:

    ffmpeg -ss <seconds> -i <https-url> -frames:v N -vf scale=1280:720 ...

Pick the window by measuring, not by eye. For that clip the useful measures were
flat-block share, mean luma (to avoid night scenes) and a scene-cut count from
frame-to-frame luma jumps. The first section chosen read luma 22 and would have
been useless.

Any new gate clip gets its operating point from `scripts/parity-clip-calib.sh`,
which applies the same band rule the original six were chosen by. Do not pick a
bitrate by hand.

## Training corpus, `/Volumes/seagate/media/train-corpus/bvi-aom/`

**BVI-AOM**, University of Bristol. 956 sequences from 239 unique 4K sources,
downsampled with Lanczos-3 to four resolution classes. Every sequence is 64
frames with no scene cuts, 10-bit 4:2:0, losslessly compressed in h264 so ffmpeg
extracts raw. Licence is a UoB custom grant for coding-standards development,
training and evaluation. Paper: arXiv 2408.03265. Index:
<https://github.com/fan-aaron-zhang/bvi-aom>

**We hold the D tier**, 239 sequences at 480x272, 1.46 GB. That is every unique
source at the cheapest pixel cost, which is the right place to start: a
64-frame 480x272 sequence already yields far more labelled macroblocks than a
model of this size needs, and the higher tiers carry the same 239 scenes.

### How to get more

The dataset is one tarball per resolution class, so a higher tier is one file:

| file | size | class |
|---|--:|---|
| `272p.tar.gz` | 1.46 GB | D, 480x272 (held) |
| `544p.tar.gz` | 5.88 GB | C, 960x544 |
| `1088p.tar.gz` | 23.3 GB | B, 1920x1088 |
| `2176p_part_a..f.tar.gz` | ~92 GB | A, 3840x2176 |

**Use the path-style S3 endpoint.** The vanity hostname
`download.opencontent.netflix.com` does not resolve from every network, and this
dataset sat recorded as "blocked" for weeks on the strength of one failed curl
to it. The bucket answers fine as:

    https://s3.amazonaws.com/download.opencontent.netflix.com/bvi_aom_dataset/<file>

List it with `?list-type=2&prefix=bvi_aom_dataset/`. The lesson generalises: a
dead hostname is not a dead host, so try the service's own endpoint form before
concluding anything is unreachable.

## Libraries worth pulling from next

Assessed but not held. Ordered by how useful they would be to us.

| library | what it is | licence | why we would want it |
|---|---|---|---|
| AWCY `objective-1` | AOM/IETF NETVC benchmark set, 13 GB (`-fast` subset 1.9 GB) | open test material | the obvious second training set, and a yardstick others report against |
| Netflix Open Content | El Fuente, Chimera, Meridian | CC-BY 4.0 | cleanest licence of any option, professional cinematic sources |
| UVG | 5 x 4K 120fps | CC-BY-NC | high frame rate, few sources. Non-commercial terms accepted for this project by owner call |
| BVI-DVC | 800 sequences | research use only | the precedent BVI-AOM replaced; skip, its terms are worse |
| Xiph / derf | assorted SD-heavy | mostly free | **our gate came from here.** Training draws need the explicit no-overlap check |
| Blender open movies | rendered features | CC-BY 3.0 | more animation, and the only easy source of clean CGI |

Two gaps we know about. There is no hand-drawn 2D source in the training set at
all, which matters now that hand-drawn is where we measurably lose. And Sol
Levante (Netflix Open Content, CC BY 4.0) is the only true hand-drawn anime
master we found; it was unreachable when we looked and is worth retrying with the
endpoint-form trick above.
