// keyed_big: the render invariant has to count the whole tree, at any size.
//
// `for_each` gives every item a fragment node. On the first render the fragment is created
// through node_new() and counted in `nnodes`; on every later render it is re-attached, and a
// count that skips it while wc_render_assert() budgets its walk by `nnodes + 64` makes the
// two disagree: a keyed list of 65 or more rows renders once and is then reported as a
// "render tree cycle" — 64 rows fine, 65 panicked, and `for_i` (no fragments) never
// affected. On wasm that panic is a trap: the page stops answering after the first click.
//
// Size is what breaks it, so the test renders past the slack (128 rows), twice, and then
// once more after a change that patches only one row.
#include <webc/test.h>

// VecI64 is prelude — a `Vec(i64)` here would be a second declaration of it.
static Veci64 g_rows;   // the row ids: the key of each row
static i64 g_val[256];  // what each row shows, so a change does not change a key

static void keyed_rows(WBuild *b) {
  (void)b;
  el("ul") {
    for_each(r, g_rows) {
      i64 id = *r;
      el("li") {
        attr_i("data-id", id);
        el("span") { text("%lld", g_val[id]); }
      }
    }
  }
}

// The same list without keyed items, as the control: for_i creates no fragment per row, so
// it never approached the budget and never failed. If this one ever starts failing, the
// invariant broke for a different reason.
static void positional_rows(WBuild *b) {
  (void)b;
  el("ul") {
    for_i(i, g_rows) {
      el("li") {
        attr_i("data-id", g_rows.at[i]);
        el("span") { text("%lld", g_val[g_rows.at[i]]); }
      }
    }
  }
}

static void fill(i32 n) {
  g_rows.n = 0;
  for (i32 i = 0; i < n; i++) {
    push(g_rows, (i64)i);
    g_val[i] = i;
  }
}

static i32 lines_of(str s) {
  i32 n = 0;
  for (size_t i = 0; i < s.n; i++)
    if (s.p[i] == '\n') n++;
  return n;
}

TEST(a_keyed_list_of_128_rows_survives_three_renders) {
  fill(128);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, keyed_rows); // the render that counted the fragments
  wcomp_render(&c, keyed_rows); // the second render: the one that must not panic
  ASSERT_EQ((i32)c.renders, 2);
  // … and still patches instead of rebuilding: one row's text changes, one op comes out
  buf_reset(&c.cmds.b);
  g_val[127] = 999;
  wcomp_render(&c, keyed_rows);
  ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), "999");
  ASSERT_EQ(lines_of(buf_str(&c.cmds.b)), 1);
  wcomp_destroy(&c);
}

TEST(a_keyed_list_just_past_the_slack_survives) {
  // 65 is the boundary the bug turned on: 64 fit in wc_render_assert()'s slack, 65 did not.
  for (i32 n = 64; n <= 66; n++) {
    fill(n);
    WComponent c;
    wcomp_init(&c, nullptr);
    wcomp_render(&c, keyed_rows);
    wcomp_render(&c, keyed_rows);
    wcomp_destroy(&c);
    ASSERT_TRUE(true);
  }
}

TEST(a_positional_list_of_128_rows_still_renders) {
  fill(128);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, positional_rows);
  wcomp_render(&c, positional_rows);
  ASSERT_EQ((i32)c.renders, 2);
  wcomp_destroy(&c);
}

TEST(a_keyed_list_that_shrinks_keeps_the_invariant) {
  fill(200);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, keyed_rows);
  g_rows.n = 50; // the sweep disposes 150 item scopes in one render
  wcomp_render(&c, keyed_rows);
  wcomp_render(&c, keyed_rows);
  ASSERT_EQ((i32)c.renders, 3);
  wcomp_destroy(&c);
}
TEST_MAIN()
