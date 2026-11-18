// query: one filter, answered three ways, plus the two things an app does to a row without
// reading it back. The alternative is a spelling the app has to guess: a `q("select 1")`
// created only to carry a bound value, a list cut down in memory because the query has no
// limit, and an update that writes every column of a row to add one.
#include <webc/test.h>

WEBC_MODEL(Note, (id, i64, .pk = true),
                 (title, str, .required = true),
                 (desk, str, .max = 40),
                 (words, i64),
                 (seen, i64))

WEBC_VEC_DEFINE(VecNote, vec_note, Note)

static WDB *fresh(void) {
  WDB *db = db_open("sqlite::memory:");
  ASSERT_NOT_NULL(db);
  ASSERT_TRUE(db_exec_sql(db,
                         "create table notes (id integer primary key autoincrement, "
                         "title text not null, desk text, words integer default 0, "
                         "seen integer default 0)")
                  .ok);
  return db;
}

static void add_note(WDB *db, const char *title, const char *desk, i64 words) {
  Note n = {.title = str_lit(title), .desk = str_lit(desk), .words = words, .seen = 0};
  ASSERT_TRUE(db_insert(db, Note, &n));
}

TEST(db_all_w_answers_a_list_the_query_ordered_and_limited) {
  WDB *db = fresh();
  add_note(db, "ferry", "city", 900);
  add_note(db, "tide", "science", 1500);
  add_note(db, "river", "city", 1200);

  // The field names are checked by the compiler and the limit is applied by the query: an app
  // that reads thirteen rows and cuts the vector down to two with `v->n = 2` is doing the
  // database's work twice.
  WWhere w;
  ww_init(&w, nullptr);
  ww_eq_str(&w, Note, desk, str_lit("city"));
  ww_order(&w, Note, words, true);
  ww_limit(&w, 1, 0);
  VecNote *list = db_all_w(db, Note, &w);
  ww_free(&w);

  ASSERT_EQ(list->n, 1);
  ASSERT_STR_EQ(list->at[0].title, "river");
  db_close(db);
}

TEST(db_one_is_the_first_row_or_null) {
  WDB *db = fresh();
  add_note(db, "ferry", "city", 900);
  add_note(db, "river", "city", 1200);

  WWhere w;
  ww_init(&w, nullptr);
  ww_eq_str(&w, Note, desk, str_lit("city"));
  ww_order(&w, Note, words, true);
  Note *one = db_one(db, Note, &w);
  ww_free(&w);
  ASSERT_NOT_NULL(one);
  ASSERT_STR_EQ(one->title, "river");

  // nothing matches: null, not a panic and not a row of zeroes
  WWhere none;
  ww_init(&none, nullptr);
  ww_eq_str(&none, Note, desk, str_lit("opinion"));
  ASSERT_NULL(db_one(db, Note, &none));
  ww_free(&none);
  db_close(db);
}

TEST(db_count_w_counts_without_reading) {
  WDB *db = fresh();
  add_note(db, "ferry", "city", 900);
  add_note(db, "river", "city", 1200);
  add_note(db, "tide", "science", 1500);

  WWhere w;
  ww_init(&w, nullptr);
  ww_eq_str(&w, Note, desk, str_lit("city"));
  ASSERT_EQ(db_count_w(db, Note, &w), 2);
  // the builder is a description, not a run: asking twice is legitimate
  ASSERT_EQ(db_count_w(db, Note, &w), 2);
  ASSERT_EQ(db_all_w(db, Note, &w)->n, 2);
  ww_free(&w);
  db_close(db);
}

TEST(a_search_over_several_columns_is_one_question) {
  WDB *db = fresh();
  add_note(db, "the last ferry", "city", 900);
  add_note(db, "what the tide left", "science", 1500);
  add_note(db, "ferry timetables", "city", 700);

  WWhere w;
  ww_init(&w, nullptr);
  ww_or_begin(&w);
  ww_contains(&w, Note, title, str_lit("FERRY")); // case does not matter, on either driver
  ww_contains(&w, Note, desk, str_lit("ferry"));
  ww_or_end(&w);
  VecNote *list = db_all_w(db, Note, &w);
  ASSERT_EQ(list->n, 2);
  ww_free(&w);
  db_close(db);
}

TEST(a_wildcard_in_a_search_is_data_not_a_pattern) {
  WDB *db = fresh();
  add_note(db, "100% pure", "city", 100);
  add_note(db, "100 proof", "city", 100);

  WWhere w;
  ww_init(&w, nullptr);
  ww_contains(&w, Note, title, str_lit("0% p"));
  ASSERT_EQ(db_all_w(db, Note, &w)->n, 1);
  ww_free(&w);
  db_close(db);
}

TEST(db_increment_touches_one_column) {
  WDB *db = fresh();
  add_note(db, "ferry", "city", 900);
  WWhere w;
  ww_init(&w, nullptr);
  ww_eq_str(&w, Note, title, str_lit("ferry"));
  Note *n = db_one(db, Note, &w);
  ww_free(&w);
  ASSERT_NOT_NULL(n);
  ASSERT_EQ(n->seen, 0);

  ASSERT_TRUE(db_increment(db, Note, n, seen));
  ASSERT_TRUE(db_increment(db, Note, n, seen));
  ww_init(&w, nullptr);
  ww_eq_str(&w, Note, title, str_lit("ferry"));
  Note *again = db_one(db, Note, &w);
  ww_free(&w);
  ASSERT_EQ(again->seen, 2);
  // ...and nothing else moved
  ASSERT_EQ(again->words, 900);
  ASSERT_STR_EQ(again->desk, "city");
  db_close(db);
}

TEST_MAIN()
// ---------------------------------------------------------------- one field, one value
//
// `db_by(Section, "slug", slug)` is what the seven `q("select 1")` in an app were for: a
// query built only to carry a value into `db_find`'s SQL string. The carrier is gone, and
// the field name is checked — against the model at runtime, and by the compiler when the
// typed macros are used.

TEST(db_by_is_one_field_one_value_with_no_carrier_query) {
  WDB *db = fresh();
  add_note(db, "ferry", "city", 900);
  add_note(db, "tide", "science", 1500);

  // A str, a literal and a number all go through the same call.
  Note *ferry = db_by(db, Note, "title", str_lit("ferry"));
  ASSERT_NOT_NULL(ferry);
  ASSERT_EQ(ferry->words, 900);
  Note *tide = db_by(db, Note, "title", str_lit("tide"));
  ASSERT_NOT_NULL(tide);
  ASSERT_EQ(tide->words, 1500);
  ASSERT_NULL(db_by(db, Note, "title", str_lit("nothing")));

  VecNote *city = db_all_by(db, Note, "desk", str_lit("city"));
  ASSERT_EQ(city->n, 1);
  ASSERT_STR_EQ(city->at[0].title, "ferry");
  ASSERT_EQ(db_count_by(db, Note, "desk", str_lit("city")), 1);
  ASSERT_EQ(db_count_by(db, Note, "desk", str_lit("nowhere")), 0);

  // more than one match: the first row, not a panic (the primary key order decides)
  add_note(db, "barge", "city", 300);
  Note *any = db_by(db, Note, "desk", str_lit("city"));
  ASSERT_NOT_NULL(any);
  ASSERT_STR_EQ(any->desk, "city");
  db_close(db);
}

TEST(db_by_names_the_fields_it_has_instead_of_pasting_the_name_into_sql) {
  WDB *db = fresh();
  add_note(db, "ferry", "city", 900);
  // A typo is a query that matches nothing and a page that is quietly empty — so it panics.
  ASSERT_PANICS((void)db_by(db, Note, "titel", str_lit("ferry")));
  db_close(db);
}

TEST(order_and_limit_with_no_condition_is_a_clause_of_its_own) {
  // `select ... from notes where  order by words desc limit 2` is not SQL, and the page it
  // broke was silently shorter than it should have been: the "most read" rail, empty.
  WDB *db = fresh();
  add_note(db, "ferry", "city", 900);
  add_note(db, "tide", "science", 1500);
  add_note(db, "river", "city", 1200);

  WWhere w;
  ww_init(&w, nullptr);
  ww_order(&w, Note, words, true);
  ww_limit(&w, 2, 0);
  VecNote *list = db_all_w(db, Note, &w);
  ww_free(&w);
  ASSERT_EQ(list->n, 2);
  ASSERT_STR_EQ(list->at[0].title, "tide");
  ASSERT_STR_EQ(list->at[1].title, "river");

  WWhere one;
  ww_init(&one, nullptr);
  ww_order(&one, Note, words, false);
  Note *first = db_one(db, Note, &one);
  ww_free(&one);
  ASSERT_NOT_NULL(first);
  ASSERT_STR_EQ(first->title, "ferry");
  db_close(db);
}
