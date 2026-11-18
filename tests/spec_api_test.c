// spec_api_test.c — the names SPEC §6.1/6.2/6.4/11 use, exercised end to end.
//
// Each test here exists because the spec spells a call a particular way and the
// implementation has to answer to that spelling: `Mutex`, `Chan(int)`,
// `app_state_set`, `body_json(T)`, `req->ctx`, `req->nonce`. A rename is cheap; a
// rename nobody tested is a rename that broke at the first user.
#include <webc/test.h>
#include <webc/server.h>
#include <webc/web.h>

// .gte, not .min: .min bounds a string's length, and the compiler rejects it on a number
WEBC_MODEL(Deposit, (account, i64, .required = true), (cents, i64, .required = true, .gte = 1))

static _Atomic int g_once_ran;
static void once_bump(void) { atomic_fetch_add(&g_once_ran, 1); }

// ---------------------------------------------------------------- sync names

TEST(mutex_rwlock_once_waitgroup_names) {
  Mutex m = WEBC_MUTEX_INIT;
  mutex_lock(&m);
  ASSERT_FALSE(mutex_trylock(&m)); // held by this task: trylock says no
  mutex_unlock(&m);
  ASSERT_TRUE(mutex_trylock(&m));
  mutex_unlock(&m);

  RwLock l = {0};
  rwlock_rlock(&l);
  rwlock_rlock(&l); // two readers at once
  rwlock_runlock(&l);
  rwlock_runlock(&l);
  rwlock_lock(&l);
  rwlock_unlock(&l);

  Once o = WEBC_ONCE_INIT;
  once_do(&o, once_bump);
  once_do(&o, once_bump);
  ASSERT_EQ(g_once_ran, 1);

  WaitGroup wg;
  waitgroup_init(&wg);
  waitgroup_add(&wg, 1);
  waitgroup_done(&wg);
  waitgroup_wait(&wg); // returns instead of hanging
}

// ---------------------------------------------------------------- channels

TEST(chan_typed_spelling) {
  Chan(int) ch = chan_new(int, 2);
  chan_send(ch, 41);
  chan_send(ch, 42);
  ASSERT_EQ(chan_len(ch), 2);
  ASSERT_FALSE(chan_try_send(ch, 43)); // full
  i32 a = 0, b = 0;
  ASSERT_TRUE(chan_recv(ch, &a));
  ASSERT_TRUE(chan_recv(ch, &b));
  ASSERT_EQ(a + b, 83);
  chan_close(ch);
  ASSERT_FALSE(chan_recv(ch, &a)); // closed and drained
}

// A channel of strings read into an int is a mistake the typed macro can name.
TEST(chan_wrong_element_size_panics) {
  Chan(int) ch = chan_new(int, 1);
  ASSERT_PANICS({ i32 v = 7; wchan_send_typed(ch, &v, sizeof(str)); });
  chan_close(ch);
}

// ---------------------------------------------------------------- app state

TEST(app_state_set_get) {
  app_state_clear();
  static int db = 7;
  app_state_set("db", &db);
  ASSERT_PTR_EQ(app_state_get("db"), &db);
  ASSERT_NULL(app_state_get("missing"));
  app_state_set("db", nullptr); // null removes
  ASSERT_NULL(app_state_get("db"));
}

// ---------------------------------------------------------------- extractors

POST("/spec_api/deposit") {
  Deposit d = body_json(Deposit);
  ASSERT_EQ(d.account, 3);
  return json(200, Deposit, &d);
}

POST("/spec_api/deposit_form") {
  Deposit d = body_form(Deposit);
  return json(200, Deposit, &d);
}

TEST(body_json_extractor) {
  Response r = webc_test_request_ex("POST", "/spec_api/deposit", "Content-Type: application/json",
                                    "{\"account\":3,\"cents\":500}", (i32)strlen("{\"account\":3,\"cents\":500}"));
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_CONTAINS(str_lit(r.body.p), "\"cents\":500");
}

// Garbage in is the caller's mistake, so it is a 400 with the parser's own
// message — not the 500 an unfixed panic guard would produce.
TEST(body_json_bad_input_is_400) {
  Response r = webc_test_request_ex("POST", "/spec_api/deposit", "Content-Type: application/json",
                                    "{\"account\":", (i32)strlen("{\"account\":"));
  ASSERT_EQ(r.status, 400);
  ASSERT_STR_CONTAINS(str_lit(r.body.p), "\"invalid_body\"");
}

TEST(body_form_extractor) {
  Response r = webc_test_request_ex("POST", "/spec_api/deposit_form",
                                    "Content-Type: application/x-www-form-urlencoded",
                                    "account=4&cents=250", (i32)strlen("account=4&cents=250"));
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_CONTAINS(str_lit(r.body.p), "\"account\":4");
}

// Validation failure (min = 1) keeps the 422 the model layer asks for.
TEST(body_form_validation_is_422) {
  Response r = webc_test_request_ex("POST", "/spec_api/deposit_form",
                                    "Content-Type: application/x-www-form-urlencoded",
                                    "account=4&cents=0", (i32)strlen("account=4&cents=0"));
  ASSERT_EQ(r.status, 422);
  // and it is the *validator* that refused, not the parser: a body that failed to
  // parse would also be 422 and this test would pass while proving nothing
  ASSERT_STR_CONTAINS(str_lit(r.body.p), "cents");
  ASSERT_STR_CONTAINS(str_lit(r.body.p), "invalid_form");
}

// ---------------------------------------------------------------- headers

GET("/spec_api/headers") {
  i64 n = header_i64("x-count");
  i64 m = header_i64_or("x-missing", -1);
  return res_text(200, str_fmt(webc_req_current()->arena, "%lld %lld", (long long)n, (long long)m));
}

TEST(header_i64) {
  Response r =
      webc_test_request_ex("GET", "/spec_api/headers", "X-Count: 42", nullptr, 0);
  ASSERT_STR_EQ(str_lit(r.body.p), "42 -1");
}

// ---------------------------------------------------------------- contexts

static void cancel_after_20ms(void *arg) {
  wg_sleep_ms(20);
  ctx_cancel(arg);
}

static void hook_bump(void *arg) {
  static _Atomic int *unused = nullptr;
  (void)unused;
  atomic_fetch_add((_Atomic int *)arg, 1);
}

TEST(ctx_cancel_and_done) {
  WCtx *c = ctx_new(nullptr);
  ASSERT_FALSE(ctx_done(c));
  ctx_cancel(c);
  ASSERT_TRUE(ctx_done(c));
  ctx_cancel(c); // idempotent
  ctx_free(c);
}

TEST(ctx_child_dies_with_parent) {
  WCtx *p = ctx_new(nullptr);
  WCtx *k = ctx_new(p);
  ASSERT_FALSE(ctx_done(k));
  ctx_cancel(p);
  ASSERT_TRUE(ctx_done(k));
  ASSERT_STR_EQ(ctx_err(k), "cancelled");
  ctx_free(k);
  ctx_free(p);
}

TEST(ctx_wait_wakes_on_cancel) {
  WCtx *c = ctx_new(nullptr);
  task_async(cancel_after_20ms, c);
  u64 t0 = wc_time_ms();
  bool done = ctx_wait(c, 2000);
  u64 took = wc_time_ms() - t0;
  ASSERT_TRUE(done);
  ASSERT(took < 1000); // woken by the cancel, not by the timeout
  ctx_free(c);
}

TEST(ctx_on_cancel_hooks_run_once) {
  static _Atomic int runs;
  WCtx *c = ctx_new(nullptr);
  ctx_on_cancel(c, hook_bump, &runs);
  ctx_cancel(c);
  ctx_free(c);
  ASSERT_EQ(runs, 1);
}

// A context handed to a background task stops it: this is the shape a handler
// uses when the client hangs up.
typedef struct {
  WCtx *ctx;
  _Atomic int ticks;
} Worker;

static void ticker(void *arg) {
  Worker *w = arg;
  for (i32 i = 0; i < 10'000 && !ctx_done(w->ctx); i++) {
    atomic_fetch_add(&w->ticks, 1);
    wg_sleep_ms(1);
  }
}

TEST(ctx_stops_a_background_task) {
  WCtx *c = ctx_new(nullptr);
  Worker w = {.ctx = c};
  task_async(ticker, &w);
  wg_sleep_ms(10);
  ctx_cancel(c);
  wg_sleep_ms(10);
  i32 before = atomic_load(&w.ticks);
  wg_sleep_ms(30);
  ASSERT_EQ(atomic_load(&w.ticks), before); // it stopped
  ASSERT(before < 10'000);
  ctx_free(c);
}

// ---------------------------------------------------------------- CSP nonce

static void page_view(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("h1") {
    text("fixture page");
  }
}

// The nonce lives in the request arena, which the next test request resets, so
// the handler copies it out before returning.
static char g_nonce[64];

GET("/spec_api/page") {
  Request *r = webc_req_current();
  snprintf(g_nonce, sizeof g_nonce, "%s", r->nonce.p ? r->nonce.p : "");
  return wc_render_page(200, "tests/fixtures/page.html", page_view, nullptr);
}

TEST(ssr_page_carries_a_nonce) {
  Response r = webc_test_request("GET", "/spec_api/page");
  ASSERT_EQ(r.status, 200);
  ASSERT(strlen(g_nonce) >= 16);
  str body = str_lit(r.body.p);
  // the inline bootstrap is the one that must survive the default CSP
  ASSERT_STR_CONTAINS(body, "<script nonce=\"");
  str want = str_fmt(nullptr, "nonce=\"%s\"", g_nonce);
  ASSERT(str_contains(body, want));
  wm_free(want.p);
  // nothing is double-marked
  ASSERT(!str_contains(body, str_lit("nonce=\" nonce=")));
}

// The default security headers name that nonce, so the bootstrap is *allowed*
// while any other inline script stays blocked.
TEST(csp_header_names_the_nonce) {
  Response r = webc_test_request("GET", "/spec_api/page");
  str csp = {0};
  for (i32 i = 0; i < r.nheaders; i++)
    if (r.headers[i].name.n == sizeof "content-security-policy" - 1 &&
        strncasecmp(r.headers[i].name.p, "content-security-policy",
                    sizeof "content-security-policy" - 1) == 0)
      csp = r.headers[i].value;
  ASSERT(csp.n > 0);
  ASSERT(str_contains(csp, str_lit("script-src 'self'")));
  str want = str_fmt(nullptr, "'nonce-%s'", g_nonce);
  ASSERT(str_contains(csp, want));
  wm_free(want.p);
}

// An app installs this in main(); the test installs it the same way, so the header
// path under test is the production one.
static void install_security_headers(void) {
  webc_use(mw_headers_default());
}
TEST_SETUP(install_security_headers);

TEST_MAIN()
// ---------------------------------------------------------------- route groups

static Response api_me(Request *req) { return res_text(200, str_lit("me")); }
static Response api_plain(Request *req) { return res_text(200, str_lit("plain")); }

static bool tag_added(Request *req, void *ctx, WNext *next) {
  (void)req;
  (void)ctx;
  if (!wc_next_run(next)) return false;
  res_header(next->res, "x-grouped", "1");
  return true;
}

TEST(route_groups_prefix_and_scope_middleware) {
  webc_use_reset();
  // a named variable, not a compound literal: `use({...})` would split on the commas
  WMiddleware tag = {.name = "tag", .fn = tag_added, .ctx = nullptr};
  group("/api/v1") {
    use(tag);
    webc_route_add("GET", "/me", api_me, nullptr, 0);
    group("/admin") { // nested: /api/v1/admin/plain
      webc_route_add("GET", "/plain", api_plain, nullptr, 0);
    }
  }
  // the prefix is gone again: a route added outside the group is not prefixed
  webc_route_add("GET", "/bare", api_plain, nullptr, 0);

  Response r = webc_test_request("GET", "/api/v1/me");
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_EQ(str_lit(r.body.p), "me");
  bool tagged = false;
  for (i32 i = 0; i < r.nheaders; i++)
    if (str_eq_c(r.headers[i].name, "x-grouped")) tagged = true;
  ASSERT_TRUE(tagged); // the group's `use()` applied to the route inside it

  Response nested = webc_test_request("GET", "/api/v1/admin/plain");
  ASSERT_EQ(nested.status, 200);

  Response bare = webc_test_request("GET", "/bare");
  ASSERT_EQ(bare.status, 200);
  bool bare_tagged = false;
  for (i32 i = 0; i < bare.nheaders; i++)
    if (str_eq_c(bare.headers[i].name, "x-grouped")) bare_tagged = true;
  ASSERT_FALSE(bare_tagged); // ...and not to the one outside

  ASSERT_EQ(webc_test_request("GET", "/me").status, 404);
  webc_use_reset();
}

// ---------------------------------------------------------------- config

WEBC_MODEL(CfgProbe, (port, i32, .doc = "listen port"), (database_url, str, .required = true),
           (debug, bool), (ratio, f64))

static void write_text(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fwrite(text, 1, strlen(text), f);
  fclose(f);
}

TEST(config_env_beats_the_file_beats_the_default) {
  write_text("/tmp/webc-cfg.json",
             "{\"defaults\":{\"port\":1234,\"ratio\":0.5},\"production\":{\"port\":8080}}");
  unsetenv("WEBC_PORT");
  unsetenv("DATABASE_URL");
  Arena *a = arena_new(8192);

  // file only: the environment section wins over defaults, and env vars win over both
  CfgProbe from_file = {0};
  ASSERT_OK(config_load_model(a, CfgProbe_modelof(), &from_file, "/tmp/webc-cfg.json",
                              "production"));
  ASSERT_EQ(from_file.port, 8080);
  ASSERT_EQ(from_file.ratio, 0.5);

  setenv("WEBC_PORT", "9090", 1);
  CfgProbe from_env = {0};
  ASSERT_OK(
      config_load_model(a, CfgProbe_modelof(), &from_env, "/tmp/webc-cfg.json", "production"));
  ASSERT_EQ(from_env.port, 9090);

  // a value that does not fit its field is an error naming the field, not a zero
  setenv("WEBC_PORT", "eighty", 1);
  WErrRes bad = config_load_model(a, CfgProbe_modelof(), &from_env, nullptr, nullptr);
  ASSERT_FALSE(bad.ok);
  ASSERT(str_contains(werr_msg(&bad.e), str_lit("port")));

  // no file, no env: defaults only, and a required field stays empty for the
  // validators to complain about rather than being invented
  unsetenv("WEBC_PORT");
  CfgProbe empty = {0};
  ASSERT_OK(config_load_model(a, CfgProbe_modelof(), &empty, nullptr, nullptr));
  ASSERT_EQ(empty.port, 0);
  ASSERT_EQ(empty.database_url.n, 0);
  arena_destroy(a);
}

TEST(config_schema_lists_fields_and_env_names) {
  Arena *a = arena_new(8192);
  str doc = config_schema_doc(a, CfgProbe_modelof());
  ASSERT(str_contains(doc, str_lit("CfgProbe")));
  ASSERT(str_contains(doc, str_lit("WEBC_PORT")));
  ASSERT(str_contains(doc, str_lit("listen port")));
  ASSERT(str_contains(doc, str_lit("required")));
  arena_destroy(a);
}
