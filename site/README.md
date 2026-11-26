# The GitHub Pages source

Everything at <https://terranvigil.github.io/yah264/> is built from this
directory.

## Editing

    scripts/site.sh            # build and open the home page
    scripts/site.sh --serve    # build and serve on http://localhost:8000

The first run creates a git-ignored venv under `local/sitevenv` with
`markdown`, the only dependency.

Each page is one file: a `---` block for the browser title and meta
description, then ordinary markdown. Links use `.md` and become `.html` at
build time, which makes them work on GitHub and on the site. An HTML comment leaves a
note for me without rendering:

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
`docs/threading.html` into the build, restyling them onto the site's fonts and
colors and lifting their contents list into the rail. The content, the layout and the
figures stay as they are. Edit both in `docs/`. The
nav order is `NAV` at the top of `scripts/build_site.py`.

## Deploying

A push to main that touches this directory makes CI run the same
`scripts/build_site.py` and publish `site/_build`, so preview and site can't
drift. Set the Pages source to GitHub Actions once, under Pages in the repo's
Settings.
