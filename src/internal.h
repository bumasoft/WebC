// WebC — helpers the framework's own translation units share, and nothing else uses.
//
// These are not public surface: they are not installed, `webc api` does not count them, and
// no header under include/webc/ mentions them. They exist so that one rule — how a value is
// escaped onto the wire, how a case-insensitive search is spelled — has one implementation
// instead of one per caller.
#ifndef WEBC_INTERNAL_H
#define WEBC_INTERNAL_H

#include <webc/core.h>

// Case-insensitive substring search. `needle` is a literal, so it is bounded by NUL.
bool str_ci_contains(str hay, const char *needle);

// The last field of a protocol line ("text <id> the rest of it"): the rest of the line,
// with backslashes and newlines escaped so the line stays one line both ways.
void wc_line_esc(Buf *b, str v);

// Text going into HTML: `&`, `<`, `>`, `"` and `'` become entities.
void wc_html_esc_buf(Buf *b, str s);

// The next `k=v` of a `k=v&k=v` string — a form body, a query string, a driver's
// connection parameters. `*i` carries the position between calls; false at the end. A pair
// with no '=' comes back with an empty value, and empty pairs are skipped, so `a=1&&b=2`
// reads as two fields.
bool wc_kv_next(str q, size_t *i, str *key, str *val);

#endif // WEBC_INTERNAL_H