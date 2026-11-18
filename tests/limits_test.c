// limits: N hits a window, the window reopens on time, and a limiter that is full does not grow.
#include <webc/test.h>
#include <webc/limits.h>

static void settle(void) { rate_reset(); }

TEST(a_budget_is_spent_one_hit_at_a_time) {
  settle();
  for (i32 i = 0; i < 5; i++) ASSERT_TRUE(rate_hit("login", 5, 60000));
  ASSERT_FALSE(rate_hit("login", 5, 60000));
  // a different key has its own budget
  ASSERT_TRUE(rate_hit("reset", 5, 60000));
}

TEST(a_window_reopens_after_its_time) {
  settle();
  ASSERT_TRUE(rate_hit("x", 1, 30));
  ASSERT_FALSE(rate_hit("x", 1, 30));
  time_sleep_ms(45);
  ASSERT_TRUE(rate_hit("x", 1, 30));
}

TEST(no_limit_configured_is_never_a_refusal) {
  settle();
  for (i32 i = 0; i < 100; i++) ASSERT_TRUE(rate_hit("x", 0, 0));
}

TEST(the_wait_says_how_long_the_window_has_left) {
  settle();
  ASSERT_TRUE(rate_hit("w", 1, 200));
  ASSERT_FALSE(rate_hit("w", 1, 200));
  i64 wait = rate_wait_ms("w", 200);
  ASSERT_TRUE(wait > 0 && wait <= 200);
}

TEST(the_table_reuses_its_oldest_slot_instead_of_growing) {
  settle();
  // 256 slots; 600 keys must all land somewhere and none may be lost
  for (i32 i = 0; i < 600; i++) {
    char key[32];
    snprintf(key, sizeof key, "k%d", i);
    ASSERT_TRUE(rate_hit(key, 2, 60000));
  }
  // the newest key is still remembered (its second hit lands on the same slot) and the table
  // never grew: 600 keys, 256 slots, no allocation
  ASSERT_TRUE(rate_hit("k599", 2, 60000));
  ASSERT_FALSE(rate_hit("k599", 2, 60000));
  ASSERT_EQ(wc_rate()->hits, 1);
}

TEST(an_empty_key_is_not_a_crash) {
  settle();
  ASSERT_TRUE(rate_hit("", 1, 1000));
}

TEST_MAIN()
