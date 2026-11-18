// SPDX-License-Identifier: Apache-2.0
// WebC — reading a response in a test: JSON by path, HTML by selector.
//
// A test asserts what a page *is*, not what it contains. Substring checks pass when
// the markup moved somewhere the CSS no longer reaches, which is exactly the regression
// a redesign causes; and a test that wants a number out of a JSON body would otherwise
// write its own parser.
//
//   ASSERT_JSON(res, "views", "42");                       // by path
//   ASSERT_HTML(res, "article.lead > h2 a");               // the structure survived
//   ASSERT_HTML_TEXT(res, "article.lead h2 a", "Ferry");   // ... with the right words
//
// The selector is a useful subset of CSS, not all of it: tag, .class, #id,
// [attr], [attr="value"], descendant (space) and child (>). Anything else is a
// test failure with a sentence saying so, because a selector that silently matches
// nothing is worse than one that says it does not understand you.
#include <math.h>
#include <webc/test.h>

// ---------------------------------------------------------------- JSON by path

// "data.0.views", "items[2].id", "views" — dots and brackets both separate.
bool wc_json_path(JVal *v, const char *path, JVal *out) {
  if (!v) return false;
  JVal *cur = v;
  const char *p = path;
  while (*p && cur) {
    while (*p == '.' || *p == '[') p++;
    const char *start = p;
    while (*p && *p != '.' && *p != ']' && *p != '[') p++;
    str key = {(char *)start, (size_t)(p - start)};
    if (*p == ']') p++;
    if (key.n == 0) return false;
    if (cur->kind == J_OBJ) {
      cur = json_get_str(cur, key);
    } else if (cur->kind == J_ARR) {
      i64 i = -1;
      if (!str_to_i64(key, &i) || i < 0 || i >= cur->n) return false;
      cur = cur->items[i];
    } else {
      return false;
    }
  }
  if (!cur) return false;
  if (out) *out = *cur;
  return true;
}

str wc_json_at_str(str body, const char *path) {
  Arena *a = wc_arena_get();
  if (!a) return (str){nullptr, 0};
  JParse jp = json_parse(a, body);
  if (!jp.ok) return (str){nullptr, 0};
  JVal v;
  if (!wc_json_path(jp.v, path, &v)) return (str){nullptr, 0};
  switch (v.kind) {
  case J_STR: return v.s;
  case J_NUM:
  case J_BOOL:
  case J_NULL:
  default: return v.kind == J_NUM ? str_from_i64(a, v.is_int ? v.inum : (i64)v.num)
                                  : (v.kind == J_BOOL ? str_lit(v.b ? "true" : "false")
                                                      : str_lit("null"));
  }
}

// The one place a page states facts for a machine: the first JSON-LD block in the body.
// Between `<script` … `application/ld+json` … `>` and the `</script>` that closes it. The
// payload is returned as it sits in the document (already escaped for the script context), and
// `wc_json_at_str` reads it from there — `\u003c` is a `<` to a JSON parser.
str wc_ld_block(str html) {
  if (!html.p || html.n == 0) return (str){nullptr, 0};
  const char *mark = "application/ld+json";
  size_t ml = strlen(mark), open = SIZE_MAX;
  for (size_t i = 0; i + ml <= html.n; i++)
    if (memcmp(html.p + i, mark, ml) == 0) { open = i + ml; break; }
  if (open == SIZE_MAX) return (str){nullptr, 0};
  size_t start = open;
  while (start < html.n && html.p[start] != '>') start++;
  if (start >= html.n) return (str){nullptr, 0};
  start++;
  size_t end = start;
  while (end + 8 <= html.n && memcmp(html.p + end, "</script", 8) != 0) end++;
  if (end + 8 > html.n) end = html.n;
  while (start < end && (html.p[start] == ' ' || html.p[start] == '\n')) start++;
  while (end > start && (html.p[end - 1] == ' ' || html.p[end - 1] == '\n')) end--;
  return (str){(char *)html.p + start, end - start};
}

// ---------------------------------------------------------------- HTML by selector

typedef struct HAttr {
  str name, value;
} HAttr;

typedef struct HEl {
  str tag;
  HAttr attrs[16];
  i32 nattrs;
  i32 nth;     // 1-based position among its parent's element children
  size_t open_end; // offset just past the '>' that opened it
  bool self_closed;
} HEl;

static bool h_is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Two `str`s, case-insensitively (HTML tag names are), with no allocation: the
// scanner runs over every tag in a page.
static bool h_eq_i(str a, str b) {
  if (a.n != b.n) return false;
  for (size_t i = 0; i < a.n; i++) {
    char x = a.p[i], y = b.p[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

static bool h_eq(str a, str b) {
  return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}

static str h_trim(str s) {
  while (s.n && h_is_space(*s.p)) {
    s.p++;
    s.n--;
  }
  while (s.n && h_is_space(s.p[s.n - 1])) s.n--;
  return s;
}

// Parse the tag that starts at `i` (which holds '<'). Returns the offset just past
// '>' and fills `el`. Returns 0 for a comment, a doctype or a stray '<'.
static size_t h_parse_tag(str html, size_t i, HEl *el, bool *is_close) {
  *is_close = false;
  el->nattrs = 0;
  el->self_closed = false;
  size_t p = i + 1;
  if (p >= html.n) return 0;
  if (html.p[p] == '!' || html.p[p] == '?') { // <!doctype, <!-- -->, <?xml
    while (p < html.n && html.p[p] != '>') p++;
    return p < html.n ? p + 1 : 0;
  }
  if (html.p[p] == '/') {
    *is_close = true;
    p++;
  }
  size_t ts = p;
  while (p < html.n && (html.p[p] == '-' || html.p[p] == ':' ||
                        (html.p[p] >= 'a' && html.p[p] <= 'z') ||
                        (html.p[p] >= 'A' && html.p[p] <= 'Z') ||
                        (html.p[p] >= '0' && html.p[p] <= '9')))
    p++;
  el->tag = (str){html.p + ts, p - ts};
  if (el->tag.n == 0) return 0;
  while (p < html.n && html.p[p] != '>') {
    while (p < html.n && h_is_space(html.p[p])) p++;
    if (p < html.n && html.p[p] == '/') {
      el->self_closed = true;
      p++;
      continue;
    }
    size_t ns = p;
    while (p < html.n && !h_is_space(html.p[p]) && html.p[p] != '=' && html.p[p] != '>') p++;
    str name = (str){html.p + ns, p - ns};
    str value = {html.p + p, 0};
    while (p < html.n && h_is_space(html.p[p])) p++;
    if (p < html.n && html.p[p] == '=') {
      p++;
      while (p < html.n && h_is_space(html.p[p])) p++;
      if (p < html.n && (html.p[p] == '"' || html.p[p] == '\'')) {
        char q = html.p[p++];
        size_t vs = p;
        while (p < html.n && html.p[p] != q) p++;
        value = (str){html.p + vs, p - vs};
        if (p < html.n) p++;
      } else {
        size_t vs = p;
        while (p < html.n && !h_is_space(html.p[p]) && html.p[p] != '>') p++;
        value = (str){html.p + vs, p - vs};
      }
    }
    if (name.n && el->nattrs < (i32)WC_COUNTOF(el->attrs))
      el->attrs[el->nattrs++] = (HAttr){name, value};
  }
  if (p >= html.n) return 0;
  el->open_end = p + 1;
  return p + 1;
}

static str h_attr(const HEl *el, const char *name) {
  str want = {(char *)name, strlen(name)};
  for (i32 i = 0; i < el->nattrs; i++)
    if (h_eq(el->attrs[i].name, want)) return el->attrs[i].value;
  return (str){nullptr, 0};
}

static str h_attr_s(const HEl *el, str name) {
  for (i32 i = 0; i < el->nattrs; i++)
    if (h_eq(el->attrs[i].name, name)) return el->attrs[i].value;
  return (str){nullptr, 0};
}

// One compound selector: `article.lead#main[data-x="1"]`.
typedef struct HSel {
  str tag;
  str classes[6];
  i32 nclasses;
  str id;
  str attrs[4], avals[4];
  char ops[4];   // '=' (default), '^' (starts with), '*' (contains), '$' (ends with)
  i32 nattrs;
  bool child;    // joined to the previous part by '>'
  i32 nth;       // 0 = no nth-child; otherwise 1-based position among element siblings
} HSel;

static bool h_end_of_compound(char c) {
  return c == ' ' || c == '>' || c == '\t' || c == '\n' || c == '\0';
}

static i32 h_parse_selector(const char *sel, HSel *parts, i32 max) {
  i32 n = 0;
  const char *p = sel;
  bool pending_child = false;
  while (*p) {
    // the combinator before this compound: '>' joins it to the parent, a space (or
    // nothing, at the start) means "any ancestor"
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '>') {
      if (*p == '>') pending_child = true;
      p++;
    }
    if (!*p) break;
    if (n >= max) return -1;
    HSel *s = &parts[n];
    memset(s, 0, sizeof *s);
    s->child = pending_child;
    pending_child = false;
    if (*p != '.' && *p != '#' && *p != '[') {
      const char *st = p;
      while (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':' && !h_end_of_compound(*p)) p++;
      s->tag = (str){(char *)st, (size_t)(p - st)};
    }
    while (*p && !h_end_of_compound(*p)) {
      if (*p == '.') {
        const char *st = ++p;
        while (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':' && !h_end_of_compound(*p)) p++;
        if (s->nclasses < (i32)WC_COUNTOF(s->classes))
          s->classes[s->nclasses++] = (str){(char *)st, (size_t)(p - st)};
      } else if (*p == '#') {
        const char *st = ++p;
        while (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':' && !h_end_of_compound(*p)) p++;
        s->id = (str){(char *)st, (size_t)(p - st)};
      } else if (*p == '[') {
        p++;
        const char *st = p;
        while (*p && *p != ']' && *p != '=' && *p != '^' && *p != '*' && *p != '$') p++;
        str name = {(char *)st, (size_t)(p - st)};
        str val = {nullptr, 0};
        char op = '=';
        if (*p == '^' || *p == '*' || *p == '$') {
          op = *p++;
          if (*p == '=') p++;
        } else if (*p == '=') {
          p++;
          if (*p == '"' || *p == '\'') {
            char q = *p++;
            const char *vs = p;
            while (*p && *p != q) p++;
            val = (str){(char *)vs, (size_t)(p - vs)};
            if (*p == q) p++;
          } else {
            const char *vs = p;
            while (*p && *p != ']') p++;
            val = (str){(char *)vs, (size_t)(p - vs)};
          }
        }
        if (*p == ']') p++;
        if (s->nattrs < (i32)WC_COUNTOF(s->attrs)) {
          s->attrs[s->nattrs] = name;
          s->avals[s->nattrs] = val;
          s->ops[s->nattrs] = op;
          s->nattrs++;
        }
      } else if (*p == ':') {
        p++;
        const char *st = p;
        while (*p && *p != '(' && *p != '.' && *p != '#' && *p != '[' && !h_end_of_compound(*p)) p++;
        str name = {(char *)st, (size_t)(p - st)};
        if (*p == '(') {
          p++;
          const char *ns = p;
          while (*p && *p != ')') p++;
          if (h_eq(name, str_lit("nth-child"))) {
            i64 k = strtoll(ns, nullptr, 10);
            s->nth = (i32)(k > 0 ? k : 1);
          }
          if (*p == ')') p++;
        } else if (h_eq(name, str_lit("first-child"))) {
          s->nth = 1;
        }
      } else {
        p++; // an unsupported character: skip one and keep the compound usable
      }
    }
    n++;
  }
  return n;
}

static bool h_has_class(str class_attr, str want) {
  size_t i = 0;
  while (i < class_attr.n) {
    while (i < class_attr.n && h_is_space(class_attr.p[i])) i++;
    size_t st = i;
    while (i < class_attr.n && !h_is_space(class_attr.p[i])) i++;
    str got = {(char *)class_attr.p + st, i - st};
    if (str_eq(got, want)) return true;
  }
  return false;
}

static bool h_match(const HEl *el, const HSel *s) {
  if (s->tag.n && !h_eq_i(el->tag, s->tag)) return false;
  if (s->id.n) {
    str id = h_attr(el, "id");
    if (!str_eq(id, s->id)) return false;
  }
  if (s->nclasses) {
    str cls = h_attr(el, "class");
    for (i32 i = 0; i < s->nclasses; i++)
      if (!h_has_class(cls, s->classes[i])) return false;
  }
  for (i32 i = 0; i < s->nattrs; i++) {
    str v = h_attr_s(el, s->attrs[i]);
    if (!v.p) return false;
    str want = s->avals[i];
    if (!want.n) continue; // [attr] alone: presence is enough
    switch (s->ops[i]) {
    case '^':
      if (!(v.n >= want.n && memcmp(v.p, want.p, want.n) == 0)) return false;
      break;
    case '$':
      if (!(v.n >= want.n && memcmp(v.p + v.n - want.n, want.p, want.n) == 0)) return false;
      break;
    case '*':
      if (!want.n || !memmem(v.p, v.n, want.p, want.n)) return false;
      break;
    default:
      if (!str_eq(v, want)) return false;
      break;
    }
  }
  return true;
}

// The scanner: walk the document, keep the open-element stack, and report every
// element whose ancestor chain satisfies the selector.
typedef struct HFind {
  str html;
  Arena *a;
  HSel parts[8];
  i32 nparts;
  HEl stack[64];
  i32 child_of[65]; // how many element children the parent at this depth has opened
  i32 depth;
  i32 matches;
  size_t first_open, first_close; // the first match's extent
  HEl first_el;                   // ... and the element that opened it
} HFind;

static void h_scan(HFind *f) {
  str html = f->html;
  size_t i = 0;
  while (i < html.n) {
    if (html.p[i] != '<') {
      i++;
      continue;
    }
    if (html.n - i >= 4 && memcmp(html.p + i, "<!--", 4) == 0) {
      size_t end = i;
      while (end + 3 <= html.n && memcmp(html.p + end, "-->", 3) != 0) end++;
      i = end + 3;
      continue;
    }
    HEl el;
    bool close = false;
    size_t next = h_parse_tag(html, i, &el, &close);
    if (!next) {
      i++;
      continue;
    }
    if (close) {
      for (i32 k = f->depth - 1; k >= 0; k--)
        if (h_eq_i(f->stack[k].tag, el.tag)) {
          f->depth = k;
          break;
        }
      i = next;
      continue;
    }
    el.nth = ++f->child_of[f->depth];
    // an element is a candidate when the last part matches and the ancestors can
    // satisfy the rest, nearest first
    const HSel *last = &f->parts[f->nparts - 1];
    if ((!last->nth || last->nth == el.nth) && h_match(&el, last)) {
      i32 at = f->depth - 1;
      bool ok = true;
      for (i32 s = f->nparts - 2; s >= 0 && ok; s--) {
        bool need_direct = f->parts[s + 1].child;
        bool found = false;
        while (at >= 0) {
          i32 want_nth = f->parts[s].nth;
          if (want_nth && f->stack[at].nth != want_nth) {
            if (need_direct) break;
            at--;
            continue;
          }
          if (h_match(&f->stack[at], &f->parts[s])) {
            found = true;
            at--;
            break;
          }
          if (need_direct) break;
          at--;
        }
        ok = found;
      }
      if (ok) {
        f->matches++;
        if (f->first_close == 0) {
          f->first_el = el;
          f->first_open = el.open_end;
          // find this element's close tag by counting nested opens of the same tag
          i32 nest = 1;
          size_t j = el.open_end;
          while (j < html.n && nest > 0) {
            if (html.p[j] == '<') {
              HEl sub;
              bool sub_close = false;
              size_t n2 = h_parse_tag(html, j, &sub, &sub_close);
              if (n2 && sub_close && h_eq_i(sub.tag, el.tag))
                nest--;
              else if (n2 && !sub_close && h_eq_i(sub.tag, el.tag) &&
                       !sub.self_closed)
                nest++;
              j = n2 ? n2 : j + 1;
              continue;
            }
            j++;
          }
          f->first_close = j;
        }
      }
    }
    if (!el.self_closed && f->depth < (i32)WC_COUNTOF(f->stack)) {
      f->stack[f->depth++] = el;
      f->child_of[f->depth] = 0; // this element's own children start at one
    }
    i = next;
  }
}

static bool h_prepare(HFind *f, str html, const char *selector) {
  memset(f, 0, sizeof *f);
  f->html = html;
  f->a = wc_arena_get();
  if (!f->a) return false;
  f->nparts = h_parse_selector(selector, f->parts, (i32)WC_COUNTOF(f->parts));
  if (f->nparts <= 0) return false;
  h_scan(f);
  return true;
}

i32 wc_html_count(str html, const char *selector) {
  HFind f;
  if (!h_prepare(&f, html, selector)) return -1;
  return f.matches;
}

// The number a piece of text is: "$10,038" and "96.3%" both read as numbers, because a
// card that says "ruined 96.3%" is asserting a value and a test should be able to compare
// it without a hand-written scanner. Everything that is not a digit, sign, dot or exponent
// is dropped; a text with no digits at all is NAN, so an empty selector cannot pass.
f64 wc_html_number_at(str html, const char *selector) {
  str t = wc_html_text_at(html, selector);
  if (!t.p || !t.n) return NAN;
  char buf[64];
  size_t o = 0;
  bool seen = false;
  for (size_t i = 0; i < t.n && o + 1 < sizeof buf; i++) {
    char c = t.p[i];
    if ((c >= '0' && c <= '9') || (c == '.' && seen) || (c == '-' && o == 0)) {
      buf[o++] = c;
      seen |= (c >= '0' && c <= '9');
    } else if (c == ',' || c == ' ' || c == '$') {
      continue; // "$1,000.50" is one number, not two
    } else if (seen && o) {
      break; // the first number in the text is the one
    }
  }
  buf[o] = 0;
  if (!seen) return NAN;
  return strtod(buf, nullptr);
}

// The text inside the first match, entities decoded, whitespace collapsed.
str wc_html_text_at(str html, const char *selector) {
  HFind f;
  if (!h_prepare(&f, html, selector) || f.matches == 0) return (str){nullptr, 0};
  str inner = {html.p + f.first_open, f.first_close - f.first_open};
  // strip tags, decode the five named entities, collapse runs of whitespace
  Buf out;
  buf_init(&out, f.a, inner.n + 1);
  for (size_t i = 0; i < inner.n;) {
    if (inner.p[i] == '<') {
      while (i < inner.n && inner.p[i] != '>') i++;
      if (i < inner.n) i++;
      if (out.n && out.p[out.n - 1] != ' ') buf_putc(&out, ' ');
      continue;
    }
    if (inner.p[i] == '&') {
      struct {
        const char *ent;
        char ch;
      } ents[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}};
      bool done = false;
      for (size_t k = 0; k < WC_COUNTOF(ents); k++) {
        size_t el = strlen(ents[k].ent);
        if (i + el <= inner.n && memcmp(inner.p + i, ents[k].ent, el) == 0) {
          buf_putc(&out, ents[k].ch);
          i += el;
          done = true;
          break;
        }
      }
      if (done) continue;
    }
    if (h_is_space(inner.p[i])) {
      if (out.n == 0 || out.p[out.n - 1] != ' ') buf_putc(&out, ' ');
      i++;
      continue;
    }
    buf_putc(&out, inner.p[i++]);
  }
  return h_trim(buf_str(&out));
}

bool wc_html_has(str html, const char *selector) { return wc_html_count(html, selector) > 0; }

// One attribute of the first match. The pair a test wants most often, and the one a
// hand-written scanner gets wrong first: `placeholder="what needs doing?"` is quoted in
// the markup and unquoted in the assertion.
str wc_html_attr_at(str html, const char *selector, const char *name) {
  HFind f;
  if (!h_prepare(&f, html, selector) || f.matches == 0) return (str){nullptr, 0};
  return h_attr(&f.first_el, name);
}

JVal *wc_test_json(Response r) {
  Arena *a = wc_arena_get();
  if (!a) return nullptr;
  JParse jp = json_parse(a, (str){r.body.p, r.body.n});
  return jp.ok ? jp.v : nullptr;
}

// How many items an array at `path` has; -1 when the path is not an array.
i32 wc_json_count_at(str body, const char *path) {
  Arena *a = wc_arena_get();
  if (!a) return -1;
  JParse jp = json_parse(a, body);
  if (!jp.ok) return -1;
  JVal v;
  if (path && *path) {
    if (!wc_json_path(jp.v, path, &v)) return -1;
  } else {
    v = *jp.v;
  }
  return v.kind == J_ARR ? (i32)v.n : -1;
}

bool wc_html_selector_ok(const char *selector) {
  HSel parts[8];
  return h_parse_selector(selector, parts, (i32)WC_COUNTOF(parts)) > 0;
}