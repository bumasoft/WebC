// SPDX-License-Identifier: Apache-2.0
// WebC — chart geometry: scales, ticks, and the points an SVG wants.
//
//   #include <webc/chart.h>
//
//   WScale x = chart_lin(0, FLIPS, pad_x, pad_x + plot_w);      // flips -> pixels
//   WScale y = chart_lin(0, peak, pad_y + plot_h, pad_y);       // value -> pixels, y up
//   Buf *pts = sb();
//   chart_span_points(pts, x, y, eq, FLIPS + 1, 476);           // 2px buckets, min+max
//   el("polyline") { class_("curve"); attr("points", sb_cstr(pts)); }
//
// This is the arithmetic an app writes once per chart and then debugs forever: a
// value-to-pixel map that inverts, ticks at 1/2/2.5/5 × 10ⁿ instead of 1,387.4, and a
// downsampler that keeps a spike instead of averaging it away. All of it is geometry —
// pixels in, pixels out. Nothing here decides a colour, a font, a layout or a label
// format; those are the app's, and `fmt_num`/`fmt_pct`/`fmt_money` (core.h) are there
// for the labels the app does want.
//
// Header-only, no allocation, no state: every function takes what it needs and returns a
// number or writes into a builder the caller owns. Same source on both targets, so a
// chart drawn on the server and one drawn in the browser agree to the last decimal.
#ifndef WEBC_CHART_H
#define WEBC_CHART_H

#include <math.h>

#include <webc/core.h>
#include <webc/dom.h>

// ---------------------------------------------------------------- the scale
//
// `p0`/`p1` are pixels and may run backwards: a chart's y axis goes up while the screen's
// goes down, and saying so here beats a `-` in front of every value at the call site.
typedef struct WScale {
  f64 v0, v1; // the domain
  f64 p0, p1; // pixels for v0 and v1
  bool log;   // the domain is decades, not units
} WScale;

static inline WScale chart_scale_(f64 lo, f64 hi, f64 p0, f64 p1, bool log) {
  WScale s = {lo, hi, p0, p1, log};
  if (log) {
    if (s.v0 < 1e-9) s.v0 = 1e-9;
    if (s.v1 < s.v0 * 10) s.v1 = s.v0 * 10; // a decade is the smallest readable log axis
  } else if (s.v0 == s.v1) {
    s.v1 = s.v0 + 1; // a flat series still has to land somewhere
  }
  return s;
}

static inline WScale chart_lin(f64 lo, f64 hi, f64 p0, f64 p1) {
  return chart_scale_(lo, hi, p0, p1, false);
}
static inline WScale chart_log(f64 lo, f64 hi, f64 p0, f64 p1) {
  return chart_scale_(lo, hi, p0, p1, true);
}

// Where a value lands, in 0..1 along the axis (0 at v0). A value outside the domain is
// clamped: a clipped point is drawn where it can be seen, not a thousand pixels away.
static inline f64 chart_frac(WScale s, f64 v) {
  f64 f;
  if (s.log) {
    f64 lo = log10(s.v0), hi = log10(s.v1);
    f64 vv = v < 1e-9 ? 1e-9 : v;
    f64 lv = log10(vv);
    f = (lv - lo) / (hi - lo);
  } else {
    f = (v - s.v0) / (s.v1 - s.v0);
  }
  return f < 0 ? 0 : (f > 1 ? 1 : f);
}

static inline f64 chart_at(WScale s, f64 v) {
  f64 f = chart_frac(s, v);
  return s.p0 + (s.p1 - s.p0) * f;
}

// The inverse, for a pointer: a pixel back to a value. What a crosshair reads.
static inline f64 chart_val(WScale s, f64 px) {
  f64 f = (s.p1 == s.p0) ? 0 : (px - s.p0) / (s.p1 - s.p0);
  if (s.log) return pow(10, log10(s.v0) + f * (log10(s.v1) - log10(s.v0)));
  return s.v0 + f * (s.v1 - s.v0);
}

// ---------------------------------------------------------------- the ticks
//
// The step a person reads without thinking: 1, 2, 2.5 or 5 times a power of ten, whichever
// gives about `want` lines. An axis that says 1 … 1387.4 is what a raw span gives.
static inline f64 chart_nice_step(f64 span, i32 want) {
  if (want < 1) want = 1;
  if (span <= 0) return 1;
  f64 raw = span / (f64)want;
  f64 mag = pow(10, floor(log10(raw)));
  f64 norm = raw / mag;
  f64 step = norm <= 1.0 ? 1.0 : norm <= 2.0 ? 2.0 : norm <= 2.5 ? 2.5 : norm <= 5.0 ? 5.0 : 10.0;
  return step * mag;
}

// Writes the tick values between lo and hi (inclusive of a tick that lands on either end)
// and returns how many. `out` must have room for `cap`; a tick list longer than that is
// truncated rather than overrunning, because an axis with too many labels is a bug the
// caller can see.
static inline i32 chart_ticks(f64 lo, f64 hi, i32 want, f64 *out, i32 cap) {
  if (!out || cap <= 0) return 0;
  if (hi < lo) {
    f64 t = lo;
    lo = hi;
    hi = t;
  }
  f64 step = chart_nice_step(hi - lo, want);
  i32 n = 0;
  // start at the first multiple of `step` at or above lo; epsilon keeps 0.1+0.2 as 0.3
  i64 k = (i64)ceil(lo / step - 1e-9);
  for (f64 v = (f64)k * step; v <= hi + step * 1e-9 && n < cap; v += step) {
    out[n++] = fabs(v) < step * 1e-9 ? 0.0 : v; // no "-0"
  }
  return n;
}

// Decades, for a log axis: $10, $100, $1,000 … Abscissae are values, not steps, so this is
// a separate call — the ticks are the powers of ten inside the domain.
static inline i32 chart_decades(f64 lo, f64 hi, f64 *out, i32 cap) {
  if (!out || cap <= 0 || hi <= 0) return 0;
  if (lo < 1e-9) lo = 1e-9;
  i32 n = 0;
  f64 mag = pow(10, floor(log10(lo)));
  for (; mag <= hi * 1.0000001 && n < cap; mag *= 10)
    if (mag >= lo * 0.9999999) out[n++] = mag;
  return n;
}

// Full decades for a log domain, so the axis ends on a round power of ten.
static inline f64 chart_log_top(f64 peak) {
  if (peak < 10) peak = 10;
  return pow(10, ceil(log10(peak)));
}

// ---------------------------------------------------------------- the plot area
//
// The plot rectangle inside a viewport with room for labels. `x`/`y` is the top-left
// corner, `w`/`h` the drawable size. (Not `WFrame`: that name is a stream's two arenas.)
typedef struct WPlot {
  f64 x, y, w, h;
} WPlot;

static inline WPlot chart_plot(f64 w, f64 h, f64 pad_l, f64 pad_r, f64 pad_t, f64 pad_b) {
  WPlot f = {pad_l, pad_t, w - pad_l - pad_r, h - pad_t - pad_b};
  if (f.w < 0) f.w = 0;
  if (f.h < 0) f.h = 0;
  return f;
}

// An x axis over indices 0..n-1, a y axis over values inside the plot. The two calls a
// chart actually makes, with the y axis flipped for you (y grows upwards).
static inline WScale chart_x(WPlot f, f64 n) { return chart_lin(0, n, f.x, f.x + f.w); }
static inline WScale chart_y(WPlot f, f64 lo, f64 hi) { return chart_lin(lo, hi, f.y + f.h, f.y); }

// ---------------------------------------------------------------- the points
//
// SVG takes `points="x,y x,y …"` and nothing else: no per-point styling, no gaps, no
// metadata. So the only interesting decision is how many points to send — a 10 001 point
// series is 90 kB of markup for a chart 600 px wide.

// Every n-th point, in order. For a series short enough to draw honestly.
static inline void chart_points(Buf *b, WScale x, WScale y, const f64 *ys, i32 n, i32 step) {
  if (!b || !ys || n <= 0) return;
  if (step < 1) step = 1;
  bool first = true;
  for (i32 i = 0; i < n; i += step) {
    if (!first) sb_putc(b, ' ');
    first = false;
    sb_num(b, chart_at(x, (f64)i), 1);
    sb_putc(b, ',');
    sb_num(b, chart_at(y, ys[i]), 1);
  }
}

// Min and max per horizontal bucket, drawn in index order: a drawdown spike survives the
// downsample instead of being averaged into a smooth lie, and a flat stretch costs two
// points like any other. This is what makes "10k points, 600 px" honest and cheap.
//
// Both extremes of one bucket are emitted, ordered by index, so the polyline stays a
// function of x and never doubles back on itself.
static inline void chart_span_points(Buf *b, WScale x, WScale y, const f64 *ys, i32 n,
                                     i32 buckets, i32 decimals) {
  if (!b || !ys || n <= 0) return;
  if (buckets < 1) buckets = 1;
  if (buckets > n) buckets = n;
  if (decimals < 0) decimals = 1;
  bool first = true;
  for (i32 k = 0; k < buckets; k++) {
    i64 i0 = (i64)k * n / buckets;
    i64 i1 = (i64)(k + 1) * n / buckets - 1; // the bucket's last index, not the next one's
    if (i1 >= n) i1 = n - 1;
    if (i1 < i0) i1 = i0;
    i64 imin = i0, imax = i0;
    f64 vmin = ys[i0], vmax = ys[i0];
    for (i64 i = i0 + 1; i <= i1; i++) {
      if (ys[i] < vmin) {
        vmin = ys[i];
        imin = i;
      }
      if (ys[i] > vmax) {
        vmax = ys[i];
        imax = i;
      }
    }
    i64 order[2] = {imin, imax};
    f64 vals[2] = {vmin, vmax};
    if (imin > imax) {
      order[0] = imax;
      order[1] = imin;
      vals[0] = vmax;
      vals[1] = vmin;
    }
    i32 count = (imin == imax) ? 1 : 2;
    for (i32 j = 0; j < count; j++) {
      if (!first) sb_putc(b, ' ');
      first = false;
      sb_num(b, chart_at(x, (f64)order[j]), 1);
      sb_putc(b, ',');
      sb_num(b, chart_at(y, vals[j]), decimals);
    }
  }
}

// A min/max band (a confidence cone, a drawdown envelope) as one closed polygon: the high
// edge left to right, the low edge back again. `lo`/`hi` are arrays of the same length.
static inline void chart_band_points(Buf *b, WScale x, WScale y, const f64 *lo, const f64 *hi,
                                     i32 n, i32 buckets, i32 decimals) {
  if (!b || !lo || !hi || n <= 0) return;
  if (buckets < 1) buckets = 1;
  if (buckets > n) buckets = n;
  if (decimals < 0) decimals = 1;
  // The top edge, left to right. Each bucket contributes one point, at its middle index,
  // carrying the bucket's highest value.
  for (i32 k = 0; k < buckets; k++) {
    i64 i0 = (i64)k * n / buckets;
    i64 i1 = (i64)(k + 1) * n / buckets - 1; // the bucket's last index, not the next one's
    if (i1 >= n) i1 = n - 1;
    if (i1 < i0) i1 = i0;
    f64 top = hi[i0];
    for (i64 i = i0 + 1; i <= i1; i++)
      if (hi[i] > top) top = hi[i];
    if (k) sb_putc(b, ' ');
    sb_num(b, chart_at(x, (f64)((i0 + i1) / 2)), 1);
    sb_putc(b, ',');
    sb_num(b, chart_at(y, top), decimals);
  }
  // …and the bottom edge back again, which is what closes the polygon.
  for (i32 k = buckets - 1; k >= 0; k--) {
    i64 i0 = (i64)k * n / buckets;
    i64 i1 = (i64)(k + 1) * n / buckets - 1; // the bucket's last index, not the next one's
    if (i1 >= n) i1 = n - 1;
    if (i1 < i0) i1 = i0;
    f64 bot = lo[i0];
    for (i64 i = i0 + 1; i <= i1; i++)
      if (lo[i] < bot) bot = lo[i];
    sb_putc(b, ' ');
    sb_num(b, chart_at(x, (f64)((i0 + i1) / 2)), 1);
    sb_putc(b, ',');
    sb_num(b, chart_at(y, bot), decimals);
  }
}

// The global min and max of a series: what a caller needs to build the y scale, and the
// two numbers an app otherwise walks the array twice to find.
static inline void chart_range(const f64 *ys, i32 n, f64 *lo, f64 *hi) {
  f64 a = 0, b = 0;
  if (ys && n > 0) {
    a = b = ys[0];
    for (i32 i = 1; i < n; i++) {
      if (ys[i] < a) a = ys[i];
      if (ys[i] > b) b = ys[i];
    }
  }
  if (lo) *lo = a;
  if (hi) *hi = b;
}

#endif // WEBC_CHART_H