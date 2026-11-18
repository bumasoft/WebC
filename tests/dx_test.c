// dx: one test per way the framework can fail silently.
//
// Each case below is a failure that reports nothing: a dropped root, a middleware that
// never ran, a script blocked by the framework's own policy. The test is the repro, so the
// behaviour cannot quietly change.
#include <webc/test.h>

static i32 count_hits(str body, const char *needle) {
  i32 n = 0;
  i64 at = 0;
  str nd = str_lit(needle);
  while ((at = str_find(body, nd, (size_t)at)) >= 0) {
    n++;
    at += (i64)nd.n;
  }
  return n;
}

// ---------------------------------------------------------------- 01 page head

static Response handmade_page(Request *req) {
  (void)req;
  head_title(str_lit("Second — a section"));
  head_meta(str_lit("description"), str_lit("a \"quoted\" blurb & more"));
  head_prop(str_lit("og:title"), str_lit("Second"));
  head_link(str_lit("canonical"), str_lit("https://example.test/second"));
  // No res_page(), no shell file: a document assembled by hand, which is what an
  // app that wants a per-page <head> is forced into when there are no head slots.
  return res_html(200, str_lit("<!doctype html><html><head><title>Shell title</title>"
                                 "<meta charset=\"utf-8\"></head><body><h1>hi</h1></body></html>"));
}

TEST(head_title_replaces_the_shell_title) {
  Response r = webc_test_request("GET", "/dx/head");
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_CONTAINS(r, "<title>Second — a section</title>");
  ASSERT_STR_CONTAINS(r, "content=\"a &quot;quoted&quot; blurb &amp; more\"");
  ASSERT_STR_CONTAINS(r, "<meta property=\"og:title\" content=\"Second\">");
  ASSERT_STR_CONTAINS(r, "<link rel=\"canonical\"");
  str body = WC_TEST_VIEW(r);
  // replaced, not doubled, and inside <head>
  ASSERT_EQ(count_hits(body, "<title>"), 1);
  ASSERT_TRUE(str_index(body, 'S') >= 0);
  ASSERT_TRUE(str_find(body, str_lit("Shell title"), 0) < 0);
  i64 head_end = str_find(body, str_lit("</head>"), 0);
  i64 canonical = str_find(body, str_lit("rel=\"canonical\""), 0);
  ASSERT_GT(head_end, canonical);
}

// ---------------------------------------------------------------- 02 second root

static void two_roots_view(WBuild *b) {
  (void)b;
  el("div") {
    text("first");
  }
  el("aside") {
    text("second");
  }
}

TEST(a_second_root_is_not_dropped) {
  WComponent c;
  wcomp_init(&c, nullptr);
  str html = wcomp_html(&c, two_roots_view);
  ASSERT_STR_CONTAINS(html, "first");
  ASSERT_STR_CONTAINS(html, "second");
  ASSERT_STR_CONTAINS(html, "<aside");
  wcomp_destroy(&c);
}

// ---------------------------------------------------------------- 03 middleware

#define DX_MW 12
static i32 dx_mw_ran;

static bool dx_mw_fn(Request *req, void *ctx, WNext *next) {
  (void)req;
  (void)ctx;
  dx_mw_ran++;
  return wc_next_run(next);
}

TEST(every_global_middleware_runs_on_a_matched_route) {
  webc_use_reset();
  dx_mw_ran = 0;
  for (i32 i = 0; i < DX_MW; i++)
    webc_use((WMiddleware){.name = "dx", .fn = dx_mw_fn});
  Response r = webc_test_request("GET", "/dx/mw");
  ASSERT_EQ(r.status, 200);
  // The stock server installs seven of its own; the eighth slot is where an app's first
  // middleware stops existing if a matched route runs eight while an unmatched path runs
  // thirty-two.
  ASSERT_EQ(dx_mw_ran, DX_MW);
  Response miss = webc_test_request("GET", "/dx/nowhere");
  ASSERT_EQ(miss.status, 404);
  ASSERT_EQ(dx_mw_ran, DX_MW * 2);
  webc_use_reset();
}

// ---------------------------------------------------------------- 04 CSP

TEST(the_default_policy_allows_the_frameworks_own_wasm) {
  webc_use_reset();
  webc_use(mw_headers_default());
  Response r = webc_test_request("GET", "/dx/mw");
  ASSERT_HEADER_CONTAINS(r, "Content-Security-Policy", "'wasm-unsafe-eval'");
  ASSERT_HEADER_COUNT(r, "Content-Security-Policy", 1);
  webc_use_reset();
}

// ---------------------------------------------------------------- 05 raw + svg

static void raw_view(WBuild *b) {
  (void)b;
  el("figure") {
    markup("<svg viewBox=\"0 0 8 8\"><path d=\"M0 0L8 8\"/></svg>");
  }
}

TEST(raw_is_markup_on_both_halves) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, raw_view);
  str html = wcomp_html(&c, raw_view);
  ASSERT_STR_CONTAINS(html, "<svg viewBox=\"0 0 8 8\">");
  ASSERT_STR_CONTAINS(html, "<path d=\"M0 0L8 8\"/>");
  // over the bridge it is a `raw` command, not a text node full of markup (which is how the
  // client half prints an SVG as its own source)
  str cmds = buf_str(&c.cmds.b);
  ASSERT_STR_CONTAINS(cmds, "raw ");
  ASSERT_EQ(count_hits(cmds, "text "), 0);
  wcomp_destroy(&c);
}

// ---------------------------------------------------------------- 06 wasm path

TEST(the_artefact_the_build_writes_is_the_one_that_is_served) {
  ASSERT_STR_EQ(str_lit(WC_WASM_PATH), ".webc/app.wasm");
  ASSERT_STR_EQ(str_lit(WC_WASM_STEM), ".webc/app");
}

// ---------------------------------------------------------------- 07 nonce

TEST(a_hand_built_page_gets_the_nonce_too) {
  Response r = webc_test_request("GET", "/dx/handmade");
  ASSERT_STR_CONTAINS(r, "<script nonce=\"");
  ASSERT_EQ(count_hits(WC_TEST_VIEW(r), "nonce=\""), 1);
}

// ---------------------------------------------------------------- 08 cookies

TEST(cookie_attributes_are_not_duplicated) {
  Response r = webc_test_request("POST", "/dx/cookie");
  str c = wc_test_res_header(r, "Set-Cookie");
  ASSERT_TRUE(c.n > 0);
  ASSERT_EQ(count_hits(c, "Path=/"), 1);
  ASSERT_EQ(count_hits(c, "HttpOnly"), 1);
  ASSERT_EQ(count_hits(c, "SameSite=Lax"), 1);
  ASSERT_STR_CONTAINS(c, "Max-Age=60");
}

// ---------------------------------------------------------------- 09 path/query

static Response text_route(Request *req) {
  (void)req;
  Text seg = path("slug");
  Text q = query_text("q");
  return res_text(200, fmt("%s|%s", seg, q));
}

TEST(path_and_query_have_a_text_flavour) {
  Response r = webc_test_request("GET", "/dx/t/hello?q=world");
  ASSERT_STR_EQ(r, "hello|world");
}

// ---------------------------------------------------------------- 10 ordered lists

WEBC_MODEL(DxNote, (id, i64, .pk = true), (title, str), (rank, i64))
WEBC_VEC_DEFINE(VecDxNote, vec_dx_note, DxNote)

TEST(db_all_can_be_ordered) {
  WDB *db = db_open("sqlite::memory:");
  ASSERT_NOT_NULL(db);
  ASSERT_OK(db_exec_sql(db, "create table dx_notes (id integer primary key autoincrement,"
                             " title text not null, rank integer not null default 0)"));
  const char *titles[] = {"c", "a", "b"};
  const i64 ranks[] = {3, 1, 2};
  for (i32 i = 0; i < 3; i++) {
    DxNote n = {.title = str_lit(titles[i]), .rank = ranks[i]};
    ASSERT_TRUE(db_insert_into(db, DxNote_modelof(), &n, nullptr));
  }
  VecDxNote *plain = db_all(db, DxNote);
  ASSERT_NOT_NULL(plain);
  ASSERT_EQ(plain->n, 3);
  VecDxNote *sorted = db_all_ordered(db, DxNote, "rank asc");
  ASSERT_EQ(sorted->n, 3);
  ASSERT_STR_EQ(sorted->at[0].title, "a");
  ASSERT_STR_EQ(sorted->at[1].title, "b");
  ASSERT_STR_EQ(sorted->at[2].title, "c");
  db_close(db);
}

// ---------------------------------------------------------------- 11 field cap

WEBC_MODEL(DxWide, (f01, i64), (f02, i64), (f03, i64), (f04, i64), (f05, i64), (f06, i64),
           (f07, i64), (f08, i64), (f09, i64), (f10, i64), (f11, i64), (f12, i64), (f13, i64),
           (f14, i64), (f15, i64), (f16, i64), (f17, i64), (f18, i64), (f19, i64), (f20, i64))

TEST(a_model_is_as_wide_as_it_says) {
  // twenty fields, which is the widest a WEBC_MODEL can be; the header that holds the slot
  // count is generated for exactly this reason
  ASSERT_EQ(DxWide_modelof()->n, 20);
  ASSERT_STR_EQ(DxWide_modelof()->fields[19].name, "f20");
}

// ---------------------------------------------------------------- routes

GET("/dx/head") { return handmade_page(req); }
GET("/dx/handmade") {
  return res_html(200, str_lit("<html><head></head><body><script>x()</script></body></html>"));
}
GET("/dx/mw") {
  (void)req;
  return res_text(200, "ok");
}
GET("/dx/t/:slug") { return text_route(req); }
POST("/dx/cookie") {
  Response r = res_text(200, "ok");
  res_cookie(&r, "session", "abc", "HttpOnly; SameSite=Lax; Max-Age=60");
  return r;
}

TEST_MAIN();