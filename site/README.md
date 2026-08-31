# site/ — the GitHub Pages source

Everything published at <https://terranvigil.github.io/yah264/> is built from
this directory. Edit the markdown, rebuild, look at it.

## Editing

    scripts/site.sh            # build and open the home page
    scripts/site.sh --serve    # build and serve on http://localhost:8000

First run creates a venv under `local/sitevenv` (git-ignored) and installs
`markdown`. Nothing else is needed, and no Ruby is involved.

One page per file. The `---` block at the top sets the browser title and the
meta description. Everything after it is ordinary markdown. Links between
pages are written with the `.md` extension and rewritten to `.html` at build
time, so they work both on GitHub and on the site.

Leave a note for me anywhere with an HTML comment. It won't render:

    <!-- TODO(owner): this claim is too strong -->

## What's where

| file | page |
|---|---|
| `index.md` | home |
| `start.md` | getting started |
| `design.md` | design |
| `results.md` | results |
| `_layout.html` | the shell every page is poured into |
| `assets/site.css` | shared styles, tokens matched to the explainers |

`docs/how-h264-works.html` and `docs/threading.html` are copied into the build
untouched. They carry their own styles and are edited in `docs/`, not here.
The nav order lives in `NAV` at the top of `scripts/build_site.py`.

## Deploying

CI runs the same `scripts/build_site.py` and publishes `site/_build` on every
push to main that touches this directory. A local preview and the deployed
site come out of one renderer, so they can't drift.

Repo Settings → Pages → Source must be set to **GitHub Actions** once, by hand.
