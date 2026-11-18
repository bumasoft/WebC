// page: a page's head and the reader's settings, as values a route passes in the context.
// The alternative is four calls for a title and five more for the rest, repeated per
// route — 25 of them in a seven-page app — with nothing said when a route forgets the
// title, which is a document that is wrong in the tab and nowhere else.
#include <webc/test.h>

typedef struct ProbeCtx {
  WPage page; // first field: this is what makes the context a page
  const char *lead;
} ProbeCtx;
declare_view(Probe, ProbeCtx);

// A view that writes its own document, so the head this test asserts on is the head a
// browser would get. (An app with public/index.html gets the same splice into its shell.)
view(Probe, ProbeCtx *c) {
  el("html") {
    el("head") {}
    el("body") {
      el("h1") { text(c->lead); }
      el("p") {
        class_("env");
        text("theme=%s|path=%s|q=%s|dark=%d", c->page.env.theme ? c->page.env.theme : "",
             c->page.env.path ? c->page.env.path : "", c->page.env.query ? c->page.env.query : "",
             (int)c->page.env.dark);
      }
    }
  }
}

GET("/probe", "probe") {
  (void)req;
  return render(Probe, .page = head(.title = "The Meridian — an independent daily",
                                    .desc = "The city, culture, science and opinion.",
                                    .og_site = "The Meridian",
                                    .canonical = "https://themeridian.example/",
                                    .alternate = "https://themeridian.example/feed.xml"),
                .lead = "Lead");
}

// Where a tag starts, so "in this order" can be asserted rather than eyeballed.
static i64 at_of(str hay, const char *needle) {
  size_t n = strlen(needle);
  for (size_t i = 0; hay.p && i + n <= hay.n; i++)
    if (memcmp(hay.p + i, needle, n) == 0) return (i64)i;
  return -1;
}

TEST(the_head_arrives_as_a_value_in_order) {
  // `head(...)` is a compound literal, so every name in it is checked by the compiler and
  // the order of the tags is the framework's business, not each route's.
  Response r = webc_test_request("GET", "/probe");
  ASSERT_EQ(r.status, 200);
  str body = (str){r.body.p, r.body.n};
  ASSERT_STR_CONTAINS(body, "<h1 data-wc=");
  ASSERT_STR_CONTAINS(body, "Lead</h1>");
  ASSERT_STR_CONTAINS(body, "<title>The Meridian — an independent daily</title>");
  ASSERT_STR_CONTAINS(body, "<meta name=\"description\" content=\"The city, culture, science and opinion.\">");
  ASSERT_STR_CONTAINS(body, "<meta property=\"og:title\"");
  ASSERT_STR_CONTAINS(body, "<meta property=\"og:site_name\" content=\"The Meridian\">");
  ASSERT_STR_CONTAINS(body, "<link rel=\"canonical\" href=\"https://themeridian.example/\">");
  ASSERT_STR_CONTAINS(body, "<link rel=\"alternate\" href=\"https://themeridian.example/feed.xml\">");
  // ...and in that order: the description, then the canonical link, then the feed
  i64 desc_at = at_of(body, "<meta name=\"description\"");
  i64 canon_at = at_of(body, "<link rel=\"canonical\"");
  i64 alt_at = at_of(body, "<link rel=\"alternate\"");
  ASSERT_TRUE(desc_at > 0);
  ASSERT_TRUE(canon_at > desc_at);
  ASSERT_TRUE(alt_at > canon_at);
}

TEST(the_environment_is_filled_from_the_request) {
  Response r = webc_test_request_ex("GET", "/probe?q=ferry",
                                    "Cookie: theme=dark\r\n", nullptr, 0);
  str body = (str){r.body.p, r.body.n};
  ASSERT_STR_CONTAINS(body, "theme=dark");
  ASSERT_STR_CONTAINS(body, "path=/probe");
  ASSERT_STR_CONTAINS(body, "q=ferry");
  ASSERT_STR_CONTAINS(body, "dark=1");
  // the reader's theme goes on <html>, so the first byte is already the right colour
  ASSERT_STR_CONTAINS(body, "data-theme=\"dark\"");
}

TEST(a_context_without_a_page_is_left_alone) {
  // The magic first field is what keeps `render()` honest for a context that predates the
  // page: no page block, no environment, no head, no crash.
  typedef struct PlainCtx {
    i32 n;
  } PlainCtx;
  PlainCtx plain = {.n = 1};
  ASSERT_EQ(webc_page_begin(&plain), (void *)&plain);
}

// `page(View, ctx)` is the form the README, the tutorial and the scaffold all teach, so it has
// to do what `webc_page_begin()` does: the theme on <html>, a filled `env`, the head, and the
// "a page with no title" warning that lives inside that call. Skipping it means an app has to
// `#undef page` to get its own pages right.
GET("/probe/one-value") {
  // A plain local: a handler is a green task and its stack survives a park, so two saves on
  // two workers never share this context (a `static` spelling here is a data race).
  ProbeCtx ctx = {.page = head(.title = "One value page", .desc = "with a head"),
                  .lead = "One value"};
  return page(Probe, &ctx);
}

TEST(page_with_one_context_is_the_same_page_as_render) {
  Response r = webc_test_request_ex("GET", "/probe/one-value?q=x", "Cookie: theme=light\r\n",
                                    nullptr, 0);
  ASSERT_EQ(r.status, 200);
  str body = (str){r.body.p, r.body.n};
  ASSERT_STR_CONTAINS(body, "<title>One value page</title>");
  ASSERT_STR_CONTAINS(body, "with a head");
  ASSERT_STR_CONTAINS(body, "data-theme=\"light\"");
  ASSERT_STR_CONTAINS(body, "theme=light");
  ASSERT_STR_CONTAINS(body, "q=x");
}

TEST(a_page_block_round_trips) {
  WPage p = head(.title = "t");
  ASSERT_EQ(p.tag, (u64)WC_PAGE_MAGIC);
  ASSERT_STR_EQ(p.head.title, "t");
  ASSERT_NULL(p.head.desc);
  ASSERT_NULL(p.head.canonical);
}

TEST_MAIN()