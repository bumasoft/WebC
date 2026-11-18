#!/usr/bin/env python3
"""Check that every relative link in the built site resolves.

    python3 tools/check-links.py [site-dir] [--anchors]

The documentation site is generated (website/build.py) and committed, and a page
can point at a source file that the generator did not turn into a page — a `.md`
href survives into the HTML and 404s for the reader. That happened three times in
language reference pages before this check existed, so it is a gate now.

External links (http:, https:, mailto:, tel:, data:, javascript:) are not fetched:
a check that needs the network is a check that fails on a plane. Fragments alone
(`#section`) are ignored unless --anchors is given, which also verifies that the
anchor exists as an id on the target page.

`--markdown` checks the other half of the repository: the relative links inside the
Markdown a reader sees on GitHub — the READMEs, CONTRIBUTING, SECURITY,
docs/*.md, the issue and pull-request templates, the agent skill. Those files promise
where things are ("the gates are in CONTRIBUTING.md", "new files get a header from
scripts/spdx.sh"), and until this mode existed nothing checked the promise. Fenced code
and inline code are skipped; a `.md` target is checked as a file, not an anchor, because
GitHub's heading slugs are its own.
"""

import argparse
import os
import re
import sys
from html.parser import HTMLParser
from urllib.parse import unquote, urlsplit

SKIP_DIRS = {"src", "__pycache__", "node_modules"}
EXTERNAL = ("http://", "https://", "mailto:", "tel:", "data:", "javascript:", "//")


class Refs(HTMLParser):
    """Every href/src on one page, plus the ids it defines."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.refs = []
        self.ids = set()

    def handle_starttag(self, tag, attrs):
        for name, value in attrs:
            if name in ("href", "src") and value:
                self.refs.append(value)
            elif name in ("id", "name") and value:
                self.ids.add(value)


def pages(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for name in sorted(filenames):
            if name.endswith(".html"):
                yield os.path.join(dirpath, name)


def page_for(path):
    """The file a browser would load for a URL path."""
    if os.path.isdir(path):
        return os.path.join(path, "index.html")
    if path.endswith("/"):
        return os.path.join(path, "index.html")
    return path


MD_LINK = re.compile(r"\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
MD_DEF = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?(\S+?)>?(?:\s|$)", re.M)
FENCE = re.compile(r"^\s*(```|~~~).*?^\s*\1", re.M | re.S)
TICK = re.compile(r"`[^`\n]*`")


def markdown_files(root):
    """The Markdown a GitHub reader sees: root, docs, .github, skills, website."""
    keep = {"website/src/content"}
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root)
        dirnames[:] = sorted(
            d for d in dirnames
            if d not in {".git", "build", "vendor", "node_modules", "__pycache__"}
            and (d != "src" or os.path.join(rel_dir, d).lstrip("./") in keep or
                 any(os.path.join(root, k).startswith(dirpath) for k in keep))
        )
        for name in sorted(filenames):
            if name.endswith(".md"):
                yield os.path.join(dirpath, name)


def check_markdown(root):
    """Relative links in repository Markdown, checked as files."""
    broken = []
    checked = 0
    for path in markdown_files(root):
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        # a link inside a fence is an example, not a promise; inside a code span, a name
        text = FENCE.sub("", text)
        text = TICK.sub("", text)
        refs = MD_LINK.findall(text) + MD_DEF.findall(text)
        rel = os.path.relpath(path, root)
        for ref in refs:
            if ref.startswith(EXTERNAL) or ref.startswith("/") or ref.startswith("#"):
                continue
            if ref.startswith("<") or "{{" in ref:
                continue
            checked += 1
            target = urlsplit(ref).path
            if not target:
                continue
            target = unquote(target)
            bases = [os.path.dirname(path)]
            if rel.startswith("website/src/content/"):
                # The site's own prose links to where it will *be* (`../reference/http.html`,
                # `quickstart.html`), not to where it is written, and which group a content file
                # publishes into is `build.py`'s business, not something a path can be read off:
                # `content/install.md` is `docs/install.html` and `content/reference-intro.md` is
                # `reference/index.html`. So accept a link that resolves in any of the published
                # groups — the promise is that the reader lands on a page the site built.
                bases += [os.path.join(root, "website", g)
                          for g in ("docs", "guides", "project", "reference", "examples")]
                bases.append(os.path.join(root, "website"))
            if rel.startswith("skills/webc/references/"):
                # A skill leaf is generated *from* the docs, so it quotes their links: a
                # `../guides/orm.html` is the site's path from `website/reference/`, and a
                # `../guides/models.md` is the file next to its author. Accept a leaf's link if
                # it resolves from any of those places — the point is that a reader who follows
                # it lands on the thing, not that one base was chosen.
                bases += [os.path.join(root, "website", "reference"),
                          os.path.join(root, "website", "src", "content", "guides"),
                          os.path.join(root, "website", "src", "content"),
                          os.path.join(root, "docs")]  # a quoted `../website/…` came from docs/
            if not any(os.path.exists(os.path.normpath(os.path.join(b, target))) for b in bases):
                broken.append((rel, ref, "no such file"))
    for rel, ref, why in broken:
        print(f"{rel}: {ref} — {why}", file=sys.stderr)
    if broken:
        print(f"check-links: {len(broken)} broken reference(s) in {checked} Markdown checked",
              file=sys.stderr)
        return 1
    print(f"check-links: {checked} reference(s) across the repository's Markdown, all resolve")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("root", nargs="?", default="website", help="site directory (default: website)")
    ap.add_argument("--anchors", action="store_true", help="also check #fragments against ids")
    ap.add_argument("--markdown", action="store_true",
                    help="check the repository's Markdown instead (relative links as files)")
    args = ap.parse_args()

    if args.markdown:
        return check_markdown(os.path.abspath(args.root if args.root != "website" else "."))

    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        print(f"check-links: no such directory: {root}", file=sys.stderr)
        return 2

    ids_cache = {}
    broken = []
    checked = 0

    def ids_of(target):
        if target not in ids_cache:
            parser = Refs()
            try:
                with open(target, encoding="utf-8", errors="replace") as fh:
                    parser.feed(fh.read())
            except OSError:
                parser.ids = set()
            ids_cache[target] = parser.ids
        return ids_cache[target]

    for page in pages(root):
        parser = Refs()
        with open(page, encoding="utf-8", errors="replace") as fh:
            parser.feed(fh.read())

        rel_page = os.path.relpath(page, root)
        for ref in parser.refs:
            if ref.startswith(EXTERNAL):
                continue

            parts = urlsplit(ref)
            path = unquote(parts.path)
            fragment = unquote(parts.fragment)

            if not path:
                # A bare fragment: an in-page anchor.
                if args.anchors and fragment and fragment not in parser.ids:
                    broken.append((rel_page, ref, "no such id on this page"))
                continue

            checked += 1
            target = page_for(os.path.normpath(os.path.join(os.path.dirname(page), path)))
            if not os.path.exists(target):
                hint = ""
                if path.endswith(".md"):
                    hint = " (a Markdown source leaked into the HTML — link the built page)"
                broken.append((rel_page, ref, "no such file" + hint))
                continue

            if args.anchors and fragment and target.endswith(".html"):
                if fragment not in ids_of(target):
                    broken.append((rel_page, ref, "no such id on the target page"))

    for rel_page, ref, why in broken:
        print(f"{rel_page}: {ref} — {why}", file=sys.stderr)

    if broken:
        print(f"check-links: {len(broken)} broken reference(s) in {checked} checked", file=sys.stderr)
        return 1

    print(f"check-links: {checked} reference(s) across the site, all resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())