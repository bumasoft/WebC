---
name: webc
description: Build, extend and debug WebC applications — a C23 full-stack framework with one model declaration shared by the server and the browser. Use whenever a project has a webc.json, uses WEBC_MODEL/view()/GET() macros, or the user asks for a WebC page, route, model, migration, form, signal, wasm client half or test. Also use when `webc check` fails and its message needs interpreting.
---

# WebC

C23, one language and one model declaration for both halves: routes and a database on
the server, signals and a DOM in the browser, compiled from the same view into a static
binary and a wasm module. Two things follow, and they are the whole job:

1. **The framework decides the shape.** A model is the schema, the validation, the form
   and the JSON. A view is markup *in C*. A handler returns a response value. If you find
   yourself writing a JSON string, a form parser, a SQL statement or an allocator, you
   have left the framework and the framework will say so.
2. **The framework refuses the C you are about to write.** Forty-four spellings are
   retired on purpose — allocating an arena by hand, a `main()`, a target macro, the long
   render call, a JSON body as a string. `webc lint` fails on each with the line and
   the sentence to write instead (`references/traps.md` is the list). Believe it; do not
   route around it.

## The loop

```sh
webc new myapp --template=fullstack   # server | web | fullstack | board
cd myapp
webc check                            # build + tests + doctor + boot + schema + routes + lint
webc dev                              # build, run, reload on save (:8080)
```

New to the framework? `webc learn` is five lessons, one idea each: real apps whose
own gates fail until the one edit lands. Coming from React, Solid or Next? The
Rosetta guide (`webc docs` puts the whole book in `.webc/docs/`) is the dictionary.

`webc check` is the gate. It exits non-zero, prints one line per step, and — this is the
important part — a **failed build comes back in WebC's words**, not the compiler's macro
expansion. Run it after every change. If you only remember one thing, remember that the
answer to "did I break it" is `webc check`, not a guess.

```sh
webc check --json      # {ok, steps:[{name, ok, skipped, output}]}
webc explain --project # the engine's diagnosis of the sources, no build
webc test --filter=x   # one suite; --test=y one test by name; --watch reruns on save
webc routes            # every route, with the file, line and view it returns
webc migrate diff      # what the models say the tables are missing, as additive SQL
webc scaffold Post     # list/form/save/delete + a test, from the model, as code you own
```

## Where to read

Read the leaf you need, not the whole documentation. Each reference opens with a contents
list of line numbers — read that span.

| When the task is… | Read |
|---|---|
| a string, `fmt()`, an arena question, an error, a `Vec`/`Map` | `references/core-and-strings.md` |
| a field, a table, a filter, a relation, a page of rows | `references/models-and-orm.md` |
| a route, a response, a cookie, SSE, a file, middleware | `references/http-and-routes.md` |
| markup, a signal, an effect, a part, a component, children | `references/views-and-signals.md` |
| a button that acts, a live value, calling JavaScript | `references/client-and-wasm.md` |
| Postgres/MySQL, migrations per dialect, Redis, the query cache | `references/data-and-sql.md` |
| a form, validation, an error under a field | `references/forms.md` |
| a password, a login, a session cookie, a guarded page | `references/auth.md` |
| a background task, a channel, `select`, a timer, a mutex | `references/concurrency.md` |
| picking a standard-library module, its cost | `references/stdlib.md` |
| a test, an assertion, a fixture | `references/testing.md` |
| logging, metrics, limits, deploying | `references/ops-and-deploy.md` |
| a command, the language server, the MCP server | `references/cli.md` |
| "why does the compiler say that?" / a retired spelling | `references/traps.md` |

Every section of `docs/reference.md` is quoted by exactly one leaf — `make skill-check` fails
when one is not — so a leaf is the answer, and this directory is the whole book for the work
you are doing. If you are reading this from an app (`webc docs` puts this directory in
`.webc/docs/`), nothing else on the machine has to be found or searched: do not go looking for
the framework's sources to answer "what does this name do".

Two exceptions worth naming, because they are answers a leaf cannot hold:

- **"what is this exact name"** — `webc api <fragment>` prints the declaration, its header and
  the doc line above it, from the headers this toolchain was built with.
- **"why is it like this"** — the comment above the code that does it, and the reference page
  for that module: every surprising choice says what it costs, in the paragraph that makes it.

## The five ideas (everything else is a consequence)

1. **A string belongs to a scope.** The framework installs an arena per scope — process,
   request, render — and resets it in one instruction. An app never allocates one:
   `fmt()` builds into the current scope, `persist()` is the one escape hatch for a value
   that must outlive it. If you can point at the function the value returns from, `fmt()`
   is enough.
2. **A response is a value.** A handler returns `Response` (`page()`, `json()`,
   `redirect()`, `res_text()`) or raises one (`fail(404, "…")`). That is why a handler is
   testable without a socket and why `webc test` runs in milliseconds.
3. **A render follows an action.** The browser half reacts to signals and events and
   re-renders the view; the server half renders the same view for the first paint and the
   client hydrates it. One view, both targets, one context type.
4. **The halves are enforced by the compiler.** `server`, `shared` and `client` file sets
   in `webc.json`. A `shared` file reaches neither the database nor the request — those
   names are not declared there — and neither is a server-only stdlib module.
5. **A model is the schema.** `WEBC_MODEL` writes the struct, the descriptor table, the
   validation, the JSON codec, the form parser and the SQL column types. Add a field
   once; the page, the API, the form and the database all move.

## The shape of a project

```text
webc.json     which sources compile for which target  ("server" / "shared" / "client")
src/app.h     WEBC_MODEL, Vec(Model), the view context, declare_view   ← shared
src/view.c    view(Name, Ctx *c) { el(…) { … } }                       ← shared
src/server.c  GET("/") { return page(Name, &ctx); } … db_all/db_save   ← server only
src/client.c  action(…), client_main { mount(Name, &C, "#root"); }      ← wasm only
public/       style.css, static files
migrations/   0001_init.sql (+ .postgres.sql and .mysql.sql when they differ)
tests/        app_test.c — `webc test` builds and runs every *_test.c
```

`webc.json` for a full-stack app:

```json
{ "name": "notes", "version": "0.1.0",
  "server": ["src/server.c"], "shared": ["src/view.c"], "client": ["src/client.c"],
  "database": "sqlite:data/dev.db", "port": 8080,
  "boot": { "title": "notes", "css": ["/public/style.css"], "el": "#root",
            "data": "/api/notes", "hydrate": true } }
```

## Recipes

**Add a field.** `WEBC_MODEL` gets the tuple (`(done, bool)`, `(title, Text, .max = 200)`,
`(owner, Text, .required = true)`) → `webc migrate diff --write` writes the `alter table`
(the file it produces is additive only; add the `.postgres.sql`/`.mysql.sql` twin if the DDL
differs) → `webc migrate up`. The view, the form and the JSON pick it up; nothing else to edit.

**A column that may be empty.** `(cover_id, i64, .null = true, .references = "media(id)")`, and
`ww_is_null(&w, Post, cover_id)` to ask. Not a plain `i64`: `0` is a value to the database and
the foreign key refuses it.

**A row with no table** (a dashboard tile, a KPI) is `WEBC_ROW(Name, …)` + `Vec(Name)`: the
struct, the descriptor, JSON and a collection, and `webc check` has no schema to be wrong about.

**HTML a user wrote** (a CMS body, a comment) is `#include <webc/richtext.h>` and
`rich(post->body_html)`; it sanitises through a policy on the way out. `text()` is for copy and
`markup()` is for bytes you wrote — never for a stored string a visitor typed.

**A CRUD page for a table** is `webc scaffold Post`: it writes `src/admin_post.c` and its test,
registers the file, and passes `webc check`. Edit what it wrote; it never edits the model.

**A page every tab watches** is `webc scaffold live Post`: the SSE route that seeds
every tab with the whole table, the mutations that broadcast after they write, and
the test that proves the room holds the newest document. The client half is three
lines, printed where they belong.

**A model for a table that already exists** is `webc model from-db --write`: the
database describes its models (`varchar(200) not null` becomes `Text, .max = 200,
.required = true`), `--write` links them and lands the baseline. Nobody greenfields.

**The API boundary, generated.** `webc openapi` writes OpenAPI 3.1 and
`webc client --ts` a typed fetch client, from the routes and models the sources
declare — bodies from the `body` family, responses from `json`/`json_list`, `path_i64`
typing the placeholders. If the app ships `openapi.json` or `webc.client.ts`, they are
files the app owns: keep them current, `--check` fails on drift and `webc ci` runs it.

**Shipping.** `webc deploy --pack docker,fly,systemd,caddy,nginx` writes the ops files
that run the artifact (SSE-aware proxy lines, a hardened unit, a Dockerfile whose base
is an ARG) beside the stripped binary; `webc ci [--gitlab]` writes the workflow that
runs `webc check` + `webc test`, the framework pinned by one `WEBC_REF` at the top.

**Add a page.** `declare_view(Thing, ThingCtx);` in the shared header, `view(Thing, ThingCtx *c) { … }`
in the view file, `GET("/things") { ThingCtx ctx = {…}; return page(Thing, &ctx); }` in a
server file. `el()` opens elements, `text()` writes text, `for_each(x, vec)` loops,
`class_(cond ? "a" : "")` sets an attribute, `post_to("/things")` on a form.

**Add an API endpoint.** `GET("/api/things") { return json_list(200, db_all(Thing)); }`.
Never build JSON by hand: `json(200, Model, &row)`, `json_list(200, rows)`, `json(200, s)`
where `s` is a `str`. `json_list` reads `rows->desc`, so a wrong argument is a compile
error; `json()` on the same collection still works and answers a 500 with a sentence
instead of a crash.

**Read a request.** Path parameters `path("id")` / `path_i64("id")`, query `query("q")`,
a validated model body `body(Thing)` (422 or a filled struct — that is the whole
statement), and `body_try(Thing, &t)` when the handler wants to answer itself.

**Make a button act.** `action(remove, Thing *t) { … }` in the client half, wired where
the button is written: `el("button") { on_click(remove); text("×"); }`. The action gets
the item by key, not by pointer, so a growing list cannot dangle.

**Write a test.**

```c
// tests/app_test.c — the suite `webc test` builds and runs
#include <webc/test.h>
TEST(creates_a_note) {
  Response r = webc_test_request("POST", "/notes");
  ASSERT_EQ(r.status, 422);
  ASSERT_HTML_COUNT(webc_test_request("GET", "/"), "ul > li", 0);
}
TEST_MAIN()
```

A test asserts a *document* (selectors, JSON paths), not substrings. `webc_test_request`
never opens a socket. For a mutating route, `TestClient` is the browser: it keeps cookies,
posts the model as the form the app parses, and `ASSERT_REDIRECT` / `ASSERT_DB_ONE` /
`ASSERT_DB_COUNT` say what a save did.

```c
// tests/notes_test.c — a save, the row it wrote, the redirect it answered
#include <webc/test.h>
WEBC_MODEL(Note, (id, i64, .pk = true), (body, Text))
Vec(Note)
TEST(saves_a_note) {
  TestClient c = test_client();
  Note n = {.body = "hello"};
  ASSERT_REDIRECT(tc_post_form_at(c, "/notes", Note, &n), "/notes");
  ASSERT_DB_ONE(Note, body, str_lit("hello"));
  ASSERT_DB_COUNT(Note, 1);
}
TEST_MAIN()
```

## The rules that bite

- **`server` vs `shared`.** `db_*`, `req`, `path*`, `query`, `body`, and the `upload`,
  `mail`, `jobs`, `limits`, `pwa` headers are server-half only. A shared file that calls
  them fails to compile and the reason is the half, not the name.
- **`on_click` is a client-half name.** A view compiled for the server cannot call it;
  use `named("hook")` in the view and `bind("hook", "click", act)` in the client half
  (the scaffold's list does exactly this).
- **No `main()`.** The framework owns it (`client_main { … }` starts the browser half).
  `webc_boot()` exists for the cases where boot needs a line of its own.
- **No target test in the source.** The file set is the target test: put the file in
  `"client"` and it compiles for the browser, leave it in `"server"` and it does not.
- **`Text` is `const char *`.** A model's string field prints with `%s`; no `.p`/`.n`, no
  cast and no length-precision format for a model field.
- **`.max`/`.min` are string lengths; numbers use `.gte`/`.lte`.** The compiler says so if
  you mix them up.
- **`fmt()` is scoped.** A string that goes into a struct which outlives the request needs
  `persist()` — and the review rule is "if it is stored, name the scope it belongs to".
- **The lint table is not advice.** `references/traps.md` is generated from the gate:
  forty-six retired spellings, each with the sentence to write instead.

## When something is wrong

```sh
webc check              # the gate; a build failure is already translated
webc explain --project  # the engine's diagnosis over the sources (no build)
webc lint -v            # the retired spelling, the line, and what to write instead
webc doctor             # toolchain: clang, c23, sqlite3, wasi-sdk, library
webc routes             # what is actually registered (a route the view never returns?)
webc test --filter=x    # one suite
```

`webc explain` is the answer to "the compiler said `use of undeclared identifier 'view_Nope'`
behind five frames of `WC_CAT`": the framework has a sentence for that, and this command
prints it. Read its sentence before you read the compiler's.

## If you are an agent with the MCP server

`webc mcp` exposes the same machinery as tools: `webc_check`, `webc_facts` (models,
views, actions, routes, halves — instead of grepping), `webc_api` (does this name exist,
and where is it declared), `webc_test`, `webc_dev` + `webc_fetch` (read the page you just
built), `webc_scaffold`. Prefer `webc_facts` over `grep`, and `webc_check` over guessing.