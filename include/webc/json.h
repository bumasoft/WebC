// SPDX-License-Identifier: Apache-2.0
// WebC — JSON. A streaming writer that walks WEBC_MODEL metadata and a bounded
// parser. Nothing here allocates outside the arena it is handed (SPEC §6.5).
#ifndef WEBC_JSON_H
#define WEBC_JSON_H

#include <webc/core.h>
#include <webc/vec.h>

// ---------------------------------------------------------------- writer

#define WC_JSON_MAX_DEPTH 256   // generous for real payloads, far from a stack blowup

typedef struct JsonWriter {
  Buf b;
  int depth;
  bool first[WC_JSON_MAX_DEPTH];
  bool failed;
  int  max_depth;
} JsonWriter;

void jw_init(JsonWriter *w, Arena *a, size_t cap);
str  jw_finish(JsonWriter *w);
void jw_raw_bytes(JsonWriter *w, const void *p, size_t n);

void jw_obj_begin(JsonWriter *w);
void jw_obj_end(JsonWriter *w);
void jw_arr_begin(JsonWriter *w);
void jw_arr_end(JsonWriter *w);
void jw_key(JsonWriter *w, const char *key);       // key + ':'
void jw_key_str(JsonWriter *w, str key);
void jw_str(JsonWriter *w, str s);
void jw_cstr(JsonWriter *w, const char *s);
void jw_i64(JsonWriter *w, i64 v);
void jw_u64(JsonWriter *w, u64 v);
void jw_f64(JsonWriter *w, f64 v);
void jw_bool(JsonWriter *w, bool v);
void jw_null(JsonWriter *w);
void jw_raw(JsonWriter *w, str json_fragment);      // caller guarantees validity
void jw_i32(JsonWriter *w, i32 v);

// ---------------------------------------------------------------- the two shapes worth a block
//
// A nested object costs four lines with the plain verbs (key, begin, …fields…, end), and the
// four lines are where the bracket goes wrong. These are the `el()` pattern applied to the
// writer — a block that closes itself, `break` and `return` included:
//
//   jw_obj_in(&w, "author") { jw_kv_cstr(&w, "name", p->author_name); }
//   jw_arr_in(&w, "keywords") { for (i32 i = 0; i < n; i++) jw_str(&w, words[i]); }
//
// `jw_kv_*` writes a key *and* its value, and skips both when the value is absent — because
// `"datePublished":""` is not a date, it is a lie a crawler has to guess about. That rule is
// stated once here rather than remembered at every call site.
void jw_kv_str(JsonWriter *w, const char *key, str v);
void jw_kv_cstr(JsonWriter *w, const char *key, const char *v);
void jw_kv_i64(JsonWriter *w, const char *key, i64 v);
void jw_kv_bool(JsonWriter *w, const char *key, bool v);
void jw_kv_raw(JsonWriter *w, const char *key, str json_fragment); // a nested document
// A null-terminated string list. `jw_str_list` is a value (an empty list is `[]`);
// `jw_kv_str_list` is the pair, and writes neither the key nor the array when there is no list.
void jw_str_list(JsonWriter *w, const char *const *items);
void jw_kv_str_list(JsonWriter *w, const char *key, const char *const *items);

void  wc_jw_obj_close(JsonWriter **w);
void  wc_jw_arr_close(JsonWriter **w);
#define jw_obj_in(w_, key_) WC_JW_OPEN_(__COUNTER__, w_, key_, obj)
#define jw_arr_in(w_, key_) WC_JW_OPEN_(__COUNTER__, w_, key_, arr)
// Two pointers: the one the cleanup holds must survive the loop, so the one the condition
// watches is a second declarator rather than the same variable set to null (which the cleanup
// would then find, and close nothing).
#define WC_JW_OPEN_(C_, w_, key_, kind_)                                                 \
  for (JsonWriter *WC_CAT(wc_jw_, C_)                                                    \
           __attribute__((cleanup(WC_CAT(wc_jw_, WC_CAT(kind_, _close))))) =             \
           (jw_key((w_), (key_)), WC_CAT(jw_, WC_CAT(kind_, _begin))((w_)), (w_)),       \
       *WC_CAT(wc_jw_go_, C_) = WC_CAT(wc_jw_, C_);                                      \
       WC_CAT(wc_jw_go_, C_); WC_CAT(wc_jw_go_, C_) = nullptr)

// A writer whose document lives in the current scope — the same arena `sb()` uses, so a
// hand-built document dies with the request that built it and nothing is freed by hand.
static inline JsonWriter jw_doc(void) {
  JsonWriter w;
  jw_init(&w, wc_arena_get(), 256);
  return w;
}

// Convenience one-string encoders (arena allocated).
str json_encode_str(Arena *a, str s);

// JSON that is about to become the body of a <script> element. JSON's own escaping is not
// enough there: a string containing `</script>` ends the element, and a browser reads the rest
// of your document as markup. This escapes `<`, `>`, `&` and the two Unicode line terminators
// as `\uXXXX`, which is the same string to a JSON reader and cannot close a tag.
str json_for_script(Arena *a, str json);

// ---------------------------------------------------------------- parser

typedef enum JKind : u8 { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JKind;

typedef struct JVal JVal;
struct JVal {
  JKind kind;
  bool  b;
  bool  is_int;      // integral number that fit i64
  i64   inum;
  f64   num;
  str   s;           // J_STR
  JVal **items;      // J_ARR
  str   *keys;       // J_OBJ, parallel to items
  i32    n;
};

typedef struct JParse {
  bool ok;
  JVal *v;
  WErr  e;
} JParse;

// Depth-bounded: untrusted input cannot blow the stack (SPEC §11).
JParse json_parse(Arena *a, str text);
JParse json_parse_depth(Arena *a, str text, int max_depth);
JVal  *json_get(JVal *obj, const char *key);
JVal  *json_get_str(JVal *obj, str key);
JVal  *json_at(JVal *arr, i32 i);
str    json_kind_name(JKind k);
bool   json_as_i64(JVal *v, i64 *out);
bool   json_as_f64(JVal *v, f64 *out);
bool   json_as_bool(JVal *v, bool *out);
str    json_as_str(JVal *v);           // empty str when not a string

#endif // WEBC_JSON_H