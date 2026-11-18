// SPDX-License-Identifier: Apache-2.0
// WebC — model metadata consumers: JSON codec, form parser, validators, SQL text.
#include <math.h>
#include <webc/model.h>
#include <webc/multipart.h>

#include "internal.h"

// Validation messages are formatted into a small thread-local ring so that a
// failed request never allocates just to describe the failure.
static str msgf(const char *fmt, ...) {
  static _Thread_local char ring[8][96];
  static _Thread_local int slot;
  char *buf = ring[slot++ & 7];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof ring[0], fmt, ap);
  va_end(ap);
  if (n < 0) n = 0;
  if ((size_t)n >= sizeof ring[0]) n = (int)sizeof ring[0] - 1;
  return (str){buf, (size_t)n};
}

i32 wc_enum_index(const WEnumDesc *e, str name) {
  if (!e) return -1;
  for (i32 i = 0; i < e->n; i++)
    if (str_eq_c(name, e->names[i])) return i;
  return -1;
}

const char *wc_enum_name(const WEnumDesc *e, i32 value) {
  if (!e || value < 0 || value >= e->n) return "";
  return e->names[value];
}

void werrs_add(WErrs *errs, str field, str msg) {
  if (!errs) return;
  if (errs->n >= (i32)WC_COUNTOF(errs->items)) {
    errs->truncated = true;
    return;
  }
  errs->items[errs->n++] = (WFieldErr){.field = field, .msg = msg};
}

const WField *model_field(const WModel *m, str name) {
  for (i32 i = 0; i < m->n; i++)
    if (str_eq_c(name, m->fields[i].name)) return &m->fields[i];
  return nullptr;
}

i32 model_field_index(const WModel *m, str name) {
  for (i32 i = 0; i < m->n; i++)
    if (str_eq_c(name, m->fields[i].name)) return i;
  return -1;
}

static inline const void *fp(const WField *f, const void *obj) {
  return (const char *)obj + f->off;
}
static inline void *fmp(const WField *f, void *obj) {
  return (char *)obj + f->off;
}

// A STR field is (pointer, length); a Text field is a NUL-terminated pointer.
// Every consumer below wants one shape, so they ask for it here.
static str wfield_str_view(const WField *f, const void *obj) {
  const void *p = fp(f, obj);
  if (f->kind == WK_TEXT) {
    const char *s = *(const char *const *)p;
    return s ? (str){s, strlen(s)} : (str){"", 0};
  }
  const str *s = p;
  return s && s->p ? *s : (str){"", 0};
}

// A Text field owns its bytes for as long as the scope does, which is why the
// framework installs the scope arena rather than asking the developer for one.
static char *dup_text(Arena *a, str v) { return str_cdup(a, v); }

// ---------------------------------------------------------------- field access

i64 wfield_get_i64(const WField *f, const void *obj) {
  const void *p = fp(f, obj);
  switch ((WKind)f->kind) {
  case WK_I8:
    return *(const i8 *)p;
  case WK_I16:
    return *(const i16 *)p;
  case WK_I32:
    return *(const i32 *)p;
  case WK_I64:
    return *(const i64 *)p;
  case WK_U8:
    return *(const u8 *)p;
  case WK_U16:
    return *(const u16 *)p;
  case WK_U32:
    return *(const u32 *)p;
  case WK_U64:
    return (i64) * (const u64 *)p;
  case WK_BOOL:
    return *(const bool *)p ? 1 : 0;
  case WK_ENUM:
    return *(const int *)p;
  case WK_F32:
    return (i64) * (const f32 *)p;
  case WK_F64:
    return (i64) * (const f64 *)p;
  case WK_STR:
  case WK_TEXT: {
    str s = wfield_str_view(f, obj);
    i64 x = 0;
    if (s.p && str_to_i64(s, &x)) return x;
    return 0;
  }
  default:
    return 0;
  }
}

f64 wfield_get_f64(const WField *f, const void *obj) {
  const void *p = fp(f, obj);
  switch ((WKind)f->kind) {
  case WK_F32:
    return (f64) * (const f32 *)p;
  case WK_F64:
    return *(const f64 *)p;
  case WK_I64:
    return (f64) * (const i64 *)p;
  default:
    return (f64)wfield_get_i64(f, obj);
  }
}

bool wfield_get_bool(const WField *f, const void *obj) {
  if (f->kind == WK_BOOL) return *(const bool *)fp(f, obj);
  return wfield_get_i64(f, obj) != 0;
}

str wfield_get_str(const WField *f, const void *obj) {
  if (!WC_KIND_IS_TEXT(f->kind)) return (str){"", 0};
  return wfield_str_view(f, obj);
}

Bytes wfield_get_bytes(const WField *f, const void *obj) {
  if (f->kind != WK_BYTES) return (Bytes){nullptr, 0};
  const Bytes *b = fp(f, obj);
  return b->p ? *b : (Bytes){nullptr, 0};
}

void wfield_set_i64(const WField *f, void *obj, i64 v) {
  void *p = fmp(f, obj);
  switch ((WKind)f->kind) {
  case WK_I8:
    *(i8 *)p = (i8)v;
    break;
  case WK_I16:
    *(i16 *)p = (i16)v;
    break;
  case WK_I32:
    *(i32 *)p = (i32)v;
    break;
  case WK_I64:
    *(i64 *)p = v;
    break;
  case WK_U8:
    *(u8 *)p = (u8)v;
    break;
  case WK_U16:
    *(u16 *)p = (u16)v;
    break;
  case WK_U32:
    *(u32 *)p = (u32)v;
    break;
  case WK_U64:
    *(u64 *)p = (u64)v;
    break;
  case WK_BOOL:
    *(bool *)p = v != 0;
    break;
  case WK_ENUM:
    *(int *)p = (int)v;
    break;
  case WK_F32:
    *(f32 *)p = (f32)v;
    break;
  case WK_F64:
    *(f64 *)p = (f64)v;
    break;
  default:
    break;
  }
}

void wfield_set_f64(const WField *f, void *obj, f64 v) {
  void *p = fmp(f, obj);
  switch ((WKind)f->kind) {
  case WK_F32:
    *(f32 *)p = (f32)v;
    break;
  case WK_F64:
    *(f64 *)p = v;
    break;
  default:
    wfield_set_i64(f, obj, (i64)v);
    break;
  }
}

void wfield_set_bool(const WField *f, void *obj, bool v) {
  if (f->kind == WK_BOOL)
    *(bool *)fmp(f, obj) = v;
  else
    wfield_set_i64(f, obj, v ? 1 : 0);
}

void wfield_set_str(Arena *a, const WField *f, void *obj, str v) {
  if (!WC_KIND_IS_TEXT(f->kind)) return;
  if (f->kind == WK_TEXT) {
    *(const char **)fmp(f, obj) = v.p ? dup_text(a, v) : "";
    return;
  }
  str *s = fmp(f, obj);
  *s = str_dup(a, v);
}

i64 wc_vec_key(const WVecDesc *d, const void *elem) {
  if (d && d->sub) {
    const WModel *m = d->sub;
    for (i32 i = 0; i < m->n; i++)
      if (m->fields[i].q.pk) return wfield_get_i64(&m->fields[i], elem);
  }
  return (i64)(uintptr_t)elem;
}

i32 wvec_len(const WVecDesc *d, const void *obj) {
  if (!d) return 0;
  const i32 *n = (const i32 *)((const char *)obj + d->off_n);
  return *n;
}

void *wvec_at(const WVecDesc *d, const void *obj, i32 i) {
  if (!d) return nullptr;
  char *const *at = (char *const *)((const char *)obj + d->off_at);
  i32 n = wvec_len(d, obj);
  if (i < 0 || i >= n) return nullptr;
  return *at + (size_t)i * d->elem_size;
}

void wvec_push_raw(Arena *a, const WVecDesc *d, void *obj, const void *elem) {
  char **at = (char **)((char *)obj + d->off_at);
  i32 *n = (i32 *)((char *)obj + d->off_n);
  i32 *cap = (i32 *)((char *)obj + d->off_cap);
  if (*n >= *cap) {
    i32 ncap = *cap ? *cap * 2 : 4;
    char *np =
        a ? arena_alloc(a, (size_t)ncap * d->elem_size) : wm_alloc((size_t)ncap * d->elem_size);
    if (*at && *n) memcpy(np, *at, (size_t)(*n) * d->elem_size);
    *at = np;
    *cap = ncap;
  }
  memcpy(*at + (size_t)(*n) * d->elem_size, elem, d->elem_size);
  (*n)++;
}

const char *wc_kind_name(WKind k) {
  switch (k) {
  case WK_I8: return "i8";
  case WK_I16: return "i16";
  case WK_I32: return "i32";
  case WK_I64: return "i64";
  case WK_U8: return "u8";
  case WK_U16: return "u16";
  case WK_U32: return "u32";
  case WK_U64: return "u64";
  case WK_F32: return "f32";
  case WK_F64: return "f64";
  case WK_BOOL: return "bool";
  case WK_ENUM: return "enum";
  case WK_STR: return "str";
  case WK_TEXT: return "text";
  case WK_BYTES: return "bytes";
  case WK_VEC: return "vec";
  case WK_MODEL: return "model";
  case WK_TIME: return "time";
  }
  return "?";
}

// ---------------------------------------------------------------- JSON writer

static void jw_scalar(JsonWriter *w, u8 kind, const void *p) {
  switch ((WKind)kind) {
  case WK_I8:
    jw_i64(w, *(const i8 *)p);
    break;
  case WK_I16:
    jw_i64(w, *(const i16 *)p);
    break;
  case WK_I32:
    jw_i64(w, *(const i32 *)p);
    break;
  case WK_I64:
    jw_i64(w, *(const i64 *)p);
    break;
  case WK_U8:
    jw_u64(w, *(const u8 *)p);
    break;
  case WK_U16:
    jw_u64(w, *(const u16 *)p);
    break;
  case WK_U32:
    jw_u64(w, *(const u32 *)p);
    break;
  case WK_U64:
    jw_u64(w, *(const u64 *)p);
    break;
  case WK_F32:
    jw_f64(w, (f64) * (const f32 *)p);
    break;
  case WK_F64:
    jw_f64(w, *(const f64 *)p);
    break;
  case WK_BOOL:
    jw_bool(w, *(const bool *)p);
    break;
  case WK_STR: {
    const str *s = p;
    jw_str(w, s->p ? *s : (str){"", 0});
    break;
  }
  case WK_TEXT: {
    const char *s = *(const char *const *)p;
    jw_str(w, s ? (str){s, strlen(s)} : (str){"", 0});
    break;
  }
  case WK_BYTES: {
    const Bytes *b = p;
    str enc = {"", 0};
    if (b->p && base64_encode(w->b.a, (str){b->p, b->n}, &enc))
      jw_str(w, enc);
    else
      jw_null(w);
    break;
  }
  default:
    jw_null(w);
    break;
  }
}

void model_json_write(JsonWriter *w, const WModel *m, const void *obj) {
  jw_obj_begin(w);
  for (i32 i = 0; i < m->n; i++) {
    const WField *f = &m->fields[i];
    if (f->q.no_json) continue;
    jw_key(w, f->name);
    const void *p = fp(f, obj);
    if (f->kind == WK_VEC && f->vec) {
      jw_arr_begin(w);
      i32 n = wvec_len(f->vec, p);
      for (i32 k = 0; k < n; k++) {
        void *e = wvec_at(f->vec, p, k);
        if (f->vec->elem_kind == WK_MODEL && f->vec->sub)
          model_json_write(w, f->vec->sub, e);
        else
          jw_scalar(w, f->vec->elem_kind, e);
      }
      jw_arr_end(w);
    } else if (f->kind == WK_MODEL && f->sub) {
      model_json_write(w, f->sub, p);
    } else if (f->kind == WK_ENUM && f->en) {
      // the set's name, not its index: the wire is read by people and other langs
      jw_str(w, str_lit(wc_enum_name(f->en, *(const int *)p)));
    } else if (f->q.null && wfield_get_i64(f, obj) == 0) {
      // `.null`: 0 is the absence, and the wire says so. A reader that got 0 here
      // would have to know the sentinel; a reader that gets `null` cannot miss it.
      jw_null(w);
    } else {
      jw_scalar(w, f->kind, p);
    }
  }
  jw_obj_end(w);
}

str model_to_json(Arena *a, const WModel *m, const void *obj) {
  JsonWriter w;
  jw_init(&w, a, 256);
  model_json_write(&w, m, obj);
  return jw_finish(&w);
}

// ---------------------------------------------------------------- documents

static bool read_field_value(Arena *a, const WField *f, JVal *v, void *obj, WErrs *errs);

str doc_write(Arena *a, const WDoc *d, const void *obj) {
  JsonWriter w;
  jw_init(&w, a, 256);
  jw_obj_begin(&w);
  for (i32 i = 0; i < d->n; i++) {
    const WField *f = &d->fields[i];
    const void *p = (const char *)obj + f->off;
    jw_key(&w, f->name);
    if (f->kind == WK_VEC && f->vec) {
      jw_arr_begin(&w);
      i32 n = wvec_len(f->vec, p);
      for (i32 k = 0; k < n; k++) {
        void *e = wvec_at(f->vec, p, k);
        if (f->vec->elem_kind == WK_MODEL && f->vec->sub)
          model_json_write(&w, f->vec->sub, e);
        else
          jw_scalar(&w, f->vec->elem_kind, e);
      }
      jw_arr_end(&w);
    } else if (f->kind == WK_MODEL && f->sub) {
      model_json_write(&w, f->sub, p);
    } else if (f->kind == WK_ENUM && f->en) {
      jw_str(&w, str_lit(wc_enum_name(f->en, *(const int *)p)));
    } else if (f->q.null && wfield_get_i64(f, obj) == 0) {
      jw_null(&w);
    } else {
      jw_scalar(&w, f->kind, p);
    }
  }
  jw_obj_end(&w);
  return jw_finish(&w);
}

// The first field a failed document read stumbled on — the one line a developer
// staring at "the seed does not decode" actually wants (used by tests, and by
// the store's own report).
str doc_read_failed_field(void);

static str g_doc_err_field;

bool doc_read(Arena *a, const WDoc *d, str json, void *out) {
  memset(out, 0, d->size);
  JParse p = json_parse(a, json);
  if (!p.ok || !p.v || p.v->kind != J_OBJ) {
    g_doc_err_field = str_lit("(not a JSON object)");
    return false;
  }
  WErrs errs = {0};
  for (i32 i = 0; i < d->n; i++) {
    const WField *f = &d->fields[i];
    JVal *v = json_get(p.v, f->name);
    if (!v || v->kind == J_NULL) continue; // absent leaves the zero the app chose
    if (!read_field_value(a, f, v, out, &errs))
      g_doc_err_field = str_lit(f->name);
  }
  return errs.n == 0;
}

str doc_read_failed_field(void) { return g_doc_err_field; }

// ---------------------------------------------------------------- JSON reader

static bool coerce_scalar(u8 kind, JVal *v, void *out, str *errmsg) {
  switch ((WKind)kind) {
  case WK_I8:
  case WK_I16:
  case WK_I32:
  case WK_I64:
  case WK_U8:
  case WK_U16:
  case WK_U32:
  case WK_U64: {
    i64 x;
    if (!json_as_i64(v, &x)) {
      *errmsg = str_lit("expected an integer");
      return false;
    }
    switch ((WKind)kind) {
    case WK_I8:
      *(i8 *)out = (i8)x;
      break;
    case WK_I16:
      *(i16 *)out = (i16)x;
      break;
    case WK_I32:
      *(i32 *)out = (i32)x;
      break;
    case WK_I64:
      *(i64 *)out = x;
      break;
    case WK_U8:
      *(u8 *)out = (u8)x;
      break;
    case WK_U16:
      *(u16 *)out = (u16)x;
      break;
    case WK_U32:
      *(u32 *)out = (u32)x;
      break;
    default:
      *(u64 *)out = (u64)x;
      break;
    }
    return true;
  }
  case WK_F32:
  case WK_F64: {
    f64 x;
    if (!json_as_f64(v, &x)) {
      *errmsg = str_lit("expected a number");
      return false;
    }
    if (kind == WK_F32)
      *(f32 *)out = (f32)x;
    else
      *(f64 *)out = x;
    return true;
  }
  case WK_BOOL: {
    bool b;
    if (!json_as_bool(v, &b)) {
      *errmsg = str_lit("expected a boolean");
      return false;
    }
    *(bool *)out = b;
    return true;
  }
  case WK_STR:
  case WK_TEXT: {
    if (v->kind == J_NULL) {
      *(str *)out = (str){"", 0};
      return true;
    }
    if (v->kind != J_STR) {
      *errmsg = str_lit("expected a string");
      return false;
    }
    *(str *)out = v->s;
    return true;
  }
  default:
    *errmsg = str_lit("unsupported field type");
    return false;
  }
}

// The values of a closed set, comma-joined for an error message: "must be one
// of: backlog, todo, doing, done" — the set's own name told nobody anything.
static str enum_values_msg(Arena *a, const WEnumDesc *e) {
  Buf b;
  buf_init(&b, a, 96);
  for (i32 i = 0; i < e->n; i++) buf_printf(&b, "%s%s", i ? ", " : "", e->names[i]);
  return buf_str(&b);
}

static bool read_field_value(Arena *a, const WField *f, JVal *v, void *obj, WErrs *errs) {
  str errmsg = {nullptr, 0};
  // `.null` makes `null` a value this field accepts: it *is* the absence, and the
  // struct spells it 0. Without the qualifier `null` is still a type error below,
  // which is what a field that cannot be absent should say.
  if (f->q.null && v->kind == J_NULL) {
    wfield_set_i64(f, obj, 0);
    return true;
  }
  if (f->kind == WK_ENUM && f->en) {
    i32 idx = -1;
    if (v->kind == J_STR) idx = wc_enum_index(f->en, v->s);
    else if (v->kind == J_NUM && v->is_int) idx = (i32)v->inum; // an integer column
    if (idx < 0 || idx >= f->en->n) {
      werrs_add(errs, str_lit(f->name),
                str_fmt(a, "must be one of: %.*s",
                        (int)enum_values_msg(a, f->en).n, enum_values_msg(a, f->en).p));
      return false;
    }
    *(int *)fmp(f, obj) = idx;
    return true;
  }
  if (f->kind == WK_VEC && f->vec) {
    if (v->kind != J_ARR) {
      werrs_add(errs, str_lit(f->name), str_lit("expected an array"));
      return false;
    }
    for (i32 i = 0; i < v->n; i++) {
      if (f->vec->elem_kind == WK_MODEL && f->vec->sub) {
        char *elem = arena_zalloc(a, f->vec->elem_size);
        // A row that fails inside a collection fails loudly: the element's errors
        // surface under the collection's own name, never as a zeroed row nobody
        // named.
        WModelParse rp = model_from_json_val(a, f->vec->sub, v->items[i], elem);
        if (!rp.ok) {
          if (rp.errs.n) {
            for (i32 e = 0; e < rp.errs.n; e++) {
              str sub = rp.errs.items[e].field;
              werrs_add(errs, str_fmt(a, "%s.%d.%.*s", f->name, i, (int)sub.n, sub.p),
                        rp.errs.items[e].msg);
            }
          } else {
            werrs_add(errs, str_lit(f->name), str_lit("a row of this list failed to parse"));
          }
          return false;
        }
        wvec_push_raw(a, f->vec, fmp(f, obj), elem);
      } else {
        char tmp[32];
        wc_assert(f->vec->elem_size <= sizeof tmp);
        memset(tmp, 0, sizeof tmp);
        if (!coerce_scalar(f->vec->elem_kind, v->items[i], tmp, &errmsg)) {
          werrs_add(errs, str_lit(f->name), errmsg.p ? errmsg : str_lit("bad element"));
          return false;
        }
        wvec_push_raw(a, f->vec, fmp(f, obj), tmp);
      }
    }
    return true;
  }
  if (f->kind == WK_MODEL && f->sub) {
    if (v->kind != J_OBJ) {
      werrs_add(errs, str_lit(f->name), str_lit("expected an object"));
      return false;
    }
    model_from_json_val(a, f->sub, v, fmp(f, obj));
    return true;
  }
  void *dst = fmp(f, obj);
  char tmp[32];
  bool is_str = WC_KIND_IS_TEXT(f->kind);
  void *target = is_str ? (void *)&tmp : dst;
  if (is_str) {
    str s = {"", 0};
    target = &s;
    if (!coerce_scalar(f->kind, v, target, &errmsg)) {
      werrs_add(errs, str_lit(f->name), errmsg.p ? errmsg : str_lit("type error"));
      return false;
    }
    wfield_set_str(a, f, obj, s);
    return true;
  }
  if (!coerce_scalar(f->kind, v, target, &errmsg)) {
    werrs_add(errs, str_lit(f->name), errmsg.p ? errmsg : str_lit("type error"));
    return false;
  }
  return true;
}

bool model_read_json_val(Arena *a, const WModel *m, JVal *v, void *out) {
  memset(out, 0, m->size);
  return model_from_json_val(a, m, v, out).ok;
}

bool model_read_json(Arena *a, const WModel *m, str json, void *out) {
  memset(out, 0, m->size);
  return model_from_json(a, m, json, out).ok;
}

WModelParse model_from_json_val(Arena *a, const WModel *m, JVal *v, void *out) {
  WModelParse r = {.ok = true, .e = {.kind = WEBC_OK}};
  if (!v || v->kind != J_OBJ) {
    r.ok = false;
    r.e = werr_c(WEBC_EPARSE, "expected a JSON object");
    return r;
  }
  for (i32 i = 0; i < v->n; i++) {
    const WField *f = model_field(m, v->keys[i]);
    if (!f) continue; // unknown keys are ignored: forward compatible
    read_field_value(a, f, v->items[i], out, &r.errs);
  }
  if (r.errs.n) {
    // A field that failed while reading (an enum stranger, a bad element) is a
    // *validation* failure and names its field — flattening it into "invalid
    // body" is how a closed-set 422 lost the one fact it existed to carry.
    r.ok = false;
    r.e = werr(WEBC_EVALIDATION, str_fmt(a, "%.*s: %.*s",
                                        (int)r.errs.items[0].field.n, r.errs.items[0].field.p,
                                        (int)r.errs.items[0].msg.n, r.errs.items[0].msg.p));
  }
  return r;
}

WModelParse model_from_json_depth(Arena *a, const WModel *m, str json, void *out, i32 max_depth) {
  JParse p = json_parse_depth(a, json, max_depth);
  if (!p.ok) return (WModelParse){.ok = false, .e = p.e};
  return model_from_json_val(a, m, p.v, out);
}

WModelParse model_from_json(Arena *a, const WModel *m, str json, void *out) {
  return model_from_json_depth(a, m, json, out, 0);
}

// ---------------------------------------------------------------- form reader

static bool form_bool(str v, bool *out) {
  if (str_eq_c(v, "on") || str_eq_c(v, "true") || str_eq_c(v, "1") || str_eq_c(v, "yes")) {
    *out = true;
    return true;
  }
  if (str_eq_c(v, "off") || str_eq_c(v, "false") || str_eq_c(v, "0") || str_eq_c(v, "") ||
      str_eq_c(v, "no")) {
    *out = false;
    return true;
  }
  return false;
}

// One field of a form body into the struct. Shared by the urlencoded parser and
// the multipart one, so a browser posting multipart and curl posting urlencoded
// cannot disagree about what the same form means.
static void model_form_field(Arena *a, const WModel *m, str key, str val, void *out, WErrs *errs,
                             bool *ok) {
  const WField *f = model_field(m, key);
  if (!f) return;
  if (f->q.null && val.n == 0) {
    // An empty input is how a browser says "nothing chosen": that is 0, i.e. NULL,
    // and not a parse failure. The app's cover picker is exactly this field.
    wfield_set_i64(f, out, 0);
    return;
  }
  if (f->kind == WK_ENUM && f->en) {
    i32 idx = wc_enum_index(f->en, str_trim(val));
    if (idx < 0) {
      werrs_add(errs, key, str_fmt(a, "must be one of: %.*s", (int)enum_values_msg(a, f->en).n,
                                   enum_values_msg(a, f->en).p));
      *ok = false;
    } else {
      *(int *)fmp(f, out) = idx;
    }
    return;
  }
  if (WC_KIND_IS_TEXT(f->kind)) {
    str s = f->q.trim ? str_trim(val) : val;
    wfield_set_str(a, f, out, s);
  } else if (f->kind == WK_BOOL) {
    bool b = false;
    if (!form_bool(str_trim(val), &b)) {
      werrs_add(errs, key, str_lit("expected a boolean"));
      *ok = false;
    } else {
      wfield_set_bool(f, out, b);
    }
  } else if (f->vec && (f->vec->elem_kind == WK_STR)) {
    size_t k = 0;
    while (k <= val.n) {
      size_t e = k;
      while (e < val.n && val.p[e] != ',') e++;
      str item = str_trim(str_sub(val, k, e));
      if (item.n) wvec_push_raw(a, f->vec, fmp(f, out), &item);
      if (e >= val.n) break;
      k = e + 1;
    }
  } else if (f->vec && f->vec->elem_kind != WK_MODEL) {
    char tmp[16] = {0};
    str errmsg = {nullptr, 0};
    JVal num = {.kind = J_NUM};
    i64 iv;
    f64 fv;
    if (str_to_i64(str_trim(val), &iv)) {
      num.is_int = true;
      num.inum = iv;
      num.num = (f64)iv;
    } else if (str_to_f64(str_trim(val), &fv)) {
      num.num = fv;
      num.inum = (i64)fv;
    } else {
      werrs_add(errs, key, str_lit("expected a number"));
      *ok = false;
      return;
    }
    if (!coerce_scalar(f->vec->elem_kind, &num, tmp, &errmsg)) {
      werrs_add(errs, key, errmsg.p ? errmsg : str_lit("type error"));
      *ok = false;
    } else {
      wvec_push_raw(a, f->vec, fmp(f, out), tmp);
    }
  } else {
    char tmp[32] = {0};
    str errmsg = {nullptr, 0};
    JVal num = {.kind = J_STR, .s = val};
    if (!coerce_scalar(f->kind, &num, tmp, &errmsg)) {
      werrs_add(errs, key, errmsg.p ? errmsg : str_lit("type error"));
      *ok = false;
    } else {
      memcpy(fmp(f, out), tmp, f->size);
    }
  }
}

// multipart/form-data: the same fields, from parts instead of pairs. A part with a
// filename is a file and is not a form field, so it is ignored here — the handler
// reads it with multipart_file() (SPEC §11).
WModelParse model_from_multipart(Arena *a, const WModel *m, const struct WMultipart *mp, void *out) {
  WModelParse r = {.ok = true, .e = {.kind = WEBC_OK}};
  for (i32 i = 0; i < mp->n; i++) {
    if (mp->parts[i].is_file) continue;
    model_form_field(a, m, mp->parts[i].name, mp->parts[i].data, out, &r.errs, &r.ok);
  }
  if (r.errs.n) {
    r.ok = false;
    r.e = werr_c(WEBC_EPARSE, "invalid form body");
  }
  return r;
}

WModelParse model_from_form(Arena *a, const WModel *m, str body, void *out) {
  WModelParse r = {.ok = true, .e = {.kind = WEBC_OK}};
  str raw_k, raw_v;
  for (size_t i = 0; wc_kv_next(body, &i, &raw_k, &raw_v);) {
    // One field, applied the one way: the same function the multipart parser uses, so
    // an enum written as a string, a `.null` field left empty and a boolean spelled
    // "on" mean one thing however the browser sent them.
    model_form_field(a, m, url_decode(a, raw_k, true), url_decode(a, raw_v, true), out,
                     &r.errs, &r.ok);
  }
  if (r.errs.n) {
    r.ok = false;
    r.e = werr_c(WEBC_EPARSE, "invalid form body");
  }
  return r;
}

// ---------------------------------------------------------------- validation

bool model_validate_field(const WField *f, const void *obj, WErrs *errs) {
  bool ok = true;
  const void *p = fp(f, obj);
  str name = str_lit(f->name);
  if (WC_KIND_IS_TEXT(f->kind)) {
    str s = wfield_str_view(f, obj);
    size_t len = s.n;
    if ((f->q.not_null || f->q.required) && len == 0) {
      werrs_add(errs, name, str_lit("required"));
      ok = false;
    }
    if (f->q.max > 0 && len > (size_t)f->q.max) {
      werrs_add(errs, name, msgf("must be at most %d characters", f->q.max));
      ok = false;
    }
    if (f->q.min > 0 && len < (size_t)f->q.min) {
      werrs_add(errs, name, msgf("must be at least %d characters", f->q.min));
      ok = false;
    }
    if (f->q.format && len) {
      str fmt = str_lit(f->q.format);
      bool good = true;
      if (str_eq_c(fmt, "email")) {
        i64 at = str_index(s, '@');
        good = at > 0 && at < (i64)len - 3 && str_index(s, '.') > at;
      } else if (str_eq_c(fmt, "uuid")) {
        good = len == 36;
      } else if (str_eq_c(fmt, "url")) {
        good = str_has_prefix_c(s, "http://") || str_has_prefix_c(s, "https://");
      }
      if (!good) {
        werrs_add(errs, name, msgf("must be a valid %s", f->q.format));
        ok = false;
      }
    }
    return ok;
  }
  if (f->kind == WK_VEC) {
    if ((f->q.not_null || f->q.required) && (!f->vec || wvec_len(f->vec, p) == 0)) {
      werrs_add(errs, name, str_lit("required"));
      return false;
    }
    return true;
  }
  if (f->kind == WK_MODEL && f->sub) return model_validate(f->sub, p, errs);
  if (f->kind == WK_ENUM && f->en) {
    i32 v = *(const int *)p;
    if (v < 0 || v >= f->en->n) {
      werrs_add(errs, name, msgf("not a value of %s", f->en->name));
      return false;
    }
    return true;
  }
  bool is_float = f->kind == WK_F32 || f->kind == WK_F64;
  if (is_float) {
    f64 v = wfield_get_f64(f, obj);
    if (WC_HAS_GTE(f->q) && v < f->q.gte) {
      werrs_add(errs, name, msgf("must be >= %g", f->q.gte));
      ok = false;
    }
    if (WC_HAS_LTE(f->q) && v > f->q.lte) {
      werrs_add(errs, name, msgf("must be <= %g", f->q.lte));
      ok = false;
    }
  } else if (f->kind != WK_BOOL) {
    i64 v = wfield_get_i64(f, obj);
    if (WC_HAS_GTE(f->q) && (f64)v < f->q.gte) {
      werrs_add(errs, name, msgf("must be >= %g", f->q.gte));
      ok = false;
    }
    if (WC_HAS_LTE(f->q) && (f64)v > f->q.lte) {
      werrs_add(errs, name, msgf("must be <= %g", f->q.lte));
      ok = false;
    }
  }
  return ok;
}

bool model_validate(const WModel *m, const void *obj, WErrs *errs) {
  bool ok = true;
  for (i32 i = 0; i < m->n; i++)
    if (!model_validate_field(&m->fields[i], obj, errs)) ok = false;
  return ok;
}

// ---------------------------------------------------------------- SQL

const char *model_column_name(const WField *f) {
  return f->q.sql ? f->q.sql : f->name;
}

str model_sql_column_type(const WField *f) {
  if (f->kind == WK_ENUM) return str_lit("TEXT");
  if (f->kind == WK_BOOL) return str_lit("INTEGER");
  if (f->kind == WK_VEC || f->kind == WK_MODEL || f->kind == WK_BYTES) return str_lit("TEXT");
  if (WC_KIND_IS_TEXT(f->kind)) return str_lit("TEXT");
  if (f->kind == WK_F32 || f->kind == WK_F64) return str_lit("REAL");
  return str_lit("INTEGER");
}

str model_sql_columns(Arena *a, const WModel *m) {
  Buf b;
  buf_init(&b, a, 128);
  for (i32 i = 0; i < m->n; i++) {
    if (i) buf_puts(&b, ", ");
    buf_puts(&b, model_column_name(&m->fields[i]));
  }
  return buf_str(&b);
}

str model_sql_placeholders(Arena *a, const WModel *m) {
  Buf b;
  buf_init(&b, a, 64);
  for (i32 i = 0; i < m->n; i++) {
    if (i) buf_puts(&b, ", ");
    buf_puts(&b, "?");
  }
  return buf_str(&b);
}

str model_sql_insert(Arena *a, const WModel *m, const char *table) {
  return str_fmt(a, "INSERT INTO %s (%s) VALUES (%s)", table, str_cstr(a, model_sql_columns(a, m)),
                 str_cstr(a, model_sql_placeholders(a, m)));
}

str model_sql_select(Arena *a, const WModel *m, const char *table) {
  return str_fmt(a, "SELECT %s FROM %s", str_cstr(a, model_sql_columns(a, m)), table);
}

str model_sql_update(Arena *a, const WModel *m, const char *table) {
  Buf b;
  buf_init(&b, a, 128);
  buf_printf(&b, "UPDATE %s SET ", table);
  bool first = true;
  for (i32 i = 0; i < m->n; i++) {
    if (m->fields[i].q.pk) continue;
    if (!first) buf_puts(&b, ", ");
    buf_printf(&b, "%s = ?", model_column_name(&m->fields[i]));
    first = false;
  }
  bool any_pk = false;
  for (i32 i = 0; i < m->n; i++) {
    if (!m->fields[i].q.pk) continue;
    buf_printf(&b, "%s%s = ?", any_pk ? " AND " : " WHERE ", model_column_name(&m->fields[i]));
    any_pk = true;
  }
  if (!any_pk) buf_puts(&b, " WHERE rowid = ?");
  return buf_str(&b);
}

str model_sql_create_table(Arena *a, const WModel *m, const char *table) {
  Buf b;
  buf_init(&b, a, 256);
  buf_printf(&b, "CREATE TABLE IF NOT EXISTS %s (", table);
  for (i32 i = 0; i < m->n; i++) {
    const WField *f = &m->fields[i];
    if (i) buf_puts(&b, ", ");
    buf_printf(&b, "%s %s", model_column_name(f), str_cstr(a, model_sql_column_type(f)));
    if (f->q.pk) {
      buf_puts(&b, " PRIMARY KEY");
      if (f->kind >= WK_I8 && f->kind <= WK_U64) buf_puts(&b, " AUTOINCREMENT");
    }
    if (f->q.not_null) buf_puts(&b, " NOT NULL");
    if (f->q.unique) buf_puts(&b, " UNIQUE");
    if (f->q.references) buf_printf(&b, " REFERENCES %s", f->q.references);
    if (f->q.def) buf_printf(&b, " DEFAULT %s", f->q.def);
  }
  buf_puts(&b, ")");
  return buf_str(&b);
}
// ---------------------------------------------------------------- table names

// "Account" -> "accounts", "LedgerEntry" -> "ledger_entries", "Person" -> "people".
char *wc_snake_plural(const char *name) {
  size_t n = strlen(name);
  char *out = malloc(n * 2 + 4);
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z') {
      if (i > 0 && !(name[i - 1] >= 'A' && name[i - 1] <= 'Z')) out[o++] = '_';
      out[o++] = (char)(c + 32);
    } else {
      out[o++] = c;
    }
  }
  out[o] = 0;
  // pluralise
  if (o >= 2 && out[o - 1] == 'y' && !strchr("aeiou", out[o - 2])) {
    out[o - 1] = 'i';
    out[o++] = 'e';
    out[o++] = 's';
  } else if (o >= 2 && (out[o - 1] == 's' || out[o - 1] == 'x' || out[o - 1] == 'z' ||
                        (o >= 2 && out[o - 2] == 'c' && out[o - 1] == 'h') ||
                        (o >= 2 && out[o - 2] == 's' && out[o - 1] == 'h'))) {
    out[o++] = 'e';
    out[o++] = 's';
  } else {
    out[o++] = 's';
  }
  out[o] = 0;
  return out;
}

// ---------------------------------------------------------------- the registry
//
// Every WEBC_MODEL registers itself at load time, so a tool can ask "what models
// does this application have?" and get the descriptors the queries actually use.
// Lives here rather than beside the diagnostics because a wasm build links model.c
// and not the server-side diagnostics, and a model registers itself either way.
//
// Deduplicated by name: WEBC_MODEL in a header expands once per translation unit,
// and the same model seen twice is the same model.
#define WC_MODELS_MAX 256
static const WModel *g_models[WC_MODELS_MAX];
static i32 g_nmodels;

void model_register(const WModel *m) {
  if (!m || !m->name) return;
  for (i32 i = 0; i < g_nmodels; i++)
    if (strcmp(g_models[i]->name, m->name) == 0) return;
  if (g_nmodels >= WC_MODELS_MAX) return; // 256 models is not a schema, it is a mistake
  g_models[g_nmodels++] = m;
}

i32 model_registry_count(void) { return g_nmodels; }

const WModel *model_registry_at(i32 i) { return i >= 0 && i < g_nmodels ? g_models[i] : nullptr; }
