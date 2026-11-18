// A has-many relation: one declaration, ONE query for the whole page, and refs() in the view.
//
// The belongs-to direction (`ref`) answers "what is this story's section" with a row.
// This is the other question — "what are this section's stories" — and it is the one that
// hides N+1: a page of sections that render their stories is one query per section unless
// the framework can be asked for all of them at once. `prefetch(all, Section, stories)` is
// that ask.
//
// What is asserted here is the *statement count*, not a stopwatch: `db_statement_count()`
// is the only honest way to say "one query, whatever the page length".
#include <webc/test.h>

#include "test_db.h"

WEBC_MODEL(MSec, (id, i64, .pk = true), (slug, str, .unique = true), (name, str, .required = true))
WEBC_MODEL(MSto, (id, i64, .pk = true), (slug, str, .unique = true), (section, str),
                  (author, i64), (title, str, .required = true))
WEBC_MODEL(MWho, (id, i64, .pk = true), (name, str, .required = true))

WEBC_VEC_DEFINE(VecMSec, vec_msec, MSec)
WEBC_VEC_DEFINE(VecMSto, vec_msto, MSto)
WEBC_VEC_DEFINE(VecMWho, vec_mwho, MWho)

WEBC_MODEL_TABLE(MSec, "m_secs")
WEBC_MODEL_TABLE(MSto, "m_stos")
WEBC_MODEL_TABLE(MWho, "m_whos")

WEBC_MODEL_REF(MSto, section, MSec, slug)
WEBC_MODEL_REF(MSto, author, MWho, id)
WEBC_MODEL_MANY(MSec, stories, MSto, section)

static WDB *fresh(void) {
  WDB *db = wc_test_open();
  ASSERT_NOT_NULL(db);
  wc_test_drop(db, "m_stos");
  wc_test_drop(db, "m_secs");
  wc_test_drop(db, "m_whos");
  char ddl[512];
  snprintf(ddl, sizeof ddl, "create table m_secs (id %s, slug %s unique, name %s not null)",
           wc_test_pk(db), wc_test_text(db), wc_test_text(db));
  ASSERT_TRUE(db_exec_sql(db, ddl).ok);
  snprintf(ddl, sizeof ddl,
           "create table m_stos (id %s, slug %s unique, section %s, author integer, "
           "title %s not null)",
           wc_test_pk(db), wc_test_text(db), wc_test_text(db), wc_test_text(db));
  ASSERT_TRUE(db_exec_sql(db, ddl).ok);
  snprintf(ddl, sizeof ddl, "create table m_whos (id %s, name %s not null)", wc_test_pk(db),
           wc_test_text(db));
  ASSERT_TRUE(db_exec_sql(db, ddl).ok);
  MSec city = {.slug = str_lit("city"), .name = str_lit("The City")};
  MSec science = {.slug = str_lit("science"), .name = str_lit("Science")};
  ASSERT_TRUE(db_insert(db, MSec, &city));
  ASSERT_TRUE(db_insert(db, MSec, &science));
  MWho ada = {.name = str_lit("Ada")};
  MWho kay = {.name = str_lit("Kay")};
  // saved, not inserted: the stories below have to name the author's *key*, and the key is
  // the database's to choose — `db_insert` leaves it out of the struct on purpose
  ASSERT_TRUE(db_save(MWho, &ada));
  ASSERT_TRUE(db_save(MWho, &kay));
  ASSERT_TRUE(ada.id > 0 && kay.id > 0 && ada.id != kay.id);
  MSto a = {.slug = str_lit("ferry"), .section = str_lit("city"), .author = ada.id,
            .title = str_lit("The Ferry")};
  MSto b = {.slug = str_lit("tide"), .section = str_lit("science"), .author = kay.id,
            .title = str_lit("The Tide")};
  MSto c = {.slug = str_lit("barge"), .section = str_lit("city"), .author = kay.id,
            .title = str_lit("The Barge")};
  ASSERT_TRUE(db_insert(db, MSto, &a));
  ASSERT_TRUE(db_insert(db, MSto, &b));
  ASSERT_TRUE(db_insert(db, MSto, &c));
  return db;
}

TEST(a_collection_costs_one_query_however_long_the_page) {
  WDB *db = fresh();
  VecMSec *all = db_all_ordered(db, MSec, "id asc");
  ASSERT_EQ(all->n, 2);

  // Before the prefetch: an empty list, never null — iterating null is a crash, and a view
  // that silently renders nothing is the failure the warning in the log is for.
  VecMSto *none = refs(MSec, &all->at[0], stories);
  ASSERT_NOT_NULL(none);
  ASSERT_EQ(none->n, 0);

  u64 before = db_statement_count();
  prefetch_on(db, all, MSec, stories);
  ASSERT_EQ(db_statement_count() - before, 1);

  VecMSto *city = refs(MSec, &all->at[0], stories);
  VecMSto *sci = refs(MSec, &all->at[1], stories);
  ASSERT_EQ(city->n, 2);
  ASSERT_EQ(sci->n, 1);
  ASSERT_STR_EQ(city->at[0].title, "The Ferry"); // the child table's order: pk
  ASSERT_STR_EQ(city->at[1].title, "The Barge");
  ASSERT_STR_EQ(sci->at[0].title, "The Tide");
  db_close(db);
}

TEST(two_hops_are_two_queries_and_the_second_reads_through_ref) {
  WDB *db = fresh();
  VecMSec *all = db_all_ordered(db, MSec, "id asc");
  u64 before = db_statement_count();
  // sections -> their stories -> each story's author
  prefetch_on(db, all, MSec, stories, author);
  ASSERT_EQ(db_statement_count() - before, 2);

  VecMSto *city = refs(MSec, &all->at[0], stories);
  ASSERT_EQ(city->n, 2);
  for (i32 i = 0; i < city->n; i++) {
    const MWho *who = ref(MSto, &city->at[i], author);
    ASSERT_NOT_NULL(who);
  }
  const MWho *ada = ref(MSto, &city->at[0], author);
  const MWho *kay = ref(MSto, &city->at[1], author);
  ASSERT_NOT_NULL(ada);
  ASSERT_NOT_NULL(kay);
  ASSERT_STR_EQ(ada->name, "Ada");
  ASSERT_STR_EQ(kay->name, "Kay");
  db_close(db);
}

TEST(a_belongs_to_hop_can_be_followed) {
  WDB *db = fresh();
  VecMSto *stories = db_all_ordered(db, MSto, "id asc");
  u64 before = db_statement_count();
  // story -> its section -> the section's stories (the reverse hop, on the sections the
  // first hop linked — a different set of rows from the page's, and the reason a hop
  // collects what the previous one found instead of reusing the original page)
  prefetch_on(db, stories, MSto, section); // hop 1: belongs-to
  ASSERT_EQ(db_statement_count() - before, 1);
  const MSec *sec = ref(MSto, &stories->at[0], section);
  ASSERT_NOT_NULL(sec);
  ASSERT_STR_EQ(sec->name, "The City");
  db_close(db);
}

TEST(a_page_longer_than_the_parameter_ceiling_is_chunked) {
  WDB *db = fresh();
  // 600 sections, so one prefetch cannot be one statement with 600 binds: the chunk is 512.
  for (i32 i = 0; i < 600; i++) {
    MSec s = {.slug = str_fmt(nullptr, "s%03d", i), .name = str_fmt(nullptr, "S%d", i)};
    ASSERT_TRUE(db_insert(db, MSec, &s));
  }
  VecMSec *all = db_all_ordered(db, MSec, "id asc");
  ASSERT_EQ(all->n, 602);
  u64 before = db_statement_count();
  prefetch_on(db, all, MSec, stories);
  ASSERT_EQ(db_statement_count() - before, 2); // 512 + 90
  db_close(db);
}

TEST(a_collection_nobody_declared_says_so) {
  // The declaration is the only place a relation exists, so reading one that was never
  // declared is a bug in the app, not an empty list at render time.
  ASSERT_PANICS((void)wc_refs(MSto_modelof(), &(MSto){0}, "nonexistent"));
}

TEST(the_declaration_says_which_models_meet) {
  const char *owner = nullptr, *field = nullptr, *target = nullptr, *back = nullptr;
  bool found = false;
  for (i32 i = 0; i < wc_many_rule_count(); i++) {
    wc_many_rule_at(i, &owner, &field, &target, &back);
    if (strcmp(owner, "MSec") == 0 && strcmp(field, "stories") == 0) {
      ASSERT_STR_EQ(target, "MSto");
      ASSERT_STR_EQ(back, "section");
      found = true;
    }
  }
  ASSERT_TRUE(found);
}

TEST_MAIN()