// client_test.c — the HTTP client against a real socket (SPEC §8).
//
// The client is the only part of the framework that speaks to a server *we did not
// write*, so testing it against a stub would test our stub. This starts the real
// `webc_serve` on an ephemeral port in a background task and calls it over a real
// socket, which is also the only way the green-thread socket waits get exercised.
// The include-order rule: system networking headers first, then the select-statement
// macro is off for the rest of the file (sys/select.h's select() is not ours).
#include <unistd.h>

#define WEBC_NO_SELECT_MACRO 1
#include <webc/test.h>
#include <webc/server.h>
#include <webc/web.h>

GET("/client/hello") { return res_text(200, str_lit("hello")); }

GET("/client/echo") {
  str v = query("v");
  return res_text(200, str_fmt(req->arena, "%.*s", (int)v.n, v.p ? v.p : ""));
}

POST("/client/reflect") {
  return res_text(201, str_fmt(req->arena, "%.*s", (int)req->body.n, req->body.p));
}

GET("/client/hop") { return res_redirect(302, "/client/hello"); }

static void chunked_body(Response *r, void *ctx) {
  (void)ctx;
  webc_stream_write(r, str_lit("chunk-"));
  webc_stream_write(r, str_lit("by-chunk"));
}

GET("/client/chunked") {
  Response r = res_stream(200, chunked_body, nullptr);
  r.content_type = str_lit("text/plain");
  return r;
}

static i32 g_port;

static void serve_task(void *arg) {
  (void)arg;
  char addr[64];
  snprintf(addr, sizeof addr, "127.0.0.1:%d", g_port);
  webc_serve(&(WebcOptions){.addr = addr, .quiet = true, .cpus = 2});
}

static void boot(void) {
  webc_use_reset();
  wtask_init(2);
  g_port = 18080 + (i32)(getpid() % 500);
  task_async(serve_task, nullptr);
  // wait for the listener: a client that connects before bind() gets ECONNREFUSED,
  // which is a flaky test, not a client bug
  Arena *a = arena_new(4096);
  for (i32 i = 0; i < 100; i++) {
    WRespC r;
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/client/hello", g_port);
    if (http_get(a, url, &r).ok) break;
    arena_reset(a);
    wc_sleep_ms(20);
  }
  arena_destroy(a);
}
TEST_SETUP(boot);

static WRespC fetch(Arena *a, const char *path) {
  char url[128];
  snprintf(url, sizeof url, "http://127.0.0.1:%d%s", g_port, path);
  WRespC r = {0};
  WErrRes e = http_get(a, url, &r);
  ASSERT_TRUE(e.ok);
  return r;
}

TEST(client_gets_a_body_and_status) {
  Arena *a = arena_new(65536);
  WRespC r = fetch(a, "/client/hello");
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_EQ(r.body, "hello");
  ASSERT(str_contains(r.content_type, str_lit("text/plain")));
  arena_destroy(a);
}

TEST(client_sends_a_query_string) {
  Arena *a = arena_new(65536);
  WRespC r = fetch(a, "/client/echo?v=42");
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_EQ(r.body, "42");
  arena_destroy(a);
}

TEST(client_posts_a_body) {
  Arena *a = arena_new(65536);
  WRespC r = {0};
  char url[128];
  snprintf(url, sizeof url, "http://127.0.0.1:%d/client/reflect", g_port);
  WErrRes e = http_post(a, url, "text/plain", str_lit("ping"), &r);
  ASSERT_TRUE(e.ok);
  ASSERT_EQ(r.status, 201);
  ASSERT_STR_EQ(r.body, "ping");
  arena_destroy(a);
}

TEST(client_reads_a_chunked_body) {
  Arena *a = arena_new(65536);
  WRespC r = fetch(a, "/client/chunked");
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_EQ(r.body, "chunk-by-chunk");
  arena_destroy(a);
}

TEST(client_does_not_follow_redirects_unless_asked) {
  Arena *a = arena_new(65536);
  char url[128];
  snprintf(url, sizeof url, "http://127.0.0.1:%d/client/hop", g_port);
  WRespC r = {0};
  ASSERT_TRUE(http_get(a, url, &r).ok);
  ASSERT_EQ(r.status, 302);
  ASSERT_STR_EQ(r.location, "/client/hello");

  // and follows them when it is: the Location here is relative, which is the case
  // a naive client gets wrong
  WRespC followed = {0};
  WErrRes e = http_do_follow(a, (WClient){.method = "GET", .url = url}, 5, &followed);
  ASSERT_TRUE(e.ok);
  ASSERT_EQ(followed.status, 200);
  ASSERT_STR_EQ(followed.body, "hello");
  arena_destroy(a);
}

TEST(client_reports_an_unreachable_host_as_an_error) {
  Arena *a = arena_new(8192);
  WRespC r;
  WErrRes e = http_get(a, "http://127.0.0.1:1/nope", &r);
  ASSERT_FALSE(e.ok);
  ASSERT(e.e.len > 0);
  arena_destroy(a);
}

TEST_MAIN()
