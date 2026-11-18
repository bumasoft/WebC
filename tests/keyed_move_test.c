// Keyed lists: what a reorder actually costs.
//
// This suite started life as "a keyed reorder moves nodes instead of re-creating
// them", then spent time as the honest record of the opposite: reuse was
// positional, so a reorder rewrote every row's text and any DOM state inside a
// row (focus, selection, scroll) was lost. That changed: reuse is per key now —
// an item's fragment owns its previous nodes, and a fragment that changes place
// emits a `move` op per element child. The assertions below are the new truth:
// a reorder is four `move` ops, zero `text`, zero `create`, zero `remove`.
#include <webc/test.h>

static const char *kNames[] = {"ada", "barry", "cleo", "dina"};
static i32 kOrder[] = {0, 1, 2, 3};
static i32 kCount = 4;

static i64 key_at(void *ctx, i32 i) {
  (void)ctx;
  return kOrder[i];
}

static void item_at(void *ctx, i32 i, WBuild *b) {
  (void)ctx;
  (void)b;
  el("li") {
    class_("row");
    text("%s", kNames[kOrder[i]]);
  }
}

static void view_list(WBuild *b) {
  (void)b;
  el("ul") {
    class_("list");
    wkeyed_list(kCount, key_at, item_at, nullptr);
  }
}

static str render_patch(WComponent *c) {
  buf_reset(&c->cmds.b);
  wcomp_render(c, view_list);
  return buf_str(&c->cmds.b);
}


static i32 count_of(str s, const char *needle) {
  i32 n = 0;
  for (const char *p = s.p ? s.p : ""; (p = strstr(p, needle)); p += strlen(needle)) n++;
  return n;
}

TEST(a_reorder_moves_every_row_and_touches_nothing_else) {
  kOrder[0] = 0, kOrder[1] = 1, kOrder[2] = 2, kOrder[3] = 3;
  kCount = 4;
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, view_list); // mount: everything is new

  // reverse it: 3, 2, 1, 0
  kOrder[0] = 3, kOrder[1] = 2, kOrder[2] = 1, kOrder[3] = 0;
  str patch = render_patch(&c);
  // correct in the browser: every row's element is moved, not rewritten — the
  // text ops are zero, which is what keeping DOM state across a reorder means
  ASSERT_EQ(count_of(patch, "move "), 4);
  ASSERT_EQ(count_of(patch, "text "), 0);
  ASSERT_EQ(count_of(patch, "create "), 0);
  ASSERT_EQ(count_of(patch, "remove "), 0);
}

TEST(the_same_order_costs_nothing) {
  kOrder[0] = 0, kOrder[1] = 1, kOrder[2] = 2, kOrder[3] = 3;
  kCount = 4;
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, view_list);
  str patch = render_patch(&c);
  ASSERT_EQ(count_of(patch, "text "), 0);
  ASSERT_EQ(count_of(patch, "create "), 0);
  ASSERT_EQ(count_of(patch, "remove "), 0);
}

// A row that leaves is removed, and the rows that stay are untouched. This is the case
// a keyed list exists for, and it already works.
TEST(a_row_that_leaves_does_not_disturb_the_others) {
  kOrder[0] = 0, kOrder[1] = 1, kOrder[2] = 2, kOrder[3] = 3;
  kCount = 4;
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, view_list);
  kOrder[0] = 0, kOrder[1] = 1, kOrder[2] = 2;
  kCount = 3;
  str patch = render_patch(&c);
  // the row's <li> and the text node inside it: two ops, and nothing else touched
  ASSERT_EQ(count_of(patch, "remove "), 2);
  ASSERT_EQ(count_of(patch, "text "), 0);
}

TEST_MAIN()