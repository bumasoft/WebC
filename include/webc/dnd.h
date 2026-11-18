// SPDX-License-Identifier: Apache-2.0
// webc/dnd.h — a row you can pick up and put somewhere else.
//
// The pointer already reaches a C action (`ev_point()`), and the bridge already sends the drag
// family (`dragstart`/`dragover`/`drop`) instead of letting the browser refuse the drop. What
// nobody enjoys writing is the part in between: from a pointer's `y` inside a row to a drop
// *index*, with the two cases everyone gets wrong — dropped on itself (a no-op, not a move) and
// dropped past the end (the last index, not a crash).
//
//   state(i64, drag_from, -1), state(i64, drop_at, -1);
//   static WDrag zdrag;                        // one per list; the rows and the drop share it
//
//   el("ul") { class_("rows");
//     dnd_list(&zdrag, WC_ARRLEN(notes));       // role="list", the shared data attributes
//     for_each(n, notes) {
//       el("li") {
//         dnd_row(&zdrag, &n->id, i++, get(drag_from), get(drop_at));
//         named("row");
//         bind("row", "dragstart", row_pick);   // set(drag_from, ev_attr_i64("data-dnd-i"))
//         bind("row", "dragover",  row_over);   // set(drop_at, dnd_over(&zdrag))
//         bind("row", "drop",      row_drop);   // reorder(get(drag_from), dnd_drop(&zdrag))
//         text(n->title);
//       }
//     }
//   }
//
// Three verbs do the maths:
//   `dnd_over(&zdrag)`  — the index the pointer is over, before/after this row by its half;
//                         -1 for "no drop here" (dragging a row over itself);
//   `dnd_drop(&zdrag)`  — the index to move to, already adjusted for the item that left the list
//                         (moving 1 to 3 in a 4-list is an insert at 2 once 1 is removed — the
//                         off-by-one that makes a hand-written kanban shuffle the wrong card);
//   `dnd_can(&zdrag)`   — false when the drag is a no-op, so a drop can be ignored cheaply.
//
// The module also supplies what a person expects and a hand-rolled version forgets: the
// `dnd-dragging` class on the row being carried, an `aria-grabbed` on it, a `dnd-list` role with
// a live-region-free announcement ("moved to position 3" is the app's copy, not the module's),
// and `dnd_keys()` so `alt+↑`/`alt+↓` reorder from the keyboard — through `keys.h`, the same
// table that handles every other key.
//
// **Refuses:** cross-list drag (a kanban needs one `WDrag` per column and the app says what
// crossing means — two structs it composes), file drop, touch gestures beyond pointer events,
// autoscroll while dragging, and animation (that is `motion.h`).
#pragma once

#include <webc/core.h>
#include <webc/dom.h>
#include <webc/keys.h>

typedef struct WDrag {
  i32 from;     // the index being carried, -1 for none
  i32 over;     // the index the pointer is over, -1 for none
  bool after;   // the pointer is in the row's second half: insert *below* it
  i32 n;        // items in the list, so "past the end" is answerable
  const char *attr_prefix; // data attribute stem; "dnd" unless the app has two lists on a page
} WDrag;

static inline WDrag wc_drag_new(i32 n, const char *attr_prefix) {
  WDrag d = {0};
  d.from = -1;
  d.over = -1;
  d.n = n;
  d.attr_prefix = attr_prefix && *attr_prefix ? attr_prefix : "dnd";
  return d;
}
#define dnd_new(n_) wc_drag_new((n_), "dnd")
#define dnd_new_as(n_, prefix_) wc_drag_new((n_), (prefix_))

// The list: a role, the count, and the prefix its rows read. Called inside the `ul`.
static inline void wc_dnd_list(WDrag *d, i32 n) {
  d->n = n;
  wc_attr_c("role", "list");
  wc_attr("data-dnd-n", str_from_i64(wc_arena_get(), n));
}

#define dnd_list(d_, n_) wc_dnd_list((d_), (n_))

// One row. `key` is the row's identity — the value that survives a reorder — and `i` is where it
// sits now. The three classes the stylesheet uses are here and nowhere else, so a row that is
// carried looks carried without the app writing a class name it must keep in sync.
static inline void wc_dnd_row(WDrag *d, const void *key, i32 i, i32 from, i32 over) {
  (void)key;
  bool carried = from >= 0 && from == i;
  bool target = over >= 0 && over == i && from != i;
  wc_attr_c("draggable", "true");
  wc_attr("data-dnd-i", str_from_i64(wc_arena_get(), i));
  if (carried) {
    wc_attr_c("aria-grabbed", "true");
    class_("dnd-dragging");
  } else {
    wc_attr_c("aria-grabbed", "false");
  }
  if (carried) class_("dnd-from");
  if (target) {
    class_("dnd-over");
    if (d->after) class_("dnd-after");
  }
}
#define dnd_row(d_, key_, i_, from_, over_) wc_dnd_row((d_), (key_), (i_), (from_), (over_))

// The index the pointer is over, from the row it is over and where on that row it is. The half of
// the row the pointer is in decides "insert before" or "insert after" — the whole interaction,
// in four numbers the bridge already sends.
static inline i32 wc_dnd_over(WDrag *d) {
  WPoint p = ev_point();
  i32 i = (i32)ev_attr_i64("data-dnd-i"); // the row the event bubbled from
  if (p.w <= 0 && p.h <= 0) {             // a keyboard or test event: no coordinates
    d->over = i;
    d->after = false;
    return i;
  }
  d->after = p.y > p.h / 2;
  d->over = i;
  if (d->from == i) return -1; // over itself: no drop, not "insert here"
  return d->after ? i + 1 : i;
}
#define dnd_over(d_) wc_dnd_over((d_))

// Where a drop would land, as the index to move *to* after the source is removed. This is the
// number an insert needs: `move(1, 3)` in a four-item list means "take item 1 out and put it
// where item 3 was", which is index 2 of the list that remains.
static inline i32 wc_dnd_drop(WDrag *d) {
  if (d->from < 0) return -1;
  i32 target = d->over < 0 ? -1 : (d->after ? d->over + 1 : d->over);
  if (target < 0) return -1;
  if (target == d->from || target == d->from + 1) return -1; // a no-op, said as one
  return target > d->from ? target - 1 : target;
}
#define dnd_drop(d_) wc_dnd_drop((d_))
#define dnd_can(d_) (wc_dnd_drop((d_)) >= 0)

// The drop, once: reads the pointer, answers "move from → to" as two numbers a handler can use
// directly. -1 in `to` means "nothing to do" and the action should return without a request.
static inline bool wc_dnd_take(WDrag *d, i32 *from, i32 *to) {
  wc_dnd_over(d);
  i32 t = wc_dnd_drop(d);
  if (from) *from = d->from;
  if (to) *to = t;
  return t >= 0;
}
#define dnd_take(d_, from_, to_) wc_dnd_take((d_), (from_), (to_))

// `alt+↑` / `alt+↓` on the focused row: the same table `keys.h` binds, so a keyboard user
// reorders without a second interaction nobody documented. The action gets the row from
// `ev_attr_i64("data-dnd-i")`.
static inline void wc_dnd_keys(void) {
  attr("aria-keyshortcuts", "Alt+ArrowUp Alt+ArrowDown");
}

#define dnd_keys() wc_dnd_keys()

// The "moved to position 3" announcement, for the app's own copy: the module does not invent
// words, it hands the number over and the app says it in its language.
static inline str wc_dnd_position(i32 i, i32 n) {
  Buf b;
  buf_init(&b, wc_arena_get(), 24);
  buf_printf(&b, "%d/%d", i + 1, n);
  return buf_str(&b);
}
#define dnd_position(i_, n_) wc_dnd_position((i_), (n_))