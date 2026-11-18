// The same model operations through every driver, compared field by field.
//
// sqlite always runs. postgres runs when WEBC_PG_URL points at a live server
// (`make pg-test` starts one in docker, runs this file, and removes it), and mysql runs when
// WEBC_MYSQL_URL does (`make mysql-test`). The tests SKIP with a reason when a server is
// missing — a suite that fails on a machine without postgres is a suite people learn to
// ignore, and one that pretends is worse.
//
// The third dialect is where a "portable" data layer stops being a slogan: MySQL has no
// `returning` (the OK packet carries the key), no `offset` without a `limit`, backtick
// quoting, `bigint auto_increment` instead of `bigserial`, and a strict mode that has to be
// on for a too-long string to be an error instead of a truncation.
//
// This is the file that found `db_insert(db, T, &t, &id)` returning 0 on postgres
// while sqlite returned the new rowid: postgres does not report an inserted key
// unless the statement asks for it.
#include <webc/test.h>

WEBC_MODEL(Parity, (id, i64, .pk = true), (email, str, .unique = true, .required = true),
           (balance, i64))

WEBC_VEC_DEFINE(VecParity, vec_parity, Parity)

static const char *PG_URL(void) {
  const char *u = getenv("WEBC_PG_URL");
  return u && *u ? u : nullptr;
}

static const char *MY_URL(void) {
  const char *u = getenv("WEBC_MYSQL_URL");
  return u && *u ? u : nullptr;
}

// A table of its own per driver, dropped first: `make pg-test` runs this against
// a server other things may be using.
static WDB *open_driver(const char *url) {
  WDB *db = db_open(url);
  if (!db) return nullptr;
  // The table name comes from the model: WEBC_MODEL pluralises it (`Parity` →
  // `parities`), and a hand-written DDL with the wrong name is exactly the kind of
  // silent mismatch this file exists to catch — it caught it here first.
  char sql[256];
  snprintf(sql, sizeof sql, "drop table if exists %s", Parity_modelof()->table);
  db_exec_sql(db, sql);
  // The identity column is spelled per dialect: sqlite assigns the rowid of an
  // `integer primary key`, postgres needs a sequence. This is exactly why
  // migrations may carry a `<name>.<driver>.sql` variant.
  str name = db_driver_name(db);
  const char *pk = str_eq_c(name, "postgres") ? "bigserial primary key"
                   : str_eq_c(name, "mysql")  ? "bigint auto_increment primary key"
                                              : "integer primary key";
  // `text unique` cannot be a MySQL key without a prefix length, so the column is a varchar
  // there; 190 characters is what utf8mb4 allows in an index on an older server too.
  const char *email = str_eq_c(name, "mysql") ? "varchar(190) unique not null"
                                              : "text unique not null";
  snprintf(sql, sizeof sql,
           "create table %s (id %s, email %s, balance integer not null default 0)",
           Parity_modelof()->table, pk, email);
  WRes ddl = db_exec_sql(db, sql);
  if (!ddl.ok) {
    db_close(db);
    return nullptr;
  }
  return db;
}

// A server that answers the URL but refuses the connection is a skip with a reason, not an
// assertion followed by a crash: `ASSERT_NOT_NULL` records a failure and carries on, so a null
// handle here segfaults three lines later and hides the driver's own sentence.
static char g_open_why[512];
static const char *open_or_skip(const char *url, WDB **out) {
  *out = open_driver(url);
  if (*out) return nullptr;
  str why = db_error(nullptr);
  snprintf(g_open_why, sizeof g_open_why, "could not open %s: %.*s", url, (int)why.n, why.p);
  return g_open_why;
}

static bool add(WDB *db, const char *email, i64 balance) {
  Parity p = {.email = str_lit(email), .balance = balance};
  return db_insert_into(db, Parity_modelof(), &p, nullptr);
}

typedef struct Snapshot {
  i32 n;
  char ids[8][32];
  char emails[8][64];
  i64 balance[8];
} Snapshot;

static Snapshot snapshot(WDB *db) {
  Snapshot s = {0};
  VecParity *v = db_all(db, Parity);
  if (!v) return s;
  s.n = vec_parity_len(v);
  for (i32 i = 0; i < s.n && i < 8; i++) {
    Parity *p = vec_parity_ref(v, i);
    // ids are per database; what must match is which row is where
    snprintf(s.ids[i], sizeof s.ids[i], "row%d", i);
    snprintf(s.emails[i], sizeof s.emails[i], "%.*s", (int)p->email.n, p->email.p);
    s.balance[i] = p->balance;
  }
  return s;
}

static void compare(WDB *lite, WDB *pg) {
  Snapshot a = snapshot(lite), b = snapshot(pg);
  ASSERT_EQ(a.n, b.n);
  for (i32 i = 0; i < a.n; i++) {
    ASSERT_STR_EQ(a.ids[i], b.ids[i]);
    ASSERT_STR_EQ(a.emails[i], b.emails[i]);
    ASSERT_EQ(a.balance[i], b.balance[i]);
  }
}

// The operations both drivers must agree on: insert with a key coming back,
// select, named parameters, update by pk, delete by pk, transaction rollback,
// transaction commit.
static void same_story(WDB *db) {
  // The key comes back from both drivers (this is the assertion that found
  // postgres returning 0: it needs `returning`, sqlite has a rowid), but its
  // *value* is the database's business — a sequence is not reset by dropping a
  // table — so the test only requires it to exist and to grow.
  i64 id1 = 0, id2 = 0, id3 = 0;
  Parity a = {.email = str_lit("ada@example.com"), .balance = 100};
  ASSERT(db_insert_into(db, Parity_modelof(), &a, &id1));
  ASSERT(id1 > 0);
  Parity g = {.email = str_lit("grace@example.com"), .balance = 50};
  ASSERT(db_insert_into(db, Parity_modelof(), &g, &id2));
  ASSERT(id2 > id1);
  Parity k = {.email = str_lit("kay@example.com"), .balance = 75};
  ASSERT(db_insert_into(db, Parity_modelof(), &k, &id3));
  ASSERT(id3 > id2);

  char sql[256];
  snprintf(sql, sizeof sql, "select * from %s where balance > :min order by balance",
           Parity_modelof()->table);
  WQuery *qy = q(sql);
  q_i64(qy, "min", 60);
  WRes r = db_exec(db, qy);
  ASSERT(r.ok);
  ASSERT_EQ(r.nrows, 2);
  ASSERT_STR_EQ(row_str(&r.rows[0], "email"), "kay@example.com");
  ASSERT_EQ(row_i64(&r.rows[1], "balance"), 100);
  wres_free(&r);

  Parity upd = {.id = id3, .email = str_lit("kay@example.com"), .balance = 10};
  ASSERT(db_update(db, Parity, &upd));
  snprintf(sql, sizeof sql, "select * from %s where id = :id", Parity_modelof()->table);
  WQuery *one = q(sql);
  q_i64(one, "id", id3);
  WRes r2 = db_exec(db, one);
  ASSERT_EQ(r2.nrows, 1);
  ASSERT_EQ(row_i64(&r2.rows[0], "balance"), 10);
  wres_free(&r2);

  ASSERT(db_delete(db, Parity, id2));

  ASSERT(db_begin(db));
  Parity t = {.email = str_lit("rolled@example.com"), .balance = 1};
  ASSERT(db_insert_into(db, Parity_modelof(), &t, nullptr));
  ASSERT(db_rollback(db));
  ASSERT_EQ(vec_parity_len(db_all(db, Parity)), 2);

  ASSERT(db_begin(db));
  Parity t2 = {.email = str_lit("kept@example.com"), .balance = 2};
  ASSERT(db_insert_into(db, Parity_modelof(), &t2, nullptr));
  ASSERT(db_commit(db));
  ASSERT_EQ(vec_parity_len(db_all(db, Parity)), 3);
}

TEST(every_driver_tells_the_same_story) {
  WDB *lite = open_driver("sqlite::memory:");
  ASSERT_NOT_NULL(lite);
  same_story(lite);
  Snapshot lite_snap = snapshot(lite);

  const char *pg_url = PG_URL();
  const char *my_url = MY_URL();
  if (!pg_url && !my_url) {
    db_close(lite);
    SKIP("sqlite half ran; postgres needs WEBC_PG_URL and mysql needs WEBC_MYSQL_URL");
    return;
  }
  char drop[256];
  snprintf(drop, sizeof drop, "drop table if exists %s", Parity_modelof()->table);
  if (pg_url) {
    WDB *pg = nullptr;
    const char *why = open_or_skip(pg_url, &pg);
    if (why) {
      db_close(lite);
      SKIP(why);
      return;
    }
    same_story(pg);
    compare(lite, pg);
    db_exec_sql(pg, drop);
    db_close(pg);
  }
  if (my_url) {
    WDB *my = nullptr;
    const char *why = open_or_skip(my_url, &my);
    if (why) {
      db_close(lite);
      SKIP(why);
      return;
    }
    same_story(my);
    compare(lite, my);
    db_exec_sql(my, drop);
    db_close(my);
  }
  db_close(lite);
}

TEST(a_page_means_the_same_rows_on_every_driver) {
  // Order + limit + offset is where the dialects part company: MySQL needs a `limit` before
  // an `offset`, sqlite accepts `offset` only after a `limit`, postgres takes it alone.
  WDB *lite = open_driver("sqlite::memory:");
  ASSERT_NOT_NULL(lite);
  for (i32 i = 1; i <= 7; i++) {
    Parity p = {.email = str_fmt(nullptr, "p%d@example.com", i), .balance = i};
    ASSERT(db_insert_into(lite, Parity_modelof(), &p, nullptr));
  }
  WWhere w;
  ww_init(&w, nullptr);
  ww_order(&w, Parity, balance, false);
  ww_limit(&w, 0, 5); // `offset 5` alone: the ORM has to make it legal for each driver
  VecParity *lite_tail = db_all_w(lite, Parity, &w);
  ASSERT_EQ(vec_parity_len(lite_tail), 2);
  ASSERT_EQ(vec_parity_ref(lite_tail, 0)->balance, 6);
  ww_free(&w);

  const char *url = MY_URL();
  if (!url) {
    db_close(lite);
    SKIP("no WEBC_MYSQL_URL: sqlite half ran, mysql half needs a live server");
    return;
  }
  WDB *my = nullptr;
  const char *why = open_or_skip(url, &my);
  if (why) {
    db_close(lite);
    SKIP(why);
    return;
  }
  for (i32 i = 1; i <= 7; i++) {
    Parity p = {.email = str_fmt(nullptr, "p%d@example.com", i), .balance = i};
    ASSERT(db_insert_into(my, Parity_modelof(), &p, nullptr));
  }
  WWhere m;
  ww_init(&m, nullptr);
  ww_order(&m, Parity, balance, false);
  ww_limit(&m, 0, 5);
  VecParity *my_tail = db_all_w(my, Parity, &m);
  ASSERT_EQ(vec_parity_len(my_tail), 2);
  ASSERT_EQ(vec_parity_ref(my_tail, 0)->balance, 6);
  ww_free(&m);
  char drop[256];
  snprintf(drop, sizeof drop, "drop table if exists %s", Parity_modelof()->table);
  db_exec_sql(my, drop);
  db_close(my);
  db_close(lite);
}

TEST(an_error_does_not_poison_the_connection) {
  WDB *lite = open_driver("sqlite::memory:");
  ASSERT_NOT_NULL(lite);
  ASSERT_FALSE(db_exec_sql(lite, "select * from nope").ok);
  ASSERT(add(lite, "after@example.com", 1));
  db_close(lite);

  const char *url = PG_URL();
  const char *my_url = MY_URL();
  if (!url && !my_url) {
    SKIP("no WEBC_PG_URL and no WEBC_MYSQL_URL");
    return;
  }
  char drop[256];
  snprintf(drop, sizeof drop, "drop table if exists %s", Parity_modelof()->table);
  if (url) {
    WDB *pg = nullptr;
    const char *why = open_or_skip(url, &pg);
    if (why) SKIP(why);
    WRes bad = db_exec_sql(pg, "select * from nope");
    ASSERT_FALSE(bad.ok);
    ASSERT(bad.e.len > 0);
    ASSERT_STR_CONTAINS(werr_msg(&bad.e), "does not exist");
    ASSERT(add(pg, "after@example.com", 1));       // still usable
    ASSERT_FALSE(add(pg, "after@example.com", 1)); // unique violation, not a crash
    ASSERT_EQ(vec_parity_len(db_all(pg, Parity)), 1);
    db_exec_sql(pg, drop);
    db_close(pg);
  }
  if (my_url) {
    WDB *my = nullptr;
    const char *why = open_or_skip(my_url, &my);
    if (why) SKIP(why);
    WRes bad = db_exec_sql(my, "select * from nope");
    ASSERT_FALSE(bad.ok);
    ASSERT(bad.e.len > 0);
    ASSERT_STR_CONTAINS(werr_msg(&bad.e), "doesn't exist"); // mysql's own wording
    ASSERT(add(my, "after@example.com", 1));
    ASSERT_FALSE(add(my, "after@example.com", 1));
    ASSERT_EQ(vec_parity_len(db_all(my, Parity)), 1);
    db_exec_sql(my, drop);
    db_close(my);
  }
}

TEST_MAIN()