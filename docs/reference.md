# WebC reference

Everything here is implemented and covered by `webc test` (52 suites) unless the
section says otherwise.

Contents
1. [House rules](#house-rules)
2. [Core: strings, memory, errors](#core)
3. [Collections](#collections)
4. [Models](#models)
5. [JSON and forms](#json-and-forms)
6. [Concurrency](#concurrency)
7. [HTTP](#http)
8. [Middleware](#middleware)
9. [Database](#database)
10. [Auth](#auth)
11. [Frontend: signals](#signals)
12. [Frontend: DOM and SSR](#dom)
13. [The browser bridge](#bridge)
14. [The CLI](#cli)
15. [Ops](#ops)
16. [Limits and deviations](#limits)

---

## <a name="house-rules"></a>1. House rules

1. **One way to do it.** If two APIs do the same thing, one of them is a bug.
2. **No hidden allocation.** Every allocation happens through `wm_alloc` or an explicit
   `Arena*`. There is no GC and no hidden copy.
3. **One noun, one macro.** `text(...)` is markup, `text(status, …)` is a response: a
   name means one thing, and a call site never has two of them to choose between.
4. **Compile-time over run-time.** Model descriptors, route tables and validation are
   all built by macros the compiler checks; nothing is parsed from a config file at
   run time except `webc.json` (the CLI).
5. **The reason is next to the code.** A non-obvious choice is explained where it is
   made, in the comment above it, and in the page of this reference that covers it.

---

## <a name="core"></a>2. Core

```c
str s = str_lit("hello");            // a (pointer, length) pair, never NUL-terminated for you
Text t = fmt("%d-%s", 42, "x");       // formatted into the current arena
str trim = str_trim(s);
bool eq = str_eq(s, t);              // by content
i64 n; str_to_i64(str_lit("42"), &n);
str esc = json_escape(a, raw);       // a complete JSON string literal, quotes included
```

Types: `i8..i64`, `u8..u64`, `f32`, `f64`, `bool`, `str`, `Bytes`.

**Memory.** An arena is a chunked bump allocator; `arena_reset` is O(1) and keeps the
memory. The framework installs one per scope — the process, the request, the render —
so application code never creates or resets one. `wm_alloc` / `wm_free` is the raw
heap with a 16-byte header, byte accounting (`wm_stats()`), and an optional hard limit
(`wm_set_limit`); that is what the framework itself uses. Allocating arena memory with
`wm_free` is a panic, on purpose.

What to use in an app, instead of an arena:

```c
Text s = fmt("%s/%lld", prefix, (long long)id);  // lives for this scope
str  v = fmt_s("%s/%lld", prefix, (long long)id); // the same, as a str: no conversion needed
Text t = text_of(some_str);                       // a str -> Text: a NUL-terminated copy
str  w = WC_AS_STR(s);                            // a Text, a str or a literal -> str, no copy
Text keep = persist(s);                           // lives for the process
```

There are two string types and they are both ordinary: **`Text`** is a `char *` that `fmt()`
returns and that markup helpers accept, and **`str`** is a `{ptr, len}` slice that model
fields, routes and `get()` on a `state(Text, …)` use. Crossing between them is one macro each
way — `WC_AS_STR` to go to a `str`, `text_of` or `fmt_s` to go to a `Text` — and every markup
helper in §12 already takes either, so `class_(fmt(...))` needs nothing at all.

**The four spellings, and what happens when you pick the wrong one.** Four types is the thing
people trip over, and the trip is always the same: a value that is one type, a place that
wants another, and a compiler message about a struct. This is the whole table, and the third
column is the message you get — which is the part a cheat sheet usually leaves out:

| where | what it accepts | if you get it wrong |
| --- | --- | --- |
| `text(...)`, `attr(...)`, `class_(...)`, `href(...)` | `str`, `Text`, `const char *`, a number, a `bool` (one value, no format) | `text("%s", x)` **is** a format: `%s` is replaced. `text("%s")` with one argument prints `%s` — it is a value. |
| `text("%s — %s", a, b)` | two or more arguments: the first is a printf format, the rest are values | a `%` you mean literally is written `%%` here. With **one** argument there is no format at all, so `text("50% off")` is exactly those bytes |
| a model field declared `(title, Text)` | a `Text` (a `char *`) | a `str` is `passing 'str' to parameter of incompatible type 'char *'` — write `text_of(s)` |
| a model field declared `(blurb, str)` | a `str` | a `Text` is `passing 'char *' to parameter of incompatible type 'str'` — write `WC_AS_STR(t)` or `str_lit(...)` |
| `query("page")`, `path("slug")`, `req_header(...)`, `get(...)` | all return `str` | assigning one to a `Text` field is the error above; `query_text("page")` is the `Text` door |
| `fmt("…", …)` | a `str`, a `Text` or a literal; every one of them prints correctly | the result is a `Text`, and keeping it past its scope is the bug `WEBC_STRICT_LIFETIME` finds |
| `res_text(200, …)`, `res_html(200, …)`, `res_json*` | `str` (a literal is one) | a `Text` needs `WC_AS_STR(t)` |
| a `Buf *` (`&b`) | the place that built it | handing one where a `str` goes is `passing 'Buf *' to parameter of incompatible type 'str'` — `buf_str(&b)` |
| `json(200, str)` | bytes already encoded | anything else in that position is a collection: use `json_list(200, v)` for a list |

The rule behind the table: **anything that writes text takes any string** (`str`, `Text` or a
literal), and a `wc_*_c` name means exactly `const char *`. The conversion is always one
macro — `WC_AS_STR` in, `text_of`/`fmt_s` out — so the fix is mechanical once the error is
read as "wrong spelling", which is what the third column is for.

`WEBC_STRICT_LIFETIME=1` mprotects a scope arena when it dies, so a `Text` used past
its scope crashes in development instead of showing yesterday's bytes.

**Errors.**

```c
WErrRes r = do_thing();
if (!r.ok) return r;
tryv(do_thing());            // return early, propagating the error
try(x)                       // unwrap a Result, returning the error from the enclosing fn
errdefer(free_it, buf) { ... }  // runs only when the block leaves through a failed try
```

- The message is **stored in the error** (`WErr.buf`, up to `WC_ERR_MSG_CAP` bytes;
  longer messages are truncated and `truncated` is set). A `WErr` you keep past the
  arena that raised it still reads correctly, and copying the error copies the message.
  Read it with `werr_msg(&e)` — a `str` into the error, so the error must outlive it —
  or `printf("%.*s", WERR_FMT(e))`. `detail` is the exception: it is request context
  and is borrowed.
- `errdefer`'s "did this block fail" counter lives in the *task*, like the panic frame
  and the current arena, so a `try()` that fails before an `await` and the `errdefer`
  that runs after it agree even when the task resumed on another worker.

`panic(...)` prints, then unwinds to the nearest `wc_try_begin` frame if one is
installed — which is how the `recover` middleware turns a panic into a 500 instead of
killing a worker.

**A 500 says where it happened.** The body is still a flat `{error, message}`, and the
message carries the request: `internal error — GET /a/x (route /a/:slug, id 6f1c…)`. A
`fail(500, …)` keeps its own sentence in that place; a genuine panic stays `internal error`
so a broken handler cannot leak its internals to a stranger. The route, the method and the
id are not internals — they are what makes the report usable — and the same four fields open
the log line and the crash report's first line, so a `crash-*.txt` names the request it
crashed in.

---

## <a name="collections"></a>3. Collections

```c
Vec(Todo)                      // → the type VecTodo, carrying its descriptor
static VecTodo todos;

push(todos, (Todo){.title = "write it"});   // every verb takes the vector
len(todos); at(todos, 0)->title;            // reads
remove_at(todos, 0);                        // keeps order
for_each(t, todos) { text("%s", t->title); } // keyed by .pk, no callbacks

WEBC_MAP_DEFINE(MapStrI64, map_str_i64, str, i64)
map_str_i64_put(&m, str_lit("a"), 1);   // keys compared by content for str
```

The descriptor lives *inside* the vector, so a collection names its element type at
run time: `json(200, &todos)` needs no model argument, and `for_each` needs no key
expression. `db_all(Todo)` returns a `VecTodo *` built the same way.

**The five primitive collections are prelude types, and `vec_new()` speaks their spelling.**
`VecI64`, `VecI32`, `VecF64`, `VecStr` and `VecBytes` are declared by the framework,
so an empty collection of numbers is one call rather than a hand-declared type:

```c
VecStr *names = vec_new(str);        // the same type as VecStr
VecI64 *ids   = vec_new(i64);        // and as VecI64
VecF64 *means = vec_new(f64);        // a collection of pointers: vec_new(Story) → VecStory
```

`vec_new()` builds its type name by pasting `Vec` onto its argument, which spells `str` as
`Vecstr` — a type nobody declared, so the one call every empty list needs did not compile and
apps wrote the three lines by hand. The paste now lands on the prelude's own type. The other
direction is a redeclaration (`Vec(str)` would declare `Vecstr` a second time), so a primitive
collection is *named*, never re-declared: `Vec(i64)` is what `webc lint` reports.

**The argument is the collection, not its address.** A value goes as `&todos`, a
`VecTodo *` from `db_all()` goes as it is — one `&` too many is a `VecTodo **`, and that
reads the pointer's own bytes as a descriptor and kills the process inside the writer.
`json_list(200, v)` is the spelling that cannot be got wrong: it reads `v->desc` and `v->at`,
so a pointer-to-pointer is a *compile* error naming the type (`member reference base type
'VecTodo **' is not a structure`), and `json()` grew a magic word in the descriptor so a wrong
argument is a `500` with a sentence instead of a crash.

Querying is the first answer to "which rows?", and `db_all_where()` is how you ask. For
the collection you already have — grouped by desk, most-read first — two macros, both
keyed on a *field* so the compiler and the model descriptor check the name:

```c
VecStory *mine = vec_new(Story);                        // an empty collection, current arena
VecStory *desk = vec_where_str(&site->stories, Story, section, slug);
VecStory *live = vec_where_bool(&site->stories, Story, published, true);
sort_desc(desk, Story, views);                          // stable, by views, descending
sort_by(desk, Story, published_at);
```

`vec_where_str` / `_i64` / `_bool` return a new collection of the same type; the
comparison follows the field's `WKind`, so a `Text` field compares as text and an `i64`
as a number.

---

**The pointer spellings** — `vlen(rows)`, `vat(rows, i)`, and the typed walk
`each(f, rows) { … f->number … }` — take the `Vec *` the caller actually holds, so the
value forms' `at((*rows), i)` / `len(*rows)` dance disappears from list views.

## <a name="models"></a>4. Models

```c
WEBC_MODEL(Todo,
           (id, i64, .pk = true),
           (title, str, .required = true, .max = 200),
           (tags, VecStr, .json_array = true),
           (owner, Account),
           (secret, str, .sensitive = true, .no_json = true))
```

The macro generates the struct, a descriptor table (`Todo_modelof()`), the table name
(`todos` — snake_case plural, override with `WEBC_MODEL_TABLE(Todo, "todos2")`), and
works for all of:

```c
model_to_json(a, Todo_modelof(), &t);
model_from_json(a, Todo_modelof(), body, &t);
model_from_form(a, Todo_modelof(), body, &t);
model_validate(Todo_modelof(), &t, &errs);
db_insert(db, Todo, &t);
```

Qualifiers (all designated initialisers, so the compiler checks them):

| qualifier | meaning |
|---|---|
| `.pk = true` | primary key; an integer pk is assigned by the database |
| `.required = true` | must be present and non-empty |
| `.not_null = true` | same, spelled for rows rather than bodies |
| `.unique = true` | declared in generated SQL; enforced by the database |
| `.max = N` / `.min = N` | string length bounds |
| `.gte = X` / `.lte = X` | numeric bounds |
| `.format = "email"` | `email`, `uuid`, `url`, `date` |
| `.trim = true` | strip surrounding whitespace before validation |
| `.sensitive = true` | redacted by the logger |
| `.no_json = true` | never serialised |
| `.json_array = true` | a vector field serialises as an array of scalars |
| `.sql = "column"` | column name override |
| `.def = "..."` | SQL default expression |
| `.doc = "..."` | surfaced by `webc doctor` |
| `.null = true` | an integer column that is NULL-able; `0` is how this side spells NULL |
| `.references = "media_assets(id)"` | the FK target, written into the generated DDL |

Bare flags (`.required` with no `= true`) are **not** valid C; write `.required = true`.

**A column that can be absent says so once**. `.null` is for an *integer* kind — the
one kind whose zero is not a value of its own — and it means the three things a nullable
foreign key needs at once:

```c
WEBC_MODEL(Post,
           (id, i64, .pk = true),
           (cover_media_asset_id, i64, .null = true, .references = "media_assets(id)"))

db_save(Post, &p);                       // 0 becomes SQL NULL, not a row pointing at nothing
ww_is_null(&w, Post, cover_media_asset_id);   // where the column is NULL
ww_not_null(&w, Post, cover_media_asset_id);  // and where a cover was chosen
```

- the DDL carries `references media_assets(id)` and no `not null`;
- `db_save` binds SQL `NULL` when the field is `0`, and reads `NULL` back as `0`;
- the JSON encoder writes `null` (and accepts `null` on the way in *only* for this field);
- an empty form input is the browser's way of saying "nothing chosen", so it becomes `0`.

`.null` on a `f64`, an enum or a `Text` is a compile error, and so is `.null` together with
`.required` or `.pk`: a float's `0` is a value, an enum's `0` is its first member, and a
primary key that could be NULL is not one. The workaround this replaces was a hand-written
table rebuild, because sqlite cannot drop a constraint — the app in the review had exactly
that migration.

**A model holds up to 64 fields**, which is the number of slots the preprocessor machinery
(`WC_MAP_S_*`, generated by `tools/gen_pp.py`) has. It was 32, and the 33rd field reported
"pasting formed `WC_MAP_S_(`" — a message about a macro, explained (confidently, wrongly) as a
fault in the field it named. A row this wide is usually two tables: long prose belongs in its
own row and its own screen.

**A closed set is a field, not a `Text` you validate by hand.** `WEBC_ENUM` declares
the set; a field of that type is an int in the struct and its *name* everywhere
bytes are exchanged — JSON, forms, SQL text — and a request that sends a stranger
is a 422 naming the field, not a row that silently renders nowhere:

```c
WEBC_ENUM(Lane, backlog, todo, doing, done)
WEBC_MODEL(Card, ..., (lane, Lane, .required = true))

Card c = {.lane = todo};                 // the C name
Lane_index(str_lit("doing")) == 2;       // helpers carry the type's name
Lane_name(3) == "done";
```

The generated `T_N`, `T_index()`, `T_ok()` and `T_name()` are all compile-checked
against the declaration; an integer column still reads (as its index).

**A row you computed is still a row.** `WEBC_ROW` declares a shape for JSON, a vector and a
view, with no table behind it — the KPI, the dashboard line, the "count by referrer" that
every admin page builds out of a `select`:

```c
WEBC_ROW(CountRow, (key, Text), (value, i64))      // no table, no migration, no pk

static VecCountRow counts;                          // in webc_boot(): VecCountRow_with(app_arena())
push(counts, ((CountRow){.key = "google", .value = 12}));
for_each(r, counts) { el("li") { text("%s: %lld", r->key, (long long)r->value); } }
json(200, &counts);                                 // and it serialises like any collection
```

It is `WEBC_MODEL` + `WEBC_MODEL_INTERNAL`, which is what the schema check already skips, so
the *only* difference is the name: `db_save` on a `WEBC_ROW` panics with a sentence instead of
sending an insert at a table that does not exist. Without it, a computed row is either a
`WEBC_MODEL` that makes `webc check` report drift forever, or a hand-written `Vec` of a struct
the JSON encoder cannot see.

**Two models and a key between them is a relation, and it is declared next to the fields it
names** — `WEBC_MODEL_REF(Story, section, Section, slug)` and the reverse
`WEBC_MODEL_MANY(Section, stories, Story, section)`. That declaration is the only place the
foreign-key column is written; `prefetch` reads it, `ref()`/`refs()` read the links, and
`db_save_all` fills the key. §9a2 and §9a3 are the whole story; the guide
[The ORM](../website/src/content/guides/orm.md) is the walk through.

---

### 4a. Documents: several collections under one wire shape

A model is a row; a **document** is the shape a live page sends — several
collections and scalars in one JSON object, one declaration for both directions:

```c
WEBC_DOCUMENT(BoardDoc,
              (me,      User),        // one row, written nested; a missing key
              (users,   VecUser),     // on the way in leaves it zeroed
              (online,  VecI64),
              (cards,   VecCard),
              (chat,    VecChatMsg))

str  j = BoardDoc_write(a, &d);        // both directions generated; the fields
bool ok = BoardDoc_read(a, j, &out);   // are checked by the compiler, so a new
                                      // field is one edit, not five
```

Field types may be models, vectors of models, vectors of scalars, enums (by name)
or the scalar kinds — everything the model codec checks applies. On the client, a
document arrives through a **store** (§11b): `WStore` owns the two generations a
streaming page needs, and `wc_doc_join()` links relations (below) after a read.

**Relations inside a document.** `WEBC_MODEL_REF(Card, assignee, User, id)` declares
the link as always; on the server `prefetch()` fills it in one query; on a client
that decoded a document, the join is in-memory:

```c
BoardDoc *d = store_read(&room, json);
wc_doc_join(store_arena(&room), Card_modelof(), &WC_VECDESC_VecCard, &d->cards,
            "assignee", User_modelof(), &WC_VECDESC_VecUser, &d->users);
const User *u = ref(Card, k, assignee);   // both halves, one spelling
```

Where a model stores an ISO timestamp as text (the common case; asking the app to parse it
into milliseconds first is what makes every app carry its own calendar), the shared half has
the patterns:

```c
fmt_date_iso(st->published_at, "%e %B %Y")   // "11 October 2026"  (a str)
fmt_date_iso(st->published_at, "%e %b")      // "11 Oct"
fmt_date_iso(st->published_at, "%H:%M")      // "09:40"
fmt_date_iso(now(), "%A, %e %B %Y")          // "Sunday, 11 October 2026"
```

`"2026-10-11"` and `"2026-10-11T09:40:00Z"` both parse; a bad value gives `""` and never
panics. The pattern language is `%Y %m %d %e %b %B %a %A %H %M %S %%` — `%e` is the day
without a leading zero, `%B`/`%A` the long names. `fmt_date()` is the same for a `time_t`.

```c
read_minutes(body)        // 220 words a minute, rounded up, never zero
read_time(body)           // "4 min read"
name_from_slug(slug)      // "ada-lovelace" → "Ada Lovelace"
contains_ci(hay, needle)  // ASCII case-insensitive substring
title_case(s), slugify(s)
```

All of them take a `str`, a `Text` or a C string and return a `str` (except `read_minutes`,
which returns an `i32`). They live in `<webc/url.h>`, which `shared.h` includes, so the
server and the browser half spell a date the same way — the alternative was a month table
in each, which is what The Meridian had.

---

**The codec links in the browser half too** — it always did; the comment that
claimed otherwise cost real apps a hand-mirrored decoder whose field names no compiler
checked. The browser-half spellings zero `out` first, so a partial document cannot leave
a stale field behind:

```c
JParse p = json_parse(a, frame);            // the generation arena is current
JVal *fl = json_get(p.v, "flights");
for (i32 i = 0; fl && i < fl->n; i++) {
  Flight f;
  if (model_read_json_val(a, Flight_modelof(), fl->items[i], &f) && f.id)
    push(g_flights, f);
}
```

## <a name="json-and-forms"></a>5. JSON and forms

`JsonWriter` (streaming, depth-bounded) for output, `JVal` (tree, depth-bounded) for
input. The parser refuses input deeper than `WC_JSON_MAX_DEPTH` (256) instead of
growing the stack. Unknown keys on input are ignored; a field that fails validation
comes back as a `WErrs` list of `{field, message}`.

Writing a response:

| you have | write | why |
| --- | --- | --- |
| one row | `json(200, Todo, &t)` | a `Todo *` carries nothing to recover the model from, so name it |
| a collection | `json_list(200, rows)` | the descriptor is in the struct, and `->desc` is checked by the compiler |
| bytes that are already JSON | `json(200, some_str)` | a tree rendered elsewhere, or another service's payload |
| an error | `res_error(422, "bad_email", "…")` | a flat `{error, message}` an app can act on |

`json(200, rows)` also works for a collection — it is the same bytes — but the argument
goes through `void *`, so a slip is a runtime answer (`500 json_not_a_vector`, one log line)
where `json_list` makes it a compile error.

Writing a document *by hand* — a payload in a shape no model has, or a fragment for someone
else's API — is the writer, and its two costs are brackets and empty strings:

```c
// src/api/ingest.c — a payload in a shape no model has, built by hand
#include <webc/server.h>

WEBC_MODEL(Todo, (id, i64, .pk = true), (title, Text, .required = true),
                    (author_name, Text))
Vec(Todo)

static str hand_built(Todo *post, const char *const *KEYWORDS) {
  JsonWriter w = jw_doc();                          // the document lives in the scope arena
  jw_obj_begin(&w);
  jw_kv_cstr(&w, "headline", post->title);         // key + value, or neither when it is empty
  jw_obj_in(&w, "author") {                        // { "author": { … } }, closed for you
    jw_kv_cstr(&w, "name", post->author_name);
    jw_arr_in(&w, "roles") { jw_str(&w, str_lit("writer")); }
  }
  jw_kv_str_list(&w, "keywords", KEYWORDS);        // a null-terminated list; no list, no key
  jw_obj_end(&w);
  return jw_finish(&w);
}
```

`jw_obj_in` / `jw_arr_in` are the `el(...)` pattern applied to the writer: the block closes
itself, `break` and `return` included. `jw_kv_*` skips the key *and* the value when the value
is absent, because `"datePublished":""` is not a date — it is a claim a reader has to guess
about. `jw_str` escapes JSON; it does not escape HTML, and the next section is why that
distinction has a name.

---

## <a name="concurrency"></a>6. Concurrency

```c
task_async(worker, arg);          // green thread on the M:N scheduler
wtask_wait();                     // wait for every task

ChanInt ch = chan_int_new(4);
chan_int_send(ch, 1);
i32 v; while (chan_int_recv(ch, &v)) { ... }

select(cases, n) { case 0: ... case 1: ... }        // statement form
i32 idx = select_wait(cases, n, timeout_ms);        // index, SELECT_TIMEOUT, SELECT_CLOSED

batch { set(a, 1); set(b, 2); }   // effects run once
```

`wm_*` primitives: `WMutex`, `WRwLock`, `WOnce`, `WWaitGroup`, and `wfd_*` for
non-blocking I/O that parks the task rather than the thread. `webc_serve` runs an
accept task per listener and one task per connection; blocking I/O inside a handler
parks only that task.

Cancellation is a value, not a goroutine:

```c
WCtx *ctx = ctx_new(srv->ctx);              // a tree: cancelling a parent cancels children
ctx_with_timeout(ctx, 250);                 // give up after 250 ms
ctx_on_cancel(ctx, cleanup, arg);           // hooks run outside the lock
if (ctx_done(ctx)) return res_error(499, "cancelled", ctx_err(ctx));
ctx_cancel(ctx); ctx_free(ctx);
```

A request carries one (`webc_req_ctx()`), cancelled when the client disconnects or the
server drains. App-wide values live in a named-slot table:

```c
app_state_set("db", db);  WDB *db = app_state_get("db");  app_state_clear("db");
```

Keep-alive channels, `select` and the `Mutex`/`RwLock`/`Once`/`WaitGroup` names from
the spec are all spelled out in `task.h` (`mutex_lock`, `rwlock_rlock`, `once_do`,
`waitgroup_wait`, `Chan(T)`/`chan_new`/`chan_send`/`chan_recv`).

**Stack size** is 256 KiB per task and not configurable at run time. There is no
guard page (see [limits](#limits)); a runaway recursion faults instead of being
diagnosed.

---

## <a name="http"></a>7. HTTP

```c
GET("/users/:id") {
  i64 id = path_i64("id");
  return json(200, User, user_find(id));
}
```

Routes are declared at file scope (a C function cannot be defined inside a function),
`GET/POST/PUT/PATCH/DELETE/HEAD/OPTIONS/ANY`, with `_MW` variants that attach
per-route middleware. Matching order is static segment > `:param` > `*wildcard`;
`HEAD` falls back to `GET`; a trailing slash is not a different route; a known path
with the wrong method is 405, not 404.

Request helpers: `path(name)` and `query_text(name)` are `Text` — the one string type an
app names; `path_i64(name)`, `query_i64(name, d)`, `req_query_bool(name, d)` are the numbers
they stand for. Then `query(name)` (the raw `str`, for a value that may be binary),
`header(name)`, `cookie(name)`, `req->body`, `req->peer`, `req->request_id`, `req->arena`.

Responses — one spelling each, and `text`/`html` take a C string or a `Text` alike:

| call | what it sends |
| --- | --- |
| `json(status, Model, ptr)` | one row as JSON |
| `json(status, collection)` / `json_list(status, collection)` | a collection as JSON; `json_list` is the spelling the compiler checks |
| `json(status, text)` | a body you already built |
| `text(status, s)` / `html(status, s)` | `text/plain` / `text/html` |
| `page(View, ctx)`, `page_with(status, View, ctx)` | a view inside the shell |
| `res_error(status, code, msg)` | an error body in the shape the client expects |
| `redirect(url)` | 303 with a `Location` |
| `res_file`, `res_download`, `res_bytes`, `res_stream`, `res_sse`, `res_upgrade` | the rest |
| `res_cookie`, `res_header(_str)` | a header on the response |

The last line of a mutation route, either way a browser asks it — a form post gets
its redirect back to the page, a JSON call gets the row it changed:

```c
return (wc_body_is_form() ? redirect(\"/\") : json(200, Card, k));   // == wc_body_is_form() ? redirect("/") : json(200, Card, k)
```

Bodies: `TRY_BODY(Model, var)` parses JSON, `TRY_FORM(Model, var)` parses
`application/x-www-form-urlencoded`; both validate with the same code the browser
would run, and return 422 with `{error, message, status}` on a failure.

Typed extractors take the model's name and fail the *request* on bad input (400
`invalid_body` for malformed JSON, 422 `invalid_form` for a form that parses but does
not validate — the message names the field):

```c
POST("/accounts/:id/deposit") {
  Deposit d = body_json(Deposit);     // or body_form(Deposit), body_bytes(), header_i64("x-count")
  return json(202, Deposit, &d);
}
```

A route can be **named** with a second argument, and then a URL is built from the
pattern instead of spelled again — `url_to()` fills `:params` from (name, value) pairs,
`link_to(Model, row)` from the model's own route. A name that is not registered, or a
parameter the pattern wants and the call did not pass, panics with the route table
rather than producing a 404 a reader finds:

```c
GET("/a/:slug", "story") { … }
GET("/s/:slug", "section") { … }

href(url_to("story", "slug", st->slug));      // → /a/the-last-ferry
href(url_to("home"));                          // a route with no parameters
href(link_to(Story, st));                      // WEBC_MODEL_ROUTE(Story, "/a/:slug")
```

A page's context is filled in by **field name** when the view names its type with
`view(Name, CtxType *c)`, or `declare_view(Name, CtxType);` in a header — the same view and
its context type in one line, which is how a route in another file names it too. Every
name is then checked by the compiler — a typo is
`field designator 'sectoin' does not refer to any field in type 'SectionPage_ctx';
did you mean 'section'?` — and the route reads as prose:

```c
view(SectionPage, SectionCtx *c) { el("h1") { text(c->section->name); } }

GET("/s/:slug") {
  const Section *sec = must(site_section(site, path("slug")), "no such section");
  return render(SectionPage, .site = site, .section = sec, .list = stories_in(sec));
}
```

`must(ptr, msg…)` answers 404 with that sentence when the pointer is null (the whole
route, not a paragraph); `must_with(status, ptr, msg…)` for another status.
`render_with(status, Name, .field = …)` is the same page with a status. `page(View, ctx)`
stays for the one-value case (`page(Notes, db_all(Note))`).

Route groups are a runtime scope (routes self-register before `main`, so a lexical
block cannot wrap them):

```c
group("/api/v1") {
  webc_use(mw_auth_bearer(token));                       // applies to this scope only
  webc_route_add("GET", "/me", me, nullptr, 0);           // prefix becomes /api/v1/me
  group("/admin") { webc_route_add("GET", "/stats", stats, nullptr, 0); }  // prefixes compose
}
```

An outbound client (dogfooded by `webc get`):

```c
WRespC r;
WErrRes e = http_get(arena, "https://example.com/api", &r);
r.status; r.body; r.content_type; r.location;

// a POST with headers, following at most 5 redirects
WErrRes e2 = http_do_follow(arena, (WClient){ .method = "POST", .url = url, .body = json,
                                               .headers = hdrs, .nheaders = 2 }, 5, &r);
```

Framing is `Connection: close` plus chunked decoding; redirects are *returned* unless
you ask for them; TLS needs `-DWEBC_TLS=openssl` (§16).

Testing without a socket: `Response r = webc_test_request("GET", "/users/1");`

### 7b. The page head

`page()` splices a view into the `<div id="root">` of an HTML file, so the `<head>`
is the file's — but the title, the canonical URL and the social cards are per page.
These fill the head of the response being built; the splice happens on the way out and
*before anything compresses the body* (§8b), so it works for `page()`, for `html()`, and
for a document you assemble by hand — and it reaches a browser that sent
`Accept-Encoding: gzip`, which is all of them:

```c
GET("/s/:slug") {
  head_title(fmt("%s — The Meridian", s->name));
  head_meta("description", s->blurb);
  head_prop("og:title", s->name);              // <meta property="og:title" …>
  head_link("canonical", fmt("https://example.com/s/%s", s->slug));
  head_html("<meta name=\"theme-color\" content=\"#fbf8f1\">");  // anything else
  head_html(partial("site/head"));   // a file of chrome (§12d4), rather than nine of those
  head_ld(ld_blog_posting(.headline = s->name, .url = abs_url(path)));  // structured data
  return page(200, Section, &ctx);
}
```

A `<title>` the shell already has is replaced where it stands (the file keeps its shape);
everything else is appended at the end of `<head>`. Text is escaped once, on the way in.
A response with no `</head>` — a fragment, JSON — is left alone.

`head_ld()` is the one exception to "text is escaped on the way in", and it exists because a
`<script>` body is two languages at once. JSON's own escaping does not escape `<`, so a value
containing `</script>` closes the element and the rest of the document is read as markup — with
whatever nonce the framework splices into every `<script>` on the way out. `head_ld()` escapes
the document for the context (`<`, `>`, `&`, U+2028/U+2029 as `\uXXXX`) and writes nothing at
all for an empty document, because an empty block is invalid rather than neutral. Never build
one with `head_html(fmt("…"))`: `webc lint` names that line.

### 7b1. A page is a value: `head()` and `WPage`

Five `head_*` calls in a row are five chances to forget one, and the reader's theme is a
sixth thing to remember per route. A page context that begins with `WPage` gets all of it
from `render()`:

```c
// app.h — the context is shared, and so is the page
typedef struct SectionCtx {
  WPage page;               // head + environment, first field
  Site site;
  const Section *section;
  const VecStory *list;
} SectionCtx;
declare_view(SectionPage, SectionCtx);

// server.c — the route says what the page is
GET("/s/:slug", "section") {
  return render(SectionPage,
                .page = head(.title = fmt("%s — The Meridian", sec->name),
                             .desc = sec->blurb,
                             .og_title = sec->name,
                             .canonical = fmt("https://example.com/s/%s", sec->slug)),
                .site = site, .section = sec, .list = list);
}
```

A project may keep its headers in folders — one per concern — and it may keep them apart from the
sources too (`src/models/people.h`, `src/views/admin/editor.h`). Every `.h` under `src/` is the
project's for the engine, whatever folder it is in: `WEBC_MODEL`, `Vec(T)` and `declare_view` are
found in all of them, and a `#include "app.h"` in a nested file resolves because `-I src` is on
every compile line.

`head(...)` is a compound literal, so every name in it is checked by the compiler.
`render()`, `page()` and `page_with()` all call `webc_page_begin(&ctx)` *before* the view
runs: it fills `page.env` from the request and records the head, which is spliced into the
document before anything compresses it (§8b) — the one-value `page(View, ctx)` form
included, which is what makes the two spellings the same page:

| `page.head` | becomes |
|---|---|
| `title` | `<title>`, and `og:title` unless `og_title` is given (`og_title = ""` suppresses it) |
| `desc` | `<meta name="description">` — skipped when empty |
| `og_type`, `og_image`, `og_site` | `og:type`, `og:image`, `og:site_name` |
| `section_name` | `<meta name="article:section">` |
| `canonical`, `alternate` | `<link rel="canonical">`, `<link rel="alternate">` |

| `page.env` (filled, read in the view) | |
|---|---|
| `theme` | the `theme` cookie when it is `light` or `dark`, otherwise `""` |
| `dark` | `theme == "dark"` |
| `path` | this request's path — where a form should send the reader back to |
| `query` | `?q=` as text, `""` when absent |

A theme in `env.theme` is also written as `data-theme` on `<html>`, so the first byte is
already the right colour. The types live in `<webc/page.h>` (shared), because the context
is compiled for both targets and a view may read `c->page.env.dark`; only filling it needs
a request. A page with no title logs one line per route, because a document that is wrong in
the tab is otherwise silent.

The `head_*` functions still exist for a response you assemble by hand — `webc_page_begin(&page)`
with a local `WPage` is the same thing without `render()`.

### 7b2. `<html>` and `<body>`

A page's document element is per request too: a site that server-renders the reader's
theme needs the attribute in the markup *before the first byte*, or the page flashes the
wrong edition. `html_attr()` and `body_attr()` splice into `<html …>` and `<body …>` on
the way out, the same way the head does, so an app never has to assemble a whole
document by hand:

```c
GET("/") {
  html_attr("data-theme", theme_from_cookie());    // <html lang="en" data-theme="dark">
  body_attr("class", "is-front");
  return page(FrontPage, ctx);
}
```

### 7c. Cookies

`res_cookie(res, name, value, attrs)` adds `Path=/`, `HttpOnly` and `SameSite=Lax` for
you, and `Secure` on a TLS request. `attrs` is *extra* attributes (`Max-Age=60`,
`SameSite=Strict`, …); an attribute you name yourself replaces the default instead of
following it, so nothing appears twice on the wire.

```c
res_cookie(res, "theme", "dark", "Max-Age=31536000");
res_cookie(res, "session", token, "HttpOnly; SameSite=Lax");   // exactly once each
```

---

### 7d. A stream that stays correct: the SSE pattern

`res_sse(fn, ctx)` hands the response body to `fn`, which owns a socket for as long as
it runs. Three facts shape every correct stream, and none of them is obvious:

```c
typedef struct Sub { bool used, live; ChanInt wake; i64 uid, last_v; Response *r; } Sub;
static Sub g_subs[64];
static WMutex g_mu;              // zeroed is an unlocked mutex
static i64 g_vers = 1;

static void bump(void) {         // any mutation ends here
  wmutex_lock(&g_mu);
  g_vers++;
  for (i32 i = 0; i < 64; i++)
    if (g_subs[i].live) { i32 one = 1; wchan_try_send_typed(g_subs[i].wake.c, &one, sizeof one); }
  wmutex_unlock(&g_mu);
}

GET("/api/stream") {
  // …find a free slot, fill it, then:
  return res_sse(stream_run, &g_subs[slot]);
}

static void stream_run(Response *r, void *vp) {
  Sub *s = vp;
  s->r = r; s->live = true; s->last_v = g_vers;
  bump();                        // the newcomer's presence is news; its first frame follows
  WFrame fr;                     // two generations: a frame dies when the next one
  frame_init(&fr, 96 * 1024);    // replaces it (a stream's memory discipline, as a verb)
  for (;;) {
    i64 sig = 0;
    SelCase cs[1] = {SEL_RECV(s->wake.c, &sig)};
    i32 got = select_wait(cs, 1, 15000);        // wake, or heartbeat every 15 s
    if (got == SELECT_CLOSED) break;
    if (got == SELECT_TIMEOUT) { if (!sse_send_text(r, "ping", str_lit("1"))) break; continue; }
    if (g_vers == s->last_v) continue;
    Arena *prev = frame_begin(&fr);             // the fresh generation, made current
    str frame = snapshot_json(prev);
    bool ok = sse_send_text(r, "sync", frame);
    frame_end(&fr, prev);
    if (!ok) break;                             // a dead socket fails on write
    s->last_v = g_vers;
  }
  frame_free(&fr);
  wmutex_lock(&g_mu); s->used = s->live = false; wchan_close(s->wake.c); wmutex_unlock(&g_mu);
  sse_end(r);
  bump();                                       // someone left: presence is news too
}
```

- The client half is `webc_subscribe("/api/stream", "sync", on_frame)`; a frame the
  bridge does not know by name (the `ping` above) is ignored, which is what heartbeats
  want. Decode frames into a private generation arena and `persist()` anything that
  must outlive the frame — the pattern is §13d's `WLive`, spelled out by hand.
- Presence is the subscriber list itself; a stale entry leaves when its next write
  fails, which the heartbeat bounds to one interval.
- A stream parks its task on the channel and on writes; the stall monitor does not
  count it (it is a streamed response, not a stuck handler).

### 7e. A file, and the part of it the client asked for

`res_file(status, path)` is a complete HTTP file response, not a path and a length. It
answers the four questions a file has: *is this the copy you have* (304), *all of it or
part* (200/206), *where does it end* (416), and *may I resume* (`Accept-Ranges: bytes`).

```c
GET("/media/:name") { return res_file(200, path("name")); }   // seekable, resumable
GET("/export")      { return res_download(200, "/tmp/report.csv", "q3 report.csv"); }
```

```
$ curl -D- -H 'Range: bytes=0-1023' /app.wasm
HTTP/1.1 206 Partial Content
Accept-Ranges: bytes
Content-Range: bytes 0-1023/96359
Content-Length: 1024
```

What it decides, and why each one is not optional:

| request | answer |
|---|---|
| `Range: bytes=0-99` | 206, `Content-Range: bytes 0-99/907` |
| `bytes=800-` / `bytes=-100` | 206, to the end / the last 100 bytes |
| `bytes=0-99999` | 206, clamped to the last byte — not an error |
| `bytes=907-` (a 907-byte file) | 416 with `Content-Range: bytes */907` |
| `bytes=0-9,20-29` | 200, the whole file: a server MAY ignore `Range`, and no browser
  asks for `multipart/byteranges` |
| `bytes=99-1`, `bytes=x-y`, `items=0-9` | 200, the whole file: malformed ranges are ignored |
| `Range` on a `POST` | ignored: a range is a GET/HEAD idea |
| `If-None-Match` / `If-Modified-Since` | 304, and the validator wins over the range |
| `If-Range: <current ETag>` | 206; a stale tag or a weak one gets 200, full |
| `Range` + `Accept-Encoding: gzip` | 206, **identity**: never compressed (below) |

Three consequences worth knowing:

- **A 206 is never compressed.** The offsets in `Content-Range` are offsets into the
  bytes the client asked about, so a compressed 206 is a deflate stream with a byte count
  that means nothing; and `mw_compress()` reads a file into memory to gzip it, which would
  turn a 1 KB range request into a whole-file read. The middleware skips 206/416, and
  `res_file()` resolves the range before the middleware sees the response.
- **`If-Range` is what makes a resumed download safe.** A client that lost its connection
  re-asks with the tag it has; if the file changed underneath it the answer is 200 and the
  whole new file, because splicing two versions is worse than a re-download.
- **`res_download()` escapes the name twice.** An ASCII fallback (`"; rm -rf ~` becomes
  `attachment; filename="_; rm -rf ~_"`) and `filename*=UTF-8''…` for names the fallback
  cannot spell — a quote or a semicolon reaching a header is response splitting.

This is also what `webc serve` and `mw_static()` serve, because both go through the same
`res_file()`: a deployment with no nginx in front of it still has a media server.

## <a name="middleware"></a>8. Middleware

```c
typedef bool (*mw_fn)(Request *req, void *ctx, WNext *next);   // false = stop the chain
```

Built in: `mw_logging`, `mw_request_id`, `mw_headers(HeadersCfg)` (the CSP, nosniff, …;
`mw_headers_default()` is `mw_headers((HeadersCfg){0})` and its policy is
`default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src
'self' 'nonce-<per request>' 'wasm-unsafe-eval'` — the nonce is put on every
`<script>`/`<style>` of an HTML response as it is written, and `wasm-unsafe-eval` is there
because without it the browser refuses to instantiate the module `mw_web_assets` serves),
`mw_cors(&(CorsCfg){...})`, `mw_body_limit(bytes)`, `mw_timeout_ms(ms)`,
`mw_auth_bearer(token)`, `mw_recover`, `mw_static(prefix, dir)`,
`mw_static_spa(prefix, dir)`, `mw_metrics`, `mw_dev(port)`, `mw_compress()`,
`mw_document()` and `mw_not_found(file)` (both below).

### 8a. The policy is a value: `mw_headers()`

The default CSP is one string with three parts, and a project that needs a CDN in `img-src`
would otherwise splice the header by hand. It is a struct, and `webc.json` reaches the same
struct:

```c
webc_use(mw_headers((HeadersCfg){
    .img_src = (const char *[]){"'self'", "data:", "https://cdn.example.com"},
    .img_src_n = 3,
    .extra = "frame-ancestors 'self'; form-action 'self'",
}));
```

```json
"csp": {"img_src": ["'self'", "data:", "https://cdn.example.com"],
        "script_src": ["'self'", "'wasm-unsafe-eval'"],
        "no_default": false}
```

**One policy, not two.** `webc_main()` installs `mw_headers()` from the `csp` block *after*
`webc_boot()`, so the framework's copy is inside the app's chain and — on the way out — writes
first. A project that registers its own `mw_headers()` therefore replaces the framework's
default rather than stacking beside it: `webc_install_default_middleware()` skips any default
whose `.name` the app already registered (§8b). Without that rule the `HeadersCfg` above
would be built, applied, and then overwritten by the policy from `webc.json` — a CDN list in
C that never reached a browser.

`img_src` and `script_src` replace the default lists (`script_src` without a nonce entry
means the nonce is *not* offered, which is what an app with no inline script wants),
`extra` is appended verbatim, and `no_default` says the app writes the header itself — the
framework then emits none. A handler may still choose the policy for its own response with
`res_header()`; `mw_headers()` defers to a policy that is already there.

**The devtools panel.** `WebC.devtools()` (or `?__webc_devtools` on the URL)
opens a small panel in the corner of the page: the bridge's own numbers — ops, bad
ops, hydrated, anchors, created elements — refreshed live, plus `WebC.why()` as a
mode you arm and click. It is dev.js, not a bridge verb: nothing an app ships knows
it exists.

`webc dev` **widens the policy that is in force** rather than building one of its own, so
whatever the app declared — in C or in `webc.json` — is what a browser sees in development
too, with the reload additions (`'unsafe-inline'`, `ws://localhost:<port>`, `dev.connect`).
That distinction is worth a sentence, because a dev server that builds its own policy from
scratch makes an `img-src` with a CDN in it hold in production and silently
stopped holding under `webc dev`. The dev server widens whatever policy is in force with what live
reload needs (`'unsafe-inline'`, `ws://localhost:<port>`, the origins in `dev.connect`), so a
strict policy costs nothing in development. `res_header_get(res, "Content-Security-Policy")`
reads one back, which is what an app that rewrites its own needs; `wc_csp_build()` is the
builder on its own, for a project that emits the header outside a response.

### 8b. Order: the document is finished before anything compresses it

`mw_document()` is the framework's own last-installed (innermost) middleware, and it exists
because of a bug worth understanding before changing the chain. A page's `<head>` — the title,
the description, the OG tags — is collected in the *request* while the view runs and spliced
into the body on the way out. When that splice ran at the socket, `mw_compress()` had already
turned the body into gzip bytes; searching compressed bytes for `</head>` finds nothing, so the
splice silently did nothing for every client that sends `Accept-Encoding: gzip` — which is
every browser — while `curl` without that header got the head and every smoke test passed.

So the splice happens while the body is still HTML: `mw_document()` runs *inside* the router's
response, before `mw_dev` appends the reload script and before gzip. Two rules follow:

- **Register it last**, so it wraps the route: `webc_use(mw_compress()); webc_use(mw_document());
  ...routes...`.
- **Anything that appends to an HTML body goes outside it.** `mw_dev` does: the reload script
  is added after the head is applied, and it carries the page's nonce itself because the
  policy names the nonce and the dev script is the framework's own inline script.

A project rarely writes the chain at all: `webc_main()` calls `webc_install_default_middleware()`
after `webc_boot()` (compress, dev, request id, headers, logging, recover, body limit, web
assets, static, document, 404), so `webc_boot()` registers only what it adds. **A default is
skipped when the app registered one with the same `.name`** — the singleton rule of §8a — so
"my own logging" or "my own policy" is one `webc_use()` in `webc_boot()` and never a second
log line or a second header. `static` and the rest are additive: `/public` and `/uploads` are
different files, so both are installed.

### 8c. A 404 that is a page

The default answer to a URL nobody routed is a JSON error. A project that wants a real page for
a browser names the file once:

```json
"not_found": "public/404.html"
```

`mw_not_found()` then answers a `404` that accepts HTML with the file's markup inside the app's
own shell — title, theme and nonce included — and leaves everything else alone: a route that
answered `404` with HTML keeps its own page, and a client that sent `Accept: application/json`
still gets the JSON error. A file that is a whole document (`<html …>`) is served as it is.

### <a name="gzip"></a>gzip

**On by default.** `mw_compress()` is the first middleware the framework installs
(`install_default_middleware`), so every response the client said it accepts goes out as
`Content-Encoding: gzip` — static files *and* rendered bodies: a JSON API shrinks by 70%,
a server-rendered page by 60-80%, a wasm module by 58%. `WEBC_NO_GZIP=1` turns it off,
for an app that ships its own compressed bytes or sits behind something that compresses.

It is a normal middleware, so an app may take it out of the default chain and put it
where it wants — in front of the middlewares that answer, so it wraps their response:

```c
webc_use(mw_compress());
webc_use(mw_static("/public", "public/"));
```

What it will not do, and why:

- **Compress what gzip does not help.** Only `text/*`, JSON, JavaScript, XML, wasm and
  SVG are eligible — not `text/event-stream`, not PNG, not woff2. A file under
  `WC_GZIP_MIN` (1 KB) is left alone: the 18-byte wrapper costs more than it saves.
- **Compress a 206.** A byte range is answered from the disk as the identity bytes (§7e):
  `Content-Range` counts bytes of the representation the client asked about, so compressing
  it would both lose the offsets and read a whole file into memory to serve its first
  kilobyte. `206` and `416` are skipped exactly like `204`/`304`.
- **Decide by itself whether the client wants it.** No `Accept-Encoding`, `gzip;q=0` or
  `identity` gets the bytes as they are. `Vary: Accept-Encoding` goes on every response
  that *could* have been compressed, so a shared cache never crosses the two.
- **Reuse a validator across the two encodings.** The gzipped representation carries
  `<etag>-gz`, so `If-None-Match` from a gzip response is answered with a 304, and one
  from an identity response is not answered with a gzip 304.
- **Compress a file twice.** Files are cached by `(path, size, mtime)` — 32 of them by
  default — so a 155 KB module is compressed once and served from memory after that.
  `mw_compress_opts((CompressOpts){.level = 6, .slots = 8})` tunes it; level 9 (default)
  is the smallest bytes, and it is zlib doing it, so it matches `gzip -9`.

Because it is first, it is the **last** word on every body: whatever a middleware or a route
wrote is what gets compressed. That is also why the page's `<head>` is spliced *inside* it
(§8b) — a splice after this one searches gzip bytes and finds nothing.

`webc serve` and `webc dev` both serve through it (§13b), so the bytes you measure in a
browser are the bytes that ship.

The `-lz` on every link line is this feature and only this feature: `webc build` adds it
to the app's link, because the framework's own default chain references the middleware.
zlib is on macOS and on every Linux worth deploying to; a platform without it is a
platform where `WEBC_NO_GZIP=1` does not help, and the fix is a `-lz`-shaped one.

Global middleware runs for unmatched paths too — that is how static files and the dev
reload script exist without being routes.

A chain carries up to `WC_MW_MAX` (32) middlewares, globally, per group and per route,
and the dispatcher runs every one of them — the same number in both places, because a
*registered* middleware that never runs is the worst kind of silent failure. Going over
the limit prints one line naming `WC_MW_MAX` instead of dropping the extra one. The
framework's own defaults use ten of the thirty-two (`compress`, `dev`, `request_id`,
`headers`, `logging`, `recover`, `web-assets`, `static` twice, `document`, and `not_found`
when `webc.json` names a file), so an app registers into an empty argument, not a race.

---

## <a name="database"></a>9. Database

Three drivers open from one URL — sqlite (a path), postgres, and MySQL/MariaDB (§9a5). Handles
below are `WDB *`; the statements are the raw layer. **The ORM is §9a to §9a4** and it is what an
app should be written in: a typed filter, one row, a relation, a page, a tree of rows. Raw SQL is
for the statement that is a report, not for the tenth `select * where id = ?`.

```c
WDB *db = db_open("sqlite:data/app.db");      // or postgres://…, mysql://…
db_insert(db, Todo, &t);                       // INSERT with named params underneath
                                             // (returns bool; db_insert_into(db, T_modelof(), &t, &id)
                                             //  also reports the new key)
WRes r = db_exec(db, q("select * from todos where done = :d", .d = false));
i64 n = row_i64(&r.rows[0], "id");
str s = row_str(&r.rows[0], "title");
```

Lists come back in the order you ask for — `db_all()` has no opinion of its own, which
made a front page look right on sqlite and wrong somewhere else:

```c
VecStory *latest = db_all_ordered(db, Story, "published desc");
VecStory *top    = db_all_where(db, Story, "where views > ? order by views desc", qy);
```

- `:name` placeholders are rewritten to `?` (sqlite) or `$n` (postgres and MySQL); the same
  code runs on all three. The rewrite lexes: a colon inside a string literal, a quoted
  identifier or a comment is left alone, so `to_char(now(), 'HH24:MI:SS')` survives.
- `db_insert(db, T, &t, &id)` reports the new key from every driver: sqlite from its rowid,
  postgres because the statement asks with `returning`, MySQL from the OK packet — which is
  the `returning` capability in §9a5, not three code paths in the app.
- `db_all(db, Model)` returns `Vec<Model> *`.
- **A transaction belongs to a session, and a session to a request.** The first statement a
  request makes pins one connection for that task; every later statement — and any
  transaction — runs on it, and the connection goes back to the pool when the request ends,
  even if the handler panicked. Two requests can therefore never sit inside each other's
  `BEGIN`/`COMMIT`: when they can, a `rollback` undoes another request's committed insert.

  ```c
  tx_scope(db) {                       // BEGIN on this request's own connection
    db_save(Order, &o);
    if (!charge(o.id)) tx_commit_here();   // …or let the scope roll back
  }                                    // no commit → ROLLBACK

  tx_scope(db) { ... tx_commit_here(); }
  ```

  `tx_scope(...) { … }` takes a block. A nested scope is a `SAVEPOINT`, so an inner failure
  can roll back alone (`tx_rollback_here()`), and committing a scope that is not open is a
  failure with a message in `WTx::err`, not a silent `false`: a commit that quietly does not
  commit is how a write vanishes. `tx_begin(db)` / `db_begin`-style handles remain for
  code that must end a transaction in a different function than it started.
- A task that ends inside a transaction has it rolled back and its connection returned (a
  warning names the task): `fail()` and a panic both unwind past the scope's cleanup.
- Migrations: `migrations_apply(&(WMigrate){ .db = db, .dir = "migrations" })`, files
  applied in filename order, recorded in `schema_migrations`. A migration is SQL, so
  it carries the dialect of one database: postgres needs `bigserial` where sqlite
  wants `integer primary key autoincrement`. Put the other spelling next to it as
  `0001_todos.postgres.sql` (or `.sqlite.sql`); it is used instead of
  `0001_todos.sql` when present, and the same name is recorded, so one sequence of
  migrations serves every dialect — `.sqlite.sql`, `.postgres.sql`, `.mysql.sql`. `examples/todo`
  and `examples/accounts` carry all three spellings. A file may carry several statements: a query with no parameters goes out on the
  server's simple protocol, which is the one that takes them, so nothing about the file
  depends on the driver.
- **A migration's column type follows how the field is *stored*, not how it reads.** An enum
  field (`RPlan`, `RKind`) is stored as its **name** — `"balanced"`, `"stake"` — so its column
  is `text`, never `integer`; a `bool` is `boolean` (or `tinyint(1)` on MySQL). SQLite accepts a
  name in an `integer` column and will not tell you, which is how an app can have a postgres
  migration that has never once run: `invalid input syntax for type integer: "balanced"`. Money
  stays integer cents, and an indexed text column is `varchar(190)` where MySQL needs a length.
- The pool opens connections on demand, up to `WC_DB_POOL` (default: the number of
  processors, at most 256), and a task waiting for one parks rather than spinning the CPU.
  Waiting is FIFO and bounded: after `db_pool_config(max, acquire_ms)` milliseconds
  (`WC_DB_ACQUIRE_MS`, default 5000) the request is refused with a message that names the
  pool size instead of hanging until the client gives up. `db_pool_size(db)` reports what
  is open.
- **The postgres driver does not block a thread.** Its socket I/O parks like every other
  fd in the runtime, so a slow query holds a connection, not a processor: 64 concurrent
  `select pg_sleep(0.3)` finish in one wave rather than in batches of `nproc`. A statement
  also *stops* when its request is cancelled — the session carries `req->ctx`, and the
  driver sends a protocol `CancelRequest` for the backend, so `mw_timeout_ms()` can answer
  504 without waiting for a query it already gave up on. That connection is then closed
  and its slot recycled; a lost connection no longer poisons the pool.
- TLS to postgres follows libpq: `?sslmode=disable|prefer|require|verify-ca|verify-full`
  (default `prefer`, or `WEBC_PG_SSLMODE`), with `sslrootcert=` for the verify modes. A
  build without TLS refuses `require` at startup instead of sending a password in the
  clear; managed postgres, which rejects plaintext clients, is the reason this exists.
- A null handle is a refusal, not a crash: `db_insert_into(nullptr, …)` and the model
  helpers return an error naming `DATABASE_URL` instead of dereferencing it.

### 9a. A filter you can read, and one row by one field

Asking for one row by a field the long way round is a query built only to carry the value:

```c
WQuery *qy = q("select 1");             // a statement with no meaning
q_str(qy, "s", slug);                   // whose only job is to hold `slug`
const Section *sec = db_find(Section, "slug = :s", qy);
```

Nothing checks the field name, nothing checks the operator, and you have to know who owns
the query's arena. Three spellings replace it, one per shape of question.

**One field, one value.** The field is checked against the model — a typo panics and lists
the fields that exist instead of returning no rows — and the value is always a bound
parameter:

```c
const Section *sec = db_by(Section, "slug", slug);
VecStory *mine     = db_all_by(Story, "section", str_lit("science"));
i64        n       = db_count_by(Story, "section", str_lit("science"));
```

The value may be a `str`, a `const char *` or an integer; a string literal goes through
`str_lit()` (`_Generic` does not decay an array). Each takes a `WDB *` first when you are
not using the default handle: `db_by(db, Todo, "id", 3)`.

**More than one condition.** `WWhere` names the field in the macro, so the compiler checks
it, and the primary key column comes from the model, not from a string:

```c
WWhere w;
ww_init(&w, nullptr);
ww_eq_str(&w, Story, section, value);
ww_order(&w, Story, published_at, true);
ww_limit(&w, 2, 0);                     // the query cuts; the caller stops truncating

VecStory *list = db_all_w(db_default(), Story, &w);   // rows, ordered and limited
Story    *one  = db_one(db_default(), Story, &w);     // the first row, or null
i64       n    = db_count_w(db_default(), Story, &w); // how many, without reading them
ww_free(&w);
```

`ww_or_begin()` / `ww_contains()` / `ww_or_end()` make one question out of several columns
instead of filtering the whole table in memory:

```c
ww_or_begin(&w);
ww_contains(&w, Story, title, qtext);   // lower(col) like lower(:p) escape '!'
ww_contains(&w, Story, dek,   qtext);
ww_or_end(&w);
```

`db_increment(Story, st, views)` adds one to one column in one statement — the alternative,
`st->views += 1; db_save(Story, st);`, rewrites the whole row (body and all) to change a
counter.

Set `WEBC_TRACE_DB=1` to log every statement with its row count and duration.

### 9a2. Relations: one declaration, one query, no query in the view

A story has a section, and the section is a row. Declare it next to the models:

```c
WEBC_MODEL_REF(Story, section, Section, slug)   // "Story.section holds a Section.slug"
```

Fetch the whole list once and link it:

```c
VecStory *all = db_all_ordered(db_default(), Story, "published_at desc");
prefetch(all, Story, section);          // one extra query, for every row at once
```

Read it in the view, server or browser half, with no query at all:

```c
const Section *sec = ref(Story, st, section);   // the row, or null if nobody prefetched
```

The links belong to the request (they live as long as its arena does), and `prefetch` is one
`select … where slug in (?, ?, ?)` however long the page is. A relation nobody prefetched
reads as `null` rather than issuing a query per row — the N+1 that is invisible until the
page is slow. `WEBC_MODEL_REF` is pure data, so it belongs in the shared header with the
models; only `prefetch` needs a database. `prefetch_on(db, vec, T, field)` is the same on a
handle you own (a test).

Postgres speaks protocol v3 with SCRAM-SHA-256 authentication; there is no libpq.
It is tested against a real server: `make pg-test` starts a throwaway postgres in
docker, runs `tests/db_parity_test.c` (the same story through every driver it can
reach, compared field by field) and both example suites against it, then removes the
container. `make mysql-test` is the same for MySQL 8 (§9a5). Without docker but with a server, set `WEBC_PG_URL`. An app's own tests
run on postgres with `WEBC_TEST_DB=postgres://… webc test`.

### 9a3. The other direction: a section's stories, still one query

`WEBC_MODEL_REF` answers "what is this row's section" with one row. The reverse question has
its own declaration, because it is a different question with a different result:

```c
WEBC_MODEL_REF(Story, section, Section, slug)     // story  -> its section
WEBC_MODEL_MANY(Section, stories, Story, section) // section -> its stories
```

The second line names the same relation, read backwards, and generates a typed accessor —
`refs(Section, sec, stories)` returns a `VecStory *`. Prefetch it for the page the way you
prefetch the single direction:

```c
VecSection *all = db_all_ordered(db_default(), Section, "name");
prefetch(all, Section, stories);            // ONE query, however many sections
for_each(sec, *all) {
  for_each_ptr(st, *refs(Section, sec, stories)) el("li") { text(st->title); }
}
```

That is one statement for the sections and one for all their stories — not one per section.
A page of 2 000 sections chunks the owner keys in groups of 512, so the parameter count never
runs into a server's ceiling. A collection nobody prefetched reads as *empty*, and logs
`relation read without a prefetch` once, naming the model and the field: an empty list that
was never fetched is the failure that looks like an empty section, and silence about it is how
a page loses its rows.

Hops chain left to right — each one is a query on the rows the previous hop found:

```c
prefetch(all, Section, stories, author);    // 2 statements: stories, then their authors
```

Three hops is the limit; deeper is a loop in the app, because a path string is a second query
language nobody asked for.

### 9a4. Questions about a relation, a page, and a whole tree

Filters reach through a relation without changing the shape of the result (so the rows still
read back into models), sort by a related column, and aggregate:

```c
WWhere w; ww_init(&w, nullptr);
ww_in_ref(&w, Story, section, (WWhere *)section_filter);   // section in (select …)
ww_order_ref(&w, Story, section, Section, name, false);    // order by the section's name
db_sum(db_default(), Story, views, &w);                    // also min, max, avg, count
```

An aggregate answers over the *conditions*: an `order by` and a `limit` in the same filter are
dropped, because a sum of the first page is a different question and the wrong answer looks
plausible.

A list that is not the whole table is three fields and two statements:

```c
WPager pg = {.page = 2, .limit = 20};
VecStory *page = db_page(db_default(), Story, &w, &pg);
// pg.total, pg.pages, pg.has_prev, pg.has_next — a page past the end clamps to the last one
```

And a row that owns rows is written as one thing:

```c
i64 n = db_save_all(db_default(), Order, &order, items, &n);  // FK filled from order.id
db_delete_tree(db_default(), Order, &order, items);            // children first
```

`db_save_all` takes the collection, not a field name: the `WEBC_MODEL_MANY` declaration is what
says which column holds the parent's key, so there is exactly one place the relation is
described. It joins an open transaction or opens its own, so a failure takes the whole tree
with it.

### 9a5. The third dialect: MySQL and MariaDB

`mysql://user:pass@host:3306/db` and `mariadb://…` open the same driver, which speaks the wire
protocol directly — no libmysqlclient, no `.so` to install:

```c
// DATABASE_URL=mysql://app:secret@127.0.0.1:3306/app
// ?ssl-mode=prefer|require|verify-ca|verify-full, ?ssl-ca=/etc/ca.pem
```

Authentication is `mysql_native_password` (SHA-1 scramble) and `caching_sha2_password` (the
SHA-256 fast path, with full authentication only over TLS). `sha256_password` requires TLS;
`ed25519` and GSSAPI are refused *by name* with the two alternatives spelled out, rather than
failing as a bad password. There is no RSA public-key exchange on purpose: a client that would
hand its password to whatever key the server offers is a client that gives the password away
to a man in the middle.

A statement with no parameters goes out as `COM_QUERY` and may carry a whole migration file; a
statement with parameters is prepared (`COM_STMT_PREPARE`) and executed as binary rows, cached
per connection. Sessions are set to `STRICT_TRANS_TABLES`, `utf8mb4` and `time_zone='+00:00'`,
so a too-long value is an error (422) instead of a silent truncation, and a `datetime` means
the same thing everywhere.

What differs is **declared, not branched on**: each driver is a table of four flags, and
every statement above reads them instead of asking which server it is talking to.

| flag | sqlite | postgres | mysql | what an app sees |
|---|---|---|---|---|
| `returning` | no | yes | no | `db_insert` reports the new key everywhere: `last_insert_rowid()` here, `insert … returning` there, the OK packet's `last_insert_id` on MySQL |
| `offset_needs_limit` | no | yes | no | `ww_limit(&w, 0, 40)` becomes `limit 9223372036854775807 offset 40`; the largest *signed* value, because MySQL documents the unsigned one and sqlite answers *datatype mismatch* |
| `ident_quote` | `"` | `"` | `` ` `` | a model with a `rank`, `groups` or `order` field is quoted, so a reserved word is a column name and not a syntax error; a name that is not a plain identifier (`public.users`, a `.sql()` override) is passed through |
| `multi_statement` | yes | yes | yes | one call may carry a whole migration file |

`WC_DB_DRIVERS[]` is that set as data (`{"sqlite", "postgres", "mysql"}`), which is what the
migration runner reads when it looks for a `<name>.mysql.sql` twin. `driver_for()` refuses an
unknown `scheme://` **at open**, naming the URLs this build speaks. Opened as a sqlite file
named after the URL, the first sign of the mistake is "my tables are empty" three screens later.

Driver errors carry a *class* (`WDBE_CONFLICT`,
`WDBE_BAD_REFERENCE`, `WDBE_NOT_NULL`, `WDBE_TOO_LONG`, `WDBE_CHECK`, `WDBE_DEADLOCK`,
`WDBE_TIMEOUT`, `WDBE_AUTH`, `WDBE_LOST`) mapped from the server's own codes, so a duplicate is
409 on every driver without searching an error message for the word "unique". On MySQL the
codes are errno: `1062/1586/1451` conflict, `1452/1216/1217` bad reference, `1048/1364` not-null,
`1406/1264/1292` too long, `3819/4025` check, `1213` deadlock, `1205/3024` timeout, `1045/1698`
auth, `2006/2013/1053` lost. The class picks the status: 409 / 400 / 422 / 422 / 422 / 503 / 503 /
500 / 500. An error a driver cannot classify stays `WDBE_NONE` and answers 500 with the server's
own sentence, deliberately — a guess at 409 is worse than a 500 that says what happened.

Two things are worth knowing because they are the same value in a different shape:

- An aggregate is `DECIMAL` on MySQL, not the integer postgres returns. `sum(int)` arrives as
  `DECIMAL(32,0)` and is read back as an integer, `avg(int)` as a real — the digits decide, so
  `db_sum`/`db_avg` answer the same numbers on all three servers.
- A `like` pattern is escaped with `!`, not with a backslash: a backslash is an escape
  character *inside a string literal* on MySQL, where `escape '\'` is a syntax error. The
  query builder writes `escape '!'`, so a search box works on every dialect.

`make mysql-test` starts a throwaway MySQL 8 in docker, runs `tests/db_parity_test.c` across
all three dialects, `tests/db_mysql_test.c` against the server, the ORM suites (they read
`WEBC_TEST_DB`) and both example apps, then removes the container. It has run green on MySQL
8.4 and MariaDB 11.8. Without docker but with a server, set `WEBC_MYSQL_URL`; without either,
every MySQL test skips with the reason in its name. An app's own suite runs against it with
`WEBC_TEST_DB=mysql://… webc test`, and migrations may carry a `<name>.mysql.sql` twin next to
`<name>.postgres.sql` — `examples/todo` and `examples/accounts` each have one, which is why
their suites run on the third dialect too.

A request with a deadline gets its cap pushed down to the server too: the driver sets
`max_execution_time` (MySQL) or `max_statement_time` (MariaDB) from the request's remaining time,
and a statement still running when the handler gives up is killed with `KILL QUERY <id>` on a
second connection — two halves of one promise, because the cap is per statement and the kill is
now. A connection the server dropped (`2006`, `2013`, `1053`) is classed `WDBE_LOST` and retired
instead of being handed to the next request.

The URL may also name a unix socket, for a container whose socket is not in a URL's authority
section: `mysql://user:pass@localhost/app?socket=/run/mysqld/mysqld.sock`.

The container the target starts passes `--mysql-native-password=ON`, because that is the one
auth plugin a non-TLS build can speak: MySQL 8.4 only loads it on request, and root otherwise
authenticates with `caching_sha2_password`, whose full path wants TLS. The TLS half of the
driver is checked from a TLS build, which keeps its binaries in `build/tests/`:

```sh
make WEBC_TLS=openssl test
WEBC_MYSQL_URL="mysql://root:webc@127.0.0.1:3306/app?ssl-mode=require" \
  ./build/tests/db_mysql_test
```

---

### 9b. Fixtures and schema drift

Two things every model-first project needs and neither of which needs a generator: put
some rows in a table, and find out when the models and the tables have drifted apart.
Both run inside the application binary, because that is the only process that has the
models.

```c
// src/boot.c — a demo or a test loads content/<table>.json (an array of objects)
void webc_boot(void) { seed_all("content", false); }   // false: only when the table is empty
```

```sh
webc seed            # run the app's boot, then load content/<table>.json for every model
webc seed --reset    # ... deleting the rows first
webc check           # build, test, doctor, boot, schema, routes, consistency
webc check --verbose # ... and every finding, with the one-line fix
```

The last line of `webc check` is the **consistency** pass: the mechanical questions a
reviewer asks, each one a grep with a rule narrow enough not to lie.

```
  consistent    2 notes (0 errors) — webc check --verbose lists them
  note ./src/server.c:63
    a query built only to carry a bound value
    fix: db_by(Model, "field", value) — one field, one value, no carrier
```

| what it looks for | severity |
|---|---|
| `q("select 1")` — a carrier query (§9a) | note |
| `fmt("/a/%s", …)` where a model already names `/a/:slug` | note |
| a month or weekday table, `/ 220` for a reading time (§4b) | note |
| `bind("x")` with no `named("x")` — a click that can never fire | **error** |
| a file that renders pages and sets no title | note |

Comments are skipped: prose that mentions an idiom is documentation, not a use of it. Notes
do not fail the check; only an error does.

`seed_from_json(Model, "content/stories.json")` is the single-table form and returns the
rows inserted; a second call on a non-empty table inserts nothing, so a seed in `webc_boot`
does not double the site. Rows are parsed and validated by the model, so a fixture that
would fail an HTTP request fails the seed and says which field.

**Schema drift** is the failure this prevents: a model with a column the table does not
have surfaced as a 500 on the first read of that column. `webc_schema_report(db, out)`
compares every registered model (every `WEBC_MODEL` registers itself) with
`select * from <table> where 1 = 0` — the column names, every driver, no pragma — and
prints what is missing *and the statement that fixes it*:

```
  schema         sqlite, 3 models
  Story          .views            missing from "stories"   → alter table stories add column views INTEGER;
  Story          table "stories" is missing      → run your migrations, or: create table …
  schema         DRIFT: 1 table(s), 1 column(s) missing
```

`webc check` runs it; a model marked `WEBC_MODEL_INTERNAL` (the framework's own `RowId`)
owns no table and is skipped — and `WEBC_ROW` is that same macro with a name that says why
(§4).

### 9b2. The migration the models imply: `webc migrate diff`

The report above ends with the fix, and the tool can write it. `webc migrate diff` runs the
app (the only process that has the models, exactly like the schema check), turns the same
comparison into statements, and prints them:

```sh
webc migrate diff                 # what is missing, as SQL, on the terminal
webc migrate diff --write         # migrations/0004_schema.sql — written, not applied
webc migrate diff --write cover   # … named 0004_cover.sql
```

```sql
alter table categories add column cover_id INTEGER references media_assets(id);
alter table categories add column pinned INTEGER not null default 0;
-- TODO categories.sort_key is not null and has no default: backfill the table and
--   add the constraint by hand, or give the field `.def`
-- note: site_settings.brand_mark is in the table and not in the model (leftover)
```

It is **additive on purpose**: a column, a table, an FK reference, a default. It never drops or
narrows one — sqlite cannot drop a constraint without copying the table, and a tool that guesses
at a data migration is a tool that loses rows. What it cannot infer it writes as a `-- TODO`
line, which is a sentence a person has to answer before the file is applied. Columns in the
table that the model does not have are notes, not statements.

### 9b3. The CRUD every table needs: `webc scaffold`

`webc scaffold Post` writes the four routes a table always gets — list with a search box and a
window, new, edit, save, delete — into a file the app owns:

```
src/admin_post.c        the views and the routes (webc/form.h draws the form)
tests/admin_post_test.c one test per route, with the test client (§14b)
webc.json               a line in the "server" set
```

What it reads is the model, from the app's own descriptor (`WEBC_MODEL_DUMP`), so the field
list, the types, the required bounds and the searchable column are the ones the app actually
declares. What it writes is app source: no hidden runtime, no generated file you cannot edit,
and nothing that exists is overwritten unless `--force`. The model is *not* touched — a
scaffold that rewrites your struct is a generator, and generators are the thing this framework
does not have. If `src/` has a header declaring the model it is included; if the model lives in
a `.c`, the declaration is copied into the generated file (the registry keys on the name, so
two copies of one model are one model) with a comment saying a header is the tidier answer.

Then `webc check` passes on what it wrote, which is the only claim worth making: it builds,
its own tests run, and `webc lint` is clean.

---

### 9c. Redis: queues, counters, pub/sub, a lock

Redis is built in, the way postgres is: `<webc/server.h>` declares it, `libwebc.a` implements
it, and there is nothing to install and no library to link. The client speaks RESP3 over the same
parked sockets every other wait in the framework uses, so a slow Redis parks a task instead of
holding a thread — `src/redis.c`, no hiredis, the same shape as the postgres driver.

```c
redis_set("visits:home", "1");                       // a key
i64 n = redis_incr("visits:home");                   // 2 — the counter, atomically
redis_set_ex("session:abc", token, 1800 * 1000);     // … with a TTL in milliseconds
str v = redis_get("session:abc");                    // .p == nullptr when the key is absent
redis_hset("user:7", "theme", "dark");               // hashes
redis_lpush("queue:mail", msg);                      // lists
redis_sadd("online", "user:7");                      // sets
redis_zadd("board", score, "ada");                    // sorted sets
redis_publish("chat", payload);                      // pub/sub
```

One URL configures it, and it is read the first time the framework asks for a connection, so an
app that never touches Redis never opens a socket:

```sh
REDIS_URL=redis://:password@127.0.0.1:6379/0
REDIS_URL=rediss://cache.internal:6380/1?pool=8&timeout_ms=200   # TLS
```

| variable | default | what it decides |
|---|---|---|
| `REDIS_URL` | — | `redis://[user:pass@]host[:port][/db][?param=value]`, or `rediss://` for TLS |
| `WEBC_REDIS_POOL` | one per worker | connections open at once |
| `WEBC_REDIS_ACQUIRE_MS` | 5000 | how long a call waits for a free connection before it fails |
| `WEBC_REDIS_TIMEOUT_MS` | 500 | one command, start to finish |
| `WEBC_REDIS_NAME` | `webc` | `CLIENT SETNAME`, and the namespace the query cache writes under |
| `WEBC_REDIS_TLS`, `WEBC_REDIS_CA` | — | `verify`/`insecure`, and a CA file |

`redis_open(url)` gives a handle you own (`redis_close` frees it); `redis()` is the process
default, opened from `REDIS_URL` on first use. **A bad URL is `nullptr`; an unreachable server is
a handle whose every call fails.** That asymmetry is deliberate: a typo must be caught at boot
with a sentence naming it, while a cache that is down must be something the app can survive.

```c
if (!redis_ok(redis()))            // configured and the last call worked
  log_warn("redis is down", { "why", redis_error(redis()) });   // never the password
```

When a command is more than the verbs: the builder is binary-safe and typed, because
`redis_set(k, v)` with `const char *` cannot carry a NUL and cannot tell an integer from text.

```c
WCmd *c = cmd(r, "SET");          // arena-owned, recycled from a small ring
cmd_cstr(c, key);
cmd_raw(c, bytes, len);           // a value with a NUL in it
cmd_i64(c, 42);                   // no snprintf at the call site
WReply rep = cmd_run(c);          // spends the handle; reply_free() when done
if (reply_is_err(&rep)) log_warn("redis said no", { "why", reply_err(&rep) });
```

A reply is a value tree, not a string: `WR_STATUS`, `WR_STR`, `WR_INT`, `WR_DOUBLE`, `WR_BOOL`,
`WR_ARRAY`, `WR_SET`, `WR_MAP`, `WR_NIL`, `WR_ERROR` — `reply_at`, `reply_val(map, key)`,
`reply_double`, `reply_bool`. RESP3's attribute, push and verbatim frames are understood rather
than skipped, so a `HELLO 3` connection does not desynchronise. **A transport failure is a
`WR_ERROR` reply**, so a caller reading `reply_is_err` cannot mistake "the server is gone" for
"the key is absent".

**`WITHSCORES` is not flat.** On a `HELLO 3` connection `ZRANGE`/`ZREVRANGE … WITHSCORES` answers
an array of `[member, score]` *pairs*, not `member, score, member, score`. Reading `reply_at(rep, 0)`
as the member gets the pair itself — which is neither a string nor an integer, so every number comes
back `0` and nothing raises. Read `reply_at(pair, 0)` and `reply_at(pair, 1)`, and keep a branch for
the flat form if the same code may meet a RESP2 server:

```c
for (i32 k = 0; k < reply_len(&rep); k++) {
  const WReply *e = reply_at(&rep, k);
  i32 num = reply_int(reply_at(e, 0));    // pair: [member, score]
  i64 hits = reply_int(reply_at(e, 1));
}
```

N commands, one round trip — the reason a cache and a leaderboard are usable at all:

```c
WPipe *p = pipe_new(r);
for (i32 i = 0; i < n; i++) pipe_add(p, "INCR", key);
i32 got; WReply *reps = pipe_run(p, &got);   // in order, arena-owned
pipe_free(p);
```

Subscriptions own a dedicated connection and one green task, and reconnect with a doubling
backoff (100 ms → 30 s), resubscribing on the way back:

```c
static void on_msg(str chan, str payload, void *arg) { /* runs on the sub's task */ }
WSub *s = redis_subscribe(r, str_lit("chat"), on_msg, nullptr);
…
redis_unsubscribe(s);   // waits for the task; a subscription that outlives its handler is a leak
```

Redis pub/sub is at-most-once: a message published while the subscription is reconnecting is
lost, and anything that must not be lost belongs in a list (`redis_lpush` + `lpop`), not in a
channel.

What this client refuses, with a sentence rather than a silent redirect: **Sentinel and
Cluster** (one server, one URL — a client that follows `MOVED` needs the whole topology story),
**EVAL/Lua** (code in the cache is not something a WebC app should be able to do), **Streams**
(a queue with consumer groups is a data model, and this framework has one), **CLIENT TRACKING**,
and Modules. `rediss://` in a build
without `-DWEBC_TLS=openssl` refuses at open, naming the flag: a plaintext connection to a port
somebody believes is encrypted is worse than a refusal (§16.5).

---

### 9d. The query cache: results in Redis, invalidated by generation

Every statement in the framework goes through one function, `WRes db_exec()`, so the cache is
one function too. It stores the *result set* — the same `WRes` a driver returns — and replays it,
which means `db_all`, `db_one`, `db_by`, a view and a test all get a cache without knowing it.

```sh
REDIS_URL=redis://127.0.0.1:6379/0
WEBC_QUERY_CACHE=1          # off unless this is set: a cache nobody asked for is a surprise
```

Nothing to call. With those two variables a `select` is served from Redis, a write invalidates
it, and the app keeps its shape:

```c
VecStory *top = db_all_ordered(db, Story, "published desc");   // first: the database
VecStory *again = db_all_ordered(db, Story, "published desc"); // second: Redis, same rows
Story s = { .title = … }; db_insert(db, &s);                    // bumps stories' generation
VecStory *fresh = db_all_ordered(db, Story, "published desc");  // a miss again: the write was seen
```

**What is never cached**, because each of these is a wrong answer waiting to happen:

| skipped | why |
|---|---|
| any write (`insert`, `update`, `delete`, `create`, `drop`, and any verb nobody classified) | they invalidate; caching a write is not a thing |
| a read inside a transaction | it must see its own writes, and it may be rolled back |
| a statement with a volatile function (`now()`, `random()`, `randomblob`, `last_insert_rowid`, …) | the answer is not a function of the inputs |
| a `for update` / `for share` read | it takes locks; replaying it takes none |
| `/* no-cache */ select …` | the author says no |
| anything inside `nocache { … }` | the scope says no |
| a statement with no tables (`select 1`) | nothing to invalidate it with |

`/* cache */ select …` overrides the volatile rule for a statement you measured, and a number in
the hint is that statement's TTL: `/* cache 250 */`.

Invalidation is a **generation counter per table**, not a tag set and not `SCAN`: a write does
`INCR webc:{<app>}:g:<table>` (one hop, O(1), atomic across processes), and a cached read's key
carries the generations it was read under — `webc:{<app>}:q:<digest>:all=7,stories=12`. When the
counter moves the key changes, so the old entry is simply never read again and dies by TTL.
The app can say so itself, for the changes the database cannot see:

```c
cache_bust(Story);   // qcache_bust(Story_modelof()) — after a background job rewrote rows
qcache_flush();      // every entry, everywhere: the big hammer, for a migration
qcache_ttl(Story, 5000);   // this model's rows go stale on their own schedule
```

| variable | default | what it decides |
|---|---|---|
| `WEBC_QUERY_CACHE` | off | the whole feature |
| `WEBC_CACHE_TTL_MS` | 30000 | how long an entry lives (the backstop for what counters cannot see) |
| `WEBC_CACHE_TTL_JITTER` | 10 | percent, so a thousand entries do not expire on the same second |
| `WEBC_CACHE_MAX_BYTES` | 262144 | a result bigger than this is served but not cached |
| `WEBC_CACHE_MS` | 5 | the cache's own budget per statement; over it, the answer comes from the database |
| `WEBC_CACHE_GEN_MS` | 0 | 0 is exact: every cached read checks the counters (two hops). A window reuses the counters it last read (one hop) — and stops seeing *other processes'* writes for that long |
| `WEBC_CACHE_BREAKER` | 1 | after three consecutive failures, stop asking for a few seconds |
| `WEBC_TRACE_CACHE` | off | one line per statement: `[cache] select … -> hit` |

**Redis failing never fails a request.** Every failure inside the cache is a miss: the statement
runs, the page renders, and the cache tries again later. The ladder is a per-statement budget
(`WEBC_CACHE_MS`), a per-command timeout (`WEBC_REDIS_TIMEOUT_MS`), a breaker, and one log line
per transition — measured by `tests/qcache_test.c`, which kills the server mid-suite and asserts
five reads still answer.

The one thing this is not: **an HTTP cache**. It caches result sets, keyed by the statement and
its bound values, not responses — a page that renders from two queries is two entries, and a
page whose body depends on the session is nobody's business here.

What it costs, measured on this tree (`make bench-cache`, `WEBC_QUERY_CACHE=1`, loopback Redis,
sqlite in process):

| statement | database | cache hit | |
|---|---|---|---|
| a 20-row select on a 200-row table | 8 µs | 270 µs | *slower*: a hit costs two round trips and a parse, so a query cheaper than that is cheaper un-cached |
| a three-way self-join over the same table | 119 ms | 0.27 ms | 440× |

That table is the honest summary: the cache is for the statements that hurt, and on sqlite in
process almost nothing hurts — the win arrives with a real database over a network, a big join, an
aggregate over a million rows. `WEBC_CACHE_GEN_MS` halves a hit (0.27 ms → 0.14 ms) by dropping
the counter check. A hit is visible in `/debug` and in
`qcache_stats()`:

```
  cache          on — redis 127.0.0.1:6379/0
  entries        hits 1204  misses 37  stores 37  bumps 12  errors 0
  skips          tx 8  write 61  volatile 4  hint 0  breaker 0  large 0  off 0
```

Metrics: `webc_cache_hits_total`, `webc_cache_misses_total`, `webc_cache_stores_total`,
`webc_cache_bumps_total`, `webc_cache_errors_total`, `webc_cache_breaker_open`,
`webc_redis_commands_total`, `webc_redis_errors_total`, `webc_redis_command_ms`,
`webc_redis_conns`, `webc_redis_pool_waits_total`.

---

## <a name="auth"></a>10. Auth

Nine functions, one header, no session table.

```c
Text hash = auth_hash(password);                 // pbkdf2-sha256$210000$salt$hash
if (!auth_verify(stored, password)) return 401;  // constant-time, re-derives with the
                                                 // iteration count in `stored`
str  h2   = auth_hash_password(a, password_str); // the `str` flavour, into an arena
str  h3   = auth_hash_password_iters(a, password_str, 600000);
auth_set_session(&res, auth_secret(), account.id);   // signed cookie, one week
auth_clear_session(&res);                            // logout: expiring cookie
i64  uid  = auth_require_session(req, auth_secret());  // 0 when absent, invalid, expired
bool ok   = auth_get_session(req, auth_secret(), &uid); // the same, with the boolean
```

Two flavours on purpose: a model field and a form value are `Text` (`const char *`), so
`auth_hash`/`auth_verify` are what an app calls; `auth_hash_password`/`_iters` take a `str`
for code that already holds a pointer and a length. The stored value is self-describing —
`pbkdf2-sha256$<iters>$<salt-b64>$<hash-b64>`, 16-byte salt, 210 000 iterations by
default — so a raised cost is a new hash and an old one still verifies.

The cookie is `<uid>.<expiry>.<hmac>`, `HttpOnly; SameSite=Lax`, named by
`WEBC_SESSION_COOKIE` (`"session"`), valid `WEBC_SESSION_TTL_S` (604800) seconds. Signed,
not encrypted: the id and the expiry are readable and cannot be changed without breaking
the HMAC, which is what makes a session table unnecessary. A deleted account is a lookup
that returns nothing, so a handler that uses the id also checks the row.

`auth_secret()` resolves once per process, first match wins: `auth_secret_set(value)` (what
a test pins), `WEBC_SECRET`, the dev loop's `data/.webc-secret` file, then a random
per-process secret **with a `LOG_WARN` that sessions will not survive a restart**.
`auth_secret_set` is also the only way to make cookies from two processes agree.

`examples/accounts` is the worked app (signup, login, logout, a guarded page, and a test
suite that mints the cookie and sends it back); the guide is
[the accounts guide](../website/src/content/guides/auth.md).

---

## <a name="signals"></a>11. Signals

```c
state(int, count, 0);           // get(count) / set(count, v)
computed(int, doubled) { return get(count) * 2; }
effect(log_it) { printf("%d\n", get(doubled)); }
batch { set(count, 1); set(count, 2); }   // one effect run
```

- Writing the same value does not re-run effects (byte comparison).
- Effects run in topological order, once per batch.
- `signal`, `computed` and `effect` declare file-scope statics (they define functions),
  so they do not work inside a function body.
- That makes them **one per translation unit**: right for app-wide state, wrong for a
  view that appears twice on a page. For that there is the `_in` family below, and for a
  list there is `state_own` (§11c).

Three declarations, one rule between them — *what is the state one of?*

| declaration | one value per | you reach it with | fits |
|---|---|---|---|
| `state(T, n, init)` | page (translation unit) | `get(n)` / `set(n, v)` | the app's own state |
| `state_in(T, n, init)` | `WInst` you create | `get_in(&inst, n)`, or `get(n)` inside `with_inst` | a view mounted N times by *your* code |
| `state_own(T, n, init)` | render position | `get(n)` / `set(n, v)` | a row of a list, a mounted view's own copy |

### 11a. What re-renders a view (and what does not)

The one thing every client author asks first, because the answer is not "the signal":

> **A render follows every action and every host callback.** The bridge re-renders each
> mounted view — and diffs it — when control returns to it: after a click, after a typed
> field, after a `webc_later` timer fires, after a fetch or an SSE frame settles, after a
> `push_url`-driven nav. Writing a signal does not by itself schedule a render; the flush
> point is the bridge boundary.

So an action never has to "touch the view":

```c
action(toggle, Todo *t) { t->done = !t->done; }   // the view runs again when this returns
```

There is no `tick` signal to bump, and a `set(tick, get(tick) + 1)` is a write nobody
reads — the render would have happened anyway. What a render *costs* is a different question: the whole
view runs again, so keep expensive work out of it — that is what §12b2's jobs are for.

The exception is the one §12j adds: a node the view declared as **live** (`text_live`,
`attr_live`, `class_live`, `show_live`) is written in place when its signal changes, and an
action marked `action_live` skips the render entirely. Everything structural — `if`,
`for_each`, `part`, `markup()`, a new node — still needs the render.

`webc check --wiring` also names the trap this rule creates for lists — a live binding on a
page-wide signal **inside** a `for_each` (every row then shows the same value, which is what
`state_own` in §11c is for) — and it knows `state_own`, so the never-read rule below still
applies to it.

`webc check --wiring` notes a signal that a write feeds and nothing else reads, because that
is exactly what a leftover `tick` looks like: `set(tick, get(tick) + 1)` *reads* the signal
it writes, which is why a plain "never read" rule misses the idiom the framework teaches
(`set(tick, get(tick) + 1)`). The check counts a read that happens on a line with no write of
the same signal, so
the self-feeding write is not mistaken for a reader.

### 11b. The store: frames in, generations out

A live document arrives as frames (a seed, then a stream), and each frame's strings
must die when the frame two later replaces it. `WStore` owns that rotation — the
~100 lines every streaming client would otherwise write first:

```c
static WStore room;                                        // once
store_init(&room, BoardDoc_docof(), sizeof(BoardDoc));

BoardDoc *d = store_read(&room, frame);  // NULL = a bad frame; state unchanged
// d lives until the frame after next; ref() reads find it (wc_ref_epoch is set)
```

`store_cur()` is the live document between frames, `store_arena()` the generation
it lives in; `store_free()` returns everything. The server twin is `WFrame` (§7d).

### Per-instance signals

The same declarations, but the state lives in a `WInst` you own, so the same view can
exist any number of times:

```c
state_in(int, count, 0);                       // get_in(&inst, count) / set_in(&inst, count, v)
computed_in(int, doubled) { return get(count) * 2; }
effect_in(log_it)        { printf("%d\n", get(doubled)); }

view(Card) { el("span") { text("%d", get(count)); } }   // get(): the ambient instance

WInst side, page;                               // two cards, two counts
winst_init(&side, nullptr);                     // nullptr = heap; an arena also works
winst_init(&page, nullptr);
set_in(&side, count, 3);
with_inst(&side) { html1 = wcomp_html_ctx(&c1, view_Card, nullptr); }
with_inst(&page) { html2 = wcomp_html_ctx(&c2, view_Card, nullptr); }   // renders 0
mount_in(log_it, &side);                        // effects are started per instance
winst_free(&side);                              // kills its nodes; nothing left behind
```

- `get` / `set` reach the instance bound by `with_inst`, and **panic** outside one:
  a per-instance read that cannot know which copy it means is a bug, not a default.
  Where there is no ambient instance — an action, a test, the other card — name it:
  `get_in(&inst, count)`. There is no second spelling: `get` reads a signal whether it
  lives in the process or in an instance, and picks the right one.`
- Instances are values. `winst_init(&x, nullptr)` is heap-backed and `winst_free`
  returns every byte; `winst_init(&x, arena)` allocates from the arena and needs no
  free, but must not outlive it.
- `CELL(T)` + `cell_init` / `cget` / `cset` is still there for one signal inside a
  struct you already have. `WInst` is for a whole view's worth of state.

---

### 11c. Per-row state: `state_own`

A list is where local state lives — a row's open flag, a row's draft text, a row's
quantity — and a row has no name to hang a `WInst` on. `state_own` is the declaration whose
owner is the **position that drew the node**: the `for_each` row, or the mounted view. (A
view rendered into an email is a third — one render, its own copy, discarded after — so a
view with per-row state can be both a page and a mail part.)

```c
state_own(i64, qty, 0);
state_own(bool, open, false);
state_own(Text, label, "");
computed_own(i64, twice) { return get(qty) * 2; }

view(Line) {
  el("tr") {
    el("td") { text_live("%lld", qty); }          // this row's value, live
    el("td") { bind_str(label); }                 // this row's field
    el("button") { named("bump"); text("+1"); }
  }
}
// client: bind("bump", "click", bump);
action(bump) { set(qty, get(qty) + 1); }          // the row the clicked button drew
```

- **Identity is `(position, declaration)`.** Two rows are two values; the same view mounted
  twice is two values, with no plumbing at all. A row that moves in a keyed list keeps its
  value, because the position is the key.
- **A live binding may target it.** `text_live`, `attr_live`, `class_live` and `show_live`
  all take a `state_own` name — one write, one op, the row the action belongs to and no
  other. This is the part `state_in` cannot do (a `WInst` signal is refused, §12j) and the
  part that makes a 1 600-row table interactive.
- **An action resolves it through the node it came from.** The bridge knows which element
  the event carried a hook for, and pushes that element's owner before the action runs; so
  `get(qty)` means "the row of the button that was clicked", not "the row rendered last".
  `get_N()` / `set_N()` are the same thing spelled out, for code that is inside a row body.
- **Lazily created, disposed with the position.** A row that never reads its slot allocates
  nothing; the keyed sweep, `wcomp_destroy` and the component's own render kill the slots
  with the row. Cost when it is used: ~184 B per live row (`WInst` + one node per
  declaration), so a 20 000-row table with per-row state is still the wrong shape (§16.8).
- **Reading it outside a position panics**, naming the declaration — the same rule as
  `state_in`: a per-position read with no position cannot know which copy it means.
  `state(…)` is the declaration for "one value, anywhere in the page".
- **The server half is unchanged.** SSR draws the declaration's initial values and registers
  nothing, so hydration is byte-for-byte what it was.
- `state_own` and `state_in` answer different questions and both can be live in one app: a
  position resolves first (`state_own`), and with no position in scope `with_inst` answers
  (`state_in`).

**`toggle(name)`** flips a flag in place — `set(x, get(x) ? 0 : 1)` written out is an
inversion the reader decodes; the macro is the name of the thing. And `state(T_, N_, …)`
is generic: `state(bool, ahead, false)` works, and a flag that is a boolean reads as one
in an `if` instead of carrying a comment that says which integers mean what.

### 11d. Component instances: `comp()`

`state_own` is one value per *render position*, and a position is a mounted view or a
`for_each` row. That leaves the unit a component author actually writes — **the call** —
without one: a `PART` is a plain C function, so calling it pushes no position and two
calls of one part in the same view shared one slot. `comp()` says which call is which:

```c
state_own(i64, qty, 0);

PART(Stepper, (label, Text));
void Stepper(Text label) {
  el("div") { class_("stepper");
    el("button") { named(fmt("dec-%s", label)); text("−"); }
    el("span")   { text_live("%lld", qty); }       // this call's slot
    el("button") { named(fmt("inc-%s", label)); text("+"); }
  }
}

view(Order) {
  el("main") {
    comp(Stepper, "Chairs");   // its own qty
    comp(Stepper, "Tables");   // and this one's
  }
}

action(inc_chairs) { set(qty, get(qty) + 1); }     // the instance of the button clicked
```

- **Identity is `(the position that drew it, the call site)`.** The call site is a
  `__COUNTER__` captured where `comp()` was written, so the same call on the next render
  finds the same instance, two calls never share one, a row that moves in a keyed list
  keeps its components, and a branch that stops drawing one disposes it.
- **Nothing is allocated for a component that asks for nothing.** The instance is created
  the first time a `state_own`/`computed_own` is read or an `effect_own` is started, and
  killed when the position is swept (the sweep is the keyed list's, keyed by call site).
- **Everything downstream already resolves the instance**, because it resolves the node's
  owner: `text_live`/`attr_live`/`class_live`/`show_live`, `bind_str`, `input_hook`, and
  every `action` (`get(qty)` in an action means "the instance of the button that was
  clicked", exactly as it means "the row" inside a `for_each`).
- **It is opt-in.** A part called without `comp()` keeps the older rule — it draws in the
  position that called it, so its `state_own` is shared with every other call in that
  position. That is what makes the change additive, and `webc check --wiring` points at
  the parts where it matters: a part that declares `state_own` and is called twice in one
  function gets a note naming both, which `comp()` at the call sites clears.
- **No wrapper element.** `comp()` is a position, not an element: the part's markup is a
  direct child of whatever element it was written in, so CSS sees the tree you wrote.
- **SSR is unchanged.** The server draws the declaration's initial values and releases the
  instances when the html is text; the browser builds its instances from the same call
  sites, so hydration is byte-for-byte what it was.

#### Lifecycle: `effect_own`, `mount_own`, `on_cleanup`, `on_mount`

An instance is worth little without a way to clean up after it, and the framework had no
per-position effect and no teardown hook at all:

```c
effect_own(save) {                        // file scope: the body
  webc_later(800, flush_draft);           // one timer per instance, not one per page
  on_cleanup(cancel_pending);             // before the next run, and when the instance dies
}

view(Editor) {
  mount_own(save);                        // start it in *this* position; idempotent
  on_cleanup(drop_draft, nullptr);        // once per position, registered on its first render
  el("textarea") { bind_str(draft); }
}
```

- `effect_own(N_) { … }` declares an effect that belongs to the ambient position; the
  body runs with that position bound, so `get`/`set` work as they do in the view.
  `mount_own(N_)` starts it, once: a second call on a later render is a no-op, which is
  what lets the line sit in the view body. (An effect body cannot be spliced into a view —
  there are no closures — which is the whole reason the pair exists, the same pair
  `effect_in` + `mount_in` already had with the app owning the lifetime.)
- `on_cleanup(fn, ctx)` inside an effect registers with **that run** — it runs before the
  next run and when the effect is disposed. Outside an effect it registers with the
  **position**, on the position's first render only (a view that re-renders sixty times a
  second must not register sixty teardowns for one subscription).
- `on_mount(fn, ctx)` runs once, after the batch that drew the position has gone out —
  which is when the node it wants to focus or measure exists. It is a client gesture: a
  server render queues nothing, and a native test runs the queue with `wc_mount_run()`
  after calling `wc_client_mode()`.
- `winst_release` is recursive: an instance's children are torn down first, with the
  parent still intact, so a nested component's cleanup can read the state it was drawn in.

### 11e. Children: `WKids`

A function cannot take a block, so a component that wraps other markup takes a callback
and a context — the `(fn, ctx)` pair `wkeyed_list` and `prose_with` already use:

```c
PART(Card, (title, Text), (inner, WKids));
void Card(Text title, WKids inner) {
  el("section") { class_("card"); el("h3") { text(title); } kids(inner); }
}

static void body(void *ctx) { el("p") { text("%s", (char *)ctx); } }
comp(Card, "Hello", kids_of(body, msg));
```

`kids_of` captures the position that *wrote* the call (the component's parent), so a
`state_own` read inside the children block is the caller's — React's rule too: children
are created by the parent. `no_kids` is the empty value, so an optional body is
`kids(inner)` where `inner` may be `{0}` and nothing is drawn.

## <a name="dom"></a>12. DOM and SSR

```c
static void view(WBuild *b, void *ctx) {
  el("div") { class_("card");
    el("h1") { text("Hello, %s", name); }
    el("button") { on("click", clicked, ctx); text("+1"); }
  }
}
```

- `el("tag") { ... }` opens an element with a cleanup-linked builder.
- **`text()`**: one argument is a *value*, two or more are a format.
  `text(story->title)` prints the title; `text("%s/%d", a, b)` formats. The count is the
  whole distinction and it is decided at compile time, because a rule that treats every first
  argument as a format turns `text("100% organic")` into `1000rganic` and
  `text(row->title)` into a read of a nonexistent variadic argument.
- **Attributes, one spelling per concept**, each taking a `Text`, a `str` or a literal
  (`WC_AS_STR`, chosen at compile time) and a number where a number belongs:
  `attr("width", 640)`, `attr_i("n", i)`, `class_("card lead")`, `style("width:50%")`,
  `id_("reader")`, `href(url)`, `src(path)`, `alt(t)`, `title_(t)`, `role(r)`, `rel(r)`,
  `lang(l)`, `target(t)`, `value_(v)`, `name(n)`, `placeholder(p)`, `type_(t)`,
  `required()`, `checked(on)`, `disabled(on)`.
  A style is a declaration list, never a format: `%` in `width:50%` is a value.
  Each of the formatted ones has a `_fmt` companion that formats for you:
  `class_fmt("row row-%d", i)`, `style_fmt("--cols:%d", n)`, `attr_fmt("data-id", "%d", id)`,
  `post_to_fmt("/flights/%d", id)`, `href_fmt(...)`, `src_fmt(...)`, `id_fmt(...)`.
- **`if` in a view is fine.** A branch that appears, grows or disappears — including in
  the middle of an element — is a supported render: siblings it did not touch keep their
  identity (and their DOM node), and the nodes the branch owned are the only ones removed.
  `show(cond)` still has its place for the case where a toggle should *not* cost a node:
  it flips `display:none` on an element that stays in the tree, so an open/closed panel
  keeps its scroll position and its listeners.
  `show()` is also the safe way to hide a *container* that holds a `position: fixed` child:
  `display:none` on an ancestor takes the whole subtree out of hit-testing, while a
  `class_("hidden")` that only sets visibility somewhere else may not. When a click goes to
  an element you cannot see, `WebC.why(x, y)` in the console names it (§13b).
- `markup(bytes)` for bytes that are already HTML (`raw()` is gone; the name now says what
  the argument is), `on(fn, event)` for a listener on an element in this view, and
  `bind_str(sig)` for a live input bound to a `state(Text, …)`.
- **A live input is one line**, in the view, with no matching line in `client_main`:
  `el("input") { attr("name", "reason"); bind_str(reason); }` for a `state(Text, reason, "")`.
  The two-step form (`input_hook(reason)` in the view + `bind_input(reason)` in `client_main`)
  still works, and it is order-sensitive — the hook resolves its field when the view runs, so
  both calls belong where the docs put them. Get it wrong and the field is reported, by name,
  once rather than doing nothing quietly.
- **Which row was clicked.** Inside an action, `ev_attr("data-id")` (or `ev_attr_i64`) is the
  nearest value of that attribute at or above the element that fired, and `ev_value()` is the
  payload the bridge sent:

  ```c
  for_i(i, (*rows)) {
    const Flight *f = at((*rows), i);
    el("tr") { attr("data-id", fmt("%d", f->id)); el("button") { on_click(pick); text("select"); } }
  }
  action(pick) { i64 id = ev_attr_i64("data-id"); if (id) set(sel, id); }
  ```

  A keyed list (`for_each`) still hands its action the item itself; a positional one has no
  item to hand, and an action that declares one and gets NULL now says so once instead of
  returning silently.
- **A listener is re-registered every render.** An element that a patch reuses keeps its node
  and its `data-wc` id; it does not keep its listener list. That is not an implementation
  detail you have to care about — `named()`/`on_click()`/`bind()` just work across renders —
  but it is why a handler must not close over render-scoped memory: it is called with the
  context the *current* render attached.
- `bind_soft(name, event, action)` is `bind()` for a control that is only rendered sometimes
  (a panel behind a branch): it is excluded from the dead-binding report, which keeps its
  teeth for a name no view ever renders.
- Reuse: re-rendering the same component patches only nodes whose text or attributes
  changed. A counter that ticks costs one `text` command.
- `wkeyed_list(n, key_at, item, ctx)` reconciles by key and disposes the per-item scope
  when an item leaves; the macro form is `for_each(item, vec, KEY)` (an index loop is
  `for_i`). `portal("#modal-root") { ... }` puts a subtree under another element, so a
  dialog escapes its stacking context.
- **`for_each` is a *render* loop, not a way to iterate.** A key drawn twice in one
  run is a panic naming the key: the key is the model's `.pk`, and `0` is its unset
  value — the shape a page of hand-built rows (`vec_new` + `push`, no ids) makes.
  Before the sentence it was a node that was its own sibling and a render that
  never finished. Each pass opens a keyed item
  scope on the node that is currently open, and the loop's last statement sweeps every item
  of that node the pass did not claim. That is what disposes a removed row's scope — and it
  is also why a `for_each` that renders *nothing* (a count, a sum, a `find`) is not
  harmless: it runs the sweep on whatever element the view has open. Counting is a plain
  loop:

  ```c
  i32 n = 0; for (i32 i = 0; i < vlen(rows); i++) n += vat(rows, i).lane == lane;
  ```

  `for_each_ptr` is the same iteration with no keyed scopes at all.
- `page(View, ctx)` renders HTML with `data-wc` / `data-on-*` anchors; the bridge adopts
  those nodes on the client without creating any.

### 12f. Numbers, money and text you build

A view that shows a number has three small problems and no good place to solve them: the
number needs thousands separators, money needs a currency and a sensible zero, and a
percentage needs a decimal count that does not jitter frame to frame. Written by hand they
come out different in every view — `10038` in one card and `$10,038.00` in the next — and
the third one silently costs a `snprintf` into a buffer somebody sized by hand.

```c
text(fmt_num(10038));            // "10,038"        (i64; fmt_num_u for u64)
text(fmt_money(10038.4));        // "$10,038"       (< 0.5 prints "$0"; "-$1,235", not "$-1,235")
text(fmt_money_c(1500, "€"));    // "€1,500"
text(fmt_pct(0.0421, 1));        // "4.2%"          (decimals 0..6, clamped)
text(fmt_compact(1_040_000));    // "1.0M"          (k / M / B / T)
```

All five return `Text` — a string that lives as long as the current scope, like `fmt()` —
so they nest in `class_fmt`, `attr_fmt`, `href_fmt` and each other, and none of them
allocates heap. `fmt_pct` takes a *fraction*: `0.0421` is `4.2%`, and the one caller that
had it backwards is now a compile-time-looking mistake instead of a page that says 0%.

An attribute that is a *list* of numbers — `points`, `d`, `stroke-dasharray` — is built with
a string builder instead of six `snprintf` calls into a scratch buffer:

```c
Buf *b = sb();                                  // a fresh builder, in the render's arena
for (i32 i = 0; i < n; i++) {
  if (i) sb_putc(b, ' ');
  sb_num(b, x_of(i), 1); sb_putc(b, ',');       // stable decimals: no jitter between frames
  sb_num(b, y_of(ys[i]), 1);
}
el("polyline") { class_("curve"); attr("points", sb_str(b)); }
```

`sb()` is scoped to the render exactly like `fmt()`: the buffer is allocated in the render's
arena, so a builder per element costs an unreclaimed pointer and nothing else, and calling
`sb()` twice in one element gives you two independent builders (which is what a chart with a
band and a line needs). `sb_puts`, `sb_putstr`, `sb_putc`, `sb_i64`, `sb_u64`, `sb_printf`
(`__attribute__((format(printf, 2, 3)))`, so a wrong format is a compiler warning),
`sb_num(b, v, decimals)` for a double with fixed decimals, `sb_cstr` for an API that wants a
NUL, `sb_str` for `attr`. Outside a render — in a test, or a task — the builder falls back
to the current scope's arena and behaves the same way.

### 12d2. HTML that came from the database: `webc/richtext.h`

The framework has three cases for authored bytes, and two would be a floor that leaves the
third unwritten: `text()` / `prose()` for copy, `markup()` for bytes *you* wrote. The third is
the one every CMS has — HTML
a **user** wrote and a column stored — and it is the only one that needs a sanitiser.

```c
#include <webc/richtext.h>

rich(post->body_html);                              // the default policy: docs, CMS bodies
rich_with(post->body_html, .allow = RICH_TIGHT);     // comments: p, br, a, em, strong, code
rich_with(post->body_html, .no_images = true, .lazy = true);
```

It parses once and re-emits through `el()`/`attr()`/`text()`, so escaping is not a second
implementation: a tag outside the policy is dropped (its text stays), `script`, `style`,
`iframe`, `svg`, `math`, `form`, `template` and friends are dropped *with their contents*, every
attribute value is entity-decoded before it is judged — `java&#115;cript:` is a `javascript:`
URL and is refused — and `href`/`src`/`poster` accept only http(s), mailto, tel and relative
URLs. A `data:` URI is refused unless `.allow_data_images`, and then only for a non-SVG image.
A `<a target="_blank">` and any absolute link get `rel="noopener noreferrer"` unless the author
wrote one. A tag left open is closed at the end of the fragment; a stray closing tag closes
nothing; nesting is capped at `.max_depth` (40).

The same policy answers the questions an article page asks, without a second parser:

```c
text("%s", wc_rich_excerpt(post->body_html, 180));   // plain text, cut on a word, with …
text("%d min read", wc_rich_reading_time(post->body_html));   // 200 words a minute, never 0
i32 words; WRichHeading toc[32];
wc_rich_headings_into(post->body_html, toc, 32, &words);      // level, text, stable id
str stored = wc_rich_stamp_ids(body);                // write `id="…"` on every heading, on save
```

`rich()` gives a heading an id derived from its own text (`getting-started`, and `-2` for the
second one), which is what a table of contents links to; `wc_rich_stamp_ids()` writes the same ids
into the stored fragment so an anchor a reader copied still lands after an edit, and it is
idempotent. Neither is Markdown (that is `prose()`) and neither is a validator: a fragment is
the unit, and sanitising on the way *out* is what makes a stored body safe to render.

### 12d3. The facts a page states: `webc/ld.h`

Every page already says what it is about — the `<title>`, the `og:` tags, the prose — and
then repeats it for a crawler in a JSON-LD block.

The shape every app arrived at on its own was a `fmt()` carrying the whole document, a
hand-written escaper for each value, and the vocabulary's nesting retyped on every page type.
Three things are wrong with it and none of them is the app's — the nesting is knowledge a
library should hold, a `fmt()` cannot *omit* a key (so a draft claims it was published at `""`,
which is not a date), and the resulting string is not safe in the element it is written into.
`webc lint` names that spelling; this is the door instead:

```c
#include <webc/ld.h>

head_ld(ld_blog_posting(.url = abs_url(fmt("/article/%s", p->slug)),
                        .headline = p->title, .description = desc,
                        .image = cover_url, .author = p->author_name,
                        .publisher = brand(site), .published = p->published_at,
                        .modified = p->updated_at, .word_count = p->word_count));
```

It emits the nesting the vocabulary asks for — `author` is a `Person`, `publisher` is an
`Organization` with a `logo` `ImageObject`, `image` is an `ImageObject`, `mainEntityOfPage` is
`{"@id": …}` — and **a field you leave null is omitted**, never written empty:

```json
{"@context":"https://schema.org","@type":"BlogPosting",
 "headline":"Go cutover walkthrough","datePublished":"2026-10-14T20:31:00Z",
 "mainEntityOfPage":{"@id":"https://example.com/article/go"},
 "author":{"@type":"Person","name":"Victor H"},
 "publisher":{"@type":"Organization","name":"VictorTrades"}}
```

| call | the type |
| --- | --- |
| `ld_article(…)` / `ld_blog_posting(…)` / `ld_news_article(…)` | `Article`, and the two subtypes |
| `ld_website(…)` | `WebSite`, with a `SearchAction` when you pass `.search_url` |
| `ld_organization(…)`, `ld_person(…)` | `Organization`, `Person` — logo, `sameAs`, `jobTitle` |
| `ld_breadcrumbs(crumbs, n)` | `BreadcrumbList`; `position` counts itself, the last crumb has no `item` |
| `ld_faq(qa, n)` | `FAQPage`; an entry with no answer is skipped |
| `ld_graph(docs, n)` | one document, many things: the site *and* the article, linked by `@id` |

Fields are C strings (a row's `Text`, a `fmt()`, `text_of()` for a value already `str`), URLs
are already absolute, and dates are passed through as you hold them — `time.h` produces
ISO-8601, which is what the vocabulary wants. A type that is not here is four `jw_*` calls in
your own function (§5); that is all these are. `head_ld()` (§7b) is the door, and
`ASSERT_LD(res, "author.name", …)` (§14b) is how a test checks it.

Nothing invariant is left to the caller: `position` numbering, the `@type` of every nested
object, and the `rel` on a link are the library's job, because they are the fields a
hand-written block gets right once and wrong on the fifth page.

### 12d4. Authored HTML as a file: `partials/`

`markup()` is for bytes typed next to the code that renders them. A site's head chrome is the
other shape of authored HTML — a dozen lines, none of it interpolated, identical on every page —
and in a C string every one of those lines is an escape and a line-continuation:

```c
// src/server/head.c — before: every line an escape, every attribute a \"
#include <webc/server.h>

static void head_global(void) {
  head_html("<link rel=\"icon\" type=\"image/png\" sizes=\"32x32\" "
            "href=\"/icons/favicon-32x32-dc00edd8.png\">");
  head_html("<meta name=\"theme-color\" media=\"(prefers-color-scheme: dark)\" "
            "content=\"#1b1714\">");
}
```

The file is the better home for those bytes. `partials/<name>.html` at the project root is
included by name — read once, cached for the process, and verified by `webc check`:

```c
// src/server/head.c — after: the same bytes, as partials/site/head.html
#include <webc/server.h>

static void head_global(void) {
  head_html(partial("site/head"));
}
```

Three doors take one:

```c
// src/server/down.c — chrome into the head, a page from a file, a banner in a view
#include <webc/server.h>

GET("/down") {
  head_html(partial("site/head"));               // the chrome every page carries
  return res_html(200, partial("maintenance"));  // a whole page, still a file
}
// inside any view: markup(partial("cta/banner")) — a partial draws like markup does
```

What a partial **is**: authored — `markup()`'s trust, no sanitiser, because the bytes are yours
the moment you commit the file; and static — no interpolation, because a fragment that needs a
value is what a view is. What it is **not**: browser-side. The wasm half ships bytes, not a
filesystem, so a fragment *both* halves render is `#embed` beside the code that renders it —
the partial is for chrome the server owns, which is most of it.

The failures are loud where they can be. A name is a name, not a path — `partial("../x")` and
`partial("/etc/passwd")` are refused, because the string came from app code. A name with no
file is a null `str` (and one warning, not one per render), `head_html()` writes nothing, and
`webc check` names the line:

```
error ./src/server/head.c:12
  a partial with no file
  fix: create partials/site/head.html
```

`webc dev` watches `partials/` with the rest, so editing the file reloads the page the same way
editing a view does.

### 12e. Parts: a view's composition unit

A part is a plain C function with typed arguments, callable from any view or part —
`PART` declares the shape once, as tuples, and the compiler checks every call site
(which a five-int helper never did):

```c
// parts.h — the declaration, shared
PART(Avatar, (name, Text), (hue, i64), (online, bool))

// parts.c — the definition is the function PART declared; the body is view code
void Avatar(Text name, i64 hue, bool online) {
  el("span") { class_("avatar"); attr("data-hue", fmt("%lld", (long long)hue));
               text("%s", name); }
}

// any view: Avatar(u->name, u->hue, user_online(c, u->id));
```

The convention: parts live in `src/parts.{h,c}` (or a `src/parts/` directory), take
what they need as arguments — never a context struct whole — and are the unit a
shared view composes. `webc check` sees inside them like any view code.

A part is *markup*; a part that wants a state of its own is a component, and the call site
says so (§11d): `comp(Avatar, u->name, u->hue, online)` gives the call its own
`state_own`, its own `effect_own`s and its own teardown. A part that wraps other markup
takes a `WKids` (§11e).

### 12d. Prose, and markup you authored

Anything longer than a label is prose, and prose is text, not markup. `prose()` turns it
into paragraphs and the four inline marks, escaping everything else:

```c
prose(story->body);          // blank line = <p>; the whole body is escaped first

prose_with(c->body,
           .first = "first-p",          // the first paragraph, styled differently
           .after = pull_quote, .ctx = st,   // called after each paragraph, 1-based
           .link_class = "inline-link"); // class on <a>, if you want one

prose_inline(deck);          // one run, no <p> wrapper: inside your own element
```

| In the text | Becomes |
|---|---|
| a blank line | a new `<p>` |
| `**bold**` | `<strong>` |
| `*italic*` | `<em>` |
| `` `code` `` | `<code>` |
| `[label](/href)` | `<a href="/href">label</a>` — `/`, `#`, `http(s)://`, `mailto:`, `tel:`, or a relative path; anything else stays text, so a `javascript:` URL cannot arrive through copy |

The vocabulary is closed on purpose: it is punctuation you can type into a database column
and read back in a terminal. It is **not** Markdown — no headings, lists, images or tables.
An unclosed `**` or an `[` with no `](` is a literal character, because that is what a stray
asterisk means. Inside bold, italic and code there is no nesting: the run is escaped text.
The render is streaming — plain runs are slices of your string handed straight to the
builder — so a 71 KB page does not copy its own copy.

**Markup you authored** — an icon, a fragment of HTML you wrote by hand — is `markup()`: the
name says what the bytes are, and it is the only way in that skips escaping.

```c
el("span") { class_("icon-sun"); attr("aria-hidden", "true"); markup(sun_svg); }
```

Long copy and markup like that belong in files, not in escaped C string literals. C23
`#embed` splices the file in at compile time; the `, 0` is the trailing NUL:

```c
static const char about[] = {
#embed "content/about.txt"
    , 0 };

static const char icon_moon[] = {
#embed "public/assets/icons/moon.svg"
    , 0 };
```

`#embed` resolves the path relative to the including file and works for both targets
(verified on clang 21 for the host and wasi-sdk clang 23 for wasm), so the same `.svg` can
be the icon in the server-rendered chrome and in the browser half. Copy in a file is
copy you can edit as prose: no quoting, no `\n\n`, no line splicing.

### 12c. Progressive enhancement

Every page works with JavaScript off; the module only sharpens it. The two pieces of that
sentence are spelled once:

```c
// in the chrome, rendered by the server and by the module alike
enhance("reader") {                                  // <div id="reader" data-enhance="reader">
  el("form") { post_to("/theme"); … }                // the no-JavaScript control
}

// a control outside the mount point retires itself once the module is up
el("form") { scripted_hide("themeswitch"); post_to("/theme"); … }

client_main { mount(Reader, "#reader"); }            // replaces the fallback's contents
```

`assets/webc.js` sets `<html data-wasm="1">` and hides every `[data-scripted-hide]`
element after the module has started, so the fallback and the interactive version are
never on the page at the same time.

---

### 12g. Where the pointer was

A pointer event arrives with the fact that matters: where it happened. The bridge sends
that with the event, so following the pointer is an action and a signal — not a script, not
`getBoundingClientRect` in a `.js` file, and not a second source of truth about what the view
drew. One app's whole tooltip was 54 lines of JavaScript for this: a `document`-level
`pointermove`, a `closest('g.stratg')`, two `data-` attributes and a fixed `div` moved with
`clientX`/`clientY`. All of it is three lines of C now.

```c
action(follow) {
  WPoint p = ev_point();                         // the point, inside the element
  set(tip_x, p.cx); set(tip_y, p.cy);            // …or p.x/p.y for the element's own frame
  set(tip_on, 1);
}

view(Tip) {
  if (get(tip_on))
    el("div") { class_("tip"); style_fmt("left:%gpx;top:%gpx", get(tip_x) + 14, get(tip_y) - 12);
                text("Martingale"); }
}
```

`WPoint` carries six numbers: `x`/`y` inside the element (0,0 at its top-left corner),
`w`/`h` for the element's own box, and `cx`/`cy` on the page (`clientX`/`clientY`, which is
what a `position: fixed` element wants). `ev_box()` is the box alone — the pair a chart needs
to turn a pixel into a fraction:

```c
action(crosshair) {
  WBox b = ev_box(); WPoint p = ev_point();
  set(at, b.w > 0 ? p.x / b.w : 0);              // where in the plot the pointer is
}
```

An event with no pointer data — a `click` fired from the keyboard, a field, a test that did
not say where — answers zeroes, so `p.w || p.h` is the guard and a fraction is never
`nan`. `pointermove`/`pointerdown`/`pointerleave` work for a mouse, a pen and a finger; there
is no separate touch vocabulary to write.

**Leave events work, and they mean what they say.** The bridge listens on the document, and
`pointerleave`/`mouseleave`/`pointerenter`/`mouseenter`/`focus`/`blur` do not bubble — so a
`bind("row", "pointerleave", …)` never ran at all, silently, and the hover it was meant to
undo stayed on screen. The bridge hears them in the **capture** phase now, and drops a leave
whose `relatedTarget` is still inside the same named element: the pointer crossing from one
child of a group to another fires a leave on the child being left, and without that check the
state blinked off and on at every boundary inside the group. The unit is the element carrying
the name, so one `named("stratg")` group is one hover.

A native test says where the pointer was with `client_dispatch_at(hook, event, x, y, w, h)`
(§14b), so hover code is under test like everything else.

The cost is a round trip and a re-render per event (a pointer move arrives at ~60 Hz). That
is 2–4 ms for a page-sized view and it is the same render every other event causes; a view
that is slower than that is telling you about itself (§11a, and `WEBC_RENDER_COST=1` in
§13b measures it). If a hover is too expensive, the honest fix is a smaller view behind a
`named()` hook, not a script that positions the element behind the framework's back.

### 12h. Charts: `webc/chart.h`

An app that draws a chart writes the same four things every time: a value-to-pixel map, a
tick step a person can read, a downsampler that does not average a spike out of existence,
and the `points="…"` string SVG wants. One app carried 341 lines of it — two scale closures,
a "nice number" picker, a bucketed min/max reducers and a `%.2f` attribute helper — and every
one of them is the same code in the next app. So it is a header, opt-in like `ui.h`:

```c
#include <webc/chart.h>

WPlot plot = chart_plot(CW, CH, 64, 24, 20, 34);          // inside a 720×340 viewport
WScale x = chart_x(plot, FLIPS);                          // indices 0..FLIPS
WScale y = chart_y(plot, 0, peak);                        // value 0..peak, y grows up

Buf *pts = sb();
chart_span_points(pts, x, y, eq, FLIPS + 1, 476, 1);      // 476 buckets, 10 001 points in
el("polyline") { class_("curve"); attr("points", sb_str(pts)); attr("fill", "none"); }

f64 ticks[12];
i32 n = chart_ticks(0, peak, 5, ticks, 12);               // 1/2/2.5/5 × 10ⁿ, not 1 387.4
for (i32 i = 0; i < n; i++) grid_line(ticks[i], y_at(ticks[i], plot));
```

What is in it:

| Call | What it is |
|---|---|
| `chart_lin(lo, hi, p0, p1)` / `chart_log(...)` | a scale; `p0`/`p1` may run backwards, which is how a y axis grows upwards |
| `chart_at(s, v)` / `chart_val(s, px)` / `chart_frac(s, v)` | value → pixel, pixel → value (the inverse a crosshair needs), value → 0..1 |
| `chart_nice_step(span, want)` / `chart_ticks(lo, hi, want, out, cap)` | round steps at 1/2/2.5/5 × 10ⁿ; the tick list is clipped to `cap` instead of overrunning |
| `chart_decades(lo, hi, out, cap)` / `chart_log_top(peak)` | the same for a log axis |
| `chart_plot(w, h, pad_l, pad_r, pad_t, pad_b)` / `chart_x` / `chart_y` | the plot rectangle and the two scales a chart actually asks for |
| `chart_span_points(b, x, y, ys, n, buckets, decimals)` | min+max per bucket: a spike survives, a flat stretch costs two points |
| `chart_points(...)` / `chart_band_points(...)` | every n-th point; a closed min/max band (a cone, a drawdown envelope) |
| `chart_range(ys, n, &lo, &hi)` | min and max in one pass |

Nothing here decides a colour, a font, a layout or a label format: the labels are the app's,
and `fmt_num`/`fmt_pct`/`fmt_money` (§12f) are what they use. It allocates nothing, keeps no
state, and is the same source on both targets — so a chart rendered on the server and one
patched in the browser agree to the last decimal, which is the property that makes a
hydration diff empty.

### 12b. Routes, resources and forms on the frontend

```c
route("/todos/:id", detail);              // same path syntax as the server router
i64 id = route_param("id");
front_route("-");                          // the current path as a signal

static WRes *res;                          // one per component field
res = res_new(arena, load_todos, ctx, sizeof(Todo));
res_start(res);
if (res_loading(res)) { ... } else if (res_failed(res)) { text("%.*s", (int)res_err_str(res).n, res_err_str(res).p); }

// A resource has four states, and `res_loading()` is "started and not settled" — it is
// false before the first start, so the guard an on-demand fetch wants is expressible:
if (!res_idle(res)) { /* somebody already asked */ }
res_ensure(res);            // start if idle, do nothing otherwise
switch (res_state(res)) { case WRES_IDLE: … case WRES_LOADING: … case WRES_OK: … case WRES_ERR: … }

state(Text, email, "");
WForm f; Signup draft;
form_init(&f, Signup_modelof(), &draft);
el("input") { bind_str(email); }        // two-way: the field writes the signal
draft.email = get(email);
if (!form_submit(&f)) text("%.*s", (int)form_error(&f, "email").n, form_error(&f, "email").p);
```

A signal that holds text is `state(Text, email, "")` — `Text` is what `fmt()` returns and
what `text()` and `set()` take, so there is no second string type to convert to:
`set(email, "ada@example.com")`, `set(email, fmt("%s@x", user))`, and `text(get(email))`.
The `str` that holds a slice is the *parser's* type (`req->body`, `query("q")`); app code
converts at the boundary with `WC_AS_STR(slice)` and then stays in `Text`.

`query("name")` reads the **query string first, then an `application/x-www-form-urlencoded`
body** — a form posted with JavaScript off puts its values in the body, and the handler that
serves it is the same handler the module calls. That is what makes
`el("form") { post_to("/theme"); el("input") { name("to"); value_("dark"); } }` work in both
halves without the app building a URL by hand. `req_body_field("to")` reads the body alone
when the distinction matters.

On the server a resource loads in a green task (the page renders `loading` first); on
wasm it runs inline, because there are no threads to wait with. `form_submit` runs the
model's own validators — the same function the request runs, which is the point of one
library for both halves.

### 12b2. Work that takes too long: `res_compute`

A resource whose loader is CPU — 21 strategies over 10 000 flips, a Monte Carlo walk, a
median of medians — cannot use `res_new`: wasm has no threads, so the loader would block the
page for the whole run. The escape that shipped in one app was "start it in a timer callback
and do it all there", which blocks just as hard and, in that app, walked the arena to 2 GiB.

A computed resource is the same four states, with the work split into chunks that yield:

```c
static void build_table(WJob *j, void *ctx) {
  State *s = ctx;
  for (i32 i = s->cursor; i < NSTRAT; i++) {
    if (!job_tick(j)) { job_steps(j, i, NSTRAT); s->cursor = i; return; }  // resumed here
    simulate(&s->strats[i]);
    job_steps(j, i + 1, NSTRAT);
  }
  job_done(j);
}

WRes *table = res_compute(nullptr, build_table, &g_state, sizeof(Run));

// in the view
res_ensure_key(table, g_key);                    // start, or restart when the key moved
if (res_loading(table)) { text("simulating %d%%", res_progress(table)); }
else { Run *run = res_data(table); … }
```

- **`job_tick(j)`** is the whole contract: it answers "you still have time", and a function
  that returns without `job_done()` is resumed where it left off. The cursor is the app's own
  loop variable in its own state — there is no closure, no coroutine, and nothing to free.
- **`res_ensure_key(r, key)`** is `res_ensure` for work that has inputs. The key is the app's
  hash of everything the work depends on (a seed, a slider, the row count); one number, so
  the framework never guesses at dependencies and never compares a struct it does not know.
  `res_invalidate(r)` forces the next one to run again.
- **`res_progress(r)`** is 0..100 from the steps the work reported. Between chunks the view
  re-renders, because a chunk ends in a host callback (§11a) — so the progress bar advances
  without a signal of its own.
- **The budget is 8 ms** per chunk (`wc_job_budget_ms`), and every chunk gets at least one
  step whatever the budget says: a work function that can never move would otherwise hand
  control straight back to the pump and spin on timers forever.
- A native build (a test, an SSR page with no browser to yield to) runs the job to completion
  through the *same* pump, so a test exercises the chunk loop rather than a special case.

A job is a resource, so `res_data`, `res_ok`, `res_loading` and `res_failed` all mean what
they mean for a fetched one, and a view that shows both kinds needs no second vocabulary.

**Client-side routing** is three verbs and the same `route()` table: a selection that
should be an address calls `push_url()` (a new history entry) or `replace_url()`, the
module reads where it is with `self_path()`, and the back and forward buttons arrive as
an action declared with `on_nav()`:

```c
static void select(i64 id, bool push) {
  set(sel, id);
  if (push) push_url(id ? WC_AS_STR(link_to(Flight, flight_by(&flights, id))) : str_lit("/"));
}
action(navigate) {          // popstate: the browser moved, the board has not
  WPageMatch m = front_route(self_path());
  ...
}
on_nav(navigate);           // in client_main
```

The initial paint needs no push: `client_main` reads `self_path()` once and sets the
selection it names, so a shared link opens on the same leg on both halves.

---

### 12i. The standard library: `webc/<module>.h`

Everything up to here is the floor: what every WebC app links. The shelf is the other
half — the parts of an app that are the same in every app, kept in headers an app
*includes* rather than headers it is built on.

```c
#include <webc/table.h>     // the include is the manifest
#include <webc/overlay.h>   // … and the whole cost
```

A module is header-only, has no hidden state, owns no colour and no wording, and never
adds a bridge verb: it may add C, and the browser bridge (`assets/webc.js`) is the one
file an app cannot opt out of, so nothing on the shelf may grow it. `webc stdlib` lists
what exists:

```
module     tier         lines  what
ui         markup       214    switch, segmented control, tab, meter, kbd · badge, stat, empty state
icons      markup       109    stroke icons as data: the ones you draw are the ones that link
table      markup       219    sortable, paginated table: order without owning the rows
overlay    markup       189    dialog, drawer, menu, toast, tip — with the ARIA and the focus
form       markup       278    a form drawn from the model that validates it
theme      markup       123    design tokens and a dark edition that does not flash
keys       interaction  102    one table of shortcuts, bound and documented from the same rows
motion     interaction  105    enter animation and stagger, no script, reduced-motion honoured
dnd        interaction  158    pick a row up and put it down: the drop index, not the maths
live       interaction  163    rooms over SSE: whole documents, coalesced, a dead tab costs nothing
csv        data         372    in and out, quoted correctly, by header name
richtext   data         799    HTML a user wrote, rendered through a policy: parsed, allowed, escaped
ld         data         307    structured data: the article, the site, the trail, the FAQ — one expression
search     data          78    a search box that ANDs its words, escapes a typed %, and ranks the title
i18n       data         106    one app, two languages: catalogs, plurals, direction
upload     data         302    a file arrives and is checked: magic bytes, a name this side makes up
time       data          73    dates, ages and two-unit durations, UTC only and proud of it
mail       platform      96    a real email from one view: SMTP, or a file in the outbox
jobs       platform     190    work that outlives the request: a table, a lease, a retry
limits     platform     150    a rate limit that is one line, and a table that cannot grow
pwa        platform      71    installable and offline: manifest, worker, cache keyed by the build
```

**What they cost**, measured — `webc size --modules` builds a probe app per module and gzips
it, so the number is a build rather than an estimate (`-Oz`, `--gc-sections`, clang / wasi-sdk):

```
floor: a client half with no module at all    33.9 KB gz

module     tier               gzip
richtext   data             5.3 KB      table      markup         1.7 KB
ld         data            1010 B       dnd        interaction     789 B
time       data            1.0 KB       ui         markup          673 B
overlay    markup           635 B       i18n       data            481 B
keys       interaction      407 B       icons      markup          354 B
form       markup             0 B      theme      markup            0 B
motion     interaction        0 B
live, csv, search, upload, mail, jobs, limits, pwa — server-side:
                                                 no bytes in the browser

the whole shelf, in the browser half:  12.3 KB gz on a floor of 33.9 KB gz
```

`richtext` is the expensive one and it is the honest kind: 5.3 KB buys a parser, an
allow-list, entity decoding and a URL policy in the browser half, and a page that renders a
stored article in wasm needs exactly that. A page that shows HTML the *server* already
sanitised and only renders the shell pays nothing for it.

Each probe is a small *page* using four or five of the module's verbs, not a call to one
helper (which the optimiser folds away and which measures nothing) and not a call to
everything (which measures a tutorial). `0 B` means the helper folded into code the floor
already had. An app that takes none of it pays zero: an unused header is not compiled into
anything, and the floor number is the open §10 story, not the shelf's.

**Picking.** There is no registry, no lockfile and no configuration key. An `#include`
line is the whole mechanism, which means the dependency graph of a WebC app is readable
from its first twenty lines and `webc stdlib --used` can answer "what is this app using"
without a build.

`webc add <module>` is the install: it copies the module's stylesheet into
`public/stdlib/<module>.css` and prints the two lines that are the actual manifest — the
`#include` and the `<link>` (`/public/stdlib/<module>.css`: `public/` is mounted at
`/public`, and a bare `/stdlib/…` is a 404 the browser refuses as a stylesheet). The CSS is yours from then on; it is not regenerated, because
a module that shipped its own look would be a module that decided what your app looks like.
The class names are the contract (`badge`, `field err`, `aria-sort` on a `th`); the CSS is
a starting point.

**A total is not a budget.** `webc api` records one name count for the whole surface *and*
one line per module in `api.txt`, because a sum can stay flat while one module doubles
and another is cut. A module that grows fails its own line and the message names it —
that is what keeps "pick and choose" true, one module at a time.

**`table`** — order without ownership. Rows stay in the app's array; the table keeps a
permutation, so a sort is cheap and a row's index survives it (that index is what a delete
button posts). The sort is stable, the third click returns to the app's own order, and the
sorted column carries `aria-sort`.

```c
static f64 by_bank(const void *r) { return ((const Row *)r)->bank; }
static const WTableKey COLS[] = {{"Team", by_team, nullptr}, {"Bank", by_bank, "right"}};
static i32 g_order[64];

WTable t;
table_init(&t, rows, n, sizeof rows[0], g_order);
table_keys(&t, COLS, WC_ARRLEN(COLS));
table_state(&t, get(sort), (i32)get(page), 20);

table_head(&t, sort_by);          // symbol, not a string: the action has to exist
table_pager(&t, page_flip, "Rows");
```

**`overlay`** — the five details that make a box on top usable: `role`, `aria-modal`, a
name, focus that moves into it after the batch, and a way out that does not need a mouse.
`dialog(&O, get(open)) { … }` emits the scrim and the panel and nothing when closed;
`overlay_esc(close_action)` is the Escape key. `drawer` is the same contract with another
class, because where the box sits is a stylesheet's business.

The backdrop and the panel are **siblings**, not parent and child. A scrim that wraps the
panel cannot tell a click outside from a click inside — the bridge gives every event in a
hook's subtree to that hook — so a `<form>` inside a dialog submitted nothing and closed the
dialog instead. The backdrop carries the close hook; the panel carries an `id` (the focus
target) and no hook at all, which makes it a boundary. `examples/board`'s new-card dialog is
the case that found it.

**`form`** — the form drawn from the model that validates it. `form_fields(&f)` walks the
model, so the input's kind, its bounds, its label and its error all come from one
declaration, and adding a field to a struct adds a field to the form.

```c
WForm f;
Signup draft = {0};
form_init(&f, Signup_modelof(), &draft);
el("form") { post_to("/signup");
  form_fields(&f);
  form_errors(&f);
  el("button") { type_("submit"); text("Sign up"); }
}
```

`f.prefix = "edit"` gives a second form on the same page its own id space — the thing a
hand-written field set gets wrong at the second form. `form_field_long(&f, "bio", 6)` is
the one place a `textarea` is chosen, because that is a decision about the page, not about
the model.

**`theme`** — tokens, two editions, and the cascade order that makes the first paint right:
the light block, then `:root[data-theme=dark]`, then `@media (prefers-color-scheme:dark)`
with `:not([data-theme=light])`. `theme_head(&T)` writes those three declarations into a
`<style>` in the document being rendered; `theme_head_html(&T)` writes the same thing into
the response's `<head>` from a route (via `head_html()`, §7b). `theme_paint(bool)` is what
a switch's action calls.

**`keys`** — one array of shortcuts, bound by `keys_bind(CMDS)` and listed by
`keys_help(CMDS, &BOX, open)`, so a shortcut cannot be bound and undocumented. `keycap()`
prints `meta+k` as ⌘K.

**`motion`** — enter animation and stagger as two attributes (`enter("rise")`,
`stagger(i)`) plus one stylesheet, with `prefers-reduced-motion` switched off for readers
who asked. It animates *into* the page: leaving and reordering need the node held for the
duration, which is a DOM decision the bridge owns, and a module may not add a bridge verb.
An app that wants that adds it itself.

**`icons`** — 45 stroke paths, each its own constant, so only what you draw is linked. A
button that is only an icon still needs a name, and the name is not an attribute the caller
remembers to add: `button_icon(WC_ICON_PLUS, "New card", "new_card")` draws the button, sets
`aria-label` and `title`, and names the hook — an unlabelled icon button is the most common
accessibility bug in a hand-written UI. Inside a larger button, `icon(d, 0)` plus
`aria_label()` is the pair.

**`dnd`** — pick a row up and put it down. The module owns the pointer arithmetic and the
adjusted drop index, and it does not own your array: `dnd_drop()` answers where the row
lands, adjusted for the row that left, and the app decides what that means.

```c
static WDrag g_drag;
dnd_list(&g_drag, WC_ARRLEN(notes));            // role="list", the shared attributes
dnd_row(&g_drag, &notes[i], i, get(from), (i32)get(over));
...
on_drop(move_row);                               // dnd_take(&g_drag, &from, &to)
```

`dnd_keys()` prints the keyboard contract (`aria-keyshortcuts`) and `dnd_can()` says whether
a drop would change anything. It is built on `bind` + `ev_point`, so it adds no bridge verb.

**`live`** — rooms over SSE: the server half broadcasts a whole document, the client half
re-renders it. A burst of writes becomes *one* frame carrying the newest document, not a
queue; a room with no subscribers has no loop and costs nothing; a tab that dies is
reclaimed by the coldest-room rule. The client half wraps the bridge's own `res_live()`,
so an app includes one header and pays no new verb.

```c
// server: the frame is the model's own encoding, so a subscriber and a tab that just
// loaded the page cannot disagree about what the board is
str doc = live_json(cards);                    // [{"id":1,…}, …]
live_broadcast("board", key, doc);             // in the handler, after the write
GET("/sse/board/:id") { return live_join("board", path("id"), doc, "doc"); }

// client: the same room, through the store that already exists
action(ready) { live_connect("/sse/board/7", "doc", on_doc); }
live_presence(&here);                          // the head count lands in a signal
```

`live_join` streams: the seed first (so a tab that opens late is never blank), then a
presence frame per head-count change, then one document frame per change, and a comment
line every 15 s so a proxy does not close an idle stream.

**`csv`** — out: `csv_write_row()`, `csv_header()` walk the model, so the columns are the
model's own order with the primary key last and `.sensitive` fields never leaving. In:
`csv_parse(body)` → `csv_next(&p, &row)` → `csv_into(Model, &p, &row, &out)`, which matches
by header name through `model_from_form`, so a file with its columns reordered still lands
correctly. Quoted commas, quotes inside quotes, embedded newlines, `\r\n`, blank lines and
width mismatches are all reported rather than guessed at.

**`search`** — a search box that does the one thing that is easy to get wrong: `search_where()`
ANDs the words, ORs the fields, escapes a typed `%`/`_`/`\`, lower-cases both sides, and
ranks by the first field you name then by primary key desc. `search_hl()` highlights in one
pass, escaping and marking together, and `search_snippet()` cuts a window around the first
hit with ellipses.

```c
static const char *const FIELDS[] = {"title", "body"};
search_where(q, Row_modelof(), FIELDS, WC_ARRLEN(FIELDS), /*limit*/ 20);
```

**`i18n`** — catalogs in arrays, binary-searched, up to four of them (`en` fallback, then
what the request asked for). `t("save")`, `t("greet", "name", name)` and `tn("files", n)`
with `_one`/`_few`/`_many`/`_other` keys; the plural rule is a per-catalog `one`/`slavic`/
`none`, so Polish is not English with different words. `i18n_use("de-AT")` answers to `de`,
and a missing key renders `??key??` — loud, because a silently blank label is a bug you find
in production.

**`upload`** — a file arrives and is checked before it is trusted: `upload_take(req, "cv", &pol)`
(the parse happens here — no `body(T)` first), sniffs the magic bytes (PNG, JPEG, GIF, WebP,
PDF, ZIP, WebM, MP4, MP3), checks the policy (`image/*`), and makes up the name on this side —
a random stem plus an extension the *mime map* chose, never the client's filename.
`upload_input()` is the field; `bytes_text()` is "1.4 MB".

**`time`** — dates, ages and durations with no timezone database: `fmt_date(ms, "%Y-%m-%d")`
over Hinnant's civil algorithms, `fmt_date_iso`, `parse_iso_ms` / `parse_iso_secs` (the unit
is in the name; `parse_iso` still works and is milliseconds — `webc check` names it), `ago(ms)`
→ "3 days ago", `dur_ms()` in two units. UTC only, and said out loud. (There is no `clock()`
macro: it would shadow libc's, so `fmt_date(now(), "%H:%M")` is the spelling.)

**`mail`** — a real email from one view. `mail_view(m, VIEW, ctx)` takes the view's *name*
and casts it, so a misspelt view is a compile error and not an empty email; the HTML is
turned into a text part (`mail_html_to_text`), the MIME body is multipart/alternative with
both parts base64, and `mail_send()` writes the message to the outbox **first** and then
tries SMTP (greet, EHLO, STARTTLS, AUTH PLAIN). `WEBC_MAIL_OUTBOX` means a dev machine with
no server still sends something you can read.

**`jobs`** — work that outlives the request. `WEBC_JOB(send_welcome)` declares a job and its
work function; the table comes from the model (`model_sql_create_table`), the claim is one
conditional `update … where`, and a crash is recovered by the lease rather than by a cron
sweep. `wc_job_worker(n)` runs the loop, `NAME_later(…, delay)` is the one-liner a route
calls, and `wc_job_failures()` is the list you actually want in an admin page.

**`limits`** — a rate limit that is one line and a table that cannot grow: 256 fixed slots,
four integers per key, oldest evicted. `mw_rate_per_ip(60, 60_000)` or
`mw_rate_for("/login", …)`; the middleware answers 429 with `Retry-After`. It allocates
nothing under attack, because a limiter that allocates under attack fails under attack.

**`pwa`** — installable and offline in three calls: `pwa_manifest()` (JSON, escaped per
field), `pwa_worker()` (navigate → network-first then offline page, assets → cache-first,
GET and own-origin only, old caches evicted) and `pwa_head()`. The cache name is the build
id, so a deploy is a new cache and a rollback is an old one.

**What the shelf must never contain:** a CSS framework, a bundler or a generator, an ORM,
a timezone database, image processing, a design system, a second router / state / fetch, or
a module that needs a bridge verb. Those are all either opinions an app should hold itself,
or the core's job.

### 12j. Live bindings: a node that updates itself

§11a's rule — a render follows every action — is the right default because it cannot be
wrong. It is the wrong *price* for a value that changes on every keystroke or pointer move:
the framework re-runs the whole view and diffs the whole tree to deliver one text node. On a
5 000-element page that is 3.4 ms of C work to emit the same 12-byte `text` op a binding
emits in 200 ns.

A live binding is that second half. The view says which node carries the value, once:

```c
state(i64, count, 0);
state(bool, open, false);
state(Text, label, "ready");

view(Counter) {
  el("span") { text_live("%lld", count); }               // this text node
  el("li")   { class_("row"); class_live("open", open); }
  el("div")  { style("width:1px"); show_live(open); }
  el("i")    { attr_live("data-n", count); }
  el("em")   { text_live("%s", label); }
}
```

What happens on `set(count, 7)`:

- The binding re-reads the signal, formats it, and emits **the op the patcher would have
  emitted** — `text <id> 7`, `attr <id> data-n 7`. No new bridge verb exists, so the two
  paths cannot disagree about syntax.
- A write that does not change the formatted value emits nothing.
- The text the view drew is updated too, so the next render diffs against what the page
  actually shows: `set(x, 1); set(x, 0)` cannot leave `1` on screen.
- Client-only. The server half renders the value and registers nothing, so SSR and
  hydration are byte-for-byte what they were.

**What still needs the render.** Anything that can add, remove or reorder a node — an `if`,
a `for_each`, a `part`, a `markup()` slot — is the render's job; a binding only ever writes
a leaf. So the bridge keeps re-rendering after every event *unless the action says
otherwise*:

```c
action_live(follow) {                 // pointermove: signals only, nothing structural
  WPoint p = ev_point();
  set(tip_x, p.cx); set(tip_y, p.cy); set(tip_on, 1);
}
// client_main: bind("tip", "pointermove", follow);
```

`action_live` means "this handler writes signals and nothing else"; the flush then sends the
bindings' ops and no view body runs. The promise is the action's to keep — it may not change
what the view draws. An action that mutates data (a vector, a struct field the markup reads)
is a plain `action`: it costs a render and cannot be wrong. `webc check` sees the wiring, not
the promise, so the rule is written where it is read, in the header.

**The numbers** (`bench` in the framework checkout, native `-O2`, 5 000 elements, one cell
changing per event):

| | C work per event | DOM ops per event |
|---|---|---|
| `text("%lld", get(tick))`, then the render | 3.4 ms | 12 B |
| `text_live("%lld", tick)` | 200 ns | 12 B |

`webc new --template=web` starts with exactly this view — a `text_live` count and an
`action_live` button — so a new app's first click already costs one op. `examples/js` is the
same thing with more around it: click its bump button and
`WebC.diagnostics().ops` goes up by two (the batch's header and one `text` line) and a
`MutationObserver` on `#root` sees one `characterData` record — where the plain action beside it
costs six.

The DOM work was never the problem; the re-render was. What a binding does not do: make a
large view mount faster (the first render is still N nodes), or raise the 20 000-op mount
budget (§16.8). A very large list still wants pagination or virtualization.

**The rules.**

- **Types**: `i32`, `u32`, `i64`, `u64`, `f32`, `f64`, `bool`, `Text`. The type comes from
  the `state(…)` declaration through `_Generic`, so nothing else has to say what `count` is.
  A `state_in`/`WInst` signal has no single node to bind to and is refused; a `state_own`
  signal has exactly one, because the binding is declared inside the position that owns it,
  so it is the supported way to make a row's value live (§11c). An unknown type prints
  `??live-binding type??` instead of guessing.
- **Ownership**: `class_live` owns one token of `class`, `show_live` owns `display` inside
  `style`, and both re-read the rest of the attribute from the node on every render — so
  `class_("row")` beside `class_live("open", open)` keeps both. Two bindings on the same
  attribute of the same node is a bug, and the second silently wins.
- **Lifetime**: the binding belongs to the node, so it leaves with the node — a keyed list
  that moves or removes a row takes its bindings along — and a binding the view stops
  declaring in a run is killed by that run's sweep. `wcomp_destroy` kills the rest.
- **Identity**: re-declaring `text_live("%lld", count)` on every render re-uses the binding
  (kind + parameter is the identity), which is why a re-render neither duplicates it nor
  loses it.

## <a name="bridge"></a>13. The browser bridge

The C side emits a line protocol, the JS side replays it:

```
create <id> <parent> <tag>     #text for text nodes
attr <id> <name> <value...>
listen <id> <event>
text <id> <value...>
attrs-clear <id>
move <id> <parent> <before>    a reused keyed item that changed place in its list;
                               <before> is the anchor element id, 0 = end of parent
remove <id>
```

`assets/webc.js` exposes `hydrate(root, commands)`, `apply(commands)`, `feed(chunk)`,
`start({wasm})`, `dispatch(name, event)` (run a binding by name — a keyboard shortcut, a
voice command, another script), `diagnostics()` and `why(x, y)`.

**A batch cannot half-apply.** One malformed op — an attribute with no name, a `create`
into a node that is gone — is skipped, counted, and named in the console; every other op in
the batch still runs. Before that, a single bad line threw out of the replay loop and the
rest of the page was simply never built, with no error to say so. `WebC.diagnostics()`
returns `{ ops, badOps, lastBad, created, hydrated }`:

```js
WebC.diagnostics()   // → { ops: 1873, badOps: 0, lastBad: null, created: 10, hydrated: true }
```

**Listeners are delegated.** One real DOM listener per event type lives on the document and
dispatches by node id, so an element that a patch re-creates keeps its binding, a click on a
child reaches the element that asked for it, and `portal()` content still works. Node ids are
stable for anything the framework reuses; names (`data-hook`) never move. Set
`globalThis.WEBC_DIRECT_LISTENERS = true` before `WebC.start()` to attach one listener per
element instead.

**`start({ data })` hands the module bytes.** The seed JSON is copied into wasm memory as
UTF-8 and the module is told the *byte* count — not `text.length`, which counts UTF-16 units.
One accented letter in the seed makes those two numbers differ, and a JSON document that is
merely truncated is not a crash: the parse fails, the app renders its empty state, and `data:`
looks like it did nothing at all.

**Hydration is verified.** With `hydrate: true` the module's first batch adopts the server's
markup node by node, by id. If a node cannot be adopted — a `markup()` slot, an id that moved
between the halves — the elements the module did not claim are dropped instead of leaving both
trees on the page, and `WebC.diagnostics().hydrated` is `false`.

```js
WebC.diagnostics()
// { ops: 2250, badOps: 0, lastBad: null,
//   created: 1, createdEls: 0, anchors: 633, hydrated: true }
```

`anchors` is how many `data-wc` nodes the server sent, `createdEls` how many elements the first
batch had to *build* (every text node is created on a perfect adoption, which is why the text
count is separate), and `hydrated` is the verdict. The two halves have to render the same tree
for adoption to work: seed the module from the server's own JSON with `data:` before it starts,
and keep the first render free of anything that does not exist in the request (`Date.now()`,
`localStorage`, a fetch that lands later).

**"The click does nothing."** `WebC.why(x, y)` names the element under that point and walks up
its ancestors with the three facts that decide whether a click can reach it: `display`,
`pointer-events`, and the node ids that actually have a listener.

```
[webc] why(420, 300) → span
  span #41 listens=NONE display=inline
  button #40 hook=pick listens=click display=block
```

### 13b. Running in a browser

The same library compiles to wasm and runs against the real DOM. An app never tests for
the target: it declares which files are server, shared and client in `webc.json`, and
`webc build` compiles each set for the side it belongs to.

```c
// src/app.h — the half both sides share
#include <webc/shared.h>
WEBC_MODEL(Todo, (id, i64, .pk = true), (title, Text), (done, bool))
Vec(Todo)
declare_view(Todos);
```

```c
// src/view.c — markup only. It cannot reach a database or a request.
view(Todos, VecTodo *todos) {
  el("ul") { for_each(t, *todos) {
    el("li") { text("%s", t->title); el("button") { named("tick"); text("✓"); } }
  } }
}
```

```c
// src/server.c
#include <webc/server.h>
#include "app.h"
GET("/")          { return page(Todos, db_all(Todo)); }
GET("/api/todos") { return json_list(200, db_all(Todo)); }
```

```c
// src/client.c — signals, actions, the mount
#include <webc/web.h>
#include "app.h"
action(toggle, Todo *t) { t->done = !t->done; }  // no tick: the view re-runs (§11a)
client_main {
  mount(Todos, "#root");
  bind("tick", "click", toggle);
  return 0;
}
```

`webc build` produces **both** halves from those three sets; `webc dev` rebuilds both
and reloads the tab.

Pieces:

| Piece | What it does |
|---|---|
| `src/wasm_glue.c` | exports `webc_start`, `webc_dispatch`, `webc_take_cmds`, `webc_cmds_len`, `webc_alloc`, `webc_free`; calls the weak `wc_app_main()` (what `client_main` becomes) once from `webc_start()` |
| `page(View, ctx)` | renders the view into the `<div id="root">` of `public/index.html`, or into a bare fragment when there is no shell: the page the browser hydrates |
| `named(name)` / `bind(name, event, action)` | the view names an element, the client names the behaviour; inside a keyed list the action receives the item |
| `mw_web_assets(WC_WASM_PATH)` | serves `/webc.js` and `/app.wasm` with the right types and `no-cache`; `WC_WASM_PATH` is `.webc/app.wasm`, which is what `webc build` writes |
| `assets/webc.js` | instantiates, fakes the WASI calls, attaches listeners, replays command batches |

The shell loads it:

```html
<div id="root"></div>
<script src="/webc.js"></script>
<script>
  WebC.start({ el: '#root', wasm: '/app.wasm', hydrate: true });
</script>
```

**Serving it in production.** A browser-only project has no server half, so `webc serve`
is the server: a release build (`-Oz`), no watcher, no reload socket, and the framework's
own chain over `public/` and `.webc/app.wasm` with `mw_compress()` in front of it.

```console
$ webc serve
  ▸ http://localhost:8080   (release, gzip on)

    module  app.wasm     151.4 KB    63.1 KB gz
    bridge  webc.js       64.8 KB    20.1 KB gz
    first load (gz)                  83.2 KB
```

What the browser actually reports for that page (`transferSize`, headers included):

| Resource | On the wire | Decoded | |
|---|---|---|---|
| `/` | 947 B | 647 B | under the 1 KB floor, sent as it is, `Vary` still set |
| `/style.css` | 2.3 KB | 6.0 KB | |
| `/webc.js` | 20.9 KB | 66.4 KB | 32% |
| `/app.wasm` | 64.9 KB | 155.0 KB | 42% |
| **first load** | **86.9 KB** | **222.7 KB** | 39% |

A reload costs nothing: every one of them carries an `ETag` (the compressed one ends in
`-gz`) and answers `If-None-Match` with a 304, so the second visit transfers headers only.

A project *with* a server half is built as a release binary and handed the process, so
its own middleware decides; `webc_use(mw_compress())` next to its `mw_static` is the
same one line.

- `hydrate: true` adopts the server markup by its `data-wc` anchors instead of building
  it again: the measured cost of the hand-over is **zero created nodes**.
- A delegated binding resolves its item by key at click time, so a list that grows or
  shifts never hands an action a stale pointer.
- **A pointer event arrives with its position.** The payload the bridge sends is the field's
  text for a field event and six numbers (`\x01x,y,w,h,cx,cy`) for a pointer event; `ev_point`
  reads the second (§12g). One payload field, two meanings, decided by the first byte.

**Every callback owns an arena.** An event, a timer, a fetch that settled, a field and a
`popstate` all run *outside* a render, so there was no arena to reclaim what they allocate —
and every synchronous `js()` copies its result into whatever arena is current. In a loop that
is a leak with no reset point: one app's deep run made 21 million `js("Date.now")` calls,
grew the app arena to 2 GiB, wrapped the command buffer's `i32` pointer and froze the page
with no error. Now each entry point from the bridge installs a callback arena, and two
generations rotate: what a callback allocates stays valid through the next callback (so a
render can still read a string it made) and is reclaimed two callbacks later. A callback
that allocates more than 8 MB reports it once, by name, and starts from released chunks —
`js()` in a loop is exactly that shape, and the message says so.

**What a render costs**, when a page feels slow and you need to know whether the render is
the problem: `WEBC_RENDER_COST=1 webc dev` prints at most one line a second naming the worst
render in it —

```
[webc] slowest render in the last second: #root — 3.41 ms
```

The measurement is two clock reads per render; the line appears only above 4 ms, and only
when asked. Whole-view re-render is the model (§11a) and 2–4 ms is what it costs for a
page-sized view; a render that takes 40 ms is a view doing work inside the render, and
`res_compute` (§12b2) is where that work goes. A view whose changing value is a leaf can
skip the render entirely — §12j.

### 13c. Calling JavaScript, and being called

One door, typed where C can check it (`<webc/js.h>`) — `examples/js` is a page that walks
through all of it:

```c
str  title = js_str(js("document.title"), "untitled");                     // a property
js("localStorage.setItem", "theme", "dark");                               // two strings
i64  cores = js_i64(js("navigator.hardwareConcurrency"), 1);               // a number
bool online = js_bool(js("navigator.onLine"), true);
js("console.log", js_obj("{\"seam\":\"ok\"}"));                           // a JSON literal
```

Arguments are chosen from their C type by `_Generic` — a `str` or a literal is a string,
an integer a number, a `bool` a boolean, `js_obj("…")` a JSON literal — so nothing in
application source spells JSON, and a quote in a value stays a quote in a value. (A
string *literal* reaches `_Generic` as `char[N]`, undecayed; that is why the default
branch is the C-string encoder.)

The result is a `WJs`, a value with a kind and its own error:

```c
WJs r = js("no.such.thing");
if (!r.ok) log_warn("%.*s", (int)r.error.n, r.error.p);   // its failure, not a global
JVal *v = js_value(r);                    // the parsed value, or null
```

`r.error` belongs to *that* call, so three calls in a handler have three answers —
`js_error()` was one slot for the whole module. A failure is also the one thing that is
logged once, by the bridge, with the path in it.

A path is dotted from `globalThis` and may name a **function** (called with the
arguments) or a **property** (read when there are no arguments). An object's fields are
only readable when it serialises as data — `JSON.stringify` of a `Response` or a
`MediaQueryList` is `{}`. There are no object handles in C, deliberately: a handle is a
lifetime two sides have to agree about. When a call resolves with data, put the one line
that unwraps it where JavaScript is written (`window.api = { get: async (u) => (await
fetch(u)).json() }`).

**Promises are callbacks**, not an event loop and not `await` in C:

```c
static void on_state(WJs r, void *ctx) {
  if (r.ok) set(state, js_str(r, ""));
}
js_then("fetch", "/api/state", on_state, nullptr);   // returns now
```

The handler runs when the promise settles, as an action: signals are written, the views
re-render, the patch batch goes to the page. `js_then` returns `false` when there is no
JavaScript or when `WC_JS_PENDING_MAX` calls are already in flight.

**The other direction** — the page driving the app:

```c
webc_export_json(note) {                        // WebC.call('note', {text: 'hi'})
  JVal *v = js_value(in);       // the payload, already decoded
  set(stored, json_get(v, "text")->s);
}
action(bump) { set(calls, get(calls) + 1); }    // WebC.dispatch('bump')
```

`WebC.call(name, …args)` marshals the arguments, calls the export and flushes the patch
batch; `WebC.dispatch(name[, event])` fires the action a view named with `named("bump")`,
with the context the listener was attached with. Both are for the click that does not
come from the DOM — a keyboard shortcut, a voice command, another script.

The low-level form is still there and still the ABI: `js_call(path, args_json)` returns
JSON text, and `js_get_str`/`js_get_bool` keep their defaults. On a server build every
call answers "undefined" and `js_available()` is `false`, so one file can be both halves.

Toolchain: [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) (clang, `wasm-ld` and a
sysroot). `webc` looks at `WASI_SDK_PATH`, `~/.local/share/wasi-sdk*`,
`/opt/homebrew/opt/wasi-sdk`, `/opt/wasi-sdk`, then `wasm-ld` on `PATH`; `webc doctor`
reports which. Nothing else in the framework needs it.

```
make wasm                                   # examples/web → wasm
cd examples/web && ../../build/webc dev     # server + browser half, live reload
```

### 13d. The host: network, keys and the document — `js()` you never write

`js()` is the escape hatch, not the tool. Everything an app actually does through it —
fetch, POST, a stream, a timer, the address bar, the title, a focus — is a verb in
`<webc/host.h>`, and the bridge is the only code that touches `fetch`, `EventSource`,
`history` or `document`. A verb is a *declaration*: the C side says what should happen,
an answer comes back as a call into the module, and nothing in an application ships a
JavaScript shim to be the other half of the door. (The bug class this deletes is real:
`js("contrail.spin")` once named a function that did not exist, returned `ok: false`,
and the call site ignored it — a spinner that never spun shipped, and both halves
compiled.)

```c
#include <webc/host.h>

static void on_board(WJs r) { if (r.ok) board_read(r.json); }   // r.json: the body as text
static void on_link(WJs r) { /* {"state":"open"|"lost"} */ }

webc_get("/api/briefing/7", on_briefing);                        // one slot per call
webc_post("/api/flights/1/reroute", body, on_reply);             // body is JSON text
webc_subscribe("/api/stream", "tick", on_feed);                  // one EventSource per URL,
webc_subscribe("/api/stream", "state", on_link);                 //   however many hear it
webc_later(5200, on_toast_expire);                               // re-arms its own timer
res_get_json(res, fmt_s("/api/briefing/%lld", id), on_briefing); // a resource whose loader is a GET
```

The handler runs like an action: it writes signals, the batch that follows reaches the
page by the same road a click takes. `r.ok` is false when the request failed and `r.error`
carries the server's own words. A resource started with `res_get_json` walks the ordinary
`WRES_*` states — `res_state(r) != WRES_IDLE` is "already asked" — and its handler owes
the resource exactly one `res_complete()`.

**Typed fields and keys** — one delegated listener each, in the bridge; the names and the
behaviours live with everything else the app means:

```c
el("input") { named_field("dest"); placeholder("LHR"); }      // the shared view
action(field_dest) { set(rdest, WC_AS_STR(keep(ev_value()))); } // the client half
bind_field("dest", field_dest);

on_key("meta+k", pal_open);      // one modifier plus one key; Mod+K fires anywhere
on_key("j", sel_next);           // a bare key never fires while someone is typing
on_key("escape", pal_close);     // escape is the one bare key that does
```

Modifiers are exact: `shift+d` does not fire on a plain `d` (and must not swallow
it in a field), and `alt` is matched the same way.
**The document** — applied after the batch's own mutations, so a title describes the tree
that is now on the page and a `focus()` lands on a node that exists:

```c
doc_attr("data-theme", str_lit("light"));   // an attribute on <html>
doc_title(fmt_s("%s — Contrail", f->number));
focus_hook("pal_q");                        // finds a data-hook, an id, or a data-field
reveal_hook("sel_row");                     // scrollIntoView({block:'nearest'})
field_value("draft", str_lit(""));          // a bound field's live value — a render cannot
                                            // write it back once the reader has typed
push_url(link_to(Flight, f));  replace_url(str_lit("/"));  self_path();
on_nav(navigate);  on_ready(ready);          // popstate; once, after the first batch
```

`on_ready()` is the moment a stream may open: the first batch is in the document, so the
frame that follows has something to land on.

**The live document, as one call** — the pattern every streaming app repeats (two
generation arenas, the stream pair, the seed hand-off, the link state):

```c
static void board_read(str json);            // one callback: the seed and every frame
static void board_link(int state);           // optional: W_LIVE_* for the pill

WLive *board = res_live(&(WLiveCfg){
    .url = str_lit("/api/stream"), .event = str_lit("tick"),
    .doc = board_read, .state = board_link, .gen_bytes = 192 * 1024,
});
// wc_app_data(str json) { board_read(json); … }  // or res_live_read(board, json)
// action(ready)        { res_live_open(board); } // the subscribe pair
```

While `doc` runs, the *next* generation arena is current: decode into it with the model
codec and `persist()` anything that must outlive the frame. The old generation dies only
after the new one is live.

**The page env on both halves** — `page_env()` in the browser half is the same fact
`c->page.env` is on the server (the bridge hands it over before `client_main` runs):

```c
set(dark, page_env().dark ? 1 : 0);   // the edition the server rendered, from the cookie
str born = page_env().path;           // where the document was rendered (self_path() is "now")
```

**Client routing's arrival is a match, not a string walk** — `on_nav` actions receive a
`WNav`: the path *and* the match against the same route table the server answered.

```c
action(navigate, WNav *nav) {
  i64 id = 0;
  str num = nav_param(nav, "number");       // nav_param_i64 for a number
  if (nav->matched && num.n) { /* find the leg */ }
}
```

**The components** — `#include <webc/ui.h>` for the controls every console reaches for,
ARIA threaded: `switch_(hook, on, label)`, `seg(hook, on, label, count?)`,
`tab(hook, on, label)`, `meter(label, value, max, readout, cls)`, `kbd(text)`, and
`classes("row", cond ? "sel" : NULL)` for conditional class names without a format
string. Opt-in include, markup only.

**The browser half, tested without a browser** — `#include <webc/client_test.h>`:
`CLIENT_TEST`, `client_dispatch("row", "click")` (the view must have rendered once),
`client_press("meta+k")`, `client_type("dest", "LHR")`, and `EXPECT_SIGNAL(live, 1)`.
The seams are the bridge's own tables, so anything the browser does through a verb, a
test can do through a hook.

### 13e. The boot block: a project with no HTML file

A fullstack project does not ship `public/index.html`. The document is generated from
`webc.json`, so the mount point, the stylesheet, the title and — the part every project
got wrong — the `WebC.start()` options cannot drift from the routes the server actually
serves:

```json
"boot": {
  "title": "Contrail — Vela Air global ops",
  "lang": "en",
  "css": ["/public/style.css"],
  "icon": "data:image/svg+xml,…",
  "el": "#root", "portal": "#portal",
  "data": "/api/board",
  "hydrate": true
}
```

`page_auto()` builds the shell from this block when no `public/index.html` exists (a
project that ships one keeps it, and it wins). With `"inline": true` and a `str
webc_seed(Arena *)` in the server half, the page embeds the document it was rendered
against (`<script type="application/json" id="webc-seed">`) and the boot script reads it
instead of fetching: no tick can land between the render and the seed, and the seed round
trip disappears. Without `webc_seed` the fetch stays. The values are escaped on the way in — a
title with a quote is a title, and a `</script>` in a URL cannot end the boot script.
`webc new --template=fullstack` writes a boot block and no HTML file; `wc_shell_auto()`
and `wc_shell_has_boot()` are the doors a test uses.

**Every key, checked.** `webc.json` is read against a table, so a key that is not here is an
error that names the line and, within two edits, the key you meant — `"clinet"` is reported
as *did you mean "client"?*, not as a build that quietly produced nothing. A key with the
wrong type says what the key is for (`"port" wants a number — the port webc dev serves on`),
and a source file listed in two file sets is refused: it would be compiled twice, once per
half, and the linker would take whichever object it saw first.

**Paths may be folders.** A set holds paths, so `"server": ["src/server/admin/editor.c", …]` is as
ordinary as `"src/main.c"`. Nothing globs: the set *is* the answer, and a file nobody listed is
not compiled into the wrong half by accident. Two things make folders comfortable — every compile
line carries `-I src`, so `#include "app.h"` resolves from any depth, and each object keeps the
shape of its source's path (`src_views_admin_editor_c.o`), so a `posts.c` in two folders is two
objects, two symbols and two names in an error. What a move *does* change is `#embed "…"`, which
is relative to the file that writes it.

| key | type | what it is |
| --- | --- | --- |
| `name` | string | the app's name, in logs and the page title |
| `version` | string | `webc version`, the deploy manifest |
| `description` | string | one line for the manifest |
| `entry` | string | the server half's main source (default `src/main.c`) |
| `web` | string | the browser half's entry, for a project with no server binary |
| `database` | string | a URL: `sqlite:./data/dev.db`, `postgres://…` |
| `migrations` | string | the migrations directory (default `./migrations`); handed to the binary as `WEBC_MIGRATIONS` |
| `port` | number | the port `webc dev` serves on |
| `cflags`, `ldflags` | string | extra compiler / linker flags, space separated |
| `server`, `shared`, `client` | list | the three file sets — one path, one set; a path may name a file in any folder |
| `dev` | object | `port` (the reload socket), `connect` (origins to proxy: one string or a list) |
| `boot` | object | the page shell: `title`, `lang`, `icon`, `css`, `el`, `portal`, `data`, `wasm`, `hydrate`, `inline`, `html` |
| `csp` | object | the Content-Security-Policy: `img_src` (a list, default `'self' data:`), `script_src` (a list, default `'self'` + the page's nonce), `no_default` (the app writes the header itself) |
| `not_found` | string | an HTML file a browser gets for a URL nobody routed, e.g. `public/404.html` |
| `lint` | object | `exempt`: up to 16 path suffixes `webc lint --all` leaves alone (§13g) |


### 13f. `webc.json`: the 404 page and the policy

The default for a URL nobody routed is `404` with a flat JSON error — right for an API, wrong
for a browser. Two keys change only that case, and neither changes anything when it is absent:

```json
"not_found": "public/404.html",
"csp": {"img_src": ["'self'", "data:", "https://cdn.example.com"]}
```

`not_found` is the page a browser gets: the file's markup inside the app's own shell, so the
`404` carries the title, the theme and the nonce like every other page, with
`head_title("Not found")` in the tab. §8c is the mechanics — what wins, what is left alone, and
what a whole-document file does. **A client that sent `Accept: application/json` still gets the
JSON error**, and a route that answered `404` with HTML keeps its own page.

`csp` is the policy as four parts instead of one header spliced by hand (§8a). A project that
needs neither key writes neither key, which is why this is a section about two settings and not
a page about configuring a framework.

### 13g. `lint.exempt`: when an app's history has to name the word

`webc lint --all` reads the app's docs as well as its code, and a document that keeps an honest
history — "we used `parse_iso(` here and it was wrong, and this is why" — names a retired
spelling *in order to forbid it*. A grep cannot tell that from teaching it, so the gate fails
on the one file that is being helpful, and the usual answer is to delete the history.

```json
"lint": {"exempt": ["docs/history.md", "docs/appendix/"]}
```

A suffix match, so the entry may be relative, absolute or a directory. Up to sixteen, which is
more than any app's history needs. The exemption applies to the docs half only: an exempt file
is still compiled, still tested, and its code is still linted — what is skipped is the prose
scan. A key that is not `exempt` is reported by the same unknown-key check as every other
(`"exemptt"` → *did you mean "exempt"?*).

---

`webc check` ends with a **wiring pass**: `named()`/`named_field()` in the views
reconciled against `bind()`/`bind_field()` in the client half, actions against the names
that wire them (`bind*`, `on_key`, `on_ready`/`on_nav` — and `on_click(act)`/`on(act, …)` in a
view, which *is* a wiring: the element carries the listener), `on_key` specs against the
bridge's own grammar, and the classes the views emit against the selectors the stylesheets
carry (both directions). These are the strings the compiler cannot check — the bugs of three
real sessions were all wiring, and so is a note nobody reads: an app that wires every button
with `on_click` would report every one of those actions as dead code. Notes for choices
(a palette-only control, a state class), errors for the things that cannot work (a bind to a
name no view declares, a key spec nothing would match). `webc check --wiring` runs just this
pass, no build — the thing an editor runs on save.

Two shapes the pass had to learn, both of them ordinary in a growing app:

- **A hook name computed at runtime still wires its action.** `for (i = 0; i <= n; i++)
  bind(tab_hook(i), "click", tab_pick)` is the whole tab strip — one action, n names off a
  table. The pass reads the call's own closing paren (an argument's paren is not the end of
  the call), skips quoted arguments, and records `tab_pick` as reachable. It cannot name the
  hooks, so it says nothing about them; a *test* that dispatches every name in the table is
  the check for that half, and it is the better one — it runs the view.
- **A comment is not a wiring.** These notes are reconciled from source text, so `//
  on_click(dead_code)` in a prose line makes a dead action read as reachable, and a
  `// see on_click(tab_flat)` would emit a prototype for an action that does not exist. Lines
  are stripped to their code before the pass reads them (`//` to end of line, `/* … */`
  skipped, both only outside string literals).

## <a name="cli"></a>14. The CLI

```
webc new <dir> [--template=server|web|fullstack|board]
                            scaffold src/, public/, migrations/, webc.json, tests/
                            `board` copies examples/board: a kanban using nine shelf
                            modules, with its own suite
webc build [--release]      compile against libwebc.a into .webc/<name>
                            (and .webc/app.wasm when webc.json names a `client` or `web` source)
webc build --wasm           compile only the browser half (.webc/app.wasm)
webc build --explain        say why each file was compiled — the object cache, out loud
webc build --target wasm    compile for wasm32 (needs wasi-sdk, see §13b)
webc dev                    build, run, rebuild on save. A css edit swaps one
                            stylesheet, a client edit swaps the wasm module under the
                            page (DOM, signals, scroll and caret survive), a server
                            edit restarts the process, a build failure shows the
                            translated error over the last good page
webc serve [--port N] [--host H] [--no-build] [--quiet]
                            the app as a deployment runs it: a release build, no
                            watcher, gzip on. A browser-only project is served by the
                            framework's own chain over public/ and .webc/app.wasm; a
                            project with a server half is built and exec'd, and gzips
                            through the framework's default chain
webc test [--filter=suite] [--test=name] [--list] [--macros] [--json] [--watch]
                            build and run every tests/*_test.c plus the JS bridge test;
                            --filter picks suites (and runs them whole), --test narrows
                            by test name, --list prints the suites, --macros type-checks
                            every public DSL construct for both targets, --watch reruns
                            whenever src/, tests/ or webc.json changes
webc check [--json]        build + test + doctor + boot + schema + routes + lint, one line
                            each (boot and schema are skipped for a browser-only app — a
                            `client` set and no `server` set has no binary to run).
                            `--json` is the same gate as `{ok, steps:[{name, ok, output}]}`
webc explain [--json]       build, then say what each error means in WebC — the language
                            server's answers, pointed at a build instead of a caret (§14f)
webc explain --project      no build: run the engine over the sources on disk
webc lint [--all] [-v]      the retired word in app code: arenas, target macros, hand-built
                            JSON, a parser in a test, an ignored parameter. Names the line,
                            the call and what to write instead. `--all` adds the examples,
                            the docs and every scaffold it can generate; `--v` shows all
webc lint --rules [--json]  the same table as a page (or as data): the rule, what it is,
                            and what to write instead — generated, so it cannot drift
webc lint --skill DIR       that table, doc scope, over a skill's own prose: a skill that
                            teaches a retired spelling is worse than no skill
webc lint --json            every hit as `{file, line, token, what, instead}`
webc api [<query>] [--limit N] [--json]
                            no query: the surface, counted against its budget. A query:
                            the declaration, its header, and the doc line above it — the
                            answer to "does this name exist", without reading the headers [--json] the public surface against its budget: writes api.txt, or
                            diffs the headers against it and fails when the count grows —
                            and when any single module grows past its own line. It also
                            splits the count: core names, and shelf names (§12i)
webc stdlib [module]        the opt-in modules, and what one of them is for
webc stdlib <module> --css  the stylesheet that module expects
webc stdlib --used          which modules this app already includes
webc add <module>           install one: copies its stylesheet into public/stdlib/ and
                            prints the #include and the <link> (--force overwrites)
webc seed [--reset]         run the app's boot, then load content/<table>.json (§9b)
webc migrate up|status|dry|new NAME
webc migrate diff [--write] [name]
                            what the models say the tables should be, as additive SQL: the
                            columns and tables that are missing, generated by the process
                            that has the models. Without --write it prints, with it writes
                            migrations/NNNN_<name>.sql and does not apply it (§9b2)
webc scaffold <Model> [--force]
                            list, form, save and delete for one model, as a file you own:
webc scaffold live <Model>
                            the live page in one command: the SSE route that seeds
                            every tab with the whole table, the mutations that
                            broadcast after they write, and the test that proves
                            the room holds the newest document. The client
                            half is three lines, printed where they belong:
                            src/admin_<model>.c, a test, and a line in webc.json's server
                            set. It reads the model from the app's descriptor, never edits
                            the model, and writes nothing that exists unless --force (§9b3)
webc doctor [--schema] [--json]
                            toolchain and project checks; --schema is the drift report,
                            --json is the same checks as `{ok, checks:[…]}`
webc routes [--json]        the declared routes, from the project index: method, path, the
                            file and line, and the view the route returns
webc docs [--emit DIR] [--list] [topic]
                            the framework documentation as files: SKILL.md plus one
                            reference per topic, written into `.webc/docs/` by default
                            and by `webc new` (§14g)
webc size [--json]          what the page costs: module and bridge, raw and gzipped,
                            the module's sections, and the statics that make them up;
                            --json is `{built, wasm{raw,gz}, bridge{raw,gz}, wire_gz,
                            budget{limit,over,ok}|null, sections[…]}`
webc size --modules         what each standard-library module costs: twenty probe apps,
                            built and gzipped, one line each (§12i)
webc get [-X M] [-d B] URL  fetch with the framework's own client (dogfood)
webc deploy [--target T] [--out DIR] [--tar] [--pack LIST]
                            stripped release binary + manifest + run script.
                            `--pack` adds the ops files that run it — docker,
                            fly, systemd, caddy, nginx, all (comma-separated) —
                            stamped from the manifest: the SSE-aware proxy
                            lines, the hardened unit, a Dockerfile whose base is
                            an ARG
webc ci [--gitlab]          the gate as a workflow: webc check + webc test, a
                            dist artifact on a tag, the framework pinned by one
                            ref at the top. A file the app owns
webc openapi [--out F] [--check]
                            the API boundary as OpenAPI 3.1, generated from the
                            routes and models the sources declare: bodies from the
                            body family, responses from json/json_list and their
                            status literals, `.max` as maxLength, `path_i64` as an
                            integer parameter. `--check` fails when the file
                            drifts — the same ratchet `webc ci` runs when the app
                            ships one
webc client --ts [--out F] [--check]
                            a typed fetch client for the same facts — wire types
                            plus Input twins, path params as arguments typed by
                            path_i64, pages left out. For the frontend
                            that is not moving today; `--check` is the ratchet

webc model from-db [--database URL] [--table NAME] [--write] [--out F]
                            the reverse of `webc migrate diff`: connect to a
                            database that exists and write the `WEBC_MODEL`
                            declarations its tables describe — table names become
                            model names, `varchar(n) not null` becomes
                            `Text, .max = n, .required = true`, a nullable
                            integer opts into `.null`. `--write` lands the models
                            (`src/models.h` + `models.c`, registered), and the
                            baseline: sqlite's own DDL (with IF NOT EXISTS);
                            postgres/MySQL print the `migrate diff --write` that
                            finishes it, now that the models are linked
webc learn [1..5]           five lessons, one idea each: real apps under
                            .webc/learn/, each failing its own gate (webc test,
                            or webc check for the wiring one) until the one edit
                            the brief names

webc fmt [--check]          clang-format over every .c/.h under src/, to the project's
                            own .clang-format; with none it says so and formats nothing
webc mcp [--root DIR]                     the MCP server, on stdio (§14f)
webc lsp [--root DIR] [--trace FILE]      the language server, on stdio (§14d)
webc lsp --selftest                       a scripted session, asserted
webc lsp --replay FILE [--record]          a recorded session, diffed
webc lsp --bench                          what one request costs
```

Three of those are about not lying to yourself:

**`webc build --explain`** prints the object cache's reasoning (`[stale] src/view.c: …deps
sub.h mtime=…`, `[cc] src/view.c`) instead of compiling in silence. `WEBC_TRACE_WATCH` still
elsewhere-documented what the dev server watches; this is the build's own account, as a flag,
because the moment somebody needs it is the moment they are staring at a build that did the
wrong thing.

**A failed build clears the cache and tries once more.** A cache that is stale in a way
mtimes cannot see — a restored checkout, a header touched back in time, a killed build's
half-written object — presents as a compile error in a file that compiles, and the cure is
`rm -rf .webc`. A failing build now says so, clears the object directory and rebuilds; if the
second attempt fails too, the errors are real. The wasm
half falls back to the one-shot command *and* clears its own object directory, so the slow
path repairs the fast one instead of masking it once.

**`webc size`** answers "what does this page cost, and where did it go" without devtools:

```
  webc size  bankroll

    module  .webc/app                   1.77 MB    69.0 KB gz
    bridge  assets/webc.js              62.5 KB    19.3 KB gz
    sent over the wire (both, gzipped):  88.3 KB

    where the module goes:
      code                               140.9 KB   7.8%
      data                                1.62 MB  91.9%
      custom (names, debug, producers)     5.0 KB   0.3%

    the biggest statics (from the object files):
      g_strats                            1.60 MB
      …

    A static declared without an initialiser is zero-fill and costs no bytes; one with an
    initialiser ships its zeros. Compare the list with the data section above: the static
    about that size is the one being paid for, and filling it in boot() moves it to zero-fill.
```

That output is not a demonstration: the command, run on a real app, finds 1.6 MB of `data` that
turns out to be one `static Strat g_strats[] =
{…}` — 21 partially-initialised structs holding 10 001 doubles each, whose zeros a wasm data
segment ships byte for byte. Declaring it without the initialiser and filling it in `boot()`
takes the module from 1.77 MB to 165 kB. Nothing else in the framework's tooling can tell you
that.

`webc.json`:

```json
{ "name": "myapp", "version": "0.1.0", "entry": "src/main.c",
  "database": "sqlite:data/dev.db", "port": 8080,
  "dev": { "port": 9999, "connect": ["https://api.example.com"] } }
```

`dev.connect` is the Content-Security-Policy `connect-src` the dev server sends: a browser
half exists to call somebody's API, and `webc dev` serves that half, so the dev policy
already allows any `https:` origin. Name the origins your app actually calls and the policy
becomes exactly those (plus the reload websocket) — a narrowing knob, never a widening one,
and it only ever applies to `webc dev`.

---


### <a name="lsp"></a>14d. The language server

`webc lsp` speaks LSP over stdio, from the same binary as the rest of the CLI: no Node,
no second download, no build. Point any editor at `webc lsp` and it has diagnostics,
completion, hover, definition, references, an outline and quick fixes for a WebC
project.

It is two engines with one line between them, because a WebC file is two things at
once. **C is clang's job** — a whole view type-checks in about a tenth of a second, and
a second hand-written C front end would be a second source of truth about C. **The DSL
is ours**: the macros that take a following `{ … }`, the model tuples, the three file
sets, the routes, the wiring. A per-file *region map* — which bytes are inside `view(…)`,
`el(…)`, `GET(…)` and which are plain C — decides which engine answers a given caret.

That translator is the product. The compiler answers `page(Nope, …)` with
`use of undeclared identifier 'view_Nope'` behind five frames of `WC_CAT`; the server
answers "no view named `Nope` in this project — `view(Nope)` draws it, or
`declare_view(Nope)` in the shared header announces it", and offers the line to add.
`GET(…)` in a `shared` source is a caret error naming the half, not eight cascading
syntax errors about parameter declarators.

The compiler half is the same compiler `webc build` runs, asked about the buffer the
editor is holding rather than the file on disk. Two rules keep it from being a second
opinion: a diagnostic inside a DSL macro's own parentheses is dropped (those bytes are
the WebC engine's to explain, and the compiler speaks about them in names like
`view_Nope`), and a block that never closed stops the compiler from running at all.
`WEBC_CC` picks the compiler; when a project's check costs more than 120 ms it moves
from every keystroke to every save, because a language server that stutters is a
language server people turn off.

What it knows, it knows from the sources: the models (including the ones declared in
headers, which no `webc.json` ever lists), the views and where they are drawn, the
actions and what is wired to them, the routes and the view each returns, and which
standard-library module may appear in which half. Nothing under `.webc/` is read, so a
fresh clone is a working workspace.

It deliberately does **not** reformat a view. `clang-format` rewrites
`el("h1") { text("Todos"); }` into four lines, which is not this language's style;
document formatting is therefore refused with a reason, and range formatting outside a
DSL block is what `webc lsp` offers instead.

There is a second opinion in most C editors, and it is clangd. So the server writes
`compile_commands.json` at the project root — the flags `webc build` uses, one entry per
source — which clangd finds without being told anything. Without it clangd invents
errors on a WebC file it cannot resolve; with it, the twenty-one it invented in this
repository became none. The file is generated, gitignored, and the server never reads it
back.

The editor client lives at `editors/vscode/`: no dependencies, no build step, nothing
`require`d from a registry. Where the binary is comes from `resolve.js` — the setting
with `~` and `${…}` expanded, else `build/webc` in the workspace or a parent, else the
PATH — because a settings field is typed by a human and `spawn(2)` expands no tilde.
`make lsp` checks that the client still loads and that the resolver is right about every
way a path can be wrong (missing, a directory, not executable, relative, on no PATH), so
a client that silently ignores the setting is a red gate rather than a broken window.

`make lsp` is the gate: `--selftest` asserts what the server says about a real project,
and `--replay` diffs the protocol against a recorded session
(`tests/lsp/sessions/*.jsonl`), so a wire change is a diff in review rather than a bug
report. `--bench` prints what one request costs, and `--trace FILE` writes every message
it saw as JSONL.

### <a name="vscode"></a>14e. The VS Code extension

`editors/vscode/` is that client: `extension.js` speaks JSON-RPC to `webc lsp` over stdio
itself (no `vscode-languageclient`), and `resolve.js` decides which binary to spawn. It is
three settings and two commands:

| setting | default | meaning |
|---|---|---|
| `webc.lsp.path` | `""` | the binary. `~`, `${workspaceFolder}`, `${userHome}` and `${env:NAME}` are expanded; a relative path is taken from the workspace folder; empty means `build/webc` in the workspace or a parent, else `webc` on the `PATH` |
| `webc.lsp.args` | `[]` | extra arguments, e.g. `["--trace", "/tmp/lsp.jsonl"]` |
| `webc.lsp.enabled` | `true` | start the server at all |

| command | what it does |
|---|---|
| `WebC: Show the language server log` | the output channel, including every diagnostic the server published |
| `WebC: Restart the language server` | respawn, without reloading the window |

It spawns `<bin> lsp --root <workspace> <args>`, clears its diagnostic collection whenever
the server stops (a dead server must not leave yesterday's errors on screen), and re-reads
its settings on change — a setting that is only read once at activation is
indistinguishable, from the outside, from one that is ignored. A failed start is **one**
message that says which failure it was (not found, not executable, exited) and quotes the
last lines of the server's stderr, with the *resolved* path in it.

Installation is a copy, a symlink, or a `.vsix`; `code --extensionDevelopmentPath=
"$PWD/editors/vscode"` runs it out of the tree. The guide
[The editor](../website/src/content/guides/editor.md) has the commands for VS Code, Cursor, Antigravity,
Neovim and Emacs, and the troubleshooting table.

---

### <a name="mcp"></a>14f. The MCP server

`webc mcp` speaks the Model Context Protocol over stdio, from the same binary as
everything else — no Node, no second download, no version skew between the server that
answers and the compiler it answers about. The argument is the one that gave us
`webc lsp`, and it is stronger here: a server that reports diagnostics must *be* the
compiler.

It exists for one reason, stated as four:

| What an agent needs | A shell already gives it | What MCP adds |
|---|---|---|
| the gate, after a change | `webc check` | `{ok, steps:[{name, ok, output}]}` — no prose to parse |
| the project's own index | `grep` | models, fields, views, actions, routes and each file's half, with positions |
| the whole public surface | `grep api.txt` | a search that returns the declaration, its header and its doc line |
| the page it just built | two processes and `curl` | a dev server held across calls, and `webc_fetch` |

Seven tools, and one rule: **every tool is a door onto something the CLI already does,
and nothing parses human output.** If a tool needs data, the command that owns it learned
`--json` first. That is what keeps this from becoming a second, disagreeing front end.

| tool | returns | writes |
|---|---|---|
| `webc_check` | the `--json` gate: build, tests, doctor, boot, schema, routes, lint, consistency | — |
| `webc_facts` | models (+fields, qualifiers), views (+context), actions, routes (+view), files (+half, modules) | — |
| `webc_api` | `{query} → matches:[{header, line, declaration, doc}]` | — |
| `webc_test` | `{filter?} → {ok, tests:[{suite, passed, failed, output}]}` | — |
| `webc_dev` | `{action: start\|stop\|status} → {running, url, port}`; one server held by the process | ✔ process |
| `webc_fetch` | `{path, method?, body?} → {status, content_type, body}` from that server | — |
| `webc_scaffold` | `{template, dir}` or `{add_module}` → what was written | ✔ files |

Resources: `webc://project` (the index, refreshed on read) and `webc://reference` (the
heading index of this document, so a reader can ask for §9a2 instead of loading 3 392
lines).

Point a client at it:

```json
{ "mcpServers": { "webc": { "command": "/path/to/WebC/build/webc", "args": ["mcp"],
                            "cwd": "/path/to/your/app" } } }
```

`webc mcp --selftest` is the gate: it runs a scripted session against `examples/todo`
and asserts what the tools answer — the same shape as `webc lsp --selftest`, for the same
reason (a protocol change should be a failing test, not a bug report). `make mcp` runs it.

`--root` defaults to the working directory, which is what a client passes as `cwd`; if
that directory has no `webc.json` but one of its ancestors does, the nearest project wins,
so a session started in `src/` still answers about the app. With no project anywhere
above, the directory given is used and the tools report a project with nothing in it.

Two knobs worth knowing: the dev server's own output goes to `.webc/mcp-dev.log`
(stdout is the protocol channel and a stray banner corrupts every message after it), and
`webc_dev` refuses to start over a build that does not compile — a serve that quietly
serves the last good binary is the most confusing way to learn your change is broken.

### <a name="agents"></a>14g. Working with agents

Three surfaces, and each is authoritative for a different thing. The rule that keeps
them from disagreeing: **a contract is written once and the other two are generated from
it or call it.**

| Surface | Authoritative for | Kept honest by |
|---|---|---|
| `docs/reference.md`, the site guides | what is true | review; `webc lint --all` over the docs |
| `skills/webc/` | what to *do*, and where to read | `make skill-check`: regenerated from the docs |
| `AGENTS.md` (scaffold and repo root) | the gate to run, and the four mistakes to avoid | `webc lint --all`; a scaffold test builds and checks every template |
| `webc mcp` | the same answers as data | `make mcp`: a scripted session, asserted |

**The documentation in an app.** `webc new` writes the skill — `SKILL.md` and its
`references/` — into the app's `.webc/docs/`, and the scaffold's `AGENTS.md` names it. The
argument is the one this section is about: an agent inside an app must not have to find the
framework's sources to answer "what does this name do", and a session spent grepping a
checkout for `auth_hash_password` is the failure that put the files there. `.webc/` is
gitignored, so the copy is never vendored and never stale; `webc docs` rewrites it,
`webc docs --list` names the topics, `webc docs <topic>` prints one, and `webc api <query>`
answers the narrower question (the declaration, its header and the doc line above it) from
the headers this binary was built against.

**`AGENTS.md`.** `webc new` writes one into every scaffold: `webc check` as the gate, the
half rules (`db_*`, `req`, `path*`, `query`, `body` are server-half names), one
`WEBC_MODEL` for struct/validation/JSON/SQL, `fmt()` and `persist()`, migrations, and
"believe `webc lint`". The repository root has its own, for a different reader: the gates,
the house rules (one scanner, one index, one set of diagnostics; the api ratchet; a budget
is a gate; machine output is a contract) and the layout. Neither is generated — they are
short on purpose, and a change to either is a change to what every agent does next.

**The skill.** `skills/webc/SKILL.md` is written by hand: the five ideas, the loop, the
project shape, recipes, the rules that bite, and a routing table into `references/`. The
fourteen leaves under `skills/webc/references/` are generated by `tools/gen_skill.py` from
`website/src/content/guides/*.md`, anchored sections of this document, and `webc lint
--rules` (`traps.md`). Generation is the whole argument: a skill that teaches a retired
spelling is a language nobody can compile, delivered with confidence, and this framework
has retired forty-six of them and deleted a render macro outright; `webc lint
--rules` is the table, and the table is the count.

`make skill` regenerates; `make skill-check` is the gate, in three parts:

| Part | Fails when |
|---|---|
| `tools/gen_skill.py --check` | a leaf would change — the documentation moved and the skill did not |
| `webc lint --skill skills/webc` | the skill's own prose names a retired spelling (generated leaves are exempt: they quote this document, which is gated where it lives) |
| `website/tools/snippets.py` | a `c` block in a skill does not compile, has an ellipsis, or has no leading comment saying which file it is |

**Machine output.** A command with a `--json` face renders what it did; it is never a
second implementation. `check`, `test`, `routes`, `doctor`, `api`, `stdlib`, `size`,
`lint` and `explain` have one (§14's command table has the shapes). Terminal behaviour is
a contract too: colour is off when stdout is not a terminal, `NO_COLOR` is set, or
`TERM=dumb` is (with `WEBC_COLOR` as the override); a failed build prints once, after one
cache-cleared retry, without the compiler's command line (`WEBC_VERBOSE` restores it); and
a failed build calls the translator automatically, so `webc build` and `webc check` say
the same sentence about the same mistake (`WEBC_NO_EXPLAIN` turns that off). **A tool
never parses human output** — if a tool needs data, the command that owns the data learns
`--json` first.

**`webc explain`.** It reads the compiler's `path:line:col: error:` lines, asks the same
engine the language server uses what that line is, and prints one sentence per mistake —
deduped per `file:line`, wrapped at 92 columns at the sentence's own indent, so a
transcript and a terminal agree byte for byte. `--project` skips the build and reports
every finding in the sources; `--raw` also prints the compiler's own output, untranslated,
for when the translation itself is what is wrong. It refuses to run over the framework checkout: that
tree is a hundred example apps, not an app.

The narrative version of this section — the loop, the two failures, the install — is the
site's [agentic coding guide](../website/src/content/guides/agents.md).

---

## <a name="testing"></a>14b. Testing

`webc test` compiles each `tests/*_test.c` into its own binary (with the app's own
sources linked in, its `main` renamed), runs it, and prints go-test-shaped output.
The surface is `<webc/test.h>`:

```c
#include <webc/test.h>

TEST(creates_an_account) {
  Response r = webc_test_request("POST", "/accounts");
  ASSERT_EQ(r.status, 201);
  Account a = { .email = str_lit("ada@example.com") };
  ASSERT_OK(model_validate(Account_modelof(), &a, nullptr));
}
```

- **`TEST(name) { }`** — a test. It registers itself with a constructor, so a file
  is a list of blocks and nothing is enumerated by hand. No `main`, no list to
  forget.
- **`TEST_MAIN()`** — the binary's entry point. **`TEST_SETUP(fn)`** /
  **`TEST_TEARDOWN(fn)`** run once around the file (a database, a scheduler).
- **`SUBTEST("label") { }`** and **`SUBTESTS(rows) { ... }`** — a subtest is one
  block; `SUBTESTS` runs the block once per row of a static array, binding the row
  to `r` and using its `name` member for the label. A failing row does not stop
  the next one.
- **Assertions**: `ASSERT`, `ASSERT_TRUE/FALSE`, `ASSERT_EQ/NE/LT/LE/GT/GE`,
  `ASSERT_FEQ`, `ASSERT_STR_EQ` (a `str`/`Buf`/`Response`/C string against a literal),
  `ASSERT_STR` (two of them), `ASSERT_STR_CONTAINS`, `ASSERT_MEM_EQ`,
  `ASSERT_NULL/NOT_NULL`, `ASSERT_OK/ERR/ERR_KIND` (anything with `.ok`).
  An assertion records a failure and **continues**, like go's `t.Error`.
- **Responses**: pass one straight to `ASSERT_STR_CONTAINS(r, "…")`, and read a header
  by name: `ASSERT_STATUS(r, 303)`, `ASSERT_HEADER(r, "Location", "/")`,
  `ASSERT_HEADER_CONTAINS(r, "Set-Cookie", "HttpOnly")`,
  `ASSERT_HEADER_COUNT(r, "Content-Security-Policy", 1)`, `ASSERT_BODY_LEN(r, 12)`. A
  duplicate header is legal HTTP and a real bug; counting is the only way a test can see
  it. **Header names are matched case-insensitively** (RFC 9110 §5.1): a handler stores
  `Set-Cookie` as it spelled it, and a test may ask for `set-cookie` — a `memcmp` comparison
  misses it, which is why every app that asserts on a session cookie writes its own helper.
- **`FAIL(fmt, ...)`** records a failure with a message; **`SKIP(why)`** stops the
  test and reports it skipped.
- **`try(expr)`** inside a test panics with the error (a `TEST` body returns
  void, so it cannot return the `Result`); the runner turns that into a failed
  test with the message. Every test runs inside a panic frame, so a panic in
  application code — not just in an assertion — fails that one test and the rest
  of the file still runs.

**The client half of a test.** A request with no memory is not a browser, and a test of a
*mutating* route written without one rebuilds a browser: a cookie jar by hand, a form body by
hand, and a helper for the header lookup. `TestClient` is that browser (§14b1 below):

```c
TEST(the_save_route_writes_the_row) {
  TestClient c = test_client();                       // a jar, per test
  Response r = tc_post_form_at(c, "/admin/posts/save", Post, &draft);
  ASSERT_REDIRECT(r, "/admin/posts");                 // 302/303/307 *and* the Location
  ASSERT_DB_ONE(Post, slug, str_lit("the-last-ferry"));  // the row it should have written
  ASSERT_DB_COUNT(Post, 1);
}
```

What is actually asserted, spelled out: `webc_test_request(method, path)` builds a
request, runs the router and the middleware chain, and returns the `Response` —
no sockets. `webc_test_request_ex(method, path, headers, body, body_len)` is the
same with a header block (`"Cookie: session=…\r\n"`) and a body, which is what a
test of an authenticated route needs. Both convert a handler panic into a 500,
exactly like the server.

An app's tests link the app. `webc test` compiles the app's sources (found the same
way `webc build` finds them) with `main` renamed, so the routes are the real ones;
the state is built by calling the app's own `webc_boot()` from `TEST_SETUP()`
(`<webc/app.h>`). `examples/todo` and `examples/accounts` are the worked examples:
8 and 7 tests, real database, real sessions, no socket.

### 14b1. The test client: a jar, a form encoder, and the row

```c
TestClient c = test_client();                      // the current test's arena; nothing to free
Response r = tc_get(c, "/admin");                  // GET, with the jar's cookies attached
Response p = tc_send(c, "PATCH", "/admin/posts/1");// any method
r = tc_post_body(c, "/theme", str_lit("to=dark")); // a body of your own
r = tc_post_json(c, "/api/posts", Post, &p);       // the model, as JSON
r = tc_post_form_at(c, "/admin/posts/save", Post, &p);  // the model, as the form it would draw
```

- **The jar.** Every `Set-Cookie` the response carries is kept (name and value, not the
  attributes) and sent back on the next request; `Max-Age=0` deletes one. A second
  `test_client()` in the same test is a second browser — the two-users case, which was
  untestable when a test could only send headers it had spelled itself.
- **The form encoder.** `tc_post_form*` serialises the model the way `model_from_form` reads
  it: one input per field in declaration order, the pk left out (a create does not post the
  id back), a `bool` as `on`/`off`, an enum as its value's name, a `VecStr` as one
  comma-separated value, and everything percent-encoded as a browser encodes it.
- **The assertions a mutation test needs.** `ASSERT_REDIRECT(r, "/where")`,
  `ASSERT_DB_ONE(T, field, str)` / `ASSERT_DB_ONE_I64(T, field, n)` ("the row it should have
  written"), `ASSERT_DB_COUNT(T, want)`.

It does not run JavaScript — that is `webc test --client`, which replays the client half's
batches through the real bridge — and it does not follow redirects: a test that follows one
hides the 303 it came for.

**`webc test --watch` reruns the suite** whenever `src/`, `tests/`, `shared/`, `content/` or
`webc.json` changes, clearing the screen each pass, so what is on it is the last run and not
the history of them. `--filter` picks suites (and runs them whole); `--test` narrows by test
name across every suite, which is the slower one because every suite builds.

---

### 14c. Assertions about structure

A substring test passes when the markup moved somewhere the CSS no longer reaches —
which is exactly the regression a redesign causes. Two assertions speak the languages a
response is actually written in:

```c
ASSERT_JSON(res, "data.0.views", "42");                       // by path
ASSERT_JSON_STR(res, "data[0].slug", "the-last-ferry");
ASSERT_HTML(res, "article.lead > h2 a");                      // the structure survived
ASSERT_HTML_TEXT(res, "article.lead > h2 a", "The chair nobody notices");
ASSERT_HTML_COUNT(res, "article.card", 6);
ASSERT_HTML_ABSENT(res, "article.lead");
```

The selector is a subset of CSS: tag, `.class`, `#id`, `[attr]`, `[attr="value"]`,
descendant (space) and child (`>`). `wc_json_path(JVal*, "a.b.0", &out)` and
`wc_html_count(html, sel)` are the functions underneath, for a test that wants the
value rather than the assertion.

Structured data is neither JSON nor HTML, and it is the thing that breaks silently — a page
whose `author` gained an empty `name` still renders, still passes `ASSERT_HTML`, and has
quietly lost its rich result. `ASSERT_LD` reads the first `application/ld+json` block out of a
page and asks it the same by-path question:

```c
ASSERT_LD(res, "headline", "The last ferry");
ASSERT_LD(res, "author.name", "Ada Okafor");
ASSERT_LD_ABSENT(res, "datePublished");     // a draft has none, and the key is not ""
ASSERT_JSON_DOC(doc, "publisher.logo.url", "https://example.com/logo.png");  // a document you built
```

`ASSERT_JSON_DOC` / `ASSERT_JSON_DOC_ABSENT` are the same question about a JSON *document*
rather than a response — what a serializer, a writer, or a `ld_*` call is tested at, before a
route exists. `wc_ld_block(html)` returns the payload if a test wants it directly.

An HTML assertion matches the server-rendered markup with its `data-wc` anchors in it,
so it tests the page a browser receives, not a string this test happened to expect.

**Every one of them takes any HTML-ish thing**, not just a `Response`: a `str` from
`wcomp_html_ctx` (the browser half's own render), a `Buf`, a `const char *`. That is the
whole of what a client-side test needs to read a card:

```c
str html = wcomp_html_ctx(&comp, view_Stats, nullptr);
ASSERT_HTML(html, ".card.ruin b");
ASSERT_HTML_TEXT(html, ".card.ruin span", "ruined");
ASSERT_HTML_NUMBER(html, ".card.ruin b", 96.3);   // "$10,038" reads as 10038, "4.2%" as 4.2
```

`ASSERT_HTML_NUMBER` is the one that replaces a hand-written scanner per app: it takes the
text at a selector, keeps the first number in it (any currency symbol, separators, a trailing
`%` or a unit around it are dropped) and compares it to within `1e-6`. A text with no number
at all fails, so an empty selector cannot pass by accident.

**A client test does not hand-write its declarations.** `webc wires > src/wires.h` emits the
view prototypes, the action prototypes (both the ones `bind()` names and the ones a view wires
with `on_click()`), and a `WIRES_<field>` constant per named field — so a test file starts
with `#include "wires.h"` instead of one `ACTION(x)` line per button, and a new button needs
no test-file edit at all. It is generated from the sources, so it cannot drift.

**A pointer can be dispatched** with a position, which is what makes hover code testable:

```c
client_dispatch_at("plot", "pointermove", 150, 10, 600, 200);  // x, y, w, h
EXPECT_SIGNAL(tip_on, 1);
```

## <a name="ops"></a>15. Ops

- Logs: `log_info("request", { "path", req->path }, { "status", ... })`. Text by
  default, JSON with `WEBC_LOG=json`; level from `WEBC_LOG_LEVEL`.
- Metrics: `metrics_counter_inc`, `metrics_observe`; `/metrics` in Prometheus format
  when `WebcOptions.metrics` is set. The server exports `webc_conns`,
  `webc_tasks`, `webc_requests_total` and the connection-close counters
  `webc_conn_closed_eof_total` (the client closed: normal keep-alive end),
  `webc_conn_close_idle_total`, `webc_conn_close_partial_total` (a request was
  cut short) and `webc_conn_read_error_total` (a read failed on our side — the
  one counter that should stay at zero).
- `/debug` reports heap, arenas, tasks and route count; `/debug/cpu?ms=2000` runs the
  sampling profiler for one window and prints a flat profile, and `webc_profile_start()`
  / `webc_profile_stop()` / `webc_profile_report()` do it from code. The sampler
  signals every process thread — including threads the runtime does not own — from a
  dedicated thread, and never itself.
- Limits, all answerable: `WebcOptions{ .max_body, .max_header_bytes, .header_timeout_ms,
  .max_conns, .max_multipart_parts, .max_multipart_bytes, .max_json_depth }`. A body
  over the limit is a 413, headers over the limit a 431, JSON nested deeper than 256 a
  400 that says so.
- Multipart (`<webc/multipart.h>`): `multipart_parse()` plus
  `model_from_multipart()`, so a form with a file lands in the same model as one
  without. Part and byte limits are arguments.
- Redis and the query cache add their counters to the same list (§9d): `webc_cache_*` for what
  the cache did and `webc_redis_*` for what the wire did. `WEBC_TRACE_CACHE=1` prints one line
  per statement (`[cache] select … -> hit`), `WEBC_TRACE_DB=1` prints the statements themselves.
- Budgets are a gate: `make budget` measures SPEC §10 and exits non-zero on a miss (SSR ≥ 100k
  req/s, 1 KB JSON ≤ 1 µs, patch ≤ 0.2 ms, wasm hello ≤ 24 KiB gzip, serve binary ≤ 460 KiB, cold
  build ≤ 1500 ms). The serve binary's line moved from 400 KiB to 460 KiB when Redis and the
  cache became part of it — hello measured 456 544 B on the machine this was written
  on; `make redis-test` runs the client and the cache against a real `redis-server`.
- Shutdown: SIGINT/SIGTERM stop accepting, drain for `drain_ms`, then exit.

---

## <a name="limits"></a>16. Limits and deviations

These are measured facts, not guesses:

1. **No nested functions.** C23 removed them, so `routes()` cannot contain `GET(...)`.
   Routes are file-scope macros; grouping is done with path prefixes and per-route
   middleware (`GET_MW`).
2. **Bare qualifier flags are invalid C.** `.required` is a syntax error; write
   `.required = true`. There are no token-pasting macros named `required`/`max`, because a
   macro that turns a field into a flag cannot be read back by the compiler's own message.
3. **`state(...)` shadows `state(2)`** in any translation unit that includes
   `<webc/web.h>`; `select(...)` shadows `select(2)` the same way, and that one predates
   it. Include the system headers first. Neither is a problem in a translation unit that
   does not call the POSIX function — and a file that does can call `select_wait` (see
   `task.h`) or keep the system header in its own translation unit.
4. **No guard page on task stacks.** `mprotect(PROT_NONE)` on one page of an anonymous
   mapping makes its neighbours SIGBUS on Darwin (verified with isolated repros), so
   overflow detection is by fault, not by message. A canary at the stack base fires
   falsely on every task because Darwin's ucontext writes its own frames there.
5. **Redis is one server, one database, one process pool.** No Sentinel, no Cluster, no
   `MOVED` redirection, no `SCAN`-driven invalidation: the query cache's counter keys and entries
   assume a single server everybody shares. A Cluster deployment is a different client, not
   a configuration of this one.
6. **TLS is a build feature, off by default.** `-DWEBC_TLS=openssl` (OpenSSL headers
   and libraries; `make tls-test`) enables `res_tls`/`tls_connect`. A build without it
   refuses to start when a listener is configured for TLS instead of serving plaintext
   on a port someone believes is encrypted. Postgres has no TLS, so `sslmode` is not
   implemented.
7. **The wasm target needs wasi-sdk** (~170 MB, a separate download; `webc doctor`
   reports whether it found it). The browser half has no threads, no filesystem and no
   SQL: `include/webc/web.h` is the subset it compiles — core, collections,
   models, JSON, signals, DOM. Verified end to end in Chrome: render, keyed list
   add/remove/toggle, two-way bound input, SSR markup hydrated in place.
8. **Postgres and MySQL are wired and tested** — `make pg-test` starts
   `postgres:16-alpine`, `make mysql-test` starts `mysql:8`, each runs the same suite across
   every driver, and each removes its container. Postgres was untested for most of this build;
   the SQL lexer and the driver twins were found by running it. Only PG16 and
   MySQL 8 are tested — MariaDB gets the same protocol but not its own run, there is no
   `PREPARE` reuse on the postgres side, and there is no pool-exhaustion test for either.
   The MySQL driver has no RSA public-key exchange, so `sha256_password` and
   `caching_sha2_password`'s full authentication need TLS.
9. **A component instance costs 56 B, and `comp()` is opt-in.** One heap `WInst` (24 B) and one
   table slot (32 B) per instance *drawn*, whether or not it reads state; a part called without
   `comp()` pays nothing and shares its position's state (§11d). The 20 000-row table with a
   component per row is therefore still the wrong shape — per-row state is the same arithmetic.
10. **`wcomp_render` re-renders the whole view and diffs.** That is component-level
   reactivity, not per-node: the DOM writes are minimal, the C work is not. §12j's live
   bindings are the per-node half — a leaf the view declares once — but they do not make a
   *structural* change cheap, and they do not raise the 20 000-op mount budget (§16.8).

11. **`webc_serve` runs one worker per core by default** (`cpus = 0` → the online
   processor count, capped at 64; set `cpus = 1` to force a single worker). A task
   may be resumed on a different worker than it started on, so request state and
   the current arena travel with the task, never in a thread-local
   (`webc_req_current()`, `wtask_local()`, and the scope accessors below). Measured with
   `make load` on a 10-core machine: 16 780 req/s of SSR HTML at 16 keep-alive
   connections and 12 112 req/s of one-request-per-connection churn, zero errors
   and no leaked connections or tasks over ~260 000 requests. The bugs that had
   to be fixed first are in `findings/scheduler-and-server-bugs.md`.

12. **Request state is task-local, not thread-local.** If you add an API that
   needs "the current request", the markup build in progress or a per-request
   arena, use the task-local accessors (`webc_req_current()`, `wbuild_current()`,
   and the scope arena the framework installs) — never an arena of your own. A
   `_Thread_local` there is a bug: many tasks share a worker thread, and a task
   that parks resumes on another one. `wbuild_current()` cost a real outage —
   `examples/todo` reads the database *inside* its view, the task parked there,
   and every worker began answering 500 "el() outside a build"; one render then
   dereferenced the null and the process died with SIGSEGV at address 72.

13. **A request allocates nothing that outlives it.** The body, headers, params
    and rows live in the request arena; the render's component (three arenas and
    a command buffer) is destroyed before the response is written; a log line
    allocates nothing at all. Measured: `/metrics` `webc_heap_bytes` is byte-for-byte
    identical before and after 130 000 SSR-with-database requests, and 95 000
    text requests; `webc_arena_bytes` is flat. RSS settles around 11 MB on that
    workload (allocator retention, not growth). Benchmarks print `B/op` for exactly
    this reason: it read 3 200 B/op the hour it was added.
14. **`arena_destroy` gives back what it took.** `wm_stats().arena_live` counts
    chunks that exist, not reservations. Double-counting and keeping the reservation of an
    arena that never allocated is how `/metrics` comes to report 9 GB of live arenas after
    300 000 requests.
15. **`webc_test_request()`'s `Response` lives until the next call** on that
    thread: the helper resets a per-thread arena instead of allocating one per
    call (which leaked 64 KB per request in a loop). Copy what you keep — the
    suites in `examples/` show the pattern.
