The reference is the framework's own manual: sixteen sections, in the order the framework is built,
with every macro, every limit and every deliberate deviation written down. It is also the same text
that ships in the repository (`docs/reference.md`) and that `webc lint` checks for retired names.

Read it in three ways.

**As a book.** Start at [§1 House rules](../reference/house-rules.html) and keep pressing next. The
sections build: strings and arenas, collections, models, HTTP, the database, then the frontend in
three parts (signals, DOM, the bridge), then the CLI, testing, ops and the limits.

**As a lookup.** Everything is one page per section, so the browser's find is usually enough; or
press <kbd>/</kbd> and search — the index covers every heading and every paragraph.

**As the contract.** The reference states what is *implemented and covered by tests*, and says so
when something is not. Where a choice is surprising, it says what the choice costs, in the same
paragraph that makes it.

## The sections

| § | Section | What is in it |
| --- | --- | --- |
| 1 | House rules | the six rules every API obeys |
| 2 | Core | strings, arenas, errors, `fmt`, `persist` |
| 3 | Collections | `Vec`, `Map`, iteration |
| 4 | Models | field options, documents, JSON, forms |
| 5 | JSON and forms | the wire shapes both directions |
| 6 | Concurrency | green threads, channels, `select`, timers |
| 7 | HTTP | router, request, response, middleware, gzip, ranges, SSE |
| 8 | Middleware | the chain, and the ones in the box |
| 9 | Database | sqlite, postgres and MySQL, the ORM (filters, relations, pages), migrations, the query cache, Redis |
| 10 | Auth | passwords, sessions, the middleware |
| 11 | Frontend: signals | `state`, `memo`, `effect`, live bindings |
| 12 | Frontend: DOM and SSR | `el`, attributes, keyed lists, parts, components, charts, the shelf |
| 13 | The browser bridge | wasm, hydration, `js()`, the host, the boot block |
| 14 | The CLI | every command and every flag, the language server (§14d) and its VS Code client (§14e) |
| 15 | Ops | logs, metrics, `/debug`, limits, config |
| 16 | Limits and deviations | what is not implemented, and what is not going to be |

> **Reading a section that says "not implemented".** Those sentences are part of the contract. The
> framework would rather name a gap than leave you to find it in production.
