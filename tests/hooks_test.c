// hooks: the wiring a page does between a control and the code behind it. Everything here
// answers one question — when a button does nothing, does anything say so? A `bind()` to a
// name no element carries compiled, ran, and produced a dead control with no message
// anywhere (that is the bug this file exists for), and a shared view could not hand a
// listener to the client half without the two halves spelling strings at each other.
#include <webc/test.h>

typedef struct GaugeCtx {
  i32 now;
} GaugeCtx;

declare_view(Gauge, GaugeCtx);

state(i64, hits, 0);

action(bump) { set(hits, get(hits) + 1); }
action(nudge) { set(hits, get(hits) + 10); }

view(Gauge, GaugeCtx *c) {
  el("main") {
    el("button") {
      class_("btn");
      on_click(bump);
      text("bump");
    }
    el("button") {
      named("nudge-hook");
      text("nudge");
    }
    el("p") {
      text("hits=%lld", (long long)get(hits));
    }
  }
}

TEST(a_view_names_its_context_type_in_the_shared_header) {
  // VIEW_CTX is the half `render()` needs: the typedef, next to the declaration. A route
  // in another file can now say `render(Gauge, .now = 3)` without restating the type.
  GaugeCtx ctx = {.now = 7};
  WComponent c;
  wcomp_init(&c, nullptr);
  str html = wcomp_html_ctx(&c, view_Gauge, &ctx);
  ASSERT_STR_CONTAINS(html, "hits=");
}

TEST(on_click_names_the_element_after_the_action) {
  // The hook is visible in the markup, so a page, a test and the browser agree about the
  // wiring — and the name came from the function, not from a string in another file.
  WComponent c;
  wcomp_init(&c, nullptr);
  str html = wcomp_html_ctx(&c, view_Gauge, nullptr);
  ASSERT_STR_CONTAINS(html, "data-hook=\"bump\"");
  // `named("nudge-hook")` still names an element by hand, for a name decided elsewhere
  ASSERT_STR_CONTAINS(html, "data-hook=\"nudge-hook\"");
}

TEST(on_click_attaches_a_listener_only_in_the_client_half) {
  // Server-side rendering is markup: `data-on-click` carries a node id, and the server's
  // ids mean nothing in the browser. So SSR says the hook, the client says the listener.
  WComponent ssr;
  wcomp_init(&ssr, nullptr);
  ASSERT_FALSE(str_contains(wcomp_html_ctx(&ssr, view_Gauge, nullptr), str_lit("data-on-click")));

  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Gauge, nullptr);
  str cmds = buf_str(&c.cmds.b);
  ASSERT_STR_CONTAINS(cmds, "listen");
}

TEST(the_action_runs_from_the_hook_it_was_bound_to) {
  set(hits, 0);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Gauge, nullptr);
  // the bridge dispatches by name; the same path the browser takes
  ASSERT_TRUE(wc_test_dispatch("bump", "click"));
  ASSERT_EQ(get(hits), 1);
  ASSERT_TRUE(wc_test_dispatch("bump", "click"));
  ASSERT_EQ(get(hits), 2);
  // a name nobody wired, or the wrong event, is a click that goes nowhere — and says so
  ASSERT_FALSE(wc_test_dispatch("bumpety", "click"));
  ASSERT_FALSE(wc_test_dispatch("bump", "input"));
}

TEST(a_bind_nothing_honours_is_reported_once) {
  // `bind("nudge_hook", …)` never matches `named("nudge-hook")`: the control is dead. Two
  // renders in, the second one reports it — once — instead of a button that quietly does
  // nothing forever. (The message goes to stderr; webc test --client asserts the text.)
  set(hits, 0);
  wc_bind_hook("nudge_hook", "click", (void *)action_nudge);
  WComponent c;
  wcomp_init(&c, nullptr);
  wc_client_mode();
  wcomp_render_ctx(&c, view_Gauge, nullptr);
  wcomp_render_ctx(&c, view_Gauge, nullptr); // the render that reports it
  ASSERT_TRUE(wc_test_dispatch("nudge_hook", "click")); // still callable, just never wired
}

TEST_MAIN()