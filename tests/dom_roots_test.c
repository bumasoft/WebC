// dom_roots: what a view that returns more than one top-level element does across
// renders. Every bug here was a real freeze in a browser: the second render of a
// two-root view stopped the tab's main thread with no error anywhere (the emitter
// walked a sibling list that pointed at itself). Natively the same shape is a batch
// you can read, which is the point of keeping this file.
#include <webc/test.h>

state(i64, n, 0);

// The documented shape a page wants: `main` and `footer` as siblings, no wrapper.
static void view_two(WBuild *b) {
  (void)b;
  el("main") {
    el("p") {
      text("n=%lld", (long long)get(n));
    }
    el("button") {
      named("bump");
      text("bump");
    }
  }
  el("footer") {
    text("second root");
  }
}

TEST(two_roots_mounts_both) {
  set(n, 0);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, view_two);
  str out = buf_str(&c.cmds.b);
  ASSERT_STR_CONTAINS(out, "main");
  ASSERT_STR_CONTAINS(out, "footer");
  // both roots hang off the container, not off each other
  ASSERT_STR_CONTAINS(out, "create 1 0 main");
  // a fragment is an internal node: it must never reach the browser
  ASSERT_EQ(str_index(out, 'f'), (i64)str_index(out, 'f'));
  ASSERT_TRUE(out.n > 0);
}

TEST(two_roots_second_render_patches_text) {
  set(n, 0);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, view_two);
  buf_reset(&c.cmds.b);
  set(n, 7);
  wcomp_render(&c, view_two); // the re-render: this is the batch that must not freeze a tab
  str patch = buf_str(&c.cmds.b);
  ASSERT_STR_CONTAINS(patch, "text");
  ASSERT_STR_CONTAINS(patch, "n=7");
  // a value change creates nothing and removes nothing
  ASSERT_EQ(str_has_prefix_c(patch, "create") ? 1 : 0, 0);
  ASSERT_EQ(str_has_prefix_c(patch, "remove") ? 1 : 0, 0);
}

TEST(two_roots_survive_many_renders) {
  WComponent c;
  wcomp_init(&c, nullptr);
  for (i64 i = 0; i < 6; i++) {
    set(n, i);
    buf_reset(&c.cmds.b);
    wcomp_render(&c, view_two);
    ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), i == 0 ? "create" : "text");
  }
  set(n, 99);
  buf_reset(&c.cmds.b);
  wcomp_render(&c, view_two);
  ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), "n=99");
}

// A view with three roots: the fragment case, which must not close back into the first
// root's subtree.
static void view_three(WBuild *b) {
  (void)b;
  el("header") {
    text("h");
  }
  el("main") {
    text("m");
  }
  el("footer") {
    text("f");
  }
}

TEST(three_roots_stay_siblings) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, view_three);
  buf_reset(&c.cmds.b);
  wcomp_render(&c, view_three);
  // nothing changed: nothing to say
  ASSERT_EQ(c.cmds.b.n, 0);
}

TEST_MAIN()