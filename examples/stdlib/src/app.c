// examples/stdlib — the shelf, in one page.
//
// This app exists to answer one question an app author actually asks: what does a page
// look like when the standard library carries the parts nobody enjoys writing? Everything
// here that is *not* about rows, teams and a "new row" dialog belongs to a module:
//
//   theme   the tokens and the dark edition            form   the fields, from the model
//   table   the sort, the aria-sort and the pager      keys   one table of shortcuts
//   overlay the dialog, the toast and the tip          icons  the icons on the buttons
//   motion  the rows arriving                          ui     the switch and the badges
//
// The app's own job is left visible on purpose: three comparators, two actions, and the
// view. Everything else is one call, and `webc stdlib --used` counts what it took.
#include <webc/web.h>
#include <webc/icons.h>
#include <webc/form.h>
#include <webc/keys.h>
#include <webc/motion.h>
#include <webc/overlay.h>
#include <webc/table.h>
#include <webc/theme.h>

// ---------------------------------------------------------------- the model

WEBC_ENUM(Tier, starter, growth, scale)

WEBC_MODEL(Row,
           (id, i64, .pk = true),
           (team, str, .required = true, .max = 40),
           (owner, str, .max = 40),
           (tier, Tier, .not_null = true),
           (seats, i32, .gte = 1, .lte = 500),
           (mrr, f64, .gte = 0))

Vec(Row)

static VecRow g_rows;
static i64 g_next_id = 1;

// Five rows, so the pager has something to page through on the first paint.
static void seed_rows(void) {
  static const struct {
    const char *team, *owner;
    Tier tier;
    i32 seats;
    f64 mrr;
  } SEED[] = {
      {"Harbour", "Ada", growth, 12, 2400},
      {"Lantern", "Bo", starter, 3, 120},
      {"Meridian", "Cy", scale, 64, 18400},
      {"Nine Bells", "Di", growth, 21, 5600},
      {"Quarry", "Ed", starter, 2, 0},
      {"Saltmarsh", "Fay", growth, 34, 9100},
  };
  for (i32 i = 0; i < WC_ARRLEN(SEED); i++)
    push(g_rows, (Row){.id = g_next_id++, .team = str_lit(SEED[i].team),
                        .owner = str_lit(SEED[i].owner), .tier = SEED[i].tier,
                        .seats = SEED[i].seats, .mrr = SEED[i].mrr});
}

// ---------------------------------------------------------------- the theme
//
// Four tokens, two editions. The page has no colour of its own anywhere else, which is
// what makes the dark edition one table instead of an audit.

static const WToken TOKENS[] = {
    {"bg", "#fbfaf7", "#14150f"},
    {"fg", "#1a1a18", "#eceae4"},
    {"acc", "#3d4ee0", "#8f9aff"},
    {"line", "#e4e1d9", "#2b2d26"},
};
static const WTheme THEME = {"shelf", TOKENS, WC_ARRLEN(TOKENS), "Inter, system-ui", nullptr};

static const WOverlay SHORTCUTS = {"keys_dlg", "Keyboard", "keys_close"};

// ---------------------------------------------------------------- state

state(i64, sort, 0);
state(i64, page, 0);
state(bool, open_new, false);
state(bool, open_keys, false);
state(bool, dark, false);
state(Text, toast_msg, "");

static i32 g_order[64];

// ---------------------------------------------------------------- comparators
//
// A column is a label, a number and an alignment. The table reads the row through the
// pointer, so these three lines are the whole of "this table sorts".

static f64 by_team(const void *r) { return (f64)strlen(((const Row *)r)->team.p); }
static f64 by_seats(const void *r) { return (f64)((const Row *)r)->seats; }
static f64 by_mrr(const void *r) { return ((const Row *)r)->mrr; }

static const WTableKey COLS[] = {
    {"Team", by_team, nullptr},
    {"Owner", nullptr, nullptr},
    {"Tier", nullptr, nullptr},
    {"Seats", by_seats, "right"},
    {"MRR", by_mrr, "right"},
};

static WTable table(void) {
  WTable t;
  table_init(&t, g_rows.at, vlen(&g_rows), sizeof(Row), g_order);
  table_keys(&t, COLS, WC_ARRLEN(COLS));
  table_state(&t, get(sort), (i32)get(page), 5);
  return t;
}

// ---------------------------------------------------------------- shortcuts
//
// One array. `keys_bind` in client_main binds it; `keys_help` in the view lists it. A
// shortcut that is bound and undocumented is not a shape this app can take.

ACTION(new_open)
ACTION(keys_open)
ACTION(esc_close)
ACTION(theme_flip)

static const WCmd CMDS[] = {
    K("New row", "n", new_open),
    K("Keyboard shortcuts", "shift+?", keys_open),
    K("Dark mode", "shift+d", theme_flip),
    K("Close", "escape", esc_close),
};

// ---------------------------------------------------------------- actions

// The dialog's draft, in signals: the form kit renders, "the binding of an input
// to a signal stays the app's one bind_str()" (form.h) — the inputs the dialog
// draws are named, and the client half binds them in client_main.
static struct {
  Text team;
  Text owner;
} g_new; // bind_ctx writes here as the reader types; no re-render per keystroke
state(i32, new_tier, 0);
state(i32, new_seats, 1);
state(f64, new_mrr, 0);

action(new_open) {
  g_new.team = "";
  g_new.owner = "";
  set(new_tier, 0);
  set(new_seats, 1);
  set(new_mrr, 0);
  set(open_new, true);
  // Reset through the host, once, after the panel is shown: a value written as
  // an attribute in the view would be re-applied by every render the draft
  // causes (the bridge mirrors an attr write to the input's value property),
  // and the pending keystroke's payload would be clobbered by a stale draft —
  // the shape that lost letters mid-word. Defaults also ride on the panel's
  // own attributes, so the draft is the only state a reset must touch.
  js("webc_set_value", "new-team", "");
  js("webc_set_value", "new-owner", "");
  js("webc_set_value", "new-seats", "1");
  js("webc_set_value", "new-mrr", "0");
  js("webc_set_value", "new-tier", "starter");
}

action(tier_pick) {
  str v = ev_value();
  if (v.n) set(new_tier, Tier_index(v));
}
action(seats_pick) {
  str v = ev_value();
  i64 n = 0;
  if (v.n && str_to_i64(v, &n) && n >= 1 && n <= 500) set(new_seats, (i32)n);
}
action(mrr_pick) {
  str v = ev_value();
  f64 d = 0;
  if (v.n) {
    char buf[32];
    size_t n = v.n < sizeof buf - 1 ? v.n : sizeof buf - 1;
    memcpy(buf, v.p, n);
    buf[n] = 0;
    d = strtod(buf, nullptr);
    if (d < 0) d = 0;
  }
  set(new_mrr, d);
}
action(keys_open) { set(open_keys, true); }
action(esc_close) {
  set(open_new, false);
  set(open_keys, false);
}

action(theme_flip) {
  bool dark = !get(dark);
  set(dark, dark);
  theme_paint(dark);
}

action(sort_by) {
  i32 key = (i32)ev_attr_i64("data-key");
  set(sort, table_click(key, get(sort)));
  set(page, 0);
}

action(page_flip) {
  WTable t = table();
  set(page, table_page(&t, (i32)get(page) + (i32)ev_attr_i64("data-step")));
}

ACTION(add_row)

static void toast_done(WJs r) {
  (void)r;
  set(toast_msg, "");
}

action(add_row) {
  // the model's own rule, applied early: a team is required (Row, .required)
  if (!g_new.team || !g_new.team[0]) {
    set(toast_msg, "A team is required");
    webc_later(2600, toast_done);
    return;
  }
  Row draft = {.id = g_next_id++,
               .team = str_lit(g_new.team),
               .owner = g_new.owner && g_new.owner[0] ? str_lit(g_new.owner) : str_lit(""),
               .tier = (Tier)get(new_tier),
               .seats = get(new_seats),
               .mrr = get(new_mrr)};
  set(open_new, false);
  set(toast_msg, "Row added");
  push(g_rows, draft);
  webc_later(2600, toast_done); // a toast that never leaves is a bug of its own
}

// ---------------------------------------------------------------- the view

view(Shelf) {
  // A client-only page has no server-rendered <head> to write into, so the two style
  // blocks the modules generate are emitted where the app starts. A server-rendered page
  // uses `theme_head_html()` / `motion_head_html()` from its route instead — same CSS,
  // into the head, one paint earlier.
  theme_head(&THEME);
  motion_head();

  el("header") {
    class_("bar");
    el("h1") { text("Shelf"); }
    switch_("theme_flip", get(dark), "Force dark");
    el("button") {
      class_("ghost");
      on_click(keys_open);
      keycap("shift+?");
    }
    el("button") {
      class_("primary");
      on_click(new_open);
      icon(WC_ICON_PLUS, 16);
      text("New row");
    }
  }

  el("main") {
    if (vlen(&g_rows) == 0) {
      empty_state("No rows yet", "Add the first team to see the table work.",
                  "new_open", "New row");
    } else {
      WTable t = table();
      el("table") {
        table_head(&t, sort_by);
        el("tbody") {
          for (i32 i = 0; i < table_len(&t); i++) {
            const Row *r = table_row_typed(Row, &t, i);
            el("tr") {
              enter("rise");
              stagger(i);
              el("td") { text(r->team); }
              el("td") { text(r->owner); }
              el("td") {
                badge(Tier_name(r->tier), r->tier == scale ? "ok" : nullptr);
              }
              el("td") {
                attr("data-align", "right");
                text("%d", r->seats);
              }
              el("td") {
                attr("data-align", "right");
                text("$%.0f", r->mrr);
              }
            }
          }
        }
      }
      table_pager(&t, page_flip, "Rows");
    }

    // The toasts wrapper lives inside <main>, present from the first render: its
    // CSS is position:fixed wherever it sits, and as a descendant of main it is
    // never a root-sibling candidate for a departing dialog's scrim — the shape
    // that traded identities and left a full-width invisible sheet behind.
    toasts(nullptr) {
      if (get(toast_msg) && *get(toast_msg)) toast(get(toast_msg), "ok");
    }
  }

  // A client-only dialog whose subtree STAYS in the tree and toggles with
  // show(): a panel that appears and disappears cannot hold a draft — every
  // keystroke re-renders the fresh subtree and the input loses focus and its
  // value mid-word. The overlay module's appear/disappear shape is for server
  // forms (the board example's dialog POSTs); a client-only draft wants stable
  // nodes — the same property the web example's always-there input has. The
  // contract stays the module's: scrim and panel are siblings, the panel carries
  // the role, the name, and no hook of its own; Escape closes (the shortcuts
  // table).
  el("div") {
    class_("scrim");
    show(get(open_new));
    attr("aria-hidden", "true");
    named("new_close");
    on_click(esc_close);
  }
  el("div") {
    class_("panel");
    show(get(open_new));
    attr("role", "dialog");
    attr("aria-modal", "true");
    attr("aria-label", "New row");
    attr("tabindex", "-1");
    id_("new_row");
    el("h2") { text("New row"); }
    // Named fields the client half binds (client_main): the form kit's own header
    // says binding is the app's job — a draft the reader types into, held in
    // stable nodes, is the client-side idiom (form.h, forms guide).
    el("div") {
      class_("field");
      el("label") { attr("for", "new-team"); text("Team"); }
      el("input") {
        type_("text");
        id_("new-team");
        named_field("nteam");
        required();
        attr_i("maxlength", 40);
      }
    }
    el("div") {
      class_("field");
      el("label") { attr("for", "new-owner"); text("Owner"); }
      el("input") {
        type_("text");
        id_("new-owner");
        named_field("nowner");
        attr_i("maxlength", 40);
      }
    }
    el("div") {
      class_("field");
      el("label") { attr("for", "new-tier"); text("Tier"); }
      el("select") {
        id_("new-tier");
        named_field("ntier");
        for (i32 i = 0; i < Tier_N; i++)
          el("option") { value_(Tier_name(i)); if (i == get(new_tier)) attr("selected", "true"); text(Tier_name(i)); }
      }
    }
    el("div") {
      class_("field");
      el("label") { attr("for", "new-seats"); text("Seats"); }
      el("input") {
        type_("number");
        id_("new-seats");
        named_field("nseats");
        attr("min", "1");
        attr("max", "500");
      }
    }
    el("div") {
      class_("field");
      el("label") { attr("for", "new-mrr"); text("MRR ($/mo)"); }
      el("input") {
        type_("number");
        id_("new-mrr");
        named_field("nmrr");
        attr("step", "any");
        attr("min", "0");
      }
    }
    el("div") {
      class_("row-actions");
      el("button") {
        class_("ghost");
        on_click(esc_close);
        text("Cancel");
      }
      el("button") {
        class_("primary");
        on_click(add_row);
        text("Add");
      }
    }
  }

  keys_help(CMDS, &SHORTCUTS, get(open_keys));

}

client_main {
  seed_rows();
  keys_bind(CMDS);
  // The dialog's draft: two-way for the text (bind_str above), one action each
  // for the fields whose values are not strings
  // the string draft: bind_ctx persists the typed text into g_new, no re-render
  // per keystroke (the draft idiom, host.h); the numbered fields parse in actions
  // bind_ctx (not the _live spelling): a draft does not re-render on keystrokes,
  // so a write from an older render cannot clobber the next event's payload.
  bind_ctx("nteam", g_new, team);
  bind_ctx("nowner", g_new, owner);
  bind_field("ntier", tier_pick);
  bind_field("nseats", seats_pick);
  bind_field("nmrr", mrr_pick);
  // a shared control names itself; the client half says what the name does. (A button
  // whose action is written where the button is would use on_click() instead.)
  bind("theme_flip", "click", theme_flip);
  mount(Shelf, "#root");
  return 0;
}