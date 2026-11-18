// auth: password hashing, signed sessions, tamper resistance.
#include <webc/test.h>

TEST(password_hash_round_trip) {
  Arena *a = arena_new(8192);
  str hash = auth_hash_password_iters(a, str_lit("hunter2"), 1000);
  ASSERT_STR_CONTAINS(hash, "pbkdf2-sha256$1000$");
  ASSERT(auth_verify_password(hash, str_lit("hunter2")));
  ASSERT_FALSE(auth_verify_password(hash, str_lit("hunter3")));
  ASSERT_FALSE(auth_verify_password(str_lit("garbage"), str_lit("hunter2")));
  arena_destroy(a);
}

TEST(the_same_password_hashes_differently) {
  Arena *a = arena_new(8192);
  str hash = auth_hash_password_iters(a, str_lit("hunter2"), 1000);
  str hash2 = auth_hash_password_iters(a, str_lit("hunter2"), 1000);
  ASSERT(!str_eq(hash, hash2));   // random salt
  ASSERT(auth_verify_password(hash2, str_lit("hunter2")));
  arena_destroy(a);
}

// the session= value out of a response's Set-Cookie header
static str session_value(Response *res) {
  str cookie = {0};
  for (i32 i = 0; i < res->nheaders; i++)
    if (str_eq_c(res->headers[i].name, "Set-Cookie")) cookie = res->headers[i].value;
  if (cookie.n == 0) return cookie;
  str value = str_sub(cookie, 8, cookie.n);   // "session=<value>; Path=/; ..."
  i64 semi = str_index(value, ';');
  return semi > 0 ? str_sub(value, 0, (size_t)semi) : value;
}

static bool session_uid(Arena *a, str secret, str value, i64 *uid) {
  Request req = { .arena = a };
  static WHeader hdr;
  hdr = (WHeader){ .name = str_lit("cookie"),
                   .value = str_fmt(a, "session=%.*s", (int)value.n, value.p) };
  req.headers = &hdr;
  req.nheaders = 1;
  return auth_get_session(&req, secret, uid);
}

TEST(session_cookie_round_trips) {
  Arena *a = arena_new(8192);
  Response res = res_empty(200);
  str secret = str_lit("topsecret");
  auth_set_session(&res, secret, 4242);
  str value = session_value(&res);
  ASSERT_GT(value.n, 0);

  i64 uid = 0;
  ASSERT(session_uid(a, secret, value, &uid));
  ASSERT_EQ(uid, 4242);
  arena_destroy(a);
}

TEST(a_tampered_or_foreign_session_is_rejected) {
  Arena *a = arena_new(8192);
  Response res = res_empty(200);
  str secret = str_lit("topsecret");
  auth_set_session(&res, secret, 4242);
  str value = session_value(&res);
  ASSERT_GT(value.n, 0);

  str sig = { "", 0 };
  i64 last_dot = str_rindex(value, '.');
  if (last_dot > 0) sig = str_sub(value, (size_t)last_dot + 1, value.n);
  str bad = str_fmt(a, "9999.9999999999.%.*s", (int)sig.n, sig.p);

  i64 uid = 0;
  ASSERT_FALSE(session_uid(a, secret, bad, &uid));                 // payload rewritten
  ASSERT_FALSE(session_uid(a, str_lit("other-secret"), value, &uid));   // wrong key
  arena_destroy(a);
}

TEST_MAIN()