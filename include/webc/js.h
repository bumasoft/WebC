// SPDX-License-Identifier: Apache-2.0
// WebC — calling into JavaScript, and out to the host (SPEC §8).
//
//   #include <webc/js.h>
//
//   js("localStorage.setItem", "meridian.theme", "dark");      // no JSON in source
//   str theme = js_str(js("localStorage.getItem", "meridian.theme"), "light");
//   bool dark = js_bool(js("matchMedia", "(prefers-color-scheme: dark)"), false);
//   js_then("fetch", "/api/state", on_state, nullptr);         // a promise, later
//
// One door, typed where C can check it: `js(...)` takes a dotted path from globalThis
// and up to three arguments chosen at compile time (a literal or `str` is a string, an
// integer a number, a bool a boolean, `js_obj(…)` a JSON literal). The result is a
// `WJs` — a value with a kind and its own error, never a global. A path that names a
// property instead of a function reads it.
//
// Only the wasm target has a JavaScript to call. A server build compiles the same code
// with every call returning an empty `WJs`, so shared component code guards one `if`
// instead of being half-copied per target.
#ifndef WEBC_JS_H
#define WEBC_JS_H

#include <webc/core.h>
#include <webc/json.h>

#ifdef __wasm__
// wasm_glue.c: re-render every mounted view into the outgoing batch. Native builds
// have no batch to flush — an action's test reads the state it wrote.
void wc_runtime_flush(void);
// Drop the batch the previous callback left. Every entry point from the bridge has to
// start from an empty one: `webc_take_cmds` hands the page whatever is in the buffer, so
// a callback that writes nothing (a quiet `bind_ctx` field, a name nobody listens for)
// would otherwise re-apply the last render — the whole tree, a second time.
void wc_runtime_clear(void);
#endif

#ifndef WC_JS_PENDING_MAX
#define WC_JS_PENDING_MAX 8 // promises in flight, per module
#endif

bool js_available(void);

// ---------------------------------------------------------------- the callback arena
//
// A host callback (an event, a timer, a fetch that settled, a field) runs *outside* a
// render, so there is no render arena to reclaim what it allocates — and every synchronous
// `js()` call copies its result into whatever arena is current. In a loop, that is a leak
// with no reset point: 21M `js("Date.now")` calls once walked a deep run to 2 GiB, wrapped
// the command buffer's i32 pointer and froze the page.
//
// So every entry point from the bridge installs a callback arena, and they rotate in two
// generations: what a callback allocates stays valid through the *next* callback (a str
// read by a render still reads), and is reclaimed two callbacks later — the same
// "generations" model WStore uses for frames. A callback that allocates more than
// WC_CB_ARENA_MAX reports it once and starts clean: that is the shape of js()-in-a-loop.
//
// Both functions are no-ops on a native build (a test allocates in its own arena, and
// there is no bridge to call back).
#ifndef WC_CB_ARENA_MAX
#define WC_CB_ARENA_MAX ((size_t)8 * 1024 * 1024)
#endif
Arena *wc_cb_arena_enter(const char *what); // returns the arena that was current
void   wc_cb_arena_leave(Arena *prev);      // rotates; reclaims a generation when it ages out

// ---------------------------------------------------------------- results
//
// A call's outcome is a value. `ok` says the call happened; `kind` says what came
// back; `json` is the value as JSON text (empty for undefined); `error` is why it did
// not, and it belongs to *this* call — a handler that makes three calls can read three
// errors, which one last-error global cannot do.
typedef enum JsKind : u8 { JS_UNDEF, JS_NULL, JS_STR, JS_NUM, JS_BOOL, JS_OBJ, JS_ARR } JsKind;

typedef struct WJs {
  bool ok;
  JsKind kind;
  str json;
  str error;
} WJs;

bool  js_ok(WJs r);
str   js_str(WJs r, const char *dflt);
bool  js_bool(WJs r, bool dflt);
i64   js_i64(WJs r, i64 dflt);
f64   js_f64(WJs r, f64 dflt);
JVal *js_value(WJs r);                // the parsed value, in the current arena, or null
const char *js_kind_name(JsKind k);
// ---------------------------------------------------------------- arguments
//
// An argument is one JSON value, chosen from its C type at compile time. Writing the
// JSON by hand (`str_lit("[\"theme\"]")`) is where every escaping bug in this seam comes
// from.
typedef enum JsArgKind : u8 {
  JA_STR,  // a string
  JA_INT,  // a whole number
  JA_NUM,  // a float
  JA_BOOL, // true / false
  JA_JSON, // a JSON literal you wrote out (js_obj)
  JA_NULL, // null
} JsArgKind;

typedef struct JsArg {
  JsArgKind kind;
  str s;
  i64 i;
  f64 f;
  bool b;
} JsArg;

JsArg wc_js_arg_str(str s);
JsArg wc_js_arg_cstr(const char *s); // a literal or a `const char *`
JsArg wc_js_arg_i64(i64 v);
JsArg wc_js_arg_f64(f64 v);
JsArg wc_js_arg_bool(bool v);
JsArg wc_js_arg_json(str json); // verbatim: an object or array you write out
static inline JsArg wc_js_arg_self(JsArg a) { return a; }

// `_Generic` picks the encoder; a JsArg passes through so helpers compose (without
// that branch a typed helper used as an argument would be wrapped a second time).
//
// A string *literal* arrives as `char[N]` and `_Generic` does not decay arrays, so the
// default branch is the C-string encoder — which is also the honest default for a type
// this list has not thought about.
#define WC_JS_ARG(v_)                                                                              \
  _Generic((v_), str: wc_js_arg_str, bool: wc_js_arg_bool, int: wc_js_arg_i64,                     \
           long: wc_js_arg_i64, long long: wc_js_arg_i64, unsigned: wc_js_arg_i64,                  \
           unsigned long: wc_js_arg_i64, double: wc_js_arg_f64, float: wc_js_arg_f64,               \
           JsArg: wc_js_arg_self, default: wc_js_arg_cstr)(v_)

// A JSON argument you spell out — the one case the typed door cannot infer.
// `js_obj("{\"method\":\"POST\"}")` passes one object, not a string.
#define js_obj(json_) wc_js_arg_json(str_lit(json_))

// ---------------------------------------------------------------- calling

WJs wc_js_call_n(const char *path, i32 n, const JsArg *args);
#define WC_JS1(p_) wc_js_call_n(p_, 0, nullptr)
#define WC_JS2(p_, a_) wc_js_call_n(p_, 1, (const JsArg[]){WC_JS_ARG(a_)})
#define WC_JS3(p_, a_, b_) wc_js_call_n(p_, 2, (const JsArg[]){WC_JS_ARG(a_), WC_JS_ARG(b_)})
#define WC_JS4(p_, a_, b_, c_)                                                                     \
  wc_js_call_n(p_, 3, (const JsArg[]){WC_JS_ARG(a_), WC_JS_ARG(b_), WC_JS_ARG(c_)})
#define WC_JS_PICK_(n_) WC_CAT(WC_JS, n_)
#define js(...) WC_JS_PICK_(WC_NARG(__VA_ARGS__))(__VA_ARGS__)

// The low-level form, kept because it is the ABI and because a path built at run time
// has no other door. `args` is a JSON array; the result is JSON text.
str js_call(const char *path, str args);
str js_error(void); // the last failure, for the low-level form

// Typed convenience over the low-level form (kept as they were).
bool js_get_bool(const char *path, str args, bool dflt);
str  js_get_str(const char *path, str args, str dflt);

// ---------------------------------------------------------------- promises
//
//   static void on_state(WJs r) {
//     if (r.ok) set(state, js_str(r, ""));
//   }
//   js_then("fetch", "/api/state", on_state);
//
// The call returns immediately; the handler runs when the promise settles, with the
// resolved value (or `r.error` when it rejects). The handler runs *as an action*: the
// views re-render and the patch batch goes to the page, so a signal written there
// shows up without a reload. Returns false when there is no JavaScript or no free
// slot (WC_JS_PENDING_MAX calls in flight).
//
// The handler takes the result and nothing else: it is registered where the call is
// made, so what it needs is a signal, not a context pointer carried through a promise
// — and `(void)ctx;` in every handler was the tell that the second parameter was
// ceremony.
typedef void (*JsThen)(WJs r);

bool wc_js_then_n(const char *path, i32 n, const JsArg *args, JsThen fn);
#define WC_JSTHEN1(p_, f_) wc_js_then_n(p_, 0, nullptr, (f_))
#define WC_JSTHEN2(p_, a_, f_) wc_js_then_n(p_, 1, (const JsArg[]){WC_JS_ARG(a_)}, (f_))
#define WC_JSTHEN3(p_, a_, b_, f_)                                                                 \
  wc_js_then_n(p_, 2, (const JsArg[]){WC_JS_ARG(a_), WC_JS_ARG(b_)}, (f_))
#define WC_JSTHEN_PICK_(n_) WC_CAT(WC_JSTHEN_PICK_, n_)
// `path_` is not part of `__VA_ARGS__`, so the count is (arguments…, handler): 1 means
// "no arguments", and every extra argument shifts the pick by one.
#define WC_JSTHEN_PICK_1 WC_JSTHEN1
#define WC_JSTHEN_PICK_2 WC_JSTHEN2
#define WC_JSTHEN_PICK_3 WC_JSTHEN3
// `js_then(path, [args…,] handler)`
#define js_then(path_, ...) WC_JSTHEN_PICK_(WC_NARG(__VA_ARGS__))(path_, __VA_ARGS__)

// ---------------------------------------------------------------- the other direction
//
// The other direction, page to app. `WebC.call('ping', {n: 1})` marshals the arguments
// and calls the export; the C side receives the payload already decoded as a `WJs` and
// returns `void` — it answers by writing signals, whose patch batch the bridge applies
// when the call returns. No envelope, no return-value protocol:
//
//   webc_export_json(ping) { clicks = js_i64(in, 0); }
//
// On a server build the body still compiles (so one file can be both halves) and is
// simply never called.
#ifdef __wasm__
#define WC_JS_EXPORT_WRAP_(name_)                                                                  \
  void WC_CAT(name_, _impl)(WJs in);                                                               \
  __attribute__((export_name(#name_))) void name_(const char *json, i32 len) {                     \
    WC_CAT(name_, _impl)(wc_js_decode(wc_arena_get(), (str){(char *)(uintptr_t)json,               \
                                                        (size_t)(len > 0 ? len : 0)}));            \
    wc_runtime_flush();                                                                            \
  }                                                                                                \
  void WC_CAT(name_, _impl)(WJs in)
#define webc_export(name_) __attribute__((export_name(#name_)))
#define webc_export_json(name_) WC_JS_EXPORT_WRAP_(name_)
#else
#define webc_export(name_)
#define webc_export_json(name_) void WC_CAT(name_, _impl)(WJs in)
#endif

// ---------------------------------------------------------------- the seam itself
//
// These two are what `js()` and `js_then()` are built from, exposed because they are
// the parts a test can check without a browser: the encoder (does a quote in an
// argument stay a quote?) and the wire decoder (does an escape survive?).
str wc_js_args_json(Arena *a, i32 n, const JsArg *args);
WJs wc_js_decode(Arena *a, str wire);

#endif // WEBC_JS_H