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

### The resolution-balance set, added 2026-08-30

The band corpus was 7 CIF, 4 at 720p and **one** 1080p clip. Two consequences
were live for months. A "corpus median" was in effect a CIF decision, which is
how `B8_QGATE=6` came to sit as a flip candidate on a case worth -0.30% at CIF
and -0.03% at 720p and above. And the entire 1080p class rested on touchdown,
which dislikes nearly every arm measured against it (mb-tree strength +5.83,
CRF_CPLX +1.81, aq +1.93), with no second clip to say whether that is the
resolution talking or the clip.

Six more at each size, `scripts/fetch_corpus.sh --res`, about 12 GB:

| clip | res | class | what it is |
|---|---|---|---|
| shields_720p, parkrun_720p, stockholm_720p | 720p | detail | the SVT "ter" pans, detail under motion |
| in_to_tree_720p | 720p | detail | slow zoom into foliage |
| old_town_720p | 720p | grain | aerial pan over roofs |
| fourpeople_720p | 720p | static | videoconference, a class the corpus otherwise had only at CIF (akiyo) |
| blue_sky_1080p | 1080p | motion | slow rotation over low texture |
| pedestrian_1080p | 1080p | static | fixed camera, walking people |
| riverbed_1080p | 1080p | detail | water at the edge of noise, the hardest one here |
| station2_1080p | 1080p | detail | pan over fine rail detail |
| sunflower_1080p | 1080p | static | smooth close-up |
| crowd_run_1080p | 1080p | crowd | dense motion at 50fps |

All twelve are native resolution, 8-bit 4:2:0, from Xiph's derf host, and every
header was read over HTTP before anything was downloaded. Sources: the SVT High
Definition Multi Format test set (shields, parkrun, stockholm, in_to_tree,
old_town, crowd_run), the TUM 1080p25 set (blue_sky, pedestrian, riverbed,
station2, sunflower), and JCT-VC class E (fourpeople). These carry research-use
terms rather than a Creative Commons licence, which is the same footing the CIF
gate clips have always stood on; nothing is redistributed, and
`scripts/fetch_corpus.sh` pulls them on the user's own machine.

**Two exclusions, both deliberate.**

The **Netflix 4K set on the same host is off limits for gate use**, and so is
anything Harmonics-derived. The ML training corpus (BVI-AOM) draws on
BVI-Texture, IRIS, Harmonics, Videvo, SJTU, MCL-JCV, LIVE-Netflix and Yonsei
material, and a gate clip that also sits in the training set breaks the
train/test split silently. The 178 BVI-AOM source names were checked against
this slate on 2026-08-30 and none of the SVT, TUM or class-E sequences appears
in it.

The **aspen / red_kayak / speed_bag / snow_mnt / west_wind_easy /
rush_field_cuts / controlled_burn** group is also absent. `touchdown_pass` comes
from that set and is the 4:2:2 clip already in this tree, so treat the whole
group as suspect until a header says otherwise.

**These clips are fetched, not yet promoted.** A new gate clip needs its
operating point from `scripts/parity-clip-calib.sh` before it can enter a band
ladder, and adding it re-medians every published number.

### Windows that are timed, not scored

`tests/corpus` also holds cuts that exist only to be timed at a matched CRF
point through `scripts/ffboard.py`: `bbb{10,15,30}s_1080p_o120` and
`perseverance_{1080p,720p}`. They are not gate clips. They have no class, no
calibrated operating point, and nothing scores them but the matched-CRF speed
board, which needs none of that. Promoting one means running
`scripts/parity-clip-calib.sh` first, same as any other candidate.

Both sources are already-compressed H.264, which is fine for a speed ratio --
both encoders see the same input -- and is the reason these carry no BD claim.

`perseverance_*` is NASA/JPL-Caltech, public domain, SVS item 31250
(<https://svs.gsfc.nasa.gov/31250/>), `Perseverance-landing-1080p.mp4`. The
window is 15s from 168s: that file has exactly one cut-free run (27.5-194s),
and within it motion sits flat around 0.4-1.5 until 138s before peaking at 5.2
over 168-185s, the touchdown and the dust plume. Mean luma 80, minimum 76, so
no dark stretch. The 720p file is the same window, Lanczos.

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
