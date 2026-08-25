# next264 comparison players

Two self-contained, dark-theme browser players for eyeballing next264 output,
adapted from the veo project's comparison player.

- `compare.html` - side-by-side slider comparing **two encodings** of the same
  source (next264 vs x264, or shot-based vs regular next264, etc.). Drag the
  divider to wipe between A and B. Both encodings' per-frame VMAF are drawn as
  two curves on the timeline, plus an A-minus-B delta curve. Quality-dip markers
  (warning/critical) sit on the timeline; click one to jump to that frame. A
  mode switch toggles the markers between "dips in the worse encoding" and
  "frames with the largest A vs B gap".
- `inspect.html` - single encoding. Maps per-frame VMAF onto a big timeline
  graph so you can click low regions (or the dip buttons, or "jump to worst
  frame") and inspect those frames. If the data file has two encodings it adds a
  dropdown to switch between them.

Both are one file each, no external dependencies. They fetch a JSON data file
(`?data=NAME.json`, defaulting to `data-compare.json` / `data-inspect.json`)
that `make_vmaf.py` writes.

## Workflow

Encode two clips from the same Y4M source (matched settings make the comparison
honest; this is the x264 baseline `bench/bench.py` uses):

```sh
SRC=src.y4m
build/cli/next264 --input-y4m $SRC --qp 32 -o next264.264
x264 --qp 32 --keyint infinite --no-scenecut --ref 1 --bframes 0 \
     --no-cabac --profile baseline --preset medium \
     --demuxer y4m --input-csp i420 -o x264.264 $SRC
```

Build the VMAF data and browser-playable MP4s into a `site/` directory:

```sh
tools/players/make_vmaf.py --reference $SRC \
    --enc next264:next264.264 --enc x264:x264.264 -o site
```

`--enc` takes `LABEL:PATH`; the label is what shows on the pane and in the
stats. Inputs can be `.264` Annex-B streams (remuxed to MP4) or `.mp4`/`.y4m`
(transcoded near-lossless). Pass one `--enc` for a single-encoding inspection
file instead. VMAF model selection matches `bench/bench.py`: it prefers the
June 2026 v1 model if this libvmaf build has it, else v0.6.1 plus its NEG
variant. Point at an explicit model with `NEXT264_VMAF_MODEL=/path/to/model.json`.

Serve the directory (browsers won't load `<video>` over `file://`) and open a
player:

```sh
tools/players/serve.sh site        # http://localhost:8787
# then open http://localhost:8787/compare.html
#           http://localhost:8787/inspect.html?data=data-inspect.json
```

`serve.sh` is a thin wrapper over `python3 -m http.server`; it symlinks the two
HTML files into the served directory for you.

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
    { "label": "next264", "video": "encA.mp4",
      "frames": [{ "frame": 0, "time": 0.0, "vmaf": 92.29 }],
      "avgVmaf": 93.86, "minVmaf": 92.29, "maxVmaf": 95.71 }
  ]
}
```

One `encodings` entry per `--enc`. `compare.html` needs two; `inspect.html`
needs one (and uses more if present). Dips are computed in the player from the
`frames` array, so you can retune thresholds without regenerating the JSON.
