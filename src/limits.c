// SPDX-License-Identifier: Apache-2.0
// webc/limits.h — the table and the middleware. Route-side only: nothing here reaches the
// client half, and nothing here allocates.
#include <webc/limits.h>

static WRateState g_rate;

WRateState *wc_rate(void) { return &g_rate; }

void wc_rate_reset(void) { memset(&g_rate, 0, sizeof g_rate); }

bool wc_rate_mw_fn(Request *req, void *ctx, WNext *next) {
  WRateMw *cfg = ctx;
  if (!cfg || cfg->max <= 0) return wc_next_run(next);
  if (cfg->prefix && *cfg->prefix) {
    str p = str_lit(cfg->prefix);
    if (req->path.n < p.n || memcmp(req->path.p, p.p, p.n) != 0) return wc_next_run(next);
  }
  Buf k;
  buf_init(&k, req->arena, 64);
  buf_puts(&k, "ip:");
  if (req->peer.p && req->peer.n) buf_putstr(&k, req->peer);
  else buf_puts(&k, "unknown");
  if (!wc_rate_hit(&g_rate, buf_str(&k), cfg->max, cfg->window_ms)) {
    i64 wait = wc_rate_wait_ms(&g_rate, buf_str(&k), cfg->window_ms);
    char secs[24];
    snprintf(secs, sizeof secs, "%lld", (long long)((wait + 999) / 1000));
    *next->res = res_error(429, "slow_down", "Too many requests.");
    res_header(next->res, "Retry-After", secs);
    return false;
  }
  return wc_next_run(next);
}
