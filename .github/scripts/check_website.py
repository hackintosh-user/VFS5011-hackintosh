#!/usr/bin/env python3
"""Checks the GitHub Pages site in docs/ for missing or broken files.

Looks at: required files, every local link/src/href, in-page and
cross-page #anchors, url() references in the CSS, duplicate ids, the
basics every page needs (title, lang, viewport, nav to all pages), and
that the nav is identical on every page. External (http/https) links are
NOT fetched, since that makes CI flaky.
"""
import re
import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlparse, unquote

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else "docs")
PAGES = ["index.html", "docs.html", "guide.html", "security.html", "contribute.html"]
ASSETS = ["assets/style.css", "assets/site.js", "assets/fingerprint.png", "assets/favicon.png"]
errors = []


def err(msg):
    errors.append(msg)
    print(f"::error::{msg}")


class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids, self.refs, self.nav = [], [], []
        self.title = self.lang = self.viewport = None
        self.in_title = self.in_nav = False

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "html":
            self.lang = a.get("lang")
        if tag == "title":
            self.in_title = True
        if tag == "nav":
            self.in_nav = True
        if tag == "meta" and a.get("name") == "viewport":
            self.viewport = a.get("content")
        if "id" in a:
            self.ids.append(a["id"])
        for k in ("href", "src"):
            if k in a:
                self.refs.append(a[k])
                if self.in_nav and tag == "a":
                    self.nav.append(a[k])

    def handle_endtag(self, tag):
        if tag == "title":
            self.in_title = False
        if tag == "nav":
            self.in_nav = False

    def handle_data(self, data):
        if self.in_title:
            self.title = (self.title or "") + data


# 1. Required files
for f in PAGES + ASSETS:
    p = ROOT / f
    if not p.is_file():
        err(f"Missing required file: {p}")
    elif p.stat().st_size == 0:
        err(f"Empty file: {p}")

parsed = {}
for f in PAGES:
    p = ROOT / f
    if not p.is_file():
        continue
    pg = Page()
    pg.feed(p.read_text(encoding="utf-8"))
    parsed[f] = pg

    # 2. Page basics
    if not (pg.title and pg.title.strip()):
        err(f"{f}: missing <title>")
    if not pg.lang:
        err(f"{f}: <html> has no lang attribute")
    if not pg.viewport:
        err(f"{f}: missing viewport meta tag")
    dupes = {i for i in pg.ids if pg.ids.count(i) > 1}
    if dupes:
        err(f"{f}: duplicate ids: {', '.join(sorted(dupes))}")
    for page in PAGES:
        if page not in pg.nav:
            err(f"{f}: nav is missing a link to {page}")

# 3. Nav identical across pages
navs = {f: pg.nav for f, pg in parsed.items()}
if len({tuple(n) for n in navs.values()}) > 1:
    err("Nav links differ between pages: " + "; ".join(f"{f}={n}" for f, n in navs.items()))

# 4. Links, assets and anchors
for f, pg in parsed.items():
    for ref in pg.refs:
        u = urlparse(ref)
        if u.scheme in ("http", "https", "mailto", "data") or ref.startswith("//"):
            continue
        target_name = f if not u.path else unquote(u.path)
        target = (ROOT / f).parent / target_name
        if not target.exists():
            err(f"{f}: broken link '{ref}' ({target} does not exist)")
            continue
        if u.fragment and target.suffix == ".html":
            tp = parsed.get(target.name)
            if tp is None:
                tp = Page()
                tp.feed(target.read_text(encoding="utf-8"))
            if u.fragment not in tp.ids:
                err(f"{f}: anchor '#{u.fragment}' not found in {target.name} (link '{ref}')")

# 5. url() references in CSS (relative to the CSS file)
css = ROOT / "assets/style.css"
if css.is_file():
    for m in re.finditer(r"url\(\s*['\"]?([^'\")]+)['\"]?\s*\)", css.read_text(encoding="utf-8")):
        ref = m.group(1)
        if ref.startswith(("http:", "https:", "data:")):
            continue
        if not (css.parent / ref).exists():
            err(f"assets/style.css: url({ref}) points to a file that does not exist")

print(f"Checked {len(parsed)} pages, {len(errors)} problem(s) found.")
sys.exit(1 if errors else 0)
