// SPDX-License-Identifier: Apache-2.0
// WebC — logging and metrics.
#include <webc/log.h>
#include <webc/task.h>

// ---------------------------------------------------------------- logging

static _Atomic int g_level = -1;
static _Atomic bool g_json = false;

LogLevel log_level(void) {
  int lvl = atomic_load(&g_level);
  if (lvl < 0) {
    const char *env = getenv("WEBC_LOG_LEVEL");
    lvl = LOG_INFO;
    if (env) {
      if (!strcasecmp(env, "debug"))
        lvl = LOG_DEBUG;
      else if (!strcasecmp(env, "info"))
        lvl = LOG_INFO;
      else if (!strcasecmp(env, "warn"))
        lvl = LOG_WARN;
      else if (!strcasecmp(env, "error"))
        lvl = LOG_ERROR;
      else if (!strcasecmp(env, "off"))
        lvl = LOG_OFF;
    }
    atomic_store(&g_level, lvl);
    const char *fmt = getenv("WEBC_LOG");
    if (fmt && !strcasecmp(fmt, "json")) atomic_store(&g_json, true);
  }
  return (LogLevel)lvl;
}

void log_set_level(LogLevel lvl) {
  atomic_store(&g_level, (int)lvl);
}
void log_set_json(bool json) {
  atomic_store(&g_json, json);
}
bool log_is_json(void) {
  log_level();
  return atomic_load(&g_json);
}

static const char *level_name(LogLevel l) {
  switch (l) {
  case LOG_DEBUG:
    return "debug";
  case LOG_INFO:
    return "info";
  case LOG_WARN:
    return "warn";
  case LOG_ERROR:
    return "error";
  default:
    return "off";
  }
}

// One log line is written on every request, so nothing on this path may allocate on the
// heap and forget it: the timestamp and the escaped fields go into stack buffers, never
// through `str_fmt(nullptr, …)` or `json_escape(nullptr, …)`, which hand back heap memory
// nobody frees — invisible in a benchmark, 450 KB per 110k requests in /metrics.
void log_write(LogLevel lvl, const char *msg, const LogField *fields, i32 n) {
  if (lvl < log_level()) return;
  u64 now = wc_wall_ms();
  char tsbuf[40];
  size_t tsn = wc_time_rfc3339_buf(tsbuf, sizeof tsbuf, now);
  if (log_is_json()) {
    // JSON needs escaping, so it gets an arena that dies with the call.
    Arena *a = arena_new(1024);
    Buf b;
    buf_init(&b, a, 256);
    buf_printf(&b, "{\"ts\":\"%.*s\",\"level\":\"%s\",\"msg\":\"%s\"", (int)tsn, tsbuf,
               level_name(lvl), msg);
    for (i32 i = 0; i < n; i++) {
      buf_printf(&b, ",\"%s\":", fields[i].key);
      buf_putstr(&b, json_escape(a, fields[i].value));
    }
    buf_puts(&b, "}\n");
    fwrite(b.p, 1, b.n, stderr);
    arena_destroy(a);
  } else {
    fprintf(stderr, "%.*s %-5s %s", (int)tsn, tsbuf, level_name(lvl), msg);
    for (i32 i = 0; i < n; i++)
      fprintf(stderr, " %s=%.*s", fields[i].key, (int)fields[i].value.n, fields[i].value.p);
    fputc('\n', stderr);
  }
  fflush(stderr);
}

// ---------------------------------------------------------------- metrics

#define WC_METRIC_MAX 128

typedef struct Metric {
  char name[64];
  const char *help;
  bool used;
  bool is_hist;
  _Atomic i64 counter;
  _Atomic i64 hist_buckets[8];
  _Atomic i64 hist_count;
  _Atomic i64 hist_sum; // in whole units (ms or bytes)
} Metric;

static Metric g_metrics[WC_METRIC_MAX];
static WMutex g_mu = WEBC_MUTEX_INIT;
static _Atomic u64 g_requests;

static const f64 kBuckets[8] = {1, 5, 10, 25, 50, 100, 250, 1000};

// The table is append-only: an entry is written once, then only its atomic counters change,
// and g_nused is published after the entry is complete. So a reader can walk it without any
// lock, and the mutex is needed only to create a metric — which happens a handful of times
// per process. A lookup that takes the mutex costs a rendered request three of them
// (requests_total, status_N_total, duration histogram), six scheduler lock acquisitions
// spent publishing numbers nobody was scraping.
static _Atomic i32 g_nused;

static Metric *metric_find_lockfree(const char *name) {
  i32 n = atomic_load_explicit(&g_nused, memory_order_acquire);
  for (i32 i = 0; i < n; i++)
    if (strcmp(g_metrics[i].name, name) == 0) return &g_metrics[i];
  return nullptr;
}

static Metric *metric_get(const char *name, bool hist, const char *help) {
  Metric *m = metric_find_lockfree(name);
  if (m) return m;
  wmutex_lock(&g_mu);
  m = metric_find_lockfree(name); // another worker may have created it while we waited
  if (m) {
    wmutex_unlock(&g_mu);
    return m;
  }
  i32 n = atomic_load(&g_nused);
  if (n >= WC_METRIC_MAX) {
    wmutex_unlock(&g_mu);
    return nullptr;
  }
  m = &g_metrics[n];
  m->used = true;
  snprintf(m->name, sizeof m->name, "%s", name);
  m->help = help;
  m->is_hist = hist;
  atomic_store_explicit(&g_nused, n + 1, memory_order_release);
  wmutex_unlock(&g_mu);
  return m;
}

void metrics_counter_add(const char *name, i64 delta, const char *help) {
  Metric *m = metric_get(name, false, help);
  if (m) atomic_fetch_add(&m->counter, delta);
}

void metrics_counter_inc(const char *name) {
  metrics_counter_add(name, 1, nullptr);
}

void metrics_gauge_set(const char *name, f64 v, const char *help) {
  Metric *m = metric_get(name, false, help);
  if (m) atomic_store(&m->counter, (i64)v);
}

void metrics_observe(const char *name, f64 v, const char *help) {
  Metric *m = metric_get(name, true, help);
  if (!m) return;
  i32 b = 0;
  while (b < 7 && v > kBuckets[b])
    b++;
  atomic_fetch_add(&m->hist_buckets[b], 1);
  atomic_fetch_add(&m->hist_count, 1);
  atomic_fetch_add(&m->hist_sum, (i64)v);
}

void metrics_observe_http(str method, i32 status, u64 duration_ms) {
  atomic_fetch_add(&g_requests, 1);
  // One metric handle per status code, resolved once and then only incremented, so the
  // name is formatted and strcmp'd once per status code per process.
  static _Atomic(Metric *) by_status[600];
  Metric *m = nullptr;
  if (status > 0 && status < 600) {
    m = atomic_load_explicit(&by_status[status], memory_order_acquire);
    if (!m) {
      char name[80];
      snprintf(name, sizeof name, "webc_http_status_%d_total", status);
      m = metric_get(name, false, nullptr);
      if (m) atomic_store_explicit(&by_status[status], m, memory_order_release);
    }
  }
  static _Atomic(Metric *) req_total, dur_hist;
  Metric *rt = atomic_load_explicit(&req_total, memory_order_acquire);
  if (!rt) {
    rt = metric_get("webc_http_requests_total", false, nullptr);
    if (rt) atomic_store_explicit(&req_total, rt, memory_order_release);
  }
  Metric *dh = atomic_load_explicit(&dur_hist, memory_order_acquire);
  if (!dh) {
    dh = metric_get("webc_http_request_duration_ms", true, "request duration in ms");
    if (dh) atomic_store_explicit(&dur_hist, dh, memory_order_release);
  }
  if (rt) atomic_fetch_add(&rt->counter, 1);
  if (m) atomic_fetch_add(&m->counter, 1);
  if (dh) {
    i32 b = 0;
    f64 v = (f64)duration_ms;
    while (b < 7 && v > kBuckets[b]) b++;
    atomic_fetch_add(&dh->hist_buckets[b], 1);
    atomic_fetch_add(&dh->hist_count, 1);
    atomic_fetch_add(&dh->hist_sum, (i64)v);
  }
  (void)method;
}

u64 metrics_request_count(void) {
  return atomic_load(&g_requests);
}

void metrics_reset(void) {
  wmutex_lock(&g_mu);
  atomic_store_explicit(&g_nused, 0, memory_order_release);
  memset(g_metrics, 0, sizeof g_metrics);
  wmutex_unlock(&g_mu);
  atomic_store(&g_requests, 0);
}

void metrics_render(Arena *a, Buf *out) {
  (void)a;
  for (i32 i = 0; i < WC_METRIC_MAX; i++) {
    Metric *m = &g_metrics[i];
    if (!m->used) continue;
    if (!m->is_hist) {
      if (m->help) buf_printf(out, "# HELP %s %s\n# TYPE %s gauge\n", m->name, m->help, m->name);
      buf_printf(out, "%s %lld\n", m->name, (long long)atomic_load(&m->counter));
      continue;
    }
    buf_printf(out, "# TYPE %s histogram\n", m->name);
    i64 cum = 0;
    for (i32 b = 0; b < 8; b++) {
      cum += atomic_load(&m->hist_buckets[b]);
      buf_printf(out, "%s_bucket{le=\"%g\"} %lld\n", m->name, kBuckets[b], (long long)cum);
    }
    buf_printf(out, "%s_bucket{le=\"+Inf\"} %lld\n", m->name,
               (long long)atomic_load(&m->hist_count));
    buf_printf(out, "%s_count %lld\n", m->name, (long long)atomic_load(&m->hist_count));
    buf_printf(out, "%s_sum %lld\n", m->name, (long long)atomic_load(&m->hist_sum));
  }
  WmStats st = wm_stats();
  buf_printf(out, "# TYPE webc_heap_bytes gauge\nwebc_heap_bytes %zu\n", st.heap_used);
  buf_printf(out, "# TYPE webc_arena_bytes gauge\nwebc_arena_bytes %zu\n", st.arena_live);
  buf_printf(out, "# TYPE webc_tasks gauge\nwebc_tasks %d\n", wtask_live());
}

// The printing half of wc_limit_once() (core.c): core.c is linked into the wasm module
// too, where there is no stderr, so the reporting is a weak symbol provided here.
void wc_limit_print(const char *msg, const char *limit, const char *what) {
  log_warn(msg, {"limit", str_lit(limit)}, {"dropped", str_lit(what)});
}
