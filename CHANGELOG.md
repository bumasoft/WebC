# Changelog

All notable changes to WebC are recorded here. The format is [Keep a
Changelog](https://keepachangelog.com/en/1.1.0/), the version numbers are
[Semantic Versioning](https://semver.org/spec/v2.0.0.html), and the project is pre-1.0: a minor
version may change the surface, and `api.txt` is the diff to read when it does.

## [Unreleased]

## [0.1.0] — 2026-11-18

The first release: a C23 framework for full-stack web work, compiled to a static binary, with one
model declaration shared by the server and the browser half.

### Added

- **One language, two halves.** `view()` markup is compiled for both targets from the same file;
  `src/` builds an SSR server, a wasm module for the browser, or both, and the client hydrates the
  server's HTML rather than re-rendering it.
- **The core runtime.** Arenas instead of a collector, `str` slices, typed errors with `must`/`?`,
  generics (`Vec(T)`, `Map(K, V)`), and a task scheduler — Go's ergonomics with C's ownership story.
- **Models.** `WEBC_MODEL` gives a table, a struct and a wire shape from one declaration, with
  validation, JSON, form decoding and SQL generated from it; migrations with a twin per dialect.
- **The ORM.** Typed filters, relations read with no query (batched or joined), aggregates, pages,
  trees, and a query cache keyed by generation.
- **Signals, DOM and patching.** `state`, memos and effects; SSR, hydration, keyed lists and
  two-way binding driven by the same renderer as the server.
- **The HTTP server.** A router table, typed handlers, keep-alive and pipelining, chunked bodies,
  `Range` on `res_file()`, gzip, CSP, multipart parsing with limits.
- **Auth.** PBKDF2-SHA256 (210 000 iterations) passwords, a signed `HttpOnly; SameSite=Lax` session
  cookie with no session table, and a guard that is one call.
- **Four storage backends, no hiredis and no libpq.** SQLite, PostgreSQL, MySQL/MariaDB and Redis,
  each one the wire protocol implemented here, each with its own gate.
- **The CLI.** `webc new`, `build`, `run`, `serve --watch`, `test`, `check`, `fmt`, `lint`,
  `openapi`, `client --ts`, `doctor`, `version`, `docs`, `explain`.
- **Tooling that is part of the framework.** A language server (diagnostics, completion, hover,
  quick fixes) and an MCP server, both from the same binary, plus a VS Code extension and an agent
  skill with generated reference leaves.
- **The documentation.** 4 230 lines of reference with `SPEC §n` citations from the source, a
  getting-started guide, seventeen guides, eight runnable examples, and a static site generated from
  all of it — no runtime, no CDN, no bundler.
- **Eight examples**, each one a different shape of the framework: hello, todo, accounts (auth),
  web (client-only), js (the seam), stdlib (the opt-in shelf), board (live rooms and a database),
  minimal (the bundle floor).
- **Linux support**: the epoll backend is the default there, and CI runs the suite on macOS and
  Linux with AddressSanitizer and UndefinedBehaviorSanitizer, plus fuzzers over the parsers.
- **Install**: a prebuilt tarball per platform, or `make install` into `~/.local`. The installer
  refuses a tarball whose digest does not match the published `.sha256`.

### Notes

- **Licence: Apache-2.0**, with a patent grant and a `NOTICE` that lists what is linked but not
  vendored. Every source file carries an SPDX header; `sh scripts/spdx.sh --check` enforces it.
- **No npm package, and no plans for one** — `docs/decisions.md` D2 says why.
- **The compiler floor is clang 21**: the suite uses `#embed`.
- **Gates that ship with the repository**, all green on `main`: the suite (104), the developer
  surface and the `api.txt` ratchet, the language server, the MCP server, the budgets, the installer
  test, the site's links and snippets, the not-stale checks for the skill, the formatter ratchet and
  the licence headers. `docs/decisions.md` and `AGENTS.md` say what each one is for.

[Unreleased]: https://github.com/bumasoft/WebC/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/bumasoft/WebC/releases/tag/v0.1.0