// jobs: enqueue is a row, a failure backs off and comes back, and the last attempt is final.
#include <webc/test.h>
#include <webc/jobs.h>

WEBC_MODEL(Line, (id, i64, .pk = true), (title, str))

static i32 g_runs;
static str g_fail_with;

WEBC_JOB(count_me, Line) {
  g_runs++;
  if (g_fail_with.p) *err = g_fail_with;
  (void)r;
}

WEBC_JOB(no_arg_job) {
  g_runs++;
  if (g_fail_with.p) *err = g_fail_with;
}

// Each test gets its own database and an empty queue: `jobs` is a table, so a test that shared
// one with another would be testing the order the suites run in.
// One database for the suite, and an empty queue between tests: a test that inherited another
// test's rows would be testing the order the suites run in.
static void boot(void);
TEST_SETUP(boot);
static void boot(void) {
  db_set_default(db_open("sqlite::memory:"));
  wc_job_install();
}

static void fresh(void) {
  g_runs = 0;
  g_fail_with = (str){0};
  job_retry(count_me, 3, 30000);
  job_retry(no_arg_job, 3, 30000);
  db_exec_sql(db_default(), "delete from wjobs");
}

TEST(a_job_is_a_registered_name) {
  fresh();
  ASSERT_TRUE(wc_job_find("count_me") != nullptr);
  ASSERT_TRUE(wc_job_find("no_arg_job") != nullptr);
  ASSERT_TRUE(wc_job_find("never_declared") == nullptr);
  ASSERT_TRUE(wc_job_kinds() >= 2);
}

TEST(the_payload_round_trips_through_the_model) {
  fresh();
  Line l = {.id = 41, .title = str_lit("invoice")};
  ASSERT_TRUE(count_me_do(&l));
  ASSERT_EQ(wc_job_pending(), 1);
  bool ran = wc_job_tick();
  ASSERT_TRUE(ran);
  ASSERT_EQ(g_runs, 1);
  ASSERT_EQ(wc_job_pending(), 0);
}

TEST(a_failure_backs_off_and_later_attempts_are_capped) {
  fresh();
  g_fail_with = str_lit("relay said no");
  job_retry(count_me, 2, 1);            // two attempts, 1 ms of backoff
  Line l = {.id = 1, .title = str_lit("x")};
  ASSERT_TRUE(count_me_do(&l));
  ASSERT_TRUE(wc_job_tick());           // attempt 1 fails
  ASSERT_EQ(g_runs, 1);
  time_sleep_ms(5);                     // past the backoff
  ASSERT_TRUE(wc_job_tick());           // attempt 2 fails: last one
  ASSERT_EQ(g_runs, 2);
  ASSERT_FALSE(wc_job_tick());          // nothing queued: it is failed, not retried
  VecWJob *f = wc_job_failures(10);
  ASSERT_EQ(f->n, 1);
  ASSERT_STR_EQ(f->at[0].last_error, "relay said no");
  ASSERT_EQ(f->at[0].attempts, 2);
}

TEST(a_success_clears_the_error_and_marks_it_done) {
  fresh();
  Line l = {.id = 2, .title = str_lit("y")};
  ASSERT_TRUE(count_me_do(&l));
  ASSERT_TRUE(wc_job_tick());
  VecWJob *f = wc_job_failures(10);
  ASSERT_EQ(f->n, 0);
  ASSERT_EQ(wc_job_pending(), 0);
}

TEST(a_job_that_is_not_due_waits) {
  fresh();
  Line l = {.id = 3, .title = str_lit("z")};
  ASSERT_TRUE(count_me_later(&l, 60000));
  ASSERT_FALSE(wc_job_tick()); // a minute from now is not now
  ASSERT_EQ(g_runs, 0);
  ASSERT_EQ(wc_job_pending(), 1);
}

TEST(a_job_that_takes_no_argument_has_a_verb_of_its_own) {
  fresh();
  ASSERT_TRUE(no_arg_job_do());
  ASSERT_TRUE(wc_job_tick());
  ASSERT_EQ(g_runs, 1);
}

TEST(a_requeue_puts_a_failed_job_back) {
  fresh();
  g_fail_with = str_lit("nope");
  job_retry(no_arg_job, 1, 1);
  ASSERT_TRUE(no_arg_job_do());
  ASSERT_TRUE(wc_job_tick());
  VecWJob *f = wc_job_failures(10);
  ASSERT_EQ(f->n, 1);
  wc_job_requeue_failed(f->at[0].id);
  g_fail_with = (str){0};
  ASSERT_TRUE(wc_job_tick());
  ASSERT_EQ(g_runs, 2);
}

TEST(a_kind_that_was_removed_fails_loudly_instead_of_vanishing) {
  fresh();
  db_exec_sql(db_default(),
              "insert into wjobs (kind, status, attempts, max_attempts, run_at, lease_until, created_at)"
              " values ('gone_job', 'queued', 0, 1, 0, 0, 0)");
  ASSERT_TRUE(wc_job_tick());
  VecWJob *f = wc_job_failures(10);
  ASSERT_EQ(f->n, 1);
  ASSERT_STR_EQ(f->at[0].last_error, "no job with that name is registered");
}

TEST_MAIN()
