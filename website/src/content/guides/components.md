A view is a page. A `part` is a function that draws a piece of one. A component is a part that gets
its **own state per call site** — and that difference is the whole feature, because a plain C function
call pushes no render position and two calls of one part share one slot.

## The part

```c
// src/view.c — a part is a function that draws, and nothing more
#include <webc/shared.h>

// the prototype, kept in step with the definition
PART(Pocket, (n, i32), (cls, Text));
void Pocket(i32 n, Text cls) {
  el("div") { class_fmt("pocket %s", cls); text("%d", n); }
}

view(Felt) {
  el("main") {
    Pocket(0, "green");       // a plain call: draws where it is written
    Pocket(17, "black");
  }
}
```

`PART(Name, (arg, Type), …)` declares the prototype; the definition follows. That is not ceremony —
the framework's own headers need the prototype to call the part from a header-declared view, and the
macro keeps the two in step.

## The problem a part does not solve

```c
// src/client.c — the state that has no answer: whose `hits` is this?
#include <webc/web.h>

state_own(i64, hits, 0);                     // "one per render position"

action(bump) { set(hits, get(hits) + 1); }

PART(Stepper, (label, Text));
void Stepper(Text label) {
  el("div") {
    text("%s=%lld", label, (long long)get(hits));
    el("button") { on_click(bump); text("+"); }
  }
}

view(Order) {
  el("main") {
    Stepper("a");     // both draw into the *view's* position
    Stepper("b");     // same position again: one `hits` for both
  }
}
```

After one click both steppers show `a=1` / `b=1`: `state_own` is per **render position**, and a
function call does not push one. `webc check --wiring` names this file:

> `Stepper is called 2 times in Order() and declares state_own — every call draws into the same
> position, so they share the state / fix: comp(Stepper, …) at the calls that each need their own, or
> state(…)`

## The component

```c
// src/client.c — the same file with one word changed at each call site
#include <webc/web.h>

state_own(i64, hits, 0);

action(bump) { set(hits, get(hits) + 1); }

PART(Stepper, (label, Text));
void Stepper(Text label) {
  el("div") {
    text("%s=%lld", label, (long long)get(hits));
    el("button") { on_click(bump); text("+"); }
  }
}

view(Order) {
  el("main") {
    comp(Stepper, "a");   // its own hits
    comp(Stepper, "b");   // and this one's
  }
}
```

`comp(Name, args…)` enters the call site as a render position, calls the part, and leaves — on every
exit path, including an early `return` and a panic. What that buys, in one list because every item is
a promise the framework keeps:

- `state_own` and `computed_own` inside the part resolve to **this call**; identity is `(the position
  that drew it, the call site)`, so a re-render finds the same instance and the state survives;
- `effect_own` and `mount_own` start per instance and die with it;
- `on_cleanup` runs when the call goes away — a branch stops drawing it, a row leaves, the mount is
  destroyed;
- `text_live`, `attr_live`, `class_live`, `show_live`, `bind_str` and every action already resolve
  through the node's owner, so they inherit the instance with no extra wiring;
- a row that moves in a keyed list keeps its components.

Calling a part **without** `comp()` keeps the older, coarser rule — it draws in the position that
called it. That is what makes the feature opt-in: existing code is unchanged, and the check tells you
which call sites want the other thing.

## Lifecycle

| Declaration | What it does |
| --- | --- |
| `state_own` / `computed_own` | a slot per render position, created on first read, disposed with the position |
| `effect_own(name) { … }` | declares an effect that belongs to a render position; the body is at file scope, like every declaration |
| `mount_own(name)` | the one line in a view that starts that effect for the instance being drawn |
| `on_mount(fn, ctx)` | once the instance's nodes are in the document |
| `on_cleanup(fn, ctx)` | inside an effect: before each re-run, and when the position dies. Outside: when the position dies |

```c
// src/client.c — an effect that belongs to a render position
#include <webc/web.h>
#include <webc/host.h>

state_own(Text, body, "");

void flush_draft(void *ctx) { (void)ctx; /* save it */ }

// runs on the first draw, then when `body` changes
effect_own(save) {
  (void)get(body);
  webc_later(800, flush_draft);        // one timer per verb: re-armed, not stacked
}

PART(Draft, (label, Text));
void Draft(Text label) {
  mount_own(save);                     // this instance wants that effect
  el("div") {
    el("input") { bind_str(body); }    // the keystroke writes the instance's `body`
    text("%s", label);
  }
}
```

Why the extra line: a C function body is not something a view can splice in — there are no closures —
so `effect_own`'s body lives at file scope and the one thing the framework cannot infer is *which*
effects a given view wants. It is the same pair `effect_in` + `mount_in` already has, except the
framework owns the lifetime instead of the app.

`on_mount` is the other piece a view has no room for: measure a box, focus an input, scroll to a row —
once the nodes exist, without a `webc_later(0, …)` and a hope. `on_cleanup` has **two scopes** and
picks by where it is written: inside an effect it belongs to that run, so a re-run cleans up the
previous one; outside an effect it belongs to the position.

## Children

A function cannot take a block, so a component that wraps other markup takes a callback and a
context — the same `(fn, ctx)` pair `for_each` and `prose_with` already use. The builder is ambient,
so the children land exactly where the part calls `kids()`:

```c
// src/view.c — a part that wraps markup it did not write
#include <webc/shared.h>

PART(Card, (title, Text), (inner, WKids));
void Card(Text title, WKids inner) {
  el("section") { class_("card");
    el("h3") { text(title); }
    kids(inner);                                  // the caller's markup lands here
  }
}

static void body(void *ctx) { el("p") { text("%s", (const char *)ctx); } }

static Text msg = "the children belong to the caller";

view(Page) {
  el("main") {
    comp(Card, "Hello", kids_of(body, msg));      // no_kids() when there are none
    comp(Card, "Empty", no_kids);
  }
}
```

`kids_of` remembers the position that wrote the call, and `kids()` re-enters it while the children
run: markup the caller wrote reads the **caller's** state. That is React's rule too — children belong
to the parent — and it is the one that surprises people when it is the other way round.

## What a component costs

56 bytes per instance **drawn**: a 24-byte `WInst` and a 32-byte table slot. A list of 49 cells that
each hold a chip is 2.7 KB. A pure-markup part costs nothing, because you call it plainly.

## When not to use one

A component is the right answer when the state belongs to *the thing being drawn*. It is the wrong
answer when the state is cross-cutting — written by a keyboard shortcut, a timer, or a document-level
listener, none of which has a render position:

```c
// src/client.c — both of these run outside any render position
#include <webc/web.h>
#include <webc/host.h>
#include <webc/keys.h>

state(bool, focus, false);             // the answer: one value per page
action(clear) { set(focus, false); }
void settle(void *ctx) { (void)ctx; set(focus, true); }

static const WCmd g_keys[] = {K("Clear the felt", "c", clear)};

client_main {
  webc_later(400, settle);             // a timer: no position either
  keys_bind(g_keys);                   // the same table draws the help sheet
  return 0;
}
```

In that case the state belongs to the page (`state`). The framework panics rather than guessing:

> `focus is render-owned state read outside the position that owns it: it is drawn by a mounted view
> or a for_each row, and read from that view or an action on it. Declare it state(...) for one value
> per page, or state_in(...) if the app owns the lifetime.`

There is a second, subtler reason to keep state at the page level: a component's state **cannot be
enumerated**. If a spin has to post `{req, bets:[…]}` built by walking every row's draft, per-row
component state is write-only — nothing can walk it. The list stays one page-level collection.

## Migrating a codebase

The change is mechanical, and the tool does the finding:

1. `webc check --wiring` lists every stateful part called more than once in one view.
2. For each call site that needs its own state, wrap the call: `Stepper("a")` → `comp(Stepper, "a")`.
3. Leave the rest alone. A part with no `state_own` cannot be affected.

## See also

- Reference [§11d](../reference/frontend-signals.html) for the identity rule and the cost table.
- [Signals and the DOM](../guides/signals.html) for `state_own` on its own.
- `tests/comp_test.c` — eleven tests, one per promise on this page.
