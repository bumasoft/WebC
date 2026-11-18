// dx_helpers: the names a developer guesses should be the names that exist.
//
// `class_fmt`, `style_fmt`, `attr_fmt`, `post_to_fmt` were guessed while writing a real app
// and did not exist; every markup helper takes a Text (so `class_(fmt(…))` worked) but
// nothing said so. These tests are compile-time as much as run-time: if the names go away,
// this file stops building.
#include <webc/test.h>

static void helpers_view(WBuild *b) {
  (void)b;
  el("a") {
    class_fmt("row row-%d", 3);
    attr_fmt("data-id", "%d", 7);
    style_fmt("--cols:%d", 2);
    post_to_fmt("/flights/%d", 9);
    text("x");
  }
}

static str attr_of(WDom *n, const char *name) {
  for (i32 i = 0; i < n->nattrs; i++)
    if (str_eq_c(n->attrs[i].name, name)) return n->attrs[i].value;
  return (str){0};
}

TEST(the_fmt_companions_emit_the_formatted_value) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, helpers_view);
  WDom *a = c.root && c.root->type == WD_ELEMENT ? c.root : (c.root ? c.root->first : nullptr);
  ASSERT_NOT_NULL(a);
  ASSERT_STR_EQ(attr_of(a, "class"), "row row-3");
  ASSERT_STR_EQ(attr_of(a, "data-id"), "7");
  ASSERT_STR_EQ(attr_of(a, "style"), "--cols:2");
  ASSERT_STR_EQ(attr_of(a, "action"), "/flights/9");
  ASSERT_STR_EQ(attr_of(a, "method"), "post");
  wcomp_destroy(&c);
}

TEST(fmt_s_is_a_str_with_no_conversion_at_the_call_site) {
  str s = fmt_s("flight %d/%s", 12, "VA111");
  ASSERT_STR_EQ(s, "flight 12/VA111");
  ASSERT_EQ((int)s.n, 15);
  // a Text converts with WC_AS_STR, still without a copy
  Text t = fmt("%d-%d", 1, 2);
  ASSERT_STR_EQ(WC_AS_STR(t), "1-2");
  // and a str converts back to a Text when a helper wants one
  Text back = text_of(s);
  ASSERT_STR_EQ(back, "flight 12/VA111");
}

TEST_MAIN()