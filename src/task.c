// SPDX-License-Identifier: Apache-2.0
// WebC — M:N scheduler: N OS threads, one run queue each, work stealing,
// kqueue-driven netpoll, timer heap, channel parking.
//
// A task is a ucontext with its own mmap'd stack (guard page at the bottom).
// Blocking operations park the task and hand the OS thread to another task; the
// OS thread is never blocked on behalf of a task except for real file I/O.
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#define WEBC_NO_SELECT_MACRO 1
// system headers first: the `select` DSL macro would otherwise shadow select(2)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#if !defined(__linux__)
#include <sys/event.h>
#endif
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <execinfo.h>

#include <webc/log.h>
#include <webc/task.h>

// The same three registers, spelled four ways: Darwin/arm64 calls them __pc/__lr/__sp,
// Darwin/x86_64 has no link register and calls the program counter __rip, and glibc uses
// uc_mcontext.pc / .gregs[REG_RIP]. The `defined(__APPLE__)` write-up that came out of the
// Linux port covered only Apple silicon, so an Intel Mac failed to build task.c at all —
// found by cross-compiling every source for x86_64-apple-darwin after the release dry run
// failed on a macos-15-intel runner.
#if defined(__APPLE__) && defined(__aarch64__)
#define WC_UCTX_PC(u) ((uintptr_t)(u)->uc_mcontext->__ss.__pc)
#define WC_UCTX_LR(u) ((uintptr_t)(u)->uc_mcontext->__ss.__lr)
#define WC_UCTX_SP(u) ((uintptr_t)(u)->uc_mcontext->__ss.__sp)
#elif defined(__APPLE__) && defined(__x86_64__)
#define WC_UCTX_PC(u) ((uintptr_t)(u)->uc_mcontext->__ss.__rip)
#define WC_UCTX_LR(u) ((uintptr_t)0) // no link register: the return address is on the stack
#define WC_UCTX_SP(u) ((uintptr_t)(u)->uc_mcontext->__ss.__rsp)
#elif defined(__aarch64__)
#define WC_UCTX_PC(u) ((uintptr_t)(u)->uc_mcontext.pc)
#define WC_UCTX_LR(u) ((uintptr_t)(u)->uc_mcontext.regs[30])
#define WC_UCTX_SP(u) ((uintptr_t)(u)->uc_mcontext.sp)
#elif defined(__x86_64__)
// x86_64 has no link register: glibc's makecontext() puts the entry point in RIP and
// the return address lives on the stack, so LR is 0 and the sanity check in g_start()
// reduces to "the program counter must be set", which is exactly right there.
#define WC_UCTX_PC(u) ((uintptr_t)(u)->uc_mcontext.gregs[REG_RIP])
#define WC_UCTX_LR(u) ((uintptr_t)0)
#define WC_UCTX_SP(u) ((uintptr_t)(u)->uc_mcontext.gregs[REG_RSP])
#endif

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#if !defined(__APPLE__) && !defined(MADV_FREE_REUSABLE)
#define MADV_FREE_REUSABLE MADV_FREE // Linux's nearest equivalent
#endif
#define WC_STACK_SIZE_DEF (256u * 1024u)
#define WC_TASK_MAGIC 0x57436757u // "WCgW": a task used after free is a named failure
// Longest a socket wait may sleep before re-checking, even with no timeout.
#define WC_FD_PARK_MAX_MS 30000
// The accept wait is the one park that must be short (see wfd_accept): it is also the
// wait a shutdown has to break, and no timer can break it for us if the listener's fd is
// closed underneath it.
#define WC_ACCEPT_PARK_MAX_MS 500
#define WC_STACK_CACHE 64 // idle stacks kept for reuse (not munmap'd)
#define WC_RQ_CAP 4096
#define WC_MAX_P 64

// ---------------------------------------------------------------- types

typedef struct WaitNode WaitNode;

struct WaitNode {
  Wg *g;
  WChan *ch;
  bool is_send;
  const void *val;
  void *out;
  bool ready;
  i32 index;
  WaitNode *next;
};

struct Wg {
  ucontext_t ctx;
  void *stack;
  size_t stack_size;
  size_t stack_mapped;
  WgFn fn;
  void *arg;
  WgState state;
  u64 id;
  Wp *p;
  Wg *next;
  const char *name;
  // timers
  i64 deadline;
  i32 timer_slot;
  // netpoll
  i32 poll_fd;
  // Set by wtask_wake_fd: the fd this task was parked on is being closed, so its wait must
  // end with "false" rather than retry — accept() on a shut-down listener returns EAGAIN,
  // which would park it again on a dead fd and look exactly like the bug this field fixes.
  bool poll_closed;
  bool poll_ready;     // readiness that arrived before the task parked
  // Which worker's kqueue holds this task's registration. Normally the worker running
  // it, but a ready task can be stolen before it resumes — and then the registration
  // has to be removed from the poller that owns it, not from the one it landed on.
  Wp *poll_owner;
  _Atomic bool on_cpu; // a task may run on exactly one thread at a time
  u32 magic;           // WC_TASK_MAGIC while this task is alive
  // select
  WaitNode *sel;
  i32 nsel;
  i32 sel_result;
  bool sel_done;
  // join handle
  struct WTaskHandle *handle;
  void *local;        // task-local storage (the current request)
  void *build;        // task-local: the markup build in progress
  void *rt;           // task-local: the reactive runtime (src/react.c)
  void *db;           // task-local: the database session (src/db.c)
  void *aux;          // task-local: the framework's own slot (src/qcache.c)
  Arena *arena;       // request arena, follows the task across workers
  void *panic_frame;  // wc_try_begin() frame: on the task's stack, so on the task
  WUnwind unwind;     // the defer()/errdefer() chain and the try() counter
  struct WMutex *held; // mutexes this task holds, released if it dies
  struct Wg *mu_next; // WMutex waiter list
  bool mu_granted;    // unlock() handed the mutex over
  // every live task, so a stall can be reported in human terms
  struct Wg *all_prev, *all_next;
  bool queued; // on a run queue right now (checked in debug builds)
#ifdef WC_TRACE_PARK
  int mu_step; // where in wmutex_lock this task is, for a stall dump
#endif
};

static Wg *g_all_head;

// Scheduler counters (/debug/sched), one cache line per worker. Eight globals incremented on
// every scheduler lock put the counters on the same line as the lock and made every
// acquisition a cross-core write: the instrumentation adds the traffic it exists to measure.
// Aggregated by wtask_stats().
typedef struct {
  _Atomic long iter, idle, poll, wake, steal, lock, unlock, ready;
} __attribute__((aligned(64))) WStat;
#define WC_MAX_PROC 256
static WStat g_st[WC_MAX_PROC];
static _Thread_local i32 t_wid = -1;
static _Thread_local Wp *t_p;


static void all_add_locked(Wg *g) {
  g->all_prev = nullptr;
  g->all_next = g_all_head;
  if (g_all_head) g_all_head->all_prev = g;
  g_all_head = g;
}

static void all_del_locked(Wg *g) {
  if (g->all_prev)
    g->all_prev->all_next = g->all_next;
  else if (g_all_head == g)
    g_all_head = g->all_next;
  if (g->all_next) g->all_next->all_prev = g->all_prev;
  g->all_prev = g->all_next = nullptr;
}

// The epoll backend is the Linux one. It is defined here rather than on a command
// line so that a hand-written `cc -std=c23 -I include` — the house rule's whole build
// for an app — keeps working after libwebc.a is built. WC_NO_EPOLL_NETPOLL opts out
// (and then needs libkqueue, because the kqueue backend wants <sys/event.h>).
#if defined(__linux__) && !defined(WC_EPOLL_NETPOLL) && !defined(WC_NO_EPOLL_NETPOLL)
#define WC_EPOLL_NETPOLL 1
#endif

#if defined(__linux__) && defined(WC_EPOLL_NETPOLL)
// Native readiness backend. The framework's scheduler needs exactly six things from a
// poller, and epoll gives them directly: translating kqueue onto Linux costs ~22 of the
// ~39 syscalls a rendered request makes (strace: 16.4 fcntl + 5.0 fstat per request), which
// is most of the reason a request costs 10x more kernel time at four workers than at one —
// measured: system time 0.033 -> 0.319 ms/request, user time 0.515 -> 0.743.
#define WC_EPOLL_BACKEND 1
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif

struct Wp {
  i32 id;
  pthread_t th;
  Wg *rq[WC_RQ_CAP];
  i32 head, n;
  ucontext_t sched;
  Wg *cur;
  bool idle;
  bool started;
  bool stop;
  const char *where; // what this processor is doing, for a stall dump
  // One kqueue per worker. With a single process-wide one, only the worker holding its
  // mutex could sit in kevent(): every other worker waited by taking the *scheduler*
  // lock. Measured cost: /health fell from 43 731 req/s on one worker to 14 812 on ten,
  // and a request cost 4.25 kernel context switches.
  i32 poll_fd;
  // Only a worker that is actually blocked in its own kqueue needs a wakeup.
  _Atomic bool blocked;
  // epoll backend only: the eventfd this worker's poller watches as its wake source.
  i32 wake_fd;
} __attribute__((aligned(64)));

struct WChan {
  size_t elem;
  i32 cap, n, head, tail;
  char *buf;
  bool closed;
  WaitNode *sendq, *sendq_tail;
  WaitNode *recvq, *recvq_tail;
};

struct WTaskHandle {
  WChan *done;
  TaskResult res;
  WgFn fn;
  void *arg;
};

// ---------------------------------------------------------------- globals

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;


#ifdef WC_LOCK_HIST
// Which g_lock() call site is hot. A single mutex that a request takes ~50 times is not
// a mystery to be reasoned about — it is a number to be read.
#define WC_LINE_SLOTS 4096
static _Atomic long c_line[WC_LINE_SLOTS];
// Count is not cost: a lock taken 8 times a request costs nothing if nobody waits for it, and
// one taken once costs everything if the holder is preempted. These time each acquisition:
//   wait = time spent *waiting* for the lock (contention, the thing that burns cores)
//   hold = time the lock was held (the thing that makes others wait)
static _Atomic long c_wn[WC_LINE_SLOTS], c_wmax[WC_LINE_SLOTS];
static _Atomic long c_hn[WC_LINE_SLOTS], c_hmax[WC_LINE_SLOTS];
static _Thread_local i64 t_hold_t = -1;
static _Thread_local i32 t_hold_line = -1;
static inline i64 lh_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (i64)ts.tv_sec * 1000000000LL + (i64)ts.tv_nsec;
}
static inline void lh_max(_Atomic long *slot, long v) {
  long cur = atomic_load(slot);
  while (v > cur && !atomic_compare_exchange_weak(slot, &cur, v)) {}
}
void lock_time_top(i32 *lines, long *counts, long *wait_ns, long *wait_max, long *hold_ns,
                   long *hold_max, i32 want, i32 *n) {
  *n = 0;
  for (i32 k = 0; k < want; k++) {
    long best = -1;
    i32 besti = -1;
    for (i32 i = 0; i < WC_LINE_SLOTS; i++) {
      long v = atomic_load(&c_wn[i]);
      if (v <= 0) continue;
      bool taken = false;
      for (i32 j = 0; j < *n; j++)
        if (lines[j] == i) taken = true;
      if (taken) continue;
      if (v > best) { best = v; besti = i; }
    }
    if (besti < 0) break;
    lines[*n] = besti;
    counts[*n] = atomic_load(&c_line[besti]);
    wait_ns[*n] = atomic_load(&c_wn[besti]);
    wait_max[*n] = atomic_load(&c_wmax[besti]);
    hold_ns[*n] = atomic_load(&c_hn[besti]);
    hold_max[*n] = atomic_load(&c_hmax[besti]);
    (*n)++;
  }
}
// wmutex_lock/unlock are two lines in this file, so the histogram above cannot say which
// *caller* is spending six lock pairs per request. Return addresses, relative to a symbol,
// stay valid across PIE relocation; map them afterwards with llvm-addr2line.
static _Atomic long c_ra_n[128];
static _Atomic uintptr_t c_ra[128];
static void ra_note(void) {
  uintptr_t rel = (uintptr_t)__builtin_return_address(1) - (uintptr_t)(void *)&wtask_init;
  i32 h = (i32)((rel >> 2) & 127);
  for (i32 k = 0; k < 8; k++) {
    i32 j = (h + k) & 127;
    uintptr_t cur = atomic_load(&c_ra[j]);
    if (cur == 0 || cur == rel) {
      atomic_store(&c_ra[j], rel);
      atomic_fetch_add(&c_ra_n[j], 1);
      return;
    }
  }
}
void ra_hist_top(uintptr_t *ras, long *counts, i32 max, i32 *n) {
  *n = 0;
  for (i32 k = 0; k < max; k++) {
    long best = 0;
    i32 besti = -1;
    for (i32 i = 0; i < 128; i++) {
      long v = atomic_load(&c_ra_n[i]);
      if (v <= 0) continue;
      bool taken = false;
      for (i32 j = 0; j < *n; j++)
        if (ras[j] == atomic_load(&c_ra[i])) taken = true;
      if (taken) continue;
      if (v > best) { best = v; besti = i; }
    }
    if (besti < 0) break;
    ras[*n] = atomic_load(&c_ra[besti]);
    counts[*n] = best;
    (*n)++;
  }
}
static void lock_hist_dump(void) __attribute__((unused));
static void lock_hist_dump(void) {
  for (i32 k = 0; k < 15; k++) {
    long best = 0;
    i32 besti = -1;
    for (i32 i = 0; i < WC_LINE_SLOTS; i++) {
      long v = atomic_load(&c_line[i]);
      if (v > best) { best = v; besti = i; }
    }
    if (besti < 0 || best == 0) break;
    fprintf(stderr, "LOCKHIST line %d: %ld\n", besti, best);
    atomic_store(&c_line[besti], 0);
  }
}
static _Atomic int hist_armed;

// Non-destructive: the top lock call sites, for /debug/locks. Same table as the exit dump,
// but without zeroing it, because a route can be called more than once.
void lock_hist_top(long *lines, long *counts, i32 max, i32 *n) {
  *n = 0;
  for (i32 k = 0; k < max; k++) {
    long best = 0;
    i32 besti = -1;
    for (i32 i = 0; i < WC_LINE_SLOTS; i++) {
      long v = atomic_load(&c_line[i]);
      if (v <= 0) continue;
      bool taken = false;
      for (i32 j = 0; j < *n; j++)
        if (lines[j] == i) taken = true;
      if (taken) continue;
      if (v > best) { best = v; besti = i; }
    }
    if (besti < 0) break;
    lines[*n] = besti;
    counts[*n] = best;
    (*n)++;
  }
}
#endif
// Counting futex syscalls from outside says how much sleeping there is; this says whether
// the scheduler lock is the reason, without any timing at all — a trylock that fails is
// the definition of "somebody else has it". Always built: two relaxed increments.
static _Atomic long c_gmu_fast, c_gmu_slow;
static _Atomic long c_park_fd_rd, c_park_fd_wr; // socket waits, by direction
#ifdef WC_TRACE_PARK
// Who holds the scheduler lock, and from which line: a stuck g_mu is otherwise invisible in
// a stall dump — a task parked with the lock handed over, and the scheduler never releasing
// it, reads as an idle process.
static _Atomic int g_mu_line;
static _Atomic uintptr_t g_mu_th;
static _Atomic long g_ho_set, g_ho_true, g_ho_false;
static _Atomic long g_lock_n, g_unlock_n;
static void g_lock(int line) {
  pthread_mutex_lock(&g_mu);
  atomic_fetch_add(&g_lock_n, 1);
  atomic_store(&g_mu_line, line);
  atomic_store(&g_mu_th, (uintptr_t)pthread_self());
}
static void g_unlock(int line) {
  uintptr_t th = (uintptr_t)pthread_self();
  uintptr_t owner = atomic_load(&g_mu_th);
  if (owner != th) {
    fprintf(stderr, "SCHED: unlock of g_mu from %p but owner is %p (line %d)\n", (void *)th,
            (void *)owner, line);
    abort();
  }
  atomic_store(&g_mu_th, 0);
  atomic_store(&g_mu_line, 0);
  pthread_mutex_unlock(&g_mu);
  atomic_fetch_add(&g_unlock_n, 1);
}
// a condvar releases the mutex inside the wait: ownership bookkeeping has to
// step aside for that, or every other thread's lock looks like a violation
static void g_cond_release(void) {
  atomic_store(&g_mu_th, 0);
}
static void g_cond_reacquire(void) {
  atomic_store(&g_mu_th, (uintptr_t)pthread_self());
}
#else
// Contended acquisitions of g_mu. Counting futex syscalls from outside says how much
// sleeping there is; this says whether the scheduler lock is the reason, without any
// timing at all — a trylock that fails is the definition of "somebody else has it".
static inline void g_lock(int line) {
  if (t_wid >= 0) atomic_fetch_add(&g_st[t_wid].lock, 1);
#ifdef WC_LOCK_HIST
  i32 slot = line & (WC_LINE_SLOTS - 1);
  atomic_fetch_add(&c_line[slot], 1);
  i64 w0 = lh_now();
#endif
  if (pthread_mutex_trylock(&g_mu) != 0) {
    atomic_fetch_add_explicit(&c_gmu_slow, 1, memory_order_relaxed);
    pthread_mutex_lock(&g_mu);
  } else {
    atomic_fetch_add_explicit(&c_gmu_fast, 1, memory_order_relaxed);
  }
#ifdef WC_LOCK_HIST
  i64 w1 = lh_now();
  atomic_fetch_add(&c_wn[slot], (long)(w1 - w0));
  lh_max(&c_wmax[slot], (long)(w1 - w0));
  t_hold_t = w1;
  t_hold_line = slot;
#endif
}
static inline void g_unlock(int line) {
  (void)line;
  if (t_wid >= 0) atomic_fetch_add(&g_st[t_wid].unlock, 1);
#ifdef WC_LOCK_HIST
  // Only a matching acquisition has a believable hold time: a parked task hands the lock to
  // its scheduler thread, so the thread that unlocks need not be the one that locked.
  if (t_hold_line >= 0 && t_hold_line == (line & (WC_LINE_SLOTS - 1))) {
    i64 u1 = lh_now();
    atomic_fetch_add(&c_hn[t_hold_line], (long)(u1 - t_hold_t));
    lh_max(&c_hmax[t_hold_line], (long)(u1 - t_hold_t));
  }
  t_hold_line = -1;
#endif
  pthread_mutex_unlock(&g_mu);
}
static inline void g_cond_release(void) {}
static inline void g_cond_reacquire(void) {}
#endif

void gmu_contention(long *fast, long *slow) {
  *fast = atomic_load(&c_gmu_fast);
  *slow = atomic_load(&c_gmu_slow);
}

void fd_park_counts(long *rd, long *wr) {
  *rd = atomic_load(&c_park_fd_rd);
  *wr = atomic_load(&c_park_fd_wr);
}
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

static Wp *g_ps;
static i32 g_np;
static _Atomic i32 g_started;
static _Atomic i32 g_live;
// Run on the task's own stack as it ends, with whatever task-local state it still holds.
// The database layer registers it to roll back a transaction a panicking handler left open.
static void (*g_task_exit_hook)(void *);
static _Atomic u64 g_next_id;
static _Thread_local Wg *t_g;
static _Thread_local void *t_local_outside;
static _Thread_local void *t_build_outside;
static _Thread_local void *t_db_outside;
static _Thread_local void *t_aux_outside;
static _Thread_local void *t_panic_outside;

// ---------------------------------------------------- thread-local, carefully
//
// swapcontext() preserves callee-saved registers, so the compiler may keep the
// *address* of a thread-local in one of them across a switch. A task that
// resumes on a different worker thread would then read and write the previous
// thread's slot. Measured symptom: a task waking up on worker 0 while its slot
// said worker 2, seeing `t_g == nullptr` and dereferencing it a few bytes later
// (a fault at 0x3c8 inside wmutex_lock), or a scheduler that never released
// g_mu. Every access from code that may switch therefore goes through these
// out-of-line helpers, which re-derive the address on the thread that is
// actually running. Rule for the whole framework: a thread-local may be read
// before a blocking call, never across one — per-task state lives in the task
// (wtask_set_local).
__attribute__((noinline)) static Wg *cur_task(void) {
  __asm__ volatile("" ::: "memory");
  return t_g;
}
__attribute__((noinline)) static Wp *cur_proc(void) {
  __asm__ volatile("" ::: "memory");
  return t_p;
}
#ifdef WC_TRACE_PARK
#define MU_STEP(n) ((void)(cur_task()->mu_step = (n)))
#else
#define MU_STEP(n) ((void)0)
#endif

__attribute__((noinline)) static void cur_task_set(Wg *g) {
  __asm__ volatile("" ::: "memory");
  t_g = g;
}

// Internal scheduler assertions. They are the right idea at the wrong price: an
// atomic exchange, a pc/sp range check and a global mutex ran on every context switch
// and every spawn in production builds. WEBC_DEBUG_SCHED=1 turns them back on (as does
// WC_TRACE_PARK, which is a debugging build anyway).
static bool sched_debug(void) {
#ifdef WC_TRACE_PARK
  return true;
#endif
  static _Atomic int on = -1;
  int v = atomic_load(&on);
  if (v < 0) {
    const char *e = getenv("WEBC_DEBUG_SCHED");
    v = e && *e && strcmp(e, "0") != 0;
    atomic_store(&on, v);
  }
  return v;
}

// The poller lives in struct Wp now. This thread-local covers the one remaining case:
// a task parking from a thread that is not a worker.
static _Thread_local i32 t_poll_fd = -1;

// The wake trigger's ident: one EVFILT_USER per worker kqueue.
#define POLL_WAKE_ID 1u

// Wake a worker parked in its own poller (defined with the netpoll backend below).
static void poll_wake(Wp *w);

static bool g_shutdown;
static size_t g_stack_size = WC_STACK_SIZE_DEF;
static void *g_stack_cache[WC_STACK_CACHE];
static i32 g_ncache;

// Which task owns each stack right now. A stack handed to two tasks corrupts
// both and shows up much later as a wild jump, so the check is always on: a
// linear scan over a few dozen entries costs nothing next to a context switch.
typedef struct {
  void *stack;
  u64 owner;
} StackOwner;
// Heap-grown: the fixed table silently stopped recording past 192 stacks, i.e. the
// check disappeared exactly when there was enough concurrency for the bug it hunts.
static StackOwner *g_stack_owner;
static i32 g_nowner, g_capowner;

// WEBC_TRACE_STACKS=1 narrates every stack hand-off and task free; read once,
// because getenv() on every task exit is a measurable cost under load.
static bool trace_stacks(void) {
  static _Atomic int on = -1;
  int v = atomic_load(&on);
  if (v < 0) {
    v = getenv("WEBC_TRACE_STACKS") ? 1 : 0;
    atomic_store(&on, v);
  }
  return v != 0;
}

// WEBC_NO_STACK_CACHE=1 makes every task map and unmap its own stack, which
// costs ~20 us per task but removes the cache from the picture when a stack
// bug is suspected.
static bool stack_cache_off(void) {
  static _Atomic int off = -1;
  int v = atomic_load(&off);
  if (v < 0) {
    v = getenv("WEBC_NO_STACK_CACHE") ? 1 : 0;
    atomic_store(&off, v);
  }
  return v != 0;
}

static void stack_claim(void *stack, u64 owner_id) {
  if (!sched_debug()) return; // the double-handoff hunt costs a global lock per spawn
  if (trace_stacks())
    fprintf(stderr, "[stack] claim %p for %llu\n", stack, (unsigned long long)owner_id);
  pthread_mutex_lock(&g_mu);
  for (i32 i = 0; i < g_nowner; i++) {
    if (g_stack_owner[i].stack != stack) continue;
    fprintf(stderr, "SCHED: stack %p handed to task %llu while task %llu owns it (cache=%d)\n",
            stack, (unsigned long long)owner_id, (unsigned long long)g_stack_owner[i].owner,
            g_ncache);
    for (i32 k = 0; k < g_nowner; k++)
      fprintf(stderr, "  owner[%d] stack=%p task=%llu\n", k, g_stack_owner[k].stack,
              (unsigned long long)g_stack_owner[k].owner);
    void *bt[24];
    int nb = backtrace(bt, 24);
    backtrace_symbols_fd(bt, nb, 2);
    abort();
  }
  if (g_nowner == g_capowner) {
    i32 ncap = g_capowner ? g_capowner * 2 : 256;
    StackOwner *ns = wm_alloc((size_t)ncap * sizeof *ns);
    if (ns) {
      if (g_nowner) memcpy(ns, g_stack_owner, (size_t)g_nowner * sizeof *ns);
      wm_free(g_stack_owner);
      g_stack_owner = ns;
      g_capowner = ncap;
    }
  }
  if (g_nowner < g_capowner) g_stack_owner[g_nowner++] = (StackOwner){.stack = stack, .owner = owner_id};
  pthread_mutex_unlock(&g_mu);
}

static void stack_release(void *stack) {
  if (!sched_debug()) return;
  if (trace_stacks()) fprintf(stderr, "[stack] release %p\n", stack);
  pthread_mutex_lock(&g_mu);
  for (i32 i = 0; i < g_nowner; i++) {
    if (g_stack_owner[i].stack != stack) continue;
    g_stack_owner[i] = g_stack_owner[--g_nowner];
    break;
  }
  pthread_mutex_unlock(&g_mu);
}

// Stacks are the expensive part of a task: reuse them.
void wtask_set_stack_size(size_t bytes) {
  if (bytes < 32u * 1024u) bytes = 32u * 1024u;
  g_stack_size = (bytes + 4095u) & ~(size_t)4095u;
}
size_t wtask_stack_size(void) {
  return g_stack_size;
}

// timer min-heap
static Wg **g_timers;
static i32 g_ntimers, g_captimers;

static Wg *g_global_q; // overflow queue (linked through ->next)
static _Thread_local bool t_in_sched;

// ---------------------------------------------------------------- fwd decls

static void g_ready_locked(Wg *g);
static bool sched_next(Wp *p, Wg **out);
static void task_release_mutexes(Wg *g); // a dead task gives its locks back

// ---------------------------------------------------------------- Wg creation

static void all_add_locked(Wg *g);

static inline void task_check(Wg *g, const char *where) {
  if (!sched_debug()) return;
  if (g && g->magic != WC_TASK_MAGIC) {
    fprintf(stderr, "SCHED: %s touched a task that is not alive (magic=%08x)\n", where, g->magic);
    abort();
  }
}

static Wg *g_new(WgFn fn, void *arg, const char *name) {
  Wg *g = wm_alloc(sizeof *g);
  g->magic = WC_TASK_MAGIC;
  g->fn = fn;
  g->arg = arg;
  g->name = name ? name : "task";
  g->id = atomic_fetch_add(&g_next_id, 1) + 1;
  g->state = WG_READY;
  g->poll_fd = -1;
  g->poll_ready = false;
  g->queued = false;
  atomic_init(&g->on_cpu, false);
  g->panic_frame = nullptr;
  g->unwind.cleanup = nullptr;
  g->unwind.err_pending = 0;
  g->unwind.unwinding = false;
  g->timer_slot = -1;
  g->deadline = 0;
  g->mu_next = nullptr;
  g->mu_granted = false;
  g->held = nullptr;
  g->arena = nullptr;
  g->local = nullptr;

  // A plain 256 KiB mapping, no guard page and no canary: Darwin's ucontext
  // writes its own frames into the low part of the stack, so both tricks lie
  // about overflows (measured, docs/reference.md).
  g_lock(__LINE__);
  void *mem = stack_cache_off() ? nullptr : (g_ncache > 0 ? g_stack_cache[--g_ncache] : nullptr);
  g_unlock(__LINE__);
  if (!mem) {
    size_t want = g_stack_size;
    mem = mmap(nullptr, want, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) wc_panic(__FILE__, __LINE__, "cannot map task stack");
    // hand back only the pages actually used, so an idle stack costs nothing
    g->stack_mapped = want;
    madvise(mem, want, MADV_FREE_REUSABLE);
  } else {
    g->stack_mapped = g_stack_size;
  }
  g->stack = mem;
  g->stack_size = g_stack_size;
  stack_claim(mem, g->id);
  g->all_prev = g->all_next = nullptr;
  g_lock(__LINE__);
  all_add_locked(g);
  g_unlock(__LINE__);
  return g;
}

static void g_free(Wg *g) {
  if (trace_stacks())
    fprintf(stderr, "[stack] free task %llu stack=%p state=%d\n", (unsigned long long)g->id,
            g->stack, (int)g->state);
  g_lock(__LINE__);
  all_del_locked(g);
  g_unlock(__LINE__);
  // No overflow canary and no mprotect'd guard page. Darwin's makecontext and
  // swapcontext keep their own frames in the low part of a ucontext stack, so a
  // canary placed there fires on every task; and PROT_NONE on one page of an
  // anonymous mapping makes its neighbours SIGBUS (measured, docs/reference.md).
  // A blown stack therefore faults, and `wtask_dump()` names the live tasks.
  if (g->stack) {
    stack_release(g->stack);
    g_lock(__LINE__);
    if (g_ncache < WC_STACK_CACHE) {
      g_stack_cache[g_ncache++] = g->stack;
      g_unlock(__LINE__);
    } else {
      g_unlock(__LINE__);
      munmap(g->stack, g->stack_mapped);
    }
  }
  wm_free(g);
}

static void g_trampoline(void) {
  Wg *g = cur_task();
  // A panic is contained to the task that raised it: a background job, a resource
  // loader or a for_par() step that panics reports itself in the log and the
  // process keeps its other workers. Without a frame here, wc_panic() fell through
  // to abort() — and the crash reporter, walking a ucontext stack, left the process
  // running with a dead worker instead of dying.
  wc_try_frame frame;
  if (wc_try_begin(&frame)) {
    g->fn(g->arg);
    wc_try_end(&frame);
  } else {
    g->build = nullptr;             // a view that panicked mid-render left a build
    g->unwind.err_pending = 0;      // and an unwound try() counter
    log_error("task panic", {"task", str_lit(g->name)},
              {"message", str_lit(wc_panic_message())});
    if (g->handle) {
      g->handle->res.ok = false;
      g->handle->res.e = werr_c(WEBC_EEXTERNAL, "task panicked");
    }
  }
  task_release_mutexes(g);
  // Whatever the task still holds goes back before its stack does: an open database
  // session is rolled back and returned to the pool here, not left to a connection nobody
  // owns. Null in the common case (the dispatcher released it at the request boundary).
  {
    void *still = g->db;
    g->db = nullptr;
    if (still && g_task_exit_hook) g_task_exit_hook(still);
  }
  g->state = WG_DEAD;
  atomic_fetch_sub(&g_live, 1);
  if (g->handle) {
    WChan *done = g->handle->done;
    bool dummy = true;
    wchan_send(done, &dummy);
  }
  // never returns: hand the thread back to the scheduler
  swapcontext(&g->ctx, &g->p->sched);
  __builtin_unreachable();
}

static void g_start(Wg *g) {
  if (getcontext(&g->ctx) != 0) wc_panic(__FILE__, __LINE__, "getcontext failed");
  g->ctx.uc_stack.ss_sp = g->stack;
  g->ctx.uc_stack.ss_size = g->stack_size;
  g->ctx.uc_link = nullptr;
  makecontext(&g->ctx, g_trampoline, 0);
  // Darwin's makecontext() leaves a fresh context "returning" into _ctx_start
  // through the link register rather than setting pc, so a usable context is
  // one with either a program counter or a return address, on its own stack.
  uintptr_t pc = WC_UCTX_PC(&g->ctx);
  uintptr_t lr = WC_UCTX_LR(&g->ctx);
  uintptr_t sp = WC_UCTX_SP(&g->ctx);
  if ((pc == 0 && lr == 0) || sp < (uintptr_t)g->stack || sp > (uintptr_t)g->stack + g->stack_size)
    wc_panic(__FILE__, __LINE__, "task context not usable after makecontext: pc=%p lr=%p sp=%p",
             (void *)pc, (void *)lr, (void *)sp);
}

// ---------------------------------------------------------------- timer heap

static void timer_swap(i32 a, i32 b) {
  Wg *t = g_timers[a];
  g_timers[a] = g_timers[b];
  g_timers[b] = t;
  g_timers[a]->timer_slot = a;
  g_timers[b]->timer_slot = b;
}

static void timer_up(i32 i) {
  while (i > 0) {
    i32 parent = (i - 1) / 2;
    if (g_timers[parent]->deadline <= g_timers[i]->deadline) break;
    timer_swap(i, parent);
    i = parent;
  }
}

static void timer_down(i32 i) {
  for (;;) {
    i32 l = i * 2 + 1, r = l + 1, m = i;
    if (l < g_ntimers && g_timers[l]->deadline < g_timers[m]->deadline) m = l;
    if (r < g_ntimers && g_timers[r]->deadline < g_timers[m]->deadline) m = r;
    if (m == i) break;
    timer_swap(i, m);
    i = m;
  }
}

static void timer_add(Wg *g, i64 deadline) {
  if (g->timer_slot >= 0) return;
  if (g_ntimers == g_captimers) {
    i32 ncap = g_captimers ? g_captimers * 2 : 64;
    Wg **nt = wm_alloc((size_t)ncap * sizeof(Wg *));
    if (g_timers) memcpy(nt, g_timers, (size_t)g_ntimers * sizeof(Wg *));
    wm_free(g_timers);
    g_timers = nt;
    g_captimers = ncap;
  }
  g->deadline = deadline;
  g->timer_slot = g_ntimers;
  g_timers[g_ntimers++] = g;
  timer_up(g->timer_slot);
}

static void timer_del(Wg *g) {
  i32 i = g->timer_slot;
  if (i < 0) return;
  g->timer_slot = -1;
  g_ntimers--;
  if (i != g_ntimers) {
    g_timers[i] = g_timers[g_ntimers];
    g_timers[i]->timer_slot = i;
    timer_down(i);
    timer_up(i);
  }
}

static i64 now_ms(void) {
  return (i64)wc_time_ms();
}

// Fire expired timers, ready their tasks.
static void timers_run_locked(i64 now) {
  while (g_ntimers > 0 && g_timers[0]->deadline <= now) {
    Wg *g = g_timers[0];
    timer_del(g);
    if (g->state == WG_PARKED) {
      g->sel_result = SELECT_TIMEOUT;
      g_ready_locked(g);
    }
  }
}

// ---------------------------------------------------------------- run queues

static void rq_push_locked(Wp *p, Wg *g) {
  task_check(g, "enqueue");
  // A task on a run queue twice runs twice: the second resume happens after it
  // has died and its stack went back to the cache. Always check.
  if (g->queued) {
    fprintf(stderr, "SCHED: task %llu queued twice (state=%d)\n", (unsigned long long)g->id,
            (int)g->state);
    abort();
  }
  g->queued = true;
  g->state = WG_READY;
  if (p->n < WC_RQ_CAP) {
    p->rq[(p->head + p->n) % WC_RQ_CAP] = g;
    p->n++;
    return;
  }
  g->next = g_global_q;
  g_global_q = g;
}

static Wg *rq_pop_locked(Wp *p) {
  if (p->n == 0) return nullptr;
  Wg *g = p->rq[p->head];
  p->head = (p->head + 1) % WC_RQ_CAP;
  p->n--;
  g->next = nullptr;
  g->queued = false;
  return g;
}

static Wg *g_global_pop_locked(void) {
  Wg *g = g_global_q;
  if (g) g_global_q = g->next;
  if (g) g->queued = false;
  return g;
}

// Wake one worker, and only if it is actually blocked in its own kqueue. The trigger is
// one syscall on that worker's descriptor: no broadcast, no condvar, no herd. A worker
// that is running does not need it — it re-checks its own run queue before it sleeps.
// (Before: every ready task broadcast the condvar *and* triggered a kqueue shared by
// every worker; at ten workers that cost ~0.45 futex sleeps per request per extra worker.)
static void wake_worker(Wp *w) {
  if (!w || w->poll_fd < 0 || !atomic_load(&w->blocked)) return;
  atomic_fetch_add(&g_st[w->id].wake, 1);
  poll_wake(w);
}

// Wake every idle processor. Used when the reason to wake is not "one more task"
// (shutdown, a stall report, an fd being closed under its waiter).
static void wake_all_locked(void) {
  Wp *self = cur_proc();
  for (i32 i = 0; i < g_np; i++)
    if (&g_ps[i] != self) wake_worker(&g_ps[i]);
  // the caller's own thread may be waiting on the scheduler condvar (wtask_wait())
  pthread_cond_broadcast(&g_cv);
}

// One task became ready, so one processor is enough — the one that owns it.
static void wake_one_locked(Wg *g) {
  Wp *w = g->p ? g->p : &g_ps[0];
  if (w != cur_proc()) wake_worker(w);
}

// A parked task becomes ready again: put it on a run queue and wake its processor.
static void g_ready_locked(Wg *g) {
  task_check(g, "ready");
  if (g->state == WG_DEAD) return;
  Wp *p = g->p ? g->p : &g_ps[0];
  atomic_fetch_add(&g_st[p->id].ready, 1);
  rq_push_locked(p, g);
  wake_one_locked(g);
}

// A park or a yield must be atomic with respect to a waker on another thread:
// if we set the task READY and then swapped out without the lock, a second
// processor could resume a ucontext that is still running. So the parking thread
// hands g_mu to its own scheduler loop, which releases it immediately after the
// switch. (Measured: without this, 500 sleeping tasks on 8 workers segfault.)
static _Thread_local bool t_lock_handoff;

// The parking thread hands g_mu to its own scheduler loop; this is how the loop
// learns that it must release it, and like every other thread-local it has to be
// read on the thread that is actually running (see cur_task()).
__attribute__((noinline)) static void handoff_set(void) {
  __asm__ volatile("" ::: "memory");
#ifdef WC_TRACE_PARK
  if (t_lock_handoff) {
    fprintf(stderr, "SCHED: handoff set twice on %p\n", (void *)(uintptr_t)pthread_self());
    abort();
  }
  atomic_fetch_add(&g_ho_set, 1);
#endif
  t_lock_handoff = true;
}
__attribute__((noinline)) static bool handoff_take(void) {
  __asm__ volatile("" ::: "memory");
  bool v = t_lock_handoff;
#ifdef WC_TRACE_PARK
  atomic_fetch_add(v ? &g_ho_true : &g_ho_false, 1);
#endif
  t_lock_handoff = false;
  return v;
}

// The one way a task leaves the processor while g_mu is held: it has already
// been marked PARKED/READY under the lock, so no waker can see it as runnable
// before the switch actually happens. The scheduler releases g_mu for us.
static void g_switch_out_locked(void) {
  Wg *g = cur_task();
  Wp *p = cur_proc();
  if (!g || !p) wc_panic(__FILE__, __LINE__, "park outside a task");
  handoff_set();
  swapcontext(&g->ctx, &p->sched);
}

// Park with the lock already held: marks PARKED and switches atomically.

static void g_park_locked(i64 deadline_ms) {
  Wg *self = cur_task();
  self->state = WG_PARKED;
  if (deadline_ms > 0) timer_add(self, deadline_ms);
  g_switch_out_locked();
  // resumed, possibly on another worker thread: re-derive the slot
  self = cur_task();
#ifdef WC_TRACE_PARK
  if (!self) {
    fprintf(stderr, "SCHED: task resumed with no current task on %p\n",
            (void *)(uintptr_t)pthread_self());
    abort();
  }
#endif
  if (self->timer_slot >= 0) {
    g_lock(__LINE__);
    timer_del(self);
    g_unlock(__LINE__);
  }
}

static void g_park(i64 deadline_ms) {
  Wg *g = cur_task();
  Wp *p = cur_proc();
  (void)p;
  g_lock(__LINE__);
  g->state = WG_PARKED;
  if (deadline_ms > 0) timer_add(g, deadline_ms);
  handoff_set();
  swapcontext(&g->ctx, &p->sched);
  if (g->timer_slot >= 0) {
    g_lock(__LINE__);
    timer_del(g);
    g_unlock(__LINE__);
  }
}

void wg_yield(void) {
  Wg *g = cur_task();
  if (!g) {
    sched_yield();
    return;
  }
  Wp *p = g->p;
  g_lock(__LINE__);
  // put at the tail so a yield is a fair hand-off
  rq_push_locked(p, g);
  handoff_set();
  swapcontext(&g->ctx, &p->sched); // the scheduler releases g_mu
}

void wtask_stats(WTaskStats *out) {
  if (!out) return;
  *out = (WTaskStats){0};
  out->procs = g_np;
  for (i32 i = 0; i < g_np; i++) {
    Wp *p = &g_ps[i];
    out->iter += atomic_load(&g_st[p->id].iter);
    out->idle += atomic_load(&g_st[p->id].idle);
    out->poll += atomic_load(&g_st[p->id].poll);
    out->wake += atomic_load(&g_st[p->id].wake);
    out->steal += atomic_load(&g_st[p->id].steal);
    out->lock += atomic_load(&g_st[p->id].lock);
    out->unlock += atomic_load(&g_st[p->id].unlock);
    out->ready += atomic_load(&g_st[p->id].ready);
  }
}

void wc_sleep_ms(u64 ms) {
  wg_sleep_ms(ms);
}

void wg_sleep_ms(u64 ms) {
  if (!cur_task()) {
    struct timespec ts = {(time_t)(ms / 1000), (long)((ms % 1000) * 1000000)};
    nanosleep(&ts, nullptr);
    return;
  }
  g_park(now_ms() + (i64)ms);
}

// ---------------------------------------------------------------- netpoll


// A self-pipe in kqueue terms: EVFILT_USER + NOTE_TRIGGER wakes an idle
// processor that is blocked inside kevent(), which is where a 50 ms poll latency
// would come from.

static i32 poll_new(void) {
#ifdef WC_EPOLL_BACKEND
  i32 ep = epoll_create1(EPOLL_CLOEXEC);
  if (ep < 0) wc_panic(__FILE__, __LINE__, "epoll_create1() failed: %s", strerror(errno));
  return ep;
#else
  i32 kq = kqueue();
  if (kq < 0) wc_panic(__FILE__, __LINE__, "kqueue() failed: %s", strerror(errno));
  fcntl(kq, F_SETFD, FD_CLOEXEC);
  struct kevent ev;
  EV_SET(&ev, POLL_WAKE_ID, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  kevent(kq, &ev, 1, nullptr, 0, nullptr);
  return kq;
#endif
}

// A worker's own poller: created on the worker's thread, used only by it, so no lock is
// needed to be inside the wait.
static void poll_init(Wp *p) {
#ifdef WC_EPOLL_BACKEND
  if (p && p->poll_fd < 0) {
    p->poll_fd = poll_new();
    // One eventfd per worker, watched by that worker's own poller: this is EVFILT_USER +
    // NOTE_TRIGGER in epoll terms. A counter rather than a flag, so a wake that arrives
    // while one is pending is not lost.
    p->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (p->wake_fd < 0) wc_panic(__FILE__, __LINE__, "eventfd() failed: %s", strerror(errno));
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.u64 = 0; // 0 marks the wake source: a task pointer is never null
    if (epoll_ctl(p->poll_fd, EPOLL_CTL_ADD, p->wake_fd, &ev) < 0)
      wc_panic(__FILE__, __LINE__, "epoll_ctl(wake) failed: %s", strerror(errno));
  } else if (!p && t_poll_fd < 0) {
    // A thread that is not a worker never gets woken: it polls with a timeout only.
    t_poll_fd = poll_new();
  }
#else
  if (p && p->poll_fd < 0) p->poll_fd = poll_new();
  else if (!p && t_poll_fd < 0) t_poll_fd = poll_new();
#endif
}

// Wake a worker parked in its own poller.
static void poll_wake(Wp *w) {
#ifdef WC_EPOLL_BACKEND
  if (w->wake_fd < 0) return;
  u64 one = 1;
  (void)!write(w->wake_fd, &one, sizeof one); // EAGAIN means one is already pending: fine
#else
  struct kevent ev;
  EV_SET(&ev, POLL_WAKE_ID, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
  kevent(w->poll_fd, &ev, 1, nullptr, 0, nullptr);
#endif
}

// Park until `fd` is ready for read (or write). Returns false when the wait ended without
// readiness: a timeout, an error, or wtask_wake_fd() telling us the fd is being closed.
static void kevent_del(i32 kq, i32 fd, bool for_write);
static bool poll_add(i32 kq, i32 fd, bool for_write, void *udata);

static bool park_on_fd(i32 fd, bool for_write, i64 deadline_ms) {
  Wg *g = cur_task();
  atomic_fetch_add_explicit(for_write ? &c_park_fd_wr : &c_park_fd_rd, 1, memory_order_relaxed);
  Wp *wp = cur_proc();
  if (!g || !wp) {
    // No scheduler on this thread: fall back to select(). It answers readiness, never a
    // timeout — returning true on a timeout too makes "wait until this fd is readable" mean
    // "wait five seconds and say yes", so a server loop that keeps no tasks of its own takes
    // every timeout for a signal. The caller's deadline is honoured.
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    if (deadline_ms > 0) {
      i64 left = deadline_ms - now_ms();
      if (left < 0) left = 0;
      tv.tv_sec = (time_t)(left / 1000);
      tv.tv_usec = (suseconds_t)((left % 1000) * 1000);
    }
    int r;
    if (for_write)
      r = select(fd + 1, nullptr, &set, nullptr, &tv);
    else
      r = select(fd + 1, &set, nullptr, nullptr, &tv);
    if (r < 0 && errno == EINTR) return false;
    return r > 0;
  }
  poll_init(wp);
  // Register first, then park: readiness that is already pending (a peer that
  // closed, data that arrived) can be delivered by this worker between these
  // two steps, and it must not be thrown away — that lost wakeup leaked a
  // connection task per closed keep-alive connection (measured: webc_conns never
  // returned to 0).
  i32 kq = wp->poll_fd;
  g_lock(__LINE__);
  g->poll_fd = fd;
  g->poll_owner = wp;
  g->poll_ready = false;
  g_unlock(__LINE__);
  if (!poll_add(kq, fd, for_write, g)) {
    g_lock(__LINE__);
    g->poll_fd = -1;
    g_unlock(__LINE__);
    return false;
  }
  g_lock(__LINE__);
  if (g->poll_ready) { // it fired before we parked
    g->poll_ready = false;
    g->poll_fd = -1;
    g_unlock(__LINE__);
    kevent_del(kq, fd, for_write);
    return true;
  }
  g->state = WG_PARKED;
  if (deadline_ms > 0) timer_add(g, deadline_ms);
  g_switch_out_locked(); // the scheduler releases g_mu
  // Resumed: normally on the worker that owns the registration. A ready task can be
  // stolen before it runs, so remove the registration from the poller that has it.
  g = cur_task();
  bool closed = g->poll_closed;
  g->poll_closed = false;
  g->poll_fd = -1;
  g->poll_ready = false;
  if (g->timer_slot >= 0) {
    g_lock(__LINE__);
    timer_del(g);
    g_unlock(__LINE__);
  }
  kevent_del(g->poll_owner ? g->poll_owner->poll_fd : kq, fd, for_write);
  g->poll_owner = nullptr;
  return !closed;
}

// One registration for (fd, direction, task). Level-triggered on epoll on purpose: the
// caller reads or writes until EAGAIN and then parks, so a level trigger costs a spurious
// wakeup at worst, while an edge trigger would lose readiness the caller did not drain
// (this is the same reason the kqueue path registers EV_CLEAR and re-arms every park).
static bool poll_add(i32 kq, i32 fd, bool for_write, void *udata) {
#ifdef WC_EPOLL_BACKEND
  struct epoll_event ev = {0};
  ev.events = (for_write ? EPOLLOUT : EPOLLIN) | EPOLLRDHUP;
  ev.data.ptr = udata;
  if (epoll_ctl(kq, EPOLL_CTL_ADD, fd, &ev) == 0) return true;
  // Already registered (the same fd parked again before its delete landed): update it.
  if (errno == EEXIST) return epoll_ctl(kq, EPOLL_CTL_MOD, fd, &ev) == 0;
  return false;
#else
  struct kevent ev;
  EV_SET(&ev, (uintptr_t)fd, for_write ? EVFILT_WRITE : EVFILT_READ, EV_ADD | EV_CLEAR, (uint16_t)0,
         0, udata);
  return kevent(kq, &ev, 1, nullptr, 0, nullptr) == 0;
#endif
}

// Drop the registration; a closed fd must not leave a stale one behind. The poller is
// named explicitly: the owner of the registration is not always the worker the task
// happens to be running on.
static void kevent_del(i32 kq, i32 fd, bool for_write) {
  if (kq < 0) return;
#ifdef WC_EPOLL_BACKEND
  (void)for_write; // one epoll entry per fd covers both directions
  epoll_ctl(kq, EPOLL_CTL_DEL, fd, nullptr);
#else
  struct kevent del;
  EV_SET(&del, (uintptr_t)fd, for_write ? EVFILT_WRITE : EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  kevent(kq, &del, 1, nullptr, 0, nullptr);
#endif
}

// One readiness event, in terms both backends can express.
typedef struct {
  i32 fd;       // the fd the event is about; -1 when the backend does not name it
  void *udata;  // the task that registered it, or null for the wake source
} PollEv;

// Wait for readiness. timeout_ms of 0 must not block (an uninitialised timespec here once
// blocked a whole processor forever, which looks exactly like a hung task).
static int poll_wait(i32 kq, PollEv *out, int max, i64 timeout_ms) {
#ifdef WC_EPOLL_BACKEND
  struct epoll_event evs[64];
  if (max > 64) max = 64;
  int n = epoll_wait(kq, evs, max, (int)timeout_ms);
  for (int i = 0; i < n; i++) {
    Wg *g = (Wg *)evs[i].data.ptr;
    out[i].udata = evs[i].data.u64 ? (void *)g : nullptr;
    // epoll does not repeat the fd in the event; the task's own registration is the fd it
    // is parked on, and the caller re-checks that before treating it as ready.
    out[i].fd = g ? g->poll_fd : -1;
  }
  return n;
#else
  struct kevent evs[64];
  struct timespec ts;
  if (max > 64) max = 64;
  ts.tv_sec = (time_t)(timeout_ms / 1000);
  ts.tv_nsec = (long)((timeout_ms % 1000) * 1000000);
  int n = kevent(kq, nullptr, 0, evs, 64, &ts);
  for (int i = 0; i < n; i++) {
    out[i].fd = (i32)evs[i].ident;
    out[i].udata = evs[i].udata;
  }
  return n;
#endif
}

// Clear the wake source. Under kqueue EV_CLEAR already consumed it.
static void poll_wake_drain(Wp *p) {
#ifdef WC_EPOLL_BACKEND
  if (!p || p->wake_fd < 0) return;
  u64 v;
  (void)!read(p->wake_fd, &v, sizeof v);
#else
  (void)p;
#endif
}

// Called by a worker when it runs out of work: harvest the fds registered in its own
// poller. Returns NP_READY (an fd fired) or NP_EMPTY (nothing fired). There is no third
// answer: with one poller per worker there is nobody to lose a race to, so a `busy` state
// would only be a condvar wait that has to take the scheduler lock.
enum { NP_EMPTY = 0, NP_READY = 1 };

static i32 netpoll_drain(Wp *p, i64 timeout_ms) {
  if (!p || p->poll_fd < 0) return NP_EMPTY;
  if (timeout_ms > 0) atomic_fetch_add(&g_st[p->id].poll, 1);
  PollEv evs[64];
  int n = poll_wait(p->poll_fd, evs, 64, timeout_ms);
  i32 any = NP_EMPTY;
  if (n > 0) {
    g_lock(__LINE__);
    for (int i = 0; i < n; i++) {
      Wg *g = (Wg *)evs[i].udata;
      if (!g) { // the wake source, not a task
        poll_wake_drain(p);
        continue;
      }
      if (evs[i].fd >= 0 && g->poll_fd != evs[i].fd) continue;
      if (g->poll_fd < 0) continue; // not parked any more (epoll names no fd)
      if (g->state == WG_PARKED) {
        g_ready_locked(g);
      } else {
        // delivered between "register" and "park": hand it to the task instead
        // of dropping it
        g->poll_ready = true;
      }
      any = NP_READY;
    }
    g_unlock(__LINE__);
  }
  return any;
}

// ---------------------------------------------------------------- scheduler loop

// Any work anywhere? Called with g_mu held.
static bool work_pending_locked(Wp *self) {
  if (self->n > 0 || g_global_q) return true;
  for (i32 i = 0; i < g_np; i++)
    if (g_ps[i].n > 0) return true;
  return false;
}

static bool sched_next(Wp *p, Wg **out) {
  g_lock(__LINE__);
  timers_run_locked(now_ms());
  Wg *g = rq_pop_locked(p);
  if (!g) g = g_global_pop_locked();
  if (!g && g_np > 1) {
    // Work stealing. A victim with one task is worth robbing when we have none: that task
    // may be sitting on a processor that is asleep or slow. Without it every connection
    // stays on the worker that accepted it, which is worker 0 for all of them (measured:
    // 1 395 req/s at every worker count with stealing off, against 1 890 at four workers
    // with it on).
    for (i32 k = 1; k < g_np && !g; k++) {
      Wp *v = &g_ps[(p->id + k) % g_np];
      if (v->n == 0) continue;
      i32 steal = v->n >= 2 ? v->n / 2 : 1;
      atomic_fetch_add(&g_st[p->id].steal, steal);
      for (i32 i = 0; i < steal; i++) {
        Wg *sg = rq_pop_locked(v);
        if (sg) rq_push_locked(p, sg);
      }
      g = rq_pop_locked(p);
    }
  }
  g_unlock(__LINE__);
  if (g) {
    *out = g;
    return true;
  }
  return false;
}

static void *p_main(void *arg) {
  Wp *p = arg;
  t_p = p;
  t_wid = p->id;
  wc_crash_thread_init(); // a fault on this worker must not scribble on another's report
  poll_init(p);           // this worker's own kqueue (T-1)
  p->where = "start";
  t_in_sched = true;
  for (;;) {
    Wg *g = nullptr;
    atomic_fetch_add(&g_st[p->id].iter, 1);
    p->where = "schedule";
    if (sched_next(p, &g)) {
      p->where = "running a task";
      p->cur = g;
      cur_task_set(g);
      g->p = p;
      g->state = WG_RUNNING;
      task_check(g, "resume");
      if (sched_debug()) {
        // A resumed context must be a real one: its program counter inside a
        // mapped text page and its stack inside this task's own stack. A zeroed
        // context resumes at address 0 and is otherwise invisible until the
        // process dies somewhere unrelated (measured).
        uintptr_t pc = WC_UCTX_PC(&g->ctx);
        uintptr_t lr = WC_UCTX_LR(&g->ctx);
        uintptr_t sp = WC_UCTX_SP(&g->ctx);
        uintptr_t lo = (uintptr_t)g->stack, hi = lo + g->stack_size;
        if ((pc == 0 && lr == 0) || sp < lo || sp > hi) {
          fprintf(stderr,
                  "SCHED: bad context for task %llu %s: pc=%p lr=%p sp=%p stack=[%p,%p) "
                  "state=%d queued=%d on_cpu=%d\n",
                  (unsigned long long)g->id, g->name ? g->name : "task", (void *)pc, (void *)lr,
                  (void *)sp, (void *)lo, (void *)hi, (int)g->state, (int)g->queued,
                  (int)atomic_load(&g->on_cpu));
          abort();
        }
      }
      // Two threads resuming the same context corrupts both stacks; make it a
      // named failure instead of a wild jump three seconds later.
      if (sched_debug()) {
        if (atomic_exchange(&g->on_cpu, true)) {
          fprintf(stderr, "SCHED: task %llu resumed on two threads at once\n",
                  (unsigned long long)g->id);
          abort();
        }
      }
      Arena *idle_arena = wc_arena_swap(g->arena);
      swapcontext(&p->sched, &g->ctx);
      if (sched_debug()) atomic_store(&g->on_cpu, false);
      g->arena = wc_arena_swap(idle_arena);
      // the task may have handed g_mu to us, and may have moved our thread's
      // thread-locals: both are read through out-of-line helpers
      if (handoff_take()) g_unlock(__LINE__);
      cur_task_set(nullptr);
      p->cur = nullptr;
      // Exactly one thread frees a task, even if a stale queue entry ever
      // brought it back: claiming the state makes the free single-winner.
      g_lock(__LINE__);
      bool dead = g->state == WG_DEAD;
      if (dead) g->state = WG_RUNNING;
      g_unlock(__LINE__);
      if (dead) g_free(g);
      continue;
    }
    // no runnable task: harvest I/O, else sleep on the timer horizon
    atomic_fetch_add(&g_st[p->id].idle, 1);
    p->where = "idle";
    g_lock(__LINE__);
    if (g_shutdown && g_live == 0) {
      g_unlock(__LINE__);
      break;
    }
    if (work_pending_locked(p)) {
      // someone queued work between sched_next() and here: sleeping now would
      // lose the wakeup entirely
      g_unlock(__LINE__);
      continue;
    }
    i64 wait_ms;
    if (g_ntimers > 0) {
      wait_ms = g_timers[0]->deadline - now_ms();
      if (wait_ms < 0) wait_ms = 0;
    } else {
      // An idle server has nothing to do until an fd is ready, and the kqueue trigger
      // wakes a worker immediately when one is. 5 ms here meant 200 wakeups/s per
      // worker doing nothing (2 000/s on ten cores); 250 ms still re-checks timers
      // and shutdown four times a second.
      wait_ms = g_live > 0 ? 50 : 250;
    }
    p->idle = true;
    atomic_store(&p->blocked, true);
    g_unlock(__LINE__);
    p->where = "kevent";
    // Block in this worker's own kqueue for that horizon: an fd becoming ready is
    // handled at once instead of up to 50 ms later (measured: 40 ms -> 0.2 ms per
    // request). No lock is held across the wait, and nobody else can be waiting for
    // it — which is the whole of T-1.
    i32 polled = netpoll_drain(p, wait_ms);
    atomic_store(&p->blocked, false);
    if (polled == NP_EMPTY && wait_ms > 0 && g_ntimers > 0) {
      // nothing was ready: the deadline may have passed, so re-check timers
      g_lock(__LINE__);
      timers_run_locked(now_ms());
      g_unlock(__LINE__);
    }
    p->idle = false;
    p->where = "woke";
  }
  return nullptr;
}

void wtask_init(i32 threads) {
  wc_crash_init();
  if (atomic_exchange(&g_started, 1)) return;
#ifdef WC_LOCK_HIST
  if (!atomic_exchange(&hist_armed, 1)) atexit(lock_hist_dump);
#endif
  i32 n = threads > 0 ? threads : (i32)sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1) n = 1;
  if (n > WC_MAX_P) n = WC_MAX_P;
  g_np = n;
  g_ps = wm_alloc((size_t)n * sizeof(Wp));
  for (i32 i = 0; i < n; i++) {
    g_ps[i].id = i;
    g_ps[i].poll_fd = -1; // created on the worker's own thread
    g_ps[i].wake_fd = -1; // 0 would be a real descriptor
    g_ps[i].started = true;
    pthread_create(&g_ps[i].th, nullptr, p_main, &g_ps[i]);
  }
}

// Wait until every task has finished (the caller keeps its own thread).
void wtask_wait(void) {
  if (!atomic_load(&g_started)) return;
  for (;;) {
    g_lock(__LINE__);
    bool done = atomic_load(&g_live) == 0 && g_ntimers == 0 && g_global_q == nullptr;
    i64 wait_ms = g_ntimers > 0 ? g_timers[0]->deadline - now_ms() : 50;
    if (wait_ms < 0) wait_ms = 0;
    if (wait_ms == 0) wait_ms = 1;
    g_unlock(__LINE__);
    if (done) {
      if (netpoll_drain(nullptr, 0) != NP_READY) return;
      continue;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(wait_ms / 1000);
    ts.tv_nsec += (long)((wait_ms % 1000) * 1000000);
    if (ts.tv_nsec >= 1000000000) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000;
    }
    g_lock(__LINE__);
    g_cond_release();
    pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    g_cond_reacquire();
    g_unlock(__LINE__);
    netpoll_drain(nullptr, 0);
  }
}

// Stopping the scheduler joins the workers. A one-way stop — the flag goes up, the workers
// exit on their own, nobody joins them, and wtask_init() answers "already started" — makes a
// second init/shutdown cycle find no workers, and every task queued into a void (a test that
// ran four cycles hung on the third). So the caller's thread joins them, and the scheduler
// can start again.
void wtask_shutdown(void) {
  g_lock(__LINE__);
  bool was_running = atomic_load(&g_started) != 0;
  g_shutdown = true;
  // A task asleep on a timer is waiting for a reason that no longer holds — the process is
  // stopping. Waking it is the difference between an instant shutdown and a slow one: the
  // stall monitor's 1 s sleep alone held every SIGTERM for a second. Tasks parked on an fd
  // are left alone, because an fd belongs to a caller who has to close it and say so with
  // wtask_wake_fd() — waking them here would just park them again.
  for (Wg *g = g_all_head; g; g = g->all_next) {
    if (g->state != WG_PARKED || g->poll_fd >= 0) continue;
    if (g->timer_slot >= 0) timer_del(g);
    g_ready_locked(g);
  }
  wake_all_locked();
  g_unlock(__LINE__);
  if (!was_running) return;
  // Called from a worker (a task shutdown): the flag is all we can do — joining
  // ourselves would deadlock. The workers leave as they drain.
  if (cur_task() || t_in_sched) return;
  // Wait for them to notice. They check g_shutdown when they have no work, so this is
  // one idle horizon at worst.
  for (i32 i = 0; i < g_np; i++)
    if (g_ps[i].started) pthread_join(g_ps[i].th, nullptr);
  g_lock(__LINE__);
  for (i32 i = 0; i < g_np; i++) {
    g_ps[i].n = 0;
    g_ps[i].started = false;
  }
  g_global_q = nullptr;
  g_ntimers = 0;
  wm_free(g_ps);
  g_ps = nullptr;
  g_np = 0;
  g_shutdown = false;
  atomic_store(&g_started, 0);
  g_unlock(__LINE__);
}

// The OS threads the scheduler runs on, for the profiler (src/profile.c): it
// samples by pthread_kill(), and a signal goes to the thread you name.
// Is the OS thread reading this currently inside a task (rather than parked in the
// scheduler with nothing to do)? The profiler asks, so idle workers do not drown
// the profile: without it 64% of samples were workers waiting on the scheduler
// lock, which says nothing about where the time goes.
bool wtask_this_thread_busy(void) {
  Wp *p = cur_proc();
  if (!p) return true; // not one of our workers (an app's own thread): sample it
  return p->cur != nullptr;
}

i32 wtask_os_threads(void **out, i32 max) {
  i32 n = 0;
  for (i32 i = 0; i < g_np && n < max; i++)
    if (g_ps[i].started) out[n++] = (void *)(uintptr_t)g_ps[i].th;
  return n;
}

i32 wtask_threads(void) {
  return g_np;
}
i32 wtask_live(void) {
  return atomic_load(&g_live);
}
u64 wtask_spawned(void) {
  return atomic_load(&g_next_id);
}

Wg *wg_spawn(WgFn fn, void *arg, const char *name) {
  if (!atomic_load(&g_started)) wtask_init(0);
  Wg *g = g_new(fn, arg, name);
  g_start(g);
  atomic_fetch_add(&g_live, 1);
  g_lock(__LINE__);
  // Placement, not theft. A new task goes to the worker with the shortest run queue; it used
  // to go to whichever worker happened to call spawn, and since every connection is accepted
  // by one accept task, that put *every* connection on worker 0. The other workers then only
  // ever got work by stealing it, which is why the stealing code looked necessary: with
  // placement fixed and stealing off, per-request CPU is flat across worker counts
  // (0.71 ms at one worker, 0.79 ms at ten, where stealing made it 3.30 ms) — the migration
  // was the cost, and it was paying for a broken placement policy all along.
  // Placement is "the worker that spawned it", and the acceptor is one task, so every
  // connection starts on worker 0 and work stealing is what spreads them. Tried to fix that
  // at the source — round-robin placement plus no stealing — and measured it against this
  // version at 10 VUs, 4 workers: 1 520 req/s at 2.13 ms per request, against 1 890 at
  // 1.65. Pinning a connection to a worker it was spawned on costs more than moving it,
  // because the move is what rebalances, and an idle worker next to a busy one is worse
  // than a warm cache. Left as-is deliberately; the placement policy is the thing to fix
  // only together with T-2, when the lock stops being the constraint.
  Wp *p = cur_proc() ? cur_proc() : &g_ps[0];
  g->p = p;
  rq_push_locked(p, g);
  wake_worker(p);
  g_unlock(__LINE__);
  return g;
}

void task_async(WgFn fn, void *arg) {
  (void)wg_spawn(fn, arg, "async");
}

typedef struct JoinBox {
  WgFn fn;
  void *arg;
  WTaskHandle *h;
} JoinBox;

static void join_trampoline(void *arg) {
  JoinBox *b = arg;
  b->h->res.ok = true;
  b->fn(b->arg);
  wm_free(b);
}

WTaskHandle *task_result(WgFn fn, void *arg) {
  WTaskHandle *h = wm_alloc(sizeof *h);
  h->done = wchan_new(sizeof(bool), 1);
  h->fn = fn;
  h->arg = arg;
  h->res.ok = false;
  // the handle is completed by the trampoline in g_trampoline
  JoinBox *box = wm_alloc(sizeof *box);
  box->fn = fn;
  box->arg = arg;
  box->h = h;
  Wg *g = wg_spawn(join_trampoline, box, "result");
  g->handle = h;
  return h;
}

TaskResult task_join(WTaskHandle *h) {
  if (!h) return (TaskResult){.ok = false, .e = werr_c(WEBC_EINVAL, "nil handle")};
  bool dummy = false;
  wchan_recv(h->done, &dummy);
  return h->res;
}

i32 task_join_all(WTaskHandle **hs, i32 n) {
  i32 failed = 0;
  for (i32 i = 0; i < n; i++) {
    TaskResult r = task_join(hs[i]);
    if (!r.ok && !failed) failed = i + 1;
  }
  return failed;
}

void task_handle_free(WTaskHandle *h) {
  if (!h) return;
  wm_free(h->done->buf);
  wm_free(h->done);
  wm_free(h);
}

typedef struct ParChunk {
  i32 lo, hi;
  void (*step)(i32 index, void *ctx);
  void *ctx;
  WWaitGroup *wg;
} ParChunk;

static void par_chunk_done(WWaitGroup **w) { wwaitgroup_done(*w); }

static void par_chunk_main(void *arg) {
  ParChunk *c = arg;
  // A step that panics must still release the group: the panic is caught by the
  // task's own frame, so without this the caller of for_par_step() waited for a
  // task that no longer existed.
  WWaitGroup *wg = c->wg;
  defer(par_chunk_done, wg);
  for (i32 i = c->lo; i < c->hi; i++)
    c->step(i, c->ctx);
  wm_free(c);
}

void for_par_step(i32 lo, i32 hi, void (*step)(i32 index, void *ctx), void *ctx) {
  if (hi <= lo) return;
  // one task per core, chunked; the caller's step function is the closure-free ctx
  i32 threads = g_np > 0 ? g_np : 1;
  i32 total = hi - lo;
  if (threads > total) threads = total;
  if (threads <= 1) {
    for (i32 i = lo; i < hi; i++)
      step(i, ctx);
    return;
  }
  WWaitGroup wg;
  wwaitgroup_init(&wg);
  i32 per = (total + threads - 1) / threads;
  for (i32 t = 0; t < threads; t++) {
    ParChunk *c = wm_alloc(sizeof *c);
    c->lo = lo + t * per;
    c->hi = lo + (t + 1) * per;
    if (c->hi > hi) c->hi = hi;
    c->step = step;
    c->ctx = ctx;
    c->wg = &wg;
    if (c->lo >= c->hi) {
      wm_free(c);
      continue;
    }
    wwaitgroup_add(&wg, 1);
    wg_spawn(par_chunk_main, c, "par");
  }
  wwaitgroup_wait(&wg);
}

// ---------------------------------------------------------------- channels

WChan *wchan_new(size_t elem, i32 cap) {
  WChan *c = wm_alloc(sizeof *c);
  c->elem = elem;
  c->cap = cap > 0 ? cap : 0;
  if (c->cap) c->buf = wm_alloc((size_t)c->cap * elem);
  return c;
}

static void waitq_push(WaitNode **head, WaitNode **tail, WaitNode *n) {
  n->next = nullptr;
  if (*tail)
    (*tail)->next = n;
  else
    *head = n;
  *tail = n;
}

static WaitNode *waitq_pop(WaitNode **head, WaitNode **tail) {
  WaitNode *n = *head;
  if (!n) return nullptr;
  *head = n->next;
  if (!*head) *tail = nullptr;
  n->next = nullptr;
  return n;
}

static void waitq_remove(WaitNode **head, WaitNode **tail, WaitNode *n) {
  WaitNode *prev = nullptr;
  for (WaitNode *it = *head; it; prev = it, it = it->next) {
    if (it != n) continue;
    if (prev)
      prev->next = it->next;
    else
      *head = it->next;
    if (*tail == it) *tail = prev;
    return;
  }
}

size_t wchan_elem(WChan *c) {
  return c ? c->elem : 0;
}

// Typed channel ops: the macros pass sizeof(the value). A mismatch means the
// program has two different ideas about what is in this channel, which is worth a
// named abort rather than a reinterpretation of bytes.
static void chan_check(WChan *c, size_t elem, const char *what) {
  if (!c) wc_panic(__FILE__, __LINE__, "chan_%s on a null channel", what);
  if (elem != c->elem)
    wc_panic(__FILE__, __LINE__, "chan_%s of a %zu-byte value on a %zu-byte channel", what, elem,
             c->elem);
}

bool wchan_send_typed(WChan *c, const void *v, size_t elem) {
  chan_check(c, elem, "send");
  return wchan_send(c, v);
}

bool wchan_recv_typed(WChan *c, void *out, size_t elem) {
  chan_check(c, elem, "recv");
  return wchan_recv(c, out);
}

bool wchan_try_send_typed(WChan *c, const void *v, size_t elem) {
  chan_check(c, elem, "try_send");
  return wchan_try_send(c, v);
}

bool wchan_try_recv_typed(WChan *c, void *out, size_t elem) {
  chan_check(c, elem, "try_recv");
  return wchan_try_recv(c, out);
}

bool wchan_try_send(WChan *c, const void *v) {
  g_lock(__LINE__);
  if (c->closed) {
    g_unlock(__LINE__);
    return false;
  }
  // a waiting receiver takes it directly
  WaitNode *r = waitq_pop(&c->recvq, &c->recvq_tail);
  if (r) {
    memcpy(r->out, v, c->elem);
    r->ready = true;
    if (r->g && r->g->sel) r->g->sel_done = true;
    if (r->g) g_ready_locked(r->g);
    g_unlock(__LINE__);
    return true;
  }
  if (c->n < c->cap) {
    memcpy(c->buf + (size_t)c->tail * c->elem, v, c->elem);
    c->tail = (c->tail + 1) % c->cap;
    c->n++;
    g_unlock(__LINE__);
    return true;
  }
  g_unlock(__LINE__);
  return false;
}

bool wchan_try_recv(WChan *c, void *out) {
  g_lock(__LINE__);
  if (c->n > 0) {
    memcpy(out, c->buf + (size_t)c->head * c->elem, c->elem);
    c->head = (c->head + 1) % c->cap;
    c->n--;
    // a waiting sender refills the slot
    WaitNode *s = waitq_pop(&c->sendq, &c->sendq_tail);
    if (s) {
      memcpy(c->buf + (size_t)c->tail * c->elem, s->val, c->elem);
      c->tail = (c->tail + 1) % c->cap;
      c->n++;
      s->ready = true;
      if (s->g && s->g->sel) s->g->sel_done = true;
      if (s->g) g_ready_locked(s->g);
    }
    g_unlock(__LINE__);
    return true;
  }
  if (c->cap == 0) {
    // rendezvous channel: pair directly with a waiting sender
    WaitNode *s = waitq_pop(&c->sendq, &c->sendq_tail);
    if (s) {
      memcpy(out, s->val, c->elem);
      s->ready = true;
      if (s->g && s->g->sel) s->g->sel_done = true;
      if (s->g) g_ready_locked(s->g);
      g_unlock(__LINE__);
      return true;
    }
  }
  g_unlock(__LINE__);
  return false;
}

bool wchan_send(WChan *c, const void *v) {
  if (!cur_task()) {
    // outside the scheduler: the caller is not a task, so poll politely
    for (;;) {
      if (wchan_try_send(c, v)) return true;
      if (wchan_closed(c)) return false;
      struct timespec ts = {0, 200000};
      nanosleep(&ts, nullptr);
    }
  }
  for (;;) {
    g_lock(__LINE__);
    if (c->closed) {
      g_unlock(__LINE__);
      return false;
    }
    WaitNode *r = waitq_pop(&c->recvq, &c->recvq_tail);
    if (r) {
      memcpy(r->out, v, c->elem);
      r->ready = true;
      if (r->g) g_ready_locked(r->g);
      g_unlock(__LINE__);
      return true;
    }
    if (c->n < c->cap) {
      memcpy(c->buf + (size_t)c->tail * c->elem, v, c->elem);
      c->tail = (c->tail + 1) % c->cap;
      c->n++;
      g_unlock(__LINE__);
      return true;
    }
    // park: our value is copied out when a receiver takes it
    WaitNode wn = {.g = cur_task(), .ch = c, .is_send = true, .val = v};
    waitq_push(&c->sendq, &c->sendq_tail, &wn);
    g_park_locked(0);
    if (wn.ready) return true;
    if (c->closed) return false;
  }
}

bool wchan_recv(WChan *c, void *out) {
  if (!cur_task()) {
    for (;;) {
      if (wchan_try_recv(c, out)) return true;
      if (wchan_closed(c) && wchan_len(c) == 0) return false;
      struct timespec ts = {0, 200000};
      nanosleep(&ts, nullptr);
    }
  }
  for (;;) {
    g_lock(__LINE__);
    if (c->n > 0) {
      memcpy(out, c->buf + (size_t)c->head * c->elem, c->elem);
      c->head = (c->head + 1) % c->cap;
      c->n--;
      WaitNode *s = waitq_pop(&c->sendq, &c->sendq_tail);
      if (s) {
        memcpy(c->buf + (size_t)c->tail * c->elem, s->val, c->elem);
        c->tail = (c->tail + 1) % c->cap;
        c->n++;
        s->ready = true;
        if (s->g) g_ready_locked(s->g);
      }
      g_unlock(__LINE__);
      return true;
    }
    WaitNode *s = waitq_pop(&c->sendq, &c->sendq_tail);
    if (s) {
      memcpy(out, s->val, c->elem);
      s->ready = true;
      if (s->g) g_ready_locked(s->g);
      g_unlock(__LINE__);
      return true;
    }
    if (c->closed) {
      g_unlock(__LINE__);
      return false;
    }
    WaitNode wn = {.g = cur_task(), .ch = c, .is_send = false, .out = out};
    waitq_push(&c->recvq, &c->recvq_tail, &wn);
    // parking inside the same critical section: a sender that pops this node
    // can no longer make a still-running task runnable a second time
    g_park_locked(0);
    if (wn.ready) return true;
    if (c->closed && c->n == 0) {
      g_lock(__LINE__);
      bool empty = c->n == 0;
      g_unlock(__LINE__);
      if (empty) return false;
    }
  }
}

void wchan_close(WChan *c) {
  g_lock(__LINE__);
  if (c->closed) {
    g_unlock(__LINE__);
    return;
  }
  c->closed = true;
  WaitNode *n;
  while ((n = waitq_pop(&c->recvq, &c->recvq_tail))) {
    n->ready = true;
    n->out = nullptr;
    if (n->g) g_ready_locked(n->g);
  }
  while ((n = waitq_pop(&c->sendq, &c->sendq_tail))) {
    n->ready = false;
    if (n->g) g_ready_locked(n->g);
  }
  g_unlock(__LINE__);
}

bool wchan_closed(WChan *c) {
  g_lock(__LINE__);
  bool b = c->closed;
  g_unlock(__LINE__);
  return b;
}

i32 wchan_len(WChan *c) {
  g_lock(__LINE__);
  i32 n = c->n;
  g_unlock(__LINE__);
  return n;
}

i32 wchan_cap(WChan *c) {
  return c->cap;
}

// ---------------------------------------------------------------- select

i32 select_wait(SelCase *cases, i32 n, i64 timeout_ms) {
  // fast path: something is already ready
  for (i32 i = 0; i < n; i++) {
    SelCase *sc = &cases[i];
    if (!sc->ch) return i; // default case
    if (sc->is_send) {
      if (wchan_try_send(sc->ch, sc->val)) return i;
    } else if (wchan_try_recv(sc->ch, sc->out)) {
      return i;
    }
  }
  if (timeout_ms == 0) return SELECT_TIMEOUT;
  if (!cur_task()) return SELECT_TIMEOUT;

  WaitNode *nodes = wm_alloc((size_t)n * sizeof(WaitNode));
  for (i32 i = 0; i < n; i++) {
    nodes[i] = (WaitNode){.g = cur_task(),
                          .ch = cases[i].ch,
                          .is_send = cases[i].is_send,
                          .val = cases[i].val,
                          .out = cases[i].out,
                          .index = i};
  }
  g_lock(__LINE__);
  for (i32 i = 0; i < n; i++) {
    if (!nodes[i].ch) continue;
    if (nodes[i].is_send)
      waitq_push(&nodes[i].ch->sendq, &nodes[i].ch->sendq_tail, &nodes[i]);
    else
      waitq_push(&nodes[i].ch->recvq, &nodes[i].ch->recvq_tail, &nodes[i]);
  }
  cur_task()->sel = nodes;
  cur_task()->nsel = n;
  cur_task()->sel_done = false;
  g_park_locked(timeout_ms > 0 ? now_ms() + timeout_ms : 0);

  // deregister whatever did not fire
  i32 result = SELECT_TIMEOUT;
  g_lock(__LINE__);
  for (i32 i = 0; i < n; i++) {
    WaitNode *wn = &nodes[i];
    if (!wn->ch) continue;
    if (wn->ready) {
      result = i;
      if (wn->is_send)
        waitq_remove(&wn->ch->sendq, &wn->ch->sendq_tail, wn);
      else
        waitq_remove(&wn->ch->recvq, &wn->ch->recvq_tail, wn);
      continue;
    }
    if (wn->is_send)
      waitq_remove(&wn->ch->sendq, &wn->ch->sendq_tail, wn);
    else
      waitq_remove(&wn->ch->recvq, &wn->ch->recvq_tail, wn);
  }
  cur_task()->sel = nullptr;
  cur_task()->nsel = 0;
  g_unlock(__LINE__);
  wm_free(nodes);
  return result;
}

// ---------------------------------------------------------------- sync

// A contended mutex parks on a waiter list rather than spinning on wg_yield():
// the spin kept every processor busy re-queueing itself and starved the holder
// (measured: 16 tasks on 4 workers never finished 80k wmutex_lock/unlock pairs).
__attribute__((noinline)) static void outside_local_set(void *p) {
  __asm__ volatile("" ::: "memory");
  t_local_outside = p;
}
__attribute__((noinline)) static void *outside_local_get(void) {
  __asm__ volatile("" ::: "memory");
  return t_local_outside;
}

__attribute__((noinline)) static void outside_build_set(void *p) {
  __asm__ volatile("" ::: "memory");
  t_build_outside = p;
}
__attribute__((noinline)) static void *outside_build_get(void) {
  __asm__ volatile("" ::: "memory");
  return t_build_outside;
}

__attribute__((noinline)) static void outside_panic_set(void *f) {
  __asm__ volatile("" ::: "memory");
  t_panic_outside = f;
}
__attribute__((noinline)) static void *outside_panic_get(void) {
  __asm__ volatile("" ::: "memory");
  return t_panic_outside;
}

// The panic handler and the error budget belong to the task, not the thread:
// both live on the task's stack and must be found again after a switch.
// Outside a task (a plain thread: the CLI, a test binary, a thread the app
// started) the frame is thread-local, which is correct because nothing can
// migrate that thread mid-call.
void *wtask_panic_get(void) {
  Wg *g = cur_task();
  return g ? g->panic_frame : outside_panic_get();
}
void wtask_panic_set(void *f) {
  Wg *g = cur_task();
  if (g)
    g->panic_frame = f;
  else
    outside_panic_set(f);
}

// The defer()/errdefer() chain and the try() counter belong to the task for the
// same reason the panic frame does: a handler that parks resumes on another worker,
// and a thread-local counter made errdefer fire (or not) depending on where it
// happened to land.
void *wtask_unwind_get(void) {
  Wg *g = cur_task();
  return g ? (void *)&g->unwind : nullptr;
}

// The other task-local: the markup build in progress. A view that parks (a query,
// a sleep) resumes on another worker, and a thread-local build pointer would be
// null there. See the comment in src/dom.c.
void wtask_set_build(void *p) {
  Wg *self = cur_task();
  if (self)
    self->build = p;
  else
    outside_build_set(p);
}

void *wtask_build(void) {
  Wg *self = cur_task();
  return self ? self->build : outside_build_get();
}

// The reactive runtime: same reason as the build (see src/react.c). Outside a task the
// thread-local fallback in react.c is the right answer, so this returns null.
void *wtask_rt_get(void) {
  Wg *self = cur_task();
  return self ? self->rt : nullptr;
}

void wtask_rt_set(void *rt) {
  Wg *self = cur_task();
  if (self) self->rt = rt;
}

void wtask_set_local(void *p) {
  Wg *self = cur_task();
  if (self)
    self->local = p;
  else
    outside_local_set(p);
}

void *wtask_local(void) {
  Wg *self = cur_task();
  return self ? self->local : outside_local_get();
}

// The task's database session, and the hook that cleans up one a task did not release.
// A session outlives the statement that opened it, so it cannot live on the task's stack:
// a handler that panics unwinds past the `cleanup` attribute, and a transaction left open
// in the pool is worse than a leaked pointer (it is locks, held until the connection is
// closed). The hook runs where the stack still exists — in g_trampoline, after the
// function returned or its panic was caught.
void wtask_set_exit_hook(void (*fn)(void *)) {
  g_task_exit_hook = fn;
}

void *wtask_db_get(void) {
  Wg *self = cur_task();
  return self ? self->db : t_db_outside;
}

void wtask_db_set(void *p) {
  Wg *self = cur_task();
  if (self)
    self->db = p;
  else
    t_db_outside = p; // a CLI command or a test body: one thread, no request to confuse
}

// The framework's own auxiliary slot (the query cache's `nocache` depth). Task-local for the same
// reason the database session is: a task that parks resumes on another worker.
void *wtask_aux_get(void) {
  Wg *self = cur_task();
  return self ? self->aux : t_aux_outside;
}

void wtask_aux_set(void *p) {
  Wg *self = cur_task();
  if (self)
    self->aux = p;
  else
    t_aux_outside = p;
}

static const char *state_name(WgState s) {
  switch (s) {
  case WG_READY:
    return "ready";
  case WG_RUNNING:
    return "running";
  case WG_PARKED:
    return "parked";
  case WG_DEAD:
    return "dead";
  }
  return "?";
}

// A mutex remembers which tasks wait for it; a task remembers which mutexes it
// holds. That second list is what makes a panic survivable: a task that dies takes
// its locks with it, and the next waiter is handed the mutex instead of parking
// forever behind a task that no longer exists. Callers hold g_mu.
static void mutex_held_push(Wg *g, WMutex *m) {
  m->held_next = g->held;
  g->held = m;
}

static void mutex_held_drop(Wg *g, WMutex *m) {
  for (WMutex **pp = &g->held; *pp; pp = &(*pp)->held_next)
    if (*pp == m) {
      *pp = m->held_next;
      m->held_next = nullptr;
      return;
    }
}

// Hand the mutex to the first waiter, or release it. `to` receives the ownership
// record so the receiver's held list stays true. Callers hold g_mu.
static void mutex_release_locked(WMutex *m) {
  Wg *w = (Wg *)m->wait_head;
  if (!w) {
    atomic_store(&m->owner_id, 0);
    atomic_store(&m->locked, 0);
    return;
  }
  m->wait_head = w->mu_next;
  if (!m->wait_head) m->wait_tail = nullptr;
  w->mu_next = nullptr;
  w->mu_granted = true;
  atomic_store(&m->owner_id, w->id);
  mutex_held_push(w, m);
  g_ready_locked(w);
}

// Called when a task is about to die (a panic that escaped its work, or a normal
// return): everything it still holds is released or handed on.
static void task_release_mutexes(Wg *g) {
  if (!g->held) return;
  g_lock(__LINE__);
  i32 n = 0;
  while (g->held) {
    WMutex *m = g->held;
    g->held = m->held_next;
    m->held_next = nullptr;
    mutex_release_locked(m);
    n++;
  }
  g_unlock(__LINE__);
  if (n && log_level() <= LOG_WARN)
    log_warn("task died holding mutexes: released them for the next waiter",
             {"task", str_lit(g->name)}, {"count", str_fmt(nullptr, "%d", (int)n)});
}

void wmutex_lock(WMutex *m) {
#ifdef WC_LOCK_HIST
  ra_note();
#endif
  for (;;) {
    MU_STEP(10);
    if (!cur_task()) {
      int e = 0;
      while (!atomic_compare_exchange_weak(&m->locked, &e, 1)) {
        e = 0;
        sched_yield();
      }
      return;
    }
    MU_STEP(11);
    Wg *g = cur_task();
    // The test and the enqueue happen under the scheduler lock the unlocker
    // also takes, so a release can never slip between "it is taken" and "I am
    // on the waiter list".
    MU_STEP(12);
    g_lock(__LINE__);
    MU_STEP(13);
    if (g->mu_granted) { // unlock() (or a dying owner) handed the lock to us
      g->mu_granted = false;
      // the giver already put m on our held list: pushing again would make the
      // list point at itself
      g_unlock(__LINE__);
      return;
    }
    MU_STEP(14);
    int expected = 0;
    if (atomic_compare_exchange_weak(&m->locked, &expected, 1)) {
      atomic_store(&m->owner_id, g->id);
      mutex_held_push(g, m);
      g_unlock(__LINE__);
      MU_STEP(15);
      return;
    }
    g->mu_next = nullptr;
#ifdef WC_TRACE_PARK
    if (g->state == WG_PARKED) {
      fprintf(stderr, "MUTEX: task %llu park while already parked\n", (unsigned long long)g->id);
      abort();
    }
#endif
    if (m->wait_tail)
      ((Wg *)m->wait_tail)->mu_next = g;
    else
      m->wait_head = g;
    m->wait_tail = g;
    MU_STEP(16);
    g_park_locked(0);
    MU_STEP(17);
#ifdef WC_TRACE_PARK
    if (!g->mu_granted) {
      static _Atomic int spurious;
      int n = atomic_fetch_add(&spurious, 1) + 1;
      if (n < 6) fprintf(stderr, "SPURIOUS wake #%d state=%d\n", n, (int)g->state);
    }
#endif
  }
}

bool wmutex_trylock(WMutex *m) {
  int expected = 0;
  if (!atomic_compare_exchange_strong(&m->locked, &expected, 1)) return false;
  Wg *g = cur_task();
  if (!g) {
    atomic_store(&m->owner_id, 0);
    return true;
  }
  g_lock(__LINE__);
  atomic_store(&m->owner_id, g->id);
  mutex_held_push(g, m);
  g_unlock(__LINE__);
  return true;
}

void wmutex_unlock(WMutex *m) {
#ifdef WC_LOCK_HIST
  ra_note();
#endif
  if (!cur_task()) {
    atomic_store(&m->owner_id, 0);
    atomic_store(&m->locked, 0);
    return;
  }
  Wg *self = cur_task();
  g_lock(__LINE__);
  mutex_held_drop(self, m);
  mutex_release_locked(m);
  g_unlock(__LINE__);
}

void wrwlock_rlock(WRwLock *l) {
  for (;;) {
    i32 w = atomic_load(&l->writer);
    if (w == 0) {
      atomic_fetch_add(&l->readers, 1);
      if (atomic_load(&l->writer) == 0) return;
      atomic_fetch_sub(&l->readers, 1);
    }
    if (!cur_task())
      sched_yield();
    else
      wg_yield();
  }
}

void wrwlock_runlock(WRwLock *l) {
  atomic_fetch_sub(&l->readers, 1);
}

void wrwlock_lock(WRwLock *l) {
  while (!atomic_compare_exchange_weak(&l->writer, &(int){0}, 1)) {
    if (!cur_task())
      sched_yield();
    else
      wg_yield();
  }
  while (atomic_load(&l->readers) != 0) {
    if (!cur_task())
      sched_yield();
    else
      wg_yield();
  }
}

void wrwlock_unlock(WRwLock *l) {
  atomic_store(&l->writer, 0);
}

void wonce_do(WOnce *o, void (*fn)(void)) {
  int expected = 0;
  if (atomic_compare_exchange_strong(&o->state, &expected, 1)) {
    fn();
    atomic_store(&o->state, 2);
    return;
  }
  while (atomic_load(&o->state) != 2) {
    if (!cur_task())
      sched_yield();
    else
      wg_yield();
  }
}

void wwaitgroup_init(WWaitGroup *w) {
  w->n = 0;
  w->done = wchan_new(sizeof(bool), 1);
  w->mu = (WMutex)WEBC_MUTEX_INIT;
}

void wwaitgroup_add(WWaitGroup *w, i32 delta) {
  atomic_fetch_add(&w->n, delta);
}

void wwaitgroup_done(WWaitGroup *w) {
  if (atomic_fetch_sub(&w->n, 1) == 1) {
    bool b = true;
    wchan_try_send(w->done, &b);
  }
}

void wwaitgroup_wait(WWaitGroup *w) {
  while (atomic_load(&w->n) > 0) {
    bool b;
    if (!wchan_recv(w->done, &b)) {
      if (atomic_load(&w->n) == 0) return;
      wg_yield();
    }
  }
}

// ---------------------------------------------------------------- fd helpers

void wfd_nonblock(i32 fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
}

i64 wfd_read(i32 fd, void *buf, size_t n) {
  for (;;) {
    ssize_t r = read(fd, buf, n);
    if (r >= 0) return r;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // A park with a horizon, not forever: if a wakeup were ever lost, the
      // task re-checks instead of leaking (a closed keep-alive connection used
      // to strand one task per connection).
      if (!park_on_fd(fd, false, now_ms() + WC_FD_PARK_MAX_MS)) return -1;
      continue;
    }
    if (errno == EINTR) continue;
    return -1;
  }
}

i64 wfd_write(i32 fd, const void *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t r = write(fd, (const char *)buf + off, n - off);
    if (r > 0) {
      off += (size_t)r;
      continue;
    }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (!park_on_fd(fd, true, now_ms() + WC_FD_PARK_MAX_MS)) return -1;
      continue;
    }
    if (r < 0 && errno == EINTR) continue;
    return -1;
  }
  return (i64)off;
}

bool wfd_accept(i32 lfd, i32 *out) {
  for (;;) {
    i32 fd = accept(lfd, nullptr, nullptr);
    if (fd >= 0) {
      wfd_nonblock(fd);
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      *out = fd;
      return true;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // A bounded park, not an indefinite one: closing an fd removes its kqueue
      // registration silently, so a task parked here on a listener that somebody
      // closed would never be woken again — and because `wtask_shutdown()` waits for
      // `g_live == 0`, one such task kept the process alive forever (measured: every
      // SIGTERM'd server became an unkillable orphan). Half a second is one wakeup per
      // half second on an idle server, which costs nothing, and it makes "a closed fd
      // never strands a task" true without trusting every waker.
      if (!park_on_fd(lfd, false, now_ms() + WC_ACCEPT_PARK_MAX_MS)) return false;
      continue;
    }
    if (errno == EINTR) continue;
    return false;
  }
}

bool wfd_wait(i32 fd, bool for_write, i64 timeout_ms) {
  return park_on_fd(fd, for_write, timeout_ms > 0 ? now_ms() + timeout_ms : 0);
}

void wfd_close(i32 fd) {
  close(fd);
}

// Wake every task parked on `fd`. Closing an fd is invisible to a task parked on it:
// the kernel drops the kqueue registration and the task stays parked until a timer it
// may not have fires. Anyone about to close an fd that another task may be waiting on
// (a listener on shutdown, a socket a peer tore down) must call this, or the waiter is
// gone for good.
void wtask_wake_fd(i32 fd) {
  if (fd < 0 || !atomic_load(&g_started)) return;
  g_lock(__LINE__);
  for (Wg *g = g_all_head; g; g = g->all_next) {
    if (g->state != WG_PARKED || g->poll_fd != fd) continue;
    g->poll_closed = true; // the waiter must not simply retry: the fd is going away
    g->poll_fd = -1;
    if (g->timer_slot >= 0) timer_del(g);
    g_ready_locked(g);
  }
  wake_all_locked();
  g_unlock(__LINE__);
}

void wfd_set_deadline(i32 fd, i64 deadline_ms) {
  (void)fd;
  (void)deadline_ms;
}

i32 wfd_open_listen(const char *addr, u16 *port_out) {
  // "127.0.0.1:8080", ":8080", "0.0.0.0:0"
  const char *colon = strrchr(addr, ':');
  if (!colon) return -1;
  char host[128] = {0};
  size_t hlen = (size_t)(colon - addr);
  if (hlen >= sizeof host) return -1;
  memcpy(host, addr, hlen);
  i32 port = atoi(colon + 1);

  struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((u16)port)};
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  if (hlen > 0 && !(hlen == 1 && host[0] == '*')) {
    struct in_addr ia;
    if (inet_pton(AF_INET, host, &ia) == 1) sa.sin_addr = ia;
  }
  i32 fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 1024) < 0) {
    close(fd);
    return -1;
  }
  wfd_nonblock(fd);
  if (port_out) {
    struct sockaddr_in bound;
    socklen_t blen = sizeof bound;
    if (getsockname(fd, (struct sockaddr *)&bound, &blen) == 0) *port_out = ntohs(bound.sin_port);
  }
  return fd;
}

// A client socket: resolve, connect, and park the task while the peer thinks. The only outbound
// connection the framework makes today is SMTP (webc/mail.h), and it is the same three POSIX
// calls an app would make — written once so a timeout is a deadline (wfd_set_deadline) rather
// than a thread that can hang a worker.
i32 wfd_open_connect(const char *host, u16 port, i64 timeout_ms) {
  if (!host || !*host) return -1;
  char portbuf[8];
  snprintf(portbuf, sizeof portbuf, "%u", (unsigned)port);
  struct addrinfo hints = {0};
  hints.ai_family = AF_UNSPEC;   // v4 or v6, whichever the name answers with
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) return -1;
  i32 out = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    i32 fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    // A driver writes its frames as several small messages (postgres: Parse, Bind, Describe,
    // Execute, Sync). Without TCP_NODELAY the second write waits for the first to be
    // acknowledged, and with a delayed ACK on the far side that is up to 40 ms — measured, it
    // put a postgres-backed page at 32 requests/second while the process was idle 91% of the
    // time. Every protocol client here speaks in small frames, so this belongs on the socket
    // rather than in each driver.
    i32 nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
    wfd_nonblock(fd);
    i32 rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
      close(fd);
      continue;
    }
    if (rc != 0 && !wfd_wait(fd, true, timeout_ms > 0 ? timeout_ms : 10000)) {
      close(fd);
      continue;
    }
    i32 soerr = 0;
    socklen_t slen = sizeof soerr;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 || soerr != 0) {
      close(fd);
      continue;
    }
    out = fd;
    break;
  }
  freeaddrinfo(res);
  return out;
}

// What every live task is doing, and where. This is the /debug view of a stall:
// ids, names, states, and the processor each one is queued on.
// A stall report that can never hang the caller: if the scheduler lock is
// wedged, that is the answer, and saying so is more useful than blocking.
void wtask_dump_safe(FILE *out) {
  if (pthread_mutex_trylock(&g_mu) != 0) {
#ifdef WC_TRACE_PARK
    fprintf(out,
            "tasks: scheduler lock is WEDGED: held at line %d by thread %p "
            "(handoff set=%ld taken=%ld missed=%ld, lock=%ld unlock=%ld)\n",
            atomic_load(&g_mu_line), (void *)atomic_load(&g_mu_th), atomic_load(&g_ho_set),
            atomic_load(&g_ho_true), atomic_load(&g_ho_false), atomic_load(&g_lock_n),
            atomic_load(&g_unlock_n));
#else
    fprintf(out, "tasks: scheduler lock is WEDGED (held by a thread that is not releasing it)\n");
#endif
    fflush(out);
    return;
  }
  pthread_mutex_unlock(&g_mu);
  wtask_dump(out);
}

void wtask_dump(FILE *out) {
#ifdef WC_TRACE_PARK
  // read the owner before taking the lock, or we report ourselves
  fprintf(out, "  g_mu: %s (line %d, thread %p) handoff set=%ld taken=%ld missed=%ld\n",
          atomic_load(&g_mu_line) ? "HELD" : "free", atomic_load(&g_mu_line),
          (void *)atomic_load(&g_mu_th), atomic_load(&g_ho_set), atomic_load(&g_ho_true),
          atomic_load(&g_ho_false));
#endif
  g_lock(__LINE__);
  fprintf(out, "tasks: %d live, %d processors, %d timers\n", atomic_load(&g_live), g_np, g_ntimers);
  for (i32 i = 0; i < g_np; i++)
    fprintf(out, "  P%d: %d queued, at %s, running task %llu, thread %p\n", i, g_ps[i].n,
            g_ps[i].where ? g_ps[i].where : "?",
            (unsigned long long)(g_ps[i].cur ? g_ps[i].cur->id : 0), (void *)(uintptr_t)g_ps[i].th);
  i32 shown = 0;
  for (Wg *g = g_all_head; g && shown < 64; g = g->all_next, shown++)
#ifdef WC_TRACE_PARK
    fprintf(out, "  task %llu %-10s %-8s P%d queued=%d on_cpu=%d mu_step=%d granted=%d\n",
            (unsigned long long)g->id, g->name ? g->name : "task", state_name(g->state),
            g->p ? (int)g->p->id : -1, (int)g->queued, (int)atomic_load(&g->on_cpu), g->mu_step,
            (int)g->mu_granted);
#else
    fprintf(out, "  task %llu %-10s %-8s\n", (unsigned long long)g->id, g->name ? g->name : "task",
            state_name(g->state));
#endif
  if (shown >= 64) fprintf(out, "  ...\n");
  g_unlock(__LINE__);
}