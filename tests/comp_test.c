// comp(): a call site is a render position — the fourth answer to "how many copies?".
//
// `state` is one value per page, `state_in` is one per WInst the app threads, `state_own` is
// one per render position (a mounted view, a for_each row). A *component* is the missing
// unit: a part is a plain C function, so calling it pushes no position, and two calls of one
// part in one view share one slot. Every test here is a promise `comp()` makes about that.
#include <webc/test.h>
#include <webc/client_test.h>

// ---------------------------------------------------------------- a stateful part

state_own(i64, hits, 0);
state_own(Text, note, "");
state(bool, open, true); // page state: the test drives the branch from outside a render

action(bump) { set(hits, get(hits) + 1); }
action(bump_row) { set(hits, get(hits) + 1); } // per row: the clicked node owns it

PART(Stepper, (label, Text));
void Stepper(Text label) {
  el("div") {
    class_("step");
    attr("data-label", label);
    el("span") { text("%s=%lld", label, (long long)get(hits)); }
    el("button") { named(fmt("bump-%s", label)); text("+"); }
  }
}

static void two_comps(WBuild *b) {
  (void)b;
  el("main") {
    comp(Stepper, "a");
    comp(Stepper, "b");
  }
}

// the same two calls, *without* comp(): the older, coarser rule, kept on purpose
static void two_parts(WBuild *b) {
  (void)b;
  el("main") {
    Stepper("a");
    Stepper("b");
  }
}

TEST(two_component_calls_are_two_states) {
  WComponent c;
  wcomp_init(&c, nullptr);
  bind("bump-a", "click", bump); // wired first, like client_main: the render records the place
  str first = wcomp_html(&c, two_comps);
  ASSERT_STR_CONTAINS(first, "a=0");
  ASSERT_STR_CONTAINS(first, "b=0");

  client_dispatch("bump-a", "click");

  str after = wcomp_html(&c, two_comps);
  ASSERT_STR_CONTAINS(after, "a=1"); // the instance of the call that was clicked
  ASSERT_STR_CONTAINS(after, "b=0"); // and only that one
  wcomp_destroy(&c);
}

TEST(a_part_without_comp_still_shares_one_slot) {
  WComponent c;
  wcomp_init(&c, nullptr);
  bind("bump-a", "click", bump);
  wcomp_html(&c, two_parts);
  client_dispatch("bump-a", "click");
  str after = wcomp_html(&c, two_parts);
  // The compatibility pin: this is what every part does without comp(), and what an
  // `webc check --wiring` warning names.
  ASSERT_STR_CONTAINS(after, "a=1");
  ASSERT_STR_CONTAINS(after, "b=1");
  wcomp_destroy(&c);
}

TEST(state_survives_a_rerender) {
  WComponent c;
  wcomp_init(&c, nullptr);
  bind("bump-a", "click", bump);
  wcomp_html(&c, two_comps);
  client_dispatch("bump-a", "click");
  wcomp_html(&c, two_comps); // render, patch, render again: the same instance each time
  str third = wcomp_html(&c, two_comps);
  ASSERT_STR_CONTAINS(third, "a=1");
  wcomp_destroy(&c);
}

// ---------------------------------------------------------------- lifecycle

static i32 g_cleanups, g_runs, g_run_cleanups;
static void count_cleanup(void *ctx) {
  (void)ctx;
  g_cleanups++;
}
static void count_run_cleanup(void *ctx) {
  (void)ctx;
  g_run_cleanups++;
}
effect_own(tick) {
  g_runs++;
  on_cleanup(count_run_cleanup, nullptr); // per run: emptied before the next run, and at die
}

PART(Badge);
void Badge(void) {
  mount_own(tick);
  on_cleanup(count_cleanup, nullptr); // per position: registered on the first render only
  el("b") { text("badge"); }
}

static void badges(WBuild *b) {
  (void)b;
  el("main") {
    // the branch is around the *call*: a component that should not exist is not drawn, which
    // is what lets the sweep dispose it (and run its cleanups)
    if (get(open)) {
      comp(Badge);
      comp(Badge);
    }
  }
}

TEST(effect_own_runs_once_per_instance_and_dies_with_it) {
  g_cleanups = g_runs = g_run_cleanups = 0;
  wc_client_mode(); // the bridge's own switch: on_mount belongs to the client half
  set(open, true);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_html(&c, badges);
  w_flush(); // effects started by the render run at the flush point
  ASSERT_EQ(g_runs, 2); // one per component call, not one per page

  wcomp_html(&c, badges); // a re-render: mount_own is a no-op, nobody re-subscribes
  w_flush();
  ASSERT_EQ(g_runs, 2);

  set(open, false);
  wcomp_html(&c, badges); // both components leave the tree
  w_flush();
  ASSERT_EQ(g_cleanups, 2);     // one teardown per position, and none of them twice
  ASSERT_EQ(g_run_cleanups, 2); // the effect's own cleanup ran with it
  ASSERT_EQ(g_runs, 2);
  wcomp_destroy(&c);
}

static i32 g_mounts;
static void counted_mount(void *ctx) {
  (void)ctx;
  g_mounts++;
}

PART(OnlyOnce);
void OnlyOnce(void) {
  on_mount(counted_mount, nullptr);
  el("i") { text("once"); }
}

static void mounts(WBuild *b) {
  (void)b;
  el("main") { comp(OnlyOnce); }
}

TEST(on_mount_runs_after_the_batch_and_only_once) {
  g_mounts = 0;
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_html(&c, mounts);
  ASSERT_EQ(g_mounts, 0); // not during the render: the node does not exist yet
  wc_mount_run();
  ASSERT_EQ(g_mounts, 1);
  wcomp_html(&c, mounts);
  wc_mount_run();
  ASSERT_EQ(g_mounts, 1); // a re-render is not a mount
  wcomp_destroy(&c);
}

TEST(a_queued_mount_that_nothing_runs_is_visible) {
  g_mounts = 0;
  wc_client_mode();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_html(&c, mounts);
  // The suite's runner fails a test that ends with one still queued, so the count has to be
  // readable: a hook nobody runs is a silent no-op, and this is the number that names it.
  ASSERT_EQ(wc_mount_pending(), 1);
  wc_mount_run();
  ASSERT_EQ(wc_mount_pending(), 0);
  ASSERT_EQ(g_mounts, 1);
  wcomp_destroy(&c);
}

// A body that reads a signal re-runs when it changes, and the run's cleanup is what undoes
// the previous run's subscription. Waiting for the instance to die would leak one per run.
static i32 g_watch_runs, g_watch_cleanups;
static void watch_cleanup(void *ctx) {
  (void)ctx;
  g_watch_cleanups++;
}
state_own(i64, watched, 0);
effect_own(watch) {
  g_watch_runs++;
  (void)get(watched);
  on_cleanup(watch_cleanup, nullptr);
}

action(poke_watched) { set(watched, get(watched) + 1); }
PART(Watcher);
void Watcher(void) {
  mount_own(watch);
  el("button") { named("poke"); text("+"); }
  el("s") { text_live("%lld", watched); }
}

static void watchers(WBuild *b) {
  (void)b;
  el("main") { comp(Watcher); }
}

TEST(a_run_cleanup_runs_before_the_next_run) {
  g_watch_runs = g_watch_cleanups = 0;
  wc_bind_hook("poke", "click", (void *)action_poke_watched);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, watchers); // the patch path: `named()` attaches listeners only there
  w_flush();
  ASSERT_EQ(g_watch_runs, 1);
  ASSERT_EQ(g_watch_cleanups, 0);

  ASSERT_TRUE(wc_test_dispatch("poke", "click")); // a write with a position in scope
  w_flush();
  ASSERT_EQ(g_watch_cleanups, 1); // the previous run, undone before the new one
  ASSERT_EQ(g_watch_runs, 2);

  wcomp_destroy(&c);
  ASSERT_EQ(g_watch_cleanups, 2); // and once more when the instance went away
  ASSERT_EQ(g_watch_runs, 2);
}

// ---------------------------------------------------------------- keyed rows

// VecI64 is prelude — a `Vec(i64)` here would be a second declaration of it.
static Veci64 g_rows;

static void fill(const i64 *keys, i32 n) {
  g_rows.n = 0;
  for (i32 i = 0; i < n; i++) push(g_rows, keys[i]);
}

static void rows(WBuild *b) {
  (void)b;
  el("ul") {
    for_each(r, g_rows, *r) {
      el("li") {
        attr_i("data-id", *r);
        comp(Stepper, fmt("r%lld", (long long)*r));
      }
    }
  }
}

// The patch path, not the html one: a keyed list keeps its item table (and therefore its
// per-row instances) only across *renders*; an html render is a fresh tree every time.
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

TEST(a_row_that_moves_keeps_its_components) {
  fill((i64[]){7, 9}, 2);
  WComponent c;
  wcomp_init(&c, nullptr);
  // click row 7 only, by the name the control inside *that* component carries
  wc_bind_hook("bump-r7", "click", (void *)action_bump_row);
  wcomp_render(&c, rows);
  ASSERT_STR_EQ(texts(c.root), "r7=0|+|r9=0|+");

  ASSERT_TRUE(wc_test_dispatch("bump-r7", "click"));
  wcomp_render(&c, rows);
  ASSERT_STR_EQ(texts(c.root), "r7=1|+|r9=0|+");

  fill((i64[]){9, 7}, 2); // the same two rows, reordered: the key is the array value
  wcomp_render(&c, rows);
  ASSERT_STR_EQ(texts(c.root), "r9=0|+|r7=1|+"); // the state moved with its row
  wcomp_destroy(&c);
}

TEST(a_row_that_leaves_takes_its_component_with_it) {
  fill((i64[]){7, 9}, 2);
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, rows);
  i32 with_two = w_node_count(wrt_current());
  fill((i64[]){7}, 1);
  wcomp_render(&c, rows);
  i32 with_one = w_node_count(wrt_current());
  wcomp_render(&c, rows);
  i32 with_one_again = w_node_count(wrt_current());
  ASSERT_TRUE(with_one < with_two); // the instance's nodes went back to the graph
  ASSERT_EQ(with_one, with_one_again); // and the next render does not leak another one
  ASSERT_STR_EQ(texts(c.root), "r7=0|+");
  wcomp_destroy(&c);
}

// ---------------------------------------------------------------- children

PART(Card, (title, Text), (inner, WKids));
void Card(Text title, WKids inner) {
  el("section") {
    class_("card");
    el("h3") { text("%s", title); }
    kids(inner);
  }
}

static void kid_body(void *ctx) {
  el("p") {
    class_("kid");
    // a state_own read in the children block is the *caller's* position, not the card's
    text("%s/%s", (const char *)ctx, get(note));
  }
}

static void cards(WBuild *b) {
  (void)b;
  set(note, "caller");
  el("main") {
    comp(Card, "first", kids_of(kid_body, "one"));
    comp(Card, "second", kids_of(kid_body, "two"));
  }
}

TEST(children_are_the_callers_markup) {
  WComponent c;
  wcomp_init(&c, nullptr);
  str html = wcomp_html(&c, cards);
  ASSERT_STR_CONTAINS(html, "<h3");
  ASSERT_STR_CONTAINS(html, ">first<");
  ASSERT_STR_CONTAINS(html, "one/caller");
  ASSERT_STR_CONTAINS(html, "two/caller");
  wcomp_destroy(&c);
}

// ---------------------------------------------------------------- markup shape

TEST(a_component_emits_no_wrapper_of_its_own) {
  WComponent c;
  wcomp_init(&c, nullptr);
  str html = wcomp_html(&c, two_comps);
  // comp() is a position, not an element: the div of each Stepper is a direct child of main.
  ASSERT_STR_CONTAINS(html, "<main");
  ASSERT_TRUE(str_contains(html, str_lit("<main")) && str_contains(html, str_lit("<div")));
  wcomp_destroy(&c);
}
TEST_MAIN()
