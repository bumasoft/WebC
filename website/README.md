# The WebC documentation site

The source of <https://bumasoft.github.io/WebC/> — and, like everything else here, a thing you can
read rather than a thing you have to run.

```sh
make site              # regenerate website/ from the sources below
make site ARGS=--serve # …and serve it on :8080 to look at it
make snippets          # compile every code sample in src/content/, on its own
make links             # every relative link on the site and in the repo's Markdown resolves
```

The footer's version line is decided by `--release`, and **remembered in `website/.release`**:
`make site ARGS=--release` writes that file (the version the site documents) and every page says
"Documentation for v0.1.0"; without it, "Documentation for the development tree, ahead of v0.1.0".
Git cannot be the authority — the site is committed *before* the tag exists — and it cannot be the
authority afterwards either: a plain `make site` an hour after the tag would flip 57 pages back and
fail the docs job. It did, on the v0.1.0 push. The committed marker means a plain `make site` now
reproduces the release build byte for byte; delete `.release` (with a `VERSION` bump) when the next
cycle starts.

`robots.txt` is generated too (everything is public; there is deliberately no `sitemap.xml` — the
sidebar is the map, and a file listing 55 URLs goes stale the first time a section is renamed).

The output is **committed**: plain HTML, CSS and JS with no runtime, so GitHub Pages serves
`website/` directly (`Settings → Pages → main /website`) and any page also opens from `file://` with
no server and no network.

## What generates what

| Source | Becomes | Who writes it |
| --- | --- | --- |
| `docs/reference.md` | `website/reference/*.html`, one page per `## N.` section | the framework: it is the same text the repository ships |
| `examples/<name>/` | `website/examples/<name>.html`, with the real files | the repository |
| `src/content/*.md` | `website/docs/*.html`, `website/guides/*.html`, `website/project/*.html` | hand-written for the site |
| `CONTRIBUTING.md`, `CHANGELOG.md`, `SECURITY.md`, `docs/decisions.md` | `website/project/*.html`, as they are | the repository: published, not summarised |
| `src/landing.html` | `website/index.html` | hand-written |
| `src/shell.html` | the frame every page shares | hand-written |
| `src/assets/*` | `website/assets/*` (copied), plus a generated `search.js` | hand-written |

The **Project** group is the exception to "the site owns only the tutorials": those four pages are
the repository's documents rendered where they are, so the changelog a reader finds on the site is
the changelog in the repository, not a summary that drifts from it. Their relative links (to
`AGENTS.md`, `LICENSE`, `scripts/spdx.sh`) become links to the file on GitHub, because the site does
not publish those and a 404 in a governance page is worse than a link to the source.

Nothing on the site is a second copy of the docs. If the reference changes, `make site` changes the
site; if an example changes, its page changes. The only prose the site owns is the tutorials
(`src/content/`), and the point of them is to be the on-ramp into the reference rather than a
replacement for it.

## The pieces

- **`build.py`** — the generator. Splits the reference into pages by its anchors, rewrites
  cross-page `#anchor` links so a section can be linked from anywhere, renders every Markdown file
  through `mdhtml`, assembles prev/next, the sidebar, the crumb trail and the on-this-page rail,
  writes the search index, and copies the assets.
- **`src/mdhtml.py`** — a dependency-free Markdown renderer: fenced code with a copy bar and a
  filename, tables, nested lists, blockquotes → callouts, and the `<a name>` anchors the reference
  already uses.
- **`src/assets/site.css`** — the design system: dark by default, light on demand, three columns,
  and every colour a custom property.
- **`src/assets/highlight.js`** — a 3 KB highlighter for C, shell, JSON, SQL, HTML, CSS and JS. It
  works on the code's text, not on the HTML, so an entity in a string cannot confuse it.
- **`src/assets/site.js`** — theme (remembered, and applied before first paint), copy buttons, the
  on-this-page rail, the mobile drawer, and search (`⌘K` or `/`).
- **The search index** is written as `assets/search.js` (`window.__WEBC_SEARCH__ = …`) rather than a
  JSON file, because `fetch()` does not work from `file://` — and the site is meant to open from a
  checkout.

## Pictures

`src/assets/media/` holds the product shots — one per example, taken from `webc dev` at 1280×800 and
downscaled with LANCZOS, plus `wasm-todo.gif`, the wasm app recording an edit. They are committed
like everything else; the site has no image pipeline and does not want one.

| File | Where it appears |
| --- | --- |
| `media/examples-<slug>.png` | the top of `examples/<slug>.html` (all but `minimal`, whose page is `hello`'s in fewer bytes) |
| `media/wasm-todo.gif` | the landing page's "in the browser" section |
| `og.png` | the share card: `og:image` and `twitter:image` on every page |

`src/og-card.html` is the **source** of `og.png`. It is rendered by hand — open it at a 1200×630
viewport and screenshot it — because the build has no browser in it and never will. Its text is
deliberately timeless: no version, no counts, because a card that says "1 841 names" is a card that
is wrong three weeks later.

The other numbers on the site are not written by hand either: the landing page's name count is
parsed from `api.txt`'s first line, the module and example counts come from the lists in
`build.py`, and the version comes from `VERSION`.

## Every sample is a file you could compile

A reader who copies a snippet into a project must be able to build it. So each C block in
`src/content/` is a **whole unit**: its own `#include`s, its own `WEBC_MODEL`, its own `view()` — and a
first line that says which file it is and which half it belongs to:

```c
// src/client.c — a draft in a signal
#include <webc/web.h>
...
```

`make snippets` (`website/tools/snippets.py`) enforces that, block by block:

| Rule | Why |
| --- | --- |
| the first line is a `//` comment naming a file | the reader needs to know where it goes, and the checker needs to know which half |
| no `…` or `...` in code | an ellipsis is a promise the sample does not keep |
| the block compiles with `clang -fsyntax-only -std=c23 -I include`, on its own | no name from a neighbouring block, no variable from the prose |
| no `return` in a body fragment | a fragment is wrapped in a function; a `return` there would hide what the sample leaves out |
| no line wider than 84 columns | that is what the article column fits; a line past the edge is a line the reader has to scroll sideways to finish, and hidden scrollbars are how a complete line looks incomplete |

It classifies a block by what it already contains: `#include` plus a `view(`/`GET(`/`client_main` is a
translation unit; a few statements are a body, and the checker wraps them in a function and lifts
their includes above it. Repeating a model declaration in a second snippet is deliberate — the reader
who lands on that snippet has not read the first one.

`make snippets ARGS=-v` prints each block and the verdict, which is also the fastest way to see
whether an API you just renamed is still used the way the docs say.

## Adding a page

1. Write `src/content/guides/your-topic.md`, starting at `##` — the page's `<h1>` comes from its
   title in `build.py`.
2. Add an entry to the `guides` list in `build.py` (`slug`, title, description).
3. `make site`, then look at it: the sidebar, the crumb, the rail and prev/next come from the list
   order, not from the file.

Links are ordinary relative links (`../reference/http.html`) and anchors work across pages
(`../reference/http.html#7c-ranges`).

## House rules the site follows

- **No dependencies.** No npm, no bundler, no CDN. A page is HTML, one CSS file and three JS files.
- **No build step for the reader.** The committed output *is* the site.
- **The docs are the source.** Prose that contradicts `docs/reference.md` is a bug in the prose.
- **Fast on a train.** No web fonts, no images to wait for, the whole site under a megabyte gzipped.