// Filters that reach through a relation: "stories in a section whose name contains …", and
// "order the stories by their section's name".
//
// A join would answer the first question, but a join changes the *shape* of the result — two
// rows for a section with two stories, and `select *` no longer reads back into a Story. The
// ORM reads rows into models, so it asks with a subquery: `section in (select slug from
// sections where …)` stays a stories query. This file is where that stays true.
#include <webc/test.h>

#include "test_db.h"

WEBC_MODEL(RFSec, (id, i64, .pk = true), (slug, str, .unique = true), (name, str))
WEBC_MODEL(RFSto, (id, i64, .pk = true), (section, str), (title, str), (views, i64))
WEBC_MODEL_TABLE(RFSec, "rf_secs")
WEBC_MODEL_TABLE(RFSto, "rf_stos")
WEBC_VEC_DEFINE(VecRFSec, vec_rfsec, RFSec)
WEBC_VEC_DEFINE(VecRFSto, vec_rfsto, RFSto)

WEBC_MODEL_REF(RFSto, section, RFSec, slug)
WEBC_MODEL_MANY(RFSec, stories, RFSto, section)

static WDB *fresh(void) {
  WDB *db = wc_test_open();
  ASSERT_NOT_NULL(db);
  wc_test_drop(db, "rf_stos");
  wc_test_drop(db, "rf_secs");
  char ddl[512];
  snprintf(ddl, sizeof ddl, "create table rf_secs (id %s, slug %s unique, name %s)",
           wc_test_pk(db), wc_test_text(db), wc_test_text(db));
  ASSERT_TRUE(db_exec_sql(db, ddl).ok);
  snprintf(ddl, sizeof ddl,
           "create table rf_stos (id %s, section %s, title %s, views integer not null default 0)",
           wc_test_pk(db), wc_test_text(db), wc_test_text(db));
  ASSERT_TRUE(db_exec_sql(db, ddl).ok);
  const char *secs[][2] = {{"city", "The City"}, {"sea", "The Sea"}, {"sport", "Sport"}};
  for (i32 i = 0; i < 3; i++) {
    RFSec s = {.slug = str_lit(secs[i][0]), .name = str_lit(secs[i][1])};
    ASSERT_TRUE(db_insert(db, RFSec, &s));
  }
  const char *stories[][3] = {
      {"city", "Ferry", "30"}, {"sea", "Tide", "10"}, {"sea", "Salt", "20"}, {"sport", "Match", "5"},
  };
  for (i32 i = 0; i < 4; i++) {
    RFSto t = {.section = str_lit(stories[i][0]), .title = str_lit(stories[i][1]),
               .views = atoi(stories[i][2])};
    ASSERT_TRUE(db_insert(db, RFSto, &t));
  }
  return db;
}

TEST(a_filter_through_a_relation_keeps_the_row_shape) {
  WDB *db = fresh();
  WWhere inner;
  ww_init(&inner, nullptr);
  ww_contains(&inner, RFSec, name, str_lit("sea"));
  WWhere w;
  ww_init(&w, nullptr);
  ww_in_ref(&w, RFSto, section, &inner);
  VecRFSto *rows = db_all_w(db, RFSto, &w);
  ASSERT_EQ(rows->n, 2); // Salt and Tide, not four rows for two sections
  ASSERT_STR_EQ(rows->at[0].title, "Tide");
  ASSERT_STR_EQ(rows->at[1].title, "Salt");
  ww_free(&w);
  ww_free(&inner);
  db_close(db);
}

TEST(the_sub_filters_parameters_do_not_collide_with_the_outer_ones) {
  WDB *db = fresh();
  // Both filters bind a parameter, and both start numbering at :p0 — the outer one names a
  // count, the inner one a substring. A naive splice would have the inner value overwrite the
  // outer one, and the query would answer the wrong rows with no error.
  WWhere inner;
  ww_init(&inner, nullptr);
  ww_contains(&inner, RFSec, name, str_lit("sea"));
  WWhere w;
  ww_init(&w, nullptr);
  ww_gt_i64(&w, RFSto, views, 15);
  ww_in_ref(&w, RFSto, section, &inner);
  VecRFSto *rows = db_all_w(db, RFSto, &w);
  ASSERT_EQ(rows->n, 1); // Salt: 20 views, in The Sea — Tide has 10
  ASSERT_STR_EQ(rows->at[0].title, "Salt");
  ww_free(&w);
  ww_free(&inner);
  db_close(db);
}

TEST(not_in_ref_is_the_complement) {
  WDB *db = fresh();
  WWhere inner;
  ww_init(&inner, nullptr);
  ww_contains(&inner, RFSec, name, str_lit("sea"));
  WWhere w;
  ww_init(&w, nullptr);
  ww_not_in_ref(&w, RFSto, section, &inner);
  VecRFSto *rows = db_all_w(db, RFSto, &w);
  ASSERT_EQ(rows->n, 2); // Ferry and Match
  ww_free(&w);
  ww_free(&inner);
  db_close(db);
}

TEST(ordering_by_a_related_column_is_a_subquery_too) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  ww_order_ref(&w, RFSto, section, RFSec, name, false);
  VecRFSto *rows = db_all_w(db, RFSto, &w);
  ASSERT_EQ(rows->n, 4);
  // by the section's *name*: Match (Sport) < Ferry (The City) < the two of The Sea, whose
  // order is the tie the sort does not decide
  ASSERT_STR_EQ(rows->at[0].title, "Match");
  ASSERT_STR_EQ(rows->at[1].title, "Ferry");
  ASSERT_TRUE(str_eq_c(rows->at[2].title, "Salt") || str_eq_c(rows->at[2].title, "Tide"));
  ASSERT_TRUE(str_eq_c(rows->at[3].title, "Salt") || str_eq_c(rows->at[3].title, "Tide"));
  ASSERT_FALSE(str_eq(rows->at[2].title, rows->at[3].title));
  ww_free(&w);
  db_close(db);
}

TEST(a_relation_filter_on_an_undeclared_relation_says_so) {
  WDB *db = fresh();
  WWhere w;
  ww_init(&w, nullptr);
  WWhere inner;
  ww_init(&inner, nullptr);
  // RFSto.title is not a relation: the caller meant `section`. The panic names the declaration
  // the model is missing rather than emitting `title in (select …)` and failing at the server.
  ASSERT_PANICS(ww_in_ref(&w, RFSto, title, &inner));
  ww_free(&w);
  ww_free(&inner);
  db_close(db);
}

TEST(a_relation_filter_and_a_page_agree_about_the_total) {
  WDB *db = fresh();
  WWhere inner;
  ww_init(&inner, nullptr);
  ww_contains(&inner, RFSec, name, str_lit("sea"));
  WWhere w;
  ww_init(&w, nullptr);
  ww_in_ref(&w, RFSto, section, &inner);
  ww_order(&w, RFSto, views, true);
  WPager pg = {.page = 1, .limit = 1};
  VecRFSto *first = db_page(db, RFSto, &w, &pg);
  ASSERT_EQ(pg.total, 2); // the relation filter counts too, or the pager lies about the pages
  ASSERT_EQ(pg.pages, 2);
  ASSERT_TRUE(pg.has_next);
  ASSERT_EQ(first->n, 1);
  ASSERT_EQ(first->at[0].views, 20);
  ww_free(&w);
  ww_free(&inner);
  db_close(db);
}

TEST_MAIN()
