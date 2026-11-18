// PART() + bind_ctx() + ev_keep(): the structure pieces a composed view wants —
// typed callables inside views, and inputs bound straight to context fields.
#include <webc/test.h>
#include <webc/client_test.h>

typedef struct Draft {
  Text body;
  i64  len;
} Draft;
static Draft D;

PART(Avatar, (name, Text), (hue, i64));

void Avatar(Text name, i64 hue) {
  el("span") { class_("avatar"); attr("data-hue", fmt("%lld", (long long)hue)); text("%s", name); }
}

static void view_head(WBuild *b) {
  (void)b;
  el("div") {
    Avatar("ada", 265); // typed call: the compiler checks name/hue
    el("input") { named_field("body"); }
    el("span") { class_("len"); text("%lld", (long long)D.len); }
  }
}

TEST(a_part_renders_its_markup) {
  WComponent c;
  wcomp_init(&c, nullptr);
  str html = wcomp_html(&c, view_head);
  ASSERT_STR_CONTAINS(html, "avatar");
  ASSERT_STR_CONTAINS(html, "ada");
  wcomp_destroy(&c);
}

static void view_body(WBuild *b, void *ctx) {
  (void)b;
  el("span") { class_("echo"); text("%s", ((Draft *)ctx)->body); }
}

CLIENT_TEST(bind_ctx_writes_the_field) {
  bind_ctx("body", D, body);            // quiet: stored, no re-render needed
  client_type("body", "hello part");
  ASSERT_STR_EQ(D.body, "hello part");  // persisted, not borrowed

  // and the view reads what the field stored
  WComponent c;
  wcomp_init(&c, nullptr);
  str html = wcomp_html_ctx(&c, view_body, &D);
  ASSERT_STR_CONTAINS(html, "hello part");
  wcomp_destroy(&c);
}

TEST(ev_keep_survives_the_callback) {
  wc_ev_enter(nullptr, str_lit("kept bytes"));
  Text kept = ev_keep();
  wc_ev_leave();
  ASSERT_STR_EQ(kept, "kept bytes");
}

TEST_MAIN()
