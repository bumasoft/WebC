// db_url: one splitter for every driver's connection string.
//
// `postgres://user:pass@host:5432/db?sslmode=require` and its mysql twin are the same
// grammar, and the drivers share db_url.c so a password containing '@', a missing port and
// an empty host mean one thing across the layer rather than three. These are the shapes a
// deployment actually hands over, asserted once, where no server is needed.
#include <webc/test.h>

#include "../src/db_internal.h"

static str S(const char *s) { return str_lit(s); }

TEST(a_full_url_splits_into_six_fields) {
  WDbUrl u;
  wc_db_url_split("postgres://ada:s3cret@db.internal:5433/app?sslmode=require", &u);
  ASSERT_TRUE(str_eq(u.user, S("ada")));
  ASSERT_TRUE(str_eq(u.pass, S("s3cret")));
  ASSERT_TRUE(str_eq(u.host, S("db.internal")));
  ASSERT_TRUE(str_eq(u.port, S("5433")));
  ASSERT_TRUE(str_eq(u.db, S("app")));
  ASSERT_TRUE(str_eq(u.query, S("sslmode=require")));
}

TEST(what_the_url_leaves_out_stays_empty) {
  WDbUrl u;
  wc_db_url_split("mysql://localhost/app", &u);
  ASSERT_EQ(u.user.n, 0u);
  ASSERT_EQ(u.pass.n, 0u);
  ASSERT_TRUE(str_eq(u.host, S("localhost")));
  ASSERT_EQ(u.port.n, 0u); // the driver's own default stands in
  ASSERT_TRUE(str_eq(u.db, S("app")));
  ASSERT_EQ(u.query.n, 0u);
}

TEST(a_password_may_contain_an_at_sign) {
  // A generated password pasted into a dashboard is not URL-encoded, and the last '@'
  // before the path is the one that ends the credentials.
  WDbUrl u;
  wc_db_url_split("postgres://ada:@p@ss@host:5432/app", &u);
  ASSERT_TRUE(str_eq(u.user, S("ada")));
  ASSERT_TRUE(str_eq(u.pass, S("@p@ss")));
  ASSERT_TRUE(str_eq(u.host, S("host")));
  ASSERT_TRUE(str_eq(u.port, S("5432")));
  ASSERT_TRUE(str_eq(u.db, S("app")));
}

TEST(an_empty_host_is_the_socket_case) {
  // "mysql://user:pw@/db?socket=/tmp/mysql.sock": the host is empty on purpose and the
  // driver keeps its default, which is what lets a container's socket decide the transport.
  WDbUrl u;
  wc_db_url_split("mysql://app:pw@/app?socket=/tmp/mysql.sock", &u);
  ASSERT_TRUE(str_eq(u.user, S("app")));
  ASSERT_TRUE(str_eq(u.pass, S("pw")));
  ASSERT_EQ(u.host.n, 0u);
  ASSERT_TRUE(str_eq(u.db, S("app")));
  ASSERT_TRUE(str_eq(u.query, S("socket=/tmp/mysql.sock")));
}

TEST(a_query_without_a_path_still_splits) {
  WDbUrl u;
  wc_db_url_split("postgres://host?sslmode=disable", &u);
  ASSERT_TRUE(str_eq(u.host, S("host")));
  ASSERT_EQ(u.db.n, 0u);
  ASSERT_TRUE(str_eq(u.query, S("sslmode=disable")));
}

TEST(no_scheme_is_a_path_and_nothing_else) {
  WDbUrl u;
  wc_db_url_split("data/dev.db", &u);
  ASSERT_EQ(u.user.n, 0u);
  ASSERT_EQ(u.query.n, 0u);
  ASSERT_TRUE(str_eq(u.host, S("data")));
}

TEST(a_query_string_walks_pair_by_pair) {
  str q = S("sslmode=require&sslrootcert=/etc/ca.pem&empty=&bare");
  str key, val;
  size_t i = 0;
  ASSERT_TRUE(wc_kv_next(q, &i, &key, &val));
  ASSERT_TRUE(str_eq(key, S("sslmode")) && str_eq(val, S("require")));
  ASSERT_TRUE(wc_kv_next(q, &i, &key, &val));
  ASSERT_TRUE(str_eq(key, S("sslrootcert")) && str_eq(val, S("/etc/ca.pem")));
  ASSERT_TRUE(wc_kv_next(q, &i, &key, &val));
  ASSERT_TRUE(str_eq(key, S("empty")) && val.n == 0);
  ASSERT_TRUE(wc_kv_next(q, &i, &key, &val));
  ASSERT_TRUE(str_eq(key, S("bare")) && val.n == 0);
  ASSERT_FALSE(wc_kv_next(q, &i, &key, &val));
}

TEST(an_empty_query_yields_nothing) {
  str key, val;
  size_t i = 0;
  ASSERT_FALSE(wc_kv_next(S(""), &i, &key, &val));
  ASSERT_FALSE(wc_kv_next(S("&&"), &i, &key, &val));
}
TEST_MAIN()
