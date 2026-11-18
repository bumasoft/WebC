// theme: the three declarations a flash-free dark mode needs, written from one table.
//
// The assertion is on the CSS text, because that text *is* the contract: cascade order is
// the only thing that makes the first paint right, and a hand-written theme that gets it
// wrong looks correct in the source and wrong for half a second on the reader's screen.
#include <webc/test.h>
#include <webc/theme.h>

static const WToken TOKENS[] = {
    {"bg", "#fbfaf7", "#12130f"},
    {"fg", "#1a1a18", "#eceae4"},
    {"acc", "#3d4ee0", nullptr}, // no dark value: the light one stands
};
static const WTheme TH = {"probe", TOKENS, WC_ARRLEN(TOKENS), "Inter, system-ui", nullptr};

static void theme_view(WBuild *b) {
  (void)b;
  theme_head(&TH);
}

static str render_one(void (*view)(WBuild *)) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, view);
  return buf_str(&c.cmds.b);
}

TEST(wc_arrlen_counts_a_table_you_can_see) {
  ASSERT_EQ(WC_ARRLEN(TOKENS), 3);
  i32 i[] = {1, 2, 3, 4, 5, 6, 7};
  ASSERT_EQ(WC_ARRLEN(i), 7);
}

TEST(the_light_block_comes_first_and_the_attribute_block_after_it) {
  str css = render_one(theme_view);
  ASSERT_TRUE(str_contains(css, str_lit(":root{color-scheme:light;--bg:#fbfaf7;--fg:#1a1a18")));
  ASSERT_TRUE(str_contains(css, str_lit(":root[data-theme=dark]{color-scheme:dark;--bg:#12130f")));
  // the attribute wins where it is present, so it has to come after the light block
  size_t light = str_find(css, str_lit(":root{color-scheme:light"), 0);
  size_t dark = str_find(css, str_lit(":root[data-theme=dark]"), 0);
  ASSERT_TRUE(light < dark);
}

TEST(a_token_without_a_dark_value_keeps_its_light_one) {
  str css = render_one(theme_view);
  // --acc has no dark column, so the dark blocks must not say --acc:… at all
  size_t at = str_find(css, str_lit("data-theme=dark"), 0);
  str tail = (str){.p = css.p + at, .n = css.n - at};
  ASSERT_FALSE(str_contains(tail, str_lit("--acc:")));
  ASSERT_TRUE(str_contains(css, str_lit("--acc:#3d4ee0")));
}

TEST(a_reader_who_never_switched_still_gets_dark_from_the_os) {
  str css = render_one(theme_view);
  ASSERT_TRUE(str_contains(css, str_lit("@media (prefers-color-scheme:dark)")));
  // … unless the cookie said light, which is what the :not() is for
  ASSERT_TRUE(str_contains(css, str_lit(":root:not([data-theme=light])")));
}

TEST(fonts_are_optional_and_only_written_when_given) {
  str css = render_one(theme_view);
  ASSERT_TRUE(str_contains(css, str_lit("--font-sans:Inter, system-ui")));
  ASSERT_FALSE(str_contains(css, str_lit("--font-mono")));
}

TEST(painting_the_edition_is_one_host_verb) {
  wc_host_ops_clear();
  theme_paint(true);
  theme_paint(false);
  const char *ops = wc_host_ops();
  ASSERT_TRUE(strstr(ops, "doc-attr data-theme dark") != nullptr);
  ASSERT_TRUE(strstr(ops, "doc-attr data-theme light") != nullptr);
}

TEST_MAIN()