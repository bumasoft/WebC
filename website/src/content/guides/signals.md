A view in WebC is a C function that draws. State is a declaration, and the two questions that decide
whether an app behaves are *what is the state one of?* and *what re-renders?* — this page answers
both, in the order they bite.

The snippets here are client-half files: each starts with `#include <webc/web.h>` and compiles on its
own. A view that is compiled for *both* halves wires its controls with `named()` + `bind()` instead
of `on_click()` — [the quickstart](../docs/quickstart.html) shows that file end to end.

## What re-renders

> **A render follows every action and every host callback.** The bridge re-renders each mounted view
> — and diffs it — when control returns to it: after a click, after a typed field, after a
> `webc_later` timer, after a fetch or an SSE frame settles, after a `push_url` nav. Writing a signal
> does not schedule a render by itself; the flush point is the bridge boundary.

So an action never has to "touch the view":

```c
// src/client.c — an action is a function; the render follows it
#include <webc/web.h>
#include <webc/shared.h>

WEBC_MODEL(Todo, (id, i64, .pk = true), (title, Text), (done, bool))

action(toggle, Todo *t) { t->done = !t->done; }
```

There is no `tick` signal to bump, and `set(tick, get(tick) + 1)` is a write nobody reads. The
exception is a **live binding** — `text_live`, `attr_live`, `class_live`, `show_live` — which writes
one node in place when its signal changes, and `action_live`, which declares that its handler only
writes signals so the bridge sends the ops and skips the render:

```c
// src/client.c — a live binding writes one node; action_live skips the render
#include <webc/web.h>

state(f64, cursor_x, 0);

action_live(mark) { set(cursor_x, ev_point().x); }   // the click's x, no render

view(Ruler) {                    // client-only: the action is in this file
  el("div") {
    on_click(mark);
    el("span") { text_live("%.0f", cursor_x); }   // one DOM op per change
  }
}
```

Everything structural — an `if`, a `for_each`, a `part`, a new node — still needs the render.

## Declaring state

Three declarations, one rule between them: *what is this state one of?*

| Declaration | One value per | Reached with | Fits |
| --- | --- | --- | --- |
| `state(T, n, init)` | page (translation unit) | `get(n)` / `set(n, v)` | the app's own state |
| `state_in(T, n, init)` | a `WInst` you create | `get_in(&inst, n)`, or `get(n)` inside `with_inst` | a view mounted N times by your code |
| `state_own(T, n, init)` | render position | `get(n)` / `set(n, v)` | a row of a list, a mounted view's own copy |

```c
// src/client.c — one value per page, and the effects that follow it
#include <webc/web.h>

state(int, count, 0);
computed(int, doubled) { return get(count) * 2; }
effect(log_it) { printf("%d\n", get(doubled)); }   // once per batch, in topo order

client_main {
  batch { set(count, 1); set(count, 2); }          // one effect run, not two
  return 0;
}
```

- Writing the same value does not re-run effects (byte comparison).
- Effects run in topological order, once per batch.
- `state`, `computed` and `effect` declare file-scope statics, so they do not work inside a function
  body — and they are **one per translation unit**, which is right for app-wide state and wrong for
  a view that appears twice.

That last point is the one that produces real bugs, and it has three answers: a row of a list takes
`state_own` (below), a component called twice takes `comp(Stepper, …)` ([Parts and
components](../guides/components.html)), and a view you mount twice yourself takes `state_in` plus the
`WInst` you pass to `mount()`.

`state_own` outside the position that owns it **panics**, with a message that names the signal and
the two ways out. That is deliberate: a per-row value read from a page-wide timer has no answer, and
guessing one is worse than stopping.

## A list's state

```c
// src/client.c — one state per row, because the row *is* the position
#include <webc/web.h>

WEBC_MODEL(Row, (id, i64, .pk = true), (body, Text, .required = true))
Vec(Row)

state_own(bool, open, false);        // the declaration is in the file, once

action(toggle_open) { set(open, !get(open)); }   // the clicked node's position

view(Rows, VecRow *rows) {
  el("ul") {
    for_each(r, *rows) {             // one position per row, keyed by the row
      el("li") {
        el("button") { on_click(toggle_open); text("▸"); }
        if (get(open)) { el("p") { text(r->body); } }
      }
    }
  }
}
```

`webc check --wiring` reads these declarations and names the mistakes: a signal a write feeds and
nothing reads, a live binding on a page-wide signal **inside** a `for_each` (every row then shows the
same value), and a stateful part called twice that thought it had two states.

## The store: frames in, generations out

A live document arrives as frames — a seed, then a stream — and each frame's strings must die when
the frame two later replaces it. `WStore` owns that rotation:

```c
// src/client.c — the document in hand, and the generation it lives in
#include <webc/web.h>
#include <webc/shared.h>

WEBC_MODEL(Card, (id, i64, .pk = true), (title, Text))
Vec(Card)
WEBC_DOCUMENT(BoardDoc, (cards, VecCard))

static WStore g_room;      // zeroed: two generations, allocated on the first frame
static bool   g_started;

void take_frame(str json) {
  if (!g_started) {              // once: the two generations, allocated here
    store_init(&g_room, BoardDoc_docof(), sizeof(BoardDoc));
    g_started = true;
  }
  BoardDoc *d = store_read(&g_room, json);   // null = bad frame, state unchanged
  if (!d) return;
  printf("%d cards\n", d->cards.n);          // the live document, in the live arena
  (void)store_cur(&g_room);                  // same pointer, spelled for a view
  (void)store_arena(&g_room);                // where a decode allocates
}
```

The server twin is `WFrame`; together they are the ~100 lines every streaming client otherwise
writes first.

## Where the DOM lives

`el("div") { … }` is a builder over a tree the framework owns. It is not a template language: it is
C, so a loop is a `for`, a conditional is an `if`, and a helper that returns markup is a function.

```c
// src/view.c — the builder is C: a loop is a C loop
#include <webc/shared.h>

WEBC_MODEL(Todo, (id, i64, .pk = true), (title, Text), (done, bool))
Vec(Todo)

view(Todos, VecTodo *todos) {
  el("main") {
    el("h1") { text("Todos"); }
    for_each(t, *todos) {
      el("li") { class_(t->done ? "done" : ""); text(t->title); }
    }
  }
}
```

Keyed lists (`for_each`) reuse nodes by key rather than by position, which is what makes a reorder
cheap and keeps focus in the right input. See [Parts and components](../guides/components.html) for
the function-shaped pieces.

## Gotchas worth knowing before they bite

- **`class_()` replaces the attribute.** A second `class_()` on the same element is not additive:
  compose the whole class string in one `class_fmt()` call.
- **`str` is not a C string.** It is a pointer and a length: print it with `%.*s` and its length,
  and copy it with `str_cdup(a, s)` when something downstream needs a NUL.
- **Framework macro names are plain words.** `<webc/*.h>` defines 104 lowercase words as macros —
  `show`, `head`, `html`, `state`, `set`, `text`, `attr`, `page`, `first`, `last`, `name`, `key`. A
  local helper called `show()` will not compile, and the error will point somewhere confusing.

## See also

- Reference [§11 Signals](../reference/frontend-signals.html) — including §11c on `state_own` and §11d on
  component instances.
- [Parts and components](../guides/components.html) — `part`, `comp`, lifecycle, children.