// WebC — a todo list that lives entirely in the browser.
//
// This is the "no server half" shape: there is no routes file and no database,
// because nothing here needs one. `client_main` is the whole entry point, and the
// wasm module is the app. The server-rendered counterpart is examples/todo.
#include <webc/web.h>
#include <webc/host.h>
#include "notes.h"

static VecNote g_notes;
static i64 g_next_id = 1;

// The typed field, bound straight to a context field: no action, no signal —
// bind_ctx persists the text into `ui.draft` as the reader types, quietly. The view
// never draws the value back (a value attribute is only the default, and the browser
// owns the live one), so `add` clears the box through the same host the rest of the
// app uses for the document: field_value(), see <webc/host.h>.
typedef struct {
  Text draft;
} Ui;
static Ui ui;

// ---------------------------------------------------------------- what a click does

action(add) {
  if (!ui.draft || !*ui.draft) return;
  push(g_notes, ((Note){.id = g_next_id++, .title = ui.draft}));
  ui.draft = "";
  field_value("draft", str_lit("")); // the box the reader typed into: theirs, not a render's
  focus_hook("draft");               // ready for the next one, without a click
}

action(toggle, Note *n) {
  n->done = !n->done;
}

action(remove, Note *n) {
  for (i32 i = 0; i < vlen(&g_notes); i++)
    if (vat(&g_notes, i).id == n->id) {
      remove_at(g_notes, i);
      break;
    }
}

// ---------------------------------------------------------------- the view

static i64 left(void) {
  i64 n = 0;
  for_each(x, g_notes) if (!x->done) n++;
  return n;
}

view(App) {
  el("main") {
    el("h1") { text("Todos, in wasm"); }
    el("p") { class_("count"); text("%lld left", left()); }

    el("form") { named("new");
      el("input") { named_field("draft"); placeholder("what needs doing?"); }
      el("button") { type_("button"); on_click(add); text("Add"); }
    }

    el("ul") {
      for_each(n, g_notes) {
        el("li") { class_(n->done ? "done" : "");
          el("button") { class_("tick"); on_click(toggle); text(n->done ? "✓" : "○"); }
          el("span") { text(n->title); }
          el("button") { class_("x"); on_click(remove); text("×"); }
        }
      }
    }

    el("p") { class_("hint"); text("every click crossed the bridge into wasm"); }
  }
}

// ---------------------------------------------------------------- the client half
//
// `webc dev` keeps the page and swaps the module on every save. Signals cross that swap
// by themselves; this example's `g_notes` is a plain vector, so a save starts the list
// over — an app that wants it kept defines `webc_dev_state_save()` and
// `webc_dev_state_load()` (see <webc/webc_dev.h>). They are dev-only and optional.

client_main {
  mount(App, "#root");
  bind_ctx("draft", ui, draft); // the input named_field("draft") in the view
  bind("new", "submit", add);   // Enter submits the form; the bridge owns the default
  return 0;                     // the buttons wire themselves, in the view
}
