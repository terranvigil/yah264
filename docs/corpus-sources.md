# Where the test and tuning material comes from

There are two corpora, and keeping them apart is a rule.

## The rule, first

**The gate corpus is TEST-ONLY, permanently.** Nothing that tunes a constant, and
nothing that trains a model, may see a clip that a gate scores. A model that has
been shown the gate produces a number nobody can interpret. The failure is
silent: everything still runs, and the number just stops meaning what it says.

Our gate clips came largely from Xiph's derf collection, so drawing training
material from derf is exactly where an accidental overlap would come from.

## Gate corpus, `tests/corpus/`

Class is in `tests/corpus/CLASSES`. The speed board's clips and their
calibrated operating points are in `scripts/parity-clips.sh`; the HD band's
are in `scripts/hd_band.py`. `scripts/fetch_corpus.sh` fetches them on the
user's own machine. Nothing is redistributed.

| clip | res | class | source |
|---|---|---|---|
| akiyo, bus, coastguard, foreman, mobile, stefan | CIF | static / motion / detail | Xiph derf standard sequences |
| ducks_720p, park_joy_720p | 720p | motion | Xiph derf |
| fourpeople_720p | 720p | static (conference, natural) | Xiph derf (`FourPeople_1280x720_60`), JCT-VC class E |
| uneven_720p | 720p | | mislabeled on disk: its container frame rate doesn't match its content. Kept, but every script reads the rate off the clip |
| touchdown_1080p | 1080p | | the 4:2:2 ORIGINAL. Never put it in a 4:2:0 sweep. It's `scripts/conformance.sh`'s only 4:2:2 recon-match coverage, so don't delete it |
| touchdown_420 | 1080p | | the converted 4:2:0 version. Usable for speed and conformance, NOT for BD: three in-band ladders of one encode pair read +4.27%, +61.01% and +1.31% |
| sintel_720p | 720p | animation (3D CGI) | Blender open movie, CC BY 3.0 |
| bbb_720p | 720p | animation (3D CGI) | Big Buck Bunny, Blender, CC BY 3.0 (the 30 fps 1080p remaster; `scripts/fetch_corpus.sh --review` cuts it). 450 frames from 9m45s, the most sustained-motion window that isn't also a night scene |
| sita_720p | 720p | animation (hand-drawn 2D) | Sita Sings the Blues, **Public Domain Mark 1.0**, archive.org item `sita-sings-the-blues_202403`. 140 frames from 35m, the longest cut-free run in a flat-color sequence |

Both animation kinds are present on purpose. They disagree sharply against
x264 (CGI is a lead, hand-drawn a deficit), so quote which kind, never
"animation" (docs/animation.md).

samsung_720p sits in older records and the legacy board set. It's vendor
material with no license and can't be fetched.

### The resolution-balance set, `scripts/fetch_corpus.sh --res` (about 12 GB)

These keep a "corpus median" from being a CIF decision, and keep the 1080p
class from resting on one clip.

| clip | res | class | what it is |
|---|---|---|---|
| shields_720p, parkrun_720p, stockholm_720p | 720p | detail | the SVT "ter" pans, detail under motion |
| in_to_tree_720p | 720p | detail | slow zoom into foliage |
| old_town_720p | 720p | grain | aerial pan over roofs |
| fourpeople_720p | 720p | static | videoconference |
| blue_sky_1080p | 1080p | motion | slow rotation over low texture |
| pedestrian_1080p | 1080p | static | fixed camera, walking people |
| riverbed_1080p | 1080p | detail | water at the edge of noise, the hardest one here |
| station2_1080p | 1080p | detail | pan over fine rail detail |
| sunflower_1080p | 1080p | static | smooth close-up |
| crowd_run_1080p | 1080p | crowd | dense motion at 50 fps |

All are native resolution, 8-bit 4:2:0, from Xiph's derf host. Sources: the
SVT High Definition Multi Format test set (shields, parkrun, stockholm,
in_to_tree, old_town, crowd_run), the TUM 1080p25 set (blue_sky, pedestrian,
riverbed, station2, sunflower), and JCT-VC class E (fourpeople). These carry
research-use terms instead of a Creative Commons license, the same footing as
the CIF gate clips.

Five of them sit on the ten-clip speed board (fourpeople, shields, sunflower,
pedestrian, riverbed), and those five plus crowd_run sit in the HD band. Adding
a clip to a band ladder re-medians every published number, so promotion is an
owner call.

**Two exclusions, both deliberate.**

The **Netflix 4K set on the same host is off limits for gate use**, and so is
anything Harmonics-derived. The ML training corpus (BVI-AOM) draws on
BVI-Texture, IRIS, Harmonics, Videvo, SJTU, MCL-JCV, LIVE-Netflix and Yonsei
material. A gate clip that also sits in the training set breaks the
train/test split silently. None of the SVT, TUM or class-E sequences appears
among the 178 BVI-AOM source names.

The **aspen / red_kayak / speed_bag / snow_mnt / west_wind_easy /
rush_field_cuts / controlled_burn** group is also absent. `touchdown_pass` comes
from that set and is the 4:2:2 clip already in this tree. Treat the whole
group as suspect until a header says otherwise.

**Calibrated operating points** come from `scripts/parity-clip-calib.sh` on
6-second windows, preset medium, threads 1. Each is the lowest ABR target that
puts us in VMAF 0.6.1 88-94 with our own rate error inside a few percent. Any
new gate clip gets its operating point the same way. Don't pick a bitrate by
hand. old_town and stockholm have no calibrated point: our ABR runs 4-11% over
target across the middle of both ladders, so no point in the band has a clean
rate. ABR rate error is content-dependent for both encoders, and window length
and thread count both move it, so quote neither without naming them.

**Don't read an ABR calibration point as a quality verdict.** At a matched ABR
target x264 can read 4-7 VMAF ahead on clips (stockholm, shields, blue_sky)
where the CRF BD curve puts us well ahead. That gap is ABR allocation, not
compression.

### Windows that are timed, not scored

`tests/corpus` also holds cuts that exist only to be timed at a matched CRF
point through `scripts/ffboard.py`: `bbb{10,15,30}s_1080p_o120` and
`perseverance_{1080p,720p}`. They aren't gate clips. They have no class and no
calibrated operating point. Promoting one means running
`scripts/parity-clip-calib.sh` first, same as any other candidate.

Both sources are already-compressed H.264, which is why these carry no BD claim.
They're fine for a speed ratio, because both encoders see the same input.

`perseverance_*` is NASA/JPL-Caltech, public domain, SVS item 31250
(<https://svs.gsfc.nasa.gov/31250/>), `Perseverance-landing-1080p.mp4`.
`scripts/fetch_corpus.sh --review` fetches it and cuts both windows. The
window is 15 s from 168 s. That file has exactly one cut-free run (27.5-194 s).
Within it, motion sits flat around 0.4-1.5 until 138 s and then peaks at 5.2
over 168-185 s, the touchdown and the dust plume. Mean luma is 80 and the
minimum 76, so there's no dark stretch. The 720p file is the same window,
Lanczos.

### Pulling a clip out of a long source

A window can come out of a large master by HTTP range request, without
downloading the file (that's how `sita_720p` came out of a 16 GB master):

    ffmpeg -ss <seconds> -i <https-url> -frames:v N -vf scale=1280:720 ...

Pick the window by measuring, not by eye: flat-block share, mean luma (to
avoid night scenes) and a scene-cut count from frame-to-frame luma jumps.

## Training corpus, `/Volumes/seagate/media/train-corpus/bvi-aom/`

**BVI-AOM**, University of Bristol. 956 sequences from 239 unique 4K sources,
downsampled with Lanczos-3 to four resolution classes. Every sequence is 64
frames with no scene cuts, 10-bit 4:2:0, losslessly compressed in H.264 so
ffmpeg extracts raw. The license is a UoB custom grant for coding-standards
development, training and evaluation. Paper: arXiv 2408.03265. Index:
<https://github.com/fan-aaron-zhang/bvi-aom>

**We hold the D tier**, 239 sequences at 480x272, 1.46 GB. That's every unique
source at the cheapest pixel cost. A 64-frame 480x272 sequence already yields
far more labeled macroblocks than a model of this size needs, and the higher
tiers carry the same 239 scenes.

The dataset is one tarball per resolution class, so a higher tier is one file:

| file | size | class |
|---|--:|---|
| `272p.tar.gz` | 1.46 GB | D, 480x272 (held) |
| `544p.tar.gz` | 5.88 GB | C, 960x544 |
| `1088p.tar.gz` | 23.3 GB | B, 1920x1088 |
| `2176p_part_a..f.tar.gz` | ~92 GB | A, 3840x2176 |

**Use the path-style S3 endpoint.** The vanity hostname
`download.opencontent.netflix.com` doesn't resolve from every network, and
answers some clients with an empty response. The bucket answers fine as:

    https://s3.amazonaws.com/download.opencontent.netflix.com/bvi_aom_dataset/<file>

List it with `?list-type=2&prefix=bvi_aom_dataset/`. A dead hostname isn't a
dead host: try the service's own endpoint form before concluding anything is
unreachable.

## Long-form, multi-shot: `local/corpus/longform/` (`fetch_corpus.sh --longform`)

Whole films, for the shot-based and orchestration work (docs/shot-based.md).
The gate corpus and the ten-clip board are single shots by construction, and
the multi-shot concatenations (`scripts/make_multishot.py`) have five hard cuts
each. A per-shot allocator or a hull orchestrator needs hundreds of shots of
one title with real editing, from sources anyone can fetch by URL. These stay
out of `tests/corpus/` and out of every gate. Nothing here may tune a default
that the gate corpus then judges.

Every entry is CC BY or freer, so windows, tone-maps and results can be
published with attribution. The script pins each SHA256 and verifies it.

| title | what it is | frames / length | master we fetch | size | license |
|---|---|---|---|---|---|
| Big Buck Bunny (Blender, 2008) | CGI comedy, 16:9 | 14,315 f, 9:56 at 24 fps | Xiph Y4M 1280x720p24, lossless from the production render: `https://media.xiph.org/video/derf/y4m/big_buck_bunny_720p24.y4m.xz` (1080p24 is 42 GB) | 5.0 GB xz | CC BY 3.0 |
| Elephants Dream (Blender, 2006) | CGI, 16:9 | 15,691 f, 10:54 at 24 fps | `https://media.xiph.org/video/derf/y4m/elephants_dream_720p24.y4m.xz` (1080p24 is 46 GB) | 4.3 GB xz | CC BY 2.5 |
| Sintel (Blender, 2010) | CGI, 2.35:1 | 21,312 f, 14:48 at 24 fps | `https://media.xiph.org/sintel/sintel-1280.y4m` (1280x544; 4K xz is 53 GB) | 22.3 GB | CC BY 3.0 |
| Tears of Steel (Blender, 2012) | live action + VFX, 2.40:1: the one live-action film with a lossless open master | 17,620 f, 12:14 at 24 fps | publisher's 720p mov, `https://download.blender.org/demo/movies/ToS/tears_of_steel_720p.mov` (lossy; the lossless master is `https://media.xiph.org/tearsofsteel/tearsofsteel-4k.y4m.xz`, 66 GB, SHA1 `3c3113e0...89f2`, and a 1080 PNG set) | 372 MB | CC BY 3.0 |
| Meridian (Netflix Open Content, 2016) | live-action noir short with a story: many real cuts, dark scenes, rain, close-ups | 11:58 at 59.94 fps, ~43,000 f | publisher's MP4, UHD 4K 59.94p HDR P3/PQ: `https://s3.amazonaws.com/download.opencontent.netflix.com/Meridian/Meridian_UHD4k5994_HDR_P3PQ.mp4` (also TIFF frames and a 769 GB IMF in the bucket) | 0.85 GB | CC BY 4.0 |
| Chimera (Netflix Open Content, 2014) | live-action montage "representative of existing titles", DCI 4K | 23.98p and 59.94p versions | `https://s3.amazonaws.com/download.opencontent.netflix.com/Chimera/Chimera_DCI4k2398p_HDR_P3PQ.mp4` (59.94p is 10.9 GB) | 2.5 GB | CC BY 4.0 |
| Cosmos Laundromat (Blender / Netflix, 2015) | CGI, 2.40:1 | 18,191 f, 12:38 at 24 fps | `https://s3.amazonaws.com/download.opencontent.netflix.com/CosmosLaundromat/CosmosLaundromat_2k24p_HDR_P3PQ.mp4` (2048x858; Xiph has PNG masters, HDR and 8-bit) | 0.73 GB | CC BY 4.0 |

SHA256, as `scripts/fetch_corpus.sh` pins them:

| file | sha256 |
|---|---|
| `big_buck_bunny_720p24.y4m.xz` | `25b7dfc5ace258ec2053c4865b3962d49b91fc60b8b141427bd4165376710332` |
| `elephants_dream_720p24.y4m.xz` | `be290da499d90d212c1b347e2d8f99475eca4894eab1b65aa1bcc77d6c647062` |
| `sintel-1280.y4m` | `c8be84c33039cfd80fb068dad94880c29562c1ee4bf171eceadcdc34c1d82fc0` (Xiph's published value) |
| `tears_of_steel_720p.mov` | `efa9062d9cdb7a338e40ad530dfdf234806743f29ae6a1a136b97ece4e588e8f` |
| `Meridian_UHD4k5994_HDR_P3PQ.mp4` | `e14ff5ab8ce4cc90269e8ed45babcf6e25d7a073b71438b3e7ffee5901a7177e` |
| `Chimera_DCI4k2398p_HDR_P3PQ.mp4` | `91fe0144daf6fc352e1e72e07f77d37ad5ae87a9cc00dc85fd4a2aa7b5c4b5e9` |
| `CosmosLaundromat_2k24p_HDR_P3PQ.mp4` | `4762d0d77ce6c09371032e0c6b4dbda8ad781237b5264829857eefeed7a3b40a` |

Listed, not fetched by default:

| title | why it matters | where | size | license |
|---|---|---|---|---|
| Sita Sings the Blues (Nina Paley, 2008) | the only feature-length hand-drawn 2D source with an open master, and hand-drawn is where we measurably lose; 82 minutes | `https://media.xiph.org/video/derf/y4m/sita_sings_the_blues_720p24.y4m.xz` (117,714 f; 1080p is 341 GB) | 152 GB xz | CC0 |
| El Fuente (Netflix Open Content, 2013) | the 96-shot title the convex-hull papers use | the public bucket holds only two 4K60 10-bit Y4M clips, FoodMarket (15.9 GB) and Boat (8 GB): `https://download.opencontent.netflix.com/ElFuente/`; the full title isn't public | 24 GB | CC BY 4.0 |
| Sparks, Sol Levante, Nocturne (Netflix Open Content) | 4K HDR HFR; Sol Levante is the one hand-drawn anime master | `https://download.opencontent.netflix.com/` (IMF, EXR, ProRes; no MP4 for Sparks) | very large | CC BY 4.0 |
| SVT Open Content 2022, Natural Complexity | 2160p50 professional captures, short sequences, not long-form | `ftp://svtopencontent.svt.se` | | CC BY 4.0 |
| UVG | 16 x 4K, 8 at 120 fps and 8 at 50 fps, 2.5-12 s each, single shots | `https://ultravideo.fi/dataset.html` | | CC BY-NC (owner accepted for this project) |
| AWCY `objective-1` | the AOM/NETVC benchmark set, short clips | `https://media.xiph.org/video/derf/` | 13 GB | open test material |

Where published work draws its material, so our numbers can sit next to theirs:
dynamic-optimizer and convex-hull papers evaluate on Netflix titles (El
Fuente's 96 shots, Chimera, Meridian); the VMAF public dataset is short clips
including BigBuckBunny, ElFuente and Seeking; the AV1 common test conditions
take short excerpts of Chimera and El Fuente. Our `ducks`, `park_joy`,
`in_to_tree` and `old_town` are the SVT 2006 set. Sintel is also the source of
the gate clip `sintel_720p`, so a study on the whole film must exclude that
window from anything it tunes.

Recipes, fixed so results reproduce:

- **Netflix bucket access**: use the path-style endpoint
  `https://s3.amazonaws.com/download.opencontent.netflix.com/<key>`; `?prefix=Meridian/`
  on it lists the folder.
- **Decode a window** (frame-accurate, from a Y4M or MP4):
  `ffmpeg -v error -ss <s> -i <src> -frames:v <n> -vf format=yuv420p -f yuv4mpegpipe out.y4m`.
  Stream the xz Y4Ms: `xz -dc big_buck_bunny_720p24.y4m.xz | yah264 --input-y4m - ...`.
- **Tone-map the Netflix HDR masters to 8-bit BT.709** (the only lossy step,
  identical for everyone). The MP4s are 8-bit H.264 carrying a PQ signal with
  NO color tags, so the tags are declared first. Without them zimg reports
  "no path between colorspaces":
  `ffmpeg -v error -i <hdr.mp4> -vf "setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv,zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv,format=yuv420p,scale=1920:-2:flags=lanczos" -f yuv4mpegpipe out.y4m`
  (frame rate untouched; add `-r 29.97` only when a study says so). It needs an
  ffmpeg built with libzimg (`zscale`). Meridian at 1080p59.94 is 134 GB as
  Y4M, so decode windows, never the whole title. A 600-frame window at 2:00
  encodes to a 5-shot table at CRF 26; the frame at 2:08 (a lit office, window
  highlights held, skin tones neutral) is the visual check.
- **Shot boundaries**: `yah264 --shot-table` on the window prints the cuts the
  encoder itself will use. `scripts/multishot_bd.py` takes any `.cuts` file.

## Libraries worth pulling from next

Assessed but not held, in order of usefulness:

| library | what it is | license | why we'd want it |
|---|---|---|---|
| AWCY `objective-1` | AOM/IETF NETVC benchmark set, 13 GB (`-fast` subset 1.9 GB) | open test material | the obvious second training set, and a yardstick others report against |
| UVG | 16 x 4K, 8 at 120 fps and 8 at 50 fps | CC BY-NC | high frame rate, few sources. Non-commercial terms accepted for this project by the owner |
| BVI-DVC | 800 sequences | research use only | the set BVI-AOM replaced; skip, its terms are worse |
| Xiph / derf | assorted, SD-heavy | mostly free | **our gate came from here.** Training draws need the explicit no-overlap check |

Netflix Open Content and the Blender films are held already (the long-form
section above).

Two gaps are known. The training set has no hand-drawn 2D source at all, which
matters because hand-drawn is where we measurably lose. And Sol Levante
(Netflix Open Content, CC BY 4.0) is the only true hand-drawn anime master we
found; try it through the path-style endpoint.
