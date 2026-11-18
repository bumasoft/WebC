// form_body: the no-JavaScript form and the module have to agree about where a value lives.
//
// `el("form") { post_to("/theme"); el("input") { name("to") } }` is the documented
// progressive-enhancement pattern, and the handler reads `query("to")`. A reader that looks
// only at the query string sees nothing from a form post: the documented example does
// nothing, and the working version builds the query string by hand — in both halves,
// identically.
#include <webc/test.h>

static str g_seen;
static bool g_hit;

static Response theme(Request *req) {
  (void)req;
  g_hit = true;
  g_seen = query("to");
  return res_text(200, str_lit("ok"));
}

static void routes(void) {
  webc_router_reset();
  webc_use_reset();
  webc_route_add("POST", "/theme", theme, nullptr, 0);
}

TEST(a_form_post_field_is_where_query_finds_it) {
  routes();
  g_hit = false;
  g_seen = (str){0};
  const char *body = "to=dark&back=%2Fsettings";
  Response r = webc_test_request_ex("POST", "/theme",
                                    "content-type: application/x-www-form-urlencoded\r\n", body,
                                    strlen(body));
  webc_use_reset();
  webc_router_reset();
  ASSERT_EQ(r.status, 200);
  ASSERT_TRUE(g_hit);
  ASSERT_STR_EQ(g_seen, "dark"); // and URL-decoded, since the next field is
}

TEST(the_query_string_still_wins_over_the_body) {
  routes();
  g_seen = (str){0};
  const char *body = "to=dark";
  webc_test_request_ex("POST", "/theme?to=light",
                       "content-type: application/x-www-form-urlencoded\r\n", body, strlen(body));
  webc_use_reset();
  webc_router_reset();
  ASSERT_STR_EQ(g_seen, "light");
}

TEST(a_body_field_is_absent_when_it_is_not_there) {
  routes();
  g_seen = str_lit("untouched");
  const char *body = "other=1";
  webc_test_request_ex("POST", "/theme",
                       "content-type: application/x-www-form-urlencoded\r\n", body, strlen(body));
  webc_use_reset();
  webc_router_reset();
  ASSERT_EQ((int)g_seen.n, 0); // query() returns empty, not the previous value
}

TEST(a_json_body_is_not_a_form) {
  routes();
  g_seen = str_lit("untouched");
  const char *body = "{\"to\":\"dark\"}";
  webc_test_request_ex("POST", "/theme", "content-type: application/json\r\n", body, strlen(body));
  webc_use_reset();
  webc_router_reset();
  ASSERT_EQ((int)g_seen.n, 0);
}

TEST_MAIN()