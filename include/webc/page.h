// SPDX-License-Identifier: Apache-2.0
// WebC — the page as a value: a title, a description, the reader's theme.
//
//   GET("/s/:slug") {
//     return render(SectionPage,
//                   .page = head(.title = fmt("%s — The Meridian", sec->name),
//                                .desc = sec->blurb,
//                                .canonical = fmt("https://…/s/%s", sec->slug)),
//                   .section = sec, .list = list);
//   }
//
// This lives in the shared half, not in <webc/http.h>, because the context struct it sits
// in is declared by the app and compiled for both targets: a view is allowed to read
// `c->page.env.dark`, so the type has to exist in the browser too. Only *filling* it needs
// a request, and that is webc_page_begin() (http.h).
#ifndef WEBC_PAGE_H
#define WEBC_PAGE_H

#include <webc/core.h>

typedef struct Head {
  const char *title;    // <title>, and og:title when og_title is not given
  const char *desc;     // <meta name="description">
  const char *og_title, *og_type, *og_image, *og_site;
  const char *section_name; // <meta name="article:section">
  const char *canonical;    // <link rel="canonical">
  const char *alternate;    // <link rel="alternate"> — a feed
} Head;

// What this request knows about the reader, filled by the framework:
//   theme  the cookie, when it says "light" or "dark" ("" otherwise)
//   path   this request's path — where a form should send the reader back to
//   query  ?q= as text, "" when absent
//   dark   theme is "dark"
typedef struct PageEnv {
  const char *theme;
  const char *path;
  const char *query;
  const char *lang;
  bool dark;
} PageEnv;

// The magic is what makes `render()` safe for a context that predates this: a context
// without a `WPage` first field simply is not a page and nothing is filled.
#define WC_PAGE_MAGIC 0x57454243454e4700ULL // "WEBCENG" + 0

typedef struct WPage {
  u64 tag;   // WC_PAGE_MAGIC
  Head head;
  PageEnv env;
} WPage;

// `head(...)` is a compound literal, so every name in it is checked by the compiler and a
// misspelling is not a page with no title.
#define head(...) ((WPage){WC_PAGE_MAGIC, .head = {__VA_ARGS__}})

#endif // WEBC_PAGE_H
