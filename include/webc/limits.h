// SPDX-License-Identifier: Apache-2.0
// webc/limits.h — "not more than N of these per window, per key".
//
// Two different jobs are often confused, and they want different modules: a *body* limit is a
// number the server reads before parsing (`mw_body_limit`, in core), and a *rate* is history
// kept across requests, which is this file. The history is a small fixed table in process
// memory — 256 keys, no allocation, no goroutine — so one `per_ip` line in a route is cheap
// enough to be on by default.
//
//   webc_use(mw_rate_per_ip(60, 60 * 1000));               // 60 requests a minute per address
//   POST("/login") {
//     if (!rate_ok("login", 5, 15 * 60 * 1000))
//       return res_error(429, "slow_down", "Too many attempts. Try again in %s.",
//                        dur_secs(rate_wait_ms("login")));
//     ...
//   }
//
// A key is the app's own word ("login", "reset:<user id>") or `ip` — never an allocation the
// module made up, so a limit that fires can always name itself in a log line.
//
// The window is a **fixed window**, not a sliding one: it costs four integers per key and it is
// what a person means by "5 attempts a quarter of an hour". The cost is the classic edge (a
// burst at the end of one window and the start of the next), which for a login form is the
// difference between five and ten attempts, not between safe and broken.
//
// **Refuses:** a distributed counter (the day an app has three servers it has Redis and an
// app's code to use it — a half-sharded local table would be a lie), quotas per plan, a
// `Retry-After` header written for you (a response, not a module, decides its own headers),
// and any logging.
#pragma once

#include <webc/core.h>
#include <webc/http.h>
#include <webc/time.h>

#define WC_RATE_SLOTS 256

typedef struct WRateSlot {
  u64 key_hash;
  char key[48];      // the key as written, so a hit can say which one it was
  i64 window_at;     // when this window opened
  i32 count;
  bool used;
} WRateSlot;

typedef struct WRateState {
  WRateSlot slots[WC_RATE_SLOTS];
  i32 hits;      // how many refusals since the last reset — for a test, not for a metric
  i32 last_slot; // where the last refusal happened
} WRateState;

// A module-level table would be hidden state; a value the app holds is state it can see, reset
// between tests, and reason about. `rate_ok` is the one-call version for the lazy path.
WRateState *wc_rate(void);   // the process's table (created on first use)
void wc_rate_reset(void);

static inline WRateSlot *wc_rate_find(WRateState *st, str key, bool create) {
  if (!key.p || !key.n) return nullptr;
  str capped = {.p = key.p, .n = key.n < 47 ? key.n : 47};
  u64 h = str_hash(capped);
  i32 start = (i32)(h % WC_RATE_SLOTS);
  i32 free_slot = -1;
  for (i32 i = 0; i < WC_RATE_SLOTS; i++) {
    i32 at = (start + i) % WC_RATE_SLOTS;
    WRateSlot *s = &st->slots[at];
    if (!s->used) {
      if (free_slot < 0) free_slot = at;
      break;
    }
    if (s->key_hash == h && strlen(s->key) == capped.n && memcmp(s->key, capped.p, capped.n) == 0)
      return s;
  }
  if (!create || free_slot < 0) {
    // The table is full: reuse the oldest window instead of growing. A limiter that allocates
    // under attack is a limiter that fails under attack.
    i32 oldest = 0;
    for (i32 i = 1; i < WC_RATE_SLOTS; i++)
      if (st->slots[i].window_at < st->slots[oldest].window_at) oldest = i;
    free_slot = oldest;
  }
  WRateSlot *s = &st->slots[free_slot];
  memset(s, 0, sizeof *s);
  s->key_hash = h;
  memcpy(s->key, capped.p, capped.n);
  s->key[capped.n] = 0;
  s->used = true;
  s->window_at = time_ms();
  return s;
}

// Count one hit against `key`. False when the window's budget is gone; `count` is left as the
// number that was over the limit, which is what a message wants to say.
static inline bool wc_rate_hit(WRateState *st, str key, i32 max, i64 window_ms) {
  if (!st || max <= 0) return true; // no limit configured is not a refusal
  WRateSlot *s = wc_rate_find(st, key, true);
  if (!s) return true;
  i64 now = time_ms();
  if (window_ms > 0 && now - s->window_at >= window_ms) {
    s->window_at = now;
    s->count = 0;
  }
  s->count++;
  if (s->count > max) {
    st->hits++;
    st->last_slot = (i32)(s - st->slots);
    return false;
  }
  return true;
}

// How long until the window that refused will open again — for "try again in 40s".
static inline i64 wc_rate_wait_ms(WRateState *st, str key, i64 window_ms) {
  WRateSlot *s = st ? wc_rate_find(st, key, false) : nullptr;
  if (!s) return 0;
  i64 left = s->window_at + window_ms - time_ms();
  return left > 0 ? left : 0;
}

#define rate_hit(key_, max_, window_ms_) wc_rate_hit(wc_rate(), WC_AS_STR(key_), (max_), (window_ms_))
#define rate_ok(key_, max_, window_ms_) wc_rate_hit(wc_rate(), WC_AS_STR(key_), (max_), (window_ms_))
#define rate_wait_ms(key_, window_ms_) wc_rate_wait_ms(wc_rate(), WC_AS_STR(key_), (window_ms_))
#define rate_reset() wc_rate_reset()

// ---------------------------------------------------------------- the middleware
//
// `per_ip` is the limit almost every app wants for almost every route: one address, N requests
// a window, and a 429 that says when to come back. The address comes from the peer, not from
// `X-Forwarded-For`, unless the app is behind a proxy it has told the server about — a header
// a client can write is a header a client can forge.
typedef struct WRateMw {
  i32 max;
  i64 window_ms;
  const char *prefix; // "/api" limits that subtree only; nullptr limits everything
} WRateMw;

bool wc_rate_mw_fn(Request *req, void *ctx, WNext *next);
#define mw_rate_per_ip(max_, window_ms_)                                                \
  ((WMiddleware){                                                                       \
      .name = "rate",                                                                   \
      .fn = wc_rate_mw_fn,                                                              \
      .ctx = &(WRateMw){.max = (max_), .window_ms = (window_ms_), .prefix = nullptr},     \
      .order = 30})
#define mw_rate_for(prefix_, max_, window_ms_)                                          \
  ((WMiddleware){                                                                       \
      .name = "rate",                                                                   \
      .fn = wc_rate_mw_fn,                                                              \
      .ctx = &(WRateMw){.max = (max_), .window_ms = (window_ms_), .prefix = (prefix_)},   \
      .order = 30})

// Shim for a site that genuinely has no `time.h` clock available (bare wasm host tests):
// `limits` is route-side only, where one always is.
#define wc_limits_now_ms() time_ms()