A test in WebC is a C function with a name, run by a binary the framework owns. A handler is
reachable without a socket, which makes the usual unit/integration split unnecessary: the suite runs
in milliseconds and it exercises the same code the server runs.

```sh
webc test                  # everything in tests/
webc test notes            # one suite by name
webc test --test=rejects   # every test whose name contains "rejects"
webc test --list           # what would run
webc test --watch          # re-run on save, clearing the screen each pass
```

`--filter` picks *suites* (and runs them whole); `--test` narrows by *name* across the suites you
filtered. A name that matches nothing is an answer, not a failure — one flag for both would report
"no tests" for a file you had just named.

## A test file

```c
// tests/notes_test.c — the whole file
#include <webc/test.h>

TEST(list_notes) {
  Response r = webc_test_request("GET", "/notes");
  ASSERT_EQ(r.status, 200);
  ASSERT_HTML_COUNT(r, "ul > li", 1);            // the page, read as a document
  ASSERT_HTML_TEXT(r, "ul > li", "hello");      // and the right words in it
  ASSERT_JSON(webc_test_request("GET", "/notes"), "0.body", "hello");
}

TEST(rejects_an_empty_note) {
  ASSERT_EQ(webc_test_request("POST", "/notes").status, 422);
}

TEST_MAIN()
```

`webc test` prints one line per test (`--- PASS: list_notes (0.00s)`), keeps going after a failure,
and turns a panicking handler into a failed test rather than a dead binary.

## The assertions

| Macro | Checks |
| --- | --- |
| `ASSERT_EQ(a, b)` / `ASSERT_NE` / `ASSERT_TRUE` / `ASSERT_FALSE` | values |
| `ASSERT_STR_EQ(a, b)` / `ASSERT_STR_CONTAINS(hay, needle)` | strings |
| `ASSERT_NULL(p)` / `ASSERT_NOT_NULL(p)` | pointers |
| `ASSERT_HTML_COUNT(r, sel, n)` | a CSS selector, counted in the response's document |
| `ASSERT_HTML_TEXT(r, sel, text)` | the text of the first match |
| `ASSERT_HTML_ATTR(r, sel, name, value)` | an attribute |
| `ASSERT_JSON(r, "0.body", "hello")` | a dotted path into the response body |
| `ASSERT_JSON_DOC(doc, "views", "42")` | the same question about a document you built |
| `ASSERT_LD(r, "author.name", "Ada")` | a dotted path into the page's JSON-LD block; `ASSERT_LD_ABSENT` proves a draft's `datePublished` is not there — and not `""` |
| `ASSERT_STATUS(r, 303)` | the status and nothing else |
| `ASSERT_HEADER(r, "Location", "/notes")` | a header by name, **case-insensitively** |
| `ASSERT_REDIRECT(r, "/notes")` | a 302/303/307 *and* the `Location` |
| `ASSERT_DB_ONE(Todo, title, "hello")` / `ASSERT_DB_ONE_I64(Todo, done, 1)` | the row a mutation should have written |
| `ASSERT_DB_COUNT(Todo, 1)` | how many rows the table has |

The HTML assertions parse the response with a real selector engine, so a test can pin structure
(`main > ul > li.todo`) rather than a substring of the markup — and it keeps passing when the
whitespace changes.

## Requests with more than a path

```c
// tests/notes_test.c — a request with a body, and the headers that go with it
#include <webc/test.h>

TEST(accepts_json) {
  str body = str_lit("{\"body\":\"hello\"}");
  Response r = webc_test_request_ex("POST", "/notes",
                                    "Content-Type: application/json",
                                    body.p, (i32)body.n);
  ASSERT_EQ(r.status, 201);
  ASSERT_JSON(r, "body", "hello");
}
```

That is also how a test logs in by hand — take the `Set-Cookie` out of the signup response and send
it back. A test that does it more than once wants the client below.

## A browser, without a socket

`TestClient` keeps the cookies, encodes a model into the form the app parses, and knows what a
successful mutation looks like:

```c
// tests/posts_test.c — a save, the row it wrote, the redirect it answered
#include <webc/test.h>

WEBC_MODEL(Post, (id, i64, .pk = true), (slug, str), (title, str))
Vec(Post)

TEST(the_save_route_writes_the_row) {
  Post draft = {.slug = str_lit("last-ferry")};
  TestClient c = test_client();      // a jar: a second one is a second browser
  Response r = tc_post_form_at(c, "/admin/posts/save", Post, &draft);
  ASSERT_REDIRECT(r, "/admin/posts");       // 302/303/307 *and* the Location
  ASSERT_DB_ONE(Post, slug, str_lit("last-ferry"));  // the row it wrote
  ASSERT_DB_COUNT(Post, 1);
}
```

`tc_get`, `tc_send(method, path)`, `tc_post_form`, `tc_post_form_at`, `tc_post_json` and
`tc_post_body` are the six ways in. The form encoder matches `model_from_form` exactly — one input
per field in declaration order, the pk left out, a `bool` as `on`/`off`, an enum as its name, a
`VecStr` as one comma-separated value — so a test and the browser it stands in for post the same
thing. What it does not do is run JavaScript (that is `webc test --client`) or follow a redirect: a
test that follows one hides the 303 it came for.

## Setup and teardown

```c
// tests/app_test.c — setup once, around every test in the file
#include <webc/server.h>
#include <webc/test.h>
#include <stdlib.h>

static void boot(void) {
  setenv("DATABASE_URL", "sqlite::memory:", 1);   // a scratch database per run
  webc_boot();                                    // the app's own wiring
  (void)db_default();                             // waits for the schema to exist
}
TEST_SETUP(boot);
```

`TEST_SETUP(fn)` and `TEST_TEARDOWN(fn)` run once per file, around every test in it. The framework
owns `webc_boot()` and `main()`, so an app puts its setup in a `webc_boot` of its own only when the
default is not enough — and then the suite calls the same function against a scratch database.

## Subtests

```c
// tests/parse_test.c — one row per case, and the row's name labels the failure
#include <webc/test.h>

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

The row's `name` labels the failure, so a broken case reads as `parses_numbers/negative: -8 != -7`.

## Testing the browser half

`<webc/client_test.h>` runs a view without a browser: it renders to a string, dispatches a named
control as if it had been clicked, and re-renders — which is how the framework tests its own
components and how an app can too.

```c
// tests/rows_test.c — render to a string, click by name, render again
#include <webc/web.h>
#include <webc/client_test.h>

state(i64, row_a, 0);
action(bump) { set(row_a, get(row_a) + 1); }

view(Rows) {
  el("button") { named("bump-a"); text("%lld", (long long)get(row_a)); }
}

client_main {
  bind("bump-a", "click", bump);          // the name the view marked
  return 0;
}

CLIENT_TEST(a_click_bumps_one_row) {
  wc_app_main();                          // wires every name, exactly once
  WComponent c;
  wcomp_init(&c, nullptr);
  // the first render
  ASSERT_STR_CONTAINS(wcomp_html_ctx(&c, view_Rows, nullptr), "0");
  // the same path a real click takes
  client_dispatch("bump-a", "click");
  EXPECT_SIGNAL(row_a, 1);                         // the app's own signal, by name
  // and the re-render
  ASSERT_STR_CONTAINS(wcomp_html_ctx(&c, view_Rows, nullptr), "1");
  wcomp_destroy(&c);
}
```

`view_Rows` is the function `view(Rows)` defines — a view is a C function, so a test can render it
into a string with no browser, no container and no mount.

The same file has `client_type("body", "typed")` for a field and `client_press("meta+k")` for a
shortcut — the bridge's own tables, so a test cannot drift from the browser. `client_dispatch_at(hook,
ev, x, y, w, h)` says where the pointer was, and `ev_point()` inside the action then answers what the
bridge would have sent, which is how a chart crosshair or a tooltip becomes ordinary testable code.

A queued `on_mount` that nobody ran is a failure rather than a silence.

## Wiring, not just behaviour

`webc check --wiring` is the static half of the suite. It reads the declarations and the reads and
names the disagreements: a signal written and never read (a leftover `tick`), a live binding on a
page-wide signal inside a `for_each`, a stateful part called twice that thought it had two states, a
`bind(...)` naming an action no view renders.

## In the framework repository

```sh
make test          # 81 suites: C, JS, scaffold, docs
webc check         # build, test, doctor, boot, schema, routes
webc api           # the public surface, against its budget
```

`webc check` also generates every scaffold and builds it, so the templates cannot rot.
