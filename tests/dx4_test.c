// dx4: the number formatters and sb(), the render-scoped string builder.
//
// These hold shut the two things every console in WebC wrote by hand:
//
//   * thousands separators, money, "4.2%", and 10k/2.5M axis labels — printf has none of
//     them, so each app grows its own
//   * a big attribute value built in a loop, which otherwise means a file-scope char array
//     with a comment explaining how long the bytes live
#include <webc/test.h>

// ---------------------------------------------------------------- the formatters

static void num_page(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("div") {
    el("i") { class_("n0"); text(fmt_num(0)); }
    el("i") { class_("n999"); text(fmt_num(999)); }
    el("i") { class_("n1000"); text(fmt_num(1000)); }
    el("i") { class_("n128537"); text(fmt_num(128537)); }
    el("i") { class_("nneg"); text(fmt_num(-1234567)); }
    el("i") { class_("m0"); text(fmt_money(0.4)); }
    el("i") { class_("mneg0"); text(fmt_money(-0.4)); }
    el("i") { class_("m1235"); text(fmt_money(1234.6)); }
    el("i") { class_("mneg"); text(fmt_money(-1234.6)); }
    el("i") { class_("m10k"); text(fmt_money(10000.0)); }
    el("i") { class_("p42"); text(fmt_pct(0.0421, 1)); }
    el("i") { class_("p100"); text(fmt_pct(0.999, 0)); }
    el("i") { class_("p002"); text(fmt_pct(0.0, 2)); }
    el("i") { class_("c999"); text(fmt_compact(999.0)); }
    el("i") { class_("c1040"); text(fmt_compact(1040.0)); }
    el("i") { class_("c2500"); text(fmt_compact(2500.0)); }
    el("i") { class_("c1m"); text(fmt_compact(1e6)); }
    el("i") { class_("c25m"); text(fmt_compact(2.5e6)); }
    el("i") { class_("c12b"); text(fmt_compact(1.2e9)); }
    el("i") { class_("c0"); text(fmt_compact(0.0)); }
    el("i") { class_("euro"); text(fmt_money_c(1500.0, "€")); }
  }
}
GET("/dx4/num") { return wc_render_fragment(200, num_page, nullptr); }

TEST(numbers_read_the_way_a_page_needs_them) {
  Response r = webc_test_request("GET", "/dx4/num");
  ASSERT_EQ(r.status, 200);
  ASSERT_HTML_TEXT(r, "i.n0", "0");
  ASSERT_HTML_TEXT(r, "i.n999", "999");
  ASSERT_HTML_TEXT(r, "i.n1000", "1,000");
  ASSERT_HTML_TEXT(r, "i.n128537", "128,537");
  ASSERT_HTML_TEXT(r, "i.nneg", "-1,234,567");
  ASSERT_HTML_TEXT(r, "i.m0", "$0");
  ASSERT_HTML_TEXT(r, "i.mneg0", "$0");     // under half a unit is no money at all
  ASSERT_HTML_TEXT(r, "i.m1235", "$1,235");
  ASSERT_HTML_TEXT(r, "i.mneg", "-$1,235"); // -$1,235, not $-1,235
  ASSERT_HTML_TEXT(r, "i.m10k", "$10,000");
  ASSERT_HTML_TEXT(r, "i.p42", "4.2%");
  ASSERT_HTML_TEXT(r, "i.p100", "100%");
  ASSERT_HTML_TEXT(r, "i.p002", "0.00%");
  ASSERT_HTML_TEXT(r, "i.c999", "999");
  ASSERT_HTML_TEXT(r, "i.c1040", "1.0k");
  ASSERT_HTML_TEXT(r, "i.c2500", "2.5k");
  ASSERT_HTML_TEXT(r, "i.c1m", "1M");
  ASSERT_HTML_TEXT(r, "i.c25m", "2.5M");
  ASSERT_HTML_TEXT(r, "i.c12b", "1.2B");
  ASSERT_HTML_TEXT(r, "i.c0", "0");
  ASSERT_HTML_TEXT(r, "i.euro", "€1,500");
}

TEST(the_formatters_are_text_and_they_nest) {
  // No cast, no buffer: they are Text in the current scope, exactly like fmt().
  Text t = fmt_money(1234567.0);
  ASSERT_STR_EQ(t, "$1,234,567");
  ASSERT_STR_EQ(fmt("%s of %s", fmt_num(1200), fmt_num(5000)), "1,200 of 5,000");
}

// ---------------------------------------------------------------- sb()

static void sb_page(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  // two builders alive at once: a curve and the fat invisible line that shares its points
  Buf *curve = sb();
  Buf *hit = sb();
  for (i32 i = 0; i < 3; i++) {
    sb_num(curve, (double)i * 10.0, 1);
    sb_putc(curve, ',');
    sb_num(curve, 100.0 - (double)i, 0);
    if (i < 2) sb_putc(curve, ' ');
    sb_i64(hit, i);
  }
  el("svg") {
    el("polyline") {
      class_("curve");
      attr("points", sb_str(curve));
    }
    el("polyline") {
      class_("hit");
      attr("points", sb_str(hit));
    }
  }
  // and a string built from printf where a format says it best
  Buf *label = sb();
  sb_printf(label, "%s / %s", fmt_compact(1040.0), fmt_pct(0.5, 0));
  el("p") { class_("label"); text(sb_str(label)); }
}
GET("/dx4/sb") { return wc_render_fragment(200, sb_page, nullptr); }

TEST(sb_builds_an_attribute_that_lives_as_long_as_the_render) {
  Response r = webc_test_request("GET", "/dx4/sb");
  ASSERT_EQ(r.status, 200);
  ASSERT_HTML(r, "svg > polyline.curve");
  ASSERT_HTML_COUNT(r, "svg > polyline", 2);
  ASSERT_STR_CONTAINS(r, "points=\"0.0,100 10.0,99 20.0,98\"");
  // a second builder in the same expression did not overwrite the first
  ASSERT_STR_CONTAINS(r, "points=\"012\"");
  ASSERT_HTML_TEXT(r, "p.label", "1.0k / 50%");
}

TEST(sb_works_outside_a_render_too) {
  // A test, a tool, a log line: no build in scope, and the same code still works
  // (the scope arena is the fallback).
  Buf *b = sb();
  sb_puts(b, "hello ");
  sb_i64(b, 42);
  ASSERT_STR_EQ(sb_str(b), "hello 42");
  ASSERT_STR_EQ(sb_cstr(b), "hello 42");
}

TEST_MAIN();