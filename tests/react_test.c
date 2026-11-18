// react: signals, memos, effects, batching, scopes, no-op writes.
#include <webc/test.h>

state(int, count, 0);
computed(int, doubled) { return get(count) * 2; }

static int effect_runs;
static int last_doubled;

effect(watch_doubled) {
  last_doubled = get(doubled);
  effect_runs++;
}

state(int, scoped, 0);
static int scoped_runs;
static Wn scoped_effect;

// --- per-instance declarations: the same view, two independent copies.
state_in(int, pcount, 0);
computed_in(int, pdoubled) { return get(pcount) * 2; }

static int pi_runs;
static int pi_last;
effect_in(pwatch) {
  pi_last = get(pdoubled);
  pi_runs++;
}

static void scoped_run(Wn *self, void *ctx) {
  (void)self;
  (void)ctx;
  (void)get(scoped);
  scoped_runs++;
}

TEST(memo_reads_before_any_write) {
  ASSERT_EQ(get(doubled), 0);
}

TEST(a_write_runs_the_effect_once) {
  effect_runs = 0;
  set(count, 1);
  ASSERT_EQ(effect_runs, 1);
  ASSERT_EQ(last_doubled, 2);
}

TEST(same_value_is_not_a_change) {
  effect_runs = 0;
  set(count, 1);
  set(count, 1);
  ASSERT_EQ(effect_runs, 0);
}

TEST(batching_coalesces) {
  effect_runs = 0;
  batch {
    set(count, 5);
    set(count, 6);
  }
  ASSERT_EQ(effect_runs, 1);
  ASSERT_EQ(last_doubled, 12);
  ASSERT_EQ(get(doubled), 12);   // and the memo is settled, not stale
}

TEST(a_scope_disposes_what_it_owns) {
  WScope s = {0};
  w_scope_enter(&s, nullptr);
  w_effect_init(&scoped_effect, scoped_run, nullptr, "scoped");
  w_own(&s, &scoped_effect);
  scoped_runs = 0;
  set(scoped, 1);
  int before = scoped_runs;
  ASSERT_EQ(before, 1);
  w_scope_dispose(&s);
  set(scoped, 2);
  ASSERT_EQ(scoped_runs, before);   // later writes do not reach it
}

static void recycled_run(Wn *self, void *ctx) {
  (void)self;
  (void)ctx;
}

TEST(two_instances_of_one_declaration_keep_their_own_state) {
  WInst a, b;
  winst_init(&a, nullptr);
  winst_init(&b, nullptr);
  ASSERT_EQ(get_in(&a, pcount), 0); // created lazily, from the declared initialiser
  set_in(&a, pcount, 1);
  set_in(&b, pcount, 2);
  ASSERT_EQ(get_in(&a, pcount), 1);
  ASSERT_EQ(get_in(&b, pcount), 2);
  ASSERT_EQ(get_in(&a, pdoubled), 2); // and their memos are per-instance too
  ASSERT_EQ(get_in(&b, pdoubled), 4);
  ASSERT_EQ(winst_count(&a), 2); // the signal and the memo, nothing else
  winst_free(&a);
  winst_free(&b);
}

TEST(an_instance_is_ambient_only_inside_with_inst) {
  WInst a, b;
  winst_init(&a, nullptr);
  winst_init(&b, nullptr);
  with_inst(&a) {
    set(pcount, 7);
    ASSERT_EQ(get(pcount), 7);
    with_inst(&b) { // nested: the inner instance wins, and the outer one comes back
      set(pcount, 9);
      ASSERT_EQ(get(pcount), 9);
    }
    ASSERT_EQ(get(pcount), 7);
  }
  ASSERT(winst_current() == nullptr);
  ASSERT_EQ(get_in(&a, pcount), 7);
  ASSERT_EQ(get_in(&b, pcount), 9);
  winst_free(&a);
  winst_free(&b);
}

TEST(a_scoped_effect_runs_for_its_own_instance_only) {
  WInst a, b;
  winst_init(&a, nullptr);
  winst_init(&b, nullptr);
  pi_runs = 0;
  mount_in(pwatch, &a);
  mount_in(pwatch, &b);
  w_flush();
  ASSERT_EQ(pi_runs, 2); // one run each, on mount
  pi_runs = 0;
  pi_last = 0;
  set_in(&a, pcount, 3);
  ASSERT_EQ(pi_runs, 1);  // b did not run
  ASSERT_EQ(pi_last, 6);  // and the effect read a's memo, through a's instance
  ASSERT_EQ(get_in(&b, pdoubled), 0);
  winst_free(&a);
  winst_free(&b);
}

TEST(a_panic_inside_with_inst_gives_the_ambient_instance_back) {
  // A longjmp skips C cleanup handlers, so the instance stack needs the unwinder's help:
  // otherwise the card a view was rendering stays "current" for the next request served
  // by the same task, and the next `iget` answers with the wrong card's number.
  WInst in;
  winst_init(&in, nullptr);
  wc_try_frame f;
  i32 caught = 0;
  if (wc_try_begin(&f)) {
    with_inst(&in) {
      set(pcount, 1);
      wc_panic(__FILE__, __LINE__, "view blew up mid-render");
    }
    wc_try_end(&f);
  } else {
    caught = 1;
  }
  ASSERT_EQ(caught, 1);
  ASSERT(winst_current() == nullptr); // nothing left bound
  ASSERT_STR_EQ(str_lit(wc_panic_message()), "view blew up mid-render");
  winst_free(&in);
}

TEST(reading_a_scoped_signal_outside_an_instance_is_loud) {
  // The failure this rules out is silent: without an instance, `iget` would have to
  // guess which copy is meant, and a view would quietly render another card's number.
  wc_try_frame f;
  i32 code = 0;
  if (wc_try_begin(&f)) {
    (void)get(pcount);
    wc_try_end(&f);
  } else {
    code = 1;
  }
  ASSERT_EQ(code, 1);
  ASSERT(str_contains(str_lit(wc_panic_message()), str_lit("with_inst")));
}

TEST(freeing_an_instance_gives_its_nodes_back) {
  WmStats before = wm_stats();
  for (int round = 0; round < 500; round++) {
    WInst in;
    winst_init(&in, nullptr);
    set_in(&in, pcount, round);
    (void)get_in(&in, pdoubled);
    mount_in(pwatch, &in);
    w_flush();
    winst_free(&in);
  }
  WmStats after = wm_stats();
  ASSERT_EQ(after.heap_used - before.heap_used, (size_t)0);
}

TEST(a_disposed_effect_does_not_grow_the_graph) {
  // A keyed list mounts an effect per row and disposes it when the row goes. The node, its
  // dependency array and its seen array staying allocated for the life of the runtime is
  // ~200 B per row per render, 1.95 MiB per 10 000 cycles measured.
  WmStats before = wm_stats();
  for (int round = 0; round < 2000; round++) {
    WScope s = {0};
    w_scope_enter(&s, nullptr);
    Wn *e = w_new(nullptr, W_EFFECT, "row");
    w_effect_init(e, recycled_run, nullptr, "row");
    w_own(&s, e);
    w_flush();
    w_scope_dispose(&s);
  }
  WmStats after = wm_stats();
  ASSERT_EQ(after.heap_used - before.heap_used, (size_t)0);
  ASSERT_EQ(w_node_count(wrt_current()), 0); // nothing is left behind, dead or alive
}

// Signals are weak globals — one per name, at file scope like every app declares them.
state(bool, tgl_b, false);
state(i64, tgl_i, 0);

TEST(toggle_flips_a_flag_in_place) {
  // `set(x, get(x) ? 0 : 1)` written by hand is an inversion the reader decodes; the
  // macro is the name of the thing. Works for a bool signal and for an i64 flag alike.
  toggle(tgl_b);
  ASSERT_TRUE(get(tgl_b));
  toggle(tgl_b);
  ASSERT_TRUE(!get(tgl_b));
  toggle(tgl_i);
  ASSERT_EQ((int)get(tgl_i), 1);
  toggle(tgl_i);
  ASSERT_EQ((int)get(tgl_i), 0);
}

TEST_MAIN()
