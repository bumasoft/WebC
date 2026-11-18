// Writing a parent and its children as one thing: the FK is filled from the parent's key,
// and a failure takes the whole tree with it.
#include <webc/test.h>

#include "test_db.h"

WEBC_MODEL(WOrd, (id, i64, .pk = true), (ref, str, .unique = true), (total, i64))
WEBC_MODEL(WLine, (id, i64, .pk = true), (order_id, i64), (sku, str), (qty, i64))

WEBC_VEC_DEFINE(VecWOrd, vec_word, WOrd)
WEBC_VEC_DEFINE(VecWLine, vec_wline, WLine)

WEBC_MODEL_TABLE(WOrd, "w_ords")
WEBC_MODEL_TABLE(WLine, "w_items")

WEBC_MODEL_REF(WLine, order_id, WOrd, id)
WEBC_MODEL_MANY(WOrd, items, WLine, order_id)

static WDB *fresh(void) {
  WDB *db = wc_test_open();
  ASSERT_NOT_NULL(db);
  wc_test_drop(db, "w_items");
  wc_test_drop(db, "w_ords");
  char ddl[512];
  snprintf(ddl, sizeof ddl,
           "create table w_ords (id %s, ref %s unique, total integer not null default 0)",
           wc_test_pk(db), wc_test_text(db));
  ASSERT_TRUE(db_exec_sql(db, ddl).ok);
  snprintf(ddl, sizeof ddl,
           "create table w_items (id %s, order_id integer not null, sku %s not null unique, "
           "qty integer not null default 1)",
           wc_test_pk(db), wc_test_text(db));
  ASSERT_TRUE(db_exec_sql(db, ddl).ok);
  return db;
}

static VecWLine *items_of(i32 n) {
  VecWLine *v = wc_alloc_zero(sizeof *v);
  *v = vec_wline_new();
  for (i32 i = 0; i < n; i++) {
    WLine it = {.sku = str_fmt(nullptr, "sku-%d", i), .qty = i + 1};
    vec_wline_push(v, it);
  }
  return v;
}

TEST(a_parent_and_its_children_are_written_together) {
  WDB *db = fresh();
  WOrd o = {.ref = str_lit("A-1"), .total = 3};
  ASSERT_TRUE(db_save(WOrd, &o));
  ASSERT_TRUE(o.id > 0);
  VecWLine *items = items_of(3);
  i64 n = db_save_all(db, WOrd, &o, items, &n);
  ASSERT_EQ(n, 3);
  ASSERT_EQ(vec_wline_len(db_all_ordered(db, WLine, "id asc")), 3);
  // the FK came from the declaration, not from the caller
  VecWLine *back = db_all_by(db, WLine, "order_id", o.id);
  ASSERT_EQ(back->n, 3);
  ASSERT_EQ(back->at[0].order_id, o.id);
  // and the relation reads them back through refs
  VecWOrd *orders = db_all_ordered(db, WOrd, "id asc");
  prefetch_on(db, orders, WOrd, items);
  ASSERT_EQ(refs(WOrd, &orders->at[0], items)->n, 3);
  db_close(db);
}

TEST(a_failure_rolls_the_whole_tree_back) {
  WDB *db = fresh();
  WOrd o = {.ref = str_lit("A-2"), .total = 2};
  ASSERT_TRUE(db_save(WOrd, &o));
  VecWLine *items = items_of(2);
  // the second insert breaks a unique key: the first must not survive
  items->at[1].sku = items->at[0].sku;
  bool failed = false;
  // db_save_all fails the request on a driver error, which a test catches as a panic
  ASSERT_PANICS((void)db_save_all(db, WOrd, &o, items, nullptr));
  failed = true;
  ASSERT_TRUE(failed);
  ASSERT_EQ(vec_wline_len(db_all(db, WLine)), 0);
  db_close(db);
}

TEST(deleting_a_tree_removes_the_children_first) {
  WDB *db = fresh();
  WOrd o = {.ref = str_lit("A-3"), .total = 1};
  ASSERT_TRUE(db_save(WOrd, &o));
  VecWLine *items = items_of(2);
  ASSERT_EQ(db_save_all(db, WOrd, &o, items, nullptr), 2);
  i64 removed = db_delete_tree(db, WOrd, &o, items);
  ASSERT_EQ(removed, 2);
  ASSERT_EQ(vec_wline_len(db_all(db, WLine)), 0);
  ASSERT_NULL(db_get(WOrd, o.id));
  db_close(db);
}

TEST(the_parent_has_to_exist_before_its_children_can_point_at_it) {
  WDB *db = fresh();
  WOrd o = {.ref = str_lit("A-4")}; // no id: never saved
  VecWLine *items = items_of(1);
  // a sentence about saving the parent first, not a foreign key error from the server
  ASSERT_PANICS((void)db_save_all(db, WOrd, &o, items, nullptr));
  db_close(db);
}

TEST_MAIN()
