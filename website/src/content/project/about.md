## Who makes this

WebC is written and maintained by **Victor-Marian Bușoi** ([@bumasoft](https://github.com/bumasoft)),
who reviews every change that lands. It is not a company product: it is a framework built to write
real applications, released because the same trade-offs are useful to other people — C's control
over memory and startup time, without giving up the ergonomics that make a web app pleasant to
write.

## Why it exists

The pitch on the front page is a set of constraints, and every one of them was chosen against an
alternative that is easier:

- **C, because the runtime is the point.** A binary that starts in milliseconds, owns its memory, and
  has no interpreter between the route and the response. Every framework in the adjacent space
  borrowed a VM or a garbage collector to get ergonomics; WebC borrows the compiler instead.
- **Arenas, because a request is a scope.** One arena per request means deallocation is a truncation,
  not a traversal. The bookkeeping that would be a collector's job is a `defer` in the handler.
- **One language for both halves**, because the alternative is two models, two validation rules and
  a serialisation boundary that drifts. `WEBC_MODEL` is declared once and every layer is generated
  from it — the form, the JSON, the table, the migration, the browser's copy.
- **A gate for every surface**, because a claim that is not counted is a claim that rots. That is why
  there is an `api.txt` ratchet, a wasm-size budget, a link checker over these pages, a snippet
  compiler over every sample, and a CI job that builds this site — including the number in the hero,
  which is read from the same file the gate writes.

## The licence

**Apache-2.0**, with an explicit patent grant, and a [NOTICE](https://github.com/bumasoft/WebC/blob/main/NOTICE)
that lists what WebC links against without vendoring it (SQLite, zlib, optionally OpenSSL) and states
that no third-party source is copied into this tree. Every source file carries an SPDX header, and
the build fails when one does not.

Copyright © 2026 Victor-Marian Bușoi.

If you are evaluating WebC for an employer: the licence is the same one most corporate policies
already have on their allow-list, and the decision to use it over MIT — and over a custom licence —
is [written down with its reasoning](decisions.html).

## How it is built

Plain `make`, one C23 compiler, and no build system to learn:

```sh
git clone https://github.com/bumasoft/WebC && cd WebC
make            # libwebc.a, the webc CLI, every example
make test       # the framework's own suites
make site       # this website, from docs/reference.md and website/src/
```

The site you are reading is generated from the same document the repository ships — the reference is
one file, split into pages by its own section headings — and it is committed, so it opens from a
checkout with no server and no network. There is no CDN, no web font, and no JavaScript framework
behind it: one CSS file, three small scripts, and a highlighter written for this site.

## Contributing, security, and the record

- [Contributing](contributing.html) — the gates, the house rules, the sign-off.
- [Security](security.html) — what is in scope, and the private channel.
- [Decisions](decisions.html) — the record: licence, name, Linux, compiler floor, version, installer.
- [Changelog](changelog.html) — what is in a release.
- [Roadmap](roadmap.html) — where this is going, and what it will never do.

## Colophon

Set in the reader's own system UI font, because a documentation site should load instantly on a
train. Dark by default, light on request, and the theme is applied before the first paint so a page
never flashes white. No tracking, no analytics, no cookies — the only thing stored in your browser is
which theme you chose.

Built with [its own tooling](https://github.com/bumasoft/WebC/tree/main/website): `website/build.py`
generates every page, `website/src/mdhtml.py` renders the Markdown, and `make links`, `make snippets`
and `make site` are the gates that keep the words attached to the code they describe.