// pager: one page forward, and the Prev button must go back. The Prev hook carries
// data-step of -1; a dispatch that reads the step as 0 moves the page forward again (the
// ops counter does not move, and dispatching by name returns ran=1 while the page stays).
// This is that sequence, natively: render, dispatch, RE-RENDER (as the bridge does after
// every action), dispatch again.
#include <webc/client_test.h>
#include <webc/table.h>

typedef struct {
  Text team;
} PRow;

static PRow g_rows[6] = {{"Harbour"}, {"Lantern"}, {"Meridian"},
                         {"Nine Bells"}, {"Quarry"}, {"Saltmarsh"}};
static i32 g_order[6];

state(i64, sort, 0);
state(i64, page, 0);

action(sort_nop) { set(sort, table_click(0, get(sort))); }

static f64 by_zero(const void *r) {
  (void)r;
  return 0;
}
static const WTableKey COLS[] = {{"Team", by_zero, nullptr}};

static WTable pager_table(void) {
  WTable t;
  table_init(&t, g_rows, 6, sizeof(PRow), g_order);
  table_keys(&t, COLS, WC_ARRLEN(COLS));
  table_state(&t, get(sort), (i32)get(page), 5);
  return t;
}

action(page_flip) {
  WTable t = pager_table();
  set(page, table_page(&t, (i32)get(page) + (i32)ev_attr_i64("data-step")));
}

view(Pager) {
  WTable t = pager_table();
  el("table") {
    table_head(&t, sort_nop);
    for (i32 i = 0; i < table_len(&t); i++) {
      const PRow *r = table_row_typed(PRow, &t, i);
      el("tr") { el("td") { text("%s", r->team); } }
    }
    table_pager(&t, page_flip, "Rows");
  }
}

static void draw(WComponent *c) {
  wcomp_render_ctx(c, view_Pager, nullptr); // what the bridge does after every action
}

TEST(prev_flips_back_after_next) {
  wc_host_reset();
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  set(page, 0);

  draw(&c);                      // page 0: Prev off, Next carries data-step +1
  client_dispatch("page_flip", "click");
  ASSERT_EQ(get(page), 1);         // Next worked

  draw(&c);                      // page 1: Next off, Prev carries data-step -1
  client_dispatch("page_flip", "click");
  ASSERT_EQ(get(page), 0);         // the bug: the page stays 1 (this fails today)

  wcomp_destroy(&c);
}
TEST_MAIN()
