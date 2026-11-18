// One name, several events: bind("card", "click", …) and bind("card", "dragstart", …)
// on a single element. If only the first entry of a name is attached, the second binding
// compiles, renders its data-hook, and never fires — which is what a draggable card that
// also selects on click needs to work.
#include <webc/test.h>

static i32 clicks = 0, drags = 0, drops = 0;

static void h_click(void *ctx, const char *ev) { (void)ctx; (void)ev; clicks++; }
static void h_drag(void *ctx, const char *ev)  { (void)ctx; (void)ev; drags++; }
static void h_drop(void *ctx, const char *ev)  { (void)ctx; (void)ev; drops++; }

static void card_view(WBuild *b) {
  (void)b;
  el("article") { named("card"); text("a card"); }
}

TEST(both_events_of_one_name_fire) {
  wc_bind_hook("card", "click", (void *)h_click);
  wc_bind_hook("card", "dragstart", (void *)h_drag);
  wc_bind_hook("card", "drop", (void *)h_drop);

  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, card_view); // render once so the hooks know their element

  ASSERT_TRUE(wc_test_dispatch("card", "click"));
  ASSERT_TRUE(wc_test_dispatch("card", "dragstart"));
  ASSERT_TRUE(wc_test_dispatch("card", "drop"));
  ASSERT_EQ(clicks, 1);
  ASSERT_EQ(drags, 1);
  ASSERT_EQ(drops, 1);

  // an event the name does not carry answers false, not a wrong fire
  ASSERT_FALSE(wc_test_dispatch("card", "mouseenter"));
  ASSERT_EQ(clicks + drags + drops, 3);
  wcomp_destroy(&c);
}

TEST_MAIN()
