// SPDX-License-Identifier: Apache-2.0
// WebC — explicit vector instantiation. The macro call IS the declaration:
// greppable, breakpointable, no type-scan and no generated code.
//
//   WEBC_VEC_DEFINE(VecTodo, vec_todo, Todo)
//     typedef VecTodo;
//     vec_todo_new(arena) / vec_todo_with(arena) / vec_todo_push / vec_todo_free
//
// The third argument is the element type; `T` is never used as a macro parameter
// name where it could collide with a field named `T` (house rule §3.4).
#ifndef WEBC_VEC_H
#define WEBC_VEC_H

#include <webc/core.h>
#include <webc/kinds.h>

// ---------------------------------------------------------------- the shape
//
//   Vec(Todo)                              // → VecTodo, carrying its descriptor
//   static VecTodo todos;                  // in webc_boot(): todos = VecTodo_with(app_arena());
//   push(todos, t);                        // one set of verbs for every collection
//   for_each(t, todos) { ... }             // keyed by .pk, no callback, no key argument
//
// The descriptor lives *inside* the struct, so a collection names its own type at
// run time: `json(200, &todos)` needs no `Todo`, and `for_each` needs no key
// expression. `push`/`len`/`at`/`remove_at` do not even need the descriptor —
// they read the element size off the type — so every one of them is a macro over
// the array, not a function generated per collection.
//
// Every verb takes the vector itself, readers and writers alike — `push(todos, x)`
// mutates `todos`, because the macro takes the address of its field, not of the
// variable.
//
//   for_each(t, todos) if (at(todos, 0) == t) push(todos, x);

#define WC_VEC_STRUCT(Name, Elem)                                              \
  typedef struct Name {                                                        \
    const WVecDesc *desc; /* first field: a collection identifies itself */     \
    Elem *at;                                                                  \
    i32 n;                                                                     \
    i32 cap;                                                                   \
    Arena *arena;                                                              \
  } Name;

#define WC_VEC_DESC(Name, Elem)                                                \
  enum { WC_KIND_##Name = WK_VEC };                                            \
  [[maybe_unused]] static const WModel *const WC_SUBMODEL_##Name = nullptr;     \
  [[maybe_unused]] static const void *const WC_ENUMOF_##Name = nullptr;          \
  [[maybe_unused]] static const WVecDesc WC_VECDESC_##Name = {                  \
      .magic = WC_VEC_MAGIC,                                                   \
      .name = #Name,                                                           \
      .size = (u32)sizeof(Name),                                               \
      .elem_size = sizeof(Elem),                                               \
      .off_at = (u32)offsetof(Name, at),                                       \
      .off_n = (u32)offsetof(Name, n),                                         \
      .off_cap = (u32)offsetof(Name, cap),                                     \
      .off_arena = (u32)offsetof(Name, arena),                                 \
      .elem_kind = WC_KIND_##Elem,                                             \
      .sub = WC_SUBMODEL_##Elem };                                             \
  [[maybe_unused]] static const WVecDesc *const WC_VECOF_##Name = &WC_VECDESC_##Name;

// A vector of `Elem`, named Vec<Elem> by construction. The constructor is derived
// from the type name, so there is no prefix to spell twice.
// The name is built first (WC_CAT) and handed to a second macro, because a
// parameter next to `##` is pasted unexpanded and `WC_CAT(Vec, Todo)` would land
// in the descriptor table as those seven characters.
#define Vec(Elem) Vec_I(WC_CAT(Vec, Elem), Elem)
#define Vec_I(Name, Elem)                                                      \
  WC_VEC_STRUCT(Name, Elem)                                                    \
  WC_VEC_DESC(Name, Elem)                                                      \
  [[maybe_unused]] static inline Name WC_CAT(Name, _with)(Arena *a_) {          \
    return (Name){ .desc = &WC_CAT(WC_VECDESC_, Name),                          \
                   .at = nullptr, .n = 0, .cap = 0, .arena = a_ };              \
  }                                                                            \
  [[maybe_unused]] static inline Name WC_CAT(Name, _new)(void) {                \
    return WC_CAT(Name, _with)(nullptr);                                        \
  }

// The prefixed form: the vector's name and its accessors' prefix spelled apart, for a
// vector whose accessors read better that way.
#define WEBC_VEC_DEFINE(Name, prefix, Elem)                                    \
  WC_VEC_STRUCT(Name, Elem)                                                    \
  WC_VEC_DESC(Name, Elem)                                                      \
  [[maybe_unused]] static inline Name prefix##_with(Arena *a_) {                \
    return (Name){ .desc = &WC_VECDESC_##Name, .at = nullptr, .n = 0, .cap = 0, \
                   .arena = a_ };                                              \
  }                                                                            \
  [[maybe_unused]] static inline Name prefix##_new(void) { return prefix##_with(nullptr); } \
  [[maybe_unused]] static inline void prefix##_push(Name *v_, Elem x_) {        \
    wvec_push_impl(&v_->at, &v_->n, &v_->cap, sizeof(Elem), v_->arena, &x_);   \
  }                                                                            \
  [[maybe_unused]] static inline Elem prefix##_at(Name *v_, i32 i_) {           \
    if (i_ < 0 || i_ >= v_->n)                                                 \
      wc_panic(__FILE__, __LINE__, #Name " index %d out of range [0,%d)", i_, v_->n); \
    return v_->at[i_];                                                         \
  }                                                                            \
  [[maybe_unused]] static inline Elem *prefix##_ref(Name *v_, i32 i_) {         \
    if (i_ < 0 || i_ >= v_->n)                                                 \
      wc_panic(__FILE__, __LINE__, #Name " index %d out of range [0,%d)", i_, v_->n); \
    return &v_->at[i_];                                                        \
  }                                                                            \
  [[maybe_unused]] static inline i32 prefix##_len(Name *v_) { return v_->n; }   \
  [[maybe_unused]] static inline void prefix##_clear(Name *v_) { v_->n = 0; }   \
  [[maybe_unused]] static inline void prefix##_pop(Name *v_) {                  \
    v_->n = v_->n > 0 ? v_->n - 1 : 0;                                          \
  }                                                                            \
  [[maybe_unused]] static inline void prefix##_free(Name *v_) {                 \
    if (!v_->arena && v_->at) wm_free(v_->at);                                  \
    v_->at = nullptr;                                                           \
    v_->n = v_->cap = 0;                                                        \
  }                                                                            \
  [[maybe_unused]] static inline Name prefix##_clone(Arena *a_, const Name *src_) { \
    Name out_ = prefix##_with(a_);                                              \
    for (i32 i_ = 0; i_ < src_->n; i_++)                                        \
      wvec_push_impl(&out_.at, &out_.n, &out_.cap, sizeof(Elem), a_, &src_->at[i_]); \
    return out_;                                                                \
  }

// ---------------------------------------------------------------- generic verbs
//
// One name for every collection. `at` is a pointer, so `at(&v, i)->field` reads like
// the array it is; `push` and `remove_at` take the element size from the type, so
// they need no descriptor and no per-collection function.

#define len(v_) ((v_).n)

// `VecStory *v = vec_new(Story);` — a collection in the current arena. The
// three-line version (`wc_alloc_zero`, `VecStory_with`, deref) appeared six times in
// one real app, which is five times too many for "an empty list".
#define vec_new(T_)                                                            \
  __extension__({                                                              \
    Vec##T_ *wc_v_ = wc_alloc_zero(sizeof *wc_v_);                             \
    *wc_v_ = WC_CAT(WC_CAT(Vec, T_), _with)(wc_arena_get());                    \
    wc_v_;                                                                     \
  })

// ---------------------------------------------------------------- rows in memory
//
// Querying is the first answer; these are for the collection you already have.
// The field name is checked by the compiler (offsetof) and by the model at run
// time (its kind decides how two rows compare):
//
//   VecStory *mine = vec_where_str(&site->stories, Story, section, slug);
//   sort_desc(mine, Story, views);
void wc_sort_vec_(const WModel *m, void *vec, const char *field, size_t off, bool desc);
void *wc_filter_str_(const WModel *m, const WVecDesc *vd, const void *vec, const char *field,
                     size_t off, str v);
void *wc_filter_i64_(const WModel *m, const WVecDesc *vd, const void *vec, const char *field,
                     size_t off, i64 v);
void *wc_filter_bool_(const WModel *m, const WVecDesc *vd, const void *vec, const char *field,
                      size_t off, bool v);

#define vec_where_str(vp_, T_, f_, v_)                                         \
  ((Vec##T_ *)wc_filter_str_(T_##_modelof(), WC_CAT(WC_VECOF_Vec, T_), (vp_), #f_,              \
                             offsetof(T_, f_), WC_AS_STR(v_)))
#define vec_where_i64(vp_, T_, f_, v_)                                         \
  ((Vec##T_ *)wc_filter_i64_(T_##_modelof(), WC_CAT(WC_VECOF_Vec, T_), (vp_), #f_,              \
                             offsetof(T_, f_), (i64)(v_)))
#define vec_where_bool(vp_, T_, f_, v_)                                        \
  ((Vec##T_ *)wc_filter_bool_(T_##_modelof(), WC_CAT(WC_VECOF_Vec, T_), (vp_), #f_,             \
                              offsetof(T_, f_), (bool)(v_)))
#define sort_by(vp_, T_, f_) wc_sort_vec_(T_##_modelof(), (void *)(vp_), #f_, offsetof(T_, f_), false)
#define sort_desc(vp_, T_, f_) wc_sort_vec_(T_##_modelof(), (void *)(vp_), #f_, offsetof(T_, f_), true)

#define at(v_, i_)                                                             \
  ((v_).at + (((i_) < 0 || (i_) >= (v_).n)                                      \
                  ? (wc_panic(__FILE__, __LINE__, "index %d out of range [0,%d)", \
                              (i_), (v_).n),                                    \
                     0)                                                        \
                  : (i_)))
#define first(v_) at(v_, 0)
#define last(v_) at(v_, (v_).n - 1)

// The pointer spellings. A Vec travels by pointer (the board owns it, the view borrows
// it), and the value forms turn every use into a dereference dance — `at((*rows), i)`,
// `len(*rows)` — which is most of the noise in a real view. These take the pointer the
// caller actually holds.
#define vlen(vp_)     ((vp_)->n)
#define vat(vp_, i_)  ((vp_)->at[i_])

// The loop every list view writes: a typed pointer per element, no index, no (*vp)
// dance. `each(f, rows) { … f->number … }` walks VecFlight *rows in order.
#define each(f_, vp_)                                                            \
  for (i64 each_i_ = 0;                                                          \
       each_i_ < (vp_)->n && ((f_) = &(vp_)->at[each_i_], true); each_i_++)

#define push(v_, ...)                                                          \
  do {                                                                         \
    typeof((v_).at[0]) _wc_x_ = (__VA_ARGS__);                                  \
    wvec_push_impl(&(v_).at, &(v_).n, &(v_).cap, sizeof _wc_x_, (v_).arena, &_wc_x_); \
  } while (0)

// Prepend: the shape a streaming app renders in — the newest row is the one that
// just arrived, so it goes at the front without rebuilding the collection.
#define push_front(v_, ...)                                                    \
  do {                                                                         \
    typeof((v_)) *_wc_v_ = &(v_);                                              \
    typeof((v_).at[0]) _wc_x_ = (__VA_ARGS__);                                  \
    wvec_push_impl(&_wc_v_->at, &_wc_v_->n, &_wc_v_->cap, sizeof _wc_x_,        \
                   _wc_v_->arena, &_wc_x_);                                     \
    for (i32 _wc_i_ = _wc_v_->n - 1; _wc_i_ > 0; _wc_i_--)                      \
      _wc_v_->at[_wc_i_] = _wc_v_->at[_wc_i_ - 1];                              \
    _wc_v_->at[0] = _wc_x_;                                                     \
  } while (0)

// Order-preserving removal: what a list rendered in order needs.
#define remove_at(v_, i_)                                                      \
  do {                                                                         \
    typeof((v_)) *_wc_v_ = &(v_);                                              \
    i32 _wc_i_ = (i_);                                                          \
    if (_wc_i_ >= 0 && _wc_i_ < _wc_v_->n) {                                     \
      if (_wc_v_->n - 1 > _wc_i_)                                                \
        memmove(_wc_v_->at + _wc_i_, _wc_v_->at + _wc_i_ + 1,                    \
                (size_t)(_wc_v_->n - 1 - _wc_i_) * sizeof *_wc_v_->at);          \
      _wc_v_->n--;                                                              \
    }                                                                           \
  } while (0)

// Prelude instantiations: everyday code never writes one of these.
WEBC_VEC_DEFINE(VecStr, vec_str, str)
WEBC_VEC_DEFINE(VecI64, vec_i64, i64)
WEBC_VEC_DEFINE(VecI32, vec_i32, i32)
WEBC_VEC_DEFINE(VecF64, vec_f64, f64)
WEBC_VEC_DEFINE(VecBytes, vec_bytes, Bytes)

// One type per element: `vec_new(T)` derives its type name by pasting `Vec` and `T`,
// which spells a `str` collection `Vecstr` — a type nobody declared, so `vec_new(str)`
// did not compile and an app that wanted an empty list of strings wrote the three lines
// by hand. These lines are that paste's result, pointing at the prelude's own type:
//
//   VecStr *names = vec_new(str);     // the same type as `VecStr`
//   VecI64 *ids   = vec_new(i64);
//
// The other direction — writing `Vec(str)` — is a redeclaration of the prelude type, so
// the prelude declares the five primitive collections and an app only declares `Vec(T)`
// for a model or a `WEBC_ROW`. `webc lint` names the mistake.
#define Vecstr VecStr
#define Veci64 VecI64
#define Veci32 VecI32
#define Vecf64 VecF64
#define VecStr_with vec_str_with
#define VecStr_new vec_str_new
#define VecI64_with vec_i64_with
#define VecI64_new vec_i64_new
#define VecI32_with vec_i32_with
#define VecI32_new vec_i32_new
#define VecF64_with vec_f64_with
#define VecF64_new vec_f64_new

// Iteration helper: real C control flow, no template syntax (§5.3). This is the
// *index* loop; the markup keyed list is `for_each(item, vec, key)` in fe.h.
#define for_i(i_, v_)                                                          \
  for (i32 i_ = 0, _wc_n_ = (v_).n; i_ < _wc_n_; i_++)

#define for_i_rev(i_, v_)                                                      \
  for (i32 i_ = (v_).n - 1, _wc_n_ = 0; i_ >= _wc_n_; i_--)

#endif // WEBC_VEC_H