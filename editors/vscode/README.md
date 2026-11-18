# WebC for VS Code

The WebC language server, wrapped in the thinnest client that works: no dependencies
(not even `vscode-languageclient`), no build step, nothing fetched from a registry. It
spawns `webc lsp` and speaks JSON-RPC to it over stdio — the same reason the framework
has no runtime dependency is the reason this has none.

Everything it can say is the server's: diagnostics (`WebC` codes and the compiler's own),
completion, hover, go-to-definition, references, document symbols, and quick fixes.
Why the answers are shaped the way they are is `docs/reference.md` §14d; what the client
adds is §14e, and the guide
`website/src/content/guides/editor.md` is the same material for a reader setting an editor up
(it also has the Neovim and Emacs configurations).

## Run it without installing anything

```bash
code --extensionDevelopmentPath="$PWD/editors/vscode" --new-window /path/to/a/webc/project
```

(`code` is any VS Code–compatible CLI: `code`, `antigravity-ide`, `cursor`, …)

## Install it for good

Copy this folder to the editor's extension directory — `~/.vscode/extensions/` for
VS Code, `~/.antigravity-ide/extensions/` for Antigravity — as
`webc.webc-lsp-0.1.0/`, or `vsce package` it into a `.vsix`. Reload the window.

A symlink instead of a copy tracks this folder:

```bash
ln -s "$PWD/editors/vscode" ~/.antigravity-ide/extensions/webc.webc-lsp-0.1.0
```

## Settings

| setting | meaning |
|---|---|
| `webc.lsp.path` | the `webc` binary. `~`, `${workspaceFolder}`, `${userHome}` and `${env:NAME}` are expanded and a relative path is taken from the workspace folder, so `~/Documents/Projects/WebC/build/webc` works as typed. Empty: `build/webc` in the workspace or any of its five parent folders, else `webc` on the PATH |
| `webc.lsp.args` | extra arguments for `webc lsp`, e.g. `["--trace", "/tmp/lsp.jsonl"]` |
| `webc.lsp.enabled` | start the server at all |

Settings are read on every start: changing one restarts the server, and
**WebC: Restart the language server** does it by hand. No window reload.

A setting that is wrong is reported as wrong, in one sentence, naming the path it
resolved to and why that is not a program (missing, a directory, not executable). It is
never quietly replaced by some other `webc`: a binary you did not ask for is worse than
no binary.

`WebC: Show the language server log` (command palette) opens what the client and the
server said, which is also written to `$TMPDIR/webc-lsp-client.log`. The first line of
that log is the resolved binary and where it came from (setting / workspace / parent /
PATH), which answers "is my setting being used" without a debugger.

## If clangd is also installed

clangd will report its own errors on a WebC file it has no include paths for, and it is
wrong about them. Either disable clangd in WebC workspaces, or wait for
`.webc/compile_commands.json` (the server does not write one yet) and point clangd at it
with `"clangd.arguments": ["--compile-commands-dir=.webc"]`.

## What is deliberately missing

Document formatting (the house style for a view is not `clang-format`'s), inlay hints,
semantic tokens, rename and call hierarchy — each for a reason, in §14d.