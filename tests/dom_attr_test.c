// dom_attr: an attribute must have a name — at the writer, in the tree, and on the wire.
//
// One nameless attribute pair (`attr 247  `) reaches the browser as `setAttribute('')`,
// which throws, and the bridge's replay loop dies there: a page comes up with its header,
// its rail and its map, and nothing else. The view that produced it was perfectly valid C
// and looked correct from outside, so the pair could only be seen on the wire.
//
// These tests pin the three places that refuse it:
//
//   1. wc_attr()       — the writer, where the node and the attribute index are known;
//   2. wc_render_assert — the tree walk that already guards parent/child/sibling links;
//   3. the emitter     — the batch the bridge consumes (tested from the other side in
//                        tests/bridge_test.js, which feeds a bad line to the real bridge).
#include <webc/test.h>

static void nameless_view(WBuild *b) {
  (void)b;
  el("div") {
    attr("", str_lit("M0 0 L1 1")); // the pair that half-built a real page
    class_("card");
    attr("data-id", str_lit("7"));
  }
}

// A line of the form `attr <id> ` followed by another space: an attribute with no name.
static bool has_nameless_attr(str cmds) {
  for (size_t i = 0; i + 5 <= cmds.n; i++) {
    if (i > 0 && cmds.p[i - 1] != '\n') continue;
    if (memcmp(cmds.p + i, "attr ", 5) != 0) continue;
    size_t j = i + 5;
    while (j < cmds.n && cmds.p[j] >= '0' && cmds.p[j] <= '9') j++;
    if (j + 1 < cmds.n && cmds.p[j] == ' ' && cmds.p[j + 1] == ' ') return true;
  }
  return false;
}

TEST(a_nameless_attribute_never_reaches_the_tree) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, nameless_view);
  ASSERT_NOT_NULL(c.root);
  // class and data-id, not three pairs
  ASSERT_EQ(c.root->nattrs, 2);
  ASSERT_STR_EQ(c.root->attrs[0].name, "class");
  ASSERT_STR_EQ(c.root->attrs[1].name, "data-id");
}

TEST(the_emitted_batch_has_no_nameless_attr) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, nameless_view);
  str cmds = buf_str(&c.cmds.b);
  ASSERT_TRUE(cmds.n > 0);
  ASSERT_TRUE(!has_nameless_attr(cmds));
  // the two real attributes are still there: one dropped pair does not cost the element
  ASSERT_STR_CONTAINS(cmds, "class card");
  ASSERT_STR_CONTAINS(cmds, "data-id 7");
}

// The damaged pair this guard exists for: a name pointer that no longer reads a name. The
// view cannot produce it, so the test writes one by hand — which is exactly what a memory bug
// in the renderer would do.
TEST(the_render_assert_names_a_damaged_pair) {
  WBuild *b = wbuild_begin(arena_new(4096), nullptr);
  ASSERT_NOT_NULL(b);
  el("div") { class_("ok"); }
  ASSERT_NOT_NULL(b->root);
  ASSERT_EQ(b->root->nattrs, 1);
  b->root->attrs[0].name = (str){0};
  ASSERT_PANICS({ wc_render_assert(b); });
}

static void bool_attr_view(WBuild *b) {
  (void)b;
  el("input") {
    wc_bool_attr("disabled", true);
    attr("name", str_lit("reason"));
  }
}

TEST(an_attribute_with_an_empty_value_is_fine) {
  // wc_bool_attr("hidden", true) writes a pair whose value is empty on purpose: the name is
  // what has to exist, and refusing empty values would break every boolean attribute.
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, bool_attr_view);
  ASSERT_NOT_NULL(c.root);
  ASSERT_EQ(c.root->nattrs, 2);
  ASSERT_STR_EQ(c.root->attrs[0].name, "disabled");
  ASSERT_EQ(c.root->attrs[0].value.n, 0);
}

// The §1 root cause, pinned. A reused element keeps its *node* but must rebuild its
// attribute list every render; the listener list was the one thing that survived, and it
// lives in a scratch arena that is reset at the end of the following render. So from the
// third render on, registering a listener wrote into memory this very render had already
// handed out — and what it landed on was the attribute array: `data-hook`'s value came back
// as "click" (the listener's event), the pair after it had no name at all, and the browser
// died on `setAttribute('')`. Intermittent, page-shaped, and invisible from the outside.
static void noop_event(void *ctx, const char *type);

static void listener_view(WBuild *b) {
  (void)b;
  el("div") {
    class_("row");
    el("button") {
      class_("icon-btn");
      type_("button");
      wc_attr_c("data-hook", "theme"); // what named() writes before it registers
      wc_on("click", noop_event, nullptr);
      attr("aria-label", str_lit("switch edition"));
    }
  }
}

static void noop_event(void *ctx, const char *type) {
  (void)ctx;
  (void)type;
}

TEST(a_reused_element_keeps_its_listeners_out_of_the_previous_arena) {
  WComponent c;
  wcomp_init(&c, nullptr);
  // render once for the mount, then patch: the second render reuses the button and its
  // listener array, and the third is where the reset arena came back around
  wcomp_render(&c, listener_view);
  ASSERT_NOT_NULL(c.root);
  WDom *btn = c.root->first;
  ASSERT_NOT_NULL(btn);
  void *first = btn->listeners;
  // The invariant, stated so a regression cannot slip past: a reused element rebuilds its
  // listener list. Keeping the pointer would be a use-after-reset — the arena it came from is
  // rewound at the end of the *following* render, and the next `wc_on` then writes this
  // render's listener into memory that render has already allocated (which is how the
  // attribute list came back with `data-hook`'s value reading "click").
  wcomp_render(&c, listener_view);
  ASSERT_TRUE(btn->listeners != first);
  for (i32 pass = 0; pass < 4; pass++) wcomp_render(&c, listener_view);
  ASSERT_EQ((int)btn->nattrs, 4);
  ASSERT_STR_EQ(btn->attrs[0].name, "class");
  ASSERT_STR_EQ(btn->attrs[1].name, "type");
  ASSERT_STR_EQ(btn->attrs[2].name, "data-hook");
  ASSERT_STR_EQ(btn->attrs[3].name, "aria-label");
  ASSERT_STR_EQ(btn->attrs[3].value, "switch edition");
  ASSERT_EQ((int)btn->nlisteners, 1);
  // and the wire agrees: no pair without a name in the batch the bridge consumes
  ASSERT_TRUE(!has_nameless_attr(buf_str(&c.cmds.b)));
}

TEST_MAIN()
