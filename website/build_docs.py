#!/usr/bin/env python3
"""Render the repository's Markdown guides into the static website.

Run from anywhere:

    python3 website/build_docs.py

Python-Markdown and Pygments are maintainer-only tools, like Pillow for the
screenshot derivatives. The generated HTML is not committed: the Pages workflow
rebuilds it before deploying, and this script produces the same files for a
local preview.
"""

from __future__ import annotations

import html
import re
import sys
from pathlib import Path

try:
    import markdown
except ImportError:  # pragma: no cover - maintainer tooling
    sys.exit("Missing dependency: python3 -m pip install markdown pygments")

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
OUT = ROOT / "website" / "docs"
REPO_BLOB = "https://github.com/stevenjwheeler/Trophy/blob/main"
SITE = "https://gettrophy.app"

# Order matters: it drives the sidebar and the previous/next pager.
PAGES = (
    {
        "source": "docs/getting-started.md",
        "slug": "getting-started",
        "title": "Getting started",
        "summary": "Install Trophy, check what your system has to provide, and get a first capture.",
    },
    {
        "source": "docs/user-guide.md",
        "slug": "user-guide",
        "title": "User guide",
        "summary": "Recordings, clips, audio, bookmarks, exports, storage and appearance.",
    },
    {
        "source": "docs/development.md",
        "slug": "development",
        "title": "Building and contributing",
        "summary": "Dependencies, building, the test suites, packaging and diagnostics.",
        "includes": ({"source": "packaging/README.md", "heading": "Packaging and diagnostics"},),
    },
)

# Guides that are folded into another page rather than published separately.
ALIASES = {
    include["source"]: page["slug"]
    for page in PAGES
    for include in page.get("includes", ())
}

MARKDOWN_EXTENSIONS = ("extra", "sane_lists", "toc", "codehilite")
MARKDOWN_CONFIG = {
    "toc": {
        "permalink": True,
        "permalink_class": "doc-anchor",
        "permalink_title": "Link to this section",
    },
    "codehilite": {"guess_lang": False, "css_class": "highlight"},
}

SITE_NAV = (
    ("Features", "../#features"),
    ("Shortcuts", "../#shortcuts"),
    ("Themes", "../#themes"),
    ("Compatibility", "../#desktop"),
    ("Docs", "./"),
)

PAGE_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} · Trophy documentation</title>
<meta name="description" content="{summary_attr}">
<link rel="canonical" href="{site}/docs/{slug}.html">
<meta property="og:title" content="{title} · Trophy documentation">
<meta property="og:description" content="{summary_attr}">
<meta property="og:type" content="article">
<link rel="stylesheet" href="../styles.css">
</head>
<body class="docs-page">
<a class="skip-link" href="#main">Skip to content</a>
<div class="beta-banner">
  <p><strong>Trophy is now in beta!</strong> Expect frequent updates as we fix bugs and improve compatibility.</p>
  <p>Found an issue? <a href="https://github.com/stevenjwheeler/Trophy/issues">Please report it on GitHub</a> and help make Trophy better.</p>
</div>
<header class="site-header wrap">
  <a class="brand" href="../" aria-label="Trophy home"><img src="../assets/trophy.svg" width="34" height="34" alt="">Trophy<span class="brand-tag">GAME RECORDING AND CLIPPING FOR LINUX</span></a>
  <nav aria-label="Main navigation">
    <a href="../#features">Features</a>
    <a href="../#shortcuts">Shortcuts</a>
    <a href="../#themes">Themes</a>
    <a href="../#desktop">Compatibility</a>
    <a href="./" aria-current="true">Docs</a>
    <a class="nav-source" href="https://github.com/stevenjwheeler/Trophy">GitHub <span aria-hidden="true">↗</span></a>
  </nav>
</header>

<main id="main" class="wrap docs-layout">
  <aside class="docs-aside">
    <details class="docs-nav-mobile">
      <summary>Documentation menu</summary>
      {nav_current}
    </details>
    <div class="docs-nav-desktop">
      {nav_current}
    </div>
  </aside>

  <article class="doc-body">
    <header class="doc-header">
      <p class="eyebrow">DOCUMENTATION</p>
      <h1>{title}</h1>
      <p class="doc-summary">{summary}</p>
      <p class="doc-meta"><a href="{blob}/{source}">Edit this page on GitHub <span aria-hidden="true">↗</span></a></p>
    </header>
    {content}
    {pager}
  </article>
</main>

<footer class="site-footer wrap"><div><a class="brand" href="../"><img src="../assets/trophy.svg" width="28" height="28" alt="">Trophy</a><p>Game recording and clipping for Linux</p></div><nav aria-label="Footer navigation"><a href="https://github.com/stevenjwheeler/Trophy">Source</a><a href="https://github.com/stevenjwheeler/Trophy/issues">Issues</a><a href="./">Documentation</a><a href="https://github.com/stevenjwheeler/Trophy/blob/main/LICENSE">GPL-3.0</a></nav></footer>
</body>
</html>
"""

# /docs/ has no page of its own: a static host cannot redirect, so this stub
# sends visitors to the first guide and keeps the canonical URL there.
REDIRECT_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Getting started · Trophy documentation</title>
<link rel="canonical" href="{site}/docs/{slug}.html">
<meta http-equiv="refresh" content="0; url={slug}.html">
<link rel="stylesheet" href="../styles.css">
</head>
<body class="docs-page">
<main id="main" class="wrap docs-redirect">
  <p class="eyebrow">DOCUMENTATION</p>
  <h1>{title}</h1>
  <p>Continue to <a href="{slug}.html">{title}</a>.</p>
</main>
</body>
</html>
"""



def slug_target(target: str, source: str, current_slug: str) -> str:
    """Map a Markdown link onto its published page, or onto GitHub."""
    if target.startswith(("http://", "https://", "mailto:", "#", "data:")):
        return target
    path, _, fragment = target.partition("#")
    suffix = f"#{fragment}" if fragment else ""
    if not path:
        return target
    parts: list[str] = []
    for part in (Path(source).parent / path).parts:
        if part == "..":
            if parts:
                parts.pop()
            continue
        if part != ".":
            parts.append(part)
    key = "/".join(parts)
    owner = next((p["slug"] for p in PAGES if p["source"] == key), None) or ALIASES.get(key)
    if owner is None:
        return f"{REPO_BLOB}/{key}{suffix}"
    if owner == current_slug and fragment:
        return f"#{fragment}"
    return f"{owner}.html{suffix}"


def rewrite_links(rendered: str, source: str, current_slug: str) -> str:
    def replace(match: re.Match[str]) -> str:
        prefix, target, suffix = match.groups()
        return f"{prefix}{slug_target(target, source, current_slug)}{suffix}"

    rendered = re.sub(r'(<a\b[^>]*?\bhref=")([^"]+)(")', replace, rendered)
    rendered = re.sub(r'(<img\b[^>]*?\bsrc=")([^"]+)(")', replace, rendered)
    return rendered.replace("<table>", '<div class="doc-table"><table>').replace("</table>", "</table></div>")


def flatten_tokens(tokens: list[dict]):
    """Python-Markdown nests h3s under their h2, so walk the tree."""
    for token in tokens:
        if token["level"] > 3:
            continue
        yield token
        yield from flatten_tokens(token.get("children", []))


def nav_section(tokens: list[dict], slug: str) -> str:
    entries = []
    for token in flatten_tokens(tokens):
        cls = "doc-nav-sub" if token["level"] == 3 else "doc-nav-h2"
        entries.append(f'<li class="{cls}"><a href="{slug}.html#{token["id"]}">{token["name"]}</a></li>')
    if not entries:
        return ""
    return (
        '<p class="docs-nav-label">On this page</p>'
        f'<ul class="docs-nav-list">{"".join(entries)}</ul>'
    )


def nav_html(current: dict, toc_by_slug: dict[str, list[dict]]) -> str:
    links = []
    for page in PAGES:
        if page["slug"] == current["slug"]:
            links.append(f'<li><a href="{page["slug"]}.html" aria-current="page">{page["title"]}</a></li>')
        else:
            links.append(f'<li><a href="{page["slug"]}.html">{page["title"]}</a></li>')
    sections = nav_section(toc_by_slug[current["slug"]], current["slug"])
    return (
        '<p class="docs-nav-label">Guides</p>'
        f'<ul class="docs-nav-list">{"".join(links)}</ul>'
        f"{sections}"
    )


def pager_html(index: int) -> str:
    parts = []
    if index > 0:
        previous = PAGES[index - 1]
        parts.append(
            f'<a class="doc-pager-link doc-pager-prev" href="{previous["slug"]}.html">'
            f'<span class="doc-pager-label">Previous</span>{previous["title"]}</a>'
        )
    if index < len(PAGES) - 1:
        following = PAGES[index + 1]
        parts.append(
            f'<a class="doc-pager-link doc-pager-next" href="{following["slug"]}.html">'
            f'<span class="doc-pager-label">Next</span>{following["title"]}</a>'
        )
    if not parts:
        return ""
    return f'<nav class="doc-pager" aria-label="Documentation pages">{"".join(parts)}</nav>'


def strip_front_matter(text: str) -> str:
    body = text.lstrip()
    # The title and the GitHub breadcrumb belong to the page header instead.
    body = re.sub(r"^# .*\n", "", body, count=1).lstrip()
    return re.sub(r"^\[Back to README\][^\n]*\n", "", body, count=1).lstrip()


def demote_headings(text: str) -> str:
    """Push headings down one level, skipping fenced code blocks."""
    out, fenced = [], False
    for line in text.splitlines(keepends=True):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            out.append(line)
            continue
        if not fenced:
            match = re.match(r"^(#{2,5}) ", line)
            if match:
                line = f"#{line}"
        out.append(line)
    return "".join(out)


def render_markdown(body: str, source: str, slug: str) -> tuple[str, list[dict]]:
    converter = markdown.Markdown(extensions=list(MARKDOWN_EXTENSIONS), extension_configs=MARKDOWN_CONFIG)
    return rewrite_links(converter.convert(body), source, slug), converter.toc_tokens


def render_page(page: dict, source_text: str) -> tuple[str, list[dict]]:
    content, tokens = render_markdown(strip_front_matter(source_text), page["source"], page["slug"])
    for include in page.get("includes", ()):
        included = (ROOT / include["source"]).read_text(encoding="utf-8")
        body = demote_headings(strip_front_matter(included))
        body = f"## {include['heading']}\n\n{body}"
        html, included_tokens = render_markdown(body, include["source"], page["slug"])
        content = f"{content}\n{html}"
        tokens = tokens + included_tokens
    return content, tokens


def main() -> int:
    if not DOCS.is_dir():
        raise SystemExit(f"Missing documentation directory: {DOCS}")

    rendered = {}
    for page in PAGES:
        source = ROOT / page["source"]
        if not source.is_file():
            raise SystemExit(f"Missing documentation source: {source}")
        rendered[page["slug"]] = render_page(page, source.read_text(encoding="utf-8"))

    toc_by_slug = {slug: tokens for slug, (_, tokens) in rendered.items()}

    OUT.mkdir(parents=True, exist_ok=True)
    for stale in OUT.glob("*.html"):
        stale.unlink()

    for index, page in enumerate(PAGES):
        content, _ = rendered[page["slug"]]
        document = PAGE_TEMPLATE.format(
            title=html.escape(page["title"]),
            summary=html.escape(page["summary"]),
            summary_attr=html.escape(page["summary"], quote=True),
            slug=page["slug"],
            source=page["source"],
            site=SITE,
            blob=REPO_BLOB,
            nav_current=nav_html(page, toc_by_slug),
            content=content,
            pager=pager_html(index),
        )
        (OUT / f"{page['slug']}.html").write_text(document, encoding="utf-8")

    first = PAGES[0]
    redirect = REDIRECT_TEMPLATE.format(
        site=SITE, slug=first["slug"], title=html.escape(first["title"])
    )
    (OUT / "index.html").write_text(redirect, encoding="utf-8")

    for page in PAGES:
        target = OUT / f"{page['slug']}.html"
        print(f"{target.relative_to(ROOT)}: {target.stat().st_size:,} bytes")
    print(f"{(OUT / 'index.html').relative_to(ROOT)}: {(OUT / 'index.html').stat().st_size:,} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
