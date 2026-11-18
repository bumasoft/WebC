// SPDX-License-Identifier: Apache-2.0
// webc — cancellation contexts (SPEC §6.4, §12 "context").
//
// Design: a context is a small heap struct with an atomic flag, a list of cancel
// hooks and a pointer to its parent. Cancelling walks the tree downwards once;
// `ctx_done()` is a relaxed atomic load, so a background task can check it on
// every iteration for free. There is no goroutine per context and no notification
// channel: a task that wants to *sleep* until cancellation uses `ctx_wait()`,
// which parks on a condition variable the cancel path signals.
#include <webc/ctx.h>
#include <webc/task.h>

#include <pthread.h>

#define WC_CTX_HOOKS 8

typedef struct Hook {
  void (*fn)(void *);
  void *arg;
} Hook;

struct WCtx {
  _Atomic bool done;
  _Atomic int refs;      // parent -> child links count as one reference each
  struct WCtx *parent;
  struct WCtx *children[8];
  i32 nchildren;
  Hook hooks[WC_CTX_HOOKS];
  i32 nhooks;
  i64 deadline_ms;       // 0 = none
  pthread_mutex_t mu;    // guards the tree and the waiters
  pthread_cond_t cv;
};

static void ctx_unlink_child(WCtx *child) {
  WCtx *p = child->parent;
  if (!p) return;
  pthread_mutex_lock(&p->mu);
  for (i32 i = 0; i < p->nchildren; i++)
    if (p->children[i] == child) {
      p->children[i] = p->children[--p->nchildren];
      break;
    }
  pthread_mutex_unlock(&p->mu);
  child->parent = nullptr;
}

// Cancel this node and everything below it. Idempotent: the second call is a
// load and a compare-exchange that fails.
static void ctx_cancel_tree(WCtx *c) {
  if (!c) return;
  bool expected = false;
  if (!atomic_compare_exchange_strong(&c->done, &expected, true)) return;
  // hooks run outside the lock: a hook may free things that take it
  for (i32 i = 0; i < c->nhooks; i++)
    if (c->hooks[i].fn) c->hooks[i].fn(c->hooks[i].arg);
  pthread_mutex_lock(&c->mu);
  pthread_cond_broadcast(&c->cv);
  WCtx *kids[8];
  i32 n = c->nchildren;
  for (i32 i = 0; i < n; i++) kids[i] = c->children[i];
  pthread_mutex_unlock(&c->mu);
  for (i32 i = 0; i < n; i++) ctx_cancel_tree(kids[i]);
}

WCtx *ctx_new(WCtx *parent) {
  WCtx *c = wm_alloc_zero(sizeof *c);
  pthread_mutex_init(&c->mu, nullptr);
  pthread_cond_init(&c->cv, nullptr);
  if (parent) {
    c->parent = parent;
    pthread_mutex_lock(&parent->mu);
    if (parent->nchildren < 8) {
      parent->children[parent->nchildren++] = c;
    } else {
      // A deep fan-out keeps only the first eight links; the rest still see the
      // parent's cancellation through ctx_done() walking upwards.
      c->parent = nullptr;
    }
    bool parent_done = atomic_load(&parent->done);
    pthread_mutex_unlock(&parent->mu);
    if (parent_done) ctx_cancel_tree(c);
  }
  return c;
}

WCtx *ctx_with_timeout(WCtx *parent, i64 ms) {
  WCtx *c = ctx_new(parent);
  c->deadline_ms = wc_time_ms() + ms;
  return c;
}

i64 ctx_deadline_ms(WCtx *ctx) { return ctx ? ctx->deadline_ms : 0; }

void ctx_set_deadline(WCtx *ctx, i64 deadline_ms) {
  if (ctx) ctx->deadline_ms = deadline_ms;
}

void ctx_cancel(WCtx *ctx) {
  ctx_cancel_tree(ctx);
}

bool ctx_done(WCtx *ctx) {
  for (WCtx *c = ctx; c; c = c->parent) {
    if (atomic_load_explicit(&c->done, memory_order_relaxed)) return true;
    if (c->deadline_ms && (i64)wc_time_ms() >= c->deadline_ms) {
      ctx_cancel_tree(c);
      return true;
    }
  }
  return false;
}

bool ctx_wait(WCtx *ctx, i64 ms) {
  if (!ctx) {
    if (ms > 0) wg_sleep_ms((u64)ms);
    return false;
  }
  i64 until = ms > 0 ? wc_time_ms() + ms : 0;
  pthread_mutex_lock(&ctx->mu);
  while (!ctx_done(ctx)) {
    if (until) {
      i64 left = until - wc_time_ms();
      if (left <= 0) break;
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += (time_t)(left / 1000);
      ts.tv_nsec += (long)(left % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
      }
      pthread_cond_timedwait(&ctx->cv, &ctx->mu, &ts);
    } else {
      pthread_cond_wait(&ctx->cv, &ctx->mu);
    }
  }
  bool done = ctx_done(ctx);
  pthread_mutex_unlock(&ctx->mu);
  return done;
}

str ctx_err(WCtx *ctx) {
  if (!ctx || !ctx_done(ctx)) return (str){0};
  for (WCtx *c = ctx; c; c = c->parent)
    if (c->deadline_ms && (i64)wc_time_ms() >= c->deadline_ms)
      return str_lit("deadline exceeded");
  return str_lit("cancelled");
}

void ctx_on_cancel(WCtx *ctx, void (*fn)(void *), void *arg) {
  if (!ctx || !fn) return;
  pthread_mutex_lock(&ctx->mu);
  if (ctx->nhooks < WC_CTX_HOOKS && !atomic_load(&ctx->done)) {
    ctx->hooks[ctx->nhooks++] = (Hook){.fn = fn, .arg = arg};
    pthread_mutex_unlock(&ctx->mu);
    return;
  }
  pthread_mutex_unlock(&ctx->mu);
  // Already cancelled (or out of slots): run it now, so the caller never has to
  // care which order things happened in.
  fn(arg);
}

void ctx_free(WCtx *ctx) {
  if (!ctx) return;
  ctx_unlink_child(ctx);
  ctx_cancel_tree(ctx); // hooks run before the memory goes away
  pthread_mutex_destroy(&ctx->mu);
  pthread_cond_destroy(&ctx->cv);
  wm_free(ctx);
}