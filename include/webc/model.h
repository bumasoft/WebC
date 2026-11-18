// SPDX-License-Identifier: Apache-2.0
// WebC — the model macro. One hand-written line per struct is the whole price of
// typed JSON, SQL, forms and validation (SPEC §5.2).
//
//   WEBC_MODEL(Account,
//     (id,      i64,  .pk = true),
//     (name,    str,  .not_null = true, .trim = true, .max = 64),
//     (balance, f64,  .not_null = true, .gte = 0),
//     (active,  bool, .not_null = true, .default_false = true))
//
// The trick that makes this work without a generator: **a function scope is the
// expansion namespace.** The macro emits a constructor that declares
// `typedef T WC_self_t;` and `WModel *WC_m = &T_model;` locally, so every field
// group can say offsetof(WC_self_t, id) without knowing which model it belongs
// to. Function-local typedefs mean two models in one file cannot collide.
#ifndef WEBC_MODEL_H
#define WEBC_MODEL_H

#include <webc/core.h>
#include <webc/json.h>
#include <webc/kinds.h>
#include <webc/vec.h>

// ---------------------------------------------------------------- field errors

typedef struct WFieldErr {
  str field;
  str msg;
} WFieldErr;

typedef struct WErrs {
  WFieldErr items[32];
  i32 n;
  bool truncated;
} WErrs;

void werrs_add(WErrs *errs, str field, str msg);
static inline bool werrs_ok(const WErrs *e) { return e->n == 0; }

// ---------------------------------------------------------------- the macro

#define WC_MEMBER(g)    WC_MEMBER_I g
#define WC_MEMBER_I(id, FT, ...) FT id;

#define WC_FIELD(g)     WC_FIELD_I g
// `.min` / `.max` bound a *string's* length; a numeric field uses `.gte` / `.lte`.
// Writing `.min = 18` on an i32 looks like a numeric bound and silently validated
// nothing, so it is a compile error instead: a validator that does not run is worse
// than no validator at all.
//
// `.null` is only for an integer column (0 is the value this side spells NULL), and
// it contradicts `.required` and `.pk`: a field that may be absent cannot also be
// mandatory, and a primary key that could be NULL is not one.
#define WC_FIELD_I(id, FT, ...)                                                \
  static_assert(WC_KIND_##FT == WK_STR || WC_KIND_##FT == WK_TEXT ||          \
                    (WC_QUAL(__VA_ARGS__).min == 0 && WC_QUAL(__VA_ARGS__).max == 0), \
                #id ": .min/.max are string-length bounds — use .gte/.lte for numbers"); \
  static_assert(!WC_QUAL(__VA_ARGS__).null || WC_KIND_IS_INT(WC_KIND_##FT),   \
                #id ": .null is for an integer column — 0 is the NULL this side reads; " \
                       "a float's 0 is a value and an enum's 0 is its first member"); \
  static_assert(!WC_QUAL(__VA_ARGS__).null || !WC_QUAL(__VA_ARGS__).required, \
                #id ": .required and .null contradict each other — a field that may be " \
                       "absent cannot also be mandatory"); \
  static_assert(!WC_QUAL(__VA_ARGS__).null || !WC_QUAL(__VA_ARGS__).pk,       \
                #id ": a primary key is never NULL");                          \
  WC_m->fields[WC_m->n++] = (WField){                                          \
      .name = #id,                                                             \
      .kind = (u8)WC_KIND_##FT,                                                \
      .off = (u32)offsetof(WC_self_t, id),                                     \
      .size = (u32)sizeof(FT),                                                 \
      .sub = WC_SUBMODEL_##FT,                                                 \
      .vec = WC_VECOF_##FT,                                                    \
      .en = WC_ENUMOF_##FT,                                                    \
      .q = WC_QUAL(__VA_ARGS__) };

// The constructor runs before main. The descriptor table is sized by the field list
// itself (WC_NARG), so a model is as wide as it says it is: a fixed
// `WField T##_fields[16]` makes the seventeenth field a compile error with no way out
// short of a -D on the whole program, which a framework should not ask of a story
// table with a slug, a deck, a body and a byline.
#define WEBC_MODEL(T, ...)                                                     \
  typedef struct T { WC_MAP_S(WC_MEMBER, __VA_ARGS__) } T;                     \
  enum { WC_KIND_##T = WK_MODEL };                                             \
  [[maybe_unused]] static WField T##_fields[WC_NARG(__VA_ARGS__)];              \
  [[maybe_unused]] static WModel T##_model = {                                 \
      .name = #T, .size = (u32)sizeof(T),                                       \
      .fields = T##_fields, .n = 0, .cap = WC_NARG(__VA_ARGS__) };             \
  [[maybe_unused]] static const WModel *const WC_SUBMODEL_##T = &T##_model;    \
  [[maybe_unused]] static const WVecDesc *const WC_VECOF_##T = nullptr;         \
  [[maybe_unused]] static const void *const WC_ENUMOF_##T = nullptr;             \
  __attribute__((constructor)) static void T##_model_boot(void) {              \
    typedef T WC_self_t;                                                       \
    WModel *WC_m = &T##_model;                                                 \
    WC_MAP_S(WC_FIELD, __VA_ARGS__)                                            \
    wc_assert(WC_m->n == WC_NARG(__VA_ARGS__));                                \
    if (!WC_m->table) WC_m->table = wc_snake_plural(#T);                       \
    model_register(WC_m);                                                      \
  }                                                                            \
  static inline const WModel *T##_modelof(void) { return &T##_model; }

// A closed set, usable as a model field:
//
//   WEBC_ENUM(Lane, backlog, todo, doing, done)
//   WEBC_MODEL(Card, ..., (lane, Lane))
//
// The field is an int in the struct and its NAME everywhere bytes are exchanged —
// JSON, forms, SQL text — and a value outside the set is a 422 that names the
// field, not a card that silently renders in no lane. The helpers (lane_index,
// lane_ok, lane_name) are generated with the enum's own name, lowercase.
#define WC_ENUM_MEMBER(x) x,
#define WC_ENUM_STR(x) #x,
#define WEBC_ENUM(T, ...)                                                     \
  typedef enum T { WC_MAP_S(WC_ENUM_MEMBER, __VA_ARGS__) } T;                 \
  enum { T##_N = WC_NARG(__VA_ARGS__) };                                      \
  static const char *const T##_names[] =                                      \
      { WC_MAP_S(WC_ENUM_STR, __VA_ARGS__) };                                  \
  static const WEnumDesc T##_enum = { .name = #T, .n = T##_N, .names = T##_names }; \
  enum { WC_KIND_##T = WK_ENUM };                                             \
  [[maybe_unused]] static const WModel *const WC_SUBMODEL_##T = nullptr;      \
  [[maybe_unused]] static const WVecDesc *const WC_VECOF_##T = nullptr;        \
  [[maybe_unused]] static const WEnumDesc *const WC_ENUMOF_##T = &T##_enum;    \
  __attribute__((constructor)) static void T##_enum_boot(void) { (void)&T##_enum; } \
  static inline i32 T##_index(str s) { return wc_enum_index(&T##_enum, s); }   \
  static inline bool T##_ok(str s) { return wc_enum_index(&T##_enum, s) >= 0; } \
  static inline const char *T##_name(T v) {                                   \
    return (i32)v >= 0 && (i32)v < T##_N ? T##_names[(i32)v] : "";             \
  }

// ---------------------------------------------------------------- documents
//
// A model is a row; a DOCUMENT is the shape a live page actually sends: several
// collections and scalars under one JSON object, one declaration for both
// directions. Until this existed, an app's snapshot was hand-encoded with jw_*
// and hand-decoded with json_get, and adding a field meant five uncoordinated
// edits — the exact drift WEBC_MODEL exists to prevent, one level up.
//
//   WEBC_DOCUMENT(BoardDoc,
//                  (me,       User),       // one row, written nested; a missing
//                  (users,    VecUser),    // key on the way in leaves it zeroed
//                  (online,   VecI64),
//                  (cards,    VecCard),
//                  (chat,     VecChatMsg))
//
//   str  BoardDoc_write(Arena *a, BoardDoc *d);        // generated name
//   bool BoardDoc_read(Arena *a, str json, BoardDoc *out);
//
// Field types may be models, vectors of models, vectors of scalars, enums, or the
// scalar kinds. Everything the model codec checks — enums by name, unknown keys
// ignored, a stranger in a closed set rejected naming the field — applies.

typedef struct WDoc {
  const char *name;
  i32 n, cap;
  WField *fields; // the same descriptor a model uses: the codec is one codec
  u32 size;
} WDoc;

str doc_write(Arena *a, const WDoc *d, const void *obj);
bool doc_read(Arena *a, const WDoc *d, str json, void *out); // zeroes out first
str doc_read_failed_field(void); // the field the last failed doc_read stumbled on

#define WC_DOC_FIELD(g) WC_DOC_FIELD_I g
#define WC_DOC_FIELD_I(id, FT, ...)                                            \
  WC_d->fields[WC_d->n++] = (WField){                                          \
      .name = #id,                                                             \
      .kind = (u8)WC_KIND_##FT,                                                \
      .off = (u32)offsetof(WC_self_t, id),                                     \
      .size = (u32)sizeof(FT),                                                 \
      .sub = WC_SUBMODEL_##FT,                                                 \
      .vec = WC_VECOF_##FT,                                                    \
      .en = WC_ENUMOF_##FT,                                                    \
      .q = {0}};

#define WEBC_DOCUMENT(T, ...)                                                  \
  typedef struct T { WC_MAP_S(WC_MEMBER, __VA_ARGS__) } T;                     \
  static WField T##_fields[WC_NARG(__VA_ARGS__)];                              \
  static WDoc T##_doc = {.name = #T,                                           \
                         .fields = T##_fields,                                 \
                         .n = 0,                                               \
                         .cap = WC_NARG(__VA_ARGS__),                          \
                         .size = (u32)sizeof(T)};                              \
  static inline const WDoc *T##_docof(void) { return &T##_doc; }               \
  static inline str T##_write(Arena *a_, const T *o_) {                        \
    return doc_write(a_, &T##_doc, o_);                                         \
  }                                                                            \
  static inline bool T##_read(Arena *a_, str j_, T *o_) {                      \
    return doc_read(a_, &T##_doc, j_, o_);                                      \
  }                                                                            \
  __attribute__((constructor)) static void T##_doc_boot(void) {                \
    typedef T WC_self_t;                                                       \
    WDoc *WC_d = &T##_doc;                                                     \
    WC_MAP_S(WC_DOC_FIELD, __VA_ARGS__)                                         \
  }

// Tables default to the snake_case plural of the model name; override with
// WEBC_MODEL_TABLE(Account, "people") after the model.
#define WEBC_MODEL_TABLE(T, name_)                                             \
  __attribute__((constructor)) static void T##_table_boot(void) {              \
    T##_model.table = name_;                                                   \
  }

// The route a row of this model lives at, so `link(Story, st)` needs no arguments:
//
//   WEBC_MODEL_ROUTE(Story, "/a/:slug")   ->  link(Story, st) == "/a/the-last-ferry"
//
// Every `:name` in the pattern is a field of the model, used as the value.
// A model the framework itself uses (RowId, the one-field form body) is registered
// like any other so `body(RowId)` works, but it owns no table: a schema check must
// not ask an app to create one.
#define WEBC_MODEL_INTERNAL(T)                                                 \
  __attribute__((constructor)) static void T##_internal_boot(void) {           \
    T##_model.internal = true;                                                 \
  }

// A row you computed, not a table you store:
//
//   WEBC_ROW(CountRow, (key, Text), (value, Text))
//   Vec(CountRow)
//   VecCountRow *rows = vec_new(CountRow);   // push, for_each, json(200, rows)
//
// A nav bar, a table of contents, a KPI strip and a "referrer -> count" panel are all
// rows that never touch the database, and written by hand each one is a fixed-size array
// and a counter, because a model reads as "a table". This is the same declaration
// minus the table: `db_save()` on it panics with a sentence instead of writing to a table
// named after the struct, and the schema check skips it.
//
// `WEBC_MODEL_INTERNAL(T)` is what this expands to; write `WEBC_ROW`.
#define WEBC_ROW(T, ...)                                                       \
  WEBC_MODEL(T, __VA_ARGS__)                                                   \
  WEBC_MODEL_INTERNAL(T)

void wc_url_register(const char *name, const char *pattern);

// The route is registered under the model's own name as well, so every one of
// `link_to(Story, st)`, `url_to("Story", "slug", s)` and a view that builds the URL by hand
// agree by construction. A model route that could be linked but not named was the one
// place a shared view had to spell a path for itself.
#define WEBC_MODEL_ROUTE(T, route_)                                            \
  __attribute__((constructor)) static void T##_route_boot(void) {              \
    T##_model.route = route_;                                                  \
    wc_url_register(#T, route_);                                               \
  }
char *wc_snake_plural(const char *name);   // "Account" -> "accounts"

// ---------------------------------------------------------------- JSON

void model_json_write(JsonWriter *w, const WModel *m, const void *obj);
str  model_to_json(Arena *a, const WModel *m, const void *obj);

typedef struct WModelParse {
  bool ok;
  WErr e;
  WErrs errs;
} WModelParse;

WModelParse model_from_json(Arena *a, const WModel *m, str json, void *out);
// Same, with an explicit nesting limit: the request path passes the configured one
// so a body cannot choose its own depth (§11).
WModelParse model_from_json_depth(Arena *a, const WModel *m, str json, void *out, i32 max_depth);
WModelParse model_from_json_val(Arena *a, const WModel *m, JVal *v, void *out);

// The browser-half spellings. The codec links in wasm too (it always did; the myth that
// it "drags in a server-only symbol" cost every streaming app a hand-mirrored decoder),
// and a decoded document deserves the guarantees a request body gets: `out` zeroed
// first, so a partial document cannot leave stale fields behind. `model_read_json` takes
// the document as text — the form a seed or an SSE frame arrives in.
bool model_read_json_val(Arena *a, const WModel *m, JVal *v, void *out);
bool model_read_json(Arena *a, const WModel *m, str json, void *out);
WModelParse model_from_form(Arena *a, const WModel *m, str urlencoded, void *out);
struct WMultipart;
WModelParse model_from_multipart(Arena *a, const WModel *m, const struct WMultipart *mp,
                                 void *out);

// ---------------------------------------------------------------- validation

// The same validators the frontend runs (§7.4): no second implementation.
bool model_validate(const WModel *m, const void *obj, WErrs *errs);
bool model_validate_field(const WField *f, const void *obj, WErrs *errs);

// ---------------------------------------------------------------- SQL

str  model_sql_columns(Arena *a, const WModel *m);          // "id, name, balance"
str  model_sql_placeholders(Arena *a, const WModel *m);     // "?, ?, ?"
str  model_sql_insert(Arena *a, const WModel *m, const char *table);
str  model_sql_update(Arena *a, const WModel *m, const char *table);  // by pk
str  model_sql_select(Arena *a, const WModel *m, const char *table);
str  model_sql_create_table(Arena *a, const WModel *m, const char *table);
str  model_sql_column_type(const WField *f);                // portable SQL affinity
const char *model_column_name(const WField *f);             // q.sql override

// ---------------------------------------------------------------- field access

i64   wfield_get_i64(const WField *f, const void *obj);
f64   wfield_get_f64(const WField *f, const void *obj);
bool  wfield_get_bool(const WField *f, const void *obj);
str   wfield_get_str(const WField *f, const void *obj);
Bytes wfield_get_bytes(const WField *f, const void *obj);

void wfield_set_i64(const WField *f, void *obj, i64 v);
void wfield_set_f64(const WField *f, void *obj, f64 v);
void wfield_set_bool(const WField *f, void *obj, bool v);
void wfield_set_str(Arena *a, const WField *f, void *obj, str v);

// Vector access through a descriptor (element pointers are arena-owned).
i32   wvec_len(const WVecDesc *d, const void *obj);
void *wvec_at(const WVecDesc *d, const void *obj, i32 i);
void  wvec_push_raw(Arena *a, const WVecDesc *d, void *obj, const void *elem);

const WField *model_field(const WModel *m, str name);
i32           model_field_index(const WModel *m, str name);

// ---------------------------------------------------------------- the registry
//
// Every WEBC_MODEL registers itself when the process loads, so a tool can ask
// "what models does this application have?" and get the real answer — the same
// descriptors the queries use. `webc check` (schema drift) and `webc seed` are the
// two callers; an app can print its own schema with three lines.
void model_register(const WModel *m);
i32 model_registry_count(void);
const WModel *model_registry_at(i32 i);

#endif // WEBC_MODEL_H