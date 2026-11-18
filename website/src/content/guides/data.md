Two things go between your app and its database: a **cache** that is invisible when it works, and
**Redis** for the state that is not a row. Neither is a dependency you have to install twice — the
sqlite driver is linked in, and the postgres, MySQL ([the third driver](../guides/mysql.html)) and Redis clients speak the
wire protocols directly.

## The query cache

Every statement in the framework goes through one function, `WRes db_exec()`, so the cache is one
function too. It stores the *result set* — the same `WRes` a driver returns — and replays it, which
means `db_all`, `db_one`, a view and a test all get a cache without knowing it.

```sh
REDIS_URL=redis://127.0.0.1:6379/0
WEBC_QUERY_CACHE=1          # off unless this is set: a cache nobody asked for is a surprise
```

Nothing to call. With those two variables a `select` is served from Redis, a write invalidates it,
and the app keeps its shape:

```c
// src/stories.c — the same calls, with a cache between the third and the fourth
#include <webc/server.h>

WEBC_MODEL(Story, (id, i64, .pk = true), (title, Text), (published, Text))
Vec(Story)

void read_twice_then_write(void) {
  WDB *db = db_default();

  // first: the database
  VecStory *top   = db_all_ordered(db, Story, "published desc");
  // second: Redis, same rows
  VecStory *again = db_all_ordered(db, Story, "published desc");

  Story s = {.title = "a story", .published = "2024-01-01"};
  // bumps stories' generation
  db_insert(db, Story, &s);

  // a miss: the write was seen
  VecStory *fresh = db_all_ordered(db, Story, "published desc");
  (void)top; (void)again; (void)fresh;
}
```

**Invalidation is a generation counter per table**, not a tag set and not `SCAN`. A write does
`INCR webc:{<app>}:g:<table>` — one hop, O(1), atomic across processes — and a cached read's key
carries the generations it was read under. When a counter moves the key changes, so the old entry is
never read again and dies of old age. There is no invalidation walk, and no thundering herd on
expiry.

**What is never cached**, because each one is a wrong answer waiting to happen:

| Skipped | Why |
| --- | --- |
| any write | they invalidate; caching a write is not a thing |
| a read inside a transaction | it must see its own writes, and it may be rolled back |
| a statement with a volatile function (`now()`, `random()`, …) | the answer is not a function of the inputs |
| `for update` / `for share` | it takes locks; replaying it takes none |
| `/* no-cache */ select …`, or anything inside `nocache { … }` | the author said no |
| a statement with no tables (`select 1`) | nothing to invalidate it with |

```c
// src/jobs.c — the three knobs, for what the automatic rules cannot see
#include <webc/server.h>
#include <webc/qcache.h>

WEBC_MODEL(Story, (id, i64, .pk = true), (title, Text))
Vec(Story)

void after_a_bulk_job(void) {
  cache_bust(Story);        // a job rewrote rows the query path never saw
  qcache_ttl(Story, 5000);  // this model's rows go stale on their own schedule
  qcache_flush();           // every entry: the hammer, for a migration
}
```

| Variable | Default | Decides |
| --- | --- | --- |
| `WEBC_QUERY_CACHE` | off | the whole feature |
| `WEBC_CACHE_TTL_MS` | 30000 | how long an entry lives |
| `WEBC_CACHE_TTL_JITTER` | 10 | percent, so a thousand entries do not expire together |
| `WEBC_CACHE_MAX_BYTES` | 262144 | a bigger result is served but not cached |
| `WEBC_CACHE_MS` | 5 | the cache's budget per statement; over it, the database answers |
| `WEBC_CACHE_GEN_MS` | 0 | 0 is exact; a window saves a hop and stops seeing other processes' writes for that long |

## Redis

```c
// src/presence.c — every Redis verb is one line, and every one takes (key, value)
#include <webc/server.h>
#include <webc/redis.h>

void touch(str token, str msg, str payload, f64 score) {
  redis_set("visits:home", "1");                     // a key
  // 2 — atomic, and the new value
  i64 n = redis_incr("visits:home");
  redis_set_ex("session:abc", token, 1800 * 1000);   // the same, with a TTL in ms
  // .p == nullptr when the key is absent
  str v = redis_get("session:abc");
  redis_del("session:abc");                          // and the key goes
  redis_hset("user:7", "theme", "dark");             // hashes
  // lists: the handle is explicit
  redis_lpush_(redis(), str_lit("queue:mail"), msg);
  redis_sadd_(redis(), str_lit("online"), str_lit("user:7"));  // sets
  redis_zadd_(redis(), str_lit("board"), score, str_lit("ada")); // sorted sets
  redis_publish("chat", payload);                    // pub/sub
  printf("%lld %.*s\n", (long long)n, (int)v.n, v.p);
}
```

`redis()` is the process-wide handle from `REDIS_URL`; `redis_open(url)` is one you own and
`redis_close()` frees. The string and counter verbs have a short form that uses the default handle;
the list, set and sorted-set verbs only have the explicit-handle form (`redis_lpush_(redis(), …)`,
which is also the form every verb has when the handle is yours). Every call is fail-open, and asking is one line:

```c
// src/health.c — asking costs one line, and the answer is never fatal
#include <webc/server.h>
#include <webc/redis.h>
#include <webc/log.h>

void check_redis(void) {
  if (!redis_ok(redis()))
    // never the password
    log_warn("redis is down", { "why", redis_error(redis()) });
}
```

The Redis connection parks like every other fd in the runtime, so a slow reply costs a task, not a
thread. Twenty commands in a row are one round trip with a pipeline; a subscription is a task:

```c
// src/chat.c — a subscription is a task, and unsubscribing joins it
#include <webc/server.h>
#include <webc/redis.h>

static void on_msg(str channel, str payload, void *arg) {
  (void)arg;
  log_info("chat", { "channel", channel }, { "payload", payload });
}

void follow(WRedis *r) {
  WSub *s = redis_subscribe(r, str_lit("chat"), on_msg, nullptr);
  if (s) redis_unsubscribe(s);   // synchronous: closes, wakes the task, joins it
}
```

## Which one is which

| Need | Use |
| --- | --- |
| a result set read a thousand times per second | the query cache (nothing to call) |
| a counter, a session, a queue, a presence set | Redis |
| work that must outlive the request | `jobs.h` — a table, a lease, a retry |
| a fan-out to every tab in a room | SSE rooms (`live.h`), with Redis pub/sub behind them when there is more than one process |

## See also

- Reference [§9 Database](../reference/database.html) — the whole of it, including the pool and
  the transaction rules.
- [Real-time](../guides/live.html) — the layer above this one.
