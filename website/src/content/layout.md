`webc new` writes the same shape every time, and the shape is the architecture: three files that
compile for the server, for the browser, or for both, and a `webc.json` that says which is which.

```sh
webc new demo --template=fullstack
```

```text
demo/
  webc.json                  the manifest: what compiles for which target
  src/
    app.h                    models, view declarations, what both halves share
    server.c                 routes, database, boot                (server only)
    view.c                   the page, as C                          (shared)
    client.c                 signals, actions, the mount            (wasm only)
  public/
    style.css                served as-is
  migrations/
    0001_init.sql            the schema, applied at boot
    0001_init.postgres.sql   the same thing, for the other driver
  partials/
    site/head.html           authored head chrome, included by partial("site/head")
  tests/
    app_test.c               the suite `webc test` runs
  data/                      the file database (created on boot, in .gitignore)
```

`webc.json` for that project:

```json
{
  "name": "demo",
  "version": "0.1.0",
  "database": "sqlite:data/dev.db",
  "port": 8080,
  "dev": { "port": 9999 },
  "server": ["src/server.c"],
  "shared": ["src/view.c"],
  "client": ["src/client.c"],
  "boot": {
    "title": "demo",
    "lang": "en",
    "css": ["/public/style.css"],
    "el": "#root",
    "data": "/api/notes",
    "hydrate": true
  }
}
```

## The three lists are the boundary

| List | Compiles into | Can see | Cannot see |
| --- | --- | --- | --- |
| `shared` | both binaries | models, views, signals, pure helpers | `db_*`, `req`, `res_*`, sockets |
| `server` | the server binary | everything, plus the database and the request | — |
| `client` | the wasm module | the shared surface, DOM, `js()`, `fetch` | the database, the request |

A function that reaches across the line is a **compile error**, not a review comment:
`<webc/shared.h>` does not declare `db_*` or `req`. `webc lint` runs the wrong-half pass on top of
that, because a wrong-half call is the mistake a `#include` makes easy.

## The boot block

`boot` is the page shell, and it is why a fullstack app has no HTML file:

- `title`, `lang`, `css` — what the generated `<head>` carries;
- `el` — the selector `page()` renders into and the client mounts over;
- `data` — the URL the module fetches for its first document (`wc_app_data` receives it);
- `hydrate` — whether the client adopts the server's nodes or rebuilds them.

```c
// src/server.c — the page is a value, not a file
#include <webc/server.h>

WEBC_MODEL(Note, (id, i64, .pk = true), (body, Text, .required = true))
Vec(Note)
declare_view(Notes, VecNote);

GET("/")          { return page(Notes, db_all(Note)); }
GET("/api/notes") { return json_list(200, db_all(Note)); }
```

`declare_view(Notes, VecNote)` is the one line that makes the name `Notes` mean the same function in
`src/view.c` (where `view(Notes, …)` defines it) and in `src/server.c` (where `page()` calls it) —
handlers and views live in different files, and this is the header that ties them.

`partials/` is where authored HTML lives — favicons, theme colours, a banner, a whole
maintenance page — included by name (`partial("site/head")`), read once, cached, and verified
by `webc check`. A fragment that needs a value is a view, not a partial.

`page()` renders the view into the shell (or into `public/index.html` if you wrote one), and marks
the nodes it drew with `data-wc` anchors. `hydrate: true` makes the client adopt those exact nodes —
zero DOM creation on the first render.

## What `.webc/` is

`webc build` writes into `.webc/`, which is generated and ignored:

```text
.webc/app          the server binary
.webc/app.wasm     the browser module (only when `client` is non-empty)
.webc/gen/         headers the toolchain generated (schema, wasm glue)
```

Nothing in `.webc/` is ever edited. `webc dev` rebuilds it on save; `webc serve` builds a release
into it once and serves that.

## Files you may not need

- **`public/index.html`** — write one when you want control of the shell; otherwise `boot` generates
  it. It is also where `page()` writes the render when it exists.
- **`src/app.h`** — a convention, not a requirement: it is where the model and the `declare_view`
  line live so both halves include one header instead of two.
- **`migrations/`** — optional for a toy, mandatory the moment you have data you care about.
