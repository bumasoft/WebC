// SPDX-License-Identifier: Apache-2.0
// webc/richtext.h — HTML that came from the database, rendered safely.
//
// The framework has three cases for authored bytes, and until this module it served two:
//
//   prose()    copy with punctuation: escaped, a closed vocabulary             (a label)
//   markup()   bytes *you* wrote: trusted, verbatim                            (an icon)
//   rich()     bytes a *user* wrote and a column stored: untrusted             (a CMS body)
//
// The third case is every CMS, blog, wiki, comment section and admin inbox, and the only
// safe answer to it is a sanitiser — one that parses once, re-emits through the framework's
// own `el()`/`attr()`/`text()` (so escaping is not a second implementation), and refuses
// what a policy does not name.
//
//   #include <webc/richtext.h>
//
//   rich(stored_html);                               // the default policy: docs, CMS bodies
//   rich_with(stored_html, .allow = RICH_TIGHT);      // comments: p, a, em, strong, code, br
//   rich_with(stored_html, .no_images = true, .external_rel = "noopener noreferrer");
//
// The same policy answers the derived questions an article page asks:
//
//   text("%s", wc_rich_excerpt(stored, 180));       // plain text, word boundary, ellipsis
//   text("%d min read", wc_rich_reading_time(stored));
//   WRichHeading toc[32]; i32 n; wc_rich_headings_into(stored, toc, 32, &n);
//   str stored = wc_rich_stamp_ids(body);           // stable heading ids, written on save
//
// **What it guarantees.** Tags and attributes outside the policy are dropped; `script`,
// `style`, `iframe`, `object`, `embed`, `svg`, `math`, `form`, `template` and friends are
// dropped *with their contents*; every attribute value is entity-decoded before it is
// judged, so `java&#115;cript:` is a `javascript:` URL and is refused; `href`/`src`/`poster`
// accept only http(s), mailto, tel and relative URLs (a `data:` URI is refused unless
// `.allow_data_images`, and then only for a non-SVG image); an element left open is closed
// at the end of the fragment; a closing tag that the fragment did not open closes nothing.
//
// **What it is not.** Not Markdown (that is `prose()`), not a validator, and not a
// document-level guarantee: it reasons about one fragment. A CMS that stores (or stamps) what
// this returns is safe to render; a CMS that renders what the *browser posted* is not, and
// no sanitiser on the read path changes that — run `wc_rich_stamp_ids` on the way in as well.
#pragma once

#include <webc/core.h>
#include <webc/dom.h>
#include <webc/pp.h>

// The default sets, as space-separated words, because a policy is data an app can read and a
// reviewer can check without reading this file.
#define RICH_DOCS                                                                              \
  "p br hr strong b em i u s code pre blockquote ul ol li h2 h3 h4 h5 h6 a img figure "        \
  "figcaption table thead tbody tfoot tr th td dl dt dd sup sub mark small span div video "    \
  "source details summary cite time abbr kbd q del ins"
#define RICH_TIGHT "p br a em strong code s blockquote ul ol li"
#define RICH_NO_IMG ""

typedef struct WRich {
  const char *allow;          // nullptr = RICH_DOCS
  bool no_images;             // drop img/picture/video/source instead of rendering them
  bool lazy;                  // loading="lazy" decoding="async" on images
  bool allow_data_images;     // permit `data:image/...;base64,` in src/srcset
  const char *external_rel;   // rel for a link; nullptr = "noopener noreferrer"
  i32 max_depth;              // 0 = 40; a nesting cap, so a hostile body cannot recurse deep
} WRich;

// ---------------------------------------------------------------- the policy's tables

// The tags whose *contents* go with them. Not a policy knob: a `script` whose text survived
// is a script again the moment something else rendered it.
static inline bool wc_rich_drop_all(str t) {
  static const char *const DROP[] = {"script", "style",  "iframe", "object",   "embed",
                                     "svg",    "math",   "form",   "template", "noscript",
                                     "frame",  "frameset", "applet", "base",    "link",
                                     "meta",   "title",  "head",   "body",     "html",
                                     "button", "select", "textarea", "input"};
  for (size_t i = 0; i < sizeof DROP / sizeof DROP[0]; i++)
    if (str_eq_c(t, DROP[i])) return true;
  return false;
}

// Void elements: emitted, never a parent.
static inline bool wc_rich_void(str t) {
  static const char *const VOID[] = {"br", "hr", "img", "source", "wbr", "area", "col",
                                     "track", "param"};
  for (size_t i = 0; i < sizeof VOID / sizeof VOID[0]; i++)
    if (str_eq_c(t, VOID[i])) return true;
  return false;
}

// Block-level tags: where a fragment becomes text, a word boundary.
static inline bool wc_rich_block(str t) {
  static const char *const BLOCK[] = {"p",    "div",  "br",   "hr",     "li",   "ul",     "ol",
                                      "h1",   "h2",   "h3",   "h4",     "h5",   "h6",     "pre",
                                      "blockquote",  "tr",   "td",     "th",   "table",  "figure",
                                      "figcaption",  "section", "article", "header", "footer",
                                      "dd",   "dt",   "dl",   "details", "summary", "hr"};
  for (size_t i = 0; i < sizeof BLOCK / sizeof BLOCK[0]; i++)
    if (str_eq_c(t, BLOCK[i])) return true;
  return false;
}

// A word in one of the policy's space-separated lists. Substring matching would let `pre`
// match `preview`; a word match cannot.
static inline bool wc_rich_has_word(const char *words, str t) {
  if (!words) return false;
  size_t i = 0, n = strlen(words);
  while (i < n) {
    while (i < n && (words[i] == ' ' || words[i] == ',')) i++;
    size_t start = i;
    while (i < n && words[i] != ' ' && words[i] != ',') i++;
    if (i > start && str_eq((str){(char *)words + start, i - start}, t)) return true;
  }
  return false;
}

static inline bool wc_rich_tag_ok(str t, const WRich *p) {
  if (wc_rich_drop_all(t)) return false;
  if (!wc_rich_has_word((p && p->allow) ? p->allow : RICH_DOCS, t)) return false;
  if (p && p->no_images &&
      (str_eq_c(t, "img") || str_eq_c(t, "picture") || str_eq_c(t, "video") ||
       str_eq_c(t, "source")))
    return false;
  return true;
}

// Attributes: what styles a document and cannot execute. `on*` never appears here, and
// neither does `style` — an attribute that can carry a URL is worth more to an attacker than
// the styling is to the author.
static inline bool wc_rich_attr_ok(str n) {
  static const char *const OK[] = {"href",  "src",    "alt",     "title",   "id",     "class",
                                   "width", "height", "datetime", "colspan", "rowspan", "target",
                                   "rel",   "loading", "decoding", "srcset", "controls", "poster",
                                   "type",  "start",  "reversed", "scope",   "lang",   "dir",
                                   "cite",  "open",   "span",     "abbr"};
  for (size_t i = 0; i < sizeof OK / sizeof OK[0]; i++)
    if (str_eq_c(n, OK[i])) return true;
  return false;
}

// ---------------------------------------------------------------- entities

// The named entities a CMS emits, plus numeric ones; anything else is left as it stands and
// escaped on the way out. Decoding is what makes `java&#115;cript:` a URL, so it runs before
// any URL decision, never after.
static inline void wc_rich_entity(Buf *b, str ent) {
  struct {
    const char *name;
    const char *bytes;
  } static const TBL[] = {
      {"amp", "&"},     {"lt", "<"},      {"gt", ">"},       {"quot", "\""},
      {"apos", "'"},    {"nbsp", " "},    {"mdash", "\xE2\x80\x94"},
      {"ndash", "\xE2\x80\x93"},          {"hellip", "\xE2\x80\xA6"},
      {"rsquo", "\xE2\x80\x99"},          {"lsquo", "\xE2\x80\x98"},
      {"ldquo", "\xE2\x80\x9C"},          {"rdquo", "\xE2\x80\x9D"},
      {"middot", "\xC2\xB7"},             {"copy", "\xC2\xA9"},
      {"reg", "\xC2\xAE"},                {"trade", "\xE2\x84\xA2"},
      {"deg", "\xC2\xB0"},                {"laquo", "\xC2\xAB"},
      {"raquo", "\xC2\xBB"},              {"times", "\xC3\x97"},
  };
  if (ent.n > 1 && ent.p[0] == '#') {
    bool hex = ent.n > 2 && (ent.p[1] == 'x' || ent.p[1] == 'X');
    i64 code = 0;
    bool any = false;
    for (size_t i = hex ? 2 : 1; i < ent.n; i++) {
      char c = ent.p[i];
      i32 d = (c >= '0' && c <= '9')   ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                       : -1;
      if (d < 0) return;
      code = code * (hex ? 16 : 10) + d;
      any = true;
      if (code > 0x10FFFF) return;
    }
    if (!any || code <= 0) return;
    if (code < 0x80) {
      buf_putc(b, (char)code);
    } else if (code < 0x800) {
      buf_putc(b, (char)(0xC0 | (code >> 6)));
      buf_putc(b, (char)(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      buf_putc(b, (char)(0xE0 | (code >> 12)));
      buf_putc(b, (char)(0x80 | ((code >> 6) & 0x3F)));
      buf_putc(b, (char)(0x80 | (code & 0x3F)));
    } else {
      buf_putc(b, (char)(0xF0 | (code >> 18)));
      buf_putc(b, (char)(0x80 | ((code >> 12) & 0x3F)));
      buf_putc(b, (char)(0x80 | ((code >> 6) & 0x3F)));
      buf_putc(b, (char)(0x80 | (code & 0x3F)));
    }
    return;
  }
  for (size_t i = 0; i < sizeof TBL / sizeof TBL[0]; i++)
    if (str_eq_c(ent, TBL[i].name)) {
      buf_puts(b, TBL[i].bytes);
      return;
    }
}

// Every `&…;` replaced by what it means. An `&` with no `;` within 12 bytes stays an `&`,
// which `text()` escapes back to the same thing.
static inline str wc_rich_unescape(Arena *a, str s) {
  if (str_index(s, '&') < 0) return s;
  Buf b;
  buf_init(&b, a, s.n + 16);
  for (size_t i = 0; i < s.n; i++) {
    if (s.p[i] != '&') {
      buf_putc(&b, s.p[i]);
      continue;
    }
    size_t semi = 0;
    for (size_t k = i + 1; k < s.n && k < i + 12; k++)
      if (s.p[k] == ';') {
        semi = k;
        break;
      }
    size_t before = b.n;
    if (semi) wc_rich_entity(&b, (str){s.p + i + 1, semi - i - 1});
    if (b.n == before) buf_putc(&b, '&');
    if (semi) i = semi;
  }
  return buf_str(&b);
}

// ---------------------------------------------------------------- URLs

// A URL attribute, judged after decoding and after the characters a browser ignores are
// gone: `java\tscript:` and `\x01javascript:` are one scheme to a browser, and `javascript:`
// is the reason this function exists.
static inline bool wc_rich_url_ok(str raw, bool allow_data_images) {
  if (raw.n == 0) return false;
  char clean[1024];
  size_t n = 0;
  for (size_t i = 0; i < raw.n && n + 1 < sizeof clean; i++) {
    unsigned char c = (unsigned char)raw.p[i];
    if (c <= 0x20 || c == 0x7F) continue; // tabs, newlines, spaces, control characters
    clean[n++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
  }
  clean[n] = 0;
  if (n == 0) return false;
  str low = {clean, n};
  i64 colon = str_index(low, ':');
  if (colon < 0) return true; // /a, #b, ?c, or a bare path: no scheme at all
  i64 slash = str_index(low, '/');
  if (slash >= 0 && slash < colon) return true; // "a/b:c" is a path, not a scheme
  if (strncmp(clean, "http:", 5) == 0 || strncmp(clean, "https:", 6) == 0) return true;
  if (strncmp(clean, "mailto:", 7) == 0 || strncmp(clean, "tel:", 4) == 0) return true;
  // A `data:` URI is a document in disguise unless it is a raster image encoded as base64:
  // `data:text/html` runs, and `data:image/svg+xml` is an SVG, which is a document.
  if (allow_data_images && strncmp(clean, "data:image/", 11) == 0)
    return strstr(clean, ";base64,") != nullptr && strstr(clean, "svg") == nullptr;
  return false;
}

// ---------------------------------------------------------------- the tag scanner

// One opening tag starting at `at`: its name in `*tag`, its attributes in `names`/`vals`
// (at most `cap`), and the index just past its `>`.
static inline size_t wc_rich_tag(str s, size_t at, str *tag, str *names, str *vals, i32 cap,
                                 i32 *nv, bool *self_close) {
  size_t i = at + 1; // past '<'
  size_t ns = i;
  while (i < s.n && s.p[i] != '>' && s.p[i] != '/' && s.p[i] != ' ' && s.p[i] != '\t' &&
         s.p[i] != '\n' && s.p[i] != '\r')
    i++;
  *tag = (str){s.p + ns, i - ns};
  *nv = 0;
  *self_close = false;
  while (i < s.n) {
    while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t' || s.p[i] == '\n' || s.p[i] == '\r')) i++;
    if (i >= s.n) break;
    if (s.p[i] == '>') return i + 1;
    if (s.p[i] == '/') {
      *self_close = true;
      i++;
      continue;
    }
    size_t ks = i;
    while (i < s.n && s.p[i] != '=' && s.p[i] != '>' && s.p[i] != ' ' && s.p[i] != '\t' &&
           s.p[i] != '\n' && s.p[i] != '\r')
      i++;
    str name = {s.p + ks, i - ks};
    while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t' || s.p[i] == '\n' || s.p[i] == '\r')) i++;
    str val = {"", 0};
    if (i < s.n && s.p[i] == '=') {
      i++;
      while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t' || s.p[i] == '\n' || s.p[i] == '\r')) i++;
      if (i < s.n && (s.p[i] == '"' || s.p[i] == '\'')) {
        char q = s.p[i++];
        size_t vs = i;
        while (i < s.n && s.p[i] != q) i++;
        val = (str){s.p + vs, i - vs};
        if (i < s.n) i++;
      } else {
        size_t vs = i;
        while (i < s.n && s.p[i] != '>' && s.p[i] != ' ' && s.p[i] != '\t' && s.p[i] != '\n' &&
               s.p[i] != '\r')
          i++;
        val = (str){s.p + vs, i - vs};
      }
    }
    if (name.n && *nv < cap) {
      names[*nv] = name;
      vals[*nv] = val;
      (*nv)++;
    }
  }
  return i;
}

// The index just past the closing tag of `name`, or `s.n` when it never closes. A dropped
// `script` may contain `</` in a string, but not `</script`, so a case-insensitive name
// match is the whole of it.
static inline size_t wc_rich_end_of(str s, size_t from, const char *name) {
  size_t nl = strlen(name);
  for (size_t i = from; i + 2 < s.n; i++) {
    if (s.p[i] != '<' || s.p[i + 1] != '/') continue;
    size_t k = i + 2;
    while (k < s.n && (s.p[k] == ' ' || s.p[k] == '\t' || s.p[k] == '\n' || s.p[k] == '\r')) k++;
    if (k + nl >= s.n) continue;
    bool same = true;
    for (size_t j = 0; j < nl; j++) {
      char c = s.p[k + j];
      if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
      if (c != name[j]) {
        same = false;
        break;
      }
    }
    if (!same) continue;
    size_t after = k + nl;
    while (after < s.n && s.p[after] != '>') after++;
    return after < s.n ? after + 1 : s.n;
  }
  return s.n;
}

// Where the content of the element that begins at `from` ends: the `<` of its closing tag,
// or `s.n` when it never closes (a fragment cut off mid-element still renders).
static inline size_t wc_rich_content_end(str s, size_t from, const char *name) {
  size_t end = wc_rich_end_of(s, from, name);
  if (end >= s.n) return s.n;
  size_t back = end;
  while (back > from && s.p[back - 1] != '<') back--;
  return back > from ? back - 1 : end;
}

// A lowercase, NUL-terminated copy of a tag name. Tag names are case-insensitive in HTML.
static inline str wc_rich_lower(Arena *a, str t) {
  char *p = arena_alloc(a, t.n + 1);
  for (size_t i = 0; i < t.n; i++)
    p[i] = (char)(t.p[i] >= 'A' && t.p[i] <= 'Z' ? t.p[i] + 32 : t.p[i]);
  p[t.n] = 0;
  return (str){p, t.n};
}

// ---------------------------------------------------------------- the renderer

// Attributes of one element after the policy and the URL rules. `v` is decoded before it is
// judged, so what the browser will see is what was checked.
static inline void wc_rich_emit_attrs(Arena *a, str *names, str *vals, i32 nv, str tag,
                                      const WRich *p) {
  bool anchor = str_eq_c(tag, "a");
  bool has_rel = false, has_target = false;
  bool external = false;
  for (i32 k = 0; k < nv; k++) {
    str n = wc_rich_lower(a, names[k]);
    if (n.n == 0 || !wc_rich_attr_ok(n)) continue;
    str v = wc_rich_unescape(a, vals[k]);
    if (str_eq_c(n, "href") || str_eq_c(n, "src") || str_eq_c(n, "poster") ||
        str_eq_c(n, "cite")) {
      if (!wc_rich_url_ok(v, p && p->allow_data_images)) continue;
      if (anchor && str_eq_c(n, "href") &&
          (str_has_prefix_c(v, "http://") || str_has_prefix_c(v, "https://")))
        external = true;
      attr(str_cstr(a, n), WC_AS_STR(v));
      continue;
    }
    if (str_eq_c(n, "srcset")) {
      // Every candidate has to pass, or the whole attribute goes: one `javascript:` member
      // is a srcset that can run.
      bool ok = v.n > 0;
      size_t k2 = 0;
      while (ok && k2 <= v.n) {
        size_t e = k2;
        while (e < v.n && v.p[e] != ',') e++;
        str cand = str_trim(str_sub(v, k2, e));
        i64 sp = str_index(cand, ' ');
        if (sp > 0) cand = str_sub(cand, 0, (size_t)sp);
        if (cand.n) ok = wc_rich_url_ok(cand, p && p->allow_data_images);
        if (e >= v.n) break;
        k2 = e + 1;
      }
      if (ok) attr("srcset", WC_AS_STR(v));
      continue;
    }
    if (str_eq_c(n, "rel")) has_rel = true;
    if (str_eq_c(n, "target")) {
      if (!(v.n == 6 && strncmp(v.p, "_blank", 6) == 0))
        continue; // `target` names a browsing context, and only the blank one is common
      has_target = true;
    }
    attr(str_cstr(a, n), WC_AS_STR(v));
  }
  // A link that leaves the site (or opens its own tab) gets `rel` unless the author wrote
  // one: without it, `target=_blank` hands the opener to the page it points at.
  if (anchor && !has_rel && (external || has_target)) {
    const char *rel = (p && p->external_rel) ? p->external_rel : "noopener noreferrer";
    if (rel && *rel) attr("rel", rel);
  }
  if (p && p->lazy && str_eq_c(tag, "img")) {
    attr("loading", "lazy");
    attr("decoding", "async");
  }
}

// The slugs a fragment has used, so a second "Getting started" is `-2` and a *different*
// heading is not — a counter is not a uniqueness check.
typedef struct WRichSlugs {
  str used[64];
  i32 n;
} WRichSlugs;

static inline str wc_rich_slug(Arena *a, str plain);
static inline str wc_rich_slug_u(Arena *a, str plain, WRichSlugs *seen);

// The visible text of a fragment with its whitespace collapsed: source indentation is not a
// heading's name, and a table of contents is a label, not a paragraph.
static inline str wc_rich_flatten(Arena *a, str s) {
  Buf b;
  buf_init(&b, a, s.n + 1);
  bool space = false;
  for (size_t i = 0; i < s.n; i++) {
    char c = s.p[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      space = true;
      continue;
    }
    if (space && b.n) buf_putc(&b, ' ');
    space = false;
    buf_putc(&b, c);
  }
  return buf_str(&b);
}

typedef struct WRichWalk {
  Arena *a;
  const WRich *p;
  i32 depth, max_depth;
  WRichSlugs *slugs; // what the heading ids have used so far
} WRichWalk;

// Renders `s[from, stop)`. The recursion is what a C block macro can express — an open tag
// with children is `el(tag) { … }` — and `max_depth` is what keeps a hostile 10,000-deep
// body from being a stack overflow instead of a page.
static inline void wc_rich_render(WRichWalk *w, str s, size_t from, size_t stop) {
  Arena *a = w->a;
  size_t i = from;
  while (i < stop) {
    if (s.p[i] != '<') {
      size_t start = i;
      while (i < stop && s.p[i] != '<') i++;
      str chunk = {s.p + start, i - start};
      text("%s", str_cstr(a, wc_rich_unescape(a, chunk)));
      continue;
    }
    if (i + 3 < stop && s.p[i + 1] == '!' && s.p[i + 2] == '-' && s.p[i + 3] == '-') {
      size_t k = i + 4;
      while (k + 2 < stop && !(s.p[k] == '-' && s.p[k + 1] == '-' && s.p[k + 2] == '>')) k++;
      i = k + 3 <= stop ? k + 3 : stop;
      continue;
    }
    if (i + 1 < stop && (s.p[i + 1] == '!' || s.p[i + 1] == '?')) {
      while (i < stop && s.p[i] != '>') i++;
      if (i < stop) i++;
      continue;
    }
    if (i + 1 < stop && s.p[i + 1] == '/') { // a closing tag closes nothing the fragment did not open
      while (i < stop && s.p[i] != '>') i++;
      if (i < stop) i++;
      continue;
    }
    str tag = {"", 0}, names[24], vals[24];
    i32 nv = 0;
    bool self_close = false;
    size_t after = wc_rich_tag(s, i, &tag, names, vals, 24, &nv, &self_close);
    if (tag.n == 0) { // a bare `<` in text
      text("<");
      i++;
      continue;
    }
    str ltag = wc_rich_lower(a, tag);
    i = after;
    if (wc_rich_drop_all(ltag)) { // its contents go with it
      i = wc_rich_end_of(s, i, str_cstr(a, ltag));
      continue;
    }
    if (!wc_rich_tag_ok(ltag, w->p)) continue; // an unknown tag goes, its text stays
    if (w->depth + 1 >= w->max_depth) continue;
    if (wc_rich_void(ltag) || self_close) {
      el(str_cstr(a, ltag)) { wc_rich_emit_attrs(a, names, vals, nv, ltag, w->p); }
      continue;
    }
    size_t content_end = wc_rich_content_end(s, i, str_cstr(a, ltag));
    bool heading = ltag.n == 2 && ltag.p[0] == 'h' && ltag.p[1] >= '1' && ltag.p[1] <= '6';
    el(str_cstr(a, ltag)) {
      if (heading && w->slugs) {
        Buf t;
        buf_init(&t, a, 64);
        for (size_t k = i; k < content_end; k++) {
          if (s.p[k] == '<') {
            while (k < content_end && s.p[k] != '>') k++;
            continue;
          }
          buf_putc(&t, s.p[k]);
        }
        str plain = wc_rich_flatten(a, wc_rich_unescape(a, buf_str(&t)));
        attr("id", WC_AS_STR(wc_rich_slug_u(a, plain, w->slugs)));
      }
      wc_rich_emit_attrs(a, names, vals, nv, ltag, w->p);
      w->depth++;
      wc_rich_render(w, s, i, content_end);
      w->depth--;
    }
    i = content_end;
    if (i < stop && s.p[i] == '<') { // past the closing tag, if there was one
      size_t k = i;
      while (k < stop && s.p[k] != '>') k++;
      i = k + 1 <= stop ? k + 1 : stop;
    }
  }
}

// ---------------------------------------------------------------- the doors

// The id a heading gets: its text, slugged.
static inline str wc_rich_slug(Arena *a, str plain) {
  Buf b;
  buf_init(&b, a, plain.n + 8);
  bool dash = false;
  for (size_t i = 0; i < plain.n; i++) {
    unsigned char c = (unsigned char)plain.p[i];
    bool lower = c >= 'a' && c <= 'z';
    bool digit = c >= '0' && c <= '9';
    bool upper = c >= 'A' && c <= 'Z';
    if (lower || digit || upper) {
      if (dash && b.n) buf_putc(&b, '-');
      dash = false;
      buf_putc(&b, upper ? (char)(c + 32) : (char)c);
    } else if (c == ' ' || c == '\t' || c == '\n' || c == '-' || c == '_' || c == '.') {
      dash = true;
    }
  }
  if (b.n == 0) buf_puts(&b, "section");
  return buf_str(&b);
}

// …and made unique against the slugs before it: `install`, then `install-2`.
static inline str wc_rich_slug_u(Arena *a, str plain, WRichSlugs *seen) {
  str base = wc_rich_slug(a, plain);
  if (!seen) return base;
  bool taken = false;
  for (i32 i = 0; i < seen->n; i++)
    if (str_eq(seen->used[i], base)) taken = true;
  str id = base;
  for (i32 n = 2; taken && n < 100; n++) {
    Buf b;
    buf_init(&b, a, base.n + 8);
    buf_putstr(&b, base);
    buf_printf(&b, "-%d", n);
    id = buf_str(&b);
    taken = false;
    for (i32 i = 0; i < seen->n; i++)
      if (str_eq(seen->used[i], id)) taken = true;
  }
  if (seen->n < 64) seen->used[seen->n++] = id;
  return id;
}

static inline void wc_rich_with(str html, WRich p) {
  if (!html.p || html.n == 0) return;
  Arena *a = wc_arena_get();
  WRichSlugs slugs = {0};
  WRichWalk w = {.a = a, .p = &p, .slugs = &slugs};
  w.max_depth = p.max_depth > 0 ? p.max_depth : 40;
  wc_rich_render(&w, html, 0, html.n);
}

// The policy's own door: `rich(stored)` is the default set, `rich_with(stored, .allow = …)`
// is a named one. Both are the same walk; the difference is a struct with a zero in it.
#define rich(s_) wc_rich_with(WC_AS_STR(s_), (WRich){0})
#define rich_with(s_, ...) wc_rich_with(WC_AS_STR(s_), (WRich){__VA_ARGS__})

// ---------------------------------------------------------------- text, words, headings

// Plain text: markup removed, entities decoded, a run of whitespace one space, a block tag a
// boundary. This is what a `<meta name="description">` and an excerpt are made of.
static inline str wc_rich_plain(Arena *a, str html) {
  if (!html.p || html.n == 0) return (str){"", 0};
  Buf b;
  buf_init(&b, a, html.n);
  bool space = false;
  size_t i = 0;
  while (i < html.n) {
    if (html.p[i] != '<') {
      size_t start = i;
      while (i < html.n && html.p[i] != '<') i++;
      str chunk = {html.p + start, i - start};
      str dec = wc_rich_unescape(a, chunk);
      for (size_t k = 0; k < dec.n; k++) {
        char c = dec.p[k];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
          space = true;
          continue;
        }
        if (space && b.n) buf_putc(&b, ' ');
        space = false;
        buf_putc(&b, c);
      }
      continue;
    }
    if (i + 1 < html.n && (html.p[i + 1] == '/' || html.p[i + 1] == '!' || html.p[i + 1] == '?')) {
      // A closing tag is a word boundary only when the element was block-level:
      // `<strong>Ada</strong>, 1843` is "Ada, 1843", not "Ada , 1843".
      bool boundary = false;
      if (html.p[i + 1] == '/') {
        size_t j = i + 2;
        size_t ts = j;
        while (j < html.n && html.p[j] != '>' && html.p[j] != ' ' && html.p[j] != '\t') j++;
        if (j > ts) boundary = wc_rich_block(wc_rich_lower(a, (str){html.p + ts, j - ts}));
      }
      while (i < html.n && html.p[i] != '>') i++;
      if (i < html.n) i++;
      if (boundary) space = true;
      continue;
    }
    str tag = {"", 0}, names[4], vals[4];
    i32 nv = 0;
    bool self_close = false;
    size_t after = wc_rich_tag(html, i, &tag, names, vals, 4, &nv, &self_close);
    str ltag = tag.n ? wc_rich_lower(a, tag) : (str){"", 0};
    i = after;
    if (ltag.n && wc_rich_drop_all(ltag)) {
      i = wc_rich_end_of(html, i, str_cstr(a, ltag));
      space = true;
      continue;
    }
    if (ltag.n && wc_rich_block(ltag)) space = true;
  }
  return buf_str(&b);
}

// Word count: "1,240 words" is this number. Every tag is a boundary, whether or not the author
// put whitespace around it — `<cite>Victor H</cite><cite>Antifragile</cite>` is three words, and
// a counter that reasoned from the plain text would glue the last two together (the browser
// does exactly that on screen, but "how long is this article" is not "how does it wrap").
static inline i32 wc_rich_words(str html) {
  if (!html.p || html.n == 0) return 0;
  Arena *a = wc_arena_get();
  i32 n = 0;
  bool in_word = false;
  size_t i = 0;
  while (i < html.n) {
    if (html.p[i] == '<') {
      in_word = false;
      while (i < html.n && html.p[i] != '>') i++;
      if (i < html.n) i++;
      continue;
    }
    size_t start = i;
    while (i < html.n && html.p[i] != '<') i++;
    str dec = wc_rich_unescape(a, (str){html.p + start, i - start});
    for (size_t k = 0; k < dec.n; k++) {
      char c = dec.p[k];
      bool sep = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
      if (sep) {
        in_word = false;
      } else if (!in_word) {
        in_word = true;
        n++;
      }
    }
  }
  return n;
}

// An excerpt: plain text cut on a word boundary. `limit` counts characters.
static inline str wc_rich_excerpt(str html, size_t limit) {
  Arena *a = wc_arena_get();
  str t = wc_rich_plain(a, html);
  if (limit == 0 || t.n <= limit) return t;
  size_t cut = limit;
  while (cut > 0 && t.p[cut] != ' ') cut--;
  if (cut == 0) cut = limit;
  Buf b;
  buf_init(&b, a, cut + 4);
  buf_putstr(&b, (str){t.p, cut});
  buf_puts(&b, "\xE2\x80\xA6"); // …
  return buf_str(&b);
}

// "6 min read" — 200 words a minute, never less than one.
static inline i32 wc_rich_reading_time(str html) {
  i32 m = (wc_rich_words(html) + 199) / 200;
  return m < 1 ? 1 : m;
}

typedef struct WRichHeading {
  i32 level;   // 1..6
  str text;    // "Getting started"
  str id;      // "getting-started", or the id the fragment already carried
} WRichHeading;

// The table of contents in document order. An id that the fragment already carries wins, so
// `rich()` and the toc agree whether the ids were stamped on save or are being derived now.
static inline void wc_rich_headings_into(str html, WRichHeading *out, i32 cap, i32 *n_out) {
  Arena *a = wc_arena_get();
  i32 n = 0;
  WRichSlugs seen = {0};
  size_t i = 0;
  while (i < html.n && n < cap) {
    if (html.p[i] != '<') {
      i++;
      continue;
    }
    if (i + 1 < html.n && (html.p[i + 1] == '/' || html.p[i + 1] == '!' || html.p[i + 1] == '?')) {
      while (i < html.n && html.p[i] != '>') i++;
      if (i < html.n) i++;
      continue;
    }
    str tag = {"", 0}, names[8], vals[8];
    i32 nv = 0;
    bool self_close = false;
    size_t after = wc_rich_tag(html, i, &tag, names, vals, 8, &nv, &self_close);
    i = after;
    if (tag.n != 2 || (tag.p[0] != 'h' && tag.p[0] != 'H') || tag.p[1] < '1' || tag.p[1] > '6')
      continue;
    size_t content_end = wc_rich_content_end(html, i, "h");
    Buf t;
    buf_init(&t, a, 64);
    for (size_t k = i; k < content_end; k++) {
      if (html.p[k] == '<') {
        while (k < content_end && html.p[k] != '>') k++;
        continue;
      }
      buf_putc(&t, html.p[k]);
    }
    str plain = wc_rich_flatten(a, wc_rich_unescape(a, buf_str(&t)));
    str given = {"", 0};
    for (i32 k = 0; k < nv; k++)
      if (str_eq_c(names[k], "id")) given = wc_rich_unescape(a, vals[k]);
    if (given.n && seen.n < 64) seen.used[seen.n++] = given; // an author's id uses its slug
    out[n].level = tag.p[1] - '0';
    out[n].text = plain;
    out[n].id = given.n ? given : wc_rich_slug_u(a, plain, &seen);
    n++;
    i = wc_rich_end_of(html, content_end, "h");
  }
  if (n_out) *n_out = n;
}

// ---------------------------------------------------------------- stamping ids
//
// The save path: the same document, with a stable `id` on every heading that lacks one, so an
// anchor a reader copied still lands after the article was edited. It does not sanitise —
// that is `rich()` at render time, and doing both is how a stored body stays re-readable.
static inline str wc_rich_stamp_ids(str html) {
  Arena *a = wc_arena_get();
  if (!html.p || html.n == 0) return html;
  Buf out;
  buf_init(&out, a, html.n + 64);
  WRichSlugs seen = {0};
  size_t i = 0;
  while (i < html.n) {
    if (html.p[i] != '<') {
      buf_putc(&out, html.p[i++]);
      continue;
    }
    if (i + 1 < html.n && (html.p[i + 1] == '/' || html.p[i + 1] == '!' || html.p[i + 1] == '?')) {
      size_t k = i;
      while (k < html.n && html.p[k] != '>') k++;
      if (k < html.n) k++;
      buf_putstr(&out, (str){html.p + i, k - i});
      i = k;
      continue;
    }
    str tag = {"", 0}, names[24], vals[24];
    i32 nv = 0;
    bool self_close = false;
    size_t after = wc_rich_tag(html, i, &tag, names, vals, 24, &nv, &self_close);
    bool heading = tag.n == 2 && (tag.p[0] == 'h' || tag.p[0] == 'H') && tag.p[1] >= '1' &&
                   tag.p[1] <= '6';
    if (!heading) {
      buf_putstr(&out, (str){html.p + i, after - i});
      i = after;
      continue;
    }
    bool has_id = false;
    for (i32 k = 0; k < nv; k++)
      if (str_eq_c(names[k], "id")) has_id = true;
    if (has_id) { // an author's id is theirs; it also uses up its slug
      str given = {"", 0};
      for (i32 k = 0; k < nv; k++)
        if (str_eq_c(names[k], "id")) given = wc_rich_unescape(a, vals[k]);
      if (given.n && seen.n < 64) seen.used[seen.n++] = given;
      buf_putstr(&out, (str){html.p + i, after - i});
      i = after;
      continue;
    }
    size_t content_end = wc_rich_content_end(html, after, "h");
    Buf t;
    buf_init(&t, a, 64);
    for (size_t k = after; k < content_end; k++) {
      if (html.p[k] == '<') {
        while (k < content_end && html.p[k] != '>') k++;
        continue;
      }
      buf_putc(&t, html.p[k]);
    }
    str plain = wc_rich_flatten(a, wc_rich_unescape(a, buf_str(&t)));
    // insert ` id="…"` just before the tag's `>`
    size_t gt = after;
    while (gt > i && html.p[gt - 1] != '>') gt--;
    size_t name_end = gt > i ? gt - 1 : after;
    if (name_end < html.n && html.p[name_end - 1] == '/') name_end--; // `<h2/>`
    buf_putstr(&out, (str){html.p + i, name_end - i});
    buf_puts(&out, " id=\"");
    buf_putstr(&out, wc_rich_slug_u(a, plain, &seen));
    buf_puts(&out, "\">");
    i = after;
  }
  return buf_str(&out);
}