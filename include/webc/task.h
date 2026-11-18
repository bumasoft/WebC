// SPDX-License-Identifier: Apache-2.0
// WebC — concurrency: green threads on an M:N scheduler, typed channels, select,
// and the sync primitives (SPEC §6.4).
//
//   task_async(fn, arg);              // spawn, do not wait
//   TaskHandle h = task_result(fn, arg);
//   task_join_all(hs, n);             // 0 = all ok, else the index+1 that failed
//   ChanInt ch = chan_int_new(16);    // capacity channels
//   chan_int_send(ch, v); chan_int_recv(ch, &v);
//   select(ops, n) { case 0: … case 1: … }
//
// Blocking inside a task is legal and cheap: send/recv, sleep, mutex and socket
// I/O all park the task and hand the OS thread to somebody else.
#ifndef WEBC_TASK_H
#define WEBC_TASK_H

#include <webc/core.h>

typedef struct Wg Wg;
typedef struct Wp Wp;
typedef struct WChan WChan;
typedef struct WTaskHandle WTaskHandle;

typedef void (*WgFn)(void *arg);

typedef enum WgState : u8 { WG_READY, WG_RUNNING, WG_PARKED, WG_DEAD } WgState;

// ---------------------------------------------------------------- scheduler

// Stacks are reused; when an app needs thousands of live tasks it can trade
// headroom for memory (default 256 KiB, minimum 32 KiB).
void   wtask_set_stack_size(size_t bytes);
size_t wtask_stack_size(void);

void wtask_init(i32 threads);          // 0 = one per core; safe to call twice
void wtask_wait(void);       // block until every task has finished
void wtask_shutdown(void);
i32  wtask_threads(void);
// The worker OS threads (as pthread_t values), for the profiler.
i32  wtask_os_threads(void **out, i32 max);
bool wtask_this_thread_busy(void); // the profiler skips idle workers
// Scheduler counters, for /debug/sched and the scale benchmark: turns, idle entries,
// polls, wakeups, steals and mutex operations. A worker count that raises "polls per
// request" instead of "requests per second" is contention, named.
typedef struct WTaskStats {
  long iter, idle, poll, wake, steal, lock, unlock, ready;
  i32 procs;
} WTaskStats;

void wtask_stats(WTaskStats *out);

// WC_LOCK_HIST: the hottest g_lock() call sites, by __LINE__. A single mutex that a request
// takes fourteen times is not a mystery to be reasoned about — it is a number to be read.
#ifdef WC_LOCK_HIST
void lock_hist_top(long *lines, long *counts, i32 max, i32 *n);
void lock_time_top(i32 *lines, long *counts, long *wait_ns, long *wait_max, long *hold_ns,
                   long *hold_max, i32 want, i32 *n);
void ra_hist_top(uintptr_t *ras, long *counts, i32 max, i32 *n);
#endif


// Acquisitions of the scheduler lock that found it already held, out of all of them.
// Counting futexes from outside the process cannot tell you *which* lock they were for;
// this can, and it needs no clock.
void gmu_contention(long *fast, long *slow);
// Socket waits, split by direction: waiting for the peer to read a body is not the same
// problem as waiting for the next request, and only one of them is fixable in the writer.
void fd_park_counts(long *rd, long *wr);

i32  wtask_live(void);                 // live (not dead) tasks
u64  wtask_spawned(void);
void wtask_dump(FILE *out);
// Same, but never blocks: reports a wedged scheduler lock instead of waiting.
void wtask_dump_safe(FILE *out);

// Task-local storage: `webc_req_bind` and friends hang the current request
// here, not on the OS thread — many tasks share a thread, and a thread-local
// lets two in-flight requests clobber each other (measured: 96% of concurrent
// POSTs failed with "no current request").
void  wtask_set_local(void *p);
void  wtask_set_build(void *p);    // the markup build in progress (src/dom.c)
void *wtask_build(void);
// The reactive runtime in use. It belongs to the task for the same reason the markup
// build does: an effect that parks resumes on another worker, and a thread-local
// runtime there would flush a different graph (silently skipped updates).
void *wtask_rt_get(void);
void wtask_rt_set(void *rt);
void *wtask_local(void);            // goroutine dump for /debug

// The task's database session (src/db.c): one connection, owned by this task for as long
// as it lives. Task-local for the same reason the request is — a task that parks resumes on
// another worker, and a thread-local session there would hand one request's transaction to
// another (SPEC §16.10 is the incident that rule comes from).
void *wtask_db_get(void);
void  wtask_db_set(void *p);
// A framework-owned task-local slot beside the database session: the query cache keeps its
// `nocache` depth here, because a depth kept in a thread-local would leak across the tasks that
// share a worker (SPEC §16.10).
void *wtask_aux_get(void);
void  wtask_aux_set(void *p);
// Run `fn(still_set)` on the task's own stack as it ends, so a session a panicking handler
// left open is rolled back and returned to the pool instead of being stranded. db.c
// registers it once; a task that released its session leaves null and nothing runs.
void  wtask_set_exit_hook(void (*fn)(void *));

Wg  *wg_spawn(WgFn fn, void *arg, const char *name);
void wg_yield(void);
void wg_sleep_ms(u64 ms);

typedef struct TaskResult {
  bool ok;
  WErr e;
  void *value;
} TaskResult;

// The task family (SPEC §6.4): spawn and forget / spawn and keep a handle / join.
//
// A task has no return channel: the only way it can fail is by panicking, and a panic
// is caught at the task's frame rather than taking the process down. task_join() then
// answers ok = false with WEBC_EEXTERNAL ("task panicked"). A handle whose result is set
// before the work runs can never report this, and the type then promises an error nobody
// can observe.
void        task_async(WgFn fn, void *arg);
WTaskHandle *task_result(WgFn fn, void *arg);
TaskResult  task_join(WTaskHandle *h);
i32         task_join_all(WTaskHandle **hs, i32 n);   // 0 = all ok, else index+1
void        task_handle_free(WTaskHandle *h);

// Parallel for over a *named* step function (closure-free; see docs/reference.md
// for why a block-bodied for_par cannot be truly parallel in C).
void for_par_step(i32 lo, i32 hi, void (*step)(i32 index, void *ctx), void *ctx);
#define for_par(i_, n_) for (i32 i_ = 0, _wc_par_n_ = (n_); i_ < _wc_par_n_; i_++)

// ---------------------------------------------------------------- channels

WChan *wchan_new(size_t elem, i32 cap);
bool   wchan_send(WChan *c, const void *v);     // false when the channel is closed
bool   wchan_recv(WChan *c, void *out);         // false when closed and drained
bool   wchan_try_send(WChan *c, const void *v);
size_t wchan_elem(WChan *c);
// The typed forms behind chan_send()/chan_recv(): same behaviour, but a size
// mismatch is a named panic instead of reading a value out of another type's
// bytes (which is how a Chan(str) misuse would otherwise present).
bool   wchan_send_typed(WChan *c, const void *v, size_t elem);
bool   wchan_recv_typed(WChan *c, void *out, size_t elem);
bool   wchan_try_send_typed(WChan *c, const void *v, size_t elem);
bool   wchan_try_recv_typed(WChan *c, void *out, size_t elem);
bool   wchan_try_recv(WChan *c, void *out);
void   wchan_close(WChan *c);
bool   wchan_closed(WChan *c);
i32    wchan_len(WChan *c);
i32    wchan_cap(WChan *c);

#define WEBC_CHAN_DEFINE(Name, prefix, T_)                                     \
  typedef struct Name { WChan *c; } Name;                                      \
  [[maybe_unused]] static inline Name prefix##_new(i32 cap_) {                  \
    return (Name){ .c = wchan_new(sizeof(T_), cap_) }; }                        \
  [[maybe_unused]] static inline bool prefix##_send(Name ch_, T_ v_) {          \
    return wchan_send(ch_.c, &v_); }                                            \
  [[maybe_unused]] static inline bool prefix##_recv(Name ch_, T_ *out_) {       \
    return wchan_recv(ch_.c, out_); }                                           \
  [[maybe_unused]] static inline bool prefix##_try_recv(Name ch_, T_ *out_) {   \
    return wchan_try_recv(ch_.c, out_); }                                       \
  [[maybe_unused]] static inline void prefix##_close(Name ch_) { wchan_close(ch_.c); } \
  [[maybe_unused]] static inline i32 prefix##_len(Name ch_) { return wchan_len(ch_.c); }

WEBC_CHAN_DEFINE(ChanInt, chan_int, i32)
WEBC_CHAN_DEFINE(ChanI64, chan_i64, i64)
WEBC_CHAN_DEFINE(ChanStr, chan_str, str)
WEBC_CHAN_DEFINE(ChanPtr, chan_ptr, void *)

// select(ops, n) { case 0: … break; case 1: … break; }
// The result is the index of the ready case, SELECT_TIMEOUT, or SELECT_CLOSED.
#define SELECT_TIMEOUT (-1)
#define SELECT_CLOSED  (-2)

typedef struct SelCase {
  WChan *ch;
  bool is_send;
  const void *val;     // send value
  void *out;           // receive destination
  i32 ready;           // filled by select_wait
  void *waiter;        // internal
} SelCase;

#define SEL_RECV(ch_, out_) ((SelCase){ .ch = (ch_), .is_send = false, .out = (out_) })
#define SEL_SEND(ch_, v_)   ((SelCase){ .ch = (ch_), .is_send = true, .val = (v_) })
#define SEL_DEFAULT         ((SelCase){ .ch = nullptr })

i32 select_wait(SelCase *cases, i32 n, i64 timeout_ms);
#ifndef WEBC_NO_SELECT_MACRO
// `select` is a statement macro, so include <webc/webc.h> after the system
// headers (or define WEBC_NO_SELECT_MACRO and call select_wait directly).
#define select(ops_, n_) switch (select_wait((ops_), (n_), -1))
#define select_timeout(ops_, n_, ms_) switch (select_wait((ops_), (n_), (ms_)))
#endif

// ---------------------------------------------------------------- sync

// A mutex remembers *which task* holds it, by id. Task ids are monotonic and never
// reused, so "is the owner still alive" is answerable after the owner's stack is
// gone — which is what makes a panic that escaped a handler recoverable instead of
// turning one mutex into a permanent hang for every task that wants it.
typedef struct WMutex {
  _Atomic int locked;
  _Atomic u64 owner_id;
  void *wait_head;
  void *wait_tail;
  struct WMutex *held_next; // the owning task's list of the mutexes it holds
} WMutex;
void wmutex_lock(WMutex *m);
bool wmutex_trylock(WMutex *m);
void wmutex_unlock(WMutex *m);

#define WEBC_MUTEX_INIT { 0, 0, nullptr, nullptr, nullptr }

typedef struct WRwLock { _Atomic int readers; _Atomic int writer; } WRwLock;
void wrwlock_rlock(WRwLock *l);
void wrwlock_runlock(WRwLock *l);
void wrwlock_lock(WRwLock *l);
void wrwlock_unlock(WRwLock *l);

typedef struct WOnce { _Atomic int state; WMutex mu; } WOnce;
#define WEBC_ONCE_INIT { 0, WEBC_MUTEX_INIT }
void wonce_do(WOnce *o, void (*fn)(void));

typedef struct WWaitGroup { _Atomic int n; WChan *done; WMutex mu; } WWaitGroup;
void wwaitgroup_init(WWaitGroup *wg);
void wwaitgroup_add(WWaitGroup *wg, i32 delta);
void wwaitgroup_done(WWaitGroup *wg);
void wwaitgroup_wait(WWaitGroup *wg);

#define atomic(T) _Atomic T

// The names SPEC §6.4 uses. The `W`-prefixed types are what the implementation
// calls them; these are the same types under the names a Go-shaped reader expects.
typedef WMutex Mutex;
typedef WRwLock RwLock;
typedef WOnce Once;
typedef WWaitGroup WaitGroup;

[[maybe_unused]] static inline void mutex_lock(Mutex *m) { wmutex_lock(m); }
[[maybe_unused]] static inline bool mutex_trylock(Mutex *m) { return wmutex_trylock(m); }
[[maybe_unused]] static inline void mutex_unlock(Mutex *m) { wmutex_unlock(m); }
[[maybe_unused]] static inline void rwlock_rlock(RwLock *l) { wrwlock_rlock(l); }
[[maybe_unused]] static inline void rwlock_runlock(RwLock *l) { wrwlock_runlock(l); }
[[maybe_unused]] static inline void rwlock_lock(RwLock *l) { wrwlock_lock(l); }
[[maybe_unused]] static inline void rwlock_unlock(RwLock *l) { wrwlock_unlock(l); }
[[maybe_unused]] static inline void once_do(Once *o, void (*fn)(void)) { wonce_do(o, fn); }
[[maybe_unused]] static inline void waitgroup_init(WaitGroup *w) { wwaitgroup_init(w); }
[[maybe_unused]] static inline void waitgroup_add(WaitGroup *w, i32 d) { wwaitgroup_add(w, d); }
[[maybe_unused]] static inline void waitgroup_done(WaitGroup *w) { wwaitgroup_done(w); }
[[maybe_unused]] static inline void waitgroup_wait(WaitGroup *w) { wwaitgroup_wait(w); }

// Typed channels with the spec's spelling (SPEC §6.4):
//
//   Chan(int) ch = chan_new(int, 16);
//   chan_send(ch, 42);  chan_recv(ch, &v);
//
// `Chan(T)` is an untyped handle, so `chan_send` copies the value through
// `__auto_type` and checks the element size against the channel's own — a wrong
// type is a named panic, not a silent reinterpretation.
#define Chan(T_) WChan *
#define chan_new(T_, cap_) wchan_new(sizeof(T_), (cap_))
#define chan_send(ch_, v_)                                                                         \
  ({                                                                                               \
    __auto_type _cv_ = (v_);                                                                       \
    wchan_send_typed((ch_), &_cv_, sizeof _cv_);                                                   \
  })
#define chan_recv(ch_, out_) wchan_recv_typed((ch_), (out_), sizeof *(out_))
#define chan_try_send(ch_, v_)                                                                     \
  ({                                                                                               \
    __auto_type _cv_ = (v_);                                                                       \
    wchan_try_send_typed((ch_), &_cv_, sizeof _cv_);                                               \
  })
#define chan_try_recv(ch_, out_) wchan_try_recv_typed((ch_), (out_), sizeof *(out_))
#define chan_close(ch_) wchan_close(ch_)
#define chan_len(ch_) wchan_len(ch_)

// ---------------------------------------------------------------- non-blocking I/O

void  wfd_nonblock(i32 fd);
i32   wfd_open_listen(const char *addr, u16 *port_out);   // "127.0.0.1:8080" / ":8080"
i32   wfd_open_connect(const char *host, u16 port, i64 timeout_ms); // the client side: -1 on failure
i64   wfd_read(i32 fd, void *buf, size_t n);              // -1 error, 0 EOF
i64   wfd_write(i32 fd, const void *buf, size_t n);
bool  wfd_accept(i32 lfd, i32 *out);
// Wake every task parked on `fd`. Call it before closing an fd another task may be
// waiting on: the kernel drops the kqueue registration silently, so the waiter would
// otherwise never be woken — and `wtask_shutdown()` waits for it forever.
void  wtask_wake_fd(i32 fd);
bool  wfd_wait(i32 fd, bool for_write, i64 timeout_ms);
void  wfd_close(i32 fd);
void  wfd_set_deadline(i32 fd, i64 deadline_ms);          // 0 clears

#endif // WEBC_TASK_H