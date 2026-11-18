// Nullable integer columns: `.null = true`.
//
// The struct stays plain C. A field whose column may be NULL spells the absence `0`, and
// the framework normalises at every edge it owns: SQL binds NULL, reads NULL back as 0,
// JSON says `null` and accepts it, an empty form input is 0, and `ww_is_null()` is how a
// query asks for it. Without it, an app copies the original's constraint into a migration
// and finds out from a 400 that 0 cannot be a foreign key.
#include <webc/test.h>

#include "test_db.h"

WEBC_MODEL(NCover, (id, i64, .pk = true), (slug, str, .unique = true))
WEBC_MODEL(NDoc, (id, i64, .pk = true),
          (title, Text, .required = true),
          (cover_id, i64, .null = true, .references = "n_covers(id)"),
          (pages, i64))
WEBC_MODEL_TABLE(NCover, "n_covers")
WEBC_MODEL_TABLE(NDoc, "n_docs")
Vec(NDoc)

static WDB *fresh(void) {
  WDB *db = wc_test_open();
  ASSERT_NOT_NULL(db);
  wc_test_drop(db, "n_docs");
  wc_test_drop(db, "n_covers");
  char ddl[512];
  snprintf(ddl, sizeof ddl, "create table n_covers (id %s, slug %s unique)", wc_test_pk(db),
           wc_test_text(db));
  ASSERT_TRUE(db_exec_sql(db, ddl).ok);
  // cover_id is nullable and referring — the shape posts.cover_media_asset_id needed.
  snprintf(ddl, sizeof ddl,
           "create table n_docs (id %s, title %s not null, cover_id integer references "
           "n_covers(id), pages integer not null default 0)",
           wc_test_pk(db), wc_test_text(db));
  ASSERT_TRUE(db_exec_sql(db, ddl).ok);
  return db;
}

TEST(zero_in_a_nullable_column_is_sql_null) {
  WDB *db = fresh();
  NDoc d = {.title = "No cover", .cover_id = 0, .pages = 3};
  ASSERT_TRUE(db_save(NDoc, &d));
  ASSERT_TRUE(d.id > 0);
  WRes r = db_exec_sql(db, "select cover_id from n_docs");
  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.nrows, 1);
  ASSERT_TRUE(row_is_null(&r.rows[0], "cover_id"));
  wres_free(&r);
}

TEST(a_nullable_column_reads_null_back_as_zero) {
  WDB *db = fresh();
  NCover c = {.slug = "hero"};
  ASSERT_TRUE(db_save(NCover, &c));
  NDoc none = {.title = "No cover", .cover_id = 0};
  NDoc some = {.title = "With cover", .cover_id = c.id};
  ASSERT_TRUE(db_save(NDoc, &none));
  ASSERT_TRUE(db_save(NDoc, &some));
  WWhere w;
  ww_init(&w, wc_arena_get());
  ww_eq_str(&w, NDoc, title, str_lit("No cover"));
  NDoc *back = db_one(db, NDoc, &w);
  ASSERT_NOT_NULL(back);
  ASSERT_EQ(back->cover_id, 0);
  WWhere w2;
  ww_init(&w2, wc_arena_get());
  ww_eq_str(&w2, NDoc, title, str_lit("With cover"));
  NDoc *with = db_one(db, NDoc, &w2);
  ASSERT_NOT_NULL(with);
  ASSERT_EQ(with->cover_id, c.id);
}

TEST(the_json_wire_says_null_not_a_sentinel) {
  NDoc d = {.title = "No cover", .cover_id = 0, .pages = 3};
  str j = model_to_json(wc_arena_get(), NDoc_modelof(), &d);
  ASSERT_STR_CONTAINS(j, "\"cover_id\":null");
  ASSERT_STR_CONTAINS(j, "\"pages\":3");
  NDoc with = {.title = "With cover", .cover_id = 7};
  str j2 = model_to_json(wc_arena_get(), NDoc_modelof(), &with);
  ASSERT_STR_CONTAINS(j2, "\"cover_id\":7");
}

TEST(null_reads_as_absence_and_a_bare_null_is_still_an_error_elsewhere) {
  Arena *a = wc_arena_get();
  NDoc d = {0};
  WModelParse rp = model_from_json(a, NDoc_modelof(), str_lit("{\"title\":\"x\",\"cover_id\":null}"), &d);
  ASSERT_TRUE(rp.ok);
  ASSERT_EQ(d.cover_id, 0);
  // `pages` is a plain i64: null is a type error there, not a silent 0.
  NDoc e = {0};
  WModelParse bad = model_from_json(a, NDoc_modelof(), str_lit("{\"title\":\"x\",\"pages\":null}"), &e);
  ASSERT_FALSE(bad.ok);
  ASSERT_EQ(bad.errs.n, 1);
}

TEST(an_empty_form_input_is_absence_on_a_nullable_column) {
  Arena *a = wc_arena_get();
  NDoc d = {0};
  WModelParse rp = model_from_form(a, NDoc_modelof(), str_lit("title=x&cover_id=&pages=4"), &d);
  ASSERT_TRUE(rp.ok);
  ASSERT_EQ(d.cover_id, 0);
  ASSERT_EQ(d.pages, 4);
}

TEST(is_null_is_a_question_only_a_nullable_field_can_be_asked) {
  WDB *db = fresh();
  NCover c = {.slug = "hero"};
  ASSERT_TRUE(db_save(NCover, &c));
  NDoc none = {.title = "No cover", .cover_id = 0};
  NDoc some = {.title = "With cover", .cover_id = c.id};
  ASSERT_TRUE(db_save(NDoc, &none));
  ASSERT_TRUE(db_save(NDoc, &some));

  WWhere w;
  ww_init(&w, wc_arena_get());
  ww_is_null(&w, NDoc, cover_id);
  VecNDoc *bare = db_all_w(db, NDoc, &w);
  ASSERT_EQ(bare->n, 1);
  ASSERT_STR_EQ(bare->at[0].title, "No cover");

  WWhere w2;
  ww_init(&w2, wc_arena_get());
  ww_not_null(&w2, NDoc, cover_id);
  VecNDoc *covered = db_all_w(db, NDoc, &w2);
  ASSERT_EQ(covered->n, 1);
  ASSERT_STR_EQ(covered->at[0].title, "With cover");
}

// The DDL a model generates: the reference the model declares is in it, and the nullable
// column carries no NOT NULL. Types are spelled per dialect (INTEGER here, bigint on
// postgres), so the check is case-insensitive.
static const char *lowered(Arena *a, str s) {
  char *p = arena_alloc(a, s.n + 1);
  for (size_t i = 0; i < s.n; i++) p[i] = (char)(s.p[i] >= 'A' && s.p[i] <= 'Z' ? s.p[i] + 32 : s.p[i]);
  p[s.n] = 0;
  return p;
}

TEST(the_generated_ddl_carries_the_reference_and_no_not_null) {
  Arena *a = wc_arena_get();
  const char *ddl = lowered(a, model_sql_create_table(a, NDoc_modelof(), "n_docs"));
  ASSERT_TRUE(strstr(ddl, "cover_id integer references n_covers(id)") != nullptr);
  ASSERT_TRUE(strstr(ddl, "cover_id integer not null") == nullptr);
  // a plain field keeps its own spelling
  ASSERT_TRUE(strstr(ddl, "pages integer") != nullptr);
}TEST_MAIN()
