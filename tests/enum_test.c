// WEBC_ENUM: a closed set as a model field — an int in the struct, its name in
// JSON, forms and SQL text, and a request that sends a stranger is a 422 naming
// the field (written while turning Flowdeck's hand-rolled lanes into a field).
#include <webc/test.h>

WEBC_ENUM(Lane, backlog, todo, doing, done)
WEBC_MODEL(Card, (id, i64, .pk = true),
                  (lane, Lane, .required = true),
                  (title, Text, .required = true, .max = 40))
WEBC_MODEL_INTERNAL(Card)

TEST(the_generated_helpers) {
  ASSERT_EQ(Lane_index(str_lit("doing")), 2);
  ASSERT_EQ(Lane_index(str_lit("limbo")), -1);
  ASSERT_TRUE(Lane_ok(str_lit("backlog")));
  ASSERT_STR_EQ(Lane_name(3), "done");
  ASSERT_STR_EQ(Lane_name(99), "");
}

TEST(json_round_trips_by_name) {
  Card c = {.id = 1, .lane = 2, .title = "a"};
  str json = model_to_json(nullptr, Card_modelof(), &c);
  ASSERT_STR_CONTAINS(json, "\"lane\":\"doing\"");

  Card back;
  Arena *a = arena_new(4096);
  ASSERT_TRUE(model_read_json_val(a, Card_modelof(),
                                  json_parse(a, json).v, &back));
  ASSERT_EQ(back.lane, 2); // Lane_doing
  arena_destroy(a);
}

TEST(a_stranger_is_a_422_naming_the_field) {
  Arena *a = arena_new(4096);
  Card bad = {0};
  JParse p = json_parse(a, str_lit("{\"lane\":\"limbo\",\"title\":\"x\"}"));
  WModelParse r = model_from_json_val(a, Card_modelof(), p.v, &bad);
  ASSERT_FALSE(r.ok);                       // the parse itself refuses the stranger
  ASSERT_TRUE(r.errs.n >= 1);
  ASSERT_STR_EQ(r.errs.items[0].field, "lane");
  arena_destroy(a);
}

TEST(forms_speak_the_name_too) {
  Arena *a = arena_new(4096);
  Card c;
  WModelParse r = model_from_form(a, Card_modelof(), str_lit("lane=done&title=x"), &c);
  ASSERT_TRUE(r.ok);
  ASSERT_EQ(c.lane, 3);
  arena_destroy(a);
}

TEST_MAIN()
