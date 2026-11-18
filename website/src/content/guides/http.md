A handler is a function that returns a `Response`. It does not write to a socket, which is why it
can be called from a test without opening a port, wrapped by middleware, or run twice to see what
it does. Routes are declarations at file scope, so the router is a table the compiler builds.

Every snippet on this page is a whole file: it includes the header it needs and declares the models
it uses, so you can paste it into an empty `src/server.c` and compile.

## The route macros

```c
// src/server.c — four routes, and everything they need
#include <webc/server.h>

WEBC_MODEL(User, (id, i64, .pk = true), (email, Text, .required = true))
WEBC_MODEL(Account, (id, i64, .pk = true), (owner, Text), (balance, i64))

GET("/users/:id") {
  i64 id = path_i64("id");                 // the `:id` in the pattern, parsed
  // the default database: no handle to thread
  User *u = db_get(User, id);
  // answers; this handler does not return
  if (!u) fail(404, "no user %lld", (long long)id);
  return json(200, User, u);
}

POST("/accounts/:id/deposit") {
  Account *a = db_get(Account, path_i64("id"));
  if (!a) fail(404, "no such account");
  a->balance += query_i64("amount", 0);    // ?amount=25
  db_save(Account, a);
  return json(200, Account, a);
}

PATCH("/accounts/:id/freeze") { return res_empty(204); }
DELETE("/accounts/:id")       { return res_empty(204); }
```

`GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, `OPTIONS`, `ANY`, each with an `_MW` variant that
attaches per-route middleware. Matching order is static segment, then `:param`, then `*wildcard`;
`HEAD` falls back to `GET`; a trailing slash is not a different route; a known path with the wrong
method is 405, not 404.

`webc routes` prints the table the compiler built — method, pattern, handler, and which middleware
runs where. It is the fastest way to answer "why is my 404 not a 405".

## Reading the request

| Call | Type | Note |
| --- | --- | --- |
| `path("name")` | `Text` | a path parameter; the string type an app names |
| `query_text("name")` | `Text` | absent and empty are different; both are `""` here |
| `path_i64("id")`, `query_i64("amount", 0)` | `i64` | parsed, or the default |
| `req_query_bool("all", false)` | `bool` | |
| `query("name")` | `str` | the raw bytes, for a value that may be binary |
| `header("accept")`, `cookie("session")` | `str` | |
| `req->body`, `req->peer`, `req->request_id`, `req->arena` | | the rest of the request |

## Answering it

One spelling per kind of answer:

| Call | Sends |
| --- | --- |
| `json(status, Model, ptr)` | a model or a `Vec` as JSON |
| `json(status, text)` | a body you already built |
| `text(status, s)` / `html(status, s)` | `text/plain` / `text/html` |
| `page(View, ctx)` / `page_with(status, View, ctx)` | a view inside the page shell |
| `fail(status, msg, …)` / `res_error(status, code, msg)` | an error body in the client's shape |
| `redirect(url)` | 303 with a `Location` |
| `res_file`, `res_download`, `res_bytes`, `res_stream`, `res_sse`, `res_upgrade` | the rest |
| `res_cookie`, `res_header(_str)` | a header on the response |

A mutation route usually ends with the line that serves both callers — a form post gets its
redirect back to the page, a JSON call gets the row it changed:

```c
// src/server.c — one mutation, two callers
#include <webc/server.h>

WEBC_MODEL(Note, (id, i64, .pk = true), (body, Text, .required = true))

POST("/notes") {
  Note n = body(Note);                     // parse + validate, or answer 422 here
  db_save(Note, &n);
  return wc_body_is_form() ? redirect("/") : json(201, Note, &n);
}
```

## The page's head

The shell owns the document; the route owns the page's facts about itself. `head_title()`,
`head_meta()`, `head_prop()` and `head_link()` splice into `<head>` on the way out — escaped
once, before anything compresses — and two doors cover what a string of calls cannot:

```c
// src/server.c — a page's head: facts as calls, chrome as a file
#include <webc/server.h>
#include <webc/ld.h>

WEBC_MODEL(Article, (id, i64, .pk = true), (slug, Text, .required = true),
                    (title, Text, .required = true), (author, Text))
Vec(Article)
declare_view(ArticlePage)

GET("/articles/:slug") {
  Article *a = must(db_by(Article, "slug", path("slug")), "no article");
  head_title(a->title);
  head_meta("description", a->title);
  head_link("canonical", fmt("https://example.test/articles/%s", a->slug));
  head_ld(ld_blog_posting(.headline = a->title, .author = a->author,
                          .publisher = "The Meridian", .language = "en"));
  head_html(partial("site/head"));   // favicons, theme colours, the font — a file
  return page(ArticlePage, a);
}
```

`head_ld()` is not sugar for a `<script>` tag: that body is JSON *and* markup, and a title
containing `</script>` would end the element if the escaping were the caller's job —
`webc lint` names the hand-built spelling. A field you leave out is omitted, never written
`""` (`"datePublished":""` is not a date), and `ASSERT_LD(r, "author.name", …)` is how a test
holds the block to the page. `partial("site/head")` is the same rule for chrome: bytes that
never change per page belong in `partials/site/head.html`, not in a C string of escaped
quotes; `webc check` verifies every name has its file.

## Bodies

`body(Model)` reads whichever body the request carries — JSON or a form — validates it with the
model's own rules, and answers the request itself on a failure. When you want to decide what happens
next, the two spellings are explicit and end the handler early:

```c
// src/server.c — the explicit spellings, when the failure needs your answer
#include <webc/server.h>

WEBC_MODEL(Signup, (email, Text, .required = true), (plan, Text))

POST("/signup") {
  // declares `s`; malformed: 400, a bad value: 422 naming the field
  TRY_FORM(Signup, s);
  db_save(Signup, &s);
  return json(201, Signup, &s);
}

POST("/api/signup") {
  TRY_BODY(Signup, s);          // JSON only; same macros, same messages
  db_save(Signup, &s);
  return json(201, Signup, &s);
}
```

Both produce the same validation messages the browser would, because they run the same code over the
same model descriptor.

## Errors

A handler may return `Response` or raise: `fail(status, fmt, …)` is a router-level error, and `try` /
`tryv` propagate a `WErr` with `errdefer` cleaning up on the way out. A panic that reaches the
boundary is a 500 and a log line — the connection is not lost and the process is not dead.

```c
// src/server.c — three ways a handler says "no"
#include <webc/server.h>

WEBC_MODEL(Story, (id, i64, .pk = true), (title, Text, .required = true))

GET("/stories/:id") {
  i64 id = path_i64("id");
  Story *s = db_get(Story, id);
  // answers now; does not return
  if (!s) fail(404, "no story %lld", (long long)id);
  return json(200, Story, s);
}

GET("/boom") {
  fprintf(stderr, "this is the log line\n");
  int *nowhere = nullptr;
  // a panic: 500, process alive
  return json(200, Story, &(Story){.id = *nowhere});
}
```

## Middleware

A middleware is a plain function — `bool (*)(Request *, void *, WNext *)` — and `false` stops the
chain. The framework's own are not special; yours sits in the same list:

```c
// src/server.c — a middleware of your own, and the built-ins around it
#include <webc/server.h>

static bool require_login(Request *req, void *ctx, WNext *next) {
  (void)ctx; (void)next;
  str session = cookie("session");          // .p is null when the cookie was absent
  return session.p != nullptr;              // false: the chain stops here
}

// the app's half of boot (the framework calls it)
void webc_boot(void) {
  webc_use(mw_request_id());
  webc_use(mw_logging());
  // the CSP, with a per-request nonce; a CDN in img-src is one struct away
  webc_use(mw_headers((HeadersCfg){
      .img_src = (const char *[]){"'self'", "data:", "https://cdn.example.com"},
      .img_src_n = 3}));
  // cancels the request's context on the way out
  webc_use(mw_timeout_ms(5000));
  webc_use((WMiddleware){.name = "require_login", .fn = require_login});
}
```

Built in: `mw_logging`, `mw_request_id`, `mw_headers(HeadersCfg)` / `mw_headers_default()`,
`mw_cors(&(CorsCfg){…})`, `mw_body_limit(bytes)`, `mw_timeout_ms(ms)`,
`mw_auth_bearer(token)`, `mw_recover`, `mw_static(prefix, dir)`, `mw_static_spa(prefix, dir)`,
`mw_metrics`, `mw_dev(port)`, `mw_web_assets(wasm_path)`, `mw_compress()`, `mw_document()`.
Each is a function you call; `webc_use` takes the value it returns.

**You rarely register all of them.** `webc_main()` calls
`webc_install_default_middleware()` after `webc_boot()`, so gzip, the request id, the CSP, the
logging, the panic recovery, the body limit, `/webc.js` + `/app.wasm`, `/public` and `/assets`,
the document pass and the 404 page are already in the chain — and because the app's own
middlewares are registered *first*, they wrap all of it. `webc dev` starts the app with the dev
reload middleware in the same place.

**What you register replaces the framework's, it does not stack beside it.** A default whose
name is already in the chain is not installed, so the `mw_headers()` above *is* the policy —
the one from `webc.json`'s `"csp"` block is the fallback for a project that registers none. The
same holds for `mw_logging()`, `mw_recover()` and `mw_document()`: one of each, the app's.

**Order matters twice, and both are stated in the reference** ([§8b](../reference/middleware.html)):
gzip goes **before** the middlewares that answer, so it wraps their bodies; and `mw_document()`
goes **last**, because it splices the page's `<head>` into the body and a splice after compression
searches gzip bytes for `</head>` and finds nothing — which is how a page kept its generic title
in every browser while `curl` looked fine.

`mw_recover` is what turns a panic into a 500 rather than a lost connection; `mw_timeout_ms` cancels
the request's context, so a query in flight is cancelled at the server rather than abandoned.

## What you get for free

- **gzip** — on by default when the client accepts it, with a small-body threshold and a cache of
  compressed responses keyed by size and etag.
- **A real 404** — `"not_found": "public/404.html"` in `webc.json` answers a browser with that
  page inside your shell (title, theme, nonce), and leaves a JSON client its JSON error.
- **A 500 that names the request** — `internal error — GET /a/x (route /a/:slug, id …)` in the
  body, the log line and the crash report, so a report is a route away from being fixed.
- **Byte ranges** — `res_file` answers 206 for a media element, so a `<video>` seeks.
- **SSE and websockets** — `res_sse` and `res_upgrade`, in-process; see
  [Real-time](../guides/live.html).
- **Metrics and `/metrics`** — request counts, latency histograms, and the pool and scheduler
  gauges, without an exporter.

## See also

- Reference [§7 HTTP](../reference/http.html) and [§8 Middleware](../reference/middleware.html).
- [Models](../guides/models.html) for what `body()` validates against.
- [Testing](../guides/testing.html) for calling a handler without a port.