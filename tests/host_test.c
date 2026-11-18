// host: the verbs an app declares instead of writing a JavaScript shim (include/webc/host.h).
//
// A server build records every op the bridge would have received, so a test can read them
// back without a browser — the same calls the browser makes, checked against the protocol
// the wasm build emits. Where a verb's behaviour lives in the page (a stream reconnecting,
// a key matching), the test lives in tests/bridge_test.js.
#include <webc/host.h>
#include <webc/test.h>

static bool has(const char *needle) { return strstr(wc_host_ops(), needle) != nullptr; }

static void on_data(WJs r) {
  (void)r;
}
static void on_data2(WJs r) {
  (void)r;
}

static void action_pal(void *ctx, const char *ev) {
  (void)ctx;
  (void)ev;
}
static void action_field(void *ctx, const char *ev) {
  (void)ctx;
  (void)ev;
}

static void NavPage(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
}
route("/f/:number", NavPage); // a constructor, not a call: file scope, like every app

TEST(a_get_is_one_line_with_the_url_escaped) {
  wc_host_reset();
  i32 id = webc_get(str_lit("/api/briefing/7"), on_data);
  ASSERT_TRUE(id > 0);
  char want[64];
  snprintf(want, sizeof want, "net-get %d /api/briefing/7\n", (int)id);
  ASSERT_STR_EQ(wc_host_ops(), want);
}

TEST(a_url_with_a_space_survives_the_line) {
  // Not hypothetical: a query string with a title in it, and the ops use "the rest of the
  // line" for the value, so the unescaper in the bridge has to be the one that reads it.
  wc_host_reset();
  i32 id = webc_get(str_lit("/search?q=late night"), on_data);
  ASSERT_TRUE(id > 0);
  ASSERT_TRUE(has("net-get "));
  ASSERT_TRUE(has("/search?q=late night\n"));
}

TEST(a_post_carries_the_body_after_the_url) {
  wc_host_reset();
  i32 id = webc_post(str_lit("/api/flights/1/reroute"), str_lit("{\"dest\":\"LHR\"}"), on_data);
  ASSERT_TRUE(id > 0);
  ASSERT_TRUE(has("net-post "));
  ASSERT_TRUE(has(" /api/flights/1/reroute {\"dest\":\"LHR\"}\n"));
}

TEST(a_subscription_names_its_event) {
  wc_host_reset();
  i32 id = webc_subscribe(str_lit("/api/stream"), str_lit("tick"), on_data);
  ASSERT_TRUE(id > 0);
  ASSERT_TRUE(has(" /api/stream tick\n"));
}

TEST(a_subscription_without_an_event_still_says_one) {
  wc_host_reset();
  webc_subscribe(str_lit("/api/stream"), (str){0}, on_data);
  ASSERT_TRUE(has(" /api/stream tick\n"));
}

TEST(a_timer_re_arms_its_own_slot_instead_of_stacking) {
  // "the toast clears itself in five seconds" must not leave a second timer behind that
  // fires into a slot the app has finished with.
  wc_host_reset();
  i32 a = webc_later(5200, on_data);
  wc_host_ops_clear();
  i32 b = webc_later(5200, on_data);
  ASSERT_EQ((int)a, (int)b);
  char want[48];
  snprintf(want, sizeof want, "net-later %d 5200\n", (int)a);
  ASSERT_STR_EQ(wc_host_ops(), want);
}

TEST(a_key_binding_carries_its_spec) {
  wc_host_reset();
  on_key("meta+k", pal);
  ASSERT_TRUE(has("key "));
  ASSERT_TRUE(has(" meta+k\n"));
}

TEST(the_same_key_spec_twice_is_one_binding) {
  wc_host_reset();
  on_key("meta+k", pal);
  i32 after_first = (i32)strlen(wc_host_ops());
  on_key("meta+k", pal);
  ASSERT_EQ((int)strlen(wc_host_ops()), (int)after_first);
}

TEST(a_bound_field_needs_no_protocol_line) {
  // The bridge routes any element carrying data-field; the name → action table is C's. An
  // op here would be one more thing that could arrive before the render that needs it.
  wc_host_reset();
  bind_field("dest", field);
  ASSERT_STR_EQ(wc_host_ops(), "");
}

TEST(the_document_verbs_are_ops_in_order) {
  wc_host_reset();
  doc_attr("data-theme", str_lit("light"));
  doc_title(str_lit("VA101 — Contrail"));
  push_url(str_lit("/f/VA101"));
  replace_url(str_lit("/"));
  focus_hook("field");
  reveal_hook("row");
  field_value("draft", str_lit(""));
  field_value("draft", str_lit("two words"));
  const char *want = "doc-attr data-theme light\n"
                     "doc-title VA101 — Contrail\n"
                     "doc-url push /f/VA101\n"
                     "doc-url replace /\n"
                     "doc-focus field\n"
                     "doc-reveal row\n"
                     "doc-value draft \n"
                     "doc-value draft two words\n";
  ASSERT_STR_EQ(wc_host_ops(), want);
}

TEST(a_null_handler_is_not_a_slot) {
  wc_host_reset();
  ASSERT_EQ((int)webc_get(str_lit("/x"), nullptr), 0);
  ASSERT_STR_EQ(wc_host_ops(), "");
}

TEST(the_slot_table_says_so_when_it_is_full) {
  wc_host_reset();
  i32 last = 0;
  for (i32 i = 0; i < WC_HOST_SLOTS + 4; i++) {
    i32 id = webc_get(str_lit("/x"), i % 2 ? on_data : on_data2);
    if (id) last = id;
  }
  ASSERT_TRUE(last > 0);
  wc_host_ops_clear();
  ASSERT_EQ((int)webc_get(str_lit("/x"), on_data), 0);
}

TEST(a_nav_arrival_carries_the_match_not_just_the_path) {
  // The browser moved; the module has not. The arrival is matched against the same
  // route table the server answered, so client routing stops re-implementing the
  // router by hand: pattern + typed params, not a string walk per app.
  wc_host_reset();
  WNav nav = {.path = str_lit("/f/VA101")};
  WPageMatch m = front_route(nav.path);
  nav.matched = m.found;
  nav.m = &m;
  nav.pattern = m.found ? m.pattern : "";
  ASSERT_TRUE(nav.matched);
  ASSERT_STR_EQ(nav.pattern, "/f/:number");
  ASSERT_STR_EQ(nav_param(&nav, "number"), "VA101");
  ASSERT_EQ((int)nav_param_i64(&nav, "number"), 0); // not a number; 0, not a crash
  // an unmatched arrival is an arrival too: no params, no match, the app decides
  WNav lost = {.path = str_lit("/nowhere")};
  WPageMatch m2 = front_route(lost.path);
  lost.m = &m2;
  ASSERT_TRUE(!m2.found);
  ASSERT_TRUE(!nav_param(&lost, "number").n);
}

TEST(the_page_env_is_a_fact_the_module_owns_before_client_main) {
  // The edition the server rendered, handed over before the first render — the client
  // half's copy of c->page.env, so the theme stops travelling through the data document.
  wc_env_set(false, str_lit("/f/VA101"));
  WClientEnv e = page_env();
  ASSERT_TRUE(!e.dark);
  ASSERT_STR_EQ(e.path, "/f/VA101");
  wc_env_set(true, str_lit("/"));
  e = page_env();
  ASSERT_TRUE(e.dark);
}

// res_live: the frame being parsed and the one on screen are never the same memory.
static str g_live_seen;
static int g_live_state = -1;

static void live_doc(str json) {
  // decode into whatever arena is current — that is res_live's generation — and keep
  // only what must outlive it, the way a real board keeps a signal's string
  JParse p = json_parse(wc_arena_get(), json);
  JVal *t = p.ok ? json_get(p.v, "title") : nullptr;
  g_live_seen = t && t->kind == J_STR ? WC_AS_STR(persist(text_of(t->s))) : str_lit("?");
  // the generation swap is asserted by the frames below: each decodes into
  // the *next* arena, and the old one dies only after the new one is live
}

static void live_state(int s_) { g_live_state = s_; }

static i32 op_slot_id(const char *prefix, const char *line_ends_with) {
  // find the op line that starts with prefix and ends with line_ends_with, take its id
  const char *ops = wc_host_ops();
  size_t plen = strlen(prefix), elen = strlen(line_ends_with);
  for (const char *line = ops; line && *line; line = strchr(line, '\n')) {
    while (*line == '\n') line++;
    size_t ll = strcspn(line, "\n");
    if (ll > plen + elen && memcmp(line, prefix, plen) == 0 &&
        memcmp(line + ll - elen, line_ends_with, elen) == 0)
      return (i32)strtol(line + plen, nullptr, 10);
  }
  return 0;
}

TEST(a_live_document_swaps_generations_and_keeps_the_link) {
  wc_host_reset();
  g_live_state = -1;
  WLive *lv = res_live(&(WLiveCfg){
      .url = str_lit("/api/stream"), .event = str_lit("tick"),
      .doc = live_doc, .on_link = live_state, .gen_bytes = 8192,
  });
  ASSERT_TRUE(lv != nullptr);

  // the seed: the first document lands in a generation of res_live's own
  res_live_read(lv, str_lit("{\"title\": \"one\"}"));
  ASSERT_STR_EQ(g_live_seen, "one");

  // the stream opens (from on_ready), then a frame arrives through the real settle path
  wc_host_ops_clear();
  res_live_open(lv);
  i32 doc_id = op_slot_id("net-sub ", " tick");
  i32 link_id = op_slot_id("net-sub ", " state");
  ASSERT_TRUE(doc_id > 0);
  ASSERT_TRUE(link_id > 0 && link_id != doc_id);

  webc_net_recv(link_id, "{\"state\":\"open\"}", (i32)strlen("{\"state\":\"open\"}"), "", 0);
  ASSERT_EQ(g_live_state, W_LIVE_STREAMING);
  webc_net_recv(doc_id, "{\"title\": \"two\"}", (i32)strlen("{\"title\": \"two\"}"), "", 0);
  ASSERT_STR_EQ(g_live_seen, "two");
  ASSERT_EQ(g_live_state, W_LIVE_STREAMING); // a frame is also "the link is up"

  // a second frame must still arrive: the trampoline survives the settle (the bug that
  // delivered exactly one frame per page, in the zero-JS round)
  webc_net_recv(doc_id, "{\"title\": \"three\"}", (i32)strlen("{\"title\": \"three\"}"), "", 0);
  ASSERT_STR_EQ(g_live_seen, "three");

  // the link drops: the state callback says so, in words the pill can show
  webc_net_recv(link_id, "{\"state\":\"lost\"}", (i32)strlen("{\"state\":\"lost\"}"), "", 0);
  ASSERT_EQ(g_live_state, W_LIVE_LOST);
  // a garbage frame is a document problem, not a link problem: the stream is still up,
  // and the app's doc callback decides what an unreadable document means
  webc_net_recv(doc_id, "not json", (i32)strlen("not json"), "", 0);
  ASSERT_EQ(g_live_state, W_LIVE_STREAMING);
}

TEST_MAIN()