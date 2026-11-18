// M4's claim: 100k concurrent sleeping tasks in <= 64 MB.
#include <webc/server.h>
#include <webc/web.h>
#include <sys/resource.h>

static _Atomic i64 done;

static void sleeper(void *arg) {
  (void)arg;
  wc_sleep_ms(10);
  atomic_fetch_add(&done, 1);
}

// Darwin reports ru_maxrss in bytes, Linux in kilobytes.
static long rss_mb(void) {
  struct rusage u;
  getrusage(RUSAGE_SELF, &u);
#if defined(__APPLE__)
  return (long)(u.ru_maxrss / (1024 * 1024));
#else
  return (long)(u.ru_maxrss / 1024);
#endif
}

int main(int argc, char **argv) {
  const i64 N = argc > 1 ? atoll(argv[1]) : 100000;
  if (argc > 2) wtask_set_stack_size((size_t)atol(argv[2]) * 1024);
  long before = rss_mb();
  int np = getenv("WEBC_CPUS") ? atoi(getenv("WEBC_CPUS")) : 8;
  wtask_init(np);
  double t0 = wc_wall_ms();
  for (i64 i = 0; i < N; i++) task_async(sleeper, nullptr);
  wtask_wait();
  double ms = wc_wall_ms() - t0;
  long after = rss_mb();
  printf("%lld tasks x 10 ms sleep, %zu KiB stacks: %8.0f ms wall, peak RSS %ld MB (+%ld MB)\n",
         (long long)N, wtask_stack_size() / 1024, ms, after, after - before);
  printf("completed                  %lld\n", (long long)done);
  wtask_shutdown();
  return done == N ? 0 : 1;
}
