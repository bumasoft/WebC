// SPDX-License-Identifier: Apache-2.0
// webc/i18n.h — catalogs, the plural table, and the lookup. Four catalogs is the ceiling
// on purpose: an app with more than four languages wants a real translation pipeline, and a
// fixed table is what keeps this a header-only module with no allocation and no registry.
#include <webc/i18n.h>
#include <webc/model.h>

typedef struct {
  WCatalog cat;
  bool used;
} Slot;

static Slot g_slots[4];
static i32 g_active;      // index into g_slots, 0 = the fallback
static const WCatalog *g_cat(int i) { return &g_slots[i].cat; }

void i18n_reset(void) {
  memset(g_slots, 0, sizeof g_slots);
  g_active = 0;
}

static void copy_cat(WCatalog *dst, WCatalog src) {
  *dst = src;
  if (!dst->lang || !*dst->lang) dst->lang = "en";
  if (!dst->plural || !*dst->plural) dst->plural = "one";
}

// An array of catalogs, four at most. The first is the fallback, which is why adding a third
// language does not disturb the two an app already had.
void i18n_init(WCatalog *cats, i32 n) {
  i18n_reset();
  if (!cats || n <= 0) return;
  for (i32 i = 0; i < n && i < 4; i++) {
    if (!cats[i].lang || !*cats[i].lang) break;
    copy_cat(&g_slots[i].cat, cats[i]);
    g_slots[i].used = true;
  }
  g_active = 0;
}

bool i18n_known(const char *lang) {
  if (!lang || !*lang) return false;
  for (i32 i = 0; i < 4; i++) {
    if (!g_slots[i].used) continue;
    // "de-AT" answers to "de": the region is a detail of the text, not of the language
    const char *have = g_cat(i)->lang;
    size_t n = strlen(have);
    if (strncmp(lang, have, n) == 0 && (lang[n] == 0 || lang[n] == '-' || lang[n] == '_'))
      return true;
  }
  return false;
}

bool i18n_use(const char *lang) {
  if (!lang || !*lang) return false;
  for (i32 i = 0; i < 4; i++) {
    if (!g_slots[i].used) continue;
    const char *have = g_cat(i)->lang;
    size_t n = strlen(have);
    if (strncmp(lang, have, n) == 0 && (lang[n] == 0 || lang[n] == '-' || lang[n] == '_')) {
      g_active = i;
      return true;
    }
  }
  return false;
}

void i18n_use_text(str lang) {
  if (!lang.p || !lang.n) return;
  char buf[24];
  size_t n = lang.n < sizeof buf - 1 ? lang.n : sizeof buf - 1;
  memcpy(buf, lang.p, n);
  buf[n] = 0;
  i18n_use(buf);
}

const char *i18n_lang(void) { return g_slots[0].used ? g_cat(g_active)->lang : "en"; }
i32 i18n_count(void) {
  i32 n = 0;
  for (i32 i = 0; i < 4; i++) n += g_slots[i].used;
  return n;
}

// The lookup: binary search over a sorted catalog, then the fallback catalog, then the loud
// `??key??`. A missing key must be visible — an empty string is a page that silently says
// nothing, which is the bug every home-grown `t()` ships once.
static const char *find_in(const WCatalog *c, const char *key) {
  i32 lo = 0, hi = c->n - 1;
  while (lo <= hi) {
    i32 mid = lo + (hi - lo) / 2;
    int cmp = strcmp(c->msgs[mid].key, key);
    if (cmp == 0) return c->msgs[mid].text;
    if (cmp < 0) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}

static const char *lookup(const char *key) {
  if (!key || !*key) return "";
  if (g_slots[g_active].used) {
    const char *t = find_in(g_cat(g_active), key);
    if (t) return t;
  }
  if (g_slots[0].used) {
    const char *t = find_in(g_cat(0), key);
    if (t) return t;
  }
  return nullptr;
}

// `{name}` substitution in one pass: the format is a lookup, not a printf, because a
// translated string may reorder its placeholders and a `%s` order cannot.
static str subst(const char *fmt, const char *const *names, const str *values, i32 npairs,
                 bool with_n, i64 n) {
  Buf b;
  buf_init(&b, wc_arena_get(), 64);
  for (const char *p = fmt; *p;) {
    if (*p != '{') {
      buf_putc(&b, *p++);
      continue;
    }
    const char *end = strchr(p, '}');
    if (!end) { // an unclosed brace is copied, not swallowed
      buf_putc(&b, *p++);
      continue;
    }
    str name = {.p = p + 1, .n = (size_t)(end - p - 1)};
    bool done = false;
    if (with_n && name.n == 1 && name.p[0] == 'n') {
      buf_printf(&b, "%lld", (long long)n);
      done = true;
    }
    for (i32 i = 0; i < npairs && !done; i++) {
      if (strlen(names[i]) == name.n && memcmp(names[i], name.p, name.n) == 0) {
        buf_putstr(&b, values[i]);
        done = true;
      }
    }
    if (!done) buf_printf(&b, "%.*s", (int)(end - p + 1), p); // left as written: visible
    p = end + 1;
  }
  return buf_str(&b);
}

str wc_i18n_get(const char *key, const char **names, const str *values, i32 npairs) {
  const char *fmt = lookup(key);
  if (!fmt) {
    Buf b;
    buf_init(&b, wc_arena_get(), 32);
    buf_printf(&b, "??%s??", key ? key : "");
    return buf_str(&b);
  }
  return subst(fmt, names, values, npairs, false, 0);
}

// The plural form a count takes, per the catalog's rule table.
static i32 plural_index(const WCatalog *c, i64 n) {
  if (!c || !c->plural) return WPL_OTHER;
  if (strcmp(c->plural, "none") == 0) return WPL_OTHER;
  if (strcmp(c->plural, "slavic") == 0) {
    i64 n10 = n % 10, n100 = n % 100;
    if (n10 == 1 && n100 != 11) return WPL_ONE;
    if (n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14)) return WPL_FEW;
    return WPL_MANY;
  }
  return n == 1 ? WPL_ONE : WPL_OTHER;
}

static const char *plural_key(const char *key, i32 form) {
  static const char *suffix[WPL_N] = {"_one", "_few", "_many", "_other"};
  Buf b;
  buf_init(&b, wc_arena_get(), 48);
  buf_puts(&b, key);
  buf_puts(&b, suffix[form]);
  return buf_cstr(&b);
}

str wc_i18n_plural(const char *key, i64 n, const char **names, const str *values, i32 npairs) {
  const WCatalog *c = g_slots[g_active].used ? g_cat(g_active) : g_cat(0);
  i32 form = plural_index(c, n);
  const char *fmt = lookup(plural_key(key, form));
  // a language that has no "few" form falls back to "other", and only then to the base key
  if (!fmt && form != WPL_OTHER) fmt = lookup(plural_key(key, WPL_OTHER));
  if (!fmt) fmt = lookup(key);
  if (!fmt) {
    Buf b;
    buf_init(&b, wc_arena_get(), 32);
    buf_printf(&b, "??%s??", key ? key : "");
    return buf_str(&b);
  }
  return subst(fmt, names, values, npairs, true, n);
}

// ---------------------------------------------------------------- direction

const char *lang_dir_of(const char *lang) {
  static const char *rtl[] = {"ar", "he", "fa", "ur", "ps", "sd", "ug", "yi", "dv", "ku"};
  if (!lang || !*lang) return "ltr";
  for (size_t i = 0; i < sizeof rtl / sizeof rtl[0]; i++) {
    size_t n = strlen(rtl[i]);
    if (strncmp(lang, rtl[i], n) == 0 && (lang[n] == 0 || lang[n] == '-' || lang[n] == '_'))
      return "rtl";
  }
  return "ltr";
}

const char *lang_dir(void) { return lang_dir_of(i18n_lang()); }
