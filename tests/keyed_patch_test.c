// The keyed-list patch path, tested without a browser: two renders of the same
// view, the second with a row appended, and the patch batch must say so — a
// create for the new row, and no remove for a row that is still in the list.
//
// The failure it pins: two keyed `for_each` loops over one collection, hydrated in a
// browser, where the second loop's nodes are removed and rebuilt and the rows the list
// started with fall out of the DOM one render at a time. The ops in a patch batch are pure
// C, so the whole failure is drivable from here.
#include <webc/test.h>
#include <webc/dom.h>

WEBC_MODEL(Thing, (id, i64, .pk = true), (name, Text, .required = true, .max = 20))
Vec(Thing)

typedef struct KPctx {
  VecThing *rows;
} KPctx;

// Two keyed loops over the same collection: the shape a board's lanes, a grouped
// list, or any "one collection, several windows onto it" view has.
static void kp_view(WBuild *b, void *vp) {
  (void)b;
  KPctx *c = vp;
  el("ul") {
    for_each(r, (*c->rows)) {
      el("li") { attr("data-id", fmt("%lld", (long long)r->id)); text("%s", r->name); }
    }
  }
  el("ul") {
    for_each(r, (*c->rows)) {
      el("li") { class_("all"); attr("data-id", fmt("%lld", (long long)r->id)); text("%s", r->name); }
    }
  }
}

static void kp_fill(VecThing *v, i64 n) {
  for (i64 i = 1; i <= n; i++)
    push(*v, ((Thing){.id = i, .name = fmt("thing %lld", (long long)i)}));
}

static str render_twice(i64 first_rows, i64 appended, str *mount_out) {
  static VecThing rows; // static: the vec's storage must outlive both renders
  rows = (VecThing){.desc = &WC_VECDESC_VecThing};
  kp_fill(&rows, first_rows);
  KPctx ctx = {.rows = &rows};

  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, kp_view, &ctx); // mount batch
  if (mount_out) *mount_out = buf_str(&c.cmds.b);

  for (i64 i = 0; i < appended; i++) {
    i64 id = first_rows + i + 1;
    push(rows, ((Thing){.id = id, .name = fmt("thing %lld", (long long)id)}));
  }
  wcomp_render_ctx(&c, kp_view, &ctx); // patch batch
  // the batch buffer dies with the component: keep a copy the assertions can read
  str src = buf_str(&c.cmds.b);
  static char keep[2][8192];
  static i32 which;
  char *out = keep[which++ & 1];
  size_t n = src.n < sizeof keep[0] - 1 ? src.n : sizeof keep[0] - 1;
  memcpy(out, src.p, n);
  out[n] = 0;
  wcomp_destroy(&c);
  return (str){out, n};
}

TEST(one_loop_append_is_a_create) {
  str ops = render_twice(3, 1, nullptr);
  ASSERT_STR_CONTAINS(ops, "create");           // the new row's nodes
  ASSERT_STR_CONTAINS(ops, "thing 4");
  ASSERT_TRUE(!strstr(ops.p, "remove") || !str_contains(ops, str_lit("remove")));
}

TEST(two_loops_one_collection_append) {
  // the failure: the second loop's rows were removed instead of patched
  str ops = render_twice(3, 1, nullptr);
  ASSERT_STR_CONTAINS(ops, "thing 4");
  // a row that is still in the list must not be removed — the removes here were
  // the second loop's li/text nodes for rows 1..3
  i32 removes = 0;
  for (const char *p = ops.p; p && (p = strstr(p, "remove")); p++) removes++;
  ASSERT_EQ(removes, 0);
}

TEST(two_loops_second_render_of_same_data) {
  // no change at all: the patch batch should be (near) empty, not a rebuild
  str ops = render_twice(3, 0, nullptr);
  i32 creates = 0;
  for (const char *p = ops.p; p && (p = strstr(p, "create")); p++) creates++;
  ASSERT_EQ(creates, 0);
}

TEST_MAIN()
