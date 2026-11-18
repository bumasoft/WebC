// SPDX-License-Identifier: Apache-2.0
// WebC — the query cache (SPEC §9d): WRes results, kept in Redis, invalidated by table generation.
//
// The whole feature lives at `WRes db_exec()` (src/db.c) because that is the one door every
// statement in the framework goes through. This file holds the parts that door calls: the
// statement classifier, the key, the envelope, the generation counters, the breaker and the
// counters an operator reads.
//
// Two rules, and they are the feature:
//   * a Redis problem never fails a request — every failure here answers "miss" and the caller
//     goes to the database, because a cache outage must not become an app outage;
//   * a stale row is a bug — nothing is cached that cannot be proved cacheable, reads inside a
//     transaction bypass the cache, and an entry is stored only under the generation it was read
//     with.
#include <webc/qcache.h>

#include "internal.h"

#include <webc/json.h>
#include <webc/log.h>
#include <webc/task.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static i64 now_ms(void) { return (i64)wc_time_ms(); }

// ---------------------------------------------------------------- configuration

#define QC_STATS_MUT WMutex
// `qcache_on()` is asked on every statement, so after the first call it is one relaxed load and no
// lock: a mutex on the path of every query would cost more than the cache saves.
static atomic_int g_sw = -1; // -1 not read yet, 0 off, 1 on

static struct {
  bool init, enabled;
  i64 ttl_ms, jitter_pct, max_bytes, op_ms, gen_ms;
  bool breaker_on;
  i32 breaker_after;
  i64 breaker_base_ms, breaker_cap_ms;
  char ns[96];
  i64 model_ttl[16];
  const WModel *model_key[16];
  i32 model_n;
  // counters
  WCacheStats st;
  // breaker
  i32 consec;
  i32 opens;
  i64 until_ms;
  WMutex mu;
} g_qc = {.mu = WEBC_MUTEX_INIT};

static bool trace_on(void) {
  static int on = -1;
  if (on < 0) on = getenv("WEBC_TRACE_CACHE") ? 1 : 0;
  return on == 1;
}

static void trace(const char *fmt, ...) {
  if (!trace_on()) return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  fprintf(stderr, "[cache] %s\n", buf);
}

static void counters_sync(void) {
  metrics_counter_add("webc_cache_hits_total", 0, "query cache hits");
  metrics_counter_add("webc_cache_misses_total", 0, "query cache misses");
}

static void qc_init(void) {
  wmutex_lock(&g_qc.mu);
  if (g_qc.init) {
    wmutex_unlock(&g_qc.mu);
    return;
  }
  g_qc.init = true;
  g_qc.ttl_ms = getenv("WEBC_CACHE_TTL_MS") ? atoll(getenv("WEBC_CACHE_TTL_MS")) : 30000;
  g_qc.jitter_pct = getenv("WEBC_CACHE_TTL_JITTER") ? atoll(getenv("WEBC_CACHE_TTL_JITTER")) : 10;
  g_qc.max_bytes = getenv("WEBC_CACHE_MAX_BYTES") ? atoll(getenv("WEBC_CACHE_MAX_BYTES")) : 262144;
  g_qc.op_ms = getenv("WEBC_CACHE_MS") ? atoll(getenv("WEBC_CACHE_MS")) : 5;
  g_qc.gen_ms = getenv("WEBC_CACHE_GEN_MS") ? atoll(getenv("WEBC_CACHE_GEN_MS")) : 0;
  g_qc.breaker_on = !(getenv("WEBC_CACHE_BREAKER") && !strcmp(getenv("WEBC_CACHE_BREAKER"), "0"));
  g_qc.breaker_after = 3;
  g_qc.breaker_base_ms = 5000;
  g_qc.breaker_cap_ms = 60000;
  const char *on = getenv("WEBC_QUERY_CACHE");
  g_qc.enabled = on && *on && strcmp(on, "0") != 0;
  const char *name = getenv("WEBC_REDIS_NAME");
  if (!name || !*name) name = "webc";
  snprintf(g_qc.ns, sizeof g_qc.ns, "%s", name);
  wmutex_unlock(&g_qc.mu);
  atomic_store_explicit(&g_sw, g_qc.enabled ? 1 : 0, memory_order_relaxed);
  counters_sync();
}

void qcache_config(i64 ttl_ms, i64 max_bytes) {
  qc_init();
  if (ttl_ms > 0) g_qc.ttl_ms = ttl_ms;
  if (max_bytes > 0) g_qc.max_bytes = max_bytes;
}

bool qcache_on(void) {
  int sw = atomic_load_explicit(&g_sw, memory_order_relaxed);
  if (sw < 0) {
    qc_init();
    sw = atomic_load_explicit(&g_sw, memory_order_relaxed);
  }
  return sw == 1 && redis_ready(redis());
}

void qcache_ttl_model(const WModel *m, i64 ms) {
  qc_init();
  wmutex_lock(&g_qc.mu);
  for (i32 i = 0; i < g_qc.model_n; i++) {
    if (g_qc.model_key[i] == m) {
      g_qc.model_ttl[i] = ms;
      wmutex_unlock(&g_qc.mu);
      return;
    }
  }
  if (g_qc.model_n < (i32)WC_COUNTOF(g_qc.model_key)) {
    g_qc.model_key[g_qc.model_n] = m;
    g_qc.model_ttl[g_qc.model_n] = ms;
    g_qc.model_n++;
  }
  wmutex_unlock(&g_qc.mu);
}

// A per-model TTL (`qcache_ttl(Todo, 5000)`), for the table whose rows go stale on their own
// schedule rather than the app's.
static i64 model_ttl_for(str table) {
  i64 ttl = g_qc.ttl_ms;
  wmutex_lock(&g_qc.mu);
  for (i32 i = 0; i < g_qc.model_n; i++) {
    const char *t = g_qc.model_key[i]->table ? g_qc.model_key[i]->table : g_qc.model_key[i]->name;
    size_t tn = t ? strlen(t) : 0;
    if (tn == table.n && tn && !memcmp(t, table.p, tn)) {
      ttl = g_qc.model_ttl[i];
      break;
    }
  }
  wmutex_unlock(&g_qc.mu);
  return ttl;
}

WCacheStats qcache_stats(void) {
  qc_init();
  return g_qc.st;
}

void qcache_reset_counters(void) {
  qc_init();
  memset(&g_qc.st, 0, sizeof g_qc.st);
  g_qc.consec = 0;
  g_qc.until_ms = 0;
}

// ---------------------------------------------------------------- the nocache scope
//
// The depth belongs to the *task*, not the thread: a handler that parks resumes on another worker,
// and a thread-local depth there would either leak "nocache" into an unrelated request or end it
// early. wtask_aux is the task-local slot the framework keeps for exactly this.

static i32 *nocache_slot(void) {
  i32 *d = wtask_aux_get();
  if (!d) {
    d = wm_alloc_zero(sizeof *d);
    wtask_aux_set(d);
  }
  return d;
}

int qcache_nocache_enter(void) { return ++*nocache_slot(); }
int qcache_nocache_leave(void) {
  i32 *d = nocache_slot();
  if (*d > 0) (*d)--;
  return *d;
}
static i32 nocache_depth(void) {
  i32 *d = wtask_aux_get();
  return d ? *d : 0;
}

// ---------------------------------------------------------------- the classifier
//
// One pass with the lexer discipline db_bind_named() already uses (src/db.c): a keyword inside a
// literal, a quoted identifier or a comment is not a keyword. Two answers come out — what kind of
// statement it is, and which tables it reads or writes.

typedef enum WStmtKind : u8 { WST_OTHER, WST_READ, WST_WRITE } WStmtKind;

typedef struct WStmt {
  WStmtKind kind;
  bool volatile_fn;
  bool locking;
  bool hint_cache, hint_nocache;
  i64 hint_ttl_ms;
  str tables[16];
  i32 ntables;
  bool unknown_tables;
} WStmt;

static bool kw_eq(str w, const char *kw) {
  size_t n = strlen(kw);
  if (w.n != n || !w.p) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)w.p[i]) != (unsigned char)kw[i]) return false;
  return true;
}

static const char *const QC_VOLATILE[] = {
    "now",          "current_timestamp", "current_date",     "current_time",
    "localtime",    "localtimestamp",    "clock_timestamp",  "statement_timestamp",
    "transaction_timestamp",             "random",           "rand",
    "randomblob",   "gen_random_uuid",   "uuid_generate_v4", "uuid",
    "nextval",      "currval",           "lastval",          "last_insert_rowid",
    "changes",      "total_changes",     "unixepoch",        "julianday",
    "strftime",     "current_setting",   "pg_backend_pid",   "version",
    "pg_sleep",     "txid_current",      "database",         "sqlite_version",
};

// Some volatile things are spelled without parentheses — `current_timestamp`, `localtime` — so a
// bare word has to count as well as a call.
static const char *const QC_VOLATILE_BARE[] = {
    "current_timestamp", "current_date", "current_time",  "localtime",
    "localtimestamp",    "current_user", "session_user",  "sysdate",
};

static bool is_volatile_bare(str w) {
  for (size_t i = 0; i < WC_COUNTOF(QC_VOLATILE_BARE); i++) {
    size_t n = strlen(QC_VOLATILE_BARE[i]);
    if (w.n != n || !w.p) continue;
    bool same = true;
    for (size_t k = 0; k < n; k++)
      if (tolower((unsigned char)w.p[k]) != (unsigned char)QC_VOLATILE_BARE[i][k]) {
        same = false;
        break;
      }
    if (same) return true;
  }
  return false;
}

static bool is_volatile_fn(str w) {
  for (size_t i = 0; i < WC_COUNTOF(QC_VOLATILE); i++) {
    size_t n = strlen(QC_VOLATILE[i]);
    if (w.n != n || !w.p) continue;
    bool same = true;
    for (size_t k = 0; k < n; k++)
      if (tolower((unsigned char)w.p[k]) != (unsigned char)QC_VOLATILE[i][k]) {
        same = false;
        break;
      }
    if (same) return true;
  }
  return false;
}

static void stmt_table(WStmt *o, str t) {
  if (!t.p || t.n == 0) return;
  for (i32 i = 0; i < o->ntables; i++)
    if (o->tables[i].n == t.n && !memcmp(o->tables[i].p, t.p, t.n)) return;
  if (o->ntables < (i32)WC_COUNTOF(o->tables)) o->tables[o->ntables++] = t;
  else o->unknown_tables = true;
}

static void stmt_scan(str sql, WStmt *o) {
  memset(o, 0, sizeof *o);
  o->kind = WST_OTHER;
  const char *p = sql.p;
  size_t n = sql.n, i = 0;
  str first = {0};
  bool pending = false;    // the next identifier is a table
  bool in_from = false;    // inside a from list, where a comma keeps naming tables
  bool write_kw = false;   // the statement mentions insert/update/delete at all

  while (i < n) {
    char c = p[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      i++;
      continue;
    }
    if (c == '\'') { // a literal: '' is an escaped quote
      i++;
      while (i < n) {
        if (p[i] == '\'') {
          if (i + 1 < n && p[i + 1] == '\'') {
            i += 2;
            continue;
          }
          i++;
          break;
        }
        i++;
      }
      pending = false;
      continue;
    }
    if (c == '"') { // a quoted identifier
      i++;
      size_t s = i;
      while (i < n && p[i] != '"') i++;
      str w = {.p = (char *)p + s, .n = i - s};
      if (i < n) i++;
      if (pending || in_from) {
        stmt_table(o, w);
        pending = false;
      }
      continue;
    }
    if (c == '-' && i + 1 < n && p[i + 1] == '-') {
      while (i < n && p[i] != '\n') i++;
      continue;
    }
    if (c == '/' && i + 1 < n && p[i + 1] == '*') {
      size_t s = i + 2;
      while (i + 1 < n && !(p[i] == '*' && p[i + 1] == '/')) i++;
      size_t e = i;
      i = i + 1 < n ? i + 2 : n;
      str body = {.p = (char *)p + s, .n = e > s ? e - s : 0};
      if (str_ci_contains(body, "no-cache")) {
        o->hint_nocache = true;
      } else if (str_ci_contains(body, "cache")) {
        o->hint_cache = true;
        for (size_t k = 0; k < body.n; k++) {
          if (isdigit((unsigned char)body.p[k])) {
            o->hint_ttl_ms = atoll(body.p + k);
            break;
          }
        }
      }
      continue;
    }
    if (!(isalpha((unsigned char)c) || c == '_')) {
      if (c == '(' || c == ')' || c == ';') {
        pending = false;
        if (c == ')') in_from = false;
      }
      if (c == ',') pending = in_from;
      i++;
      continue;
    }

    // a word, possibly qualified (`public.todos`, `"public"."todos"` handled above)
    size_t s = i;
    while (i < n && (isalnum((unsigned char)p[i]) || p[i] == '_' || p[i] == '$')) i++;
    str w = {.p = (char *)p + s, .n = i - s};
    while (i < n && p[i] == '.') {
      i++;
      if (i < n && p[i] == '"') {
        i++;
        while (i < n && p[i] != '"') i++;
        if (i < n) i++;
      } else {
        while (i < n && (isalnum((unsigned char)p[i]) || p[i] == '_')) i++;
      }
      w.n = (size_t)((p + i) - w.p);
    }
    size_t j = i;
    while (j < n && (p[j] == ' ' || p[j] == '\t' || p[j] == '\n' || p[j] == '\r')) j++;
    bool call = j < n && p[j] == '(';

    if (!first.p) first = w;

    if (call && is_volatile_fn(w)) o->volatile_fn = true;
    if (!call && is_volatile_bare(w)) o->volatile_fn = true;

    if (kw_eq(w, "from") || kw_eq(w, "join") || kw_eq(w, "into") || kw_eq(w, "using") ||
        kw_eq(w, "table") || kw_eq(w, "update")) {
      pending = true;
      if (kw_eq(w, "from")) in_from = true;
      if (kw_eq(w, "update")) write_kw = true;
      continue;
    }
    if (kw_eq(w, "where") || kw_eq(w, "group") || kw_eq(w, "order") || kw_eq(w, "limit") ||
        kw_eq(w, "offset") || kw_eq(w, "having") || kw_eq(w, "union") || kw_eq(w, "on") ||
        kw_eq(w, "set") || kw_eq(w, "values") || kw_eq(w, "returning") || kw_eq(w, "window") ||
        kw_eq(w, "for")) {
      pending = false;
      in_from = false;
      if (kw_eq(w, "for")) o->locking = true; // `for update` / `for share` is never cacheable
      continue;
    }
    if (kw_eq(w, "insert") || kw_eq(w, "delete") || kw_eq(w, "replace") || kw_eq(w, "merge")) {
      write_kw = true;
    }

    if (pending) {
      // `insert into other (id) values (…)` — the parenthesis is a column list, not a call, which
      // is why a pending table name is taken whether or not one follows it.
      stmt_table(o, w);
      pending = false;
      continue;
    }
    if (in_from) {
      if (!call) {
        stmt_table(o, w);
        continue;
      }
    }
  }

  // The kind, from the first word.
  if (!first.p) {
    o->kind = WST_OTHER;
    return;
  }
  if (kw_eq(first, "select")) {
    o->kind = WST_READ;
  } else if (kw_eq(first, "with")) {
    // A CTE is a read unless it writes; when it writes the statement's table set is not worth
    // guessing, and `!all` is the safe answer.
    if (write_kw) {
      o->kind = WST_WRITE;
      o->unknown_tables = true;
    } else {
      o->kind = WST_READ;
    }
  } else if (kw_eq(first, "insert") || kw_eq(first, "update") || kw_eq(first, "delete") ||
             kw_eq(first, "replace") || kw_eq(first, "truncate") || kw_eq(first, "drop") ||
             kw_eq(first, "alter") || kw_eq(first, "create") || kw_eq(first, "merge") ||
             kw_eq(first, "upsert") || kw_eq(first, "rename") || kw_eq(first, "grant") ||
             kw_eq(first, "revoke")) {
    o->kind = WST_WRITE;
  } else if (kw_eq(first, "begin") || kw_eq(first, "start") || kw_eq(first, "commit") ||
             kw_eq(first, "rollback") || kw_eq(first, "end") || kw_eq(first, "savepoint") ||
             kw_eq(first, "release") || kw_eq(first, "set") || kw_eq(first, "pragma") ||
             kw_eq(first, "explain") || kw_eq(first, "analyze") || kw_eq(first, "analyse") ||
             kw_eq(first, "vacuum") || kw_eq(first, "show") || kw_eq(first, "reset") ||
             kw_eq(first, "attach") || kw_eq(first, "detach") || kw_eq(first, "copy") ||
             kw_eq(first, "listen") || kw_eq(first, "notify")) {
    o->kind = WST_OTHER;
  } else {
    // An unknown verb is a write: a statement nobody classified must invalidate rather than be
    // cached, because the cost of the wrong guess is a stale page.
    o->kind = WST_WRITE;
    o->unknown_tables = true;
  }
  if (o->kind == WST_READ && o->ntables == 0) o->unknown_tables = false;
}

bool qcache_is_write(const char *sql) {
  WStmt st;
  stmt_scan(str_lit(sql), &st);
  return st.kind == WST_WRITE;
}

// ---------------------------------------------------------------- the key

static void put_hex(Buf *b, const u8 *p, size_t n) {
  static const char *hex = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    buf_putc(b, hex[p[i] >> 4]);
    buf_putc(b, hex[p[i] & 15]);
  }
}

// Whitespace runs collapse to one space and comments disappear; everything else is kept, so two
// spellings of the same query are two entries — boring, and correct.
static void sql_normalized_into(Buf *b, str sql) {
  const char *p = sql.p;
  size_t n = sql.n, i = 0;
  bool space = false;
  while (i < n) {
    char c = p[i];
    if (c == '\'' || c == '"') {
      char q = c;
      size_t s = i;
      i++;
      while (i < n) {
        if (p[i] == q) {
          if (i + 1 < n && p[i + 1] == q) {
            i += 2;
            continue;
          }
          i++;
          break;
        }
        i++;
      }
      if (space) buf_putc(b, ' ');
      space = false;
      buf_write(b, p + s, i - s);
      continue;
    }
    if (c == '-' && i + 1 < n && p[i + 1] == '-') {
      while (i < n && p[i] != '\n') i++;
      space = true;
      continue;
    }
    if (c == '/' && i + 1 < n && p[i + 1] == '*') {
      while (i + 1 < n && !(p[i] == '*' && p[i + 1] == '/')) i++;
      i = i + 1 < n ? i + 2 : n;
      space = true;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      space = true;
      i++;
      continue;
    }
    if (space) buf_putc(b, ' ');
    space = false;
    buf_putc(b, c);
    i++;
  }
}

// Every parameter is length-prefixed, so ("ab","c") and ("a","bc") cannot digest the same and
// one row's answer can never be served for another's question.
static void params_into(Buf *b, const WQuery *qy) {
  for (i32 i = 0; qy && i < qy->nparams; i++) {
    const WVal *v = &qy->params[i];
    switch (v->kind) {
    case WV_NULL: buf_puts(b, "n:"); break;
    case WV_I64:
      buf_puts(b, "i:");
      buf_puti64(b, v->i);
      break;
    case WV_F64: {
      char tmp[40];
      snprintf(tmp, sizeof tmp, "%.17g", v->f);
      buf_puts(b, "f:");
      buf_puts(b, tmp);
      break;
    }
    case WV_BOOL: buf_puts(b, v->b ? "t:1" : "t:0"); break;
    case WV_TEXT:
    case WV_BLOB:
      buf_puts(b, v->kind == WV_TEXT ? "s:" : "b:");
      buf_puti64(b, (i64)v->s.n);
      buf_putc(b, ':');
      if (v->s.n) buf_write(b, v->s.p, v->s.n);
      break;
    }
    buf_putc(b, ';');
  }
}

// The digest: sha256 of the normalised statement and its bound values, 128 bits kept.
static void digest_of(Arena *a, str sql, const WQuery *qy, char out[33]) {
  Buf b;
  buf_init(&b, a, sql.n + 64);
  sql_normalized_into(&b, sql);
  buf_putc(&b, 0);
  params_into(&b, qy);
  u8 h[32];
  sha256(b.p, b.n, h);
  Buf hx;
  buf_init(&hx, a, 40);
  put_hex(&hx, h, 16);
  memcpy(out, hx.p, 32);
  out[32] = 0;
}

static void gen_key_into(Buf *b, str table) {
  buf_putc(b, 'w');
  buf_puts(b, "ebc:{");
  buf_puts(b, g_qc.ns);
  buf_puts(b, "}:g:");
  buf_putstr(b, table);
}

static void entry_key_into(Buf *b, const char *digest, const i64 *gens, const str *tables, i32 n) {
  buf_puts(b, "webc:{");
  buf_puts(b, g_qc.ns);
  buf_puts(b, "}:q:");
  buf_puts(b, digest);
  buf_puts(b, ":all=");
  buf_puti64(b, gens[0]);
  for (i32 i = 0; i < n; i++) {
    buf_putc(b, ',');
    buf_putstr(b, tables[i]);
    buf_putc(b, '=');
    buf_puti64(b, gens[i + 1]);
  }
}

str qcache_key_for_test(Arena *a, const char *sql, const WQuery *qy) {
  qc_init();
  char digest[33];
  digest_of(a, str_lit(sql), qy, digest);
  Buf b;
  buf_init(&b, a, 128);
  buf_puts(&b, "webc:{");
  buf_puts(&b, g_qc.ns);
  buf_puts(&b, "}:q:");
  buf_puts(&b, digest);
  return buf_str(&b);
}

// ---------------------------------------------------------------- generations

// ---------------------------------------------------------------- the generation memo
//
// Each cached read needs the generation of the tables it touches, and reading them is the extra
// round trip that separates a hit (MGET then GET) from a bare GET. WEBC_CACHE_GEN_MS trades
// exactness for that hop: for that many milliseconds a process reuses the counters it last read,
// so a write from *another* process is invisible until the memo expires. Zero — the default — is
// exact, and the write path is never memoised, so the app's own writes still invalidate at once.
typedef struct GenMemo {
  char sig[224];
  i64 gens[17];
  i32 n;
  i64 at;
} GenMemo;
#define QC_MEMO 64
static GenMemo g_memo[QC_MEMO];
static i32 g_memo_at;
static WMutex g_memo_mu = WEBC_MUTEX_INIT;

static void memo_sig(char *out, size_t cap, const str *tables, i32 n) {
  size_t k = 0;
  k += (size_t)snprintf(out, cap, "%s", g_qc.ns);
  for (i32 i = 0; i < n && k < cap - 1; i++)
    k += (size_t)snprintf(out + k, cap - k, "|%.*s", (int)tables[i].n, tables[i].p);
}

static bool memo_get(const char *sig, i32 n, i64 *gens) {
  bool hit = false;
  wmutex_lock(&g_memo_mu);
  for (i32 i = 0; i < QC_MEMO; i++) {
    if (g_memo[i].n == n + 1 && !strcmp(g_memo[i].sig, sig) &&
        now_ms() - g_memo[i].at < g_qc.gen_ms) {
      memcpy(gens, g_memo[i].gens, sizeof(i64) * (size_t)(n + 1));
      hit = true;
      break;
    }
  }
  wmutex_unlock(&g_memo_mu);
  return hit;
}

static void memo_put(const char *sig, i32 n, const i64 *gens) {
  wmutex_lock(&g_memo_mu);
  GenMemo *m = &g_memo[g_memo_at++ % QC_MEMO];
  snprintf(m->sig, sizeof m->sig, "%s", sig);
  m->n = n + 1;
  memcpy(m->gens, gens, sizeof(i64) * (size_t)(n + 1));
  m->at = now_ms();
  wmutex_unlock(&g_memo_mu);
}

// Reads `!all` plus one counter per table in a single MGET. A missing counter reads as 0, which is
// exactly what a table nobody has written yet is.
static bool gens_read(WRedis *r, Arena *a, const str *tables, i32 n, i64 *gens, i64 timeout_ms) {
  char sig[224];
  bool memo = g_qc.gen_ms > 0;
  if (memo) {
    memo_sig(sig, sizeof sig, tables, n);
    if (memo_get(sig, n, gens)) return true;
  }
  gens[0] = 0;
  for (i32 i = 0; i < n; i++) gens[i + 1] = 0;
  str argv[WC_REDIS_MAX_ARGS];
  if (n + 2 > WC_REDIS_MAX_ARGS) return false;
  argv[0] = str_lit("MGET");
  for (i32 i = 0; i <= n; i++) {
    Buf one_key;
    buf_init(&one_key, a, 96);
    gen_key_into(&one_key, i == 0 ? str_lit("!all") : tables[i - 1]);
    argv[i + 1] = buf_str(&one_key);
  }
  WReply rep = {0};
  if (!redis_call(r, n + 2, argv, timeout_ms, a, &rep)) return false;
  if (rep.kind != WR_ARRAY || rep.n < n + 1) return false;
  for (i32 i = 0; i <= n; i++) {
    const WReply *v = reply_at(&rep, i);
    if (v && v->kind == WR_INT) gens[i] = v->i;
    else if (v && v->kind == WR_STR) gens[i] = atoll(reply_str(v).p ? reply_str(v).p : "0");
  }
  if (memo) memo_put(sig, n, gens);
  return true;
}

static void gens_bump(str *tables, i32 n, bool all) {
  WRedis *r = redis();
  if (!redis_ready(r)) return;
  // The app's own writes are never subject to the memo: that is the line between "a replica is a
  // few milliseconds behind" and "this process reads its own writes".
  wmutex_lock(&g_memo_mu);
  for (i32 i = 0; i < QC_MEMO; i++) g_memo[i].at = 0;
  wmutex_unlock(&g_memo_mu);
  i64 timeout = g_qc.op_ms > 0 ? g_qc.op_ms : 1000;
  if (all || n == 0) {
    Buf kb;
    Arena *a = wc_arena_get();
    bool own = a == nullptr;
    if (own) a = arena_new(1024);
    buf_init(&kb, a, 96);
    gen_key_into(&kb, str_lit("!all"));
    str argv[2] = {str_lit("INCR"), buf_str(&kb)};
    WReply rep = {0};
    redis_call(r, 2, argv, timeout, a, &rep);
    g_qc.st.bumps++;
    metrics_counter_inc("webc_cache_bumps_total");
    trace("bump !all");
    if (own) arena_destroy(a);
    return;
  }
  Arena *a = arena_new(1024);
  for (i32 i = 0; i < n; i++) {
    Buf kb;
    buf_init(&kb, a, 96);
    gen_key_into(&kb, tables[i]);
    str argv[2] = {str_lit("INCR"), buf_str(&kb)};
    WReply rep = {0};
    redis_call(r, 2, argv, timeout, a, &rep);
    g_qc.st.bumps++;
    metrics_counter_inc("webc_cache_bumps_total");
    trace("bump %.*s", (int)tables[i].n, tables[i].p);
  }
  arena_destroy(a);
}

void qcache_bust(const WModel *m) {
  qc_init();
  if (!g_qc.enabled || !m) return;
  const char *t = m->table ? m->table : m->name;
  str one = str_lit(t);
  gens_bump(&one, 1, false);
}

void qcache_flush(void) {
  qc_init();
  if (!g_qc.enabled) return;
  gens_bump(nullptr, 0, true);
}

// ---------------------------------------------------------------- the envelope

static void cell_into(Buf *b, const WVal *v) {
  switch (v->kind) {
  case WV_NULL: buf_puts(b, "\"n:\""); break;
  case WV_I64:
    buf_puts(b, "\"i:");
    buf_puti64(b, v->i);
    buf_putc(b, '"');
    break;
  case WV_F64: {
    char tmp[40];
    snprintf(tmp, sizeof tmp, "%.17g", v->f);
    buf_puts(b, "\"f:");
    buf_puts(b, tmp);
    buf_puts(b, "\"");
    break;
  }
  case WV_BOOL: buf_puts(b, v->b ? "\"t:1\"" : "\"t:0\""); break;
  case WV_TEXT: // handled by cell_text_into(): the tag lives inside the quotes
    break;
  case WV_BLOB: {
    // base64 kept in the same tagged string: the one place bytes cost 33 %, and a blob in a
    // cached row set is rare enough not to design around.
    str enc = {0};
    Arena *tmp = arena_new(256);
    if (base64_encode(tmp, v->s, &enc)) {
      buf_puts(b, "\"b:");
      buf_write(b, enc.p, enc.n);
      buf_puts(b, "\"");
    } else {
      buf_puts(b, "\"n:\"");
    }
    arena_destroy(tmp);
    break;
  }
  }
}

// json_escape_into writes "…" with the prefix already there; a tagged text cell needs the tag
// inside the quotes, so the string is escaped into a scratch buffer and re-wrapped.
static void cell_text_into(Buf *b, str s) {
  Buf esc;
  buf_init(&esc, nullptr, s.n + 8);
  json_escape_into(&esc, s);
  buf_puts(b, "\"s:");
  if (esc.n >= 2) buf_write(b, esc.p + 1, esc.n - 2);
  buf_puts(b, "\"");
  buf_free(&esc);
}

static str envelope_build(Arena *a, str sql, const i64 *gens, const str *tables, i32 ngt, WRes *r,
                          i64 ttl) {
  Buf b;
  buf_init(&b, a, 512);
  buf_puts(&b, "{\"v\":1,\"ns\":");
  json_escape_into(&b, str_lit(g_qc.ns));
  buf_puts(&b, ",\"sql\":");
  json_escape_into(&b, sql);
  buf_puts(&b, ",\"at\":");
  buf_puti64(&b, (i64)wc_time_ms());
  buf_puts(&b, ",\"ttl\":");
  buf_puti64(&b, ttl);
  buf_puts(&b, ",\"gens\":{\"!all\":");
  buf_puti64(&b, gens[0]);
  for (i32 i = 0; i < ngt; i++) {
    buf_putc(&b, ',');
    json_escape_into(&b, tables[i]);
    buf_putc(&b, ':');
    buf_puti64(&b, gens[i + 1]);
  }
  buf_puts(&b, "},\"cols\":[");
  for (i32 i = 0; i < r->ncols; i++) {
    if (i) buf_putc(&b, ',');
    json_escape_into(&b, r->keys[i]);
  }
  buf_puts(&b, "],\"rows\":[");
  for (i32 row = 0; row < r->nrows; row++) {
    if (row) buf_putc(&b, ',');
    buf_putc(&b, '[');
    for (i32 col = 0; col < r->rows[row].n; col++) {
      if (col) buf_putc(&b, ',');
      const WVal *v = &r->rows[row].vals[col];
      if (v->kind == WV_TEXT) cell_text_into(&b, v->s);
      else cell_into(&b, v);
    }
    buf_putc(&b, ']');
  }
  buf_puts(&b, "]}");
  return buf_str(&b);
}

static bool cell_decode(Arena *a, str tagged, WVal *out) {
  memset(out, 0, sizeof *out);
  if (tagged.n < 2 || tagged.p[1] != ':') return false;
  str body = {.p = tagged.p + 2, .n = tagged.n - 2};
  switch (tagged.p[0]) {
  case 'n': out->kind = WV_NULL; return true;
  case 'i': {
    out->kind = WV_I64;
    i64 v = 0;
    bool neg = false;
    size_t k = 0;
    if (body.n && body.p[0] == '-') {
      neg = true;
      k = 1;
    }
    for (; k < body.n; k++) {
      if (body.p[k] < '0' || body.p[k] > '9') return false;
      v = v * 10 + (body.p[k] - '0');
    }
    out->i = neg ? -v : v;
    return true;
  }
  case 'f': {
    char tmp[48];
    size_t k = body.n < sizeof tmp - 1 ? body.n : sizeof tmp - 1;
    memcpy(tmp, body.p, k);
    tmp[k] = 0;
    out->kind = WV_F64;
    out->f = strtod(tmp, nullptr);
    return true;
  }
  case 't': out->kind = WV_BOOL; out->b = body.n && body.p[0] == '1'; return true;
  case 's': out->kind = WV_TEXT; out->s = (str){.p = arena_memdup(a, body.p, body.n), .n = body.n}; return true;
  case 'b': {
    str dec = {0};
    bool ok = base64_decode(a, body, &dec);
    out->kind = WV_BLOB;
    out->s = dec;
    return ok;
  }
  default: return false;
  }
}

static bool envelope_decode(Arena *a, str text, WRes *out) {
  // The JSON parser slices the text it is given, so the text has to live as long as the result:
  // it is copied into the result's own arena before it is read.
  text = (str){.p = arena_memdup(a, text.p, text.n), .n = text.n};
  JParse jp = json_parse(a, text);
  if (!jp.ok || !jp.v || jp.v->kind != J_OBJ) return false;
  JVal *v = jp.v;
  JVal *ver = json_get(v, "v");
  i64 vern = 0;
  if (!ver || !json_as_i64(ver, &vern) || vern != 1) return false; // an unknown version is a miss
  JVal *ns = json_get(v, "ns");
  if (!ns || ns->kind != J_STR) return false;
  if (!kw_eq(ns->s, g_qc.ns)) return false;
  JVal *cols = json_get(v, "cols");
  JVal *rows = json_get(v, "rows");
  if (!cols || cols->kind != J_ARR || !rows || rows->kind != J_ARR) return false;

  memset(out, 0, sizeof *out);
  out->ok = true;
  out->own = a;
  out->ncols = cols->n;
  out->keys = arena_zalloc(a, sizeof(str) * (size_t)(cols->n ? cols->n : 1));
  for (i32 i = 0; i < cols->n; i++) out->keys[i] = json_as_str(cols->items[i]);

  i32 nrows = rows->n;
  out->rows = arena_zalloc(a, sizeof(WRow) * (size_t)(nrows ? nrows : 1));
  out->nrows = 0;
  for (i32 r = 0; r < nrows; r++) {
    JVal *row = rows->items[r];
    if (row->kind != J_ARR) return false;
    WRow *dst = &out->rows[out->nrows];
    dst->n = row->n;
    dst->keys = out->keys;
    dst->vals = arena_zalloc(a, sizeof(WVal) * (size_t)(row->n ? row->n : 1));
    for (i32 c = 0; c < row->n; c++) {
      if (row->items[c]->kind != J_STR) return false;
      if (!cell_decode(a, row->items[c]->s, &dst->vals[c])) return false;
    }
    out->nrows++;
  }
  return true;
}

// ---------------------------------------------------------------- the breaker

static bool breaker_ok(void) {
  if (!g_qc.breaker_on) return true;
  if (g_qc.until_ms == 0) return true;
  if (now_ms() < g_qc.until_ms) return false;
  return true; // the window is over: let one through and see
}

static void breaker_fail(void) {
  g_qc.consec++;
  metrics_counter_inc("webc_cache_errors_total");
  g_qc.st.errors++;
  if (!g_qc.breaker_on) return;
  if (g_qc.consec < g_qc.breaker_after) return;
  i64 window = g_qc.breaker_base_ms;
  for (i32 i = 0; i < g_qc.opens && window < g_qc.breaker_cap_ms; i++) window *= 2;
  if (window > g_qc.breaker_cap_ms) window = g_qc.breaker_cap_ms;
  g_qc.until_ms = now_ms() + window;
  g_qc.opens++;
  metrics_gauge_set("webc_cache_breaker_open", 1, "1 while the query cache is skipping redis");
  log_warn("cache: breaker open — the query cache is skipping redis",
           {"window_ms", str_from_i64(wc_app_arena(), window)}, {"why", redis_error(redis())});
}

static void breaker_ok_again(void) {
  if (g_qc.consec > 0) {
    g_qc.consec = 0;
    g_qc.until_ms = 0;
    metrics_gauge_set("webc_cache_breaker_open", 0, "1 while the query cache is skipping redis");
  }
}

// ---------------------------------------------------------------- the hooks

static i64 op_timeout(void) {
  return g_qc.op_ms > 0 ? g_qc.op_ms : 1000;
}

WQHit qcache_probe(WDB *db, WQuery *qy, WQCtx *ctx) {
  WQHit hit = {0};
  (void)db;
  memset(ctx, 0, sizeof *ctx);
  qc_init();
  if (!g_qc.enabled) {
    g_qc.st.skip_off++;
    return hit;
  }
  if (nocache_depth() > 0) return hit;
  if (!qy || !qy->sql.p || qy->sql.n == 0) return hit;
  WStmt st;
  stmt_scan(qy->sql, &st);
  if (st.kind == WST_OTHER) return hit;
  if (st.kind == WST_WRITE) {
    g_qc.st.skip_write++;
    return hit;
  }
  if (st.hint_nocache) {
    g_qc.st.skip_hint++;
    trace("%.*s -> skip: no-cache hint", (int)(qy->sql.n < 60 ? qy->sql.n : 60), qy->sql.p);
    return hit;
  }
  if ((st.volatile_fn || st.locking) && !st.hint_cache) {
    g_qc.st.skip_volatile++;
    trace("%.*s -> skip: volatile", (int)(qy->sql.n < 60 ? qy->sql.n : 60), qy->sql.p);
    return hit;
  }
  if (st.ntables == 0 && !st.unknown_tables) return hit; // `select 1`: nothing to invalidate
  if (tx_depth() > 0) {
    g_qc.st.skip_tx++;
    trace("%.*s -> skip: inside a transaction", (int)(qy->sql.n < 60 ? qy->sql.n : 60), qy->sql.p);
    return hit;
  }
  if (!breaker_ok()) {
    g_qc.st.skip_breaker++;
    return hit;
  }

  Arena *tmp = arena_new(4096);
  char digest[33];
  digest_of(tmp, qy->sql, qy, digest);
  i64 gens[WC_REDIS_MAX_ARGS];
  if (!gens_read(redis(), tmp, st.tables, st.ntables, gens, op_timeout())) {
    breaker_fail();
    arena_destroy(tmp);
    return hit;
  }
  Buf kb;
  buf_init(&kb, tmp, 256);
  entry_key_into(&kb, digest, gens, st.tables, st.ntables);
  str key = buf_str(&kb);

  WReply rep = {0};
  str argv[2] = {str_lit("GET"), key};
  if (!redis_call(redis(), 2, argv, op_timeout(), tmp, &rep)) {
    breaker_fail();
    arena_destroy(tmp);
    return hit;
  }
  if (rep.kind == WR_NIL) {
    g_qc.st.misses++;
    metrics_counter_inc("webc_cache_misses_total");
    trace("%.*s -> miss", (int)(qy->sql.n < 60 ? qy->sql.n : 60), qy->sql.p);
    arena_destroy(tmp);
    // Remember what the store needs, so the entry is written under the generation it was read
    // with — not under whatever is current when the query finishes.
    ctx->active = true;
    ctx->ntables = st.ntables;
    for (i32 i = 0; i < st.ntables; i++) ctx->tables[i] = st.tables[i];
    for (i32 i = 0; i <= st.ntables; i++) ctx->gens[i] = gens[i];
    return hit;
  }
  if (rep.kind != WR_STR) {
    arena_destroy(tmp);
    return hit;
  }
  Arena *out_arena = arena_new(8192);
  if (!envelope_decode(out_arena, rep.s, &hit.res)) {
    arena_destroy(out_arena);
    arena_destroy(tmp);
    g_qc.st.misses++;
    metrics_counter_inc("webc_cache_misses_total");
    return hit;
  }
  arena_destroy(tmp);
  breaker_ok_again();
  hit.hit = true;
  g_qc.st.hits++;
  metrics_counter_inc("webc_cache_hits_total");
  trace("%.*s -> hit (gens all=%lld%s)", (int)(qy->sql.n < 60 ? qy->sql.n : 60), qy->sql.p,
        (long long)gens[0], "");
  return hit;
}

void qcache_note(WDB *db, WQuery *qy, WRes *r, const WQCtx *ctx) {
  (void)db;
  qc_init();
  if (!g_qc.enabled || !qy || !qy->sql.p) return;
  WStmt st;
  stmt_scan(qy->sql, &st);
  if (st.kind == WST_WRITE) {
    if (st.unknown_tables || st.ntables == 0) gens_bump(nullptr, 0, true);
    else gens_bump(st.tables, st.ntables, false);
    return;
  }
  if (st.kind != WST_READ || !r || !r->ok) return;
  if (nocache_depth() > 0) return;
  if (st.hint_nocache || ((st.volatile_fn || st.locking) && !st.hint_cache)) return;
  if (st.ntables == 0 && !st.unknown_tables) return;
  if (tx_depth() > 0) return; // a row read inside a transaction may be rolled back under us

  Arena *tmp = arena_new(8192);
  char digest[33];
  digest_of(tmp, qy->sql, qy, digest);

  // Store under the generation the *probe* read. Re-read and compare: if a write landed while
  // the query ran, this answer is already older than the generation it would be filed under.
  i64 gens[WC_REDIS_MAX_ARGS];
  if (!gens_read(redis(), tmp, st.tables, st.ntables, gens, op_timeout())) {
    arena_destroy(tmp);
    return;
  }
  if (ctx && ctx->active) {
    for (i32 i = 0; i <= st.ntables; i++)
      if (ctx->gens[i] != gens[i]) {
        trace("store skipped: a write landed during the query");
        arena_destroy(tmp);
        return;
      }
  }

  i64 ttl = st.hint_ttl_ms > 0
                ? st.hint_ttl_ms
                : (st.ntables == 1 ? model_ttl_for(st.tables[0]) : g_qc.ttl_ms);
  if (ttl <= 0) ttl = 1;
  i32 jitter = (i32)g_qc.jitter_pct;
  if (jitter > 0) {
    u64 h = str_hash(qy->sql) % 1000;
    i64 span = ttl * jitter / 100;
    if (span > 0) ttl += (i64)(h * (u64)span / 1000) * 2 - span;
    if (ttl < 1) ttl = 1;
  }

  str env = envelope_build(tmp, qy->sql, gens, st.tables, st.ntables, r, ttl);
  if (env.n > (size_t)g_qc.max_bytes) {
    g_qc.st.skip_too_large++;
    arena_destroy(tmp);
    return;
  }
  Buf kb;
  buf_init(&kb, tmp, 256);
  entry_key_into(&kb, digest, gens, st.tables, st.ntables);
  str key = buf_str(&kb);

  str argv[5] = {str_lit("SET"), key, env, str_lit("PX"), str_from_i64(tmp, ttl)};
  WReply rep = {0};
  if (redis_call(redis(), 5, argv, op_timeout(), tmp, &rep)) {
    g_qc.st.stores++;
    metrics_counter_inc("webc_cache_stores_total");
    trace("%.*s -> stored %zu B ttl %lld ms", (int)(qy->sql.n < 60 ? qy->sql.n : 60), qy->sql.p,
          env.n, (long long)ttl);
    breaker_ok_again();
  } else {
    breaker_fail();
  }
  arena_destroy(tmp);
}

WRes db_exec_nocache(WDB *db, WQuery *qy) {
  qcache_nocache_enter();
  WRes r = db_exec(db, qy);
  qcache_nocache_leave();
  return r;
}

// ---------------------------------------------------------------- debug

str qcache_debug_text(Arena *a) {
  qc_init();
  Buf b;
  buf_init(&b, a, 512);
  WRedis *r = redis();
  buf_printf(&b, "namespace      webc:{%s}:…  (tables %d)\n", g_qc.ns, g_qc.model_n);
  buf_printf(&b, "cache          %s — redis %.*s\n", qcache_on() ? "on" : "off",
             (int)redis_addr(r).n, redis_addr(r).p ? redis_addr(r).p : "");
  buf_printf(&b, "policy         ttl %lld ms (+%lld%% jitter), max %lld B, op cap %lld ms, gens %lld ms, breaker %s\n",
             (long long)g_qc.ttl_ms, (long long)g_qc.jitter_pct, (long long)g_qc.max_bytes,
             (long long)g_qc.op_ms, (long long)g_qc.gen_ms, g_qc.breaker_on ? "on" : "off");
  buf_printf(&b, "breaker        %s (%d consecutive errors, %d opens)\n",
             (g_qc.until_ms && now_ms() < g_qc.until_ms) ? "OPEN" : "closed", g_qc.consec,
             g_qc.opens);
  buf_printf(&b, "pool           %d connections\n", redis_pool_size(r));
  buf_printf(&b, "entries        hits %llu  misses %llu  stores %llu  bumps %llu  errors %llu\n",
             (unsigned long long)g_qc.st.hits, (unsigned long long)g_qc.st.misses,
             (unsigned long long)g_qc.st.stores, (unsigned long long)g_qc.st.bumps,
             (unsigned long long)g_qc.st.errors);
  buf_printf(&b, "skips          tx %llu  write %llu  volatile %llu  hint %llu  breaker %llu  large %llu  off %llu\n",
             (unsigned long long)g_qc.st.skip_tx, (unsigned long long)g_qc.st.skip_write,
             (unsigned long long)g_qc.st.skip_volatile, (unsigned long long)g_qc.st.skip_hint,
             (unsigned long long)g_qc.st.skip_breaker, (unsigned long long)g_qc.st.skip_too_large,
             (unsigned long long)g_qc.st.skip_off);
  return buf_str(&b);
}