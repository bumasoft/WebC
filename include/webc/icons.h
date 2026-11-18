// SPDX-License-Identifier: Apache-2.0
// webc/icons.h — a stroke icon, as a datum you include or do not.
//
// Every icon is its own symbol, so the linker drops the ones a page never draws: an app
// that uses three icons pays for three paths, not for the set. That is the whole reason
// this is a header of constants and not an icon font or a JSON manifest.
//
//   icon(WC_ICON_PLUS, 16);          // 16 pixels, currentColor, aria-hidden
//   icon(WC_ICON_SEARCH, 0);         // size 0: CSS decides (width/height omitted)
//
// The grid is 24×24, the stroke is 1.75, and nothing is filled: an icon inherits the text
// colour of wherever it sits, which is the only colour opinion this file is allowed to
// have. A button that is only an icon still needs a name, and that is `aria_label()`:
//
//   el("button") { named("close"); aria_label("Close"); icon(WC_ICON_X, 18); }
#pragma once

#include <webc/dom.h>

#define WC_ICON_PLUS "M12 5v14M5 12h14"
#define WC_ICON_MINUS "M5 12h14"
#define WC_ICON_X "M18 6 6 18M6 6l12 12"
#define WC_ICON_CHECK "M20 6 9 17l-5-5"
#define WC_ICON_CHEVRON_DOWN "m6 9 6 6 6-6"
#define WC_ICON_CHEVRON_UP "m18 15-6-6-6 6"
#define WC_ICON_CHEVRON_LEFT "m15 18-6-6 6-6"
#define WC_ICON_CHEVRON_RIGHT "m9 18 6-6-6-6"
#define WC_ICON_ARROW_UP "M12 19V5M6 11l6-6 6 6"
#define WC_ICON_ARROW_DOWN "M12 5v14M18 13l-6 6-6-6"
#define WC_ICON_ARROW_LEFT "M19 12H5M11 18l-6-6 6-6"
#define WC_ICON_ARROW_RIGHT "M5 12h14M13 6l6 6-6 6"
#define WC_ICON_SORT "M8 20V4M4 8l4-4 4 4M16 4v16M12 16l4 4 4-4"
#define WC_ICON_SEARCH "M11 4a7 7 0 1 0 0 14 7 7 0 0 0 0-14M20.5 20.5 16 16"
#define WC_ICON_MENU "M4 7h16M4 12h16M4 17h16"
#define WC_ICON_MORE "M5 12h.01M12 12h.01M19 12h.01"
#define WC_ICON_FILTER "M3 5h18l-7 8v6l-4 2v-8z"
#define WC_ICON_TRASH "M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"
#define WC_ICON_PENCIL "m4 20 1-4L16 5l3 3L8 19zM14 7l3 3"
#define WC_ICON_COPY "M9 9h11v11H9zM15 5H5v14"
#define WC_ICON_DOWNLOAD "M12 3v12M7 11l5 5 5-5M4 21h16"
#define WC_ICON_UPLOAD "M12 21V9M7 13l5-5 5 5M4 3h16"
#define WC_ICON_EXTERNAL                                                                           \
  "M14 4h6v6M20 4l-9 9M18 14v5a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V8a1 1 0 0 1 1-1h5"
#define WC_ICON_REFRESH "M20 11a8 8 0 0 0-13.6-4.6L3 9M3 4v5h5M4 13a8 8 0 0 0 13.6 4.6L21 15M21 20v-5h-5"
#define WC_ICON_LINK "M9 15l6-6M8 12l-2 2a3.5 3.5 0 0 0 5 5l2-2M16 12l2-2a3.5 3.5 0 0 0-5-5l-2 2"
#define WC_ICON_STAR "m12 3 2.9 6 6.6.9-4.8 4.6 1.2 6.5L12 17.9 6.1 21l1.2-6.5L2.5 9.9 9 9z"
#define WC_ICON_HEART "M12 20s-7-4.4-7-9a4 4 0 0 1 7-2.6A4 4 0 0 1 19 11c0 4.6-7 9-7 9"
#define WC_ICON_INFO "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18M12 11v5M12 8h.01"
#define WC_ICON_ALERT "M12 3 2 21h20zM12 9v5M12 18h.01"
#define WC_ICON_CLOCK "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18M12 7v5l4 2"
#define WC_ICON_CALENDAR "M4 6h16v14H4zM8 3v4M16 3v4M4 10h16"
#define WC_ICON_USER "M12 4a4 4 0 1 0 0 8 4 4 0 0 0 0-8M4 21c0-4 3.6-6 8-6s8 2 8 6"
#define WC_ICON_USERS "M9 4a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7M2 20c0-3.5 3-5.5 7-5.5s7 2 7 5.5M16 4.5a3.5 3.5 0 0 1 0 7M18 14c2.5.6 4 2.4 4 6"
#define WC_ICON_FOLDER "M3 7a2 2 0 0 1 2-2h4l2 3h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"
#define WC_ICON_FILE "M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8zM14 3v5h5"
#define WC_ICON_SAVE "M5 3h11l3 3v15H5zM8 3v6h7V3M8 15h8"
#define WC_ICON_SUN "M12 4V2M12 22v-2M4 12H2M22 12h-2M6.3 6.3 4.9 4.9M19.1 19.1l-1.4-1.4M17.7 6.3l1.4-1.4M4.9 19.1l1.4-1.4M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8"
#define WC_ICON_MOON "M21 12.8A8.5 8.5 0 1 1 11.2 3a6.6 6.6 0 0 0 9.8 9.8"
#define WC_ICON_PLAY "m8 5 11 7-11 7z"
#define WC_ICON_STOP "M6 6h12v12H6z"
#define WC_ICON_LOCK "M6 11h12v9H6zM9 11V8a3 3 0 0 1 6 0v3"
#define WC_ICON_BARS "M4 20V10M10 20V4M16 20v-7M2 20h20"

static inline void wc_icon(const char *d, i32 size) {
  el("svg") {
    class_("icon");
    attr("viewBox", "0 0 24 24");
    attr("fill", "none");
    attr("stroke", "currentColor");
    attr("stroke-width", "1.75");
    attr("stroke-linecap", "round");
    attr("stroke-linejoin", "round");
    attr("aria-hidden", "true");
    attr("focusable", "false");
    if (size > 0) {
      attr_i("width", size);
      attr_i("height", size);
    }
    if (d && *d) el("path") { attr("d", d); }
  }
}
#define icon(d_, size_) wc_icon((d_), (size_))

// The accessible name of an icon-only control. An SVG cannot carry one, so the control
// that holds it does — and the attribute is here so that reading an app's view tells you
// which controls have a name and which quietly do not.
static inline void wc_aria_label(Text label) {
  if (label && *label) wc_attr("aria-label", WC_AS_STR(label));
}
#define aria_label(v_) wc_aria_label((v_))

// A button whose face is a path. The accessible name is not optional and not an attribute the
// caller remembers: an icon with no text is a button a screen reader reads as "button", which
// is why every hand-rolled icon button in every codebase is unlabelled. `label` is the name
// (and the tooltip); `hook` is what `bind`/`on_click` targets, exactly like `named()`.
//
//   button_icon(WC_ICON_PLUS, "New card", "new_card");
//
// It is markup and nothing else — no click handler, no state (the shell rule: a module adds C,
// never behaviour the bridge must grow a verb for).
static inline void wc_button_icon(const char *d, Text label, const char *hook) {
  el("button") {
    class_("icon-btn");
    type_("button");
    wc_aria_label(label);
    if (label && *label) attr("title", WC_AS_STR(label));
    if (hook) named(hook);
    wc_icon(d, 0);
  }
}
#define button_icon(d_, label_, hook_) wc_button_icon((d_), (label_), (hook_))