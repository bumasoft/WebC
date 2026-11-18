// model: descriptor table, JSON in/out, form parsing, validation, SQL text.
#include <webc/test.h>

WEBC_MODEL(Todo, (id, i64, .pk = true),
           (title, str, .required = true, .max = 20),
           (done, bool),
           (tags, str))

WEBC_VEC_DEFINE(VecStr2, vec_str2, str)

TEST(descriptor_table) {
  const WModel *m = Todo_modelof();
  ASSERT_EQ(m->n, 4);
  ASSERT_STR_EQ(str_lit(m->table), "todos");
  ASSERT_EQ(offsetof(Todo, title), m->fields[1].off);
  ASSERT(m->fields[0].q.pk);
  ASSERT(m->fields[1].q.required);
}

TEST(json_in_and_out) {
  Arena *a = arena_new(16384);
  const WModel *m = Todo_modelof();
  Todo t = {0};
  WModelParse p = model_from_json(
      a, m, str_lit("{\"id\":9,\"title\":\"ship it\",\"done\":true,\"extra\":\"ignored\"}"), &t);
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(t.id, 9);
  ASSERT_TRUE(t.done);
  ASSERT_STR_EQ(t.title, "ship it");
  ASSERT_STR_EQ(model_to_json(a, m, &t),
                "{\"id\":9,\"title\":\"ship it\",\"done\":true,\"tags\":\"\"}");
  arena_destroy(a);
}

TEST(form_parsing) {
  Arena *a = arena_new(16384);
  const WModel *m = Todo_modelof();
  Todo f = {0};
  WModelParse pf = model_from_form(a, m, str_lit("title=write+the+docs&done=on"), &f);
  ASSERT_TRUE(pf.ok);
  ASSERT_STR_EQ(f.title, "write the docs");   // + is a space
  ASSERT_TRUE(f.done);
  arena_destroy(a);
}

TEST(validation_says_which_rule) {
  const WModel *m = Todo_modelof();
  struct {
    const char *name;
    const char *title;
    const char *want;   // null means "valid"
  } rows[] = {
      { "missing", "", "required" },
      { "too long", "0123456789012345678901234567890", "must be at most 20 characters" },
      { "fine", "short", nullptr },
  };
  SUBTESTS(rows) {
    Todo todo = { .title = str_lit(r->title) };
    WErrs errs = {0};
    bool ok = model_validate(m, &todo, &errs);
    if (r->want == nullptr) {
      ASSERT_TRUE(ok);
    } else {
      ASSERT_FALSE(ok);
      ASSERT_EQ(errs.n, 1);
      ASSERT_STR_EQ(errs.items[0].msg, r->want);
    }
  }
}

TEST(sql_text) {
  Arena *a = arena_new(16384);
  const WModel *m = Todo_modelof();
  ASSERT_STR_EQ(model_sql_columns(a, m), "id, title, done, tags");
  ASSERT_STR_EQ(model_sql_insert(a, m, m->table),
                "INSERT INTO todos (id, title, done, tags) VALUES (?, ?, ?, ?)");
  ASSERT_STR_EQ(model_sql_update(a, m, m->table),
                "UPDATE todos SET title = ?, done = ?, tags = ? WHERE id = ?");
  ASSERT_STR_EQ(model_sql_column_type(&m->fields[1]), "TEXT");
  ASSERT_STR_EQ(model_sql_column_type(&m->fields[2]), "INTEGER");
  arena_destroy(a);
}

TEST(read_json_zeroes_first_so_a_partial_document_cannot_reuse_stale_fields) {
  // The browser-half spelling: a seed or an SSE frame arrives as text, and the decoded
  // row deserves the guarantee a request body gets — no field left over from the row
  // that used this memory before it. (Also the proof the codec needs no server: this is
  // the same code the wasm half links.)
  Arena *a = arena_new(4096);
  Todo t = {.id = 41, .title = str_lit("stale"), .done = true};
  str both = str_lit("{\"id\": 7, \"title\": \"fresh\"}");
  ASSERT_TRUE(model_read_json(a, Todo_modelof(), both, &t));
  ASSERT_EQ((int)t.id, 7);
  ASSERT_TRUE(str_eq(t.title, str_lit("fresh")));
  ASSERT_TRUE(!t.done); // zeroed: the document said nothing about it
  // a document that omits the title: the stale string must NOT survive
  str only_id = str_lit("{\"id\": 8}");
  ASSERT_TRUE(model_read_json(a, Todo_modelof(), only_id, &t));
  ASSERT_EQ((int)t.id, 8);
  ASSERT_TRUE(!t.title.n);
}

// A model is as wide as the preprocessor machinery allows, and that was 32: field 33
// produced "pasting formed 'WC_MAP_S_('" — a message about a macro, explained as a fault in
// the field it named. The generator emits 64 slots now, and this row is 40 of them, so the
// day the table shrinks again a *test* says so instead of an app's build.
WEBC_MODEL(Wide, (id, i64, .pk = true), (f01, i64),
                        (f02, i64),
                        (f03, i64),
                        (f04, i64),
                        (f05, i64),
                        (f06, i64),
                        (f07, i64),
                        (f08, i64),
                        (f09, i64),
                        (f10, i64),
                        (f11, i64),
                        (f12, i64),
                        (f13, i64),
                        (f14, i64),
                        (f15, i64),
                        (f16, i64),
                        (f17, i64),
                        (f18, i64),
                        (f19, i64),
                        (f20, i64),
                        (f21, i64),
                        (f22, i64),
                        (f23, i64),
                        (f24, i64),
                        (f25, i64),
                        (f26, i64),
                        (f27, i64),
                        (f28, i64),
                        (f29, i64),
                        (f30, i64),
                        (f31, i64),
                        (f32, i64),
                        (f33, i64),
                        (f34, i64),
                        (f35, i64),
                        (f36, i64),
                        (f37, i64),
                        (f38, i64),
                        (f39, i64),
                        (f40, i64))

TEST(a_row_of_forty_fields_is_a_model) {
  ASSERT_EQ(Wide_modelof()->n, 41);
  const WField *last = nullptr;
  for (i32 i = 0; i < Wide_modelof()->n; i++)
    if (strcmp(Wide_modelof()->fields[i].name, "f40") == 0) last = &Wide_modelof()->fields[i];
  ASSERT_NOT_NULL(last);
  ASSERT_EQ((int)last->kind, (int)WK_I64);
}

TEST_MAIN()