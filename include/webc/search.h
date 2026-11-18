// SPDX-License-Identifier: Apache-2.0
// webc/search.h — a search box that finds a row, not a substring of a column name.
//
// Every app grows one: a filter that is more than `where title like ?`. Three things make the
// hand-written version bad, and all three are here:
//
//   1. **The query is user input.** `%` and `_` are wildcards in LIKE, and a search box that
//      quietly treats `50%` as "fifty followed by anything" is a search box that lies.
//   2. **The words are ANDed, not ORed.** `ada lovelace` should not match every row with "Ada".
//   3. **Ranking is the feature.** A hit in the title beats a hit in the body, and the newest of
//      two equal hits comes first — otherwise the page is a list of rows in id order, which is
//      a table, not a search result.
//
//   VecNote *hits = search(Note, q, "title");      // the string fields, title ranked first
//   for_each(n, (*hits)) {
//     el("li") {
//       el("a") { attr("href", link_to(Note, n)); html(search_hl(n->title, q)); }
//       el("p") { html(search_snippet(n->body, q, 24)); }
//     }
//   }
//
// `search_hl` and `search_snippet` are the two things a view needs and a SQL query cannot give:
// the match, marked, in the reader's own string, escaped for HTML.
//
// **Refuses:** a query language (`title:foo AND -bar`), stemming, synonyms, fuzzy matching,
// a full-text index (that is FTS5 or a `tsvector` and the app can add the column), and
// multi-table search (search each model and show two lists — the app owns the ordering across
// them). It searches `str` and `Text` fields only: a number is a filter, not a search.
#pragma once

#include <webc/core.h>
#include <webc/db.h>
#include <webc/model.h>
#include <webc/dom.h>

// How a query is broken up: words on whitespace, capped at 8, each at most 64 bytes. A search
// box that costs a scan per keystroke does not need to consider the seventeenth word.
typedef struct WSearchWords {
  str w[8];
  i32 n;
} WSearchWords;

WSearchWords wc_search_words(str q);
#define search_words(q_) wc_search_words(WC_AS_STR(q_))

// LIKE-escape: `%`, `_` and the escape character itself, so a user's `50%` means fifty percent.
str wc_search_like(Arena *a, str word); // "50\%" — bind it as a parameter, never inline the SQL
#define search_like(word_) wc_search_like(wc_arena_get(), WC_AS_STR(word_))

// The `where` clause and its bindings, for the fields named in `only` (comma-separated names) or
// every searchable field when `only` is null. The first field named ranks highest.
str wc_search_where(Arena *a, const WModel *m, const char *only, WSearchWords w, WQuery *qy);

// The one-call form: the model's rows that match, best first.
void *wc_search_vec(WDB *db, const WModel *m, const WVecDesc *vec, const char *only, str q, i32 limit);
#define search_vec(db_, T_, only_, q_, limit_) \
  ((Vec##T_ *)wc_search_vec((db_), T_##_modelof(), WC_VECOF_Vec##T_, (only_), WC_AS_STR(q_), (limit_)))
#define search(T_, q_) search_vec(db_default(), T_, nullptr, q_, 50)

// ---------------------------------------------------------------- showing the hit
//
// HTML with every matched word in `<mark>`, escaped on the way. Marks do not nest and a mark is
// never opened inside another, so the output is always balanced — a text node can contain `<`
// and no amount of it can produce a broken document.
str wc_search_hl(Arena *a, str text, WSearchWords w);
#define search_hl(text_, q_) wc_search_hl(wc_arena_get(), WC_AS_STR(text_), wc_search_words(WC_AS_STR(q_)))

// A window of `words` words around the first match, with the whole run highlighted and an ellipsis
// where it was cut. The answer to "why did this row match" for a long body.
str wc_search_snippet(Arena *a, str text, WSearchWords w, i32 words);
#define search_snippet(text_, q_, words_) \
  wc_search_snippet(wc_arena_get(), WC_AS_STR(text_), wc_search_words(WC_AS_STR(q_)), (words_))

// Does this text contain the words? The same rule the SQL applies, in C — for a test, or for a
// client-side filter over rows the app already has.
bool wc_search_match(str text, WSearchWords w);
#define search_match(text_, q_) wc_search_match(WC_AS_STR(text_), wc_search_words(WC_AS_STR(q_)))

// A field the searcher will look at: `str` and `Text`, never the primary key or a sensitive field.
bool wc_search_field(const WModel *m, const WField *f);