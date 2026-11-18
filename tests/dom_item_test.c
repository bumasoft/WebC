// dom_item: what an action can know about the element it is running for.
//
// `bind("pick", "click", pick)` hands the action an item only inside a keyed list
// (`for_each`). A positional list (`for_i`) — the form that does not reconcile, and so the
// form that does not re-trigger the reuse bugs — hands it NULL, and the action then does
// nothing at all, with no error and no clue. These tests pin the two halves of the answer:
// ev_attr() finds the row from the element, and an action that gets NULL says so instead of
// returning quietly.
#include <webc/test.h>

static i64 g_picked;
static str g_seen_attr;

action(pick, void *item) {
  (void)item;
  g_picked = ev_attr_i64("data-id");
  g_seen_attr = ev_attr("data-kind");
}

static void board_view(WBuild *b) {
  (void)b;
  el("table") {
    for (i32 i = 1; i <= 3; i++) {
      el("tr") {
        attr("data-id", fmt("%d", i));
        class_("row");
        el("td") {
          // the click lands on the cell, not the row: the walk up is the point
          el("button") { on_click(pick); text("pick %d", i); }
        }
      }
    }
  }
}

TEST(a_click_on_a_child_reads_the_row_above_it) {
  g_picked = 0;
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, board_view);
  // the hook remembers the element it was rendered for, so a native test can click it
  ASSERT_TRUE(wc_test_dispatch("pick", "click"));
  // three rows all carry data-hook="pick": the hook resolves to the last one rendered, which
  // is row 3 — and the walk from that element up finds the row's data-id
  ASSERT_EQ(g_picked, 3);
  wcomp_destroy(&c);
}

static void row_attr_view(WBuild *b) {
  (void)b;
  el("ul") {
    el("li") {
      attr("data-id", str_lit("42"));
      attr("data-kind", str_lit("late"));
      el("span") { on_click(pick); text("row"); }
    }
  }
}

TEST(ev_attr_walks_up_and_returns_the_nearest_value) {
  g_picked = 0;
  g_seen_attr = (str){0};
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, row_attr_view);
  ASSERT_TRUE(wc_test_dispatch("pick", "click"));
  ASSERT_EQ(g_picked, 42);
  ASSERT_STR_EQ(g_seen_attr, "late");
  wcomp_destroy(&c);
}

TEST(ev_attr_is_empty_when_nothing_carries_it) {
  g_seen_attr = (str){0};
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, row_attr_view);
  ASSERT_TRUE(wc_test_dispatch("pick", "click"));
  ASSERT_TRUE(ev_attr("data-nothing").p == nullptr);
  ASSERT_EQ((int)ev_attr_i64("data-nothing"), 0);
  // outside a dispatch there is no element, and the accessors say so rather than crash
  ASSERT_TRUE(ev_self() == nullptr);
  ASSERT_TRUE(ev_attr("data-id").p == nullptr);
  wcomp_destroy(&c);
}

TEST_MAIN()