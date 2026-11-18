// SPDX-License-Identifier: Apache-2.0
// The test runner behind <webc/test.h> (SPEC §6.7).
//
// Tests register themselves with a constructor, so a file is a list of TEST
// blocks and nothing has to be enumerated. Output is go test's shape, and one
// failing or panicking test never takes the suite down.
#include <webc/test.h>
#include <webc/relations.h>

#include <webc/react.h> // wc_mount_pending/wc_mount_clear: a queued on_mount that never ran

#include <stdarg.h>
#include <time.h>

#define WC_TEST_MAX 512
#define WC_DETAIL_MAX 4096

typedef struct {
  const char *name;
  WcTestFn fn;
} WTestEntry;

static WTestEntry g_entries[WC_TEST_MAX];
static int g_nentries;

// where we are: the test and subtest that is running, and its verdict so far
static const char *g_cur; // current test name
static const char *g_sub; // current subtest label, or null
static int g_cur_failed;
static int g_cur_skipped;
static const char *g_cur_skip_why;

// verdict details are buffered so they print under their verdict line, as go does
static char g_detail[WC_DETAIL_MAX];
static size_t g_detail_n;

// running totals
static int g_passed, g_failed, g_skipped;

static void (*g_setup)(void);
static void (*g_teardown)(void);
static const char *g_setup_panic;

void wc_test_setup_fn(void (*fn)(void)) {
  g_setup = fn;
}
void wc_test_teardown_fn(void (*fn)(void)) {
  g_teardown = fn;
}

void wc_test_register(const char *name, WcTestFn fn) {
  if (g_nentries >= WC_TEST_MAX) {
    fprintf(stderr, "webc: more than %d tests in one binary\n", WC_TEST_MAX);
    abort();
  }
  g_entries[g_nentries++] = (WTestEntry){.name = name, .fn = fn};
}

// ---------------------------------------------------------------- output

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void detail_clear(void) {
  g_detail_n = 0;
}

static void detail_add(const char *file, int line, const char *fmt, va_list ap) {
  if (g_detail_n >= sizeof g_detail - 1) return;
  const char *base = file ? strrchr(file, '/') : nullptr;
  base = base ? base + 1 : file;
  int n = snprintf(g_detail + g_detail_n, sizeof g_detail - g_detail_n, "        ");
  if (n > 0) g_detail_n += (size_t)n;
  if (base && g_detail_n < sizeof g_detail - 1) {
    n = snprintf(g_detail + g_detail_n, sizeof g_detail - g_detail_n, "%s:%d: ", base, line);
    if (n > 0) g_detail_n += (size_t)n;
  }
  if (g_detail_n < sizeof g_detail - 1) {
    n = vsnprintf(g_detail + g_detail_n, sizeof g_detail - g_detail_n, fmt, ap);
    if (n > 0)
      g_detail_n +=
          ((size_t)n < sizeof g_detail - g_detail_n) ? (size_t)n : sizeof g_detail - g_detail_n - 2;
  }
  if (g_detail_n < sizeof g_detail - 1) g_detail[g_detail_n++] = '\n';
  g_detail[g_detail_n] = '\0';
}

static void verdict(const char *name, bool ok, bool skipped, double secs) {
  if (skipped)
    printf("  --- SKIP: %s (%s)\n", name, g_cur_skip_why ? g_cur_skip_why : "skipped");
  else
    printf("  --- %s: %s (%.2fs)\n", ok ? "PASS" : "FAIL", name, secs);
  if (g_detail_n) {
    fputs(g_detail, stdout);
    detail_clear();
  }
}

// ---------------------------------------------------------------- assertions

void wc_test_expect(bool ok, const char *file, int line, const char *fmt, ...) {
  if (ok) return;
  g_cur_failed++;
  va_list ap;
  va_start(ap, fmt);
  detail_add(file, line, fmt, ap);
  va_end(ap);
}

void wc_test_failf(const char *file, int line, const char *fmt, ...) {
  g_cur_failed++;
  va_list ap;
  va_start(ap, fmt);
  detail_add(file, line, fmt, ap);
  va_end(ap);
}

void wc_test_skip(const char *file, int line, const char *why) {
  (void)file;
  (void)line;
  g_cur_skipped = 1;
  g_cur_skip_why = why;
}

const char *wc_test_str(str s) {
  static _Thread_local char buf[512];
  if (!s.p) return "";
  size_t n = s.n < sizeof buf - 1 ? s.n : sizeof buf - 4;
  memcpy(buf, s.p, n);
  if (n < s.n) memcpy(buf + n, "...", 3), n += 3;
  buf[n] = '\0';
  return buf;
}

// ---------------------------------------------------------------- subtests

static char g_label[256];

static const char *joined_label(void) {
  if (!g_sub) return g_cur ? g_cur : "test";
  snprintf(g_label, sizeof g_label, "%s/%s", g_cur ? g_cur : "test", g_sub);
  return g_label;
}

static int g_sub_failed_at_entry;
static double g_sub_t0;
static bool g_in_sub;

void wc_test_sub_begin(const char *label) {
  g_sub = label ? label : "sub";
  g_in_sub = true;
  g_sub_failed_at_entry = g_cur_failed;
  g_sub_t0 = now_s();
}

void wc_test_sub_end(void) {
  if (!g_in_sub) return;
  g_in_sub = false;
  bool ok = g_cur_failed == g_sub_failed_at_entry;
  verdict(joined_label(), ok, g_cur_skipped, now_s() - g_sub_t0);
  g_sub = nullptr;
}

// ---------------------------------------------------------------- runner

// A filter is a substring of the test name, like `go test -run`. `webc test
// --filter` sets it in the environment, so a suite needs no plumbing to be
// runnable one test at a time.
static bool filtered_out(const char *name) {
  static const char *pat;
  static bool read;
  if (!read) {
    read = true;
    pat = getenv("WEBC_TEST_FILTER");
  }
  if (!pat || !*pat) return false;
  return strstr(name, pat) == nullptr;
}

// Boot the app the way the server does, once, unless the suite said how it wants it. The
// point is that a test constructs the world the same way the server does — and that the ten
// lines doing it are in the framework rather than at the top of every suite.
__attribute__((weak)) void webc_boot(void) {}

void wc_test_boot(void) {
  static bool booted;
  if (booted) return;
  booted = true;
  // `WEBC_TEST_DB=postgres://… webc test` is the documented way to run a suite against a real
  // server, and it has to be decided *here*: an app that opens its database in `webc_boot()` (so
  // migrations run before the first request) has already opened the in-memory default by the time
  // a TEST_SETUP gets control, and the suite then passes against a database nobody meant.
  if (!getenv("DATABASE_URL"))
    setenv("DATABASE_URL", getenv("WEBC_TEST_DB") ? getenv("WEBC_TEST_DB") : "sqlite::memory:", 1);
  webc_boot();
}

int wc_test_run(void) {
  g_passed = g_failed = g_skipped = 0;

  // A test runs the same code a request runs, so it gets the same ambient arena:
  // without one, anything that allocates for its result (a filtered collection, the
  // text of a matched element) has nowhere to put it and returns null. One arena for
  // the whole run, reset per test so a suite cannot exhaust it.
  static Arena *app;
  if (!app) {
    app = arena_new_labeled(2 * 1024 * 1024, "tests");
    wc_app_arena_set(app);
    wc_arena_swap(app); // the *current* arena too: that is what wc_arena_get() returns
  }

  if (g_setup) {
    wc_try_frame frame;
    if (wc_try_begin(&frame)) {
      g_setup();
      wc_try_end(&frame);
    } else {
      // every test fails, with the setup's message, instead of the binary dying
      g_setup_panic = wc_panic_message();
      for (int i = 0; i < g_nentries; i++) {
        g_failed++;
        g_detail_n = 0;
        snprintf(g_detail, sizeof g_detail, "        setup panicked: %s\n",
                 g_setup_panic ? g_setup_panic : "(no message)");
        g_detail_n = strlen(g_detail);
        verdict(g_entries[i].name, false, false, 0.0);
      }
      printf("  setup failed: %s\n", g_setup_panic ? g_setup_panic : "(no message)");
      return g_failed;
    }
  }
  for (int i = 0; i < g_nentries; i++) {
    if (filtered_out(g_entries[i].name)) continue;
    arena_reset(app);
    // a test starts with no relations linked: the arena is reused at the same address, and a
    // collection from the previous test must not be found through it
    wc_ref_clear();
    wc_row_pool_clear();
    // Every state back to the value it was declared with, so a test starts in the same
    // world as the one before it. This is what replaces the "leave the app on the compare
    // tab" line at the end of a test that clicks something.
    w_state_rewind();
    g_cur = g_entries[i].name;
    g_sub = nullptr;
    g_cur_failed = 0;
    g_cur_skipped = 0;
    g_cur_skip_why = nullptr;
    wc_err_pending = 0; // a test must not inherit another test's pending error
    detail_clear();
    double t0 = now_s();

    // A panicking test (a failing `try`, a wc_assert, a framework bug) is a
    // failed test, not a dead suite: the frame catches it and we carry on.
    wc_try_frame frame;
    if (wc_try_begin(&frame)) {
      g_entries[i].fn();
      wc_try_end(&frame);
    } else {
      g_cur_failed++;
      const char *msg = wc_panic_message();
      // the panic already printed its own detail line in most cases; this one
      // says which test it belonged to
      g_detail_n = 0;
      snprintf(g_detail, sizeof g_detail, "        panic: %s\n", msg ? msg : "(no message)");
      g_detail_n = strlen(g_detail);
      // the unwound frame took the subtest bookkeeping with it
      g_in_sub = false;
      g_sub = nullptr;
    }

    // A queued `on_mount` that is still queued here never ran: the test drew a position and
    // never let the batch go out. Dropping it quietly is the failure this runner exists to
    // stop — the assertion the test forgot to write is usually about the node the hook was
    // going to touch (`focus()`, a measurement), so the next test starts with an empty queue
    // and this one is told.
    if (wc_mount_pending() > 0) {
      if (g_detail_n < (int)sizeof g_detail) {
        int w = snprintf(g_detail + g_detail_n, sizeof g_detail - (size_t)g_detail_n,
                         "        %d on_mount hook(s) still queued when the test ended: "
                         "nothing ran them — call wc_mount_run() (or wc_runtime_flush())\n",
                         (int)wc_mount_pending());
        if (w > 0) g_detail_n += (int)w < (int)(sizeof g_detail - (size_t)g_detail_n)
                                     ? w
                                     : (int)(sizeof g_detail - (size_t)g_detail_n);
      }
      g_cur_failed++;
    }
    wc_mount_clear();

    if (g_cur_failed == 0) {
      if (g_cur_skipped) {
        g_skipped++;
        verdict(g_cur, true, true, now_s() - t0);
      } else {
        g_passed++;
        verdict(g_cur, true, false, now_s() - t0);
      }
    } else {
      g_failed++;
      verdict(g_cur, false, false, now_s() - t0);
    }
  }
  g_cur = nullptr;

  if (g_teardown) {
    wc_try_frame frame;
    if (wc_try_begin(&frame)) {
      g_teardown();
      wc_try_end(&frame);
    } else {
      printf("  teardown panicked: %s\n", wc_panic_message());
      g_failed++;
    }
  }

  if (g_passed + g_failed + g_skipped == 0) {
    // Two different emptinesses: a file with no TEST blocks is a mistake, a name filter that
    // matched nothing is an answer. Only the first is a failure.
    const char *filter = getenv("WEBC_TEST_FILTER");
    if (filter && *filter) {
      printf("  no test matched \"%s\" in this suite\n", filter);
      return 0;
    }
    printf("  no tests (a test file needs TEST(name) blocks and TEST_MAIN())\n");
    return 1;
  }
  printf("  %d passed, %d failed", g_passed, g_failed);
  if (g_skipped) printf(", %d skipped", g_skipped);
  printf("\n");
  return g_failed;
}