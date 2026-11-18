// keys and motion: the two tiers that are mostly markup, and the two places where the
// interesting question is what the module refuses to do.
#include <webc/test.h>
#include <webc/keys.h>
#include <webc/motion.h>

ACTION(pal_open)
ACTION(help_open)
ACTION(card_new)
ACTION(esc_close)

action(pal_open) {}
action(help_open) {}
action(card_new) {}
action(esc_close) {}

static const WCmd CMDS[] = {
    K("Command palette", "meta+k", pal_open),
    K("Help", "shift+?", help_open),
    K("New card", "n", card_new),
    K("Close", "escape", esc_close),
};

static const WOverlay HELP = {"help_dlg", "Keyboard", "help_close"};

static str cmds_of(void (*v)(WBuild *)) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, v);
  return buf_str(&c.cmds.b);
}

static void help_view(WBuild *b) {
  (void)b;
  keys_help(CMDS, &HELP, true);
}

static void caps_view(WBuild *b) {
  (void)b;
  keycap("meta+k");
  keycap("shift+?");
  keycap("escape");
  keycap("alt+enter");
}

static void motion_view(WBuild *b) {
  (void)b;
  motion_head();
}

static void items_view(WBuild *b) {
  (void)b;
  el("li") {
    enter("rise");
    stagger(3);
    text("third");
  }
  el("li") { stagger(40); text("clamped"); }
}

TEST(the_table_binds_every_row_and_says_so_once_per_key) {
  wc_host_ops_clear();
  keys_bind(CMDS);
  const char *ops = wc_host_ops();
  ASSERT_TRUE(strstr(ops, "meta+k") != nullptr);
  ASSERT_TRUE(strstr(ops, "shift+?") != nullptr);
  ASSERT_TRUE(strstr(ops, "escape") != nullptr);
  ASSERT_TRUE(strstr(ops, "n") != nullptr);
}

TEST(the_sheet_is_the_same_table_so_nothing_can_be_bound_and_undocumented) {
  str cmds = cmds_of(help_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("role dialog")));
  ASSERT_TRUE(str_contains(cmds, str_lit("Command palette")));
  ASSERT_TRUE(str_contains(cmds, str_lit("New card")));
  ASSERT_TRUE(str_contains(cmds, str_lit("kbd")));
}

TEST(a_spec_reads_as_the_key_cap) {
  str cmds = cmds_of(caps_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("⌘K")));
  ASSERT_TRUE(str_contains(cmds, str_lit("⇧?")));
  ASSERT_TRUE(str_contains(cmds, str_lit("Esc")));
  ASSERT_TRUE(str_contains(cmds, str_lit("⌥↵")));
}

TEST(the_stylesheet_has_the_keyframes_the_query_that_switches_them_off) {
  str cmds = cmds_of(motion_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("@keyframes rise")));
  ASSERT_TRUE(str_contains(cmds, str_lit("[data-anim=rise]")));
  ASSERT_TRUE(str_contains(cmds, str_lit("prefers-reduced-motion:reduce")));
  ASSERT_TRUE(str_contains(cmds, str_lit("animation:none!important")));
}

TEST(an_item_says_what_it_does_and_how_late_it_is) {
  str cmds = cmds_of(items_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("data-anim rise")));
  ASSERT_TRUE(str_contains(cmds, str_lit("data-stagger 3")));
  // the table stops at eleven steps; a row past that is the last step, not off the end
  ASSERT_TRUE(str_contains(cmds, str_lit("data-stagger 11")));
}

TEST_MAIN()
