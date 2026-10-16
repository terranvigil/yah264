# yah264 comparison players

Two self-contained, dark-theme browser players for eyeballing yah264 output.
They're adapted from the veo project's comparison player.

- `compare.html` is a side-by-side slider comparing **two encodings** of the
  same source (yah264 vs x264, or shot-based vs regular yah264, etc.). Drag the
  divider to wipe between A and B. The timeline draws both encodings'
  per-frame VMAF as two curves, plus an A-minus-B delta curve. Quality-dip
  markers (warning/critical) sit on the timeline. Click one to jump to that
  frame. A mode switch toggles the markers between "dips in the worse encoding"
  and "frames with the largest A vs B gap".
- `inspect.html` shows a single encoding. It maps per-frame VMAF onto a big
  timeline graph so you can click low regions (or the dip buttons, or "jump to
  worst frame") and inspect those frames. If the data file has two encodings it
  adds a dropdown to switch between them.

Each is one file with no external dependencies. They fetch a JSON data file
(`?data=NAME.json`, defaulting to `data-compare.json` / `data-inspect.json`)
that `make_vmaf.py` writes.

## Workflow

Encode two clips from the same Y4M source. Matched settings keep the
comparison honest. These are the x264 baseline settings `bench/bench.py`
uses:

```sh
SRC=src.y4m
build/cli/yah264 --input-y4m $SRC --qp 32 -o yah264.264
x264 --qp 32 --keyint infinite --no-scenecut --ref 1 --bframes 0 \
     --no-cabac --profile baseline --preset medium \
     --demuxer y4m --input-csp i420 -o x264.264 $SRC
```

Build the VMAF data and browser-playable MP4s into a `site/` directory:

```sh
tools/players/make_vmaf.py --reference $SRC \
    --enc yah264:yah264.264 --enc x264:x264.264 -o site
```

`--enc` takes `LABEL:PATH`. The label is what shows on the pane and in the
stats. Inputs can be `.264` Annex-B streams, which get remuxed to MP4, or
`.mp4`/`.y4m`, which get transcoded near-lossless. Pass one `--enc` for a
single-encoding inspection file instead. VMAF model selection matches
`bench/bench.py`. It prefers the June 2026 v1 model if this libvmaf build has
it and falls back to v0.6.1 plus its NEG variant. Point at an explicit model
with `YAH264_VMAF_MODEL=/path/to/model.json`.

Serve the directory and open a player. Browsers won't load `<video>` over
`file://`.

```sh
tools/players/serve.sh site        # http://localhost:8787
# then open http://localhost:8787/compare.html
#           http://localhost:8787/inspect.html?data=data-inspect.json
```

`serve.sh` is a thin wrapper over `python3 -m http.server` that also symlinks
the two HTML files into the served directory for you.

## Keyboard

Space play/pause, Left/Right step one frame, `[` / `]` jump to previous/next
dip marker.

## Data format

`make_vmaf.py` emits:

```json
{
  "reference": "reference.mp4",
  "fps": 30.0,
  "model": "vmaf",
  "encodings": [
    { "label": "yah264", "video": "encA.mp4",
      "frames": [{ "frame": 0, "time": 0.0, "vmaf": 92.29 }],
      "avgVmaf": 93.86, "minVmaf": 92.29, "maxVmaf": 95.71 }
  ]
}
```

There's one `encodings` entry per `--enc`. `compare.html` needs two.
`inspect.html` needs one and uses more if present. The player computes dips
from the `frames` array, so you can retune thresholds without regenerating the
JSON.
