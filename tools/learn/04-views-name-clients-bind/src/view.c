// SPDX-License-Identifier: Apache-2.0
// the view draws and *names*: named() is the hook the client half binds to.
// A view compiled for both targets cannot call on_click — it names the element
// and the client names the behaviour. One of the two names here is wrong.
#include "app.h"

view(Tasks, VecTask *tasks) {
  el("main") {
    el("ul") {
      for_each(t, (*tasks)) {
        el("li") {
          text("%s", t->title);
          el("button") { named("done"); text(t->done ? "undo" : "done"); }
        }
      }
    }
  }
}
