// SPDX-License-Identifier: Apache-2.0
// WebC — the shared half of URLs and page text: route patterns, links, dates, headings,
// slugs. Target-safe on purpose: the same code answers on the server and in the browser,
// which is the whole reason it is not in http.c.
#include <webc/url.h>
#include <webc/log.h>

// ---------------------------------------------------------------- the pattern table
//
// A fixed table rather than a heap one: this is initialised from constructors, before
// any arena exists, and a page has tens of routes, not thousands.
#define WC_URL_MAX 64
typedef struct UrlEntry {
  const char *name;
  const char *pattern;
} UrlEntry;

static UrlEntry g_urls[WC_URL_MAX];
static i32 g_nurls;

void wc_url_register(const char *name, const char *pattern) {
  if (!name || !pattern) return;
  for (i32 i = 0; i < g_nurls; i++) {
    if (strcmp(g_urls[i].name, name) != 0) continue;
    if (strcmp(g_urls[i].pattern, pattern) == 0) return; // the same fact, said twice
    panic("route \"%s\" is declared twice, as \"%s\" and as \"%s\". One pattern per name: "
          "rename one, or drop the shared ROUTE() line and let the server's "
          "GET(\"…\", \"%s\") name it.",
          name, g_urls[i].pattern, pattern, name);
  }
  if (g_nurls >= WC_URL_MAX) {
    wc_limit_once(&g_nurls, "route name dropped: too many named routes", "WC_URL_MAX", "route");
    return;
  }
  g_urls[g_nurls++] = (UrlEntry){name, pattern};
}

i32 wc_url_count(void) {
  return g_nurls;
}

const char *wc_url_pattern(const char *name) {
  if (!name) return nullptr;
  for (i32 i = 0; i < g_nurls; i++)
    if (strcmp(g_urls[i].name, name) == 0) return g_urls[i].pattern;
  return nullptr;
}

// ---------------------------------------------------------------- expansion

// Reserved characters in a path segment (RFC 3986 §3.3) plus the ones that would end
// the segment: a slug is data, and data does not get to choose where the URL points.
static bool needs_encoding(u8 c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return false;
  switch (c) {
  case '-': case '_': case '.': case '~': return false;
  default: return true;
  }
}

static void url_put(Buf *b, str v, bool query) {
  for (size_t i = 0; i < v.n; i++) {
    u8 c = (u8)v.p[i];
    bool keep = query ? ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
                      : !needs_encoding(c);
    if (keep) {
      buf_putc(b, (char)c);
      continue;
    }
    static const char *hex = "0123456789ABCDEF";
    buf_putc(b, '%');
    buf_putc(b, hex[(c >> 4) & 0xf]);
    buf_putc(b, hex[c & 0xf]);
  }
}

str wc_url_from(Arena *a, const char *pattern, const str *kv, i32 npairs, const char *what) {
  if (!pattern) return (str){"", 0};
  Buf b;
  buf_init(&b, a, 64);
  const char *p = pattern;
  while (*p) {
    if (*p != ':') {
      buf_putc(&b, *p++);
      continue;
    }
    const char *start = ++p;
    while (*p && *p != '/' && *p != '.' && *p != '?') p++;
    str key = {(char *)start, (size_t)(p - start)};
    str val = {nullptr, 0};
    bool found = false;
    for (i32 i = 0; i + 1 < npairs * 2; i += 2)
      if (str_eq(kv[i], key)) {
        val = kv[i + 1];
        found = true;
        break;
      }
    if (!found) {
      // A missing parameter is a link that would go to `/a/:slug` — a 404 a reader
      // finds, hours later. Say which name wanted which parameter.
      Buf have;
      buf_init(&have, a, 32);
      for (i32 i = 0; i + 1 < npairs * 2; i += 2)
        buf_printf(&have, "%.*s=… ", (int)kv[i].n, kv[i].p);
      panic("%s: \"%s\" wants '%.*s' and was not given it (got: %s)", what, pattern,
            (int)key.n, key.p, have.n ? have.p : "nothing");
    }
    url_put(&b, val, false);
  }
  return buf_str(&b);
}

str wc_url_for(const char *name, const str *kv, i32 npairs) {
  const char *pattern = wc_url_pattern(name);
  if (!pattern)
    panic("url_to(\"%s\"): no route has that name (%d are named). Name a route with "
          "GET(\"/a/:slug\", \"%s\") on the server, or declare it where both halves can see "
          "it: ROUTE(\"%s\", \"/a/:slug\").",
          name, g_nurls, name, name);
  return wc_url_from(wc_arena_get(), pattern, kv, npairs, "url_to");
}

str wc_link(const WModel *m, const void *obj) {
  if (!m || !obj) return (str){"", 0};
  if (!m->route)
    panic("link_to(%s, …): the model names no route. Add WEBC_MODEL_ROUTE(%s, \"/things/:slug\")",
          m->name, m->name);
  str kv[16];
  i32 n = 0;
  const char *p = m->route;
  while (*p && n + 1 < (i32)WC_COUNTOF(kv)) {
    if (*p != ':') {
      p++;
      continue;
    }
    const char *start = ++p;
    while (*p && *p != '/' && *p != '.') p++;
    str key = {(char *)start, (size_t)(p - start)};
    const WField *f = model_field(m, key);
    if (!f)
      panic("link_to(%s, …): the route wants '%.*s', which is not a field of %s", m->name,
            (int)key.n, key.p, m->name);
    kv[n++] = key;
    kv[n++] = WC_KIND_IS_TEXT(f->kind) || f->kind == WK_BYTES
                  ? wfield_get_str(f, obj)
                  : str_from_i64(wc_arena_get(), wfield_get_i64(f, obj));
  }
  return wc_url_from(wc_arena_get(), m->route, kv, n / 2, "link_to");
}

// ---------------------------------------------------------------- figures

// "how long is this going to take me" — 220 words a minute, rounded up, never zero, which
// is the number every reading app shows and every app computes slightly differently.
i32 wc_read_minutes(str body) {
  if (!body.p || body.n == 0) return 1;
  i32 words = 0;
  bool in_word = false;
  for (size_t i = 0; i < body.n; i++) {
    char c = body.p[i];
    bool space = c == ' ' || c == '\n' || c == '\t' || c == '\r';
    if (!space && !in_word) {
      words++;
      in_word = true;
    } else if (space) {
      in_word = false;
    }
  }
  i32 mins = (words + 219) / 220;
  return mins < 1 ? 1 : mins;
}

str wc_read_time(str body) {
  i32 mins = wc_read_minutes(body);
  Buf b;
  buf_init(&b, wc_arena_get(), 16);
  buf_printf(&b, "%d min read", (int)mins);
  return buf_str(&b);
}

// A slug is a name with the spaces taken out: `ada-lovelace` is `Ada Lovelace`, and a desk
// called `science-nature` is `Science Nature`. Every app writes this loop; none of them
// agree about capitalisation.
str wc_name_from_slug(str slug) {
  Buf b;
  buf_init(&b, wc_arena_get(), slug.n ? slug.n + 1 : 8);
  bool cap = true;
  for (size_t i = 0; i < slug.n; i++) {
    char c = slug.p[i];
    if (c == '-' || c == '_') {
      buf_putc(&b, ' ');
      cap = true;
      continue;
    }
    if (cap && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    cap = false;
    buf_putc(&b, c);
  }
  return buf_str(&b);
}

// Case-insensitive substring, ASCII only — which is what a search box over an English
// archive needs and what `str_contains` (exact) does not do.
bool wc_contains_ci(str hay, str needle) {
  if (needle.n == 0) return true;
  if (!hay.p || hay.n < needle.n) return false;
  for (size_t i = 0; i + needle.n <= hay.n; i++) {
    size_t k = 0;
    while (k < needle.n) {
      char a = hay.p[i + k], b = needle.p[k];
      if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
      if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
      if (a != b) break;
      k++;
    }
    if (k == needle.n) return true;
  }
  return false;
}

// ---------------------------------------------------------------- headings and slugs

static bool word_start(str s, size_t i) {
  if (i == 0) return true;
  char prev = s.p[i - 1];
  return prev == ' ' || prev == '/' || prev == '-' || prev == '&' || prev == '\t' || prev == '\n';
}

str wc_title_case(str s) {
  Buf b;
  buf_init(&b, wc_arena_get(), s.n ? s.n : 8);
  for (size_t i = 0; i < s.n; i++) {
    char c = s.p[i];
    if (word_start(s, i) && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    else if (!word_start(s, i) && c >= 'A' && c <= 'Z' && i > 0 && s.p[i - 1] != ' ')
      c = (char)(c - 'A' + 'a');
    buf_putc(&b, c);
  }
  return buf_str(&b);
}

// "What the tide left behind" → "what-the-tide-left-behind". ASCII only on purpose: a
// slug with an accent is a URL that gets mangled by every mail client, so an accented
// byte becomes a dash and the rest of the word survives.
str wc_slugify(str s) {
  Buf b;
  buf_init(&b, wc_arena_get(), s.n ? s.n : 8);
  bool dash = false;
  for (size_t i = 0; i < s.n; i++) {
    u8 c = (u8)s.p[i];
    if (c >= 'A' && c <= 'Z') c = (u8)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      buf_putc(&b, (char)c);
      dash = false;
      continue;
    }
    if (!dash && b.n) {
      buf_putc(&b, '-');
      dash = true;
    }
  }
  while (b.n && b.p[b.n - 1] == '-') b.n--;
  return buf_str(&b);
}