// csv: the two bugs (a quoted comma, and the \r of a \r\n) and the one an import always has
// (a row that is not as wide as the header).
#include <webc/test.h>
#include <webc/csv.h>

WEBC_MODEL(Line,
           (id, i64, .pk = true),
           (title, str, .required = true),
           (n, i32),
           (ratio, f64),
           (done, bool),
           (secret, str, .no_json = true))

TEST(a_cell_is_quoted_only_when_it_has_to_be) {
  Buf b;
  buf_init(&b, wc_arena_get(), 64);
  csv_cell(&b, str_lit("plain"));
  buf_putc(&b, '|');
  csv_cell(&b, str_lit("a,b"));
  buf_putc(&b, '|');
  csv_cell(&b, str_lit("say \"hi\""));
  buf_putc(&b, '|');
  csv_cell(&b, str_lit("two\nlines"));
  buf_putc(&b, '|');
  csv_cell(&b, str_lit(" padded "));
  str out = buf_str(&b);
  ASSERT_STR_EQ(out, "plain|\"a,b\"|\"say \"\"hi\"\"\"|\"two\nlines\"|\" padded \"");
}

TEST(the_header_is_the_docs_and_the_id_goes_last) {
  str h = csv_header_text(Line_modelof());
  // id last, the secret out, the doc when a field has one
  ASSERT_STR_EQ(h, "title,n,ratio,done,id");
  Line one = {.id = 7, .title = str_lit("a,b"), .n = 3, .ratio = 0.5, .done = true,
              .secret = str_lit("nope")};
  str row = csv_row_text(Line_modelof(), &one);
  ASSERT_STR_EQ(row, "\"a,b\",3,0.5,true,7");
}

TEST(an_enum_is_its_name_not_a_number) {
  Line two = {.title = str_lit("x")};
  str row = csv_row_text(Line_modelof(), &two);
  ASSERT_STR_EQ(row, "x,0,0,false,0");
}

TEST(the_parser_keeps_a_quoted_comma_and_drops_the_cr) {
  CSVParse p = csv_parse(str_lit("a,b\r\n\"x,y\",2\r\n"));
  CSVRow row;
  ASSERT_TRUE(csv_next(&p, &row));
  ASSERT_EQ(row.n, 2);
  ASSERT_STR_EQ(row.cells[0], "x,y");
  ASSERT_STR_EQ(row.cells[1], "2");
  ASSERT_STR_EQ(row.why, "");
  ASSERT_EQ(row.line, 2);
  ASSERT_FALSE(csv_next(&p, &row));
}

TEST(a_quoted_field_may_hold_a_newline_and_a_quote) {
  CSVParse p = csv_parse(str_lit("h\n\"one\ntwo \"\"q\"\"\"\n"));
  CSVRow row;
  ASSERT_TRUE(csv_next(&p, &row));
  ASSERT_STR_EQ(row.cells[0], "one\ntwo \"q\"");
}

TEST(a_row_that_is_not_the_header_s_Width_says_so) {
  CSVParse p = csv_parse(str_lit("a,b,c\n1,2\n"));
  CSVRow row;
  ASSERT_TRUE(csv_next(&p, &row));
  ASSERT_STR_EQ(row.why, "the row does not have the header's columns");
}

TEST(a_blank_line_in_the_middle_keeps_the_line_numbers_honest) {
  CSVParse p = csv_parse(str_lit("a\n1\n\n3\n"));
  CSVRow row;
  ASSERT_TRUE(csv_next(&p, &row));
  ASSERT_EQ(row.line, 2);
  ASSERT_TRUE(csv_next(&p, &row));
  ASSERT_EQ(row.line, 4);
  ASSERT_STR_EQ(row.cells[0], "3");
}

TEST(a_row_lands_in_the_model_by_header_name) {
  CSVParse p = csv_parse(str_lit("n,title,id\n5,hello,42\n"));
  CSVRow row;
  Line got = {0};
  ASSERT_TRUE(csv_next(&p, &row));
  ASSERT_TRUE(csv_into(Line_modelof(), &p, &row, &got));
  ASSERT_STR_EQ(got.title, "hello");
  ASSERT_EQ(got.n, 5);
  ASSERT_EQ(got.id, 42);
}

TEST(a_column_the_model_does_not_have_is_ignored_a_missing_one_leaves_the_default) {
  CSVParse p = csv_parse(str_lit("title,unheard_of\nhi,x\n"));
  CSVRow row;
  Line got = {.n = 9};
  ASSERT_TRUE(csv_next(&p, &row));
  ASSERT_TRUE(csv_into(Line_modelof(), &p, &row, &got));
  ASSERT_STR_EQ(got.title, "hi");
  ASSERT_EQ(got.n, 9); // untouched: no shifting
}

TEST(a_value_that_does_not_fit_its_field_fails_the_row) {
  CSVParse p = csv_parse(str_lit("n\nnot-a-number\n"));
  CSVRow row;
  Line got = {0};
  ASSERT_TRUE(csv_next(&p, &row));
  ASSERT_FALSE(csv_into(Line_modelof(), &p, &row, &got));
  ASSERT_TRUE(row.why && *row.why);
}

TEST(an_empty_file_is_a_reason_not_a_crash) {
  CSVParse p = csv_parse(str_lit(""));
  ASSERT_TRUE(p.bad);
  ASSERT_TRUE(p.why && *p.why);
  CSVRow row;
  ASSERT_FALSE(csv_next(&p, &row));
}

TEST_MAIN()
