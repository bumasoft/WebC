// SPDX-License-Identifier: Apache-2.0
// webc — a small sampling profiler for `/debug/cpu` (SPEC §6.6 "CPU sampler").
//
//   GET /debug/cpu?ms=250
//
// What it does: for `ms` milliseconds it lets the kernel interrupt the process on
// a CPU-time timer (ITIMER_PROF at 4 kHz) and records the interrupted program
// counter. The handler records into a lock-free array and nothing else — no
// malloc, no locks, no formatting — because it runs on a worker thread in the
// middle of whatever it was doing. When the window closes, the samples are
// grouped by symbol (dladdr) and printed as a flat profile.
//
// That is the whole design, and it is why there is no pprof endpoint here: a
// sampling profiler needs no cooperation from the code it profiles.
// System headers first: <mach/mach.h> declares panic(), and <webc/core.h> defines a
// `panic` macro, so including them the other way round does not compile.
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#endif
#if defined(__linux__)
#include <dlfcn.h>
#endif

#include <webc/core.h>
#include <webc/log.h>

#define WEBC_NO_SELECT_MACRO 1 // this file is not a route table
#include <webc/tls.h>
#include <webc/task.h>

// One sample per busy worker per scheduler tick: 8 192 overflowed in a 3 s window on
// a 10-worker box (measured: 79 626 dropped). 65 536 is 512 KB of addresses.
#define PROF_MAX 65536

static _Atomic u64 g_pcs[PROF_MAX]; // the interrupted PCs
static _Atomic i32 g_npcs;
static _Atomic int g_running;
static _Atomic u64 g_missed;

static void prof_handler(int sig, siginfo_t *info, void *uctx) {
  (void)sig;
  (void)info;
  if (!atomic_load_explicit(&g_running, memory_order_relaxed)) return;
  if (!wtask_this_thread_busy()) return; // an idle worker is not where the time went
  u64 pc = 0;
#if defined(__APPLE__) && defined(__aarch64__)
  ucontext_t *uc = uctx;
  pc = (u64)uc->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
  // Darwin names the program counter `__rip` on Intel and `__pc` on Apple silicon — the
  // same split src/task.c has for WC_UCTX_PC. Without this arm the profile build failed
  // on every Intel Mac ("no member named '__pc' in 'struct __darwin_x86_thread_state64'"),
  // which nobody noticed because the release matrix row had never been built: the first
  // workflow_dispatch dry run is what said so.
  ucontext_t *uc = uctx;
  pc = (u64)uc->uc_mcontext->__ss.__rip;
#elif defined(__x86_64__)
  ucontext_t *uc = uctx;
  pc = (u64)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
  // Linux/aarch64 had no branch here, so every sample stored address 0 and the
  // report said "8192 unresolved" while the ring overflowed.
  ucontext_t *uc = uctx;
  pc = (u64)uc->uc_mcontext.pc;
#else
  (void)uctx;
#endif
  i32 i = atomic_fetch_add_explicit(&g_npcs, 1, memory_order_relaxed);
  if (i < PROF_MAX)
    atomic_store_explicit(&g_pcs[i], pc, memory_order_relaxed);
  else
    atomic_fetch_add(&g_missed, 1);
}

// The sampler thread. ITIMER_PROF alone was not enough: on Darwin it delivers
// SIGPROF to the main thread, so a busy worker stayed invisible (measured: one
// sample in a second while a task spun). pthread_kill names the thread, so every
// worker gets sampled whether it is running, parked or in the scheduler.
#include <pthread.h>

static pthread_t g_sampler;
static bool g_have_sampler;
static i64 g_profile_ms;
static _Atomic int g_stop_sampler;

// Threads to sample. The scheduler's workers are not the whole process: an app can
// start its own pthreads, and a profile that cannot see them is a profile of half
// the program. Darwin gives thread ports (and a pthread_t from each); Linux has the
// task directory.
#if defined(__APPLE__)
static i32 all_threads(void **out, i32 max) {
  thread_act_array_t list = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return 0;
  i32 n = 0;
  for (mach_msg_type_number_t i = 0; i < count && n < max; i++) {
    out[n++] = (void *)(uintptr_t)pthread_from_mach_thread_np(list[i]);
    mach_port_deallocate(mach_task_self(), list[i]);
  }
  vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(thread_act_t));
  return n;
}
static void kill_thread(void *th) {
  pthread_kill((pthread_t)(uintptr_t)th, SIGPROF);
}
#elif defined(__linux__)
#include <dirent.h>
#include <sys/syscall.h>
static i32 all_threads(void **out, i32 max) {
  DIR *d = opendir("/proc/self/task");
  if (!d) return 0;
  i32 n = 0;
  struct dirent *e;
  while ((e = readdir(d)) && n < max) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    out[n++] = (void *)(uintptr_t)strtoul(e->d_name, nullptr, 10);
  }
  closedir(d);
  return n;
}
static void kill_thread(void *th) {
  syscall(SYS_tgkill, getpid(), (pid_t)(uintptr_t)th, SIGPROF);
}
#else
static i32 all_threads(void **out, i32 max) {
  return wtask_os_threads(out, max);
}
static void kill_thread(void *th) {
  (void)th;
}
#endif

static _Atomic i32 g_rounds;

static void *sampler_main(void *arg) {
  (void)arg;
  void *ths[128];
  i32 nth = all_threads(ths, 128);
  pthread_t self = pthread_self();
  u64 deadline = (u64)g_profile_ms + wc_time_ms();
  while (!atomic_load(&g_stop_sampler)) {
    nth = all_threads(ths, 128); // threads come and go: refresh each round
    for (i32 i = 0; i < nth; i++)
      // never signal the sampler itself: the signal interrupts its own sleep, and
      // with SA_RESTART that restarts a 500 us sleep forever (measured: one round
      // of sampling took 200 ms and the window produced 2 samples)
      if ((pthread_t)(uintptr_t)ths[i] != self) kill_thread(ths[i]);
    struct timespec ts = {0, 500000}; // 500 us: two samples a millisecond, 64 threads
    nanosleep(&ts, nullptr);
    atomic_fetch_add(&g_rounds, 1);
      if (wc_time_ms() >= deadline) break;
  }
  atomic_store(&g_running, 0);
  return nullptr;
}

bool webc_profile_start(i64 duration_ms, i32 *err) {
  if (atomic_exchange(&g_running, 1)) {
    // A second window is refused, but it must not touch g_running: clearing it
    // would stop the window that is already collecting, so asking twice would
    // silently kill the profile you asked for.
    if (err) *err = 1;
    return false;
  }
  atomic_store(&g_npcs, 0);
  atomic_store(&g_missed, 0);
  struct sigaction sa = {0};
  sa.sa_sigaction = prof_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGPROF, &sa, nullptr) != 0) {
    atomic_store(&g_running, 0);
    if (err) *err = 2;
    return false;
  }
  if (duration_ms <= 0) duration_ms = 100;
  if (duration_ms > 5000) duration_ms = 5000; // a debug endpoint is not a batch job
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPROF);
  pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
  // A CPU-time timer as a floor: even with no sampler thread we get samples.
  struct itimerval it = {
      .it_interval = {.tv_sec = 0, .tv_usec = 1000},
      .it_value = {.tv_sec = 0, .tv_usec = 1000},
  };
  setitimer(ITIMER_PROF, &it, nullptr);
  g_profile_ms = duration_ms;
  atomic_store(&g_stop_sampler, 0);
  if (!g_have_sampler) {
    // one sampler for the life of the process: it costs a thread and idles between
    // windows, and creating it per request would need a join per request
    if (pthread_create(&g_sampler, nullptr, sampler_main, nullptr) == 0) {
      pthread_detach(g_sampler);
      g_have_sampler = true;
    }
  }
  if (g_have_sampler) {
    // wake the sampler for this window
    pthread_t again;
    if (pthread_create(&again, nullptr, sampler_main, nullptr) == 0) pthread_detach(again);
  }
  return true;
}

i32 webc_profile_rounds(void) { return atomic_load(&g_rounds); }

void webc_profile_stop(void) {
  struct itimerval off = {0};
  setitimer(ITIMER_PROF, &off, nullptr);
  atomic_store(&g_stop_sampler, 1);
  atomic_store(&g_running, 0);
}

i32 webc_profile_samples(void) {
  i32 n = atomic_load(&g_npcs);
  return n > PROF_MAX ? PROF_MAX : n;
}

bool webc_profile_active(void) {
  return atomic_load(&g_running) != 0;
}

typedef struct Sym {
  char name[96];
  i32 count;
} Sym;

// Group the samples by enclosing symbol. dladdr is not signal-safe, which is
// exactly why it is called here and not in the handler.
str webc_profile_report(Arena *a) {
  i32 n = atomic_load(&g_npcs);
  if (n <= 0) return str_lit("no samples (nothing was running on CPU time)\n");
  if (n > PROF_MAX) n = PROF_MAX;
  Sym *syms = arena_alloc(a, 256 * sizeof(Sym));
  i32 nsyms = 0;
  i32 unknown = 0;
  for (i32 i = 0; i < n; i++) {
    u64 pc = atomic_load(&g_pcs[i]);
    Dl_info di;
    // A PC with no exported symbol dropped as "unresolved" hides 65% of every profile
    // behind libkqueue and libsqlite3. Name the module and the offset
    // instead: "libkqueue.so+0x4d20" still says who is spending the CPU.
    char label[160];
    const char *name = nullptr;
    if (pc && dladdr((void *)(uintptr_t)pc, &di)) {
      if (di.dli_sname) {
        name = di.dli_sname;
      } else if (di.dli_fname && di.dli_fbase) {
        const char *slash = strrchr(di.dli_fname, '/');
        snprintf(label, sizeof label, "%s+0x%llx", slash ? slash + 1 : di.dli_fname,
                 (unsigned long long)(pc - (u64)(uintptr_t)di.dli_fbase));
        name = label;
      }
    }
    if (!name) {
      unknown++;
      continue;
    }
    i32 k = 0;
    for (; k < nsyms; k++)
      if (strcmp(syms[k].name, name) == 0) break;
    if (k == nsyms) {
      if (nsyms == 256) {
        unknown++;
        continue;
      }
      snprintf(syms[nsyms].name, sizeof syms[0].name, "%s", name);
      syms[nsyms].count = 0;
      nsyms++;
    }
    syms[k].count++;
  }
  // flat profile, hottest first: an 8-line insertion sort on at most 256 rows
  for (i32 i = 1; i < nsyms; i++) {
    Sym key = syms[i];
    i32 j = i - 1;
    while (j >= 0 && syms[j].count < key.count) {
      syms[j + 1] = syms[j];
      j--;
    }
    syms[j + 1] = key;
  }
  Buf b;
  buf_init(&b, a, 2048);
  buf_printf(&b, "# flat profile: %d samples (of %d), %d unresolved\n", n - unknown, n, unknown);
  buf_puts(&b, "#   samples   percent   symbol\n");
  for (i32 i = 0; i < nsyms && i < 32; i++) {
    double pct = 100.0 * (double)syms[i].count / (double)n;
    buf_printf(&b, "%10d   %6.1f%%   %s\n", syms[i].count, pct, syms[i].name);
  }
  u64 missed = atomic_load(&g_missed);
  if (missed) buf_printf(&b, "# %llu samples dropped (buffer full)\n", (unsigned long long)missed);
  return buf_str(&b);
}