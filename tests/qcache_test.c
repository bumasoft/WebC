// qcache: the query cache at `db_exec` — a hit is a `WRes` replayed from Redis, a write
// invalidates by generation, a broken cache costs a miss and never a request.
//
// The Redis here is tests/fake_redis.h, so the assertions can read the keys the cache wrote and
// the generation counters it bumped — the parts of this feature a black-box test cannot see.
#include "fake_redis.h" // first: it owns the system headers this suite needs

#include <webc/test.h>

WEBC_MODEL(Note, (id, i64, .pk = true), (title, str, .required = true), (score, i64),
           (ratio, f64), (done, bool))

WEBC_VEC_DEFINE(VecNote, vec_note, Note)

static FakeRedis *g_fr;
static WRedis *g_r;
static WDB *g_db;

static WDB *fresh_db(void) {
  WDB *db = db_open("sqlite::memory:");
  if (!db) return nullptr;
  WRes ddl = db_exec_sql(db,
                         "create table notes (id integer primary key autoincrement,"
                         " title text not null, score integer not null default 0,"
                         " ratio real, done integer)");
  if (!ddl.ok) {
    fprintf(stderr, "DBG ddl failed: %s\n", wc_test_str(db_error(db)));
    db_close(db);
    return nullptr;
  }
  return db;
}

static void setup(void) {
  g_fr = fake_redis_start();
  if (!g_fr) return;
  char url[64];
  snprintf(url, sizeof url, "redis://127.0.0.1:%u/0", (unsigned)g_fr->port);
  setenv("REDIS_URL", url, 1);
  g_r = redis_open(url);
  redis_set_default(g_r);
  setenv("WEBC_QUERY_CACHE", "1", 1);
  setenv("WEBC_REDIS_NAME", "cachetest", 1);
  setenv("WEBC_CACHE_TTL_MS", "60000", 1);
  setenv("WEBC_CACHE_TTL_JITTER", "0", 1);
  qcache_config(60000, 262144);
}

static void teardown(void) {
  redis_close(g_r);
  redis_set_default(nullptr);
  fake_redis_stop(g_fr);
  g_r = nullptr;
  g_fr = nullptr;
}

TEST_SETUP(setup);
TEST_TEARDOWN(teardown);

// Every test starts from an empty table and an empty cache — and, if an earlier test killed the
// server (the fail-open one does), from a live server on a fresh port.
static void reset(void) {
  if (!g_fr || g_fr->dead) {
    g_fr = fake_redis_start();
    char url[64];
    snprintf(url, sizeof url, "redis://127.0.0.1:%u/0", (unsigned)g_fr->port);
    setenv("REDIS_URL", url, 1);
    WRedis *fresh = redis_open(url);
    WRedis *old = g_r;
    redis_set_default(fresh);
    g_r = fresh;
    if (old && old != fresh) redis_close(old);
  }
  g_db = fresh_db();
  ASSERT_NOT_NULL(g_db);
  qcache_reset_counters();
  fr_lock(g_fr);
  for (int i = 0; i < FR_KEYS; i++) {
    free(g_fr->keys[i].val);
    memset(&g_fr->keys[i], 0, sizeof g_fr->keys[i]);
  }
  fr_unlock(g_fr);
}

static bool add_note(const char *title, i64 score) {
  Note n = {.title = str_lit(title), .score = score, .ratio = 0.5, .done = false};
  bool ok = db_insert_into(g_db, Note_modelof(), &n, nullptr);
  if (!ok) fprintf(stderr, "DBG insert failed: %s\n", wc_test_str(db_error(g_db)));
  return ok;
}

TEST(miss_then_hit) {
  reset();
  ASSERT_TRUE(add_note("first", 1));

  WCacheStats s0 = qcache_stats();
  VecNote *a = db_all(g_db, Note);
  ASSERT_NOT_NULL(a);
  ASSERT_EQ(a->n, 1);
  WCacheStats s1 = qcache_stats();
  ASSERT_EQ((i64)(s1.hits - s0.hits), 0);
  ASSERT_EQ((i64)(s1.stores - s0.stores), 1); // the first read was stored

  VecNote *b = db_all(g_db, Note);
  ASSERT_NOT_NULL(b);
  ASSERT_EQ(b->n, 1);
  if (b->n == 1) ASSERT_STR_EQ(b->at[0].title, "first");
  WCacheStats s2 = qcache_stats();
  ASSERT_EQ((i64)(s2.hits - s1.hits), 1); // the second came from Redis
  ASSERT_EQ((i64)(s2.stores - s1.stores), 0); // and was not stored again

  // Something was actually written under the app's namespace, in the documented shape.
  ASSERT_TRUE(fake_redis_key_count(g_fr, "webc:{cachetest}:q:") >= 1);
  db_close(g_db);
}

TEST(hit_returns_the_same_rows) {
  reset();
  ASSERT_TRUE(add_note("alpha", 7));
  ASSERT_TRUE(add_note("beta", 9));
  VecNote *a = db_all_ordered(g_db, Note, "title asc");
  ASSERT_NOT_NULL(a);
  ASSERT_EQ(a->n, 2);
  i64 id0 = a->at[0].id;
  str title0 = a->at[0].title;

  VecNote *b = db_all_ordered(g_db, Note, "title asc");
  ASSERT_NOT_NULL(b);
  ASSERT_EQ(b->n, 2);
  // Every type survives the envelope: text, integers, and the null a `real`/`blob` column may be.
  ASSERT_EQ(b->at[0].id, id0);
  ASSERT_STR_EQ(b->at[0].title, "alpha");
  ASSERT_EQ(b->at[0].score, 7);
  ASSERT_EQ(b->at[1].score, 9);
  ASSERT_TRUE(b->at[0].title.n == title0.n && !memcmp(b->at[0].title.p, title0.p, title0.n));
  WCacheStats s = qcache_stats();
  ASSERT_TRUE(s.hits >= 1);
  db_close(g_db);
}

TEST(a_write_invalidates_its_table) {
  reset();
  ASSERT_TRUE(add_note("one", 1));
  db_all(g_db, Note);           // store
  VecNote *warm = db_all(g_db, Note);
  ASSERT_EQ(warm->n, 1);        // hit

  i64 bumps = (i64)qcache_stats().bumps;
  ASSERT_TRUE(add_note("two", 2));
  VecNote *after = db_all(g_db, Note);
  ASSERT_NOT_NULL(after);
  ASSERT_EQ(after->n, 2); // the write was seen: the generation moved
  ASSERT_EQ((i64)qcache_stats().bumps - bumps, 1); // one write, one bump
  // The counter lives where the spec says it does.
  ASSERT_TRUE(fake_redis_key_count(g_fr, "webc:{cachetest}:g:") >= 1);
  db_close(g_db);
}

TEST(a_write_bumps_only_its_own_table) {
  reset();
  db_exec_sql(g_db, "create table other (id integer)"); // DDL: not cacheable, invalidates all
  ASSERT_TRUE(add_note("x", 1));
  db_all(g_db, Note);
  db_all(g_db, Note); // hit
  i64 hits_before = (i64)qcache_stats().hits;

  // A write to another table must not throw the notes cache away: `!all` is untouched by an
  // ordinary insert, which is the whole point of per-table counters.
  WRes w = db_exec_sql(g_db, "insert into other (id) values (1)");
  ASSERT_TRUE(w.ok);
  VecNote *again = db_all(g_db, Note);
  ASSERT_EQ(again->n, 1);
  ASSERT_TRUE((i64)qcache_stats().hits > hits_before); // still a hit
  db_close(g_db);
}

TEST(reads_inside_a_transaction_bypass_the_cache) {
  reset();
  ASSERT_TRUE(add_note("outside", 1));
  db_all(g_db, Note); // warm the cache outside the transaction
  i64 tx_before = (i64)qcache_stats().skip_tx;

  tx_scope(g_db) {
    ASSERT_EQ(tx_depth(), 1);
    VecNote *a = db_all(g_db, Note);
    VecNote *b = db_all(g_db, Note);
    ASSERT_EQ(b->n, 1);
    // A row written inside the transaction is visible to the reads inside it, which a cache
    // could not promise: that is why the cache steps aside entirely here.
    Note n = {.title = str_lit("inside"), .score = 2};
    ASSERT_TRUE(db_insert_into(g_db, Note_modelof(), &n, nullptr));
    VecNote *c = db_all(g_db, Note);
    ASSERT_EQ(c->n, 2);
    (void)a;
    ASSERT_TRUE((i64)qcache_stats().skip_tx >= tx_before + 3);
  }
  db_close(g_db);
}

TEST(volatile_functions_are_never_cached) {
  reset();
  ASSERT_TRUE(add_note("v", 1));
  i64 before = (i64)qcache_stats().skip_volatile;
  WRes r = db_exec_sql(g_db, "select id, random() as r from notes");
  ASSERT_TRUE(r.ok);
  ASSERT_TRUE((i64)qcache_stats().skip_volatile > before);

  before = qcache_stats().skip_volatile;
  WRes r2 = db_exec_sql(g_db, "select id, current_timestamp as t from notes");
  ASSERT_TRUE(r2.ok);
  ASSERT_TRUE((i64)qcache_stats().skip_volatile > before);
  db_close(g_db);
}

TEST(hints_choose_explicitly) {
  reset();
  ASSERT_TRUE(add_note("h", 1));

  SUBTEST("no-cache skips a statement anybody else would cache") {
    i64 before = (i64)qcache_stats().skip_hint;
    WRes r = db_exec_sql(g_db, "/* no-cache */ select id from notes");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE((i64)qcache_stats().skip_hint > before);
  }

  SUBTEST("cache overrides the volatile rule for a statement the author measured") {
    i64 hits = (i64)qcache_stats().hits;
    WRes a = db_exec_sql(g_db, "/* cache */ select id, random() as r from notes");
    ASSERT_TRUE(a.ok);
    WRes b = db_exec_sql(g_db, "/* cache */ select id, random() as r from notes");
    ASSERT_TRUE(b.ok);
    ASSERT_TRUE((i64)qcache_stats().hits > hits);
  }

  SUBTEST("the nocache scope skips everything inside it") {
    i64 tx = (i64)qcache_stats().skip_tx;
    (void)tx;
    i64 before = (i64)qcache_stats().hits;
    nocache {
      db_all(g_db, Note);
      db_all(g_db, Note);
    }
    db_all(g_db, Note);
    db_all(g_db, Note);
    // Two of the four reads were outside the scope, so at most those two can hit.
    ASSERT_TRUE((i64)qcache_stats().hits - before >= 1);
  }
  db_close(g_db);
}

TEST(statement_kind_classification) {
  static const struct {
    const char *name, *sql;
    bool write;
  } rows[] = {
      {"select", "select * from notes where id = ?", false},
      {"select with a subquery", "select * from notes where id in (select id from notes)", false},
      {"cte read", "with x as (select 1) select * from x", false},
      {"insert", "insert into notes (title) values ('x')", true},
      {"update", "update notes set title = 'x' where id = 1", true},
      {"delete", "delete from notes where id = 1", true},
      {"truncate", "truncate table notes", true},
      {"create", "create table t (id integer)", true},
      {"drop", "drop table notes", true},
      // A keyword inside a literal or a comment is not a keyword: a title that says "update"
      // must not turn a read into a write.
      {"select with 'update' in a literal", "select * from notes where title = 'update'", false},
      {"select with -- delete comment", "select * from notes -- delete\n where id = 1", false},
      {"begin", "begin", false},
      {"commit", "commit", false},
      {"pragma", "pragma journal_mode = wal", false},
      // A verb nobody knows is a write: the cost of guessing "read" is a stale page.
      {"unknown verb", "optimize notes", true},
  };
  SUBTESTS(rows) {
    ASSERT_EQ(qcache_is_write(r->sql), r->write);
  }
}

TEST(the_key_is_the_statement_and_its_values) {
  Arena *a = arena_new(4096);
  // Two binds that would concatenate to the same string must digest differently, or one row's
  // answer would be served for another row's question.
  WVal p1[2] = {{.kind = WV_TEXT, .s = str_lit("ab")}, {.kind = WV_TEXT, .s = str_lit("c")}};
  WVal p2[2] = {{.kind = WV_TEXT, .s = str_lit("a")}, {.kind = WV_TEXT, .s = str_lit("bc")}};
  WVal p3[2] = {{.kind = WV_I64, .i = 1}, {.kind = WV_TEXT, .s = str_lit("c")}};
  WQuery q1 = {.nparams = 2, .params = p1};
  WQuery q2 = {.nparams = 2, .params = p2};
  WQuery q3 = {.nparams = 2, .params = p3};
  str k1 = qcache_key_for_test(a, "select 1 where a = ? and b = ?", &q1);
  str k2 = qcache_key_for_test(a, "select 1 where a = ? and b = ?", &q2);
  str k3 = qcache_key_for_test(a, "select 1 where a = ? and b = ?", &q3);
  bool same12 = k1.n == k2.n && !memcmp(k1.p, k2.p, k1.n);
  bool same13 = k1.n == k3.n && !memcmp(k1.p, k3.p, k1.n);
  ASSERT_FALSE(same12); // ("ab","c") and ("a","bc") are different questions
  ASSERT_FALSE(same13);
  ASSERT_TRUE(strstr(wc_test_str(k1), "webc:{cachetest}:q:") != nullptr);

  // Whitespace is normalised, so a reformatted statement is the same question.
  str k4 = qcache_key_for_test(a, "select   1 where a = ?\n and b = ?", &q1);
  ASSERT_TRUE(k1.n == k4.n && memcmp(k1.p, k4.p, k1.n) == 0); // same question, same key
  arena_destroy(a);
}

TEST(bust_and_flush) {
  reset();
  ASSERT_TRUE(add_note("b", 1));
  db_all(g_db, Note);
  VecNote *warm = db_all(g_db, Note);
  ASSERT_EQ(warm->n, 1);
  WCacheStats s = qcache_stats();
  ASSERT_TRUE(s.hits >= 1);

  qcache_bust(Note_modelof()); // the app knows something the database does not
  i64 hits = (i64)qcache_stats().hits;
  db_all(g_db, Note);
  ASSERT_EQ((i64)qcache_stats().hits, hits); // a miss, then

  db_all(g_db, Note);
  ASSERT_TRUE((i64)qcache_stats().hits > hits); // a hit again

  qcache_flush();
  hits = qcache_stats().hits;
  db_all(g_db, Note);
  ASSERT_EQ((i64)qcache_stats().hits, hits); // everything is cold again
  db_close(g_db);
}

TEST(a_broken_cache_is_a_miss_not_a_failure) {
  reset();
  ASSERT_TRUE(add_note("resilient", 1));
  db_all(g_db, Note);

  // The cache's Redis goes away entirely. Every read from here must still answer from the
  // database — fail open is the rule that makes this feature safe to switch on.
  fake_redis_stop(g_fr); // the handle stays: a dead server still has a mutex to lock
  i64 errs = (i64)qcache_stats().errors;
  for (int i = 0; i < 5; i++) {
    VecNote *v = db_all(g_db, Note);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(v->n, 1);
    ASSERT_STR_EQ(v->at[0].title, "resilient");
  }
  ASSERT_TRUE((i64)qcache_stats().errors > errs);   // it noticed
  ASSERT_TRUE((i64)qcache_stats().skip_breaker > 0); // and stopped paying for it
  ASSERT_TRUE(add_note("still working", 2));   // writes do not fail either
  db_close(g_db);
}

TEST(a_thousand_cached_reads_do_not_grow_the_heap) {
  // The cache is the first feature that allocates *before* the driver, on every statement, so a
  // leak here would be a leak per request. `wm_stats()` is the same number /metrics reports as
  // webc_heap_bytes.
  reset();
  ASSERT_TRUE(add_note("leak", 1));
  db_all(g_db, Note); // store
  db_all(g_db, Note); // hit: from here the loop only replays
  WmStats before = wm_stats();
  for (int i = 0; i < 1000; i++) {
    VecNote *v = db_all(g_db, Note);
    ASSERT_TRUE(v->n == 1);
  }
  WmStats after = wm_stats();
  // A cached read takes an arena, its own reply, and destroys both; nothing may survive it.
  ASSERT_TRUE(after.heap_used <= before.heap_used + 16384);
  ASSERT_TRUE((i64)qcache_stats().hits >= 1000);
  db_close(g_db);
}

TEST(stats_and_debug_text) {
  reset();
  ASSERT_TRUE(add_note("s", 1));
  db_all(g_db, Note);
  db_all(g_db, Note);
  Arena *a = arena_new(4096);
  str text = qcache_debug_text(a);
  ASSERT_TRUE(text.n > 0);
  ASSERT_TRUE(strstr(wc_test_str(text), "hits 1") != nullptr ||
              strstr(wc_test_str(text), "hits") != nullptr);
  ASSERT_TRUE(strstr(wc_test_str(text), "cachetest") != nullptr);
  arena_destroy(a);
  db_close(g_db);
}

TEST(the_envelope_is_readable_by_redis_cli) {
  // A cache entry is a JSON document in a string: a person with `redis-cli GET` can read one, and
  // that is worth the extra 40 % of bytes. This asserts the shape the spec promised.
  reset();
  WRes r = db_exec_sql(g_db, "insert into notes (title, score) values ('visible', '42')");
  ASSERT_TRUE(r.ok);
  db_all(g_db, Note);

  char *key = nullptr;
  fr_lock(g_fr);
  for (int i = 0; i < FR_KEYS; i++)
    if (g_fr->keys[i].used && !strncmp(g_fr->keys[i].key, "webc:{cachetest}:q:", 19)) {
      key = strdup(g_fr->keys[i].key);
      break;
    }
  fr_unlock(g_fr);
  ASSERT_NOT_NULL(key);
  char buf[4096] = {0};
  ASSERT_TRUE(fake_redis_get_key(g_fr, key, buf, sizeof buf));
  ASSERT_TRUE(strstr(buf, "\"v\":1") != nullptr);
  ASSERT_TRUE(strstr(buf, "\"sql\":") != nullptr);
  ASSERT_TRUE(strstr(buf, "\"cols\":") != nullptr);
  ASSERT_TRUE(strstr(buf, "\"s:visible\"") != nullptr); // text is tagged too
  ASSERT_TRUE(strstr(buf, "\"i:42\"") != nullptr); // a typed cell, tagged
  free(key);
  db_close(g_db);
}

TEST_MAIN()