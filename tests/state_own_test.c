// state_own: one slot per render position.
//
// The gap this closes: `state` is one value for the page, so a list of rows reading it is a
// list of rows showing the same value; `state_in` is one value per instance the *app* owns,
// needs a WInst created and threaded by hand, and cannot feed a live binding at all
// (`text_live` names `&N_##_node`, a file-scope static). `state_own` is the third answer: the
// framework owns an instance for each position that draws — a mounted view, or one `for_each`
// row — creates the slot on first read, disposes it with the position, and lets a live binding
// and an action address it without naming anything.
//
// Every test here is one of the promises the reference makes about it.
#include <webc/test.h>

// VecI64 is prelude — a `Vec(i64)` here would be a second declaration of it.
static Veci64 g_rows;

state_own(i64, n, 0);
state_own(bool, open, false);
computed_own(i64, twice) { return get(n) * 2; }

// Two declarations on one position, written by one action: both are the row's.
action(bump) {
  set(n, get(n) + 1);
  toggle(open);
}
action_live(bump_live) { set(n, get(n) + 1); }

// ---------------------------------------------------------------- helpers

static void fill_keys(const i64 *keys, i32 n) {
  g_rows.n = 0;
  for (i32 i = 0; i < n; i++) push(g_rows, keys[i]);
}

// The rendered text of the tree, in document order, separated by '|'. Reading the *nodes*
// and not the op stream is deliberate: it is what the page shows, before and after a patch.
static void texts_into(WDom *n, Buf *out) {
  for (WDom *p = n; p; p = p->next) {
    if (p->type == WD_TEXT && p->text.n) {
      if (out->n) buf_putc(out, '|');
      buf_putstr(out, p->text);
    }
    if (p->first) texts_into(p->first, out);
  }
}

static str texts(WDom *root) {
  Buf b;
  buf_init(&b, nullptr, 64);
  texts_into(root, &b);
  return buf_str(&b);
}

static void bindings_into(WDom *n, WDom **out, i32 cap, i32 *count) {
  for (WDom *p = n; p && *count < cap; p = p->next) {
    if (p->bind_sig) out[(*count)++] = p;
    if (p->first) bindings_into(p->first, out, cap, count);
  }
}

// ---------------------------------------------------------------- the views

// A list whose every row reads its own `n` (and its own flag, and a value derived from both).
static void rows_view(WBuild *b) {
  (void)b;
  el("ul") {
    for_each(r, g_rows, *r) {
      el("li") {
        attr_i("data-id", *r);
        el("span") { text("n=%lld", get(n)); }
        el("span") { text("t=%lld", get(twice)); }
        el("span") { text(get(open) ? "open" : "shut"); }
        el("button") { named("bump"); text("bump"); }
      }
    }
  }
}

// The same rows, with the values delivered by live bindings instead of by a re-render.
static void live_rows_view(WBuild *b) {
  (void)b;
  el("ul") {
    for_each(r, g_rows, *r) {
      el("li") {
        attr_i("data-id", *r);
        el("span") { text_live("n=%lld", n); }
        el("i") { attr_live("data-n", n); }
        el("button") { named("bump_live"); text("+"); }
      }
    }
  }
}

// One view, mounted twice: two positions, two values, no plumbing at all.
static void counter_view(WBuild *b) {
  (void)b;
  el("div") {
    el("span") { text("n=%lld", get(n)); }
    el("button") { named("bump"); }
  }
}

// A field per row, two-way bound to that row's own text.
state_own(Text, label, "");

static void field_rows_view(WBuild *b) {
  (void)b;
  el("ul") {
    for_each(r, g_rows, *r) {
      el("li") {
        attr_i("data-id", *r);
        el("input") { name("label"); bind_str(label); }
        el("em") { text("[%s]", get(label)); }
      }
    }
  }
}

// SSR: no position of its own beyond the render, and the initials are the page.
static void ssr_view(WBuild *b) {
  (void)b;
  el("ul") {
    for_each(r, g_rows, *r) {
      el("li") { text("%lld/%lld", *r, get(n)); }
    }
  }
}

// ---------------------------------------------------------------- 1. one value per row

TEST(a_row_reads_its_own_slot_and_not_its_neighbours) {
  const i64 keys[] = {1, 2, 3};
  fill_keys(keys, 3);
  wc_bind_hook("bump", "click", (void *)action_bump);

  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, rows_view);
  ASSERT_STR_EQ(texts(c.root), "n=0|t=0|shut|bump|n=0|t=0|shut|bump|n=0|t=0|shut|bump");

  // the hook names one element: the last row's button (the last place it was recorded).
  // Before `state_own` this wrote the one shared slot and all three rows said 1.
  ASSERT_TRUE(wc_test_dispatch("bump", "click"));
  wcomp_render(&c, rows_view);
  // the flag is a second declaration on the same position: the bump wrote both, and the
  // rows it did not touch kept theirs
  ASSERT_STR_EQ(texts(c.root), "n=0|t=0|shut|bump|n=0|t=0|shut|bump|n=1|t=2|open|bump");

  // …and it stays that row's: a second click is 2, not a fresh slot
  ASSERT_TRUE(wc_test_dispatch("bump", "click"));
  wcomp_render(&c, rows_view);
  ASSERT_STR_EQ(texts(c.root), "n=0|t=0|shut|bump|n=0|t=0|shut|bump|n=2|t=4|shut|bump");
  wcomp_destroy(&c);
}

TEST(a_value_follows_its_key_through_a_reorder) {
  const i64 before[] = {1, 2, 3};
  fill_keys(before, 3);
  wc_bind_hook("bump", "click", (void *)action_bump);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, rows_view);
  ASSERT_TRUE(wc_test_dispatch("bump", "click")); // row 3 → 1
  wcomp_render(&c, rows_view);

  const i64 after[] = {3, 1, 2}; // row 3 moves to the front
  fill_keys(after, 3);
  wcomp_render(&c, rows_view);
  // both values travelled with the key, not with the position
  ASSERT_STR_EQ(texts(c.root), "n=1|t=2|open|bump|n=0|t=0|shut|bump|n=0|t=0|shut|bump");
  wcomp_destroy(&c);
}

TEST(a_row_that_leaves_takes_its_state_with_it) {
  const i64 keys[] = {1, 2, 3};
  fill_keys(keys, 3);
  wc_bind_hook("bump", "click", (void *)action_bump);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, rows_view);
  ASSERT_TRUE(wc_test_dispatch("bump", "click"));
  wcomp_render(&c, rows_view);

  // the row leaves: its slots are killed and the graph shrinks back
  i32 live_before = w_node_count(wrt_current());
  const i64 keep[] = {1, 2};
  fill_keys(keep, 2);
  wcomp_render(&c, rows_view);
  ASSERT_STR_EQ(texts(c.root), "n=0|t=0|shut|bump|n=0|t=0|shut|bump");
  i32 live_after = w_node_count(wrt_current());
  ASSERT_LT(live_after, live_before);

  // and coming back is a *new* position: it starts at the declaration's value again
  fill_keys(keys, 3);
  wcomp_render(&c, rows_view);
  ASSERT_STR_EQ(texts(c.root), "n=0|t=0|shut|bump|n=0|t=0|shut|bump|n=0|t=0|shut|bump");
  wcomp_destroy(&c);
}

// ---------------------------------------------------------------- 2. a mount is a position

TEST(the_same_view_mounted_twice_keeps_two_values) {
  wc_bind_hook("bump", "click", (void *)action_bump);
  WComponent left, right;
  wcomp_init(&left, nullptr);
  wcomp_init(&right, nullptr);
  wcomp_render(&left, counter_view);
  wcomp_render(&right, counter_view); // this mount is the one the hook now names
  ASSERT_TRUE(wc_test_dispatch("bump", "click"));
  wcomp_render(&left, counter_view);
  wcomp_render(&right, counter_view);
  ASSERT_STR_EQ(texts(left.root), "n=0");
  ASSERT_STR_EQ(texts(right.root), "n=1");
  wcomp_destroy(&left);
  wcomp_destroy(&right);
}

// ---------------------------------------------------------------- 3. live bindings, per row

TEST(an_owned_binding_writes_one_row_and_no_render) {
  const i64 keys[] = {1, 2, 3};
  fill_keys(keys, 3);
  wc_client_mode();
  wc_bind_hook("bump_live", "click", (void *)action_bump_live);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, live_rows_view); // the mount batch has gone out
  buf_reset(&c.cmds.b);

  wc_live_reset();
  ASSERT_TRUE(wc_test_dispatch("bump_live", "click"));
  ASSERT_EQ(wc_live_marks(), 1); // the bridge may skip the render for this action
  // The row declares two bindings (a text and an attribute) on one signal, so the row the
  // action belongs to emits two ops — and the other two rows emit none. With one signal for
  // the whole list this was six ops and every row said 1.
  str cmds = buf_str(&c.cmds.b);
  i32 lines = 0;
  for (size_t i = 0; i < cmds.n; i++)
    if (cmds.p[i] == '\n') lines++;
  ASSERT_EQ(lines, 2);
  ASSERT_STR_CONTAINS(cmds, "text ");
  ASSERT_STR_CONTAINS(cmds, "n=1");
  ASSERT_STR_CONTAINS(cmds, "data-n 1");
  wcomp_destroy(&c);
}

TEST(a_bound_input_per_row_writes_that_rows_text) {
  const i64 keys[] = {1, 2, 3};
  fill_keys(keys, 3);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, field_rows_view);
  // every row's field carries its own node; typing in the second one is that row's text
  WDom *inputs[8];
  i32 ninputs = 0;
  bindings_into(c.root, inputs, 8, &ninputs);
  ASSERT_EQ(ninputs, 3);
  ASSERT_TRUE(wdom_apply_binding(inputs[1], str_lit("second")));
  ASSERT_TRUE(wdom_apply_binding(inputs[2], str_lit("third")));
  wcomp_render(&c, field_rows_view);
  ASSERT_STR_EQ(texts(c.root), "[]|[second]|[third]");
  wcomp_destroy(&c);
}

// ---------------------------------------------------------------- 4. the server half

TEST(the_server_renders_the_initial_values) {
  const i64 keys[] = {7, 8};
  fill_keys(keys, 2);
  WComponent c;
  wcomp_init(&c, nullptr);
  str html = wcomp_html(&c, ssr_view);
  ASSERT_STR_CONTAINS(html, "7/0");
  ASSERT_STR_CONTAINS(html, "8/0");
  wcomp_destroy(&c);
}

// ---------------------------------------------------------------- 5. the failure is loud

state_own(i64, unowned, 0);

TEST(reading_owned_state_outside_a_position_panics) {
  // A read that cannot know which copy it means is a bug, not a default: `state` is the
  // declaration for "one value, anywhere in the page".
  ASSERT_PANICS({ set(unowned, 1); });
  ASSERT_PANICS({ (void)get(unowned); });
}

TEST_MAIN()