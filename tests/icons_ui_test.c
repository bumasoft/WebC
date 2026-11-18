// icons and the four controls the shelf assumes: the assertions are the attributes that
// make the difference between "looks fine" and "reachable without a mouse".
#include <webc/test.h>
#include <webc/icons.h>
#include <webc/ui.h>

static str cmds_of(void (*v)(WBuild *)) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, v);
  return buf_str(&c.cmds.b);
}

static void icon_view(WBuild *b) {
  (void)b;
  icon(WC_ICON_PLUS, 16);
  icon(WC_ICON_SEARCH, 0);
  el("button") {
    named("close");
    aria_label("Close");
    icon(WC_ICON_X, 18);
  }
}

static void controls_view(WBuild *b) {
  (void)b;
  badge("2 left", "warn");
  badge("plain", nullptr);
  stat("Bank", "$10,038", "+12%", "good");
  stat("Seats", "24", nullptr, nullptr);
  empty_state("Nothing yet", "Add the first row.", "add_row", "Add");
  empty_state("Nothing yet", nullptr, nullptr, nullptr);
  skeleton("row", 3);
}

TEST(an_icon_is_stroke_markup_that_inherits_its_colour) {
  str cmds = cmds_of(icon_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("stroke currentColor")));
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-hidden true")));
  ASSERT_TRUE(str_contains(cmds, str_lit("width 16")));
  ASSERT_TRUE(str_contains(cmds, str_lit("M12 5v14M5 12h14")));
}

TEST(a_size_of_zero_leaves_the_size_to_css) {
  str cmds = cmds_of(icon_view);
  ASSERT_FALSE(str_contains(cmds, str_lit("height 0")));
  ASSERT_TRUE(str_contains(cmds, str_lit("class icon")));
}

TEST(an_icon_only_button_has_a_name) {
  str cmds = cmds_of(icon_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-label Close")));
}

TEST(a_badge_carries_a_tone_word_never_a_colour) {
  str cmds = cmds_of(controls_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("class badge warn")));
  ASSERT_TRUE(str_contains(cmds, str_lit("2 left")));
  // no tone, no trailing space and no empty class
  ASSERT_TRUE(str_contains(cmds, str_lit("class badge\n")));
}

TEST(a_stat_shows_its_change_only_when_it_has_one) {
  str cmds = cmds_of(controls_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("stat-delta good")));
  ASSERT_TRUE(str_contains(cmds, str_lit("+12%")));
  ASSERT_TRUE(str_contains(cmds, str_lit("$10,038")));
}

TEST(an_empty_state_offers_an_action_only_when_there_is_one) {
  str cmds = cmds_of(controls_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("role status")));
  ASSERT_TRUE(str_contains(cmds, str_lit("data-hook add_row")));
  ASSERT_TRUE(str_contains(cmds, str_lit("class empty-action")));
  ASSERT_FALSE(str_contains(cmds, str_lit("Add the first row. Add")));
}

TEST(a_skeleton_has_the_shape_of_what_is_coming) {
  str cmds = cmds_of(controls_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("class skel skel-row")));
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-hidden true")));
}

TEST_MAIN()
