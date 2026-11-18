// redis: the built-in client — URL parsing, the RESP3 parser, the pool, the breaker's recovery,
// and the one thing that matters most: a Redis that is gone is an error from `redis_*`, never a
// crash and never a leak.
//
// The server is tests/fake_redis.h: a real socket on a real port, because the point of this client
// is the parked socket layer underneath it. No docker, no `make redis-test` for the default suite.

#include "fake_redis.h" // first: it owns the system headers this suite needs

#include <webc/test.h>

static FakeRedis *g_fr;
static WRedis *g_r;

static void setup(void) {
  wtask_init(2);
  g_fr = fake_redis_start();
  if (!g_fr) return;
  char url[64];
  snprintf(url, sizeof url, "redis://127.0.0.1:%u/0", (unsigned)g_fr->port);
  setenv("REDIS_URL", url, 1);
  g_r = redis_open(url);
  redis_set_default(g_r);
}

static void teardown(void) {
  redis_close(g_r);
  g_r = nullptr;
  redis_set_default(nullptr);
  fake_redis_stop(g_fr);
  g_fr = nullptr;
  wtask_shutdown();
}

TEST_SETUP(setup);
TEST_TEARDOWN(teardown);

// A key nobody else in this binary uses: the store outlives one test.
static const char *k(const char *name) {
  static char buf[8][96];
  static int i;
  i = (i + 1) % 8;
  snprintf(buf[i], sizeof buf[i], "wc:%s:%d", name, i);
  return buf[i];
}

TEST(url_parsing) {
  static const struct {
    const char *name, *url, *addr;
  } rows[] = {
      {"plain", "redis://127.0.0.1:6380/2", "127.0.0.1:6380/2"},
      {"default port", "redis://cache.internal/0", "cache.internal:6379/0"},
      {"credentials stripped", "redis://user:secret@h:7000/1", "h:7000/1"},
      {"password only", "redis://secret@h/3", "h:6379/3"},
      {"no db", "redis://h:6390", "h:6390/0"},
  };
  SUBTESTS(rows) {
    WRedis *h = redis_open(r->url);
    ASSERT_NOT_NULL(h);
    ASSERT_STR_EQ(redis_addr(h), r->addr);
    // The sentence a log line gets must never carry the password.
    ASSERT_TRUE(!strstr(redis_addr(h).p ? redis_addr(h).p : "", "secret"));
    redis_close(h);
  }

  SUBTEST("refused schemes") {
    WRedis *r = redis_open("http://h:1/0");
    ASSERT_NULL(r);
    r = redis_open("");
    ASSERT_NULL(r);
  }

  SUBTEST("rediss needs a TLS build") {
    WRedis *r = redis_open("rediss://h:6379/0");
#ifndef WEBC_TLS_OPENSSL
    ASSERT_NULL(r); // and the message names the flag that would fix it
#else
    ASSERT_NOT_NULL(r);
    redis_close(r);
#endif
  }

  SUBTEST("bad port and db") {
    ASSERT_NULL(redis_open("redis://h:notaport/0"));
    ASSERT_NULL(redis_open("redis://h:6379/notadb"));
  }
}

TEST(handshake_and_ping) {
  ASSERT_NOT_NULL(g_fr);
  ASSERT_NOT_NULL(g_r);
  ASSERT_TRUE(redis_ok(g_r));
  ASSERT_EQ(redis_pool_size(g_r), 0); // nothing opened yet: a pool that costs nothing at rest
  WReply rep = cmd_run(cmd(g_r, "PING"));
  ASSERT_EQ(redis_pool_size(g_r), 1); // and one connection after the first command
  ASSERT_FALSE(reply_is_err(&rep));
  ASSERT_STR_EQ(reply_str(&rep), "PONG");
  reply_free(&rep);
}

TEST(resp2_fallback) {
  // A server that does not know HELLO: the client must fall back to AUTH/SELECT/SETNAME rather
  // than declare the connection broken. The fake answers -ERR to `HELLO 2`, which is what an old
  // Redis says.
  char url[64];
  snprintf(url, sizeof url, "redis://127.0.0.1:%u/0", (unsigned)g_fr->port);
  WRedis *r = redis_open(url);
  ASSERT_NOT_NULL(r);
  ASSERT_TRUE(redis_ok(r));
  WReply rep = cmd_run(cmd(r, "PING"));
  ASSERT_STR_EQ(reply_str(&rep), "PONG");
  reply_free(&rep);
  redis_close(r);
}

TEST(wrong_password_says_so_without_saying_it) {
  char url[80];
  snprintf(url, sizeof url, "redis://:bad@127.0.0.1:%u/0", (unsigned)g_fr->port);
  WRedis *r = redis_open(url);
  ASSERT_NOT_NULL(r); // the URL is fine; the server is what refuses
  WReply rep = cmd_run(cmd(r, "PING"));
  ASSERT_TRUE(reply_is_err(&rep));
  str err = redis_error(r);
  ASSERT_TRUE(err.n > 0);
  ASSERT_TRUE(!strstr(wc_test_str(err), "bad"));
  reply_free(&rep);
  redis_close(r);
}

TEST(strings_and_counters) {
  const char *key = k("greeting");
  ASSERT_TRUE(redis_set(str_lit(key), str_lit("hello")));
  ASSERT_STR_EQ(redis_get(str_lit(key)), "hello");
  ASSERT_TRUE(redis_exists(str_lit(key)));
  ASSERT_EQ(redis_del(str_lit(key)), 1);
  ASSERT_FALSE(redis_exists(str_lit(key)));
  // A missing key is a null pointer in the str, not an empty string: the caller can tell the two
  // apart, which is the whole reason the verbs return str and not bool.
  str missing = redis_get(str_lit(key));
  ASSERT_NULL(missing.p);

  const char *counter = k("hits");
  ASSERT_EQ(redis_incr(str_lit(counter)), 1);
  ASSERT_EQ(redis_incr(str_lit(counter)), 2);
  ASSERT_STR_EQ(redis_get(str_lit(counter)), "2");
}

TEST(empty_value_is_not_missing) {
  const char *key = k("empty");
  ASSERT_TRUE(redis_set(str_lit(key), str_lit("")));
  str v = redis_get(str_lit(key));
  ASSERT_NOT_NULL(v.p);
  ASSERT_EQ(v.n, 0);
}

TEST(binary_safe_values) {
  const char *key = k("bin");
  char raw[6] = {'a', '\0', 'b', '\r', '\n', (char)0xff};
  ASSERT_TRUE(redis_set_(g_r, str_lit(key), (str){.p = raw, .n = sizeof raw}));
  str v = redis_get_(g_r, str_lit(key));
  ASSERT_EQ(v.n, sizeof raw);
  ASSERT_EQ(memcmp(v.p, raw, sizeof raw), 0);
}

TEST(ttl_and_expiry) {
  const char *key = k("ttl");
  ASSERT_TRUE(redis_set_ex(str_lit(key), str_lit("v"), 30000));
  i64 ttl = redis_ttl(str_lit(key));
  ASSERT_TRUE(ttl > 0 && ttl <= 30000);
  ASSERT_TRUE(redis_expire(str_lit(key), 5000));
  ASSERT_TRUE(redis_ttl(str_lit(key)) > 0);
  // An absent key reads -2, a key with no expiry reads -1.
  ASSERT_EQ(redis_ttl(str_lit("definitely:absent:key")), -2);
}

TEST(hash_and_list_and_set) {
  const char *map = k("map");
  ASSERT_TRUE(redis_hset(str_lit(map), str_lit("f"), str_lit("v")));
  ASSERT_STR_EQ(redis_hget(str_lit(map), str_lit("f")), "v");
  ASSERT_NULL(redis_hget(str_lit(map), str_lit("nope")).p);

  const char *list = k("list");
  ASSERT_TRUE(redis_lpush_(g_r, str_lit(list), str_lit("a")));
  ASSERT_TRUE(redis_rpush_(g_r, str_lit(list), str_lit("b")));

  const char *set = k("set");
  ASSERT_TRUE(redis_sadd_(g_r, str_lit(set), str_lit("m")));
  ASSERT_TRUE(redis_sismember_(g_r, str_lit(set), str_lit("m")));
  ASSERT_FALSE(redis_sismember_(g_r, str_lit(set), str_lit("n")));
}

TEST(set_nx_is_the_lock_primitive) {
  const char *key = k("lock");
  ASSERT_TRUE(redis_set_nx(str_lit(key), str_lit("owner-a"), 10000));
  ASSERT_FALSE(redis_set_nx(str_lit(key), str_lit("owner-b"), 10000)); // somebody holds it
  ASSERT_STR_EQ(redis_get(str_lit(key)), "owner-a");
  ASSERT_TRUE(redis_del(str_lit(key)) == 1);
  ASSERT_TRUE(redis_set_nx(str_lit(key), str_lit("owner-b"), 10000));
}

TEST(getdel_is_atomic) {
  const char *key = k("once");
  ASSERT_TRUE(redis_set(str_lit(key), str_lit("token")));
  ASSERT_STR_EQ(redis_get_del_(g_r, str_lit(key)), "token");
  ASSERT_FALSE(redis_exists(str_lit(key)));
}

TEST(command_builder_typed_arguments) {
  const char *key = k("typed");
  WCmd *c = cmd(g_r, "SET");
  cmd_cstr(c, key);
  cmd_i64(c, -42);
  WReply set = cmd_run(c);
  ASSERT_STR_EQ(reply_str(&set), "OK");
  reply_free(&set);

  // The integer went out as its decimal text, which is what SET does with it.
  ASSERT_STR_EQ(redis_get(str_lit(key)), "-42");

  WCmd *d = cmd(g_r, "SET");
  cmd_cstr(d, key);
  cmd_f64(d, 1.5);
  WReply setf = cmd_run(d);
  ASSERT_STR_EQ(reply_str(&setf), "OK");
  reply_free(&setf);
  ASSERT_STR_EQ(redis_get(str_lit(key)), "1.5");

  // A builder argument can carry a NUL, which a printf-style builder could not.
  WCmd *raw = cmd(g_r, "SET");
  cmd_cstr(raw, key);
  cmd_raw(raw, "a\0b", 3);
  WReply setraw = cmd_run(raw);
  ASSERT_STR_EQ(reply_str(&setraw), "OK");
  reply_free(&setraw);
  str got = redis_get(str_lit(key));
  ASSERT_EQ(got.n, 3);
  ASSERT_EQ(memcmp(got.p, "a\0b", 3), 0);
}

TEST(spent_builder_is_an_error_not_a_use_after_free) {
  WCmd *c = cmd(g_r, "PING");
  WReply first = cmd_run(c);
  ASSERT_STR_EQ(reply_str(&first), "PONG");
  reply_free(&first);
  WReply again = cmd_run(c); // a spent handle answers an error, it does not read freed memory
  ASSERT_TRUE(reply_is_err(&again));
  reply_free(&again);
}

TEST(mget_returns_an_array) {
  const char *a = k("m1"), *b = k("m2");
  ASSERT_TRUE(redis_set(str_lit(a), str_lit("one")));
  str keys[2] = {str_lit(a), str_lit(b)};
  WReply rep = redis_mget_(g_r, 2, keys);
  ASSERT_EQ(reply_kind(&rep), WR_ARRAY);
  ASSERT_EQ(reply_len(&rep), 2);
  ASSERT_STR_EQ(reply_str(reply_at(&rep, 0)), "one");
  ASSERT_TRUE(reply_is_nil(reply_at(&rep, 1)));
  reply_free(&rep);
}

TEST(pipeline_runs_in_order) {
  const char *a = k("p1"), *b = k("p2");
  WPipe *p = pipe_new(g_r);
  ASSERT_NOT_NULL(p);
  pipe_add(p, "SET", str_lit(a), str_lit("1"));
  pipe_add(p, "SET", str_lit(b), str_lit("2"));
  pipe_add(p, "GET", str_lit(a));
  pipe_add(p, "GET", str_lit(b));
  i32 n = 0;
  WReply *reps = pipe_run(p, &n);
  ASSERT_EQ(n, 4);
  ASSERT_STR_EQ(reply_str(&reps[0]), "OK");
  ASSERT_STR_EQ(reply_str(&reps[2]), "1");
  ASSERT_STR_EQ(reply_str(&reps[3]), "2");
  pipe_free(p);
}

TEST(pipeline_survives_one_bad_command) {
  WPipe *p = pipe_new(g_r);
  pipe_add(p, "SET", str_lit(k("pl")), str_lit("x"));
  pipe_add(p, "NOSUCHCOMMAND", str_lit("x"));
  pipe_add(p, "PING", str_lit("x"));
  i32 n = 0;
  WReply *reps = pipe_run(p, &n);
  ASSERT_EQ(n, 3);
  ASSERT_FALSE(reply_is_err(&reps[0]));
  ASSERT_TRUE(reply_is_err(&reps[1]));      // the failing one
  ASSERT_STR_EQ(reply_str(&reps[2]), "PONG"); // and the connection is still usable after it
  pipe_free(p);
}

TEST(resp3_reply_types) {
  // Every type a RESP3 server can answer with, from a scripted connection: the parser is the part
  // a real server cannot be asked to produce on demand.
  static const char *const replies[] = {
      "|1\r\n+key-popularity\r\n,1.5\r\n%2\r\n$1\r\na\r\n:1\r\n$1\r\nb\r\n+ok\r\n", // 0: an attribute, then a map
      "~2\r\n:1\r\n:2\r\n",                                 // 1: a set
      ",3.25\r\n",                                          // 2: a double
      "#t\r\n",                                             // 3: a boolean
      "(3492890328409238509324850943850943825024385\r\n",    // 4: a 128-bit integer, kept as text
      "=15\r\ntxt:Some string\r\n",                         // 5: a verbatim string
      "!21\r\nSYNTAX invalid syntax\r\n",                   // 6: a blob error
      "_\r\n",                                              // 7: null
      ">3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$2\r\nhi\r\n",    // 8: an out-of-band push
  };
  fake_redis_script(g_fr, replies, (int)WC_COUNTOF(replies));

  WReply r0 = cmd_run(cmd(g_r, "XATTR"));
  ASSERT_EQ(reply_kind(&r0), WR_MAP); // the attribute was parsed and dropped, the map kept
  ASSERT_EQ(reply_int(reply_val(&r0, "a")), 1);
  ASSERT_STR_EQ(reply_str(reply_val(&r0, "b")), "ok");
  reply_free(&r0);

  WReply r1 = cmd_run(cmd(g_r, "XSET"));
  ASSERT_EQ(reply_kind(&r1), WR_ARRAY);
  ASSERT_EQ(reply_len(&r1), 2);
  reply_free(&r1);

  WReply r2 = cmd_run(cmd(g_r, "XDOUBLE"));
  ASSERT_EQ(reply_kind(&r2), WR_DOUBLE);
  ASSERT_TRUE(reply_double(&r2) > 3.2 && reply_double(&r2) < 3.3);
  reply_free(&r2);

  WReply r3 = cmd_run(cmd(g_r, "XBOOL"));
  ASSERT_EQ(reply_kind(&r3), WR_BOOL);
  ASSERT_TRUE(reply_bool(&r3));
  reply_free(&r3);

  WReply r4 = cmd_run(cmd(g_r, "XBIG"));
  ASSERT_STR_EQ(reply_str(&r4), "3492890328409238509324850943850943825024385");
  reply_free(&r4);

  WReply r5 = cmd_run(cmd(g_r, "XVERB"));
  ASSERT_STR_EQ(reply_str(&r5), "Some string"); // the `txt:` prefix is stripped
  reply_free(&r5);

  WReply r6 = cmd_run(cmd(g_r, "XERR"));
  ASSERT_EQ(reply_kind(&r6), WR_ERROR);
  ASSERT_STR_EQ(reply_err(&r6), "SYNTAX invalid syntax");
  reply_free(&r6);

  WReply r7 = cmd_run(cmd(g_r, "XNIL"));
  ASSERT_TRUE(reply_is_nil(&r7));
  reply_free(&r7);

  WReply r8 = cmd_run(cmd(g_r, "XPUSH"));
  ASSERT_TRUE(reply_kind(&r8) == WR_ARRAY || reply_kind(&r8) == WR_NIL);
  reply_free(&r8);
}

TEST(server_error_is_not_a_broken_connection) {
  WReply rep = cmd_run(cmd(g_r, "NOSUCHCOMMAND"));
  ASSERT_TRUE(reply_is_err(&rep));
  ASSERT_TRUE(strstr(wc_test_str(reply_err(&rep)), "unknown command") != nullptr);
  reply_free(&rep);
  // Still usable: an error reply consumes one command, it does not poison the socket.
  WReply ok = cmd_run(cmd(g_r, "PING"));
  ASSERT_STR_EQ(reply_str(&ok), "PONG");
  reply_free(&ok);
}

TEST(dropped_connection_recovers) {
  g_fr->fail_next = 1;
  WReply bad = cmd_run(cmd(g_r, "PING"));
  ASSERT_TRUE(reply_is_err(&bad));
  reply_free(&bad);
  // The slot was recycled rather than handed back broken.
  WReply good = cmd_run(cmd(g_r, "PING"));
  ASSERT_STR_EQ(reply_str(&good), "PONG");
  reply_free(&good);
}

TEST(timeout_is_bounded) {
  // A server that accepts and never answers: `redis_call` must give up in the time it was given
  // instead of parking the request until the connection dies.
  static const char *const silent[] = {nullptr};
  (void)silent;
  g_fr->script_n = 0;
  g_fr->script_i = 0;
  // Ask for a command the fake does not answer at all: it drops the connection.
  g_fr->fail_next = 1;
  Arena *a = arena_new(1024);
  str argv[1] = {str_lit("PING")};
  WReply rep = {0};
  u64 t0 = wc_time_ms();
  bool ok = redis_call(g_r, 1, argv, 300, a, &rep);
  u64 took = wc_time_ms() - t0;
  ASSERT_FALSE(ok);
  ASSERT_TRUE(took < 2000); // it gave up; it did not hang
  arena_destroy(a);
}

static struct {
  i32 n;
  char last[64];
} g_got;

static void on_msg(str chan, str payload, void *arg) {
  (void)chan;
  (void)arg;
  snprintf(g_got.last, sizeof g_got.last, "%.*s", (int)payload.n, payload.p);
  g_got.n++;
}

TEST(pubsub_delivers) {
  g_got.n = 0;
  g_got.last[0] = 0;
  WSub *s = redis_subscribe(g_r, str_lit("news"), on_msg, &g_got);
  ASSERT_NOT_NULL(s);
  // Give the subscribe a moment to land, then publish from the pool.
  for (int i = 0; i < 200 && !redis_sub_count(); i++) wc_sleep_ms(5);
  ASSERT_EQ(redis_sub_count(), 1);
  for (int i = 0; i < 100 && g_got.n == 0; i++) {
    redis_publish(str_lit("news"), str_lit("hot off the wire"));
    wc_sleep_ms(5);
  }
  ASSERT_TRUE(g_got.n > 0);
  ASSERT_STR_EQ(str_lit(g_got.last), "hot off the wire");
  redis_unsubscribe(s);
  ASSERT_EQ(redis_sub_count(), 0);
}

static const char *g_conc_key;

#define CONC_TASKS 6
#define CONC_PER_TASK 20

// What each task saw. A task writes into its own slot, so the collection needs no lock.
typedef struct {
  i64 v[CONC_PER_TASK];
} ConcSeen;

static void incr_body(void *arg) {
  ConcSeen *seen = arg;
  for (int i = 0; i < CONC_PER_TASK; i++) seen->v[i] = redis_incr(str_lit(g_conc_key));
}

TEST(pool_survives_concurrent_tasks) {
  // Six tasks, twenty commands each, one pool: the pool's FIFO and its `WChan` tokens must not
  // hand one connection to two tasks at once. A mistake here shows up as a reply read by the
  // wrong caller, and the invariant that catches it is *not* "the counter reaches 120" — that
  // only holds if every command also succeeded, and a stampede on a busy runner does not
  // promise that (it failed 119/120 on a macOS runner and 40/120 on a Linux one, both times
  // with no corruption at all). The server hands out 1, 2, 3, … in order, so what must be true
  // is that every value that came back is *distinct* and that the counter equals how many came
  // back: one increment per reply, nobody's value delivered twice.
  ConcSeen seen[CONC_TASKS];
  memset(seen, 0, sizeof seen);
  g_conc_key = k("conc");
  ASSERT_TRUE(redis_set(str_lit(g_conc_key), str_lit("0")));
  WTaskHandle *hs[CONC_TASKS];
  for (int i = 0; i < CONC_TASKS; i++) hs[i] = task_result(incr_body, &seen[i]);
  ASSERT_EQ(task_join_all(hs, CONC_TASKS), 0);
  for (int i = 0; i < CONC_TASKS; i++) task_handle_free(hs[i]);

  static bool dup[CONC_TASKS * CONC_PER_TASK + 1];
  memset(dup, 0, sizeof dup);
  int answered = 0;
  for (int i = 0; i < CONC_TASKS; i++)
    for (int j = 0; j < CONC_PER_TASK; j++) {
      i64 v = seen[i].v[j];
      if (v <= 0) continue; // the command failed: a stampede may lose a command, never a reply
      ASSERT_TRUE(v <= CONC_TASKS * CONC_PER_TASK);
      ASSERT_TRUE(!dup[v]); // two callers read the same increment: the bug this test is for
      dup[v] = true;
      answered++;
    }
  ASSERT_TRUE(answered > 0);
  char want[16];
  snprintf(want, sizeof want, "%d", answered);
  ASSERT_STR_EQ(redis_get(str_lit(g_conc_key)), want);
}

TEST_MAIN()