## Where this is going

WebC is pre-1.0 and one person's framework. That is not a disclaimer — it is the reason the roadmap
below is short and the reason every item on it is something an app in this repository already
needed. There is no list of features "planned" in the abstract: a feature exists here after it has
been used, not before.

## What a version number means here

- **0.x** — the surface may change in a minor version. `api.txt` is the diff to read: it ratchets
  every public name, and `webc api` tells you what a build has that the file does not.
- **1.0** — not a date. It is the point where the reference stops needing a "may change" sentence
  in every section, which is a decision about the *language*, not about a release calendar.
- **Patches** (`0.1.x`) fix bugs and never add names.
- Nothing here is a promise that a name added today will outlive 1.0. The api budget exists so that
  adding one is a decision made in a pull request, not a side effect of a patch.

## Platforms

- **Server:** macOS (arm64 and x86_64) and Linux (arm64 and x86_64) are supported and tested on
  every push. The epoll backend is the default on Linux, kqueue on macOS.
- **Browser:** the client half compiles to WebAssembly for the four major engines. `wasm32-wasi`
  through wasi-sdk is the only toolchain it is built with today.
- **Windows:** not supported, and not a "soon". The server is built on kqueue/epoll and the runtime
  assumes a POSIX process. If that ever changes it will be a port with a CI job on it, not a claim
  in a README.
- **The compiler floor is clang 21.** The suite uses `#embed`; that is a hard requirement, not a
  preference, and it is written down in [the decisions](decisions.html).

## What is next, in the order it will probably happen

1. **1.0 — the stability pass.** The `SPEC §n` citations become stable section numbers, the reference
   drops its hedges, and `api.txt` freezes for a major version.
2. **Postgres and MySQL are already the operative databases** — the open work is not "add support",
   it is refining what the ORM emits for each dialect (index hints, `ON CONFLICT` variants,
   generated columns), driven by the parity suite.
3. **More of the client half in C.** The JS seam (`js()`) exists for the things the browser owns;
   every one of those that turns out to be expressible in the framework's own vocabulary moves out
   of the seam.
4. **The editor story.** The language server is real today; the next step is the same diagnostics
   inside `webc build` output so an editor without LSP still sees them.

## What it will never do

- **No npm package, no `node_modules`, no bundler.** The browser half is a wasm module built by the
  same compiler that built the server. [D2](decisions.html) says why, and no amount of ecosystem
  pressure changes the reasoning.
- **No garbage collector.** Arenas are the memory model, and every request is a scope.
- **No ORM that hides SQL.** `db_exec` and `db_all` are there, first-class, next to the typed layer.
- **No configuration file format of its own.** `webc.json` is JSON; there is no `webc.yaml` coming.

## How to argue with this page

Open an issue. A roadmap that cannot be argued with is a marketing document, and this one is meant
to be a plan — the reasoning is in the [decision record](decisions.html), and a case made there has
moved this list before.