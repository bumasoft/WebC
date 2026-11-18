A model is the only place a piece of data is declared. From one declaration the framework generates
the struct, a descriptor table, the SQL text for the table, the JSON codec, the form parser, the
validation rules and the labels a form renders. Those six cannot drift apart, because there is only
one of them.

## Declaring a model

```c
// src/app.h — the model, in the header both halves include
#include <webc/shared.h>

WEBC_MODEL(Todo,
           (id, i64, .pk = true),
           (title, Text, .required = true, .max = 200),
           (tags, VecStr, .json_array = true),
           (owner, i64),                       // a foreign key: the account's id
           (secret, Text, .sensitive = true, .no_json = true))

Vec(Todo)          // VecTodo: a collection that carries Todo's descriptor
```

That gives you `Todo_modelof()` (the descriptor), the table name `todos` (snake_case plural — override
with `WEBC_MODEL_TABLE(Todo, "todos2")`), and every one of:

```c
// src/notes.c — the codec, one call in each direction
#include <webc/server.h>

WEBC_MODEL(Todo, (id, i64, .pk = true), (title, Text, .required = true, .max = 200))
Vec(Todo)

bool import_one(Arena *a, str body) {
  Todo t;
  WErrs errs = {0};
  // not JSON, or wrong shape
  if (!model_from_json(a, Todo_modelof(), body, &t).ok) return false;
  // `errs` names each field
  if (!model_validate(Todo_modelof(), &t, &errs)) return false;
  // insert, or update if id is set
  db_save(Todo, &t);
  // the same wire shape out
  str back = model_to_json(a, Todo_modelof(), &t);
  (void)back;
  return true;
}
```

`Vec(Todo)` declares `VecTodo` *carrying its element type*, so `json(200, todos)` needs no type name
and `for_each(t, todos)` knows its key.

## The qualifiers

All designated initialisers, so the compiler checks them — and a bare flag (`.required` with no
`= true`) is not valid C, so it is a compile error rather than a silently ignored option.

| Qualifier | Meaning |
| --- | --- |
| `.pk = true` | primary key; an integer pk is assigned by the database |
| `.required = true` | must be present and non-empty |
| `.not_null = true` | the same, spelled for rows rather than bodies |
| `.unique = true` | declared in generated SQL; enforced by the database |
| `.max = N` / `.min = N` | string **length** bounds |
| `.gte = X` / `.lte = X` | numeric bounds |
| `.format = "email"` | `email`, `uuid`, `url`, `date` |
| `.trim = true` | strip surrounding whitespace before validation |
| `.sensitive = true` | redacted by the logger |
| `.no_json = true` | never serialised |
| `.json_array = true` | a vector field serialises as an array of scalars |
| `.sql = "column"` | column name override |
| `.def = "..."` | SQL default expression |
| `.doc = "..."` | surfaced by `webc doctor` |
| `.null = true` | an **integer** column the row may not have; `0` is the NULL this side reads |
| `.references = "table(col)"` | the FK target the generated DDL carries |

`.max`/`.min` are lengths and `.gte`/`.lte` are numbers; mixing them up is a compile error, not a
runtime surprise.

### A column that can be absent

A foreign key nobody has chosen yet is the ordinary case, and the failure it produces is not: a
plain `i64` saved as `0` reaches the database as a value, the foreign key refuses it, and the
message is `FOREIGN KEY constraint failed` on a `POST`. Declared nullable, the same save is a
`NULL`:

```c
// src/app.h — an optional cover image
#include <webc/shared.h>

WEBC_MODEL(MediaAsset, (id, i64, .pk = true), (path, Text, .required = true))
WEBC_MODEL(Post, (id, i64, .pk = true),
           (title, Text, .required = true),
           (cover_id, i64, .null = true, .references = "media_assets(id)"))
Vec(Post)
```

```c
// src/posts.c — the server half: "posts with no cover"
#include <webc/server.h>

WEBC_MODEL(Post, (id, i64, .pk = true), (title, Text),
           (cover_id, i64, .null = true))
Vec(Post)

static void naked(void) {
  WWhere w;
  ww_init(&w, nullptr);
  ww_is_null(&w, Post, cover_id);        // ww_not_null for the other question
  VecPost *rows = db_all_w(db_default(), Post, &w);
  (void)rows;
}
```

One qualifier carries through all four directions: the DDL has no `not null` and carries the
`references` clause, `db_save` binds SQL `NULL` for `0`, a read turns `NULL` back into `0`, the JSON
codec writes `json: null` and accepts `null` for this field only, and an empty form input means
"nothing chosen". `.null` is for integer kinds alone — a float's `0` is a value and an enum's `0` is
its first member — and `.null` with `.required` or `.pk` is a contradiction the compiler refuses.

## A closed set is a type, not a string

```c
// src/app.h — a closed set is a type, not a string you validate by hand
#include <webc/shared.h>

WEBC_ENUM(Lane, backlog, todo, doing, done)

WEBC_MODEL(Card, (id, i64, .pk = true), (lane, Lane, .required = true))

// and in src/handlers.c:
static void show_lane(Arena *a) {
  // the C name, checked by the compiler
  Card c = {.lane = doing};
  // 2: the helpers carry the type's name
  i32  i = Lane_index(str_lit("doing"));
  str  n = str_lit(Lane_name(i));                 // "doing"
  printf("%s (%d) %s\n", c.lane == doing ? "doing" : "?", i, n.p);
  (void)a;
}
```

The field is an int in the struct and its *name* everywhere bytes are exchanged — JSON, forms, SQL
text. A request that sends a stranger is a 422 naming the field, rather than a row that renders
nowhere. A legacy integer column still reads, as its index.

## Migrations

Migrations are SQL files in `migrations/`, applied in filename order and recorded in
`schema_migrations`. The dialect belongs to the database, so postgres gets its own spelling beside
the sqlite one:

```text
migrations/0001_init.sql            -- integer primary key autoincrement
migrations/0001_init.postgres.sql   -- bigserial primary key
```

The tool picks the file for the driver and records the same name, so one sequence serves both.

```sh
webc migrate up
webc migrate status
webc migrate new add_tags      # writes a numbered, empty pair
webc migrate diff              # what the models say the tables are missing, as SQL
webc migrate diff --write      # … written as migrations/0004_<name>.sql, not applied
```

The server applies pending migrations at boot. `webc check` compares the models against the migrated
schema and names a column that has drifted, which is the failure that otherwise shows up as a 500 in
production. `webc migrate diff` is that comparison as statements: an `alter table … add column`, a
`create table`, the `references` a `.null` field declares, the default a `not null` needs. It is
**additive on purpose** — it never drops or narrows, because sqlite cannot drop a constraint without
copying the table and a tool that guesses at a data migration loses rows — and what it cannot infer
is a `-- TODO` line a person answers before the file is applied.

### The routes every table gets

`webc scaffold Post` writes `src/admin_post.c`: the list (a `WPager` window and a `ww_like` search
over the first bounded text field), new, edit, save and delete, plus `tests/admin_post_test.c` with
one test per route. It reads the model from the app's own descriptor, so the columns and bounds are
the ones you declared, and it never edits the model: the file is yours to change, `webc check` passes
on it, and nothing that exists is overwritten without `--force`.

## Queries

The typed filters are the way to read: `db_by` for one value, `WWhere` for conditions, relations
declared once and read with `ref()`, and `db_page` for a list — [The ORM](../guides/orm.html) is the whole
guide. What follows is the raw layer those sit on, which is what you want for the report-shaped
statement that is not worth a verb.

```c
// src/stories.c — the five reads and writes an app actually makes
#include <webc/server.h>

WEBC_MODEL(Story,
           (id, i64, .pk = true),
           (title, Text, .required = true),
           (views, i64))
Vec(Story)

void reads_and_writes(i64 id) {
  // DATABASE_URL, opened once
  WDB *db = db_default();

  // the configured handle, no argument
  VecStory *all   = db_all(Story);
  // arena-allocated, null if gone
  Story     *one  = db_get(Story, id);
  VecStory *newer = db_all_ordered(db, Story, "published desc");
  WQuery *qy = q("");
  q_i64(qy, "n", 10);                                      // one bound value
  VecStory *top = db_all_where(db, Story,
                               "where views > :n order by views desc", qy);

  Story s = {.title = fmt("story %lld", (long long)id), .views = 0};
  // false when the row conflicts
  if (!db_insert(db, Story, &s)) return;
  s.views++;
  // insert, or update: the pk is set
  db_save(Story, &s);
  (void)all; (void)one; (void)newer; (void)top;
}
```

`webc_config`'s default connection is the one-argument forms above use; every function also takes an
explicit `WDB *`.

## Transactions

A transaction belongs to a session, and a session belongs to a request: the first statement a request
makes pins a connection for that task, and every later statement runs on it. Two requests can never
sit inside each other's `BEGIN`/`COMMIT`.

```c
// src/orders.c — a transaction belongs to the request; it rolls back on the way out
#include <webc/server.h>

WEBC_MODEL(Order, (id, i64, .pk = true), (total, i64))

static bool charge(i64 order_id) { return order_id != 0; }   // your payment call

void place(Order o) {
  WDB *db = db_default();
  // BEGIN on this request's own connection
  tx_scope(db) {
    db_save(Order, &o);
    if (!charge(o.id)) fail(402, "declined");     // unwinds: the scope rolls back
    tx_commit_here();                             // no commit → ROLLBACK
  }
}
```

A nested scope is a `SAVEPOINT`, so an inner failure can roll back alone. A task that ends inside a
transaction has it rolled back and the connection returned, with a warning naming the task.

## Documents

A model is a row. A **document** is the shape a live page sends: several collections and scalars in
one JSON object, one declaration for both directions.

```c
// src/app.h — a document is several collections in one wire shape
#include <webc/shared.h>

WEBC_MODEL(Card, (id, i64, .pk = true), (title, Text, .required = true))
Vec(Card)

WEBC_DOCUMENT(BoardDoc,
              (title,  Text),
              (online, VecI64),
              (cards,  VecCard))

// and in src/board.c — both directions, checked by the compiler:
static void roundtrip(Arena *a) {
  BoardDoc d = {.title = "Board",
                .online = vec_i64_with(a),
                .cards = VecCard_with(a)};
  push(d.cards, ((Card){.id = 1, .title = "first"}));
  // the whole document, as one JSON string
  str  j  = BoardDoc_write(a, &d);
  BoardDoc out;
  bool ok = BoardDoc_read(a, j, &out);      // a new field is one edit, not five
  (void)ok;
}
```

On the client a document arrives through a `WStore` (see [Real-time](../guides/live.html)), and
`wc_doc_join()` links its relations in memory.

## A row you computed

A dashboard tile is a row the database does not have — a count, a breakdown, a KPI. `WEBC_ROW`
declares it exactly like a model and gives the same four things (struct, descriptor, `Vec(T)`,
JSON), minus the table:

```c
// src/dashboard.c — a row with no table behind it
#include <webc/server.h>

WEBC_ROW(DeviceCount, (device, Text, .required = true), (visits, i64))
Vec(DeviceCount)

void report(void) {
  VecDeviceCount *rows = vec_new(DeviceCount);
  push(*rows, ((DeviceCount){.device = "mobile", .visits = 412}));
  push(*rows, ((DeviceCount){.device = "desktop", .visits = 88}));
  json(200, rows);                       // [{device, visits}], no encoder by hand
}
```

Declared as a `WEBC_MODEL` instead, `webc check` reports schema drift on every run for a table that
was never meant to exist; `db_save` on a row panics with a sentence saying so rather than sending an
insert at a table nobody created.

## See also

- Reference [§4 Models](../reference/models.html) and [§9 Database](../reference/database.html).
- [The ORM](../guides/orm.html) — filters, relations, pages and trees.
- [MySQL and MariaDB](../guides/mysql.html) — the same migrations on the third server.
- [Forms](../guides/forms.html) — the same declaration drawn as inputs.
