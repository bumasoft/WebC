// SPDX-License-Identifier: Apache-2.0
// webc/search.h — words, a LIKE that means what the user typed, and two renderers for the hit.
#include <webc/search.h>

WSearchWords wc_search_words(str q) {
  WSearchWords out = {0};
  if (!q.p || !q.n) return out;
  size_t i = 0;
  while (i < q.n && out.n < 8) {
    while (i < q.n && (q.p[i] == ' ' || q.p[i] == '\t' || q.p[i] == '\n' || q.p[i] == '\r')) i++;
    if (i >= q.n) break;
    size_t start = i;
    while (i < q.n && q.p[i] != ' ' && q.p[i] != '\t' && q.p[i] != '\n' && q.p[i] != '\r') i++;
    size_t n = i - start;
    if (n > 64) n = 64;
    if (n) out.w[out.n++] = (str){q.p + start, n};
  }
  return out;
}

str wc_search_like(Arena *a, str word) {
  Buf b;
  buf_init(&b, a, word.n * 2 + 4);
  for (size_t i = 0; i < word.n; i++) {
    char c = word.p[i];
    // `\` is the escape the clause declares; `%` and `_` are wildcards, and a search box that
    // silently turns a typed `%` into "anything" is a search box that lies
    if (c == '%' || c == '_' || c == '\\') buf_putc(&b, '\\');
    buf_putc(&b, c);
  }
  return buf_str(&b);
}

bool wc_search_field(const WModel *m, const WField *f) {
  (void)m;
  if (!f || f->q.pk || f->q.sensitive || f->q.no_json) return false;
  return f->kind == WK_STR || f->kind == WK_TEXT;
}

// The clause: every word must appear in at least one field (AND across words, OR across fields),
// and the fields are tried in the order the caller named them, so "title" can rank first.
str wc_search_where(Arena *a, const WModel *m, const char *only, WSearchWords w, WQuery *qy) {
  Buf b;
  buf_init(&b, a, 256);
  if (w.n == 0) {
    buf_puts(&b, "1 = 1");
    return buf_str(&b);
  }
  // which fields: the names in `only`, or every searchable one, model order
  const WField *fields[32];
  i32 nf = 0;
  if (only && *only) {
    size_t i = 0;
    while (only[i] && nf < 32) {
      while (only[i] == ' ' || only[i] == ',') i++;
      size_t start = i;
      while (only[i] && only[i] != ',') i++;
      size_t n = i - start;
      while (n && only[start + n - 1] == ' ') n--;
      if (!n) continue;
      i32 idx = model_field_index(m, (str){only + start, n});
      if (idx >= 0 && idx < m->n && wc_search_field(m, &m->fields[idx])) fields[nf++] = &m->fields[idx];
    }
  }
  if (nf == 0)
    for (i32 i = 0; i < m->n && nf < 32; i++)
      if (wc_search_field(m, &m->fields[i])) fields[nf++] = &m->fields[i];
  if (nf == 0) {
    buf_puts(&b, "1 = 0"); // a model with nothing to search finds nothing, and says so
    return buf_str(&b);
  }
  for (i32 wi = 0; wi < w.n; wi++) {
    if (wi) buf_puts(&b, " and ");
    buf_putc(&b, '(');
    for (i32 fi = 0; fi < nf; fi++) {
      if (fi) buf_puts(&b, " or ");
      // lower(...) on both sides: the driver's LIKE is case-insensitive for ASCII on SQLite and
      // case-sensitive on Postgres, and a search that works on one and not the other is a bug an
      // app finds in production. `escape` names the character search_like() actually used.
      buf_printf(&b, "lower(%s) like lower(:w%d) escape '\\'", fields[fi]->name, wi);
    }
    buf_putc(&b, ')');
  }
  // rank: a hit in the first named field first, then the newest of equals
  buf_puts(&b, " order by case");
  for (i32 fi = 0; fi < nf && fi < 4; fi++)
    buf_printf(&b, " when lower(%s) like lower(:w0) escape '\\' then %d", fields[fi]->name, fi);
  buf_puts(&b, " else 9 end");
  // and among equals, the newest: the pk is the only monotonic column a model is guaranteed to
  // have, and a search whose ties come back in insertion order is a table with extra steps
  for (i32 i = 0; i < m->n; i++)
    if (m->fields[i].q.pk) {
      buf_printf(&b, ", %s desc", m->fields[i].name);
      break;
    }
  for (i32 wi = 0; wi < w.n; wi++) {
    Buf name;
    buf_init(&name, a, 16);
    buf_printf(&name, "w%d", wi);
    // one `%word%` per word, escaped and bound — never pasted into the SQL
    str pat = wc_search_like(a, w.w[wi]);
    Buf full;
    buf_init(&full, a, pat.n + 4);
    buf_putc(&full, '%');
    buf_putstr(&full, pat);
    buf_putc(&full, '%');
    q_str(qy, buf_cstr(&name), buf_str(&full));
  }
  return buf_str(&b);
}

void *wc_search_vec(WDB *db, const WModel *m, const WVecDesc *vec, const char *only, str query,
                    i32 limit) {
  WSearchWords w = wc_search_words(query);
  Arena *a = wc_arena_get();
  WQuery *qy = q("");
  str clause = wc_search_where(a, m, only, w, qy);
  Buf full;
  buf_init(&full, a, clause.n + 32);
  buf_puts(&full, "where ");
  buf_putstr(&full, clause);
  buf_printf(&full, " limit %d", limit > 0 ? limit : 50);
  return db_all_clause_vec(db, m, vec, buf_cstr(&full), qy);
}

// ---------------------------------------------------------------- showing the hit

static bool word_at(str text, size_t i, WSearchWords w, size_t *len) {
  for (i32 k = 0; k < w.n; k++) {
    size_t n = w.w[k].n;
    if (i + n > text.n) continue;
    bool eq = true;
    for (size_t c = 0; c < n && eq; c++) {
      char x = text.p[i + c], y = w.w[k].p[c];
      if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
      if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
      eq = x == y;
    }
    if (eq) {
      *len = n;
      return true;
    }
  }
  return false;
}

// One pass: escape everything, wrap what matches. Because the wrap happens *while* escaping, no
// markup the text contains can interfere and the marks cannot cross.
static str mark_up(Arena *a, str text, WSearchWords w, size_t from, size_t to, bool ellipsis) {
  Buf b;
  buf_init(&b, a, (to - from) + 32);
  if (ellipsis && from > 0) buf_puts(&b, "… ");
  for (size_t i = from; i < to;) {
    size_t len = 0;
    if (w.n && word_at(text, i, w, &len) && i + len <= to) {
      buf_puts(&b, "<mark>");
      buf_putstr(&b, html_escape(a, (str){text.p + i, len}));
      buf_puts(&b, "</mark>");
      i += len;
      continue;
    }
    size_t run = i;
    size_t stop = i + 1;
    while (stop < to && !word_at(text, stop, w, &len)) stop++;
    buf_putstr(&b, html_escape(a, (str){text.p + run, stop - run}));
    i = stop;
  }
  if (ellipsis && to < text.n) buf_puts(&b, " …");
  return buf_str(&b);
}

str wc_search_hl(Arena *a, str text, WSearchWords w) {
  return mark_up(a, text, w, 0, text.n, false);
}

// A window of N words around the first match: the sentence a reader needs to see why the row was
// in the answer, without the six paragraphs above it.
str wc_search_snippet(Arena *a, str text, WSearchWords w, i32 words) {
  if (!text.p || !text.n) return (str){0};
  if (words <= 0) words = 24;
  size_t first = text.n;
  for (size_t i = 0; i < text.n; i++) {
    size_t len = 0;
    if (word_at(text, i, w, &len)) {
      first = i;
      break;
    }
  }
  if (first >= text.n) { // no match in this text: the opening words, unmarked
    size_t i = 0;
    for (i32 k = 0; k < words && i < text.n; k++) {
      while (i < text.n && text.p[i] == ' ') i++;
      while (i < text.n && text.p[i] != ' ') i++;
    }
    return mark_up(a, text, (WSearchWords){0}, 0, i, true);
  }
  // back up a few words for context, then forward `words` from there
  size_t start = first;
  for (i32 k = 0; k < 6 && start > 0; k++) {
    while (start > 0 && text.p[start - 1] == ' ') start--;
    while (start > 0 && text.p[start - 1] != ' ') start--;
  }
  size_t end = start;
  for (i32 k = 0; k < words && end < text.n; k++) {
    while (end < text.n && text.p[end] == ' ') end++;
    while (end < text.n && text.p[end] != ' ') end++;
  }
  return mark_up(a, text, w, start, end, true);
}

bool wc_search_match(str text, WSearchWords w) {
  if (!w.n) return true;
  for (i32 k = 0; k < w.n; k++) {
    bool found = false;
    if (w.w[k].n && text.p) {
      for (size_t i = 0; i + w.w[k].n <= text.n && !found; i++) {
        size_t len = 0;
        WSearchWords one = {{w.w[k]}, 1};
        if (word_at(text, i, one, &len)) found = true;
      }
    }
    if (!found) return false;
  }
  return true;
}
