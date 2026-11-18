// SPDX-License-Identifier: Apache-2.0
// webc/keys.h — one table of shortcuts, bound and documented from the same rows.
//
// A shortcut lives in three places in every app and agrees in none of them: the `on_key`
// that binds it, the help sheet that lists it, and the user's memory. Here it is one row.
//
//   static const WCmd CMDS[] = {
//       {"Command palette", "meta+k", pal_open},
//       {"Help",            "shift+?", help_open},
//       {"New card",        "n",       card_new},
//       {"Close",           "escape",  esc_close},
//   };
//
//   client_main { keys_bind(CMDS); on_ready(ready); }   // the bindings
//   view(App)   { keys_help(CMDS, help_open); }          // the sheet, same table
//
// The sheet is a dialog of `<kbd>` rows — so a shortcut that is bound and not documented,
// or documented and not bound, is impossible: there is one array.
//
// `keycap()` is the other half of remembering: the hint on a button that says what to
// press. It reads the same spec ("meta+k") and prints "⌘K", because the platform's
// notation is a fact the framework can own and each app should not.
#pragma once

#include <webc/dom.h>
#include <webc/host.h>
#include <webc/overlay.h>
#include <webc/ui.h>
#include <webc/pp.h>

typedef struct WCmd {
  const char *label; // what it does, in words
  const char *spec;  // "meta+k", "shift+?", "escape" — one modifier plus one key
  WEventFn fn;       // the action
} WCmd;

// `K("Help", "shift+?", help_open)`: `key(key_, key_, act)` would repeat the action's name
// twice, and the macro form names it once.
#define K(label_, spec_, ACT_) {.label = (label_), .spec = (spec_), .fn = (WEventFn)WC_CAT(action_, ACT_)}

// Bind every row. Same spec twice is one binding (the bridge's own rule), so a table that
// repeats a key is not an error here — it is a key with two names, and the help sheet says
// so in two rows, which is the truth about it.
static inline void wc_keys_bind(const WCmd *cmds, i32 n) {
  for (i32 i = 0; i < n; i++)
    if (cmds[i].spec && cmds[i].fn) wc_on_key(cmds[i].spec, cmds[i].fn);
}
#define keys_bind(cmds_) wc_keys_bind((cmds_), WC_ARRLEN(cmds_))

// "meta+k" -> "⌘K". One spec, one notation, and the notation is the reader's: a spec with
// `meta` is a Mac chord, so it is written the Mac way and read the Mac way.
static inline const char *wc_keycap_of(const char *spec) {
  static char bufs[4][48];
  static int which;
  char *out = bufs[which++ & 3];
  size_t o = 0;
  const char *p = spec ? spec : "";
  while (*p && o + 8 < sizeof bufs[0]) {
    const char *plus = strchr(p, '+');
    size_t n = plus ? (size_t)(plus - p) : strlen(p);
    if (n == 4 && strncmp(p, "meta", 4) == 0) out[o++] = '\xe2', out[o++] = '\x8c', out[o++] = '\x98'; // ⌘
    else if (n == 5 && strncmp(p, "shift", 5) == 0) out[o++] = '\xe2', out[o++] = '\x87', out[o++] = '\xa7'; // ⇧
    else if (n == 3 && strncmp(p, "alt", 3) == 0) out[o++] = '\xe2', out[o++] = '\x8c', out[o++] = '\xa5'; // ⌥
    else if (n == 5 && strncmp(p, "ctrl", 5) == 0) out[o++] = '\xe2', out[o++] = '\x8c', out[o++] = '\x83'; // ⌃
    else if (n == 5 && strncmp(p, "enter", 5) == 0) out[o++] = '\xe2', out[o++] = '\x86', out[o++] = '\xb5'; // ↵
    else if (n == 6 && strncmp(p, "escape", 6) == 0) {
      memcpy(out + o, "Esc", 3);
      o += 3;
    } else {
      for (size_t i = 0; i < n && o + 1 < sizeof bufs[0]; i++) {
        char c = p[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[o++] = c;
      }
    }
    if (!plus) break;
    p = plus + 1;
  }
  out[o] = 0;
  return out;
}
static inline void wc_keycap(const char *spec) {
  el("kbd") { text(wc_keycap_of(spec)); }
}
#define keycap(spec_) wc_keycap(spec_)

// The sheet: every row, in the order the table lists them, inside the same dialog the app's
// other panels use. `open` is the app's signal, as everywhere else.
#define keys_help(cmds_, box_, open_) wc_keys_help((cmds_), WC_ARRLEN(cmds_), (box_), (open_))

static inline void wc_keys_help(const WCmd *cmds, i32 n, const WOverlay *box, bool open) {
  dialog(box, open) {
    el("h2") { class_("keys-title"); text("Keyboard"); }
    el("dl") {
      class_("keys");
      for (i32 i = 0; i < n; i++) {
        el("dt") {
          if (cmds[i].spec && *cmds[i].spec) keycap(cmds[i].spec);
        }
        el("dd") { text(cmds[i].label ? cmds[i].label : ""); }
      }
    }
  }
}