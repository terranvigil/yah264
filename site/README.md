# site/ — the GitHub Pages source

Everything published at <https://terranvigil.github.io/yah264/> is built from
this directory. Edit the markdown, rebuild, look at it.

## Editing

    scripts/site.sh            # build and open the home page
    scripts/site.sh --serve    # build and serve on http://localhost:8000

The first run creates a venv under `local/sitevenv` (git-ignored) and installs
`markdown`. Nothing else is needed. No Ruby is involved.

One page per file. The `---` block at the top sets the browser title and the
meta description. Everything after it is ordinary markdown. Links between
pages use the `.md` extension and get rewritten to `.html` at build time, so
they work both on GitHub and on the site.

Leave a note for me anywhere with an HTML comment. It won't render:

    <!-- TODO(owner): this claim is too strong -->

## What's where

| file | page |
|---|---|
| `../README.md` | home: the repository README, built into `index.html` |
| `encoding.md` | how video encoding works |
| `start.md` | getting started |
| `hdr.md` | HDR: what's supported, how to encode it, HDR-VMAF results |
| `design.md` | design |
| `architectures.md` | architectures |
| `results.md` | results |
| `check-it-yourself.md` | check it yourself |
| `blog.md` | blog |
| `methodology.md` | methodology, kept in the repository and not built (`UNPUBLISHED`) |
| `_layout.html` | the shell every page is poured into |
| `assets/site.css` | shared styles, tokens matched to the explainers |

`adopt()` in `scripts/build_site.py` pulls `docs/how-h264-works.html` and
`docs/threading.html` into the build. It restyles them onto the site's fonts
and color tokens and lifts their contents list into the site's rail. The
content, the layout and the figures stay as they are. Both pages are edited in
`docs/`, not here.
The nav order lives in `NAV` at the top of `scripts/build_site.py`.

## Deploying

On every push to main that touches this directory, CI runs the same
`scripts/build_site.py` and publishes `site/_build`. A local preview and the
deployed site come out of one renderer, so they can't drift.

Repo Settings → Pages → Source must be set to **GitHub Actions** once, by hand.
