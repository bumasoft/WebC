// SPDX-License-Identifier: Apache-2.0
// webc/time.h — dates and durations, plus the two conversions every feed and activity log
// needs: "how long ago" and "how long".
#include <ctype.h>

#include <webc/time.h>
#include <webc/log.h>
#include <webc/task.h>

// ---------------------------------------------------------------- dates
//
// Days since the epoch → year/month/day without a timezone database or a `struct tm`
// (the wasm target has neither). Howard Hinnant's civil_from_days, which is exact for
// every date anyone will put in a blog post.
static void civil_from_days(i64 z, i64 *y, i64 *m, i64 *d) {
  z += 719468;
  i64 era = (z >= 0 ? z : z - 146096) / 146097;
  i64 doe = z - era * 146097;                                  // [0, 146096]
  i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
  i64 yy = yoe + era * 400;
  i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           // [0, 365]
  i64 mp = (5 * doy + 2) / 153;                                // [0, 11]
  *d = doy - (153 * mp + 2) / 5 + 1;                           // [1, 31]
  *m = mp < 10 ? mp + 3 : mp - 9;                              // [1, 12]
  *y = yy + (*m <= 2);
}

static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *kMonthsLong[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};
static const char *kDays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
static const char *kDaysLong[] = {"Thursday", "Friday", "Saturday", "Sunday",
                                  "Monday",   "Tuesday", "Wednesday"};

// Days since 1970-01-01 for a civil date, and back. Both are Howard Hinnant's algorithms:
// exact for every date anyone will put on a page, and no `struct tm` or timezone database
// in sight — the wasm target has neither.
static i64 days_from_civil(i64 y, i64 m, i64 d) {
  y -= m <= 2;
  i64 era = (y >= 0 ? y : y - 399) / 400;
  i64 yoe = y - era * 400;                                       // [0, 399]
  i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;      // [0, 365]
  i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;               // [0, 146096]
  return era * 146097 + doe - 719468;
}

// The formatter, over a broken-down UTC instant, so the epoch and ISO paths share one
// pattern language and cannot drift. `%B`/`%A` are the long names: a masthead wants
// "Sunday, 11 October 2026", and the short forms are for a byline.
static str put_date(Buf *b, i64 days, i64 hh, i64 mm, i64 ss, const char *pattern) {
  i64 y = 1970, m = 1, d = 1;
  civil_from_days(days, &y, &m, &d);
  i64 wd = ((days % 7) + 7) % 7;
  for (const char *p = pattern; p && *p; p++) {
    if (*p != '%') {
      buf_putc(b, *p);
      continue;
    }
    char c = *++p;
    switch (c) {
    case 'Y': buf_printf(b, "%lld", (long long)y); break;
    case 'm': buf_printf(b, "%02lld", (long long)m); break;
    case 'd': buf_printf(b, "%02lld", (long long)d); break;
    case 'e': buf_printf(b, "%lld", (long long)d); break;
    case 'b': buf_puts(b, kMonths[(m - 1) % 12]); break;
    case 'B': buf_puts(b, kMonthsLong[(m - 1) % 12]); break;
    case 'a': buf_puts(b, kDays[wd]); break;
    case 'A': buf_puts(b, kDaysLong[wd]); break;
    case 'H': buf_printf(b, "%02lld", (long long)hh); break;
    case 'M': buf_printf(b, "%02lld", (long long)mm); break;
    case 'S': buf_printf(b, "%02lld", (long long)ss); break;
    case '%': buf_putc(b, '%'); break;
    case 0: buf_putc(b, '%'); p--; break;
    default: buf_putc(b, '%'); buf_putc(b, c); break;
    }
  }
  return buf_str(b);
}

str wc_fmt_date(i64 ms, const char *pattern) {
  i64 secs = ms >= 0 ? ms / 1000 : -((-ms + 999) / 1000);
  i64 days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
  i64 sod = secs - days * 86400;
  Buf b;
  buf_init(&b, wc_arena_get(), 32);
  return put_date(&b, days, sod / 3600, (sod % 3600) / 60, sod % 60, pattern);
}

// "2026-10-11" and "2026-10-11T09:40:00Z" both work: an archive that stores ISO text is the
// common case, and asking the app to parse it into milliseconds first is what makes every
// app carry its own calendar.
str wc_fmt_date_iso(str iso, const char *pattern) {
  Buf b;
  buf_init(&b, wc_arena_get(), 32);
  if (!iso.p || iso.n < 10) return buf_str(&b);
  i64 y = 0, m = 0, d = 0;
  for (int i = 0; i < 4; i++) y = y * 10 + (iso.p[i] - '0');
  for (int i = 5; i < 7; i++) m = m * 10 + (iso.p[i] - '0');
  for (int i = 8; i < 10; i++) d = d * 10 + (iso.p[i] - '0');
  if (m < 1 || m > 12 || d < 1 || d > 31) return buf_str(&b);
  i64 hh = 0, mm = 0, ss = 0;
  if (iso.n >= 16 && (iso.p[10] == 'T' || iso.p[10] == ' ')) {
    hh = (iso.p[11] - '0') * 10 + (iso.p[12] - '0');
    mm = (iso.p[14] - '0') * 10 + (iso.p[15] - '0');
  }
  if (iso.n >= 19) ss = (iso.p[17] - '0') * 10 + (iso.p[18] - '0');
  return put_date(&b, days_from_civil(y, m, d), hh, mm, ss, pattern);
}


// ---------------------------------------------------------------- the clock

i64 time_ms(void) { return (i64)wc_wall_ms(); }
i64 time_secs(void) { return (i64)wc_wall_ms() / 1000; }
void time_sleep_ms(i64 ms) { wg_sleep_ms(ms > 0 ? (u64)ms : 0); }

// "2026-10-11" or "2026-10-11T09:40:00Z": the epoch *milliseconds*. Anything else is 0,
// which is the epoch — the caller that cares checks `!parse_iso_ms(...)`.
//
// The name says the unit, because the unit belongs in the name:
// `parse_iso(updated) > parse_iso(published) + 24 * 60 * 60` looks like a day and is 86
// seconds. A value whose unit lives only in the comment is a value the next reader will
// guess wrong, and `_ms`/`_secs` in the name is the cheapest place to put it.
i64 wc_parse_iso_ms(str iso) {
  if (!iso.p || iso.n < 10) return 0;
  i64 y = 0, m = 0, d = 0;
  for (int i = 0; i < 4; i++) {
    if (!isdigit((unsigned char)iso.p[i])) return 0;
    y = y * 10 + (iso.p[i] - '0');
  }
  for (int i = 5; i < 7; i++) m = m * 10 + (iso.p[i] - '0');
  for (int i = 8; i < 10; i++) d = d * 10 + (iso.p[i] - '0');
  if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
  i64 hh = 0, mm = 0, ss = 0;
  if (iso.n >= 16 && (iso.p[10] == 'T' || iso.p[10] == ' ')) {
    hh = (iso.p[11] - '0') * 10 + (iso.p[12] - '0');
    mm = (iso.p[14] - '0') * 10 + (iso.p[15] - '0');
  }
  if (iso.n >= 19) ss = (iso.p[17] - '0') * 10 + (iso.p[18] - '0');
  return days_from_civil(y, m, d) * 86400000 + hh * 3600000 + mm * 60000 + ss * 1000;
}

i64 wc_parse_iso_secs(str iso) { return wc_parse_iso_ms(iso) / 1000; }
// The alias the surface kept: same value, a name that does not say the unit.
i64 wc_parse_iso(str iso) { return wc_parse_iso_ms(iso); }

// ---------------------------------------------------------------- how long ago
//
// The ladder is the product decision, so it is one table and not a chain of `if`s in every
// view: under a minute is "just now", then minutes, then hours, then "yesterday", then the
// date. Rounds down — "1h ago" at 119 minutes is right and "2h ago" is not.
str wc_ago_at(i64 then_ms, i64 now_ms) {
  Buf b;
  buf_init(&b, wc_arena_get(), 32);
  if (then_ms <= 0) return buf_str(&b);
  i64 d = now_ms - then_ms;
  if (d < 0) d = 0; // a stamp from the future is "now", not a negative age
  i64 secs = d / 1000;
  if (secs < 45) {
    buf_puts(&b, "just now");
  } else if (secs < 3600) {
    i64 mins = secs / 60;
    buf_printf(&b, "%lld min ago", (long long)(mins < 1 ? 1 : mins));
  } else if (secs < 86400) {
    buf_printf(&b, "%lld h ago", (long long)(secs / 3600));
  } else if (secs < 172800) {
    buf_puts(&b, "yesterday");
  } else if (secs < 7 * 86400) {
    buf_printf(&b, "%lld days ago", (long long)(secs / 86400));
  } else {
    return wc_fmt_date(then_ms, "%e %b");
  }
  return buf_str(&b);
}

str wc_ago(i64 then_ms) { return wc_ago_at(then_ms, time_ms()); }

// ---------------------------------------------------------------- how long
//
// Two units, biggest first: "1h 8m", never "0h 1m 30s". A duration that reads like a
// machine printed it is a duration nobody reads.
str wc_dur_ms(i64 ms) {
  i64 secs = ms >= 0 ? ms / 1000 : -((-ms + 999) / 1000);
  return wc_dur_secs(secs);
}

str wc_dur_secs(i64 secs) {
  bool neg = secs < 0;
  if (neg) secs = -secs;
  Buf b;
  buf_init(&b, wc_arena_get(), 24);
  if (neg) buf_putc(&b, '-');
  i64 days = secs / 86400, hours = (secs % 86400) / 3600, mins = (secs % 3600) / 60,
      s = secs % 60;
  if (days > 0) {
    buf_printf(&b, "%lldd", (long long)days);
    if (hours > 0) buf_printf(&b, " %lldh", (long long)hours);
  } else if (hours > 0) {
    buf_printf(&b, "%lldh", (long long)hours);
    if (mins > 0) buf_printf(&b, " %lldm", (long long)mins);
  } else if (mins > 0) {
    buf_printf(&b, "%lldm", (long long)mins);
    if (s > 0) buf_printf(&b, " %llds", (long long)s);
  } else {
    buf_printf(&b, "%llds", (long long)s);
  }
  return buf_str(&b);
}
