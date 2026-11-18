# Getting started

Ten minutes from nothing to a running app with a database, a page, and a JSON API —
and not one line of it about arenas, targets, or HTTP plumbing.

## 1. Build the framework

The quick door is the installer — a prebuilt CLI and framework tree, no compiler needed
until the first `webc dev`:

```sh
curl -fsSL https://raw.githubusercontent.com/bumasoft/WebC/main/scripts/install.sh | sh
```

From a checkout (developing on the framework, or when you prefer the build):

```sh
make -C /path/to/WebC            # libwebc.a, the webc CLI, the examples
/path/to/WebC/build/webc doctor  # what works on this machine
```

Add the framework to your shell so `webc` is on the path:

```sh
export PATH="/path/to/WebC/build:$PATH"
```

## 2. Scaffold

```sh
webc new hello && cd hello
webc dev
```

`webc dev` compiles the sources named in `webc.json`, runs the app on :8080, watches
the tree, and keeps the page you are looking at: a stylesheet change re-fetches that one
file, a browser-half change swaps the wasm module under the page (DOM, signals, scroll
and caret survive), and a build failure appears as the translated error over the last
good build. Open
<http://localhost:8080>. The scaffold already builds and passes its own tests, so the
first thing you do is change something, not fix something.

Three shapes, one command each:

```sh
webc new api  --template=server      # routes, a database, a page
webc new app  --template=web         # a wasm app, no server
webc new full --template=fullstack   # shared view, both halves
```

## 3. A model is the schema, the form and the JSON

```c
WEBC_MODEL(Note,
           (id, i64, .pk = true),
           (body, Text, .required = true, .max = 500),
           (created_at, Text))

Vec(Note)
```

That declaration gives you `Note_modelof()` (a descriptor table), JSON in and out,
form parsing, validation, and the SQL text for the `notes` table. `Text` is
`const char *`: a model field prints with `%s`, no `.p`/`.n`, no cast.

`.max` and `.min` are *string lengths*; numbers use `.gte`/`.lte`. The compiler says
so if you mix them up.

## 4. The table

`migrations/0001_init.sql`:

```sql
create table if not exists notes (
  id integer primary key autoincrement,
  body text not null,
  created_at text not null
);
```

```sh
webc migrate up
webc migrate status
webc migrate diff            # what the models say this file should also say
```

The server applies `migrations/` at boot when the files are there, so in development
you rarely need to run this by hand — and when you add a field and forget the file,
`webc migrate diff` prints the `alter table` that fixes it (`--write` lands it as the next
migration). It only ever adds: a column, a table, a default. A dialect that spells the identity column differently has
its own file, and the runner picks the one that matches `DATABASE_URL`:
`0001_init.postgres.sql` (`bigserial primary key`) and `0001_init.mysql.sql`
(`bigint auto_increment primary key`). The plain `0001_init.sql` is what sqlite runs.

## 5. A view, and the routes that fill it

`src/view.c` — markup and nothing else:

```c
view(Notes, VecNote *notes) {
  el("main") {
    el("h1") { text("Notes"); }

    el("form") { post_to("/notes");
      el("input") { name("body"); placeholder("a note"); required(); }
      el("button") { text("Add"); }
    }

    el("ul") {
      for_each(n, *notes) {
        el("li") { text("%s", n->body); }
      }
    }
  }
}
```

`src/server.c` — routes, and the three words that answer a request:

```c
GET("/")      { return page(Notes, db_all(Note)); }        // renders the view
GET("/notes") { return json_list(200, db_all(Note)); }     // the same rows, as JSON

POST("/notes") {
  Note n = body(Note);          // parse + validate, or fail the request with 422
  n.created_at = now();
  db_save(Note, &n);            // insert, or update when the id is set
  return (wc_body_is_form() ? redirect(\"/\") : json(201, Note, &n)); // 303 for a form, 201+JSON for an API
}
```

`page()` renders into `public/index.html` when that file exists, and into a bare
fragment when it does not. It writes `data-wc` anchors into the HTML, so the same
markup can be taken over by the client later without being rebuilt.

Nothing above mentions an arena. A request's strings live as long as the request; a
render's strings live as long as the render. `fmt()` formats, and the result is safe
for the rest of that scope. `persist()` is the one escape hatch, for a string that has
to survive the request (a cached value, a model in a global).

## 6. Watch it work

```sh
curl -d 'body=hello' localhost:8080/notes -o /dev/null -w '%{http_code}\n'
curl localhost:8080/notes
curl localhost:8080/metrics | head
```

A validation failure answers with the field that failed and why:

```console
$ curl -s -d 'body=' localhost:8080/notes
{"error":"invalid_body","message":"body: required","status":422}
```

A database failure is not a mystery either: the driver's own words come back, with
the SQLSTATE.

## 7. Prove it

```sh
webc test
```

A test file is a list of `TEST` blocks. A handler is reachable without a socket
through `webc_test_request()`, so the suite runs in milliseconds:

```c
#include <webc/test.h>

TEST(list_notes) {
  Response r = webc_test_request("GET", "/notes");
  ASSERT_EQ(r.status, 200);
  ASSERT_HTML_COUNT(r, "ul > li", 1);           // the page, read as a document
  ASSERT_HTML_TEXT(r, "ul > li", "hello");      // ... with the right words in it
  ASSERT_JSON(webc_test_request("GET", "/notes"), "0.body", "hello");  // and the API
}

TEST(rejects_an_empty_note) {
  Response r = webc_test_request("POST", "/notes");
  ASSERT_EQ(r.status, 422);
}

TEST_MAIN()
```

`webc test` prints one line per test (`--- PASS: list_notes (0.00s)`), keeps going
after a failure, and turns a panicking handler into a failed test instead of a
dead binary. A mutating route is tested the way a browser would reach it — a client that keeps
its cookies, posts the model as a form, and then looks at the row:

```c
TEST(saves_a_note) {
  TestClient c = test_client();
  Note n = {.body = str_lit("hello")};
  ASSERT_REDIRECT(tc_post_form_at(c, "/notes", Note, &n), "/notes");
  ASSERT_DB_ONE(Note, body, str_lit("hello"));
}
```

`webc test --watch` reruns on save; `--test=<name>` runs one test across every suite.
Subtests name their rows:

```c
TEST(parses_numbers) {
  static const struct { const char *name, *in; i64 want; } rows[] = {
    { "zero", "0", 0 }, { "negative", "-7", -7 },
  };
  SUBTESTS(rows) {
    i64 got = 0;
    ASSERT_TRUE(str_to_i64(str_lit(r->in), &got));
    ASSERT_EQ(got, r->want);
  }
}
```

`TEST_SETUP(fn)` / `TEST_TEARDOWN(fn)` run once around the whole file, for a
database or a scheduler. The framework owns `webc_boot()` and `main()`, so an app
puts its setup in a `webc_boot` of its own only when the default is not enough — and
then the test suite calls the same function against a scratch database:

```c
static void boot(void) {
  setenv("DATABASE_URL", "sqlite::memory:", 1);
  webc_boot();
  db_default();
}
TEST_SETUP(boot);

TEST(creates_a_note) {
  Response r = webc_test_request_ex("POST", "/notes", "Content-Type: application/json",
                                    "{\"body\":\"hello\"}", 16);
  ASSERT_EQ(r.status, 201);
}
```

`webc_test_request_ex` also carries headers, which is how a test logs in: take the
`Set-Cookie` out of the signup response and send `"Cookie: session=…"` back.

## 8. Adding a browser half

An app with a client half is three files, and `webc.json` says which is which:

```json
{
  "name": "notes",
  "server": ["src/server.c"],
  "shared": ["src/view.c"],
  "client": ["src/client.c"]
}
```

- **shared** — the model and the view. Compiled for both targets. It cannot reach a
  database or a request: `<webc/shared.h>` does not declare `db_*` or `req`, so a
  `db_get` in the view file is a compile error, not a code review.
- **server** — routes, database, boot.
- **client** — signals, actions, the mount.

The client half names behaviour, never a selector:

```c
client_main {
  mount(Notes, "#board");
  return 0;
}
```

and the view attaches the action to the element it belongs to — one line, at the place
the button is written, and the compiler checks the name:

```c
el("button") { class_("x"); on_click(remove); text("×"); }
```

`action(remove, Note *n) { ... }` receives the item the click landed on, resolved by
its key rather than by a stored pointer, so a growing list cannot dangle.

`public/index.html` loads the module:

```html
<div id="root"></div>
<script src="/webc.js"></script>
<script>
  WebC.start({ el: '#root', wasm: '/app.wasm', hydrate: true });
</script>
```

```sh
webc build      # .webc/app and .webc/app.wasm, from one set of sources
webc dev        # both halves; a client edit swaps the module under the page
webc serve      # a release build, served as a deployment does it (gzip on)
```

`page()` renders the view into the shell's `#root`, so the first paint needs no
JavaScript; `hydrate: true` adopts those nodes (zero DOM creation) and the page becomes
interactive. The wasm target needs
[wasi-sdk](https://github.com/WebAssembly/wasi-sdk) — `webc doctor` tells you whether it
is installed and where it looked. `examples/todo` is the server shape complete,
`examples/web` the client-only one.

## 9. Keep it this way

```sh
webc lint          # in your app: nothing here is a word an app should name
webc api           # in the framework repo: the public surface, against its budget
```

`webc lint` walks `src/`, `tests/` and `public/` and names the line, the retired call
and what to write instead — arenas, target macros, hand-built JSON, a parser in a test,
ignored parameters. `webc check` runs it with the rest. In the framework repo the same
command adds the examples, every scaffold it can generate and the docs; `webc api` keeps
the count of public names from growing, so the surface stays small after you stop
looking.

## 10. Hand it to an agent

Everything above is the loop an agent needs, and the framework has the three surfaces that
make handing it over work rather than hope:

```sh
webc check          # one command, one exit code, and a failure in WebC's words
webc explain --project   # the same translator, over the sources, with no build
```

- **`AGENTS.md`** — every `webc new` writes one into the app: `webc check` as the gate,
  the half rules, `fmt()`/`persist()`, migrations, `partial()` for authored chrome,
  `head_ld()` for structured data, "believe `webc lint`". Any tool that
  reads that file works with no other setup.
- **`skills/webc/`** — a skill: `SKILL.md` (the five ideas, the loop, recipes, the traps)
  and fourteen reference leaves generated from this documentation. Install it with one
  symlink: `ln -s "$PWD/skills/webc" ~/.agents/skills/webc`. `make skill` regenerates the
  leaves, `make skill-check` fails when they drift or the skill teaches a retired spelling.
- **`webc mcp`** — the same machinery as seven MCP tools (the gate, the project index, the
  surface, the tests, a dev server and a fetch from it), for a client that would rather ask
  than shell out.

The guide that covers all of it — what an agent gets, what it gets wrong, and the gate that
stops each mistake — is `website/src/content/guides/agents.md`.

## Where to go next

- `docs/reference.md` for every macro and every limit.
- The site's guides, one page each: **the ORM** (filters, relations, pages, trees),
  **MySQL and MariaDB** (the third driver), **the editor** (`webc lsp` and the VS Code
  extension). `make site` builds `website/` from `docs/reference.md` and
  `website/src/content/`.
- `examples/todo` for a full-stack page, `examples/accounts` for sessions and
  passwords, `examples/web` for the browser half.
- The site's **API-boundary guide** (`website/src/content/guides/api.md`) for the OpenAPI
  document and the typed client, generated from the routes and models.
- The site's **Rosetta guide** (`website/src/content/guides/rosetta.md`) if React, Solid
  or Next.js is the vocabulary you already speak.
- `docs/reference.md` for the whole language, and the reason behind each choice where it
  is made.
