// SPDX-License-Identifier: Apache-2.0
// webc/overlay.h — the things that sit on top: a dialog, a drawer, a menu, a tip, a toast.
//
// What makes an overlay an overlay is not the CSS, it is five details that are invisible
// until one is missing: the `role`, the `aria-modal`, a name, focus that moves into it, and
// a way out that does not need a mouse. This file is those five details, once.
//
//   static const WOverlay CARD = {"card_dlg", "Card", "card_close", "card_title"};
//
//   dialog(&CARD, open) {
//     el("h2") { text("New card"); }
//     el("button") { class_("x"); named("card_close"); aria_label("Close"); }
//   }
//
// `open` is the app's own signal. While it is true the box is rendered, focused (so a
// screen reader announces it and the keys land in the right place), and marked with the
// hook `CARD.id`; while it is false nothing is emitted at all, which is what keeps a closed
// dialog out of the tab order and out of the accessibility tree.
//
// The way out has two halves, and both are one line in the app because the framework
// cannot invent its action:
//
//   bind(CARD.close, "click", card_close);   // client_main: the scrim and the close button
//   overlay_esc(card_close);                 // the Escape key, wherever focus is
//
// A `drawer` is the same contract with a different class, because the difference between a
// dialog and a drawer is where the box sits — a stylesheet's business, not a view's.
#pragma once

#include <webc/dom.h>
#include <webc/host.h>
#include <webc/ui.h> // classes(): one spelling for a conditional class list

typedef struct WOverlay {
  const char *id;    // the panel's id, and the name the box is known by
  const char *label; // its accessible name
  const char *close; // optional hook the scrim is named after (bound to the app's close)
  // Optional: the id of the field the caret belongs in. Left null the panel itself takes the
  // focus, which is right for a box full of controls and wrong for a box that is one field —
  // there the first keystroke would go nowhere. The hook is the panel's id still.
  const char *focus;
} WOverlay;

typedef struct WOverlayScope {
  WDom *panel;        // the backdrop is emitted and closed in the same breath: nothing to keep
  bool open;
} WOverlayScope;

// The scrim and the panel are opened here and popped when the loop ends, so the app's body
// is the panel's content and nothing else: no closing tag to forget, no nesting to count.
// The backdrop and the panel are **siblings**, not parent and child. A scrim that wraps the
// panel cannot tell a click outside from a click inside: the bridge attributes every event in
// a hook's subtree to that hook (an innermost hook wins), so a submit button inside a dialog
// closed the dialog instead of submitting it. `examples/board` found it the first time a
// dialog held a form; no test could, because the tests read markup and this is a click.
//
// So: one `data-hook` per node, and the panel is a boundary — nothing inside it belongs to
// the backdrop. The panel carries an `id` (the focus target and the name the box is known by)
// and no hook at all.
static inline WOverlayScope wc_overlay_open_(const WOverlay *o, bool open, const char *cls) {
  WOverlayScope s = {0};
  if (!open) return s;
  WDom *backdrop = wc_el_open("div");
  wc_class(str_lit("scrim"));
  wc_attr_c("aria-hidden", "true");
  if (o->close && *o->close) wc_hook(o->close);
  wc_el_pop(&backdrop);
  s.panel = wc_el_open("div");
  wc_class(WC_AS_STR(classes("panel", cls ? cls : "")));
  wc_attr_c("role", "dialog");
  wc_attr_c("aria-modal", "true");
  if (o->label && *o->label) wc_attr_c("aria-label", o->label);
  wc_attr_c("tabindex", "-1");
  if (o->id && *o->id) wc_attr_c("id", o->id);
  // Focus moves after the batch, when the node exists — the one ordering rule an app
  // writing this by hand gets wrong, and the reason a hand-written dialog opens with the
  // caret still in the field behind it.
  const char *to = o->focus && *o->focus ? o->focus : o->id;
  if (to && *to) focus_hook(to);
  s.open = true;
  return s;
}

static inline void wc_overlay_close_(WOverlayScope *s) {
  if (!s->open) return;
  wc_el_pop(&s->panel);
  s->open = false;
}

#define WC_OVERLAY_(C_, o_, open_, cls_)                                       \
  for (WOverlayScope WC_CAT(wc_ov_, C_) __attribute__((cleanup(wc_overlay_close_))) = \
           wc_overlay_open_((o_), (open_), (cls_));                            \
       WC_CAT(wc_ov_, C_).open; WC_CAT(wc_ov_, C_).open = false)

// A modal box. Emitted only while it is open.
#define dialog(o_, open_) WC_OVERLAY_(__COUNTER__, (o_), (open_), nullptr)
// The same box, anchored to the edge: a drawer is a dialog whose panel the stylesheet
// moves. The contract — role, name, focus, scrim — is identical, so nothing here can drift
// from a dialog.
#define drawer(o_, open_) WC_OVERLAY_(__COUNTER__, (o_), (open_), "drawer")

// The Escape key. One key spec, the app's own close action: a framework that guessed which
// dialog was open would be keeping state it promised not to keep.
#define overlay_esc(N_) on_key("escape", N_)

// A scope that is one element plus its attributes: opened by the macro, popped when the
// loop ends. Every wrapper in this file is this, so a module never counts closing braces.
typedef struct WOverlayEl {
  WDom *el;
  bool open;
} WOverlayEl;

static inline WOverlayEl wc_scope_open_(const char *tag, const char *cls, const char *role) {
  WOverlayEl s = {0};
  s.el = wc_el_open(tag);
  if (cls) wc_class(str_lit(cls));
  if (role) wc_attr_c("role", role);
  s.open = true;
  return s;
}

static inline void wc_scope_close_(WOverlayEl *s) {
  if (!s->open) return;
  wc_el_pop(&s->el);
  s->open = false;
}

#define WC_SCOPE_(C_, tag_, cls_, role_)                                       \
  for (WOverlayEl WC_CAT(wc_sc_, C_) __attribute__((cleanup(wc_scope_close_))) =    \
           wc_scope_open_((tag_), (cls_), (role_));                             \
       WC_CAT(wc_sc_, C_).open; WC_CAT(wc_sc_, C_).open = false)

// A menu: one list, shown or not, with the ARIA that makes a screen reader call it a menu
// rather than a bulleted list.
#define menu(o_) WC_SCOPE_(__COUNTER__, "div", "menu", "menu")

// One row of a menu, or of any list that behaves like one: the role is what tells a screen
// reader it can be activated, and the hook is what makes activating it do something.
static inline void wc_menu_item(const char *hook, Text label, bool off) {
  el("button") {
    class_(off ? "menu-item off" : "menu-item");
    type_("button");
    attr("role", "menuitem");
    if (off) attr("aria-disabled", "true");
    else if (hook && *hook) named(hook);
    if (label) text(label);
  }
}
#define menu_item wc_menu_item

// A toast is a sentence that arrives on its own, so it is announced: `role="status"` inside
// a live region is what makes a screen reader read it without being asked. The app decides
// how long it stays (a `webc_later` in the action that set it) and what it says.
static inline void wc_toast(Text msg, const char *tone) {
  el("div") {
    class_(classes("toast", tone ? tone : ""));
    attr("role", "status");
    if (msg) text(msg);
  }
}
#define toast wc_toast

// The wrapper a page needs once, so its toasts stack instead of fighting for the corner.
static inline WOverlayEl wc_toasts_open_(const char *cls) {
  WOverlayEl s = wc_scope_open_("div", classes("toasts", cls ? cls : ""), nullptr);
  wc_attr_c("aria-live", "polite");
  return s;
}
// `toasts("wide") { … }`: the class is the app's, the live region is the module's.
#define toasts(cls_) WC_TOASTS_(__COUNTER__, (cls_))
#define WC_TOASTS_(C_, cls_)                                                   \
  for (WOverlayEl WC_CAT(wc_ts_, C_) __attribute__((cleanup(wc_scope_close_))) = \
           wc_toasts_open_((cls_));                                            \
       WC_CAT(wc_ts_, C_).open; WC_CAT(wc_ts_, C_).open = false)

// An inline tip: the element wraps its own trigger, so the hint is anchored to the thing it
// describes and the app never positions it by hand.
//
//   tip("Scores reset at midnight") { el("span") { text("ⓘ"); } }
//
// `tabindex` is there so the hint is reachable without a mouse — the reason a CSS-only
// tooltip is unusable, and the thing every app learns after shipping one.
static inline WOverlayEl wc_tip_open_(Text text) {
  WOverlayEl s = wc_scope_open_("span", "tip", nullptr);
  wc_attr("data-tip", text ? WC_AS_STR(text) : str_lit(""));
  wc_attr_c("tabindex", "0");
  return s;
}

#define tip(text_) WC_TIP_(__COUNTER__, (text_))
// the tip's scope is a WOverlayEl like every other wrapper here
#define WC_TIP_(C_, text_)                                                     \
  for (WOverlayEl WC_CAT(wc_tp_, C_) __attribute__((cleanup(wc_scope_close_))) =    \
           wc_tip_open_((text_));                                              \
       WC_CAT(wc_tp_, C_).open; WC_CAT(wc_tp_, C_).open = false)
