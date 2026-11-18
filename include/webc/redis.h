// SPDX-License-Identifier: Apache-2.0
// webc/redis.h — Redis, built in. RESP3 over the same parked sockets the database uses (SPEC §9c).
//
// Postgres is spoken by src/db_pg.c with no libpq; Redis is spoken here with no hiredis, for the
// same reason: a client library is a dependency, a version floor and a build flag, and RESP is a
// protocol the size of a driver's reply parser. Every byte goes through wfd_open_connect/wfd_read/
// wfd_write (task.h), so a Redis round trip parks the task and hands the worker to somebody else —
// the difference between "ten workers, ten commands in flight" and "ten workers, a thousand".
//
//   WRedis *r = redis();                       // opened from REDIS_URL on first use
//   if (!redis_ok(r)) log_warn("redis", {"why", redis_error(r)});
//
//   bool ok = redis_set("greeting", "hello");
//   str  v  = redis_get("greeting");           // .p == nullptr when the key is absent
//   i64  n  = redis_incr("hits:today");
//
// The long tail is one builder away — the q()/db_exec() contract, one type over:
//
//   WCmd *c = cmd(r, "HSET");
//   cmd_str(c, str_lit("user:1")); cmd_str(c, str_lit("email")); cmd_str(c, email);
//   WReply rep = cmd_run(c);                   // one round trip; `c` is spent afterwards
//   if (rep.kind == WR_ERROR) log_error("redis", {"why", rep.s});
//
// A nil is not the empty string: `redis_get("missing")` has `.p == nullptr`, `redis_get("empty")`
// has `.p != nullptr && .n == 0`. A cache that cannot tell those apart serves an empty page for a
// missing row, which is why the reply type keeps them apart instead of folding both into "".
//
// **One connection per command.** A command borrows a pooled connection, writes, reads its reply
// and gives the connection back. A pipeline (one round trip, n replies) borrows it for the whole
// run. That means a task never holds two, so a nested call cannot deadlock a pool of one — the
// alternative (pin a connection to the task for the request) is what the database does, and it is
// not needed here because nothing in the protocol has session state a Redis caller cares about.
//
// **Refuses:** Sentinel, Cluster and redirection; EVAL/Lua and Modules; Streams and consumer groups
// (webc/jobs.h is the queue, and its table is the design); CLIENT TRACKING as an invalidation
// channel; a distributed rate limiter (webc/limits.h documents why that is its own spec); a
// per-process L1 in front of Redis (a cache inside the process cannot be invalidated by the write
// that happened in another process); locks, leader election and cache warming.
#pragma once

#include <webc/core.h>
#include <webc/pp.h>   // WC_NARG/WC_CAT: the handle-first overloads below
#include <webc/task.h>

typedef struct WRedis WRedis;
typedef struct WCmd WCmd;
typedef struct WPipe WPipe;
typedef struct WSub WSub;

// A connection is borrowed for one command, so the pool is a compile-time array of 64 slots: 64
// pointers, and no way for a misconfiguration to open 4 000 sockets to a server whose maxclients is
// 10 000 and whose memory is not.
#define WC_REDIS_POOL_MAX 64
#define WC_REDIS_MAX_BULK (64u * 1024u * 1024u)
#define WC_REDIS_MAX_DEPTH 32
#define WC_REDIS_MAX_ARGS 32   // arguments in one command (a pipeline has no such limit)

// ---------------------------------------------------------------- configuration
//
// redis://[user[:password]@]host[:port][/db][?param=value&…], or rediss:// for TLS. Recognised
// parameters: timeout_ms, acquire_ms, pool, db, tls=verify|insecure, ca=<path>, name=<client name>.
// A parameter wins over the environment, and the environment over the defaults (REDIS_URL,
// WEBC_REDIS_POOL, WEBC_REDIS_ACQUIRE_MS, WEBC_REDIS_TIMEOUT_MS, WEBC_REDIS_NAME, WEBC_REDIS_TLS,
// WEBC_REDIS_CA).
//
// A rediss:// URL in a build without -DWEBC_TLS=openssl is refused at open with a message naming
// the flag: sending a password in the clear to a port somebody believes is encrypted is worse than
// not starting.
WRedis *redis_open(const char *url);
void    redis_close(WRedis *r);

// The process default: opened from REDIS_URL the first time it is asked for, so an app that never
// touches Redis never opens a socket. Unlike db_default() a failure here does NOT panic — the
// handle exists and every call on it fails with redis_error(), because a cache must be able to be
// down. An app that cannot live without Redis says so itself:
//
//   if (!redis_ok(redis())) panic("this app needs Redis: %s", redis_error(redis()).p);
WRedis *redis(void);
void    redis_set_default(WRedis *r);

bool  redis_ok(WRedis *r);            // configured, and the last operation did not fail
// Configured, whether or not the last call worked: "may I try?", the question the query cache
// asks. The breaker decides how often (§9d).
bool  redis_ready(WRedis *r);
str   redis_error(WRedis *r);         // why not, as a sentence — never the password
str   redis_addr(WRedis *r);          // "host:port/db", no credentials

// How many connections the pool may open (default: one per worker, capped at WC_REDIS_POOL_MAX)
// and how long a task waits for one before the call fails with a message naming the pool size.
void  redis_pool_config(i32 max_conns, i64 acquire_ms);
i32   redis_pool_size(WRedis *r);     // connections actually open

// ---------------------------------------------------------------- replies

typedef enum WReplyKind : u8 {
  WR_NIL,     // RESP3 `_`, or RESP2 `$-1`/`*-1`
  WR_STATUS,  // `+OK`
  WR_STR,     // `$…`, and RESP3 verbatim `=`
  WR_INT,     // `:`
  WR_DOUBLE,  // `,` (inf, -inf and nan included)
  WR_BOOL,    // `#t` / `#f`
  WR_ARRAY,   // `*`, and RESP3 sets `~` (with .set = true)
  WR_MAP,     // `%`, keys and values alternating in items[]
  WR_ERROR,   // `-` and RESP3 blob errors `!`; .s is the server's own text ("WRONGTYPE …")
} WReplyKind;

typedef struct WReply {
  WReplyKind kind;
  str    s;      // arena-owned
  i64    i;
  f64    d;
  bool   b;
  bool   set;    // the reply was a RESP3 set
  i32    n;      // ARRAY/MAP: element count (a map's n is the number of pairs)
  struct WReply *items;  // arena-owned, n long; a map alternates key, value
  Arena *own;    // set when the reply owns its arena — reply_free() releases it
} WReply;

WReplyKind reply_kind(const WReply *r);
bool  reply_is_nil(const WReply *r);
bool  reply_is_err(const WReply *r);
str   reply_str(const WReply *r);      // "" for a non-string; nil is reply_is_nil()
i64   reply_int(const WReply *r);
f64   reply_double(const WReply *r);
bool  reply_bool(const WReply *r);
i32   reply_len(const WReply *r);
const WReply *reply_at(const WReply *r, i32 i);
const WReply *reply_val(const WReply *r, const char *key);  // a map, by key
str   reply_err(const WReply *r);      // the server's text, "" when it was not an error
void  reply_free(WReply *r);

// ---------------------------------------------------------------- one command
//
// A command is one shot: cmd_run() sends it, reads its reply and marks the handle spent. Using it
// afterwards is a sentence naming the statement, not a use-after-free two requests later — the
// same rule db_exec() enforces on a WQuery.
WCmd *cmd(WRedis *r, const char *verb);   // arena-owned, recycled from a fixed ring
void  cmd_str(WCmd *c, str a);            // one bulk argument (binary-safe)
void  cmd_cstr(WCmd *c, const char *a);
void  cmd_i64(WCmd *c, i64 a);
void  cmd_f64(WCmd *c, f64 a);
void  cmd_raw(WCmd *c, const void *p, size_t n);
WReply cmd_run(WCmd *c);                  // own = a fresh arena: reply_free() it

// The verbs an app actually writes. Every one exists twice, with one name: on the process default
// handle (redis_get(k)) and on a handle you own (redis_get(r, k)) — the db_all(T)/db_all(db,T)
// rule. They answer the useful thing, and a failure is reported through redis_error() on the
// handle, exactly like db_fetch() answering null.
str   redis_get_(WRedis *r, str k);                    // .p == nullptr when the key is absent
bool  redis_set_(WRedis *r, str k, str v);
bool  redis_set_ex_(WRedis *r, str k, str v, i64 ms);  // SET … PX
bool  redis_set_nx_(WRedis *r, str k, str v, i64 ms);  // SET … NX PX — the lock primitive
str   redis_get_del_(WRedis *r, str k);                // GETDEL: the value and the key, atomically
i64   redis_del_(WRedis *r, str k);                    // keys deleted
bool  redis_exists_(WRedis *r, str k);
i64   redis_incr_(WRedis *r, str k);                   // the new value
i64   redis_incr_by_(WRedis *r, str k, i64 by);
bool  redis_expire_(WRedis *r, str k, i64 ms);
i64   redis_ttl_(WRedis *r, str k);                    // -1 no expiry, -2 absent
str   redis_hget_(WRedis *r, str map, str field);
bool  redis_hset_(WRedis *r, str map, str field, str v);
bool  redis_hdel_(WRedis *r, str map, str field);
str   redis_lpop_(WRedis *r, str list);
bool  redis_lpush_(WRedis *r, str list, str v);
bool  redis_rpush_(WRedis *r, str list, str v);
bool  redis_sadd_(WRedis *r, str set, str v);
bool  redis_srem_(WRedis *r, str set, str v);
bool  redis_sismember_(WRedis *r, str set, str v);
// Sorted sets: enough for a leaderboard without a query language of our own. Anything more
// elaborate is one `cmd(g_r, "ZRANGEBYSCORE")` away.
bool  redis_zadd_(WRedis *r, str key, f64 score, str member);
f64   redis_zscore_(WRedis *r, str key, str member);   // NaN when absent
WReply redis_zrange_(WRedis *r, str key, i64 start, i64 stop);  // own = caller arena
i64   redis_publish_(WRedis *r, str channel, str payload);  // receivers that got it
// MSET/MGET, the two commands a cache cannot do without:
bool  redis_mset_(WRedis *r, i32 n, const str *kv);          // pairs: k1 v1 k2 v2 …
WReply redis_mget_(WRedis *r, i32 n, const str *keys);       // own = caller arena

// The two shapes, generated per verb: `redis_x(args…)` is the default handle and
// `redis_x(r, args…)` is yours. WC_AS_STR is why a literal, a `str` and an integer all work.
#define redis_get(...) WC_CAT(WC_RGET_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_RGET_1(k_) redis_get_(redis(), WC_AS_STR(k_))
#define WC_RGET_2(r_, k_) redis_get_((r_), WC_AS_STR(k_))

#define redis_set(...) WC_CAT(WC_RSET_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_RSET_2(k_, v_) redis_set_(redis(), WC_AS_STR(k_), WC_AS_STR(v_))
#define WC_RSET_3(r_, k_, v_) redis_set_((r_), WC_AS_STR(k_), WC_AS_STR(v_))

#define redis_set_ex(...) WC_CAT(WC_RSETX_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_RSETX_3(k_, v_, ms_) redis_set_ex_(redis(), WC_AS_STR(k_), WC_AS_STR(v_), (i64)(ms_))
#define WC_RSETX_4(r_, k_, v_, ms_) \
  redis_set_ex_((r_), WC_AS_STR(k_), WC_AS_STR(v_), (i64)(ms_))

#define redis_set_nx(...) WC_CAT(WC_RSETNX_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_RSETNX_3(k_, v_, ms_) redis_set_nx_(redis(), WC_AS_STR(k_), WC_AS_STR(v_), (i64)(ms_))
#define WC_RSETNX_4(r_, k_, v_, ms_) \
  redis_set_nx_((r_), WC_AS_STR(k_), WC_AS_STR(v_), (i64)(ms_))

#define redis_del(...) WC_CAT(WC_RDEL_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_RDEL_1(k_) redis_del_(redis(), WC_AS_STR(k_))
#define WC_RDEL_2(r_, k_) redis_del_((r_), WC_AS_STR(k_))

#define redis_exists(...) WC_CAT(WC_REX_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_REX_1(k_) redis_exists_(redis(), WC_AS_STR(k_))
#define WC_REX_2(r_, k_) redis_exists_((r_), WC_AS_STR(k_))

#define redis_incr(...) WC_CAT(WC_RINC_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_RINC_1(k_) redis_incr_(redis(), WC_AS_STR(k_))
#define WC_RINC_2(r_, k_) redis_incr_((r_), WC_AS_STR(k_))

#define redis_expire(...) WC_CAT(WC_REXP_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_REXP_2(k_, ms_) redis_expire_(redis(), WC_AS_STR(k_), (i64)(ms_))
#define WC_REXP_3(r_, k_, ms_) redis_expire_((r_), WC_AS_STR(k_), (i64)(ms_))

#define redis_ttl(...) WC_CAT(WC_RTTL_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_RTTL_1(k_) redis_ttl_(redis(), WC_AS_STR(k_))
#define WC_RTTL_2(r_, k_) redis_ttl_((r_), WC_AS_STR(k_))

#define redis_hget(...) WC_CAT(WC_RHGET_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_RHGET_2(m_, f_) redis_hget_(redis(), WC_AS_STR(m_), WC_AS_STR(f_))
#define WC_RHGET_3(r_, m_, f_) redis_hget_((r_), WC_AS_STR(m_), WC_AS_STR(f_))

#define redis_hset(...) WC_CAT(WC_RHSET_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_RHSET_3(m_, f_, v_) redis_hset_(redis(), WC_AS_STR(m_), WC_AS_STR(f_), WC_AS_STR(v_))
#define WC_RHSET_4(r_, m_, f_, v_) \
  redis_hset_((r_), WC_AS_STR(m_), WC_AS_STR(f_), WC_AS_STR(v_))

#define redis_publish(...) WC_CAT(WC_RPUB_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_RPUB_2(c_, p_) redis_publish_(redis(), WC_AS_STR(c_), WC_AS_STR(p_))
#define WC_RPUB_3(r_, c_, p_) redis_publish_((r_), WC_AS_STR(c_), WC_AS_STR(p_))

// One command with its own deadline and its own arena: what the query cache uses, so one slow
// Redis costs the cache its budget instead of the request its timeout.
bool redis_call(WRedis *r, i32 argc, const str *argv, i64 timeout_ms, Arena *a, WReply *out);

// ---------------------------------------------------------------- pipelining
//
// One round trip for n commands, replies in order. An error in the middle does not cancel the
// rest — that is Redis's own rule, and a pipeline that swallowed the rest would be a different
// feature with a different name.
//
//   WPipe *p = pipe_new(r);
//   pipe_add(p, "MGET", k1, k2, k3);           // arguments, not a printf format
//   pipe_add(p, "INCR", key);
//   i32 n; WReply *reps = pipe_run(p, &n);     // arena-owned, in order
WPipe *pipe_new(WRedis *r);              // borrows a connection until run/free
void   pipe_cmd(WPipe *p, WCmd *c);      // append a builder's frame
WReply *pipe_run(WPipe *p, i32 *n_out);  // arena-owned replies, n in order
void   pipe_free(WPipe *p);

void pipe_add_(WPipe *p, const char *verb, i32 n, const str *args);
#define pipe_add(p_, verb_, ...)                                                                   \
  pipe_add_((p_), (verb_), WC_NARG(__VA_ARGS__), (const str[]){WC_MAP(WC_AS_STR, __VA_ARGS__)})

// ---------------------------------------------------------------- pub/sub
//
// A subscription owns a dedicated connection (a subscribed connection cannot run ordinary
// commands) and one task. The callback runs on that task; it must not block for long, and it may
// allocate in the task's arena. Reconnects with 100 ms → 30 s backoff, resubscribing everything.
//
// **Messages sent while disconnected are lost.** Redis pub/sub is at-most-once, and a module that
// pretended otherwise would be a queue — queues are webc/jobs.h.
typedef void (*WRedisMsgFn)(str channel, str payload, void *arg);
WSub *redis_subscribe(WRedis *r, str channel, WRedisMsgFn fn, void *arg);
WSub *redis_psubscribe(WRedis *r, str pattern, WRedisMsgFn fn, void *arg);
void  redis_unsubscribe(WSub *s);   // synchronous: closes, wakes the task, joins it
i32   redis_sub_count(void);        // live subscriptions, for /debug and for tests