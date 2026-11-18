// url: route patterns, links and the three text conversions every page needs, on both
// halves. Everything here was a helper an app wrote for itself (The Meridian had five),
// and the failure mode of a hand-written copy is a link that drifts from its route or a
// date that formats differently in the view and in the feed.
#include <webc/test.h>

// A pattern declared where both halves can see it.
ROUTE("story", "/a/:slug")
ROUTE("section", "/s/:slug")

TEST(route_patterns_are_shared) {
  ASSERT_STR_EQ(str_lit(wc_url_pattern("story")), "/a/:slug");
  ASSERT_STR_EQ(str_lit(wc_url_pattern("section")), "/s/:slug");
  ASSERT_NULL(wc_url_pattern("nope"));
  ASSERT_GE(wc_url_count(), 2);
}

TEST(saying_the_same_route_twice_is_fine) {
  wc_url_register("story", "/a/:slug"); // the server's GET() and a shared ROUTE() agree
  ASSERT_GE(wc_url_count(), 2);
}

TEST(url_to_fills_the_pattern) {
  ASSERT_STR_EQ(url_to("story", "slug", str_lit("the-last-ferry")), "/a/the-last-ferry");
  ASSERT_STR_EQ(url_to("section", "slug", str_lit("science")), "/s/science");
}

TEST(url_values_are_encoded) {
  // a slug is data: it does not get to add a path segment or a query parameter
  ASSERT_STR_EQ(url_to("story", "slug", str_lit("a b")), "/a/a%20b");
  ASSERT_STR_EQ(url_to("story", "slug", str_lit("x?y=1")), "/a/x%3Fy%3D1");
  ASSERT_STR_EQ(url_to("story", "slug", str_lit("q/../etc")), "/a/q%2F..%2Fetc");
}

WEBC_MODEL(Piece, (id, i64, .pk = true), (slug, Text, .required = true))
WEBC_MODEL_ROUTE(Piece, "/p/:slug")

TEST(a_model_route_is_named_after_the_model) {
  // `link_to(Piece, p)` and `url_to("Piece", "slug", p->slug)` are the same fact; a model
  // route that could be linked but not named is what made a shared view spell its paths.
  ASSERT_STR_EQ(str_lit(wc_url_pattern("Piece")), "/p/:slug");
  ASSERT_STR_EQ(url_to("Piece", "slug", str_lit("a b")), "/p/a%20b");
}

TEST(url_build_from_a_literal_pattern) {
  ASSERT_STR_EQ(url_build("/by/:who", "who", str_lit("ada")), "/by/ada");
  ASSERT_STR_EQ(url_build("/archive"), "/archive");
}

TEST(a_missing_parameter_says_which_one) {
  ASSERT_PANICS({ url_to("story", "wrong", str_lit("x")); });
}

TEST(a_name_nobody_declared_says_so) {
  ASSERT_PANICS({ url_to("nosuchroute"); });
}

// ---------------------------------------------------------------- dates

TEST(fmt_date_reads_like_a_newspaper) {
  // 2026-10-11T09:30:00Z
  i64 ms = 1791711000LL * 1000; // 2026-10-11T09:30:00Z, from `date -u`
  ASSERT_STR_EQ(fmt_date(ms, "%e %b %Y"), "11 Oct 2026");
  ASSERT_STR_EQ(fmt_date(ms, "%Y-%m-%d"), "2026-10-11");
  ASSERT_STR_EQ(fmt_date(ms, "%H:%M"), "09:30");
}

TEST(fmt_date_handles_the_epoch_and_before_it) {
  ASSERT_STR_EQ(fmt_date(0, "%Y-%m-%d"), "1970-01-01");
  ASSERT_STR_EQ(fmt_date(-86400LL * 1000, "%Y-%m-%d"), "1969-12-31");
  ASSERT_STR_EQ(fmt_date(951782400000LL, "%e %b %Y"), "29 Feb 2000");
}

TEST(fmt_date_leap_day_and_year_end) {
  i64 new_year = 1767225600000LL; // 2026-01-01T00:00:00Z
  ASSERT_STR_EQ(fmt_date(new_year, "%Y-%m-%d"), "2026-01-01");
  ASSERT_STR_EQ(fmt_date(new_year - 86400000LL, "%Y-%m-%d"), "2025-12-31");
}

// ---------------------------------------------------------------- ISO stamps
//
// The form an archive actually stores: text, not milliseconds. Both spellings of it — a
// bare date and a full instant — and the pattern language reaches the long names, because
// a masthead wants "Sunday, 11 October 2026".

TEST(fmt_date_iso_reads_a_bare_date_and_a_full_instant) {
  ASSERT_STR_EQ(fmt_date_iso(str_lit("2026-10-11"), "%e %b %Y"), "11 Oct 2026");
  ASSERT_STR_EQ(fmt_date_iso(str_lit("2026-10-11T09:40:00Z"), "%e %b %Y"), "11 Oct 2026");
  ASSERT_STR_EQ(fmt_date_iso(str_lit("2026-10-11T09:40:00Z"), "%H:%M"), "09:40");
  ASSERT_STR_EQ(fmt_date_iso(str_lit("2026-10-11T09:40:00Z"), "%A, %e %B %Y"),
                "Sunday, 11 October 2026");
  ASSERT_STR_EQ(fmt_date_iso(str_lit("2026-01-01"), "%a %e %b"), "Thu 1 Jan");
}

TEST(fmt_date_iso_says_nothing_rather_than_guessing) {
  // an empty stamp, half of one, a month that does not exist: "" and no panic, because a
  // missing date on a card is a gap and a crash is an outage
  ASSERT_STR_EQ(fmt_date_iso(str_lit(""), "%e %b %Y"), "");
  ASSERT_STR_EQ(fmt_date_iso(str_lit("2026"), "%e %b %Y"), "");
  ASSERT_STR_EQ(fmt_date_iso(str_lit("2026-13-01"), "%e %b %Y"), "");
}

// ---------------------------------------------------------------- figures and names

TEST(read_minutes_rounds_up_and_never_says_zero) {
  ASSERT_EQ(read_minutes(str_lit("")), 1);
  ASSERT_EQ(read_minutes(str_lit("one")), 1);
  ASSERT_EQ(read_minutes(str_lit("  \n\t ")), 1); // whitespace is not a word
  // 220 words is a minute; the 221st word makes it two
  Arena *a = arena_new(4096);
  Buf b;
  buf_init(&b, a, 4096);
  for (i32 i = 0; i < 220; i++) buf_puts(&b, "word ");
  ASSERT_EQ(read_minutes(buf_str(&b)), 1);
  buf_puts(&b, "word");
  ASSERT_EQ(read_minutes(buf_str(&b)), 2);
  arena_destroy(a);
}

TEST(read_time_is_the_phrase_a_page_shows) {
  ASSERT_STR_EQ(read_time(str_lit("a few words")), "1 min read");
}

TEST(name_from_slug_gives_a_person_back_their_name) {
  ASSERT_STR_EQ(name_from_slug(str_lit("ada-lovelace")), "Ada Lovelace");
  ASSERT_STR_EQ(name_from_slug(str_lit("science-nature")), "Science Nature");
  ASSERT_STR_EQ(name_from_slug(str_lit("mara_vance")), "Mara Vance");
  ASSERT_STR_EQ(name_from_slug(str_lit("")), "");
}

TEST(contains_ci_ignores_case) {
  ASSERT_TRUE(contains_ci(str_lit("The Last Ferry"), str_lit("ferry")));
  ASSERT_TRUE(contains_ci(str_lit("the last ferry"), str_lit("LAST")));
  ASSERT_FALSE(contains_ci(str_lit("the last ferry"), str_lit("tide")));
  ASSERT_TRUE(contains_ci(str_lit("anything"), str_lit("")));
  ASSERT_FALSE(contains_ci(str_lit(""), str_lit("x")));
}

// ---------------------------------------------------------------- headings and slugs

TEST(title_case_capitalises_words_not_every_letter) {
  ASSERT_STR_EQ(title_case(str_lit("science & nature")), "Science & Nature");
  ASSERT_STR_EQ(title_case(str_lit("the city")), "The City");
  ASSERT_STR_EQ(title_case(str_lit("OPINION")), "Opinion");
}

TEST(slugify_makes_a_url_out_of_a_headline) {
  ASSERT_STR_EQ(slugify(str_lit("What the tide left behind")), "what-the-tide-left-behind");
  ASSERT_STR_EQ(slugify(str_lit("  Ship's log: 11th  night ")), "ship-s-log-11th-night");
  ASSERT_STR_EQ(slugify(str_lit("Café")), "caf");
}

TEST_MAIN()