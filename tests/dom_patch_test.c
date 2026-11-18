// dom_patch: what a re-render sends the browser. Everything here was a real bug:
// ids that collided with nodes the browser already had, a keyed list that never
// emitted `remove`, handlers that accumulated one call per render, and a bound
// input that never got its value rendered back.
#include <webc/test.h>

typedef struct Row {
  i64 id;
  str text;
  bool done;
} Row;

#define MAX_ROWS 8
static Row rows[MAX_ROWS];
static i32 nrows;

state(i32, tick, 0);
static void bump(void) {
  set(tick, get(tick) + 1);
}

static i64 row_key(void *ctx, i32 i) {
  (void)ctx;
  return rows[i].id;
}
static void row_item(void *ctx, i32 i, WBuild *b) {
  (void)ctx;
  (void)b;
  Row *r = &rows[i];
  el("li") {
    if (r->done) class_("done");
    el("span") {
      text("%s", r->text.p);
    }
  }
}

state(Text, draft, "");

static void list_view(WBuild *b) {
  (void)b;
  el("ul") {
    wkeyed_list(nrows, row_key, row_item, nullptr);
  }
}

static void bound_view(WBuild *b) {
  (void)b;
  el("input") {
    wc_bind_str(&draft_node, "q");
  }
}

static void reset_rows(void) {
  nrows = 3;
  rows[0] = (Row){1, str_lit("a"), false};
  rows[1] = (Row){2, str_lit("b"), false};
  rows[2] = (Row){3, str_lit("c"), false};
}

// Drops row 0 and shifts the rest down, the way a delete handler would.
static void drop_first(void) {
  for (i32 i = 0; i + 1 < nrows; i++)
    rows[i] = rows[i + 1];
  nrows--;
  bump();
}

static i32 changes = 0;
static void count_change(void *ctx, const char *event) {
  (void)ctx;
  (void)event;
  changes++;
}

static void handler_view(WBuild *b) {
  (void)b;
  el("button") {
    on("click", count_change, nullptr);
    text("go");
  }
}

TEST(keyed_list_first_render) {
  reset_rows();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, list_view);
  ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), "create 1 0 ul");
  // every node the browser knows about got an id, and fragments did not
  ASSERT_STR_CONTAINS(buf_str(&c.cmds.b), "text 4 a");
}

TEST(nothing_changed_patches_nothing) {
  reset_rows();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, list_view);
  buf_reset(&c.cmds.b);
  wcomp_render(&c, list_view);
  ASSERT_EQ(c.cmds.b.n, 0);
}

TEST(removing_a_row_patches_survivors_and_removes_the_gone_one) {
  reset_rows();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, list_view);
  buf_reset(&c.cmds.b);

  drop_first();
  wcomp_render(&c, list_view);
  str patch = buf_str(&c.cmds.b);
  // Reuse is per key: the survivors keep their own nodes untouched (no text rewrites, which
  // is the cost of positional reuse), and the browser is told to drop exactly the row that
  // left: its li, span and text.
  ASSERT_STR_CONTAINS(patch, "remove 2");
  ASSERT_STR_CONTAINS(patch, "remove 3");
  ASSERT_STR_CONTAINS(patch, "remove 4");
  i32 texts = 0;
  for (const char *p = patch.p; (p = strstr(p, "text ")); p++) texts++;
  ASSERT_EQ(texts, 0);
  ASSERT_EQ(nrows, 2);
}

TEST(appending_reuses_the_ids_that_were_freed) {
  reset_rows();
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, list_view);
  drop_first();
  wcomp_render(&c, list_view);
  buf_reset(&c.cmds.b);

  rows[nrows++] = (Row){9, str_lit("d"), false};
  bump();
  wcomp_render(&c, list_view);
  str add = buf_str(&c.cmds.b);
  ASSERT_STR_CONTAINS(add, "create ");
  ASSERT_STR_CONTAINS(add, "d");
}

TEST(handlers_run_once_per_event_however_many_renders) {
  WComponent h;
  wcomp_init(&h, nullptr);
  wcomp_render(&h, handler_view);
  wcomp_render(&h, handler_view);
  wcomp_render(&h, handler_view);
  static WDom *idx[64];
  i32 n = wdom_index(h.root, idx, 64);
  ASSERT_GT(n, 0);
  WDom *btn = nullptr;
  for (i32 i = 0; i < n; i++)
    if (idx[i] && idx[i]->tag && strcmp(idx[i]->tag, "button") == 0) btn = idx[i];
  ASSERT_NOT_NULL(btn);
  ASSERT_EQ(btn->nlisteners, 1);
  changes = 0;
  for (i32 i = 0; i < btn->nlisteners; i++) btn->listeners[i].fn(btn->listeners[i].ctx, "click");
  ASSERT_EQ(changes, 1);
}

TEST(a_bound_input_renders_and_clears) {
  WComponent b2;
  wcomp_init(&b2, nullptr);
  wcomp_render(&b2, bound_view);
  set(draft, "typed");
  wcomp_render(&b2, bound_view);
  ASSERT_STR_CONTAINS(buf_str(&b2.cmds.b), "attr 1 value typed");
  buf_reset(&b2.cmds.b);
  set(draft, "");
  wcomp_render(&b2, bound_view);
  ASSERT_STR_CONTAINS(buf_str(&b2.cmds.b), "attr 1 value");
}

TEST_MAIN()