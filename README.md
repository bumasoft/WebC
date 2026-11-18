# WebC

[![Licence: Apache-2.0](https://img.shields.io/badge/licence-Apache--2.0-blue.svg)](LICENSE)
[![C23](https://img.shields.io/badge/C-23-informational.svg)](docs/decisions.md)
[![Release](https://img.shields.io/github/v/release/bumasoft/WebC?color=informational)](CHANGELOG.md)

C23 for full-stack web work: Go's concurrency and ergonomics on the server, SolidJS's
reactivity model in the browser, one language and one model declaration for both,
compiled to a static binary.

```c
// src/app.h — the model, shared by both halves
#include <webc/shared.h>

WEBC_MODEL(Todo, (id, i64, .pk = true),
                 (title, Text, .required = true, .max = 200),
                 (done, bool))
Vec(Todo)
declare_view(Todos)      // the view's prototype, in one line
```

```c
// src/view.c — markup only, compiled for both targets
view(Todos, VecTodo *todos) {
  el("main") {
    el("h1") { text("Todos"); }
    el("form") { post_to("/todos"); el("input") { name("title"); required(); } }
    el("ul") { for_each(t, *todos) {
      el("li") { class_(t->done ? "done" : ""); text(t->title); }
    } }
  }
}
```

```c
// src/server.c — routes, database, nothing else
#include <webc/server.h>
#include "app.h"

GET("/")          { return page(Todos, db_all(Todo)); }
GET("/api/todos") { return json(200, db_all(Todo)); }
POST("/todos")    { Todo t = body(Todo); db_save(Todo, &t); return redirect("/"); }
```

No `main()`, no middleware list, no arena, no `#ifdef` for the target. `webc.json`
names the file sets (`server` / `shared` / `client`); `webc build` produces the binary
and, when there is a client half, the wasm module from the same view.

## Requirements

| Piece | Why | What it is |
| --- | --- | --- |
| A C23 compiler | `nullptr`, `__VA_OPT__`, `[[maybe_unused]]`, `#embed` | `clang` 21 or newer (the suite uses `#embed`, which clang 18 does not have). GCC 15 takes `-std=c23` but the suite is not run against it |
| `make` | builds the library, the CLI and the examples | any POSIX `make` |
| sqlite3 headers | the default database | shipped with macOS; `libsqlite3-dev` on Linux |
| zlib | HTTP response compression | shipped with macOS; `zlib1g-dev` on Linux |
| wasi-sdk | only for an app with a browser half | optional, `webc doctor` says whether it found it |
| node | only for the bridge and `check` test suites | optional |

**Platforms.** macOS (Apple Silicon and Intel) and Linux (x86_64 and aarch64) both build and pass
`make test` in CI; the release artefacts are macOS today and the Linux matrix leg is the source
build. The scheduler uses kqueue on macOS and epoll on Linux, and the epoll backend is what the
Linux leg exercises. Windows is not supported.

Nothing else is installed, vendored or fetched at build time: one external library (sqlite3) and
one optional toolchain (wasi-sdk).

## Quickstart

```sh
curl -fsSL https://raw.githubusercontent.com/bumasoft/WebC/main/scripts/install.sh | sh
# or, from a checkout:
make                      # builds build/libwebc.a, build/webc and every example
build/webc new myapp --template=fullstack   # or server | web | board
cd myapp && ../build/webc dev
```

`webc dev` builds the app, runs it, and watches the tree. A CSS edit re-fetches one
stylesheet; a client-half edit swaps the wasm module in place — the DOM, the signals,
the scroll position and the caret survive; a server edit restarts the process; a build
failure shows the translated error over the last good page instead of touching it.
`webc serve` is the same app as a deployment runs it: a release build, no watcher, gzip
on — a browser-only project served over `public/` and `.webc/app.wasm` (87 KB on the wire
for a 223 KB first load), a project with a server half built and exec'd.
`build/webc doctor` tells you what your toolchain is missing.

## What is in the box

| Area | What you get |
|---|---|
| Language | C23, no preprocessor tricks beyond macros, no GC, no hidden allocation |
| Memory | scope arenas the framework installs (process, request, render) with O(1) reset; an app says `fmt()` or `persist()`, never "arena" |
| Errors | `WErr`/`WErrRes` + `try`/`tryv`/`errdefer`, panics that become HTTP 500s at the boundary |
| Concurrency | M:N green threads on kqueue, channels, `select`, timers, mutexes, wait groups, `task_async` |
| HTTP | typed router, typed request/response, middleware chain, **gzip on by default** (`mw_compress()`: files and rendered bodies, `Vary` and `-gz` ETags included, `WEBC_NO_GZIP=1` to turn it off), **byte ranges** (`Range`/206/`Content-Range`/`If-Range`/416 on every file response — seekable media, resumable downloads, `res_download()` for a saved export), SSE, websockets, graceful drain; the page's **`<head>` spliced before anything compresses it** (`mw_document()`) with **authored chrome from `partials/`** (`partial("site/head")`) and **structured data as one expression** (`head_ld(ld_blog_posting(...))` from `webc/ld.h` — a field you leave out is omitted, never `""`), a **real 404 page** and a **CSP as a value** from `webc.json` (`not_found`, `csp`), and a **500 that names its request** — route, method and id in the body, the log line and the crash report |
| Data | model-first SQL over **sqlite, postgres and MySQL/MariaDB** (each a hand-written wire protocol — no libpq, no libmysqlclient), migrations with a twin per dialect, `make pg-test` / `make mysql-test` as live-server gates; **enums** as fields, `.null` columns and the `NULL` they read as (`ww_is_null`), **documents** that compose models into one wire shape with both directions generated, and `WEBC_ROW` for a row you computed (a dashboard KPI) — JSON and a `Vec`, no table, so the schema check has nothing to be wrong about |
| ORM | typed filters (`WWhere`: `ww_eq_i64(&w, Todo, owner, id)`, `ww_limit`, `ww_contains` with the escaping built in), `db_one`/`db_all_w`/`db_count_w`, aggregates (`db_sum`/`db_min`/`db_max`/`db_avg`), `db_page` with a `WPager`, and relations declared once — `WEBC_MODEL_REF`/`WEBC_MODEL_MANY`, `prefetch`/`ref`/`refs` (one statement per collection, never one per row), `db_save_all`/`db_delete_tree`. `db_statement_count()` is how a test proves a page's query count is constant |
| Cache | a **query cache** at `db_exec`: result sets kept in Redis and invalidated by a per-table generation counter (`WEBC_QUERY_CACHE=1`, off by default), with `nocache { }` and `/* no-cache */` for the exceptions |
| Redis | built in, RESP3 over parked sockets (no hiredis): strings, hashes, lists, sets, sorted sets, expiry, counters, pipelines and pub/sub, with `rediss://` for TLS |
| Frontend | signals/memos/effects (global or per-instance), a DOM builder with node reuse, keyed lists, **parts** (typed view functions) and **component instances** (`comp(Name, …)` gives a call its own `state_own`, its own `effect_own`/`on_cleanup`/`on_mount` lifecycle, and children via `WKids`), **live bindings** (`text_live`/`attr_live`/`class_live`/`show_live` write one node in place, and `action_live` handles an event without re-rendering the view), SSR + hydration; a **WStore** for streaming frames; `bind_ctx` inputs bound straight to context fields; pointer positions in an action (`ev_point`) and `webc/chart.h` for chart geometry; `fmt_num`/`fmt_money`/`fmt_pct`/`fmt_compact` and `sb()` for attributes built from numbers |
| Browser | the same frontend compiles to wasm32-wasip1, hydrates the server's markup and drives the real DOM through `assets/webc.js`; `js()` for JavaScript — typed arguments, `WJs` results, `js_then()` for promises — and `WebC.call`/`WebC.dispatch`/`webc_export_json()` for the other direction |
| Standard library | 21 opt-in headers an app `#include`s and nothing is forced — markup (`ui`, `icons`, `table`, `overlay`, `form`, `theme`), interaction (`keys`, `motion`, `dnd`, `live`), data (`csv`, `search`, `i18n`, `upload`, `time`, `richtext`, `ld`) and platform (`mail`, `jobs`, `limits`, `pwa`). `richtext` is the one the floor cannot do without a parser: HTML a *user* wrote, re-emitted through a policy — allow-listed tags, entity-decoded URLs, no `on*`, headings given ids, and the excerpt and reading time from the same parse. `null` in a JSON body and a nullable column likewise need no hand-written half: `.null = true` on an integer field is the whole declaration (`ww_is_null`, `references`) Unused costs nothing and `webc size --modules` proves it by building a probe per module; `webc stdlib` lists them, `webc add <module>` installs one |
| Tooling | `webc new/build/dev/serve/test/fmt/migrate/doctor/routes/size/get/deploy/ci/openapi/client/learn/check/explain/lint/api/docs/stdlib/add/lsp/mcp`. Two of them close the gap between "the model says" and "the table is": `webc migrate diff` turns schema drift into additive SQL (`--write` lands it as a migration, what it cannot infer becomes a `-- TODO`), and `webc scaffold Post` writes list/form/save/delete and its tests from the app's own descriptor — code you own, never a rewrite of your model. `webc deploy --pack docker,fly,systemd,caddy,nginx` writes the ops files that run the artifact — stamped from the manifest, SSE-aware — and `webc ci` writes the gate (`webc check` + `webc test`, a dist artifact on a tag) as a workflow the app owns. `webc openapi` writes the API boundary as OpenAPI 3.1 and `webc client --ts` a typed fetch client, both from the routes and models the sources declare — `--check` fails when either drifts. `webc learn` is five lessons whose own gates fail until the one edit lands. `webc test --watch` reruns on save and `--test=` narrows by name; `TestClient` in `<webc/test.h>` is a jar, a form encoder and `ASSERT_REDIRECT`/`ASSERT_DB_ONE`/`ASSERT_LD`; three templates that each build and pass their own tests; five fuzzers; `make budget` as a performance gate and `webc lint --all` + `webc api` as the surface gates. Every gate a tool needs has a `--json` face (`check`, `test`, `routes`, `doctor`, `api`, `lint`, `stdlib`) |
| Editor | `webc lsp`, the language server in the same binary: diagnostics, completion, hover, definition, references, workspace symbols and quick fixes, with the DSL ours and C clang's (`docs/reference.md` §14d). `editors/vscode/` is a dependency-free client for VS Code, Cursor and Antigravity; Neovim and Emacs need one `cmd` (§14e). `make lsp` is the gate. `webc explain` points the same translator at a build, so a failed compile is one sentence instead of five frames of `WC_CAT` |
| Agents | `webc docs` writes the framework's documentation into an app (`.webc/docs/`, and `webc new` does it for you), so an agent never has to find these sources to answer "what is this name"; `webc api <name>` answers that anyway, from the headers. `skills/webc/` is the same documentation as a skill: the model, the loop, the traps, and reference leaves generated from this documentation (`make skill-check` keeps them honest, and fails if any reference section has no leaf). `webc mcp` is the same machinery as seven MCP tools — the gate, the project index, the surface, the tests, a dev server and the page it serves (§14f). Every `webc new` writes an `AGENTS.md` into the app |
| Ops | structured logs, Prometheus metrics, `/debug`, a CPU sampler, explicit limits, config from env + JSON with the model as its schema |

## Layout

```
include/webc/     the framework headers (core, vec, map, model, json, react, dom,
                  fe, task, http, auth, db, log, chart, web.h, server.h) and the
                  opt-in shelf (ui, table, overlay, form, theme, keys, motion, icons,
                  richtext, …)
src/              the framework implementation
tools/webc/       the CLI
examples/         hello, todo (server + no-JS forms), accounts (auth + sessions), web (client-only wasm), js (the interop seam), stdlib (the shelf in one page), board (a kanban built from the shelf, server + live + drag), minimal (bundle floor)
tests/            the test suites (`webc test`), including the language server's
                  recorded sessions and the cross-driver parity suite
bench/            micro-benchmarks (`make bench`), socket load (`make load`)
docs/             the reference and the tutorial
website/          the documentation site, committed (`make site` builds it)
editors/vscode/   the language server's VS Code client (no dependencies, no build)
skills/webc/      the agent skill: SKILL.md by hand, references/ generated by `make skill`
assets/           webc.js (the browser bridge) and dev.js (live reload)
```

## Documentation

- [docs/README.md](docs/README.md) — the map: which file is the source of which page, and which
  command regenerates what.
- `docs/getting-started.md` — build something real in ten minutes.
- `website/` — the documentation site (`make site`, committed, served from `website/`): the
  start pages, the guides ([the ORM](website/src/content/guides/orm.md), [MySQL and
  MariaDB](website/src/content/guides/mysql.md), [the editor](website/src/content/guides/editor.md),
  [agentic coding](website/src/content/guides/agents.md)) and the reference, one page each.
- `skills/webc/SKILL.md` — for a coding agent: the five ideas, the loop, the half rules,
  the traps, and where to read. `webc docs` copies it and its leaves into an app's
  `.webc/docs/`, which is what `webc new` leaves there. `make skill` regenerates its reference leaves from the
  pages above, `make skill-check` fails when they drift.
- `docs/reference.md` — every macro, every rule, and the things C makes impossible.
  §12i is the standard library: what is on the shelf, what each module costs in bytes, and
  what it refuses to be.
  The `SPEC §n` citations throughout the source are sections of this document.

## Status

Working and tested: core runtime, collections, models (JSON/form/validation/SQL), the ORM
(filters, relations, aggregates, pages, trees), signals/DOM/SSR/hydration/patching, the
scheduler, the HTTP server, sqlite, **postgres and MySQL/MariaDB**, migrations with a twin per
dialect, auth, the CLI and its language server, the JS bridge, and the wasm target — `examples/web`
runs in a browser, with keyed lists and two-way binding driven by the same C
renderer as the server (see `docs/reference.md#13b-running-in-a-browser`).

Redis is built in the same way as postgres and MySQL — the wire protocol, by hand, no hiredis — and the
query cache on top of it is one function wide (`docs/reference.md` §9c and §9d). The client's unit
suites talk to a fake server in the test binary, so `make test` needs nothing; `make redis-test`
starts a throwaway `redis-server` and runs the live half against it, and `make bench-cache` reports
what a hit costs and what it saves.

The wasm half needs wasi-sdk (`webc doctor` says whether it found it). Each database driver
speaks the wire protocol itself — no libpq, no libmysqlclient, no hiredis — and each is tested
against a real server: `make pg-test` starts a throwaway postgres in docker,
`make mysql-test` a throwaway MySQL 8, and both run the cross-driver parity test, the driver's
own suite and the example apps, then remove the container. Both examples carry a spelling of their
migration per dialect (`0001_todos.sql`, `.postgres.sql`, `.mysql.sql`), so their suites run
unchanged on any of them (`WEBC_TEST_DB=postgres://… webc test`, or `mysql://…`). The MySQL half
has been run green on MySQL 8.4 and MariaDB 11.8, including the TLS path from a
`WEBC_TLS=openssl` build.

Every budget in the spec is a gate, not a promise: `make budget` measures SSR
throughput, JSON round-trip, the wasm bundle size, the binary size, the patch
latency and the cold build, and exits non-zero when one is missed. `make fuzz` runs
five fuzzers over the parsers for the JSON, form, router, cookie and multipart code
and falls back to a self-contained mutational driver when the toolchain has no
libFuzzer runtime.

Tests are `TEST(name) { }` blocks (`<webc/test.h>`): a file is a list of tests,
each runs inside a panic frame, and a handler is reachable without a socket through
`webc_test_request()`. A response is asserted as a *document* (`ASSERT_HTML`,
`ASSERT_HTML_TEXT`, `ASSERT_JSON` — selectors and paths, not substring scans), and the
runner starts every test from the world its declarations name, not from whatever the last
test left behind. `webc test` prints one line per test. An app's suite links
the app itself and boots it with the app's own `webc_boot()` against a scratch
database — `examples/todo` and `examples/accounts` are the worked examples.

Memory is part of the contract, not an afterthought: a request allocates only in
its arena, and `/metrics` `webc_heap_bytes` is byte-for-byte identical before and
after 130 000 SSR-with-database requests. The benchmarks print `B/op` next to
`ns/op`; that column is how the last four leaks were found.

The server runs one worker per core by default and is now measured on sockets,
not just in process: `make load` boots `examples/hello`, hammers it with
keep-alive and one-request-per-connection load, and fails on any error. On a
10-core machine that is ~18 000 req/s of SSR HTML at 16 connections and ~13 000
req/s of connection churn, with zero errors and no leaked connections or tasks.
`make pg-test` is the database equivalent: docker, a throwaway postgres, the
parity suite and both example suites, then the container is gone.

## Contributing

WebC is written and maintained by [Victor-Marian Bușoi](https://github.com/bumasoft), who reviews
every change. Contributions are welcome and are held to the gates in
[CONTRIBUTING.md](CONTRIBUTING.md): `make`, `make test`, `make lsp` and `make dx` green, a
`Signed-off-by` line on every commit (the DCO — there is no CLA), and the docs updated in the same
commit as the behaviour they describe. The [house rules](AGENTS.md) explain why each gate exists and
what the reviews here keep coming back to.

- [CONTRIBUTING.md](CONTRIBUTING.md) — how to build it, the gates, the sign-off.
- [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) — the Contributor Covenant.
- [SECURITY.md](SECURITY.md) — what is in scope, and the private channel for a vulnerability.
- [CHANGELOG.md](CHANGELOG.md) — what is in a release.
- [docs/decisions.md](docs/decisions.md) — why the licence, the name, the compiler floor and the
  version file are what they are.

The documentation site is <https://bumasoft.github.io/WebC/>, generated from `docs/reference.md`
and the sources under `website/src/` and served straight out of `website/` — which is why a
documentation change is a `make site` and a commit, not a deploy.

## Licence

Apache-2.0. The full text is in [LICENSE](LICENSE); the attribution notices, and the short list of
libraries WebC links against but does not vendor, are in [NOTICE](NOTICE). Every C source file
carries `// SPDX-License-Identifier: Apache-2.0` on its first line.

Copyright 2026 Victor-Marian Bușoi.

Contributions come in under the same licence. `sh scripts/spdx.sh` puts the header on a new file;
`sh scripts/spdx.sh --check` fails when one is missing.