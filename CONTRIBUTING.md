# Contributing to WebC

WebC is a C23 full-stack web framework: one language and one model declaration for the server and
the browser, compiled to a static binary. This is the framework's own repository — not an app built
with it — so the rules below are about changing the framework without breaking what it promises.

If you are here to *use* WebC, start at [docs/getting-started.md](docs/getting-started.md) or the
[documentation site](https://bumasoft.github.io/WebC/). If you are here to change it, read on.

## Build it

You need a C23 compiler (`clang` 21 or newer — the suite uses `#embed`), `make`, and the sqlite3
and zlib development headers. wasi-sdk is optional and only needed for the browser half; without it
`make` skips the examples that have one and says so.

```sh
make                 # libwebc.a, build/webc, the examples
make test            # the framework's own suites, through `webc test`
```

`make install` puts the built tree in `~/.local` (no sudo); `make uninstall` takes it back out.

## The gates

Nothing lands red. A pull request says which of these you ran and pastes the last lines.

| Command | What it proves |
| --- | --- |
| `make` | the library, the CLI and every example build |
| `make test` | 104 suites, in-process plus the JS and scaffold suites |
| `make dx` | `webc lint --all` and the `api.txt` surface budget |
| `make lsp` | the language server: a scripted session, then a replayed one |
| `make mcp` | the MCP server: a scripted session over `examples/todo` |
| `make check-version` | `VERSION`, the CLI, the site, the extension and an exact tag agree |
| `make install-test` | the installer's checksum check refuses a tampered tarball |
| `make site` | the committed site matches its sources (`git status website/` must be empty) |
| `make links` | every relative link and anchor on the site resolves |
| `make snippets` | every C sample in the docs compiles on its own |
| `make skill-check` | the agent skill's generated leaves are current |
| `sh scripts/spdx.sh --check` | every C source carries its licence header |
| `make fmt-check` | no new file is added to the list of unformatted sources |
| `make budget` | SSR throughput, JSON, wasm size, binary size, patch latency, cold build |

CI runs all of it on macOS and Linux (`.github/workflows/ci.yml`), plus a weekly fuzz run. `make
budget` needs wasi-sdk, so it is a local gate today.

`make check` is `make test` + `make lsp`, the two you will run most.

## House rules that catch everyone

These are the conventions that the reviews here keep coming back to. They are also in
[AGENTS.md](AGENTS.md), because an agent working in this tree needs them as much as a person does.

- **One answer, not four.** The DSL has one scanner (`tools/webc/dsl.c`), one project index
  (`tools/webc/facts.c`) and one set of diagnostics (`tools/webc/lspx.c`). A tool that needs to know
  what a file says uses those, not a fourth regex.
- **No new public names without a reason.** `webc api` ratchets [`api.txt`](api.txt): a name that
  appears there is a decision. Adding one is fine — deleting one is a break, and both should be
  argued in the pull request. If you add one, run `./build/webc api --write` and put the reason in
  the file's header, as a `# N: why` line — the tool carries those across every rewrite, so the
  reason stays next to the number it explains.
- **A budget is a gate.** If it matters, `make` exits non-zero when it is missed. Numbers that
  matter have a comment beside them saying what was measured, on what, and what the number is for.
- **The retired spellings are retired.** `webc lint --rules` is the table; the gate runs it over
  apps, examples, scaffolds and the docs. If you are adding a name, check it does not collide with
  one that was retired.
- **Machine output is a contract.** If a command grows `--json`, a tool may depend on it: keep the
  shape, and keep it a rendering of what the command actually did.
- **Comments explain *why*, in the project's voice, and never restate the code.** The best comment
  in this repository says what the alternatives were and what broke when they were tried.
- **The docs are part of the change.** Behaviour that changes without a line in
  `docs/reference.md` is a change a reader cannot find later.

## Sign your commits (DCO)

Every commit needs a `Signed-off-by` line. This is a
[Developer Certificate of Origin](https://developercertificate.org/) — one line in a commit message
that says you wrote the patch or otherwise have the right to submit it under the project's licence
(Apache-2.0). There is no CLA to sign and no paperwork to keep: the sign-off *is* the record.

```sh
git commit -s -m "the router refuses a duplicate pattern"
```

That appends:

```
Signed-off-by: Your Name <you@example.com>
```

Use a name and address you are happy to see in the permanent history — a `users.noreply.github.com`
address is fine. A pull request whose commits have no sign-off will be asked for one; `git rebase
--signoff` fixes a branch that already exists. If you are contributing on behalf of an employer,
make sure you are allowed to.

## Commits and pull requests

- One change per commit, and an **imperative subject line** that says what it does: "the router
  refuses a duplicate pattern", not "fixed routing bug". The body says *why* — what was wrong, what
  broke, what you measured. The commit messages in this repository are the design record; match
  their voice.
- A pull request should be reviewable in one sitting. If it is not, it is two pull requests.
- Say which gates you ran, and paste the output. "Tests pass" is not evidence; `webc 104 suites, 0
  failing` is.
- If you touch the docs, run `make site` and commit the regenerated `website/` — it is generated
  *and* committed, and CI fails when the two disagree. `make links` and `make snippets` are the
  other two that catch doc mistakes.
- If you touch a public name, the `api.txt` diff is part of the review. Say why the name is worth
  adding.
- If your change is a behaviour change, the pull request should say what a user would notice.

## Where the docs live

`docs/README.md` is the map: which file is the source of which page, and which command regenerates
what. The short version:

- `docs/reference.md` → the Reference section of the site, one page per `## N.` section. It is the
  whole language; the `SPEC §n` citations in the source are its sections.
- `website/src/content/*.md` → the Start pages and the Guides, hand-written for the site.
- `examples/<name>/` → the Examples pages, showing real files.
- `docs/decisions.md` → the decision record: why the licence, why the name, why the Linux port,
  why the version file. If you are about to argue that something should be different, read it first.
- `skills/webc/` → the agent skill: `SKILL.md` by hand, `references/` generated from the reference.

Editing `website/*.html` by hand is always wrong: `make site` overwrites it.

## Reporting a bug

Use the issue template; it asks for `webc doctor`, your compiler, the target, and the command that
failed, which is what a fix needs. A bug with a failing test is a bug that gets fixed — `tests/` is
where a suite goes, and `./build/webc test --filter=your_test.c` runs just yours.

**Security issues do not go in the issue tracker.** See [SECURITY.md](SECURITY.md).

## Code of conduct

By participating you agree to the [Code of Conduct](CODE_OF_CONDUCT.md). It is the Contributor
Covenant, and it applies to issues, pull requests, discussions and anything else that carries this
repository's name.

## Licence

Contributions are accepted under [Apache-2.0](LICENSE), the same licence as the project. The
sign-off above is what records that. New files need `// SPDX-License-Identifier: Apache-2.0` on
their first line — `sh scripts/spdx.sh` writes it for you.