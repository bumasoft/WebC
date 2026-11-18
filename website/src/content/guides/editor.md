`webc lsp` is the language server, and it ships in the binary you already have: no Node, no second
download, no build step. The VS Code extension in `editors/vscode/` is a thin client for it — no
dependencies, nothing fetched from a registry — and every other editor talks to the same command over
stdio.

What it is for is the seam between WebC and C. `GET(…)`, `view(…)`, `el(…)` and the model tuples are
macros, so the compiler's answer to a typo is `use of undeclared identifier 'view_Nope'` behind five
frames of `WC_CAT`. The server answers "no view named `Nope` in this project — `view(Nope)` draws it,
or `declare_view(Nope)` in the shared header announces it", and offers the line to add.

## What it knows

| LSP capability | what you get |
| --- | --- |
| `publishDiagnostics` | WebC's own errors (a route in the wrong half, an unknown view or model field, a stdlib module in the wrong file set) and the compiler's, with the DSL's bytes left to the DSL engine |
| completion | inside a `WEBC_MODEL` tuple: field kinds, models, enums and every `.qualifier`; inside `el(…)`: HTML tags; inside `page(…)`/`mount(…)`: the project's views; inside `#include <webc/…>`: the shelf, filtered by the half this file compiles for |
| hover | a macro's doc line, its shape and **which half** it belongs to; a model's fields and the file it is declared in; a view's signature and where it is drawn |
| go-to-definition | the model, the view, the action or the route handler the name refers to |
| references | every place that name is used |
| document and workspace symbols | the outline: models, views, actions, routes |
| code actions (`quickfix`) | two fixes, the ones a person actually makes: close an unclosed block, and add `declare_view(Name)` to the shared header |

The diagnostics have codes, so a rule in `webc lint` and an underline in the editor are the same
sentence:

| code | what it says |
| --- | --- |
| `no-view` | `page(Nope)` names a view nothing draws — with the fix that adds `declare_view(Nope)` to the header both halves include |
| `unclosed`, `unclosed-block` | `el("li") {` never closed: one error, not thirty, and the fix closes it |
| `no-vec` | `db_all(Todo)` without a `Vec(Todo)` — the diagnostic names the line to add |
| `not-a-model` | a `db_*(…)` call whose first argument is not a `WEBC_MODEL` type |
| `no-action` | `on_click(name)` with no `action(name)` to bind |
| `half-module` | a stdlib header that may not appear in this half (`webc/mail.h` in a shared file) |

It knows all of that from the sources — including models declared in shared headers, which no
`webc.json` lists — and never reads `.webc/`, so a fresh clone is a working workspace.

Two things it deliberately does **not** do:

- **Format a document.** `clang-format` rewrites `el("h1") { text("Todos"); }` into four lines, which
  is not this language's style. Whole-document formatting is refused with that reason; range
  formatting outside a DSL block is what `webc lsp` offers instead. `webc fmt` exists, but it is
  clang-format over the whole tree — point it at a project whose DSL blocks you have already
  formatted by hand, or expect it to disagree with you.
- **Second-guess the compiler.** A diagnostic inside a DSL macro's own parentheses is dropped: those
  bytes are the WebC engine's to explain. A block that never closed stops the compiler half from
  running at all, so an unbalanced brace is one error instead of thirty.

Positions are UTF-16 (as the protocol requires) and every save sends the whole buffer: a WebC file is
small, and a full text sync cannot desynchronise from the editor's copy.

## In VS Code, Cursor, Antigravity, …

```sh
# run it from this repository, without installing anything
code --extensionDevelopmentPath="$PWD/editors/vscode" --new-window /path/to/a/webc/project
```

`code` is any VS Code–compatible CLI. To install it for good, copy the folder into the editor's
extension directory as `webc.webc-lsp-0.1.0/` (`~/.vscode/extensions/` for VS Code,
`~/.antigravity-ide/extensions/` for Antigravity), or `vsce package` it into a `.vsix`. A symlink
rather than a copy tracks the folder:

```sh
ln -s "$PWD/editors/vscode" ~/.vscode/extensions/webc.webc-lsp-0.1.0
```

Three settings, and two commands:

| setting | meaning |
| --- | --- |
| `webc.lsp.path` | the `webc` binary. `~`, `${workspaceFolder}`, `${userHome}` and `${env:NAME}` are expanded and a relative path is taken from the workspace folder. Empty: `build/webc` in the workspace or a parent, else `webc` on the `PATH` |
| `webc.lsp.args` | extra arguments, e.g. `["--trace", "/tmp/lsp.jsonl"]` |
| `webc.lsp.enabled` | start the server at all (default `true`) |

| command | what it does |
| --- | --- |
| `WebC: Show the language server log` | the channel, including every diagnostic the server sent |
| `WebC: Restart the language server` | respawn without reloading the window |

Changing a setting restarts the client in place — a setting that is read once at activation and never
again is the reason "I set the correct path and it still fails" exists. When the binary cannot start,
there is **one** message that says which of the three it is (not found, not executable, exited) with
the resolved path and the last lines of the server's stderr, because "set `webc.lsp.path`" shown to
someone who already did is the fastest way to teach them their setting is ignored.

## Any other editor

The command is `webc lsp --root <project>`; everything else is the editor's LSP client.

```lua
-- ~/.config/nvim/lua/webc.lua — Neovim 0.8+, the built-in client
vim.api.nvim_create_autocmd({ "BufRead", "BufNewFile" }, {
  pattern = { "*.c", "*.h" },
  callback = function()
    if vim.fn.filereadable(vim.fn.getcwd() .. "/webc.json") == 1 then
      vim.lsp.start({ name = "webc", cmd = { "webc", "lsp", "--root", vim.fn.getcwd() } })
    end
  end,
})
```

```elisp
;; ~/.emacs.d/init.el — eglot, which ships with Emacs 29
(add-to-list 'eglot-server-programs
             '((c-mode c-ts-mode) . ("webc" "lsp" "--root" ".")))
```

Anything that can spawn a process and speak LSP works the same way: Helix, Zed, Sublime, Emacs with
`lsp-mode`. The server announces its capabilities at `initialize`, so an editor that offers fewer
features simply uses fewer.

## Living beside clangd

C editors have their own language server, and it is clangd. `webc lsp` writes
`compile_commands.json` at the project root — the flags `webc build` uses, one entry per source — which
clangd looks for without being told anything. That file is why clangd agrees with `webc build` about
include paths instead of inventing errors: measured in this repository, twenty-one invented errors
became none. `WEBC_LSP_NO_COMPILE_COMMANDS=1` turns it off for a project that manages its own.

Run both if you like. clangd is right about C and has no idea what `el("li") { … }` means; the WebC
server is right about the DSL and asks the compiler about the C.

## When it looks broken

| symptom | the cause |
| --- | --- |
| "no diagnostics at all" | `webc.lsp.enabled` is false, or the workspace has no `webc.json` — the server needs a project, not a folder |
| the extension's message names a path you did not type | `webc.lsp.path` was wrong and `~`/`${…}` were expanded; the message shows the *resolved* path |
| completions for models are empty | the models are declared in a file the project does not list — put them in the shared header, where both halves can see them |
| clangd floods the file with errors | `compile_commands.json` is missing or stale; open a `.c` file and save it, and the server rewrites it |
| a diagnostic stays after the fix | save the file: the compiler half runs on save when a check costs more than 120 ms |

`--trace FILE` writes every message the server saw and sent as JSONL, which is the fastest way to
report a problem with the editor integration rather than with your code.

## The gate

`make lsp` is what keeps the two halves honest:

```sh
make lsp        # --selftest, then a replayed session, then the client's resolver tests
webc lsp --bench                        # what one request costs
webc lsp --replay tests/lsp/sessions/todo.jsonl --record   # re-record after a wire change
```

`--selftest` asserts what the server says about a real project (diagnostics, a hover, a definition, a
quick fix) and `--replay` diffs the whole protocol against a recorded session, so a change to the wire
is a diff in review rather than a bug report. `--bench` exists because a language server that stutters
is a language server people turn off.

The client's own test is `editors/vscode/resolve.test.js` (no dependencies, run by `make lsp` when
`node` is present): it checks the resolver against every way a path can be wrong — missing, a
directory, not executable, relative, on no `PATH` — because a client that silently ignores the setting
is a broken window.

## See also

- Reference [§14d The language server](../reference/the-cli.html) — the design, in reference voice.
- [Agentic coding](../guides/agents.html) — the same diagnostics as data, through `webc mcp`.
- Reference [§12](../reference/the-cli.html) — why there are two engines with one line between
  them.
- [Testing](../guides/testing.html) — `webc test`, which is the other half of not guessing.