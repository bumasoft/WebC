// SPDX-License-Identifier: Apache-2.0
// WebC — rows in memory: filtering and sorting a collection by a model field.
//
// The database is the right place to sort and filter, and `db_all_where()` is how
// you say so. What is left is the collection you already have: the twelve stories a
// page loaded together, grouped by desk, or the four most read. Doing that by hand
// was a `for` loop with a `strcmp` — and, in one real project, a hand-written
// insertion sort, because the query could not be ordered (that part is fixed; this
// is the other half).
//
//   VecStory *mine  = vec_where_str(&site->stories, Story, section, slug);
//   VecStory *recent = vec_new(Story);   // one row, in the current arena
//   sort_desc(&mine, Story, views);
//
// A field that is not on the model is a compile error at the macro (offsetof), and a
// field whose *kind* cannot be compared is a panic naming it.
#include <webc/server.h>
#include <webc/web.h>

// ---------------------------------------------------------------- sorting

// Stable bottom-up merge sort over bytes: `stride` is the element size, `off` the
// field inside it. No comparator function pointer, so no per-field wrapper has to
// exist anywhere — the offset is the whole configuration.
static i64 cmp_field(const char *a, const char *b, size_t off, u8 kind, bool bytes) {
  const void *fa = a + off, *fb = b + off;
  switch (kind) {
  case WK_I64: {
    i64 x = *(const i64 *)fa, y = *(const i64 *)fb;
    return x < y ? -1 : x > y ? 1 : 0;
  }
  case WK_F64: {
    f64 x = *(const f64 *)fa, y = *(const f64 *)fb;
    return x < y ? -1 : x > y ? 1 : 0;
  }
  case WK_BOOL: {
    bool x = *(const bool *)fa, y = *(const bool *)fb;
    return x == y ? 0 : x ? 1 : -1;
  }
  // A `str` is {pointer, length}; a `Text` is a plain `const char *`. Reading one as
  // the other is exactly the kind of field-kind bug the model descriptor exists to
  // prevent, so the two are separate cases here.
  case WK_STR:
  case WK_BYTES: {
    str x = *(const str *)fa, y = *(const str *)fb;
    size_t n = x.n < y.n ? x.n : y.n;
    int c = n ? memcmp(x.p, y.p, n) : 0;
    if (c) return c < 0 ? -1 : 1;
    return x.n < y.n ? -1 : x.n > y.n ? 1 : 0;
  }
  case WK_TEXT: {
    const char *x = *(const char *const *)fa, *y = *(const char *const *)fb;
    int c = strcmp(x ? x : "", y ? y : "");
    return c < 0 ? -1 : c > 0 ? 1 : 0;
  }
  default:
    wc_panic(__FILE__, __LINE__, "sort: a field of kind %s cannot be ordered", wc_kind_name(kind));
    return 0;
  }
}

// Merge two adjacent runs: [0, mid) and [mid, n), both already sorted.
static void merge_run(char *base, char *tmp, size_t n, size_t mid, size_t stride, size_t off,
                      u8 kind, bool bytes, bool desc) {
  size_t i = 0, j = mid, k = 0;
  while (i < mid && j < n) {
    i64 c = cmp_field(base + i * stride, base + j * stride, off, kind, bytes);
    bool take_left = desc ? c >= 0 : c <= 0; // stable in both directions
    if (take_left) {
      memcpy(tmp + k * stride, base + i * stride, stride);
      i++;
    } else {
      memcpy(tmp + k * stride, base + j * stride, stride);
      j++;
    }
    k++;
  }
  while (i < mid) memcpy(tmp + (k++) * stride, base + (i++) * stride, stride);
  while (j < n) memcpy(tmp + (k++) * stride, base + (j++) * stride, stride);
  memcpy(base, tmp, n * stride);
}

static void wc_sort_rows_(void *base, i32 *n, size_t stride, size_t off, u8 kind, bool bytes, bool desc) {
  if (!base || !n || *n < 2) return;
  size_t count = (size_t)*n;
  Arena *a = wc_arena_get();
  char *tmp = arena_alloc(a, count * stride);
  char *buf = base;
  for (size_t w = 1; w < count; w *= 2)
    for (size_t i = 0; i < count; i += 2 * w) {
      size_t run = count - i < 2 * w ? count - i : 2 * w;
      if (run < 2) continue;
      // the left run is a full w wide unless the tail is shorter; splitting at n/2
      // instead would merge an unsorted right half
      size_t mid = run > w ? w : run / 2;
      merge_run(buf + i * stride, tmp, run, mid, stride, off, kind, bytes, desc);
    }
}

// The macro half: the field is looked up in the model, so its kind is known, and a
// name that is not a field panics with the model's name rather than sorting garbage.
void wc_sort_vec_(const WModel *m, void *vec, const char *field, size_t off, bool desc) {
  const WField *f = model_field(m, str_lit(field));
  if (!f)
    wc_panic(__FILE__, __LINE__, "sort: %s has no field '%s'", m->name, field);
  const WVecDesc *vd = *(const WVecDesc **)vec;
  bool bytes = f->kind == WK_STR && vd && f->size == sizeof(char *);
  // At the array and the count through the collection's own offsets, so this works
  // for every Vec<T> without a generated function per type.
  void **atp = (void **)((char *)vec + vd->off_at);
  i32 *np = (i32 *)((char *)vec + vd->off_n);
  wc_sort_rows_(*atp, np, vd->elem_size, off, f->kind, bytes, desc);
}

// ---------------------------------------------------------------- filtering

static void *filter_new(const WVecDesc *vd) {
  Arena *a = wc_arena_get();
  void *out = arena_zalloc(a, vd->size);
  *(const WVecDesc **)out = vd;
  return out;
}

static WValKind val_kind(const WModel *m, const char *field, size_t off, const WField **out) {
  const WField *f = model_field(m, str_lit(field));
  if (!f) wc_panic(__FILE__, __LINE__, "filter: %s has no field '%s'", m->name, field);
  (void)off;
  *out = f;
  switch (f->kind) {
  case WK_STR:
  case WK_TEXT: return WV_TEXT;
  case WK_F64: return WV_F64;
  case WK_BOOL: return WV_BOOL;
  default: return WV_I64;
  }
}

// One pass, one allocation, the same collection type back.
static void *filter_impl(const WModel *m, const WVecDesc *vd, const void *vec, const char *field,
                         size_t off, WVal want, bool bytes) {
  if (!vd || !vec) return nullptr;
  const WField *f = nullptr;
  val_kind(m, field, off, &f);
  const void **atp = (const void **)((const char *)vec + vd->off_at);
  const i32 *np = (const i32 *)((const char *)vec + vd->off_n);
  void *out = filter_new(vd);
  void **oat = (void **)((char *)out + vd->off_at);
  i32 *on = (i32 *)((char *)out + vd->off_n);
  i32 *ocap = (i32 *)((char *)out + vd->off_cap);
  Arena **oarena = (Arena **)((char *)out + vd->off_arena);
  *oarena = wc_arena_get();
  for (i32 i = 0; i < *np; i++) {
    const char *row = (const char *)*atp + (size_t)i * vd->elem_size;
    bool hit = false;
    switch (want.kind) {
    case WV_TEXT: {
      str have = f->kind == WK_STR ? *(const str *)(row + off)
                                   : str_lit(*(const char *const *)(row + off));
      hit = str_eq(have, want.s);
      break;
    }
    case WV_I64:
      hit = *(const i64 *)(row + off) == want.i;
      break;
    case WV_F64:
      hit = *(const f64 *)(row + off) == want.f;
      break;
    case WV_BOOL:
      hit = *(const bool *)(row + off) == want.b;
      break;
    default: break;
    }
    (void)bytes;
    if (hit) wvec_push_impl(oat, on, ocap, vd->elem_size, wc_arena_get(), row);
  }
  return out;
}
void *wc_filter_str_(const WModel *m, const WVecDesc *vd, const void *vec, const char *field,
                     size_t off, str v) {
  return filter_impl(m, vd, vec, field, off, (WVal){.kind = WV_TEXT, .s = v}, false);
}

void *wc_filter_i64_(const WModel *m, const WVecDesc *vd, const void *vec, const char *field,
                     size_t off, i64 v) {
  return filter_impl(m, vd, vec, field, off, (WVal){.kind = WV_I64, .i = v}, false);
}

void *wc_filter_bool_(const WModel *m, const WVecDesc *vd, const void *vec, const char *field,
                      size_t off, bool v) {
  return filter_impl(m, vd, vec, field, off, (WVal){.kind = WV_BOOL, .b = v}, false);
}
