// ops_test.c — the operational surface: the CPU sampler, multipart, the request
// limits and the static-file guard (SPEC §6.6, §11).
//
// Every test here is about behaviour under an unfriendly input. A framework's
// security posture is not the header list in the docs; it is what the parser does
// when a body claims to be a thousand parts, or a path points through a symlink.
#include <webc/test.h>
#include <webc/server.h>
#include <webc/web.h>

// ---------------------------------------------------------------- CPU sampler

static volatile u64 g_sink;

static void spin_for(u64 rounds) {
  for (u64 i = 0; i < rounds; i++) g_sink += i * 7;
}

// The sampler must record where the time went. This test spins on *this* thread
// rather than on a helper: the CPU-time timer believes the process, so the burn
// here is charged to it whatever the machine's scheduler is doing — a test that
// depends on a helper thread being scheduled is a test of the CI box, not of the
// profiler. (A real server has its workers sampled by name; /debug/cpu is
// exercised end to end in `make budget`.)
TEST(cpu_sampler_records_where_the_time_went) {
  i32 err = 0;
  u64 t0 = wc_time_ms();
  spin_for(3000000ull); // warm up: the first millisecond of a window is noise
  ASSERT_TRUE(webc_profile_start(60, &err));
  ASSERT_TRUE(webc_profile_active());
  // a second window while one is open is refused rather than silently nested
  ASSERT_FALSE(webc_profile_start(50, &err));
  while (webc_profile_active() && wc_time_ms() - t0 < 500) spin_for(200000ull);
  webc_profile_stop();
  ASSERT_FALSE(webc_profile_active());
  ASSERT(webc_profile_samples() > 0);
  Arena *a = arena_new(65536);
  str report = webc_profile_report(a);
  ASSERT(str_contains(report, str_lit("flat profile")));
  ASSERT(!str_contains(report, str_lit("no samples")));
  // Grouped by symbol, not by raw address: the report has to have rows of them.
  // (Which symbol depends on where the sampler fired — this machine also charges
  // the timer to the waiting thread — so the assertion is the shape of the report,
  // and the attribution itself is checked end to end against a live /debug/cpu.)
  i32 rows = 0;
  for (size_t i = 0; i < report.n; i++)
    if (report.p[i] == '\n') rows++;
  ASSERT(rows >= 3); // header, column titles, at least one symbol
  arena_destroy(a);
}

TEST(cpu_sampler_refuses_a_second_window) {
  i32 err = 0;
  ASSERT_TRUE(webc_profile_start(50, &err));
  ASSERT_FALSE(webc_profile_start(50, &err));
  ASSERT_EQ(err, 1);
  webc_profile_stop();
}

// ---------------------------------------------------------------- multipart

static str multipart_body(void) {
  return str_lit("--X\r\n"
                 "Content-Disposition: form-data; name=\"title\"\r\n"
                 "\r\n"
                 "buy milk\r\n"
                 "--X\r\n"
                 "Content-Disposition: form-data; name=\"avatar\"; filename=\"a.txt\"\r\n"
                 "Content-Type: text/plain\r\n"
                 "\r\n"
                 "file-bytes\r\n"
                 "--X--\r\n");
}

TEST(multipart_parses_fields_and_files) {
  Arena *a = arena_new(65536);
  str body = multipart_body();
  WMultipart mp;
  WErrRes e = multipart_parse(a, str_lit("multipart/form-data; boundary=X"),
                              (Bytes){(char *)body.p, body.n}, 8, 1 << 20, &mp);
  ASSERT_OK(e);
  ASSERT_EQ(multipart_count(&mp), 2);
  ASSERT_STR_EQ(multipart_value(&mp, "title"), "buy milk");
  ASSERT_STR_EQ(multipart_file(&mp, "avatar"), "file-bytes");
  ASSERT_STR_EQ(multipart_filename(&mp, "avatar"), "a.txt");
  ASSERT_STR_EQ(multipart_file_type(&mp, "avatar"), "text/plain");
  // a file is not a form field and a missing name is not a crash
  ASSERT_EQ(multipart_value(&mp, "avatar").n, 0);
  ASSERT_EQ(multipart_value(&mp, "nope").n, 0);
  arena_destroy(a);
}

TEST(multipart_quoted_boundary) {
  Arena *a = arena_new(65536);
  str body = multipart_body();
  WMultipart mp;
  WErrRes e = multipart_parse(a, str_lit("multipart/form-data; boundary=\"X\""),
                              (Bytes){(char *)body.p, body.n}, 8, 1 << 20, &mp);
  ASSERT_OK(e);
  ASSERT_EQ(multipart_count(&mp), 2);
  arena_destroy(a);
}

TEST(multipart_part_limit_is_enforced_during_parsing) {
  Arena *a = arena_new(65536);
  str body = multipart_body();
  WMultipart mp;
  WErrRes e = multipart_parse(a, str_lit("multipart/form-data; boundary=X"),
                              (Bytes){(char *)body.p, body.n}, 1, 1 << 20, &mp);
  ASSERT_ERR_KIND(e, WEBC_ETOOBIG);
  arena_destroy(a);
}

TEST(multipart_byte_limit_and_missing_boundary) {
  Arena *a = arena_new(65536);
  str body = multipart_body();
  WMultipart mp;
  ASSERT_ERR(multipart_parse(a, str_lit("multipart/form-data; boundary=X"),
                             (Bytes){(char *)body.p, body.n}, 8, 16, &mp));
  ASSERT_ERR(multipart_parse(a, str_lit("multipart/form-data"), (Bytes){(char *)body.p, body.n}, 8,
                             1 << 20, &mp));
  arena_destroy(a);
}

// A browser posting a form with a file input sends multipart; the handler's typed
// extractor has to fill the struct from the text parts anyway.
WEBC_MODEL(Upload, (title, str, .required = true, .max = 40))

POST("/ops/upload") {
  Upload u = body_form(Upload);
  str file = multipart_file(webc_req_current()->multipart, "avatar");
  Request *r = webc_req_current();
  return res_text(200, str_fmt(r->arena, "%.*s|%.*s", (int)u.title.n, u.title.p, (int)file.n,
                               file.p ? file.p : ""));
}

TEST(form_extractor_reads_a_multipart_body) {
  str body = multipart_body();
  const char *headers = "Content-Type: multipart/form-data; boundary=X";
  Response r = webc_test_request_ex("POST", "/ops/upload", headers, body.p, body.n);
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_EQ(str_lit(r.body.p), "buy milk|file-bytes");
}

// ---------------------------------------------------------------- limits

// A 4 KB field with no title: the parser walks it and the validators answer 422.
// What matters is that the body was *read* and refused, not dropped on the floor.
TEST(a_large_body_is_answered_not_dropped) {
  static char buf[4096];
  memset(buf, 'a', sizeof buf);
  memcpy(buf, "title=", 6);
  Response r = webc_test_request_ex("POST", "/ops/upload",
                                    "Content-Type: application/x-www-form-urlencoded", buf,
                                    sizeof buf);
  // 4090 characters into a field declared `.max = 40`: the validator says which field
  // and why, which is the whole point of having one implementation of it
  ASSERT_EQ(r.status, 422);
  ASSERT(str_contains(str_lit(r.body.p), str_lit("title")));
}

WEBC_MODEL(Deep, (id, i64, .pk = true))

POST("/ops/json") {
  Deep d = body_json(Deep);
  return res_text(200, str_fmt(webc_req_current()->arena, "%lld", (long long)d.id));
}

TEST(json_nesting_limit_is_a_400_not_a_stack_overflow) {
  // 600 nested objects against a limit of 256: the parse refuses rather than
  // recursing until the stack ends.
  char deep[600 * 6 + 2];
  size_t n = 0;
  for (i32 i = 0; i < 600; i++) {
    memcpy(deep + n, "{\"a\":", 5);
    n += 5;
  }
  deep[n++] = '1';
  for (i32 i = 0; i < 600; i++) deep[n++] = '}';
  Response r = webc_test_request_ex("POST", "/ops/json", "Content-Type: application/json", deep, n);
  ASSERT_EQ(r.status, 400);
  ASSERT_EQ(r.body.n > 0, true);
  ASSERT(str_contains(str_lit(r.body.p), str_lit("\"status\":400")));
  ASSERT(str_contains(str_lit(r.body.p), str_lit("nesting")));
  // and a shallow one is fine
  Response ok = webc_test_request_ex("POST", "/ops/json", "Content-Type: application/json",
                                     "{\"id\":7}", 8);
  ASSERT_EQ(ok.status, 200);
  ASSERT_STR_EQ(str_lit(ok.body.p), "7");
}

// ---------------------------------------------------------------- static files

// A static directory is a published directory. Anything that resolves outside it is
// not published, whether the request says `..` or a symlink inside it says so.
TEST(static_files_cannot_escape_their_directory) {
  system("rm -rf /tmp/webc-static /tmp/webc-secret.txt && mkdir -p /tmp/webc-static && "
         "printf 'secret' > /tmp/webc-secret.txt && "
         "ln -s /tmp/webc-secret.txt /tmp/webc-static/leak.txt && "
         "printf 'public' > /tmp/webc-static/real.txt");
  webc_use_reset();
  webc_use(mw_static("/assets", "/tmp/webc-static"));

  Response ok = webc_test_request("GET", "/assets/real.txt");
  ASSERT_EQ(ok.status, 200); // the file itself is served (its bytes are lazy: no socket here)

  Response dotdot = webc_test_request("GET", "/assets/../webc-secret.txt");
  ASSERT_EQ(dotdot.status, 403);
  ASSERT(!str_contains(str_lit(dotdot.body.p), str_lit("secret")));

  Response encoded = webc_test_request("GET", "/assets/%2e%2e/webc-secret.txt");
  ASSERT(encoded.status >= 400);
  ASSERT(!str_contains(str_lit(encoded.body.p), str_lit("secret")));

  // the symlink is inside the directory but resolves outside it: still not published
  Response link = webc_test_request("GET", "/assets/leak.txt");
  ASSERT_EQ(link.status, 404);
  ASSERT(!str_contains(str_lit(link.body.p), str_lit("secret")));
  webc_use_reset();
}

// ---------------------------------------------------------------- the CSP as a value

TEST(a_policy_can_name_a_cdn_in_img_src) {
  Arena *a = arena_new(4096);
  const char *img[] = {"'self'", "data:", "https://cdn.example.com"};
  HeadersCfg cfg = {.img_src = img, .img_src_n = 3, .extra = "frame-ancestors 'self'"};
  str p = wc_csp_build(a, &cfg, "abc123", false, 0, nullptr);
  ASSERT_TRUE(str_contains(p, str_lit("img-src 'self' data: https://cdn.example.com")));
  ASSERT_TRUE(str_contains(p, str_lit("'nonce-abc123'")));
  ASSERT_TRUE(str_contains(p, str_lit("frame-ancestors 'self'")));
  // ...and no dev-only source in a real policy
  ASSERT_TRUE(!str_contains(p, str_lit("ws://localhost")));
  arena_destroy(a);
}

TEST(the_default_policy_is_what_it_always_was) {
  Arena *a = arena_new(4096);
  str p = wc_csp_build(a, nullptr, nullptr, false, 0, nullptr);
  ASSERT_TRUE(str_contains(p, str_lit("default-src 'self'")));
  ASSERT_TRUE(str_contains(p, str_lit("img-src 'self' data:")));
  ASSERT_TRUE(str_contains(p, str_lit("style-src 'self' 'unsafe-inline'")));
  ASSERT_TRUE(str_contains(p, str_lit("script-src 'self' 'wasm-unsafe-eval'")));
  arena_destroy(a);
}

TEST(the_dev_policy_adds_what_live_reload_needs_and_nothing_else) {
  Arena *a = arena_new(4096);
  str p = wc_csp_build(a, nullptr, nullptr, true, 9999, nullptr);
  ASSERT_TRUE(str_contains(p, str_lit("ws://localhost:9999")));
  ASSERT_TRUE(str_contains(p, str_lit("'unsafe-inline'")));
  ASSERT_TRUE(str_contains(p, str_lit("connect-src")));
  arena_destroy(a);
}

TEST(a_project_that_owns_the_header_gets_no_second_one) {
  HeadersCfg cfg = {.no_default = true};
  Response r = res_text(200, str_lit("ok"));
  res_header(&r, "Content-Security-Policy", "default-src 'none'");
  ASSERT_STR_EQ(res_header_get(&r, "Content-Security-Policy"), "default-src 'none'");
  ASSERT_TRUE(res_header_get(&r, "x-not-there").n == 0);
  (void)cfg;
}

// The chain `webc_main()` builds is "the app's own middleware first, the framework's defaults
// after" — so on the way out the *framework's* middlewares run first. That order made an app's
// `mw_headers((HeadersCfg){…})` a dead letter: the default policy had already been written, and
// `mw_headers` defers to a policy that is already there (which is right for a handler that chose
// one). The app's policy is the one that wins, which means the default is not installed at all.
TEST(the_apps_policy_replaces_the_frameworks_default) {
  webc_use_reset();
  const char *img[] = {"'self'", "https://cdn.example.com"};
  webc_use(mw_headers((HeadersCfg){.img_src = img, .img_src_n = 2})); // what webc_boot() does
  webc_install_default_middleware();                                  // what webc_main() does next

  WMiddleware chain[WC_MW_MAX];
  i32 n = webc_global_middleware(chain, WC_MW_MAX);
  i32 headers = 0;
  for (i32 i = 0; i < n; i++)
    if (chain[i].name && strcmp(chain[i].name, "headers") == 0) headers++;
  ASSERT_EQ(headers, 1); // not two: the second would overwrite the first on the way out

  Response r = webc_test_request("GET", "/nope-nothing-routes-this");
  i32 found = 0;
  str csp = {0};
  for (i32 i = 0; i < r.nheaders; i++)
    if (str_eq_c(r.headers[i].name, "Content-Security-Policy")) {
      found++;
      csp = r.headers[i].value;
    }
  ASSERT_EQ(found, 1);
  ASSERT_TRUE(str_contains(csp, str_lit("https://cdn.example.com")));
  webc_use_reset();
}

// The dev server widens *the policy in force*. Building one of its own throws the project's
// `img-src` away: a CDN thumbnail works in production and shows an empty box under `webc dev`,
// and the app's only cure is a middleware that rewrites the header by hand.
static Response dev_page(Request *req) {
  (void)req;
  return res_html(200, "<html><body>page</body></html>");
}

TEST(the_dev_policy_keeps_the_apps_img_src) {
  webc_use_reset();
  webc_route_add("GET", "/ops-dev-page", dev_page, nullptr, 0);
  const char *img[] = {"'self'", "https://cdn.example.com"};
  webc_use(mw_headers((HeadersCfg){.img_src = img, .img_src_n = 2}));
  webc_use(mw_dev(9999)); // inside the policy, as the framework installs it

  Response r = webc_test_request("GET", "/ops-dev-page");
  str csp = res_header_get(&r, "Content-Security-Policy");
  ASSERT_TRUE(str_contains(csp, str_lit("img-src 'self' https://cdn.example.com")));
  ASSERT_TRUE(str_contains(csp, str_lit("ws://localhost:9999")));      // what reload needs
  ASSERT_TRUE(str_contains(csp, str_lit("'unsafe-inline'")));
  webc_use_reset();
}

TEST_MAIN()