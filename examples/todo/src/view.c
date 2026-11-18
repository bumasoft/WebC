// The page. Markup only: no handlers, no state, no database.
//
// It renders on the server and (unchanged) in a browser, and every interaction in
// it is a plain form post, so the whole app works with JavaScript switched off.
#include "todo.h"

// 2026-10-28T12:50:36.083Z → "2026-10-28 12:50". The stored timestamp is RFC3339;
// nobody wants ten characters of milliseconds in a list.
static Text short_time(Text ts) {
  if (!ts || strlen(ts) < 16) return "";
  return fmt("%.10s %.5s", ts, ts + 11);
}

static i64 left(const VecTodo *v) {
  i64 n = 0;
  for_each(t, (*v))
    if (!t->done) n++;
  return n;
}

view(Todos, VecTodo *todos) {
  el("main") {
    el("h1") { text("Todos"); }
    el("p") { class_("count"); text("%lld left", left(todos)); }

    el("form") { post_to("/api/todos");
      el("input") { name("title"); placeholder("what needs doing?"); required(); }
      el("button") { text("Add"); }
    }

    el("ul") { class_("todos");
      for_each(t, (*todos)) {
        el("li") { class_(t->done ? "done" : "");
          el("form") { post_to("/todos/toggle"); hidden_i("id", t->id);
            el("button") { class_("tick"); text(t->done ? "✓" : "○"); }
          }
          el("span") { class_("title"); text(t->title); }
          el("span") { class_("when"); text(short_time(t->created_at)); }
          el("form") { post_to("/todos/delete"); hidden_i("id", t->id);
            el("button") { class_("x"); text("×"); }
          }
        }
      }
    }

    el("p") { class_("empty"); show(len(*todos) == 0); text("Nothing left. Enjoy it."); }
  }
}
