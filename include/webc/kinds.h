// SPDX-License-Identifier: Apache-2.0
// WebC — model metadata vocabulary. Shared by the JSON codec, the form parser,
// the validators and the SQL mappers: one descriptor table, four consumers, and
// no generator. Included by vec.h so a vector can describe its
// element type to the model machinery.
#ifndef WEBC_KINDS_H
#define WEBC_KINDS_H

#include <webc/core.h>
#include <webc/pp.h>

typedef enum WKind : u8 {
  WK_I8, WK_I16, WK_I32, WK_I64,
  WK_U8, WK_U16, WK_U32, WK_U64,
  WK_F32, WK_F64, WK_BOOL,
  WK_STR, WK_TEXT, WK_BYTES,
  WK_VEC, WK_MODEL, WK_TIME,
  WK_ENUM, // one of a closed set (WEBC_ENUM): an int in the struct, a name on the wire
} WKind;

// Every string path (JSON, form, SQL, validation, rendering) treats the two the
// same; they differ only in what a model field holds: `str` is (pointer, length),
// `Text` is a NUL-terminated `const char *`.
#define WC_KIND_IS_TEXT(k_) ((k_) == WK_STR || (k_) == WK_TEXT)
// The integer kinds, in the one order the enum has them in. `.null` is only
// meaningful here: a float's 0 is a value, and an enum's 0 is its first member.
#define WC_KIND_IS_INT(k_) ((k_) >= WK_I8 && (k_) <= WK_U64)

typedef struct WModel WModel;
typedef struct WVecDesc WVecDesc;

// Qualifiers are struct-initialiser syntax because that is what a C programmer
// already reads as "named options" — and the compiler checks them (§5.2).
typedef struct WQual {
  bool pk;            // primary key
  bool not_null;
  bool required;      // must be present and non-empty
  bool trim;          // strings: trim before validation
  bool unique;
  bool default_false;
  bool no_json;       // never serialised (secrets stay in)
  bool json_array;    // Vec fields: array of scalars
  bool sensitive;     // redacted by the logger
  i32  max, min;      // string length bounds (0 = unset)
  f64  gte, lte;      // numeric bounds; NaN = unset
  const char *format; // "email", "uuid", "url", "date"
  const char *sql;    // column name override
  const char *def;    // SQL default expression literal
  const char *doc;    // documentation string, surfaced by `webc doctor`
  // `.null = true` — an integer column that is NULL-able, with 0 as the value this
  // side reads and writes for it. The struct stays plain C; the framework
  // normalises at every edge: SQL writes NULL, reads NULL back as 0, JSON says
  // `null`, an empty form input is 0, and `ww_is_null()` is how you ask for it.
  bool null;
  // `.references = "media_assets(id)"` — a foreign key, emitted verbatim in the DDL
  // this framework generates. Combined with `.null` it is the schema this app's
  // `posts.cover_media_asset_id` needed: nullable *and* referring.
  const char *references;
} WQual;

#define WC_QNAN __builtin_nan("")

#ifdef __clang__
#define WC_QUAL(...)                                                           \
  ((WQual){ _Pragma("clang diagnostic ignored \"-Winitializer-overrides\"")     \
                .gte = WC_QNAN, .lte = WC_QNAN, __VA_ARGS__ })
#elif defined(__GNUC__)
#define WC_QUAL(...)                                                           \
  ((WQual){ _Pragma("GCC diagnostic ignored \"-Woverride-init\"")               \
                .gte = WC_QNAN, .lte = WC_QNAN, __VA_ARGS__ })
#else
#define WC_QUAL(...) ((WQual){ .gte = WC_QNAN, .lte = WC_QNAN, __VA_ARGS__ })
#endif

#define WC_HAS_GTE(q) (!__builtin_isnan((q).gte))
#define WC_HAS_LTE(q) (!__builtin_isnan((q).lte))

const char *wc_kind_name(WKind k); // "i64", "str": one vocabulary for errors and schema docs

typedef struct WField {
  const char *name;
  u8   kind;               // WKind
  u32  off;                // offsetof(model, field)
  u32  size;               // sizeof(field)
  const WModel   *sub;     // WK_MODEL
  const WVecDesc *vec;     // WK_VEC
  const struct WEnumDesc *en; // WK_ENUM: the closed set this field is one of
  WQual q;
} WField;

// A closed set of names: WEBC_ENUM(Lane, backlog, todo, doing, done) is an int in
// the struct, its name on the wire (JSON, forms, SQL text), and a 422 naming the
// field when a request sends a value that is not in the set.
typedef struct WEnumDesc {
  const char *name;            // "Lane"
  i32 n;                       // how many
  const char *const *names;    // {"backlog", "todo", ...}
} WEnumDesc;

i32 wc_enum_index(const WEnumDesc *e, str name);           // -1 when not in the set
const char *wc_enum_name(const WEnumDesc *e, i32 value);   // "", when out of range

struct WModel {
  const char *name;
  const char *table;      // default: snake_case plural of name (see model_table)
  const char *route;      // "/a/:slug": what link(Model, &row) is built from
  bool internal;          // a framework row type (RowId), not a table of the app's
  u32 size;               // sizeof(the model struct): lets a handler get one back
  WField *fields;
  i32 n, cap;
};

// A descriptor's first word. `json(200, &vec)` is a pointer one level too deep, and reading
// it as a descriptor walks into a struct's front and segfaults inside the writer — no
// handler name, no line, nothing to follow. Letting the reader *check* is 4 bytes per
// collection in a static table and turns the worst debugging session in the framework into
// a 500 with a sentence. (`json_list(status, v)` is the spelling that cannot be wrong; this
// catches the forms a person reaches for anyway.)
#define WC_VEC_MAGIC 0x57454356u // "WECV"

struct WVecDesc {
  u32 magic;              // WC_VEC_MAGIC: what makes a pointer a collection
  const char *name;
  u32 size;               // sizeof(the collection struct)
  u32 elem_size;
  u32 off_at, off_n, off_cap;  // offsets inside the vector struct
  u32 off_arena;               // ... and of the arena it allocates in
  u8  elem_kind;          // WKind of the element
  const WModel *sub;      // element model, when elem_kind == WK_MODEL
};

// One place per scalar type: the token in the field list maps to a WKind.
#define WC_KIND_i8   WK_I8
#define WC_KIND_i16  WK_I16
#define WC_KIND_i32  WK_I32
#define WC_KIND_i64  WK_I64
#define WC_KIND_u8   WK_U8
#define WC_KIND_u16  WK_U16
#define WC_KIND_u32  WK_U32
#define WC_KIND_u64  WK_U64
#define WC_KIND_f32  WK_F32
#define WC_KIND_f64  WK_F64
#define WC_KIND_bool WK_BOOL
#define WC_KIND_str  WK_STR
#define WC_KIND_Text WK_TEXT
#define WC_KIND_Bytes WK_BYTES

// An enum descriptor hangs off a field, exactly like a submodel does; scalars have
// none. (Defined here so WC_FIELD_I can mention WC_ENUMOF_##FT for every type.)

// Non-model, non-vector field types answer "no submodel / no vector descriptor".
#define WC_SUBMODEL_i8 nullptr
#define WC_ENUMOF_i8 nullptr
#define WC_SUBMODEL_i16 nullptr
#define WC_ENUMOF_i16 nullptr
#define WC_SUBMODEL_i32 nullptr
#define WC_ENUMOF_i32 nullptr
#define WC_SUBMODEL_i64 nullptr
#define WC_ENUMOF_i64 nullptr
#define WC_SUBMODEL_u8 nullptr
#define WC_ENUMOF_u8 nullptr
#define WC_SUBMODEL_u16 nullptr
#define WC_ENUMOF_u16 nullptr
#define WC_SUBMODEL_u32 nullptr
#define WC_ENUMOF_u32 nullptr
#define WC_SUBMODEL_u64 nullptr
#define WC_ENUMOF_u64 nullptr
#define WC_SUBMODEL_f32 nullptr
#define WC_ENUMOF_f32 nullptr
#define WC_SUBMODEL_f64 nullptr
#define WC_ENUMOF_f64 nullptr
#define WC_SUBMODEL_bool nullptr
#define WC_ENUMOF_bool nullptr
#define WC_SUBMODEL_str nullptr
#define WC_ENUMOF_str nullptr
#define WC_SUBMODEL_Text nullptr
#define WC_ENUMOF_Text nullptr
#define WC_SUBMODEL_Bytes nullptr
#define WC_ENUMOF_Bytes nullptr

#define WC_VECOF_i8 nullptr
#define WC_VECOF_i16 nullptr
#define WC_VECOF_i32 nullptr
#define WC_VECOF_i64 nullptr
#define WC_VECOF_u8 nullptr
#define WC_VECOF_u16 nullptr
#define WC_VECOF_u32 nullptr
#define WC_VECOF_u64 nullptr
#define WC_VECOF_f32 nullptr
#define WC_VECOF_f64 nullptr
#define WC_VECOF_bool nullptr
#define WC_VECOF_str nullptr
#define WC_VECOF_Text nullptr
#define WC_VECOF_Bytes nullptr

// The key of one element of a collection: the primary key when the element is a
// model that has one, the element's own address when it does not. Keyed-list
// reconciliation only needs an identity that is stable while the item lives, and
// this is the one function that decides it.
i64 wc_vec_key(const WVecDesc *d, const void *elem);

// Ceiling for the parts of the framework that keep a fixed table (a reactive form's
// fields, react.h). Models are *not* limited by it any more: WEBC_MODEL sizes its
// descriptor by the field list, so a model has as many fields as it declares.
#ifndef WC_FIELD_MAX
#define WC_FIELD_MAX 16
#endif

#endif // WEBC_KINDS_H