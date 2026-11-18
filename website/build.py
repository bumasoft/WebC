#!/usr/bin/env python3
"""Build the WebC documentation site into `website/`.

Sources of truth, by design:

  docs/reference.md   -> the seventeen Reference pages, one per `## N.` section
  examples/<name>/    -> one page per example, with its real files
  src/content/*.md    -> the hand-written start pages and guides
  src/landing.html    -> the landing page
  src/assets/*        -> css, js, highlighter, favicon

  CONTRIBUTING.md     -> `website/project/contributing.html`, and the same for CHANGELOG.md,
  SECURITY.md,        published as they are rather than summarised — a second copy of a document
  docs/decisions.md   is a copy that goes stale

Output is committed: `website/index.html`, `website/docs/…`, `website/guides/…`,
`website/reference/…`, `website/examples/…`, `website/project/…`, `website/assets/…`.
That is what GitHub Pages serves (`Settings -> Pages -> main /website`) and what makes the
site work from `file://` with no server and no network.

    python3 website/build.py            # build
    python3 website/build.py --serve    # build, then serve on :8080 for a look
"""

from __future__ import annotations

import argparse
import html
import json
import os
import re
import shutil
import string
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "src"))
import mdhtml  # noqa: E402

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent
SRC = ROOT / "src"
CONTENT = SRC / "content"
GITHUB = "https://github.com/bumasoft/WebC"
# The site's footer says the version it documents, so it reads the same file the CLI does.
VERSION = (REPO / "VERSION").read_text().strip() if (REPO / "VERSION").exists() else "0.0.0-dev"
SITE_NAME = "WebC"
# Where the site is published. `og:image` and friends want an absolute URL, and this is the one
# address that is not a guess: GitHub Pages serves the uploaded `website/` under the repository's
# name (see .github/workflows/pages.yml and docs/decisions.md D8 — branch publishing cannot be
# pointed at a directory called `website`).
SITE = "https://bumasoft.github.io/WebC"


def page_title(title: str) -> str:
    """The <title> and the share-card title of a page: "Thing · WebC".

    Every page says what it is and then whose it is. The home page *is* the site, so it says
    "WebC — web apps in C" once instead of "... · WebC" twice — which is what the tab, the search
    result and the share card all showed before this."""
    return title if SITE_NAME in title else f"{title} · {SITE_NAME}"


# Is this site the documentation *for the release*, or for the tree ahead of it?
#
# Git cannot answer this. The site is committed before it is tagged (the tag is what the release
# workflow acts on), so at the moment the release build runs HEAD is not yet the tag — and a plain
# `make site` an hour later would flip all 57 pages back to "development tree" and fail the docs
# job. That happened on the v0.1.0 push, in exactly that order.
#
# So the committed tree carries the answer: `website/.release` names the version this site
# documents, `make site ARGS=--release` writes it, and every later plain `make site` reproduces
# the same bytes. Delete it (and bump VERSION) when the next release cycle starts.
RELEASE = False
RELEASE_MARKER = ROOT / ".release"


def version_note() -> str:
    """What this build *is*: the documentation for a release, or for the tree ahead of one.

    A site that prints `v0.1.0` on a page generated from `main` is telling a small lie — the page
    may describe something the release does not have.
    """
    if RELEASE:
        return f"Documentation for v{VERSION}"
    try:
        out = subprocess.run(["git", "describe", "--exact-match", "--tags", "HEAD"],
                             cwd=REPO, capture_output=True, text=True)
        tag = out.stdout.strip() if out.returncode == 0 else ""
    except OSError:
        tag = ""
    if tag == f"v{VERSION}":
        return f"Documentation for {tag}"
    return f"Documentation for the development tree, ahead of v{VERSION}"


def api_name_count() -> str:
    """The api.txt ratchet's own number, for the front page's headline statistic.

    It was the literal "1 641" while `make dx` printed 1840 — on a site whose pitch is "every
    surface is counted", the one number nobody recounted. It is parsed from the file the gate
    writes now, so the page cannot disagree with the ratchet again.
    """
    head = (REPO / "api.txt").read_text(encoding="utf-8").splitlines()[0]
    m = re.match(r"# webc api — (\d+) names", head)
    if not m:
        raise SystemExit(f"build.py: api.txt's first line is not '# webc api — N names': {head!r}")
    return f"{int(m.group(1)):,}".replace(",", "\u2009")  # a thin space: 1 841


# --------------------------------------------------------------------------- pages

class Page:
    def __init__(self, group, title, path, kind, source, description="", nav=True, short=None):
        self.group = group
        self.title = title
        self.short = short or title
        self.crumb = title                       # what the breadcrumb says (the sidebar uses `short`)
        self.path = path                       # relative to the site root, e.g. "docs/install.html"
        self.kind = kind                       # md | ref | examples | example | raw
        self.source = source
        self.description = description
        self.nav = nav
        # filled while building
        self.html = ""
        self.headings = []
        self.toc = []
        self.weight = 0

    @property
    def id(self):
        return self.path.replace("/", "-").replace(".html", "")


REFERENCE_SECTIONS = [
    ("house-rules", "1. House rules"),
    ("core", "2. Core: strings, memory, errors"),
    ("collections", "3. Collections"),
    ("models", "4. Models"),
    ("json-and-forms", "5. JSON and forms"),
    ("concurrency", "6. Concurrency"),
    ("http", "7. HTTP"),
    ("middleware", "8. Middleware"),
    ("database", "9. Database"),
    ("auth", "10. Auth"),
    ("signals", "11. Frontend: signals"),
    ("dom", "12. Frontend: DOM and SSR"),
    ("bridge", "13. The browser bridge"),
    ("cli", "14. The CLI"),
    ("testing", "14b. Testing"),
    ("ops", "15. Ops"),
    ("limits", "16. Limits and deviations"),
]

EXAMPLES = [
    ("hello", "Hello", "A finished first page: a greeting form, a JSON endpoint, and one shared model."),
    ("todo", "Todo", "A list that works with no JavaScript at all, then enhances itself."),
    ("accounts", "Accounts", "Password auth, sessions, a middleware that knows who you are."),
    ("web", "Web", "A client-only page: the same views compiled to wasm."),
    ("js", "JS interop", "The seam to JavaScript: `js()`, promises, and calls back the other way."),
    ("stdlib", "Stdlib", "The opt-in shelf: every module on one page."),
    ("board", "Board", "A kanban with live rooms, drag and drop, and a real database."),
    ("minimal", "Minimal", "The bundle floor: what a page costs when it asks for nothing."),
]


def build_pages():
    pages: list[Page] = []

    def add(group, title, path, kind, source, description="", **kw):
        pages.append(Page(group, title, path, kind, source, description, **kw))

    # ---- start
    add("Start", "What is WebC?", "docs/index.html", "md", "intro.md",
        "A framework for web apps in C: one language from the route to the DOM.")
    add("Start", "Install", "docs/install.html", "md", "install.md",
        "A compiler, the repository, and the one command that builds an app.")
    add("Start", "Quickstart", "docs/quickstart.html", "md", "quickstart.md",
        "Scaffold an app, run it, change it, test it — in about five minutes.")
    add("Start", "Project layout", "docs/layout.html", "md", "layout.md",
        "What `webc new` writes, and what each file is for.")
    add("Start", "Mental model", "docs/mental-model.html", "md", "mental-model.md",
        "Two halves, one set of views; arenas instead of a collector; a render that follows an action.")

    # ---- guides
    guides = [
        ("http", "Routes, requests, responses", "Handlers are typed, the router is a table, and a response is a value."),
        ("models", "Models and migrations", "A table, a struct and a wire shape from one declaration."),
        ("orm", "The ORM: filters, relations, writes",
         "Typed filters, relations read with no query, pages and trees — and what each costs."),
        ("mysql", "MySQL and MariaDB",
         "The third driver: the wire protocol, auth plugins, TLS, and the `.mysql.sql` twin."),
        ("forms", "Forms, with and without JS", "The form works before the module loads, and gets better after."),
        ("auth", "Auth: passwords and sessions",
         "PBKDF2, a signed cookie, one guard call — and no session table."),
        ("signals", "Signals and the DOM", "`state`, memos, effects, and a view that is C."),
        ("components", "Parts and components", "Typed view functions, and the call site that owns its own state."),
        ("stdlib", "The standard library", "Twenty-one opt-in modules, and the cost of asking for one."),
        ("data", "Redis and the query cache", "Caching result sets by generation, and the room around them."),
        ("live", "Real-time: SSE and live rooms", "Whole documents, coalesced, with a dead tab costing nothing."),
        ("testing", "Testing", "`webc test`, requests in-process, and assertions about structure."),
        ("editor", "The editor: LSP and VS Code",
         "Diagnostics, completion, hover and quick fixes — from one binary, in any editor."),
        ("agents", "Agentic coding",
         "The skill, the MCP server, the AGENTS.md every scaffold writes, and what an agent "
         "gets wrong here — with the gate that stops it."),
        ("api", "The API boundary",
         "`webc openapi` and `webc client --ts`: the boundary, generated from what the "
         "sources already declare — and the beachhead for the frontend that is not moving today."),
        ("rosetta", "Coming from React, Solid and Next",
         "The dictionary: the name you know, the WebC spelling, and the difference "
         "underneath — useState to state_own, Server Components to the shared file set."),
        ("deploy", "Deploying", "One binary, one directory, one manifest; and what the size gate says."),
    ]
    for slug, title, desc in guides:
        add("Guides", title, f"guides/{slug}.html", "md", f"guides/{slug}.md", desc)

    # ---- reference
    add("Reference", "Reference: how to read it", "reference/index.html", "md", "reference-intro.md",
        "One page per section of `docs/reference.md` — the whole framework, in order.")
    for anchor, title in REFERENCE_SECTIONS:
        num = title.split(".")[0]
        # the slug is the section's *name*, not its number: `reference/signals.html` is a URL
        # someone can type, and renumbering the reference must not break a link.
        named = title.split(".", 1)[1] if "." in title else title
        slug = slugify(named) or f"section-{num}"
        page = Page("Reference", title, f"reference/{slug}.html", "ref", anchor,
                    f"Reference §{num}.", nav=True, short=title)
        page.crumb = named.strip()
        pages.append(page)

    # ---- project: the documents a reader wants *after* they like the framework
    add("Project", "Contributing", "project/contributing.html", "repo", "CONTRIBUTING.md",
        "The gates, the house rules, and the sign-off every commit carries.")
    add("Project", "Changelog", "project/changelog.html", "repo", "CHANGELOG.md",
        "What is in a release, and what the first one contains.")
    add("Project", "Security", "project/security.html", "repo", "SECURITY.md",
        "What is in scope, and the private channel for a vulnerability.")
    add("Project", "Decisions", "project/decisions.html", "repo", "docs/decisions.md",
        "Why the licence, the name, the compiler floor and the version file are what they are.")
    add("Project", "Roadmap", "project/roadmap.html", "md", "project/roadmap.md",
        "Where this is going, what it will never do, and what a version number means here.")
    add("Project", "About", "project/about.html", "md", "project/about.md",
        "Who writes it, under what licence, and what the gate is for.")

    # ---- examples
    add("Examples", "All examples", "examples/index.html", "examples", "",
        "Eight apps in the repository, each one runnable, each one different.")
    for slug, title, desc in EXAMPLES:
        add("Examples", title, f"examples/{slug}.html", "example", slug, desc)

    for i, p in enumerate(pages):
        p.weight = i
    return pages


# --------------------------------------------------------------------------- cross-links

class Linker:
    """`#anchor` in a reference page may name a section on another page: resolve it.

    It also rewrites a link written against a *source file* (`../guides/models.md`,
    `../website/src/content/guides/auth.md`) into the built page it became. Those
    links are correct on GitHub and dead on the site, which is why the check in
    tools/check-links.py exists — three of them survived to a release candidate.
    """

    def __init__(self):
        self.anchors: dict[str, str] = {}
        # repo-relative source path -> built page path
        self.sources: dict[str, str] = {}

    def add(self, anchor, path):
        self.anchors[anchor] = path

    def add_source(self, source_path, page_path):
        self.sources[str(Path(source_path))] = page_path

    def resolver(self, page_path, source_path=None):
        here = Path(page_path).name
        parent = str(Path(page_path).parent)

        def link(href):
            if href.startswith("#"):
                target = href[1:]
                path = self.anchors.get(target)
                if not path:
                    return href
                if Path(path).name == here and str(Path(path).parent) == parent:
                    return href
                rel = _rel(page_path, path)
                return f"{rel}#{target}"
            # `docs/install.html` and `reference/orm.html` are site paths, but only in the site's
            # own prose. In a repository document — CONTRIBUTING.md, SECURITY.md — `docs/…` is a
            # repo file, and rewriting it as a site path produced a link to a page that is not
            # published (`../docs/getting-started.md`). The shortcut is for site sources only.
            site_source = source_path is None or source_path.startswith("website/")
            if site_source and (href.startswith("docs/") or href.startswith("reference/")):
                return _rel(page_path, href)
            # A link the author wrote against the repo, not against the site.
            if source_path and not href.startswith(("/", "#")) and "://" not in href \
                    and not href.startswith(("mailto:", "tel:")):
                target, _, fragment = href.partition("#")
                if target:
                    resolved = os.path.normpath(os.path.join(str(Path(source_path).parent), target))
                    page = self.sources.get(resolved)
                    if page:
                        rel = _rel(page_path, page)
                        return f"{rel}#{fragment}" if fragment else rel
                    # A document published from the repository — CONTRIBUTING.md, CHANGELOG.md,
                    # SECURITY.md — links to its neighbours: AGENTS.md, LICENSE, api.txt, a script.
                    # The site does not publish those, and a reader who followed the link should
                    # still land on the file, so a repo file that exists becomes a GitHub link
                    # instead of a 404. `docs/README.md`-style paths resolve the same way.
                    if not resolved.startswith("..") and (REPO / resolved).is_file():
                        return f"{GITHUB}/blob/main/{resolved}"
            return href

        return link


def edit_row(url: str) -> str:
    """The footer link, or nothing at all — a page the generator composed has no source to edit."""
    if not url:
        return ""
    return ('<footer class="article__foot">'
            f'<a class="editlink" href="{url}" target="_blank" rel="noreferrer">'
            "Edit this page on GitHub</a></footer>")


def _rel(from_path: str, to_path: str) -> str:
    """A relative href from one page to another (both site-root relative)."""
    from_parts = Path(from_path).parent.parts
    to_parts = Path(to_path).parts
    common = 0
    for a, b in zip(from_parts, to_parts):
        if a != b:
            break
        common += 1
    ups = len(from_parts) - common
    return ("../" * ups) + "/".join(to_parts[common:])


# --------------------------------------------------------------------------- generators

def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def reference_sections() -> dict[str, str]:
    """Split docs/reference.md into {anchor: markdown} by its `## <a name="x"></a>` heads."""
    text = read(REPO / "docs/reference.md")
    parts: dict[str, str] = {}
    current = None
    buf: list[str] = []
    for line in text.split("\n"):
        m = re.match(r'^## <a name="([^"]+)"></a>(.*)$', line)
        if m:
            if current:
                parts[current] = "\n".join(buf).strip()
            current = m.group(1)
            buf = []
        elif re.match(r"^## ", line):
            if current:
                parts[current] = "\n".join(buf).strip()
            current = None
            buf = []
        elif current:
            buf.append(line)
    if current:
        parts[current] = "\n".join(buf).strip()
    return parts


# --------------------------------------------------------------------------- example files
#
# An example page shows the app's real files, so the rule is "the files a reader needs", not
# "whatever is on disk". Two things are on disk that are not the app:
#
#   - build output. `compile_commands.json` is written by the build for clangd and gitignored;
#     a page that published it printed the author's absolute paths and a clang command line
#     onto the site.
#   - the dialect twins of one migration (`0001_init.postgres.sql`, `.mysql.sql`). Same table
#     three times; one spelling is enough to read, and the reference documents the rest.
#
# Everything else is kept, including a file that is not committed yet: a new example shows up
# on the site while it is being written, which is what you want while writing it.

_LINE_BUDGET = 110

# A shortened snippet ends with this, in a comment the file's own language understands.
_MORE = {
    "c": ("// … ", ""), "js": ("// … ", ""), "json": ("// … ", ""),
    "css": ("/* … ", " */"), "html": ("<!-- … ", " -->"), "sql": ("-- … ", ""),
}


def git_ignored(paths: list[Path]) -> set[Path]:
    """The subset of `paths` that git ignores: build output, `data/`, editor noise.

    `make site` runs where the examples have been built, so a generator that reads the
    directory publishes `compile_commands.json` — absolute paths, clang flags. Git already
    knows what is output, so ask it; a tree without git has nothing ignored.
    """
    if not paths:
        return set()
    try:
        run = subprocess.run(
            ["git", "-C", str(REPO), "check-ignore", "--no-index", "--stdin"],
            input="".join(f"{p}\n" for p in paths),
            capture_output=True, text=True, timeout=20,
        )
    except (OSError, subprocess.SubprocessError):
        return set()
    if run.returncode not in (0, 1):  # 0 = some ignored, 1 = none, anything else = git said no
        return set()
    return {Path(line) for line in run.stdout.splitlines() if line.strip()}


def dialect_twin(p: Path) -> bool:
    """`0001_init.postgres.sql` when `0001_init.sql` is right there."""
    if not p.name.endswith((".postgres.sql", ".mysql.sql")):
        return False
    return p.with_name(p.name.split(".")[0] + ".sql").exists()


def shortened(body: str, lang: str) -> str:
    """The file, cut to a page-sized snippet at a seam a reader can read.

    A hard line count ends the snippet *inside* a function — half of `webc_dev_state_load()`,
    then "16 more lines" — and never reaches the block the example exists to teach. A shortened
    file ends where its author could have stopped: after a top-level `}`, or (in a file with no
    braces) at a blank line.
    """
    lines = body.split("\n")
    if len(lines) <= _LINE_BUDGET:
        return body
    cut = _LINE_BUDGET
    for i in range(_LINE_BUDGET - 1, _LINE_BUDGET // 2, -1):
        if lines[i].startswith("}"):
            cut = i + 1
            break
    else:
        if lang in ("css", "html", "json", "sql"):
            for i in range(_LINE_BUDGET - 1, _LINE_BUDGET // 2, -1):
                if not lines[i].strip():
                    cut = i
                    break
    pre, post = _MORE.get(lang, ("// … ", ""))
    return ("\n".join(lines[:cut])
            + f"\n\n{pre}{len(lines) - cut} more lines in the repository{post}")


# A caption under a file whose *role* the code does not show, because the page would otherwise
# read as "write this too". Per example, because the note is a claim about that file: `js`'s
# shell carries a line of page-side glue and `web`'s is the scaffold's, verbatim.
FILE_NOTES = {
    ("web", "public/index.html"):
        "Not the app's code: this is the document `webc new` writes for a browser-only "
        "project. You edit the title and the stylesheet link; the mount point and "
        "`WebC.start()` are the scaffold's and stay as they are.",
    ("minimal", "public/index.html"):
        "The whole document: a viewport, a stylesheet, a mount point and `WebC.start()`. "
        "Nothing here is app logic — the page is wasm, and this is the page it lands in.",
    ("minimal", "public/style.css"):
        "The look, in one file and one class per element. Shown here up to the card, the "
        "brand mark and the heading; the rest is the big number, the button, the dark half "
        "and the phone case.",
}


def example_page(slug: str) -> tuple[str, list[dict], str]:
    """One page per example: what it is, how to run it, and its real files."""
    meta = {s: (t, d) for s, t, d in EXAMPLES}
    title, desc = meta[slug]
    folder = REPO / "examples" / slug
    candidates = sorted(
        p for p in folder.rglob("*")
        if p.is_file()
        and p.suffix in {".c", ".h", ".json", ".sql", ".css", ".js", ".html", ".md"}
        and ".webc" not in p.parts
        and "data" not in p.parts
        and not dialect_twin(p)
    )
    ignored = git_ignored(candidates)  # one question to git, not one per file
    files = [p for p in candidates if p not in ignored]
    out: list[str] = []
    out.append(f"<p class=\"lede\">{mdhtml._Inline(lambda h: h).render(desc)}</p>")
    out.append(
        '<div class="runbar"><code>webc dev</code><span>run it from '
        f"<code>examples/{slug}</code></span>"
        f'<a class="runbar__link" href="{GITHUB}/tree/main/examples/{slug}">open on GitHub</a></div>'
    )
    # The product shot. `minimal` shares hello's page by design — it is the same counter in
    # fewer bytes — so it asks for no picture of its own and the page says what it weighs instead.
    shot = SRC / "assets" / "media" / f"examples-{slug}.png"
    if shot.exists():
        out.append(
            f'<figure class="figure"><img src="../assets/media/examples-{slug}.png" '
            f'alt="The {html.escape(title)} example running in a browser" '
            f'loading="lazy" decoding="async">'
            f"<figcaption>The real app, from <code>examples/{slug}</code> — screenshot taken from "
            f'<code>webc dev</code>.</figcaption></figure>'
        )
    shown = 0
    main_files = [p for p in files if p.name in ("webc.json", "main.c")]
    src_files = [p for p in files if "src" in p.parts]
    rest = [p for p in files if p not in main_files and p not in src_files]
    ordered = main_files + src_files + rest
    for p in ordered:
        if shown >= 6:
            break
        rel = p.relative_to(folder)
        lang = {
            ".c": "c", ".h": "c", ".json": "json", ".sql": "sql",
            ".css": "css", ".js": "js", ".html": "html", ".md": "text",
        }[p.suffix]
        body = p.read_text(encoding="utf-8")
        body = shortened(body, lang)
        out.append(f'<h2 id="file-{slugify(str(rel))}">{html.escape(str(rel))}</h2>')
        note = FILE_NOTES.get((slug, str(rel)))
        if note:
            out.append(f'<p class="muted">{mdhtml._Inline(lambda h: h).render(note)}</p>')
        out.append(mdhtml.code_block(body, lang, f"examples/{slug}/{rel}"))
        shown += 1
    if len(ordered) > shown:
        out.append(
            f'<p class="muted">…and {len(ordered) - shown} more files in '
            f'<a href="{GITHUB}/tree/main/examples/{slug}">examples/{slug}</a>.</p>'
        )
    headings = [
        {"level": 2, "id": m.group(1), "text": m.group(2)}
        for m in re.finditer(r'<h2 id="([^"]+)">(.*?)</h2>', "\n".join(out))
    ]
    return "\n".join(out), headings, f"examples/{slug}"


def examples_index() -> tuple[str, list[dict], str]:
    out = [
        '<p class="lede">Eight apps live in the repository. Each builds, each passes its own '
        "tests, and each one is small enough to read in a sitting. They are not toys: the board is "
        "the reference for live rooms and drag-and-drop, the accounts app is the reference for "
        "sessions.</p>",
        '<div class="cards cards--2">',
    ]
    for slug, title, desc in EXAMPLES:
        out.append(
            f'<a class="card card--link" href="{slug}.html">'
            f'<span class="card__kicker">examples/{slug}</span>'
            f'<h3 class="card__title">{title}</h3>'
            f'<p class="card__text">{mdhtml._Inline(lambda h: h).render(desc)}</p>'
            '<span class="card__more">Read the page →</span></a>'
        )
    out.append("</div>")
    out.append(
        "<h2 id=\"run-them\">Run them</h2>"
        "<p>Every example is a project of its own. From the repository root:</p>"
        + mdhtml.code_block("cd examples/board\nwebc dev            # build, serve, reload on save\nwebc test           # its own suites", "sh")
    )
    headings = [{"level": 2, "id": "run-them", "text": "Run them"}]
    return "\n".join(out), headings, "examples/"


def slugify(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


# --------------------------------------------------------------------------- assembly

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serve", action="store_true", help="serve the built site on :8080")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--release", action="store_true",
                    help="this build is the documentation for the tagged release")
    args = ap.parse_args()
    global RELEASE
    if args.release:
        RELEASE_MARKER.write_text(f"{VERSION}\n", encoding="utf-8")
        RELEASE = True
    elif RELEASE_MARKER.exists():
        marked = RELEASE_MARKER.read_text(encoding="utf-8").strip()
        if marked == VERSION:
            RELEASE = True
        else:
            print(f"website: .release says {marked!r} but VERSION says {VERSION!r} — "
                  "treating this as a development build (re-run with --release)", file=sys.stderr)

    pages = build_pages()
    by_path = {p.path: p for p in pages}
    linker = Linker()

    # reference pages: split the one file, record anchors for cross-page links
    sections = reference_sections()
    for p in pages:
        if p.kind == "ref":
            p.title_html = next(t for a, t in REFERENCE_SECTIONS if a == p.source)
            linker.add(p.source, p.path)
    # a link to the reference *file* means the reference's front page
    linker.add_source("docs/reference.md", "reference/index.html")
    # every hand-written page, so a link written against its Markdown source lands
    for p in pages:
        if p.kind == "md":
            linker.add_source(f"website/src/content/{p.source}", p.path)
        elif p.kind == "repo":
            linker.add_source(p.source, p.path)
    # anchors inside each reference section
    for p in pages:
        if p.kind != "ref":
            continue
        md = sections.get(p.source, "")
        for m in re.finditer(r'^###\s+(?:<a name="([^"]+)"></a>)?(.*)$', md, flags=re.M):
            if m.group(1):
                linker.add(m.group(1), p.path)

    # ---- render every page
    for p in pages:
        prefix = "../" * (len(Path(p.path).parent.parts) if str(Path(p.path).parent) != "." else 0)
        if p.kind == "md":
            raw = read(CONTENT / p.source)
            # the page's own <h1> is added below, so `##` in the source is an <h2> — shift 0
            body, heads = mdhtml.render(raw, link=linker.resolver(p.path, f"website/src/content/{p.source}"), shift=0)
            p.html, p.headings = body, heads
            p.edit = f"{GITHUB}/blob/main/website/src/content/{p.source}"
        elif p.kind == "repo":
            # A repository document, published here as it is rather than summarised: the changelog
            # and the contributing guide are read by the same person in both places, and a second
            # copy is a copy that goes stale. The page's own <h1> replaces the file's.
            raw = re.sub(r"\A#\s+[^\n]*\n+", "", read(REPO / p.source))
            body, heads = mdhtml.render(raw, link=linker.resolver(p.path, p.source), shift=0)
            p.html, p.headings = body, heads
            p.edit = f"{GITHUB}/blob/main/{p.source}"
        elif p.kind == "ref":
            raw = sections.get(p.source, "")
            body, heads = mdhtml.render(raw, link=linker.resolver(p.path, "docs/reference.md"), shift=-1)
            intro = (
                # the section's own anchor, so a `#limits` written in the reference still lands
                f'<p class="page-kicker" id="{p.source}">Reference · <code>docs/reference.md</code></p>'
            )
            p.html = intro + body
            p.headings = [h for h in heads if h["level"] == 2]
            p.edit = f"{GITHUB}/blob/main/docs/reference.md"
        elif p.kind == "examples":
            p.html, p.headings, _ = examples_index()
            p.edit = f"{GITHUB}/tree/main/examples"
        elif p.kind == "example":
            p.html, p.headings, _ = example_page(p.source)
            p.edit = f"{GITHUB}/tree/main/examples/{p.source}"
        p.toc = [h for h in p.headings if 2 <= h["level"] <= 3]

    # ---- prev / next over the whole book, in nav order
    order = [p for p in pages]
    for i, p in enumerate(order):
        p.prev = order[i - 1] if i > 0 else None
        p.next = order[i + 1] if i + 1 < len(order) else None

    shell = string.Template(read(SRC / "shell.html"))
    groups: dict[str, list[Page]] = {}
    for p in pages:
        groups.setdefault(p.group, []).append(p)

    for p in pages:
        prefix = "../" * (len(Path(p.path).parent.parts) if str(Path(p.path).parent) != "." else 0)
        content = p.html
        if p.kind in ("md", "ref", "repo", "example", "examples"):
            content = f'<h1 id="top">{html.escape(p.title)}</h1>\n' + content
        out_html = shell.substitute(
            title=page_title(html.escape(p.title)),
            description=html.escape(p.description),
            prefix=prefix,
            version=VERSION,
            github=GITHUB,
            edit=edit_row(p.edit),
            content=content,
            nav=render_nav(groups, p, prefix),
            topnav=render_topnav(p.group, prefix),
            crumbs=render_crumbs(p, prefix),
            toc=render_toc(p, prefix),
            pager=render_pager(p, prefix),
            group=html.escape(p.group),
            body_class="page",
            og_type="article",
            version_note=html.escape(version_note()),
            og_url=f"{SITE}/{p.path}",
            site=SITE,
        )
        target = ROOT / p.path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(out_html, encoding="utf-8")

    # ---- landing page
    stats = {
        "stat_api": api_name_count(),
        "stat_headers": str(len(list((REPO / "include/webc").glob("*.h")))),
        "stat_modules": str(len(MODULES)),
        "stat_examples": str(len(EXAMPLES)),
    }
    landing = string.Template(read(SRC / "landing.html")).substitute(
        prefix="",
        version=VERSION,
        github=GITHUB,
        **stats,
        nav=render_nav(groups, None, ""),
        topnav=render_topnav("home", ""),
        crumbs="",
        modules=render_modules(prefix=""),
        features=render_features(),
    )
    (ROOT / "index.html").write_text(
        shell.substitute(
            title=page_title(html.escape("WebC — web apps in C")),
            description="A framework for web apps in C: one language from the route to the DOM, "
                        "arenas instead of a collector, and a browser half compiled from the same views.",
            prefix="",
            version=VERSION,
            github=GITHUB,
            edit="",
            content=landing,
            nav=render_nav(groups, None, ""),
            topnav=render_topnav("home", ""),
            crumbs="",
            toc="",
            pager="",
            group="home",
            body_class="home",
            og_type="website",
            version_note=html.escape(version_note()),
            og_url=f"{SITE}/",
            site=SITE,
        ),
        encoding="utf-8",
    )

    (ROOT / "404.html").write_text(
        shell.substitute(
            title=page_title(html.escape("Not found")),
            description="That page does not exist.",
            prefix="",
            version=VERSION,
            github=GITHUB,
            edit="",
            content='<div class="missing"><p class="page-kicker">404</p>'
                    "<h1>That page is not here.</h1>"
                    '<p class="lede">The docs are indexed in the sidebar; '
                    '<a href="docs/index.html">start at the beginning</a>, or press '
                    "<kbd>/</kbd> to search.</p></div>",
            nav=render_nav(groups, None, ""),
            topnav=render_topnav("home", ""),
            crumbs="",
            toc="",
            pager="",
            group="home",
            body_class="page",
            og_type="website",
            version_note=html.escape(version_note()),
            og_url=f"{SITE}/404.html",
            site=SITE,
        ),
        encoding="utf-8",
    )

    # ---- assets
    assets = ROOT / "assets"
    assets.mkdir(exist_ok=True)
    for f in (SRC / "assets").iterdir():
        if f.is_dir():
            shutil.copytree(f, assets / f.name, dirs_exist_ok=True)
        else:
            shutil.copy2(f, assets / f.name)
    (ROOT / ".nojekyll").write_text("", encoding="utf-8")
    # Every page is public documentation; the only thing worth telling a crawler is that there is
    # nothing to hide. (No sitemap: the site has one entry point per group and the sidebar is the
    # map — a file that lists 55 URLs goes stale the first time a section is renamed.)
    (ROOT / "robots.txt").write_text("User-agent: *\nAllow: /\n", encoding="utf-8")

    # ---- search index (a JS file, not JSON: `fetch` does not work from file://)
    entries = []
    for p in pages:
        text = mdhtml.plain(p.html)
        entries.append({
            "t": p.title,
            "p": p.path,
            "g": p.group,
            "h": [h["text"] for h in p.headings if h["level"] in (2, 3)][:40],
            "b": text[:9000],
        })
    (assets / "search.js").write_text(
        "window.__WEBC_SEARCH__=" + json.dumps(entries, separators=(",", ":")) + ";\n",
        encoding="utf-8",
    )

    total = len(pages) + 2
    # One search entry per page, and it is asserted: a page that quietly fell out of the index is
    # a page nobody can find, and the count is the only symptom it has.
    if len(entries) != len(pages):
        raise SystemExit(f"build.py: {len(pages)} pages but {len(entries)} search entries")
    print(f"website: {total} pages, {sum(len(p.html) for p in pages) // 1024} KB of content, "
          f"{len(entries)} search entries")

    if args.serve:
        import http.server, socketserver, functools
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(ROOT))
        with socketserver.TCPServer(("127.0.0.1", args.port), handler) as httpd:
            print(f"serving website/ on http://127.0.0.1:{args.port}/")
            httpd.serve_forever()


# --------------------------------------------------------------------------- fragments

TOP = [
    ("docs/index.html", "Docs", "Start"),
    ("guides/http.html", "Guides", "Guides"),
    ("reference/index.html", "Reference", "Reference"),
    ("examples/index.html", "Examples", "Examples"),
    ("project/contributing.html", "Project", "Project"),
]


def render_topnav(group, prefix):
    out = ['<nav class="topnav" aria-label="Sections">']
    for path, label, target in TOP:
        active = group == target
        aria = ' aria-current="true"' if active else ""
        out.append(f'<a href="{prefix}{path}"{aria}>{label}</a>')
    out.append("</nav>")
    return "\n".join(out)


def render_crumbs(page, prefix):
    if page.path == "docs/index.html":
        return '<span class="crumbs__here">Docs</span>'
    out = [f'<a href="{prefix}{"docs/index.html"}">Docs</a>']
    if page.group != "Start":
        target = {"Guides": "guides/http.html", "Reference": "reference/index.html",
                  "Examples": "examples/index.html",
                  "Project": "project/contributing.html"}.get(page.group)
        if target:
            out.append('<span class="crumbs__sep">/</span>')
            out.append(f'<a href="{prefix}{target}">{html.escape(page.group)}</a>')
    out.append('<span class="crumbs__sep">/</span>')
    out.append(f'<span class="crumbs__here">{html.escape(page.short)}</span>')
    return "".join(out)


def render_nav(groups, current, prefix):
    out = ['<nav class="sidenav" aria-label="Documentation">']
    for gname, items in groups.items():
        out.append('<div class="sidenav__group">')
        out.append(f'<div class="sidenav__label">{html.escape(gname)}</div>')
        out.append("<ul>")
        for p in items:
            active = current is not None and p.path == current.path
            cls = ' class="is-active"' if active else ""
            aria = ' aria-current="page"' if active else ""
            out.append(
                f'<li><a href="{prefix}{p.path}"{cls}{aria}>{html.escape(p.short)}</a></li>'
            )
        out.append("</ul></div>")
    out.append("</nav>")
    return "\n".join(out)


def render_toc(page, prefix):
    items = [h for h in page.toc if h["id"] != "top"]
    if len(items) < 2:
        return ""
    out = ['<aside class="toc" aria-label="On this page">', '<div class="toc__label">On this page</div>', "<ul>"]
    for h in items:
        out.append(
            f'<li class="toc__l{h["level"]}"><a href="#{h["id"]}">{html.escape(h["text"])}</a></li>'
        )
    out.append("</ul></aside>")
    return "\n".join(out)


def render_pager(page, prefix):
    parts = ['<nav class="pager" aria-label="Previous and next">']
    if page.prev:
        parts.append(
            f'<a class="pager__link pager__link--prev" href="{prefix}{page.prev.path}">'
            f'<span class="pager__dir">Previous</span>'
            f'<span class="pager__title">{html.escape(page.prev.title)}</span></a>'
        )
    else:
        parts.append("<span></span>")
    if page.next:
        parts.append(
            f'<a class="pager__link pager__link--next" href="{prefix}{page.next.path}">'
            f'<span class="pager__dir">Next</span>'
            f'<span class="pager__title">{html.escape(page.next.title)}</span></a>'
        )
    parts.append("</nav>")
    return "\n".join(parts)


MODULES = [
    ("ui", "markup", "switch, segmented control, tab, meter, kbd · badge, stat, empty state, skeleton"),
    ("icons", "markup", "stroke icons as data — the ones you draw are the ones that link"),
    ("table", "markup", "sortable, paginated table: order without owning the rows"),
    ("overlay", "markup", "dialog, drawer, menu, toast, tip — with the ARIA and the focus"),
    ("form", "markup", "a form drawn from the model that validates it"),
    ("theme", "markup", "design tokens and a dark edition that does not flash"),
    ("keys", "interaction", "one table of shortcuts, bound and documented from the same rows"),
    ("motion", "interaction", "enter animation and stagger, no script, reduced-motion honoured"),
    ("dnd", "interaction", "pick a row up and put it down: the drop index, not the maths"),
    ("live", "interaction", "rooms over SSE: whole documents, coalesced, a dead tab costs nothing"),
    ("csv", "data", "in and out, quoted correctly, by header name"),
    ("search", "data", "a search box that ANDs its words, escapes a typed %, and ranks the title"),
    ("i18n", "data", "one app, two languages: catalogs, plurals, direction"),
    ("upload", "data", "a file arrives and is checked: magic bytes, a name this side makes up"),
    ("time", "data", "dates, ages and two-unit durations, UTC only and proud of it"),
    ("richtext", "data", "HTML a user wrote, rendered through a policy: parsed, allow-listed"),
    ("ld", "data", "structured data: the article, the site, the trail, the FAQ"),
    ("mail", "platform", "a real email from one view: SMTP, or a file in the outbox"),
    ("jobs", "platform", "work that outlives the request: a table, a lease, a retry"),
    ("limits", "platform", "a rate limit that is one line, and a table that cannot grow"),
    ("pwa", "platform", "installable and offline: manifest, worker, cache keyed by the build"),
]


def render_modules(prefix):
    out = ['<div class="modules">']
    for name, tier, what in MODULES:
        out.append(
            f'<div class="module"><div class="module__head"><code>{name}</code>'
            f'<span class="module__tier">{tier}</span></div>'
            f'<p>{html.escape(what)}</p></div>'
        )
    out.append("</div>")
    return "\n".join(out)


def render_features():
    rows = [
        ("C23, and only C23",
         "No preprocessor tricks beyond macros, no GC, no hidden allocation. Every allocation goes "
         "through <code>wm_alloc</code> or an arena you can see."),
        ("Arenas, not a collector",
         "The framework installs one arena per scope — process, request, render — and resets it in "
         "O(1). An app says <code>fmt()</code> or <code>persist()</code>, never “arena”."),
        ("Green threads",
         "M:N on kqueue: channels, <code>select</code>, timers, mutexes, wait groups. A handler that "
         "awaits is a handler that costs a few kilobytes."),
        ("One language, two halves",
         "The same view compiles to the server binary and to wasm32-wasip1. The browser half "
         "hydrates the markup the server already sent."),
        ("Typed all the way down",
         "Routes, models, forms, JSON and migrations are macros the compiler checks. A typo is a "
         "compile error, not a 500 at 3 a.m."),
        ("The boundary, generated",
         "The API leaves the app as it was declared: <code>webc openapi</code> writes OpenAPI 3.1 "
         "and <code>webc client --ts</code> a typed fetch client, from the routes and models "
         "themselves — and <code>webc deploy --pack</code> writes the ops files that run the "
         "binary. Generated once, owned from then on, ratcheted in CI."),
        ("A gate for every surface",
         "<code>webc api</code>, <code>webc lint</code>, <code>make budget</code>: the public names, "
         "the leaks and the bytes are all counted on every build."),
    ]
    out = ['<div class="features">']
    for title, text in rows:
        out.append(f'<div class="feature"><h3>{title}</h3><p>{text}</p></div>')
    out.append("</div>")
    return "\n".join(out)


if __name__ == "__main__":
    main()