# Where the test and tuning material comes from

Two corpora, and the separation between them is a rule rather than a convention.

## The rule, first

**The gate corpus is TEST-ONLY, permanently.** Nothing that tunes a constant, and
nothing that trains a model, may see a clip that a gate scores. A model that has
been shown the gate produces a number nobody can interpret, and the failure is
silent: everything still runs, the number just stops meaning what it says.

Our gate clips came largely from Xiph's derf collection, so drawing training
material from derf is exactly where an accidental overlap would come from.

## Gate corpus, `tests/corpus/`

Sixteen clips. Class is in `tests/corpus/CLASSES`; the ten that form the speed
table and their calibrated operating points are in `scripts/parity-clips.sh`.

| clip | res | class | source |
|---|---|---|---|
| akiyo, bus, coastguard, foreman, mobile, stefan | CIF | static / motion / detail | Xiph derf standard sequences |
| ducks_720p, park_joy_720p | 720p | motion | Xiph derf |
| fourpeople_720p | 720p | static (conference, natural) | Xiph derf (`FourPeople_1280x720_60`); replaced samsung_720p in the board on 2026-09-04 (vendor material, no licence, not fetchable) |
| uneven_720p | 720p | | mislabeled on disk: its container frame rate does not match its content. Kept, but every script reads the rate off the clip |
| touchdown_1080p | 1080p | | the 4:2:2 ORIGINAL. Never put it in a 4:2:0 sweep. It is `scripts/conformance.sh`'s only 4:2:2 recon-match coverage, so do not delete it |
| touchdown_420 | 1080p | | the converted 4:2:0 version. Usable for speed and conformance, NOT for BD: three in-band ladders of one encode pair read +4.27%, +61.01% and +1.31% |
| sintel_720p | 720p | animation (3D CGI) | Blender open movie, CC-BY 3.0 |
| bbb_720p | 720p | animation (3D CGI) | Big Buck Bunny, Blender, CC-BY 3.0 (the 2013 30 fps 1080p remaster; `scripts/fetch_corpus.sh --review` cuts it). 450 frames from 9m45s, the most sustained-motion window that is not also a night scene |
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

**Calibrated operating points**, from `scripts/parity-clip-calib.sh`, 6-second
windows, preset medium, threads 1 -- the lowest ABR target landing us in VMAF
0.6.1 88-94 with our own rate error inside a few percent:

| clip | kbps | our rate err | our vmaf |
|---|--:|--:|--:|
| fourpeople_720p | 1600 | +1.3% | 92.12 |
| shields_720p | 2200 | +1.5% | 90.43 |
| in_to_tree_720p | 5000 | +2.5% | 88.76 |
| parkrun_720p | 6600 | +3.1% | 90.24 |
| old_town_720p, stockholm_720p | see note | | |
| blue_sky_1080p | 1500 | +1.1% | 88.08 |
| sunflower_1080p | 1500 | +2.6% | 90.71 |
| station2_1080p | 2000 | -0.4% | 89.41 |
| pedestrian_1080p | 2800 | -0.2% | 88.67 |
| riverbed_1080p | 12500 | -1.4% | 88.97 |
| crowd_run_1080p | 22000 | +1.0% | 90.67 |

Note on old_town and stockholm: our ABR runs 4-11% over target across the
middle of both ladders while x264 tracks within 3% there, so no point in the
band has a clean rate and their calibration is unfinished.

**That is a per-clip result and NOT a general rate-control defect** -- checked
2026-08-31 rather than assumed. At threads 1 and the same 6-second windows, on
the old corpus we land samsung +1.6%, park_joy +2.2%, ducks +4.0% against
x264's +3.8%, +6.4%, +5.8%: we are the tighter of the two on every one. At a
low rate both encoders miss badly in the same direction (bus_cif at 1000 kbps:
ours -16.2%, x264 -20.7%). ABR rate error here is content-dependent for both
encoders, and old_town is a clip where ours runs high rather than evidence of
a systemic overshoot. Window length and thread count both move it, so quote
neither without naming them.

**These clips are fetched, not yet promoted.** Adding one to a band ladder
re-medians every published number, so promotion is an owner call.

### What they say about where we stand

BD-rate against x264 medium, VMAF-NEG, CRF, 150-frame windows, points chosen
per clip to keep the curve off saturation. Negative means we spend fewer bits
for the same quality:

| clip | BD-rate | | clip | BD-rate |
|---|--:|---|---|--:|
| in_to_tree_720p | -24.18% | | pedestrian_1080p | -0.90% |
| stockholm_720p | -21.48% | | crowd_run_1080p | +1.76% |
| old_town_720p | -14.58% | | riverbed_1080p | +2.40% |
| station2_1080p | -13.69% | | blue_sky_1080p | +14.17% |
| shields_720p | -13.28% | | | |
| fourpeople_720p | -12.48% | | | |
| sunflower_1080p | -8.39% | | | |
| parkrun_720p | -1.10% | | | |

**720p median -13.93%, all six ahead. 1080p median +0.43%, three ahead and
three behind.** On content nothing here was ever tuned against, the quality
lead is a 720p result that does not survive to 1080p.

**Do not read the calibration table above as a quality verdict.** It is ABR at
a matched target, and it says the opposite: it has x264 ahead by 4-7 VMAF on
stockholm, shields and blue_sky, three clips where the BD curve puts us 13-21%
ahead. The difference is our ABR allocation, not our compression.

### Windows that are timed, not scored

`tests/corpus` also holds cuts that exist only to be timed at a matched CRF
point through `scripts/ffboard.py`: `bbb{10,15,30}s_1080p_o120` and
`perseverance_{1080p,720p}`. They are not gate clips. They have no class, no
calibrated operating point, and nothing scores them but the matched-CRF speed
table, which needs none of that. Promoting one means running
`scripts/parity-clip-calib.sh` first, same as any other candidate.

Both sources are already-compressed H.264, which is fine for a speed ratio --
both encoders see the same input -- and is the reason these carry no BD claim.

`perseverance_*` is NASA/JPL-Caltech, public domain, SVS item 31250
(<https://svs.gsfc.nasa.gov/31250/>), `Perseverance-landing-1080p.mp4`;
`scripts/fetch_corpus.sh --review` fetches it and cuts both windows. The
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

## Long-form, multi-shot: `local/corpus/longform/` (`fetch_corpus.sh --longform`)

Whole films, for the shot-based and orchestration work
(docs/shot-based-plan.md S4 onward). The gate corpus and the ten-clip board are
single shots by construction; the S0 concatenations (`scripts/make_multishot.py`)
have five hard cuts each. A per-shot allocator or a hull orchestrator needs
hundreds of shots of one title with real editing, and it needs sources anyone
can fetch by URL. Kept out of `tests/corpus/` and out of every gate: nothing
here may be used to tune a default that the gate corpus then judges.

Licence first: every entry is CC BY or freer, so windows, tone-maps and
results can be published with attribution.

| title | what it is | frames / length | master we fetch | size | licence | checksum |
|---|---|---|---|---|---|---|
| Big Buck Bunny (Blender, 2008) | CGI comedy, 16:9 | 14,315 f, 9:56 at 24 fps | Xiph Y4M 1280x720p24, lossless from the production render: `https://media.xiph.org/video/derf/y4m/big_buck_bunny_720p24.y4m.xz` (1080p24 is 42 GB) | 5.0 GB xz | CC BY 3.0 | `25b7dfc5...0332` (pinned in the script) |
| Elephants Dream (Blender, 2006) | CGI, 16:9 | 15,691 f, 10:54 at 24 fps | `https://media.xiph.org/video/derf/y4m/elephants_dream_720p24.y4m.xz` (1080p24 is 46 GB) | 4.3 GB xz | CC BY 2.5 | `be290da4...7062` (pinned) |
| Sintel (Blender, 2010) | CGI, 2.35:1 | 21,312 f, 14:48 at 24 fps | `https://media.xiph.org/sintel/sintel-1280.y4m` (1280x544; 4K xz is 53 GB) | 22.3 GB | CC BY 3.0 | Xiph SHA256 `c8be84c3...82fc0` (verified by the script) |
| Tears of Steel (Blender, 2012) | live action + VFX, 2.40:1: the one live-action film with a lossless open master | 17,620 f, 12:14 at 24 fps | publisher's 720p mov, `https://download.blender.org/demo/movies/ToS/tears_of_steel_720p.mov` (lossy; the lossless master is `https://media.xiph.org/tearsofsteel/tearsofsteel-4k.y4m.xz`, 66 GB, SHA1 `3c3113e0...89f2`, and a 1080 PNG set) | 372 MB | CC BY 3.0 | `efa9062d...8e8f` (pinned) |
| Meridian (Netflix Open Content, 2016) | live-action noir short with a story: many real cuts, dark scenes, rain, close-ups | 11:58 at 59.94 fps, ~43,000 f | publisher's MP4, UHD 4K 59.94p HDR P3/PQ: `https://download.opencontent.netflix.com/Meridian/Meridian_UHD4k5994_HDR_P3PQ.mp4` (also TIFF frames and a 769 GB IMF in the bucket) | 0.85 GB | CC BY 4.0 | `e14ff5ab...177e` (pinned) |
| Chimera (Netflix Open Content, 2014) | live-action montage "representative of existing titles", DCI 4K | 23.98p and 59.94p versions | `https://download.opencontent.netflix.com/Chimera/Chimera_DCI4k2398p_HDR_P3PQ.mp4` (59.94p is 10.9 GB) | 2.5 GB | CC BY 4.0 | `91fe0144...b5e9` (pinned) |
| Cosmos Laundromat (Blender / Netflix, 2015) | CGI, 2.40:1 | 18,191 f, 12:38 at 24 fps | `https://download.opencontent.netflix.com/CosmosLaundromat/CosmosLaundromat_2k24p_HDR_P3PQ.mp4` (2048x858; Xiph has PNG masters, HDR and 8-bit) | 0.73 GB | CC BY 4.0 | `4762d0d7...b40a` (pinned) |

Listed, not fetched by default:

| title | why it matters | where | size | licence |
|---|---|---|---|---|
| Sita Sings the Blues (Nina Paley, 2008) | the only feature-length hand-drawn 2D source with an open master, and hand-drawn is where we measurably lose; 82 minutes | `https://media.xiph.org/video/derf/y4m/sita_sings_the_blues_720p24.y4m.xz` (117,714 f; 1080p is 341 GB) | 152 GB xz | CC0 since 2013 (CC BY-SA 3.0 before) |
| El Fuente (Netflix Open Content, 2013) | the 96-shot title the convex-hull papers use | the public bucket holds only two 4K60 10-bit Y4M clips, FoodMarket (15.9 GB) and Boat (8 GB): `https://download.opencontent.netflix.com/ElFuente/`; the full title is not public | 24 GB | CC BY 4.0 |
| Sparks, Sol Levante, Nocturne (Netflix Open Content) | 4K HDR HFR; Sol Levante is the one anime master | `https://download.opencontent.netflix.com/` (IMF, EXR, ProRes; no MP4 for Sparks) | very large | CC BY 4.0 |
| SVT Open Content 2022, Natural Complexity | 2160p50 professional captures, short sequences, not long-form | `ftp://svtopencontent.svt.se` | | CC BY 4.0 |
| UVG | 16 x 4K 50/120 fps, 5-12 s each, single shots | `https://ultravideo.fi/dataset.html` | | CC BY-NC (owner accepted for this project) |
| AWCY `objective-1` | the AOM/NETVC benchmark set, short clips | `https://media.xiph.org/video/derf/` | 13 GB | open test material |

Who uses what, so our numbers can sit next to theirs: Netflix's dynamic
optimizer work and the RCN-Hull paper evaluate on Netflix titles (El Fuente's
96 shots, Chimera, Meridian); the ACM TOMM 2025 hull-prediction benchmark
built 300 UHD shots and encodes them with AVC, HEVC and VVC; the VMAF public
dataset (the `NFLX_dataset_public` in the vmaf repository, Google Drive on
request) is short clips including BigBuckBunny, ElFuente and Seeking; ATHENA's
VCA/OPTE per-title work uses their Video Complexity Dataset and Inter4K-derived
material, neither cleanly licensed; the AV1/AOM common test conditions take
short excerpts of Chimera and El Fuente (our `ducks`, `park_joy`, `in_to_tree`,
`old_town` are the SVT 2006 set that predates all of this). The Blender films
are the long-form material every open encoder project has used since 2008.
Sintel is also the source of our gate clip `sintel_720p`, so a study on the
whole film must exclude that window from anything it tunes.

Recipes, fixed so results reproduce:

- **Netflix bucket access**: the bucket's own hostname answers some clients with an empty
  response; the path-style endpoint `https://s3.amazonaws.com/download.opencontent.netflix.com/<key>`
  is the same object, and `?prefix=Meridian/` on it lists the folder.
- **Decode a window** (frame-accurate, from a Y4M or MP4):
  `ffmpeg -v error -ss <s> -i <src> -frames:v <n> -vf format=yuv420p -f yuv4mpegpipe out.y4m`;
  for the xz Y4Ms stream them, `xz -dc big_buck_bunny_720p24.y4m.xz | yah264 --input-y4m - ...`.
- **Tone-map the Netflix HDR masters to 8-bit BT.709** (the only lossy step,
  identical for everyone):
  `ffmpeg -v error -i <hdr.mp4> -vf "zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv,format=yuv420p,scale=1920:-2:flags=lanczos" -f yuv4mpegpipe out.y4m`
  (frame rate untouched; add `-r 29.97` only when a study says so). Needs an
  ffmpeg built with libzimg (`zscale`); the Homebrew formula takes
  `--with-zimg`, and the build on the dev box does not have it yet. Meridian
  at 1080p59.94 is 134 GB as Y4M: decode windows, never the whole title.
- **Shot boundaries**: `yah264 --shot-table` on the window prints the cuts the
  encoder itself will use; `scripts/multishot_bd.py` takes any `.cuts` file.

## Libraries worth pulling from next

Assessed but not held. Ordered by how useful they would be to us.

| library | what it is | licence | why we would want it |
|---|---|---|---|
| AWCY `objective-1` | AOM/IETF NETVC benchmark set, 13 GB (`-fast` subset 1.9 GB) | open test material | the obvious second training set, and a yardstick others report against |
| Netflix Open Content | El Fuente, Chimera, Meridian | CC-BY 4.0 | cleanest licence of any option, professional cinematic sources. **Fetched now, see the long-form section** |
| UVG | 5 x 4K 120fps | CC-BY-NC | high frame rate, few sources. Non-commercial terms accepted for this project by owner call |
| BVI-DVC | 800 sequences | research use only | the precedent BVI-AOM replaced; skip, its terms are worse |
| Xiph / derf | assorted SD-heavy | mostly free | **our gate came from here.** Training draws need the explicit no-overlap check |
| Blender open movies | rendered features | CC-BY 3.0 | more animation, and the only easy source of clean CGI. **Fetched now, see the long-form section** |

Two gaps we know about. There is no hand-drawn 2D source in the training set at
all, which matters now that hand-drawn is where we measurably lose. And Sol
Levante (Netflix Open Content, CC BY 4.0) is the only true hand-drawn anime
master we found; it was unreachable when we looked and is worth retrying with the
endpoint-form trick above.
