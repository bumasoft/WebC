// time: the ladder, the two-unit duration, and the ISO round trip. A timestamp module is
// judged by the strings it refuses to print.
#include <webc/test.h>
#include <webc/time.h>

static i64 ts(const char *iso) { return parse_iso(str_lit(iso)); }

TEST(a_stamp_formats_the_way_the_pattern_says) {
  i64 ms = ts("2026-10-11T09:30:05Z");
  ASSERT_STR_EQ(fmt_date(ms, "%e %b %Y"), "11 Oct 2026");
  ASSERT_STR_EQ(fmt_date(ms, "%Y-%m-%d"), "2026-10-11");
  ASSERT_STR_EQ(fmt_date(ms, "%H:%M"), "09:30");
  ASSERT_STR_EQ(fmt_date(ms, "%A, %B %e"), "Sunday, October 11");
  ASSERT_STR_EQ(fmt_date_iso(str_lit("2026-10-11"), "%d/%m"), "11/10");
}

TEST(the_epoch_and_before_it_both_work) {
  ASSERT_STR_EQ(fmt_date(0, "%Y-%m-%d"), "1970-01-01");
  ASSERT_STR_EQ(fmt_date(-86400000LL, "%Y-%m-%d"), "1969-12-31");
  ASSERT_STR_EQ(fmt_date(ts("2000-02-29T00:00:00Z"), "%e %b %Y"), "29 Feb 2000");
}

TEST(iso_text_and_a_number_are_the_same_instant) {
  ASSERT_EQ(ts("2026-10-11T09:30:05Z"), 1791711005000LL);
  ASSERT_EQ(ts("2026-10-11"), 1791676800000LL);
  ASSERT_EQ(parse_iso(str_lit("not a date")), 0);
  ASSERT_EQ(parse_iso(str_lit("2026-13-01")), 0);
  // a stamp this module wrote reads back as itself
  str iso = fmt_date(ts("2026-10-11T09:30:05Z"), "%Y-%m-%dT%H:%M:%SZ");
  ASSERT_EQ(parse_iso(iso), 1791711005000LL);
}

TEST(the_age_ladder_rounds_down_and_stops_being_precise) {
  i64 now = ts("2026-10-11T12:00:00Z");
  ASSERT_STR_EQ(ago_at(now - 10 * 1000, now), "just now");
  ASSERT_STR_EQ(ago_at(now - 3 * 60000, now), "3 min ago");
  ASSERT_STR_EQ(ago_at(now - 59 * 60000, now), "59 min ago");
  ASSERT_STR_EQ(ago_at(now - 119 * 60000, now), "1 h ago");
  ASSERT_STR_EQ(ago_at(now - 30 * 3600000, now), "yesterday");
  ASSERT_STR_EQ(ago_at(now - 4 * 86400000LL, now), "4 days ago");
  ASSERT_STR_EQ(ago_at(now - 40 * 86400000LL, now), "1 Sep");
}

TEST(a_stamp_from_the_future_is_now_not_a_negative_age) {
  i64 now = ts("2026-10-11T12:00:00Z");
  ASSERT_STR_EQ(ago_at(now + 5000, now), "just now");
  // and no stamp at all says nothing, rather than "56 years ago"
  ASSERT_STR_EQ(ago_at(0, now), "");
}

TEST(a_duration_is_two_units_never_three) {
  ASSERT_STR_EQ(dur_secs(0), "0s");
  ASSERT_STR_EQ(dur_secs(45), "45s");
  ASSERT_STR_EQ(dur_secs(90), "1m 30s");
  ASSERT_STR_EQ(dur_secs(4120), "1h 8m");
  ASSERT_STR_EQ(dur_secs(3 * 86400 + 4 * 3600), "3d 4h");
  ASSERT_STR_EQ(dur_ms(4120000), "1h 8m");
  ASSERT_STR_EQ(dur_ms(-90000), "-1m 30s");
}

TEST(the_clock_is_the_raw_one_inside_a_plausible_window) {
  i64 t = time_ms();
  ASSERT_GT(t, ts("2024-01-01"));
  ASSERT_EQ(time_secs(), t / 1000);
}

TEST_MAIN()
