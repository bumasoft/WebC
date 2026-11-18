// dx7: chart geometry (include/webc/chart.h).
//
// The app this came from carried 341 lines of chart arithmetic: two scale closures, a
// "nice number" picker, a bucketed downsample, a band polygon and a `%.2f` attribute
// helper. Every one of them was the same code in the next app. These tests hold the
// header to what a chart actually needs — and to being a *function*, so the server and
// the browser draw the same picture.
#include <webc/chart.h>
#include <webc/test.h>

TEST(a_linear_scale_maps_both_ways) {
  WScale x = chart_lin(0, 100, 0, 200);
  ASSERT_FEQ(chart_at(x, 0), 0);
  ASSERT_FEQ(chart_at(x, 50), 100);
  ASSERT_FEQ(chart_at(x, 100), 200);
  ASSERT_FEQ(chart_val(x, 100), 50);
  ASSERT_FEQ(chart_frac(x, 25), 0.25);
  // outside the domain clamps: a point is drawn where it can be seen
  ASSERT_FEQ(chart_frac(x, -10), 0);
  ASSERT_FEQ(chart_frac(x, 250), 1);
}

TEST(the_y_axis_can_run_upwards) {
  // pixels for the *low* value are at the bottom: p0 > p1, and nothing else changes
  WScale y = chart_lin(0, 10, 300, 100);
  ASSERT_FEQ(chart_at(y, 0), 300);
  ASSERT_FEQ(chart_at(y, 10), 100);
  ASSERT_FEQ(chart_at(y, 5), 200);
  ASSERT_FEQ(chart_val(y, 200), 5);
}

TEST(a_log_scale_is_decades) {
  WScale y = chart_log(10, 1000, 0, 2);     // two decades over two pixels
  ASSERT_FEQ(chart_at(y, 10), 0);
  ASSERT_FEQ(chart_at(y, 100), 1);
  ASSERT_FEQ(chart_at(y, 1000), 2);
  ASSERT_FEQ(chart_val(y, 1), 100);
  // a domain that is not a decade is widened rather than dividing by zero
  WScale z = chart_log(100, 100, 0, 1);
  ASSERT_TRUE(z.v1 > z.v0);
  ASSERT_FEQ(chart_log_top(1234), 10000);
  ASSERT_FEQ(chart_log_top(3), 10);
}

TEST(a_flat_series_still_lands_somewhere) {
  WScale y = chart_lin(50, 50, 0, 100);
  ASSERT_TRUE(y.v1 > y.v0);
  ASSERT_FEQ(chart_at(y, 50), 0); // v0 is the bottom of the widened domain
}

TEST(ticks_are_numbers_a_person_reads) {
  f64 t[16];
  i32 n = chart_ticks(0, 100, 5, t, 16);
  ASSERT_TRUE(n >= 4 && n <= 6);
  ASSERT_FEQ(t[0], 0);
  ASSERT_TRUE(t[n - 1] <= 100);
  for (i32 i = 1; i < n; i++) ASSERT_TRUE(t[i] > t[i - 1]);
  // 1 / 2 / 2.5 / 5, never 1.3874
  ASSERT_FEQ(chart_nice_step(100, 5), 20);
  ASSERT_FEQ(chart_nice_step(10, 5), 2);
  ASSERT_FEQ(chart_nice_step(25, 5), 5);
  ASSERT_FEQ(chart_nice_step(1, 4), 0.25);
  // no "-0" in the list: it prints as "-0" in an axis label
  n = chart_ticks(-1, 1, 4, t, 16);
  for (i32 i = 0; i < n; i++)
    if (t[i] == 0) ASSERT_TRUE(!signbit(t[i]));
}

TEST(ticks_stop_at_the_end_of_the_buffer) {
  f64 t[3];
  i32 n = chart_ticks(0, 1000, 20, t, 3);
  ASSERT_EQ(n, 3);
}

TEST(decades_are_the_powers_of_ten_in_the_domain) {
  f64 t[16];
  i32 n = chart_decades(50, 20000, t, 16);
  ASSERT_EQ(n, 3);
  ASSERT_FEQ(t[0], 100);
  ASSERT_FEQ(t[1], 1000);
  ASSERT_FEQ(t[2], 10000);
}

TEST(the_plot_area_insets_the_frame) {
  WPlot f = chart_plot(600, 200, 40, 16, 12, 28);
  ASSERT_FEQ(f.x, 40);
  ASSERT_FEQ(f.y, 12);
  ASSERT_FEQ(f.w, 544);
  ASSERT_FEQ(f.h, 160);
  WScale x = chart_x(f, 10);
  WScale y = chart_y(f, 0, 100);
  ASSERT_FEQ(chart_at(x, 0), 40);
  ASSERT_FEQ(chart_at(x, 10), 584);
  ASSERT_FEQ(chart_at(y, 0), 172); // the bottom edge, y up
  ASSERT_FEQ(chart_at(y, 100), 12);
}

TEST(points_are_two_decimals_and_a_space) {
  f64 ys[3] = {0, 50, 100};
  WScale x = chart_lin(0, 2, 0, 2);
  WScale y = chart_lin(0, 100, 100, 0);
  Buf *b = sb();
  chart_points(b, x, y, ys, 3, 1);
  ASSERT_STR_EQ(sb_str(b), "0.0,100.0 1.0,50.0 2.0,0.0");
}

TEST(the_downsample_keeps_the_spike) {
  // 1000 points that are flat at 10 with one spike to 900 and one dip to -50: the bucket
  // that contains each must still show it, which is the whole point of min+max.
  f64 ys[1000];
  for (i32 i = 0; i < 1000; i++) ys[i] = 10;
  ys[437] = 900;
  ys[811] = -50;
  WScale x = chart_lin(0, 999, 0, 499);
  WScale y = chart_lin(-100, 1000, 100, 0);
  Buf *b = sb();
  chart_span_points(b, x, y, ys, 1000, 100, 1);
  str pts = sb_str(b);
  // 100 buckets of at most two points each: ~90 kB of markup becomes a few KB
  i32 n = 0;
  for (size_t i = 0; i < pts.n; i++)
    if (pts.p[i] == ',') n++;
  ASSERT_TRUE(n <= 200);
  // the spike's y pixel: 900 in a 100..1000 domain over 100 pixels is 10.0
  ASSERT_TRUE(str_contains(pts, str_lit("10.0")));
  ASSERT_TRUE(!str_contains(pts, str_lit("nan")));
}

TEST(one_point_is_not_a_crash) {
  f64 ys[1] = {42};
  WScale x = chart_lin(0, 1, 0, 10);
  WScale y = chart_lin(0, 100, 10, 0);
  Buf *b = sb();
  chart_span_points(b, x, y, ys, 1, 476, 1);
  ASSERT_STR_EQ(sb_str(b), "0.0,5.8");
  // a series shorter than the bucket count must not divide by zero
  b = sb();
  chart_span_points(b, x, y, ys, 1, 476, 1);
  ASSERT_TRUE(sb_str(b).n > 0);
}

TEST(the_band_is_a_closed_polygon) {
  f64 lo[4] = {1, 2, 3, 4};
  f64 hi[4] = {5, 6, 7, 8};
  WScale x = chart_lin(0, 3, 0, 3);
  WScale y = chart_lin(0, 10, 100, 0);
  Buf *b = sb();
  chart_band_points(b, x, y, lo, hi, 4, 4, 0);
  // high edge left to right, then the low edge back
  ASSERT_STR_EQ(sb_str(b), "0.0,50 1.0,40 2.0,30 3.0,20 3.0,60 2.0,70 1.0,80 0.0,90");
}

TEST(the_range_is_one_pass) {
  f64 ys[5] = {3, -2, 9, 9, 0};
  f64 lo = 0, hi = 0;
  chart_range(ys, 5, &lo, &hi);
  ASSERT_FEQ(lo, -2);
  ASSERT_FEQ(hi, 9);
  chart_range(nullptr, 0, &lo, &hi);
  ASSERT_FEQ(lo, 0);
  ASSERT_FEQ(hi, 0);
}

TEST_MAIN()