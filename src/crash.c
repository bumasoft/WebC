// SPDX-License-Identifier: Apache-2.0
#define WEBC_NO_SELECT_MACRO 1
// WebC — a crash is a diagnosis, not a silence.
//
// A server that dies should say where it died and what every task was doing.
// Two things make that hard in a green-thread runtime: the fault arrives on a
// worker thread that has a task's stack, and the interesting state (queues,
// parked tasks, the scheduler lock) is not on any one stack. So the handler
// does both: a backtrace of the faulting thread and a task dump.
#include <webc/log.h>
#include <webc/task.h>
#include <webc/http.h> // webc_req_current: the report names the request it crashed in

#include <stdatomic.h>
#include <string.h>

#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static _Atomic bool g_installed;
// One alternate stack for the whole process, not one per thread: the handler always
// terminates the process (the first fault wins by definition), so a shared buffer cannot
// be clobbered mid-report. A thread-local one also costs a thread-exit bookkeeping fault
// of its own: the sanitizer runtime maps a 64 KiB thread-local block as a secondary
// allocation and cannot unmap it when the thread leaves, which turns a clean test run into
// "AddressSanitizer failed to deallocate 65536 bytes".
static char g_altstack[64 * 1024];
static _Atomic int g_in_handler;

// async-signal-safe write of a string
static void wr(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  ssize_t ignored = write(2, s, n);
  (void)ignored;
}

// ...and of bytes that are not terminated: a request's path is a slice of the request
// buffer, so a `write` that walked to a NUL would print the headers after it.
static void wrn(const char *s, size_t n) {
  if (!s || n == 0) return;
  ssize_t ignored = write(2, s, n);
  (void)ignored;
}

static void wr_hex(unsigned long v) {
  char buf[24];
  int i = (int)sizeof buf;
  buf[--i] = 0;
  do {
    buf[--i] = "0123456789abcdef"[v & 15];
    v >>= 4;
  } while (v);
  wr(buf + i);
}

static void wr_int(long v) {
  char buf[24];
  int i = (int)sizeof buf;
  bool neg = v < 0;
  unsigned long u = neg ? (unsigned long)-v : (unsigned long)v;
  buf[--i] = 0;
  do {
    buf[--i] = (char)('0' + (u % 10));
    u /= 10;
  } while (u);
  if (neg) buf[--i] = '-';
  wr(buf + i);
}

#include <sys/ucontext.h>

// The frame chain is not always walkable (a fault on a bad stack, a jump to a
// wild address), so print the machine state first: the program counter alone
// pins the fault down, and `atos -o <binary> <pc>` names it.
static void dump_machine_state(void *ctx) {
#if defined(__aarch64__) && defined(__APPLE__)
  ucontext_t *uc = ctx;
  wr("webc: pc=0x");
  wr_hex((unsigned long)uc->uc_mcontext->__ss.__pc);
  wr(" sp=0x");
  wr_hex((unsigned long)uc->uc_mcontext->__ss.__sp);
  wr(" fp=0x");
  wr_hex((unsigned long)uc->uc_mcontext->__ss.__fp);
  wr(" lr=0x");
  wr_hex((unsigned long)uc->uc_mcontext->__ss.__lr);
  wr("\n");
#elif defined(__aarch64__)
  // glibc spells the same registers without the __ss indirection
  ucontext_t *uc = ctx;
  wr("webc: pc=0x");
  wr_hex((unsigned long)uc->uc_mcontext.pc);
  wr(" sp=0x");
  wr_hex((unsigned long)uc->uc_mcontext.sp);
  wr(" fp=0x");
  wr_hex((unsigned long)uc->uc_mcontext.regs[29]);
  wr(" lr=0x");
  wr_hex((unsigned long)uc->uc_mcontext.regs[30]);
  wr("\n");
#else
  (void)ctx;
#endif
}

static void on_fault(int sig, siginfo_t *si, void *ctx) {
  // A fault *inside* the reporter (backtrace() on a ucontext stack is the usual
  // one) must not be counted twice: leave with the first answer we have.
  if (atomic_exchange(&g_in_handler, 1)) _exit(70);
  const char *name = sig == SIGSEGV   ? "SIGSEGV"
                     : sig == SIGBUS  ? "SIGBUS"
                     : sig == SIGABRT ? "SIGABRT"
                     : sig == SIGILL  ? "SIGILL"
                     : sig == SIGFPE  ? "SIGFPE"
                                      : "signal";
  wr("\nwebc: crash — ");
  wr(name);
  if (si && (sig == SIGSEGV || sig == SIGBUS)) {
    wr(" at ");
    wr_int((long)(intptr_t)si->si_addr);
  }
  wr("\n");
  // Which request. Without this, a crash in a handler reads exactly like a crash in the
  // scheduler: a backtrace, a task dump, and nothing that names the route. The request is
  // on the faulting task (a plain thread-local pointer read, which is safe here), and every
  // string in it is already allocated, so this needs no allocation either.
  Request *rq = webc_req_current();
  if (rq) {
    wrn("webc: request ", 14);
    wrn(rq->method.p ? rq->method.p : "?", rq->method.n);
    wrn(" ", 1);
    wrn(rq->path.p ? rq->path.p : "?", rq->path.n);
    wrn("  route=", 9);
    if (rq->matched_pattern.p)
      wrn(rq->matched_pattern.p, rq->matched_pattern.n);
    else
      wrn("(unmatched)", 11);
    wrn("  id=", 6);
    wrn(rq->request_id.p ? rq->request_id.p : "?", rq->request_id.n);
    wrn("\n", 1);
  } else {
    wr("webc: no request (boot, a background task, or a reload)\n");
  }
  dump_machine_state(ctx);
  wr("webc: backtrace (faulting thread):\n");
  void *bt[48];
  int n = backtrace(bt, 48);
  backtrace_symbols_fd(bt, n, 2);
  wr("webc: tasks:\n");
  wtask_dump(stderr);
  // The report is out; now the process must die. Re-raising and trusting the signal to
  // finish the job is not enough: when the fault happened on a green-thread stack the
  // process *survives* — with a dead worker, a corrupt heap, and exit 0. So: exit, unless
  // WEBC_CORE_DUMP=1 asks for a core file.
  const char *cd = getenv("WEBC_CORE_DUMP");
  if (cd && *cd) {
    wr("webc: reraising for a core dump\n");
    signal(sig, SIG_DFL);
    raise(sig);
  }
  _exit(70);
}

// Each worker gets its own alternate stack: `sigaltstack` is per thread, and one
// shared buffer meant a second fault corrupted the first one's report.
// A sanitizer runtime owns the alternate signal stack, and swapping it out makes its
// thread teardown fail ("AddressSanitizer failed to deallocate 65536 bytes"). The check
// is at runtime, not compile time, because only the test translation units are built
// with -fsanitize=… — this library is not, and still runs inside a sanitized process.
void wc_crash_thread_init(void) {
  // If a thread already has an alternate stack, something that knows better put it
  // there — a sanitizer runtime installs one per thread and cannot cope with a second
  // party swapping it out mid-flight. Keep it and report on theirs.
  stack_t cur;
  if (sigaltstack(nullptr, &cur) == 0 && cur.ss_sp != nullptr) return;
  stack_t ss = {.ss_sp = g_altstack, .ss_size = sizeof g_altstack};
  sigaltstack(&ss, nullptr);
}

void wc_crash_init(void) {
  if (atomic_exchange(&g_installed, true)) return;
  wc_crash_thread_init();
  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  const int sigs[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};
  for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
    sigaction(sigs[i], &sa, nullptr);
}