// json: writer structure, parser kinds, depth limit, unknown keys.
#include <webc/test.h>

TEST(writer_builds_structure) {
  Arena *a = arena_new(8192);
  JsonWriter w;
  jw_init(&w, a, 64);
  jw_obj_begin(&w);
  jw_key(&w, "n");
  jw_i64(&w, 7);
  jw_key(&w, "s");
  jw_cstr(&w, "hi");
  jw_key(&w, "arr");
  jw_arr_begin(&w);
  jw_bool(&w, true);
  jw_null(&w);
  jw_f64(&w, 1.5);
  jw_arr_end(&w);
  jw_obj_end(&w);
  ASSERT_STR_EQ(jw_finish(&w), "{\"n\":7,\"s\":\"hi\",\"arr\":[true,null,1.5]}");
  arena_destroy(a);
}

TEST(parser_kinds) {
  Arena *a = arena_new(8192);
  JParse p = json_parse(a, str_lit("{\"a\":1,\"b\":[1,2,{\"c\":\"x\"}],\"d\":null}"));
  ASSERT_TRUE(p.ok);
  JVal *b = json_get(p.v, "b");
  ASSERT_NOT_NULL(b);
  ASSERT_EQ(b->kind, J_ARR);
  ASSERT_EQ(b->n, 3);
  JVal *c = json_get(json_at(b, 2), "c");
  ASSERT_NOT_NULL(c);
  ASSERT_EQ(c->kind, J_STR);
  ASSERT_STR_EQ(c->s, "x");
  ASSERT_EQ(json_get(p.v, "d")->kind, J_NULL);
  arena_destroy(a);
}

TEST(depth_is_bounded) {
  Arena *a = arena_new(8192);
  // 200 deep is fine, 2000 is refused (never a stack blowup)
  Buf deep;
  buf_init(&deep, a, 256);
  for (int i = 0; i < 200; i++) buf_putc(&deep, '[');
  for (int i = 0; i < 200; i++) buf_putc(&deep, ']');
  ASSERT(json_parse(a, buf_str(&deep)).ok);

  Buf deeper;
  buf_init(&deeper, a, 4096);
  for (int i = 0; i < 2000; i++) buf_putc(&deeper, '[');
  for (int i = 0; i < 2000; i++) buf_putc(&deeper, ']');
  ASSERT_FALSE(json_parse(a, buf_str(&deeper)).ok);
  arena_destroy(a);
}

TEST(malformed_is_an_error_not_a_crash) {
  Arena *a = arena_new(8192);
  JParse bad = json_parse(a, str_lit("{\"a\":}"));
  ASSERT_FALSE(bad.ok);
  ASSERT_GT(bad.e.len, 0);
  arena_destroy(a);
}

// ---------------------------------------------------------------- a list, two spellings

WEBC_MODEL(JPost, (id, i64, .pk = true), (slug, str, .unique = true), (title, str))
Vec(JPost);

GET("/probe/posts") {
  VecJPost *v = vec_new(JPost);
  JPost a = {.id = 1, .slug = str_lit("a"), .title = str_lit("first")};
  JPost b = {.id = 2, .slug = str_lit("b"), .title = str_lit("second")};
  push(*v, a);
  push(*v, b);
  return json(200, v);
}

GET("/probe/posts-wrong") {
  VecJPost *v = vec_new(JPost);
  JPost a = {.id = 1, .slug = str_lit("a"), .title = str_lit("first")};
  push(*v, a);
  // One `&` too many. It compiles (a `Vec **` is a `void *`) and reads the front of the
  // pointer as a descriptor list, which segfaults inside the writer, unnamed.
  return json(200, &v);
}

GET("/probe/posts-typed") {
  VecJPost *v = vec_new(JPost);
  JPost a = {.id = 1, .slug = str_lit("a"), .title = str_lit("first")};
  push(*v, a);
  return json_list(200, v);
}

TEST(a_collection_is_encoded_from_its_own_descriptor) {
  Response r = webc_test_request("GET", "/probe/posts");
  ASSERT_EQ(r.status, 200);
  ASSERT_TRUE(str_contains((str){r.body.p, r.body.n}, str_lit("\"title\":\"second\"")));

  Response typed = webc_test_request("GET", "/probe/posts-typed");
  ASSERT_EQ(typed.status, 200);
  ASSERT_TRUE(str_eq((str){typed.body.p, typed.body.n},
                     str_lit("[{\"id\":1,\"slug\":\"a\",\"title\":\"first\"}]")));
}

TEST(a_pointer_one_level_too_deep_is_a_500_not_a_crash) {
  // The magic word is what turns the worst failure in the framework into a message: no
  // handler name, no line, just a dead process — found while porting, in a route that
  // looked exactly like the one above.
  Response r = webc_test_request("GET", "/probe/posts-wrong");
  ASSERT_EQ(r.status, 500);
  str body = (str){r.body.p, r.body.n};
  ASSERT_STR_CONTAINS(body, "json_list");
}

TEST_MAIN()