// overlay: the five details that make a box on top usable, and the one that has to happen
// after the batch (focus), which is the one a hand-written dialog always gets wrong.
#include <webc/test.h>
#include <webc/overlay.h>

static const WOverlay CARD = {"card_dlg", "Card", "card_close"};
static const WOverlay PLAIN = {"plain_dlg", nullptr, nullptr};

static str cmds_of(void (*v)(WBuild *)) {
  wc_host_ops_clear();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, v);
  return buf_str(&c.cmds.b);
}

action(esc_close) {}

static void open_card(WBuild *b) {
  (void)b;
  dialog(&CARD, true) {
    el("h2") { text("New card"); }
    el("button") {
      named("card_close");
      text("Close");
    }
  }
}

static void closed_card(WBuild *b) {
  (void)b;
  dialog(&CARD, false) { el("h2") { text("never rendered"); } }
}

static void plain_card(WBuild *b) {
  (void)b;
  dialog(&PLAIN, true) { text("bare"); }
}

static void drawer_view(WBuild *b) {
  (void)b;
  drawer(&CARD, true) { text("Filters"); }
}

static void menu_view(WBuild *b) {
  (void)b;
  menu(true) {
    menu_item("mi_edit", "Edit", false);
    menu_item("mi_del", "Delete", true);
  }
}

static void tip_view(WBuild *b) {
  (void)b;
  tip("Scores reset at midnight") { text("ⓘ"); }
}

static void toast_view(WBuild *b) {
  (void)b;
  toasts(nullptr) { toast("Saved", "ok"); }
}

TEST(an_open_dialog_is_modal_named_and_contains_its_body) {
  str cmds = cmds_of(open_card);
  ASSERT_TRUE(str_contains(cmds, str_lit("role dialog")));
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-modal true")));
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-label Card")));
  ASSERT_TRUE(str_contains(cmds, str_lit("tabindex -1")));
  // the body landed inside it, not beside it
  ASSERT_TRUE(str_contains(cmds, str_lit("New card")));
  ASSERT_TRUE(str_contains(cmds, str_lit("data-hook card_close")));
}

TEST(opening_it_moves_focus_after_the_batch) {
  cmds_of(open_card);
  const char *ops = wc_host_ops();
  ASSERT_TRUE(strstr(ops, "doc-focus card_dlg") != nullptr);
}

TEST(a_closed_dialog_is_not_in_the_page_at_all) {
  str cmds = cmds_of(closed_card);
  ASSERT_FALSE(str_contains(cmds, str_lit("role dialog")));
  ASSERT_FALSE(str_contains(cmds, str_lit("never rendered")));
  ASSERT_NULL(strstr(wc_host_ops(), "doc-focus card_dlg"));
}

TEST(a_dialog_without_a_name_still_opens_and_says_no_label) {
  str cmds = cmds_of(plain_card);
  ASSERT_TRUE(str_contains(cmds, str_lit("role dialog")));
  ASSERT_FALSE(str_contains(cmds, str_lit("aria-label")));
  ASSERT_TRUE(str_contains(cmds, str_lit("bare")));
}

TEST(a_drawer_is_a_dialog_with_a_different_class) {
  str cmds = cmds_of(drawer_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("class panel drawer")));
  ASSERT_TRUE(str_contains(cmds, str_lit("role dialog")));
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-modal true")));
}

// The backdrop is the panel's sibling and the panel is a boundary: the bridge gives every
// event in a hook's subtree to that hook, so a scrim that *wrapped* the panel closes the
// dialog on any click inside it — including a submit button.
TEST(the_backdrop_closes_before_the_panel_opens_so_it_cannot_swallow_it) {
  str cmds = cmds_of(open_card);
  size_t scrim = strstr(cmds.p, "class scrim") ? (size_t)(strstr(cmds.p, "class scrim") - cmds.p) : 0;
  size_t panel = strstr(cmds.p, "class panel") ? (size_t)(strstr(cmds.p, "class panel") - cmds.p) : 0;
  ASSERT_TRUE(scrim > 0 && panel > scrim);
  // the close hook is on the backdrop, and the panel carries the id instead of a hook
  ASSERT_TRUE(str_contains(cmds, str_lit("data-hook card_close")));
  ASSERT_TRUE(str_contains(cmds, str_lit("id card_dlg")));
  ASSERT_FALSE(str_contains(cmds, str_lit("data-hook card_dlg")));
}

TEST(a_menu_is_a_menu_and_its_rows_say_whether_they_work) {
  str cmds = cmds_of(menu_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("role menu")));
  ASSERT_TRUE(str_contains(cmds, str_lit("role menuitem")));
  ASSERT_TRUE(str_contains(cmds, str_lit("data-hook mi_edit")));
  // the disabled row is announced, and is not a hook: nothing may bind it
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-disabled true")));
  ASSERT_FALSE(str_contains(cmds, str_lit("data-hook mi_del")));
}

TEST(a_tip_is_reachable_without_a_mouse) {
  str cmds = cmds_of(tip_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("data-tip Scores reset at midnight")));
  ASSERT_TRUE(str_contains(cmds, str_lit("tabindex 0")));
  ASSERT_TRUE(str_contains(cmds, str_lit("class tip")));
}

TEST(a_toast_is_announced_inside_a_live_region) {
  str cmds = cmds_of(toast_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-live polite")));
  ASSERT_TRUE(str_contains(cmds, str_lit("role status")));
  ASSERT_TRUE(str_contains(cmds, str_lit("class toast ok")));
}

TEST(the_escape_key_is_one_key_spec_and_the_app_s_own_action) {
  wc_host_ops_clear();
  // the macro is the whole API: a key spec and the name of the action
  overlay_esc(esc_close);
  const char *ops = wc_host_ops();
  ASSERT_TRUE(strstr(ops, "key ") != nullptr);
  ASSERT_TRUE(strstr(ops, "escape") != nullptr);
}

TEST_MAIN()