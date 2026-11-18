# Working in the WebC repository

This is the framework itself, not an app. Read `docs/reference.md` (the whole language)
before changing behaviour; the `SPEC §n` citations in the source are sections of it.

## The gates

```sh
make              # libwebc.a, the webc CLI, every example
make test         # the framework's own suites, through `webc test`
make check        # test + lsp
make dx           # the developer surface: `webc lint --all` + the `webc api` budget
make budget       # SSR throughput, JSON, wasm size, binary size, patch latency, cold build
make lsp          # the language server's selftest and a replayed session
make mcp          # the MCP server's selftest (a scripted session, asserted)
make skill        # regenerate skills/webc/references from the docs
make skill-check  # fail when a generated leaf is stale, or a skill teaches a retired word
make site         # regenerate website/ from docs/ and website/src/content/
make pg-test | make mysql-test | make redis-test   # the live-server halves, in docker
make links        # every relative link and #anchor on the site resolves
make snippets     # every C sample in the docs compiles on its own
make fmt          # clang-format every source — a big-bang reformat, not a per-file touch
make fmt-check    # the ratchet: no file may be *added* to tools/fmt-baseline.txt
make fuzz         # libFuzzer over the parsers (falls back to a mutational driver)
make check-version    # VERSION, the CLI, the site, the extension and an exact tag agree
make install-test     # the installer refuses a tarball whose digest does not match
sh scripts/spdx.sh --check   # every source carries its licence header
```

A change is not done until `make`, `make test`, `make lsp` and `make dx` are green; a change that
touches docs, or a public name, is not done until the gate for it is green too — `make site` with
`git status website/` empty, `make links`, `make snippets`, `make skill-check`, and a deliberate
`api.txt` diff.

**A gate log is only evidence from where the gate is enforced.** Running `make test` on a laptop
proves the laptop. CI (`.github/workflows/ci.yml`) runs the suite on macOS and Linux; it was red for
four phases because nobody looked at the Actions tab while quoting a green terminal. Check it.

## House rules that catch everyone

- **One answer, not four.** The DSL has one scanner (`tools/webc/dsl.c`), one project
  index (`tools/webc/facts.c`) and one set of diagnostics (`tools/webc/lspx.c`). A new
  tool that needs to know what a file says uses those, not a fourth regex.
- **No new public names without a reason.** `webc api` ratchets `api.txt`; adding a name
  is a decision, and the budget exists to make you think about it.
- **A budget is a gate.** If it matters, `make` exits non-zero when it is missed.
- **The retired spellings are retired.** `webc lint --rules` is the table; the gate runs
  it over apps, examples, scaffolds and docs. `skills/webc/references/traps.md` is exempt
  because it names the words in order to forbid them.
- **Machine output is a contract.** If a command grows `--json`, a tool may depend on it:
  keep the shape, and keep it a rendering of what the command actually did.
- Comments explain *why*, in the project's voice, and never restate the code.
- **The docs are part of the change**, and `docs/README.md` is the map of which file is the source
  of what. The site is generated *and* committed: `make site`, then commit `website/`.
- **Every commit is signed off** (`git commit -s`) — the DCO, no CLA. See `CONTRIBUTING.md`.

## Layout

```
include/webc/    the framework headers, and the opt-in shelf (ui, table, overlay, …)
src/             the framework implementation
tools/webc/      the CLI: main.c, the DSL (dsl.c), the index (facts.c), the language
                 server (lsp.c, lspx.c, ceng.c), the MCP server (mcp.c), explain.c, tty.c
examples/        hello, todo, accounts, web, js, stdlib, board, minimal
tests/           `webc test` suites, the language server's recorded sessions, fuzzers
docs/            reference.md, getting-started.md, decisions.md, README.md (the map)
website/         the documentation site (committed; `make site` builds it)
skills/webc/     the agent skill: SKILL.md (by hand) + references/ (generated)
```

In an app (a project with a `webc.json`), `AGENTS.md` is written by `webc new` and says
what an agent needs there: `webc check`, the half rules, `fmt()`/`persist()`, migrations.
