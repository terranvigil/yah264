#!/usr/bin/env python3
"""Build the GitHub Pages site from site/*.md into site/_build/.

Same script runs locally and in CI, so a local preview is byte-identical to
what deploys. Requires the `markdown` package; scripts/site.sh sets up a venv.

Usage:
    python3 scripts/build_site.py [--serve]
"""

import argparse
import html
import http.server
import pathlib
import re
import shutil
import socketserver
import sys

try:
    import markdown
except ImportError:
    sys.exit("missing dependency: pip install markdown (or run scripts/site.sh)")

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "site"
OUT = SRC / "_build"
DOCS = ROOT / "docs"

# Nav order. Left column is the source page (or a docs/ HTML file copied in
# verbatim); right is the label in the top bar.
NAV = [
    ("index.html", "Home"),
    ("encoding.html", "Encoding"),
    ("how-h264-works.html", "H.264"),
    ("start.html", "Getting started"),
    ("design.html", "Design"),
    ("results.html", "Results"),
    ("methodology.html", "Methodology"),
    ("story.html", "Story"),
]

# Standalone interactive pages copied in as-is. They carry their own styles.
VERBATIM = {
    "how-h264-works.html": DOCS / "how-h264-works.html",
    "threading.html": DOCS / "threading.html",
}


def parse_front_matter(text):
    """Read a simple `key: value` block delimited by --- at the top of a file."""
    meta = {}
    if text.startswith("---\n"):
        end = text.find("\n---\n", 4)
        if end != -1:
            for line in text[4:end].splitlines():
                if ":" in line:
                    k, v = line.split(":", 1)
                    meta[k.strip()] = v.strip()
            text = text[end + 5 :]
    return meta, text.lstrip("\n")


def render_nav(current):
    out = []
    for href, label in NAV:
        mark = ' aria-current="page"' if href == current else ""
        out.append(f'<a href="{href}"{mark}>{html.escape(label)}</a>')
    return "\n      ".join(out)


def render_toc(tokens, meta):
    """The section rail. Only pages with enough sections get one; a three-heading
    page reads better without a column of links beside it.

    toc_tokens nests by heading level, so the h2s of a page that opens with an
    h1 are that h1's children rather than top-level entries. Walk the tree.
    """

    def walk(nodes, out):
        for n in nodes:
            if n.get("level") == 2:
                out.append(n)
            walk(n.get("children") or [], out)
        return out

    items = walk(tokens, [])
    if len(items) < 4:
        return ""
    label = meta.get("toc_label", "On this page")
    rows = "".join(
        f'<li><a href="#{t["id"]}" data-s="{t["id"]}">{html.escape(t["name"])}</a></li>'
        for t in items
    )
    return f'<div class="lbl">{html.escape(label)}</div><ol>{rows}</ol>'


def build():
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)

    layout = (SRC / "_layout.html").read_text()
    md = markdown.Markdown(extensions=["extra", "toc", "sane_lists", "smarty"])

    shutil.copytree(SRC / "assets", OUT / "assets")

    for name, path in VERBATIM.items():
        if path.exists():
            shutil.copy(path, OUT / name)
        else:
            print(f"  warning: {path} missing, skipping {name}")

    pages = 0
    for src in sorted(SRC.glob("*.md")):
        if src.name.startswith("_") or src.name == "README.md":
            continue
        meta, body = parse_front_matter(src.read_text())
        md.reset()
        content = md.convert(body)
        toc = render_toc(getattr(md, "toc_tokens", []), meta)
        target = src.with_suffix(".html").name
        title = meta.get("title", src.stem)
        page = (
            layout.replace("{{content}}", content)
            .replace("{{title}}", html.escape(title))
            .replace("{{description}}", html.escape(meta.get("description", "")))
            .replace("{{nav}}", render_nav(target))
            .replace("{{toc}}", toc)
            .replace("{{root}}", "")
        )
        # Links written as page.md in source resolve to page.html when built.
        page = re.sub(r'href="([^"#:]+)\.md((?:#[^"]*)?)"', r'href="\1.html\2"', page)
        (OUT / target).write_text(page)
        pages += 1

    print(f"built {pages} pages + {len(VERBATIM)} verbatim -> {OUT}")


def serve(port=8000):
    handler = lambda *a, **kw: http.server.SimpleHTTPRequestHandler(
        *a, directory=str(OUT), **kw
    )
    with socketserver.TCPServer(("", port), handler) as httpd:
        print(f"serving {OUT} at http://localhost:{port}/  (ctrl-c to stop)")
        httpd.serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--serve", action="store_true", help="serve after building")
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()
    build()
    if args.serve:
        serve(args.port)
