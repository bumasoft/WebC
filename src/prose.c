// SPDX-License-Identifier: Apache-2.0
// Prose: text that has paragraphs and four pieces of inline punctuation, rendered as
// markup without the app owning a parser.
//
// Every WebC app that shows anything longer than a label ends up here: a story body, a
// help page, a colophon. Written by hand it is a scanner of a few dozen lines per app, and
// each one gets the escaping, the unclosed markers and the `%.*s` slicing slightly
// different. The vocabulary is deliberately closed — it is punctuation
// you can type into a database column and read back in a terminal:
//
//   blank line   a paragraph
//   **bold**     <strong>
//   *italic*     <em>
//   `code`       <code>
//   [label](x)   <a href="x">, for x = /, #, http://, https://, mailto:, or anything relative
//
// Everything else in the text is escaped by the builder, exactly like text(). A malformed
// marker is not an error and not markup: an unclosed `**` or a `[` with no `](` is a
// literal character, which is what the author of a stray asterisk meant.
//
// The render is streaming: plain runs are `str` views over the caller's text, handed
// straight to the builder. There is no intermediate buffer, so a 71 KB archive page does
// not copy its own copy.
#include <webc/server.h>
#include <webc/web.h>

// A link target is only a link if it cannot leave the page: a javascript: URL in a column
// somebody else typed is an XSS with extra steps.
static bool safe_href(str h) {
  if (!h.n) return false;
  if (h.p[0] == '/' || h.p[0] == '#') return true;
  static const char *const ok[] = {"http://", "https://", "mailto:", "tel:"};
  for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) {
    str pre = str_lit(ok[i]);
    if (str_has_prefix(h, pre)) return true;
  }
  // A bare relative path ("colophon", "a/story") is fine; "javascript:..." is not, and
  // it is the only scheme-with-a-colon worth refusing by name.
  return !str_contains(h, str_lit(":"));
}

// Index of the next `needle` in s at or after `from`, or s.n.
static size_t find_from(str s, size_t from, str needle) {
  if (!needle.n) return s.n;
  for (size_t i = from; i + needle.n <= s.n; i++)
    if (memcmp(s.p + i, needle.p, needle.n) == 0) return i;
  return s.n;
}

// The end of a paragraph: a newline, then a run of spaces/tabs/CR, then another newline.
// Returns the paragraph's end index and writes where the next paragraph starts.
static size_t para_end(str s, size_t from, size_t *next) {
  for (size_t i = from; i < s.n; i++) {
    if (s.p[i] != '\n') continue;
    size_t j = i + 1;
    while (j < s.n && (s.p[j] == ' ' || s.p[j] == '\t' || s.p[j] == '\r')) j++;
    if (j < s.n && s.p[j] == '\n') {
      *next = j + 1;
      return i;
    }
  }
  *next = s.n;
  return s.n;
}

// One run of inline text: plain characters, plus the four markers. `run` is the start of
// the plain text not yet emitted.
static void inline_run(str s, const WProseOpts *o) {
  size_t i = 0, run = 0;
  for (; i < s.n; i++) {
    char c = s.p[i];
    if (c != '*' && c != '`' && c != '[') continue;

    if (c == '*' && i + 1 < s.n && s.p[i + 1] == '*') {
      size_t end = find_from(s, i + 2, str_lit("**"));
      if (end == s.n) continue; // unclosed: a literal asterisk
      if (run < i) wc_text_str(str_sub(s, run, i));
      WDom *bold = wc_el_open("strong");
      wc_text_str(str_sub(s, i + 2, end));
      wc_el_pop(&bold);
      i = end + 1;
      run = i + 1;
      continue;
    }

    if (c == '*') {
      // The closing `*` must not be the first half of a `**`.
      size_t end = s.n;
      for (size_t j = i + 1; j < s.n; j++) {
        if (s.p[j] != '*') continue;
        if (j + 1 < s.n && s.p[j + 1] == '*') continue;
        end = j;
        break;
      }
      if (end == s.n) continue;
      if (run < i) wc_text_str(str_sub(s, run, i));
      WDom *em = wc_el_open("em");
      wc_text_str(str_sub(s, i + 1, end));
      wc_el_pop(&em);
      i = end;
      run = i + 1;
      continue;
    }

    if (c == '`') {
      size_t end = find_from(s, i + 1, str_lit("`"));
      if (end == s.n) continue;
      if (run < i) wc_text_str(str_sub(s, run, i));
      WDom *code = wc_el_open("code");
      wc_text_str(str_sub(s, i + 1, end));
      wc_el_pop(&code);
      i = end;
      run = i + 1;
      continue;
    }

    // [label](href)
    size_t close = find_from(s, i + 1, str_lit("]("));
    if (close == s.n) continue;
    size_t paren = find_from(s, close + 2, str_lit(")"));
    if (paren == s.n) continue;
    str href = str_sub(s, close + 2, paren);
    if (!safe_href(href)) continue;
    if (run < i) wc_text_str(str_sub(s, run, i));
    WDom *a = wc_el_open("a");
    if (o->link_class) wc_class(str_lit(o->link_class));
    wc_attr("href", href);
    wc_text_str(str_sub(s, i + 1, close));
    wc_el_pop(&a);
    i = paren;
    run = i + 1;
  }
  if (run < s.n) wc_text_str(str_sub(s, run, s.n));
}

void wc_prose_inline(str text) {
  if (!text.p || !text.n) return;
  static const WProseOpts none;
  inline_run(text, &none);
}

i32 wc_prose(str text) {
  static const WProseOpts none;
  return wc_prose_with(text, none);
}

i32 wc_prose_with(str text, WProseOpts o) {
  if (!text.p || !text.n) return 0;
  i32 n = 0;
  size_t at = 0;
  while (at < text.n) {
    size_t next = text.n;
    size_t end = para_end(text, at, &next);
    str para = str_trim(str_sub(text, at, end));
    if (para.n) {
      WDom *p = wc_el_open("p");
      if (n == 0 && o.first)
        wc_class(str_lit(o.first));
      else if (o.class)
        wc_class(str_lit(o.class));
      inline_run(para, &o);
      wc_el_pop(&p);
      n++;
      if (o.after) o.after(n, o.ctx); // between paragraphs: the app's own furniture
    }
    at = next;
  }
  return n;
}