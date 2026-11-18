Two doors, both one command. The first is the one a curious developer meets — a prebuilt CLI
and framework tree, no compiler needed until the first `webc dev`:

```sh
curl -fsSL https://raw.githubusercontent.com/bumasoft/WebC/main/scripts/install.sh | sh
# add --wasi for the browser half, --ref vX.Y.Z to pin a release
```

That lays out `~/.local/share/webc/<version>/` (the CLI, `libwebc.a`, the headers, the bridge,
the skills, the docs) and symlinks `~/.local/bin/webc` into it — and the CLI finds its own tree
through the symlink, so nothing is baked in and the tree can move. `WEBC_HOME` points it at a
checkout instead, any time.

There is no Homebrew tap yet, and no half-filled formula pretending there is one: `brew` users
get the same `curl | sh` line, and the tarball it unpacks is the one a formula would pour.

The second door is the checkout — for developing on the framework itself, or when you prefer the
build. WebC is a C23 project with one external library (sqlite3) and one optional toolchain
(wasi-sdk, for the browser half). Nothing else is installed, vendored or fetched at build time.

## What you need

| Piece | Why | Test |
| --- | --- | --- |
| A C23 compiler | `nullptr`, `__VA_OPT__`, `[[maybe_unused]]`, `#embed` | `clang --version` — 21 or newer, which is where `#embed` lands and what the suite needs. `gcc --version` — 15 or newer takes `-std=c23`, but the suite is not run against it |
| `make` | builds the library, the CLI and the examples | `make --version` |
| sqlite3 | the default database | `pkg-config --exists sqlite3` |
| wasi-sdk | only for an app with a browser half | `ls ~/.local/share/wasi-sdk-*` |
| node | only for the bridge and check test suites | `node --version` |

macOS (Apple Silicon and Intel) and Linux (x86_64 and aarch64) both build and pass `make test` in
CI; the prebuilt artefacts are macOS today and Linux builds from the checkout. The scheduler uses
kqueue on macOS and epoll on Linux, and the Linux leg is what exercises the epoll backend. Windows
is not supported.

## Build the framework

```sh
git clone https://github.com/bumasoft/WebC.git
cd WebC
make                 # libwebc.a, build/webc, the examples, the tests
make test            # the whole suite
make install         # the same tree install.sh lays out, into ~/.local
```

`make install` needs no sudo: it writes `~/.local/share/webc/<version>/` (binary, library, headers,
assets, skills, docs, the lessons) and symlinks `~/.local/bin/webc` into it — the shape the CLI
resolves itself through. `make install PREFIX=/usr/local` for a system install, `make uninstall`
to take it back out.

`make` produces three things worth knowing about:

- `build/libwebc.a` — the framework, one static library;
- `build/webc` — the CLI, which is also how you build an app;
- `build/include/` — the generated headers (`webc.json` schemas, wasm glue).

Put the CLI on your path:

```sh
export PATH="$PWD/build:$PATH"      # or add it to ~/.zshrc
```

## Check the install

```console
$ webc doctor
webc 0.1.0 (framework at /Users/you/WebC)
  compiler   Apple clang version 21.0.0
  libc23     ok
  sqlite3    ok
  wasi-sdk   ok (/Users/you/.local/share/wasi-sdk-34.0-arm64-macos)
  node       ok
  library    ok (/Users/you/WebC/build/libwebc.a)
  project    no webc.json here (framework checkout, not an app)
```

`doctor` checks the compiler actually accepts the dialect, that the library is built, and where the
optional pieces are. If `wasi-sdk` says *not found*, a server-only app still builds and runs — only
`--template=web` and `--template=fullstack` need it.

## Optional services

The defaults are deliberately boring: a file database in `data/`, no cache, no queue.

```sh
DATABASE_URL=postgres://user:pass@localhost/app    # postgres, no libpq
DATABASE_URL=mysql://user:pass@localhost/app       # MySQL or MariaDB, no libmysqlclient
REDIS_URL=redis://localhost:6379                   # the query cache and pub/sub
WEBC_QUERY_CACHE=1                                 # cache result sets in Redis
WEBC_SECRET=$(openssl rand -hex 32)                # sign sessions
```

There is no separate server to install for sqlite — it is linked in. Postgres, MySQL and Redis
speak their wire protocols directly, so `libpq`, `libmysqlclient` and `hiredis` are not
dependencies either.

## The editor

`webc lsp` is the language server, and it is the binary you just built — no Node, no second
download. In VS Code (or Cursor, or Antigravity) run it out of the tree once:

```sh
code --extensionDevelopmentPath="$PWD/editors/vscode" --new-window ~/my-app
```

[A whole guide](../guides/editor.html) covers installing it for good, Neovim and Emacs, the three
settings, and living beside clangd.

## Next

- **[Quickstart](quickstart.html)** — scaffold, run, change, test.
- **[The API boundary](../guides/api.html)** — the OpenAPI document and the typed client, generated.
- **[The ORM](../guides/orm.html)** — the query API a model gives you.
- **[Project layout](layout.html)** — what `webc new` wrote and why.
- **`webc learn`** — five lessons, one idea each; a lesson's own gate fails until the edit lands.
- **[Coming from React, Solid and Next](../guides/rosetta.html)** — the dictionary, if those are the names you know.
- **[Agentic coding](../guides/agents.html)** — if an agent will be doing the writing, start here.
