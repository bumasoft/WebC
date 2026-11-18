// SPDX-License-Identifier: Apache-2.0
// learn 03 — the view is markup, and the route fills its context by name.
//
// `render(Home, .items = ..., .note = ...)` passes fields the compiler checks; the
// view reads them as c->items and c->note. The list is drawn; the note is not.
// Draw it: one element, one class, one text — then look at the test again.
#include <webc/server.h>
#include "app.h"

view(Home, HomeCtx *c) {
  el("main") {
    el("ul") {
      for_each(it, (*c->items)) {
        el("li") { text("%s", it->name); }
      }
    }
  }
}

GET("/") {
  // ids are set on purpose: for_each keys a list by the model's .pk, and 0 is
  // its unset value — two hand-built rows without ids share a key, and the
  // framework says so (a panic naming the key, not a page that spins)
  VecItem *items = vec_new(Item);
  push((*items), ((Item){.id = 1, .name = "compass"}));
  push((*items), ((Item){.id = 2, .name = "chart"}));
  return render(Home, .items = items, .note = "3 voyages out");
}
