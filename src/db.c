// SPDX-License-Identifier: Apache-2.0
// WebC — database: query builder, pool, model helpers, transactions, migrations.
//
// `select` is a statement macro in <webc/task.h>, and this file includes <unistd.h>, which
// pulls in the system's select(2) declaration. Same opt-out tls.c and bench_load use.
#define WEBC_NO_SELECT_MACRO 1
#include "db_internal.h"
#include <webc/app.h>
#include <webc/http.h>
#if !defined(__wasm__)
#include <webc/qcache.h> // the query cache is a server thing: a wasm build has no sockets
#endif
#include <webc/relations.h>
#include <webc/log.h>
#include <webc/task.h>

#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// A pool is bounded by what one process can really hold open, not by the default (cpus).
// 16 was low enough that a burst of requests could never be served concurrently even where
// postgres could take it, and it is a compile-time array size: 256 slots cost 2 KB.
#define WC_POOL_MAX 256
#define WC_ACQUIRE_DEFAULT_MS 5000

struct WDB {
  const WDBDriver *drv;
  char url[512];
  Arena *arena; // descriptor/column-name storage, process lifetime
  WMutex mu;
  WConn *conns[WC_POOL_MAX];
  bool busy[WC_POOL_MAX];
  i32 n;   // highest slot ever opened + 1
  i32 cap; // how many slots the pool may use (= tokens in `slots`)
  WChan *slots; // 0..cap-1, one token per free connection. Waiting is a FIFO park on it.
  i64 acquire_ms;
  bool closed;
  char err[512];
  WDbErrKind err_cls; // the class of the last failure: what db_fail() maps to a status
};

// One session per task per database: a connection this task owns, the transaction depth on
// it, and the request it belongs to (for cancellation). Held in task-local storage, never in
// a thread-local — a task may resume on another worker (§16.10). A task that talks to two
// databases holds two, which is why they are a list.
struct WDbSession {
  WDB *db;
  i32 slot;
  i32 depth; // 0 = no transaction; n>0 = the n'th nested scope is open (a savepoint)
  WCtx *ctx;
  WDbSession *next;
};

// The process's default handle. Written once at boot and then only read, so it is an
// atomic pointer rather than a mutex-guarded global: every query in a request calls
// db(), and taking the scheduler lock for each one was the single hottest lock in the
// framework (measured with WC_LOCK_HIST: 20 lock pairs per rendered request).
static _Atomic(WDB *) g_default;
// The last open failure's sentence: a handle that failed to open is freed, so `db_error(null)`
// is the only place a caller can still read why ("no driver for postgre://…").
static char g_open_err[512];

// ---------------------------------------------------------------- query builder

// ---------------------------------------------------------------- query handles
//
// A query is one shot: db_exec() frees the arena the SQL and the parameters live in, and
// using the handle afterwards is a bug. So the *handle* must not live in that arena — a
// flag read out of freed memory is both a use-after-free and, worse, a lie: the allocator
// hands the same address to the next query, and "select 1" on a health check looked
// exactly like a query that had already been released (that false alarm stopped an app
// from booting: two `q("select name from schema_migrations")` in a row, same address,
// same text).
//
// Handles therefore come from a fixed ring that is never freed. A released handle keeps
// its SQL text so the message can quote it, and is recycled by the next q(): the window
// in which a stale use is caught is "until the next query", which is where the bug
// actually happens (the next line of the same function).
#define WC_QHANDLES 256
static WQuery g_qh[WC_QHANDLES];
static i32 g_qh_at;
static _Atomic int g_qh_lock;

static void qh_lock(void) {
  while (atomic_exchange_explicit(&g_qh_lock, 1, memory_order_acquire)) {
  }
}
static void qh_unlock(void) { atomic_store_explicit(&g_qh_lock, 0, memory_order_release); }

static WQuery *qh_take(void) {
  WQuery *out = nullptr;
  qh_lock();
  for (i32 i = 0; i < WC_QHANDLES; i++) {
    i32 k = (g_qh_at + i) % WC_QHANDLES;
    if (g_qh[k].in_use) continue;
    out = &g_qh[k];
    g_qh_at = (k + 1) % WC_QHANDLES;
    break;
  }
  if (out) {
    memset(out, 0, sizeof *out);
    out->ring = true;
    out->in_use = true;
  }
  qh_unlock();
  if (!out)
    panic("more than %d queries are open at once. A query is one shot — db_exec() ends "
          "it — so this means that many were created and never executed or q_free()d.",
          WC_QHANDLES);
  return out;
}

static void qh_done(WQuery *qy) {
  if (!qy) return;
  qh_lock();
  qy->released = true;
  qy->in_use = false;
  qy->arena = nullptr;
  qy->params = nullptr;
  qy->nparams = qy->cap = 0;
  qy->names = nullptr;
  qh_unlock();
}

// The check that turns "the next request segfaults somewhere else" into a sentence about
// this call. Safe to read: the handle is ring memory, never freed.
static void q_live(WQuery *qy, const char *what) {
  if (!qy) panic("%s: the query is null", what);
  // A query embedded in a builder (WWhere) describes a statement, not a run of one: it is
  // the caller's struct, reused for `db_select` then `db_count`, and it is never ours to
  // retire. Only handles from the ring are one-shot.
  if (!qy->ring || !qy->released) return;
  panic("%s: this query has already been run (\"%s\"). db_exec()/db_find()/"
        "db_all_where() end a query — a `q()` is used once, never twice.",
        what, qy->sql_head);
}

WQuery *q_a(Arena *a, const char *sql) {
  WQuery *qy = qh_take();
  qy->arena = a;
  qy->sql = str_lit(sql);
  snprintf(qy->sql_head, sizeof qy->sql_head, "%.*s", (int)(sizeof qy->sql_head - 1),
           sql ? sql : "");
  qy->owns_arena = false;
  return qy;
}

WQuery *q(const char *sql) {
  Arena *a = arena_new(4096);
  WQuery *qy = q_a(a, sql);
  qy->owns_arena = true; // db_exec() destroys it, so a query does not leak
  return qy;
}

void q_free(WQuery *qy) {
  if (!qy) return;
  if (qy->released) return; // already ended (by db_exec() or an earlier q_free())
  if (qy->owns_arena && qy->arena) arena_destroy(qy->arena);
  if (qy->ring) qh_done(qy);
}

static void q_push(WQuery *qy, const char *name, WVal v) {
  q_live(qy, "q_bind");
  if (!qy->arena) qy->arena = wc_arena_get();
  if (qy->nparams == qy->cap) {
    i32 ncap = qy->cap ? qy->cap * 2 : 4;
    WVal *nv = arena_alloc(qy->arena, (size_t)ncap * sizeof(WVal));
    str *nn = arena_alloc(qy->arena, (size_t)ncap * sizeof(str));
    if (qy->params) {
      memcpy(nv, qy->params, (size_t)qy->nparams * sizeof(WVal));
      memcpy(nn, qy->names, (size_t)qy->nparams * sizeof(str));
    }
    qy->params = nv;
    qy->names = nn;
    qy->cap = ncap;
  }
  qy->params[qy->nparams] = v;
  qy->names[qy->nparams] = name ? str_lit(name) : (str){"", 0};
  qy->nparams++;
}

void q_i64(WQuery *qy, const char *name, i64 v) {
  q_push(qy, name, (WVal){WV_I64, .i = v});
}
void q_f64(WQuery *qy, const char *name, f64 v) {
  q_push(qy, name, (WVal){WV_F64, .f = v});
}
void q_bool(WQuery *qy, const char *name, bool v) {
  q_push(qy, name, (WVal){WV_BOOL, .b = v});
}
void q_null(WQuery *qy, const char *name) {
  q_push(qy, name, (WVal){WV_NULL});
}
void q_str(WQuery *qy, const char *name, str v) {
  q_push(qy, name, v.p ? (WVal){WV_TEXT, .s = v} : (WVal){WV_NULL});
}
void q_cstr(WQuery *qy, const char *name, const char *v) {
  q_str(qy, name, v ? str_lit(v) : (str){nullptr, 0});
}
void q_bytes(WQuery *qy, const char *name, Bytes v) {
  q_push(qy, name, (WVal){WV_BLOB, .s = (str){(char *)v.p, v.n}});
}

// :name → ? (sqlite) or $n (postgres), keeping the order of first appearance.
// Positional placeholders are the other half of the same problem. The model helpers write
// their own SQL — `INSERT INTO … VALUES (?, ?, …)`, `… where slug in (?, ?, ?)` — and a `?` is
// sqlite's spelling. Postgres wants `$1, $2, …` and answered "syntax error at or near \",\""
// the first time a page prefetched a related row, so the whole page 500'd. The order of the
// `?`s is the order the values were bound, so renumbering left to right is exact; doing it here
// rather than in the pg driver keeps one meaning for `?` across drivers.
static WQuery *db_bind_positional(WQuery *qy, Arena *a, char ph) {
  if (ph == '?' || !qy->sql.p || !str_contains(qy->sql, str_lit("?"))) return qy;
  Buf out;
  buf_init(&out, a, qy->sql.n + 16);
  i32 n = 0;
  for (size_t i = 0; i < qy->sql.n; i++) {
    char ch = qy->sql.p[i];
    if (ch == '\'' || ch == '"') { // a literal is data: copy it whole, escapes and all
      char q = ch;
      buf_putc(&out, ch);
      for (i++; i < qy->sql.n; i++) {
        buf_putc(&out, qy->sql.p[i]);
        if (qy->sql.p[i] == q && i + 1 < qy->sql.n && qy->sql.p[i + 1] == q) {
          buf_putc(&out, qy->sql.p[++i]); // '' inside a literal
          continue;
        }
        if (qy->sql.p[i] == q) break;
      }
      continue;
    }
    if (ch == '-' && i + 1 < qy->sql.n && qy->sql.p[i + 1] == '-') { // line comment
      while (i < qy->sql.n && qy->sql.p[i] != '\n') buf_putc(&out, qy->sql.p[i++]);
      if (i < qy->sql.n) buf_putc(&out, qy->sql.p[i]);
      continue;
    }
    if (ch == '?') {
      buf_putc(&out, ph);
      buf_printf(&out, "%d", (int)++n);
      continue;
    }
    buf_putc(&out, ch);
  }
  qy->sql = buf_str(&out);
  return qy;
}

WQuery *db_bind_named(WQuery *qy, const char *ph) {
  str sql = qy->sql;
  Buf out;
  buf_init(&out, qy->arena, sql.n + 16);
  WVal *ordered = arena_alloc(qy->arena, ((size_t)qy->nparams + 1) * sizeof(WVal));
  i32 used = 0;
  bool is_dollar = ph[0] == '$';
  for (size_t i = 0; i < sql.n;) {
    // A colon inside a literal is not a placeholder. `to_char(now(),
    // 'HH24:MI:SS')` turned into `to_char(now(), 'HH24$1MI$2SS')` and a postgres
    // migration died on "bind message supplies 2 parameters, but prepared
    // statement requires 0" — the query text had been mangled, not the call.
    if (sql.p[i] == '\'') {
      buf_putc(&out, sql.p[i++]);
      while (i < sql.n) {
        char c = sql.p[i++];
        buf_putc(&out, c);
        if (c == '\'') {
          if (i < sql.n && sql.p[i] == '\'') { // '' is an escaped quote
            buf_putc(&out, sql.p[i++]);
            continue;
          }
          break;
        }
      }
      continue;
    }
    if (sql.p[i] == '"') { // a quoted identifier
      buf_putc(&out, sql.p[i++]);
      while (i < sql.n) {
        char c = sql.p[i++];
        buf_putc(&out, c);
        if (c == '"') break;
      }
      continue;
    }
    if (sql.p[i] == '-' && i + 1 < sql.n && sql.p[i + 1] == '-') { // line comment
      while (i < sql.n && sql.p[i] != '\n')
        buf_putc(&out, sql.p[i++]);
      continue;
    }
    if (sql.p[i] == '/' && i + 1 < sql.n && sql.p[i + 1] == '*') { // block comment
      buf_putc(&out, sql.p[i++]);
      buf_putc(&out, sql.p[i++]);
      while (i < sql.n) {
        char c = sql.p[i++];
        buf_putc(&out, c);
        if (c == '*' && i < sql.n && sql.p[i] == '/') {
          buf_putc(&out, sql.p[i++]);
          break;
        }
      }
      continue;
    }
    // postgres casts: `value::text` is two colons, not a placeholder named `:text`. Without
    // this, every `::` in a query became a bind and postgres answered "syntax error at or
    // near \":\"" — a cast is ordinary SQL and this is a driver-agnostic rewriter.
    if (sql.p[i] == ':' && i + 1 < sql.n && sql.p[i + 1] == ':') {
      buf_putc(&out, sql.p[i++]);
      buf_putc(&out, sql.p[i++]);
      continue;
    }
    if (sql.p[i] == ':' && i + 1 < sql.n &&
        (isalpha((unsigned char)sql.p[i + 1]) || sql.p[i + 1] == '_')) {
      size_t j = i + 1;
      while (j < sql.n && (isalnum((unsigned char)sql.p[j]) || sql.p[j] == '_'))
        j++;
      str name = str_sub(sql, i + 1, j);
      i64 found = -1;
      for (i32 k = 0; k < qy->nparams; k++)
        if (str_eq(qy->names[k], name)) {
          found = k;
          break;
        }
      if (found < 0) {
        // unnamed/unknown: bind NULL so the driver reports a clear error
        ordered[used++] = (WVal){WV_NULL};
      } else {
        ordered[used++] = qy->params[found];
      }
      if (is_dollar)
        buf_printf(&out, "$%d", used);
      else
        buf_putc(&out, '?');
      i = j;
      continue;
    }
    buf_putc(&out, sql.p[i]);
    i++;
  }
  qy->sql = buf_str(&out);
  qy->params = ordered;
  qy->nparams = used;
  return qy;
}

// ---------------------------------------------------------------- rows

WVal row_at(WRow *r, i32 i) {
  if (!r || i < 0 || i >= r->n) return (WVal){WV_NULL};
  return r->vals[i];
}

WVal row_get(WRow *r, const char *name) {
  if (!r) return (WVal){WV_NULL};
  for (i32 i = 0; i < r->n; i++)
    if (r->keys && str_eq_c(r->keys[i], name)) return r->vals[i];
  return (WVal){WV_NULL};
}

i64 row_i64(WRow *r, const char *name) {
  WVal v = row_get(r, name);
  switch (v.kind) {
  case WV_I64:
    return v.i;
  case WV_F64:
    return (i64)v.f;
  case WV_BOOL:
    return v.b ? 1 : 0;
  case WV_TEXT: {
    i64 o = 0;
    str_to_i64(v.s, &o);
    return o;
  }
  default:
    return 0;
  }
}

i32 row_i32(WRow *r, const char *name) {
  return (i32)row_i64(r, name);
}

f64 row_f64(WRow *r, const char *name) {
  WVal v = row_get(r, name);
  if (v.kind == WV_F64) return v.f;
  if (v.kind == WV_I64) return (f64)v.i;
  if (v.kind == WV_TEXT) {
    f64 o = 0;
    str_to_f64(v.s, &o);
    return o;
  }
  return 0;
}

str row_str(WRow *r, const char *name) {
  WVal v = row_get(r, name);
  return v.kind == WV_TEXT || v.kind == WV_BLOB ? v.s : (str){"", 0};
}

bool row_bool(WRow *r, const char *name) {
  WVal v = row_get(r, name);
  if (v.kind == WV_BOOL) return v.b;
  if (v.kind == WV_I64) return v.i != 0;
  if (v.kind == WV_TEXT) return str_eq_c(v.s, "t") || str_eq_c(v.s, "true") || str_eq_c(v.s, "1");
  return false;
}

bool row_is_null(WRow *r, const char *name) {
  return row_get(r, name).kind == WV_NULL;
}
i64 row_i64_at(WRow *r, i32 i) {
  WVal v = row_at(r, i);
  return v.kind == WV_I64 ? v.i : 0;
}
str row_str_at(WRow *r, i32 i) {
  WVal v = row_at(r, i);
  return v.s;
}
i32 row_cols(WRow *r) {
  return r ? r->n : 0;
}

// ---------------------------------------------------------------- pool

static const WDBDriver *driver_for(const char *url) {
  if (strncmp(url, "postgres://", 11) == 0 || strncmp(url, "postgresql://", 13) == 0)
    return &wc_driver_pg;
  if (strncmp(url, "mysql://", 8) == 0 || strncmp(url, "mariadb://", 10) == 0)
    return &wc_driver_mysql;
  // A URL with a scheme is a claim about a driver, and a scheme nobody implements is a
  // typo: opened as a *sqlite file* named `postgre://x` it fails much later, in a place
  // with no mention of the URL. Everything without `://` is a sqlite path (and `sqlite:` /
  // `file:` / `:memory:` keep their meaning).
  if (strstr(url, "://")) return nullptr;
  return &wc_driver_sqlite;
}

// Every driver's name, for the places that need "the dialects we know about" as data
// rather than as a pair of string comparisons (migration twins, `webc check`).
const char *const WC_DB_DRIVERS[] = {"sqlite", "postgres", "mysql"};
const i32 WC_DB_DRIVER_N = (i32)(sizeof WC_DB_DRIVERS / sizeof WC_DB_DRIVERS[0]);

// Generated SQL names columns through here, so a driver that has reserved words of its
// own (`rank`, `groups`, `window` on MySQL) is spelled the driver's way once instead of
// in every app's `.sql("…")`. A `.sql()` override is an expression, and is passed
// through untouched — quoting `coalesce(a, '')` would make it a column of that name.
str db_ident(const WDBDriver *drv, const WField *f) {
  if (!f) return (str){"", 0};
  if (f->q.sql || !drv || !drv->ident_quote) return str_lit(model_column_name(f));
  return db_ident_c(drv, nullptr, model_column_name(f));
}

// A name that is not a plain identifier is a caller's own SQL — `public.users`, a table
// override, a column written as an expression via `.sql`. Quoting it would turn working SQL
// into a name with a dot in it, so those pass through untouched.
static bool plain_ident(const char *s) {
  if (!s || !*s) return false;
  if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_')) return false;
  for (const char *p = s; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
          *p == '_'))
      return false;
  return true;
}

str db_ident_c(const WDBDriver *drv, Arena *a, const char *name) {
  if (!name) name = "";
  if (!drv || !drv->ident_quote || !plain_ident(name)) return str_lit(name);
  char q = drv->ident_quote;
  Buf b;
  buf_init(&b, a ? a : wc_arena_get(), strlen(name) + 8);
  buf_putc(&b, q);
  for (const char *p = name; *p; p++) {
    if (*p == q) buf_putc(&b, q); // the quote character escapes itself: a`b -> `a``b`
    buf_putc(&b, *p);
  }
  buf_putc(&b, q);
  return buf_str(&b);
}

str db_table(const WDBDriver *drv, const WModel *m) {
  return db_ident_c(drv, nullptr, m->table ? m->table : m->name);
}

// The descriptor's column list, spelled the driver's way. `model_sql_columns` is the
// driver-agnostic one (a client decoding a document has no driver); this is the one the
// SQL generator uses, so `select * from stories` becomes ``select `id`, `rank` from
// stories`` on MySQL and a column named `rank` stops being a syntax error.
static str db_sql_columns(Arena *a, const WDBDriver *drv, const WModel *m) {
  if (!drv || !drv->ident_quote) return model_sql_columns(a, m);
  Buf b;
  buf_init(&b, a, 128);
  for (i32 i = 0; i < m->n; i++) {
    if (i) buf_puts(&b, ", ");
    str c = db_ident(drv, &m->fields[i]);
    buf_write(&b, c.p, c.n);
  }
  return buf_str(&b);
}

// The pool is a counting semaphore whose tokens are slot numbers: a token *is* your
// connection, so nothing has to scan for a free one, and waiting is a park on a channel
// (FIFO, no polling) rather than a 1 ms re-check.
static i32 g_pool_default;
static i64 g_acquire_default = WC_ACQUIRE_DEFAULT_MS;

void db_pool_config(i32 max_conns, i64 acquire_ms) {
  if (max_conns > 0) g_pool_default = max_conns > WC_POOL_MAX ? WC_POOL_MAX : max_conns;
  if (acquire_ms > 0) g_acquire_default = acquire_ms;
}

static i32 pool_default_size(void) {
  if (g_pool_default > 0) return g_pool_default;
  const char *env = getenv("WEBC_DB_POOL");
  if (env && *env) {
    i32 v = atoi(env);
    if (v > 0) return v > WC_POOL_MAX ? WC_POOL_MAX : v;
  }
  i32 threads = wtask_threads();
  if (threads <= 0) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    threads = n > 0 ? (i32)n : 4;
  }
  // One connection per worker was the default, and it is a throughput ceiling rather than a
  // safety margin: a request holds its connection for every round trip of a page, so a worker
  // with N statements in flight needs more than one. Measured on the same box, same Postgres,
  // 22 clients, three runs each (median): 6 connections 1,886 rps, 12 → 2,254 (+19.5%),
  // 24 → 2,354 (+25%, diminishing). Two per worker is the knee; WC_POOL_MAX still caps it.
  i32 want = threads * 2;
  return want > WC_POOL_MAX ? WC_POOL_MAX : want;
}

static i64 pool_acquire_ms(void) {
  if (g_acquire_default > 0) return g_acquire_default;
  const char *env = getenv("WEBC_DB_ACQUIRE_MS");
  if (env && *env) {
    i64 v = atoll(env);
    if (v > 0) return v;
  }
  return WC_ACQUIRE_DEFAULT_MS;
}

WDB *db_open(const char *url) {
  if (!url || !*url) {
    snprintf((char[1]){0}, 1, "%s", "");
    return nullptr;
  }
  WDB *db = wm_alloc(sizeof *db);
  memset(db, 0, sizeof *db);
  db->drv = driver_for(url);
  if (!db->drv) {
    // A sentence, at open, naming the URL and the drivers that exist — the alternative
    // was a sqlite database with a file named after the URL, which is a bug report about
    // "my tables are empty" three screens later.
    snprintf(g_open_err, sizeof g_open_err,
             "database: no driver for \"%s\" — the URLs this build speaks are sqlite:path, "
             "postgres://…, mysql://… (and mariadb://…)",
             url);
    wm_free(db);
    return nullptr;
  }
  snprintf(db->url, sizeof db->url, "%s", url);
  db->arena = arena_new(64 * 1024);
  db->cap = pool_default_size();
  // An in-memory sqlite database belongs to its *connection*: a second one is a second,
  // empty database. So the pool is one connection, which is also what `webc test` relies on
  // (every suite runs against `sqlite::memory:`).
  if (strstr(url, ":memory:")) db->cap = 1;
  db->acquire_ms = pool_acquire_ms();
  db->slots = wchan_new(sizeof(i32), db->cap);
  for (i32 i = 0; i < db->cap; i++)
    wchan_send(db->slots, &i); // every slot is free, and slot 0 — the eager connection — first
  // open one connection eagerly so configuration errors surface at startup
  WErrRes err = {.ok = true};
  Arena *ca = arena_new(16 * 1024);
  WConn *c = db->drv->open(url, ca, &err);
  if (c) c->arena = ca;
  if (!c) {
    snprintf(db->err, sizeof db->err, "%.*s", WERR_FMT(err.e));
    snprintf(g_open_err, sizeof g_open_err, "%s", db->err);
    wchan_close(db->slots);
    wm_free(db);
    return nullptr;
  }
  g_open_err[0] = 0;
  db->conns[0] = c;
  db->n = 1;
  return db;
}

void db_close(WDB *db) {
  if (!db) return;
  wmutex_lock(&db->mu);
  db->closed = true;
  wmutex_unlock(&db->mu);
  for (i32 i = 0; i < db->cap; i++)
    if (db->conns[i]) {
      db->drv->close(db->conns[i]);
      arena_destroy(db->conns[i]->arena);
    }
  if (db->slots) wchan_close(db->slots);
  arena_destroy(db->arena);
  wm_free(db);
}

i32 db_pool_size(WDB *db) {
  if (!db) return 0;
  wmutex_lock(&db->mu);
  i32 n = db->n;
  wmutex_unlock(&db->mu);
  return n;
}

bool db_ok(WDB *db) {
  return db && db->err[0] == 0;
}
// A handle that failed to open is gone, so its message would go with it — and the message is
// the whole point ("no driver for postgre://…"). The last open attempt's sentence is kept
// here for exactly that caller: `WDB *db = db_open(url); if (!db) … db_error(db)`.
str db_error(WDB *db) {
  if (db) return str_lit(db->err);
  if (g_open_err[0]) return str_lit(g_open_err);
  return str_lit("no database");
}
str db_driver_name(WDB *db) {
  return db ? str_lit(db->drv->name) : str_lit("none");
}

WDB *db(void) {
  return atomic_load(&g_default);
}

void db_set_default(WDB *d) {
  atomic_store(&g_default, d);
}

// Take a connection out of the pool. The token decides which one, so two tasks can never
// be handed the same slot, and the wait is a park on the channel — first in, first served,
// no polling, and a deadline instead of an unbounded sleep.
static WConn *db_pin(WDB *db, i64 timeout_ms, i32 *slot_out, WErrRes *err) {
  *err = (WErrRes){.ok = true};
  if (!db) {
    *err = wfail(WEBC_EINVAL, str_lit("no database"));
    return nullptr;
  }
  if (db->closed) {
    *err = wfail(WEBC_EDB, str_lit("database is closed"));
    return nullptr;
  }
  i32 slot = -1;
  if (timeout_ms > 0) {
    SelCase c = SEL_RECV(db->slots, &slot);
    if (select_wait(&c, 1, timeout_ms) != 0) {
      *err = wfail(WEBC_EDB,
                   str_fmt(nullptr,
                           "database: no free connection after %lld ms (pool of %d in use) — "
                           "the request was refused rather than made to wait forever",
                           (long long)timeout_ms, (int)db->cap));
      return nullptr;
    }
  } else if (!wchan_recv(db->slots, &slot)) {
    *err = wfail(WEBC_EDB, str_lit("database is closed"));
    return nullptr;
  }
  // The slot is ours until the token goes back: nobody else can look at conns[slot],
  // so opening it needs no lock, and a slow connect does not stall the pool's other users.
  WConn *c = db->conns[slot];
  if (!c) {
    WErrRes e = {.ok = true};
    Arena *ca = arena_new(16 * 1024); // per connection: see WConn::arena
    c = db->drv->open(db->url, ca, &e);
    if (!c) {
      arena_destroy(ca);
      wchan_send(db->slots, &slot); // give the slot back rather than lose it
      *err = e.ok ? wfail(WEBC_EDB, str_lit("database: cannot open a connection")) : e;
      return nullptr;
    }
    c->arena = ca;
    wmutex_lock(&db->mu);
    db->conns[slot] = c;
    if (slot >= db->n) db->n = slot + 1;
    wmutex_unlock(&db->mu);
  }
  db->busy[slot] = true;
  *slot_out = slot;
  return c;
}

static void db_unpin(WDB *db, i32 slot) {
  if (!db || slot < 0) return;
  db->busy[slot] = false;
  wchan_send(db->slots, &slot);
}

// ---------------------------------------------------------------- the session

static WDbSession *db_session_open(WDB *db, WCtx *ctx);

static void db_session_abandon(void *p);
// Registered once, from the first session: a task that ends holding a session gets its
// transaction rolled back and its connection back. A panic unwinds past `cleanup`, so this
// is the only thing standing between a failed handler and a connection parked in the pool
// mid-transaction.
static bool g_exit_hook_set;

static WDbSession *session_head(void) { return (WDbSession *)wtask_db_get(); }

static WDbSession *session_for(WDB *db) {
  for (WDbSession *s = session_head(); s; s = s->next)
    if (s->db == db) return s;
  return nullptr;
}

WDbSession *db_session_current(void) { return session_head(); }

bool db_session_owns(WDbSession *s, WDB *db) { return s && (!db || s->db == db); }
bool db_session_in_tx(WDbSession *s) { return s && s->depth > 0; }
i32 tx_depth(void) {
  WDbSession *s = session_head();
  return s ? s->depth : 0;
}

WDbSession *db_session(WCtx *ctx) {
  WDB *db = db_default();
  if (!db) return nullptr;
  return db_session_open(db, ctx);
}

// The same, for a handle the app opened itself: `tx_scope(other_db)` has to pin *that*
// database, not the process default.
static WDbSession *db_session_open(WDB *db, WCtx *ctx) {
  if (!db) return nullptr;
  // The request's own context, so a query can be cancelled when the client goes away or
  // mw_timeout_ms() fires — the driver uses it to interrupt a statement in flight.
  if (!ctx) {
    Request *r = webc_req_current();
    if (r) ctx = r->ctx;
  }
  WDbSession *s = session_for(db);
  if (s) {
    if (ctx) s->ctx = ctx;
    return s;
  }
  i32 slot = -1;
  WErrRes err = {.ok = true};
  if (!db_pin(db, db->acquire_ms, &slot, &err)) {
    snprintf(db->err, sizeof db->err, "%.*s", WERR_FMT(err.e));
    return nullptr;
  }
  s = wm_alloc(sizeof *s);
  memset(s, 0, sizeof *s);
  s->db = db;
  s->slot = slot;
  s->ctx = ctx;
  s->next = session_head();
  wtask_db_set(s);
  if (!g_exit_hook_set) {
    g_exit_hook_set = true;
    wtask_set_exit_hook(db_session_abandon);
  }
  return s;
}

// Roll back what the session still has open, innermost scope first, then the transaction.
static void session_unwind(WDbSession *s, bool loud) {
  WConn *c = s->db && s->slot >= 0 ? s->db->conns[s->slot] : nullptr;
  if (s->depth > 0 && c) {
    while (s->depth > 1) {
      s->depth--;
      s->db->drv->rollback_to(c, s->depth);
    }
    s->depth = 0;
    if (loud)
      log_warn("database: a task ended inside a transaction — rolling it back",
               {"connection", str_fmt(nullptr, "%d", (int)(s->slot + 1))});
    s->db->drv->rollback(c);
  }
}

// Everything the session did goes back here: an open transaction is rolled back (it was
// never committed, so nothing else is right), the connection returns to the pool, and the
// task stops having a session. The dispatcher calls this at the request boundary.
void db_session_release(void) {
  WDbSession *s = session_head();
  if (!s) return;
  wtask_db_set(nullptr);
  while (s) {
    WDbSession *next = s->next;
    session_unwind(s, false);
    db_unpin(s->db, s->slot);
    wm_free(s);
    s = next;
  }
}

static void db_session_abandon(void *p) {
  WDbSession *s = p;
  wtask_db_set(nullptr);
  while (s) {
    WDbSession *next = s->next;
    session_unwind(s, true);
    db_unpin(s->db, s->slot);
    wm_free(s);
    s = next;
  }
}

// ---------------------------------------------------------------- queries

// WEBC_TRACE_DB=1 prints every statement with its row count and its time. "The front page
// runs four full-table selects" is invisible in the code and obvious in this log; a query
// nobody meant to make is the first thing an app should be able to see.
static bool db_trace_on(void) {
  static int on = -1;
  if (on < 0) on = getenv("WEBC_TRACE_DB") ? 1 : 0;
  return on == 1;
}

static _Atomic u64 g_statements;

u64 db_statement_count(void) { return atomic_load_explicit(&g_statements, memory_order_relaxed); }

// The trace line is capped at 190 columns so a statement stays readable, which makes two
// different statements with the same first 190 characters look identical. The FNV hash of the
// whole text is printed with it: identical work is then provable from the trace alone.
static u32 db_sql_hash(str sql) {
  u32 h = 2166136261u;
  for (size_t i = 0; i < sql.n; i++) h = (h ^ (u8)sql.p[i]) * 16777619u;
  return h;
}

static void db_trace(str sql, i32 nrows, u64 ms, bool ok, bool cached) {
  if (!db_trace_on()) return;
  char head[200];
  snprintf(head, sizeof head, "%.*s", (int)(sql.n < 190 ? sql.n : 190), sql.p ? sql.p : "");
  fprintf(stderr, "[db] %08x %s%s -> %d row%s (%llu ms)%s%s\n", db_sql_hash(sql), head,
          sql.n > 190 ? "…" : "", nrows, nrows == 1 ? "" : "s", (unsigned long long)ms,
          ok ? "" : "  [failed]", cached ? "  [cache hit]" : "");
}

// ---------------------------------------------------------------- dialect repair
//
// A `WWhere` is built before anybody knows which driver will run it, so `ww_limit(&w, 0,
// 20)` writes ` offset 20` on its own. sqlite and postgres accept that; MySQL answers
// "You have an error in your SQL syntax". The repair belongs here, where the handle — and
// therefore the driver's capabilities — is in hand, and it is lexical on purpose: an
// `offset` inside a string literal or a comment is data, not a clause.
static bool wc_word_at(const char *p, const char *end, const char *word) {
  size_t n = strlen(word);
  if ((size_t)(end - p) < n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)p[i]) != (unsigned char)word[i]) return false;
  if ((size_t)(end - p) > n) {
    char c = p[n];
    if (isalnum((unsigned char)c) || c == '_' || c == '$') return false;
  }
  return true;
}

static str db_offset_limit(const WDBDriver *drv, Arena *a, str sql) {
  if (!drv->offset_needs_limit || !sql.p) return sql;
  const char *p = sql.p, *end = sql.p + sql.n;
  bool saw_limit = false, repaired = false;
  Buf out;
  buf_init(&out, a, sql.n + 48);
  while (p < end) {
    char c = *p;
    // literals, quoted identifiers and comments are copied whole: none of them can
    // contain a clause, and an `offset` in them is data
    if (c == '\'' || c == '"' || c == '`') {
      char q = c;
      buf_putc(&out, *p++);
      while (p < end) {
        char d = *p++;
        buf_putc(&out, d);
        if (d == '\\' && q != '`' && p < end) { // backslash escape (mysql's default mode)
          buf_putc(&out, *p++);
          continue;
        }
        if (d == q) {
          if (p < end && *p == q) { // a doubled quote escapes itself
            buf_putc(&out, *p++);
            continue;
          }
          break;
        }
      }
      continue;
    }
    if (c == '-' && p + 1 < end && p[1] == '-') {
      while (p < end && *p != '\n') buf_putc(&out, *p++);
      continue;
    }
    if (c == '#') {
      while (p < end && *p != '\n') buf_putc(&out, *p++);
      continue;
    }
    if (c == '/' && p + 1 < end && p[1] == '*') {
      buf_putc(&out, *p++);
      buf_putc(&out, *p++);
      while (p < end) {
        char d = *p++;
        buf_putc(&out, d);
        if (d == '*' && p < end && *p == '/') {
          buf_putc(&out, *p++);
          break;
        }
      }
      continue;
    }
    bool at_token =
        !(p > sql.p && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '$'));
    if (at_token && wc_word_at(p, end, "limit")) {
      saw_limit = true;
    } else if (at_token && wc_word_at(p, end, "offset") && !saw_limit) {
      // The largest *signed* 64-bit limit: "every row from here". MySQL documents
      // 18446744073709551615 for this, but sqlite parses a limit as a signed integer and
      // answers "datatype mismatch" to that number, so the smaller one is the only spelling
      // all three accept.
      buf_puts(&out, "limit 9223372036854775807 ");
      saw_limit = true;
      repaired = true;
    }
    buf_putc(&out, *p++);
  }
  return repaired ? buf_str(&out) : sql;
}

// Defined with the row pool and the model helpers further down; db_exec and the pool both
// need them before that point.
static str key_text(Arena *a, const WField *f, const void *row);
static bool sql_is_select(str sql);

WRes db_exec(WDB *db, WQuery *qy) {
  WRes r = {0};
  if (!db) {
    r.e = werr(WEBC_EINVAL, str_lit("no database"));
    return r;
  }
  q_live(qy, "db_exec");
  u64 t0 = wc_time_ms();  if (!qy->arena) qy->arena = db->arena;
  // A write forgets every pooled row: within one request an admin save followed by a render must
  // show the new value, and a pool that outlived a write is a stale page. One branch per
  // statement, in the place every statement already passes through.
  if (qy->sql.p && !sql_is_select(qy->sql)) wc_row_pool_clear();
#if !defined(__wasm__)
  // The query cache (§9d): before a connection is even taken. A hit owes the caller exactly what
  // a driver result owes it — the arena destroyed, the ring handle spent, the trace line written —
  // so the early return below has to do all three.
  WQCtx qcx = {0};
  bool cached_on = qcache_on(); // one relaxed load per statement, not one per call site
  if (cached_on) {
    WQHit h = qcache_probe(db, qy, &qcx);
    if (h.hit) {
      if (qy->owns_arena && qy->arena) arena_destroy(qy->arena);
      if (qy->ring) qh_done(qy);
      db_trace(qy->sql, h.res.nrows, wc_time_ms() - t0, true, true);
      return h.res;
    }
  }
#endif
  // A task with a session runs on its own connection, so a statement cannot land in the
  // middle of somebody else's transaction. A request's session is opened here, on its first
  // statement — a request that never touches the database never takes a connection, which is
  // why the static file handler costs nothing. A task with no request (a CLI command, a
  // migration) borrows a connection for the length of the statement, as it always did.
  WDbSession *s = session_for(db);
  if (!s && webc_req_current()) s = db_session(nullptr);
  bool pinned = s != nullptr;
  i32 slot = -1;
  WConn *c = nullptr;
  if (pinned) {
    c = db->conns[s->slot];
    if (!c) { // the session's connection was lost (cancelled, or the server went away)
      r.ok = false;
      r.e = werr(WEBC_EDB, str_lit("database: this session's connection was lost"));
      if (qy->owns_arena && qy->arena) arena_destroy(qy->arena);
      if (qy->ring) qh_done(qy);
      return r;
    }
  } else {
    WErrRes err = {.ok = true};
    c = db_pin(db, db->acquire_ms, &slot, &err);
    if (!c) {
      snprintf(db->err, sizeof db->err, "%.*s", WERR_FMT(err.e));
      r.ok = false;
      r.e = err.ok ? werr(WEBC_EDB, str_lit("database is closed")) : err.e;
      if (qy->owns_arena && qy->arena) arena_destroy(qy->arena);
      if (qy->ring) qh_done(qy);
      return r;
    }
  }
  // Rows get an arena of their own: the descriptor arena (db->arena) is shared
  // by every task, and appending to it from several threads corrupted it under
  // load (measured: 16 concurrent requests to a sqlite app segfaulted).
  Arena *rows = arena_new(8 * 1024);
  // only statements written with :name placeholders need rewriting
  WQuery *bound =
      str_contains(qy->sql, str_lit(":")) ? db_bind_named(qy, db->drv->placeholder) : qy;
  bound = db_bind_positional(bound, rows, db->drv->placeholder[0]);
  if (!bound->arena) bound->arena = db->arena;
  // `offset` without `limit` is the one dialect difference the generator cannot see
  // coming (it builds a clause before a handle exists), so it is repaired here, into the
  // per-statement arena — never into the shared descriptor arena.
  bound->sql = db_offset_limit(db->drv, rows, bound->sql);
  c->ctx = s ? s->ctx : nullptr;
  atomic_fetch_add_explicit(&g_statements, 1, memory_order_relaxed);
  r = db->drv->exec(c, bound, rows);
  c->ctx = nullptr;
  r.own = rows;
  // A connection the driver can no longer use is closed and its slot left empty for the
  // next request to open: left in place, a lost socket poisons its slot until the process
  // ends.
  if (db->drv->broken && db->drv->broken(c)) {
    if (s) s->depth = 0;
    Arena *ca = c->arena;
    db->drv->close(c);
    arena_destroy(ca);
    wmutex_lock(&db->mu);
    db->conns[pinned ? s->slot : slot] = nullptr;
    wmutex_unlock(&db->mu);
    if (!r.ok && r.e.len > 0) {
      snprintf(db->err, sizeof db->err, "%.*s", WERR_FMT(r.e));
      db->err_cls = r.cls ? r.cls : WDBE_LOST;
    }
  }
  if (!pinned) db_unpin(db, slot);
  db_trace(bound->sql, r.nrows, wc_time_ms() - t0, r.ok, false);
#if !defined(__wasm__)
  // A write invalidates the tables it touched; a cacheable read is filed under the generation it
  // was read with. Both are the cache's business, not the driver's, and both are asked *after* the
  // statement so a failure does not invalidate anything — and *before* the arena goes, because the
  // statement text and its bound values live in it.
  if (cached_on && r.ok) qcache_note(db, qy, &r, &qcx);
#endif
  // The query is over, whatever happened. If it owned its arena (a `q()`), the arena goes
  // now; either way the handle is marked spent, so a second use is a sentence and not an
  // abort two requests later.
  if (qy->owns_arena && qy->arena) arena_destroy(qy->arena);
  if (qy->ring) qh_done(qy);
  if (!r.ok && r.e.len > 0) {
    snprintf(db->err, sizeof db->err, "%.*s", WERR_FMT(r.e));
    db->err_cls = r.cls;
  } else if (r.ok) {
    db->err_cls = WDBE_NONE;
  }
  return r;
}

WRes db_exec_sql(WDB *db, const char *sql) {
  Arena *a = arena_new(2048);
  WQuery *qy = q_a(a, sql);
  qy->owns_arena = true;
  return db_exec(db, qy);
}

WRes db_ping(WDB *db) {
  return db_exec_sql(db, db->drv->name[0] == 's' ? "select 1" : "select 1");
}

// The session a transaction runs on, opening one if this task has none yet. Every path
// that could not get a session (no database, pool exhausted) lands here with a sentence
// in `err` instead of a BEGIN on somebody else's connection.
static WDbSession *tx_session(WDB *db, char *err, size_t errcap) {
  if (!db) db = db_default();
  if (!db) {
    snprintf(err, errcap, "no database configured (set DATABASE_URL)");
    return nullptr;
  }
  WDbSession *s = session_for(db);
  if (!s) s = db_session_open(db, nullptr);
  if (!s) {
    snprintf(err, errcap, "%s", db->err[0] ? db->err : "database: no session available");
    return nullptr;
  }
  return s;
}

static void set_tx_err(WTx *t, WDB *db) {
  if (!t) return;
  snprintf(t->err, sizeof t->err, "%s",
           db && db->err[0] ? db->err : "the database refused the transaction statement");
}

WTx tx_begin(WDB *db) {
  WTx t = {0};
  t.db = db ? db : db_default();
  WDbSession *s = tx_session(t.db, t.err, sizeof t.err);
  if (!s) {
    t.db = nullptr;
    return t;
  }
  t.db = s->db;
  i32 want = s->depth + 1;
  WConn *c = s->db->conns[s->slot];
  bool ok = want == 1 ? s->db->drv->begin(c) : s->db->drv->savepoint(c, want);
  if (!ok) {
    set_tx_err(&t, s->db);
    return t;
  }
  s->depth = want;
  t.began = true;
  t.depth = want;
  return t;
}

// Commit the innermost open scope of this task's session. Returns false with a message in
// `t->err` when the database refused — and a COMMIT that finds nothing open is a refusal,
// not a no-op: a transaction that quietly does not commit is how a write vanishes.
//
// `t` may be null, which means "the innermost scope" — that is what `tx_commit_here()` is,
// and it is what lets a scope be committed without naming the handle the macro declares.
// The scope's own handle notices (its depth is above the session's now) and its cleanup
// does not roll back a transaction that is already over.
static bool tx_end(bool commit, WTx *t) {
  WDbSession *s = session_head();
  if (!s || s->depth == 0) {
    if (t) {
      snprintf(t->err, sizeof t->err, "%s: no open transaction on this task",
               commit ? "commit" : "rollback");
      t->done = true;
    }
    return !commit; // rolling back nothing is not a failure; committing nothing is
  }
  i32 depth = s->depth;
  if (t && t->began && t->depth != depth) {
    snprintf(t->err, sizeof t->err,
             "%s: that transaction is not the innermost open scope (depth %d of %d)",
             commit ? "commit" : "rollback", (int)t->depth, (int)depth);
    return false;
  }
  WConn *c = s->db->conns[s->slot];
  bool ok;
  if (commit)
    ok = depth == 1 ? s->db->drv->commit(c) : s->db->drv->release_savepoint(c, depth);
  else
    ok = depth == 1 ? s->db->drv->rollback(c) : s->db->drv->rollback_to(c, depth);
  if (!ok) {
    set_tx_err(t, s->db);
    if (commit) return false; // the session stays in the transaction: release rolls it back
  }
  s->depth = depth - 1;
  if (t) {
    t->done = true;
    t->began = false;
  }
  return ok;
}

bool tx_commit(WTx *t) { return tx_end(true, t); }
bool tx_rollback(WTx *t) { return tx_end(false, t); }

// A scope that was ended from inside (tx_commit_here/tx_rollback_here) or by an outer
// unwind has a depth above the session's, and must not be rolled back a second time.
void tx_cleanup(WTx *t) {
  if (!t || t->done) return;
  WDbSession *s = session_head();
  t->done = true;
  if (!t->began) return;
  if (!s || s->depth < t->depth) return;
  t->began = false;
  (void)tx_end(false, t);
}

// begin/commit/rollback run on this task's own connection, so two tasks' transactions
// cannot cross.
bool db_begin(WDB *db) {
  WTx t = tx_begin(db);
  return t.began; // the scope lives on the session; db_commit()/db_rollback() end it
}

bool db_commit(WDB *db) {
  (void)db;
  return tx_commit(nullptr);
}

bool db_rollback(WDB *db) {
  (void)db;
  return tx_rollback(nullptr);
}

// ---------------------------------------------------------------- model SQL

// An integer primary key is the database's to assign.
static bool field_is_auto(const WField *f) {
  return f->q.pk && (f->kind == WK_I64 || f->kind == WK_I32 || f->kind == WK_U64);
}

static void bind_field(WQuery *qy, const WField *f, const void *obj) {
  const char *name = model_column_name(f);
  // `.null`: 0 is the absence, and the column must receive NULL — the sentinel is this
  // side's spelling, not the server's. This is the line that would have saved a post
  // save from a foreign-key failure on "no cover".
  if (f->q.null && WC_KIND_IS_INT(f->kind) && wfield_get_i64(f, obj) == 0) {
    q_null(qy, name);
    return;
  }
  switch (f->kind) {
  case WK_I64:
    q_i64(qy, name, wfield_get_i64(f, obj));
    break;
  case WK_I32:
    q_i64(qy, name, (i64)wfield_get_i64(f, obj));
    break;
  case WK_U64:
    q_i64(qy, name, wfield_get_i64(f, obj));
    break;
  case WK_F64:
    q_f64(qy, name, wfield_get_f64(f, obj));
    break;
  case WK_F32:
    q_f64(qy, name, (f64)wfield_get_f64(f, obj));
    break;
  case WK_BOOL:
    q_bool(qy, name, wfield_get_bool(f, obj));
    break;
  case WK_STR:
  case WK_TEXT:
    q_str(qy, name, wfield_get_str(f, obj));
    break;
  case WK_ENUM:
    // the set's name is the column's text: readable in a terminal, stable on the wire
    q_str(qy, name, str_lit(wc_enum_name(f->en, (i32)wfield_get_i64(f, obj))));
    break;
  case WK_BYTES: {
    Bytes b = wfield_get_bytes(f, obj);
    q_bytes(qy, name, b);
    break;
  }
  default:
    // nested models and vectors have no scalar binding; a model that reaches
    // here should have marked the field .sql("column") or .no_json
    q_null(qy, name);
    break;
  }
}

// Named placeholders (:column) so the same statement serves both drivers: the
// binder rewrites them to `?` or `$n`. The *column* is spelled the driver's way
// (``rank`` on MySQL); the *placeholder name* stays the bare field name, because it
// is a key into the query's own parameter list and not SQL at all.
static str sql_insert_named(Arena *a, const WDBDriver *drv, const WModel *m) {
  Buf b;
  buf_init(&b, a, 128);
  str table = db_table(drv, m);
  buf_printf(&b, "INSERT INTO %.*s (", (int)table.n, table.p);
  bool first = true;
  for (i32 i = 0; i < m->n; i++) {
    if (field_is_auto(&m->fields[i])) continue;
    if (!first) buf_puts(&b, ", ");
    str c = db_ident(drv, &m->fields[i]);
    buf_write(&b, c.p, c.n);
    first = false;
  }
  buf_puts(&b, ") VALUES (");
  first = true;
  for (i32 i = 0; i < m->n; i++) {
    if (field_is_auto(&m->fields[i])) continue;
    if (!first) buf_puts(&b, ", ");
    buf_printf(&b, ":%s", model_column_name(&m->fields[i]));
    first = false;
  }
  buf_puts(&b, ")");
  return buf_str(&b);
}

static str sql_update_named(Arena *a, const WDBDriver *drv, const WModel *m) {
  Buf b;
  buf_init(&b, a, 128);
  str table = db_table(drv, m);
  buf_printf(&b, "UPDATE %.*s SET ", (int)table.n, table.p);
  bool first = true;
  for (i32 i = 0; i < m->n; i++) {
    const WField *f = &m->fields[i];
    if (f->q.pk) continue;
    if (!first) buf_puts(&b, ", ");
    str c = db_ident(drv, f);
    buf_printf(&b, "%.*s = :%s", (int)c.n, c.p, model_column_name(f));
    first = false;
  }
  bool any = false;
  for (i32 i = 0; i < m->n; i++) {
    const WField *f = &m->fields[i];
    if (!f->q.pk) continue;
    str c = db_ident(drv, f);
    buf_printf(&b, "%s%.*s = :%s", any ? " AND " : " WHERE ", (int)c.n, c.p,
               model_column_name(f));
    any = true;
  }
  return buf_str(&b);
}

WRes db_exec_model(WDB *db, const char *verb, const WModel *m, const void *obj) {
  // A WEBC_ROW is a row you computed: it has no table, and writing to the table its
  // struct name would spell is never what a caller meant. Say so where the mistake is
  // still one frame deep, instead of at "no such table: count_rows" from a server.
  if (m && m->internal)
    wc_panic(__FILE__, __LINE__,
             "db_%s: %s is a WEBC_ROW (a row you computed), not a table. Give it a "
             "WEBC_MODEL and a migration if it should be stored.",
             verb, m->name);
  // A missing handle is a configuration mistake, and the answer to it has to be a refusal:
  // dereferencing `db->drv` takes the process down with it (SIGSEGV in a handler, i.e. a
  // crashed request instead of a 500 with a line in the log).
  if (!db)
    return (WRes){.e = werr(WEBC_EDB, str_lit("database: no database configured (set DATABASE_URL)"))};
  // per-call arena: the SQL text and binds belong to this query only
  Arena *qa = arena_new(4096);
  WQuery *qy = q_a(qa, "");
  qy->owns_arena = true;
  if (strcmp(verb, "insert") == 0) {
    qy->sql = sql_insert_named(qa, db->drv, m);
    for (i32 i = 0; i < m->n; i++) {
      const WField *f = &m->fields[i];
      if (field_is_auto(f)) continue;
      bind_field(qy, f, obj);
    }
  } else if (strcmp(verb, "update") == 0) {
    qy->sql = sql_update_named(qa, db->drv, m);
    for (i32 i = 0; i < m->n; i++) {
      const WField *f = &m->fields[i];
      if (field_is_auto(f)) continue;
      bind_field(qy, f, obj);
    }
    for (i32 i = 0; i < m->n; i++) {
      const WField *f = &m->fields[i];
      if (f->q.pk) bind_field(qy, f, obj);
    }
  } else {
    return (WRes){.e = werr(WEBC_EINVAL, str_lit("unknown verb"))};
  }
  // An insert asks for the new row's key when the driver cannot report one any other
  // way: sqlite keeps a rowid and MySQL reports `last_insert_id` in its OK packet, but
  // postgres only answers with `returning`. The capability is a flag on the driver, not
  // a sniff at the placeholder character (which said "postgres" and nothing more).
  i64 pk_row = -1;
  if (strcmp(verb, "insert") == 0) {
    for (i32 i = 0; i < m->n; i++)
      if (m->fields[i].q.pk) pk_row = i;
    if (pk_row >= 0 && db->drv->returning)
      qy->sql = str_fmt(qa, "%.*s returning %s", (int)qy->sql.n, qy->sql.p,
                        model_column_name(&m->fields[pk_row]));
  }
  WRes r = db_exec(db, qy);
  if (strcmp(verb, "insert") == 0 && r.ok && r.last_id == 0 && r.nrows > 0)
    r.last_id = row_i64_at(&r.rows[0], 0);
  // The rows of an insert/update belong to nobody: `db_exec_model` is only ever
  // used for shape, never to read data back, so hand them back here. (They used
  // to be dropped on the floor — one arena per insert, i.e. an unbounded leak in
  // any app that writes.)
  wres_free(&r);
  return r;
}

bool db_insert_into(WDB *db, const WModel *m, const void *obj, i64 *id_out) {
  WRes r = db_exec_model(db, "insert", m, obj);
  if (id_out) *id_out = r.last_id;
  return r.ok;
}

// ---------------------------------------------------------------- typed filters

// A column as the driver in play spells it. The builder has no handle of its own — a filter
// is built with no database in sight, and the browser half has none at all — so it asks the
// default one: `rank` and `group` are reserved in MySQL, and a filter that named them bare
// was a syntax error there and fine here. With no default handle the name goes out bare,
// which is what every driver accepted before there was a second dialect.
static const WDBDriver *ww_dialect(void) {
  WDB *db = db_default();
  return db ? db->drv : nullptr;
}

static str ww_ident(WWhere *w, const WField *f) {
  const char *name = model_column_name(f);
  if (!f || f->q.sql || !name) return str_lit(name ? name : "");
  return db_ident_c(ww_dialect(), w->a, name);
}

static str ww_column(WWhere *w, const WModel *m, const char *field) {
  for (i32 i = 0; m && i < m->n; i++)
    if (strcmp(m->fields[i].name, field) == 0) return ww_ident(w, &m->fields[i]);
  // Unreachable from the macros (they reject an unknown field at compile time); a
  // hand-written call gets a loud failure instead of SQL that silently matches nothing.
  log_error("typed filter: no such field on the model", {"model", str_lit(m && m->name ? m->name : "?")},
            {"field", str_lit(field)});
  wc_panic(__FILE__, __LINE__, "typed filter on a field the model does not have");
  return str_lit(field);
}

void ww_init(WWhere *w, Arena *a) {
  memset(w, 0, sizeof *w);
  w->owns_arena = a == nullptr;
  w->a = a ? a : arena_new(1024);
  buf_init(&w->sql, w->a, 96);
  memset(&w->qy, 0, sizeof w->qy);
  w->qy.arena = w->a;
}

void ww_free(WWhere *w) {
  if (!w || !w->a) return;
  if (w->owns_arena) arena_destroy(w->a);
  memset(w, 0, sizeof *w);
}

const char *ww_sql(const WWhere *w) { return w && w->sql.p ? w->sql.p : ""; }
WQuery *ww_query(WWhere *w) { return w ? &w->qy : nullptr; }

static void ww_glue(WWhere *w, const char *op, const WModel *m, const char *field) {
  if (w->order) wc_panic(__FILE__, __LINE__, "typed filter: condition after order by");
  const char *sep = w->no_sep ? "" : (w->n ? (w->in_or ? " or " : " and ") : "");
  str col = ww_column(w, m, field);
  buf_printf(&w->sql, "%s%.*s %s :p%d", sep, (int)col.n, col.p, op, w->n);
  w->n++;
  w->no_sep = false;
}

static bool ww_kind_is_number(const WModel *m, const char *field) {
  for (i32 i = 0; m && i < m->n; i++)
    if (strcmp(m->fields[i].name, field) == 0) {
      u8 k = m->fields[i].kind;
      return k != WK_STR && k != WK_TEXT && k != WK_BYTES && k != WK_MODEL && k != WK_VEC;
    }
  return false;
}

// The parameter name has to outlive the call (q_push() keeps the pointer, not a copy),
// so it is made in the filter's own arena: a stack buffer here leaves the name dangling
// and the value binds as NULL, which reads as "no rows" rather than as an error.
static const char *ww_pname(WWhere *w) {
  char tmp[24];
  int k = snprintf(tmp, sizeof tmp, "p%d", w->n - 1);
  char *nm = arena_alloc(w->a, (size_t)k + 1);
  if (!nm) wc_panic(__FILE__, __LINE__, "typed filter: out of memory");
  memcpy(nm, tmp, (size_t)k + 1);
  return nm;
}

static void ww_i64(WWhere *w, const WModel *m, const char *field, const char *op, i64 v) {
  // A comparison on a text column would bind an integer against text and match
  // nothing; that is a mistake in the call, not a query to run.
  if (!ww_kind_is_number(m, field))
    wc_panic(__FILE__, __LINE__, "typed filter: numeric comparison on a non-numeric field");
  ww_glue(w, op, m, field);
  q_i64(&w->qy, ww_pname(w), v);
}

void ww_eq_i64_(WWhere *w, const WModel *m, const char *f, i64 v) { ww_i64(w, m, f, "=", v); }
void ww_ne_i64_(WWhere *w, const WModel *m, const char *f, i64 v) { ww_i64(w, m, f, "<>", v); }
void ww_gt_i64_(WWhere *w, const WModel *m, const char *f, i64 v) { ww_i64(w, m, f, ">", v); }
void ww_gte_i64_(WWhere *w, const WModel *m, const char *f, i64 v) { ww_i64(w, m, f, ">=", v); }
void ww_lt_i64_(WWhere *w, const WModel *m, const char *f, i64 v) { ww_i64(w, m, f, "<", v); }
void ww_lte_i64_(WWhere *w, const WModel *m, const char *f, i64 v) { ww_i64(w, m, f, "<=", v); }

void ww_eq_bool_(WWhere *w, const WModel *m, const char *f, bool v) {
  if (!ww_kind_is_number(m, f) && m) { /* bools are stored as integers */ }
  ww_glue(w, "=", m, f);
  q_bool(&w->qy, ww_pname(w), v);
}

// `.null` and the two questions that only it can answer. `= 0` is not one of them:
// a server holding NULL matches it never, and a server holding 0 is a different row.
static void ww_null(WWhere *w, const WModel *m, const char *field, bool negate) {
  if (w->order) wc_panic(__FILE__, __LINE__, "typed filter: condition after order by");
  const WField *f = nullptr;
  for (i32 i = 0; m && i < m->n; i++)
    if (strcmp(m->fields[i].name, field) == 0) f = &m->fields[i];
  if (!f || !f->q.null)
    wc_panic(__FILE__, __LINE__,
             "ww_is_null: %s.%s is not declared .null = true. A column that may hold NULL is "
             "the one this asks about; for any other field 0 is a value, and `ww_eq_i64(w, %s, "
             "%s, 0)` is the question you meant.",
             m ? m->name : "?", field, m ? m->name : "T", field);
  const char *sep = w->no_sep ? "" : (w->n ? (w->in_or ? " or " : " and ") : "");
  str col = ww_column(w, m, field);
  buf_printf(&w->sql, "%s%.*s is %snull", sep, (int)col.n, col.p, negate ? "not " : "");
  w->no_sep = false;
  w->n++;
}

void ww_is_null_(WWhere *w, const WModel *m, const char *f) { ww_null(w, m, f, false); }
void ww_not_null_(WWhere *w, const WModel *m, const char *f) { ww_null(w, m, f, true); }

void ww_eq_str_(WWhere *w, const WModel *m, const char *f, str v) {
  ww_glue(w, "=", m, f);
  q_str(&w->qy, ww_pname(w), v);
}

void ww_like_(WWhere *w, const WModel *m, const char *f, str pattern) {
  ww_glue(w, "like", m, f);
  q_str(&w->qy, ww_pname(w), pattern);
}

void ww_order_(WWhere *w, const WModel *m, const char *f, bool desc) {
  str col = ww_column(w, m, f);
  buf_printf(&w->sql, " order by %.*s%s", (int)col.n, col.p, desc ? " desc" : "");
  w->order = true;
}

// `(a like ? or b like ? or c like ?)`: a search over several columns is one question, not
// three. Everything between ww_or_begin and ww_or_end is OR'ed together and the group is
// AND'ed with the rest — built by hand it is string surgery, and skipped it means loading
// the whole table and filtering in memory after the query came back.
void ww_or_begin_(WWhere *w) {
  buf_printf(&w->sql, "%s(", w->n ? " and " : "");
  w->in_or = true;
  w->no_sep = true;
}

void ww_or_end_(WWhere *w) {
  buf_putc(&w->sql, ')');
  w->in_or = false;
}

void ww_contains_(WWhere *w, const WModel *m, const char *field, str v) {
  if (w->order) wc_panic(__FILE__, __LINE__, "typed filter: condition after order by");
  const char *sep = w->no_sep ? "" : (w->n ? (w->in_or ? " or " : " and ") : "");
  // lower(col) like lower(?): sqlite's LIKE is already case-insensitive and postgres's is
  // not, so the portable spelling is the explicit one. `%` in the value is data, and it is
  // escaped rather than becoming a wildcard.
  Buf pat;
  buf_init(&pat, w->a, v.n + 8);
  buf_putc(&pat, '%');
  for (size_t i = 0; i < v.n; i++) {
    // `!`, not a backslash: a backslash is an escape character *inside a string literal* on
    // MySQL, so the portable spelling `escape '\\'` is a syntax error there and this one is
    // not. The character is escaped itself, so a search for `!` still means `!`.
    if (v.p[i] == '%' || v.p[i] == '_' || v.p[i] == '!') buf_putc(&pat, '!');
    buf_putc(&pat, v.p[i]);
  }
  buf_putc(&pat, '%');
  // `escape '!'` is what makes the escaping above mean anything: sqlite's LIKE has no
  // default escape character, so without it `100%` would match `100 something` again.
  str col = ww_column(w, m, field);
  buf_printf(&w->sql, "%slower(%.*s) like lower(:p%d) escape '!'", sep, (int)col.n, col.p,
             w->n);
  q_str(&w->qy, str_fmt(w->a, "p%d", w->n).p, buf_str(&pat));
  w->n++;
  w->no_sep = false;
}

void ww_limit_(WWhere *w, i64 n, i64 offset) {
  // limit/offset are integers this code formats itself: no parameters to bind and no
  // way for a value to reach the SQL text
  if (n > 0) buf_printf(&w->sql, " limit %lld", (long long)n);
  if (offset > 0) buf_printf(&w->sql, " offset %lld", (long long)offset);
}

// The condition list of a filter, without the order and the limit: what a count, a sum or a
// page's total asks about. A sum of the first ten rows is a different question.
static str ww_text(WWhere *w) {
  return w && w->sql.p ? (str){w->sql.p, w->sql.n} : (str){"", 0};
}

static str ww_conditions(WWhere *w, Arena *a) {
  str c = ww_text(w);
  for (size_t i = 0; i < c.n; i++) {
    if (strncmp(c.p + i, " order by ", 10) == 0 || strncmp(c.p + i, " limit ", 7) == 0 ||
        strncmp(c.p + i, " offset ", 8) == 0)
      return str_sub(c, 0, i);
  }
  (void)a;
  return c.p ? c : str_lit("");
}

// The `order by …` half, without a trailing limit: the window a page renders in.
static str ww_order_only(WWhere *w, Arena *a) {
  str c = ww_text(w);
  for (size_t i = 0; i < c.n; i++) {
    if (strncmp(c.p + i, " order by ", 10) != 0) continue;
    size_t j = i;
    for (; j < c.n; j++)
      if (strncmp(c.p + j, " limit ", 7) == 0 || strncmp(c.p + j, " offset ", 8) == 0) break;
    return str_sub(c, i, j);
  }
  (void)a;
  return str_lit("");
}



WRes db_select_where(WDB *db, const WModel *m, const char *where, WQuery *params) {
  bool own = params == nullptr;
  Arena *a = own ? arena_new(4096) : params->arena;
  Buf sql;
  buf_init(&sql, a, 256);
  str cols = db_sql_columns(a, db->drv, m);
  str tbl = db_table(db->drv, m);
  buf_printf(&sql, "select %.*s from %.*s", (int)cols.n, cols.p, (int)tbl.n, tbl.p);
  // A clause that begins with `order`/`limit` has no conditions in it: it must follow the
  // table, not a `where` that would make the statement invalid.
  if (where && *where) {
    while (*where == ' ') where++;
    if (str_has_prefix_c(str_lit(where), "order ") || str_has_prefix_c(str_lit(where), "limit "))
      buf_printf(&sql, " %s", where);
    else
      buf_printf(&sql, " where %s", where);
  }
  WQuery *qy = params ? params : q_a(a, "");
  qy->arena = qy->arena ? qy->arena : a;
  // Only the query we just made is ours to destroy. A caller's own `q()` says who owns
  // it — clobbering that here leaked the caller's arena on every db_find()/db_all(), and
  // a leak that is invisible is worse than a crash that is not.
  if (!params) qy->owns_arena = true;
  qy->sql = buf_str(&sql);
  return db_exec(db, qy);
}

// The clause form: `clause` is appended after the table name, so it can be an
// `order by`, a `where … order by …`, a `limit`. `db_all()` had no way to say either
// — every list came back in whatever order the storage engine felt like, which is
// fine for a test and wrong for a front page.
// ---------------------------------------------------------------- one row, one fetch
//
// `ref()` is served by whatever fetched that row, and a page typically fetches the same small
// table several times: `/` reads the three latest posts' categories, the featured post's
// series' category, and then the whole category table. Those are three statements returning
// two distinct sets of the same rows.
//
// The pool below is per request: a row already materialised this request, keyed by its primary
// key, is handed out instead of fetched again, and a prefetch whose keys are all pooled runs no
// statement at all. Rows live in the request arena, so the pool is invalidated by the arena
// changing — the same lifetime, no ownership question, nothing to free. It is a *correctness*
// rule as much as a speed one: two different copies of one row in one page is how a page shows
// two versions of the same category.

// Keyed by NAME, not by descriptor pointer: a model is declared in a header, so every
// translation unit has its own static copy of it. `all_categories()` runs in home.c and
// `wc_prefetch()` runs in db.c, and their `Category_model` are two different objects — a
// pointer key would silently never match. The names are the same everywhere, and they are
// what the SQL uses.
// A page asks for the same row twice: the listing loads the categories it needs, then the card
// renderer prefetches the category of each post. Each of those repeats is a real Postgres round
// trip — about 0.08 ms at the server and again in the driver — so the row is kept for the life
// of the request, which in this framework is the life of the request arena.
#define WC_ROW_POOL_MAX 64 // a page needs a handful; the bound is a backstop, not a target

typedef struct WPoolEnt {
  const char *mname;
  const char *fname; // the field the key is the *value of* — a relation names its own key
  str key;
  void *row;
} WPoolEnt;

typedef struct WPool {
  i32 n;
  WPoolEnt e[WC_ROW_POOL_MAX];
} WPool;

// The pool lives inside the request arena and is reached through it. That is the whole
// point. A thread is not a request in this framework: tasks interleave at every database
// await and arenas are recycled at the same addresses, so anything held in thread-local
// state outlives the request that owns it — a pool keyed on the thread hands a stale row to
// the next request on the same worker.
// Keyed on the arena, the pool and the rows it points at are allocated and released
// together, and no code path can leave a pointer behind.
static WPool *wpool(bool create) {
  Arena *a = wc_arena_get();
  if (!a) return nullptr; // not in a request (a test harness, a job): behave as before
  WPool *p = (WPool *)a->row_pool;
  if (p || !create) return p;
  p = arena_zalloc(a, sizeof *p);
  a->row_pool = p;
  return p;
}

static bool sql_is_select(str sql) {
  const char *p = sql.p;
  size_t n = sql.n;
  while (n > 0 && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '(')) {
    p++;
    n--;
  }
  return n >= 6 && strncasecmp(p, "select", 6) == 0;
}

// Forget every row this request has cached. Called before a write, so that a save followed by
// a render in the same request shows the new value; the end of a request needs no call at all,
// because the arena takes the pool with it.
void wc_row_pool_clear(void) {
  WPool *p = wpool(false);
  if (p) p->n = 0;
}

static void *pool_get(const WModel *m, const WField *f, str key) {
  if (!m || !f || !key.p || !key.n) return nullptr;
  WPool *p = wpool(false);
  if (!p) return nullptr;
  for (i32 i = 0; i < p->n; i++)
    if (strcmp(p->e[i].mname, m->name) == 0 && strcmp(p->e[i].fname, f->name) == 0 &&
        str_eq(p->e[i].key, key))
      return p->e[i].row;
  return nullptr;
}

static void pool_put(const WModel *m, const WField *f, str key, void *row) {
  if (!m || !f || !key.p || !key.n || !row) return;
  WPool *p = wpool(true);
  if (!p) return;
  for (i32 i = 0; i < p->n; i++) // the freshest copy wins: a re-read replaces the old row
    if (strcmp(p->e[i].mname, m->name) == 0 && strcmp(p->e[i].fname, f->name) == 0 &&
        str_eq(p->e[i].key, key)) {
      p->e[i].row = row;
      return;
    }
  if (p->n >= WC_ROW_POOL_MAX) return; // bounded: past it, prefetch behaves as it always did
  p->e[p->n++] = (WPoolEnt){.mname = m->name, .fname = f->name, .key = key, .row = row};
}

// One row joins the pool under its primary key and under every text field, because a relation
// names its target by whatever field it declared (posts point at `category_slug`, not at a
// category id). Registering only the primary key would pool "1", "2", "3" while the
// prefetch asks for "risk", and no page would hit once.
static void pool_put_row(const WModel *m, const void *row, Arena *a) {
  if (!m || !row) return;
  for (i32 i = 0; i < m->n; i++) {
    const WField *f = &m->fields[i];
    if (!f->q.pk && !WC_KIND_IS_TEXT(f->kind)) continue;
    str k = key_text(a, f, row);
    if (k.p && k.n) pool_put(m, f, k, (void *)(uintptr_t)row);
  }
}

// A bound value as the key the pool stores, spelled the way key_text() spells a column: text
// as-is, everything else as decimal, which is what the relation key arrives as.
static str wv_text_for_pool(Arena *a, WValue v) {
  if (v.is_i64) return str_fmt(a, "%lld", (long long)v.n);
  return v.s; // text as-is: the same spelling key_text() gives a text column
}

static void pool_put_rows(const WModel *m, void *v, const WVecDesc *vec, Arena *a) {
  if (!v || !vec) return;
  i32 n = *(const i32 *)((const char *)v + vec->off_n);
  // `off_at` is the offset of the `at` *member*, a pointer to the elements — not the elements.
  const char *at = *(const char *const *)((const char *)v + vec->off_at);
  if (!at) return;
  for (i32 i = 0; i < n; i++) pool_put_row(m, at + (size_t)i * vec->elem_size, a);
}

void *db_all_clause_vec(WDB *db, const WModel *m, const WVecDesc *vec, const char *clause,
                        WQuery *qy_params) {
  if (!db) {
    // A list that is silently empty because no database was configured is the kind
    // of bug a person spends an hour on.
    webc_fail(500, "db", "no database configured (set DATABASE_URL)");
  }
  // The vector and its strings must live in memory the caller owns. When there
  // is no request arena (a background task), take a fresh one: db->arena is
  // shared by every task and appending to it from two threads corrupts it
  // (measured: 16 concurrent requests, ASan "requested allocation size 0x8000…").
  Request *cur = webc_req_current();
  Arena *live = wc_arena_get();
  Arena *a = cur ? cur->arena : (live ? live : arena_new(8 * 1024));
  void *v = arena_zalloc(a, vec->size);
  // A collection carries its own descriptor, so `json(200, v)` and
  // `for_each(t, v)` work on what a query returned without naming the type twice.
  *(const WVecDesc **)v = vec;
  WRes r;
  if (clause && *clause) {
    // The clause is `where … order by …`, and the statement is assembled around it. A whole
    // `select …` pasted in here compiles, runs and matches nothing — six existing posts showed
    // "No posts match" and nothing else — because the statement it builds is
    // `select … from posts select * from posts where …`. One word of the clause is enough to
    // refuse, and the test is on the *first* word: a `where x in (select …)` subquery is a
    // perfectly good clause, so only a clause that opens with `select` is the mistake.
    {
      const char *first = clause;
      while (*first == ' ' || *first == '\t' || *first == '\n' || *first == '\r' || *first == '(')
        first++;
      if (strncasecmp(first, "select ", 7) == 0)
        webc_fail(500, "db_clause",
                  "db_all_where() takes a clause — \"where status = ? order by id desc\" — not a "
                  "statement, and a whole `select …` here matches nothing. For a statement use "
                  "db_exec(db, q(\"select …\")); for a filtered list use db_all_w with a WWhere");
    }
    Arena *qa = qy_params ? qy_params->arena : (cur ? cur->arena : a);
    Buf sql;
    buf_init(&sql, qa, 256);
    str cols = db_sql_columns(qa, db->drv, m);
    str tbl = db_table(db->drv, m);
    buf_printf(&sql, "select %.*s from %.*s %s", (int)cols.n, cols.p, (int)tbl.n, tbl.p, clause);
    WQuery *qy = qy_params ? qy_params : q_a(qa, "");
    qy->arena = qy->arena ? qy->arena : qa;
    qy->sql = buf_str(&sql);
    r = db_exec(db, qy);
  } else {
    r = db_select_where(db, m, nullptr, nullptr);
  }
  if (!r.ok) {
    wres_free(&r);
    return v;
  }
  for (i32 i = 0; i < r.nrows; i++) {
    void *elem = arena_zalloc(a, vec->elem_size);
    model_from_row_dup(m, &r.rows[i], elem, a); // the row goes away below
    wvec_push_raw(a, vec, v, elem);
  }
  pool_put_rows(m, v, vec, a); // a prefetch of the same model now costs nothing
  wres_free(&r);
  return v;
}

void *db_all_vec(WDB *db, const WModel *m, const WVecDesc *vec) {
  return db_all_clause_vec(db, m, vec, nullptr, nullptr);
}

// ---------------------------------------------------------------- relations

// One query for the whole list: `select * from sections where slug in (?, ?, ?)`. The keys
// come from the rows, so a relation costs one statement however long the page is — which is
// the difference between a page that is correct and a page that is correct *and* fast.
// A relation's key as text, whichever type it is declared with. `WEBC_MODEL_REF(Story,
// section, Section, slug)` joins on a slug, `WEBC_MODEL_REF(Line, order_id, Order, id)` on an
// integer — and an integer field read as text is "", so an integer-keyed relation silently
// matches nothing: the prefetch runs, costs a query, and links no rows.
static str key_text(Arena *a, const WField *f, const void *row) {
  if (!f) return (str){"", 0};
  if (WC_KIND_IS_TEXT(f->kind)) return wfield_get_str(f, row);
  if (f->kind == WK_BYTES) return (str){"", 0};
  return str_fmt(a, "%lld", (long long)wfield_get_i64(f, row));
}

void wc_prefetch(WDB *db, const WModel *m, const void *vec, const char *field) {
  const RefRule *rule = wc_ref_rule(m ? m->name : "", field);
  if (!rule) {
    wc_panic(__FILE__, __LINE__,
             "prefetch: %s.%s is not a declared relation. Add "
             "WEBC_MODEL_REF(%s, %s, RefModel, keyfield) next to the models.",
             m ? m->name : "?", field, m ? m->name : "Model", field);
    return;
  }
  const WVecDesc *vd = *(const WVecDesc *const *)vec;
  if (!vd) return;
  const WModel *target = nullptr;
  for (i32 i = 0; i < model_registry_count(); i++) {
    const WModel *cand = model_registry_at(i);
    if (strcmp(cand->name, rule->target) == 0) target = cand;
  }
  if (!target) {
    wc_panic(__FILE__, __LINE__,
             "prefetch: %s.%s points at %s, which is not a registered model (it has to be "
             "declared with WEBC_MODEL before it is used as a target).",
             m->name, field, rule->target);
    return;
  }
  const WField *tf_key = nullptr;
  for (i32 i = 0; i < target->n; i++)
    if (strcmp(target->fields[i].name, rule->key) == 0) tf_key = &target->fields[i];
  const WField *owner_field = nullptr, *owner_pk = nullptr;
  for (i32 i = 0; i < m->n; i++) {
    if (strcmp(m->fields[i].name, field) == 0) owner_field = &m->fields[i];
    if (m->fields[i].q.pk) owner_pk = &m->fields[i];
  }
  if (!tf_key || !owner_field) {
    wc_panic(__FILE__, __LINE__,
             "prefetch: %s.%s -> %s.%s does not resolve: a field is missing on one of them.",
             m->name, field, target->name, rule->key);
    return;
  }
  Arena *a = wc_arena_get();
  const char *at = *(const char *const *)((const char *)vec + vd->off_at);
  i32 n = *(const i32 *)((const char *)vec + vd->off_n);
  if (!at || n <= 0) return;
  // Which keys the page still needs: the ones this request has not already materialised. A page
  // that reads the category table and then prefetches three posts' categories asks one question,
  // and the pool is why it is one.
  void **found = arena_zalloc(a, (size_t)n * sizeof(void *));
  Buf sql;
  buf_init(&sql, a, 256);
  i32 missing = 0;
  for (i32 i = 0; i < n; i++) {
    str k = key_text(a, owner_field, at + (size_t)i * vd->elem_size);
    if (!k.p || !k.n) continue;
    void *hit = pool_get(target, tf_key, k);
    if (hit) {
      found[i] = hit;
      continue;
    }
    // already asked for by an earlier owner in this same list? then it is one parameter
    bool dup = false;
    for (i32 j = 0; j < i && !dup; j++) {
      str kj = key_text(a, owner_field, at + (size_t)j * vd->elem_size);
      dup = kj.p && kj.n && str_eq(kj, k);
    }
    if (!dup) missing++;
  }
  if (missing > 0) {
    // The distinct missing keys, as bound parameters: a small list, and never spliced into SQL.
    str cols = db_sql_columns(a, db->drv, target);
    str tbl = db_table(db->drv, target);
    str keyc = db_ident(db->drv, tf_key);
    buf_printf(&sql, "select %.*s from %.*s where %.*s in (", (int)cols.n, cols.p, (int)tbl.n,
               tbl.p, (int)keyc.n, keyc.p);
    i32 nkeys = 0;
    for (i32 i = 0; i < n; i++) {
      str k = key_text(a, owner_field, at + (size_t)i * vd->elem_size);
      if (!k.p || !k.n || found[i]) continue;
      bool dup = false;
      for (i32 j = 0; j < i && !dup; j++) {
        str kj = key_text(a, owner_field, at + (size_t)j * vd->elem_size);
        dup = kj.p && kj.n && str_eq(kj, k);
      }
      if (dup) continue;
      buf_printf(&sql, "%s?", nkeys ? ", " : "");
      nkeys++;
    }
    buf_putc(&sql, ')');
    WQuery *qy = q(buf_cstr(&sql));
    for (i32 i = 0; i < n; i++) {
      str k = key_text(a, owner_field, at + (size_t)i * vd->elem_size);
      if (!k.p || !k.n || found[i]) continue;
      bool dup = false;
      for (i32 j = 0; j < i && !dup; j++) {
        str kj = key_text(a, owner_field, at + (size_t)j * vd->elem_size);
        dup = kj.p && kj.n && str_eq(kj, k);
      }
      if (dup) continue;
      q_str(qy, "", k); // positional: the driver takes them in the order they were pushed
    }
    WRes r = db_exec(db, qy);
    if (!r.ok) {
      wres_free(&r);
      db_fail(target->name);
    }
    // Every target row is kept for the rest of the request (it is what `ref()` hands out),
    // it joins the pool so no later prefetch asks again, and the link table says which owner
    // row asked for which one.
    for (i32 i = 0; i < r.nrows; i++) {
      void *row = arena_alloc(a, target->size ? target->size : 1);
      model_from_row_dup(target, &r.rows[i], row, a);
      str key = key_text(a, tf_key, row);
      pool_put_row(target, row, a);
      for (i32 k = 0; k < n; k++) {
        str ok = key_text(a, owner_field, at + (size_t)k * vd->elem_size);
        if (!ok.p || !str_eq(ok, key)) continue;
        found[k] = row;
      }
    }
    wres_free(&r);
  }
  // Link owners to rows — including the ones the pool already held, which is what makes a row
  // fetched once reachable from every owner that asked for it.
  const RefRule *self = rule;
  for (i32 k = 0; k < n; k++) {
    if (!found[k]) continue;
    const char *owner = at + (size_t)k * vd->elem_size;
    i64 pk = owner_pk ? wfield_get_i64(owner_pk, owner) : (i64)(uintptr_t)owner;
    wc_ref_link(self->owner, pk, self->field, found[k], a);
  }
}

// ---------------------------------------------------------------- a collection, prefetched
//
// `ref()` is one row; `refs()` is a list, and a list is where N+1 hides: a page of thirteen
// sections that each renders its stories is thirteen queries unless somebody asked for all
// of them at once. `prefetch(all, Section, stories)` is that ask, and it is one statement —
// `select * from stories where section in (?, ?, …)` over the owner keys of the page,
// chunked so a page of 2 000 rows is four statements and never one statement with 2 000
// parameters (every server has a ceiling, and they differ).

// A vector built at run time: the same layout WC_VEC_STRUCT gives every collection, so the
// existing prefetch machinery can read it through a descriptor with its own offsets. It is
// how "the rows the previous hop linked" become "the collection the next hop prefetches".
typedef struct WCTmpVec {
  const WVecDesc *desc;
  char *at;
  i32 n;
  i32 cap;
  Arena *arena;
} WCTmpVec;

static WVecDesc tmp_desc(const WModel *m) {
  WVecDesc d = {0};
  d.magic = WC_VEC_MAGIC; // built by hand, so it says what it is by hand
  d.name = "<prefetch>";
  d.size = (u32)sizeof(WCTmpVec);
  d.elem_size = m->size ? m->size : 1;
  d.off_at = (u32)offsetof(WCTmpVec, at);
  d.off_n = (u32)offsetof(WCTmpVec, n);
  d.off_cap = (u32)offsetof(WCTmpVec, cap);
  d.off_arena = (u32)offsetof(WCTmpVec, arena);
  d.elem_kind = WK_MODEL;
  d.sub = m;
  return d;
}

// Any collection — a VecSection the app owns or a WCTmpVec this file built — is pushed into
// through its own descriptor, because that is what the descriptor is for.
static void coll_push(void *coll, const void *row) {
  WCTmpVec *v = coll;
  if (!v || !v->desc) return;
  Arena *a = v->arena ? v->arena : wc_arena_get();
  wvec_push_impl(&v->at, &v->n, &v->cap, v->desc->elem_size, a, row);
}

static const WModel *model_by_name(const char *name) {
  for (i32 i = 0; i < model_registry_count(); i++) {
    const WModel *m = model_registry_at(i);
    if (strcmp(m->name, name) == 0) return m;
  }
  return nullptr;
}

static const WField *model_field_named(const WModel *m, const char *name) {
  for (i32 i = 0; m && i < m->n; i++)
    if (strcmp(m->fields[i].name, name) == 0) return &m->fields[i];
  return nullptr;
}

static const WField *model_pk_field(const WModel *m) {
  for (i32 i = 0; m && i < m->n; i++)
    if (m->fields[i].q.pk) return &m->fields[i];
  return nullptr;
}

// One query per chunk of 512 owner keys, however long the page is.
#define WC_PREFETCH_CHUNK 512

// The collection itself: `select * from <target> where <fk> in (…)`, linked back to every
// owner row whose key the FK names. Returns the rows in one flat collection, which is what
// a second hop needs.
static void wc_prefetch_many(WDB *db, const WModel *m, const void *vec, const char *field,
                             void *flat) {
  const ManyRule *rule = wc_many_rule(m->name, field);
  if (!rule) {
    wc_panic(__FILE__, __LINE__,
             "prefetch: %s.%s is not a declared collection. Add WEBC_MODEL_MANY(%s, %s, "
             "Target, back_field) next to the models.",
             m->name, field, m->name, field);
    return;
  }
  const WModel *target = model_by_name(rule->target);
  if (!target) {
    wc_panic(__FILE__, __LINE__,
             "prefetch: %s.%s points at %s, which is not a registered model.",
             m->name, field, rule->target);
    return;
  }
  // The FK column lives on the target; the *key* it holds is named by the belongs-to rule,
  // so the relation is still described in exactly one place.
  const RefRule *back = wc_ref_rule(target->name, rule->back_field);
  const WField *fk = model_field_named(target, rule->back_field);
  const WField *owner_pk = model_pk_field(m);
  const WField *key = back ? model_field_named(m, back->key) : nullptr;
  if (!fk || !owner_pk || !key) {
    wc_panic(__FILE__, __LINE__,
             "prefetch: %s.%s -> %s.%s does not resolve. It needs WEBC_MODEL_REF(%s, %s, %s, "
             "key) for the key, and a `%s` field on %s.",
             m->name, field, target->name, rule->back_field, target->name, rule->back_field,
             m->name, rule->back_field, target->name);
    return;
  }
  const WVecDesc *vd = *(const WVecDesc *const *)vec;
  if (!vd) return;
  const char *at = *(const char *const *)((const char *)vec + vd->off_at);
  i32 n = *(const i32 *)((const char *)vec + vd->off_n);
  if (!at || n <= 0) return;
  Arena *a = wc_arena_get();
  // one collection per owner row, created once and appended to across chunks
  for (i32 i = 0; i < n; i++) {
    const char *owner = at + (size_t)i * vd->elem_size;
    i64 pk = wfield_get_i64(owner_pk, owner);
    if (!wc_many_vec(m->name, pk, field, a)) {
      void *coll = wc_many_vec_new(rule, a);
      if (coll) wc_many_link(m->name, pk, field, coll, a);
    }
  }
  str cols = db_sql_columns(a, db->drv, target);
  str tbl = db_table(db->drv, target);
  str fkc = db_ident(db->drv, fk);
  for (i32 start = 0; start < n; start += WC_PREFETCH_CHUNK) {
    i32 end = start + WC_PREFETCH_CHUNK < n ? start + WC_PREFETCH_CHUNK : n;
    Buf sql;
    buf_init(&sql, a, 256 + (size_t)(end - start) * 4);
    buf_printf(&sql, "select %.*s from %.*s where %.*s in (", (int)cols.n, cols.p, (int)tbl.n,
               tbl.p, (int)fkc.n, fkc.p);
    i32 nkeys = 0;
    for (i32 i = start; i < end; i++) {
      const char *owner = at + (size_t)i * vd->elem_size;
      if (!key_text(a, key, owner).p) continue; // a null key matches nothing
      buf_printf(&sql, "%s?", nkeys ? ", " : "");
      nkeys++;
    }
    if (!nkeys) continue;
    buf_putc(&sql, ')');
    WQuery *qy = q(buf_cstr(&sql));
    for (i32 i = start; i < end; i++) {
      const char *owner = at + (size_t)i * vd->elem_size;
      str k = key_text(a, key, owner);
      if (!k.p) continue;
      q_str(qy, "", k);
    }
    WRes r = db_exec(db, qy);
    if (!r.ok) {
      wres_free(&r);
      db_fail(target->name);
    }
    for (i32 i = 0; i < r.nrows; i++) {
      void *row = arena_alloc(a, target->size ? target->size : 1);
      model_from_row_dup(target, &r.rows[i], row, a);
      str fkv = key_text(a, fk, row);
      coll_push(flat, row);
      for (i32 k = start; k < end; k++) {
        const char *owner = at + (size_t)k * vd->elem_size;
        str ok = key_text(a, key, owner);
        if (!ok.p || !str_eq(ok, fkv)) continue;
        i64 pk = wfield_get_i64(owner_pk, owner);
        void *coll = wc_many_vec(m->name, pk, field, a);
        coll_push(coll, row);
      }
    }
    wres_free(&r);
  }
}

// Every row a hop linked, deduplicated by primary key — the input to the next hop, and the
// reason two hops cost two statements instead of one per row.
static void wc_prefetch_collect(const WModel *m, const void *vec, const char *field,
                               void *flat) {
  const WVecDesc *vd = *(const WVecDesc *const *)vec;
  if (!vd) return;
  const char *at = *(const char *const *)((const char *)vec + vd->off_at);
  i32 n = *(const i32 *)((const char *)vec + vd->off_n);
  const WField *pk = model_pk_field(m);
  if (!at || n <= 0 || !pk) return;
  for (i32 i = 0; i < n; i++) {
    const char *row = at + (size_t)i * vd->elem_size;
    const void *target = wc_ref(m, row, field);
    if (!target) continue;
    i64 id = wfield_get_i64(pk, target);
    bool seen = false;
    WCTmpVec *f = flat;
    const WVecDesc *fd = f->desc;
    for (i32 k = 0; k < f->n && !seen; k++)
      seen = wfield_get_i64(pk, f->at + (size_t)k * fd->elem_size) == id;
    if (!seen) coll_push(flat, target);
  }
}

void wc_prefetch_path(WDB *db, const WModel *m, const void *vec, const char *const *fields,
                      i32 nfields) {
  if (!db || !m || !vec || !fields || nfields <= 0) return;
  const WModel *cur = m;
  const void *curvec = vec;
  // Two scratch collections, alternating: hop N reads what hop N-1 linked and writes the rows
  // it linked itself. One buffer would be overwritten by the hop that is about to read it.
  WVecDesc tdesc[2] = {0};
  WCTmpVec tvec[2] = {0};
  for (i32 hop = 0; hop < nfields; hop++) {
    const char *field = fields[hop];
    const ManyRule *many = wc_many_rule(cur->name, field);
    const RefRule *one = wc_ref_rule(cur->name, field);
    if (!many && !one) {
      wc_panic(__FILE__, __LINE__,
               "prefetch: %s.%s is not a declared relation. Declare it with "
               "WEBC_MODEL_REF(%s, %s, Target, key) or WEBC_MODEL_MANY(%s, %s, Target, "
               "back_field).",
               cur->name, field, cur->name, field, cur->name, field);
      return;
    }
    const WModel *next = model_by_name(many ? many->target : one->target);
    if (!next) {
      wc_panic(__FILE__, __LINE__, "prefetch: %s.%s points at %s, which is not registered.",
               cur->name, field, many ? many->target : one->target);
      return;
    }
    i32 slot = hop & 1;
    tdesc[slot] = tmp_desc(next);
    tvec[slot] = (WCTmpVec){.desc = &tdesc[slot], .arena = wc_arena_get()};
    void *flat = &tvec[slot];
    if (many)
      wc_prefetch_many(db, cur, curvec, field, flat);
    else {
      // one statement for the whole list (sqlite and postgres both reach the same code)
      wc_prefetch(db, cur, curvec, field);
      wc_prefetch_collect(cur, curvec, field, flat);
    }
    if (hop + 1 >= nfields) return; // the last hop needs no rows of its own
    cur = next;
    curvec = flat;
  }
}

// ---------------------------------------------------------------- a filter, answered
//
// One builder, three questions: the list, the first row, the count. Spelled three ways —
// `db_all_where` with a string for the list, `db_find` with a string for the row,
// `db_count` for all of them or nothing — the string forms are the ones apps reach for, and
// a string is a filter that loses the field check.

// A `WWhere` carries conditions, an order and a limit — but not the keyword: `db_select`
// takes a bare condition list and `db_all_clause_vec` takes whatever follows the table
// name. This is the one place that knows the difference, so no caller has to.
// The clause as `... from <table> <clause>` wants it: `where a = ? order by b` when there
// are conditions, `order by b limit 5` when there are only those. The leading space the
// builder writes before `order by` is trimmed first — an untrimmed query that is only an
// order and a limit comes out as `where  order by views desc`, which the database rejects
// and which reads as "no rows" on the page.
static const char *ww_clause(WWhere *w, Arena *a) {
  const char *c = w ? ww_sql(w) : "";
  while (c && (*c == ' ' || *c == '\t')) c++;
  if (!c || !*c) return "";
  if (str_has_prefix_c(str_lit(c), "order ") || str_has_prefix_c(str_lit(c), "limit ") ||
      str_has_prefix_c(str_lit(c), "where "))
    return c;
  Buf b;
  buf_init(&b, a, 128);
  buf_puts(&b, "where ");
  buf_puts(&b, c);
  return buf_cstr(&b);
}

void *db_all_w_(WDB *db, const WModel *m, const WVecDesc *vec, WWhere *w) {
  Arena *a = wc_arena_get();
  return db_all_clause_vec(db, m, vec, w ? ww_clause(w, a) : nullptr,
                           w ? ww_query(w) : nullptr);
}

static bool clause_has_limit(const char *clause) {
  return clause && strstr(clause, " limit ") != nullptr;
}

void *db_one_w_(WDB *db, const WModel *m, WWhere *w) {
  Arena *a = wc_arena_get();
  const char *clause = ww_clause(w, a);
  Buf sql;
  buf_init(&sql, a, 128);
  buf_puts(&sql, clause);
  if (!clause_has_limit(clause)) buf_puts(&sql, " limit 1");
  // db_select_where splices its argument after `where`, so it wants the conditions only
  Arena *bare_a = wc_arena_get();
  Buf bare;
  buf_init(&bare, bare_a, 128);
  buf_puts(&bare, w ? ww_sql(w) : "");
  if (!clause_has_limit(buf_cstr(&bare))) buf_puts(&bare, " limit 1");
  WRes r = db_select_where(db, m, buf_cstr(&bare), w ? ww_query(w) : nullptr);
  if (!r.ok) {
    wres_free(&r);
    db_fail(m->name);
  }
  if (r.nrows == 0) {
    wres_free(&r);
    return nullptr;
  }
  void *out = arena_zalloc(a, m->size ? m->size : 1);
  model_from_row_dup(m, &r.rows[0], out, a);
  pool_put_row(m, out, a);
  wres_free(&r);
  return out;
}

i64 db_count_w_(WDB *db, const WModel *m, WWhere *w) {
  Arena *a = wc_arena_get();
  // counting does not need an order, a limit or an offset: drop them rather than make the
  // database sort a table to answer "how many" — and never emit a dangling `where`
  str cond = ww_conditions(w, a);
  Buf sql;
  buf_init(&sql, a, 160);
  str tbl = db_table(db->drv, m);
  buf_printf(&sql, "select count(*) as n from %.*s", (int)tbl.n, tbl.p);
  if (cond.n) buf_printf(&sql, " where %.*s", (int)cond.n, cond.p);
  // Its own query, with the builder's bound values copied across: a `WWhere` is a
  // description the caller owns and may reuse, so counting must not consume it.
  WQuery *src = w ? ww_query(w) : nullptr;
  WQuery *qy = q(buf_cstr(&sql));
  for (i32 i = 0; src && i < src->nparams; i++)
    q_push(qy, str_cstr(a, src->names[i]), src->params[i]);
  WRes r = db_exec(db, qy);
  if (!r.ok) {
    wres_free(&r);
    db_fail(m->name);
  }
  i64 n = r.nrows ? row_i64_at(&r.rows[0], 0) : 0;
  wres_free(&r);
  return n;
}

WValue wv_of_str(str s) { return (WValue){.is_i64 = false, .s = s}; }
WValue wv_of_cstr(const char *p) { return (WValue){.is_i64 = false, .s = str_lit(p)}; }
WValue wv_of_i64(i64 n) { return (WValue){.is_i64 = true, .n = n}; }
WValue wv_of_u64(u64 n) { return (WValue){.is_i64 = true, .n = (i64)n}; }

// The field has to be one of the model's, and the panic says which ones are: the whole
// point of this vocabulary over `db_find("slug = :s", qy)` is that a name can be checked.
static const WField *by_field(const WModel *m, const char *field) {
  for (i32 i = 0; i < m->n; i++)
    if (strcmp(m->fields[i].name, field) == 0) return &m->fields[i];
  Buf names;
  buf_init(&names, wc_arena_get(), 128);
  for (i32 i = 0; i < m->n; i++)
    buf_printf(&names, "%s%s", i ? ", " : "", m->fields[i].name);
  panic("%s has no field '%s'. Its fields are: %s", m->name, field, buf_cstr(&names));
  return nullptr;
}

// One field equals one value, built as the same WWhere the typed macros build — so the
// column name comes from the model (a `.sql` override still applies) and the value is
// always a bound parameter.
static WWhere *by_filter(WWhere *w, Arena *a, const WModel *m, const char *field, WValue v) {
  const WField *f = by_field(m, field);
  ww_init(w, a);
  if (v.is_i64)
    ww_eq_i64_(w, m, f->name, v.n);
  else
    ww_eq_str_(w, m, f->name, v.s);
  return w;
}

void *db_by_(WDB *db, const WModel *m, const char *field, WValue v) {
  Arena *a = wc_arena_get();
  // A row this request already holds answers `db_by` without a statement — the same rule
  // prefetch follows, so "the category this post is filed under" and "the category table" are
  // one read on a page that needs both.
  {
    const WField *f = model_field(m, str_lit(field));
    str key = wv_text_for_pool(a, v);
    if (f && key.p && key.n) {
      void *hit = pool_get(m, f, key);
      if (hit) return hit;
    }
  }
  WWhere *w = by_filter(&(WWhere){0}, a, m, field, v);
  void *row = db_one_w_(db, m, w);
  ww_free(w);
  return row;
}

void *db_all_by_(WDB *db, const WModel *m, const WVecDesc *vec, const char *field, WValue v) {
  Arena *a = wc_arena_get();
  WWhere *w = by_filter(&(WWhere){0}, a, m, field, v);
  void *out = db_all_w_(db, m, vec, w);
  ww_free(w);
  return out;
}

i64 db_count_by_(WDB *db, const WModel *m, const char *field, WValue v) {
  Arena *a = wc_arena_get();
  WWhere *w = by_filter(&(WWhere){0}, a, m, field, v);
  i64 n = db_count_w_(db, m, w);
  ww_free(w);
  return n;
}

// `db_increment(Story, st, views)` — one statement, `set views = views + 1`, instead of
// writing the whole row back to add one. The difference is not style: a row with a 4 KB
// body was rewritten, WAL-logged and replicated on every page view.
bool db_increment_(WDB *db, const WModel *m, const void *obj, const char *field) {
  if (!db) {
    webc_fail(500, "db", "no database configured (set DATABASE_URL)");
    return false;
  }
  const WField *f = nullptr, *pk = nullptr;
  for (i32 i = 0; m && i < m->n; i++) {
    if (strcmp(m->fields[i].name, field) == 0) f = &m->fields[i];
    if (m->fields[i].q.pk) pk = &m->fields[i];
  }
  if (!f || !pk) {
    wc_panic(__FILE__, __LINE__,
             "db_increment: the model needs that field and a primary key");
    return false;
  }
  Arena *a = wc_arena_get();
  Buf sql;
  buf_init(&sql, a, 128);
  str tbl = db_table(db->drv, m);
  str fc = db_ident(db->drv, f), pkc = db_ident(db->drv, pk);
  buf_printf(&sql, "update %.*s set %.*s = %.*s + 1 where %.*s = :pk", (int)tbl.n, tbl.p,
             (int)fc.n, fc.p, (int)fc.n, fc.p, (int)pkc.n, pkc.p);
  WQuery *qy = q(buf_cstr(&sql));
  q_i64(qy, "pk", wfield_get_i64(pk, obj));
  WRes r = db_exec(db, qy);
  bool ok = r.ok;
  if (!ok && r.e.len > 0) snprintf(db->err, sizeof db->err, "%.*s", WERR_FMT(r.e));
  wres_free(&r);
  return ok;
}


// ---------------------------------------------------------------- across a relation
//
// The question is "stories in a section whose name contains 'sea'", and there are two
// portable ways to ask it: a join, which changes the shape of the result, or a subquery,
// which does not. The ORM reads rows back into models, so it needs the shape — `select *
// from stories where section in (select slug from sections where …)` stays a stories query
// and still becomes VecStory.
void ww_in_ref_(WWhere *w, const WModel *m, const char *field, WWhere *sub, bool negate) {
  if (w->order) wc_panic(__FILE__, __LINE__, "typed filter: condition after order by");
  const RefRule *rule = wc_ref_rule(m ? m->name : "", field);
  if (!rule) {
    wc_panic(__FILE__, __LINE__,
             "ww_in_ref: %s.%s is not a declared relation. Add WEBC_MODEL_REF(%s, %s, Target, "
             "key).",
             m ? m->name : "?", field, m ? m->name : "Model", field);
    return;
  }
  const WModel *target = model_by_name(rule->target);
  const WField *tk = model_field_named(target, rule->key);
  const WField *own = model_field_named(m, field);
  if (!target || !tk || !own) {
    wc_panic(__FILE__, __LINE__, "ww_in_ref: %s.%s -> %s.%s does not resolve.", m->name, field,
             rule->target, rule->key);
    return;
  }
  // The sub-filter's conditions only: an order inside `in (select …)` is at best pointless,
  // and MySQL rejects a LIMIT in a subquery used with IN.
  str inner = sub ? ww_conditions(sub, w->a) : (str){"", 0};
  // Its placeholders are renamed: both queries share one parameter list, and `:p0` would
  // otherwise mean two different values. The rename is done by looking each placeholder *up*
  // in the sub-filter's own parameter list rather than by rewriting `:p<digits>` — a filter
  // may bind any name it likes, and a rewrite that only knew `:p0` leaves `:min` pointing at
  // a value that is not there (the query runs, binds nothing, and answers zero rows).
  Buf q;
  buf_init(&q, w->a, inner.n + 24);
  char tag[24];
  snprintf(tag, sizeof tag, "s%d_", (int)w->n);
  WQuery *src = sub ? ww_query(sub) : nullptr;
  for (size_t i = 0; i < inner.n;) {
    if (inner.p[i] != ':') {
      buf_putc(&q, inner.p[i]);
      i++;
      continue;
    }
    size_t j = i + 1;
    while (j < inner.n && (isalnum((unsigned char)inner.p[j]) || inner.p[j] == '_')) j++;
    str name = (str){inner.p + i + 1, (size_t)(j - i - 1)};
    bool known = false;
    for (i32 k = 0; src && k < src->nparams; k++)
      if (str_eq(name, src->names[k])) known = true;
    if (known && name.n) {
      buf_printf(&q, ":%s%.*s", tag, (int)name.n, name.p);
    } else {
      // `::type` in postgres, or a colon in something this builder did not write: copied
      // through untouched rather than guessed at
      buf_puts(&q, str_cstr(w->a, (str){inner.p + i, (size_t)(j - i)}));
    }
    i = j;
  }
  const char *sep = w->no_sep ? "" : (w->n ? (w->in_or ? " or " : " and ") : "");
  const char *tt = target->table ? target->table : target->name;
  const WDBDriver *drv = ww_dialect();
  str ownc = db_ident_c(drv, w->a, model_column_name(own));
  str tkc = db_ident_c(drv, w->a, model_column_name(tk));
  str ttc = db_ident_c(drv, w->a, tt);
  buf_printf(&w->sql, "%s%.*s %s (select %.*s from %.*s%s%s)", sep, (int)ownc.n, ownc.p,
             negate ? "not in" : "in", (int)tkc.n, tkc.p, (int)ttc.n, ttc.p,
             q.n ? " where " : "", q.n ? buf_cstr(&q) : "");
  // The sub-filter's values move into the outer query under their new names. `ww_free` the
  // inner filter *after* the query that used it: `w`'s arena holds these names.
  for (i32 k = 0; src && k < src->nparams; k++) {
    size_t cap = strlen(tag) + src->names[k].n + 1;
    char *nm = arena_alloc(w->a, cap);
    snprintf(nm, cap, "%s%.*s", tag, (int)src->names[k].n, src->names[k].p);
    q_push(&w->qy, nm, src->params[k]);
  }
  w->n++;
  w->no_sep = false;
}

void ww_order_ref_(WWhere *w, const WModel *m, const char *field, const WModel *target,
                   const char *target_key, bool desc) {
  const WField *own = model_field_named(m, field);
  const WField *tk = model_field_named(target, target_key);
  // The join condition is the *relation's* key (`Section.slug`), which the declaration names —
  // `target_key` is only what to sort by. Comparing the sort column to the foreign key ordered
  // by nothing at all: every subquery came back NULL, so the list kept the table's own order
  // and looked sorted.
  const RefRule *rule = wc_ref_rule(m ? m->name : "", field);
  const WField *rk = rule ? model_field_named(target, rule->key) : nullptr;
  if (!own || !tk) {
    wc_panic(__FILE__, __LINE__, "ww_order_ref: %s.%s or %s.%s does not exist.", m->name, field,
             target->name, target_key);
    return;
  }
  const WDBDriver *drv = ww_dialect();
  str tkc = db_ident_c(drv, w->a, model_column_name(tk));
  str ttc = db_ident_c(drv, w->a, target->table ? target->table : target->name);
  str ot = db_ident_c(drv, w->a, m->table ? m->table : m->name);
  str ownc = db_ident_c(drv, w->a, model_column_name(own));
  if (!rule || !rk) {
    wc_panic(__FILE__, __LINE__,
             "ww_order_ref: %s.%s is not a declared relation. Add WEBC_MODEL_REF(%s, %s, %s, "
             "key) so the subquery knows which column it joins on.",
             m->name, field, m->name, field, target->name);
    return;
  }
  str rkc = db_ident_c(drv, w->a, model_column_name(rk));
  buf_printf(&w->sql, " order by (select %.*s from %.*s where %.*s = %.*s.%.*s)%s",
             (int)tkc.n, tkc.p, (int)ttc.n, ttc.p, (int)rkc.n, rkc.p, (int)ot.n, ot.p,
             (int)ownc.n, ownc.p, desc ? " desc" : "");
  w->order = true;
}

// ---------------------------------------------------------------- aggregates
//
// One statement, one number. `count(*)` was the only question the builder could answer with
// a value, which is why an app that needed a total loaded the rows and added them up.
WVal db_agg_(WDB *db, const WModel *m, const char *field, const char *fn, WWhere *w) {
  if (!db) webc_fail(500, "db", "no database configured (set DATABASE_URL)");
  const WField *f = model_field_named(m, field);
  if (!f) {
    wc_panic(__FILE__, __LINE__, "db_%s: %s has no field '%s'.", fn, m->name, field);
    return (WVal){WV_NULL};
  }
  if (!ww_kind_is_number(m, field))
    wc_panic(__FILE__, __LINE__, "db_%s: %s.%s is not numeric, so there is nothing to add up.",
             fn, m->name, field);
  Arena *a = wc_arena_get();
  str cond = ww_conditions(w, a);
  Buf sql;
  buf_init(&sql, a, 160);
  str tbl = db_table(db->drv, m);
  str col = db_ident(db->drv, f);
  buf_printf(&sql, "select %s(%.*s) as v from %.*s", fn, (int)col.n, col.p, (int)tbl.n, tbl.p);
  if (cond.n) buf_printf(&sql, " where %.*s", (int)cond.n, cond.p);
  WQuery *src = w ? ww_query(w) : nullptr;
  WQuery *qy = q(buf_cstr(&sql));
  for (i32 i = 0; src && i < src->nparams; i++)
    q_push(qy, str_cstr(a, src->names[i]), src->params[i]);
  WRes r = db_exec(db, qy);
  if (!r.ok) {
    wres_free(&r);
    db_fail(m->name);
  }
  WVal v = r.nrows ? row_at(&r.rows[0], 0) : (WVal){WV_NULL};
  wres_free(&r);
  return v;
}

// Every aggregate below answers with the same two shapes: the driver hands back a number,
// or a decimal rendered as text (postgres a numeric, MySQL a decimal). One coercion, four
// facades.
static i64 agg_i64(WDB *db, const WModel *m, const char *field, const char *fn, WWhere *w) {
  WVal v = db_agg_(db, m, field, fn, w);
  if (v.kind == WV_I64) return v.i;
  if (v.kind == WV_F64) return (i64)v.f;
  if (v.kind == WV_TEXT) {
    i64 o = 0;
    str_to_i64(v.s, &o);
    return o;
  }
  return 0;
}

i64 db_sum_(WDB *db, const WModel *m, const char *field, WWhere *w) {
  return agg_i64(db, m, field, "sum", w);
}

i64 db_min_(WDB *db, const WModel *m, const char *field, WWhere *w) {
  return agg_i64(db, m, field, "min", w);
}

i64 db_max_(WDB *db, const WModel *m, const char *field, WWhere *w) {
  return agg_i64(db, m, field, "max", w);
}

// f64 even for an integer column, because that is what every dialect returns: sqlite a real,
// postgres a numeric, MySQL a decimal. This is the one place the three servers disagree about
// the *type* of the same value, so the value is what a test asserts.
f64 db_avg_(WDB *db, const WModel *m, const char *field, WWhere *w) {
  WVal v = db_agg_(db, m, field, "avg", w);
  if (v.kind == WV_F64) return v.f;
  if (v.kind == WV_I64) return (f64)v.i;
  if (v.kind == WV_TEXT) {
    f64 o = 0;
    str_to_f64(v.s, &o);
    return o;
  }
  return 0;
}

// ---------------------------------------------------------------- one page of a list
void *db_page_(WDB *db, const WModel *m, const WVecDesc *vec, WWhere *w, WPager *pg) {
  if (!db) webc_fail(500, "db", "no database configured (set DATABASE_URL)");
  Arena *a = wc_arena_get();
  i64 limit = pg && pg->limit > 0 ? pg->limit : 20;
  if (limit > 1000) limit = 1000;
  i64 total = db_count_w_(db, m, w); // conditions only: db_count_w_ already drops order/limit
  i64 pages = (total + limit - 1) / limit;
  if (pages < 1) pages = 1;
  i64 page = pg && pg->page > 0 ? pg->page : 1;
  if (page > pages) page = pages; // an out-of-range page is the same list, not an empty one
  if (pg) {
    pg->page = page;
    pg->limit = limit;
    pg->total = total;
    pg->pages = pages;
    pg->has_prev = page > 1;
    pg->has_next = page < pages;
  }
  str cond = ww_conditions(w, a);
  str order = ww_order_only(w, a);
  Buf clause;
  buf_init(&clause, a, 192);
  if (cond.n) buf_printf(&clause, "where %.*s", (int)cond.n, cond.p);
  if (order.n) buf_printf(&clause, "%.*s", (int)order.n, order.p);
  buf_printf(&clause, " limit %lld offset %lld", (long long)limit,
             (long long)((page - 1) * limit));
  return db_all_clause_vec(db, m, vec, buf_cstr(&clause), w ? ww_query(w) : nullptr);
}

// The key of one row, written into a child's FK column. A relation's key and its FK may be
// spelled with different types (`slug` is a str, `id` an i64, and an app may join on the
// other), so the conversion happens here once instead of in every caller.
static void bind_key(const WField *fk, const WField *pk, const void *parent, void *child,
                     Arena *a) {
  bool fk_str = fk->kind == WK_STR || fk->kind == WK_TEXT;
  bool pk_str = pk->kind == WK_STR || pk->kind == WK_TEXT;
  if (fk_str) {
    if (pk_str)
      wfield_set_str(a, fk, child, wfield_get_str(pk, parent));
    else
      wfield_set_str(a, fk, child, str_fmt(a, "%lld", (long long)wfield_get_i64(pk, parent)));
  } else {
    if (pk_str) {
      i64 v = 0;
      str_to_i64(wfield_get_str(pk, parent), &v);
      wfield_set_i64(fk, child, v);
    } else {
      wfield_set_i64(fk, child, wfield_get_i64(pk, parent));
    }
  }
}

// ---------------------------------------------------------------- writing a tree
//
// A parent and its children are one write, or they are a broken order. `db_save_all` fills
// the FK from the parent's key (so the caller never repeats it), joins an open transaction if
// there is one and opens its own otherwise, and returns how many children it wrote.
// Which collection is this? The vector carries its descriptor, and the declaration names
// that same descriptor — so the caller does not have to repeat a field name that the
// declaration already contains. One name for one relation, and a typo is impossible.
// Two collections are the same collection when they are the same *type*, not when they are the
// same object. `WC_VECDESC_X` is a `static const` in every translation unit that includes the
// header, so an app that declares its models and its relations in one shared header has one
// descriptor per TU. Comparing the pointers
// asked "is this the copy I registered", which answers no for every caller but one, and the
// answer a caller got was "that collection is not declared on Spin".
static bool vec_desc_is_same(const WVecDesc *a, const WVecDesc *b) {
  if (a == b) return true;
  if (!a || !b || !a->name || !b->name) return false;
  if (strcmp(a->name, b->name) != 0) return false;
  if (a->size != b->size || a->elem_size != b->elem_size || a->elem_kind != b->elem_kind)
    return false;
  if (a->off_at != b->off_at || a->off_n != b->off_n || a->off_cap != b->off_cap ||
      a->off_arena != b->off_arena)
    return false;
  // `sub` is a model descriptor, which is per-TU too: compare what it names.
  if (a->sub == b->sub) return true;
  return a->sub && b->sub && a->sub->name && b->sub->name &&
         strcmp(a->sub->name, b->sub->name) == 0;
}

static const ManyRule *many_rule_for(const WModel *parent, const void *vec) {
  const WVecDesc *vd = vec ? *(const WVecDesc *const *)vec : nullptr;
  if (!vd) return nullptr;
  for (i32 i = 0; i < wc_many_rule_count(); i++) {
    const char *owner = nullptr, *field = nullptr, *target = nullptr, *back = nullptr;
    wc_many_rule_at(i, &owner, &field, &target, &back);
    (void)target;
    (void)back;
    if (strcmp(owner, parent->name) != 0) continue;
    const ManyRule *rule = wc_many_rule(owner, field);
    if (rule && vec_desc_is_same(rule->vec, vd)) return rule;
  }
  return nullptr;
}

i64 db_save_all_(WDB *db, const WModel *parent, void *obj, void *vec, i64 *n_out) {
  if (n_out) *n_out = 0;
  if (!db) webc_fail(500, "db", "no database configured (set DATABASE_URL)");
  const ManyRule *rule = many_rule_for(parent, vec);
  if (!rule) {
    wc_panic(__FILE__, __LINE__,
             "db_save_all: that collection is not declared on %s. Add WEBC_MODEL_MANY(%s, "
             "field, Target, back_field) — the declaration is what says which column holds "
             "the parent's key.",
             parent->name, parent->name);
    return -1;
  }
  const WModel *target = model_by_name(rule->target);
  const WField *fk = model_field_named(target, rule->back_field);
  const WField *pk = model_pk_field(parent);
  const WField *tpk = model_pk_field(target);
  if (!target || !fk || !pk) {
    wc_panic(__FILE__, __LINE__, "db_save_all: %s.%s -> %s.%s does not resolve.",
             parent->name, rule->field, rule->target, rule->back_field);
    return -1;
  }
  const WVecDesc *vd = *(const WVecDesc *const *)vec;
  if (!vd) return 0;
  i64 parent_id = wfield_get_i64(pk, obj);
  if (!parent_id) {
    webc_fail(500, "db",
              "db_save_all: the parent has no key yet — save it first, so its children can "
              "point at it");
    return -1;
  }
  bool own_tx = tx_depth() == 0;
  WTx t = {0};
  if (own_tx) {
    t = tx_begin(db);
    if (!t.began) webc_fail(500, "db", t.err[0] ? t.err : "db_save_all: cannot begin");
  }
  const char *at = *(const char *const *)((const char *)vec + vd->off_at);
  i32 n = *(const i32 *)((const char *)vec + vd->off_n);
  i64 written = 0;
  for (i32 i = 0; at && i < n; i++) {
    void *row = (void *)(at + (size_t)i * vd->elem_size);
    // The FK is the rule's back_field, so a typo cannot point at the wrong column: the
    // declaration is the only place the column is named.
    Arena *ka = wc_arena_get();
    if (!ka) ka = arena_new(4096); // no request arena (a test, a CLI command)
    bind_key(fk, pk, obj, row, ka);
    bool saved = tpk && wfield_get_i64(tpk, row)
                     ? db_exec_model(db, "update", target, row).ok
                     : db_insert_into(db, target, row, nullptr);
    if (!saved) {
      if (own_tx) (void)tx_rollback(&t);
      db_fail(target->name);
    }
    written++;
  }
  if (own_tx && !tx_commit(&t)) db_fail(parent->name);
  if (n_out) *n_out = written;
  return written;
}

i64 db_delete_tree_(WDB *db, const WModel *parent, const void *obj, void *vec) {
  if (!db) webc_fail(500, "db", "no database configured (set DATABASE_URL)");
  const ManyRule *rule = many_rule_for(parent, vec);
  if (!rule) {
    wc_panic(__FILE__, __LINE__,
             "db_delete_tree: that collection is not declared on %s. Add "
             "WEBC_MODEL_MANY(%s, field, Target, back_field).",
             parent->name, parent->name);
    return -1;
  }
  const WModel *target = model_by_name(rule->target);
  const WField *fk = model_field_named(target, rule->back_field);
  const WField *pk = model_pk_field(parent);
  if (!target || !fk || !pk) {
    wc_panic(__FILE__, __LINE__, "db_delete_tree: %s.%s -> %s.%s does not resolve.",
             parent->name, rule->field, rule->target, rule->back_field);
    return -1;
  }
  Arena *a = wc_arena_get();
  str tbl = db_table(db->drv, target);
  str fkc = db_ident(db->drv, fk);
  Buf sql;
  buf_init(&sql, a, 96);
  // children first: an app's migration may not have `on delete cascade`, and a delete that
  // fails on a foreign key is a 400 the app did not mean
  buf_printf(&sql, "delete from %.*s where %.*s = :v", (int)tbl.n, tbl.p, (int)fkc.n, fkc.p);
  WQuery *qy = q(buf_cstr(&sql));
  bool strkey = fk->kind == WK_STR || fk->kind == WK_TEXT;
  bool pk_str = pk->kind == WK_STR || pk->kind == WK_TEXT;
  if (strkey && pk_str)
    q_str(qy, "v", wfield_get_str(pk, obj));
  else if (strkey)
    q_str(qy, "v", str_fmt(a, "%lld", (long long)wfield_get_i64(pk, obj)));
  else if (pk_str) {
    i64 v = 0;
    str_to_i64(wfield_get_str(pk, obj), &v);
    q_i64(qy, "v", v);
  } else
    q_i64(qy, "v", wfield_get_i64(pk, obj));
  WRes r = db_exec(db, qy);
  if (!r.ok) {
    wres_free(&r);
    db_fail(target->name);
  }
  i64 removed = r.affected;
  wres_free(&r);
  if (!db_delete_(parent, wfield_get_i64(pk, obj))) db_fail(parent->name);
  return removed;
}
// ---------------------------------------------------------------- the app kit
//
// The four calls a handler makes, each of which either works or answers the
// request with a status that says what went wrong. Nothing here returns
// an error for the caller to translate: a database error that reaches an app is
// already a response, with the server's own words in it.

static const WField *model_pk(const WModel *m) {
  for (i32 i = 0; i < m->n; i++)
    if (m->fields[i].q.pk) return &m->fields[i];
  return nullptr;
}

// The two-driver reading of an error message, kept only as a fallback for a failure that
// never reached a driver's own classifier. New code asks the driver (WRes::cls); this is
// what makes an unclassified error behave exactly as it did before the classification
// existed, instead of turning a 409 into a 500.
static WDbErrKind db_class_from_text(const char *m) {
  if (!m) return WDBE_NONE;
  if (strstr(m, "23505") || strstr(m, "UNIQUE constraint") || strstr(m, "1062"))
    return WDBE_CONFLICT;
  if (strstr(m, "23503") || strstr(m, "FOREIGN KEY") || strstr(m, "1452") ||
      strstr(m, "1451"))
    return WDBE_BAD_REFERENCE;
  if (strstr(m, "23502") || strstr(m, "NOT NULL") || strstr(m, "1048")) return WDBE_NOT_NULL;
  if (strstr(m, "1406") || strstr(m, "too long")) return WDBE_TOO_LONG;
  if (strstr(m, "1213") || strstr(m, "deadlock")) return WDBE_DEADLOCK;
  if (strstr(m, "1205")) return WDBE_TIMEOUT;
  return WDBE_NONE;
}

// A driver error becomes the status a client can act on: a duplicate is the
// client's fault (409), a missing parent is a bad reference (400), a NULL in a
// required column is a bad body (422), a deadlock is "try again" (503), everything
// else is ours (500).
void db_fail(const char *what) {
  WDB *h = db_default();
  str msg = h ? db_error(h) : str_lit("no database configured");
  const char *m = str_cstr(wc_arena_get(), msg);
  i32 status = 500;
  const char *code = "db";
  // The driver already said *what kind* of failure this is — its own errno, extended
  // result code or SQLSTATE, classified where the dialect lives. Sniffing the message for
  // "23505" was the two-driver version of the same idea, and it stops holding the moment
  // a third server words it differently.
  WDbErrKind cls = h && h->err_cls ? h->err_cls : db_class_from_text(m);
  switch (cls) {
  case WDBE_CONFLICT:
    status = 409;
    code = "conflict";
    break;
  case WDBE_BAD_REFERENCE:
    status = 400;
    code = "bad_reference";
    break;
  case WDBE_NOT_NULL:
  case WDBE_TOO_LONG:
  case WDBE_CHECK:
    status = 422;
    code = "invalid";
    break;
  case WDBE_DEADLOCK:
  case WDBE_TIMEOUT:
    status = 503;
    code = "busy";
    break;
  default:
    break;
  }
  LogLevel lvl = status >= 500 ? LOG_ERROR : LOG_WARN;
  log_write(lvl, what ? what : "query", (LogField[]){ { "error", { m, strlen(m) } } }, 1);
  webc_fail(status, code, m);
}

bool db_save_(const WModel *m, void *obj) {
  WDB *h = db_default();
  if (!h) webc_fail(500, "db", "no database configured (set DATABASE_URL)");
  const WField *pk = model_pk(m);
  i64 id = pk ? wfield_get_i64(pk, obj) : 0;
  if (id) {
    if (!db_exec_model(h, "update", m, obj).ok) db_fail(m->name);
    return true;
  }
  i64 new_id = 0;
  if (!db_insert_into(h, m, obj, &new_id)) db_fail(m->name);
  // The caller keeps the key: `db_save(Todo, &t)` then `json(201, Todo, &t)` is a
  // complete create handler, and the id is not lost on the way back.
  if (pk && new_id) wfield_set_i64(pk, obj, new_id);
  return true;
}

bool db_try_save_(const WModel *m, void *obj) {
  WDB *h = db_default();
  if (!h) webc_fail(500, "db", "no database configured (set DATABASE_URL)");
  const WField *pk = model_pk(m);
  i64 id = pk ? wfield_get_i64(pk, obj) : 0;
  bool ok = id ? db_exec_model(h, "update", m, obj).ok : db_insert_into(h, m, obj, &id);
  if (ok) {
    if (pk && id && !wfield_get_i64(pk, obj)) wfield_set_i64(pk, obj, id);
    return true;
  }
  str msg = db_error(h);
  const char *text = str_cstr(wc_arena_get(), msg);
  if (h->err_cls == WDBE_CONFLICT || db_class_from_text(text) == WDBE_CONFLICT) {
    log_write(LOG_WARN, m->name, (LogField[]){{"error", msg}}, 1);
    return false;
  }
  db_fail(m->name); // not a conflict: a bug or an outage, and it panics
  return false;
}

// One row that matches a condition you write, or null. The whole ceremony of a
// login query — select list, row copy, arena, wres_free — is the framework's.
void *db_find_where(const WModel *m, const char *where, WQuery *qy) {
  WDB *h = db_default();
  if (!h) webc_fail(500, "db", "no database configured (set DATABASE_URL)");
  Arena *a = wc_arena_get();
  WRes r = db_select_where(h, m, where, qy);
  if (!r.ok) {
    wres_free(&r);
    db_fail(m->name);
  }
  if (r.nrows == 0) {
    wres_free(&r);
    return nullptr;
  }
  void *out = arena_zalloc(a, m->size ? m->size : 1);
  model_from_row_dup(m, &r.rows[0], out, a);
  wres_free(&r);
  return out;
}

void *db_fetch(const WModel *m, i64 id) {
  WDB *h = db_default();
  if (!h) webc_fail(500, "db", "no database configured (set DATABASE_URL)");
  const WField *pk = model_pk(m);
  if (!pk) webc_fail(500, "db", "model has no primary key");
  Arena *a = wc_arena_get();
  Buf w;
  buf_init(&w, a, 64);
  str pkc = db_ident(h->drv, pk);
  buf_printf(&w, "%.*s = :pk", (int)pkc.n, pkc.p);
  WQuery *qy = q_a(a, "");
  qy->owns_arena = false;
  q_i64(qy, "pk", id);
  WRes r = db_select_where(h, m, buf_cstr(&w), qy);
  if (!r.ok) {
    wres_free(&r);
    db_fail(m->name);
  }
  if (r.nrows == 0) {
    wres_free(&r);
    return nullptr;
  }
  void *out = arena_zalloc(a, m->size ? m->size : 1);
  model_from_row_dup(m, &r.rows[0], out, a);
  wres_free(&r);
  return out;
}

bool db_delete_(const WModel *m, i64 id) {
  WDB *h = db_default();
  if (!h) webc_fail(500, "db", "no database configured (set DATABASE_URL)");
  str tbl = db_table(h->drv, m);
  str sql = str_fmt(nullptr, "delete from %.*s where id = %lld", (int)tbl.n, tbl.p,
                    (long long)id);
  WRes r = db_exec_sql(h, str_cstr(wc_arena_get(), sql));
  bool ok = r.ok;
  wres_free(&r);
  if (!ok) db_fail(m->name);
  return true;
}

i64 db_count_(const WModel *m) {
  WDB *h = db_default();
  if (!h) webc_fail(500, "db", "no database configured (set DATABASE_URL)");
  str tbl = db_table(h->drv, m);
  str sql = str_fmt(nullptr, "select count(*) as n from %.*s", (int)tbl.n, tbl.p);
  WRes r = db_exec_sql(h, str_cstr(wc_arena_get(), sql));
  i64 n = 0;
  if (r.ok && r.nrows) {
    WVal v = row_get(&r.rows[0], "n");
    n = v.kind == WV_I64 ? v.i : 0;
  }
  bool ok = r.ok;
  wres_free(&r);
  if (!ok) db_fail(m->name);
  return n;
}

void wres_free(WRes *r) {
  if (!r) return;
  if (r->own) arena_destroy(r->own);
  r->own = nullptr;
  r->rows = nullptr;
  r->nrows = 0;
}

bool model_from_row_dup(const WModel *m, WRow *r, void *out, Arena *a) {
  model_from_row(m, r, out);
  for (i32 i = 0; i < m->n; i++) {
    const WField *f = &m->fields[i];
    char *dst = (char *)out + f->off;
    if (f->kind == WK_STR) {
      str *s = (str *)dst;
      *s = (*s).n ? str_dup(a, *s) : (str){"", 0};
    } else if (f->kind == WK_TEXT) {
      // The row owns a (pointer, length) pair in its own arena; a Text field is a
      // NUL-terminated pointer in the caller's. This is the only place the two
      // meet, so it is the only place that converts.
      WVal v = row_get(r, model_column_name(f));
      *(const char **)dst = (v.kind == WV_TEXT && v.s.n) ? str_cdup(a, v.s) : "";
    } else if (f->kind == WK_BYTES) {
      Bytes *bb = (Bytes *)dst;
      if (bb->n) {
        Bytes copy = {.p = arena_alloc(a, bb->n), .n = bb->n};
        memcpy(copy.p, bb->p, bb->n);
        *bb = copy;
      }
    }
  }
  return true;
}

bool model_from_row(const WModel *m, WRow *r, void *out) {
  for (i32 i = 0; i < m->n; i++) {
    const WField *f = &m->fields[i];
    WVal v = row_get(r, model_column_name(f));
    char *dst = (char *)out + f->off;
    switch (f->kind) {
    case WK_I64:
      *(i64 *)dst = v.kind == WV_I64 ? v.i : 0;
      break;
    case WK_I32:
      *(i32 *)dst = (i32)(v.kind == WV_I64 ? v.i : 0);
      break;
    case WK_U64:
      *(u64 *)dst = (u64)(v.kind == WV_I64 ? v.i : 0);
      break;
    case WK_F64:
      *(f64 *)dst = v.kind == WV_F64 ? v.f : (f64)v.i;
      break;
    case WK_F32:
      *(f32 *)dst = (f32)(v.kind == WV_F64 ? v.f : (f64)v.i);
      break;
    case WK_BOOL:
      *(bool *)dst = v.kind == WV_BOOL ? v.b : v.i != 0;
      break;
    case WK_STR:
      *(str *)dst = v.kind == WV_TEXT ? v.s : (str){"", 0};
      break;
    case WK_ENUM:
      // by name; an integer column reads as its index
      if (v.kind == WV_TEXT)
        *(int *)dst = f->en ? wc_enum_index(f->en, v.s) : 0;
      else
        *(int *)dst = v.kind == WV_I64 ? (i32)v.i : 0;
      break;
    case WK_TEXT:
      // Left for model_from_row_dup(), which has both the row and the arena: a
      // `str` written into a `const char *` slot would be a 16-byte write into an
      // 8-byte field, which is exactly the kind of bug this comment is cheaper
      // than.
      break;
    case WK_BYTES:
      *(Bytes *)dst = (Bytes){.p = (char *)v.s.p, .n = v.s.n};
      break;
    default:
      break;
    }
  }
  return true;
}

// ---------------------------------------------------------------- migrations

// `0001_todos.postgres.sql` is a per-driver variant of `0001_todos.sql`, not a migration
// of its own — and which suffixes count as "a dialect" is the driver table's business, not
// a pair of hardcoded `strstr`s (the pair is why a `.mysql.sql` twin would have been
// applied twice).
static bool db_is_dialect_twin(const char *name) {
  size_t n = strlen(name);
  for (i32 i = 0; i < WC_DB_DRIVER_N; i++) {
    char suffix[64];
    int k = snprintf(suffix, sizeof suffix, ".%s.sql", WC_DB_DRIVERS[i]);
    if (k > 0 && (size_t)k < n && strcmp(name + n - (size_t)k, suffix) == 0) return true;
  }
  return false;
}

static i32 cmp_names(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

bool migrations_apply(WMigrate *m) {
  WDB *db = m->db;
  if (!db) return false;
  // `varchar(255)`, not `text`: MySQL cannot index a `text` column without a prefix length,
  // so the bookkeeping table itself failed to create there — and the runner returned false
  // without a word, which read as "no migrations needed" and left an empty database behind.
  WRes r = db_exec_sql(db, "create table if not exists schema_migrations ("
                           "name varchar(255) primary key, applied_at varchar(40) not null)");
  if (!r.ok) {
    log_error("migrations: cannot create schema_migrations this driver can index",
              {"error", r.e.len ? werr_msg(&r.e) : str_lit("?")});
    wres_free(&r);
    return false;
  }
  wres_free(&r);

  const char *dir = m->dir ? m->dir : "migrations";
  DIR *d = opendir(dir);
  if (!d) {
    log_warn("no migrations directory", {"dir", str_lit(dir)});
    return true;
  }
  char *names[512];
  i32 n = 0;
  struct dirent *e;
  while ((e = readdir(d)) && n < 512) {
    size_t len = strlen(e->d_name);
    if (len < 5 || strcmp(e->d_name + len - 4, ".sql") != 0) continue;
    if (db_is_dialect_twin(e->d_name)) continue;
    names[n++] = strdup(e->d_name);
  }
  closedir(d);
  qsort(names, (size_t)n, sizeof(char *), cmp_names);

  WRes done = db_exec_sql(db, "select name from schema_migrations");
  i32 applied = 0;
  for (i32 i = 0; i < n; i++) {
    bool already = false;
    for (i32 k = 0; k < done.nrows; k++) {
      str nm = row_str_at(&done.rows[k], 0);
      if (str_eq_c(nm, names[i])) {
        already = true;
        break;
      }
    }
    if (already) continue;
    // A migration is plain SQL, so it carries the dialect of whichever database
    // it was written for: `integer primary key autoincrement` is sqlite, and
    // postgres needs `bigserial`/`generated always as identity`. When a file for
    // this driver exists (`0001_todos.postgres.sql`) it is used instead of
    // `0001_todos.sql`; the same name is recorded either way, so the sequence stays
    // one sequence. Without this, an app that supports both drivers keeps two migration
    // directories and hopes they stay in step.
    char path[1024], alt[1100];
    snprintf(path, sizeof path, "%s/%s", dir, names[i]);
    size_t nlen = strlen(names[i]);
    snprintf(alt, sizeof alt, "%s/%.*s.%s.sql", dir, (int)(nlen - 4), names[i], db->drv->name);
    FILE *f = fopen(alt, "rb");
    if (f) {
      snprintf(path, sizeof path, "%s", alt);
    } else {
      f = fopen(path, "rb");
    }
    if (!f) continue;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *sql = malloc((size_t)sz + 1);
    if (fread(sql, 1, (size_t)sz, f) != (size_t)sz) {
      free(sql);
      fclose(f);
      continue;
    }
    sql[sz] = 0;
    fclose(f);
    if (m->dry_run) {
      printf("-- would apply %s\n%s\n", names[i], sql);
      free(sql);
      continue;
    }
    if (!m->quiet) printf("applying %s\n", names[i]);
    db_begin(db);
    bool ok = db_exec_sql(db, sql).ok;
    if (ok) {
      WQuery *ins = q("insert into schema_migrations (name, applied_at) values (:n, :t)");
      q_cstr(ins, "n", names[i]);
      char ts[40];
      wc_time_rfc3339_buf(ts, sizeof ts, wc_wall_ms());
      q_cstr(ins, "t", ts);
      ok = db_exec(db, ins).ok;
    }
    if (ok)
      db_commit(db);
    else {
      db_rollback(db);
      fprintf(stderr, "migration %s failed: %.*s\n", names[i], (int)db_error(db).n, db_error(db).p);
    }
    free(sql);
    applied++;
  }
  for (i32 i = 0; i < n; i++)
    free(names[i]);
  if (!m->quiet && !m->dry_run) printf("%d migration(s) applied\n", applied);
  return true;
}

// One migration's SQL, run in its own transaction with its record. Shared by the
// embedded and the on-disk forms so both leave the same trace.
static bool migration_run_sql(WDB *db, const char *name, const char *sql, bool quiet) {
  if (!quiet) printf("applying %s\n", name);
  db_begin(db);
  bool ok = db_exec_sql(db, sql).ok;
  if (ok) {
    WQuery *ins = q("insert into schema_migrations (name, applied_at) values (:n, :t)");
    q_cstr(ins, "n", name);
    char ts[40];
    wc_time_rfc3339_buf(ts, sizeof ts, wc_wall_ms());
    q_cstr(ins, "t", ts);
    ok = db_exec(db, ins).ok;
  }
  if (ok) {
    db_commit(db);
  } else {
    db_rollback(db);
    fprintf(stderr, "migration %s failed: %.*s\n", name, (int)db_error(db).n, db_error(db).p);
  }
  return ok;
}

const char *migration_for_driver(const WMig *migs, i32 n, const char *name, const char *driver) {
  char want[256];
  size_t base = strlen(name);
  if (base > 4 && strcmp(name + base - 4, ".sql") == 0) base -= 4;
  snprintf(want, sizeof want, "%.*s.%s.sql", (int)base, name, driver);
  for (i32 i = 0; i < n; i++)
    if (strcmp(migs[i].name, want) == 0) return migs[i].sql;
  return nullptr;
}

bool migrations_apply_embedded(WDB *db, const WMig *migs, i32 n) {
  if (!db) return false;
  if (!db_exec_sql(db, "create table if not exists schema_migrations ("
                       "name text primary key, applied_at text not null)").ok)
    return false;
  WRes done = db_exec_sql(db, "select name from schema_migrations");
  i32 applied = 0;
  bool all_ok = true;
  for (i32 i = 0; i < n; i++) {
    const char *name = migs[i].name;
    // a driver twin is applied *as* its base name, so the sequence stays one
    if (db_is_dialect_twin(name)) continue;
    bool already = false;
    for (i32 k = 0; k < done.nrows && !already; k++) {
      str nm = row_str_at(&done.rows[k], 0);
      already = str_eq_c(nm, name);
    }
    if (already) continue;
    const char *sql = migration_for_driver(migs, n, name, db->drv->name);
    if (!sql) sql = migs[i].sql;
    if (!migration_run_sql(db, name, sql, false)) {
      all_ok = false;
      break;
    }
    applied++;
  }
  printf("%d migration(s) applied\n", applied);
  return all_ok;
}

bool migrations_status(WMigrate *m) {
  WDB *db = m->db;
  WRes r = db_exec_sql(db, "select name, applied_at from schema_migrations order by name");
  if (!r.ok) return false;
  for (i32 i = 0; i < r.nrows; i++)
    printf("%.*s  %.*s\n", (int)r.rows[i].vals[0].s.n, r.rows[i].vals[0].s.p,
           (int)r.rows[i].vals[1].s.n, r.rows[i].vals[1].s.p);
  return true;
}