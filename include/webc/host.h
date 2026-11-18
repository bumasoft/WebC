// SPDX-License-Identifier: Apache-2.0
// WebC — the host the app runs in: network, keyboard, and the document around the view.
//
//   #include <webc/host.h>
//
//   static void on_briefing(WJs r) { … }          // a result, exactly like js_then's
//   webc_get("/api/briefing/7", on_briefing);
//   webc_subscribe("/api/stream", "tick", on_feed);
//   webc_later(5200, toast_expire);
//
//   bind_field("dest", field_dest);               // input → action, in the view's own idiom
//   on_key("meta+k", pal_open);
//
//   doc_title(fmt_s("%s — Contrail", f->number));
//   push_url(link_to(Flight, f));
//
// Why this exists: `js()` is a typed door into `globalThis`, and an app can do every one of
// these things through it — but only by shipping a JavaScript shim to be the door's other
// half. That shim is untyped across the language boundary: `js("contrail.spin")` named a
// function that did not exist, returned `ok: false`, and the call site ignored it, so a
// spinner that never spun shipped and both halves still compiled. Every verb here is one
// string fewer in an application, and one behaviour the framework owns and tests.
//
// One rule, holding for everything in this header: a verb is a *declaration*. The C side
// says what should happen; the bridge (`assets/webc.js`) is the only code that touches
// `fetch`, `EventSource`, `history` or `document`. Nothing here is wasm-only at compile
// time — a server build records the same calls and does nothing with them, so one file can
// be both halves (see js.h for the same decision).
#ifndef WEBC_HOST_H
#define WEBC_HOST_H

#include <webc/core.h>
#include <webc/dom.h>
#include <webc/js.h>
#include <webc/fe.h>

#ifndef WC_HOST_SLOTS
#define WC_HOST_SLOTS 48 // verbs in flight: fetches, streams, timers, keys, one nav.
                          // One table still: an app with a keyboard-heavy client half
                          // (16 on_key bindings) plus subscriptions, timers and posts
                          // reaches two-thirds of a 24-slot table before it starts
                          // dropping verbs. A separate static table for keys is the
                          // real fix; the budget buys it time.
#endif

// ---------------------------------------------------------------- network
//
// The handler runs as an action: it writes signals, and the batch that follows reaches the
// page by the same road a click takes. `r.ok` is false when the request failed, and
// `r.error` says why in the server's own words ("HTTP 503", the message from an error body).
//
// A `str` url/body is read at call time; the slot keeps its own copy, so the caller may
// build them in a render arena that is about to be rewound.
i32 webc_get(str url, JsThen fn);
i32 webc_post(str url, str json_body, JsThen fn);
// A server-sent event stream. The bridge owns reconnection (EventSource retries on its own),
// delivers every `event` frame, and reports the link's state as {"state":"open"|"lost"} to
// the same handler when `event` is "state" — or the frame's parsed body otherwise.
i32 webc_subscribe(str url, str event_name, JsThen fn);
// Do this in a moment: one timer per *verb*, re-armed if called again, so "the toast clears
// itself in five seconds" needs no variable in the app.
i32 webc_later(i64 ms, JsThen fn);
void webc_unsubscribe(i32 slot);

// A resource whose loader is a GET: start → WRES_LOADING, settle → WRES_OK/WRES_ERR, so
// `res_loading(r)` drives the spinner and `res_err_str(r)` shows the server's message. The
// handler decides what the resource's data is (that is the one thing the app knows better
// than the framework does) and calls res_complete(); saying "the request failed" is the
// one line in the recipe a handler must not forget.
//
//   static void on_briefing_json(WRes *r, WJs j) {
//     if (!j.ok) { res_complete(r, (WErrRes){.ok = false, .e = j.error}, nullptr); return; }
//     g_box = parse_briefing(j);            // the app's shape
//     res_complete(r, (WErrRes){.ok = true}, &g_box);
//   }
//   res_get_json(g_brief, fmt_s("/api/briefing/%lld", id), on_briefing_json);
void res_get_json(WRes *res, str url, void (*fn)(WRes *, WJs));

// ---------------------------------------------------------------- keyboard and fields
//
// `bind_field("dest", field_dest)` makes the input the view marked `named_field("dest")`
// live: one delegated `input` listener in the bridge, the text in `ev_value()`, the element
// in `ev_self()`. It is `bind()` for a text field whose action is not a string signal.
void wc_bind_field_action(const char *name, WEventFn fn);

// Bind a named input straight to a context field — the shared-view half of
// bind_str(), which can only reference client-file signals. The value is
// persisted (it must outlive the keystroke), written into the field, and the
// view re-renders only in the `_live` spelling (a filter box wants its list to
// follow each keystroke; a draft box does not).
//
//   bind_ctx("draft_title", C, draft_title);       // in client_main
//   bind_ctx_live("pal_q", C, pal_q);
void wc_bind_ctx(const char *name, void *base, u32 offset, bool live);
#define bind_ctx(name_, base_, field_) \
  wc_bind_ctx(name_, &(base_), (u32)offsetof(__typeof__(base_), field_), false)
#define bind_ctx_live(name_, base_, field_) \
  wc_bind_ctx(name_, &(base_), (u32)offsetof(__typeof__(base_), field_), true)
// `on_key("meta+k", pal_open)`: one modifier plus one key, no sequences. The bridge skips
// the binding while the user is typing in a field unless the spec has a modifier — so
// `Mod+K` fires everywhere and `j` only when the list has focus.
void wc_on_key(const char *spec, WEventFn fn);
// The field name, beside the input: `el("input") { named_field("dest"); }` — or
// `named_field(dest)`, the bare identifier. Both spellings work: the macro stringifies
// its argument, and the quoted form alone would ship the quotes into the attribute
// (`data-field="&quot;dest&quot;"`), so typed input never reached the action and nothing
// said so. The helper strips one pair of surrounding quotes.
static inline const char *wc_field_name_of(const char *s) {
  static char bufs[4][80];
  static int which;
  char *out = bufs[which++ & 3];
  size_t n = s ? strlen(s) : 0;
  if (n >= 2 && s[0] == '"' && s[n - 1] == '"' && n - 2 < sizeof bufs[0]) {
    memcpy(out, s + 1, n - 2);
    out[n - 2] = 0;
    return out;
  }
  return s;
}
void wc_named_field(const char *name);

#define bind_field(name_, N_) wc_bind_field_action((name_), (void *)WC_CAT(action_, N_))
#define on_key(spec_, N_) wc_on_key((spec_), (void *)WC_CAT(action_, N_))
#define named_field(N_) wc_named_field(wc_field_name_of(#N_))

// ---------------------------------------------------------------- the document
//
// Applied by the bridge *after* the batch's own mutations, so a title computed from a signal
// describes the tree that is now on the page, and a focus() lands on a node that exists.
void doc_attr(const char *name, str value); // an attribute on <html>: data-theme, data-ready
void doc_title(str value);                  // <title>
void push_url(str path);                    // history.pushState — a new entry, a back button
void replace_url(str path);                 // history.replaceState — the same entry
str self_path(void);                        // the current pathname, today's address

// Where this document was born: the page env the server rendered against, handed to the
// module before client_main runs. The reader's edition and the address are one fact on
// the server (c->page.env) — this is the same fact on the client, so an app stops
// smuggling the edition through its data document and guarding against frames flipping
// it back.
typedef struct WClientEnv {
  bool dark;      // the edition the server rendered (the cookie, the page env's own read)
  str path;       // where the document was rendered ("/f/VA101"), not where it is now
} WClientEnv;

WClientEnv page_env(void);

#ifndef __wasm__
// A native test sets the env the way the bridge would, before the client half runs.
void wc_env_set(bool dark, str path);
#endif
void focus_hook(const char *name);          // focus + select the element carrying this hook
void reveal_hook(const char *name);         // scrollIntoView({block:'nearest'})
// Put a value into a bound field, by the name the view gave it (`named_field`). This is
// the one write a render cannot make: a value attribute is only *the default the render
// draws*, and once the reader has typed, the browser owns the live `value` property — so
// `draft = ""` after an add clears the app's copy and leaves the text on screen. Clears a
// draft once the action has consumed it:
//
//   action(add) { push(g_notes, ((Note){.title = g_draft})); field_value("draft", str_lit("")); }
void field_value(const char *name, str value);
// The back and forward buttons: the browser moved, the app has not. Runs as an action; read
// The browser moved; the module has not. The action receives the arrival as a WNav —
// the path *and* the match against the same route table the server answered — instead of
// re-implementing the router by hand (`self_path` + `front_route` + a string walk):
//
//   action(navigate, WNav *nav) {
//     if (nav->matched) { str num = nav_param(nav, "number"); … }
//   }
typedef struct WNav {
  str path;         // the address that arrived ("/f/VA101")
  bool matched;     // did a route claim it?
  const char *pattern; // the route that claimed it ("/f/:number"), or ""
  void *m;          // the WPageMatch itself; nav_param() reads it
} WNav;

str nav_param(const WNav *nav, const char *name);      // the param as text
i64 nav_param_i64(const WNav *nav, const char *name);  // … as a number, 0 if absent

void on_nav(WEventFn fn);
// Called once, after the first batch is in the document — the hook an app needs before it
// can open its stream or declare "the module is up".
void on_ready(WEventFn fn);

#define on_nav(N_) wc_on_nav((void *)WC_CAT(action_, N_))
#define on_ready(N_) wc_on_ready((void *)WC_CAT(action_, N_))
void wc_on_nav(void *fn);
void wc_on_ready(void *fn);

// ---------------------------------------------------------------- the live document
//
// The pattern every streaming app repeats, as one call: a document that keeps arriving
// (the first paint's seed, then an SSE frame every tick) into two generation arenas —
// the frame being parsed and the one still on screen — with the link's health reported
// as it changes. The verbs underneath stay: this is a composite, not a replacement.
//
//   WLive *board = res_live(&(WLiveCfg){
//       .url = str_lit("/api/stream"), .event = str_lit("tick"),
//       .doc = board_read,                       // one callback, every document
//       .state = board_link,                     // optional: the pill
//   });
//   // wc_app_data(str json)  -> res_live_read(board, json);   // the seed
//   // action(ready)          -> res_live_open(board);         // the subscribe pair

enum { W_LIVE_CONNECTING = 0, W_LIVE_STREAMING = 1, W_LIVE_LOST = 2 };

typedef struct WLiveCfg {
  str url;               // the stream's URL ("/api/stream")
  str event;             // the event name that carries documents ("tick")
  void (*doc)(str json); // every document: seed and frames alike, in arrival order
  void (*on_link)(int s); // optional: W_LIVE_* as the link changes (not `state`:
                          // that is the DSL's declaration macro)
  size_t gen_bytes;      // one generation arena's size; 0 = 128 KiB
} WLiveCfg;

typedef struct WLive WLive;

WLive *res_live(const WLiveCfg *cfg);
void res_live_read(WLive *lv, str json); // a document arrived (seed or frame)
void res_live_open(WLive *lv);           // from on_ready: subscribe, and report the link

// A native test's hands (include/webc/client_test.h): press a key by its spec, type
// into a field by its name — the same tables the bridge walks, without the page.
bool wc_client_press(const char *spec);
bool wc_client_type(const char *field, const char *value);

// The bridge's answer door: a reply or frame reaches the module as bytes and the slot
// that asked for it is settled. Declared here so a test can be the bridge.
void webc_net_recv(i32 id, const char *json, i32 len, const char *err, i32 err_len);

// ---------------------------------------------------------------- what a test can see
//
// On the wasm target these are the ops the bridge receives; on a server build there is no
// bridge, so the same calls are recorded here instead and a test can read them back. One
// implementation, two observers — the alternative is a test that mocks the thing it is
// trying to prove.
const char *wc_host_ops(void);       // every op recorded since the last clear, newline-terminated
void wc_host_ops_clear(void);
void wc_host_reset(void);            // forget every slot: a test starts from a known table

#endif // WEBC_HOST_H