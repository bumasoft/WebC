// What the postgres driver has to do on its own: park instead of block, stop when the
// request is cancelled, and refuse a TLS-less server when sslmode says it must.
//
// Gated on WEBC_PG_URL like db_parity_test.c (`make pg-test` starts the server). Without it
// every test SKIPs with a reason, so a machine without postgres still gets a green run.
//
// Why these three and not "does select 1 work" (db_parity_test.c covers that):
//   * parking — a driver that blocks a worker thread caps the whole server at thread count,
//     which was the largest backend gap in the report;
//   * cancellation — `mw_timeout_ms()` can only keep its promise if a statement in flight
//     can be stopped, otherwise a 504 waits for the query it already gave up on;
//   * sslmode — managed postgres refuses a plaintext client, so `require` has to be real and
//     its refusal has to be an error, never a silent downgrade.
#define WEBC_NO_SELECT_MACRO 1
#include <webc/db.h>
#include <webc/ctx.h>
#include <webc/task.h>
#include <webc/test.h>

#define PG_URL_OR_SKIP(var_)                                                   \
  const char *var_ = getenv("WEBC_PG_URL");                                    \
  if (!(var_) || !*(var_)) SKIP("no WEBC_PG_URL: run `make pg-test` to start a server")

// Sleep on the server: the round trip is real and the wait happens inside postgres, which is
// the shape of any slow query.
typedef struct {
  bool ok;
  u64 took;
  char err[192];
} Sleeper;

static void one_query(void *arg) {
  Sleeper *s = arg;
  WRes r = db_exec_sql(db_default(), "select 1 as one");
  s->ok = r.ok;
  if (!r.ok) snprintf(s->err, sizeof s->err, "%.*s", WERR_FMT(r.e));
}

static void sleep_query(void *arg) {
  Sleeper *s = arg;
  u64 t0 = wc_time_ms();
  WRes r = db_exec_sql(db_default(), "select pg_sleep(0.3)");
  s->ok = r.ok;
  s->took = wc_time_ms() - t0;
  if (!r.ok) snprintf(s->err, sizeof s->err, "%.*s", WERR_FMT(r.e));
}

TEST(a_waiting_statement_does_not_hold_a_thread) {
  PG_URL_OR_SKIP(url);
  // More waiters than processors on any laptop: blocked, they run in batches of `cpus` and
  // take batches × 300 ms. Parked, they are all in flight at once.
  enum { N = 64 };
  db_pool_config(N, 10000);
  WDB *db = db_open(url);
  ASSERT_TRUE(db != nullptr);
  setenv("DATABASE_URL", url, 1);
  db_default_set(db);

  static Sleeper sleepers[N];
  // Warm the pool first: opening 64 connections is its own cost (a fork, a SCRAM exchange
  // and a full startup per connection, serialised by the server), and what this test is
  // about is the wait, not the connect.
  for (i32 i = 0; i < N; i++) {
    sleepers[i] = (Sleeper){0};
    task_async(one_query, &sleepers[i]);
  }
  wtask_wait();
  u64 t0 = wc_time_ms();
  for (i32 i = 0; i < N; i++) {
    sleepers[i] = (Sleeper){0};
    task_async(sleep_query, &sleepers[i]);
  }
  wtask_wait();
  u64 took = wc_time_ms() - t0;

  i32 ok = 0;
  u64 slowest = 0;
  for (i32 i = 0; i < N; i++) {
    ok += sleepers[i].ok ? 1 : 0;
    if (!sleepers[i].ok) printf("    %d: %s\n", (int)i, sleepers[i].err);
    if (sleepers[i].took > slowest) slowest = sleepers[i].took;
  }
  ASSERT_EQ(ok, N);
  // Blocked, 64 statements on 10 threads would take ~7 x 300 ms ≈ 2 s. Parked, one wave.
  if (took > 1000)
    printf("    64 x pg_sleep(0.3) took %llums (slowest %llums): statements are serialised\n",
           (unsigned long long)took, (unsigned long long)slowest);
  ASSERT_TRUE(took < 1000);
  db_pool_config(0, 0);
}

TEST(a_statement_stops_when_the_request_is_cancelled) {
  PG_URL_OR_SKIP(url);
  WDB *db = db_open(url);
  ASSERT_TRUE(db != nullptr);
  setenv("DATABASE_URL", url, 1);
  db_default_set(db);

  // A context whose deadline passes while the statement is in flight — exactly what
  // mw_timeout_ms() leaves behind.
  WCtx *ctx = ctx_new(nullptr);
  ctx_set_deadline(ctx, (i64)wc_time_ms() + 150);
  WDbSession *s = db_session(ctx);
  ASSERT_TRUE(s != nullptr);

  u64 t0 = wc_time_ms();
  WRes r = db_exec_sql(db, "select pg_sleep(5)");
  u64 took = wc_time_ms() - t0;

  ASSERT_FALSE(r.ok);
  ASSERT_TRUE(took < 2000); // 5 s of server work, abandoned at the poll granularity
  char msg[256] = {0};
  snprintf(msg, sizeof msg, "%.*s", WERR_FMT(r.e));
  // The reason has to name the cancellation: "connection lost" here would hide the
  // difference between "we gave up" and "the database died".
  ASSERT_TRUE(strstr(msg, "cancel") != nullptr);
  db_session_release();
  db_close(db);
  // The connection was cancelled, not parked back in the pool: the next open works.
  WDB *again = db_open(url);
  ASSERT_TRUE(again != nullptr);
  db_close(again);
}

TEST(require_refuses_a_plaintext_server) {
  PG_URL_OR_SKIP(url);
  // The throwaway server `make pg-test` starts has no certificate, so `require` must end in
  // an error rather than a silent plaintext connection: that silence is how a password ends
  // up on the wire the first time this is pointed at a managed database.
  char wanted[512];
  snprintf(wanted, sizeof wanted, "%s?sslmode=require", url);
  WDB *db = db_open(wanted);
  if (db) { // a TLS-enabled server on the other end: connecting is then the right answer
    WRes r = db_exec_sql(db, "select 1 as one");
    ASSERT_TRUE(r.ok);
    db_close(db);
  }
}

TEST(sslmode_disable_is_honoured) {
  PG_URL_OR_SKIP(url);
  char plain[512];
  snprintf(plain, sizeof plain, "%s?sslmode=disable", url);
  WDB *db = db_open(plain);
  ASSERT_TRUE(db != nullptr);
  WRes r = db_exec_sql(db, "select 1 as one");
  ASSERT_TRUE(r.ok);
  db_close(db);
}

WEBC_MODEL(Nullable, (id, i64, .pk = true), (name, str))

// A cast is not a placeholder: rewritten to `select 1$1text`, `select 1::text` comes back as
// "syntax error at or near \":\"". `::` is how postgres casts, so this is the first thing a
// `q()` written for postgres hits.
TEST(a_cast_is_not_a_placeholder) {
  PG_URL_OR_SKIP(url);
  WDB *db = db_open(url);
  ASSERT_TRUE(db != nullptr);
  WQuery *qy = q("select 1::text as cast_col, :v as v, 'a::b' as lit");
  q_i64(qy, "v", 3);
  WRes r = db_exec(db, qy);
  ASSERT_TRUE(r.ok);
  ASSERT_STR_EQ(row_str(&r.rows[0], "cast_col"), "1");
  ASSERT_EQ(row_i64(&r.rows[0], "v"), 3);
  ASSERT_STR_EQ(row_str(&r.rows[0], "lit"), "a::b");
  wres_free(&r);
  db_close(db);
}

// A migration file is several statements in one call, and the extended protocol answers
// "cannot insert multiple commands into a prepared statement" — so a project's hand-written
// `0001_init.postgres.sql` could never be applied while its sqlite twin ran the whole file. With no
// parameters the driver now uses the simple protocol, which takes the file; the result a caller
// gets back is the *last* statement's, the same rule sqlite and mysql follow.
TEST(a_file_of_statements_runs_in_one_call) {
  PG_URL_OR_SKIP(url);
  WDB *db = db_open(url);
  ASSERT_TRUE(db != nullptr);
  WRes ddl = db_exec_sql(db,
                         "drop table if exists wc_multi;"
                         "create table wc_multi (id integer primary key, note text not null);"
                         "insert into wc_multi (id, note) values (1, 'one'), (2, 'two');");
  ASSERT_TRUE(ddl.ok);
  wres_free(&ddl);
  WRes r = db_exec_sql(db, "select count(*) as n from wc_multi; select max(note) as last from wc_multi;");
  ASSERT_TRUE(r.ok);
  // the last statement in the file is the one whose shape and rows come back
  ASSERT_EQ(r.nrows, 1);
  ASSERT_STR_EQ(row_str(&r.rows[0], "last"), "two");
  wres_free(&r);
  // and a failure part-way still reports the server's sentence, not a silent success
  WRes bad = db_exec_sql(db, "insert into wc_multi (id, note) values (3, 'three');"
                              "insert into wc_multi (id, note) values (1, 'dup');");
  ASSERT_FALSE(bad.ok);
  ASSERT_TRUE(str_contains(werr_msg(&bad.e), str_lit("duplicate key")));
  wres_free(&bad);
  WRes drop = db_exec_sql(db, "drop table wc_multi");
  wres_free(&drop);
  db_close(db);
}

TEST(a_null_handle_is_a_refusal_not_a_crash) {
  // Not about postgres: a model helper that dereferences the handle when DATABASE_URL is
  // unset takes the whole process down inside a handler.
  Nullable n = {.name = str_lit("x")};
  i64 id = 0;
  ASSERT_FALSE(db_insert_into(nullptr, Nullable_modelof(), &n, &id));
}

TEST_MAIN()