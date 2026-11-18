// SPDX-License-Identifier: Apache-2.0
// webc/time.h — time as text, without a calendar library.
//
// Three conversions, and the honest limit written where you meet it: **UTC and fixed
// offsets only.** No timezone database, no DST arithmetic, no locale. A server-rendered page
// that says "3 min ago" almost never needs more, and a framework that ships tzdata is a
// framework that has to update tzdata.
//
//   text(fmt_date(st->published, "%e %b %Y"))   // 11 Oct 2026
//   text(fmt_date_iso(st->at, "%H:%M"))         // 09:40, from "2026-10-11T09:40:00Z"
//   text(ago(n->created_at))                    // "just now" · "3 min ago" · "yesterday" · "11 Oct"
//   text(ago_at(n->created_at, now_ms()))       // the same, against a clock you chose
//   text(dur_ms(4120000))                       // "1h 8m"
//   text(dur_secs(uptime))                      // "3d 4h"
//   text(fmt_date(at, "%H:%M"))                 // "09:40" — the clock face, no new verb
//
// Two rules the hand-written versions always break, and these do not:
//
// - **`ago()` rounds down and stops being precise as it gets old.** "59 min ago", then
//   "2 h ago", then "yesterday", then a date. A feed that says "2871 min ago" is a feed
//   that never decided.
// - **a duration is not a timestamp.** `dur_ms(90000)` is "1m 30s" — two units, because
//   "0h 1m 30s" is what a machine prints.
//
// There is no `clock()`: a macro by that name shadowed the C library's own, and the thing it
// would have saved is nine characters — `fmt_date(ms, "%H:%M")` says it and cannot collide.
//
// `%Y %m %d %e %b %B %a %A %H %M %S %%` are the pattern letters; anything else is copied
// through as it stands, so a pattern with a literal `at` works.
#pragma once

#include <webc/core.h>

// UTC milliseconds, from the wall clock. Inside a request `now()` is the framework's; this
// is the raw one, for a test that wants a fixed clock to pass to `ago_at()`.
i64  time_ms(void);
i64  time_secs(void);
void time_sleep_ms(i64 ms); // parks the task; the same wait `wg_sleep_ms` does

// ---------------------------------------------------------------- a stamp, formatted

str wc_fmt_date(i64 ms, const char *pattern);
// The same, from an ISO stamp: "2026-10-11" or "2026-10-11T09:40:00Z". An archive that
// stores dates as text is the common case, and making the app parse them into milliseconds
// first is what puts a calendar in every app.
str wc_fmt_date_iso(str iso, const char *pattern);
// ISO text → milliseconds since the epoch (0 when it does not parse). The unit is in the
// name on purpose: a `parse_iso()` that returns milliseconds while reading like seconds is
// a date comparison one character off, which turns a day into 86 seconds.
i64 wc_parse_iso_ms(str iso);
i64 wc_parse_iso_secs(str iso); // the same instant, rounded down
i64 wc_parse_iso(str iso);      // = parse_iso_ms: kept, flagged by `webc check`

// ---------------------------------------------------------------- how long ago

// The bucket ladder, in one place: 60 s, 60 min, 24 h, then dates. Rounds *down*.
str wc_ago_at(i64 then_ms, i64 now_ms);
str wc_ago(i64 then_ms); // against time_ms()

// ---------------------------------------------------------------- how long

// "0s" · "45s" · "1m 30s" · "1h 8m" · "2d 3h": the two largest units that are not zero.
str wc_dur_ms(i64 ms);
str wc_dur_secs(i64 secs);

#define fmt_date(ms_, pat_)      wc_fmt_date((ms_), (pat_))
#define fmt_date_iso(iso_, pat_) wc_fmt_date_iso(WC_AS_STR(iso_), (pat_))
#define parse_iso_ms(s_)         wc_parse_iso_ms(WC_AS_STR(s_))
#define parse_iso_secs(s_)       wc_parse_iso_secs(WC_AS_STR(s_))
#define parse_iso(s_)            wc_parse_iso_ms(WC_AS_STR(s_))
#define ago(ms_)                 wc_ago((ms_))
#define ago_at(ms_, now_)        wc_ago_at((ms_), (now_))
#define dur_ms(ms_)              wc_dur_ms((ms_))
#define dur_secs(s_)             wc_dur_secs((s_))