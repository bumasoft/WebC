// hot_swap_test.c — the two things a hot module swap moves from one wasm instance to the
// next: named state (signals) and the rendered tree (so the first render is a diff).
//
// Both are text on the wire between two modules, so both are testable without a browser:
// a value in, the same value out; a tree in, the same ops out.
#include <webc/test.h>
#include <webc/server.h>
#include <webc/web.h>

state(i64, page_num, 7);
state(Text, query, "unset");

static Buf dump(void) {
  Buf b;
  buf_init(&b, nullptr, 256);
  w_state_dump(&b);
  return b;
}

TEST(signals_cross_by_name) {
  set(page_num, 41);
  set(query, "harbour");
  Buf b = dump();
  ASSERT_TRUE(b.n > 0);
  // a fresh module starts from the declarations
  set(page_num, 0);
  set(query, "unset");
  char warn[64] = {0};
  i32 got = w_state_load((str){b.p, (i64)b.n}, warn, sizeof warn);
  ASSERT_EQ(got, 2);
  ASSERT_EQ(get(page_num), 41);
  ASSERT_STR_EQ(get(query), "harbour");
  ASSERT_TRUE(warn[0] == 0);
  buf_free(&b);
}

TEST(a_signal_the_new_module_does_not_have_is_ignored) {
  char src[64];
  snprintf(src, sizeof src, "gone\t8\t0\n%.8s\n", "\1\0\0\0\0\0\0\0");
  set(page_num, 3);
  char warn[64] = {0};
  i32 got = w_state_load((str){src, (i64)strlen(src)}, warn, sizeof warn);
  ASSERT_EQ(got, 0);
  ASSERT_EQ(get(page_num), 3); // untouched, not zeroed
}

TEST(a_signal_that_changed_shape_is_skipped_and_reported) {
  // a module that grew page_num from 4 to 8 bytes: the old image is not this module's
  // shape, and guessing there would put a truncated number on a live page
  char src[64];
  i32 n = snprintf(src, sizeof src, "page_num\t4\t0\n");
  memcpy(src + n, "\1\0\0\0\n", 5);
  n += 5;
  set(page_num, 5);
  char warn[96] = {0};
  i32 got = w_state_load((str){src, (i64)n}, warn, sizeof warn);
  ASSERT_EQ(got, 0);
  ASSERT_EQ(get(page_num), 5);
  ASSERT_TRUE(warn[0] != 0);
  ASSERT_STR_CONTAINS(str_lit(warn), "page_num");
}

// ---------------------------------------------------------------- the tree

// `n` rows, and optionally the last one renamed: enough shape for a diff test, no more.
static i64 g_rows_n;
static const char *g_rows_last;

static void view_rows(WBuild *b, void *ctx) {
  (void)b;
  el("ul") {
    for (i64 i = 0; i < g_rows_n; i++) {
      el("li") {
        class_("row");
        el("span") {
          if (g_rows_last && i == g_rows_n - 1)
            text(g_rows_last);
          else
            text("item %lld", i);
        }
      }
    }
  }
  (void)ctx;
}

static void render_rows(WComponent *c, i64 n, const char *last) {
  g_rows_n = n;
  g_rows_last = last;
  wcomp_render_ctx(c, view_rows, nullptr);
}

TEST(a_rendered_tree_loads_back_and_diffs_as_a_patch) {
  Arena *a = arena_new(256 * 1024);
  WComponent first = {0};
  wcomp_init(&first, a);
  render_rows(&first, 3, nullptr);
  ASSERT_TRUE(first.root != nullptr);

  Buf t;
  buf_init(&t, nullptr, 1024);
  wdom_tree_dump(&t, first.root);
  ASSERT_TRUE(t.n > 0);

  // the "new module": a fresh component, told what the page already shows
  Arena *b_arena = arena_new(256 * 1024);
  WComponent second = {0};
  wcomp_init(&second, b_arena);
  ASSERT_EQ(wdom_tree_load((str){t.p, (i64)t.n}), 10); // ul + 3*(li + span + text)
  WDom **loaded = nullptr;
  i32 nl = wdom_tree_take(0, &loaded);
  ASSERT_EQ(nl, 10);
  wcomp_adopt(&second, loaded, nl);
  render_rows(&second, 3, nullptr);
  // nothing changed, so the patch is empty: no create, no text
  ASSERT_EQ(second.cmds.b.n, 0);

  buf_free(&t);
  wcomp_destroy(&first);
  wcomp_destroy(&second);
  arena_destroy(a);
  arena_destroy(b_arena);
}

TEST(one_changed_word_is_one_text_op) {
  Arena *a = arena_new(256 * 1024);
  WComponent first = {0};
  wcomp_init(&first, a);
  render_rows(&first, 2, nullptr);
  Buf t;
  buf_init(&t, nullptr, 1024);
  wdom_tree_dump(&t, first.root);

  Arena *b_arena = arena_new(256 * 1024);
  WComponent second = {0};
  wcomp_init(&second, b_arena);
  ASSERT_TRUE(wdom_tree_load((str){t.p, (i64)t.n}) > 0);
  WDom **loaded = nullptr;
  i32 nl = wdom_tree_take(0, &loaded);
  wcomp_adopt(&second, loaded, nl);
  render_rows(&second, 2, "changed");
  const char *cmds = second.cmds.b.p ? second.cmds.b.p : "";
  ASSERT_TRUE(second.cmds.b.n > 0);
  // a patch, not a rebuild: no create ops at all, and the change is a text op
  ASSERT_TRUE(strstr(cmds, "create") == nullptr);
  ASSERT_TRUE(strstr(cmds, "changed") != nullptr);

  buf_free(&t);
  wcomp_destroy(&first);
  wcomp_destroy(&second);
  arena_destroy(a);
  arena_destroy(b_arena);
}

TEST_MAIN()
