// panic: a handler that blows up is a 500, not a dead worker or a dead process
// (SPEC §6.6 `recover`). The failure it pins: a setjmp() in a helper function never
// resumes the caller's frame, so the recovery branch is skipped and the stack is
// clobbered on the way back.
#include <webc/test.h>

static int frames_taken;

static Response boom(Request *req) {
  (void)req;
  panic("deliberate");
  return res_text(200, str_lit("unreachable"));
}

static Response alive(Request *req) {
  (void)req;
  return res_text(200, str_lit("alive"));
}

// Each test owns the router while it runs, so the order of tests cannot matter.
static void routes_with(Response (*handler)(Request *)) {
  webc_router_reset();
  webc_use_reset();
  webc_route_add("GET", "/probe", handler, nullptr, 0);
}

static void routes_done(void) {
  webc_use_reset();
  webc_router_reset();
}

TEST(handler_panic_becomes_500) {
  routes_with(boom);
  Response res = webc_test_request("GET", "/probe");
  routes_done();
  ASSERT_EQ(res.status, 500);
  ASSERT_STR_CONTAINS(res.body, "internal error");
}

TEST(mw_recover_turns_a_panic_into_a_500) {
  routes_with(boom);
  webc_use(mw_recover());
  Response res = webc_test_request("GET", "/probe");
  routes_done();
  ASSERT_EQ(res.status, 500);
  ASSERT_STR_CONTAINS(res.body, "internal error");
}

// A 500 that names its route. Without it a crash in a handler reads exactly like a crash in the
// scheduler — a backtrace, a task dump, no route — and the 500 body says "internal error" and
// nothing else, over an app with forty routes.
TEST(a_500_names_the_request_it_happened_in) {
  routes_with(boom);
  webc_use(mw_recover());
  Response res = webc_test_request("GET", "/probe");
  routes_done();
  ASSERT_EQ(res.status, 500);
  str body = {res.body.p, res.body.n};
  ASSERT_STR_CONTAINS(body, "internal error");
  ASSERT_STR_CONTAINS(body, "GET /probe");
  ASSERT_STR_CONTAINS(body, "route /probe");
  ASSERT_STR_CONTAINS(body, "id "); // the request id, which is what a log search starts from
}

TEST(the_router_keeps_answering_after_a_panic) {
  routes_with(boom);
  ASSERT_EQ(webc_test_request("GET", "/probe").status, 500);
  routes_with(alive);
  Response res = webc_test_request("GET", "/probe");
  routes_done();
  ASSERT_EQ(res.status, 200);
  ASSERT_STR_EQ(res.body, "alive");
}

TEST(a_panic_unwinds_to_the_nearest_frame) {
  // the frame chain has to come back out in the right order: an inner panic
  // must not pop an outer frame that nobody re-installed
  for (int i = 0; i < 3; i++) {
    wc_try_frame outer;
    if (wc_try_begin(&outer)) {
      wc_try_frame inner;
      if (wc_try_begin(&inner)) {
        panic("inner %d", i);
        wc_try_end(&inner);
        FAIL("the panic did not unwind");
      } else {
        frames_taken++;
      }
      wc_try_end(&outer);
      frames_taken += 10;
    } else {
      FAIL("the panic escaped to the outer frame");
    }
  }
  ASSERT_EQ(frames_taken, 33);
}

TEST(a_frame_can_be_reused_after_it_was_spent) {
  // the same wc_try_frame on the stack, panicked through three times
  wc_try_frame f;
  int caught = 0;
  for (int i = 0; i < 3; i++) {
    if (wc_try_begin(&f)) {
      panic("again %d", i);
      wc_try_end(&f);
    } else {
      caught++;
    }
  }
  ASSERT_EQ(caught, 3);
}

static WMutex g_dead_mutex = WEBC_MUTEX_INIT;
static long g_counted;

static void panicking_task(void *arg) {
  (void)arg;
  panic("background job failed");
}

static void lock_then_panic(void *arg) {
  (void)arg;
  wmutex_lock(&g_dead_mutex);
  panic("the holder died while holding it");
}

static void count_under_mutex(void *arg) {
  (void)arg;
  for (int i = 0; i < 100; i++) {
    wmutex_lock(&g_dead_mutex);
    g_counted++;
    wmutex_unlock(&g_dead_mutex);
  }
}

TEST(a_task_panic_is_contained_and_reported_by_its_handle) {
  // A panic in a background task must not abort the process: a crash reporter that walks a
  // ucontext stack kills the whole server over one worker. The task is what dies, and the
  // join says so.
  wtask_init(1);
  WTaskHandle *h = task_result(panicking_task, nullptr);
  TaskResult r = task_join(h);
  ASSERT_FALSE(r.ok);
  ASSERT_EQ((int)r.e.kind, (int)WEBC_EEXTERNAL);
  task_handle_free(h);
  wtask_shutdown();
}

TEST(a_task_that_dies_holding_a_mutex_gives_it_back) {
  // The measured hang: a task that panics between lock and unlock left the mutex held
  // forever, and the next waiter parked behind a task that no longer existed.
  wtask_init(2);
  task_async(lock_then_panic, nullptr);
  wg_sleep_ms(50);
  for (int i = 0; i < 2; i++) task_async(count_under_mutex, nullptr);
  wtask_wait();
  ASSERT_EQ(g_counted, 200);
  wtask_shutdown();
}

TEST_MAIN()
