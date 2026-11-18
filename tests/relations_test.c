// relations: one declaration, one query, and `ref()` in the view with no query at all.
//
// The alternative is to do one of two things when an app needs a story's section: a query
// per row, or a `Site` struct that carries every collection into every function. Neither is
// a relation — both are what you settle for when there is nowhere to declare one.
#include <webc/test.h>

WEBC_MODEL(RelSection, (id, i64, .pk = true), (slug, str, .unique = true),
                         (name, str, .required = true))
WEBC_MODEL(RelStory, (id, i64, .pk = true), (slug, str, .unique = true), (section, str),
                     (title, str, .required = true))

WEBC_VEC_DEFINE(VecRelSection, vec_rel_section, RelSection)
WEBC_VEC_DEFINE(VecRelStory, vec_rel_story, RelStory)

WEBC_MODEL_REF(RelStory, section, RelSection, slug)

static WDB *fresh(void) {
  WDB *db = db_open("sqlite::memory:");
  ASSERT_NOT_NULL(db);
  ASSERT_TRUE(db_exec_sql(db,
                         "create table rel_sections (id integer primary key autoincrement, "
                         "slug text unique, name text not null)").ok);
  ASSERT_TRUE(db_exec_sql(db,
                         "create table rel_stories (id integer primary key autoincrement, "
                         "slug text unique, section text, title text not null)").ok);
  RelSection city = {.slug = str_lit("city"), .name = str_lit("The City")};
  RelSection science = {.slug = str_lit("science"), .name = str_lit("Science & Nature")};
  ASSERT_TRUE(db_insert(db, RelSection, &city));
  ASSERT_TRUE(db_insert(db, RelSection, &science));
  RelStory a = {.slug = str_lit("ferry"), .section = str_lit("city"), .title = str_lit("The Ferry")};
  RelStory b = {.slug = str_lit("tide"), .section = str_lit("science"), .title = str_lit("The Tide")};
  RelStory c = {.slug = str_lit("barge"), .section = str_lit("city"), .title = str_lit("The Barge")};
  ASSERT_TRUE(db_insert(db, RelStory, &a));
  ASSERT_TRUE(db_insert(db, RelStory, &b));
  ASSERT_TRUE(db_insert(db, RelStory, &c));
  return db;
}

TEST(prefetch_links_a_whole_list_in_one_query) {
  WDB *db = fresh();
  VecRelStory *all = db_all_ordered(db, RelStory, "id asc");
  ASSERT_EQ(all->n, 3);

  // Before: nothing is linked, and `ref` says so rather than guessing.
  ASSERT_NULL(ref(RelStory, &all->at[0], section));

  prefetch_on(db, all, RelStory, section);

  for (i32 i = 0; i < all->n; i++) {
    const RelStory *st = &all->at[i];
    const RelSection *sec = ref(RelStory, st, section);
    ASSERT_NOT_NULL(sec);
    ASSERT_STR_EQ(sec->name, i == 1 ? "Science & Nature" : "The City");
  }
  // Two different stories of the same desk share one row: three links, two sections.
  ASSERT_TRUE(ref(RelStory, &all->at[0], section) == ref(RelStory, &all->at[2], section));
  db_close(db);
}

// The whole point of a relation is the view: `prefetch` in the handler, `ref()` in the
// template, no query in between. It did not work — the render swaps the arena for its own
// scratch (`wcomp_html_ctx`), and `ref()` matched links by arena, so *every* `ref()` in
// *every* view was null. The port that hit this wrote its own `vt_ref_epoch()` and called it
// around every render, which is the shape of a framework bug with a workaround on top.
typedef struct RelCtx {
  WPage page;
  VecRelStory *stories;
} RelCtx;
declare_view(WithSection, RelCtx);

view(WithSection, RelCtx *c) {
  el("ul") {
    const RelStory *st;
    each(st, c->stories) {
      el("li") {
        text("%s in ", st->title);
        const RelSection *sec = ref(RelStory, st, section);
        text("%s", sec ? sec->name.p : "(none)");
      }
    }
  }
}

GET("/probe/with-sections") {
  // The link is made in the *request* arena, which is where `prefetch` puts it. The render
  // then runs the view in a scratch arena of its own, which is what must not break it.
  VecRelStory *rows = wc_alloc_zero(sizeof *rows);
  *rows = vec_rel_story_with(wc_arena_get());
  RelSection city = {.id = 1, .slug = str_lit("city"), .name = str_lit("The City")};
  RelStory one = {.id = 1, .slug = str_lit("a"), .section = str_lit("city"),
                  .title = str_lit("A story")};
  push(*rows, one);
  wc_ref_link("RelStory", 1, "section", &city, wc_arena_get());
  return render(WithSection, .page = head(.title = "sections"), .stories = rows);
}

TEST(a_view_reads_the_relation_the_handler_linked) {
  webc_use_reset();
  Response r = webc_test_request("GET", "/probe/with-sections");
  ASSERT_EQ(r.status, 200);
  str body = (str){r.body.p, r.body.n};
  ASSERT_TRUE(str_contains(body, str_lit("A story in The City")));
  ASSERT_TRUE(!str_contains(body, str_lit("(none)")));
}

TEST(prefetch_of_a_field_nobody_declared_says_which_models_are_wrong) {
  WDB *db = fresh();
  VecRelStory *all = db_all_ordered(db, RelStory, "id asc");
  // The declaration above is RelStory.section; "author" was never declared.
  ASSERT_PANICS((void)wc_prefetch(db, RelStory_modelof(), (const void *)&all, "author"));
  db_close(db);
}

TEST(a_declaration_says_which_model_and_which_key) {
  const char *owner = nullptr, *field = nullptr, *target = nullptr, *key = nullptr;
  bool found = false;
  for (i32 i = 0; i < wc_ref_rule_count(); i++) {
    wc_ref_rule_at(i, &owner, &field, &target, &key);
    if (strcmp(owner, "RelStory") == 0 && strcmp(field, "section") == 0) {
      ASSERT_STR_EQ(target, "RelSection");
      ASSERT_STR_EQ(key, "slug");
      found = true;
    }
  }
  ASSERT_TRUE(found);
}
TEST_MAIN()
