// table: order without ownership, and the three-state sort.
//
// The valuable assertions here are the ones a hand-rolled table gets wrong: that a tie
// keeps the app's own order, that the third click returns home, and that a row's index in
// the app's array survives the sort (that index is what a delete button posts).
#include <webc/test.h>
#include <webc/table.h>

typedef struct Row {
  const char *name;
  i64 bank;
} Row;

static const Row ROWS[] = {
    {"Ada", 10}, {"Bo", 40}, {"Cy", 10}, {"Di", 25}, {"Ed", 5},
};

static f64 score_name(const void *r) { return (f64)strlen(((const Row *)r)->name); }
static f64 score_bank(const void *r) { return (f64)((const Row *)r)->bank; }

static const WTableKey KEYS[] = {
    {"Name", score_name, nullptr},
    {"Bank", score_bank, "right"},
};

static i32 g_order[WC_ARRLEN(ROWS)];

static void setup(WTable *t, i64 sort, i32 page, i32 per) {
  table_init(t, ROWS, WC_ARRLEN(ROWS), sizeof ROWS[0], g_order);
  table_keys(t, KEYS, WC_ARRLEN(KEYS));
  table_state(t, sort, page, per);
}

TEST(the_un_sorted_table_is_the_app_s_own_order) {
  WTable t;
  setup(&t, 0, 0, 0);
  ASSERT_EQ(table_len(&t), 5);
  ASSERT_EQ(table_at(&t, 0), 0);
  ASSERT_EQ(table_at(&t, 4), 4);
  ASSERT_STR_EQ(table_row_typed(Row, &t, 0)->name, "Ada");
}

TEST(a_sort_is_stable_so_ties_keep_the_author_s_order) {
  WTable t;
  setup(&t, 2, 0, 0); // bank ascending
  // Ed 5, then Ada 10, Cy 10 (Ada before Cy: the array's order), Di 25, Bo 40
  ASSERT_STR_EQ(table_row_typed(Row, &t, 0)->name, "Ed");
  ASSERT_STR_EQ(table_row_typed(Row, &t, 1)->name, "Ada");
  ASSERT_STR_EQ(table_row_typed(Row, &t, 2)->name, "Cy");
  ASSERT_STR_EQ(table_row_typed(Row, &t, 3)->name, "Di");
  ASSERT_STR_EQ(table_row_typed(Row, &t, 4)->name, "Bo");
}

TEST(descending_is_the_negative_key) {
  WTable t;
  setup(&t, -2, 0, 0);
  ASSERT_STR_EQ(table_row_typed(Row, &t, 0)->name, "Bo");
  ASSERT_STR_EQ(table_row_typed(Row, &t, 4)->name, "Ed");
}

TEST(the_index_travels_with_the_row) {
  WTable t;
  setup(&t, -2, 0, 0);
  // Bo is the second row of the array (index 1) and the first of this view
  ASSERT_EQ(table_at(&t, 0), 1);
  ASSERT_EQ(table_at(&t, 4), 4);
  ASSERT_STR_EQ(ROWS[table_at(&t, 0)].name, "Bo");
}

TEST(the_third_click_goes_home) {
  i64 s = 0;
  s = table_click(0, s); // "Name" clicked: ascending
  ASSERT_EQ(s, 1);
  s = table_click(0, s); // again: descending
  ASSERT_EQ(s, -1);
  s = table_click(0, s); // again: the app's own order, not a toggle
  ASSERT_EQ(s, 0);
  // a different column starts ascending, without passing through descending
  ASSERT_EQ(table_click(1, 1), 2);
}

TEST(pages_are_counted_and_clamped) {
  WTable t;
  setup(&t, 2, 0, 2);
  ASSERT_EQ(table_pages(&t), 3);
  ASSERT_EQ(table_len(&t), 2);
  ASSERT_STR_EQ(table_row_typed(Row, &t, 0)->name, "Ed");
  table_state(&t, 2, 2, 2);
  ASSERT_EQ(table_len(&t), 1); // the last page holds the remainder
  ASSERT_STR_EQ(table_row_typed(Row, &t, 0)->name, "Bo");
  ASSERT_EQ(table_page(&t, 99), 2);
  ASSERT_EQ(table_page(&t, -3), 0);
}

TEST(a_page_of_nothing_is_empty_not_a_crash) {
  WTable t;
  setup(&t, 0, 7, 2);
  ASSERT_EQ(table_len(&t), 0);
  ASSERT_NULL(table_row(&t, 0));
  ASSERT_EQ(table_at(&t, 0), -1);
}

TEST(an_unknown_sort_key_leaves_the_order_alone) {
  WTable t;
  setup(&t, 9, 0, 0);
  ASSERT_EQ(table_at(&t, 0), 0);
  ASSERT_STR_EQ(table_row_typed(Row, &t, 0)->name, "Ada");
}

// ---------------------------------------------------------------- markup

static WTable g_t;

static str cmds_of(void (*v)(WBuild *)) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, v);
  return buf_str(&c.cmds.b);
}

action(sort_by) { (void)ev_attr_i64("data-key"); }
action(page_flip) { (void)ev_attr_i64("data-step"); }

static void head_view(WBuild *b) {
  (void)b;
  setup(&g_t, -2, 0, 2);
  table_head(&g_t, sort_by);
}

static void pager_view(WBuild *b) {
  (void)b;
  setup(&g_t, 0, 0, 2);
  table_pager(&g_t, page_flip, "Players");
}

TEST(the_sorted_column_says_so_and_the_others_say_none) {
  str cmds = cmds_of(head_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-sort descending")));
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-sort none")));
  ASSERT_TRUE(str_contains(cmds, str_lit("data-key 1")));
  ASSERT_TRUE(str_contains(cmds, str_lit("data-align right")));
  ASSERT_TRUE(str_contains(cmds, str_lit("th-btn on")));
}

TEST(the_pager_counts_the_rows_on_this_page_and_shuts_its_first_button) {
  str cmds = cmds_of(pager_view);
  ASSERT_TRUE(str_contains(cmds, str_lit("1–2 of 5")));
  ASSERT_TRUE(str_contains(cmds, str_lit("1 / 3")));
  // the first page's Prev is announced as disabled and carries no handler
  ASSERT_TRUE(str_contains(cmds, str_lit("aria-disabled true")));
  ASSERT_TRUE(str_contains(cmds, str_lit("data-step 1")));
}

static void pager_one_view(WBuild *b) {
  (void)b;
  setup(&g_t, 0, 0, 0);
  table_pager(&g_t, page_flip, "Players");
}

TEST(a_single_page_has_no_pager_at_all) {
  str cmds = cmds_of(pager_one_view);
  ASSERT_FALSE(str_contains(cmds, str_lit("class pager")));
}

TEST_MAIN()