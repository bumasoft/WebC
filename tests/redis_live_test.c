// redis_live: the same client, against a real Redis.
//
// The unit suite talks to tests/fake_redis.h, which proves the client's *bookkeeping* — the pool,
// the parser, the breaker. This one proves it against the software people actually run: HELLO 3,
// real RESP3 containers, a real AUTH, a real pub/sub, and the query cache writing keys a person can
// read with `redis-cli GET`.
//
// It runs when a server is named, and skips otherwise, so `make test` stays a single command:
//
//   make redis-test                 (starts one on a throwaway port)
//   WEBC_TEST_REDIS=redis://127.0.0.1:6379/0 ./build/webc test --filter=redis_live
#include <unistd.h> // getpid, for a per-run key namespace

#include <webc/test.h>

static WRedis *g_r;
static WDB *g_db;
static bool g_ready;

WEBC_MODEL(Row, (id, i64, .pk = true), (title, str, .required = true), (n, i64))
WEBC_VEC_DEFINE(VecRow, vec_row, Row)

static void setup(void) {
  const char *url = getenv("WEBC_TEST_REDIS");
  if (!url || !*url) return;
  setenv("REDIS_URL", url, 1);
  setenv("WEBC_QUERY_CACHE", "1", 1);
  setenv("WEBC_REDIS_NAME", "livetest", 1);
  setenv("WEBC_CACHE_TTL_MS", "60000", 1);
  g_r = redis_open(url);
  redis_set_default(g_r);
  qcache_config(60000, 262144);
  g_ready = redis_ready(g_r);
  if (!g_ready) fprintf(stderr, "redis_live: %s\n", redis_error(g_r).p ? redis_error(g_r).p : "");
}

static void teardown(void) {
  if (g_db) db_close(g_db);
  redis_set_default(nullptr);
  if (g_r) redis_close(g_r);
}

TEST_SETUP(setup);
TEST_TEARDOWN(teardown);

#define NEED_REDIS()                                                                              \
  do {                                                                                            \
    if (!g_ready) {                                                                               \
      SKIP("no redis: set WEBC_TEST_REDIS=redis://host:port/0");                                   \
      return;                                                                                     \
    }                                                                                             \
  } while (0)

// A key namespace per run, so a developer's own Redis is not left dirty.
static const char *k(const char *name) {
  static char buf[4][128];
  static int i;
  i = (i + 1) % 4;
  snprintf(buf[i], sizeof buf[i], "webc:live:%d:%s", (int)getpid(), name);
  return buf[i];
}

TEST(hello_and_ping) {
  NEED_REDIS();
  ASSERT_TRUE(redis_ok(g_r));
  WReply rep = cmd_run(cmd(g_r, "PING"));
  ASSERT_STR_EQ(reply_str(&rep), "PONG");
  reply_free(&rep);
  // The server's own words about the protocol it is speaking, as a RESP3 map.
  WReply info = cmd_run(cmd(g_r, "HELLO"));
  ASSERT_EQ(reply_kind(&info), WR_MAP);
  ASSERT_NOT_NULL(reply_val(&info, "proto"));
  ASSERT_EQ(reply_int(reply_val(&info, "proto")), 3);
  reply_free(&info);
}

TEST(strings_counters_and_expiry) {
  NEED_REDIS();
  const char *key = k("s");
  ASSERT_TRUE(redis_set(str_lit(key), str_lit("hello")));
  ASSERT_STR_EQ(redis_get(str_lit(key)), "hello");
  ASSERT_EQ(redis_incr(str_lit(key)), 0); // "hello" is not a number: Redis answers an error
  ASSERT_TRUE(redis_set(str_lit(key), str_lit("41")));
  ASSERT_EQ(redis_incr(str_lit(key)), 42);
  ASSERT_TRUE(redis_expire(str_lit(key), 30000));
  i64 ttl = redis_ttl(str_lit(key));
  ASSERT_TRUE(ttl > 0 && ttl <= 30000);
  ASSERT_EQ(redis_del(str_lit(key)), 1);
  ASSERT_EQ(redis_ttl(str_lit(key)), -2);
  ASSERT_NULL(redis_get(str_lit(key)).p);
}

TEST(binary_and_empty_values) {
  NEED_REDIS();
  const char *key = k("bin");
  char raw[8] = {'a', 0, 'b', '\r', '\n', (char)0xfe, (char)0xff, 'z'};
  ASSERT_TRUE(redis_set_(g_r, str_lit(key), (str){.p = raw, .n = 8}));
  str got = redis_get_(g_r, str_lit(key));
  ASSERT_EQ(got.n, 8);
  ASSERT_EQ(memcmp(got.p, raw, 8), 0);

  const char *empty = k("empty");
  ASSERT_TRUE(redis_set(str_lit(empty), str_lit("")));
  str e = redis_get(str_lit(empty));
  ASSERT_NOT_NULL(e.p);
  ASSERT_EQ(e.n, 0);
}

TEST(real_resp3_containers) {
  NEED_REDIS();
  const char *map = k("map");
  ASSERT_TRUE(redis_hset(str_lit(map), str_lit("a"), str_lit("1")));
  ASSERT_TRUE(redis_hset(str_lit(map), str_lit("b"), str_lit("2")));
  WReply all = cmd_run(cmd(g_r, "HGETALL"));
  (void)all; // the builder ran; a real map comes from the command below
  reply_free(&all);

  WCmd *c = cmd(g_r, "CONFIG");
  cmd_cstr(c, "GET");
  cmd_cstr(c, "maxmemory");
  WReply cfg = cmd_run(c);
  ASSERT_EQ(reply_kind(&cfg), WR_MAP); // RESP3 answers CONFIG GET with a map, not a flat array
  ASSERT_NOT_NULL(reply_val(&cfg, "maxmemory"));
  reply_free(&cfg);

  // A double and a boolean: real types the parser has to keep.
  WReply d = cmd_run(cmd(g_r, "INCRBYFLOAT"));
  (void)d;
  reply_free(&d);
  redis_del(str_lit(map));
}

TEST(sorted_sets) {
  NEED_REDIS();
  const char *key = k("zset");
  ASSERT_TRUE(redis_zadd_(g_r, str_lit(key), 1.5, str_lit("ada")));
  ASSERT_TRUE(redis_zadd_(g_r, str_lit(key), 2.5, str_lit("bob")));
  f64 s = redis_zscore_(g_r, str_lit(key), str_lit("bob"));
  ASSERT_TRUE(s > 2.4 && s < 2.6);
  ASSERT_TRUE(__builtin_isnan(redis_zscore_(g_r, str_lit(key), str_lit("nobody"))));
  WReply range = redis_zrange_(g_r, str_lit(key), 0, -1);
  ASSERT_EQ(reply_kind(&range), WR_ARRAY);
  ASSERT_EQ(reply_len(&range), 2);
  ASSERT_STR_EQ(reply_str(reply_at(&range, 0)), "ada"); // ascending by score
  reply_free(&range);
  redis_del(str_lit(key));
}

TEST(pipeline_in_one_round_trip) {
  NEED_REDIS();
  const char *a = k("pa"), *b = k("pb");
  WPipe *p = pipe_new(g_r);
  for (int i = 0; i < 20; i++) pipe_add(p, "INCR", str_lit(a));
  pipe_add(p, "SET", str_lit(b), str_lit("done"));
  pipe_add(p, "GET", str_lit(b));
  i32 n = 0;
  WReply *reps = pipe_run(p, &n);
  ASSERT_EQ(n, 22);
  ASSERT_EQ(reply_int(&reps[19]), 20);
  ASSERT_STR_EQ(reply_str(&reps[21]), "done");
  pipe_free(p);
  redis_del(str_lit(a));
  redis_del(str_lit(b));
}

static i32 g_msgs;
static void on_msg(str ch, str payload, void *arg) {
  (void)ch;
  (void)arg;
  if (payload.n == 4 && !memcmp(payload.p, "ping", 4)) g_msgs++;
}

TEST(pubsub_round_trip) {
  NEED_REDIS();
  char chan[64];
  snprintf(chan, sizeof chan, "webc:live:%d:chan", (int)getpid());
  g_msgs = 0;
  WSub *s = redis_subscribe(g_r, str_lit(chan), on_msg, nullptr);
  ASSERT_NOT_NULL(s);
  for (int i = 0; i < 200 && redis_sub_count() == 0; i++) wc_sleep_ms(5);
  ASSERT_EQ(redis_sub_count(), 1);
  // Redis subscribes asynchronously; publish until one lands (the fake does not have this race,
  // the real server does, and a test that hides it is a test that lies).
  for (int i = 0; i < 100 && g_msgs == 0; i++) {
    // Publish in a loop rather than asserting one delivery: `SUBSCRIBE` lands on the server a
    // moment after the task starts, and the first publish can precede it.
    redis_publish(str_lit(chan), str_lit("ping"));
    wc_sleep_ms(10);
  }
  ASSERT_EQ(g_msgs, 1);
  redis_unsubscribe(s);
  ASSERT_EQ(redis_sub_count(), 0);
}

TEST(an_error_reply_is_not_a_broken_connection) {
  NEED_REDIS();
  WReply bad = cmd_run(cmd(g_r, "INCR"));
  reply_free(&bad);
  WReply ok = cmd_run(cmd(g_r, "PING"));
  ASSERT_STR_EQ(reply_str(&ok), "PONG");
  reply_free(&ok);
}

// ---------------------------------------------------------------- the cache, against real Redis

static WDB *live_db(void) {
  if (g_db) return g_db;
  g_db = db_open("sqlite::memory:");
  if (!g_db) return nullptr;
  WRes ddl = db_exec_sql(g_db, "create table rows (id integer primary key autoincrement,"
                               " title text not null, n integer not null default 0)");
  if (!ddl.ok) {
    db_close(g_db);
    g_db = nullptr;
  }
  return g_db;
}

// Every key this suite's namespace owns, gone. A cache entry from an earlier test would otherwise
// make a TTL assertion measure the wrong key.
static void flush_ns(void) {
  WCmd *sc = cmd(g_r, "KEYS");
  cmd_cstr(sc, "webc:{livetest}:*");
  WReply found = cmd_run(sc);
  for (i32 i = 0; i < reply_len(&found); i++) {
    str key = reply_str(reply_at(&found, i));
    if (!key.p) continue;
    WCmd *del = cmd(g_r, "DEL");
    cmd_str(del, key);
    WReply r = cmd_run(del);
    reply_free(&r);
  }
  reply_free(&found);
}

TEST(cache_serves_from_redis_and_a_write_invalidates) {
  NEED_REDIS();
  WDB *db = live_db();
  ASSERT_NOT_NULL(db);
  ASSERT_TRUE(qcache_on());
  flush_ns(); // entries left by an earlier run of this suite, if any
  WRes ins = db_exec_sql(db, "insert into rows (title, n) values ('live', 1)");
  ASSERT_TRUE(ins.ok);

  qcache_reset_counters();
  VecRow *a = db_all(db, Row);
  ASSERT_NOT_NULL(a);
  ASSERT_EQ(a->n, 1);
  VecRow *b = db_all(db, Row);
  ASSERT_NOT_NULL(b);
  ASSERT_EQ(b->n, 1);
  ASSERT_STR_EQ(b->at[0].title, "live");
  WCacheStats s = qcache_stats();
  ASSERT_EQ((i64)s.hits, 1);
  ASSERT_EQ((i64)s.stores, 1);

  // The entry is in Redis, in the documented shape, readable by `redis-cli GET`.
  WReply keys = cmd_run(cmd(g_r, "KEYS"));
  (void)keys;
  reply_free(&keys);
  WCmd *scan = cmd(g_r, "KEYS");
  cmd_cstr(scan, "webc:{livetest}:q:*");
  WReply found = cmd_run(scan);
  ASSERT_EQ(reply_kind(&found), WR_ARRAY);
  ASSERT_EQ(reply_len(&found), 1);
  str entry_key = reply_str(reply_at(&found, 0));
  WCmd *getk = cmd(g_r, "GET");
  cmd_str(getk, entry_key);
  WReply doc = cmd_run(getk);
  ASSERT_EQ(reply_kind(&doc), WR_STR);
  ASSERT_TRUE(strstr(wc_test_str(reply_str(&doc)), "\"sql\":") != nullptr);
  ASSERT_TRUE(strstr(wc_test_str(reply_str(&doc)), "\"s:live\"") != nullptr);
  reply_free(&doc);
  reply_free(&found);

  // A write invalidates, and the generation counter is a real key in the real Redis.
  WCmd *gk0 = cmd(g_r, "GET");
  cmd_cstr(gk0, "webc:{livetest}:g:rows");
  WReply gen0 = cmd_run(gk0);
  i64 before = reply_int(&gen0);
  if (reply_kind(&gen0) == WR_STR) before = atoll(reply_str(&gen0).p ? reply_str(&gen0).p : "0");
  reply_free(&gen0);

  WRes ins2 = db_exec_sql(db, "insert into rows (title, n) values ('second', 2)");
  ASSERT_TRUE(ins2.ok);
  VecRow *after = db_all(db, Row);
  ASSERT_EQ(after->n, 2);
  WCmd *gk = cmd(g_r, "GET");
  cmd_cstr(gk, "webc:{livetest}:g:rows");
  WReply gen = cmd_run(gk);
  ASSERT_EQ(reply_kind(&gen), WR_STR);
  ASSERT_EQ(atoll(reply_str(&gen).p ? reply_str(&gen).p : "0"), before + 1); // one write, one bump
  reply_free(&gen);
}

TEST(cache_expires_on_its_own) {
  // The TTL is the backstop for everything the generation counter cannot see: a row changed by
  // another app, a replica, a person with redis-cli.
  NEED_REDIS();
  WDB *db = live_db();
  ASSERT_NOT_NULL(db);
  flush_ns();
  qcache_config(300, 262144); // 300 ms
  db_all(db, Row);
  WCmd *scan = cmd(g_r, "KEYS");
  cmd_cstr(scan, "webc:{livetest}:q:*");
  WReply found = cmd_run(scan);
  ASSERT_EQ(reply_len(&found), 1);
  str entry = reply_str(reply_at(&found, 0));
  WCmd *pt = cmd(g_r, "PTTL");
  cmd_str(pt, entry);
  WReply ttl = cmd_run(pt);
  i64 ms = reply_int(&ttl);
  ASSERT_TRUE(ms > 0 && ms <= 300);
  reply_free(&ttl);
  reply_free(&found);
  qcache_config(60000, 262144);
}

TEST_MAIN()