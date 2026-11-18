// SPDX-License-Identifier: Apache-2.0
// webc/ui.h — the components layer: the handful of controls every console reaches for,
// with the ARIA threaded. Markup only, no styling opinions beyond class names, and
// nothing here is required: it is an opt-in include, not part of the core headers.
//
// The name is the first argument of each control — the same name `bind()`/`bind_soft()`
// resolves at dispatch time, so a control that exists only sometimes is a soft bind away.
// Every one emits exactly the attributes an app writing it by hand emits today; the
// point is that the ARIA stops being something each author has to remember.
//
//   switch_("toggle_ahead", get(ahead), "Only flights ahead");
//   seg("filt_all", get(filt) == 0, "All", fmt("%lld", n));
//   seg("sort_num", get(sort) == 0, "No.", nullptr);      // a count is optional
//   tab("tab_telemetry", get(tab) == 0, "Telemetry");
//   meter("Fuel", f->fuel, 100, fmt("%lld%%", f->fuel), "m-fuel");
//   kbd("⌘K");
//   class_(classes("row", sel ? "sel" : "", NULL));       // no trailing space, no fmt()
// An absent class is the EMPTY string. A NULL ends the list (the macro appends one), so a
// NULL in the middle silently truncates everything after it — a "dragging" class placed
// after an absent one never reaches the attribute.
#pragma once

#include <webc/dom.h>

// Conditional class names without the format string: the classes that are present,
// space-joined, empty ones skipped. Four rotating buffers, because an author may build
// two in one expression (a parent and a child) but never five.
static inline const char *wc_classes(const char *a, ...) {
  static char bufs[4][256];
  static int which;
  char *out = bufs[which++ & 3];
  size_t n = 0;
  va_list ap;
  va_start(ap, a);
  for (const char *c = a; c; c = va_arg(ap, const char *)) {
    if (!*c) continue;
    size_t l = strlen(c);
    if (n && n + 1 < sizeof bufs[0]) out[n++] = ' ';
    if (n + l >= sizeof bufs[0]) break;
    memcpy(out + n, c, l);
    n += l;
  }
  va_end(ap);
  out[n] = 0;
  return out;
}
#define classes(...) wc_classes(__VA_ARGS__, (const char *)nullptr)

// A switch is a button with a state, not a checkbox: the state lives in a signal, so it
// is drawn from the signal every time rather than carried by the browser.
static inline void wc_switch(const char *hook, bool on, Text label) {
  el("button") {
    class_(on ? "switch on" : "switch");
    type_("button");
    named(hook);
    attr("role", "switch");
    attr("aria-checked", on ? "true" : "false");
    el("i") { class_("track"); }
    if (label) text(label);
  }
}
#define switch_ wc_switch

// One button of a segmented control: on/off state, an optional count on the right.
static inline void wc_seg(const char *hook, bool on, Text label, Text count) {
  el("button") {
    class_(on ? "seg on" : "seg");
    type_("button");
    named(hook);
    attr("aria-pressed", on ? "true" : "false");
    if (label) {
      el("span") { class_("seg-label"); text(label); }
    }
    if (count) {
      el("span") { class_("seg-count"); text(count); }
    }
  }
}
#define seg wc_seg

// One tab of a strip. The strip itself is the app's — its class, its layout, and where
// the panel under it is drawn.
static inline void wc_tab(const char *hook, bool on, Text label) {
  el("button") {
    class_(on ? "tab on" : "tab");
    type_("button");
    named(hook);
    if (label) text(label);
  }
}
#define tab wc_tab

// A labelled bar: the value, the maximum it is measured against, the words to read.
static inline void wc_meter(const char *label, i64 value, i64 max, Text readout,
                            const char *cls) {
  el("div") {
    class_("meter");
    el("div") {
      class_("meter-top");
      el("span") { class_("meter-label"); if (label) text(label); }
      el("span") { class_("meter-value"); if (readout) text(readout); }
    }
    el("div") {
      class_("meter-track");
      el("i") {
        class_(cls ? cls : "");
        style(fmt("width:%lld%%", (long long)(max ? value * 100 / max : 0)));
      }
    }
  }
}
#define meter wc_meter

// A key cap: what to press, where a hint belongs.
static inline void wc_kbd(Text label) {
  el("kbd") { if (label) text(label); }
}
#define kbd wc_kbd

// ---------------------------------------------------------------- the four the rest of
//                                                                 the shelf assumes
//
// A badge, a stat, an empty state and a skeleton: the pieces a page needs the moment it
// has data, and the ones every app redraws slightly differently. As with the controls
// above, the class names are the interface and the colours are the app's — `tone` is a
// word ("ok", "warn", "bad"), never a colour, so the stylesheet decides what it looks
// like and the view says what it *means*.

// A word in a tone. `tone` may be null: a badge with no meaning is just a chip.
static inline void wc_badge(Text label, const char *tone) {
  el("span") {
    class_(classes("badge", tone ? tone : ""));
    if (label) text(label);
  }
}
#define badge wc_badge

// One number that matters, with the words it needs: a label, a value, and an optional
// change. `up` is meaning, not colour — a fall in waiting time is good and a stylesheet
// that only knows "up/down" cannot say so, so the tone is spelled: pass "good" or "bad".
static inline void wc_stat(Text label, Text value, Text delta, const char *tone) {
  el("div") {
    class_("stat");
    el("span") {
      class_("stat-label");
      if (label) text(label);
    }
    el("strong") {
      class_("stat-value");
      if (value) text(value);
    }
    if (delta && *delta) {
      el("span") {
        class_(classes("stat-delta", tone ? tone : ""));
        if (delta) text(delta);
      }
    }
  }
}
#define stat wc_stat

// Nothing here yet, and what to do about it. The action is a hook name, so the empty
// state's button is wired the same way every other button is — and it is omitted when
// the app has nothing to offer, which is the case a hand-written "No items. Add one!"
// always gets wrong.
static inline void wc_empty(Text title, Text body, const char *hook, Text action) {
  el("div") {
    class_("empty");
    attr("role", "status");
    el("p") {
      class_("empty-title");
      if (title) text(title);
    }
    if (body && *body) {
      el("p") {
        class_("empty-body");
        text(body);
      }
    }
    if (hook && *hook) {
      el("button") {
        class_("empty-action");
        type_("button");
        named(hook);
        text(action && *action ? action : title);
      }
    }
  }
}
#define empty_state wc_empty

// A placeholder with the shape of what is coming: `lines` rows of it. `kind` is "text",
// "card" or "row"; the count is what makes a skeleton honest, because a three-line
// skeleton for a forty-row table is a lie the layout tells.
static inline void wc_skeleton(const char *kind, i32 lines) {
  el("div") {
    class_(classes("skel", kind && *kind ? fmt("skel-%s", kind) : ""));
    attr("aria-hidden", "true");
    for (i32 i = 0; i < (lines > 0 ? lines : 1); i++) el("i");
  }
}
#define skeleton wc_skeleton

// An icon-only control still needs a name; the ARIA lives here so it cannot be forgotten
// in the one place it is easiest to forget.
static inline void wc_icon_button(const char *hook, Text label) {
  el("button") {
    class_("icon-btn");
    type_("button");
    named(hook);
    wc_attr("aria-label", WC_AS_STR(label));
  }
}
#define icon_button wc_icon_button
