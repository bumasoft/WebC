Five ideas explain almost every API in WebC. They are not abstract: each one is the reason a
particular function has the shape it has, and each one is something the framework will not let you
forget.

## 1. A string belongs to a scope

The framework installs an arena per scope — the process, the request, the render — and resets it in
one instruction when the scope ends. An app never allocates one and never resets one; it only decides
*which* scope a value must live in.

```c
// src/greet.c — a whole file, so you can see the scope each string is built in
#include <webc/core.h>

Text greeting(i64 n) {
  str  a = str_lit("hello");        // a (pointer, length) pair in the binary
  Text b = fmt("%d-%s", n, a);      // formatted into the current scope
  Text c = persist(b);              // the one escape hatch: outlive the request
  return c;
}
```

`fmt()` is right for a value you are about to render, validate or send. `persist()` is right for a
cached string, a model in a global, or anything a second request will read. Using `persist()` for
everything is a leak; using `fmt()` for something that outlives the scope is a bug that will show up
as mojibake under load — which is why the framework's own review rule is "if it is stored, name the
scope it belongs to".

> **The rule of thumb.** If you can point at the line that reads the value and say which function it
> returns from, `fmt()` is enough. If it goes into a struct that outlives the function, `persist()`.

## 2. A response is a value

A handler does not write to a socket; it returns a `Response`. That is what makes a handler testable
without a port, composable in middleware, and cheap to call from a test.

```c
// src/server.c — every handler returns a Response, or raises one
#include <webc/server.h>

WEBC_MODEL(Note, (id, i64, .pk = true), (body, Text, .required = true))
Vec(Note)
declare_view(Notes, VecNote);

GET("/notes")  { return json_list(200, db_all(Note)); }

POST("/notes") {
  Note n = body(Note);              // parsed and validated, or a 422 already sent
  db_save(Note, &n);
  return wc_body_is_form() ? redirect("/") : json(201, Note, &n);
}

GET("/nope")   { fail(404, "no such note"); }   // `fail` answers and never returns
```

Errors travel the same way. `try` and `tryv` propagate a `WErr`, `errdefer` cleans up on the way
out, and a panic that reaches the boundary is a 500 with the message in the log — not a dead server.

## 3. A render follows an action

In the browser half there is no `tick` signal and no `redraw()` call. The bridge re-renders each
mounted view when control returns to it: after a click, after a typed field, after a `webc_later`
timer, after a fetch settles. Writing a signal does not schedule anything by itself — the flush
point is the bridge boundary.

```c
// src/client.c — the browser half
#include <webc/web.h>
#include <webc/shared.h>

WEBC_MODEL(Todo, (id, i64, .pk = true), (title, Text), (done, bool))

// the view runs again when this returns
action(toggle, Todo *t) { t->done = !t->done; }
```

The exception is a **live binding**: `text_live`, `attr_live`, `class_live`, `show_live` write one
node in place, and an action marked `action_live` skips the render entirely. That is for a pointer
moving or a keystroke — not for anything structural.

## 4. State has a lifetime, and the lifetime has a name

| Declaration | One value per | Use it for |
| --- | --- | --- |
| `state(T, x, init)` | page | a value the whole page reads |
| `state_in(T, x, init)` | instance you own | per-record state you hold |
| `state_own(T, x, init)` | render position | a row, a mounted view |
| `state_own` + `comp(Name, …)` | call site | one copy per component call |

Choosing wrong is a real bug, not a style question: a `state_own` read from a page-wide timer
panics, and a page-wide state written by one row is a value every row then shows. The reference has
the long version in [§11c](../reference/frontend-signals.html).

## 5. The halves are separated by a file listing

`shared` code compiles into both binaries and can reach neither the database nor the request. That
is the whole trick: the compiler enforces the client/server boundary, so a view cannot accidentally
do I/O, and a migration cannot accidentally depend on a render.

```json
{
  "name": "notes",
  "server": ["src/server.c"],
  "shared": ["src/view.c"],
  "client": ["src/client.c"]
}
```

The same view function is the server's HTML and the browser's tree. There is no second template, no
serialisation schema, and no client-side model to keep in sync — which is most of the code you would
otherwise write.

## What follows from these

- **Nothing is hidden.** Every allocation is a `fmt()`, a `persist()`, or a value on the stack.
- **A handler is a function.** It can be called from a test with a fabricated request.
- **A view is a function.** It can be compiled for two very different targets, or rendered to a
  string in a test.
- **A UI state is a declaration.** `webc check` reads the declarations and the reads, and names the
  ones that disagree — a signal nobody reads, a declaration read outside its position, a part called
  twice that thought it had two states.
