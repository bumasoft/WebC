// SPDX-License-Identifier: Apache-2.0
// webc — the JavaScript interop seam (SPEC §8).
//
// One import goes out — `webc.js_call(path, pathLen, args, argsLen, thenId)` — and one
// export comes back in, `webc_js_settle(id, json, len)`, for a promise that settled
// later. The bridge (assets/webc.js) owns every JavaScript object; this side owns every
// value. Arguments are encoded from C types (never from JSON written in source), the
// result is decoded once into a `WJs`, and a failure is a field on that value rather
// than a global that the next call overwrites.
#include <webc/core.h>
#include <webc/js.h>

#ifdef __wasm__
#include <webc/web.h> // wc_runtime_flush

__attribute__((import_module("webc"), import_name("js_call")))
extern const char *wc_js_call_import(const char *path, i32 path_len, const char *args,
                                     i32 args_len, i32 then_id);
#endif

// ---------------------------------------------------------------- encoding
//
// A JSON array, built by streaming each argument into one buffer. A string is escaped
// by the same code the JSON writer uses, so a quote in a value is a quote in a value —
// which a hand-written literal cannot promise.
static void js_encode(Buf *b, i32 n, const JsArg *args) {
  buf_putc(b, '[');
  for (i32 i = 0; i < n; i++) {
    if (i) buf_putc(b, ',');
    const JsArg *a = &args[i];
    switch (a->kind) {
    case JA_STR: json_escape_into(b, a->s); break;
    case JA_BOOL: buf_puts(b, a->b ? "true" : "false"); break;
    case JA_INT: buf_puti64(b, a->i); break;
    case JA_JSON: buf_putstr(b, a->s); break; // written out by the caller, verbatim
    case JA_NUM:
      // NaN and the infinities have no JSON spelling; null says "no number" in the one
      // way the other side can parse.
      if (a->f != a->f || a->f > 1.7976931348623157e308 || a->f < -1.7976931348623157e308)
        buf_puts(b, "null");
      else
        buf_printf(b, "%.17g", a->f);
      break;
    default: buf_puts(b, "null"); break;
    }
  }
  buf_putc(b, ']');
}

str wc_js_args_json(Arena *a, i32 n, const JsArg *args) {
  Buf b;
  buf_init(&b, a, 64);
  js_encode(&b, n, args);
  return buf_str(&b);
}

JsArg wc_js_arg_str(str s) { return (JsArg){.kind = JA_STR, .s = s}; }
JsArg wc_js_arg_cstr(const char *s) { return (JsArg){.kind = JA_STR, .s = str_lit(s)}; }
JsArg wc_js_arg_i64(i64 v) { return (JsArg){.kind = JA_INT, .i = v}; }
JsArg wc_js_arg_f64(f64 v) { return (JsArg){.kind = JA_NUM, .f = v}; }
JsArg wc_js_arg_bool(bool v) { return (JsArg){.kind = JA_BOOL, .b = v}; }
JsArg wc_js_arg_json(str json) { return (JsArg){.kind = JA_JSON, .s = json}; }

// ---------------------------------------------------------------- decoding

WJs wc_js_decode(Arena *a, str wire) {
  (void)a;
  WJs r = {.ok = true, .kind = JS_UNDEF};
  if (wire.n == 0 || !wire.p) return r;
  // The bridge marks a failure the way a page never can: a JSON string whose first
  // character is 0x01. So an error cannot be mistaken for data, and no envelope had to
  // be invented for the wire format.
  if (wire.n >= 3 && wire.p[0] == '"' && wire.p[1] == '\001') {
    r.ok = false;
    r.error = str_sub(wire, 2, wire.n - 1); // without the marker and the closing quote
    return r;
  }
  r.json = wire;
  switch (wire.p[0]) {
  case '"': r.kind = JS_STR; break;
  case '{': r.kind = JS_OBJ; break;
  case '[': r.kind = JS_ARR; break;
  case 't':
  case 'f': r.kind = JS_BOOL; break;
  case 'n': r.kind = JS_NULL; break;
  default: r.kind = JS_NUM; break;
  }
  return r;
}

bool js_ok(WJs r) { return r.ok; }

const char *js_kind_name(JsKind k) {
  switch (k) {
  case JS_UNDEF: return "undefined";
  case JS_NULL: return "null";
  case JS_STR: return "string";
  case JS_NUM: return "number";
  case JS_BOOL: return "boolean";
  case JS_OBJ: return "object";
  case JS_ARR: return "array";
  }
  return "?";
}

// Parsed on demand, in the caller's arena: a call that returns a big object costs
// nothing until something asks for its fields.
JVal *js_value(WJs r) {
  if (!r.ok || r.json.n == 0) return nullptr;
  JParse p = json_parse(wc_arena_get(), r.json);
  return p.ok ? p.v : nullptr;
}

static JVal *parse(WJs r) { return js_value(r); }

str js_str(WJs r, const char *dflt) {
  if (!r.ok) return str_lit(dflt ? dflt : "");
  if (r.kind == JS_STR) {
    // Through the JSON parser, not by hand: `\u2014` is one character, and the version
    // of this function that unescaped by hand dropped its backslash and returned
    // "u2014" — an em dash rendered as five letters, only in values that had one.
    JVal *v = parse(r);
    return v && v->kind == J_STR ? v->s : str_lit(dflt ? dflt : "");
  }
  if (r.kind == JS_NUM || r.kind == JS_BOOL) return r.json; // "42", "true"
  return str_lit(dflt ? dflt : "");
}

bool js_bool(WJs r, bool dflt) {
  if (!r.ok) return dflt;
  if (r.kind == JS_BOOL) return r.json.n == 4 && r.json.p[0] == 't';
  if (r.kind == JS_NUM) return r.json.n > 0 && r.json.p[0] != '0';
  return dflt;
}

i64 js_i64(WJs r, i64 dflt) {
  if (!r.ok) return dflt;
  JVal *v = parse(r);
  i64 out = dflt;
  if (v && json_as_i64(v, &out)) return out;
  return dflt;
}

f64 js_f64(WJs r, f64 dflt) {
  if (!r.ok) return dflt;
  JVal *v = parse(r);
  f64 out = dflt;
  if (v && json_as_f64(v, &out)) return out;
  return dflt;
}

// ---------------------------------------------------------------- the call

#ifdef __wasm__

static str g_js_error; // the low-level form's last failure; `WJs.error` is the real one

bool js_available(void) { return true; }

WJs wc_js_call_n(const char *path, i32 n, const JsArg *args) {
  str args_json = (str){nullptr, 0};
  Buf b;
  if (n > 0) {
    buf_init(&b, nullptr, 96); // freed before returning: no arena growth per call
    js_encode(&b, n, args);
    args_json = buf_str(&b);
  }
  const char *out = wc_js_call_import(path, (i32)strlen(path), args_json.p ? args_json.p : "[]",
                                      (i32)args_json.n, 0);
  if (n > 0) buf_free(&b);
  if (!out) return (WJs){.ok = true, .kind = JS_UNDEF};
  size_t len = strlen(out);
  Arena *a = wc_arena_get();
  char *copy = arena_alloc(a, len + 1);
  for (size_t i = 0; i < len; i++) copy[i] = out[i];
  copy[len] = '\0';
  WJs r = wc_js_decode(a, (str){copy, len});
  // The bridge already wrote the failure to the console, with the same path and
  // message; printing it here as well put every error in the console twice.
  if (!r.ok) g_js_error = r.error;
  return r;
}

str js_call(const char *path, str args) {
  g_js_error = (str){nullptr, 0};
  const char *out =
      wc_js_call_import(path, (i32)strlen(path), args.p ? args.p : "", (i32)args.n, 0);
  if (!out) return (str){nullptr, 0};
  size_t len = strlen(out);
  Arena *a = wc_arena_get();
  char *copy = arena_alloc(a, len + 1);
  for (size_t i = 0; i < len; i++) copy[i] = out[i];
  copy[len] = '\0';
  WJs r = wc_js_decode(a, (str){copy, len});
  if (!r.ok) {
    g_js_error = r.error;
    return (str){nullptr, 0};
  }
  return (str){copy, len};
}

str js_error(void) { return g_js_error; }

// ---------------------------------------------------------------- promises
//
// A promise is a callback, not an event loop: the call returns, the page resolves it,
// and the bridge calls back into `webc_js_settle`. The slot table is static because a
// wasm module has one instance and a fixed number of calls in flight is a bound worth
// stating out loud (WC_JS_PENDING_MAX).
static struct {
  JsThen fn;
} g_pending[WC_JS_PENDING_MAX];

bool wc_js_then_n(const char *path, i32 n, const JsArg *args, JsThen fn) {
  if (!fn) return false;
  i32 slot = -1;
  for (i32 i = 0; i < WC_JS_PENDING_MAX; i++)
    if (!g_pending[i].fn) {
      slot = i;
      break;
    }
  if (slot < 0) {
    static char once;
    wc_limit_once(&once, "too many promises in flight: this one is dropped", "WC_JS_PENDING_MAX",
                  "call");
    return false;
  }
  g_pending[slot].fn = fn;

  str args_json = (str){nullptr, 0};
  Buf b;
  if (n > 0) {
    buf_init(&b, nullptr, 96);
    js_encode(&b, n, args);
    args_json = buf_str(&b);
  }
  wc_js_call_import(path, (i32)strlen(path), args_json.p ? args_json.p : "[]", (i32)args_json.n,
                    slot + 1);
  if (n > 0) buf_free(&b);
  return true;
}

// Called by the bridge when the promise settles; `then_id` is the slot it was given.
// It runs as an action: the handler writes signals, then the same flush an event gets.
__attribute__((export_name("webc_js_settle"))) void webc_js_settle(i32 then_id, const char *json,
                                                                   i32 len) {
  i32 slot = then_id - 1;
  if (slot < 0 || slot >= WC_JS_PENDING_MAX || !g_pending[slot].fn) return;
  JsThen fn = g_pending[slot].fn;
  g_pending[slot].fn = nullptr; // free before running: the handler may call again
  // Copied out of the bridge's buffer first: the handler is free to make another call,
  // and the next call frees the buffer this one was handed — a dangling `r.json` that
  // only shows up when a settled promise leads to another interop call.
  Arena *a = wc_arena_get();
  size_t n = len > 0 ? (size_t)len : 0;
  char *copy = arena_alloc(a, n + 1);
  for (size_t i = 0; i < n; i++) copy[i] = json[i];
  copy[n] = '\0';
  WJs r = wc_js_decode(a, (str){copy, n});
  fn(r);
  wc_runtime_flush();
}

#else // ---------------------------------------------------------------- no JavaScript

bool js_available(void) { return false; }

WJs wc_js_call_n(const char *path, i32 n, const JsArg *args) {
  (void)path;
  (void)n;
  (void)args;
  return (WJs){.ok = true, .kind = JS_UNDEF};
}

str js_call(const char *path, str args) {
  (void)path;
  (void)args;
  return (str){nullptr, 0};
}

str js_error(void) { return (str){nullptr, 0}; }

bool wc_js_then_n(const char *path, i32 n, const JsArg *args, JsThen fn) {
  (void)path;
  (void)n;
  (void)args;
  (void)fn;
  return false; // there is no promise on a server; the caller's fallback is the page
}

#endif

// ---------------------------------------------------------------- typed convenience

bool js_get_bool(const char *path, str args, bool dflt) {
  str v = js_call(path, args);
  if (v.n == 0) return dflt;
  WJs r = wc_js_decode(wc_arena_get(), v);
  return js_bool(r, dflt);
}

str js_get_str(const char *path, str args, str dflt) {
  str v = js_call(path, args);
  if (v.n == 0) return dflt;
  WJs r = wc_js_decode(wc_arena_get(), v);
  return js_str(r, dflt.p ? dflt.p : "");
}