WebC is unusually good for a coding agent to work in, and most of that is not on purpose — it falls
out of decisions taken for other reasons. A model is one declaration, so a change has one place to
start and four places it lands. The halves are a file list, not a preprocessor test. `webc check` is
one command with one exit code. And the framework has already retired 44 spellings because they were
the second way to do something, which means the wrong shape is *refused* rather than merely not
recommended.

What was missing was the part between the agent and the compiler: a vocabulary, a gate it can call,
and a way to ask the project a question without grepping. Three things now supply that, and this page
is about giving them to an agent and what it gets.

## Three things, cheapest first

| Give it | What it carries | Who reads it | Cost |
| --- | --- | --- | --- |
| `AGENTS.md` + `.webc/docs/` | `webc check` as the gate, the half rules, `fmt()`/`persist()`, migrations, "believe the lint" — and the documentation itself, in the app | any tool that reads `AGENTS.md` — which is most of them, with no setup | a file `webc new` already wrote |
| `skills/webc/` | the five ideas, the loop, recipes, the traps, and fourteen reference leaves generated from this documentation | harnesses that read a skills folder — Claude Code, pi, and the ones that follow the same layout | one symlink |
| `webc mcp` | the project index, the surface, the gate, the tests, and a dev server it can read | clients that speak the Model Context Protocol | one block of JSON in a config file |

They are layers, not alternatives. The `AGENTS.md` is what an agent reads before it writes anything;
the skill is what it thinks in; the MCP server is what it asks when the first two are not enough. An
agent with a shell and only the skill can do the whole job — the MCP server removes the greps, not
the need to think.

## 1. `AGENTS.md`, which the scaffold writes

Every `webc new` writes one into the app, next to `webc.json`. Eight lines, and each one is a mistake
an agent makes by default:

```markdown
# Working in a WebC app

C23, one model declaration for the server and the browser. Verify with
`webc check` (build + tests + schema + lint); it exits non-zero. Run it after
every change — a failed build comes back in WebC's words, not the compiler's.

- A `shared` source (see webc.json) reaches neither the database nor the
  request: `db_*`, `req`, `path*`, `query`, `body` are server-half names. Put
  them in a `server` source and pass the values in through the view's context.
- One `WEBC_MODEL(...)` gives the struct, the validation, the JSON and the SQL.
  Never write a JSON body, a form parser or a SQL statement by hand.
- No arena, no allocator in app code: `fmt()` builds into the current scope,
  `persist()` is the one escape hatch for a value that outlives it.
- Add a column as a file in `migrations/`; the server applies it at boot.
- Authored HTML that is not interpolated is a file under `partials/`, included by
  `partial("name")` — head chrome is a file edit, not a C string of escaped quotes, and
  `webc check` verifies every name has its file.
- Structured data is `head_ld(ld_blog_posting(...))` from `<webc/ld.h>`; `webc lint` refuses
  the hand-built version, and a field you leave out is omitted, never written empty.
- `webc lint` names each retired spelling and what to write instead. Believe
  it: the framework refuses the C you are about to write on purpose.
- Code you own, generated from the app's own models: `webc scaffold Post` writes the
  admin CRUD, `webc scaffold live Post` the SSE room and its broadcasting mutations,
  with tests. Edit what they wrote; they never rewrite the model.
- The API boundary is generated too: `webc openapi` writes OpenAPI 3.1 and
  `webc client --ts` a typed fetch client, from the routes and models. If the app
  ships `openapi.json` or `webc.client.ts`, keep them current — `--check` (and
  `webc ci`) fails when they drift.
- Deploying: `webc deploy --pack docker,fly,systemd,caddy,nginx` writes the ops files
  (SSE-aware) beside the binary; `webc ci` writes the workflow that runs this file's
  gate.
- The full surface is `webc api`; the rules are `docs/reference.md` in the
  framework checkout; `webc explain` translates a build failure.
```

This is the highest value per line of anything on this page. A skill has to be installed and a server
has to be configured; `AGENTS.md` is read because it is there, in the one place every harness looks,
and it answers the questions an agent would otherwise answer by guessing.

### The documentation goes into the app

`webc new` also writes the framework's documentation into `.webc/docs/` — `SKILL.md` plus one
reference per topic — and the last lines of `AGENTS.md` say so. That is deliberate: an agent working
in an app should not have to find the framework's sources to answer "what does this name do", and
the one session that taught us this was spent grepping a checkout for `auth_hash_password` after
deciding the auth API was undocumented.

```sh
webc docs              # (re)write .webc/docs/ from this toolchain
webc docs --list       # the topics: models, http, auth, concurrency, stdlib, traps, …
webc docs auth         # print one, without opening a pager
```

`.webc/` is build output, so the docs are gitignored: the toolchain owns them, `webc docs` rewrites
them, and a fresh clone gets them back with the one command `AGENTS.md` names. Nothing is vendored
and nothing goes stale — the leaves are generated from the same reference the toolchain was built
from.

When a name is still not in a leaf, the answer is one command, not a search:

```sh
webc api auth_verify     # the declaration, its header, and the line above it
```

The repository has its own `AGENTS.md` at the root, for a different reader: the gates (`make`,
`make test`, `make lsp`, `make mcp`, `make dx`, `make skill-check`), the house rules (one scanner,
one index, one set of diagnostics; the api ratchet; a budget is a gate; machine output is a
contract), and the layout. If you are having an agent change the framework rather than an app, that
is the file that matters.

## 2. The skill

`skills/webc/` is a skill in the shape harnesses expect: `SKILL.md` (a router, written by hand) and
`references/` (fourteen leaves, generated; one per reference section).

`SKILL.md` carries the part that cannot be generated — what to do. The five ideas that make the rest
of the framework a consequence rather than a list of rules:

1. a string belongs to a scope — the framework installs the arena, an app writes `fmt()` or
   `persist()`;
2. a response is a value — a handler returns one, which is why tests need no socket;
3. a render follows an action — one view, both targets, one context type;
4. the halves are enforced by the compiler — `server` / `shared` / `client` are file sets, and a
   name that is not declared in your half is not available;
5. a model is the schema — struct, descriptor, validation, JSON, form and SQL column types from one
   tuple.

Then the loop, the shape of a project, five recipes (add a field, add a page, add an endpoint, read a
request, make a button act), the rules that bite, and a routing table: which leaf to read for which
task.

### Why the leaves are generated

A hand-written skill teaches whatever was true the day it was written. This framework retires
spellings — `webc lint --rules` is the table — and a skill still teaching a retired call is a
language nobody can compile, delivered with confidence.

So `tools/gen_skill.py` builds the fourteen leaves from pages that are already maintained:
`website/src/content/guides/*.md`, anchored sections of `docs/reference.md`, and `webc lint --rules`
for the traps table. Each leaf opens with a line-number index, so a reader loads a span rather than a
file. `make skill` regenerates; `make skill-check` is the gate, and it fails on three things:

- a leaf that would change — the documentation moved and the skill did not;
- a retired spelling anywhere in the skill's own prose — the gate the framework applies to apps,
  applied to the thing that teaches them;
- a C block in `SKILL.md` that does not compile, or that has an ellipsis in it pretending to be a
  file.

That third gate is `website/tools/snippets.py`, the same one that checks the site:

```sh
make skill        # write skills/webc/references/ from the docs
make skill-check  # stale leaf, retired spelling, uncompilable snippet
```

It is worth saying that the gate works, because it is not a formality: it failed twice while this
page was being written — once on a documentation edit that had not been propagated into the leaves,
and once because `SKILL.md` named a retired target macro in prose in order to say *don't*. Both are
exactly the failure the gate exists to make impossible to ship.

### Installing it

The repository is the source of truth; install it by pointing at it.

```sh
git clone https://github.com/bumasoft/WebC && cd WebC && make
ln -s "$PWD/skills/webc" ~/.agents/skills/webc
```

A symlink, not a copy, so `make skill` updates the installed skill in the same breath. Harnesses that
keep skills elsewhere want the same directory — `skills/webc/` is plain Markdown with YAML front
matter (`name`, `description`) and needs no build step to be read.

If your tool does not read a skills folder at all, it probably reads `AGENTS.md`. Add one line to the
app you are working in — the scaffold's file already ends with the pointer:

```markdown
For the framework's vocabulary and traps, read `skills/webc/SKILL.md` in the WebC checkout.
```

### What is in the leaves

| Leaf | Read it when the task is |
| --- | --- |
| `core-and-strings.md` | a string, `fmt()`, a scope, an error, a `Vec` or `Map` |
| `models-and-orm.md` | a field, a table, a filter, a relation, a page of rows |
| `http-and-routes.md` | a route, a response, a cookie, SSE, middleware, a file |
| `views-and-signals.md` | markup, a signal, an effect, a part, a component |
| `client-and-wasm.md` | a button that acts, a live value, calling JavaScript |
| `data-and-sql.md` | Postgres, MySQL, a migration per dialect, Redis, the query cache |
| `forms.md` | a form, validation, the error under a field |
| `auth.md` | a password, a login, a session cookie, a guarded page |
| `concurrency.md` | a background task, a channel, `select`, a timer, a mutex |
| `stdlib.md` | choosing a module, and what it costs in bytes |
| `testing.md` | a test, an assertion, a fixture |
| `ops-and-deploy.md` | logs, metrics, limits, a deploy |
| `cli.md` | a `webc` command, the language server, the MCP server |
| `traps.md` | "why does the compiler say that" — the retired spellings, as a generated table |

Fourteen leaves for seventeen sections, and that is checked rather than intended: every top-level
section of `docs/reference.md` must be quoted by some leaf, and `make skill-check` fails naming the
section when one is not. The gate exists because auth was missing from the eleven-leaf version —
the coverage was an opinion until it became a build error.

## 3. The MCP server

`webc mcp` speaks the Model Context Protocol over stdio from the same binary as everything else: no
Node, no second download, no version skew between the server that answers and the compiler it answers
about. The design of it — and the seven tools in detail — is [reference §14f](../reference/the-cli.html);
what matters here is when it earns its place.

A shell already gives an agent `webc check` and `grep`. Four things it does not:

| What the agent needs | What the shell costs | What the tool returns |
| --- | --- | --- |
| the gate, as data | prose to parse, per run | `{ok, steps:[{name, ok, output}]}` |
| the project's own index | a grep that cannot tell a half from a comment | models, fields, views, actions, routes, and each file's half, with positions |
| the whole public surface | `grep api.txt`, guessing at spellings | a match's declaration, header and doc line |
| the page it just built | two processes, `curl`, a port to remember | a dev server held across calls, and a fetch from it |

Point a client at it:

```json
{
  "mcpServers": {
    "webc": {
      "command": "/absolute/path/to/WebC/build/webc",
      "args": ["mcp", "--root", "/absolute/path/to/the/app"]
    }
  }
}
```

`--root` defaults to the working directory, so a client that starts in the project can leave it out.
`webc mcp --selftest` runs a scripted session against `examples/todo` and asserts what the tools
answer, which is how you tell a protocol problem from a configuration problem before you blame the
editor. `make mcp` runs the same thing in CI.

Two behaviours worth knowing: the dev server's own output goes to `.webc/mcp-dev.log` (stdout is the
protocol channel, and one stray banner corrupts every message after it), and `webc_dev` refuses to
start over a build that does not compile — a server that quietly serves the last good binary is the
most confusing possible way to learn that your change is broken.

## The loop, end to end

The task: add a `due_on` field, with the migration, the filter and the test that go with it. This
is the shape every change takes, and none of it is specific to the task.

```sh
webc check                     # green first: what green looks like is a fact, not a guess
```

Edit `WEBC_MODEL` in `src/app.h` — the struct, the validation, the JSON and the form move together:

```c
// src/app.h — one field: the page, the API, the form and the SQL all follow
#include <webc/shared.h>

WEBC_MODEL(Note, (id, i64, .pk = true),
                 (title, Text, .required = true, .max = 200),
                 (due_on, Text))
Vec(Note)
```

Then a migration file, then the view, then the route, then the test — and after each one:

```sh
webc check
```

```
  build          ok (./.webc/notes, .webc/app.wasm)
  tests          ok
  doctor         ok
  boot           ok
  schema         ok
  routes         5 declared
  lint           ok
  consistency    ok

  all clear — next: webc dev
```

The `schema` step is the interesting one: it boots the app with a schema check, so a migration
file that does not match the model fails there, before the page is ever drawn.

When it is not green, the failure is a sentence about the mistake and not a transcript of the macro
expansion. Two real ones:

```
  ./src/view.c:5:27
    `db_all()` is a server-half macro: this file is in the shared set, which compiles for
    the browser too. Move the call into a `server` source and pass the data in through the
    view's context
```

```
  ./src/server.c:16:10
    a JSON body written by hand — `json(200, s)` takes a `str`: pass the model
    (`json(200, Note, &t)`), the collection (`json_list(200, notes)`), or the bytes as
    `str_lit("...")`. A JSON string literal is never what this framework asks an app to
    write
```

Note what the second one does: it names *this project's* model, and it gives the three answers that
are actually valid. That is the whole difference between a gate an agent can act on and a gate it has
to interpret.

For a machine, the same run:

```sh
webc check --json      # {"ok":true, "name":"notes", "steps":[{"name":"build","ok":true}, …]}
webc test --json       # {"ok":true, "suites":1, "failing":0, "tests":[{"suite":"…","passed":9,…}]}
```

## Make the gate automatic

An agent that runs the gate after every change is an agent that never carries two mistakes
forward. Two ways to make that not depend on the agent remembering:

```sh
#!/bin/sh
# .git/hooks/pre-commit — the gate CI runs, before the commit exists.
# The quiet run answers the question; the loud one explains a no.
webc check --json > /dev/null || webc check
```

```json
{
  "hooks": {
    "PostToolUse": [
      { "matcher": "Edit|Write",
        "hooks": [{ "type": "command", "command": "webc check --json" }] }
    ]
  }
}
```

The second is a Claude Code hook, and most harnesses that do not call it that have the
equivalent; what they all need is a command whose exit code is the answer and whose output is
short. That is why `--json` exists: a passing run is one line, and a failing one is the failing
step's own output and nothing else — the whole thing is a few hundred bytes, which is the
difference between a gate the agent can afford to run every turn and one it learns to skip.

Two rules of hygiene for the agent, both already in the scaffold: `.webc/` is build output (it
is gitignored, and reading it is how an agent confuses a stale artifact for a fact), and
`compile_commands.json` exists for clangd — `webc check` is the answer to "does it compile",
not an inference from either of them.

## What agents get wrong here, and what stops it

Every row is a real mistake, and the middle column is the gate that catches it — not a style rule,
a refusal.

| The mistake | Caught by | What it says |
| --- | --- | --- |
| a JSON body as a string literal | `webc explain`, `webc lint` | pass the model, the collection, or `str_lit("…")` |
| a `db_*` call in a `shared` file | the compiler, then `webc explain` | a server-half macro; move it and pass the data through the context |
| a route in a `shared` file | the same | routes belong in a `server` source |
| an arena, or an allocator, in app code | `webc lint` | `fmt()`, and `persist()` for what outlives the scope |
| a target test in a source file | `webc lint` | the file set is the target test: list the file under `"client"` |
| a `main()` | the framework | `client_main { … }` starts the browser half |
| an ignored parameter `(void)(ctx)` | `webc lint` | write the signature the view needs |
| a hand-indexed vector | `webc lint` | `for_each(item, vec)` |
| `%.*s` on a `Text` field | `webc lint` | a `Text` is already a C string |
| an unclosed `el()` or macro block | the language server, `webc explain` | one error, not thirty, and the fix to close it |
| a `page(Nope)` naming a view nothing draws | `webc explain` | `view(Nope, …)`, or `declare_view(Nope)` in the header |

The pattern is the point: the framework decided long ago that a second way to do something is a cost
paid by every reader forever, and an agent is a reader with no memory of the first way.

## Machine output is a contract

Anything a tool may depend on has a `--json` face, and the face is a rendering of what the command
did — not a second implementation.

| Command | Object |
| --- | --- |
| `webc check` | `{ok, name, steps:[{name, ok, skipped, artifact?, routes?, output}]}` |
| `webc test` | `{ok, suites, failing, tests:[{suite, passed, failed, skipped, tests:[…], output}]}` |
| `webc routes` | every route with method, path, file, line and the view it returns |
| `webc doctor` | `{ok, checks:[{name, ok, detail, fix}]}` |
| `webc api` | the public surface, counted |
| `webc stdlib` | the twenty modules (`--used` to filter) |
| `webc lint` | `{hits:[{file, line, token, what, instead}], total, wrong_half, fail}` |
| `webc size` | `{built, wasm{raw, gz}, bridge{raw, gz}, wire_gz, budget, sections[…]}` |
| `webc explain` | the diagnosis, as data |

The terminal side is a contract too. Colour is off when stdout is not a terminal, when `NO_COLOR` is
set, and when `TERM=dumb` — `WEBC_COLOR` overrides it either way. A failed build is printed once,
after one cache-cleared retry, without the compiler's command line (that is `WEBC_VERBOSE`). And a
build failure calls the translator automatically, so `webc build` and `webc check` say the same
sentence about the same mistake. `WEBC_NO_EXPLAIN` turns that off for a script that wants clang's
words.

## If you are writing a tool on top of this

One rule, and it is the one the MCP server is held to: **no tool parses human output.** If a tool
needs data, the command that owns the data learns `--json` first — otherwise you have built a second
front end that will disagree with the first within a release.

Two more, if you are inside the repository:

- **Use the project index, not a regex.** `tools/webc/facts.c` is the one place that knows what a
  project declares, and `dsl.c` is the one place that knows what a macro call looks like. `webc
  routes`, the language server and the MCP server all read them; a fourth scanner would be a fourth
  answer.
- **Write the sentence where the framework already writes them.** `lspx.c` is the diagnostics table:
  the editor underline, the `webc explain` line and the MCP tool result are the same sentence by
  construction, which is why they never contradict each other.

## What this is not

- **Not a code generator.** There is no "build my app" tool. `webc new` writes a working app —
  model, view, routes, migrations and a test, under 300 lines — that already builds and passes its
  own tests; everything after that is the model and the view, which is what the recipes are for.
- **Not a documentation index.** No embeddings, no RAG: the skill is a router and the reference is
  addressable by section, which cannot hallucinate a section that does not exist.
- **Not measured yet.** The honest form of the claim on this page is "this removes the ways an agent
  gets WebC wrong on the first try". The measurement — one fixed task, three arms, turns to green,
  lint hits, tokens spent reading build output, invented APIs — has not been run, and until it is,
  the numbers here are counts of what was built, not improvements.

## Related

- **[The editor](../guides/editor.html)** — the same translator, pointed at your cursor instead of a
  build.
- **[Testing](../guides/testing.html)** — what `webc check` runs, and how to assert a document.
- **[The CLI](../reference/the-cli.html)** — every command, `--json` shapes, §14f for the MCP server
  and §14g for the contract above.
- **[The reference](../reference/index.html)** — the whole language and every module, one page each,
  with the cost of each choice written next to it.