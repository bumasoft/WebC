# The docs, and which one to change

Seven places hold prose about WebC. They are not copies of each other: each one is generated from, or
generates, exactly one other thing, and the point of this page is that you never have to guess which
file to edit.

| You want to change… | Edit | Then run |
| --- | --- | --- |
| how the framework *works* — every macro, every rule | `docs/reference.md` | `make site`, `make skill` |
| the ten-minute on-ramp | `docs/getting-started.md` | — |
| why something is the way it is | `docs/decisions.md` | — |
| a tutorial or guide on the site | `website/src/content/guides/*.md` | `make site` |
| the site's landing page | `website/src/landing.html` | `make site` |
| what an agent is told before it writes code here | `skills/webc/SKILL.md` | `make skill` |
| how to work *in this repository* | `AGENTS.md`, `CONTRIBUTING.md` | — |
| what is in the release | `CHANGELOG.md` | — |

## `docs/reference.md` — the truth

4 200 lines, and the only document that is allowed to define behaviour. The `SPEC §n` citations in
`src/` and `tools/` are sections of it, which is why a behaviour change and a reference change belong
in the same commit: a citation that points at a paragraph which no longer says that is a bug the
compiler will not find.

Its `## N.` sections are the site's Reference group, **one page each**, split by anchor. A section
can be linked from any page as `../reference/http.html#7c-ranges` — the generator rewrites
cross-page anchors so the URL is stable, and `tools/check-links.py` (`make links`) is the gate that
notices when one stops resolving.

§12i is the standard library: the opt-in shelf, what each module costs in bytes, and what it refuses
to be. §1 is the house rules, which is the section to read before proposing a name.

## `docs/getting-started.md` — the on-ramp

Not published on the site, and deliberately so. It is the sibling of the site's
[Quickstart](https://bumasoft.github.io/WebC/docs/quickstart.html): the same walk in the same order
from a partly different starting point — this one begins with the installer and a checkout, the site
one begins with a scaffolded app and assumes a browser tab rather than a terminal. Two audiences
that share a spine, which is cheaper to keep true than one page trying to be both.

## `docs/decisions.md` — why

The decision record: one `## D<n>` per decision, newest last. D1 the licence, D2 the name and no npm,
D3 the Linux port, D4 the compiler floor, D5 the leak debt gate, D6 the one version number, D7 the
installer's refusal. Each entry says what was decided, what the alternative was, and what it cost.

If you are about to argue that something here should be different, this is the file to read first —
and the file to add an entry to if you win. It is a record, not a proposal process.

## `website/src/content/` — the site's own prose

Hand-written for the site, in Markdown, rendered by `website/src/mdhtml.py`. Start pages in the
directory root, guides in `guides/`. Adding a page means adding it to the `guides` list in
`website/build.py` as well: the sidebar, the crumb trail, prev/next and the search entry all come
from that list's order, not from the file.

Every C block in here must compile on its own — its own `#include`s, own `WEBC_MODEL`, own `view()`
— and start with a `//` comment naming the file it is. `make snippets` enforces it, block by block,
and the rules are in `website/README.md`.

## `skills/webc/` — the agent skill

`SKILL.md` is written by hand. `references/*.md` are **generated** from the pages above by
`make skill`, and `make skill-check` fails when they drift or when a leaf teaches a retired word.
Never edit a generated leaf: change the source page and run `make skill`.

## Somebody else's words

`LICENSE` (Apache-2.0), `NOTICE` (what is linked and not vendored) and `CODE_OF_CONDUCT.md` (the
Contributor Covenant) are quoted documents with one local edit — a copyright line or a contact —
and are not part of the docs pipeline at all.

## The rule underneath all of it

**The site is generated and committed.** `make site` writes `website/`; the output is in the
repository because GitHub Pages serves it directly and because a page that opens from `file://` is a
page nobody needs a server to read. Editing `website/*.html` by hand is always wrong — the next
`make site` overwrites it, and CI fails when `website/` and its sources disagree.

So the loop is: change the source, `make site`, commit both. Then:

```sh
make links      # every relative link and #anchor on the site resolves
make snippets   # every C sample compiles on its own
make skill      # regenerate the agent skill's leaves
```