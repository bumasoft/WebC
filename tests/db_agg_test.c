// Aggregates over a filter: sum, avg, min, max — one statement, one number.
//
// `db_count_w` was the only question the builder could turn into a number, so an app that
// needed a total read every row and added them up in the handler. These ask the database,
// and they strip `order by`/`limit` first: a sum of the first page is a different question.
#include <webc/test.h>

#include "test_db.h"

WEBC_MODEL(Agg, (id, i64, .pk = true), (kind, str), (views, i64), (score, f64))
WEBC_VEC_DEFINE(VecAgg, vec_agg, Agg)

static WDB *fresh(void) {
  WDB *db = wc_test_open();
  ASSERT_NOT_NULL(db);
  wc_test_drop(db, "aggs");
  char ddl[512];
  snprintf(ddl, sizeof ddl,
           "create table aggs (id %s, kind %s, views integer not null default 0, "
           "score double not null default 0)",
           wc_test_pk(db), wc_test_text(db));
  ASSERT_TRUE(db_exec_sql(db, ddl).ok);
  struct { const char *kind; i64 views; f64 score; } rows[] = {
      {"news", 10, 1.5}, {"news", 30, 2.5}, {"opinion", 5, 4.0},
  };
  for (i32 i = 0; i < 3; i++) {
    Agg a = {.kind = str_lit(rows[i].kind), .views = rows[i].views, .score = rows[i].score};
    ASSERT_TRUE(db_insert(db, Agg, &a));
  }
  return db;
}

TEST(sum_min_max_and_avg_over_every_row) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  ASSERT_EQ(db_sum(db, Agg, views, &w), 45);
  ASSERT_EQ(db_min(db, Agg, views, &w), 5);
  ASSERT_EQ(db_max(db, Agg, views, &w), 30);
  ASSERT_EQ((i64)(db_avg(db, Agg, views, &w) * 100), (i64)(15.0 * 100));
  ww_free(&w);
  db_close(db);
}

TEST(an_aggregate_over_a_filter_counts_only_the_matching_rows) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  ww_eq_str(&w, Agg, kind, str_lit("news"));
  ASSERT_EQ(db_sum(db, Agg, views, &w), 40);
  ASSERT_EQ(db_count_w_(db, Agg_modelof(), &w), 2);
  ww_free(&w);
  db_close(db);
}

TEST(order_and_limit_are_stripped_from_the_aggregate) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  ww_order(&w, Agg, views, true);
  ww_limit(&w, 1, 0); // "the ten most viewed" must still sum all of them
  ASSERT_EQ(db_sum(db, Agg, views, &w), 45);
  ww_free(&w);
  db_close(db);
}

TEST(no_rows_is_zero_not_an_error) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  ww_eq_str(&w, Agg, kind, str_lit("nothing-matches"));
  ASSERT_EQ(db_sum(db, Agg, views, &w), 0);
  ASSERT_EQ(db_max(db, Agg, views, &w), 0);
  ASSERT_EQ(db_avg(db, Agg, views, &w), 0.0);
  ww_free(&w);
  db_close(db);
}

TEST(avg_of_an_integer_column_is_a_real_number) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  ww_eq_str(&w, Agg, kind, str_lit("opinion"));
  // 5 / 1 = 5 exactly; the point is the *type*: every dialect answers avg() with a
  // non-integer type even for an integer column, and this must not truncate to 5 and 0
  ASSERT_TRUE(db_avg(db, Agg, views, &w) == 5.0);
  ww_free(&w);
  db_close(db);
}

TEST(an_aggregate_on_a_non_numeric_field_is_refused_by_name) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  ASSERT_PANICS((void)db_sum_(db, Agg_modelof(), "kind", &w));
  ww_free(&w);
  db_close(db);
}

TEST_MAIN()
