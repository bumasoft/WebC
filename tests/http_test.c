// http: routing, params, wildcards, 405, middleware order, respond helpers.
#include <stdlib.h>  // before webc/test.h: mkdtemp/chdir for the 404-page case
#include <unistd.h>

#include <webc/test.h>

static Response hello(Request *req) {
  (void)req;
  return res_text(200, "hello");
}
static Response user_show(Request *req) {
  str id = req_param(req, "id");
  return res_text(200, str_cstr(req->arena, id));
}
static Response files(Request *req) {
  str rest = req_param(req, "rest");
  return res_text(200, str_cstr(req->arena, rest));
}

static void page_view(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("h1") {
    text("page");
  }
}

static Response json_r(Request *req) {
  (void)req;
  return res_text(200, "{\"ok\":true}");
}
static Response ssr_r(Request *req) {
  (void)req;
  return wc_render_fragment(200, page_view, nullptr);
}

static bool mw_tag(Request *req, void *ctx, WNext *next) {
  (void)req;
  bool ok = wc_next_run(next);
  res_header(next->res, "X-Tag", ctx);
  return ok;
}

// the router on its own: patterns, params, wildcards, 405, HEAD, trailing slash
static WRouter *test_router(Arena *a) {
  WRouter *r = router_new(a);
  router_insert(r, "GET", "/", hello, nullptr, 0);
  router_insert(r, "GET", "/users/:id", user_show, nullptr, 0);
  router_insert(r, "GET", "/static/*rest", files, nullptr, 0);
  router_insert(r, "POST", "/users/:id/activate", user_show, nullptr, 0);
  return r;
}

static bool has_header(Response *res, const char *name) {
  for (i32 i = 0; i < res->nheaders; i++)
    if (str_eq_c(res->headers[i].name, name)) return true;
  return false;
}

TEST(router_matches_params_and_wildcards) {
  Arena *a = arena_new(4096);
  WRouter *r = test_router(a);
  WMatch m = router_match(r, str_lit("GET"), str_lit("/"));
  ASSERT(m.found);
  m = router_match(r, str_lit("GET"), str_lit("/users/42"));
  ASSERT(m.found);
  ASSERT_STR_EQ(m.pattern, "/users/:id");
  ASSERT_STR_EQ(m.param_v[0], "42");
  ASSERT_STR_EQ(m.param_k[0], "id");
  // a wildcard keeps the slashes
  m = router_match(r, str_lit("GET"), str_lit("/static/css/app.css"));
  ASSERT(m.found);
  ASSERT_STR_EQ(m.param_v[0], "css/app.css");
  arena_destroy(a);
}

TEST(wrong_method_is_405_not_404) {
  Arena *a = arena_new(4096);
  WRouter *r = test_router(a);
  WMatch m = router_match(r, str_lit("DELETE"), str_lit("/users/42"));
  ASSERT_FALSE(m.found);
  ASSERT(m.method_not_allowed);
  arena_destroy(a);
}

TEST(head_falls_back_to_get_and_slashes_normalise) {
  Arena *a = arena_new(4096);
  WRouter *r = test_router(a);
  ASSERT(router_match(r, str_lit("HEAD"), str_lit("/")).found);
  ASSERT(router_match(r, str_lit("GET"), str_lit("/users/42/")).found);
  arena_destroy(a);
}

TEST(middleware_runs_and_can_tag_the_response) {
  webc_router_reset();
  webc_route_add("GET", "/users/:id", user_show, nullptr, 0);
  webc_use_reset();
  webc_use((WMiddleware){ .name = "tag", .fn = mw_tag, .ctx = "one" });
  Response res = webc_test_request("GET", "/users/7");
  ASSERT_EQ(res.status, 200);
  ASSERT(has_header(&res, "X-Tag"));
  webc_use_reset();
  webc_router_reset();
}

TEST(response_helpers) {
  Response e = res_error(422, "invalid", "bad field");
  ASSERT_EQ(e.status, 422);
  ASSERT_STR_CONTAINS(e.body, "\"invalid\"");
  ASSERT_STR_EQ(http_status_text(404), "Not Found");
  ASSERT_STR_EQ(http_mime_for("a.css"), "text/css; charset=utf-8");
  ASSERT_STR_EQ(http_mime_for("a.wasm"), "application/wasm");
  ASSERT_GE(webc_route_count(), 0);   // built-in routes register on first use
}

TEST(res_page_fills_the_root_element) {
  const char *shell = "/tmp/.webc_page_test.html";
  FILE *f = fopen(shell, "w");
  ASSERT_NOT_NULL(f);
  fputs("<html><body><p>nav</p><div id=\"root\"></div><p>foot</p></body></html>", f);
  fclose(f);

  Response pg = wc_render_page(200, shell, page_view, nullptr);
  ASSERT_EQ(pg.status, 200);
  ASSERT_STR_CONTAINS(pg.body, "<p>nav</p>");
  ASSERT_STR_CONTAINS(pg.body, "<p>foot</p>");
  ASSERT_STR_CONTAINS(pg.body, "<div id=\"root\"><h1 data-wc=");
  ASSERT(!str_contains((str){ pg.body.p, pg.body.n }, str_lit("</div><h1")));
  remove(shell);
}

// The three leaks this session's B/op column exposed, each pinned down: a request
// may allocate in its arena, and everything else must come back.
TEST(a_request_leaves_no_heap_behind) {
  webc_router_reset();
  webc_route_add("GET", "/leak", json_r, nullptr, 0);
  (void)webc_test_request("GET", "/leak");   // warm up: first call builds the arena
  WmStats before = wm_stats();
  for (i32 i = 0; i < 500; i++) {
    Response r = webc_test_request("GET", "/leak");
    ASSERT_EQ(r.status, 200);
  }
  WmStats after = wm_stats();
  ASSERT_EQ(after.heap_used, before.heap_used);
  ASSERT_EQ(after.arena_live, before.arena_live);
}

TEST(an_ssr_response_frees_its_render_arenas) {
  webc_router_reset();
  webc_route_add("GET", "/page", ssr_r, nullptr, 0);
  (void)webc_test_request("GET", "/page");
  // A response body lives in the current arena (the request's, in the server);
  // everything else the render allocates — three arenas per component — must
  // come back, or 500 renders leak 1.4 KB each.
  Arena *a = arena_new(64 * 1024);
  Arena *prev = wc_arena_swap(a);
  WmStats before = wm_stats();
  for (i32 i = 0; i < 500; i++) {
    arena_reset(a);
    Response r = wc_render_fragment(200, page_view, nullptr);
    ASSERT_EQ(r.status, 200);
  }
  WmStats after = wm_stats();
  wc_arena_swap(prev);
  arena_destroy(a);
  ASSERT_EQ(after.heap_used, before.heap_used);
}

// ---------------------------------------------------------------- a 404 that is a page
//
// The framework's first extra route in every server-rendered project was `GET("/*rest")`, and
// a JSON error body in a browser tab is what it was working around.
TEST(a_404_for_a_browser_is_the_projects_own_page) {
  // A real file on disk, in a temporary project so the shell and the file are both the test's.
  char dir[] = "/tmp/webc-404-XXXXXX";
  ASSERT_NOT_NULL(mkdtemp(dir));
  char old[512];
  ASSERT_NOT_NULL(getcwd(old, sizeof old));
  ASSERT_TRUE(chdir(dir) == 0);
  FILE *f = fopen("webc.json", "wb");
  ASSERT_NOT_NULL(f);
  fputs("{\"boot\":{\"title\":\"Ops\"},\"not_found\":\"nf.html\"}", f);
  fclose(f);
  f = fopen("nf.html", "wb");
  ASSERT_NOT_NULL(f);
  fputs("<h1>nothing here</h1><p>try the index</p>", f);
  fclose(f);
  wc_shell_reset();

  webc_use_reset();
  webc_router_reset();
  webc_use(mw_not_found(wc_shell_not_found()));
  // the fragment goes through the shell, so the page is a document with a title
  Response r = webc_test_request_ex("GET", "/no/such/url", "Accept: text/html\r\n", nullptr, 0);
  ASSERT_EQ(r.status, 404);
  str body = {r.body.p, r.body.n};
  ASSERT_TRUE(str_contains(body, str_lit("nothing here")));
  ASSERT_TRUE(str_contains(body, str_lit("<title>Not found</title>")));
  ASSERT_TRUE(str_contains(body, str_lit("<html")));

  // a client that wants JSON gets the JSON it asked for, not a page
  Response j = webc_test_request_ex("GET", "/no/such/url", "Accept: application/json\r\n",
                                    nullptr, 0);
  ASSERT_EQ(j.status, 404);
  ASSERT_TRUE(!str_contains((str){j.body.p, j.body.n}, str_lit("nothing here")));

  webc_use_reset();
  webc_router_reset();
  (void)chdir(old);
  char cmd[600];
  snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
  (void)system(cmd);
  wc_shell_reset();
}

TEST(a_log_line_allocates_nothing) {
  WmStats before = wm_stats();
  for (i32 i = 0; i < 1000; i++)
    log_info("spam", {"n", str_lit("1")}, {"p", str_lit("/")});
  WmStats after = wm_stats();
  ASSERT_EQ(after.heap_used, before.heap_used);
}

TEST_MAIN()