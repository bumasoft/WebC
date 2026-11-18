// bench/cache.c — what the query cache actually buys, on this machine.
//
//   REDIS_URL=redis://127.0.0.1:6379/0 ./build/bench/cache        (make bench-cache)
//
// Three numbers per statement: the database, a cache hit, and the break-even point between them.
// The interesting one is the third, because a cache has a *fixed* cost — two Redis round trips and
// a JSON parse — and a statement cheaper than that cost is slower cached than not. On sqlite in
// process the crossover is around "any query at all", which is why the numbers are reported per
// query rather than as one headline: the cache is for the statements that hurt.
#include <webc/log.h>
#include <webc/server.h>

#include <stdio.h>

WEBC_MODEL(Row, (id, i64, .pk = true), (title, str, .required = true), (n, i64))
WEBC_VEC_DEFINE(VecRow, vec_row, Row)

static f64 now_us(void) { return (f64)wc_time_ms() * 1000.0; }

// `reads` iterations of `fn`, in microseconds each.
static f64 time_reads(i32 reads, void (*fn)(void)) {
  fn(); // warm: the first call pays for a connection, an arena, a prepared statement
  fn();
  f64 t0 = now_us();
  for (i32 i = 0; i < reads; i++) fn();
  return (now_us() - t0) / (f64)reads;
}

static WDB *g_db;
static void small(void) {
  WQuery qy = {0};
  (void)qy;
  db_all_where(g_db, Row, "where id <= 20", nullptr);
}
static void heavy(void) {
  WRes r = db_exec_sql(g_db, "select count(*) as c from rows a, rows b, rows c"
                             " where a.n < b.n and b.n < c.n");
  wres_free(&r);
}

static void line(const char *name, f64 db_us, f64 hit_us) {
  const char *verdict = hit_us < db_us ? "cache wins" : "database wins";
  printf("  %-28s %10.1f us%12.1f us        %-14s %+6.2fx\n", name, db_us, hit_us, verdict,
         db_us / hit_us);
}

int main(void) {
  const char *url = getenv("REDIS_URL");
  if (!url || !*url) {
    fprintf(stderr, "bench/cache: set REDIS_URL (this measures the cache against a real server)\n");
    return 1;
  }
  setenv("WEBC_QUERY_CACHE", "1", 1);
  if (!getenv("WEBC_CACHE_TTL_MS")) setenv("WEBC_CACHE_TTL_MS", "60000", 1);
  if (!getenv("WEBC_REDIS_NAME")) setenv("WEBC_REDIS_NAME", "bench", 1);
  redis_set_default(redis_open(url));

  g_db = db_open(getenv("WEBC_TEST_DB") ? getenv("WEBC_TEST_DB") : "sqlite::memory:");
  if (!g_db) {
    fprintf(stderr, "bench/cache: could not open a database (set DATABASE_URL)\n");
    return 1;
  }
  db_exec_sql(g_db, "create table rows (id integer primary key autoincrement,"
                    " title text not null, n integer not null default 0)");
  for (i32 i = 0; i < 200; i++)
    db_exec_sql(g_db, str_fmt(nullptr, "insert into rows (title, n) values ('row %d', %d)", i, i).p);

  if (!qcache_on()) {
    fprintf(stderr, "bench/cache: the cache is off — is Redis at %s?\n", url);
    return 1;
  }

  // The database alone: the same binary with the cache switched off for the measurement.
  qcache_nocache_enter();
  f64 small_db = time_reads(2000, small);
  f64 heavy_db = time_reads(20, heavy);
  qcache_nocache_leave();

  // Cached: a hit is a GET (plus the counters, unless the memo is on).
  f64 small_hit = time_reads(2000, small);
  f64 heavy_hit = time_reads(20, heavy);

  WCacheStats s = qcache_stats();
  printf("\n  query cache, 200-row table, redis %s\n", redis_addr(redis()).p);
  printf("  %-28s %12s %13s        %-14s %s\n", "statement", "database", "cache hit", "", "");
  line("a 20-row select", small_db, small_hit);
  line("a 200-row three-way self-join", heavy_db, heavy_hit);
  printf("\n  hits %llu  misses %llu  stores %llu  errors %llu   (gens memo %s)\n",
         (unsigned long long)s.hits, (unsigned long long)s.misses, (unsigned long long)s.stores,
         (unsigned long long)s.errors, getenv("WEBC_CACHE_GEN_MS") ? "on" : "off");
  printf("  break-even: a statement is worth caching when it costs more than a hit\n\n");
  db_close(g_db);
  return 0;
}