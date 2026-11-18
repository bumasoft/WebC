// SPDX-License-Identifier: Apache-2.0
// webc/ctx.h — cancellation, the Go `context` equivalent (SPEC §6.4, §12).
//
//   WCtx *ctx = req->ctx;                       // the request's own context
//   task_async(worker, ctx);                    // hand it to background work
//   if (ctx_done(ctx)) return;                   // the client went away
//   ctx_on_cancel(ctx, cleanup, arg);            // run when it is cancelled
//
// A request's context is cancelled when
//   * the peer closes the connection before the response is written,
//   * `mw_timeout_ms()` fires,
//   * the server starts draining on SIGINT/SIGTERM,
//   * a handler cancels it (`ctx_cancel`), e.g. after a failed prerequisite.
//
// A context is a value, not a goroutine: `ctx_done()` is a flag read, so a
// background task polls it (or parks in `ctx_wait(ctx, ms)`) and stops early
// instead of finishing work nobody wants. No thread-local state, no async
// colour, no goroutine per context — the budget for this file is 100 lines.
#ifndef WEBC_CTX_H
#define WEBC_CTX_H

#include <webc/core.h>

typedef struct WCtx WCtx;

// A child inherits cancellation: cancelling the parent cancels the child.
WCtx *ctx_new(WCtx *parent);
WCtx *ctx_with_timeout(WCtx *parent, i64 ms);
void  ctx_cancel(WCtx *ctx);
// Set an absolute deadline (ms since the epoch, as wc_time_ms() reports it). A
// middleware that promises a timeout uses this so the handler's own ctx_done()
// polls tell the same story as the 504 the client gets.
void  ctx_set_deadline(WCtx *ctx, i64 deadline_ms);
// That deadline back, for a driver that wants to hand the *server* the same limit:
// `SET max_execution_time` stops the work even when this client is already gone.
i64   ctx_deadline_ms(WCtx *ctx); // 0 = none
bool  ctx_done(WCtx *ctx);       // true once cancelled (or its deadline passed)
// Park the current task until the context is done or `ms` elapses. Returns
// ctx_done(). This is what makes cancellation *cheap*: no polling loop.
bool  ctx_wait(WCtx *ctx, i64 ms);
str   ctx_err(WCtx *ctx);        // "cancelled" / "deadline exceeded" / ""
void  ctx_on_cancel(WCtx *ctx, void (*fn)(void *), void *arg);
// Free the context and drop its link to the parent. Cancel hooks run first, so a
// context that is freed still cleans up after itself.
void  ctx_free(WCtx *ctx);

#endif // WEBC_CTX_H