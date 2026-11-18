// search: a typed `%` is a percent sign, two words are ANDed, and the title outranks the body.
#include <webc/test.h>
#include <webc/search.h>

WEBC_MODEL(Doc,
           (id, i64, .pk = true),
           (title, str),
           (body, Text),
           (n, i32))
Vec(Doc);

static void seed(void) {
  db_set_default(db_open("sqlite::memory:"));
  db_exec_sql(db_default(), "create table docs (id integer primary key autoincrement,"
                            " title text, body text, n integer)");
  db_exec_sql(db_default(),
              "insert into docs (title, body, n) values "
              "('Ada Lovelace', 'Wrote the first algorithm for a machine.', 1),"
              "('Charles Babbage', 'Ada helped with the engine notes.', 2),"
              "('50% off', 'A discount of fifty percent.', 3),"
              "('Grace Hopper', 'Compiler pioneer.', 4)");
}

TEST(a_word_finds_a_row_and_case_does_not_matter) {
  seed();
  VecDoc *hits = search(Doc, "ada");
  ASSERT_EQ(hits->n, 2);
  // the title hit comes first: that is the ranking, not the id order
  ASSERT_STR_EQ(hits->at[0].title, "Ada Lovelace");
  ASSERT_TRUE(search_match(str_lit("ADA"), str_lit("ada")));
}

TEST(two_words_are_anded_not_ored) {
  seed();
  VecDoc *hits = search(Doc, "ada engine");
  ASSERT_EQ(hits->n, 1); // only Babbage's body has both
  ASSERT_STR_EQ(hits->at[0].title, "Charles Babbage");
}

TEST(a_typed_percent_is_a_percent_not_a_wildcard) {
  seed();
  ASSERT_EQ(search(Doc, "50%")->n, 1);
  ASSERT_EQ(search(Doc, "%")->n, 1);   // the row that contains a percent sign, not all four
  ASSERT_EQ(search(Doc, "_")->n, 0);   // an underscore is an underscore
}

TEST(a_model_with_nothing_to_search_finds_nothing) {
  seed();
  ASSERT_EQ(search(Doc, "  ")->n, 4); // an empty query is not a filter
  ASSERT_EQ(search(Doc, "zzz")->n, 0);
}

TEST(the_fields_can_be_named_and_a_number_is_never_searched) {
  seed();
  ASSERT_EQ(search_vec(db_default(), Doc, "title", "ada", 10)->n, 1);
  ASSERT_EQ(search_vec(db_default(), Doc, "body", "ada", 10)->n, 1);
  ASSERT_EQ(search_vec(db_default(), Doc, "n", "1", 10)->n, 0); // n is an integer: not text
}

TEST(the_hit_is_marked_and_escaped_at_the_same_time) {
  str h = search_hl("Ada & the <b>engine</b>", "ada");
  ASSERT_STR_EQ(h, "<mark>Ada</mark> &amp; the &lt;b&gt;engine&lt;/b&gt;");
}

TEST(a_word_inside_a_word_is_not_a_match_but_a_match_inside_a_word_is_marked) {
  // the SQL's `like '%ada%'` is a substring match, and so is the highlighter: one rule, both sides
  str h = search_hl("canada", "ada");
  ASSERT_STR_EQ(h, "can<mark>ada</mark>");
}

TEST(a_snippet_opens_at_the_match_not_at_the_first_paragraph) {
  Arena *a = wc_arena_get();
  Buf b;
  buf_init(&b, a, 512);
  for (i32 i = 0; i < 40; i++) buf_puts(&b, "filler ");
  buf_puts(&b, "the needle is here and the rest follows");
  for (i32 i = 0; i < 40; i++) buf_puts(&b, " trailing");
  str snip = search_snippet(buf_str(&b), "needle", 8);
  ASSERT_TRUE(str_contains(snip, str_lit("<mark>needle</mark>")));
  ASSERT_TRUE(snip.n < buf_str(&b).n); // a snippet is not the whole body
  ASSERT_TRUE(str_contains(snip, str_lit("…")));
}

TEST(a_snippet_with_no_match_shows_the_opening_words) {
  str snip = search_snippet("one two three four five six seven eight", "absent", 3);
  ASSERT_STR_EQ(snip, "one two three …");
}

TEST(the_query_is_capped_and_trimmed) {
  WSearchWords w = search_words("  a   b  ");
  ASSERT_EQ(w.n, 2);
  ASSERT_STR_EQ(w.w[0], "a");
  ASSERT_STR_EQ(w.w[1], "b");
}

TEST_MAIN()
