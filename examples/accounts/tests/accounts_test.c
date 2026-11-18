// The accounts app's own suite: signup, login, a session cookie, an authenticated
// page — through the router, with no socket and no browser.
//
// webc_boot() is the same function main() calls, with the secret pinned so the
// cookies the tests mint are the cookies the tests verify.
#include <webc/test.h>

static void boot(void) {
  // sqlite by default; `WEBC_TEST_DB=postgres://… webc test` runs the same tests
  // against a live postgres, which is the only way to know the app is portable.
  setenv("DATABASE_URL", getenv("WEBC_TEST_DB") ? getenv("WEBC_TEST_DB") : "sqlite::memory:", 1);
  setenv("WEBC_SECRET", "test-secret", 1);
  auth_secret_set(str_lit("test-secret"));   // the cookies the tests mint and verify
  webc_boot();
  db_default();
}
TEST_SETUP(boot);

// a "Cookie: session=..." header line built from a response's Set-Cookie
static const char *session_cookie(Response *res) {
  str cookie = str_lit("");
  for (i32 i = 0; i < res->nheaders; i++)
    if (str_eq_c(res->headers[i].name, "Set-Cookie")) cookie = res->headers[i].value;
  if (cookie.n == 0) return "";
  str value = str_sub(cookie, 8, cookie.n);   // "session=<value>; Path=/; ..."
  i64 semi = str_index(value, ';');
  value = semi > 0 ? str_sub(value, 0, (size_t)semi) : value;
  // the scope arena, not an arena of the test's own: this is the arena a request would
  // have used, and it is reset for the next test.
  return fmt("Cookie: session=%.*s", (int)value.n, value.p);
}

static Response signup(const char *email, const char *password) {
  char body[256];
  int n = snprintf(body, sizeof body, "email=%s&password=%s", email, password);
  return webc_test_request_ex("POST", "/signup", "Content-Type: application/x-www-form-urlencoded",
                              body, (size_t)n);
}

TEST(signup_creates_a_session) {
  Response r = signup("ada@example.com", "hunter2hunter2");
  ASSERT_EQ(r.status, 303);
  const char *cookie = session_cookie(&r);
  ASSERT_STR_CONTAINS(cookie, "session=");
}

TEST(an_authenticated_page_needs_the_cookie) {
  Response r = signup("grace@example.com", "hunter2hunter2");
  ASSERT_EQ(r.status, 303);
  const char *cookie = session_cookie(&r);
  ASSERT_STR_CONTAINS(cookie, "session=");

  // without it, /me sends you to the login page
  Response anon = webc_test_request("GET", "/me");
  ASSERT_EQ(anon.status, 303);
  bool to_login = false;
  for (i32 i = 0; i < anon.nheaders; i++)
    if (str_eq_c(anon.headers[i].name, "Location") &&
        str_eq_c(anon.headers[i].value, "/login"))
      to_login = true;
  ASSERT(to_login);

  // with it, /me renders the account
  Response me = webc_test_request_ex("GET", "/me", cookie, nullptr, 0);
  ASSERT_EQ(me.status, 200);
  ASSERT_HTML_TEXT(me, "main > h1", "Your account");
  ASSERT_HTML_TEXT(me, "main p.sub", "Signed in as grace@example.com.");
  // the page says who it is about, and the tab says which page it is
  ASSERT_HTML_TEXT(me, "head > title", "Your account — accounts");
}

TEST(a_duplicate_email_is_409) {
  ASSERT_EQ(signup("dup@example.com", "hunter2hunter2").status, 303);
  Response again = signup("dup@example.com", "hunter2hunter2");
  ASSERT_EQ(again.status, 409);
  ASSERT_HTML_TEXT(again, "main > p.error", "That email is already registered.");
  // the field the sentence is about is marked, and it still holds what was typed
  ASSERT_HTML_ATTR(again, "input[name=email]", "value", "dup@example.com");
  ASSERT_HTML_ATTR(again, "#email", "aria-invalid", "true");
}

TEST(a_short_password_is_422) {
  Response r = signup("short@example.com", "abc");
  ASSERT_EQ(r.status, 422);
}

TEST(login_verifies_the_password) {
  ASSERT_EQ(signup("login@example.com", "correct-horse").status, 303);

  const char *bad = "email=login@example.com&password=wrong-horse";
  Response wrong = webc_test_request_ex("POST", "/login",
                                        "Content-Type: application/x-www-form-urlencoded", bad,
                                        strlen(bad));
  ASSERT_EQ(wrong.status, 401);

  const char *good = "email=login@example.com&password=correct-horse";
  Response ok = webc_test_request_ex("POST", "/login",
                                     "Content-Type: application/x-www-form-urlencoded", good,
                                     strlen(good));
  ASSERT_EQ(ok.status, 303);
  const char *cookie = session_cookie(&ok);
  Response me = webc_test_request_ex("GET", "/me", cookie, nullptr, 0);
  ASSERT_EQ(me.status, 200);
  ASSERT_HTML_TEXT(me, "main > h1", "Your account");
  ASSERT_HTML_TEXT(me, "main p.sub", "Signed in as login@example.com.");
  // a failed sign-in keeps the address, so the reader fixes the password and not both
  ASSERT_HTML_ATTR(wrong, "input[name=email]", "value", "login@example.com");
}

TEST(logout_clears_the_cookie) {
  Response r = webc_test_request("POST", "/logout");
  ASSERT_EQ(r.status, 303);
  bool cleared = false;
  for (i32 i = 0; i < r.nheaders; i++)
    if (str_eq_c(r.headers[i].name, "Set-Cookie") &&
        str_contains(r.headers[i].value, str_lit("Max-Age=0")))
      cleared = true;
  ASSERT(cleared);
}

TEST(an_unknown_email_is_401) {
  const char *form = "email=nobody@example.com&password=hunter2hunter2";
  Response r = webc_test_request_ex("POST", "/login", "Content-Type: application/x-www-form-urlencoded",
                                    form, strlen(form));
  ASSERT_EQ(r.status, 401);
  // one sentence for both a wrong password and an unknown address: telling them apart
  // would say which addresses have accounts
  ASSERT_HTML_TEXT(r, "main > p.error", "That email and password do not match.");
}

TEST_MAIN()