// dx2: one test per surface rule, kept so the framework cannot quietly drift back to a
// workaround. Each test is the smallest program that would need the workaround, written
// without it.
//
//   01 keyword routes / typed views      07 seed from JSON
//   02 <html>/<body> attributes          08 collections: filter, sort, new
//   03 text() is not a format string     09 body errors as data
//   04 named routes, url_to, link_to     10 enhance() and scripted_hide()
//   05 one attribute vocabulary          11 ASSERT_JSON / ASSERT_HTML
//   06 schema drift                      12 boot diagnostics (webc check's half)
#include <unistd.h>

#include <webc/test.h>

// ---------------------------------------------------------------- 01 typed views

typedef struct SectionCtx {
  Text heading;
  i32 count;
} SectionCtx;

// The view names its context type, which is what lets `render()` fill it by field
// name — and the compiler check every name against the struct.
declare_view(Dx2Section, SectionCtx);
view(Dx2Section, SectionCtx *c) {
  el("main") {
    el("h1") { text(c->heading); }
    el("p") { class_("count"); text("%d stories", c->count); }
  }
}

GET("/dx2/section/:slug") {
  SectionCtx probe = {.heading = (Text)"Design", .count = 7};
  (void)probe;
  return render(Dx2Section, .heading = (Text)"Design", .count = 7);
}

TEST(a_view_that_names_its_context_is_filled_by_field) {
  Response r = webc_test_request("GET", "/dx2/section/design");
  ASSERT_EQ(r.status, 200);
  // the structure, not the substring: every node carries a hydration anchor, so a
  // substring check for "<h1>Design</h1>" would be a test of the renderer's internals
  ASSERT_HTML(r, "main");
  ASSERT_HTML(r, "main > h1");
  ASSERT_HTML_TEXT(r, "main > p.count", "7 stories");
}

// `must()` is the other half of the same idea: a lookup that cannot be absent
// answers 404 with a sentence, instead of a page with a null in it.
GET("/dx2/must/:slug") {
  const char *hit = strcmp(path("slug"), "here") == 0 ? "found" : nullptr;
  const char *got = must(hit, "no such thing: %.*s", (int)strlen(path("slug")), path("slug"));
  return res_text(200, str_lit(got));
}

TEST(must_answers_404_with_the_sentence) {
  Response ok = webc_test_request("GET", "/dx2/must/here");
  ASSERT_EQ(ok.status, 200);
  Response miss = webc_test_request("GET", "/dx2/must/gone");
  ASSERT_EQ(miss.status, 404);
  ASSERT_STR_CONTAINS(miss, "no such thing: gone");
}

// ---------------------------------------------------------------- 02 doc attributes

GET("/dx2/docattrs") {
  html_attr("data-theme", "dark");
  body_attr("class", "is-article");
  return res_html(200, str_lit("<!doctype html><html lang=\"en\"><head><title>t</title>"
                                 "</head><body><p>x</p></body></html>"));
}

TEST(html_and_body_attributes_reach_the_document) {
  Response r = webc_test_request("GET", "/dx2/docattrs");
  ASSERT_STR_CONTAINS(r, "<html lang=\"en\" data-theme=\"dark\">");
  ASSERT_STR_CONTAINS(r, "<body class=\"is-article\">");
}

// ---------------------------------------------------------------- 03 text()

static void text_views(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("p") {
    class_("a");
    text("50% off");            // one argument: a value, `%` and all
  }
  el("p") {
    class_("b");
    const char *runtime = "100% organic";
    text(runtime);              // one argument is a value; a format would print "1000rganic"
  }
  el("p") {
    class_("c");
    text("%s/%d", "fmt", 7);    // two or more: a format, as before
  }
}

GET("/dx2/text") { return wc_render_fragment(200, text_views, nullptr); }

TEST(text_is_a_value_with_one_argument_and_a_format_with_more) {
  Response r = webc_test_request("GET", "/dx2/text");
  ASSERT_HTML_TEXT(r, "p.a", "50% off");
  ASSERT_HTML_TEXT(r, "p.b", "100% organic");
  ASSERT_HTML_TEXT(r, "p.c", "fmt/7");
}

// ---------------------------------------------------------------- 04 named routes

// WEBC_MODEL defines the struct: writing the typedef as well is a duplicate
// definition, which clang 21 accepts as an extension and clang 18 rejects.
WEBC_MODEL(Dx2Thing, (id, i64, .pk = true), (slug, Text, .required = true));
WEBC_MODEL_ROUTE(Dx2Thing, "/dx2/thing/:slug");

static void named_views(WBuild *b, void *ctx) {
  (void)b;
  const Dx2Thing *t = ctx;
  el("a") {
    class_("thing");
    href(url_to("dx2/thing", "slug", t->slug));
    text("%s", t->slug);
  }
  el("a") {
    class_("by-model");
    href(link_to(Dx2Thing, t));
    text("%s", t->slug);
  }
}

GET("/dx2/named") {
  static Dx2Thing t = {.id = 1, .slug = (Text)"the-last-ferry"};
  return wc_render_fragment(200, named_views, &t);
}
GET("/dx2/thing/:slug", "dx2/thing") { return res_text(200, str_lit("a thing")); }

TEST(url_to_builds_the_url_from_the_registered_pattern) {
  Response r = webc_test_request("GET", "/dx2/named");
  ASSERT_HTML_TEXT(r, "a.thing", "the-last-ferry");
  ASSERT_STR_CONTAINS(r, "href=\"/dx2/thing/the-last-ferry\"");
  // the model that names its route needs no arguments at the call site
  ASSERT_STR_CONTAINS(r, "class=\"by-model\" href=\"/dx2/thing/the-last-ferry\"");
  ASSERT_HTML(r, "a.by-model[href=\"/dx2/thing/the-last-ferry\"]");
  // and the URL it built is a URL that routes
  Response hit = webc_test_request("GET", "/dx2/thing/the-last-ferry");
  ASSERT_EQ(hit.status, 200);
}

TEST(url_to_says_which_parameter_is_missing) {
  ASSERT_PANICS(url_to("dx2/thing", "wrong", "x"));   // wants 'slug'
  ASSERT_PANICS(url_to("no/such/route", "slug", "x")); // no route of that name
}

// ---------------------------------------------------------------- 05 attributes

static void attr_views(WBuild *b, void *ctx) {
  (void)b;
  const Text url = (Text)"/art/x.svg";
  el("img") {
    src(url);             // a Text, a str or a literal: no cast, no `_c`
    alt("a plate");
    attr("width", 640);   // i64 in, attribute out
    class_("plate lead");
    style("width:50%");
  }
}

GET("/dx2/attrs") { return wc_render_fragment(200, attr_views, nullptr); }

TEST(every_attribute_takes_a_text_a_str_or_a_literal) {
  Response r = webc_test_request("GET", "/dx2/attrs");
  ASSERT_STR_CONTAINS(r, "src=\"/art/x.svg\"");
  ASSERT_STR_CONTAINS(r, "alt=\"a plate\"");
  ASSERT_STR_CONTAINS(r, "width=\"640\"");
  ASSERT_STR_CONTAINS(r, "class=\"plate lead\"");
  // a style is a declaration list, not a format: 50% must survive
  ASSERT_STR_CONTAINS(r, "style=\"width:50%\"");
  ASSERT_HTML(r, "img.plate[width=\"640\"]");
}

// ---------------------------------------------------------------- 06 schema drift

WEBC_MODEL(Dx2Row, (id, i64, .pk = true), (name, Text, .required = true), (views, i64));

static WDB *drift_db(const char *ddl) {
  WDB *db = db_open("sqlite::memory:");
  if (!db) return nullptr;
  WRes r = db_exec_sql(db, ddl);
  if (!r.ok) {
    db_close(db);
    return nullptr;
  }
  return db;
}

TEST(schema_drift_is_reported_by_the_model_registry) {
  // the table the model wants, minus one column
  WDB *db = drift_db("create table dx2_rows (id integer primary key, name text)");
  ASSERT_NOT_NULL(db);
  ASSERT_FALSE(webc_schema_report(db, stdout));
  db_close(db);
  // and agreement is green
  WDB *ok = drift_db("create table dx2_rows (id integer primary key, name text, views integer);"
                     "create table dx2_things (id integer primary key, slug text not null);"
                     "create table dx2_signups (id integer primary key, email text not null)");
  ASSERT_NOT_NULL(ok);
  ASSERT_TRUE(webc_schema_report(ok, stdout));
  db_close(ok);
}

TEST(every_model_registers_itself) {
  bool seen = false;
  for (i32 i = 0; i < model_registry_count(); i++)
    if (strcmp(model_registry_at(i)->name, "Dx2Row") == 0) seen = true;
  ASSERT_TRUE(seen);
}

// ---------------------------------------------------------------- 07 seeding

TEST(seed_from_json_loads_rows_once) {
  WDB *db = drift_db("create table dx2_rows (id integer primary key, name text, views integer)");
  ASSERT_NOT_NULL(db);
  db_set_default(db);
  char path[] = "/tmp/webc_dx2_seed_XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  const char *json = "[{\"name\":\"one\",\"views\":3},{\"name\":\"two\",\"views\":9}]";
  ASSERT_EQ(write(fd, json, strlen(json)), (i64)strlen(json));
  close(fd);
  ASSERT_EQ(seed_from_json(Dx2Row_modelof(), path), 2);
  // a second run is a no-op: a seed that runs on every boot must not double the site
  ASSERT_EQ(seed_from_json(Dx2Row_modelof(), path), 0);
  ASSERT_EQ(db_count(Dx2Row), 2);
  unlink(path);
  db_set_default(nullptr);
  db_close(db);
}

// ---------------------------------------------------------------- 08 collections

Vec(Dx2Row)

static VecDx2Row three_rows(void) {
  VecDx2Row v = VecDx2Row_with(wc_arena_get());
  push(v, ((Dx2Row){.id = 1, .name = (Text)"alpha", .views = 3}));
  push(v, ((Dx2Row){.id = 2, .name = (Text)"beta", .views = 90}));
  push(v, ((Dx2Row){.id = 3, .name = (Text)"alpha", .views = 12}));
  return v;
}

TEST(vec_new_is_an_empty_collection_in_the_current_arena) {
  VecDx2Row *v = vec_new(Dx2Row);
  ASSERT_NOT_NULL(v);
  ASSERT_EQ(v->n, 0);
  ASSERT_NOT_NULL(v->desc);
}

TEST(vec_where_str_and_i64_filter_by_a_field) {
  VecDx2Row v = three_rows();
  VecDx2Row *alpha = vec_where_str(&v, Dx2Row, name, "alpha");
  ASSERT_EQ(alpha->n, 2);
  ASSERT_STR_EQ(alpha->at[0].name, "alpha");
  ASSERT_STR_EQ(alpha->at[1].name, "alpha");
  VecDx2Row *id2 = vec_where_i64(&v, Dx2Row, id, 2);
  ASSERT_EQ(id2->n, 1);
  ASSERT_STR_EQ(id2->at[0].name, "beta");
}

TEST(sort_orders_by_a_field_and_stays_stable) {
  VecDx2Row v = three_rows();
  sort_desc(&v, Dx2Row, views);
  ASSERT_EQ(v.at[0].views, 90);
  ASSERT_EQ(v.at[1].views, 12);
  ASSERT_EQ(v.at[2].views, 3);
  // stability, on a fresh collection so the order is the one written above:
  // the two "alpha" rows come back as 1 then 3, not reversed
  VecDx2Row fresh = three_rows();
  sort_by(&fresh, Dx2Row, name);
  ASSERT_STR_EQ(fresh.at[0].name, "alpha");
  ASSERT_EQ(fresh.at[0].id, 1);
  ASSERT_EQ(fresh.at[1].id, 3);
  ASSERT_STR_EQ(fresh.at[2].name, "beta");
}

// ---------------------------------------------------------------- 09 body errors

WEBC_MODEL(Dx2Signup, (id, i64, .pk = true), (email, Text, .required = true, .format = "email"));

POST("/dx2/signup") {
  Dx2Signup s = {0};
  if (!body_try(Dx2Signup, &s)) {
    return res_text(422, str_fmt(wc_arena_get(), "%s|%s|%d", body_error_field(),
                                   werr_kind_name((WErrKind)body_error_kind()),
                                   (int)body_error_kind()));
  }
  return res_text(200, str_lit("ok"));
}

TEST(a_failed_body_says_which_field_and_what_kind) {
  Response missing = webc_test_request_ex("POST", "/dx2/signup",
                                          "content-type: application/x-www-form-urlencoded\r\n",
                                          "email=", 6);
  ASSERT_EQ(missing.status, 422);
  ASSERT_STR_CONTAINS(missing, "email|validation");
  Response malformed = webc_test_request_ex("POST", "/dx2/signup", "content-type: application/json\r\n",
                                            "{oops", 5);
  ASSERT_EQ(malformed.status, 422);
}

// ---------------------------------------------------------------- 10 enhance()

static void enhance_views(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("form") {
    class_("theme");
    scripted_hide("themeswitch");
    post_to("/theme");
    el("button") { text("Night"); }
  }
  enhance("reader") {
    el("p") {
      class_("fallback");
      text("works without JavaScript");
    }
  }
}

GET("/dx2/enhance") { return wc_render_fragment(200, enhance_views, nullptr); }

TEST(enhance_marks_the_mount_point_and_the_fallback) {
  Response r = webc_test_request("GET", "/dx2/enhance");
  ASSERT_HTML(r, "div#reader[data-enhance=\"reader\"]");
  ASSERT_HTML_TEXT(r, "#reader p.fallback", "works without JavaScript");
  ASSERT_HTML(r, "form.theme[data-scripted-hide=\"themeswitch\"]");
}

// ---------------------------------------------------------------- 11 assertions

GET("/dx2/json") {
  return wc_res_json_bytes(200, str_lit("{\"data\":[{\"id\":1,\"views\":42,\"name\":\"a\"}]}"));
}

TEST(json_assertions_read_a_path) {
  Response r = webc_test_request("GET", "/dx2/json");
  ASSERT_JSON(r, "data.0.views", "42");
  ASSERT_JSON_STR(r, "data[0].name", "a");
  ASSERT_JSON(r, "data[0].id", "1");
  JVal v;
  ASSERT_TRUE(wc_json_at_str(WC_TEST_VIEW(r), "data.0.name").p != nullptr);
}

TEST(html_assertions_check_structure_not_substrings) {
  Response r = webc_test_request("GET", "/dx2/docattrs");
  ASSERT_HTML(r, "html");
  ASSERT_HTML(r, "body > p");
  ASSERT_HTML_ABSENT(r, "article.lead");
  ASSERT_HTML_COUNT(r, "p", 1);
  ASSERT_FALSE(wc_html_selector_ok(""));
}

// ---------------------------------------------------------------- 12 boot hooks

TEST(boot_diagnostics_answers_for_check_and_seed) {
  // the pipeline `webc check` drives: schema over the real registry
  WDB *db = drift_db("create table dx2_rows (id integer primary key, name text, views integer)"
                     "; create table dx2_things (id integer primary key, slug text not null)"
                     "; create table dx2_signups (id integer primary key, email text not null)");
  ASSERT_NOT_NULL(db);
  ASSERT_TRUE(webc_schema_report(db, stdout));
  db_close(db);
}
TEST_MAIN();
