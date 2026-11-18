// Emits the DOM command stream for a small app, so tests/bridge_test.js can run
// the real protocol through the JS bridge with a fake document.
#include <webc/server.h>
#include <webc/web.h>

state(int, count, 0);
static void inc(void *ctx, const char *type) {
  (void)ctx; (void)type;
  set(count, get(count) + 1);
}

static void app_view(WBuild *b) {
  (void)b;
  el("div") { class_("app");
    el("h1") { text("Clicks: %d", get(count)); }
    el("button") { on("click", inc, nullptr); text("+1"); }
  }
}

static const char *titles[3] = { "write spec", "build framework", "ship it" };
static i64 key_at(void *ctx, i32 i) { (void)ctx; return i + 1; }
static void item(void *ctx, i32 i, WBuild *b) {
  (void)ctx; (void)b;
  el("li") { text("%s", titles[i]); }
}
static void list_view(WBuild *b) {
  (void)b;
  el("ul") { wkeyed_list(3, key_at, item, nullptr); }
}

// A portal: its children belong to #modal-root, not to the div they are written in.
static void portal_view(WBuild *b) {
  (void)b;
  el("div") { class_("app");
    portal("#modal-root") {
      el("p") { text("dialog"); }
    }
    el("p") { text("page"); }
  }
}

// A live binding: the mount carries the value, and a later write carries one `text` op.
// tests/bridge_test.js applies both streams through the real webc.js, which is what proves
// the op is the protocol a browser already replays — not a second dialect.
state(int, live_n, 0);

static void live_view(WBuild *b) {
  (void)b;
  el("div") {
    class_("live");
    el("p") { text_live("%d", live_n); }
    el("b") { text("static"); }
  }
}

int main(void) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, app_view);
  printf("=== mount\n%s", buf_cstr(&c.cmds.b));

  buf_reset(&c.cmds.b);
  set(count, 41);
  wcomp_render(&c, app_view);
  printf("=== patch\n%s", buf_cstr(&c.cmds.b));

  WComponent l;
  wcomp_init(&l, nullptr);
  wcomp_render(&l, list_view);
  printf("=== list\n%s", buf_cstr(&l.cmds.b));
  printf("=== html\n%s\n", wcomp_html(&l, list_view).p);

  WComponent p;
  wcomp_init(&p, nullptr);
  wcomp_render(&p, portal_view);
  printf("=== portal\n%s", buf_cstr(&p.cmds.b));

  // last, because `wc_client_mode()` changes what every later render emits (listeners)
  wc_client_mode();
  WComponent v;
  wcomp_init(&v, nullptr);
  wcomp_render(&v, live_view);
  printf("=== live_mount\n%s", buf_cstr(&v.cmds.b));
  buf_reset(&v.cmds.b);
  set(live_n, 12); // the binding runs here: no render, one op
  printf("=== live_op\n%s", buf_cstr(&v.cmds.b));
  return 0;
}