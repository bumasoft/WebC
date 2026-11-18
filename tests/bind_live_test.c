// Live bindings: one node, one op, no re-render.
//
// The contract these tests hold: a signal write reaches exactly the node that read it, as
// the same `text`/`attr` line the patcher would have emitted — and a value that did not
// change emits nothing at all. The last two tests are the safety property: a binding whose
// node leaves the tree stops writing, and a re-render does not double it.
#include <webc/test.h>

state(i64, count, 0);
state(bool, open, false);
state(i32, small, 3);
state(f64, ratio, 0.5);
state(Text, label, "ready");
state(bool, shown, true);

// A row of every kind of binding, plus one plain node that must never be re-emitted.
view(Panel) {
  el("main") {
    el("span") { text_live("%lld", count); }
    el("b") { text("static"); }
    el("i") { attr_live("data-n", small); }
    el("li") {
      class_("row");
      class_live("open", open);
    }
    el("div") {
      style("width:1px");
      show_live(shown);
    }
    el("em") { text_live("%s", label); }
    el("u") { text_live("%.2f", ratio); }
  }
}

static i32 count_lines(str s, const char *needle) {
  i32 n = 0;
  size_t nl = strlen(needle);
  for (size_t i = 0; i + nl <= s.n; i++)
    if (memcmp(s.p + i, needle, nl) == 0) n++;
  return n;
}

TEST(a_signal_write_emits_one_op_and_runs_no_view) {
  set(count, 0);
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Panel, nullptr);
  buf_reset(&c.cmds.b); // the mount batch has gone out; this is the next callback

  set(count, 7);
  str cmds = buf_str(&c.cmds.b);
  ASSERT_EQ(count_lines(cmds, "\n"), 1); // exactly one line: one op
  ASSERT_STR_CONTAINS(cmds, "text ");
  ASSERT_STR_CONTAINS(cmds, "7");
  wcomp_destroy(&c);
}

TEST(a_write_that_changes_nothing_emits_nothing) {
  set(count, 4);
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Panel, nullptr);
  buf_reset(&c.cmds.b);

  set(count, 4); // same bytes: w_write suppresses the notification entirely
  ASSERT_EQ((i32)c.cmds.b.n, 0);
  set(count, 5);
  ASSERT_GT((i32)c.cmds.b.n, 0);
  wcomp_destroy(&c);
}

TEST(every_kind_of_binding_writes_its_own_attribute) {
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Panel, nullptr);
  buf_reset(&c.cmds.b);

  set(small, 42);
  ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), "data-n 42");

  buf_reset(&c.cmds.b);
  set(open, true);
  ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), "class row open");
  buf_reset(&c.cmds.b);
  set(open, false);
  // the class the *view* wrote survives; only the live token is removed
  ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), "class row");
  ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), "attr ");

  buf_reset(&c.cmds.b);
  set(shown, false);
  ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), "style width:1px;display:none");
  buf_reset(&c.cmds.b);
  set(shown, true);
  ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), "style width:1px");
  wcomp_destroy(&c);
}

TEST(a_text_signal_and_a_double_both_format) {
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Panel, nullptr);
  buf_reset(&c.cmds.b);

  set(label, "busy");
  ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), "busy");
  buf_reset(&c.cmds.b);
  set(ratio, 0.75);
  ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), "0.75");
  wcomp_destroy(&c);
}

TEST(a_render_does_not_double_a_binding) {
  set(count, 1);
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Panel, nullptr);
  wcomp_render_ctx(&c, view_Panel, nullptr); // patch, re-registers every binding
  wcomp_render_ctx(&c, view_Panel, nullptr);
  buf_reset(&c.cmds.b);

  set(count, 2);
  ASSERT_EQ(count_lines(buf_str(&c.cmds.b), "\n"), 1); // one binding, one op
  wcomp_destroy(&c);
}

state(bool, gate, true);

view(Gated) {
  el("div") {
    if (get(gate)) el("span") { text_live("%lld", count); }
  }
}

TEST(a_binding_dies_with_the_node_it_was_declared_on) {
  set(count, 0);
  set(gate, true);
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Gated, nullptr);
  buf_reset(&c.cmds.b);
  set(count, 3);
  ASSERT_GT((i32)c.cmds.b.n, 0); // alive while the branch is rendered

  set(gate, false);
  wcomp_render_ctx(&c, view_Gated, nullptr); // the span leaves; the binding goes with it
  buf_reset(&c.cmds.b);
  set(count, 4);
  ASSERT_EQ((i32)c.cmds.b.n, 0); // and writes nothing into a node nobody can see
  wcomp_destroy(&c);
}

TEST(the_server_half_renders_the_value_and_keeps_no_binding) {
  set(count, 11);
  WComponent c;
  wcomp_init(&c, nullptr);
  str html = wcomp_html_ctx(&c, view_Panel, nullptr);
  ASSERT_STR_CONTAINS(html, "11");
  // SSR never registers a live node, so a write has nowhere to land
  ASSERT_EQ((i32)c.cmds.b.n, 0);
  set(count, 12);
  ASSERT_EQ((i32)c.cmds.b.n, 0);
  wcomp_destroy(&c);
}

// ---------------------------------------------------------------- action_live

action(bump) { set(count, get(count) + 1); }
action_live(follow) { set(count, get(count) + 2); }

view(Wired) {
  el("div") {
    el("button") { named("bump"); text("bump"); }
    el("button") { named("follow"); text("follow"); }
  }
}

TEST(action_live_marks_its_run_and_a_plain_action_does_not) {
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Wired, nullptr);
  wc_bind_hook("bump", "click", (void *)action_bump);
  wc_bind_hook("follow", "click", (void *)action_follow);
  wcomp_render_ctx(&c, view_Wired, nullptr);

  wc_live_reset();
  ASSERT_TRUE(wc_test_dispatch("bump", "click"));
  ASSERT_EQ(wc_live_marks(), 0); // a plain action always earns a render

  wc_live_reset();
  ASSERT_TRUE(wc_test_dispatch("follow", "click"));
  ASSERT_EQ(wc_live_marks(), 1); // the bridge may skip the render for this one
  wcomp_destroy(&c);
}

TEST_MAIN()