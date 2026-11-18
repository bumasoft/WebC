// SPDX-License-Identifier: Apache-2.0
// motion: what a list does when it is not still.
//
// The framework already knows what changed — a patch is a diff, not a re-render — so a
// module that moves can be a *stylesheet plus two attributes*, and no script at all. That is
// the whole point: `motion_head()` writes the keyframes once, and an item says what it does
// when it arrives.
//
//   motion_head();                             // once, in the shell's <head>
//
//   el("li") {
//     enter("rise");                           // data-anim="rise"
//     stagger(i);                              // data-stagger="3" -> 3 × 40ms of delay
//     text(card->title);
//   }
//
// `stagger(0)` is no delay, `stagger(3)` is three steps: one attribute, and a list that
// arrives as a list instead of a wall.
//
// The honest limitation, said here rather than discovered later: this animates an element
// *into* the page. Animating one *out*, or moving a row from position 3 to 1, needs the
// node held for the duration — a DOM decision the bridge owns. A module may add C, never a
// bridge verb (the admission bar), so leave/reorder is not in this file; it is what an
// app that truly wants it adds for itself.
#pragma once

#include <webc/dom.h>

void webc_head_html(str markup); // the route-side door (see theme.h)

typedef struct WMotion {
  const char *name;     // "rise": the data-anim value
  const char *frames;   // the keyframes' body, without the braces
  const char *easing;   // null: cubic-bezier(.2,.7,.3,1)
  i32 ms;               // null ms: 180
} WMotion;

static const WMotion WC_MOTION_DEFAULT[] = {
    {"rise", "from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}",
     "cubic-bezier(.2,.7,.3,1)", 180},
    {"pop", "from{opacity:0;transform:scale(.96)}to{opacity:1;transform:none}",
     "cubic-bezier(.2,.7,.3,1)", 140},
    {"slide", "from{opacity:0;transform:translateX(-8px)}to{opacity:1;transform:none}",
     "cubic-bezier(.2,.7,.3,1)", 200},
    {"flash", "from{background:color-mix(in srgb,var(--acc) 24%,transparent)}to{background:transparent}",
     "linear", 700},
};

// The stylesheet: one animation per name, one delay per stagger step, and the media query
// that switches all of it off for a reader who asked for less motion. That query is not
// optional politeness — it is the reason a motion module is worth having at all, because
// hand-rolled animation is where it is usually forgotten.
static inline void wc_motion_head(const WMotion *motions, i32 n) {
  if (!motions || n <= 0) {
    motions = WC_MOTION_DEFAULT;
    n = WC_ARRLEN(WC_MOTION_DEFAULT);
  }
  Buf *b = sb();
  for (i32 i = 0; i < n; i++) {
    const WMotion *m = &motions[i];
    sb_printf(b, "@keyframes %s{%s}", m->name ? m->name : "rise", m->frames ? m->frames : "");
    sb_printf(b, "[data-anim=%s]{animation:%s %s %dms both}", m->name ? m->name : "rise",
              m->name ? m->name : "rise", m->easing ? m->easing : "cubic-bezier(.2,.7,.3,1)",
              m->ms > 0 ? m->ms : 180);
  }
  for (i32 s = 0; s < 12; s++) sb_printf(b, "[data-stagger=\"%d\"]{animation-delay:%dms}", s, s * 40);
  sb_puts(b, "@media (prefers-reduced-motion:reduce){[data-anim]{animation:none!important}"
             "[data-stagger]{animation-delay:0!important}}");
  el("style") {
    attr("id", "motion");
    markup(sb_cstr(b));
  }
}
#define motion_head() wc_motion_head(nullptr, 0)
#define motion_head_with(motions_) wc_motion_head((motions_), WC_ARRLEN(motions_))

// For a server-rendered page: the same keyframes, into the response's own `<head>`.
static inline void wc_motion_head_html(const WMotion *motions, i32 n) {
  if (!motions || n <= 0) {
    motions = WC_MOTION_DEFAULT;
    n = WC_ARRLEN(WC_MOTION_DEFAULT);
  }
  Buf *b = sb();
  sb_puts(b, "<style id=\"motion\">");
  for (i32 i = 0; i < n; i++) {
    const WMotion *m = &motions[i];
    sb_printf(b, "@keyframes %s{%s}", m->name ? m->name : "rise", m->frames ? m->frames : "");
    sb_printf(b, "[data-anim=%s]{animation:%s %s %dms both}", m->name ? m->name : "rise",
              m->name ? m->name : "rise", m->easing ? m->easing : "cubic-bezier(.2,.7,.3,1)",
              m->ms > 0 ? m->ms : 180);
  }
  for (i32 s = 0; s < 12; s++)
    sb_printf(b, "[data-stagger=\"%d\"]{animation-delay:%dms}", s, s * 40);
  sb_puts(b, "@media (prefers-reduced-motion:reduce){[data-anim]{animation:none!important}"
             "[data-stagger]{animation-delay:0!important}}</style>");
  webc_head_html(sb_str(b));
}
#define motion_head_html() wc_motion_head_html(nullptr, 0)
static inline void wc_enter(const char *name) {
  if (name && *name) wc_attr_c("data-anim", name);
}
#define enter(name_) wc_enter((name_))

static inline void wc_stagger(i32 i) {
  if (i >= 0) wc_attr_i64("data-stagger", i < 12 ? i : 11);
}
#define stagger(i_) wc_stagger((i_))