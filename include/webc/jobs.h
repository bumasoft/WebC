// SPDX-License-Identifier: Apache-2.0
// webc/jobs.h — work that outlives the request.
//
// `task.h` gives green threads and `res_compute` gives work in the browser. The missing half is
// the durable one: send this receipt in a minute, retry it three times, and show me what failed.
// Every app writes that as a table with a status column plus a `for(;;)` nobody trusts.
//
// The queue here *is* a table, and that is the design: `webc migrate` owns it, an admin page is
// `db_all(VecJob)`, a test can read it, and a `kill -9` is a lease that expires rather than a
// job that vanished.
//
//   WEBC_JOB(send_receipt, Receipt) {               // typed payload: `r` is the argument
//     mail_send(&r->m, str_lit("Your receipt"));
//   }
//
//   send_receipt_do(&rec);                          // enqueue: one insert, no thread
//   send_receipt_later(&rec, 5 * 60 * 1000);        // ... in five minutes
//   job_every(60000, sweep_sessions);               // a named repeat, no cron syntax
//   job_retry(send_receipt, 3, 30000);              // attempts, base backoff — the app says
//   job_worker(2);                                  // in main(), after webc_boot()
//
// A job's payload is the model's own JSON (`model_to_json`), so there is no second serializer
// and no second place a field can be renamed. A job that throws is not lost: `attempts` goes up,
// `last_error` says what happened, `run_at` moves out by the backoff, and after the last attempt
// the row is `failed` and `job_failures()` lists it.
//
// **Enqueue after the commit, not inside it.** `send_receipt_do` writes a row like any other;
// inside a transaction it is rolled back with the transaction, which is usually right and
// occasionally exactly wrong (the mail that is sent for an order that was never saved). The rule
// the module documents is the plain one: call it after `commit`, and if you must call it inside,
// you have said so to yourself.
//
// **Refuses:** a workflow engine, DAGs, cron syntax, priorities, a dashboard, distributed
// workers. The lease makes a second process safe to run, and that is the whole of "distributed".
#pragma once

#include <webc/core.h>
#include <webc/db.h>
#include <webc/model.h>
#include <webc/task.h>
#include <webc/time.h>
#include <webc/vec.h>

// ---------------------------------------------------------------- the table
//
// A model like any other: registered at load, so `webc migrate` creates it and `webc doctor`
// sees it. Text columns rather than an enum, because a job kind is an app's word and an enum
// would make every new job a migration.
WEBC_MODEL(WJob,
           (id, i64, .pk = true),
           (kind, str, .required = true, .doc = "Job kind — the WEBC_JOB name"),
           (payload, Text, .doc = "The argument, as the model's own JSON"),
           (status, str, .doc = "queued | running | done | failed"),
           (attempts, i32),
           (max_attempts, i32),
           (backoff_ms, i64),
           (run_at, i64),
           (lease_until, i64),
           (last_error, Text),
           (created_at, i64),
           (updated_at, i64))

enum { W_JOB_QUEUED = 0, W_JOB_RUNNING = 1, W_JOB_DONE = 2, W_JOB_FAILED = 3 };

// ---------------------------------------------------------------- the register
//
// One row per `WEBC_JOB` in the app, filled by a constructor: the name the app wrote is the
// name the row carries, so a renamed job is a renamed row and a queued job from the previous
// deploy fails loudly ("no such job") instead of silently never running.
// `WJobBody` rather than `WJobFn`: that name is `res_compute`'s in <webc/fe.h>, and two
// different call shapes under one type name is the kind of collision a compiler catches once.
typedef void (*WJobBody)(const void *arg_model, str payload, str *err);

typedef struct WJobKind {
  const char *name;
  const WModel *arg;      // null for a job that takes nothing
  WJobBody run;
  i32 max_attempts;       // defaults, overridable per registration
  i64 backoff_ms;
} WJobKind;

#define WC_JOB_MAX 32
void wc_job_register(const WJobKind *k);
const WJobKind *wc_job_find(const char *name);
i32 wc_job_kinds(void);
const WJobKind *wc_job_kind_at(i32 i);

// `WEBC_JOB(name, T)` — the body is the work, and `r` is the argument decoded from the payload:
//
//   WEBC_JOB(send_receipt, Receipt) {
//     mail_send(&r->m, str_lit("Your receipt"));
//     if (!sent) *err = str_lit("the relay refused it");   // -> attempts++, backoff, retry
//   }
//
//   WEBC_JOB(sweep_sessions) { ... }                        // no argument at all
//
// The second spelling is chosen by arity, so a job that takes nothing does not have to name a
// type it does not use.
#define WEBC_JOB(...) WC_CAT(WC_JOB_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)

#define WC_JOB_2(NAME_, T_)                                                    \
  static void WC_CAT(wc_job_body_, NAME_)(T_ *r, str *err);                    \
  static void WC_CAT(wc_job_run_, NAME_)(const void *, str, str *);            \
  __attribute__((constructor)) static void WC_CAT(wc_job_boot_, NAME_)(void) {  \
    static const WJobKind WC_CAT(k_, NAME_) = {                                \
        .name = #NAME_, .arg = &T_##_model, .run = WC_CAT(wc_job_run_, NAME_),   \
        .max_attempts = 3, .backoff_ms = 30000};                               \
    wc_job_register(&WC_CAT(k_, NAME_));                                       \
  }                                                                            \
  static void WC_CAT(wc_job_run_, NAME_)(const void *arg, str payload, str *e) { \
    (void)arg;                                                                 \
    Arena *job_a = wc_arena_get();                                             \
    T_ *r = (T_ *)arena_alloc(job_a, sizeof(T_));                              \
    memset(r, 0, sizeof(T_));                                                  \
    WModelParse job_p = model_from_json(job_a, &T_##_model, payload, r);        \
    if (!job_p.ok) {                                                           \
      *e = str_lit("the payload did not decode");                              \
      return;                                                                  \
    }                                                                          \
    WC_CAT(wc_job_body_, NAME_)(r, e);                                         \
  }                                                                            \
  /* Enqueue is a verb per job, so the type is named once (in WEBC_JOB) and the    \
     call site says the job's name and nothing else. */                          \
  [[maybe_unused]] static inline bool WC_CAT(NAME_, _do)(const T_ *r) {          \
    return wc_job_enqueue(wc_job_find(#NAME_),                                  \
                          model_to_json(wc_arena_get(), &T_##_model, (const void *)r), 0); \
  }                                                                            \
  [[maybe_unused]] static inline bool WC_CAT(NAME_, _later)(const T_ *r, i64 delay_ms) { \
    return wc_job_enqueue(wc_job_find(#NAME_),                                  \
                          model_to_json(wc_arena_get(), &T_##_model, (const void *)r), delay_ms); \
  }                                                                            \
  static void WC_CAT(wc_job_body_, NAME_)(T_ *r, str *err)

#define WC_JOB_1(NAME_)                                                        \
  static void WC_CAT(wc_job_body_, NAME_)(str *err);                           \
  static void WC_CAT(wc_job_run_, NAME_)(const void *, str, str *);            \
  __attribute__((constructor)) static void WC_CAT(wc_job_boot_, NAME_)(void) {  \
    static const WJobKind WC_CAT(k_, NAME_) = {                                \
        .name = #NAME_, .arg = nullptr, .run = WC_CAT(wc_job_run_, NAME_),      \
        .max_attempts = 3, .backoff_ms = 30000};                               \
    wc_job_register(&WC_CAT(k_, NAME_));                                       \
  }                                                                            \
  static void WC_CAT(wc_job_run_, NAME_)(const void *arg, str payload, str *e) { \
    (void)arg; (void)payload;                                                  \
    WC_CAT(wc_job_body_, NAME_)(e);                                            \
  }                                                                            \
  [[maybe_unused]] static inline bool WC_CAT(NAME_, _do)(void) {                \
    return wc_job_enqueue(wc_job_find(#NAME_), (str){0}, 0);                   \
  }                                                                            \
  [[maybe_unused]] static inline bool WC_CAT(NAME_, _later)(i64 delay_ms) {     \
    return wc_job_enqueue(wc_job_find(#NAME_), (str){0}, delay_ms);            \
  }                                                                            \
  static void WC_CAT(wc_job_body_, NAME_)(str *err)

// ---------------------------------------------------------------- enqueue
//
// `wc_job_enqueue` is the raw door; a job's own `NAME_do()` / `NAME_later()` are the ones to
// call, because they are the two lines where the type is already known.
bool wc_job_enqueue(const WJobKind *k, str payload, i64 delay_ms);
void wc_job_retry_for(const char *name, i32 max_attempts, i64 backoff_ms);

#define job_retry(NAME_, MAX_, BACKOFF_) wc_job_retry_for((#NAME_), (MAX_), (BACKOFF_))

// ---------------------------------------------------------------- the worker
//
// `job_worker(n)` starts n green tasks, each looping: claim one due row, run it, commit. The
// claim is a conditional UPDATE (`status='queued'` → `running`), so two workers never take the
// same job and a second process on the same database is safe without a lock service.
void wc_job_worker(i32 n);

// A repeat: `job_every(60000, sweep)` runs `sweep` on the scheduler every minute, once per
// process (one task, not one per worker). It is not persisted — a repeat that must survive a
// restart belongs in the table as a job that enqueues itself, which is three lines the app owns.
void wc_job_every(i64 every_ms, void (*fn)(void), const char *name);
#define job_every(every_, FN_) wc_job_every((every_), (FN_), #FN_)

// ---------------------------------------------------------------- reading the queue
Vec(WJob); // VecWJob: `db_all(VecWJob)` lists the queue, ordered however the page wants
i32 wc_job_pending(void);                  // queued + running
VecWJob *wc_job_failures(i32 limit);       // failed rows, newest first
void wc_job_requeue_failed(i64 id);        // the admin page's one button
void wc_job_prune(i64 older_than_ms);      // delete done rows older than this

// The one-time table setup, for an app that never ran `webc migrate` (a test, a script).
bool wc_job_install(void);

// ---------------------------------------------------------------- what a test can see
//
// The worker is a green task, and a test that spawned one would have to wait for a scheduler.
// `job_tick()` runs exactly one claim-and-run and returns whether it did anything, so a test of
// a job is a test of the job, not of the loop.
bool wc_job_tick(void);