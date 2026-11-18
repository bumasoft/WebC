// SPDX-License-Identifier: Apache-2.0
// WebC — JSON writer and bounded parser.
#include <math.h>
#include <webc/json.h>

// ---------------------------------------------------------------- writer

void jw_init(JsonWriter *w, Arena *a, size_t cap) {
  *w = (JsonWriter){.depth = 0, .failed = false, .max_depth = WC_JSON_MAX_DEPTH};
  buf_init(&w->b, a, cap ? cap : 256);
  w->first[0] = true;
}

str jw_finish(JsonWriter *w) {
  return buf_str(&w->b);
}

void jw_raw_bytes(JsonWriter *w, const void *p, size_t n) {
  buf_write(&w->b, p, n);
}

static bool jw_sep(JsonWriter *w) {
  if (w->depth < 0 || w->depth >= w->max_depth) {
    w->failed = true;
    return false;
  }
  if (!w->first[w->depth]) buf_putc(&w->b, ',');
  w->first[w->depth] = false;
  return true;
}

void jw_obj_begin(JsonWriter *w) {
  if (!jw_sep(w)) return;
  buf_putc(&w->b, '{');
  if (w->depth + 1 < w->max_depth)
    w->first[++w->depth] = true;
  else
    w->failed = true;
}

void jw_obj_end(JsonWriter *w) {
  buf_putc(&w->b, '}');
  if (w->depth > 0) w->depth--;
  w->first[w->depth] = false;
}

void jw_arr_begin(JsonWriter *w) {
  if (!jw_sep(w)) return;
  buf_putc(&w->b, '[');
  if (w->depth + 1 < w->max_depth)
    w->first[++w->depth] = true;
  else
    w->failed = true;
}

void jw_arr_end(JsonWriter *w) {
  buf_putc(&w->b, ']');
  if (w->depth > 0) w->depth--;
  w->first[w->depth] = false;
}

void jw_key_str(JsonWriter *w, str key) {
  jw_sep(w);
  json_escape_into(&w->b, key);
  buf_putc(&w->b, ':');
  // the value that follows must not emit a separator of its own
  w->first[w->depth] = true;
}

void jw_key(JsonWriter *w, const char *key) {
  jw_key_str(w, str_lit(key));
}

void jw_str(JsonWriter *w, str s) {
  if (!jw_sep(w)) return;
  json_escape_into(&w->b, s);
}

void jw_cstr(JsonWriter *w, const char *s) {
  jw_str(w, s ? str_lit(s) : (str){"", 0});
}

void jw_i64(JsonWriter *w, i64 v) {
  if (!jw_sep(w)) return;
  buf_puti64(&w->b, v);
}

void jw_i32(JsonWriter *w, i32 v) {
  jw_i64(w, v);
}

void jw_u64(JsonWriter *w, u64 v) {
  if (!jw_sep(w)) return;
  buf_printf(&w->b, "%llu", (unsigned long long)v);
}

void jw_f64(JsonWriter *w, f64 v) {
  if (!jw_sep(w)) return;
  if (!isfinite(v)) {
    buf_puts(&w->b, "null"); // JSON has no NaN/Inf; null is the honest encoding
    return;
  }
  str s = str_from_f64(w->b.a, v);
  buf_putstr(&w->b, s);
}

void jw_bool(JsonWriter *w, bool v) {
  if (!jw_sep(w)) return;
  buf_puts(&w->b, v ? "true" : "false");
}

void jw_null(JsonWriter *w) {
  if (!jw_sep(w)) return;
  buf_puts(&w->b, "null");
}

void jw_raw(JsonWriter *w, str frag) {
  if (!jw_sep(w)) return;
  buf_putstr(&w->b, frag);
}

str json_encode_str(Arena *a, str s) {
  return json_escape(a, s);
}

// ---------------------------------------------------------------- keys, blocks, and script bodies

void jw_kv_str(JsonWriter *w, const char *key, str v) {
  if (!v.p || v.n == 0) return; // an absent field is omitted, never ""
  jw_key(w, key);
  jw_str(w, v);
}

void jw_kv_cstr(JsonWriter *w, const char *key, const char *v) {
  if (!v || !*v) return;
  jw_key(w, key);
  jw_cstr(w, v);
}

void jw_kv_i64(JsonWriter *w, const char *key, i64 v) {
  jw_key(w, key);
  jw_i64(w, v);
}

void jw_kv_bool(JsonWriter *w, const char *key, bool v) {
  jw_key(w, key);
  jw_bool(w, v);
}

void jw_kv_raw(JsonWriter *w, const char *key, str frag) {
  if (!frag.p || frag.n == 0) return; // nothing computed, nothing claimed
  jw_key(w, key);
  jw_raw(w, frag);
}

void jw_str_list(JsonWriter *w, const char *const *items) {
  jw_arr_begin(w);
  for (const char *const *p = items; p && *p; p++)
    if (**p) jw_cstr(w, *p);
  jw_arr_end(w);
}

void jw_kv_str_list(JsonWriter *w, const char *key, const char *const *items) {
  if (!items || !items[0]) return; // no list, no key
  jw_key(w, key);
  jw_str_list(w, items);
}

void wc_jw_obj_close(JsonWriter **w) {
  if (*w) jw_obj_end(*w);
}

void wc_jw_arr_close(JsonWriter **w) {
  if (*w) jw_arr_end(*w);
}

str json_for_script(Arena *a, str json) {
  Buf b;
  buf_init(&b, a, json.n + 16);
  size_t i = 0;
  while (i < json.n) {
    unsigned char c = (unsigned char)json.p[i];
    switch (c) {
    case '<': buf_puts(&b, "\\u003c"); break;
    case '>': buf_puts(&b, "\\u003e"); break;
    case '&': buf_puts(&b, "\\u0026"); break;
    case 0xe2: // U+2028 / U+2029: valid JSON, and a line terminator to JavaScript
      if (i + 2 < json.n && (unsigned char)json.p[i + 1] == 0x80 &&
          ((unsigned char)json.p[i + 2] == 0xa8 || (unsigned char)json.p[i + 2] == 0xa9)) {
        buf_puts(&b, (unsigned char)json.p[i + 2] == 0xa8 ? "\\u2028" : "\\u2029");
        i += 3;
        continue;
      }
      buf_putc(&b, (char)c);
      break;
    default: buf_putc(&b, (char)c);
    }
    i++;
  }
  return buf_str(&b);
}

// ---------------------------------------------------------------- parser

typedef struct JParser {
  const char *p;
  const char *end;
  Arena *a;
  int depth, max_depth;
  WErr e;
} JParser;

static bool jp_fail(JParser *P, WErrKind k, const char *msg) {
  if (P->e.kind == WEBC_OK) {
    P->e = werr_c(k, msg);
    P->e.detail = str_fmt(P->a, "at byte %zu", (size_t)(P->p - (P->end - 0)));
  }
  return false;
}

static void jp_ws(JParser *P) {
  while (P->p < P->end) {
    char c = *P->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      P->p++;
    else
      break;
  }
}

static JVal *jp_new(JParser *P, JKind k) {
  JVal *v = arena_zalloc(P->a, sizeof *v);
  v->kind = k;
  return v;
}

static bool jp_value(JParser *P, JVal **out);

static bool jp_string(JParser *P, str *out) {
  if (P->p >= P->end || *P->p != '"') return jp_fail(P, WEBC_EPARSE, "expected string");
  P->p++;
  const char *start = P->p;
  // Fast path: no escapes. Eight bytes at a time, the same scan the writer uses —
  // one byte per iteration made parsing a 1 KB string cost 0.7 us by itself, which
  // is most of the 1 us round-trip budget in SPEC §10.
  size_t off = json_str_scan(start, (size_t)(P->end - start));
  if (start + off < P->end && start[off] == '"') {
    *out = (str){start, off};
    P->p = start + off + 1;
    return true;
  }
  if (start + off >= P->end) return jp_fail(P, WEBC_EPARSE, "unterminated string");
  if ((unsigned char)start[off] < 0x20) {
    // a raw control byte is not valid JSON: it has to be escaped
    return jp_fail(P, WEBC_EPARSE, "control character in string");
  }
  P->p = start + off; // at the backslash: the slow path takes it from here
  // slow path: build an escaped copy
  Buf b;
  buf_init(&b, P->a, 32);
  buf_write(&b, start, (size_t)(P->p - start));
  while (P->p < P->end && *P->p != '"') {
    char c = *P->p++;
    if (c != '\\') {
      buf_putc(&b, c);
      continue;
    }
    if (P->p >= P->end) return jp_fail(P, WEBC_EPARSE, "truncated escape");
    char e = *P->p++;
    switch (e) {
    case '"':
      buf_putc(&b, '"');
      break;
    case '\\':
      buf_putc(&b, '\\');
      break;
    case '/':
      buf_putc(&b, '/');
      break;
    case 'b':
      buf_putc(&b, '\b');
      break;
    case 'f':
      buf_putc(&b, '\f');
      break;
    case 'n':
      buf_putc(&b, '\n');
      break;
    case 'r':
      buf_putc(&b, '\r');
      break;
    case 't':
      buf_putc(&b, '\t');
      break;
    case 'u': {
      if (P->end - P->p < 4) return jp_fail(P, WEBC_EPARSE, "truncated \\u escape");
      unsigned cp = 0;
      for (int i = 0; i < 4; i++) {
        char h = *P->p++;
        unsigned d = (h >= '0' && h <= '9')   ? (unsigned)(h - '0')
                     : (h >= 'a' && h <= 'f') ? (unsigned)(h - 'a' + 10)
                     : (h >= 'A' && h <= 'F') ? (unsigned)(h - 'A' + 10)
                                              : 99u;
        if (d > 15) return jp_fail(P, WEBC_EPARSE, "bad \\u escape");
        cp = cp * 16 + d;
      }
      // encode as UTF-8; surrogate pairs handled crudely but safely
      if (cp < 0x80) {
        buf_putc(&b, (char)cp);
      } else if (cp < 0x800) {
        buf_putc(&b, (char)(0xC0 | (cp >> 6)));
        buf_putc(&b, (char)(0x80 | (cp & 0x3F)));
      } else {
        buf_putc(&b, (char)(0xE0 | (cp >> 12)));
        buf_putc(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(&b, (char)(0x80 | (cp & 0x3F)));
      }
      break;
    }
    default:
      return jp_fail(P, WEBC_EPARSE, "unknown escape");
    }
  }
  if (P->p >= P->end) return jp_fail(P, WEBC_EPARSE, "unterminated string");
  P->p++; // closing quote
  *out = buf_str(&b);
  return true;
}

static bool jp_number(JParser *P, JVal *v) {
  const char *start = P->p;
  if (P->p < P->end && (*P->p == '-' || *P->p == '+')) P->p++;
  while (P->p < P->end && ((*P->p >= '0' && *P->p <= '9') || *P->p == '.' || *P->p == 'e' ||
                           *P->p == 'E' || *P->p == '+' || *P->p == '-'))
    P->p++;
  str tok = {start, (size_t)(P->p - start)};
  if (tok.n == 0) return jp_fail(P, WEBC_EPARSE, "expected number");
  i64 iv;
  if (str_to_i64(tok, &iv)) {
    v->is_int = true;
    v->inum = iv;
    v->num = (f64)iv;
  } else if (str_to_f64(tok, &v->num)) {
    v->is_int = false;
    v->inum = (i64)v->num;
  } else {
    return jp_fail(P, WEBC_EPARSE, "malformed number");
  }
  return true;
}

static bool jp_array(JParser *P, JVal *v) {
  P->p++; // [
  v->kind = J_ARR;
  i32 cap = 4;
  v->items = arena_alloc(P->a, (size_t)cap * sizeof(JVal *));
  jp_ws(P);
  if (P->p < P->end && *P->p == ']') {
    P->p++;
    return true;
  }
  for (;;) {
    JVal *item = nullptr;
    if (!jp_value(P, &item)) return false;
    if (v->n == cap) {
      i32 ncap = cap * 2;
      JVal **ni = arena_alloc(P->a, (size_t)ncap * sizeof(JVal *));
      memcpy(ni, v->items, (size_t)cap * sizeof(JVal *));
      v->items = ni;
      cap = ncap;
    }
    v->items[v->n++] = item;
    jp_ws(P);
    if (P->p < P->end && *P->p == ',') {
      P->p++;
      jp_ws(P);
      continue;
    }
    if (P->p < P->end && *P->p == ']') {
      P->p++;
      return true;
    }
    return jp_fail(P, WEBC_EPARSE, "expected , or ] in array");
  }
}

static bool jp_object(JParser *P, JVal *v) {
  P->p++; // {
  v->kind = J_OBJ;
  i32 cap = 4;
  v->items = arena_alloc(P->a, (size_t)cap * sizeof(JVal *));
  v->keys = arena_alloc(P->a, (size_t)cap * sizeof(str));
  jp_ws(P);
  if (P->p < P->end && *P->p == '}') {
    P->p++;
    return true;
  }
  for (;;) {
    jp_ws(P);
    str key;
    if (!jp_string(P, &key)) return false;
    jp_ws(P);
    if (P->p >= P->end || *P->p != ':') return jp_fail(P, WEBC_EPARSE, "expected : after key");
    P->p++;
    jp_ws(P);
    JVal *item = nullptr;
    if (!jp_value(P, &item)) return false;
    if (v->n == cap) {
      i32 ncap = cap * 2;
      JVal **ni = arena_alloc(P->a, (size_t)ncap * sizeof(JVal *));
      memcpy(ni, v->items, (size_t)cap * sizeof(JVal *));
      v->items = ni;
      str *nk = arena_alloc(P->a, (size_t)ncap * sizeof(str));
      memcpy(nk, v->keys, (size_t)cap * sizeof(str));
      v->keys = nk;
      cap = ncap;
    }
    v->keys[v->n] = key;
    v->items[v->n++] = item;
    jp_ws(P);
    if (P->p < P->end && *P->p == ',') {
      P->p++;
      continue;
    }
    if (P->p < P->end && *P->p == '}') {
      P->p++;
      return true;
    }
    return jp_fail(P, WEBC_EPARSE, "expected , or } in object");
  }
}

static bool jp_lit(JParser *P, const char *lit) {
  size_t n = strlen(lit);
  if ((size_t)(P->end - P->p) < n || memcmp(P->p, lit, n) != 0) return false;
  P->p += n;
  return true;
}

static bool jp_value(JParser *P, JVal **out) {
  if (P->depth >= P->max_depth) return jp_fail(P, WEBC_EPARSE, "JSON nesting too deep");
  jp_ws(P);
  if (P->p >= P->end) return jp_fail(P, WEBC_EPARSE, "unexpected end of input");
  char c = *P->p;
  if (c == '{') {
    JVal *v = jp_new(P, J_OBJ);
    P->depth++;
    bool ok = jp_object(P, v);
    P->depth--;
    *out = v;
    return ok;
  }
  if (c == '[') {
    JVal *v = jp_new(P, J_ARR);
    P->depth++;
    bool ok = jp_array(P, v);
    P->depth--;
    *out = v;
    return ok;
  }
  if (c == '"') {
    JVal *v = jp_new(P, J_STR);
    bool ok = jp_string(P, &v->s);
    *out = v;
    return ok;
  }
  if (c == 't' || c == 'f') {
    JVal *v = jp_new(P, J_BOOL);
    if (jp_lit(P, "true"))
      v->b = true;
    else if (jp_lit(P, "false"))
      v->b = false;
    else
      return jp_fail(P, WEBC_EPARSE, "expected true/false");
    *out = v;
    return true;
  }
  if (c == 'n') {
    JVal *v = jp_new(P, J_NULL);
    if (!jp_lit(P, "null")) return jp_fail(P, WEBC_EPARSE, "expected null");
    *out = v;
    return true;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    JVal *v = jp_new(P, J_NUM);
    bool ok = jp_number(P, v);
    *out = v;
    return ok;
  }
  return jp_fail(P, WEBC_EPARSE, "unexpected character");
}

JParse json_parse_depth(Arena *a, str text, int max_depth) {
  JParser P = {.p = text.p,
               .end = text.p + text.n,
               .a = a,
               .depth = 0,
               .max_depth = max_depth > 0 ? max_depth : WC_JSON_MAX_DEPTH,
               .e = {.kind = WEBC_OK}};
  JVal *v = nullptr;
  bool ok = jp_value(&P, &v);
  if (ok) {
    jp_ws(&P);
    if (P.p != P.end) {
      ok = false;
      jp_fail(&P, WEBC_EPARSE, "trailing data after JSON value");
    }
  }
  if (!ok) return (JParse){.ok = false, .v = nullptr, .e = P.e};
  return (JParse){.ok = true, .v = v, .e = {.kind = WEBC_OK}};
}

JParse json_parse(Arena *a, str text) {
  return json_parse_depth(a, text, WC_JSON_MAX_DEPTH);
}

JVal *json_get_str(JVal *obj, str key) {
  if (!obj || obj->kind != J_OBJ) return nullptr;
  for (i32 i = 0; i < obj->n; i++)
    if (str_eq(obj->keys[i], key)) return obj->items[i];
  return nullptr;
}

JVal *json_get(JVal *obj, const char *key) {
  return json_get_str(obj, str_lit(key));
}

JVal *json_at(JVal *arr, i32 i) {
  if (!arr || arr->kind != J_ARR || i < 0 || i >= arr->n) return nullptr;
  return arr->items[i];
}

str json_kind_name(JKind k) {
  switch (k) {
  case J_NULL:
    return str_lit("null");
  case J_BOOL:
    return str_lit("bool");
  case J_NUM:
    return str_lit("number");
  case J_STR:
    return str_lit("string");
  case J_ARR:
    return str_lit("array");
  case J_OBJ:
    return str_lit("object");
  }
  return str_lit("?");
}

bool json_as_i64(JVal *v, i64 *out) {
  if (!v) return false;
  if (v->kind == J_NUM) {
    *out = v->is_int ? v->inum : (i64)v->num;
    return true;
  }
  if (v->kind == J_STR) return str_to_i64(v->s, out);
  return false;
}

bool json_as_f64(JVal *v, f64 *out) {
  if (!v) return false;
  if (v->kind == J_NUM) {
    *out = v->num;
    return true;
  }
  if (v->kind == J_STR) return str_to_f64(v->s, out);
  return false;
}

bool json_as_bool(JVal *v, bool *out) {
  if (!v) return false;
  if (v->kind == J_BOOL) {
    *out = v->b;
    return true;
  }
  if (v->kind == J_NUM) {
    *out = v->inum != 0;
    return true;
  }
  if (v->kind == J_STR) {
    if (str_eq_c(v->s, "true") || str_eq_c(v->s, "1")) {
      *out = true;
      return true;
    }
    if (str_eq_c(v->s, "false") || str_eq_c(v->s, "0")) {
      *out = false;
      return true;
    }
  }
  return false;
}

str json_as_str(JVal *v) {
  if (!v) return (str){"", 0};
  if (v->kind == J_STR) return v->s;
  return (str){"", 0};
}