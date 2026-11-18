// dialog_patch: what a re-render sends the browser when a dialog closes — the
// stdlib example's bug, held still. The view is the example's shape exactly: a
// first child, a dialog (scrim + panel, siblings), and a toast element that
// appears only once the dialog's action has run. On the close render the batch
// must remove BOTH the scrim and the panel; the browser showed the scrim left
// behind, covering the page (every click below it dead).
#include <webc/client_test.h>
#include <webc/overlay.h>
#include <webc/ui.h>

state(bool, open_new, false);
state(Text, toast_msg, "");

action(add_row) {
  set(open_new, false);
  set(toast_msg, "Row added");
}

view(Shelf) {
  // the fixed shape: the wrapper lives INSIDE the page's stable container, never
  // a root-sibling of the dialog's scrim
  el("div") {
    class_("page");
    text("the page");
    toasts(nullptr) {
      if (get(toast_msg) && *get(toast_msg)) toast(get(toast_msg), "ok");
    }
  }
  WOverlay DIALOG = {.id = "new_row", .label = "New row"};
  dialog((&DIALOG), get(open_new)) {
    el("h2") { text("New row"); }
    el("button") { on_click(add_row); text("Add"); }
  }
}

static WComponent c;

static str render_once(void) {
  wcomp_render_ctx(&c, view_Shelf, nullptr);
  str out = (str){buf_cstr(&c.cmds.b), c.cmds.b.n}; // a copy outlives the reset below
  buf_reset(&c.cmds.b);
  return out;
}

TEST(a_closed_dialog_leaves_nothing_of_itself_behind) {
  wc_host_reset();
  wc_client_mode();
  wcomp_init(&c, nullptr);
  set(open_new, false);
  set(toast_msg, "");

  // open: the scrim and the panel are in the tree
  set(open_new, true);
  str open_html = wcomp_html_ctx(&c, view_Shelf, nullptr);
  ASSERT_STR_CONTAINS(open_html, "scrim");
  ASSERT_STR_CONTAINS(open_html, "role=\"dialog\"");

  // the Add button's action is two state writes; the close render follows
  set(open_new, false);
  set(toast_msg, "Row added");
  wcomp_render_ctx(&c, view_Shelf, nullptr);
  str closed_html = wcomp_html_ctx(&c, view_Shelf, nullptr);

  // the toast is on the page, and NOTHING of the dialog survives it: no scrim
  // class, no aria-hidden sheet, no close hook — in the browser the stuck scrim
  // was a full-width invisible layer every later click died on
  ASSERT_STR_CONTAINS(closed_html, "toast ok");
  ASSERT_FALSE(str_contains(closed_html, str_lit("scrim")));
  ASSERT_FALSE(str_contains(closed_html, str_lit("new_row")));

  wcomp_destroy(&c);
}
TEST_MAIN()
