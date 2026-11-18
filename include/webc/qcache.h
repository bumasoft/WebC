// SPDX-License-Identifier: Apache-2.0
// webc/qcache.h — the query cache: the results db_exec() returns, kept in Redis (SPEC §9d).
//
// One place, because the framework has one: every statement — db_all, db_one, db_count, db_by,
// db_find, db_save, db_increment, prefetch, a raw db_exec_sql — passes through
// `WRes db_exec()` (src/db.c). Cache a WRes, replay a WRes, and nothing above the driver knows
// the cache exists.
//
//   WEBC_QUERY_CACHE=1 REDIS_URL=redis://localhost:6379 db_all(Todo);   // second call is a hit
//
// Invalidation is a **per-table generation** in Redis: a write INCRs
// `webc:{<app>:<db>}:g:<table>`, and every cached select's key carries the generation of every
// table it read. A write therefore invalidates in O(1) with no SCAN, no tag sets to keep in step,
// and no way for two processes to disagree. A TTL is the backstop for writes that did not go
// through the framework at all (`psql`, a script, another service).
//
//   cache_bust(Todo);        // after a bulk load, a seed, a migration
//   qcache_flush();          // INCR of the global generation: every key, O(1)
//   nocache { VecTodo *t = db_all(Todo); }     // a block that reads the database
//
// **Two rules decide whether this is a feature or an outage.**
// 1. A Redis problem never fails a request: cache errors fail open to the database, are counted,
//    and open a breaker. Only an explicit redis_* call in a handler can fail that handler.
// 2. A stale row is a bug, so the cache is off unless asked (`WEBC_QUERY_CACHE=1`), and a
//    statement that cannot be proved cacheable is not cached.
//
// Environment: WEBC_QUERY_CACHE (0), WEBC_CACHE_TTL_MS (30000), WEBC_CACHE_TTL_JITTER (10 %),
// WEBC_CACHE_MAX_BYTES (262144), WEBC_CACHE_MS (5 — the cap on one cache operation),
// WEBC_CACHE_BREAKER (1), WEBC_TRACE_CACHE (0).
//
// **Refuses:** HTTP response caching (mw_compress and the pwa worker already do that, keyed by
// URL); caching writes; a per-process L1 (a cache inside the process cannot be invalidated by a
// write in another process — the same reason limits.h refuses a local rate table); background
// refresh; compression of entries.
#pragma once

#include <webc/core.h>
#include <webc/db.h>
#include <webc/model.h>
#include <webc/redis.h>

typedef struct WCacheStats {
  u64 hits, misses, stores, errors, bumps, skipped;
  u64 skip_tx, skip_write, skip_volatile, skip_hint, skip_breaker, skip_too_large, skip_off;
} WCacheStats;

// Configuration, for a test or an app that would rather not use the environment. Called before
// the first statement. `ttl_ms` 0 = the environment/default.
void qcache_config(i64 ttl_ms, i64 max_bytes);
bool qcache_on(void);            // the cache is live for this process
void qcache_ttl_model(const WModel *m, i64 ms);
#define qcache_ttl(T_, ms_) qcache_ttl_model(T_##_modelof(), (ms_))
void qcache_bust(const WModel *m);        // bump one table's generation
#define cache_bust(T_) qcache_bust(T_##_modelof())
void qcache_flush(void);                  // bump the global generation (every key, O(1))
WCacheStats qcache_stats(void);
void qcache_reset_counters(void);         // tests
str  qcache_debug_text(Arena *a);         // the /debug/cache body

// A block that must read the database: the tx_scope shape, so the braces are a real block.
//
//   nocache {
//     VecTodo *fresh = db_all(Todo);   // no probe, no store
//   }
int  qcache_nocache_enter(void);
int  qcache_nocache_leave(void);
#define nocache                                                                                    \
  for (int _wc_nc_ = (qcache_nocache_enter(), 1); _wc_nc_; _wc_nc_ = (qcache_nocache_leave(), 0))

// One statement, uncached.
WRes db_exec_nocache(WDB *db, WQuery *qy);

// ---------------------------------------------------------------- the hooks db.c calls
//
// Not part of the app surface: `webc lint` treats a direct call in an app as an internal leak.
// What the probe learned, handed to the store so an entry is filed under the generation it was
// read with — never under whatever is current when the query finishes.
typedef struct WQCtx {
  bool active;
  i64  gens[WC_REDIS_MAX_ARGS];   // !all first, then one per table
  str  tables[16];
  i32  ntables;
} WQCtx;

typedef struct WQHit {
  bool hit;
  WRes res;
} WQHit;

//
// Both hooks are called from `db_exec`, so an app links the client and the cache whether or not it
// uses Redis — 35 KB of a hello-world binary, the same order as the postgres driver that is
// already always linked. That cost is in the budget (`serve binary`), not hidden here.
WQHit qcache_probe(WDB *db, WQuery *qy, WQCtx *ctx);                 // before the driver
void  qcache_note(WDB *db, WQuery *qy, WRes *r, const WQCtx *ctx);   // after it
bool  qcache_is_write(const char *sql);           // for `webc check` and for tests
str   qcache_key_for_test(Arena *a, const char *sql, const WQuery *qy); // the key, without generations