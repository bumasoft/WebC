// The todo app's own suite: real routes, a real (in-memory) database, no socket.
//
//   webc test
//
// The framework's main() boots the same way (`webc_boot()`, the default database,
// migrations), so the tests cannot drift away from how the server actually starts.
#include <webc/test.h>

static void boot(void) {
  // sqlite by default; `WEBC_TEST_DB=postgres://… webc test` runs the same tests
  // against a live postgres, which is the only way to know the app is portable.
  setenv("DATABASE_URL", getenv("WEBC_TEST_DB") ? getenv("WEBC_TEST_DB") : "sqlite::memory:", 1);
  webc_boot();          // the app's half (none) — the handle opens on first use
  db_default();         // open it now, so migrations run before the first request
}
TEST_SETUP(boot);

// how many todos the list currently has, through the JSON route
static i32 todo_count(void) {
  Response r = webc_test_request("GET", "/api/todos");
  return wc_json_count_at(wc_test_view_buf(r.body), "");
}

// the id of the newest todo whose title contains `needle`, or 0
static i64 find_todo(const char *needle) {
  JVal *rows = wc_test_json(webc_test_request("GET", "/api/todos"));
  for (i32 i = 0; rows && i < rows->n; i++) {
    JVal *title = json_get(json_at(rows, i), "title");
    if (title && str_contains(title->s, str_lit(needle))) return json_get(json_at(rows, i), "id")->inum;
  }
  return 0;
}

TEST(health_is_not_needed_but_the_list_answers) {
  Response r = webc_test_request("GET", "/api/todos");
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_EQ(r.body, "[]");
}

TEST(create_then_list) {
  Response created = webc_test_request_ex("POST", "/api/todos", "Content-Type: application/json",
                                          "{\"title\":\"ship it\"}", strlen("{\"title\":\"ship it\"}"));
  ASSERT_EQ(created.status, 201);
  ASSERT_JSON(created, "title", "ship it");          // the row it created, by path

  Response list = webc_test_request("GET", "/api/todos");
  ASSERT_EQ(list.status, 200);
  ASSERT_JSON_COUNT(list, "", 1);
  ASSERT_JSON(list, "0.title", "ship it");
}

TEST(a_form_post_redirects_to_the_page) {
  const char *form = "title=write+the+docs";
  Response r = webc_test_request_ex("POST", "/api/todos", "Content-Type: application/x-www-form-urlencoded",
                                    form, strlen(form));
  ASSERT_EQ(r.status, 303);
  ASSERT_EQ(r.body.n, 0);   // a redirect carries no body
}

TEST(an_empty_title_is_rejected) {
  Response r = webc_test_request_ex("POST", "/api/todos", "Content-Type: application/json",
                                    "{\"title\":\"\"}", strlen("{\"title\":\"\"}"));
  ASSERT_EQ(r.status, 422);
  ASSERT_STR_CONTAINS(r.body, "required");
}

TEST(toggling_flips_done) {
  ASSERT_EQ(webc_test_request_ex("POST", "/api/todos", "Content-Type: application/json",
                                 "{\"title\":\"toggle me\"}", strlen("{\"title\":\"toggle me\"}"))
                .status,
            201);
  i64 id = find_todo("toggle me");
  ASSERT_GT(id, 0);

  char path[64];
  snprintf(path, sizeof path, "/api/todos/%lld", (long long)id);
  Response on = webc_test_request("PATCH", path);
  ASSERT_EQ(on.status, 200);
  ASSERT_JSON(on, "done", "true");
  Response off = webc_test_request("PATCH", path);
  ASSERT_JSON(off, "done", "false");
}

TEST(deleting_removes_it) {
  ASSERT_EQ(webc_test_request_ex("POST", "/api/todos", "Content-Type: application/json",
                                 "{\"title\":\"delete me\"}", strlen("{\"title\":\"delete me\"}"))
                .status,
            201);
  i64 id = find_todo("delete me");
  ASSERT_GT(id, 0);

  i32 before = todo_count();
  char path[64];
  snprintf(path, sizeof path, "/api/todos/%lld", (long long)id);
  ASSERT_EQ(webc_test_request("DELETE", path).status, 204);
  ASSERT_EQ(todo_count(), before - 1);
}

TEST(missing_id_is_404_not_a_crash) {
  ASSERT_EQ(webc_test_request("PATCH", "/api/todos/999999").status, 404);
}

TEST(the_page_is_server_rendered) {
  Response r = webc_test_request("GET", "/");
  ASSERT_EQ(r.status, 200);
  ASSERT_HTML(r, "main > h1");                    // the page, as a document
  ASSERT_HTML_TEXT(r, "main > h1", "Todos");
  ASSERT_HTML(r, "[data-wc]");                    // SSR markup carries the anchors
}

// The row's toggle is a form post, so the whole list works with JavaScript off. This
// is the half of the UI a browser demo actually exercises, and it is reachable here
// because the test runs the same router the server does.
TEST(toggling_from_the_page_needs_no_javascript) {
  Response created = webc_test_request_ex("POST", "/api/todos", "Content-Type: application/json",
                                          "{\"title\":\"toggle me\"}", (i32)strlen("{\"title\":\"toggle me\"}"));
  ASSERT_EQ(created.status, 201);
  i64 id = find_todo("toggle me");
  ASSERT_GT(id, 0);
  i32 rows = todo_count();
  ASSERT_GE(rows, 1);

  Response page = webc_test_request("GET", "/");
  ASSERT_HTML_COUNT(page, "ul.todos li", rows);           // one <li> per row, no more
  ASSERT_HTML_COUNT(page, "form[action=\"/todos/toggle\"]", rows);  // a toggle form per row ...
  ASSERT_HTML_COUNT(page, "form[action=\"/todos/delete\"]", rows);  // ... a delete form per row
  ASSERT_HTML(page, "title");                             // and a whole document

  char form[32];
  snprintf(form, sizeof form, "id=%lld", (long long)id);
  Response toggled =
      webc_test_request_ex("POST", "/todos/toggle", "Content-Type: application/x-www-form-urlencoded",
                           form, (i32)strlen(form));
  ASSERT_EQ(toggled.status, 303);
}

TEST_MAIN()