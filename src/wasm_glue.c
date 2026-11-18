// SPDX-License-Identifier: Apache-2.0
// WebC — wasm target glue (SPEC §7.1: the same component code runs in the browser).
//
// Only compiled for wasm32: `webc build --target wasm`. The browser bridge
// (assets/webc.js) calls these exports; the C side never touches the DOM and the
// JS side never touches a signal.
#include <webc/web.h>
#include <webc/js.h> // wc_cb_arena_enter/leave: every bridge callback owns an arena

#ifdef __wasm__

#define WC_MAX_VIEWS 8

typedef struct View {
  void (*fn)(WBuild *b, void *ctx);
  void *ctx;
  const char *selector;
  WComponent comp;
  WDom **nodes; // id -> node, sized to the tree (grown, never truncated)
  i32 nnodes, capnodes;
  bool live;
  WInst *inst; // per-instance state for this mount (see webc/react.h): ambient while
               // the view renders and while one of its listeners runs, so `get()`
               // means the same thing in a view, an action, and a test
  i32 id_base; // where this view numbers its nodes (assigned on its first render)
} View;

static View g_views[WC_MAX_VIEWS];
static i32 g_nviews;
static i32 g_high_id; // the highest node id handed out across every view
static WDom **g_all; // id -> node across all views: what an event looks up
static i32 g_all_cap;
static Buf g_cmds; // the batch handed to JS
// Ops the *host verbs* were declared with (net-get, key, doc-title, …). They are emitted
// whenever the app calls a verb — often before the first render, from client_main — so they
// cannot share g_cmds, which every flush resets. Each batch carries them and clears them.
static Buf g_host_ops;

void wc_host_emit(const char *p, size_t n) {
  if (!g_host_ops.p) buf_init(&g_host_ops, nullptr, 1024);
  buf_write(&g_host_ops, p, n);
}

__attribute__((export_name("webc_alloc"))) void *webc_alloc(u32 n) {
  return wm_alloc(n);
}

__attribute__((export_name("webc_free"))) void webc_free(void *p) {
  wm_free(p);
}

// `mount(App, "#root")` in the component source lands here on the wasm target.
__attribute__((export_name("webc_mount"))) void webc_mount_view(i32 idx, const char *sel) {
  (void)idx;
  (void)sel;
}

static void mount_any(void (*view2)(WBuild *b, void *ctx), void *ctx, const char *selector) {
  if (g_nviews >= WC_MAX_VIEWS) {
    wc_limit_once(&g_nviews, "view limit reached: this view never mounts", "WC_MAX_VIEWS", "view");
    return;
  }
  View *v = &g_views[g_nviews++];
  v->fn = view2;
  v->ctx = ctx;
  v->selector = selector ? selector : "#root";
  wcomp_init(&v->comp, nullptr);
  // Every mount gets an instance, even a view that declares no per-instance signal:
  // an empty one costs a struct, and it means `signal_in` never has to be "the API
  // that only works on the server".
  v->inst = wm_alloc(sizeof *v->inst);
  if (v->inst) winst_init(v->inst, nullptr);
}

// Pushing a null instance would shadow a real one further out, so a view mounted
// without state simply does not push anything.
static void inst_enter(WInst *in) {
  if (in) winst_push(in);
}
static void inst_leave(WInst *in) {
  if (in) winst_pop();
}

// … and the same for the render position a node belongs to: an action on a `for_each` row
// reads that row's `state_own` slots, not its neighbour's.
static void own_enter(WInst *in) {
  if (in) winst_own_push(in);
}
static void own_leave(WInst *in) {
  if (in) winst_own_pop();
}

// A plain view is wrapped, so both signatures are accepted without a cast.
static void plain_thunk(WBuild *b, void *ctx) {
  ((void (*)(WBuild *))ctx)(b);
}



// `mount(App, "#root")` in a client file lands here through the macro in dom.h.
void wc_mount(void (*fn)(WBuild *b), const char *selector) {
  mount_any(plain_thunk, (void *)fn, selector);
}

void wc_mount_ctx(void (*fn)(WBuild *b, void *ctx), void *ctx, const char *selector) {
  wc_client_mode(); // from here on a render attaches listeners: this is the browser half
  mount_any(fn, ctx, selector);
}

// The tree, by node id, so an event can find the node it belongs to. It has to run
// after *every* render, not just the first: a node created by a later render (a list
// item, say) is otherwise unreachable and its listeners never fire — which looks
// exactly like a dead button in the browser.
// The id tables grow with the view, because a fixed-size table has a cliff: past its end
// nodes render, look right, and have no listeners — the bridge finds the id out of range
// and drops the event without a word.
static void ensure_tables(i32 need) {
  if (need > g_all_cap) {
    i32 cap = g_all_cap ? g_all_cap : 1024;
    while (cap < need) cap *= 2;
    WDom **na = wm_alloc((size_t)cap * sizeof *na);
    if (!na) return;
    for (i32 i = 0; i < g_all_cap; i++) na[i] = g_all[i];
    for (i32 i = g_all_cap; i < cap; i++) na[i] = nullptr;
    wm_free(g_all);
    g_all = na;
    g_all_cap = cap;
  }
}

static void index_view(View *v) {
  i32 need = wdom_id_space(v->comp.root);
  if (need > v->capnodes) {
    i32 cap = v->capnodes ? v->capnodes : 1024;
    while (cap < need) cap *= 2;
    WDom **na = wm_alloc((size_t)cap * sizeof *na);
    if (!na) return;
    for (i32 i = 0; i < cap; i++) na[i] = nullptr;
    wm_free(v->nodes);
    v->nodes = na;
    v->capnodes = cap;
  }
  ensure_tables(need);
  for (i32 i = 0; i < v->capnodes; i++) v->nodes[i] = nullptr;
  v->nnodes = wdom_index(v->comp.root, v->nodes, v->capnodes);
  for (i32 k = 0; k < v->nnodes; k++)
    if (v->nodes[k]) {
      i32 id = v->nodes[k]->id;
      if (id >= 0 && id < g_all_cap) g_all[id] = v->nodes[k];
      if (id > g_high_id) g_high_id = id;
    }
}

// What a render costs, for the one question that comes up the first time a page feels
// slow: is it the render, or is it something inside it? Whole-view re-render is the model
// (§11a), and it is fine at 2 ms — but a view that walks ten thousand rows is not, and the
// only way to know which one is to measure. WEBC_RENDER_COST=1 prints at most one line a
// second, naming the worst render in it, so a hover does not turn the console into a
// flood. Off unless asked: a shipped page prints nothing.
static f64 g_render_worst;
static const char *g_render_worst_name;
static i64 g_render_report_at;
static i32 g_render_reported;

// Resolved once: two clock reads per render are free, printing is not.
static bool render_cost_on(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("WEBC_RENDER_COST");
    v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
  }
  return v == 1;
}

static void render_cost_report(f64 took, const char *name) {
  if (took > g_render_worst) {
    g_render_worst = took;
    g_render_worst_name = name;
  }
  i64 now = wc_time_ms();
  if (g_render_report_at == 0) g_render_report_at = now + 1000;
  if (now < g_render_report_at || g_render_worst < 4.0) return;
  g_render_report_at = now + 1000;
  if (g_render_reported < 25)
    printf("[webc] slowest render in the last second: %s — %.2f ms\n",
           g_render_worst_name ? g_render_worst_name : "a view", g_render_worst);
  g_render_worst = 0;
  g_render_worst_name = nullptr;
  g_render_reported++;
}

static f64 render_now_ms(void) { return (f64)wc_wall_ns() / 1e6; }

static void render_view(View *v, bool patch) {
  if (v->id_base == 0) v->id_base = g_high_id + 1;
  v->comp.id_base = v->id_base;
  bool cost = render_cost_on();
  f64 t0 = cost ? render_now_ms() : 0;
  inst_enter(v->inst);
  wcomp_render_ctx(&v->comp, v->fn, v->ctx);
  // Effects the render just started (`mount_own`) belong to this batch: flushing after the
  // body and before the batch goes out puts their ops in the same message as the markup.
  w_flush();
  inst_leave(v->inst);
  if (cost) render_cost_report(render_now_ms() - t0, v->selector);
  index_view(v);
  (void)patch;
}

// Every batch says which container it belongs to, before its operations: the stream
// is shared by all the views this module mounts, and an op's parent id 0 means "the
// container". `replace` on a view's first batch is what retires the server's fallback
// markup; `keep` is every patch after it.
static void emit_view_batch(View *v, i32 idx) {
  if (!v->comp.cmds.b.n) return;
  buf_printf(&g_cmds, "mount %d %s ", (int)idx, v->live ? "keep" : "replace");
  for (const char *q = v->selector; q && *q; q++)
    if (*q != '\n' && *q != '\r') buf_putc(&g_cmds, *q);
  buf_putc(&g_cmds, '\n');
  buf_write(&g_cmds, v->comp.cmds.b.p, v->comp.cmds.b.n);
}

// The declarations the app made (on_key, webc_get, on_ready, …) ride along with whatever
// batch goes out next. webc_start() re-renders instead of flushing — its mount batch has
// to say `replace` — and without this drain the declarations sat in the buffer until the
// *first interaction*: the page was up, and not one verb was installed. Keys were dead and
// a stream that on_ready() opens never opened, until something was clicked.
static void drain_host_ops(void) {
  if (g_host_ops.n) {
    buf_write(&g_cmds, g_host_ops.p, g_host_ops.n);
    buf_reset(&g_host_ops);
  }
}

// A reactor module has no `_start`: nothing ran the app's main, which is where
// `mount()` is called. So the first webc_start() runs it exactly once.
__attribute__((weak)) int wc_app_main(void) {
  return 0;
}

__attribute__((export_name("webc_start"))) void webc_start(void) {
  // The command buffer exists before the app does: `client_main` may render (and so write
  // a batch) and then call `js_then`, and a buffer that is initialised *after* all that
  // hands back a pointer to memory the batch was just written into — which showed up as
  // the JSON reply of an interop call arriving as the text of the ops batch.
  if (!g_cmds.p) buf_init(&g_cmds, nullptr, 4096);
  else buf_reset(&g_cmds);
  static bool booted;
  if (!booted) {
    booted = true;
    wc_app_main();
    buf_reset(&g_cmds); // the mount batch is not this start's: it is rendered again below
  }
  for (i32 i = 0; i < g_nviews; i++) {
    // A hot swap: the page already shows this view, so the render diffs against the tree
    // the old module left behind. `live` makes the batch a `keep`, which is what tells the
    // bridge not to empty the container.
    WDom **loaded = nullptr;
    i32 nloaded = wdom_tree_take(i, &loaded);
    if (nloaded > 0) {
      wcomp_adopt(&g_views[i].comp, loaded, nloaded);
      g_views[i].live = true;
    }
    render_view(&g_views[i], false);
    emit_view_batch(&g_views[i], i);
    buf_reset(&g_views[i].comp.cmds.b); // sent: an empty buffer means "nothing new"
    g_views[i].live = true;
  }
  drain_host_ops();
}

// An event arrives from the browser: apply it, run the effects it triggers, and
// re-render each view into a patch batch.
// Diagnostics for `tests/bridge_test.js` and for a human with a console: how many
// listeners does the node with this id have, and was it ever indexed at all?
__attribute__((export_name("webc_arena"))) i32 webc_arena(void) {
  return (i32)(uintptr_t)wc_arena_get();
}

// A node by the id the page knows it by: the bridge sends ids back for fields and events,
// and a host verb that arrives from the bridge has to hand its action the same element a
// click would.
WDom *wc_node_by_id(i32 id) {
  if (id < 0 || id >= g_all_cap) return nullptr;
  WDom *n = g_all[id];
  return (n && !n->dead) ? n : nullptr;
}

__attribute__((export_name("webc_probe"))) i32 webc_probe(i32 id) {
  if (id < 0 || id >= g_all_cap) return -1;
  WDom *n = g_all[id];
  if (!n) return -2;
  return n->nlisteners * 1000 + n->id;
}

// Whether the next flush re-renders. Conservative by default: a timer, a fetch settling, a
// stream frame, a nav — anything that is not an action — sets this to true simply by not
// touching it. Only a dispatch whose listeners *all* declared `action_live` turns it off,
// and then the flush sends the live bindings' ops (`text_live`, `class_live`, …) and no
// view body runs at all.
static bool g_force_render = true;

// The one way a batch reaches the page. Every entry point below ends here, so a promise
// settling re-renders exactly as a click does — the alternative (each entry point
// growing its own copy of this loop) is how the two drifted apart the first time.
//
// A view's batch has two possible authors now: the render (a diff of the whole tree) or a
// live binding (one op per changed node). `cmds.b` is reset after it goes out, so "something
// is in there" always means "this callback wrote it".
void wc_runtime_clear(void) { buf_reset(&g_cmds); }

void wc_runtime_flush(void) {
  bool force = g_force_render;
  g_force_render = true; // the next callback starts conservative again
  buf_reset(&g_cmds);
  for (i32 i = 0; i < g_nviews; i++) {
    View *v = &g_views[i];
    if (force) render_view(v, true);
    // Whatever is in the batch now is this callback's: a render's diff, or the live
    // bindings' ops when nothing structural moved. An empty one sends nothing.
    emit_view_batch(v, i);
    buf_reset(&v->comp.cmds.b);
  }
  // Declarations the app made since the last batch ride along with it: a `webc_get` in
  // client_main must reach the bridge, and client_main runs before any render.
  drain_host_ops();
  // Last, and only now: `on_mount` is "the node exists", and the batch that made it went out
  // above. A hook whose position did not survive the render is skipped by name.
  wc_mount_run();
}


// The event comes in as a pointer *and* a length: the bridge knows the length, and
// reading past the end of a view into linear memory is the kind of thing a NUL that
// happens to be there would hide until it did not.
static void dispatch_inner(i32 id, const char *event, i32 event_len, const char *payload,
                           i32 payload_len) {
  // clear first: an ignored event must not replay the previous batch
  buf_reset(&g_cmds);
  wc_note_interaction(event);
#ifdef WEBC_TRACE_EVENTS
  printf("[webc] dispatch id=%d ev=%.*s node=%p\n", (int)id, (int)event_len, event,
         (void *)(id >= 0 && id < g_all_cap ? g_all[id] : nullptr));
#endif
  if (id < 0 || id >= g_all_cap) return;
  WDom *n = g_all[id];
  if (!n || n->dead) return;   // a node this render removed cannot be clicked
  str ev = (str){event, (size_t)(event_len > 0 ? event_len : 0)};
  str value = (str){payload ? payload : "", (size_t)(payload ? payload_len : 0)};
  if (str_eq_c(ev, "input") || str_eq_c(ev, "change")) wdom_apply_binding(n, value);
  inst_enter((WInst *)n->inst);
  own_enter((WInst *)n->owner);
  // the element an action is running for: ev_attr()/ev_value() read from here, and a
  // positional list is the reason (see the accessors in dom.h)
  wc_ev_enter(n, value);
  wc_live_reset();
  i32 ran = 0;
  for (i32 i = 0; i < n->nlisteners; i++) {
    if (!str_eq(n->listeners[i].event, ev)) continue;
    ran++;
    if (n->listeners[i].fn) n->listeners[i].fn(n->listeners[i].ctx, event);
  }
  wc_ev_leave();
  own_leave((WInst *)n->owner);
  inst_leave((WInst *)n->inst);
  // Every listener that ran promised it only wrote signals: the bindings have already
  // written their ops, and a render would only re-derive what the page already shows.
  g_force_render = !(ran > 0 && wc_live_marks() == ran);
  wc_runtime_flush();
#ifdef WEBC_TRACE_EVENTS
  printf("[webc] cmds %d bytes:\n%.*s", (int)g_cmds.n, (int)g_cmds.n, g_cmds.p);
#endif
}

// Every entry point from the bridge owns an arena for the duration of the callback
// (js.h, "the callback arena"): an event is not a render, and a loop of js() calls in one
// needs somewhere to reclaim.
__attribute__((export_name("webc_dispatch"))) void webc_dispatch(i32 id, const char *event,
                                                               i32 event_len, const char *payload,
                                                               i32 payload_len) {
  Arena *prev = wc_cb_arena_enter("event");
  dispatch_inner(id, event, event_len, payload, payload_len);
  wc_cb_arena_leave(prev);
}

// A page driving the app by name (`WebC.dispatch("theme")`): find the element the view
// named and fire its listeners, with the context they were attached with — so an action
// inside a keyed list still receives its item. The manual entry point for a click that
// comes from a keyboard shortcut, a voice command, or another script.
static i32 action_inner(const char *name, i32 name_len, const char *event, i32 event_len) {
  // Clear first, exactly as webc_dispatch does: an action nobody is listening for must
  // not leave the previous batch in the buffer. `webc_take_cmds` hands the page whatever
  // is there, so without this a name that matches nothing re-applies the last render —
  // the whole tree, a second time, on top of itself (`WebC.dispatch('typo')` and the page
  // doubles).
  buf_reset(&g_cmds);
  str nm = (str){name, (size_t)(name_len > 0 ? name_len : 0)};
  str ev = (str){event, (size_t)(event_len > 0 ? event_len : 0)};
  if (str_eq_c(ev, "")) ev = str_lit("click");
  i32 ran = 0;
  wc_live_reset();
  for (i32 id = 0; id < g_all_cap; id++) {
    WDom *n = g_all[id];
    if (!n || n->dead) continue;
    bool marked = false;
    for (i32 k = 0; k < n->nattrs; k++)
      if (str_eq_c(n->attrs[k].name, "data-hook") && str_eq(n->attrs[k].value, nm)) marked = true;
    if (!marked) continue;
    inst_enter((WInst *)n->inst);
    own_enter((WInst *)n->owner);
    wc_ev_enter(n, (str){0});
    for (i32 k = 0; k < n->nlisteners; k++)
      if (str_eq(n->listeners[k].event, ev)) {
        if (n->listeners[k].fn) n->listeners[k].fn(n->listeners[k].ctx, ev.p);
        ran++;
      }
    wc_ev_leave();
    own_leave((WInst *)n->owner);
    inst_leave((WInst *)n->inst);
  }
  if (ran) {
    g_force_render = !(wc_live_marks() == ran);
    wc_runtime_flush();
  }
  return ran;
}

__attribute__((export_name("webc_action"))) i32 webc_action(const char *name, i32 name_len,
                                                           const char *event, i32 event_len) {
  Arena *prev = wc_cb_arena_enter("action");
  i32 ran = action_inner(name, name_len, event, event_len);
  wc_cb_arena_leave(prev);
  return ran;
}

// `WebC.start({data})` fetches the server's JSON and hands it here before the first
// render — the bridge has done this since it had a `data` option, and there was no C
// side to receive it, so the option loaded a file nothing could read. An app defines
// `void wc_app_data(str json)` (weak here) to seed its state from it.
__attribute__((weak)) void wc_app_data(str json) {
  (void)json;
}

__attribute__((export_name("webc_data"))) void webc_data(const char *json, i32 len) {
  wc_app_data((str){json, (size_t)(len > 0 ? len : 0)});
}

// ---------------------------------------------------------------- hot swap (dev)
//
// A hot module swap keeps the page and replaces the code. Everything that crosses the
// boundary has to be named, so there are two exports per kind of state and no more:
//
//   webc_state_dump_len / webc_state_dump    the old module, just before it is dropped
//   webc_state_load                          the new module, before webc_start()
//   webc_tree_dump_len / webc_tree_dump      what the page currently shows
//   webc_tree_load                           so the first render is a patch, not a rebuild
//
// Signals are named and registered (w_state_register), so their values cross without the
// app doing anything. Anything else opts in with `webc_dev_state_save/load` — a pair of
// weak functions, so an app that does not care never sees them.

extern void webc_dev_state_save(Buf *out) __attribute__((weak));
extern void webc_dev_state_load(str in) __attribute__((weak));

static Buf g_state_out;

__attribute__((export_name("webc_state_dump_len"))) i32 webc_state_dump_len(void) {
  if (!g_state_out.p) buf_init(&g_state_out, nullptr, 4096);
  buf_reset(&g_state_out);
  w_state_dump(&g_state_out);
  if (webc_dev_state_save) {
    // the app's own bytes go in one labelled section, so the reader never has to know
    // what they mean — only where they end
    Buf app;
    buf_init(&app, nullptr, 1024);
    webc_dev_state_save(&app);
    if (app.n > 0) {
      buf_printf(&g_state_out, "#app\t%zu\t0\n", (size_t)app.n);
      buf_write(&g_state_out, app.p, (size_t)app.n);
      buf_putc(&g_state_out, '\n');
    }
    buf_free(&app);
  }
  return (i32)g_state_out.n;
}

__attribute__((export_name("webc_state_dump"))) i32 webc_state_dump(void) {
  return (i32)(uintptr_t)g_state_out.p;
}

__attribute__((export_name("webc_state_load"))) i32 webc_state_load(const char *p, i32 n) {
  if (!p || n <= 0) return 0;
  str in = {(char *)p, (size_t)n};
  // split off the app's section first: w_state_load only understands its own lines
  const char *app = in.p;
  size_t app_len = 0;
  for (const char *q = in.p; q && q < in.p + in.n; q++) {
    if (q[0] == '#' && q[1] == 'a' && q[2] == 'p' && q[3] == 'p') {
      app = q;
      app_len = (size_t)(in.p + in.n - q);
      in.n = (size_t)(q - in.p);
      break;
    }
  }
  char warn[160] = {0};
  i32 got = w_state_load(in, warn, sizeof warn);
  if (warn[0]) printf("[webc] %s\n", warn);
  if (webc_dev_state_load && app_len > 0) {
    const char *nl = memchr(app, '\n', app_len);
    if (nl) webc_dev_state_load((str){(char *)nl + 1, app_len - (size_t)(nl - app) - 1});
  }
  return got;
}

static Buf g_tree_out;

__attribute__((export_name("webc_tree_dump_len"))) i32 webc_tree_dump_len(void) {
  if (!g_tree_out.p) buf_init(&g_tree_out, nullptr, 8192);
  buf_reset(&g_tree_out);
  for (i32 i = 0; i < g_nviews; i++) {
    if (!g_views[i].comp.root) continue;
    buf_printf(&g_tree_out, "view %d\n", (int)i);
    wdom_tree_dump(&g_tree_out, g_views[i].comp.root);
  }
  return (i32)g_tree_out.n;
}

__attribute__((export_name("webc_tree_dump"))) i32 webc_tree_dump(void) {
  return (i32)(uintptr_t)g_tree_out.p;
}

// Load before webc_start(): the views then render against the tree the page already has,
// and their first batch is a diff. Returns the number of nodes taken.
__attribute__((export_name("webc_tree_load"))) i32 webc_tree_load(const char *p, i32 n) {
  if (!p || n <= 0) return 0;
  return wdom_tree_load((str){(char *)p, (size_t)n});
}

__attribute__((export_name("webc_take_cmds"))) i32 webc_take_cmds(void) {
  return (i32)(uintptr_t)g_cmds.p;
}

__attribute__((export_name("webc_cmds_len"))) i32 webc_cmds_len(void) {
  return (i32)g_cmds.n;
}

#endif // __wasm__