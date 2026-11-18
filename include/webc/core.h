// SPDX-License-Identifier: Apache-2.0
// WebC — core runtime. Types, one allocation seam, arenas, strings, errors.
//
// Doctrine (SPEC §4.1): every allocation funnels through wm_alloc so that it can
// be instrumented, capped and reported. Requests and renders own an Arena and
// reset it in one instruction. There is no collector and there never will be.
#ifndef WEBC_CORE_H
#define WEBC_CORE_H

#include <stdalign.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- version
//
// The framework version comes from the `VERSION` file at the repository root, which the
// Makefile turns into -DWEBC_VERSION="…". A hand-written compile — the house rule's
// `cc -std=c23 -I include app.c build/libwebc.a` — defines nothing, and must not fail:
// it gets this fallback instead. One number, one file; tools/check-version.py is what
// keeps the CLI, the site and the editor extension from drifting apart.
#ifndef WEBC_VERSION
#define WEBC_VERSION "0.0.0-dev"
#endif

// ---------------------------------------------------------------- scalar types

typedef int8_t     i8;
typedef int16_t    i16;
typedef int32_t    i32;
typedef int64_t    i64;
typedef uint8_t    u8;
typedef uint16_t   u16;
typedef uint32_t   u32;
typedef uint64_t   u64;
typedef float      f32;
typedef double     f64;

// A fat string: immutable by convention, no NUL reliance. `str_lit("x")` is the
// literal form; anything else uses an explicit length.
typedef struct str { const char *p; size_t n; } str;

// The string a *model field* uses: a NUL-terminated string owned by the arena of
// whatever scope produced it (the request, the render, the app). It prints with
// `%s`, it goes into an attribute with `attr_c`/`attr`, and no length or cast ever
// appears in application code. Strings that may contain a NUL — a blob, a
// file, a hash — stay `str`/`Bytes`.
typedef const char *Text;

// What wasm linear memory hands you, and what the HTTP body reader fills in.
typedef struct Bytes { char *p; size_t n; } Bytes;

#define WC_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
#define WC_LIT(s)     ((str){ (s), sizeof(s) - 1 })

// ---------------------------------------------------------------- allocation seam

void *wm_alloc(size_t n);              // zeroed, 16-byte aligned, aborts on OOM
void *wm_alloc_zero(size_t n);
void *wm_realloc(void *p, size_t n);
void  wm_free(void *p);                // no-op for arena pointers
bool  wc_heap_alive(const void *p);    // DIAGNOSTIC: still a live wm_alloc block?
void *wc_heap_freed_by(const void *p);  // DIAGNOSTIC: return address of its wm_free, or null
void  wc_heap_describe(const void *p, char *out, size_t n); // DIAGNOSTIC
char *wm_strdup(const char *s);
char *wm_strndup(const char *s, size_t n);

typedef struct WmStats {
  size_t heap_used;      // live bytes handed out by wm_alloc
  size_t heap_peak;
  size_t heap_allocs;
  size_t heap_frees;
  size_t arena_live;     // bytes currently reserved by all arenas
  size_t arena_peak;
  size_t alloc_fail;     // cap breaches
} WmStats;

WmStats wm_stats(void);
void    wm_stats_reset_peak(void);
size_t  wm_heap_limit(void);           // 0 = unlimited
void    wm_set_heap_limit(size_t bytes);

// ---------------------------------------------------------------- arena

typedef struct ArenaChunk ArenaChunk;

typedef struct Arena Arena;

struct Arena {
  Arena *next_free;        // freelist of recycled arenas (server hot path)
  ArenaChunk **chunks;     // chunk table; reset is O(1) and keeps the memory
  size_t nchunks, chunks_cap;
  size_t cur;              // index of the chunk being filled
  size_t used;             // bytes used in chunks[cur]
  size_t first_chunk;      // default chunk size
  size_t total;            // bytes reserved right now
  size_t peak;
  size_t allocs;
  void  *row_pool;         // per-request row cache owned by this arena, released with it (db.c)
  const char *label;       // "request", "render", "wasm", … (debug view)
};

Arena *arena_new(size_t chunk_size);
Arena *arena_new_labeled(size_t chunk_size, const char *label);
void   arena_destroy(Arena *a);
void   arena_reset(Arena *a);          // O(1): rewind to the first chunk
void   arena_release(Arena *a);        // drop all chunks but keep the handle
void  *arena_alloc(Arena *a, size_t n);
void  *arena_zalloc(Arena *a, size_t n);
void  *arena_memdup(Arena *a, const void *p, size_t n);
char  *arena_strdup(Arena *a, const char *s);
size_t arena_bytes(const Arena *a);
size_t arena_high_water(const Arena *a);

// An arena-pool keeps hot request arenas alive between requests. `max_bytes` bounds
// what it keeps, so one large upload cannot pin its chunks for the life of the process.
typedef struct ArenaPool {
  Arena *free_list;
  size_t chunk;
  _Atomic size_t live;
  _Atomic size_t free_bytes; // reserved bytes sitting on the free list
  size_t max_bytes;
  // The free list itself is the shared state, and two workers claiming connections at
  // the same instant is the normal case, not the corner: without this lock the same
  // arena was handed to two requests and one connection's headers were overwritten by
  // another's in flight (measured: a response carrying a fragment of an unrelated
  // request line). A spinlock rather than a pthread_mutex_t: the header is shared with
  // the wasm build, which has no threads, and the critical section is two pointer
  // writes inside a running task, so there is nothing to block on.
  _Atomic int lock;
} ArenaPool;
ArenaPool *arena_pool_new(size_t chunk_size);
Arena     *arena_pool_get(ArenaPool *p);
void       arena_pool_put(ArenaPool *p, Arena *a);

// Convenience: allocation through the current arena when one is bound (request
// or render), the heap otherwise. This is the seam the whole framework uses.
extern _Thread_local Arena *wc_arena_cur;
// Install `a` as the current arena, returning the previous one. The scheduler
// uses it to carry a task's arena across a switch to another worker thread.
Arena *wc_arena_swap(Arena *a);
// The current arena, read on the running thread (never keep this pointer in a
// local across a blocking call: use wc_arena_get() again after it).
Arena *wc_arena_get(void);
void *wc_alloc(size_t n);              // arena if bound, heap otherwise
void *wc_alloc_zero(size_t n);
char *wc_strndup(const char *s, size_t n);
void  wc_free(void *p);

// ---------------------------------------------------------------- app arena
//
// A request's arena dies with the request; an app that keeps state between
// requests (a cache, an in-memory list, a session key) needs one place that
// outlives them. `persist()` is the only door into it, so "how long does this
// string live" has exactly one answer per scope.
Arena *wc_app_arena(void);
void   wc_app_arena_set(Arena *a);

// ---------------------------------------------------------------- text
//
// The three functions an application ever needs for strings:
//
//   Text t = fmt("%lld left", (long long)n);   // in the current scope
//   Text c = text_copy(name);                  // in the current scope
//   g_secret = persist(secret);                // for the app's whole life
//
// No arena argument, because the framework already installed one.
char *str_cdup(Arena *a, str s);   // NUL-terminated copy of a str (a or heap)

// The one growth routine every Vec(T) uses: element size is a parameter, so no
// collection needs a generated push function.
void wvec_push_impl(void *at_p, i32 *n, i32 *cap, size_t elem_size, Arena *a, const void *elem);
Text  fmt(const char *spec, ...) __attribute__((format(printf, 1, 2)));
str   fmt_s(const char *spec, ...) __attribute__((format(printf, 1, 2))); // the same, as a str

// ---------------------------------------------------------------- numbers as text
//
// The four numbers every dashboard writes by hand, because printf has no portable
// thousands separator and no idea what money is:
//
//   text(fmt_num(128537));          // 128,537
//   text(fmt_money(v));             // $1,234,567  — and $0 under half a unit
//   text(fmt_pct(0.042, 1));        // 4.2%        — a *fraction* in, a percent out
//   text(fmt_compact(1.04e3));      // 1.04k       — 10k, 2.5M, 1.2B for axis labels
//
// All four return Text in the current scope, exactly like fmt(), so they nest in
// text()/attr()/fmt() with no cast and no buffer to size.
Text fmt_num(i64 v);
Text fmt_num_u(u64 v);
Text fmt_money(double v);                       // "$…", rounded to the unit
Text fmt_money_c(double v, const char *sym);     // the same with another symbol ("€", "")
Text fmt_pct(double frac, int decimals);         // the fraction 0.0421 as "4.2%"
Text fmt_compact(double v);                      // 1040 -> "1.04k", 2.5e6 -> "2.5M"

Text  text_copy(const char *s);
Text  persist(const char *s);
Text  now(void);                   // RFC3339, in the current scope
Text  text_of(str s);              // a NUL-terminated copy of a fat string

// ---------------------------------------------------------------- strings

static inline str str_lit(const char *s) { return (str){ s, s ? strlen(s) : 0 }; }
static inline str str_from(const char *s) { return str_lit(s); }
static inline str str_mem(const char *p, size_t n) { return (str){ p, n }; }
static inline bool str_ok(str s) { return s.p != nullptr; }
static inline bool str_is_empty(str s) { return s.n == 0; }
static inline char str_at(str s, size_t i) { return i < s.n ? s.p[i] : '\0'; }

bool str_eq(str a, str b);bool str_eq_c(str a, const char *b);
int  str_cmp(str a, str b);
bool str_has_prefix(str s, str pre);
bool str_has_suffix(str s, str suf);
bool str_has_prefix_c(str s, const char *pre);
i64  str_index(str s, char c);          // -1 if absent
i64  str_rindex(str s, char c);         // -1 if absent
str  str_sub(str s, size_t from, size_t to);
str  str_trim(str s);
str  str_trim_left(str s);
str  str_trim_right(str s);
str  str_dup(Arena *a, str s);
char *str_cstr(Arena *a, str s);        // NUL-terminated copy (arena or heap)
str  str_fmt(Arena *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
str  str_upper(Arena *a, str s);
str  str_lower(Arena *a, str s);
bool str_to_i64(str s, i64 *out);
bool str_to_u64(str s, u64 *out);
bool str_to_f64(str s, f64 *out);
str  str_from_i64(Arena *a, i64 v);

// A string literal is `char *`, a WebC string is `str`; accept both everywhere a
// value is expected, chosen at compile time and never at run time. The branches
// are function designators: a compound literal there would be type-checked even
// when not selected. Lives here rather than in dom.h because the request API
// (html_attr, head_meta) takes the same values.
[[maybe_unused]] static inline str wc_as_str_ptr(const char *p) { return str_lit(p); }
[[maybe_unused]] static inline str wc_as_str_str(str s) { return s; }
// A number is an attribute value too — `attr("width", 640)` needs no second macro,
// and the buffer it is formatted into belongs to the current arena (the build's).
[[maybe_unused]] static inline str wc_as_str_num(i64 v) { return str_from_i64(wc_arena_get(), v); }
#define WC_AS_STR(v_)                                                                              \
  _Generic((v_), char *: wc_as_str_ptr, const char *: wc_as_str_ptr, int: wc_as_str_num,            \
           long: wc_as_str_num, long long: wc_as_str_num, default: wc_as_str_str)((v_))

str  str_from_u64(Arena *a, u64 v);
str  str_from_f64(Arena *a, f64 v);
bool str_contains(str hay, str needle);
i64  str_find(str hay, str needle, size_t from); // byte offset of needle, -1 if absent
u64  str_hash(str s);

// percent / url handling
str  url_decode(Arena *a, str s, bool plus_is_space);
str  url_encode(Arena *a, str s);
str  html_escape(Arena *a, str s);
str  json_escape(Arena *a, str s);
bool base64_encode(Arena *a, str in, str *out);
bool base64_decode(Arena *a, str in, str *out);
void sha256(const void *data, size_t n, u8 out[32]);
void hmac_sha256(const void *key, size_t klen, const void *msg, size_t mlen, u8 out[32]);
void pbkdf2_sha256(const void *pw, size_t pwlen, const void *salt, size_t saltlen,
                   u32 iters, u8 *out, size_t outlen);
void constant_time_eq(const void *a, const void *b, size_t n, bool *eq);

// ---------------------------------------------------------------- fixed limits

// A fixed limit that silently degrades behaviour is the hardest kind of bug to see
// from the outside ("why is this button dead?"). Every ceiling in the framework
// reports its first crossing through this, once per `id` (pass a static address).
void wc_limit_once(const void *id, const char *msg, const char *limit, const char *what);

// ---------------------------------------------------------------- growable buffer

typedef struct Buf {
  char *p;
  size_t n, cap;
  Arena *a;              // NULL = heap
  bool oom;
} Buf;

void  buf_init(Buf *b, Arena *a, size_t cap);
void  buf_free(Buf *b);
void  buf_reserve(Buf *b, size_t extra);
void  buf_write(Buf *b, const void *p, size_t n);
void  buf_puts(Buf *b, const char *s);
void  buf_putstr(Buf *b, str s);
void  buf_putc(Buf *b, char c);
void  buf_puti64(Buf *b, i64 v);   // decimal, no printf
void  buf_putu64(Buf *b, u64 v);
void  buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void  buf_vprintf(Buf *b, const char *fmt, va_list ap);
str   buf_str(Buf *b);
char *buf_cstr(Buf *b);
void  buf_reset(Buf *b);
void  json_escape_into(Buf *b, str s); // quotes included, straight into a buffer
size_t json_str_scan(const char *p, size_t n); // bytes copyable verbatim in a JSON string

// ---------------------------------------------------------------- errors

// Result(T, E) — errors are values, never exceptions (SPEC §4.2).
#define Result(T, E)                                                           \
  struct {                                                                     \
    bool ok;                                                                   \
    union {                                                                    \
      T v;                                                                     \
      E e;                                                                     \
    };                                                                         \
  }

#define Option(T)                                                              \
  struct {                                                                     \
    bool some;                                                                 \
    T v;                                                                       \
  }

#define Ok(v)  { .ok = true, .v = (v) }
#define Err(e) { .ok = false, .e = (e) }
#define Some(v) { .some = true, .v = (v) }
#define None(T) { .some = false, .v = (T){0} }

// The canonical error value. Drivers and helpers carry a code plus a message.
typedef enum WErrKind : i32 {
  WEBC_OK = 0,
  WEBC_EINVAL,
  WEBC_ENOTFOUND,
  WEBC_EEXIST,
  WEBC_EPERM,
  WEBC_EIO,
  WEBC_ETIMEOUT,
  WEBC_ECANCELED,
  WEBC_ECONFLICT,
  WEBC_EUNSUPPORTED,
  WEBC_ENOMEM,
  WEBC_EPARSE,
  WEBC_EEXTERNAL,
  WEBC_EDB,        // database driver error
  WEBC_EVALIDATION,
  WEBC_ETOOBIG, // a limit was hit: body, parts, depth (§11)
} WErrKind;

// The message is *stored in the error*, not borrowed from whoever raised it: an error
// that outlives its arena (a `db_*` failure logged after the request that caused it, a
// result kept for retry) would otherwise carry a `str` into memory that is already reset.
// The bytes live in `buf`, so copying a `WErr` by value copies the message with it.
// `detail` stays borrowed (it is request context by construction) and is empty unless a
// caller filled it in.
#define WC_ERR_MSG_CAP 128

typedef struct WErr {
  WErrKind kind;
  i32 len;      // bytes of `msg` stored in `buf`
  str detail;   // borrowed: request context, not owned
  bool truncated;
  char buf[WC_ERR_MSG_CAP];
} WErr;

static inline WErr werr(WErrKind k, str msg) {
  WErr e = {.kind = k};
  size_t n = msg.p ? msg.n : 0;
  if (n >= WC_ERR_MSG_CAP) {
    n = WC_ERR_MSG_CAP - 1;
    e.truncated = true;
  }
  if (n && msg.p) memcpy(e.buf, msg.p, n);
  e.buf[n] = 0;
  e.len = (i32)n;
  return e;
}
static inline WErr werr_c(WErrKind k, const char *msg) {
  return werr(k, msg ? (str){msg, strlen(msg)} : (str){"", 0});
}
// The message as a `str` pointing into the error's own buffer — so the error has to
// outlive the `str`, hence the pointer: a by-value argument would point at the copy and
// dangle the moment the call returned (measured: AddressSanitizer caught exactly that).
static inline str werr_msg(const WErr *e) { return (str){(char *)e->buf, (size_t)e->len}; }
#define WERR_FMT(e_) (int)((e_).len), ((e_).buf)
const char *werr_kind_name(WErrKind k);

// try(expr) returns the failing Result from the enclosing function.
#define try(x)                                                                 \
  __extension__({                                                              \
    __auto_type _wc_r_ = (x);                                                  \
    if (!_wc_r_.ok) {                                                          \
      wc_err_pending++;                                                        \
      return _wc_r_;                                                           \
    }                                                                          \
    _wc_r_.v;                                                                  \
  })

#define try_or(x, fallback)                                                    \
  __extension__({                                                              \
    __auto_type _wc_r_ = (x);                                                  \
    _wc_r_.ok ? _wc_r_.v : (fallback);                                         \
  })

#define try_or_return(x, ret)                                                  \
  __extension__({                                                              \
    __auto_type _wc_r_ = (x);                                                  \
    if (!_wc_r_.ok) return (ret);                                              \
    _wc_r_.v;                                                                  \
  })

// panic is converted into a 500 at the HTTP boundary; never used for flow.
[[noreturn]] void wc_panic(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
#define panic(...) wc_panic(__FILE__, __LINE__, __VA_ARGS__)
#define wc_assert(c)                                                           \
  do {                                                                         \
    if (!(c)) wc_panic(__FILE__, __LINE__, "assertion failed: %s", #c);         \
  } while (0)

// ---------------------------------------------------------------- panic frames
// wc_panic() unwinds to the nearest installed frame, so a middleware can turn a
// panic into a 500 without the process dying (SPEC §6.6 `recover`).
// wasm has no setjmp until the exception-handling proposal lands in every
// engine, so on wasm a panic traps and the frame is a plain marker.
#if defined(__wasm__)
typedef struct wc_try_frame {
  void *cleanup_mark; // the defer()/errdefer() chain when the frame was installed
  i32 inst_mark;      // how many component instances were bound (see with_inst)
  i32 own_mark;       // how many render-owned positions were drawing (see state_own)
  struct wc_try_frame *prev;
} wc_try_frame;
#else
#include <setjmp.h>
typedef struct wc_try_frame {
  jmp_buf jb;
  void *cleanup_mark; // the defer()/errdefer() chain when the frame was installed
  i32 inst_mark;      // how many component instances were bound (see with_inst)
  i32 own_mark;       // how many render-owned positions were drawing (see state_own)
  struct wc_try_frame *prev;
} wc_try_frame;
#endif

// `if (wc_try_begin(&f)) { ... } else { a panic unwound to here }`.
//
// This is a macro, and setjmp() must stay in the caller's frame: setjmp() then
// returning from a helper, and longjmp()ing back into it later, resumes a frame
// that no longer exists. The result is not a tidy failure — with clang the
// caller keeps the "it returned true" answer in a register and takes the success
// branch again on the way back (measured: a panic silently skipped the recovery
// block, then SIGBUS when the jump clobbered the stack). So: setjmp here, the
// frame bookkeeping in wc_try_install()/wc_try_spent().
bool wc_try_install(wc_try_frame *f);   // push f, true
void wc_try_spent(wc_try_frame *f);     // pop f after an unwind
void wc_try_end(wc_try_frame *f);       // pop f on the normal path
const char *wc_panic_message(void);

// A longjmp skips C `cleanup` handlers, so the ambient component instance stack has to
// be restored by the unwinder: a view that panicked inside with_inst(&card) would
// otherwise leave that card bound for the next request served by the same task.
// Declared weak because the reactive runtime is not part of every link (wasm has it,
// but a core-only embedder does not).
__attribute__((weak)) i32 wc_inst_depth(void);
__attribute__((weak)) void wc_inst_restore(i32 depth);
// The same for the render-owned owner stack (react.h): a view that panicked while drawing a
// keyed row must not leave that row bound to whatever runs next on the task.
__attribute__((weak)) i32 wc_own_depth(void);
__attribute__((weak)) void wc_own_restore(i32 depth);

#if defined(__wasm__)
// no setjmp on wasm: a panic traps, so the frame is a plain marker
#define wc_try_begin(f_) (wc_try_install(f_), true)
#else
#define wc_try_begin(f_)                                                       \
  __extension__({                                                              \
    __auto_type wc_tf_ = (f_);                                                 \
    setjmp(wc_tf_->jb) == 0 ? (wc_try_install(wc_tf_), true)                    \
                            : (wc_try_spent(wc_tf_), false);                    \
  })
#endif

// ---------------------------------------------------------------- defer
//
// A longjmp does not run a scope's cleanup attributes, so a `defer` that relied on them
// alone would only hold on the return path: a handler that panicked would skip every
// cleanup it had registered — and a WMutex taken under a defer would stay taken for the
// life of the process. So the slot is registered twice: the compiler runs it at
// block exit (cleanup attribute), and the panic unwinder runs whatever is left when
// it jumps to a frame. Both go through wc_cleanup_pop_run/unwind, which run a slot
// once.
#define WC_CAT_(a, b) a##b
#define WC_CAT(a, b)  WC_CAT_(a, b)
#define WC_UNIQ_(n)   WC_CAT(wc_uniq_, n)
#define WC_UNIQ(n)    WC_UNIQ_(n)

typedef struct WCleanupSlot {
  void (*fn)(void *);
  void *arg;
  struct WCleanupSlot *prev;
  int pending_at_entry; // errdefer: wc_err_pending when the slot was installed
  bool errdefer;
} WCleanupSlot;

// The unwinder state: per task, so a handler that parks keeps it across a worker
// switch (the same reason the panic frame and the build pointer live there).
typedef struct WUnwind {
  WCleanupSlot *cleanup;
  int err_pending;
  bool unwinding;
} WUnwind;

WCleanupSlot *wc_cleanup_push(void (*fn)(void *), void *arg, bool errdefer);
void wc_cleanup_pop_run(WCleanupSlot **slot); // block exit
void wc_cleanup_unwind_to(void *mark);        // panic: run everything above `mark`
int *wc_err_pending_ptr(void);

// defer(fn, x) calls fn(&x) when the enclosing block exits, in reverse order —
// including when it exits through a panic.
#define defer(fn_, x_) WC_DEFER_(WC_UNIQ(__COUNTER__), fn_, x_)
#define WC_DEFER_(id, fn_, x_)                                                 \
  for (WCleanupSlot *id __attribute__((cleanup(wc_cleanup_pop_run))) =          \
           wc_cleanup_push((void (*)(void *))(fn_), &(x_), false);             \
       id; id = nullptr)

// errdefer(fn, x) calls fn(&x) only when the block exits through a failed try()
// (or a panic while one is pending).
#define errdefer(fn_, x_) WC_ERRDEFER_(WC_UNIQ(__COUNTER__), fn_, x_)
#define WC_ERRDEFER_(id, fn_, x_)                                              \
  for (WCleanupSlot *id __attribute__((cleanup(wc_cleanup_pop_run))) =          \
           wc_cleanup_push((void (*)(void *))(fn_), &(x_), true);              \
       id; id = nullptr)

// The counter try() bumps and errdefer() consumes. Task-local, like the cleanup
// chain: a _Thread_local one was read on whichever worker a parked task resumed on.
#define wc_err_pending (*wc_err_pending_ptr())

// ---------------------------------------------------------------- Result of void

typedef struct WErrRes {
  bool ok;
  WErr e;
} WErrRes;

static inline WErrRes wok(void) { return (WErrRes){ .ok = true }; }
static inline WErrRes wfail(WErrKind k, str msg) { return (WErrRes){ .ok = false, .e = werr(k, msg) }; }
#define tryv(x)                                                                \
  do {                                                                         \
    WErrRes _wc_v_ = (x);                                                      \
    if (!_wc_v_.ok) {                                                          \
      wc_err_pending++;                                                        \
      return _wc_v_;                                                           \
    }                                                                          \
  } while (0)

// ---------------------------------------------------------------- hashing

bool wc_ct_eq(str a, str b);     // constant-time comparison

u64 wc_hash_bytes(const void *p, size_t n);
static inline u64 wc_hash_str(str s) { return wc_hash_bytes(s.p, s.n); }
static inline u64 wc_hash_u64(u64 v) {
  v ^= v >> 33; v *= 0xff51afd7ed558ccdULL;
  v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ULL;
  v ^= v >> 33;
  return v;
}
static inline u64 wc_hash_i64(i64 v) { return wc_hash_u64((u64)v); }

// ---------------------------------------------------------------- misc helpers

static inline i64 i64_min(i64 a, i64 b) { return a < b ? a : b; }
static inline i64 i64_max(i64 a, i64 b) { return a > b ? a : b; }
static inline size_t size_min(size_t a, size_t b) { return a < b ? a : b; }

u64  wc_time_ms(void);          // monotonic milliseconds
u64  wc_wall_ns(void);          // monotonic nanoseconds (benchmarks, budgets)
u64  wc_wall_ms(void);          // unix epoch milliseconds
void wc_sleep_ms(u64 ms);       // green-thread aware when called inside a task
str  wc_time_rfc3339(Arena *a, u64 unix_ms);
size_t wc_time_rfc3339_buf(char *out, size_t cap, u64 unix_ms); // no allocation

#endif // WEBC_CORE_H