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
    ("threading.html", "Threading"),
    ("results.html", "Results"),
    ("methodology.html", "Methodology"),
    ("story.html", "Story"),
]

# Hand-written standalone explainers. They are HTML rather than markdown, so
# they cannot go through the pipeline the other pages use, and copying them in
# as-is meant the nav dropped readers onto a page in a different palette, in
# different fonts, with no way back. They are restyled at build time instead:
# same tokens, same typefaces, same header and footer, their own layout and
# figures untouched. The transform lives here rather than in the files so they
# stay standalone and still open correctly straight out of docs/.
ADOPTED = {
    "how-h264-works.html": DOCS / "how-h264-works.html",
    "threading.html": DOCS / "threading.html",
}

# Their type stacks onto the site's. Longest first: the Avenir variants are
# prefixes of each other and a short one would match inside a long one.
FONT_MAP = [
    ('Charter, "Bitstream Charter", "Iowan Old Style", Georgia, serif',
     '"IBM Plex Serif", Georgia, serif'),
    ('"Avenir Next", Avenir, "Helvetica Neue", Helvetica, Arial, sans-serif',
     '"IBM Plex Sans", system-ui, -apple-system, sans-serif'),
    ('"Avenir Next", Avenir, "Helvetica Neue", sans-serif',
     '"IBM Plex Sans", system-ui, -apple-system, sans-serif'),
    ('"Avenir Next", Avenir, sans-serif',
     '"IBM Plex Sans", system-ui, -apple-system, sans-serif'),
    ('system-ui, -apple-system, "Segoe UI", sans-serif',
     '"IBM Plex Sans", system-ui, -apple-system, sans-serif'),
    ('ui-monospace, "SF Mono", Menlo, Consolas, monospace',
     '"IBM Plex Mono", ui-monospace, Menlo, monospace'),
]

# Only the tokens that carry the site's identity move. The structural ones --
# surface, hairline, grid, baseline, code-bg, the s1-s6 figure series -- are
# what the SVG figures are drawn against and are left alone, because recolouring
# a diagram's series is a design decision and not a palette swap.
TOKEN_MAP = [
    ("#f8f8f6", "#f8f9fa"),                        # paper
    ("#16181a", "#212529"),                        # ink
    ("#4d5257", "#495057"),                        # ink-2
    ("#8a8f94", "#6b7178"),                        # ink-3
    ("#0e7c8c", "#6741d9"),                        # accent, light
    ("rgba(14, 124, 140, 0.08)", "rgba(103, 65, 217, 0.08)"),
    ("rgba(14,124,140,0.08)", "rgba(103,65,217,0.08)"),
    ("#5ac8da", "#845ef7"),                        # accent, dark
    ("rgba(90, 200, 218, 0.10)", "rgba(132, 94, 247, 0.14)"),
    ("rgba(90,200,218,0.10)", "rgba(132,94,247,0.14)"),
]

FONTS_LINK = (
    '<link rel="preconnect" href="https://fonts.googleapis.com">'
    '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>'
    '<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Serif:ital,'
    'wght@0,400;0,600;0,700;1,400&family=IBM+Plex+Sans:wght@400;500;600'
    '&family=IBM+Plex+Mono:wght@400;500&family=Caveat:wght@600&display=swap" '
    'rel="stylesheet">'
)

# Two blocks. The first is the site's base typography, which these pages have to
# be held to or they only half-match: their root stays at the browser's 16px
# while site.css runs 19px, so every rem on the page -- including the injected
# nav's 0.84rem -- comes out a fifth small, and their headings are sans where
# the site sets serif. Appended after the page's own rules so it wins at equal
# specificity, which is cheaper and far less brittle than rewriting each rule in
# place. The second block is the header and footer, scoped so none of it reaches
# the page's own content.
CHROME_CSS = """
  html { font-size: 19px; }
  body { font-size: 1rem; line-height: 1.75; }
  h1, h2 { font-family: "IBM Plex Serif", Georgia, serif; }
  h1 { font-size: 2.9rem; font-weight: 700; letter-spacing: -0.022em; line-height: 1.16; }
  h2 { font-size: 1.8rem; font-weight: 700; letter-spacing: -0.015em; line-height: 1.35; }
  h3 { font-family: "IBM Plex Sans", system-ui, sans-serif; font-size: 1rem;
    font-weight: 600; color: var(--ink); }
  .wrap { max-width: 44rem; padding: 0 1.5rem; }
  /* Base prose the site owns. Component styles the pages define for themselves
     -- their figures, their table.cmp -- are deliberately left alone; only what
     site.css sets for every page is held in common. The link treatment is the
     visible one: the site underlines with an inset shadow rather than
     text-decoration, and these pages have 23 and 40 links respectively. The
     literal #e5dbff is site.css's --accent-soft, spelled out because the pages
     use their own --accent-soft at a much lower alpha for figure fills. */
  a { text-decoration: none; box-shadow: inset 0 -0.11em 0 #e5dbff; }
  a:hover { background: #e5dbff; }
  strong { font-weight: 700; }
  ul, ol { padding-left: 1.2rem; margin: 20px 0; }
  li { margin: 9px 0; margin-bottom: 9px; }
  figcaption { color: var(--ink-3); font-size: 0.83rem; line-height: 1.6; }
  header.site nav.top a, header.site .brand, footer.site a { box-shadow: none; }
  header.site nav.top a:hover, footer.site a:hover { background: none; }
  /* The contents rail, same shape as _layout.html's. These pages carried a
     boxed list sitting inline above the prose; the rest of the site puts it in
     a sticky column on the left, and two presentations of the same thing is
     what made the page read as a different site. Own class names rather than
     site.css's .wrap, because on these pages .wrap is the 44rem prose column
     and reusing it would collide. */
  .railwrap { max-width: 1220px; margin: 0 auto; padding: 0 28px;
    display: grid; grid-template-columns: 220px minmax(0, 1fr); gap: 54px; }
  .railmain { grid-column: 2; min-width: 0; }
  aside.toc { grid-column: 1; position: sticky; top: 0; align-self: start;
    max-height: 100vh; overflow-y: auto; padding: 52px 0;
    font-family: "IBM Plex Sans", system-ui, sans-serif; font-size: 0.8rem;
    border: 0; background: none; border-radius: 0; margin: 0; }
  aside.toc .lbl { text-transform: uppercase; letter-spacing: 0.09em;
    font-size: 0.66rem; color: var(--ink-3); margin-bottom: 12px; }
  aside.toc ol { list-style: none; margin: 0; padding: 0;
    border-left: 2px solid var(--hairline); }
  aside.toc li { margin: 9px 0; }
  aside.toc a { display: block; padding: 5px 0 5px 14px; margin-left: -2px;
    border-left: 2px solid transparent; color: var(--ink-2); box-shadow: none; }
  aside.toc a:hover { color: var(--accent); border-left-color: #845ef7;
    background: none; }
  aside.toc a.on { color: var(--ink); font-weight: 600;
    border-left-color: var(--accent); }
  @media (max-width: 62rem) {
    .railwrap { grid-template-columns: minmax(0, 1fr); gap: 0; }
    aside.toc { display: none; }
    .railmain { grid-column: 1; }
  }
  header.site { border-bottom: 1px solid var(--hairline); background: var(--surface);
    position: sticky; top: 0; z-index: 20; }
  header.site .bar { max-width: 1220px; margin: 0 auto; padding: 14px 28px;
    display: flex; flex-wrap: wrap; align-items: baseline; gap: 0.4rem 1.3rem;
    font-family: "IBM Plex Sans", system-ui, sans-serif; }
  header.site .brand { font-weight: 600; font-size: 1.05rem; color: var(--ink);
    letter-spacing: -0.01em; text-decoration: none; }
  header.site .brand span { color: var(--accent); }
  header.site nav.top { display: flex; flex-wrap: wrap; gap: 0.2rem 1.25rem;
    font-size: 0.84rem; margin-left: auto; border: 0; padding: 0; background: none;
    margin-top: 0; border-radius: 0; }
  header.site nav.top a { color: var(--ink-2); text-decoration: none; }
  header.site nav.top a:hover { color: var(--accent); }
  header.site nav.top a[aria-current="page"] { color: var(--accent); font-weight: 600; }
  footer.site { border-top: 1px solid var(--hairline); color: var(--ink-3);
    font-family: "IBM Plex Sans", system-ui, sans-serif; font-size: 0.82rem;
    padding: 1.6rem 0 3rem; margin-top: 4rem; }
  footer.site a { color: var(--ink-3); text-decoration: underline; }
"""


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


# The layout's scroll-spy, repeated here because these pages do not go through
# it. It watches h2[id], which is what both pages hang their sections on.
SPY_JS = """<script>
(function () {
  var links = [].slice.call(document.querySelectorAll('aside.toc a[data-s]'));
  if (!links.length || !window.IntersectionObserver) return;
  var io = new IntersectionObserver(function (es) {
    es.forEach(function (e) {
      if (!e.isIntersecting) return;
      links.forEach(function (a) { a.classList.toggle('on', a.dataset.s === e.target.id); });
    });
  }, { rootMargin: '-15% 0px -75% 0px' });
  document.querySelectorAll('h2[id]').forEach(function (h) { io.observe(h); });
})();
</script>"""


# The masthead rule under a page title. It was on the two hand-written
# explainers and nowhere else, which is the sort of inconsistency a reader
# notices without being able to name. Emitted after the first h1 so every page
# gets one, markdown pages included.
SMPTE = (
    '<div class="smpte" aria-hidden="true">'
    '<span style="background:#b8b8a8"></span><span style="background:#b8b83e"></span>'
    '<span style="background:#3eb8b8"></span><span style="background:#3eb83e"></span>'
    '<span style="background:#b83eb8"></span><span style="background:#b83e3e"></span>'
    '<span style="background:#3e3eb8"></span></div>'
)


def strip_dark_mode(text):
    """Drop the page's dark theme.

    These pages shipped a full dark mode and site.css has none, so on a browser
    set to dark the nav took you from a light site to a black page -- the exact
    mismatch this whole transform exists to fix. Losing it is the right trade
    only while the site is light-only; if site.css ever grows a dark theme, this
    should come out and the two should share one.
    """
    for opener in ('@media (prefers-color-scheme: dark) {', ':root[data-theme="dark"] {'):
        while True:
            i = text.find(opener)
            if i == -1:
                break
            depth, j = 0, i + len(opener) - 1
            while j < len(text):
                if text[j] == '{':
                    depth += 1
                elif text[j] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            text = text[:i] + text[j + 1:]
    return text


def adopt(text, target):
    """Restyle a standalone docs/*.html page onto the site's design system.

    Text substitution rather than a CSS rewrite, on purpose: these pages have
    their own layout, their own figures and their own dark mode, and merging
    two stylesheets would fight over `.wrap` and every heading rule. Retuning
    the tokens and the type stacks gets the page looking like the site while
    leaving everything that makes it work alone.
    """
    text = strip_dark_mode(text)
    for a, b in FONT_MAP:
        text = text.replace(a, b)
    for a, b in TOKEN_MAP:
        text = text.replace(a, b)

    text = text.replace("</head>", FONTS_LINK + "</head>", 1)
    text = text.replace("</style>", CHROME_CSS + "</style>", 1)

    # Lift the page's boxed contents list out of the prose and into the rail the
    # rest of the site uses. The list itself is reused as-is -- it is hand-built
    # and ordered the way the author wanted, which a heading scrape would not
    # reproduce -- only its container and label change. data-s is what the
    # scroll-spy below matches against the section headings.
    aside = ""
    m = re.search(r'<nav class="toc".*?</nav>', text, re.S)
    if m:
        block = m.group(0)
        text = text.replace(block, "", 1)
        items = re.sub(r'<p class="eyebrow">[^<]*</p>',
                       '<div class="lbl">On this page</div>', block)
        items = re.sub(r'<nav class="toc"[^>]*>', "", items)
        items = items.replace("</nav>", "")
        items = re.sub(r'<a href="#([^"]+)"', r'<a data-s="\1" href="#\1"', items)
        aside = f'<aside class="toc">{items}</aside>'

    header = (
        '<header class="site"><div class="bar">'
        '<a class="brand" href="index.html">yah<span>264</span></a>'
        f'<nav class="top">{render_nav(target)}</nav>'
        "</div></header>"
    )
    footer = (
        '<footer class="site"><div class="wrap"><p>yah264 is BSD-2-Clause. '
        "Source on "
        '<a href="https://github.com/terranvigil/yah264">GitHub</a>.</p>'
        "</div></footer>"
    )
    # Everything between the two injected pieces becomes the grid's second
    # column. It has to be wrapped in one element: a grid lays out its direct
    # children, and the page body is a long sequence of siblings.
    text = text.replace(
        "<body>",
        f'<body>{header}<div class="railwrap">{aside}<main class="railmain">',
        1,
    )
    text = text.replace("</body>", f"</main></div>{footer}{SPY_JS}</body>", 1)
    return text


def build():
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)

    layout = (SRC / "_layout.html").read_text()
    md = markdown.Markdown(extensions=["extra", "toc", "sane_lists", "smarty"])

    shutil.copytree(SRC / "assets", OUT / "assets")

    for name, path in ADOPTED.items():
        if path.exists():
            (OUT / name).write_text(adopt(path.read_text(), name))
        else:
            print(f"  warning: {path} missing, skipping {name}")

    pages = 0
    for src in sorted(SRC.glob("*.md")):
        if src.name.startswith("_") or src.name == "README.md":
            continue
        meta, body = parse_front_matter(src.read_text())
        md.reset()
        content = md.convert(body)
        content = content.replace("</h1>", "</h1>" + SMPTE, 1)
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

    print(f"built {pages} pages + {len(ADOPTED)} adopted -> {OUT}")


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
