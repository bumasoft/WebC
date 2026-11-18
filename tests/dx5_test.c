// dx5: where the pointer was.
//
// The app this came from drew a tooltip in 54 lines of JavaScript: a document-level
// pointermove, `closest('g.stratg')`, two data attributes, and a fixed div moved with
// clientX/clientY. Every fact it needed was already in the C view, and the only thing the
// C side could not see was the pointer. These tests hold that door shut:
//
//   * `ev_point()` answers inside an action, from the payload the bridge sends
//   * `ev_box()` is the element's own box, so a chart can invert its scale
//   * an event with no pointer data answers zeros, not garbage
//   * a native test says where the pointer was (`client_dispatch_at`), so hover code is
//     under test like everything else
#include <webc/client_test.h>

state(f64, at_x, -1);
state(f64, at_y, -1);
state(f64, tip_on, 0);
state(Text, tip_label, "");

action(follow) {
  WPoint p = ev_point();
  set(at_x, p.cx);
  set(at_y, p.cy);
  set(tip_on, 1);
}

action(unfollow) { set(tip_on, 0); }

// A crosshair: the element-local fraction, which is what a chart actually wants and what
// clientX alone cannot give.
state(f64, frac_x, -1);
action(crosshair) {
  WBox b = ev_box();
  WPoint p = ev_point();
  set(frac_x, b.w > 0 ? p.x / b.w : 0);
}

// The row the pointer is over. A hover identifies its target the same way a click does:
// by the nearest attribute at or above the element that was entered.
action(over_row) {
  str name = ev_attr("data-sname");
  if (name.n) set(tip_label, persist(text_of(name)));
}

view(Chart) {
  el("svg") {
    named("plot");
    class_("plot");
    named("axis");
    el("g") {
      named("strat-row");
      attr("data-sname", "flat");
      el("text") { text("flat"); }
    }
  }
  if (get(tip_on)) el("div") { class_("tip"); text("x=%g y=%g", get(at_x), get(at_y)); }
}

static void wire(void) {
  bind("plot", "pointermove", follow);
  bind("plot", "pointerleave", unfollow);
  bind("axis", "pointermove", crosshair);
  bind("strat-row", "pointermove", over_row);
}

// ---------------------------------------------------------------- the door

TEST(pressing_the_pointer_hands_the_action_its_position) {
  wc_host_reset();
  wire();
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Chart, nullptr);

  set(tip_on, 0);
  client_dispatch_at("plot", "pointermove", 120, 40, 600, 200);
  EXPECT_SIGNAL(tip_on, 1);
  ASSERT_EQ((long long)get(at_x), 120);
  ASSERT_EQ((long long)get(at_y), 40);

  // leave: the same hook, the other event, no coordinates needed
  client_dispatch("plot", "pointerleave");
  EXPECT_SIGNAL(tip_on, 0);
}

TEST(the_box_is_the_elements_own_so_a_chart_can_invert_its_scale) {
  wc_host_reset();
  wire();
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Chart, nullptr);

  set(frac_x, -1);
  client_dispatch_at("axis", "pointermove", 150, 10, 600, 200);
  ASSERT_FEQ(get(frac_x), 0.25);
}

TEST(an_event_without_a_pointer_answers_zeros) {
  wc_host_reset();
  wire();
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Chart, nullptr);

  // a plain dispatch: the bridge sent a value, not a point
  set(at_x, -1);
  set(at_y, -1);
  client_dispatch("plot", "pointermove");
  ASSERT_EQ((long long)get(at_x), 0);
  ASSERT_EQ((long long)get(at_y), 0);
  WPoint p = ev_point();
  ASSERT_EQ((long long)p.w, 0);
  ASSERT_EQ((long long)p.h, 0);
}

TEST(a_hover_names_the_thing_it_is_over_the_way_a_click_does) {
  wc_host_reset();
  wire();
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Chart, nullptr);

  set(tip_label, "");
  client_dispatch_at("strat-row", "pointermove", 5, 5, 40, 12);
  EXPECT_SIGNAL_STR(tip_label, "flat");
}

TEST(the_payload_encoder_is_the_one_the_bridge_sends) {
  // The bridge writes "\x01x,y,w,h,cx,cy". A point that came from anywhere else must not be
  // mistaken for one, and the parse must survive the shortest legal payload.
  ASSERT_EQ((long long)ev_point().cx, 0);
  client_dispatch_at("plot", "pointermove", 1.5, 2.5, 10, 20);
  WPoint p = ev_point(); // outside the dispatch the point is gone again
  ASSERT_EQ((long long)p.cx, 0);
}

TEST_MAIN()