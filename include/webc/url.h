// SPDX-License-Identifier: Apache-2.0
// WebC — URLs and the small text formatting every page needs, on both halves.
//
// This is the half of a page's vocabulary that a *view* uses: the link to a story, the
// date under it, the heading the desk name becomes. It cannot live in <webc/http.h>,
// which a shared view may not include: an app that needs those helpers there restates its
// own — `story_href`, `section_href`, `author_href`, `wire_date`, `person_name` — five
// copies of facts the framework already knows, each one a place for a route to drift.
//
// Nothing here needs a request, a socket or a database: a pattern, a number and a
// string. So it links into both targets.
#ifndef WEBC_URL_H
#define WEBC_URL_H

#include <webc/core.h>
#include <webc/model.h>

// ---------------------------------------------------------------- named patterns
//
// A pattern declared once, where both halves can see it:
//
//   src/routes.h:  ROUTE("story", "/a/:slug")
//   view:          href(url_to("story", "slug", st->slug))        -> /a/the-last-ferry
//
// On the server a `GET("/a/:slug", "story")` registers the same name by itself, so an
// app that only renders on the server needs no ROUTE() line. The client half has no
// router, so a mounted view that builds URLs wants the declaration in a shared header.
#define ROUTE(name_, pattern_)                                                          \
  __attribute__((constructor)) static void WC_CAT(wc_url_reg_, __COUNTER__)(void) {      \
    wc_url_register((name_), (pattern_));                                               \
  }

// Registering the same name with the same pattern twice is fine (server and shared
// header both say it); the same name with a *different* pattern is the drift this
// exists to catch, and it panics naming both.
void wc_url_register(const char *name, const char *pattern);
i32  wc_url_count(void);
const char *wc_url_pattern(const char *name); // NULL when the name is not registered

// ---------------------------------------------------------------- building a URL
//
// `url_to("story", "slug", st->slug)` — name, then (param, value) pairs. A value may be
// a Text, a `str` or a literal. Values are percent-encoded as they go in, so a slug with
// a space or a `?` in it cannot break the path or inject a query parameter.
str wc_url_for(const char *name, const str *kv, i32 npairs);
// The same, from a literal pattern instead of a name: `url_build("/s/:slug", ...)`.
str wc_url_from(Arena *a, const char *pattern, const str *kv, i32 npairs, const char *what);

#define WC_STR_OF(x_) WC_AS_STR(x_)
#define url_to(...) WC_CAT(WC_URLTO_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_URLTO_1(name_) wc_url_for((name_), nullptr, 0)
#define WC_URLTO_3(name_, k_, v_)                                              \
  wc_url_for((name_), (str[]){WC_STR_OF(k_), WC_STR_OF(v_)}, 1)
#define WC_URLTO_5(name_, k1_, v1_, k2_, v2_)                                  \
  wc_url_for((name_), (str[]){WC_STR_OF(k1_), WC_STR_OF(v1_), WC_STR_OF(k2_), WC_STR_OF(v2_)}, 2)
#define WC_URLTO_7(name_, k1_, v1_, k2_, v2_, k3_, v3_)                        \
  wc_url_for((name_),                                                          \
             (str[]){WC_STR_OF(k1_), WC_STR_OF(v1_), WC_STR_OF(k2_), WC_STR_OF(v2_), WC_STR_OF(k3_), \
                     WC_STR_OF(v3_)},                                          \
             3)

#define url_build(...) WC_CAT(WC_URLB_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_URLB_1(pattern_)                                                    \
  wc_url_from(wc_arena_get(), (pattern_), nullptr, 0, "url_build")
#define WC_URLB_3(pattern_, k_, v_)                                            \
  wc_url_from(wc_arena_get(), (pattern_), (str[]){WC_STR_OF(k_), WC_STR_OF(v_)}, 1, "url_build")
#define WC_URLB_5(pattern_, k1_, v1_, k2_, v2_)                                \
  wc_url_from(wc_arena_get(), (pattern_),                                      \
              (str[]){WC_STR_OF(k1_), WC_STR_OF(v1_), WC_STR_OF(k2_), WC_STR_OF(v2_)}, 2, \
              "url_build")

// `link_to(Story, st)` — the URL for a model that names its route:
//   WEBC_MODEL_ROUTE(Story, "/a/:slug")
// Every `:field` in the pattern is read from the object, so a renamed field is a
// compile error rather than a link that goes to `/a/(null)`.
str wc_link(const WModel *m, const void *obj);
#define link_to(T_, obj_) wc_link(T_##_modelof(), (const void *)(obj_))

// ---------------------------------------------------------------- text
//
// A heading, a slug: the two conversions every app writes by hand and every app gets
// slightly differently.
//
//   title_case("science & nature")        -> Science & Nature
//   slugify("What the tide left behind")  -> what-the-tide-left-behind
//
// Dates and durations moved to `webc/time.h`: a URL module that also formatted
// timestamps made "the text helpers" and "the time helpers" one header, so an app that
// needed `ago()` pulled in the router's escaping and an app that needed `slugify()`
// pulled in a civil-calendar conversion. One subject per header.
#include <webc/time.h>

str wc_title_case(str s);
str wc_slugify(str s);
str wc_name_from_slug(str slug);   // "ada-lovelace"     -> "Ada Lovelace"
i32 wc_read_minutes(str body);     // 220 words a minute, rounded up, never 0
str wc_read_time(str body);        // "4 min read"
bool wc_contains_ci(str hay, str needle); // ASCII case-insensitive substring

#define title_case(s_)           wc_title_case(WC_AS_STR(s_))
#define slugify(s_)              wc_slugify(WC_AS_STR(s_))
#define name_from_slug(s_)       wc_name_from_slug(WC_AS_STR(s_))
#define read_minutes(s_)         wc_read_minutes(WC_AS_STR(s_))
#define read_time(s_)            wc_read_time(WC_AS_STR(s_))
#define contains_ci(hay_, needle_) wc_contains_ci(WC_AS_STR(hay_), WC_AS_STR(needle_))

#endif // WEBC_URL_H