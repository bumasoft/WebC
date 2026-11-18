// SPDX-License-Identifier: Apache-2.0
// webc/theme.h — the tokens a page paints with, and the dark half of them.
//
// One table, two columns: every token says what it is in the light edition and what it
// is in the dark one. `theme_head()` writes the three declarations a flash-free dark mode
// actually needs —
//
//   :root{…light tokens…; color-scheme:light}
//   :root[data-theme=dark]{…dark tokens…; color-scheme:dark}
//   @media (prefers-color-scheme:dark){ :root:not([data-theme=light]){…dark tokens…} }
//
// — so the first paint is right whether the reader's OS says dark, their cookie says
// light, or neither has an opinion. The attribute is the override; the media query is the
// default. That order is the whole trick, and every app was writing it by hand.
//
//   static const WToken BR_TOKENS[] = {
//       {"bg", "#fbfaf7", "#12130f"},
//       {"fg", "#1a1a18", "#eceae4"},
//       {"acc", "#3d4ee0", "#8b96ff"},
//       {"line", "#e3e0d8", "#2a2c26"},
//   };
//   static const WTheme BR = {"bankroll", BR_TOKENS, WC_ARRLEN(BR_TOKENS), "Inter, system-ui",
//                             "ui-monospace, monospace"};
//   // in the shell's <head>:
//   theme_head(&BR);
//
// The tokens become `--bg`, `--fg`, `--acc`, `--line`: CSS custom properties on `:root`,
// used as `color: var(--fg)`. There is no colour in this file, only the reader's ability
// to name one.
//
// The control that flips the edition is not here, because it is not a second kind of
// control: it is `switch_("theme", dark, "Dark mode")` from ui.h, and the action it names
// is `theme_paint()` plus the app's own `POST /theme` for the cookie.
#pragma once

#include <webc/host.h>

// The route-side door into a page's <head> (include/webc/http.h). Declared, not included:
// this header is included by browser halves too, and a client module has no response head
// to write into — the declaration costs nothing and the call is the server's decision.
void webc_head_html(str markup);

typedef struct WToken {
  const char *name;  // the custom property, without the leading "--"
  const char *light; // the light edition's value ("#fbfaf7", "12px", "1.5")
  const char *dark;  // the dark edition's; null keeps the light value
} WToken;

typedef struct WTheme {
  const char *name;   // the value of <html data-theme="…"> this theme answers to
  const WToken *tokens;
  i32 n;
  const char *sans;   // optional font stacks -> --font-sans / --font-mono
  const char *mono;
} WTheme;

static inline void wc_theme_block(Buf *b, const WTheme *t, bool dark) {
  for (i32 i = 0; i < t->n; i++) {
    const char *v = dark && t->tokens[i].dark ? t->tokens[i].dark : t->tokens[i].light;
    if (!v) continue;
    if (dark && !t->tokens[i].dark) continue;
    sb_printf(b, ";--%s:%s", t->tokens[i].name, v);
  }
}

static inline void wc_theme_head(const WTheme *t) {
  if (!t) return;
  Buf *b = sb();
  sb_puts(b, ":root{color-scheme:light");
  wc_theme_block(b, t, false);
  if (t->sans) sb_printf(b, ";--font-sans:%s", t->sans);
  if (t->mono) sb_printf(b, ";--font-mono:%s", t->mono);
  sb_puts(b, "}");
  sb_puts(b, ":root[data-theme=dark]{color-scheme:dark");
  wc_theme_block(b, t, true);
  sb_puts(b, "}");
  sb_puts(b, "@media (prefers-color-scheme:dark){:root:not([data-theme=light]){color-scheme:dark");
  wc_theme_block(b, t, true);
  sb_puts(b, "}}");
  el("style") {
    attr("id", "theme");
    markup(sb_cstr(b));
  }
}
#define theme_head(t_) wc_theme_head((t_))

// The same declarations for a *route*: a server-rendered page's `<head>` belongs to the
// shell, and `head_html()` is the door into it. One builder, two places a head can be.
//
//   GET("/") { theme_head_html(&BR); return page(Board, &ctx); }
static inline void wc_theme_head_html(const WTheme *t) {
  if (!t) return;
  Buf *b = sb();
  sb_puts(b, "<style id=\"theme\">");
  sb_puts(b, ":root{color-scheme:light");
  wc_theme_block(b, t, false);
  if (t->sans) sb_printf(b, ";--font-sans:%s", t->sans);
  if (t->mono) sb_printf(b, ";--font-mono:%s", t->mono);
  sb_puts(b, "}");
  sb_puts(b, ":root[data-theme=dark]{color-scheme:dark");
  wc_theme_block(b, t, true);
  sb_puts(b, "}");
  sb_puts(b, "@media (prefers-color-scheme:dark){:root:not([data-theme=light]){color-scheme:dark");
  wc_theme_block(b, t, true);
  sb_puts(b, "}}</style>");
  webc_head_html(sb_str(b));
}
#define theme_head_html(t_) wc_theme_head_html((t_))

// What the switch's action calls. It is one host verb, not a cookie jar: the edition the
// *server* should render on the next request is the app's own form post, because a server
// that reads its theme from a cookie it never received is a server guessing.
//
//   action(theme_flip) { bool dark = !get(dark); set(dark, dark); theme_paint(dark); }
static inline void wc_theme_paint(bool dark) {
  doc_attr("data-theme", str_lit(dark ? "dark" : "light"));
}
#define theme_paint(dark_) wc_theme_paint((dark_))

// The edition this document was rendered in, for the half that has to agree with the
// server: `page_env().dark`. It is the bridge's own answer, not a second source of truth.
static inline bool wc_theme_is_dark(void) {
  WClientEnv e = page_env();
  return e.dark;
}