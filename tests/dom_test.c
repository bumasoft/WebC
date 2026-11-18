// dom: SSR markup, hydration anchors, command streams, keyed lists.
#include <webc/test.h>

state(int, count, 0);
static void inc(void *ctx, const char *type) { (void)ctx; (void)type; set(count, get(count) + 1); }

static void app_view(WBuild *b) {
  (void)b;
  el("div") {
    class_("app");
    el("h1") { text("Clicks: %d", get(count)); }
    el("button") { on("click", inc, nullptr); text("+1"); }
    el("input") { attr("name", "q"); }
  }
}

// The same view, twice on one page: `signal_in` puts the state in the instance,
// so two cards are two instances and neither can see the other's number.
state_in(int, card_count, 0);
view(Card) {
  el("span") { class_("card"); text("%d", get(card_count)); }
}

static const char *items[3] = { "a", "b", "c" };
static i64 key_at(void *ctx, i32 i) { (void)ctx; return i + 1; }
static void item(void *ctx, i32 i, WBuild *b) { (void)ctx; (void)b; el("li") { text("%s", items[i]); } }
static void list_view(WBuild *b) { (void)b; el("ul") { wkeyed_list(3, key_at, item, nullptr); } }

TEST(two_instances_of_one_view_render_their_own_state) {
  WInst side, page;
  winst_init(&side, nullptr);
  winst_init(&page, nullptr);
  set_in(&side, card_count, 3);
  set_in(&page, card_count, 8);

  WComponent c1, c2;
  wcomp_init(&c1, nullptr);
  wcomp_init(&c2, nullptr);
  str h1, h2;
  with_inst(&side) { h1 = wcomp_html_ctx(&c1, view_Card, nullptr); }
  with_inst(&page) { h2 = wcomp_html_ctx(&c2, view_Card, nullptr); }

  ASSERT_STR_CONTAINS(h1, ">3<");
  ASSERT_STR_CONTAINS(h2, ">8<");
  ASSERT(!str_contains(h1, str_lit(">8<")));
  ASSERT(!str_contains(h2, str_lit(">3<")));

  // …and the instances keep working after the render: an update aims at one card.
  set_in(&page, card_count, 9);
  str h3, h4;
  with_inst(&page) { h3 = wcomp_html_ctx(&c2, view_Card, nullptr); }
  with_inst(&side) { h4 = wcomp_html_ctx(&c1, view_Card, nullptr); }
  ASSERT_STR_CONTAINS(h3, ">9<");
  ASSERT_STR_CONTAINS(h4, ">3<");

  wcomp_destroy(&c1);
  wcomp_destroy(&c2);
  winst_free(&side);
  winst_free(&page);
}

TEST(first_render_creates_the_tree) {
  set(count, 0);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, app_view);
  str first = buf_str(&c.cmds.b);
  ASSERT_STR_CONTAINS(first, "create 1 0 div");
  ASSERT_STR_CONTAINS(first, "attr 1 class app");
  ASSERT_STR_CONTAINS(first, "listen 4 click");
  ASSERT_STR_CONTAINS(first, "text 3 Clicks: 0");
}

TEST(a_signal_write_patches_one_text_node) {
  set(count, 0);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, app_view);   // build
  buf_reset(&c.cmds.b);
  set(count, 1);
  wcomp_render(&c, app_view);   // patch
  ASSERT_STR(buf_str(&c.cmds.b), "text 3 Clicks: 1\n");
}

TEST(no_write_no_dom_work) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, app_view);
  buf_reset(&c.cmds.b);
  wcomp_render(&c, app_view);   // nothing changed
  ASSERT_EQ(c.cmds.b.n, 0);
}

TEST(ssr_carries_hydration_anchors) {
  WComponent l;
  wcomp_init(&l, nullptr);
  str html = wcomp_html(&l, list_view);
  ASSERT_STR_CONTAINS(html, "<ul data-wc=");
  ASSERT_STR_CONTAINS(html, "<li data-wc=");
}

TEST(ssr_void_elements_are_not_closed) {
  set(count, 1);
  WComponent v;
  wcomp_init(&v, nullptr);
  str vhtml = wcomp_html(&v, app_view);
  ASSERT_STR_CONTAINS(vhtml, "<input data-wc=");
  ASSERT_STR_CONTAINS(vhtml, "data-on-click=");
  ASSERT(!str_contains(vhtml, str_lit("</input>")));
  ASSERT_STR_CONTAINS(vhtml, "Clicks: 1");
}

// ---------------------------------------------------------------- the cost of a render

static int g_rows;
static i64 row_key(void *ctx, i32 i) { (void)ctx; return i; }
static void row_item(void *ctx, i32 i, WBuild *b) {
  (void)ctx;
  (void)b;
  el("li") { text("row %d", i); }
}
static void big_view(WBuild *b) {
  (void)b;
  el("ul") { wkeyed_list(g_rows, row_key, row_item, nullptr); }
}

TEST(a_re_render_does_not_grow_the_component_arena) {
  // Allocating the run list, every text copy and every attribute array in the node arena —
  // which lives as long as the component — costs 303 B per render for a single changing text
  // node, i.e. a browser tab that climbs forever.
  WComponent c;
  wcomp_init(&c, nullptr);
  for (int i = 0; i < 200; i++) wcomp_render(&c, app_view);
  size_t warm = arena_high_water(c.nodes);
  for (int i = 0; i < 2000; i++) {
    set(count, i);
    wcomp_render(&c, app_view);
  }
  ASSERT_EQ(arena_high_water(c.nodes), warm);
  wcomp_destroy(&c);
}

TEST(a_big_keyed_list_renders_and_every_node_has_an_addressable_id) {
  // 4 096 ids is a silent cliff: past it the nodes render, look right, and have no
  // listeners, because the bridge checks the id and drops anything out of range.
  g_rows = 3000; // two nodes per row: the id space passes the 4 096 mark
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, big_view);
  i32 space = wdom_id_space(c.root);
  i32 n = 0;
  ASSERT_GT(space, 4096); // the tree really does need more ids than a fixed cap would give
  for (i32 i = 0; i < c.nseq; i++) {
    WDom *nd = c.seq[i];
    if (!nd || nd->type == WD_FRAGMENT || nd->dead) continue;
    ASSERT_GT(nd->id, 0);
    ASSERT_LT(nd->id, space);
    n++;
  }
  ASSERT_GT(n, 4000);
  wcomp_destroy(&c);
}

TEST_MAIN()
