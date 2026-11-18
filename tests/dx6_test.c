// dx6: computed resources (res_compute — fe.h).
//
// The app this came from ran 21 strategies over 10 000 flips inside a timer callback and
// blocked the page for a quarter of a second per step; the run it started deep reached
// 2 GiB of arena and froze. The shape that fixes it is not a thread and not a closure: a
// work function that returns when its time is up, resumes where it left off, and reports
// progress the view can draw. These tests hold the loop, the key and the progress.
//
// The native build runs a job to completion through the same pump — no special case — so
// what a test sees is what a browser sees, minus the yielding.
#include <webc/fe.h>
#include <webc/test.h>

typedef struct Sum {
  i32 rows;
  i64 total;
} Sum;

typedef struct Job1 {
  i32 cursor;
  i32 rows;
  i32 runs; // how many times the work started (a restart shows up here)
} Job1;

static Job1 g_job;
static WRes *g_res;

static void add_rows(WJob *j, void *ctx) {
  Job1 *s = ctx;
  Sum *out = job_out(j);
  s->runs++;
  for (i32 i = s->cursor; i < s->rows; i++) {
    if (!job_tick(j)) {
      job_steps(j, i, s->rows);
      s->cursor = i; // resume here next chunk
      return;
    }
    out->total += i;
    out->rows = i + 1;
    job_steps(j, i + 1, s->rows);
  }
  job_done(j);
}

static void reset_job(i32 rows) {
  g_job = (Job1){.rows = rows};
}

static void bounded_work(WJob *j, void *ctx) {
  Job1 *s = ctx;
  Sum *out = job_out(j);
  s->runs++;
  for (i32 i = s->cursor; i < s->rows; i++) {
    if (!job_tick(j)) {
      job_steps(j, i, s->rows);
      s->cursor = i;
      return;
    }
    out->total += i;
    out->rows = i + 1;
    job_steps(j, i + 1, s->rows);
  }
  job_done(j);
}

TEST(a_computed_resource_fills_its_data_and_settles) {
  wc_job_budget_ms(1000); // one chunk for the whole job
  reset_job(100);
  g_res = res_compute(nullptr, add_rows, &g_job, sizeof(Sum));
  ASSERT_TRUE(g_res != nullptr);
  ASSERT_TRUE(res_idle(g_res)); // nothing asked yet

  res_ensure_key(g_res, 7);
  ASSERT_TRUE(res_ok(g_res));
  ASSERT_TRUE(!res_loading(g_res));
  Sum *s = (Sum *)res_data(g_res);
  ASSERT_EQ(s->rows, 100);
  ASSERT_EQ(s->total, 4950); // 0+1+…+99
  ASSERT_EQ(res_progress(g_res), 100);
  res_destroy(g_res);
}

TEST(chunks_resume_where_they_stopped) {
  // 1 ms of budget and 4000 rows: the work cannot finish in one chunk, so the pump comes
  // back, and the total is still exact — a resumption that restarted would double-count.
  wc_job_budget_ms(0); // one step per chunk: every chunk boundary is under test
  reset_job(4000);
  g_res = res_compute(nullptr, bounded_work, &g_job, sizeof(Sum));
  res_ensure_key(g_res, 1);
  ASSERT_TRUE(res_ok(g_res));
  Sum *s = (Sum *)res_data(g_res);
  ASSERT_EQ(s->rows, 4000);
  ASSERT_EQ(s->total, 4000LL * 3999 / 2);
  ASSERT_EQ(g_job.runs, 4000); // one step per chunk: the same function, re-entered
  res_destroy(g_res);
}

TEST(the_key_is_what_says_run_again) {
  wc_job_budget_ms(1000);
  reset_job(10);
  g_res = res_compute(nullptr, add_rows, &g_job, sizeof(Sum));
  res_ensure_key(g_res, 42);
  i32 runs_once = g_job.runs;
  res_ensure_key(g_res, 42); // same key: not again
  ASSERT_EQ(g_job.runs, runs_once);
  res_ensure_key(g_res, 43); // a new key: again, from the start
  ASSERT_EQ(g_job.runs, runs_once + 1);
  ASSERT_TRUE(res_ok(g_res));
  // and invalidate() forces the next one even when the key did not move
  res_invalidate(g_res);
  res_ensure_key(g_res, 43);
  ASSERT_EQ(g_job.runs, runs_once + 2);
  res_destroy(g_res);
}

TEST(a_restart_does_not_keep_the_old_answer) {
  wc_job_budget_ms(1000);
  reset_job(10);
  g_res = res_compute(nullptr, add_rows, &g_job, sizeof(Sum));
  res_ensure_key(g_res, 1);
  ASSERT_EQ(((Sum *)res_data(g_res))->total, 45);
  ASSERT_TRUE(res_ok(g_res));
  res_invalidate(g_res);
  ASSERT_TRUE(!res_ok(g_res)); // invalidated is not "still the old answer"
  g_job.cursor = 0;
  res_ensure_key(g_res, 1);
  ASSERT_EQ(((Sum *)res_data(g_res))->total, 45);
  res_destroy(g_res);
}

TEST(progress_reads_the_steps_the_work_reports) {
  wc_job_budget_ms(1000);
  reset_job(200);
  g_res = res_compute(nullptr, add_rows, &g_job, sizeof(Sum));
  res_ensure_key(g_res, 3);
  ASSERT_EQ(res_progress(g_res), 100);
  res_destroy(g_res);
}

TEST(progress_is_a_percentage_the_view_can_draw) {
  wc_job_budget_ms(0);
  reset_job(400);
  g_res = res_compute(nullptr, bounded_work, &g_job, sizeof(Sum));
  res_ensure_key(g_res, 9);
  ASSERT_TRUE(res_ok(g_res));
  ASSERT_EQ(res_progress(g_res), 100);
  reset_job(400);
  res_invalidate(g_res);
  res_ensure_key(g_res, 10);
  ASSERT_TRUE(res_ok(g_res)); // the native build finishes within the call
  res_destroy(g_res);
}

TEST(a_job_leaves_nothing_behind) {
  wc_job_budget_ms(1000);
  reset_job(5);
  g_res = res_compute(nullptr, add_rows, &g_job, sizeof(Sum));
  res_ensure_key(g_res, 1);
  ASSERT_TRUE(!wc_jobs_alive()); // settled jobs are not "alive"
  res_destroy(g_res);
}

TEST_MAIN()