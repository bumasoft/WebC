// SPDX-License-Identifier: Apache-2.0
// WebC — the host verbs: network, keyboard, the document (see include/webc/host.h).
//
// Everything here is a *declaration*: a slot in a small table, and one line of protocol for
// the bridge. The bridge is the only code that touches fetch, EventSource, history or
// document; this file never does, which is why the same source compiles for the server,
// where the verbs are recorded and nothing else happens.
#include <webc/web.h>
#include <webc/host.h>
#include <webc/js.h> // wc_runtime_flush: a live field's re-render goes out now

#include "internal.h"

// ---------------------------------------------------------------- the callback arena
//
// See js.h for why this exists. Two generations rotate: a callback runs with a clean
// arena, and what it allocated is reclaimed when that generation comes round again, which
// is two callbacks later — so the render that follows the callback (same flush) and the
// one after it can still read a string the callback made. A callback that allocates past
// WC_CB_ARENA_MAX is reported once and starts from released chunks: one `js()` per
// iteration of a loop is exactly this shape, and without the report it walks to 2 GiB in
// silence.
#ifdef __wasm__
static Arena *g_cb_gen[2];
static i32 g_cb_live = -1; // the generation in use, -1 before the first callback
static bool g_cb_reported;

static Arena *cb_arena(i32 i) {
  if (!g_cb_gen[i]) g_cb_gen[i] = arena_new_labeled(32 * 1024, "webc callback");
  return g_cb_gen[i];
}

Arena *wc_cb_arena_enter(const char *what) {
  (void)what;
  Arena *prev = wc_arena_get();
  i32 next = g_cb_live == 0 ? 1 : 0;
  Arena *a = cb_arena(next);
  if (arena_bytes(a) > WC_CB_ARENA_MAX) {
    if (!g_cb_reported) {
      g_cb_reported = true;
      printf("[webc] a host callback allocated %zu bytes — that is %.0f KB of interop "
             "results with no reset point inside one callback. Each js() copies its result "
             "into this arena: if this is a loop, hoist the call out of it, or split the "
             "work across webc_later() ticks.\n",
             arena_bytes(a), (double)arena_bytes(a) / 1024.0);
      fflush(stdout);
    }
    arena_release(a); // the next callback starts from nothing, not from a 100 MB chunk
  }
  arena_reset(a);
  g_cb_live = next;
  wc_arena_swap(a);
  return prev;
}

void wc_cb_arena_leave(Arena *prev) {
  Arena *a = wc_arena_get();
  if (a != prev && arena_bytes(a) > WC_CB_ARENA_MAX) {
    if (!g_cb_reported) {
      g_cb_reported = true;
      printf("[webc] a host callback allocated %zu bytes — each js() copies its result "
             "into the callback's arena. If this is a loop, hoist the call out of it, or "
             "split the work across webc_later() ticks.\n",
             arena_bytes(a));
      fflush(stdout);
    }
    arena_release(a);
  }
  wc_arena_swap(prev);
}
#else
// A native build has no bridge: a test allocates in its own arena and there is nothing to
// rotate. The two calls exist so the glue is the same source on both targets.
Arena *wc_cb_arena_enter(const char *what) {
  (void)what;
  return wc_arena_get();
}
void wc_cb_arena_leave(Arena *prev) {
  (void)prev;
}
#endif

typedef enum WSlotKind : u8 {
  SLOT_FREE = 0,
  SLOT_GET,
  SLOT_POST,
  SLOT_SUB,
  SLOT_LATER, // one-shot
  SLOT_KEY,   // a key spec, fired every time it matches
  SLOT_NAV,   // popstate
  SLOT_READY, // once, after the first batch
} WSlotKind;

typedef struct WHostSlot {
  WSlotKind kind;
  JsThen then;                   // a data verb: the result arrives here
  WEventFn on;                   // a fired verb: the action runs with no payload
  const char *key;               // SLOT_KEY: the spec, kept so a repair can re-emit it
  char *url;                     // heap: the caller's arena may be rewound before the reply
  char *aux;                     // the event name, or the POST body
  WRes *res;                     // set by res_get_json(): the resource to complete
  void (*res_fn)(WRes *, WJs);   // …and the app's decision about what the data is
  void *user;                    // a composite's context (res_live's WLive)
  void (*live_fn)(void *, WJs);  // …and its trampoline, so SUB slots can carry one
} WHostSlot;

static WHostSlot g_slots[WC_HOST_SLOTS];

// ---------------------------------------------------------------- the op sink
//
// On wasm the glue owns a buffer that every batch carries (wasm_glue.c); on a server build
// there is no bridge, so the ops are kept here and a test reads them back. One writer, two
// observers, and no second implementation to drift.
#ifdef __wasm__
void wc_host_emit(const char *p, size_t n);
#else
static Buf g_ops;

void wc_host_emit(const char *p, size_t n) {
  if (!g_ops.p) buf_init(&g_ops, nullptr, 512);
  buf_write(&g_ops, p, n);
}

const char *wc_host_ops(void) { return g_ops.p ? g_ops.p : ""; }
void wc_host_ops_clear(void) {
  if (g_ops.p) buf_reset(&g_ops);
}
#endif

// One scratch line, reused: every verb below writes it, ends it, hands it to the sink and
// starts again. A Buf per call would leak a heap block per keystroke.
static Buf g_line;

static Buf *line_begin(void) {
  if (!g_line.p) buf_init(&g_line, nullptr, 256);
  buf_reset(&g_line);
  return &g_line;
}

static void line_end(Buf *b) {
  buf_putc(b, '\n');
  wc_host_emit(b->p, b->n);
}

static char *dup_keep(str s) {
  if (!s.p || !s.n) return nullptr;
  char *p = wm_alloc(s.n + 1);
  for (size_t i = 0; i < s.n; i++) p[i] = s.p[i];
  p[s.n] = '\0';
  return p;
}

static void slot_release(WHostSlot *s) {
  if (!s) return;
  if (s->url) wm_free(s->url);
  if (s->aux) wm_free(s->aux);
  *s = (WHostSlot){0};
}

// 1-based, so 0 is "no slot" and a protocol line never carries an id the bridge has to
// interpret.
static i32 slot_id(const WHostSlot *s) { return s ? (i32)(s - g_slots) + 1 : 0; }

static WHostSlot *slot_new(WSlotKind kind) {
  for (i32 i = 0; i < WC_HOST_SLOTS; i++) {
    if (g_slots[i].kind != SLOT_FREE) continue;
    g_slots[i] = (WHostSlot){.kind = kind};
    return &g_slots[i];
  }
  wc_limit_once(&g_slots, "the host slot table is full: this verb is dropped", "WC_HOST_SLOTS",
                "host verb");
  return nullptr;
}

static WHostSlot *slot_of(i32 id) {
  if (id < 1 || id > WC_HOST_SLOTS) return nullptr;
  WHostSlot *s = &g_slots[id - 1];
  return s->kind == SLOT_FREE ? nullptr : s;
}

// ---------------------------------------------------------------- network

i32 webc_get(str url, JsThen fn) {
  if (!fn || !url.p || !url.n) return 0;
  WHostSlot *s = slot_new(SLOT_GET);
  if (!s) return 0;
  s->then = fn;
  s->url = dup_keep(url);
  Buf *b = line_begin();
  buf_printf(b, "net-get %d ", slot_id(s));
  wc_line_esc(b, url);
  line_end(b);
  return slot_id(s);
}

i32 webc_post(str url, str json_body, JsThen fn) {
  if (!url.p || !url.n) return 0;
  WHostSlot *s = slot_new(SLOT_POST);
  if (!s) return 0;
  s->then = fn;
  s->url = dup_keep(url);
  s->aux = dup_keep(json_body);
  Buf *b = line_begin();
  buf_printf(b, "net-post %d ", slot_id(s));
  wc_line_esc(b, url);
  buf_putc(b, ' ');
  wc_line_esc(b, json_body.n ? json_body : str_lit("{}"));
  line_end(b);
  return slot_id(s);
}

i32 webc_subscribe(str url, str event_name, JsThen fn) {
  if (!fn || !url.p || !url.n) return 0;
  WHostSlot *s = slot_new(SLOT_SUB);
  if (!s) return 0;
  s->then = fn;
  s->url = dup_keep(url);
  s->aux = dup_keep(event_name);
  Buf *b = line_begin();
  buf_printf(b, "net-sub %d ", slot_id(s));
  wc_line_esc(b, url);
  buf_putc(b, ' ');
  wc_line_esc(b, event_name.n ? event_name : str_lit("tick"));
  line_end(b);
  return slot_id(s);
}

void webc_unsubscribe(i32 id) {
  WHostSlot *s = slot_of(id);
  if (!s) return;
  Buf *b = line_begin();
  buf_printf(b, "net-unsub %d", id);
  line_end(b);
  slot_release(s);
}

i32 webc_later(i64 ms, JsThen fn) {
  if (!fn) return 0;
  // One timer per verb: a toast shown again re-arms the same timer instead of leaving a
  // second one in the bridge that will fire into a slot nobody is listening to.
  WHostSlot *s = nullptr;
  for (i32 i = 0; i < WC_HOST_SLOTS; i++)
    if (g_slots[i].kind == SLOT_LATER && g_slots[i].then == fn) s = &g_slots[i];
  if (!s) {
    s = slot_new(SLOT_LATER);
    if (!s) return 0;
    s->then = fn;
  }
  Buf *b = line_begin();
  buf_printf(b, "net-later %d %d", slot_id(s), (int)ms);
  line_end(b);
  return slot_id(s);
}

// A resource whose loader is the network. The GET starts (so `res_loading()` is true from
// this moment) and the app's handler decides what the data is and calls res_complete(): the
// four states and the re-read signal under them stay the framework's, and the app keeps the
// one thing it knows better than we do — the shape of its own payload.
void res_get_json(WRes *res, str url, void (*fn)(WRes *, WJs)) {
  if (!res) return;
  WHostSlot *s = slot_new(SLOT_GET);
  if (!s) {
    res_complete(res, (WErrRes){.ok = false}, nullptr);
    return;
  }
  s->res = res;
  s->res_fn = fn;
  s->url = dup_keep(url);
  res_start(res);
  Buf *b = line_begin();
  buf_printf(b, "net-get %d ", slot_id(s));
  wc_line_esc(b, url);
  line_end(b);
}

// ---------------------------------------------------------------- keyboard, fields

static WEventFn g_field_fns[WC_HOST_SLOTS];
static const char *g_field_names[WC_HOST_SLOTS];
static i32 g_nfields_act;

// No op: an input is found by its `data-field` attribute, so the bridge's one delegated
// `input` listener is enough. This table is the *name → action* half, on the C side.
void wc_bind_field_action(const char *name, WEventFn fn) {
  if (!name || !*name || !fn) return;
  if (g_nfields_act >= WC_HOST_SLOTS) return;
  for (i32 i = 0; i < g_nfields_act; i++)
    if (strcmp(g_field_names[i], name) == 0) {
      g_field_fns[i] = fn; // rebound: the last bind wins, like bind()
      return;
    }
  g_field_names[g_nfields_act] = name;
  g_field_fns[g_nfields_act++] = fn;
}

void wc_named_field(const char *name) { wc_attr_c("data-field", name); }

// ---- context-bound inputs (bind_ctx): the field writes straight into the ctx
typedef struct CtxField {
  const char *name;
  char *base;
  u32 off;
  bool live;
} CtxField;
#define WC_CTX_FIELDS 32
static CtxField g_ctxfields[WC_CTX_FIELDS];
static i32 g_nctxfields;

void wc_bind_ctx(const char *name, void *base, u32 offset, bool live) {
  if (!name || !base) return;
  for (i32 i = 0; i < g_nctxfields; i++)
    if (strcmp(g_ctxfields[i].name, name) == 0) {
      g_ctxfields[i].base = (char *)base; // rebound: the latest context wins
      g_ctxfields[i].off = offset;
      g_ctxfields[i].live = live;
      return;
    }
  if (g_nctxfields >= WC_CTX_FIELDS) {
    wc_limit_once(&g_nctxfields, "ctx-field dropped: too many bound", "WC_CTX_FIELDS", "field");
    return;
  }
  g_ctxfields[g_nctxfields++] = (CtxField){name, (char *)base, offset, live};
}

static bool ctx_field_write(const char *name, size_t nn, const char *value, i32 vn,
                            bool *live) {
  for (i32 i = 0; i < g_nctxfields; i++) {
    CtxField *c = &g_ctxfields[i];
    if (strlen(c->name) != nn || memcmp(c->name, name, nn) != 0) continue;
    str v = {(char *)value, (size_t)(vn > 0 ? vn : 0)};
    Text kept = v.n ? persist(text_of(v)) : (Text) "";
    *(Text *)(c->base + c->off) = kept;
    if (live) *live = c->live;
    return true;
  }
  return false;
}

void wc_on_key(const char *spec, WEventFn fn) {
  if (!spec || !*spec || !fn) return;
  for (i32 i = 0; i < WC_HOST_SLOTS; i++)
    if (g_slots[i].kind == SLOT_KEY && g_slots[i].key && strcmp(g_slots[i].key, spec) == 0)
      return; // the same spec twice is one binding
  WHostSlot *s = slot_new(SLOT_KEY);
  if (!s) return;
  s->on = fn;
  s->key = spec;
  Buf *b = line_begin();
  buf_printf(b, "key %d ", slot_id(s));
  wc_line_esc(b, str_lit(spec));
  line_end(b);
}

// ---------------------------------------------------------------- the document

void doc_attr(const char *name, str value) {
  if (!name || !*name) return;
  Buf *b = line_begin();
  buf_printf(b, "doc-attr %s ", name);
  wc_line_esc(b, value);
  line_end(b);
}

void doc_title(str value) {
  Buf *b = line_begin();
  buf_puts(b, "doc-title ");
  wc_line_esc(b, value);
  line_end(b);
}

static void url_op(const char *how, str path) {
  Buf *b = line_begin();
  buf_printf(b, "doc-url %s ", how);
  wc_line_esc(b, path);
  line_end(b);
}

void push_url(str path) { url_op("push", path); }
void replace_url(str path) { url_op("replace", path); }

str self_path(void) {
  // A read, not an op: the module cannot answer "where am I" a batch later.
  return js_str(js("location.pathname"), "/");
}

// ---------------------------------------------------------------- the page env
//
// The bridge calls webc_env() before webc_start: the edition and the address the server
// rendered, read from the document the module is about to hydrate.

static WClientEnv g_env = {.dark = true, .path = {0}};

WClientEnv page_env(void) { return g_env; }

#if defined(__wasm__)
// export_name is a wasm attribute: on a native build clang warns and ignores it.
__attribute__((export_name("webc_env")))
#endif
void webc_env(bool dark, const char *path, i32 len) {
  g_env.dark = dark;
  g_env.path = (str){path ? path : "", path ? (size_t)(len < 0 ? 0 : len) : 0};
}

#ifndef __wasm__
// A native test sets the env the way the bridge would.
void wc_env_set(bool dark, str path) {
  g_env.dark = dark;
  g_env.path = path;
}
#endif

void focus_hook(const char *name) {
  if (!name || !*name) return;
  Buf *b = line_begin();
  buf_printf(b, "doc-focus %s", name);
  line_end(b);
}

void reveal_hook(const char *name) {
  if (!name || !*name) return;
  Buf *b = line_begin();
  buf_printf(b, "doc-reveal %s", name);
  line_end(b);
}

void field_value(const char *name, str value) {
  if (!name || !*name) return;
  Buf *b = line_begin();
  buf_printf(b, "doc-value %s ", name);
  wc_line_esc(b, value);
  line_end(b);
}

// ---------------------------------------------------------------- the live document
//
// Two generation arenas and a swap: the frame being parsed and the one still on screen
// are never the same memory, so a document can be read in full while the page still
// shows the last one — and the old generation dies only after the new one is live.

struct WLive {
  WLiveCfg cfg;
  Arena *gen[2];
  i32 which;
};

static Arena *live_arena(WLive *lv, i32 i) {
  if (!lv->gen[i])
    lv->gen[i] = arena_new_labeled(lv->cfg.gen_bytes ? lv->cfg.gen_bytes : 128 * 1024,
                                   "res_live");
  return lv->gen[i];
}

WLive *res_live(const WLiveCfg *cfg) {
  if (!cfg || !cfg->doc) return nullptr;
  Arena *keep = arena_new_labeled(sizeof(WLive) + 64, "res_live");
  WLive *lv = arena_alloc(keep, sizeof(WLive));
  memset(lv, 0, sizeof *lv);
  lv->cfg = *cfg;
  // the strings must outlive the caller's arena: copy them into the live's own
  lv->cfg.url = str_dup(keep, cfg->url);
  lv->cfg.event = str_dup(keep, cfg->event.n ? cfg->event : str_lit("tick"));
  return lv;
}

void res_live_read(WLive *lv, str json) {
  if (!lv || !json.n) return;
  i32 next = 1 - lv->which;
  Arena *a = live_arena(lv, next);
  arena_reset(a);
  Arena *prev = wc_arena_swap(a);
  lv->cfg.doc(json); // decodes into the generation; what must outlive it persists
  wc_arena_swap(prev);
  arena_reset(lv->gen[lv->which]); // the generation the page was showing dies now
  lv->which = next;
}

static void live_frame(void *user, WJs r) {
  WLive *lv = user;
  if (r.ok) {
    res_live_read(lv, r.json);
    if (lv->cfg.on_link) lv->cfg.on_link(W_LIVE_STREAMING);
  } else if (lv->cfg.on_link) {
    lv->cfg.on_link(W_LIVE_LOST);
  }
}

static void live_link(void *user, WJs r) {
  WLive *lv = user;
  if (!lv->cfg.on_link) return;
  JVal *v = js_value(r);
  str st = v ? json_as_str(json_get(v, "state")) : (str){0};
  if (st.n) lv->cfg.on_link(str_eq_c(st, "open") ? W_LIVE_STREAMING : W_LIVE_LOST);
}

void res_live_open(WLive *lv) {
  if (!lv) return;
  WHostSlot *doc = slot_new(SLOT_SUB);
  if (doc) {
    doc->user = lv;
    doc->live_fn = live_frame;
    Buf *b = line_begin();
    buf_printf(b, "net-sub %d ", slot_id(doc));
    wc_line_esc(b, lv->cfg.url);
    buf_putc(b, ' ');
    wc_line_esc(b, lv->cfg.event.n ? lv->cfg.event : str_lit("tick"));
    line_end(b);
  }
  WHostSlot *link = slot_new(SLOT_SUB);
  if (link) {
    link->user = lv;
    link->live_fn = live_link;
    Buf *b = line_begin();
    buf_printf(b, "net-sub %d ", slot_id(link));
    wc_line_esc(b, lv->cfg.url);
    buf_putc(b, ' ');
    wc_line_esc(b, str_lit("state"));
    line_end(b);
  }
}

str nav_param(const WNav *nav, const char *name) {
  if (!nav || !nav->matched || !nav->m) return (str){0};
  return front_route_param((WPageMatch *)nav->m, name);
}

i64 nav_param_i64(const WNav *nav, const char *name) {
  str v = nav_param(nav, name);
  i64 out = 0;
  str_to_i64(v, &out);
  return out;
}

void wc_on_nav(void *fn) {
  WHostSlot *s = slot_new(SLOT_NAV);
  if (!s) return;
  s->on = (WEventFn)fn;
  Buf *b = line_begin();
  buf_printf(b, "nav %d", slot_id(s));
  line_end(b);
}

void wc_on_ready(void *fn) {
  WHostSlot *s = slot_new(SLOT_READY);
  if (!s) return;
  s->on = (WEventFn)fn;
  Buf *b = line_begin();
  buf_printf(b, "ready %d", slot_id(s));
  line_end(b);
}

// ---------------------------------------------------------------- arriving from the page
//
// Three exports, not nine: a *data* verb is settled with a value, a *fired* verb is not, and
// a field is a third shape (a name and its text). All three end the same way — the handler
// writes signals, and the batch that follows is the one the bridge applies.
// Logic, not platform: the bridge settles a slot the same way a native test playing the
// bridge does. A composite built on the verbs (res_live) rides along by trampoline.
static void slot_settle(WHostSlot *s, WJs r) {
  i32 id = slot_id(s);
  WRes *res = s->res;
  void (*res_fn)(WRes *, WJs) = s->res_fn;
  JsThen then = s->then;
  void (*live_fn)(void *, WJs) = s->live_fn;
  void *user = s->user;
  bool one_shot = s->kind == SLOT_GET || s->kind == SLOT_POST || s->kind == SLOT_LATER;
  s->then = nullptr; // cleared before the call: a handler may register another verb
  s->res_fn = nullptr;
  s->live_fn = nullptr;
  if (res_fn)
    res_fn(res, r);
  else if (then)
    then(r);
  else if (live_fn)
    live_fn(user, r); // res_live's frame or link trampoline
  // A subscription is not one-shot: the next frame must find its handler where it left
  // it. Everything else (get, post, later) releases the slot below — this restore is
  // what makes a stream keep feeding the app instead of delivering exactly one frame.
  if (s->kind == SLOT_SUB) {
    if (!s->then) s->then = then;
    if (!s->res_fn) s->res_fn = res_fn;
    if (!s->live_fn) s->live_fn = live_fn;
  }
  if (one_shot) {
    WHostSlot *again = slot_of(id);
    if (again && !again->then && !again->res_fn) slot_release(again);
  }
}

// The bridge's answer door: a reply or a frame reaches the module as bytes and the slot
// that asked for it is settled. wasm: exported. native: the same function, so a test can
// play the bridge without a browser.
void webc_net_recv(i32 id, const char *json, i32 len, const char *err, i32 err_len) {
  WHostSlot *s = slot_of(id);
  if (!s) return;
  Arena *a = wc_arena_get();
  size_t n = len > 0 ? (size_t)len : 0;
  size_t en = err_len > 0 ? (size_t)err_len : 0;
  // Copied out of the bridge's buffer before the handler can make another call and free it.
  char *copy = arena_alloc(a, n + 1);
  for (size_t i = 0; i < n; i++) copy[i] = json ? json[i] : '\0';
  copy[n] = '\0';
  char *ecopy = arena_alloc(a, en + 1);
  for (size_t i = 0; i < en; i++) ecopy[i] = err ? err[i] : '\0';
  ecopy[en] = '\0';
  WJs r = en ? (WJs){.ok = false, .kind = JS_UNDEF, .error = (str){ecopy, en}}
             : wc_js_decode(a, (str){copy, n});
  slot_settle(s, r);
#ifdef __wasm__
  wc_runtime_flush(); // the handler wrote signals; the batch it earned goes out now
#endif
}

// A popstate arrives with the address it arrived at: the nav slot's action gets a WNav
// carrying the path *and* the match against the route table — the same table the server
// answered — so client routing stops re-implementing the router by hand.
#ifdef __wasm__
static void fire_nav_inner(i32 id, const char *path, i32 len) {
  WHostSlot *s = slot_of(id);
  if (!s || s->kind != SLOT_NAV || !s->on) return;
  WNav nav = {.path = {path ? path : "", path ? (size_t)(len < 0 ? 0 : len) : 0}};
  WPageMatch m = front_route(nav.path);
  nav.matched = m.found;
  nav.m = &m;
  nav.pattern = m.found ? m.pattern : "";
  // `on` is a view macro (dom.h); through an event-function pointer the member keeps
  // its name without tripping the preprocessor on member access.
  WEventFn fn = s->on;
  fn(&nav, "nav");
  wc_runtime_flush();
}

__attribute__((export_name("webc_fire_nav"))) void webc_fire_nav(i32 id, const char *path,
                                                                 i32 len) {
  Arena *prev = wc_cb_arena_enter("nav");
  fire_nav_inner(id, path, len);
  wc_cb_arena_leave(prev);
}

__attribute__((export_name("webc_net_recv"))) void webc_net_recv_wasm(i32 id,
                                                                      const char *json,
                                                                      i32 len, const char *err,
                                                                      i32 err_len) {
  Arena *prev = wc_cb_arena_enter("net");
  webc_net_recv(id, json, len, err, err_len);
  wc_cb_arena_leave(prev);
}

// `node` is the element the slot is firing for, or -1 for "nothing in particular". A key
// binding is the reason it exists: `alt+arrowdown` is a command about the card the reader is
// standing on, and `ev_attr("data-id")` has to read that card — so the focused element enters
// as the event's node exactly as it would for a click. A timer has no element and passes -1.
static void fire_inner(i32 id, i32 node) {
  WHostSlot *s = slot_of(id);
  if (!s) return;
  // A timer is a data verb whose answer is "it happened": it settles like one, which is
  // also what releases the slot. Reading only `on` here meant webc_later()'s handler was
  // never called — the toast that cleared itself never did.
  if (!s->on && s->then) {
    slot_settle(s, (WJs){.ok = true, .kind = JS_NULL});
    wc_runtime_flush();
    return;
  }
  if (!s->on) return;
  WEventFn fn = s->on;
  bool one_shot = s->kind == SLOT_READY;
  if (one_shot) s->on = nullptr;
  WDom *el = wc_node_by_id(node);
  if (el) wc_ev_enter(el, str_lit("host"));
  fn(nullptr, "host");
  if (el) wc_ev_leave();
  if (one_shot) {
    WHostSlot *again = slot_of(id);
    if (again) slot_release(again);
  }
  wc_runtime_flush();
}

__attribute__((export_name("webc_fire"))) void webc_fire(i32 id, i32 node) {
  Arena *prev = wc_cb_arena_enter("timer");
  fire_inner(id, node);
  wc_cb_arena_leave(prev);
}

#endif // __wasm__ (the fire/field exports above are wasm-only)

// The one field dispatch: the wasm export and the native test hands both land
// here, so a field can never behave differently under test than in the page.
static void wc_field_dispatch(i32 node, const char *name, i32 name_len, const char *value,
                              i32 value_len) {
  size_t nn = name_len > 0 ? (size_t)name_len : 0;
  WEventFn fn = nullptr;
  for (i32 i = 0; i < g_nfields_act; i++)
    if (strlen(g_field_names[i]) == nn && memcmp(g_field_names[i], name, nn) == 0) {
      fn = g_field_fns[i];
      break;
    }
  if (!fn) {
    // bind_ctx: the field writes straight into a context field, persisted
    bool live = false;
    if (ctx_field_write(name, nn, value, value_len, &live)) {
#ifdef __wasm__
      if (live) wc_runtime_flush(); // the list follows the keystroke; a quiet
                                    // field costs no re-render at all
#endif
      return;
    }
    // A field the app never bound is not an error: the page may have a plain search
    // box in someone else's markup. But a name that matches a binding once you take
    // one pair of quotes off is the signature of a hand-written attribute with
    // quotes in the value — an app shipped with a dead input deserves the one line.
    char stripped[96];
    if (nn >= 2 && nn + 2 < sizeof stripped && name[0] == '"' && name[nn - 1] == '"') {
      memcpy(stripped, name + 1, nn - 2);
      stripped[nn - 2] = 0;
      for (i32 i = 0; i < g_nfields_act; i++)
        if (strcmp(g_field_names[i], stripped) == 0) {
          char msg[192];
          snprintf(msg, sizeof msg,
                   "webc: a field named \"%s\" arrived wrapped in quotes and no action "
                   "is bound to it — bind_field(\"%s\", …) is. An attribute written "
                   "by hand with quotes in the value does this",
                   stripped, stripped);
          wc_limit_once(stripped, msg, "quoted-field", "field");
          return;
        }
    }
    return;
  }
#ifdef __wasm__
  WDom *n = wc_node_by_id(node);
#else
  WDom *n = nullptr; // a native test types by name; the element is the hook's node
  (void)node;
#endif
  size_t vn = value_len > 0 ? (size_t)value_len : 0;
  wc_ev_enter(n, (str){(char *)(uintptr_t)value, vn});
  fn(nullptr, "input");
  wc_ev_leave();
#ifdef __wasm__
  wc_runtime_flush();
#endif
}


#ifdef __wasm__
__attribute__((export_name("webc_field"))) void webc_field(i32 node, const char *name,
                                                           i32 name_len, const char *value,
                                                           i32 value_len) {
  Arena *prev = wc_cb_arena_enter("field");
  // Clear first, exactly as webc_dispatch and webc_action do: a field that reaches no
  // action and only a quiet `bind_ctx` writes nothing at all, and `webc_take_cmds`
  // hands the page whatever is left in the buffer. Without this every keystroke in such
  // an input re-applies the previous batch — the whole view, drawn a second time on top
  // of itself (examples/web showed a new copy of the list per character typed).
  wc_runtime_clear();
  wc_field_dispatch(node, name, name_len, value, value_len);
  wc_cb_arena_leave(prev);
}
#endif

// ---------------------------------------------------------------- the test harness's hands
//
// A native test cannot type into an input or press a key; it can ask for both by the
// name the app registered — the same tables the bridge walks, without the browser.
// (tests/client_harness_test.c proves them; include/webc/client_test.h is the door.)

bool wc_client_press(const char *spec) {
  for (i32 i = 0; i < WC_HOST_SLOTS; i++)
    if (g_slots[i].kind == SLOT_KEY && g_slots[i].key &&
        strcmp(g_slots[i].key, spec) == 0 && g_slots[i].on) {
      WEventFn fn = g_slots[i].on; // `on` is a view macro; through a pointer it is a call
      fn(nullptr, "key");
      return true;
    }
  return false; // nothing pressed this spec: itself worth asserting
}

bool wc_client_type(const char *field, const char *value_) {
  // One road for every field spelling: the dispatch the browser's webc_field uses
  // resolves action-bound fields AND context-bound fields, so a test types
  // through the same dispatch the page runs — a second dispatch would be a second
  // set of rules.
  wc_field_dispatch(-1, field, (i32)strlen(field), value_, (i32)strlen(value_));
  for (i32 i = 0; i < g_nfields_act; i++)
    if (strcmp(g_field_names[i], field) == 0 && g_field_fns[i]) return true;
  for (i32 i = 0; i < g_nctxfields; i++)
    if (strcmp(g_ctxfields[i].name, field) == 0) return true;
  return false;
}

void wc_host_reset(void) {
  for (i32 i = 0; i < WC_HOST_SLOTS; i++) slot_release(&g_slots[i]);
  g_nfields_act = 0;
#ifndef __wasm__
  wc_host_ops_clear();
#endif
}