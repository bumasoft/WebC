A model is a row; the ORM is how you ask for rows without writing SQL. There is no unit of work, no
lazy proxy and no second query language: a filter names the model's **fields**, so the compiler
checks it, a relation is declared once next to the fields it points at, and a page or a whole tree of
rows is one call each.

That is the whole design. Everything below is the same idea at four sizes: one value, a
condition, a relation, a page.

## One field, one value

```c
// src/todos.c — the server half: one row, a list, a count, all by one field
#include <webc/server.h>

WEBC_MODEL(Todo, (id, i64, .pk = true), (title, str, .max = 200), (done, bool))
Vec(Todo)

static void lookups(str title) {
  // the first row, or null — a typo in "tittle" panics and lists the real fields
  Todo *one = db_by(Todo, "title", title);
  // a collection
  VecTodo *open = db_all_by(Todo, "done", 0);
  // how many, without reading them
  i64 n = db_count_by(Todo, "done", 0);
  (void)one;
  (void)open;
  (void)n;
}
```

The field name is checked against the model's descriptor, and the value is always a bound parameter:
the clause is built from the field, never from a string you typed. `db_by(db, Todo, "id", 3)` is the
same call on a handle you own (a test, a second database).

A string literal goes through `str_lit()`, because `_Generic` does not decay an array — the reference
[§9a](../reference/database.html) has the long version of why.

## A filter with more than one condition

`WWhere` builds the clause *and* its bound values from the descriptor. Nothing is pasted, so a value
containing a quote is a value containing a quote.

```c
// src/todos.c — the server half: three conditions, an order, a limit, one statement
#include <webc/server.h>

WEBC_MODEL(Todo, (id, i64, .pk = true), (title, str), (done, bool),
           (owner, i64), (created, i64))
Vec(Todo)

static void recent(i64 me) {
  WWhere w;
  ww_init(&w, nullptr);              // nullptr: the current arena owns the clause
  ww_eq_i64(&w, Todo, owner, me);    // one bound value
  ww_eq_bool(&w, Todo, done, false);
  ww_order(&w, Todo, created, true); // desc
  ww_limit(&w, 20, 0);               // the query cuts; the caller stops truncating

  VecTodo *rows = db_all_w(db_default(), Todo, &w);   // ordered and limited
  Todo *first = db_one(db_default(), Todo, &w);       // one row; limit 1 is implied
  i64 total = db_count_w(db_default(), Todo, &w);     // how many match
  ww_free(&w);
  (void)rows;
  (void)first;
  (void)total;
}
```

| call | clause it writes |
| --- | --- |
| `ww_eq_i64` / `ww_ne_i64` | `= ?` / `<> ?` |
| `ww_gt_i64` / `ww_gte_i64` / `ww_lt_i64` / `ww_lte_i64` | `> ?` / `>= ?` / `< ?` / `<= ?` |
| `ww_eq_bool(&w, Todo, done, false)` | a bool, spelled the driver's way |
| `ww_eq_str(&w, Todo, title, s)` | string equality |
| `ww_like(&w, Todo, title, pattern)` | `like` — the pattern is yours, wildcards included |
| `ww_contains(&w, Todo, title, q)` | `lower(col) like lower(?) escape '!'`, escaped for you |
| `ww_order(&w, Todo, created, true)` | `order by … desc` (`true`) or asc (`false`) |
| `ww_limit(&w, 20, 40)` | `limit 20 offset 40` |

Two of those are worth a sentence more:

- **`ww_limit` belongs to the query, not to your loop.** Reading forty rows to keep two is the shape
  this removes, and an `offset` with no `limit` (illegal in sqlite and MySQL) is repaired from the
  driver's declared capability instead of in every app.
- **`ww_contains` owns the escaping.** `%`, `_` and `!` in the user's words are escaped and the
  clause says `escape '!'` — not a backslash, which is an escape character *inside a string literal*
  on MySQL and makes `escape '\'` a syntax error there.

The field name is a compile-time check: `ww_eq_i64(&w, Todo, onwer, 1)` does not compile. A field that
is not a column (a `.sql()` override aside) is the same error.

## One question over several columns

A search box is one question about three columns, and answering it by filtering a whole table in
memory is a page that works on your laptop:

```c
// src/todos.c — the server half: one question, two columns, one clause
#include <webc/server.h>

WEBC_MODEL(Todo, (id, i64, .pk = true), (title, str), (body, str))
Vec(Todo)

static VecTodo *search(str q) {
  WWhere w;
  ww_init(&w, nullptr);
  ww_or_begin(&w);
  ww_contains(&w, Todo, title, q);
  ww_contains(&w, Todo, body, q);
  ww_or_end(&w);
  VecTodo *rows = db_all_w(db_default(), Todo, &w);
  ww_free(&w);
  return rows;
}
```

## Aggregates

The same filter answers a number instead of rows — `db_sum`, `db_min`, `db_max`, `db_avg`:

```c
// src/stories.c — the server half: aggregates over the conditions, not over a page
#include <webc/server.h>

WEBC_MODEL(Story, (id, i64, .pk = true), (section, str), (views, i64))
Vec(Story)

static void totals(str section) {
  WWhere w;
  ww_init(&w, nullptr);
  ww_eq_str(&w, Story, section, section);
  ww_order(&w, Story, views, true);   // ignored by the aggregates below
  ww_limit(&w, 10, 0);                // and so is this
  i64 views = db_sum(db_default(), Story, views, &w);
  i64 top = db_max(db_default(), Story, views, &w);
  f64 mean = db_avg(db_default(), Story, views, &w);   // f64 even for an i64 column
  ww_free(&w);
  (void)views;
  (void)top;
  (void)mean;
}
```

`order by` and `limit` are **stripped** before the aggregate: a sum of the first page is a different
question, and it is the one whose wrong answer looks plausible. No rows, or every row `NULL`, is `0` —
the sum of nothing is nothing, not an error.

**One column, plus one, in one statement.** `db_increment(Todo, t, views)` is `update … set views =
views + 1`, which is not the same thing as `t->views += 1; db_save(Todo, t);` — that rewrites the whole
row (a `str` body and all) to change a counter, and loses a concurrent increment.

## Relations: declared once, read with no query

A relation is a **declaration**, not a lazy proxy. Two lines go next to the models, in the header both
halves include:

```c
// src/app.h — the shared header: the only place a foreign-key column is named
#include <webc/shared.h>

WEBC_MODEL(Section, (id, i64, .pk = true), (slug, str, .unique = true), (name, str))
Vec(Section)
WEBC_MODEL(Story, (id, i64, .pk = true), (title, str), (section, str))
Vec(Story)

// Story.section holds a Section.slug, and a section has many stories
WEBC_MODEL_REF(Story, section, Section, slug)
WEBC_MODEL_MANY(Section, stories, Story, section)
```

`WEBC_MODEL_REF` is pure data: it registers "this field points at that key" and nothing else. It costs
one constructor call at startup and no memory per row, and it is what `prefetch` reads — which is why
the column name appears exactly once in the app.

Then the handler runs **one query for the whole page** and the view reads the links:

```c
// src/pages.c — the server half: one prefetch for the list, no query in the view
#include <webc/server.h>

WEBC_MODEL(Section, (id, i64, .pk = true), (slug, str), (name, str))
Vec(Section)
WEBC_MODEL(Story, (id, i64, .pk = true), (title, str), (section, str))
Vec(Story)
WEBC_MODEL_REF(Story, section, Section, slug)
WEBC_MODEL_MANY(Section, stories, Story, section)

static void load(void) {
  VecSection *all = db_all_ordered(db_default(), Section, "name");
  prefetch(all, Section, stories);     // ONE query, however many sections
  VecStory *recent = db_all_ordered(db_default(), Story, "id desc");
  prefetch(recent, Story, section);    // and one for the other direction
  (void)all;
  (void)recent;
}
```

```c
// src/view.c — both halves: no query at all, the links were built for this request
#include <webc/shared.h>

WEBC_MODEL(Section, (id, i64, .pk = true), (slug, str), (name, str))
Vec(Section)
WEBC_MODEL(Story, (id, i64, .pk = true), (title, str), (section, str))
Vec(Story)
WEBC_MODEL_REF(Story, section, Section, slug)
WEBC_MODEL_MANY(Section, stories, Story, section)

static void draw(const Section *sec, const Story *st) {
  const Section *owner = ref(Story, st, section);   // the row, or null
  VecStory *kids = refs(Section, sec, stories);     // VecStory *, or empty
  (void)owner;
  (void)kids;
}
```

- **One statement per collection, not per row.** 2 000 sections are 2 000 owner keys, chunked into
  groups of 512, so the parameter count never meets a server's ceiling.
- **`ref()` never fetches.** A relation nobody prefetched reads as `null`; a collection nobody
  prefetched reads as *empty* and logs `relation read without a prefetch` once, naming the model and
  the field — because an empty list that was never fetched is a page that lost its rows in silence.
- **Hops chain left to right.** `prefetch(all, Section, stories, author)` is two statements: the
  stories of every section on the page, then the authors those stories named. Three hops is the limit;
  deeper is a loop in the app, because a path string is a second query language nobody asked for.
- **The links belong to the request.** The server clears the link table at every request boundary, and
  per test — so a view can never read last request's rows, including the case the pool makes easy to
  get wrong, where the request arena comes back at the same address.
- **`prefetch_on(db, all, Section, stories)`** is the same on a handle you own. In the browser half
  there is no database, so a document's rows are joined with `wc_doc_join()` after they decode.

## Questions *across* a relation

A filter can reach through a relation without changing the shape of the result — which matters,
because the ORM reads rows back into models and a join changes the row shape:

```c
// src/stories.c — the section's name in the conditions, the story's shape out
#include <webc/server.h>

WEBC_MODEL(Section, (id, i64, .pk = true), (slug, str), (name, str))
Vec(Section)
WEBC_MODEL(Story, (id, i64, .pk = true), (title, str), (section, str))
Vec(Story)
WEBC_MODEL_REF(Story, section, Section, slug)

static VecStory *in_open_sections(str q) {
  WWhere sec;
  ww_init(&sec, nullptr);
  ww_contains(&sec, Section, name, q);

  WWhere w;
  ww_init(&w, nullptr);
  // section in (select slug from sections where lower(name) like lower(?))
  ww_in_ref(&w, Story, section, &sec);
  // order by the section's name, through the relation
  ww_order_ref(&w, Story, section, Section, name, false);
  VecStory *rows = db_all_w(db_default(), Story, &w);
  ww_free(&w);     // the outer filter first: the inner clause is part of its SQL
  ww_free(&sec);
  return rows;
}
```

`ww_not_in_ref` is the same clause negated. `ww_order_ref` is a correlated subquery, so it costs one
index lookup per row on the referenced key — fine for a page of rows, and the honest alternative for a
whole table is an indexed column on the row itself.

## A page of a list

```c
// src/stories.c — two statements, a clamped page number, and its fields
#include <webc/server.h>

WEBC_MODEL(Story, (id, i64, .pk = true), (section, str), (views, i64))
Vec(Story)

static void second_page(str section) {
  WWhere w;
  ww_init(&w, nullptr);
  ww_eq_str(&w, Story, section, section);
  ww_order(&w, Story, views, true);
  WPager pg = {.page = 2, .limit = 20};       // limit 0 means 20
  VecStory *rows = db_page(db_default(), Story, &w, &pg);
  ww_free(&w);
  // pg.total, pg.pages, pg.has_prev, pg.has_next; past the end clamps
  (void)rows;
}
```

Two statements on purpose: the count is what tells the pager how many pages there are, and a window
cannot answer it. `page` is clamped into `1..pages`, so an out-of-range page number is the same list
rather than an empty render or a 404. Between the two statements a concurrent insert can shift every
row by one; the framework does not open a transaction to hide it, because doubling the cost of every
list render to hide a race nobody has hit is the wrong trade.

## A tree of rows

Rows that belong to a row are written as one thing, and the foreign key comes from the `MANY`
declaration rather than from your fingers:

```c
// src/orders.c — the server half: a parent and its lines in one transaction
#include <webc/server.h>

WEBC_MODEL(Order, (id, i64, .pk = true), (total, i64))
Vec(Order)
WEBC_MODEL(OrderLine, (id, i64, .pk = true), (order, i64), (sku, str), (qty, i64))
Vec(OrderLine)

// OrderLine.order holds Order.id
WEBC_MODEL_MANY(Order, lines, OrderLine, order)

static void place(Order *o, VecOrderLine *lines) {
  i64 written = 0;
  // each line's `order` is filled from o->id; joins an open tx, or opens one
  if (db_save_all(db_default(), Order, o, lines, &written) != (i64)lines->n) return;
  (void)written;
}
```

`db_save_all` takes the **collection**, not a field name: matching the vector's own descriptor against
the declaration is what makes a typo impossible, and a collection that is not declared on the model is
a panic naming the declaration to add — not a write into the wrong column. `db_delete_tree` is the
same in the other direction, children first, because an app's migration may not have
`on delete cascade`.

## What it does not do

- **No `group by`.** "Per kind, how many and how much" is a filtered read plus a sum in C, or an
  aggregate per group in the app. Adding the verb is the fix; a hand-written join in one handler is
  not.
- **No lazy loading.** Nothing fetches on read. A view that needs rows gets them from a handler that
  prefetched them, which is the property that keeps a page's query count fixed.
- **No unit of work.** A save is a statement; a tree of saves is `db_save_all` inside `tx_scope`
  ([Models and migrations](../guides/models.html)).
- **No dialect in your code.** Identifiers, `offset`, `returning` and multi-statement files are
  declared per driver ([MySQL and MariaDB](../guides/mysql.html)), so the same filter runs on sqlite,
  postgres and MySQL.

## Count the queries

The ORM's promise is a *constant* number of statements per page, and a stopwatch cannot check that.
`db_statement_count()` can, and `WEBC_TRACE_DB=1` prints the same number as one line per statement:

```c
// tests/app_test.c — the assertion that keeps a page's query count fixed
#include <webc/test.h>

TEST(a_page_asks_a_fixed_number_of_questions) {
  u64 before = db_statement_count();
  // the request goes here
  ASSERT_EQ(db_statement_count() - before, 3);
}
```

That test is where an N+1 gets caught: `prefetch` is one statement for the list and one per hop, so a
page that grows a query per row fails a test instead of becoming a slow page nobody can reproduce. A
typical seven-page app holds `/table` at three statements and `/books` at five, measured at three
rows and again at fifteen.

## See also

- Reference [§9 Database](../reference/database.html) — §9a to §9a4 are this guide in reference voice.
- [Models and migrations](../guides/models.html) — the declaration this rests on.
- [MySQL and MariaDB](../guides/mysql.html) — the third driver, and the four capabilities it declares.
- Reference [§9 Database](../reference/database.html) — relations as declarations, the driver
  capability table, and how a driver classifies its own errors.