WebC is a framework for web applications written in C23. Not a binding, not a wrapper around
another runtime: the route table, the model layer, the template language and the browser half are
all C, compiled by the same compiler, checked by the same type checker.

It exists because one language all the way down turns out to be worth a lot: a field you add to a
model is a column, a form input, a JSON key and a struct member at the same time, and none of those
four can drift from the other three without the compiler saying so.

## The bet

Three decisions shape everything else in the box.

**Memory is a scope, not a collector.** The framework installs an arena per scope — the process, the
request, the render — and resets it in O(1). An app says `fmt()` for a string it needs now and
`persist()` for one that must outlive the request. There is no GC, no reference counting, and no
hidden copy. When you read an allocation in the source, you are looking at every allocation the
program makes.

**One set of views, two halves.** `src/view.c` is compiled into the server binary and into a wasm
module. The server renders it to HTML; the module hydrates that exact markup and takes over. There is
no separate client template to keep in sync, no serialisation format to design, and no façade to
write: the boundary is a file listing in `webc.json`.

**Every surface is counted.** `webc api` counts the public names and fails when the list grows
without a note in the changelog. `webc lint` fails when a name that belongs to the framework leaks
into an app, an example, a scaffold or these docs. `make budget` fails when the wasm or the binary
grows past its number. The gates run against this repository first, so they are not advice.

## What is in the box

| Area | What you get |
| --- | --- |
| Language | C23: no preprocessor tricks beyond macros, no GC, no hidden allocation |
| Errors | `WErr` / `WErrRes` with `try` / `tryv` / `errdefer`; a panic becomes a 500 at the boundary |
| Concurrency | M:N green threads on kqueue, channels, `select`, timers, mutexes, wait groups |
| HTTP | typed router, typed request and response, middleware chain, gzip on by default, byte ranges, SSE, websockets, graceful drain; the page's head spliced on the way out, with authored chrome from `partials/` and structured data as one expression |
| Data | model-first SQL over sqlite, postgres and MySQL/MariaDB — each a hand-written wire protocol, each tested against a real server; migrations with a twin per dialect, enums, documents, and a query cache keyed by generation |
| The ORM | typed filters (`WWhere`), aggregates, `db_page`, and relations declared once — `prefetch`/`ref`/`refs` read a whole page's relations in one statement per collection, `db_save_all` fills the foreign keys. A view never queries, and `db_statement_count()` proves it |
| Redis | RESP3 over parked sockets — strings, hashes, lists, sets, sorted sets, expiry, counters, pipelines, pub/sub |
| Frontend | signals, memo, effects, a DOM builder with node reuse, keyed lists, parts, component instances, live bindings, SSR and hydration, charts |
| Browser | the same views as wasm32-wasip1, `js()` for the seam, `WebC.call` / `WebC.dispatch` for the other direction |
| Standard library | 21 opt-in headers, from `table` to `richtext`, taken one `#include` at a time |
| Tooling | `webc new/build/dev/serve/test/fmt/migrate/doctor/routes/size/deploy/check/lint/api/stdlib/add`, and `webc lsp` — the language server, in the same binary, with a dependency-free VS Code client in `editors/vscode/` |
| Ops | structured logs, Prometheus metrics, `/debug`, a CPU sampler, config from env and JSON with the model as its schema |

## How a request runs

1. The server accepts on a green thread. A request gets an arena; the router matches a *table* the
   compiler generated from your `GET` / `POST` macros.
2. Middleware runs as a chain of plain functions. A handler returns a `Response` value — it does not
   write a socket.
3. Rendering a view fills a second arena for the render. `page(View, ctx)` renders into
   `public/index.html` if it exists, writing `data-wc` anchors so the client can adopt the markup
   later without rebuilding it.
4. The request's arena is reset in one instruction when the response is written.

In the browser, the same view functions run against a build of the DOM tree, which the bridge diffs
into operations. A render follows every action and every host callback; there is no `tick` to bump.

## Where to start

- **[Install](install.html)** — a compiler, the repository, and `webc doctor`.
- **[Quickstart](quickstart.html)** — from nothing to a running app with a database in ten minutes.
- **[Mental model](mental-model.html)** — the three decisions above, spelled out with code.
- **[The reference](../reference/index.html)** — every macro, every limit, in order.
- **[The ORM](../guides/orm.html)** — filters, relations, pages and trees, and what each costs.
- **[The editor](../guides/editor.html)** — `webc lsp`, the VS Code extension, and any other
  editor that speaks LSP.
- **[Agentic coding](../guides/agents.html)** — the `AGENTS.md` every scaffold writes, the skill
  generated from this documentation, and `webc mcp`.

> **Who it is for.** People who like C, who want one language across the whole app, and who would
> rather be told by a compiler than by a code review. **Who it is not for:** anything that needs to
> run on a platform without a C toolchain, or a team that would rather not think about lifetimes at
> all. The framework hides the arenas; it does not pretend they are not there.
