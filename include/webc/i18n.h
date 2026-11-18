// SPDX-License-Identifier: Apache-2.0
// webc/i18n.h — one app, two languages, and the strings in one place.
//
// The moment a second language arrives, every literal in every view is wrong, and the usual
// fix is a second copy of the views. A catalog is the cheaper answer, and the two things that
// actually need more than a hash map are **plurals** (English has two forms, Polish has four)
// and **direction** (`<html dir="rtl">` changes the layout, not the text).
//
//   static const WMsg EN[] = {
//       MSG("greeting", "Hello, {name}"),
//       MSG("items_one", "{n} item"),
//       MSG("items_other", "{n} items"),
//   };
//   static const WMsg DE[] = {
//       MSG("greeting", "Hallo, {name}"),
//       MSG("items_one", "{n} Ding"),
//       MSG("items_other", "{n} Dinge"),
//   };
//
//   i18n_init(CATS(en_catalog(EN), ltr_catalog("de", DE)));   // once, at boot
//   i18n_use("de");                                 // per request, from the cookie or ?lang
//
//   text(t("greeting", "name", user->name));        // "Hallo, Ada"
//   text(tn("items", 3));                           // "3 Dinge" — the form this language needs
//   attr("dir", lang_dir());                        // "rtl" for ar, he, fa, ur
//
// A catalog is a *sorted* array and the lookup is a binary search: a missing key is a loud
// `??key??` in the page (visible in a test, impossible to ship by accident) rather than an
// empty string, which is the failure mode of every hand-rolled `t()`.
//
// The plural rule that ships is the one the languages people actually write apps in need:
// "one" and "other" (English, German, Dutch, Spanish, Turkish…), "one/few/many/other"
// (Polish, Russian, Ukrainian, Czech), and "other" only (Japanese, Chinese, Korean, Thai).
// That is a table of four integers per language, not ICU.
//
// **Refuses:** a translation management system, ICU MessageFormat (select/selectordinal), a
// catalog in the database, plural *ordinals* ("3rd"), and date/number locales — numbers are
// `fmt_*`, dates are `webc/time.h`.
#pragma once

#include <webc/core.h>
#include <webc/dom.h>

typedef struct WMsg {
  const char *key;
  const char *text; // `{name}` placeholders, `{n}` for the count
} WMsg;

#define MSG(key_, text_) {.key = (key_), .text = (text_)}

// A plural rule: which of the four forms a count takes.
enum { WPL_ONE = 0, WPL_FEW = 1, WPL_MANY = 2, WPL_OTHER = 3, WPL_N = 4 };

typedef struct WCatalog {
  const char *lang;       // "en", "de", "pl"
  const WMsg *msgs;
  i32 n;
  const char *plural;     // "one" (…one/other), "slavic" (one/few/many/other), "none" (other)
  bool rtl;               // the language writes right-to-left
} WCatalog;

// The two helpers that keep a catalog a value instead of a macro soup.
// The three shapes almost every app needs, so the common case names no rule at all.
#define en_catalog(m_) ((WCatalog){"en", (m_), WC_ARRLEN(m_), "one", false})
#define ltr_catalog(lang_, m_) ((WCatalog){(lang_), (m_), WC_ARRLEN(m_), "one", false})
#define slavic_catalog(lang_, m_) ((WCatalog){(lang_), (m_), WC_ARRLEN(m_), "slavic", false})
#define rtl_catalog(lang_, m_) ((WCatalog){(lang_), (m_), WC_ARRLEN(m_), "one", true})
#define catalog(lang_, m_, plural_, rtl_) ((WCatalog){(lang_), (m_), WC_ARRLEN(m_), (plural_), (rtl_)})
#define CATS(...) (WCatalog[]){__VA_ARGS__}, WC_NARG(__VA_ARGS__)

void i18n_reset(void);
void i18n_init(WCatalog *cats, i32 n); // up to four catalogs; the first is the fallback
void i18n_use_text(str lang);              // "" or "xx-YY" -> the "xx" catalog that exists
bool i18n_use(const char *lang);
const char *i18n_lang(void);               // the language in use ("en" when nothing matched)
bool i18n_known(const char *lang);
i32 i18n_count(void);

// The message, with `{name}` substitution. Missing placeholders are left as they are (a
// typo is visible); a missing *key* is `??key??`.
str wc_i18n_get(const char *key, const char **names, const str *values, i32 npairs);
str wc_i18n_plural(const char *key, i64 n, const char **names, const str *values, i32 npairs);

// `t("greeting", "name", user->name)`: one key and then name/value pairs, checked by arity.
#define t(key_, ...) WC_CAT(WC_T_, WC_NARG(__VA_ARGS__))(key_ __VA_OPT__(, __VA_ARGS__))
// `WC_NARG` counts the arguments of `...` with an empty list as one, so both `t("k")` and
// `t("k", "name", v)` land on a macro that exists.
#define WC_T_1(key_) wc_i18n_get((key_), nullptr, nullptr, 0)
#define WC_T_2(key_, n1_, v1_)                                                 \
  wc_i18n_get((key_), (const char *[]){(n1_)}, (const str[]){WC_AS_STR(v1_)}, 1)
#define WC_T_4(key_, n1_, v1_, n2_, v2_)                                       \
  wc_i18n_get((key_), (const char *[]){(n1_), (n2_)},                          \
              (const str[]){WC_AS_STR(v1_), WC_AS_STR(v2_)}, 2)
#define WC_T_6(key_, n1_, v1_, n2_, v2_, n3_, v3_)                              \
  wc_i18n_get((key_), (const char *[]){(n1_), (n2_), (n3_)},                    \
              (const str[]){WC_AS_STR(v1_), WC_AS_STR(v2_), WC_AS_STR(v3_)}, 3)

// `tn("items", 3)` and `tn("items", 3, "who", user->name)`: the count picks the form
// ("items_one" / "items_other"), `{n}` is the count itself.
#define tn(key_, n_, ...)                                                      \
  WC_CAT(WC_TN_, WC_NARG(__VA_ARGS__))((key_), (n_) __VA_OPT__(, __VA_ARGS__))
#define WC_TN_1(key_, n_) wc_i18n_plural((key_), (n_), nullptr, nullptr, 0)
#define WC_TN_2(key_, n_, n1_, v1_)                                            \
  wc_i18n_plural((key_), (n_), (const char *[]){(n1_)}, (const str[]){WC_AS_STR(v1_)}, 1)

// The layout direction of the language in use, for `<html dir=…>`.
const char *lang_dir(void);
const char *lang_dir_of(const char *lang);