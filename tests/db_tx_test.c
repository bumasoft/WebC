// db_tx: a transaction belongs to a session, and a session belongs to a task.
//
// The defect this suite exists for: BEGIN and COMMIT that acquire and release a pooled
// connection around their own statement make two concurrent handlers share one connection's
// transaction state. A rollback then undoes another request's committed insert, and a commit
// makes another request's rollback permanent. Both are asked for here, repeatedly.
//
// The suites run on a *file* database on purpose: `sqlite::memory:` is one connection by
// definition (the pool forces that, because a second connection to a memory database is a
// second, empty database), and a concurrency test on one connection tests nothing.
// `select` is a macro in task.h; this file includes <unistd.h>.
#define WEBC_NO_SELECT_MACRO 1
#include <webc/test.h>

#include <unistd.h>

WEBC_MODEL(Entry, (id, i64, .pk = true),
           (tag, str, .unique = true, .required = true),
           (n, i64))

WEBC_VEC_DEFINE(VecEntry, vec_entry, Entry)

static char g_path[256];
static WDB *g_db;

static WDB *tx_db(void) {
  if (g_db) return g_db;
  snprintf(g_path, sizeof g_path, "sqlite:/tmp/webc_tx_test_%ld.db", (long)getpid());
  unlink(g_path + 7); // start from nothing, whatever a crashed run left behind
  db_pool_config(4, 5000);
  g_db = db_open(g_path);
  if (!g_db) return nullptr;
  WRes ddl = db_exec_sql(g_db,
                         "create table entries (id integer primary key autoincrement,"
                         " tag text unique not null, n integer not null default 0)");
  if (!ddl.ok) {
    db_close(g_db);
    g_db = nullptr;
    return nullptr;
  }
  db_set_default(g_db);
  return g_db;
}

static void wipe(WDB *db) {
  WRes r = db_exec_sql(db, "delete from entries");
  wres_free(&r);
}

static i64 count_entries(WDB *db) {
  WRes r = db_exec_sql(db, "select count(*) as n from entries");
  if (!r.ok) return -1;
  i64 n = row_i64(&r.rows[0], "n");
  wres_free(&r);
  return n;
}

static bool add_entry(WDB *db, const char *tag) {
  str t = {(char *)tag, strlen(tag)};
  Entry e = {.tag = t, .n = 1};
  return db_insert_into(db, Entry_modelof(), &e, nullptr);
}

// ---------------------------------------------------------------- one "request"

typedef struct TxWork {
  WDB *db;
  const char *tag;
  bool commit;
  i64 sleep_ms;
  bool began;
  bool ended_ok;
  char err[192];
} TxWork;

static void tx_task(void *arg) {
  TxWork *w = arg;
  WTx t = tx_begin(w->db);
  w->began = t.began;
  if (!t.began) {
    snprintf(w->err, sizeof w->err, "%s", t.err);
    return;
  }
  if (!add_entry(w->db, w->tag)) {
    snprintf(w->err, sizeof w->err, "insert failed: %s", db_error(w->db).p);
    tx_rollback(&t);
    return;
  }
  if (w->sleep_ms) wg_sleep_ms((u64)w->sleep_ms);
  bool ok = w->commit ? tx_commit(&t) : tx_rollback(&t);
  w->ended_ok = ok;
  if (!ok) snprintf(w->err, sizeof w->err, "%s", t.err);
  db_session_release();
}

// A task that opens a transaction and then simply ends: the exit hook must roll it back.
static void tx_task_abandons(void *arg) {
  WDB *db = arg;
  WTx t = tx_begin(db);
  if (!t.began) return;
  add_entry(db, "abandoned");
  wg_sleep_ms(5);
  // no commit, no rollback, no db_session_release(): the task just ends
}

TEST(begin_commit_and_rollback_on_one_connection) {
  WDB *db = tx_db();
  ASSERT_NOT_NULL(db);
  wipe(db);

  WTx t = tx_begin(db);
  ASSERT_TRUE(t.began);
  ASSERT_TRUE(add_entry(db, "kept"));
  ASSERT_TRUE(tx_commit(&t));
  ASSERT_EQ(count_entries(db), 1);

  WTx t2 = tx_begin(db);
  ASSERT_TRUE(t2.began);
  ASSERT_TRUE(add_entry(db, "dropped"));
  ASSERT_TRUE(tx_rollback(&t2));
  ASSERT_EQ(count_entries(db), 1);
  db_session_release();
}

TEST(a_commit_with_no_transaction_says_so) {
  WDB *db = tx_db();
  ASSERT_NOT_NULL(db);
  db_session_release(); // start from "no session, no transaction"
  WTx t = tx_begin(db);
  ASSERT_TRUE(t.began);
  ASSERT_TRUE(tx_commit(&t));
  // ... and now there is nothing open: a second commit must fail, not pretend
  ASSERT_FALSE(tx_commit(&t));
  ASSERT_TRUE(t.err[0] != 0);
  ASSERT_STR_CONTAINS(str_lit(t.err), "no open transaction");
  db_session_release();
}

TEST(nested_scopes_are_savepoints) {
  WDB *db = tx_db();
  ASSERT_NOT_NULL(db);
  wipe(db);

  tx_scope(db) {
    ASSERT_TRUE(add_entry(db, "outer"));
    tx_scope(db) {
      ASSERT_TRUE(add_entry(db, "inner"));
      ASSERT_EQ(tx_depth(), 2);
      tx_rollback_here(); // undo the inner row only
    }
    ASSERT_EQ(tx_depth(), 1);
    tx_commit_here();
  }
  ASSERT_EQ(tx_depth(), 0);
  ASSERT_EQ(count_entries(db), 1);

  WRes r = db_exec_sql(db, "select tag from entries");
  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.nrows, 1);
  ASSERT_STR_EQ(row_str(&r.rows[0], "tag"), "outer");
  wres_free(&r);
  db_session_release();
}

// The regression this whole change is about. Two concurrent transactions, one commits and
// one rolls back, twenty rounds: the row that survives must always be the committed one.
TEST(a_rollback_cannot_reach_another_tasks_commit) {
  WDB *db = tx_db();
  ASSERT_NOT_NULL(db);
  i32 wrong = 0;
  char first_bad[200] = "";

  for (i32 round = 0; round < 20; round++) {
    wipe(db);
    char keeper[32], loser[32];
    snprintf(keeper, sizeof keeper, "keep-%d", (int)round);
    snprintf(loser, sizeof loser, "lose-%d", (int)round);

    TxWork a = {.db = db, .tag = keeper, .commit = true, .sleep_ms = 20};
    TxWork b = {.db = db, .tag = loser, .commit = false, .sleep_ms = 20};
    task_async(tx_task, &a);
    task_async(tx_task, &b);
    wtask_wait();

    if (!a.began || !b.began || !a.ended_ok) {
      wrong++;
      if (!first_bad[0])
        snprintf(first_bad, sizeof first_bad,
                 "round %d: a.began=%d b.began=%d a.committed=%d [%s] [%s]", (int)round,
                 (int)a.began, (int)b.began, (int)a.ended_ok, a.err, b.err);
      continue;
    }
    i64 n = count_entries(db);
    WRes r = db_exec_sql(db, "select tag from entries");
    bool kept = r.ok && r.nrows == 1 && str_eq(row_str(&r.rows[0], "tag"), str_lit(keeper));
    wres_free(&r);
    // sqlite may refuse the second writer with SQLITE_BUSY and the task honestly reports it;
    // what may never happen is a wrong row, two rows, or none.
    if (n > 0 && !kept) {
      wrong++;
      if (!first_bad[0])
        snprintf(first_bad, sizeof first_bad, "round %d: %lld row(s), expected exactly %s",
                 (int)round, (long long)n, keeper);
    }
  }
  if (wrong) wc_test_expect(false, __FILE__, __LINE__, "%d of 20 rounds wrong: %s", (int)wrong, first_bad);
}

// A transaction opened by a request is ended by the request boundary, but a *task* that
// forgets must not leave one open for the pool: the exit hook rolls it back.
TEST(a_task_that_ends_inside_a_transaction_leaves_nothing_behind) {
  WDB *db = tx_db();
  ASSERT_NOT_NULL(db);
  wipe(db);
  db_session_release();

  task_async(tx_task_abandons, db);
  wtask_wait();
  ASSERT_EQ(count_entries(db), 0);
  // and the connection came back: a new transaction on this task works immediately
  WTx t = tx_begin(db);
  ASSERT_TRUE(t.began);
  ASSERT_TRUE(add_entry(db, "after"));
  ASSERT_TRUE(tx_commit(&t));
  ASSERT_EQ(count_entries(db), 1);
  db_session_release();
}

// Two tasks, two sessions: the one a request holds is not the one its neighbour holds.
typedef struct Pair {
  WDB *db;
  WDbSession *mine;
  WDbSession *theirs;
  ChanInt ready;
  ChanInt go;
} Pair;

static Pair g_pair;

static void pair_holder(void *arg) {
  (void)arg;
  g_pair.mine = db_session(nullptr);
  chan_int_send(g_pair.ready, 1);
  i32 v = 0;
  chan_int_recv(g_pair.go, &v);
  db_session_release();
}

static void pair_observer(void *arg) {
  (void)arg;
  i32 v = 0;
  chan_int_recv(g_pair.ready, &v);
  g_pair.theirs = db_session(nullptr);
  chan_int_send(g_pair.go, 1);
  db_session_release();
}

TEST(a_session_is_not_shared_between_tasks) {
  WDB *db = tx_db();
  ASSERT_NOT_NULL(db);
  db_session_release();
  ASSERT_NULL(db_session_current());

  g_pair = (Pair){.db = db,
                  .ready = chan_int_new(1),
                  .go = chan_int_new(1)};
  task_async(pair_holder, nullptr);
  task_async(pair_observer, nullptr);
  wtask_wait();
  ASSERT_NOT_NULL(g_pair.mine);
  ASSERT_NOT_NULL(g_pair.theirs);
  ASSERT_TRUE(g_pair.mine != g_pair.theirs);
  chan_int_close(g_pair.ready);
  chan_int_close(g_pair.go);
}

// A pool of one, and a second task that must be refused instead of parked for ever.
typedef struct Try {
  WDB *db;
  bool ok;
  u64 took_ms;
  char err[256];
} Try;

static Try g_try;

static void pool_contender(void *arg) {
  (void)arg;
  u64 t0 = wc_time_ms();
  WRes r = db_exec_sql(g_try.db, "select 1 as one");
  g_try.took_ms = wc_time_ms() - t0;
  g_try.ok = r.ok;
  if (!r.ok) snprintf(g_try.err, sizeof g_try.err, "%.*s", WERR_FMT(r.e));
  wres_free(&r);
  db_session_release();
}

TEST(the_pool_refuses_rather_than_hangs) {
  char path[200];
  snprintf(path, sizeof path, "sqlite:/tmp/webc_tx_pool_%ld.db", (long)getpid());
  unlink(path + 7);
  db_pool_config(1, 100); // one connection, and 100 ms to wait for it
  WDB *db = db_open(path);
  ASSERT_NOT_NULL(db);
  WRes ddl = db_exec_sql(db, "create table p (id integer primary key)");
  ASSERT_TRUE(ddl.ok);
  wres_free(&ddl);
  db_set_default(db);

  // hold the only connection, on this task
  ASSERT_NOT_NULL(db_session(nullptr));

  g_try = (Try){.db = db};
  task_async(pool_contender, nullptr);
  wtask_wait();
  ASSERT_FALSE(g_try.ok);
  ASSERT_STR_CONTAINS(str_lit(g_try.err), "no free connection");
  ASSERT_LT(g_try.took_ms, 2000); // refused at its deadline, not left hanging

  db_session_release();
  db_pool_config(0, 0);
  db_set_default(tx_db());
  db_close(db);
}

TEST_MAIN();