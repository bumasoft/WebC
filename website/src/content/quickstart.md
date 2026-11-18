Ten minutes from nothing to a running app with a database, a page and a JSON API — and not one line
of it about arenas, targets or HTTP plumbing. Every snippet below is a file you can compile: it
carries its own `#include`, declares the model it uses, and names the file it belongs in.

## 1. Scaffold

```sh
webc new hello && cd hello
webc dev
```

`webc dev` compiles what `webc.json` names, runs the app on `:8080`, and watches the tree —
without losing the page: a stylesheet edit re-fetches that one file, a browser-half edit swaps
the wasm module under the page (DOM, signals, scroll and caret survive), and a build failure
shows the translated error over the last good build. The scaffold already builds and passes its
own tests, so the first thing you do is change something, not fix something.

New here? `webc learn` is five lessons, one idea each — real apps whose own gates fail until the
one edit lands.

Three shapes, one command each:

```sh
webc new api  --template=server       # routes, a database, a page
webc new app  --template=web          # a wasm app, no server
webc new full --template=fullstack    # a shared view, both halves
```

## 2. A model is the schema, the form and the JSON

This is `src/app.h`. It is the one header both halves include.

```c
// src/app.h — the half the server and the browser share
#include <webc/shared.h>

WEBC_MODEL(Note,
           (id, i64, .pk = true),
           (body, Text, .required = true, .max = 500),
           (created_at, Text))

Vec(Note)          // VecNote: the collection type, carrying the model's descriptor
```

That declaration gives you `Note_modelof()` (a descriptor table), JSON in and out, form parsing,
validation, and the SQL text for the `notes` table. `Text` is a `const char *`: a model field prints
with `%s`, no `.p`/`.n`, no cast.

`.max` and `.min` are *string lengths*; numbers use `.gte`/`.lte`. The compiler says so if you mix
them up.

## 3. The table

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
```

The server applies `migrations/` at boot when the files are there, so in development you rarely run
this by hand. Postgres has its own spelling of each file (`0001_init.postgres.sql`) and the tool
picks the right one for the driver.

## 4. A view, and the routes that fill it

`src/view.c` — markup and nothing else. The model is repeated here only so the snippet stands alone;
in a real project this file starts with `#include "app.h"`.

```c
// src/view.c — one view, compiled for the server and for the browser
#include <webc/shared.h>

WEBC_MODEL(Note, (id, i64, .pk = true), (body, Text, .required = true, .max = 500))
Vec(Note)

view(Notes, VecNote *notes) {
  el("main") {
    el("h1") { text("Notes"); }

    el("form") { post_to("/notes");
      el("input") { name("body"); placeholder("a note"); required(); }
      el("button") { text("Add"); }
    }

    el("ul") {
      for_each(n, *notes) {
        el("li") { text(n->body); }
      }
    }
  }
}
```

`src/server.c` — routes, and the three words that answer a request:

```c
// src/server.c — the server half: the routes, and the page they render
#include <webc/server.h>

WEBC_MODEL(Note, (id, i64, .pk = true), (body, Text, .required = true, .max = 500),
                 (created_at, Text))
Vec(Note)
declare_view(Notes, VecNote);

GET("/")      { return page(Notes, db_all(Note)); }        // renders the view
GET("/notes") { return json_list(200, db_all(Note)); }     // the same rows, as JSON

POST("/notes") {
  Note n = body(Note);            // parse + validate, or answer 422
  n.created_at = now();
  db_save(Note, &n);              // insert, or update when the id is set
  return wc_body_is_form() ? redirect("/") : json(201, Note, &n);
}
```

`page()` renders into `public/index.html` when that file exists, and into a bare fragment when it
does not. It writes `data-wc` anchors into the HTML, so the same markup can be adopted by the client
later without being rebuilt.

Nothing above mentions an arena. A request's strings live as long as the request; a render's strings
live as long as the render. `fmt()` formats into the current scope and the result is safe for the
rest of it. `persist()` is the one escape hatch, for a string that has to survive the request.

## 5. Watch it work

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

A database failure is not a mystery either: the driver's own words come back, with the SQLSTATE.

## 6. Prove it

```sh
webc test
```

A test file is a list of `TEST` blocks, and it is a C file `webc test` compiles for you:

```c
// tests/app_test.c — a request without a socket
#include <webc/test.h>

TEST(list_notes) {
  Response r = webc_test_request("GET", "/notes");
  ASSERT_EQ(r.status, 200);
  ASSERT_HTML_COUNT(r, "ul > li", 1);          // the page, read as a document
  ASSERT_HTML_TEXT(r, "ul > li", "hello");     // and the right words in it
  ASSERT_JSON(webc_test_request("GET", "/notes"), "0.body", "hello");
}

TEST(rejects_an_empty_note) {
  ASSERT_EQ(webc_test_request("POST", "/notes").status, 422);
}

TEST_MAIN()
```

A handler is reachable without a port, so the suite runs in milliseconds. `webc test` prints one
line per test, keeps going after a failure, and turns a panicking handler into a failed test.

## 7. Add a browser half

An app with a client half is three file sets, and `webc.json` says which is which:

```json
{
  "name": "notes",
  "server": ["src/server.c"],
  "shared": ["src/view.c"],
  "client": ["src/client.c"]
}
```

- **shared** — the model and the view, compiled for both targets. It cannot reach a database or a
  request: `<webc/shared.h>` does not declare `db_*` or `req`, so a `db_get` in a view is a compile
  error rather than a code review.
- **server** — routes, database, boot.
- **client** — signals, actions, the mount.

This is `src/client.c` as the fullstack scaffold writes it: one context, decoded from the seed the
page already carries, then the *same* view mounted over the server's markup.

```c
// src/client.c — the browser half: the same view, mounted over the page
#include <webc/web.h>       // mount, signal, action, bind
#include <webc/host.h>      // webc_post
#include <webc/shared.h>

WEBC_MODEL(Note, (id, i64, .pk = true), (body, Text, .required = true, .max = 500))
Vec(Note)
declare_view(Notes, VecNote);

static VecNote g_notes = {.desc = &WC_VECDESC_VecNote};

action(remove, Note *n) {                    // `n` is the row the click landed on
  for (i32 i = 0; i < len(g_notes); i++)
    if (at(g_notes, i)->id == n->id) { remove_at(g_notes, i); break; }
  str url = fmt_s("/api/notes/%lld/delete", (long long)n->id);
  webc_post(url, str_lit("{}"), nullptr);
}

client_main {
  mount(Notes, &g_notes, "#root");    // adopt the server's markup, then take over
  // the name the view marked with named("remove")
  bind("remove", "click", remove);
  return 0;
}
```

```sh
webc build      # .webc/app and .webc/app.wasm, from one set of sources
webc dev        # both halves; a client edit swaps the module under the page
webc serve      # a release build, served the way a deployment is (gzip on)
```

`page()` renders the view into the shell's `#root`, so the first paint needs no JavaScript;
`hydrate: true` adopts those nodes and the page becomes interactive. The wasm target needs
[wasi-sdk](https://github.com/WebAssembly/wasi-sdk) — `webc doctor` tells you whether it is there.

## 8. Keep it this way

```sh
webc check        # build, test, doctor, boot, schema and routes in one pass
webc lint         # nothing here is a word an app should name
webc api          # the public surface, against its budget
```

`webc lint` walks `src/`, `tests/` and `public/` and names the line, the retired call and what to
write instead. `webc check` runs it with everything else, and in the framework repository the same
command adds the examples, every scaffold it can generate, and these docs.

## Where to go next

- **[Routes, requests, responses](../guides/http.html)** — the handler surface in full.
- **[Models and migrations](../guides/models.html)** — relations, enums, documents, schema drift.
- **[Signals and the DOM](../guides/signals.html)** — what re-renders, and what does not.
- **[The API boundary](../guides/api.html)** — the OpenAPI document and the typed fetch client,
  generated from what your routes and models already declare.
- **[Coming from React, Solid and Next](../guides/rosetta.html)** — the dictionary, if those are
  the names you know.
- **[Agentic coding](../guides/agents.html)** — the `AGENTS.md` in your new app, the skill, and
  `webc mcp`: what to give an agent so it writes WebC instead of C that looks like it.
- **The reference** — every macro and every limit, starting at [§1 House rules](../reference/house-rules.html).