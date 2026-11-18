// qcache_gen: the generation memo (WEBC_CACHE_GEN_MS), the one knob that trades exactness for a
// round trip. Default 0 means every cached read checks the counters; a window means a process
// reuses the counters it last read, and pays one hop instead of two.
//
// What must never move, even with the widest window: this process's own writes invalidate at once.
// The memo is dropped on every bump, because "reads its own writes" is not a tradeoff.
#include "fake_redis.h" // first: it owns the system headers this suite needs

#include <webc/test.h>

WEBC_MODEL(Item, (id, i64, .pk = true), (label, str, .required = true))
WEBC_VEC_DEFINE(VecItem, vec_item, Item)

static FakeRedis *g_fr;
static WRedis *g_r;
static WDB *g_db;

static void setup(void) {
  g_fr = fake_redis_start();
  if (!g_fr) return;
  char url[64];
  snprintf(url, sizeof url, "redis://127.0.0.1:%u/0", (unsigned)g_fr->port);
  setenv("REDIS_URL", url, 1);
  setenv("WEBC_QUERY_CACHE", "1", 1);
  setenv("WEBC_REDIS_NAME", "gentest", 1);
  setenv("WEBC_CACHE_GEN_MS", "60000", 1); // the widest window: correctness must not depend on it
  g_r = redis_open(url);
  redis_set_default(g_r);
  qcache_config(60000, 262144);
}

static void teardown(void) {
  redis_close(g_r);
  redis_set_default(nullptr);
  fake_redis_stop(g_fr);
}

TEST_SETUP(setup);
TEST_TEARDOWN(teardown);

static void reset(void) {
  g_db = db_open("sqlite::memory:");
  ASSERT_NOT_NULL(g_db);
  WRes ddl = db_exec_sql(g_db, "create table items (id integer primary key autoincrement,"
                               " label text not null)");
  ASSERT_TRUE(ddl.ok);
  qcache_reset_counters();
  fr_lock(g_fr);
  for (int i = 0; i < FR_KEYS; i++) {
    free(g_fr->keys[i].val);
    memset(&g_fr->keys[i], 0, sizeof g_fr->keys[i]);
  }
  g_fr->commands = 0;
  fr_unlock(g_fr);
}

static bool add(const char *label) {
  Item it = {.label = str_lit(label)};
  return db_insert_into(g_db, Item_modelof(), &it, nullptr);
}

TEST(a_hit_costs_one_round_trip_once_the_memo_is_filled) {
  reset();
  ASSERT_TRUE(add("one"));
  db_all(g_db, Item); // miss + store (MGET, GET, SET, and the store's own MGET)
  VecItem *warm = db_all(g_db, Item); // hit, and the memo is filled here
  ASSERT_EQ(warm->n, 1);

  fr_lock(g_fr);
  int before = g_fr->commands;
  fr_unlock(g_fr);
  for (int i = 0; i < 5; i++) {
    VecItem *v = db_all(g_db, Item);
    ASSERT_EQ(v->n, 1);
  }
  fr_lock(g_fr);
  int used = g_fr->commands - before;
  fr_unlock(g_fr);
  // Five hits, five GETs: no MGET per read while the memo holds.
  ASSERT_EQ(used, 5);
  ASSERT_TRUE((i64)qcache_stats().hits >= 5);
}

TEST(the_apps_own_writes_still_invalidate_at_once) {
  reset();
  ASSERT_TRUE(add("first"));
  db_all(g_db, Item);
  db_all(g_db, Item); // hit, memo filled
  ASSERT_TRUE(add("second"));
  VecItem *after = db_all(g_db, Item);
  ASSERT_EQ(after->n, 2); // no window, no staleness: the bump dropped the memo
  ASSERT_TRUE((i64)qcache_stats().hits >= 1);
}

TEST(an_in_place_edit_is_seen) {
  reset();
  ASSERT_TRUE(add("before"));
  db_all(g_db, Item);
  db_all(g_db, Item);
  WRes u = db_exec_sql(g_db, "update items set label = 'after' where id = 1");
  ASSERT_TRUE(u.ok);
  VecItem *v = db_all(g_db, Item);
  ASSERT_EQ(v->n, 1);
  ASSERT_STR_EQ(v->at[0].label, "after");
}

TEST_MAIN()
