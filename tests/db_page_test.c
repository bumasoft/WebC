// A page of a list: two statements, one clamp, and a total that does not depend on the page.
#include <webc/test.h>

#include "test_db.h"

WEBC_MODEL(Pg, (id, i64, .pk = true), (n, i64), (group, str))
WEBC_VEC_DEFINE(VecPg, vec_pg, Pg)

static WDB *fresh(void) {
  WDB *db = wc_test_open();
  ASSERT_NOT_NULL(db);
  wc_test_drop(db, "pgs");
  char ddl[512];
  snprintf(ddl, sizeof ddl, "create table pgs (id %s, n integer not null default 0, %sgroup%s %s)",
           wc_test_pk(db), wc_test_q(db), wc_test_q(db), wc_test_text(db));
  ASSERT_TRUE(db_exec_sql(db, ddl).ok);
  for (i32 i = 1; i <= 45; i++) {
    Pg p = {.n = i, .group = str_lit(i % 2 ? "odd" : "even")};
    ASSERT_TRUE(db_insert(db, Pg, &p));
  }
  return db;
}

TEST(a_page_is_a_window_and_a_total) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  ww_order(&w, Pg, n, false);
  WPager pg = {.page = 2, .limit = 20};
  u64 before = db_statement_count();
  VecPg *rows = db_page(db, Pg, &w, &pg);
  ASSERT_EQ(db_statement_count() - before, 2); // count + window
  ASSERT_EQ(pg.total, 45);
  ASSERT_EQ(pg.pages, 3);
  ASSERT_TRUE(pg.has_prev);
  ASSERT_TRUE(pg.has_next);
  ASSERT_EQ(rows->n, 20);
  ASSERT_EQ(rows->at[0].n, 21); // page 2 starts at the 21st row
  ASSERT_EQ(rows->at[19].n, 40);
  ww_free(&w);
  db_close(db);
}

TEST(the_last_page_is_short_and_has_no_next) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  ww_order(&w, Pg, n, false);
  WPager pg = {.page = 3, .limit = 20};
  VecPg *rows = db_page(db, Pg, &w, &pg);
  ASSERT_EQ(rows->n, 5);
  ASSERT_TRUE(pg.has_prev);
  ASSERT_FALSE(pg.has_next);
  ww_free(&w);
  db_close(db);
}

TEST(an_out_of_range_page_is_the_same_list_not_an_empty_render) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  ww_order(&w, Pg, n, false);
  WPager pg = {.page = 99, .limit = 20};
  VecPg *rows = db_page(db, Pg, &w, &pg);
  ASSERT_EQ(pg.page, 3); // clamped
  ASSERT_EQ(rows->n, 5);
  WPager zero = {.page = 0, .limit = 20}; // 0 or unset means the first page
  VecPg *first = db_page(db, Pg, &w, &zero);
  ASSERT_EQ(zero.page, 1);
  ASSERT_EQ(first->at[0].n, 1);
  ww_free(&w);
  db_close(db);
}

TEST(the_total_ignores_the_filter_order_and_limit) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  ww_eq_str(&w, Pg, group, str_lit("odd"));
  ww_order(&w, Pg, n, true);
  ww_limit(&w, 2, 0); // a page the caller asked for: 2 rows of 23 odd ones
  WPager pg = {.page = 1, .limit = 10};
  VecPg *rows = db_page(db, Pg, &w, &pg);
  ASSERT_EQ(pg.total, 23); // the conditions, not the caller's limit
  ASSERT_EQ(pg.pages, 3);
  ASSERT_EQ(rows->n, 10);
  ASSERT_EQ(rows->at[0].n, 45); // the caller's order survives: descending
  ww_free(&w);
  db_close(db);
}

TEST(an_offset_without_a_limit_is_repaired_for_the_drivers_that_need_it) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  ww_limit(&w, 0, 40); // `offset 40` on its own: legal in sqlite, a syntax error in MySQL
  WPager first = {.page = 1, .limit = 10};
  VecPg *rows = db_page(db, Pg, &w, &first);
  ASSERT_EQ(rows->n, 10); // 45 rows, 10 per page: page 1 is full
  WWhere bare;
  ww_init(&bare, nullptr);
  ww_order(&bare, Pg, n, false);
  ww_limit(&bare, 0, 40);
  VecPg *tail = db_all_w(db, Pg, &bare);
  ASSERT_EQ(tail->n, 5); // 45 rows from the 41st on
  ww_free(&bare);
  ww_free(&w);
  db_close(db);
}

TEST_MAIN()
