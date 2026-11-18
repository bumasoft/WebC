// the test client: a jar, a form encoder, and the assertions a mutation test needs.
//
// The routes here are deliberately small. What is under test is the *client*: that a cookie
// the server set comes back on the next request, that a model becomes the form the framework
// parses, and that "the row it should have written" is one line.
#include <webc/model.h>
#include <webc/test.h>

#include "test_db.h"

WEBC_MODEL(BlogPost,
           (id, i64, .pk = true),
           (slug, Text, .required = true, .unique = true),
           (title, Text, .required = true),
           (views, i64),
           (featured, bool),
           (tags, VecStr))

static WDB *g_db;

// POST /client/log-in: sets a session cookie and redirects, the way every app's log-in does.
static Response log_in(Request *req) {
  (void)req;
  Response r = res_redirect(302, "/client/home");
  res_header(&r, "Set-Cookie", "session=abc123; Path=/; HttpOnly; SameSite=Lax");
  return r;
}

// GET /client/home: asks for the cookie, and says no when it is missing.
static Response home(Request *req) {
  str c = req_header(req, "cookie");
  if (!str_contains(c, str_lit("session=abc123")))
    return res_text(401, str_lit("signed out"));
  return res_text(200, str_lit("signed in"));
}

// POST /client/posts: a form body into the model, then into the table.
static Response create_post(Request *req) {
  BlogPost p = {0};
  WModelParse r = model_from_form(wc_arena_get(), BlogPost_modelof(),
                                  (str){req->body.p, req->body.n}, &p);
  if (r.ok) { // and the rules the model carries: `.required`, `.max`, an empty string
    WErrs errs = {0};
    if (!model_validate(BlogPost_modelof(), &p, &errs)) {
      r.ok = false;
      r.errs = errs;
    }
  }
  if (!r.ok) { // the errors as data: a field and the sentence about it
    Buf b;
    buf_init(&b, wc_arena_get(), 128);
    buf_puts(&b, "[");
    for (i32 i = 0; i < r.errs.n; i++)
      buf_printf(&b, "%s{\"field\":\"%.*s\"}", i ? "," : "", (int)r.errs.items[i].field.n,
                 r.errs.items[i].field.p);
    buf_puts(&b, "]");
    return res_bytes(422, "application/json", b.p, b.n);
  }
  if (!db_save(BlogPost, &p)) return res_empty(500);
  return res_redirect(303, "/client/posts");
}

static void routes(void) {
  webc_router_reset();
  webc_use_reset();
  webc_route_add("POST", "/client/log-in", log_in, nullptr, 0);
  webc_route_add("GET", "/client/home", home, nullptr, 0);
  webc_route_add("POST", "/client/posts", create_post, nullptr, 0);
}

static void schema(void) {
  if (g_db) {
    db_exec_sql(g_db, "delete from blog_posts"); // every test starts from an empty table
    return;
  }
  g_db = wc_test_open();
  char sql[1024];
  snprintf(sql, sizeof sql,
           "create table blog_posts (id %s, slug %s, title %s, views integer, featured integer, "
           "tags %s)",
           wc_test_pk(g_db), wc_test_text(g_db), wc_test_text(g_db), wc_test_text(g_db));
  wc_test_drop(g_db, "blog_posts");
  db_exec_sql(g_db, sql);
}

// ---------------------------------------------------------------- the jar

TEST(a_cookie_the_server_set_comes_back_on_the_next_request) {
  schema();
  routes();
  TestClient c = test_client();
  Response r = tc_send(c, "POST", "/client/log-in");
  ASSERT_EQ(r.status, 302);
  ASSERT_HEADER(r, "set-cookie", "session=abc123; Path=/; HttpOnly; SameSite=Lax");
  ASSERT_STR_CONTAINS(buf_str(&c.jar), "session=abc123"); // the attributes stayed behind

  Response next = tc_get(c, "/client/home");
  ASSERT_EQ(next.status, 200);
  ASSERT_STR_CONTAINS(next, "signed in");
  webc_router_reset();
  webc_use_reset();
}

TEST(a_second_client_is_a_second_browser) {
  schema();
  routes();
  TestClient alice = test_client();
  TestClient bob = test_client();
  tc_send(alice, "POST", "/client/log-in");
  Response r = tc_get(bob, "/client/home");
  ASSERT_EQ(r.status, 401); // bob never logged in, and alice's cookie is not his
  ASSERT_STR_CONTAINS(r, "signed out");
  webc_router_reset();
  webc_use_reset();
}

TEST(an_expired_cookie_is_a_delete) {
  schema();
  routes();
  TestClient c = test_client();
  wc_tc_keep_cookie(&c, str_lit("session=abc123; Path=/"));
  ASSERT_STR_CONTAINS(buf_str(&c.jar), "session=abc123");
  wc_tc_keep_cookie(&c, str_lit("session=; Max-Age=0; Path=/"));
  ASSERT_TRUE(strstr(wc_test_str(buf_str(&c.jar)), "session") == nullptr);
}

// ---------------------------------------------------------------- the form encoder

TEST(a_model_becomes_the_form_the_framework_parses) {
  schema();
  BlogPost p = {.slug = "a b&c", .title = "T+1", .views = 7, .featured = true};
  TestClient c = test_client();
  str body = wc_tc_form_body(&c, BlogPost_modelof(), &p);
  // the id is not posted back on a create; the space and the & are escaped, not quoted
  ASSERT_STR_EQ(body, "slug=a+b%26c&title=T%2B1&views=7&featured=on&tags=");
}

TEST(a_posted_form_lands_in_the_table) {
  schema();
  routes();
  TestClient c = test_client();
  BlogPost p = {.slug = "the-last-ferry", .title = "The last ferry", .views = 42};
  Response r = tc_post_form_at(c, "/client/posts", BlogPost, &p);
  ASSERT_REDIRECT(r, "/client/posts");
  ASSERT_DB_COUNT(BlogPost, 1);
  ASSERT_DB_ONE(BlogPost, slug, str_lit("the-last-ferry"));
  webc_router_reset();
  webc_use_reset();
}

TEST(a_body_the_model_refuses_is_a_422_and_writes_nothing) {
  schema();
  routes();
  TestClient c = test_client();
  Response r = tc_post_body(c, "/client/posts", str_lit("slug=&title="));
  ASSERT_EQ(r.status, 422);
  ASSERT_JSON_STR(r, "0.field", "slug");
  ASSERT_DB_COUNT(BlogPost, 0);
  webc_router_reset();
  webc_use_reset();
}

TEST(the_row_assertions_say_what_is_missing) {
  schema();
  BlogPost p = {.slug = "only-one", .title = "One"};
  ASSERT_TRUE(db_save(BlogPost, &p));
  ASSERT_DB_COUNT(BlogPost, 1);
  ASSERT_DB_ONE_I64(BlogPost, id, p.id);
  ASSERT_DB_ONE(BlogPost, slug, str_lit("only-one"));
}

// ---------------------------------------------------------------- the header helpers

TEST(a_header_name_is_case_insensitive) {
  schema();
  routes();
  TestClient c = test_client();
  Response r = tc_send(c, "POST", "/client/log-in");
  // the handler wrote "Set-Cookie"; the test asks in lower case, as RFC 9110 says it may
  ASSERT_STR_CONTAINS(wc_test_res_header(r, "set-cookie"), "session=abc123");
  ASSERT_STR_CONTAINS(wc_test_res_header(r, "SET-COOKIE"), "session=abc123");
  ASSERT_HEADER(r, "set-cookie", "session=abc123; Path=/; HttpOnly; SameSite=Lax");
  webc_router_reset();
  webc_use_reset();
}

TEST(redirects_are_asserted_whole) {
  schema();
  routes();
  TestClient c = test_client();
  Response r = tc_send(c, "POST", "/client/log-in");
  ASSERT_REDIRECT(r, "/client/home");
  webc_router_reset();
  webc_use_reset();
}

TEST_MAIN()