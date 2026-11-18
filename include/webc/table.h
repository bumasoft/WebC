// SPDX-License-Identifier: Apache-2.0
// webc/table.h — the sortable, paginated list, without the twenty lines of sort loop an
// app writes before its first column can be clicked.
//
// The table owns *order*, never data: rows stay in the app's array, and the table keeps a
// permutation of indices. That is what makes a sort cheap, a re-render honest (row i is
// still row i for everything else in the app), and the selection stable while the list
// moves underneath it.
//
//   static const WTableKey COLS[] = {
//       {"Player", score_name, nullptr},
//       {"Bank",   score_bank, "right"},
//       {"Seat",   score_seat, "right"},
//   };
//   WTable t;
//   table_init(&t, players, n_players, sizeof players[0], g_order);
//   table_keys(&t, COLS, WC_ARRLEN(COLS));
//   table_state(&t, get(sort), get(page), 20);      // the app's two signals
//
//   view(App) {
//     table_head(&t, sort_by);                      // <thead>, real click targets
//     el("tbody") { for (i32 i = 0; i < table_len(&t); i++) {
//       Player *p = table_row(&t, i); …
//     } }
//     table_pager(&t, page_flip, "Players");
//   }
//
//   action(sort_by) { set(sort, table_click(ev_attr_i64("data-key"), get(sort))); }
//   action(page_flip) { set(page, table_page(&t, get(page) + ev_attr_i64("data-step"))); }
//
// Three things the hand-rolled version always leaves out, and this one cannot: `aria-sort`
// on the column that is actually sorted, a sort that cycles back to the app's own order
// (a third click, not a toggle), and page buttons that announce themselves as disabled
// instead of vanishing.
//
// A key's `score` reads the row through the pointer it is given, so the app's comparator
// is a one-liner per column and the module never learns what a Player is:
//
//   static f64 score_name(const void *row) { return (f64)strlen(((const Player *)row)->name); }
//   static f64 score_bank(const void *row) { return ((const Player *)row)->bank; }
#pragma once

#include <webc/dom.h>

typedef struct WTableKey {
  const char *label;             // the header cell's text
  f64 (*score)(const void *row); // what this column sorts by
  const char *align;             // "right"/"center", null for the default
} WTableKey;

typedef struct WTable {
  const void *rows;
  i32 n;
  u32 stride;
  const WTableKey *keys;
  i32 nkeys;
  i32 *order;      // n indices, app-owned (a file-scope array or the render arena)
  i64 sort;        // 0 = the app's order; +k = key k ascending; -k = descending
  i32 page;        // 0-based
  i32 per_page;    // 0 = one page
} WTable;

static inline void wc_table_init(WTable *t, const void *rows, i32 n, u32 stride, i32 *order) {
  *t = (WTable){.rows = rows, .n = n, .stride = stride, .order = order};
  for (i32 i = 0; i < n; i++) order[i] = i;
}
#define table_init(...) wc_table_init(__VA_ARGS__)

static inline void wc_table_keys(WTable *t, const WTableKey *keys, i32 nkeys) {
  t->keys = keys;
  t->nkeys = nkeys;
}
#define table_keys(t_, keys_, n_) wc_table_keys((t_), (keys_), (n_))

// The state the app has already put in signals. Called on every render, so the module
// never keeps a copy that can disagree with the page.
static inline void wc_table_state(WTable *t, i64 sort, i32 page, i32 per_page) {
  t->sort = sort;
  t->page = page < 0 ? 0 : page;
  t->per_page = per_page;

  for (i32 i = 0; i < t->n; i++) t->order[i] = i;
  if (sort == 0 || !t->keys || t->n < 2) return;

  i32 k = (i32)(sort < 0 ? -sort : sort) - 1;
  if (k < 0 || k >= t->nkeys || !t->keys[k].score) return;
  f64 (*score)(const void *) = t->keys[k].score;
  bool desc = sort < 0;
  // Insertion sort: the rows are usually nearly ordered already (one column changed),
  // and a stable sort keeps ties in the app's own order instead of shuffling them.
  for (i32 i = 1; i < t->n; i++) {
    i32 cur = t->order[i], j = i - 1;
    f64 a = score((const char *)t->rows + (size_t)cur * t->stride);
    while (j >= 0) {
      f64 b = score((const char *)t->rows + (size_t)t->order[j] * t->stride);
      bool after = desc ? a > b : a < b;
      if (!after) break;
      t->order[j + 1] = t->order[j];
      j--;
    }
    t->order[j + 1] = cur;
  }
}
#define table_state(t_, sort_, page_, per_page_) wc_table_state((t_), (sort_), (page_), (per_page_))

// The three-state cycle a header click means: ascending, descending, and back to the order
// the app ranked the rows in. A two-state toggle cannot get home again.
static inline i64 wc_table_click(i32 key, i64 sort) {
  i64 want = key + 1;
  if (sort == want) return -want;
  if (sort == -want) return 0;
  return want;
}
#define table_click(key_, sort_) wc_table_click((key_), (sort_))

static inline i32 wc_table_pages(const WTable *t) {
  if (t->per_page <= 0 || t->n == 0) return 1;
  return (t->n + t->per_page - 1) / t->per_page;
}
#define table_pages(t_) wc_table_pages((t_))

static inline i32 wc_table_len(const WTable *t) {
  if (t->per_page <= 0) return t->n;
  i32 from = t->page * t->per_page;
  if (from >= t->n) return 0;
  i32 left = t->n - from;
  return left < t->per_page ? left : t->per_page;
}
#define table_len(t_) wc_table_len((t_))

// Moving by `step` pages, clamped. The clamp is here rather than in the action because a
// page button that is disabled must not be able to move anyway.
static inline i32 wc_table_page(const WTable *t, i32 want) {
  i32 last = wc_table_pages(t) - 1;
  if (want < 0) return 0;
  if (want > last) return last;
  return want;
}
#define table_page(t_, want_) wc_table_page((t_), (want_))

// The row at position i *of this page*, and the row's index in the app's own array.
static inline const void *wc_table_row(const WTable *t, i32 i) {
  i32 at = t->per_page > 0 ? t->page * t->per_page + i : i;
  if (at < 0 || at >= t->n) return nullptr;
  return (const char *)t->rows + (size_t)t->order[at] * t->stride;
}
#define table_row(t_, i_) wc_table_row((t_), (i_))
#define table_row_typed(T_, t_, i_) ((const T_ *)wc_table_row((t_), (i_)))

static inline i32 wc_table_at(const WTable *t, i32 i) {
  i32 at = t->per_page > 0 ? t->page * t->per_page + i : i;
  if (at < 0 || at >= t->n) return -1;
  return t->order[at];
}
#define table_at(t_, i_) wc_table_at((t_), (i_))

// `<thead>`: one cell per key, each one a real button in the header, carrying `data-key` and
// the `aria-sort` that tells a screen reader which column is sorted and which way.
static inline void wc_table_head_(WTable *t, const char *hook, WEventFn fn) {
  el("thead") {
    el("tr") {
      for (i32 k = 0; k < t->nkeys; k++) {
        bool on = t->sort == k + 1 || t->sort == -(i64)(k + 1);
        el("th") {
          attr("scope", "col");
          if (t->keys[k].align) attr("data-align", t->keys[k].align);
          if (t->sort == (i64)k + 1) attr("aria-sort", "ascending");
          else if (t->sort == -(i64)(k + 1)) attr("aria-sort", "descending");
          else attr("aria-sort", "none");
          if (hook && *hook) {
            el("button") {
              class_(on ? "th-btn on" : "th-btn");
              type_("button");
              wc_on_click(hook, fn);
              attr_i("data-key", k);
              text(t->keys[k].label ? t->keys[k].label : "");
            }
          } else {
            text(t->keys[k].label ? t->keys[k].label : "");
          }
        }
      }
    }
  }
}
// The action is passed as a symbol, not a string: a header that sorts has to name the
// behaviour, and the compiler is the only thing that checks the name exists.
#define table_head(t_, ACT_) wc_table_head_((t_), "" #ACT_, (WEventFn)WC_CAT(action_, ACT_))

// The pager: how many rows are shown, how many there are, and two buttons. They carry
// `data-step` and `aria-disabled`, so one action moves the page and the ends do not shift.
static inline void wc_table_pager_(WTable *t, const char *hook, WEventFn fn, const char *unit) {
  i32 pages = wc_table_pages(t);
  if (pages <= 1) return;
  i32 from = t->page * t->per_page + 1;
  i32 to = from + wc_table_len(t) - 1;
  el("nav") {
    class_("pager");
    attr("aria-label", unit && *unit ? unit : "Pagination");
    el("span") {
      class_("pager-count");
      text(fmt("%lld–%lld of %lld", (long long)from, (long long)to, (long long)t->n));
    }
    for (i32 step = -1; step <= 1; step += 2) {
      bool off = step < 0 ? t->page == 0 : t->page >= pages - 1;
      el("button") {
        class_(off ? "pager-btn off" : "pager-btn");
        type_("button");
        attr_i("data-step", step);
        if (off) attr("aria-disabled", "true");
        else wc_on_click(hook, fn);
        text(step < 0 ? "Prev" : "Next");
      }
    }
    el("span") {
      class_("pager-page");
      text(fmt("%lld / %lld", (long long)t->page + 1, (long long)pages));
    }
  }
}
#define table_pager(t_, ACT_, unit_) wc_table_pager_((t_), "" #ACT_, (WEventFn)WC_CAT(action_, ACT_), (unit_))