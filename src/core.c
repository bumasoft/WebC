// SPDX-License-Identifier: Apache-2.0
// WebC — core runtime implementation.
#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include <webc/core.h>

#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- allocation seam

static _Atomic size_t g_heap_used;
static _Atomic size_t g_heap_peak;
static _Atomic size_t g_heap_allocs;
static _Atomic size_t g_heap_frees;
static _Atomic size_t g_alloc_fail;
static _Atomic size_t g_heap_limit;
static _Atomic size_t g_arena_live;
static _Atomic size_t g_arena_peak;

static _Thread_local wc_try_frame *t_panic_frame;
static _Thread_local char t_panic_msg[512];

// A panic handler is a frame on the task's own stack, and a task that awaits can
// resume on another worker thread. Keeping the frame in thread-local storage
// therefore both loses it (a panic becomes an abort) and, worse, can find a
// frame from a *finished* request and longjmp into a recycled stack (measured:
// SIGBUS, a jump to a wild address under load). The scheduler provides per-task
// storage; without one (no scheduler, or wasm) thread-local is exactly right.
#if defined(__wasm__)
#define FRAME_GET() t_panic_frame
#define FRAME_SET(f) (t_panic_frame = (f))
#else
extern void *wtask_panic_get(void) __attribute__((weak));
extern void wtask_panic_set(void *f) __attribute__((weak));
static wc_try_frame *frame_get(void) {
  return wtask_panic_get ? (wc_try_frame *)wtask_panic_get() : t_panic_frame;
}
static void frame_set(wc_try_frame *f) {
  if (wtask_panic_set)
    wtask_panic_set(f);
  else
    t_panic_frame = f;
}
#define FRAME_GET() frame_get()
#define FRAME_SET(f) frame_set(f)
#endif

// The setjmp() itself lives in the caller (see core.h): these two only keep the
// frame chain, which is where the "one frame per task, not per thread" rule lives.
static WUnwind *unwind_state(void); // defined with the unwind state, below

bool wc_try_install(wc_try_frame *f) {
  f->prev = FRAME_GET();
  f->cleanup_mark = unwind_state()->cleanup;
  f->inst_mark = wc_inst_depth ? wc_inst_depth() : 0;
  f->own_mark = wc_own_depth ? wc_own_depth() : 0;
  FRAME_SET(f);
  return true;
}

void wc_try_spent(wc_try_frame *f) {
  FRAME_SET(f->prev); // unwound: the frame is spent
}

void wc_try_end(wc_try_frame *f) {
  FRAME_SET(f->prev);
}
const char *wc_panic_message(void) {
  return t_panic_msg[0] ? t_panic_msg : "panic";
}

[[noreturn]] void wc_panic(const char *file, int line, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(t_panic_msg, sizeof t_panic_msg, fmt, ap);
  va_end(ap);
#if !defined(__wasm__)
  wc_try_frame *frame = FRAME_GET();
  if (frame) {
    // A longjmp skips the cleanup attributes of every scope it leaves, so the
    // registered ones are run by hand first: locking a mutex under a defer() used
    // to leak the mutex for the life of the process.
    wc_cleanup_unwind_to(frame->cleanup_mark);
    if (wc_inst_restore) wc_inst_restore(frame->inst_mark);
    if (wc_own_restore) wc_own_restore(frame->own_mark);
    longjmp(frame->jb, 1);
  }
#endif
  fprintf(stderr, "webc: panic at %s:%d: %s\n", file, line, t_panic_msg);
  fflush(stderr);
  abort();
}

bool wc_ct_eq(str a, str b) {
  if (a.n != b.n) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.n; i++)
    diff |= (unsigned char)(a.p[i] ^ b.p[i]);
  return diff == 0;
}

// Every user pointer carries a 16-byte header with its size and a magic word so
// wm_free can account without the caller remembering the length.
typedef struct BlkHdr {
  size_t size;
  u32 magic;
  u32 kind; // 0 heap, 1 arena (freed as a no-op)
} BlkHdr;

#define WC_MAGIC 0x57424331u // 'WBC1'

void *wm_alloc(size_t n) {
  if (n == 0) n = 1;
  size_t limit = atomic_load_explicit(&g_heap_limit, memory_order_relaxed);
  if (limit) {
    size_t used = atomic_load_explicit(&g_heap_used, memory_order_relaxed);
    if (used + n > limit) {
      atomic_fetch_add_explicit(&g_alloc_fail, 1, memory_order_relaxed);
      wc_panic(__FILE__, __LINE__, "heap limit reached (%zu + %zu > %zu)", used, n, limit);
    }
  }
  BlkHdr *h = calloc(1, sizeof(BlkHdr) + n);
  if (!h) wc_panic(__FILE__, __LINE__, "out of memory allocating %zu bytes", n);
  h->size = n;
  h->magic = WC_MAGIC;
  h->kind = 0;
  atomic_fetch_add_explicit(&g_heap_allocs, 1, memory_order_relaxed);
  size_t used = atomic_fetch_add_explicit(&g_heap_used, n, memory_order_relaxed) + n;
  size_t peak = atomic_load_explicit(&g_heap_peak, memory_order_relaxed);
  while (used > peak &&
         !atomic_compare_exchange_weak_explicit(&g_heap_peak, &peak, used, memory_order_relaxed,
                                                memory_order_relaxed)) {
  }
  return h + 1;
}

void *wm_alloc_zero(size_t n) {
  return wm_alloc(n);
}

void *wm_realloc(void *p, size_t n) {
  if (!p) return wm_alloc(n);
  BlkHdr *h = (BlkHdr *)p - 1;
  if (h->magic != WC_MAGIC) wc_panic(__FILE__, __LINE__, "wm_realloc of a foreign pointer");
  if (h->kind == 1)
    wc_panic(__FILE__, __LINE__, "wm_realloc of arena memory (bounded lifetime by design)");
  size_t old = h->size;
  void *np = wm_alloc(n);
  memcpy(np, p, old < n ? old : n);
  wm_free(p);
  return np;
}

// ---------------------------------------------------------------- DIAGNOSTIC ONLY
// WEBC_QUARANTINE=N stops returning the last N blocks to the allocator and poisons
// their payload, so a read of freed memory lands on mapped, recognisable bytes and
// `wc_heap_alive()` can answer "this was freed" instead of crashing somewhere later.
// This is how the intermittent keyed-list SIGSEGV gets named: ASan's timing hides it.
#define WC_QMAX 262144
static void *g_qring[WC_QMAX];
static size_t g_qn;
static int quarantine_slots(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("WEBC_QUARANTINE");
    v = e && *e ? atoi(e) : 0;
    if (v < 0) v = 0;
    if (v > WC_QMAX) v = WC_QMAX;
  }
  return v;
}

// Who freed this block? Only tracked under WEBC_QUARANTINE, and only because a
// quarantined block is never reused, so the address is unambiguous.
// The free site is recorded in the same slot as the quarantined block, so the answer
// cannot be lost to a wrapping side table (the first attempt used a separate 8k ring
// and answered "(nil)" exactly when it mattered).
static void *g_qcaller[WC_QMAX];
// What does the allocator think this pointer is? Fills `out` with a one-line verdict.
void wc_heap_describe(const void *p, char *out, size_t n) {
  if (!p) { snprintf(out, n, "null"); return; }
  const BlkHdr *h = (const BlkHdr *)p - 1;
  int q = quarantine_slots();
  bool inq = false;
  for (int i = 0; i < q; i++)
    if (g_qring[i] == (void *)p) { inq = true; break; }
  if (h->magic == WC_MAGIC)
    snprintf(out, n, "wm block: size=%zu kind=%u quarantined=%d", h->size, h->kind, (int)inq);
  else
    snprintf(out, n, "NOT a wm block: raw magic=0x%08x size=%zu quarantined=%d",
             h->magic, h->size, (int)inq);
}

void *wc_heap_freed_by(const void *p) {
  int q = quarantine_slots();
  for (int i = 0; i < q; i++)
    if (g_qring[i] == p) return g_qcaller[i];
  return nullptr;
}

// True while `p` is a live wm_alloc block. Safe to call on a quarantined pointer
// (the memory is still mapped); anything else is the caller's bug.
bool wc_heap_alive(const void *p) {
  if (!p) return false;
  const BlkHdr *h = (const BlkHdr *)p - 1;
  return h->magic == WC_MAGIC && h->kind == 0;
}

void wm_free(void *p) {
  if (!p) return;
  BlkHdr *h = (BlkHdr *)p - 1;
  if (h->magic != WC_MAGIC) return; // not ours (arena tail, literal, …): ignore
  if (h->kind == 1) return;         // arena memory: no-op by contract
  h->magic = 0;
  atomic_fetch_sub_explicit(&g_heap_used, h->size, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_heap_frees, 1, memory_order_relaxed);
  int q = quarantine_slots();
  if (q > 0) {
    size_t slot = g_qn++ % (size_t)q;
    void *old = g_qring[slot];
    g_qring[slot] = h;
#if defined(__wasm__)
    // wasm has no return address to record (clang refuses the builtin outright);
    // the quarantine itself — mapped, poisoned bytes — is what the diagnostic needs
    g_qcaller[slot] = nullptr;
#else
    g_qcaller[slot] = __builtin_return_address(0);
#endif
    memset(h + 1, 0xDD, h->size);
    if (old) free(old);
    return;
  }
  free(h);
}

char *wm_strdup(const char *s) {
  if (!s) return nullptr;
  size_t n = strlen(s);
  char *p = wm_alloc(n + 1);
  memcpy(p, s, n + 1);
  return p;
}

char *wm_strndup(const char *s, size_t n) {
  char *p = wm_alloc(n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

WmStats wm_stats(void) {
  return (WmStats){
      .heap_used = atomic_load(&g_heap_used),
      .heap_peak = atomic_load(&g_heap_peak),
      .heap_allocs = atomic_load(&g_heap_allocs),
      .heap_frees = atomic_load(&g_heap_frees),
      .arena_live = atomic_load(&g_arena_live),
      .arena_peak = atomic_load(&g_arena_peak),
      .alloc_fail = atomic_load(&g_alloc_fail),
  };
}

void wm_stats_reset_peak(void) {
  atomic_store(&g_heap_peak, atomic_load(&g_heap_used));
  atomic_store(&g_arena_peak, atomic_load(&g_arena_live));
}

size_t wm_heap_limit(void) {
  return atomic_load(&g_heap_limit);
}
void wm_set_heap_limit(size_t bytes) {
  atomic_store(&g_heap_limit, bytes);
}

// ---------------------------------------------------------------- arena

struct ArenaChunk {
  ArenaChunk *next;
  size_t cap, used;
  max_align_t data[];
};

static ArenaChunk *chunk_new(size_t cap) {
  if (cap < 256) cap = 256;
  ArenaChunk *c = malloc(sizeof(ArenaChunk) + cap);
  if (!c) wc_panic(__FILE__, __LINE__, "out of memory for arena chunk (%zu)", cap);
  c->next = nullptr;
  c->cap = cap;
  c->used = 0;
  return c;
}

Arena *arena_new(size_t chunk_size) {
  return arena_new_labeled(chunk_size, "arena");
}

Arena *arena_new_labeled(size_t chunk_size, const char *label) {
  Arena *a = calloc(1, sizeof(Arena));
  if (!a) wc_panic(__FILE__, __LINE__, "out of memory for arena");
  a->first_chunk = chunk_size ? chunk_size : 8192;
  a->chunks_cap = 8;
  a->chunks = calloc(a->chunks_cap, sizeof(ArenaChunk *));
  if (!a->chunks) wc_panic(__FILE__, __LINE__, "out of memory for arena table");
  a->label = label;
  // Nothing is reserved yet: the first chunk appears in arena_push_chunk(), the one
  // place that accounts real bytes. Counting first_chunk here as well double-counts a
  // used arena and leaks the whole reservation for an arena that is created and destroyed
  // without allocating — /metrics would report 9 GB of live arenas after 300k requests.
  return a;
}

void arena_destroy(Arena *a) {
  if (!a) return;
  for (size_t i = 0; i < a->nchunks; i++)
    free(a->chunks[i]);
  atomic_fetch_sub(&g_arena_live, a->total);
  free(a->chunks);
  free(a);
}

// O(1): rewind to the first chunk and keep the rest for the next request.
//
// WEBC_ARENA_POISON=1 fills the bytes being rewound with 0xDB instead of leaving whatever was
// there. Nothing resets memory in normal operation (that would cost a memset per request and
// per render), so a stale pointer into a rewound chunk usually still *reads* as the string it
// held — which is why a dangling attribute name can survive thousands of renders and then,
// one time in three, read a NUL: some other allocation landed on it. Poisoning makes a stale
// read say so every time instead of never. Diagnostics only; off unless asked for.
static bool arena_poison_wanted(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *v = getenv("WEBC_ARENA_POISON");
    cached = v && *v && strcmp(v, "0") != 0 ? 1 : 0;
  }
  return cached == 1;
}

void arena_reset(Arena *a) {
  if (!a) return;
  a->cur = 0;
  a->used = 0;
  size_t live = 0;
  bool poison = arena_poison_wanted();
  for (size_t i = 0; i < a->nchunks; i++) {
    if (poison) memset(a->chunks[i]->data, 0xDB, a->chunks[i]->cap);
    a->chunks[i]->used = 0;
    live += a->chunks[i]->cap;
  }
  // `total` is the bytes the arena still holds reserved (the chunk capacities), not the
  // bytes in use: the chunks are kept for the next render, so the accounting has to
  // stay with them or the next arena_grow() reports a number it does not have.
  a->total = live;
  a->allocs = 0;
  // Everything this arena owned is gone, including the per-request row cache: it points at
  // rows that no longer exist, and the arena is about to be handed to a different request.
  a->row_pool = nullptr;
}

void arena_release(Arena *a) {
  if (!a) return;
  for (size_t i = 0; i < a->nchunks; i++)
    free(a->chunks[i]);
  atomic_fetch_sub(&g_arena_live, a->total);
  a->nchunks = 0;
  a->cur = 0;
  a->used = 0;
  a->total = 0;
}

static ArenaChunk *arena_push_chunk(Arena *a, size_t need) {
  size_t cap = a->first_chunk;
  while (cap < need)
    cap *= 2;
  if (a->nchunks == a->chunks_cap) {
    size_t ncap = a->chunks_cap * 2;
    ArenaChunk **nc = realloc(a->chunks, ncap * sizeof(ArenaChunk *));
    if (!nc) wc_panic(__FILE__, __LINE__, "out of memory growing arena table");
    a->chunks = nc;
    a->chunks_cap = ncap;
  }
  // Reuse a chunk that was released earlier if it is big enough.
  ArenaChunk *c = chunk_new(cap);
  a->chunks[a->nchunks++] = c;
  a->total += c->cap;
  atomic_fetch_add(&g_arena_live, c->cap);
  size_t live = atomic_load(&g_arena_live);
  size_t peak = atomic_load(&g_arena_peak);
  while (live > peak && !atomic_compare_exchange_weak(&g_arena_peak, &peak, live)) {
  }
  if (a->total > a->peak) a->peak = a->total;
  return c;
}

void *arena_alloc(Arena *a, size_t n) {
  if (!a) return wm_alloc(n);
  if (n == 0) n = 1;
  const size_t align = alignof(max_align_t);
  n = (n + align - 1) & ~(align - 1);
  if (a->nchunks == 0 || a->used + n > a->chunks[a->cur]->cap) {
    if (a->nchunks > 0 && a->cur + 1 < a->nchunks && a->chunks[a->cur + 1]->cap >= n) {
      a->cur++;
      a->used = 0;
    } else {
      ArenaChunk *c = arena_push_chunk(a, n);
      a->cur = a->nchunks - 1;
      a->used = 0;
      (void)c;
    }
  }
  ArenaChunk *c = a->chunks[a->cur];
  void *p = (char *)c->data + a->used;
  a->used += n;
  c->used = a->used;
  a->allocs++;
  return p;
}

void *arena_zalloc(Arena *a, size_t n) {
  void *p = arena_alloc(a, n);
  memset(p, 0, n);
  return p;
}

void *arena_memdup(Arena *a, const void *p, size_t n) {
  void *q = arena_alloc(a, n);
  if (n) memcpy(q, p, n);
  return q;
}

char *arena_strdup(Arena *a, const char *s) {
  if (!s) return nullptr;
  size_t n = strlen(s) + 1;
  char *p = arena_alloc(a, n);
  memcpy(p, s, n);
  return p;
}

size_t arena_bytes(const Arena *a) {
  size_t n = 0;
  for (size_t i = 0; i < a->nchunks; i++)
    n += a->chunks[i]->used;
  return n;
}

size_t arena_high_water(const Arena *a) {
  return a->peak;
}

ArenaPool *arena_pool_new(size_t chunk_size) {
  ArenaPool *p = calloc(1, sizeof *p);
  if (!p) wc_panic(__FILE__, __LINE__, "oom");
  p->chunk = chunk_size ? chunk_size : 16 * 1024;
  p->max_bytes = p->chunk * 64; // a few hot requests, not every burst ever seen
  atomic_store(&p->lock, 0);
  return p;
}

static void pool_lock(ArenaPool *p) {
  while (atomic_exchange_explicit(&p->lock, 1, memory_order_acquire)) {
  }
}

static void pool_unlock(ArenaPool *p) {
  atomic_store_explicit(&p->lock, 0, memory_order_release);
}

Arena *arena_pool_get(ArenaPool *p) {
  if (!p) return arena_new(16 * 1024);
  pool_lock(p);
  Arena *a = p->free_list;
  if (a) p->free_list = a->next_free;
  pool_unlock(p);
  if (a) {
    atomic_fetch_sub(&p->free_bytes, a->total);
    arena_reset(a);
    return a;
  }
  atomic_fetch_add(&p->live, 1);
  return arena_new_labeled(p->chunk, "request");
}

void arena_pool_put(ArenaPool *p, Arena *a) {
  if (!p || !a) {
    arena_destroy(a);
    return;
  }
  arena_reset(a);
  // Keep the small, hot arenas and drop the ones a big body made expensive: a pooled
  // arena keeps every chunk it ever grew, and 200 concurrent uploads would otherwise
  // hold their peak forever.
  if (a->total > p->chunk * 8) {
    arena_destroy(a);
    atomic_fetch_sub(&p->live, 1);
    return;
  }
  pool_lock(p);
  bool room = atomic_load(&p->free_bytes) + a->total <= p->max_bytes;
  if (room) {
    a->next_free = p->free_list;
    p->free_list = a;
    atomic_fetch_add(&p->free_bytes, a->total);
  }
  pool_unlock(p);
  if (!room) {
    arena_destroy(a);
    atomic_fetch_sub(&p->live, 1);
  }
}

// ---------------------------------------------------------------- bound arena

_Thread_local Arena *wc_arena_cur;

// The current arena follows the *task*, not the thread: a task that parks on an
// await and resumes on another worker thread must find its request arena again.
// The scheduler installs the task's arena around each switch through this
// function, which also keeps the access on the right thread (a thread-local
// address cached in a callee-saved register must never survive a switch).
__attribute__((noinline)) Arena *wc_arena_get(void) {
  __asm__ volatile("" ::: "memory");
  return wc_arena_cur;
}

__attribute__((noinline)) Arena *wc_arena_swap(Arena *a) {
  __asm__ volatile("" ::: "memory");
  Arena *prev = wc_arena_cur;
  wc_arena_cur = a;
  return prev;
}

void *wc_alloc(size_t n) {
  return wc_arena_cur ? arena_alloc(wc_arena_cur, n) : wm_alloc(n);
}
void *wc_alloc_zero(size_t n) {
  void *p = wc_alloc(n);
  memset(p, 0, n);
  return p;
}

char *wc_strndup(const char *s, size_t n) {
  char *p = wc_alloc(n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

void wc_free(void *p) {
  wm_free(p);
}

// ---------------------------------------------------------------- app arena, text

static _Atomic(uintptr_t) g_app_arena;

Arena *wc_app_arena(void) {
  return (Arena *)atomic_load(&g_app_arena);
}

void wc_app_arena_set(Arena *a) {
  atomic_store(&g_app_arena, (uintptr_t)a);
}

void wvec_push_impl(void *at_p, i32 *n, i32 *cap, size_t elem_size, Arena *a, const void *elem) {
  char **at = at_p;
  if (*n >= *cap) {
    i32 ncap = *cap ? *cap * 2 : 8;
    char *np = a ? arena_alloc(a, (size_t)ncap * elem_size) : wm_alloc((size_t)ncap * elem_size);
    if (*at && *n) memcpy(np, *at, (size_t)(*n) * elem_size);
    if (!a && *at) wm_free(*at);
    *at = np;
    *cap = ncap;
  }
  memcpy(*at + (size_t)(*n) * elem_size, elem, elem_size);
  (*n)++;
}

char *str_cdup(Arena *a, str s) {
  char *p = a ? arena_alloc(a, s.n + 1) : wm_alloc(s.n + 1);
  if (s.n) memcpy(p, s.p, s.n);
  p[s.n] = 0;
  return p;
}

// The scope arena is whatever the framework installed: a request, a render, or
// the app itself. No caller ever names it.
// `fmt()` as a fat string: the form that needs no conversion at the point of use —
// `link_to(fmt_s("/f/%s", n))`, `set(title, fmt_s("%s — %s", a, b))`. `fmt()` returns a Text
// (a `char *`), and mixing the two was a wall of "initializing 'str' with 'Text'" with no
// mention of WC_AS_STR anywhere in it.
str fmt_s(const char *spec, ...) {
  va_list ap;
  va_start(ap, spec);
  va_list cp;
  va_copy(cp, ap);
  int n = vsnprintf(nullptr, 0, spec, cp);
  va_end(cp);
  if (n < 0) {
    va_end(ap);
    return (str){0};
  }
  char *out = wc_alloc((size_t)n + 1);
  vsnprintf(out, (size_t)n + 1, spec, ap);
  va_end(ap);
  return (str){out, (size_t)n};
}

Text fmt(const char *spec, ...) {
  va_list ap;
  va_start(ap, spec);
  va_list cp;
  va_copy(cp, ap);
  int n = vsnprintf(nullptr, 0, spec, cp);
  va_end(cp);
  if (n < 0) {
    va_end(ap);
    return "";
  }
  char *out = wc_alloc((size_t)n + 1);
  vsnprintf(out, (size_t)n + 1, spec, ap);
  va_end(ap);
  return out;
}

Text text_copy(const char *s) {
  if (!s) return "";
  return wc_strndup(s, strlen(s));
}

// ---------------------------------------------------------------- numbers as text
//
// Grouped digits, written once. `sep` is a parameter because a number field and a chart
// label want the same digits with and without the commas.
static char *wc_group_digits(char *out, u64 u, char sep) {
  char tmp[24];
  int n = snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)u);
  char *o = out;
  for (int i = 0; i < n; i++) {
    if (i && (n - i) % 3 == 0) *o++ = sep;
    *o++ = tmp[i];
  }
  *o = 0;
  return o;
}

Text fmt_num(i64 v) {
  char *out = wc_alloc(32); // 20 digits + 6 separators + sign + NUL
  char *p = out;
  if (v < 0) *p++ = '-';
  u64 u = v < 0 ? (u64)0 - (u64)v : (u64)v;
  wc_group_digits(p, u, ',');
  return out;
}

Text fmt_num_u(u64 v) {
  char *out = wc_alloc(32);
  wc_group_digits(out, v, ',');
  return out;
}

Text fmt_money_c(double v, const char *sym) {
  const char *s = sym ? sym : "$";
  if (v > -0.5 && v < 0.5) return fmt("%s0", s); // under half a unit is no money at all
  bool neg = v < 0;
  double a = neg ? -v : v;
  size_t sl = strlen(s);
  char *out = wc_alloc(sl + 40);
  char *p = out;
  if (neg) *p++ = '-';   // -$1,234, not $-1,234
  memcpy(p, s, sl);
  p += sl;
  wc_group_digits(p, (u64)(a + 0.5), ',');
  return out;
}

Text fmt_money(double v) { return fmt_money_c(v, "$"); }

Text fmt_pct(double frac, int decimals) {
  if (decimals < 0) decimals = 0;
  if (decimals > 6) decimals = 6;
  char *out = wc_alloc(48);
  snprintf(out, 48, "%.*f%%", decimals, frac * 100.0);
  return out;
}

Text fmt_compact(double v) {
  double a = v < 0 ? -v : v;
  const char *unit = "";
  double scale = 1.0;
  if (a >= 1e12)      { unit = "T"; scale = 1e12; }
  else if (a >= 1e9)  { unit = "B"; scale = 1e9; }
  else if (a >= 1e6)  { unit = "M"; scale = 1e6; }
  else if (a >= 1e3)  { unit = "k"; scale = 1e3; }
  if (!*unit) return fmt("%g", v);
  double s = v / scale;
  double as = s < 0 ? -s : s;
  char *out = wc_alloc(32);
  // One decimal when it says something (2.5M), none when it does not (1M, 10k).
  if (as >= 10 || s == (double)(i64)s) snprintf(out, 32, "%g%s", s, unit);
  else snprintf(out, 32, "%.1f%s", s, unit);
  return out;
}

Text text_of(str s) { return str_cdup(wc_arena_get(), s); }

Text now(void) {
  // wc_time_rfc3339 already NUL-terminates; the arena is the current scope.
  return wc_time_rfc3339(wc_arena_get(), wc_wall_ms()).p;
}

Text persist(const char *s) {
  if (!s) return "";
  Arena *app = wc_app_arena();
  if (!app) {
    // No app arena: the caller is outside a boot (a test, a tool). Heap text is
    // the honest answer, and it is what a `persist` outside an app means.
    return wm_strdup(s);
  }
  return arena_strdup(app, s);
}

// ---------------------------------------------------------------- strings

bool str_eq(str a, str b) {
  if (a.n != b.n) return false;
  if (a.n == 0) return true;
  return memcmp(a.p, b.p, a.n) == 0;
}

bool str_eq_c(str a, const char *b) {
  size_t n = b ? strlen(b) : 0;
  if (a.n != n) return false;
  return n == 0 || memcmp(a.p, b, n) == 0;
}

int str_cmp(str a, str b) {
  size_t n = a.n < b.n ? a.n : b.n;
  int c = n ? memcmp(a.p, b.p, n) : 0;
  if (c) return c;
  if (a.n == b.n) return 0;
  return a.n < b.n ? -1 : 1;
}

bool str_has_prefix(str s, str pre) {
  return s.n >= pre.n && (pre.n == 0 || memcmp(s.p, pre.p, pre.n) == 0);
}

bool str_has_prefix_c(str s, const char *pre) {
  return str_has_prefix(s, str_lit(pre));
}

bool str_has_suffix(str s, str suf) {
  return s.n >= suf.n && (suf.n == 0 || memcmp(s.p + s.n - suf.n, suf.p, suf.n) == 0);
}

i64 str_rindex(str s, char c) {
  for (size_t i = s.n; i-- > 0;)
    if (s.p[i] == c) return (i64)i;
  return -1;
}

i64 str_index(str s, char c) {
  for (size_t i = 0; i < s.n; i++)
    if (s.p[i] == c) return (i64)i;
  return -1;
}

str str_sub(str s, size_t from, size_t to) {
  if (from > s.n) from = s.n;
  if (to > s.n) to = s.n;
  if (to < from) to = from;
  return (str){s.p + from, to - from};
}

static bool wc_isspace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

str str_trim_left(str s) {
  size_t i = 0;
  while (i < s.n && wc_isspace(s.p[i]))
    i++;
  return (str){s.p + i, s.n - i};
}

str str_trim_right(str s) {
  size_t n = s.n;
  while (n > 0 && wc_isspace(s.p[n - 1]))
    n--;
  return (str){s.p, n};
}

str str_trim(str s) {
  return str_trim_right(str_trim_left(s));
}

str str_dup(Arena *a, str s) {
  char *p = arena_alloc(a, s.n + 1);
  if (s.n) memcpy(p, s.p, s.n);
  p[s.n] = 0;
  return (str){p, s.n};
}

char *str_cstr(Arena *a, str s) {
  char *p = a ? arena_alloc(a, s.n + 1) : wm_alloc(s.n + 1);
  if (s.n) memcpy(p, s.p, s.n);
  p[s.n] = 0;
  return p;
}

str str_fmt(Arena *a, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    return (str){"", 0};
  }
  char *p = a ? arena_alloc(a, (size_t)n + 1) : wm_alloc((size_t)n + 1);
  vsnprintf(p, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  return (str){p, (size_t)n};
}

str str_upper(Arena *a, str s) {
  char *p = a ? arena_alloc(a, s.n + 1) : wm_alloc(s.n + 1);
  for (size_t i = 0; i < s.n; i++)
    p[i] = (char)((s.p[i] >= 'a' && s.p[i] <= 'z') ? s.p[i] - 32 : s.p[i]);
  p[s.n] = 0;
  return (str){p, s.n};
}

str str_lower(Arena *a, str s) {
  char *p = a ? arena_alloc(a, s.n + 1) : wm_alloc(s.n + 1);
  for (size_t i = 0; i < s.n; i++)
    p[i] = (char)((s.p[i] >= 'A' && s.p[i] <= 'Z') ? s.p[i] + 32 : s.p[i]);
  p[s.n] = 0;
  return (str){p, s.n};
}

bool str_to_i64(str s, i64 *out) {
  s = str_trim(s);
  if (s.n == 0 || s.n > 20) return false;
  size_t i = 0;
  bool neg = false;
  if (s.p[0] == '-' || s.p[0] == '+') {
    neg = s.p[0] == '-';
    i = 1;
  }
  if (i == s.n) return false;
  u64 acc = 0;
  for (; i < s.n; i++) {
    if (s.p[i] < '0' || s.p[i] > '9') return false;
    u64 d = (u64)(s.p[i] - '0');
    if (acc > (UINT64_MAX - d) / 10) return false;
    acc = acc * 10 + d;
  }
  *out = neg ? (i64)(0 - acc) : (i64)acc;
  return true;
}

bool str_to_u64(str s, u64 *out) {
  i64 v;
  if (!str_to_i64(s, &v) || v < 0) {
    // allow full u64 range
    s = str_trim(s);
    if (s.n == 0 || s.n > 20) return false;
    u64 acc = 0;
    for (size_t i = 0; i < s.n; i++) {
      if (s.p[i] < '0' || s.p[i] > '9') return false;
      u64 d = (u64)(s.p[i] - '0');
      if (acc > (UINT64_MAX - d) / 10) return false;
      acc = acc * 10 + d;
    }
    *out = acc;
    return true;
  }
  *out = (u64)v;
  return true;
}

bool str_to_f64(str s, f64 *out) {
  s = str_trim(s);
  if (s.n == 0 || s.n > 64) return false;
  char tmp[80];
  memcpy(tmp, s.p, s.n);
  tmp[s.n] = 0;
  char *end = nullptr;
  errno = 0;
  double v = strtod(tmp, &end);
  if (errno != 0 || end != tmp + s.n) return false;
  *out = v;
  return true;
}

str str_from_i64(Arena *a, i64 v) {
  return str_fmt(a, "%lld", (long long)v);
}
str str_from_u64(Arena *a, u64 v) {
  return str_fmt(a, "%llu", (unsigned long long)v);
}
str str_from_f64(Arena *a, f64 v) {
  // shortest representation that round-trips, without a trailing ".0" surprise
  char tmp[40];
  for (int prec = 1; prec <= 17; prec++) {
    snprintf(tmp, sizeof tmp, "%.*g", prec, v);
    double back = strtod(tmp, nullptr);
    if (back == v) break;
  }
  return str_fmt(a, "%s", tmp);
}

i64 str_find(str hay, str needle, size_t from) {
  if (!hay.p || !needle.p || needle.n == 0 || from > hay.n) return -1;
  for (size_t i = from; i + needle.n <= hay.n; i++)
    if (memcmp(hay.p + i, needle.p, needle.n) == 0) return (i64)i;
  return -1;
}

bool str_contains(str hay, str needle) {
  if (needle.n == 0) return true;
  if (hay.n < needle.n) return false;
  for (size_t i = 0; i + needle.n <= hay.n; i++)
    if (memcmp(hay.p + i, needle.p, needle.n) == 0) return true;
  return false;
}

bool str_ci_contains(str hay, const char *needle) {
  size_t nn = strlen(needle);
  if (!hay.p || hay.n < nn) return false;
  for (size_t i = 0; i + nn <= hay.n; i++) {
    size_t k = 0;
    while (k < nn && tolower((unsigned char)hay.p[i + k]) == tolower((unsigned char)needle[k])) k++;
    if (k == nn) return true;
  }
  return false;
}

u64 str_hash(str s) {
  return wc_hash_bytes(s.p, s.n);
}

u64 wc_hash_bytes(const void *p, size_t n) {
  // FNV-1a then a final avalanche: fast and good enough for open addressing.
  const unsigned char *b = p;
  u64 h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= b[i];
    h *= 1099511628211ULL;
  }
  return wc_hash_u64(h);
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

str url_decode(Arena *a, str s, bool plus_is_space) {
  char *out = a ? arena_alloc(a, s.n + 1) : wm_alloc(s.n + 1);
  size_t o = 0;
  for (size_t i = 0; i < s.n; i++) {
    char c = s.p[i];
    if (c == '%' && i + 2 < s.n) {
      int h = hexval(s.p[i + 1]), l = hexval(s.p[i + 2]);
      if (h >= 0 && l >= 0) {
        out[o++] = (char)((h << 4) | l);
        i += 2;
        continue;
      }
    }
    if (c == '+' && plus_is_space) {
      out[o++] = ' ';
      continue;
    }
    out[o++] = c;
  }
  out[o] = 0;
  return (str){out, o};
}

str url_encode(Arena *a, str s) {
  static const char *hex = "0123456789ABCDEF";
  char *out = a ? arena_alloc(a, s.n * 3 + 1) : wm_alloc(s.n * 3 + 1);
  size_t o = 0;
  for (size_t i = 0; i < s.n; i++) {
    unsigned char c = (unsigned char)s.p[i];
    bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      out[o++] = (char)c;
    } else {
      out[o++] = '%';
      out[o++] = hex[c >> 4];
      out[o++] = hex[c & 15];
    }
  }
  out[o] = 0;
  return (str){out, o};
}

str html_escape(Arena *a, str s) {
  Buf b;
  buf_init(&b, a, s.n + 16);
  wc_html_esc_buf(&b, s);
  return buf_str(&b);
}

void wc_html_esc_buf(Buf *b, str s) {
  for (size_t i = 0; i < s.n; i++) {
    switch (s.p[i]) {
    case '&': buf_puts(b, "&amp;"); break;
    case '<': buf_puts(b, "&lt;"); break;
    case '>': buf_puts(b, "&gt;"); break;
    case '"': buf_puts(b, "&quot;"); break;
    case '\'': buf_puts(b, "&#39;"); break;
    default: buf_putc(b, s.p[i]); break;
    }
  }
}

void wc_line_esc(Buf *b, str v) {
  for (size_t i = 0; i < v.n; i++) {
    char c = v.p[i];
    if (c == '\\')
      buf_puts(b, "\\\\");
    else if (c == '\n')
      buf_puts(b, "\\n");
    else if (c == '\r')
      buf_puts(b, "\\r");
    else
      buf_putc(b, c);
  }
}

// Escaping, in runs: a JSON string is mostly bytes that need no escaping, so scan
// to the next one that does and copy the span in a single memcpy. Byte-at-a-time
// putc cost 2.5 ns per byte on a 1 KB value (measured: model_to_json of a 1 KB
// object was 2.7 us, over the 1 us budget in SPEC §10).
static inline bool json_safe(unsigned char c) {
  return c >= 0x20 && c != '"' && c != '\\';
}

// Where does the next byte that needs escaping live? Eight bytes at a time with
// the usual SWAR trick, because a per-byte scan is what a 1 KB value pays for
// (~2.5 cycles a byte, measured: 0.9 us of the 1 us round-trip budget). `word` is
// unaligned load; the masks find any byte < 0x20, or equal to '"' or '\\'.
static inline u64 swar_load(const char *p) {
  u64 v;
  memcpy(&v, p, sizeof v);
  return v;
}
static inline u64 swar_haszero(u64 v) {
  return (v - 0x0101010101010101ull) & ~v & 0x8080808080808080ull;
}
static inline u64 swar_hasless(u64 v, u64 n) { // any byte < n (n <= 0x80)
  return (v - 0x0101010101010101ull * n) & ~v & 0x8080808080808080ull;
}
static size_t json_scan(str s, size_t i) {
  const u64 quote = 0x2222222222222222ull, bslash = 0x5c5c5c5c5c5c5c5cull;
  while (i + 8 <= s.n) {
    u64 v = swar_load(s.p + i);
    u64 bad = swar_hasless(v, 0x20) | swar_haszero(v ^ quote) | swar_haszero(v ^ bslash);
    if (bad) {
      // some byte in this word needs escaping: find it one by one (rare)
      while (i < s.n && json_safe((unsigned char)s.p[i])) i++;
      return i;
    }
    i += 8;
  }
  while (i < s.n && json_safe((unsigned char)s.p[i])) i++;
  return i;
}

// How far a JSON string body can be copied verbatim: up to the next quote, backslash
// or control byte. Exported because the parser needs the same scan the writer does —
// byte-at-a-time made parsing a 1 KB value cost 0.7 us on its own.
size_t json_str_scan(const char *p, size_t n) {
  size_t i = 0;
  const u64 quote = 0x2222222222222222ull, bslash = 0x5c5c5c5c5c5c5c5cull;
  while (i + 8 <= n) {
    u64 v = swar_load(p + i);
    u64 bad = swar_hasless(v, 0x20) | swar_haszero(v ^ quote) | swar_haszero(v ^ bslash);
    if (bad) {
      while (i < n) {
        unsigned char c = (unsigned char)p[i];
        if (c == '"' || c == '\\' || c < 0x20) return i;
        i++;
      }
      return i;
    }
    i += 8;
  }
  while (i < n) {
    unsigned char c = (unsigned char)p[i];
    if (c == '"' || c == '\\' || c < 0x20) return i;
    i++;
  }
  return i;
}

void json_escape_into(Buf *b, str s) {
  // one reservation for the whole value: growing a buffer mid-string copies it
  // again, and that copy is what a 1 KB value pays for
  buf_reserve(b, s.n + 2);
  buf_putc(b, '"');
  size_t i = 0;
  while (i < s.n) {
    size_t run = json_scan(s, i);
    if (run > i) {
      buf_write(b, s.p + i, run - i);
      i = run;
      if (i >= s.n) break;
    }
    unsigned char c = (unsigned char)s.p[i];
    switch (c) {
    case '"':
      buf_puts(b, "\\\"");
      break;
    case '\\':
      buf_puts(b, "\\\\");
      break;
    case '\n':
      buf_puts(b, "\\n");
      break;
    case '\r':
      buf_puts(b, "\\r");
      break;
    case '\t':
      buf_puts(b, "\\t");
      break;
    case '\b':
      buf_puts(b, "\\b");
      break;
    case '\f':
      buf_puts(b, "\\f");
      break;
    default:
      if (c < 0x20) {
        char tmp[8];
        snprintf(tmp, sizeof tmp, "\\u%04x", c);
        buf_write(b, tmp, 6);
      } else {
        // the byte is part of a multi-byte UTF-8 sequence: copy it as it is
        buf_putc(b, (char)c);
      }
      break;
    }
    i++;
  }
  buf_putc(b, '"');
}

str json_escape(Arena *a, str s) {
  Buf b;
  buf_init(&b, a, s.n + 16);
  json_escape_into(&b, s);
  return buf_str(&b);
}

// ---------------------------------------------------------------- buf

void buf_init(Buf *b, Arena *a, size_t cap) {
  *b = (Buf){.a = a};
  if (cap) buf_reserve(b, cap);
}

void buf_free(Buf *b) {
  if (!b->a && b->p) wm_free(b->p);
  b->p = nullptr;
  b->n = b->cap = 0;
}

void buf_reserve(Buf *b, size_t extra) {
  if (b->n + extra + 1 <= b->cap) return;
  size_t need = b->n + extra + 1;
  size_t want = b->cap ? b->cap : 64;
  // A big append sizes the buffer once instead of doubling through every
  // intermediate size (which copies the value again at each step): four
  // reallocations and 1.9 KB of copying for a 1 KB string.
  if (extra >= want)
    want = need;
  else
    while (want < need) want *= 2;
  if (b->a) {
    char *np = arena_alloc(b->a, want);
    if (b->p && b->n) memcpy(np, b->p, b->n);
    b->p = np;
  } else {
    char *np = wm_alloc(want);
    if (b->p && b->n) memcpy(np, b->p, b->n);
    wm_free(b->p);
    b->p = np;
  }
  b->cap = want;
  if (b->p) b->p[b->n] = 0;
}

void buf_write(Buf *b, const void *p, size_t n) {
  if (n == 0) return;
  buf_reserve(b, n);
  memcpy(b->p + b->n, p, n);
  b->n += n;
  b->p[b->n] = 0;
}

void buf_puts(Buf *b, const char *s) {
  if (s) buf_write(b, s, strlen(s));
}

void buf_putstr(Buf *b, str s) {
  buf_write(b, s.p, s.n);
}

void buf_putc(Buf *b, char c) {
  buf_reserve(b, 1);
  b->p[b->n++] = c;
  b->p[b->n] = 0;
}

void buf_vprintf(Buf *b, const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap);
  if (n < 0) {
    va_end(ap2);
    return;
  }
  buf_reserve(b, (size_t)n);
  vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap2);
  b->n += (size_t)n;
  va_end(ap2);
}

// Integers without vsnprintf: one conversion measured 90 ns, and a JSON object
// with a handful of numbers paid it per field (the profile of a 1 KB round trip
// was a quarter `_vsnprintf`).
void buf_puti64(Buf *b, i64 v) {
  char tmp[24];
  char *p = tmp + sizeof tmp;
  bool neg = v < 0;
  u64 u = neg ? (u64)(-(v + 1)) + 1 : (u64)v;
  do {
    *--p = (char)('0' + (u % 10));
    u /= 10;
  } while (u);
  if (neg) *--p = '-';
  buf_write(b, p, (size_t)(tmp + sizeof tmp - p));
}

void buf_putu64(Buf *b, u64 v) {
  char tmp[24];
  char *p = tmp + sizeof tmp;
  do {
    *--p = (char)('0' + (v % 10));
    v /= 10;
  } while (v);
  buf_write(b, p, (size_t)(tmp + sizeof tmp - p));
}

void buf_printf(Buf *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  buf_vprintf(b, fmt, ap);
  va_end(ap);
}

str buf_str(Buf *b) {
  return (str){b->p ? b->p : "", b->n};
}

char *buf_cstr(Buf *b) {
  if (!b->p) buf_reserve(b, 1);
  b->p[b->n] = 0;
  return b->p;
}

void buf_reset(Buf *b) {
  b->n = 0;
  if (b->p) b->p[0] = 0;
}

// ---------------------------------------------------------------- key=value

// One walker for every `k=v&k=v` string the framework reads: a form body, a query string,
// a driver's connection parameters. The pair grammar is the same in all of them, and a
// second copy of it is a second set of rules for an empty pair or a missing '='.
bool wc_kv_next(str q, size_t *i, str *key, str *val) {
  while (*i < q.n && q.p[*i] == '&') (*i)++;
  if (!q.p || *i >= q.n) return false;
  size_t j = *i;
  while (j < q.n && q.p[j] != '&') j++;
  str pair = (str){q.p + *i, j - *i};
  size_t eq = 0;
  while (eq < pair.n && pair.p[eq] != '=') eq++;
  *key = (str){pair.p, eq};
  *val = eq < pair.n ? (str){pair.p + eq + 1, pair.n - eq - 1} : (str){nullptr, 0};
  *i = j;
  return true;
}

// ---------------------------------------------------------------- errors


const char *werr_kind_name(WErrKind k) {
  switch (k) {
  case WEBC_OK:
    return "ok";
  case WEBC_EINVAL:
    return "invalid";
  case WEBC_ENOTFOUND:
    return "not_found";
  case WEBC_EEXIST:
    return "exists";
  case WEBC_EPERM:
    return "permission";
  case WEBC_EIO:
    return "io";
  case WEBC_ETIMEOUT:
    return "timeout";
  case WEBC_ECANCELED:
    return "canceled";
  case WEBC_ECONFLICT:
    return "conflict";
  case WEBC_EUNSUPPORTED:
    return "unsupported";
  case WEBC_ENOMEM:
    return "no_memory";
  case WEBC_EPARSE:
    return "parse";
  case WEBC_EEXTERNAL:
    return "external";
  case WEBC_EDB:
    return "database";
  case WEBC_EVALIDATION:
    return "validation";
  case WEBC_ETOOBIG:
    return "too_big";
  }
  return "unknown";
}

// ---------------------------------------------------------------- unwind state
//
// The cleanup chain and the errdefer counter belong to the *task*: a handler that
// parks keeps them across a worker switch, exactly like the panic frame (see the
// note above FRAME_GET). Without a scheduler, or on wasm, thread-local is right.
static _Thread_local WUnwind t_unwind;

#if !defined(__wasm__)
extern void *wtask_unwind_get(void) __attribute__((weak));
#endif

static WUnwind *unwind_state(void) {
#if defined(__wasm__)
  return &t_unwind;
#else
  WUnwind *u = wtask_unwind_get ? (WUnwind *)wtask_unwind_get() : nullptr;
  return u ? u : &t_unwind;
#endif
}

int *wc_err_pending_ptr(void) {
  return &unwind_state()->err_pending;
}

WCleanupSlot *wc_cleanup_push(void (*fn)(void *), void *arg, bool errdefer) {
  WCleanupSlot *s = wm_alloc(sizeof *s);
  WUnwind *u = unwind_state();
  s->fn = fn;
  s->arg = arg;
  s->errdefer = errdefer;
  s->pending_at_entry = u->err_pending;
  s->prev = u->cleanup;
  u->cleanup = s;
  return s;
}

static void cleanup_run(WCleanupSlot *s) {
  if (!s || !s->fn) return;
  if (s->errdefer) {
    WUnwind *u = unwind_state();
    if (u->err_pending <= s->pending_at_entry) return;
    u->err_pending--;
  }
  s->fn(s->arg);
}

// The normal path: the compiler calls this when the block closes. The slot is
// normally the head; after a nested unwinder ran past it, it may not be.
void wc_cleanup_pop_run(WCleanupSlot **slot) {
  WCleanupSlot *s = *slot;
  if (!s) return;
  WUnwind *u = unwind_state();
  for (WCleanupSlot **pp = &u->cleanup; *pp; pp = &(*pp)->prev)
    if (*pp == s) {
      *pp = s->prev;
      break;
    }
  cleanup_run(s);
  wm_free(s);
  *slot = nullptr;
}

// The panic path: run every slot installed above `mark`, innermost first. A panic
// raised *by* a cleanup is a bug in the cleanup, and recursing would be worse than
// dying: say so and stop.
void wc_cleanup_unwind_to(void *mark) {
  WUnwind *u = unwind_state();
  if (u->unwinding) {
    fprintf(stderr, "webc: panic inside a defer()/errdefer() handler\n");
    fflush(stderr);
    abort();
  }
  u->unwinding = true;
  while (u->cleanup && u->cleanup != (WCleanupSlot *)mark) {
    WCleanupSlot *s = u->cleanup;
    u->cleanup = s->prev;
    cleanup_run(s);
    wm_free(s);
  }
  u->unwinding = false;
}

// ---------------------------------------------------------------- time

u64 wc_wall_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

u64 wc_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000u + (u64)(ts.tv_nsec / 1000000);
}

u64 wc_wall_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (u64)ts.tv_sec * 1000u + (u64)(ts.tv_nsec / 1000000);
}

// Into a caller's buffer: the log path formats a timestamp on every line and must
// not allocate at all.
size_t wc_time_rfc3339_buf(char *out, size_t cap, u64 unix_ms) {
  time_t secs = (time_t)(unix_ms / 1000);
  struct tm tm;
  gmtime_r(&secs, &tm);
  int n =
      snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
               tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(unix_ms % 1000));
  return n > 0 ? (size_t)n : 0;
}

str wc_time_rfc3339(Arena *a, u64 unix_ms) {
  char buf[40];
  size_t n = wc_time_rfc3339_buf(buf, sizeof buf, unix_ms);
  char *p = arena_alloc(a, n + 1);
  memcpy(p, buf, n);
  p[n] = 0;
  return (str){.p = p, .n = n};
}

// ---------------------------------------------------------------- fixed limits

// A ceiling that silently degrades behaviour is the hardest kind of bug to see from
// the outside ("why is this button dead?"). Every fixed limit in the framework reports
// its first crossing here, once per `id` (a static address). The report itself is a
// weak symbol: the wasm build links neither the logger nor stderr.
__attribute__((weak)) void wc_limit_print(const char *msg, const char *limit, const char *what);

#define WC_LIMIT_ONCE_SLOTS 32
static _Atomic(uintptr_t) g_limit_seen[WC_LIMIT_ONCE_SLOTS];

void wc_limit_once(const void *id, const char *msg, const char *limit, const char *what) {
  uintptr_t key = (uintptr_t)id;
  for (i32 i = 0; i < WC_LIMIT_ONCE_SLOTS; i++) {
    uintptr_t slot = atomic_load(&g_limit_seen[i]);
    if (slot == key) return; // already reported
    if (!slot) {
      uintptr_t expected = 0;
      if (atomic_compare_exchange_strong(&g_limit_seen[i], &expected, key) && wc_limit_print)
        wc_limit_print(msg, limit, what);
      return;
    }
  }
}
