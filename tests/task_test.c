// scheduler: spawn/join, channels, select, parallel work, watchers.
#include <webc/test.h>

static void setup(void) { wtask_init(1); }
static void teardown(void) { wtask_shutdown(); }
TEST_SETUP(setup);
TEST_TEARDOWN(teardown);

static _Atomic i64 g_sum;

static void adder(void *arg) {
  i64 n = (i64)(intptr_t)arg;
  i64 s = 0;
  for (i64 i = 1; i <= n; i++) s += i;
  atomic_fetch_add(&g_sum, s);
}

static void producer(void *arg) {
  ChanInt ch = *(ChanInt *)arg;   // channels are handles: copy them freely
  for (i32 i = 0; i < 5; i++) chan_int_send(ch, i);
  chan_int_close(ch);
}

static WMutex g_mu = WEBC_MUTEX_INIT;
static long g_shared;

static void contend(void *arg) {
  (void)arg;
  for (int i = 0; i < 2000; i++) {
    wmutex_lock(&g_mu);
    long v = g_shared;
    if ((i & 15) == 0) wg_yield();   // yields while holding the lock
    g_shared = v + 1;
    wmutex_unlock(&g_mu);
  }
}

typedef struct PingPong {
  ChanInt in, out;
} PingPong;

static void pingpong(void *arg) {
  PingPong *pp = arg;
  for (;;) {
    i32 v = 0;
    if (!chan_int_recv(pp->in, &v)) return;   // closed: stop
    if (!chan_int_send(pp->out, v + 1)) return;
  }
}

TEST(spawn_and_join) {
  g_sum = 0;
  task_async(adder, (void *)(intptr_t)1000);
  task_async(adder, (void *)(intptr_t)10);
  wtask_wait();
  ASSERT_EQ(g_sum, 500500 + 55);
  ASSERT_EQ(wtask_live(), 0);
}

TEST(channels_carry_values_and_close) {
  ChanInt ch = chan_int_new(4);
  task_async(producer, &ch);
  i32 total = 0, v = 0, got = 0;
  while (chan_int_recv(ch, &v)) {
    total += v;
    got++;
  }
  ASSERT_EQ(got, 5);
  ASSERT_EQ(total, 10);
  chan_int_close(ch);
  wtask_wait();
}

TEST(select_picks_the_ready_case) {
  i32 v = 0;
  ChanInt a = chan_int_new(1), b = chan_int_new(1);
  chan_int_send(b, 42);
  SelCase cases[] = {
    { .ch = a.c, .out = &v },
    { .ch = b.c, .out = &v },
  };
  i32 idx = select_wait(cases, 2, 0);
  ASSERT_EQ(idx, 1);
  ASSERT_EQ(v, 42);
  chan_int_close(a);
  chan_int_close(b);
}

TEST(a_contended_mutex_loses_nothing) {
  g_shared = 0;
  for (int i = 0; i < 8; i++) task_async(contend, nullptr);
  wtask_wait();
  ASSERT_EQ(g_shared, 16000);
}

TEST(a_channel_passed_back_and_forth) {
  PingPong pp = { .in = chan_int_new(1), .out = chan_int_new(1) };
  task_async(pingpong, &pp);
  i32 seen = 0;
  for (i32 i = 0; i < 200; i++) {
    chan_int_send(pp.in, i);
    chan_int_recv(pp.out, &seen);
  }
  ASSERT_EQ(seen, 200);
  chan_int_close(pp.in);
  chan_int_close(pp.out);
  wtask_wait();
}

TEST(nothing_is_left_running) {
  ASSERT_EQ(wtask_live(), 0);
}

TEST_MAIN()