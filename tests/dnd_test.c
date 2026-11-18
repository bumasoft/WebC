// dnd: the drop index the pointer means, the no-op that must not become a move, and the
// past-the-end case that must not become a crash.
#include <webc/test.h>
#include <webc/dnd.h>

static WDrag drag(i32 n, i32 from) {
  WDrag d = dnd_new(n);
  d.from = from;
  return d;
}

TEST(dropping_on_itself_is_not_a_move) {
  WDrag d = drag(4, 2);
  d.over = 2;
  d.after = false;
  ASSERT_EQ(dnd_drop(&d), -1);
  d.after = true; // the row's own lower half is still its own place
  ASSERT_EQ(dnd_drop(&d), -1);
  ASSERT_FALSE(dnd_can(&d));
}

TEST(moving_down_lands_where_the_row_looked) {
  // [a,b,c,d], carry b (1) and drop it below d: [a,c,d,b] — b ends at index 3
  WDrag d = drag(4, 1);
  d.over = 3;
  d.after = true;
  ASSERT_EQ(dnd_drop(&d), 3);
  // and dropping it above c means "where c was": [a,b,c,d] is unchanged, so it is the index
  // c keeps once b is out — 1, and 1 is where b already is: the no-op that a naive index
  // says is a move
  d.over = 2;
  d.after = false; // "before c" while b is immediately before c: nowhere to go
  ASSERT_EQ(dnd_drop(&d), -1);
  ASSERT_FALSE(dnd_can(&d));
  // above d: [a,c,b,d] — index 2
  d.over = 3;
  d.after = false;
  ASSERT_EQ(dnd_drop(&d), 2);
}

TEST(moving_up_does_not_shift) {
  WDrag d = drag(4, 3);
  d.over = 1;
  d.after = false; // before item 1
  ASSERT_EQ(dnd_drop(&d), 1);
  d.after = true; // after item 1 is index 2
  ASSERT_EQ(dnd_drop(&d), 2);
}

TEST(past_the_end_is_the_last_index_not_a_crash) {
  WDrag d = drag(4, 0);
  d.over = 3;
  d.after = true; // below the last row: "move it to the end"
  ASSERT_EQ(dnd_drop(&d), 3);
}

TEST(the_first_slot_is_reachable) {
  WDrag d = drag(3, 2);
  d.over = 0;
  d.after = false;
  ASSERT_EQ(dnd_drop(&d), 0);
}

TEST(nothing_dragged_and_nothing_hovered_are_both_minus_one) {
  WDrag d = drag(3, -1);
  d.over = 1;
  d.after = false;
  ASSERT_EQ(dnd_drop(&d), -1);
  WDrag e = drag(3, 1);
  e.over = -1;
  ASSERT_EQ(dnd_drop(&e), -1);
}

TEST(take_reads_the_event_and_says_nothing_to_do) {
  // no element under the pointer in a test, so the row reports index 0; a drag that started
  // on row 0 therefore has nowhere to go, and says so rather than moving one place
  WDrag d = drag(3, 0);
  i32 from = -9, to = -9;
  ASSERT_FALSE(dnd_take(&d, &from, &to));
  ASSERT_EQ(from, 0);
  ASSERT_EQ(to, -1);
}

TEST(a_position_reads_as_one_of_n) {
  ASSERT_STR_EQ(dnd_position(2, 5), "3/5");
}

TEST_MAIN()
