// WEBC_ROW: a row you computed, not a table you store, and one collection type per
// element — `vec_new(str)` is a `VecStr *`, not a name nobody declared.
#include <webc/test.h>

#include "test_db.h"

// The shape of every nav bar, table of contents and KPI strip: a list built in the
// request and rendered in the same breath.
WEBC_ROW(CountRow, (key, Text), (value, Text))
Vec(CountRow)

WEBC_ROW(NavLink, (label, Text), (path, Text))
Vec(NavLink)

WEBC_MODEL(NStored, (id, i64, .pk = true), (name, Text))
WEBC_MODEL_TABLE(NStored, "n_stored")

TEST(a_computed_row_is_a_collection_like_any_other) {
  VecCountRow *rows = vec_new(CountRow);
  ASSERT_EQ(len(*rows), 0);
  CountRow r = {.key = "referrer", .value = "12"};
  push(*rows, r);
  CountRow r2 = {.key = "device", .value = "3"};
  push(*rows, r2);
  ASSERT_EQ(len(*rows), 2);
  ASSERT_STR_EQ(at(*rows, 0)->key, "referrer");
  i32 n = 0;
  for (i32 i = 0; i < len(*rows); i++) n += at(*rows, i)->value[0] != 0;
  ASSERT_EQ(n, 2);
}

TEST(a_computed_row_serialises_without_a_table) {
  VecNavLink *links = vec_new(NavLink);
  NavLink l1 = {.label = "Home", .path = "/"};
  NavLink l2 = {.label = "Archive", .path = "/archive"};
  push(*links, l1);
  push(*links, l2);
  str j = model_to_json(wc_arena_get(), NavLink_modelof(), &links->at[0]);
  ASSERT_STR_CONTAINS(j, "\"label\":\"Home\"");
  // The collection describes its own element, which is what json(200, rows) reads: a
  // computed row is a wire shape too, not only a rendering convenience.
  ASSERT_NOT_NULL(links->desc);
  ASSERT_EQ((int)links->desc->elem_kind, (int)WK_MODEL);
  ASSERT_PTR_EQ(links->desc->sub, NavLink_modelof());
}

// json(200, rows) needs the descriptor to name a submodel; a WEBC_ROW has one.
TEST(a_computed_row_is_not_a_table) {
  ASSERT_TRUE(CountRow_modelof()->internal);
  WDB *db = wc_test_open();
  ASSERT_NOT_NULL(db);
  // Saving one is a panic with a sentence, not "no such table: count_rows".
  CountRow row = {.key = "k", .value = "v"};
  ASSERT_PANICS(db_save(CountRow, &row));
}

TEST(a_vector_of_strings_has_one_type) {
  VecStr *v = vec_new(str);
  push(*v, str_lit("a"));
  push(*v, str_lit("b"));
  ASSERT_EQ(v->n, 2);
  ASSERT_STR_EQ(v->at[1], "b");
  // the prelude name and the derived name are the same type: this assignment is the test
  VecStr *same = v;
  ASSERT_PTR_EQ(same, v);
}

TEST_MAIN()