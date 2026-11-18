// ui: the components layer (include/webc/ui.h). The ARIA these controls need is the
// part every author re-derives — `role="switch"` against `aria-checked`, a segmented
// button's `aria-pressed` — and the part that silently goes wrong when they don't.
// The assertions here are against the emitted batch, because that is the contract the
// bridge and the browser actually see.
#include <webc/test.h>
#include <webc/ui.h>

static str render_one(void (*view)(WBuild *)) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, view);
  return buf_str(&c.cmds.b);
}

static void switch_view(WBuild *b) {
  (void)b;
  switch_("sound", true, "Sound on");
  switch_("dots", false, "Show dots");
}

static void seg_view(WBuild *b) {
  (void)b;
  seg("filt_all", true, "All", fmt("%lld", (long long)24));
  seg("sort_num", false, "No.", nullptr);
}

static void tab_view(WBuild *b) {
  (void)b;
  tab("tab_telemetry", true, "Telemetry");
  tab("tab_briefing", false, "Crew briefing");
}

static void meter_view(WBuild *b) {
  (void)b;
  meter("Fuel", 62, 100, "62%", "m-fuel");
}

TEST(a_switch_says_its_role_and_state) {
  str cmds = render_one(switch_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("data-hook sound")));
  ASSERT_TRUE(str_contains(cmds, str_lit("role switch")));
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-checked true")));
  ASSERT_TRUE(str_contains(cmds, str_lit("data-hook dots")));
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-checked false")));
  // both carry the track: the CSS draws the knob from it
  ASSERT_TRUE(str_contains(cmds, str_lit("class track")));
  ASSERT_TRUE(str_contains(cmds, str_lit("class track")));
}

TEST(a_segmented_button_is_pressed_or_not_and_may_carry_a_count) {
  str cmds = render_one(seg_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-pressed true")));
  ASSERT_TRUE(str_contains(cmds, str_lit("class seg on")));
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-pressed false")));
  ASSERT_TRUE(str_contains(cmds, str_lit("class seg\n")));
  // a count only when there is one: no empty span for the sorter
  ASSERT_TRUE(str_contains(cmds, str_lit("24")));
  i32 seg_counts = 0;
  for (size_t i = 0; i + 10 <= cmds.n; i++)
    if (memcmp(cmds.p + i, "class seg-count", 15) == 0) seg_counts++;
  ASSERT_EQ((int)seg_counts, 1);
}

TEST(a_tab_and_a_meter_and_a_keycap) {
  str cmds = render_one(tab_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("class tab on")));
  ASSERT_TRUE(str_contains(cmds, str_lit("class tab\n")));
  cmds = render_one(meter_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("class m-fuel")));
  ASSERT_TRUE(str_contains(cmds, str_lit("style width:62%")));
}

TEST(classes_joins_the_present_and_skips_the_absent) {
  // No format string, no trailing space, no empty class when everything is absent —
  // the three shapes the hand-rolled `fmt("%s %s", a, b ? b : "")` produced.
  ASSERT_STR_EQ(classes("row", "sel", (const char *)nullptr), "row sel");
  ASSERT_STR_EQ(classes("row", (const char *)nullptr), "row");
  ASSERT_STR_EQ(classes("row", "", (const char *)nullptr), "row");
  ASSERT_STR_EQ(classes("", (const char *)nullptr), "");
  // two in one expression must not clobber each other
  ASSERT_STR_EQ(classes("a", (const char *)nullptr), "a");
}

TEST_MAIN()