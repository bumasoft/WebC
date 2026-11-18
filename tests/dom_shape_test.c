// dom_shape: what a re-render does when the *shape* of the tree changes.
//
// The patcher reuses nodes positionally, so a branch that appears or disappears in the
// middle of a view shifts everything after it. These tests pin the two things that must
// stay true whatever the shape does:
//
//   1. the browser is never told to remove a node the new render still has, and
//   2. a node that survives keeps its id — so its listeners, its scroll position and the
//      element the page already has are the same element afterwards.
//
// Written while chasing a real chart that lost its own header element between renders;
// the app "fixed" it by keeping every render the same shape, which is a workaround, not
// a guarantee. If these pass, the guarantee holds and the app can write `if`.
#include <webc/test.h>

state(i32, branches, 0);
state(i32, leaves, 0);

// A tree with a variable branch in the middle and two fixed siblings after it.
static void shape_view(WBuild *b) {
  (void)b;
  el("div") {
    class_("wrap");
    el("h1") {
      class_("title");
      text("head");
    }
    if (get(branches)) {
      el("section") {
        class_("branch");
        for (i32 i = 0; i < get(leaves); i++) {
          el("span") {
            class_("leaf");
            text("leaf %d", (int)i);
          }
        }
      }
    }
    el("p") {
      class_("tail");
      text("tail A");
    }
    el("p") {
      class_("tail");
      text("tail B");
    }
  }
}

// Every `remove <id>` the emitter sent, as a number.
static i32 count_removes(str cmds) {
  i32 n = 0;
  for (size_t i = 0; i + 7 <= cmds.n; i++)
    if (i == 0 || cmds.p[i - 1] == '\n')
      if (memcmp(cmds.p + i, "remove ", 7) == 0) n++;
  return n;
}

// A real walk of the tree the view wrote — first/next, not the id index, which also
// remembers nodes a render dropped.
static i32 count_tag(WDom *n, const char *tag) {
  if (!n) return 0;
  i32 hits = n->tag && strcmp(n->tag, tag) == 0 ? 1 : 0;
  for (WDom *c = n->first; c; c = c->next) hits += count_tag(c, tag);
  return hits;
}

static WDom *find_class(WDom *n, const char *cls) {
  if (!n) return nullptr;
  for (i32 k = 0; k < n->nattrs; k++)
    if (str_eq_c(n->attrs[k].name, "class") && strcmp(n->attrs[k].value.p, cls) == 0) return n;
  for (WDom *c = n->first; c; c = c->next) {
    WDom *hit = find_class(c, cls);
    if (hit) return hit;
  }
  return nullptr;
}

static size_t text_len(WDom *n) { return n->text.n; }

TEST(the_first_render_is_the_whole_tree) {
  set(branches, 0);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, shape_view);
  ASSERT_EQ(count_tag(c.root, "h1"), 1);
  ASSERT_EQ(count_tag(c.root, "p"), 2);
  ASSERT_EQ(count_tag(c.root, "section"), 0);
}

TEST(a_branch_appearing_in_the_middle_keeps_its_siblings) {
  set(branches, 0);
  set(leaves, 3);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, shape_view);
  WDom *h1_before = find_class(c.root, "title");
  ASSERT_NOT_NULL(h1_before);
  buf_reset(&c.cmds.b);

  set(branches, 1);
  wcomp_render(&c, shape_view);
  str patch = buf_str(&c.cmds.b);

  // the branch exists, and the two tails and the header survived it
  ASSERT_EQ(count_tag(c.root, "section"), 1);
  ASSERT_EQ(count_tag(c.root, "span"), 3);
  ASSERT_EQ(count_tag(c.root, "p"), 2);
  WDom *h1_after = find_class(c.root, "title");
  ASSERT_NOT_NULL(h1_after);
  ASSERT_TRUE(h1_after == h1_before); // the same node, not a replacement
  ASSERT_EQ(count_removes(patch), 0); // nothing the new render still has was removed
}

TEST(a_branch_disappearing_in_the_middle_removes_only_the_branch) {
  set(branches, 1);
  set(leaves, 3);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, shape_view);
  buf_reset(&c.cmds.b);

  set(branches, 0);
  wcomp_render(&c, shape_view);
  str patch = buf_str(&c.cmds.b);

  ASSERT_EQ(count_tag(c.root, "section"), 0);
  ASSERT_EQ(count_tag(c.root, "span"), 0);
  ASSERT_EQ(count_tag(c.root, "p"), 2);  // the tails did not go with it
  ASSERT_EQ(count_tag(c.root, "h1"), 1);
  // the section, its three leaves and their three text nodes — and nothing else
  ASSERT_EQ(count_removes(patch), 7);
}

TEST(the_branch_can_change_size_without_touching_the_tails) {
  set(branches, 1);
  set(leaves, 2);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, shape_view);
  WDom *a = find_class(c.root, "tail");
  ASSERT_NOT_NULL(a);
  size_t ta = text_len(a);
  buf_reset(&c.cmds.b);

  set(leaves, 5);
  wcomp_render(&c, shape_view);
  str patch = buf_str(&c.cmds.b);

  ASSERT_EQ(count_tag(c.root, "span"), 5);
  ASSERT_EQ(count_tag(c.root, "p"), 2);
  WDom *b2 = find_class(c.root, "tail");
  ASSERT_NOT_NULL(b2);
  ASSERT_EQ(b2 == a, true);          // same node
  ASSERT_EQ(text_len(b2), ta);      // same text
  ASSERT_EQ(count_removes(patch), 0);
}

TEST_MAIN()