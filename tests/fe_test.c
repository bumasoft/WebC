// fe_test.c — the frontend application layer (SPEC §7.3, §7.4, §13): context,
// resources, the client router, typed forms and the block-shaped keyed list.
#include <webc/test.h>
#include <webc/server.h>
#include <webc/web.h>

// ---------------------------------------------------------------- context

WPROV_KEY(Theme);
WPROV_KEY(User);

TEST(provide_and_use) {
  static str theme = {"dark", 4}, user = {"ada", 3}, dflt = {"light", 5};
  WProv *root = provide(nullptr, &Theme, &theme);
  WProv *inner = provide(root, &User, &user);
  ASSERT_STR_EQ(*(str *)use_ctx_from(inner, &Theme), "dark"); // walks up
  ASSERT_STR_EQ(*(str *)use_ctx_from(inner, &User), "ada");
  ASSERT_STR_EQ(*(str *)use_ctx_or(inner, &Theme, &dflt), "dark");
  wm_free(inner);
  wm_free(root);
}

TEST(use_ctx_missing_panics_with_the_key_name) {
  static str theme = {"dark", 4};
  WProv *p = provide(nullptr, &Theme, &theme);
  ASSERT_PANICS({ use_ctx_from(p, &User); });
  wm_free(p);
}

// The provide/use chain inside a render: a nested partial finds what its parent
// provided, without the app threading it through every signature.
WPROV_KEY(Label);

static void child_partial(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  const str *l = use_ctx_from(wprov_current(), &Label);
  el("span") {
    text(*l);
  }
}

static void parent_view(WBuild *b) {
  (void)b;
  static str label = {"from the parent", 15};
  el("div") {
    wprov_push(&Label, &label);
    child_partial(nullptr, nullptr);
  }
}

TEST(context_flows_into_nested_partials) {
  WComponent c;
  wcomp_init(&c, nullptr);
  str html = wcomp_html(&c, parent_view);
  ASSERT_STR_CONTAINS(html, "from the parent");
  wcomp_destroy(&c);
}

// ---------------------------------------------------------------- stores

WEBC_MODEL(Account, (id, i64, .pk = true), (name, str), (email, str))
WEBC_STORE(acct, Account)

static _Atomic int g_name_runs;
static void watch_name(Wn *self, void *ctx) {
  (void)self;
  (void)ctx;
  (void)sget(acct, name);
  atomic_fetch_add(&g_name_runs, 1);
}

TEST(store_field_reactivity) {
  static str ada = {"ada", 3}, grace = {"grace", 5}, mail = {"a@example.com", 13};
  sset(acct, name, ada);
  sset(acct, email, mail);
  Wn *w = w_new(nullptr, W_EFFECT, "watch_name");
  w_effect_init(w, watch_name, nullptr, "watch_name");
  w_flush();
  i32 after_first = atomic_load(&g_name_runs);
  ASSERT_EQ(after_first, 1);

  // a different field must not re-run the effect that reads only `name`
  sset(acct, email, mail);
  w_flush();
  ASSERT_EQ(g_name_runs, after_first);

  sset(acct, name, grace);
  w_flush();
  ASSERT_EQ(g_name_runs, after_first + 1);
  ASSERT_STR_EQ(sget(acct, name), "grace");
  w_kill(w);
}

// ---------------------------------------------------------------- resource

typedef struct Todo {
  i64 id;
  str title;
} Todo;

static str g_load_title;
static WErrRes load_ok(WCtx *ctx, void *user, void *out, size_t size) {
  (void)ctx;
  (void)user;
  ASSERT_EQ(size, sizeof(Todo));
  Todo *t = out;
  t->id = 7;
  t->title = g_load_title;
  return wok();
}

static WErrRes load_fail(WCtx *ctx, void *user, void *out, size_t size) {
  (void)ctx;
  (void)user;
  (void)out;
  (void)size;
  return wfail(WEBC_ENOTFOUND, str_lit("no such todo"));
}

TEST(resource_runs_and_reports_ok) {
  Arena *a = arena_new(4096);
  g_load_title = str_lit("ship it");
  WRes *r = res_new(a, load_ok, nullptr, sizeof(Todo));
  // idle is not loading. Counting idle as loading ("the view says Loading…") makes the
  // natural "don't fetch twice" guard skip the fetch forever.
  ASSERT_TRUE(res_idle(r));
  ASSERT_FALSE(res_loading(r));
  ASSERT_EQ(res_state(r), WRES_IDLE);
  res_run_now(r);
  ASSERT_TRUE(res_ok(r));
  ASSERT_FALSE(res_loading(r));
  Todo *t = res_data(r);
  ASSERT_NOT_NULL(t);
  ASSERT_EQ(t->id, 7);
  ASSERT_STR_EQ(t->title, "ship it");
  arena_destroy(a);
}

TEST(resource_states_are_four_and_the_guard_works) {
  Arena *a = arena_new(4096);
  g_load_title = str_lit("ship it");
  WRes *r = res_new(a, load_ok, nullptr, sizeof(Todo));
  ASSERT_EQ(res_state(r), WRES_IDLE);
  // the guard every on-demand fetch writes, now correct
  for (i32 i = 0; i < 3; i++) res_ensure(r);
  ASSERT_TRUE(res_ok(r));
  ASSERT_EQ(res_state(r), WRES_OK);
  ASSERT_FALSE(res_loading(r));
  ASSERT_FALSE(res_idle(r));
  arena_destroy(a);
}

TEST(resource_reports_loading_while_a_bridge_fetch_is_in_flight) {
  Arena *a = arena_new(4096);
  WRes *r = res_new(a, nullptr, nullptr, sizeof(Todo)); // no loader: the page drives it
  ASSERT_TRUE(res_idle(r));
  res_start(r);
  ASSERT_TRUE(res_loading(r));
  ASSERT_EQ(res_state(r), WRES_LOADING);
  res_complete(r, (WErrRes){.ok = true}, nullptr);
  ASSERT_EQ(res_state(r), WRES_OK);
  arena_destroy(a);
}

TEST(resource_reports_the_error_string) {
  Arena *a = arena_new(4096);
  WRes *r = res_new(a, load_fail, nullptr, sizeof(Todo));
  res_run_now(r);
  ASSERT_FALSE(res_ok(r));
  ASSERT_TRUE(res_failed(r));
  ASSERT_STR_EQ(res_err_str(r), "no such todo");
  ASSERT_NULL(res_data(r));
  arena_destroy(a);
}

TEST(resource_bridge_completion) {
  Arena *a = arena_new(4096);
  WRes *r = res_new(a, load_fail, nullptr, sizeof(Todo));
  // the browser path: the fetch resolves later and hands the answer back
  Todo got = {.id = 9, .title = str_lit("from the network")};
  res_complete(r, wok(), &got);
  ASSERT_TRUE(res_ok(r));
  ASSERT_EQ(((Todo *)res_data(r))->id, 9);
  arena_destroy(a);
}

// A resource started from a request runs on a green task and the request's
// cancellation reaches the loader.
static _Atomic int g_async_done;

static WErrRes load_counted(WCtx *ctx, void *user, void *out, size_t size) {
  (void)ctx;
  (void)user;
  (void)out;
  (void)size;
  atomic_fetch_add(&g_async_done, 1);
  return wok();
}

TEST(resource_starts_on_a_task) {
  wtask_init(2);
  Arena *a = arena_new(4096);
  WRes *r = res_new(a, load_counted, nullptr, sizeof(Todo));
  res_start(r);
  for (i32 i = 0; i < 200 && !res_ok(r); i++) wg_sleep_ms(2);
  ASSERT_TRUE(res_ok(r));
  ASSERT_EQ(g_async_done, 1);
  arena_destroy(a);
}

// ---------------------------------------------------------------- router

static void page_todo(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
}
static void page_list(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
}

route("/", page_list);
route("/todos", page_list);
route("/todos/new", page_list);
route("/todos/:id", page_todo);
route("/files/*path", page_todo);

TEST(route_literal_beats_param) {
  WPageMatch m = front_route(str_lit("/todos/new"));
  ASSERT_TRUE(m.found);
  ASSERT_STR_EQ(m.pattern, "/todos/new");
  ASSERT_EQ(m.nparams, 0);
}

TEST(route_param) {
  WPageMatch m = front_route(str_lit("/todos/42"));
  ASSERT_TRUE(m.found);
  ASSERT_STR_EQ(m.pattern, "/todos/:id");
  ASSERT_EQ(m.nparams, 1);
  ASSERT_STR_EQ(front_route_param(&m, "id"), "42");
}

TEST(route_wildcard_takes_the_tail) {
  WPageMatch m = front_route(str_lit("/files/a/b/c.txt"));
  ASSERT_TRUE(m.found);
  ASSERT_STR_EQ(front_route_param(&m, "path"), "a/b/c.txt");
}

TEST(route_root_and_miss) {
  ASSERT_TRUE(front_route(str_lit("/")).found);
  ASSERT_TRUE(front_route(str_lit("")).found);
  WPageMatch miss = front_route(str_lit("/nope/deep"));
  ASSERT_FALSE(miss.found);
  // a path deeper than the pattern does not match it
  ASSERT_FALSE(front_route(str_lit("/todos/42/extra")).found);
}

// ---------------------------------------------------------------- forms

WEBC_MODEL(Signup,
           (email, str, .required = true, .max = 40),
           (password, str, .required = true, .min = 8))

TEST(form_submit_runs_the_shared_validators) {
  Signup s = {.email = str_lit("ada@example.com"), .password = str_lit("hunter22")};
  WForm f;
  form_init(&f, Signup_modelof(), &s);
  ASSERT_TRUE(form_submit(&f));
  ASSERT_EQ(form_error_count(&f), 0);
  ASSERT_TRUE(form_field_ok(&f, "email"));
}

TEST(form_reports_per_field_errors) {
  Signup s = {.email = str_lit("ada@example.com"), .password = str_lit("short")};
  WForm f;
  form_init(&f, Signup_modelof(), &s);
  ASSERT_FALSE(form_submit(&f));
  ASSERT_EQ(form_error_count(&f), 1);
  ASSERT_STR_CONTAINS(form_error(&f, "password"), "at least");
  ASSERT_TRUE(form_field_ok(&f, "email"));
  form_reset(&f);
  ASSERT_EQ(form_error_count(&f), 0);
}

// ---------------------------------------------------------------- keyed list

// The block form, over a real Vec.
WEBC_MODEL(Row, (id, i64, .pk = true), (text, str))
WEBC_VEC_DEFINE(VecRowX, vec_rowx, Row)
static VecRowX g_vec;

static void list_view2(WBuild *b) {
  (void)b;
  el("ul") {
    for_each(r, g_vec, r->id) {
      el("li") {
        text(r->text);
      }
    }
  }
}

TEST(for_each_block_keys_and_reuses) {
  g_vec = vec_rowx_with(nullptr);
  vec_rowx_push(&g_vec, (Row){1, str_lit("a")});
  vec_rowx_push(&g_vec, (Row){2, str_lit("b")});
  vec_rowx_push(&g_vec, (Row){3, str_lit("c")});

  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, list_view2);
  str first = wcomp_html(&c, list_view2);
  ASSERT_STR_CONTAINS(first, "<li");
  ASSERT_EQ(c.nseq, 10); // ul + 3 item fragments + 3 li + 3 text (fragments joined
                         // the sequence: per-key reuse and move ops need them)

  // reorder: the same three items, different order — nodes move, none are created
  Row tmp = g_vec.at[0];
  g_vec.at[0] = g_vec.at[1];
  g_vec.at[1] = tmp;
  wcomp_render(&c, list_view2);
  i32 creates = 0;
  for (const char *p = buf_cstr(&c.cmds.b); (p = strstr(p, "create")); p++) creates++;
  ASSERT_EQ(creates, 0);
  ASSERT_EQ(c.nseq, 10);

  // one item leaves: the node for it is removed, the others stay
  g_vec.n = 2;
  wcomp_render(&c, list_view2);
  ASSERT_STR_CONTAINS(str_lit(buf_cstr(&c.cmds.b)), "remove");
  ASSERT_EQ(c.nseq, 7); // ul + 2 fragments + 2 li + 2 text
  wcomp_destroy(&c);
  vec_rowx_free(&g_vec);
}

state(Text, bound_text, "");

static void bound_view(WBuild *b) {
  (void)b;
  el("input") { attr("name", "email"); wc_bind_str(&bound_text_node, "email"); }
}

TEST(a_bound_input_writes_its_signal) {
  set(bound_text, "");
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, bound_view);
  // a single top-level element becomes the root itself (no wrapper fragment)
  WDom *in = c.root && c.root->type == WD_ELEMENT ? c.root : (c.root ? c.root->first : nullptr);
  ASSERT_NOT_NULL(in);
  // the bridge echoes an input event back with the field's value: the write lands in
  // the signal the render subscribed to, which is what makes the field two-way
  ASSERT_TRUE(wdom_apply_binding(in, str_lit("ada@example.com")));
  ASSERT_STR_EQ(get(bound_text), "ada@example.com");
  wcomp_destroy(&c);
}

state(Text, bound_a, "");
state(Text, bound_b, "");

static void two_inputs(WBuild *b) {
  (void)b;
  el("form") {
    el("input") { attr("name", "a"); wc_bind_str(&bound_a_node, "a"); }
    el("input") { attr("name", "b"); wc_bind_str(&bound_b_node, "b"); }
  }
}

// Two bound inputs, two values: the signal must point at the *node's* storage, not at
// one shared buffer. With a shared buffer both fields showed whatever was typed last
// — which is what a browser showed and a single-input test could never catch.
TEST(each_bound_input_keeps_its_own_value) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, two_inputs);
  WDom *a = c.root->first;
  WDom *b = a ? a->next : nullptr;
  ASSERT_NOT_NULL(a);
  ASSERT_NOT_NULL(b);
  ASSERT_TRUE(wdom_apply_binding(a, str_lit("first@example.com")));
  ASSERT_TRUE(wdom_apply_binding(b, str_lit("second@example.com")));
  ASSERT_STR_EQ(get(bound_a), "first@example.com");
  ASSERT_STR_EQ(get(bound_b), "second@example.com");
  // and the first value survives a third write, so the copy is not a rotating buffer
  ASSERT_TRUE(wdom_apply_binding(a, str_lit("third@example.com")));
  ASSERT_STR_EQ(get(bound_a), "third@example.com");
  ASSERT_STR_EQ(get(bound_b), "second@example.com");
  wcomp_destroy(&c);
}

// The value the signal points at must survive the *next* write. A bound input that grows its
// buffer in place (`free` then `alloc`) frees the bytes the signal still points at: a render
// holding the previous value then reads whatever the allocator wrote
// there. In a real app a typed "lcrew duty limits" came back as four NULs and a wedge.
TEST(a_bound_value_stays_readable_across_the_next_write) {
  set(bound_text, "");
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, bound_view);
  WDom *in = c.root && c.root->type == WD_ELEMENT ? c.root : (c.root ? c.root->first : nullptr);
  ASSERT_NOT_NULL(in);
  ASSERT_TRUE(wdom_apply_binding(in, str_lit("lcrew duty limits")));
  Text held = get(bound_text); // what a render, or a view, is reading
  ASSERT_EQ((int)strlen(held), 17);
  ASSERT_TRUE(wdom_apply_binding(in, str_lit("x")));
  // the buffer `held` points at is untouched: same bytes, still terminated
  ASSERT_TRUE(memcmp(held, "lcrew duty limits", 17) == 0);
  ASSERT_EQ((int)held[17], 0);
  // and the signal has moved on to the new value
  ASSERT_STR_EQ(get(bound_text), "x");
  wcomp_destroy(&c);
}

TEST(a_bound_value_survives_growing_and_shrinking) {
  set(bound_text, "");
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, bound_view);
  WDom *in = c.root && c.root->type == WD_ELEMENT ? c.root : (c.root ? c.root->first : nullptr);
  ASSERT_NOT_NULL(in);
  char buf[64];
  for (i32 len = 1; len < 40; len++) {
    memset(buf, 'a' + (len % 26), (size_t)len);
    buf[len] = 0;
    ASSERT_TRUE(wdom_apply_binding(in, (str){buf, (size_t)len}));
    ASSERT_EQ((int)strlen(get(bound_text)), len);
    ASSERT_TRUE(memcmp(get(bound_text), buf, (size_t)len) == 0);
  }
  wcomp_destroy(&c);
}

static void unbound_input(WBuild *b) {
  (void)b;
  // the mistake this reports on: a hook with no field behind it
  el("input") { attr("name", "reason"); input_hook(nobody_bound_this); }
}

TEST(an_input_hook_with_no_field_still_renders) {
  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render(&c, unbound_input);
  WDom *in = c.root && c.root->type == WD_ELEMENT ? c.root : (c.root ? c.root->first : nullptr);
  ASSERT_NOT_NULL(in);
  // no binding was installed, so nothing claims to be reactive — and the render is not
  // damaged by it. wc_input_hook() reports the mistake once, by name.
  for (i32 i = 0; i < in->nattrs; i++) ASSERT_TRUE(!str_eq_c(in->attrs[i].name, "data-bind"));
  wcomp_destroy(&c);
}

TEST_MAIN()
// ---------------------------------------------------------------- exports

// The wasm export name is part of the ABI the page calls, so it is spelled once.
webc_export(probe_export) int probe_export(void) { return 42; }

TEST(webc_export_names_a_function_for_the_page) {
#ifndef __wasm__
  // on the server the macro is empty: the function still exists and still works,
  // which is what lets one file be both halves
  ASSERT_EQ(probe_export(), 42);
#endif
}

