// SPDX-License-Identifier: Apache-2.0
// webc — the frontend application layer (SPEC §7.3, §7.4, §13).
//
// Context, resources, the client router, typed forms, portals and the block-shaped
// keyed list. Everything here is closure-free and works in the browser build, so
// it lives in FRONTEND_SRC and links into the wasm module as well as a server.
#include <webc/log.h>
#include <webc/fe.h>
#include <webc/host.h> // webc_later: the yield between chunks of a computed resource
#include <webc/js.h>   // WJs, the type a host callback is called with
#include <webc/dom.h>
#include <webc/relations.h>

// ---------------------------------------------------------------- context

WProv *provide(WProv *parent, const WProvKey *key, void *val) {
  WProv *p = wm_alloc(sizeof *p);
  *p = (WProv){.parent = parent, .key = key, .val = val};
  return p;
}

void *use_ctx_from(WProv *from, const WProvKey *key) {
  for (WProv *p = from; p; p = p->parent)
    if (p->key == key) return p->val;
  wc_panic(__FILE__, __LINE__, "use_ctx: nothing provided for \"%s\"",
           key && key->name ? key->name : "?");
}

void *use_ctx_or(WProv *from, const WProvKey *key, void *dflt) {
  for (WProv *p = from; p; p = p->parent)
    if (p->key == key) return p->val;
  return dflt;
}

WProv *wprov_current(void) {
  WBuild *b = wbuild_current();
  return b ? b->prov : nullptr;
}

WProv *wprov_push(const WProvKey *key, void *val) {
  WBuild *b = wbuild_current();
  if (!b) return provide(nullptr, key, val);
  WProv *p = arena_alloc(b->a, sizeof *p);
  *p = (WProv){.parent = b->prov, .key = key, .val = val};
  b->prov = p;
  return p;
}

// ---------------------------------------------------------------- resource

struct WJob {
  WRes *r;
  WJobFn fn;
  void *ctx;
  u64 key;
  f64 deadline;
  bool done;
  bool cancelled;
  bool invalid;
  bool any_tick; // this chunk has already been allowed one step
  i32 done_steps;
  i32 total_steps;
};

static void job_release(WJob *j);
static i32 g_jobs_alive; // declared early: res_destroy() below needs it

struct WRes {
  WResLoad load;
  void *user;
  void *data;      // elem bytes
  size_t elem;
  str err;         // at most 200 bytes, copied
  Wn state;        // signal: the view depends on it
  Wn payload;      // signal: data generation counter (re-read after a load)
  i32 gen;
  Arena *a;        // resource scratch, null = heap
  char errbuf[200];
  WErrRes result;
  bool ran;     // a load has settled (success or error)
  bool started; // somebody asked for one — res_loading() has to tell these apart
  WJob *job;    // a computed resource's in-flight work (res_compute)
};

static void res_touch(WRes *r) {
  i32 gen = r->ran ? 1 : 0;
  w_write(&r->payload, &gen, sizeof gen);
}

WRes *res_new(Arena *a, WResLoad load, void *user, size_t elem) {
  WRes *r = a ? arena_alloc(a, sizeof *r) : wm_alloc_zero(sizeof *r);
  if (a) memset(r, 0, sizeof *r);
  r->a = a;
  r->load = load;
  r->user = user;
  r->elem = elem ? elem : 1;
  r->data = a ? arena_alloc(a, r->elem) : wm_alloc(r->elem);
  memset(r->data, 0, r->elem);
  w_sig_init(&r->state, &r->result.ok, sizeof r->result.ok, "resource.state");
  w_sig_init(&r->payload, &r->gen, sizeof r->gen, "resource.gen");
  return r;
}

void res_destroy(WRes *r) {
  if (!r) return;
  if (r->job) {
    if (!r->job->done && !r->job->cancelled) g_jobs_alive--;
    job_release(r->job);
    r->job = nullptr;
  }
  // The two signals hold a pointer into this struct, so they go first: leaving them
  // alive left the graph reading freed memory once the resource was dropped.
  w_kill(&r->state);
  w_kill(&r->payload);
  if (!r->a) {
    wm_free(r->data);
    wm_free(r);
  }
}

// The load itself. `ctx` is the request's cancellation context when there is one,
// so a load whose client hung up stops instead of finishing into a dead response.
typedef struct ResJob {
  WRes *r;
  WCtx *ctx;
} ResJob;

static void res_job_free(ResJob **j) { wm_free(*j); }

static void res_job(void *arg) {
  ResJob *j = arg;
  // a loader that panics is caught by the task frame; the box still has to go
  defer(res_job_free, j);
  WErrRes e = j->r->load(j->ctx, j->r->user, j->r->data, j->r->elem);
  res_complete(j->r, e, j->r->data);
}

// Weak hooks: the wasm build has no scheduler and no weak symbols to link
// against, so the guard is a compile-time one instead (WEBC_NO_TASKS is set for
// the frontend target by tools/webc).
#ifndef WEBC_NO_TASKS
__attribute__((weak)) i32 wtask_threads(void);
__attribute__((weak)) void task_async(void (*fn)(void *), void *arg);
__attribute__((weak)) WCtx *webc_req_ctx(void);
#endif

void res_start(WRes *r) {
  if (!r) return;
  // Started before the loader is consulted: a resource with no C loader is completed by the
  // bridge (`res_complete`), and "in flight" has to be true while that fetch is out.
  r->started = true;
  if (!r->load) return;
  if (r->result.ok == false && r->ran) return; // already finished with an error
  if (r->ran && r->result.ok) {
    // restart: a dependency changed
    r->result.ok = false;
    r->ran = false;
  }
#ifndef WEBC_NO_TASKS
  if (wtask_threads && wtask_threads() > 0 && task_async) {
    ResJob *j = wm_alloc(sizeof *j);
    j->r = r;
    j->ctx = webc_req_ctx ? webc_req_ctx() : nullptr;
    // mark loading before the task can finish, so a view that reads the flag in
    // between never sees "idle"
    w_write(&r->state, &r->result.ok, sizeof r->result.ok);
    task_async(res_job, j);
    return;
  }
#endif
  res_run_now(r);
}

void res_run_now(WRes *r) {
  if (!r || !r->load) return;
  r->started = true;
  WErrRes e = r->load(nullptr, r->user, r->data, r->elem);
  res_complete(r, e, nullptr);
}

void res_complete(WRes *r, WErrRes e, const void *data) {
  if (!r) return;
  r->result = e;
  r->ran = true;
  if (!e.ok) {
    size_t n = (size_t)e.e.len < sizeof r->errbuf - 1 ? (size_t)e.e.len : sizeof r->errbuf - 1;
    if (n) memcpy(r->errbuf, e.e.buf, n);
    r->errbuf[n] = 0;
    r->err = (str){r->errbuf, n};
  } else if (data && data != r->data) {
    memcpy(r->data, data, r->elem);
  }
  w_write(&r->state, &r->result.ok, sizeof r->result.ok);
  res_touch(r);
}

// ---------------------------------------------------------------- computed resources
//
// The chunked engine (fe.h). One job per resource, one heap box, no closure: the cursor
// lives in the app's own state, so every resumption re-enters the same function. A single
// timer drives every live job — one `webc_later` slot for the whole framework, armed only
// while something is actually running.
#define WC_JOBS_MAX 8
static WJob *g_jobs[WC_JOBS_MAX];
static f64 g_job_budget = 8.0; // ms of work between yields

void wc_job_budget_ms(f64 ms) { g_job_budget = ms; }
bool wc_jobs_alive(void) { return g_jobs_alive > 0; }

#if defined(__wasm__) || defined(WEBC_JOBS_TIMER)
static bool g_resume_armed;
#endif

static f64 job_now_ms(void) { return (f64)wc_wall_ns() / 1e6; }

static void job_pump(WJob *j);

static void job_schedule(void);

static void job_pump(WJob *j) {
  if (!j || j->done || j->cancelled) return;
  j->deadline = job_now_ms() + g_job_budget;
  j->any_tick = false;
  j->fn(j, j->ctx);
  if (j->cancelled || j->done) return;
  job_schedule(); // not finished: come back for the next chunk
}

#if defined(__wasm__) || defined(WEBC_JOBS_TIMER)
static void job_resume(WJs r) {
  (void)r;
  g_resume_armed = false;
  for (i32 i = 0; i < WC_JOBS_MAX; i++)
    if (g_jobs[i] && !g_jobs[i]->done && !g_jobs[i]->cancelled) {
      job_pump(g_jobs[i]);
      if (g_jobs[i] && g_jobs[i]->done) return; // the rest resume on this tick too
    }
}
#endif

static void job_schedule(void) {
#if defined(__wasm__) || defined(WEBC_JOBS_TIMER)
  if (g_resume_armed) return;
  g_resume_armed = true;
  webc_later(0, job_resume); // a host callback: the view re-renders between chunks (§11a)
#else
  // A native build has no browser to yield to. The work finishes now, through the same
  // pump, so a test exercises the chunk loop instead of a special case that only exists
  // in a browser.
  for (i32 i = 0; i < WC_JOBS_MAX; i++)
    while (g_jobs[i] && !g_jobs[i]->done && !g_jobs[i]->cancelled) job_pump(g_jobs[i]);
#endif
}

static WJob *job_new(WRes *r, WJobFn fn, void *ctx, u64 key) {
  WJob *j = wm_alloc_zero(sizeof *j);
  if (!j) return nullptr;
  j->r = r;
  j->fn = fn;
  j->ctx = ctx;
  j->key = key;
  for (i32 i = 0; i < WC_JOBS_MAX; i++)
    if (!g_jobs[i]) {
      g_jobs[i] = j;
      break;
    }
  g_jobs_alive++;
  return j;
}

static void job_release(WJob *j) {
  for (i32 i = 0; i < WC_JOBS_MAX; i++)
    if (g_jobs[i] == j) g_jobs[i] = nullptr;
  wm_free(j);
}

// The work's voice. `job_tick` is the only thing the app calls in a loop; everything else
// is bookkeeping the view reads.
bool job_tick(WJob *j) {
  if (!j || j->done || j->cancelled) return false;
  // Every chunk gets at least one step, whatever the budget says. Without this a budget of
  // 0 (or a clock too coarse to see it) makes the work return without moving, the pump
  // reschedules it, and the page spins on timers forever — a hang that looks like a slow
  // page. With it, "0 ms" means "one step per chunk", which is exactly what a test wants.
  if (!j->any_tick) {
    j->any_tick = true;
    return true;
  }
  return job_now_ms() < j->deadline;
}

void job_done(WJob *j) {
  if (!j || j->done) return;
  j->done = true;
  j->total_steps = j->total_steps > 0 ? j->total_steps : 1;
  j->done_steps = j->total_steps;
  g_jobs_alive--;
  if (j->r) {
    WRes *r = j->r;
    r->result = (WErrRes){.ok = true};
    r->ran = true;
    w_write(&r->state, &r->result.ok, sizeof r->result.ok);
    res_touch(r);
    j->r = nullptr;
  }
}

void job_steps(WJob *j, i32 done, i32 total) {
  if (!j) return;
  j->done_steps = done;
  j->total_steps = total;
}

void *job_out(WJob *j) { return (j && j->r) ? j->r->data : nullptr; }
bool job_cancelled(WJob *j) { return !j || j->cancelled; }

i32 res_progress(WRes *r) {
  if (!r || !r->job) return r && r->ran ? 100 : 0;
  WJob *j = r->job;
  if (j->done) return 100;
  if (j->total_steps <= 0) return 0;
  i32 pct = (i32)((i64)j->done_steps * 100 / j->total_steps);
  return pct < 0 ? 0 : (pct > 99 ? 99 : pct);
}

WRes *res_compute(Arena *a, WJobFn fn, void *ctx, size_t elem) {
  WRes *r = res_new(a, nullptr, ctx, elem);
  if (!r) return nullptr;
  r->job = job_new(r, fn, ctx, 0);
  return r;
}

void res_invalidate(WRes *r) {
  if (!r || !r->job) return;
  r->job->invalid = true;   // the next res_ensure_key() runs again, whatever the key
  r->job->cancelled = true; // a chunk already running stops at its next job_tick()
  r->started = false;
  r->ran = false;
  // The signal the view reads, not just the flag: `res_ok()` asks the *published* value,
  // and a caller that invalidated must stop seeing the old answer immediately.
  r->result = (WErrRes){0};
  r->gen++;
  w_write(&r->state, &r->result.ok, sizeof r->result.ok);
  res_touch(r);
}

void res_ensure_key(WRes *r, u64 key) {
  if (!r || !r->job) return;
  WJob *j = r->job;
  if (!j->invalid && j->key == key && !j->cancelled) return; // running, or done, for this key
  bool was_live = !j->done && !j->cancelled;
  j->invalid = false;
  j->cancelled = false;
  j->done = false;
  j->key = key;
  j->done_steps = 0;
  j->total_steps = 0;
  j->r = r;
  if (!was_live) g_jobs_alive++;
  // Zeroed data: a half-finished run must not be mistaken for an answer, and every run
  // starts from "nothing computed" rather than from the last one's leftovers.
  if (r->data) memset(r->data, 0, r->elem);
  r->started = true;
  r->ran = false;
  r->result = (WErrRes){0};
  w_write(&r->state, &r->result.ok, sizeof r->result.ok);
  job_pump(j);
}

bool res_ok(WRes *r) {
  if (!r) return false;
  bool ok = *(bool *)w_read(&r->payload) ? r->result.ok : false;
  return ok;
}

bool res_failed(WRes *r) {
  if (!r) return false;
  return *(bool *)w_read(&r->payload) && !r->result.ok;
}

// "In flight" — which is not the same as "nobody asked". Answered from `!ran` alone, a
// resource nobody had started reports loading forever, and the natural guard
//
//   if (res_ok(r) || res_loading(r)) return;   // don't fetch twice
//
// made the fetch never happen: the panel sat at "Loading…" with no error and no request.
// A resource that is only started on demand needs three states, so it has them.
bool res_loading(WRes *r) {
  if (!r) return false;
  if (!r->started) return false;
  bool ran = *(bool *)w_read(&r->payload);
  return !ran;
}

// Nobody has asked yet: the state a resource is in before the first res_start().
bool res_idle(WRes *r) {
  if (!r) return false;
  return !r->started;
}

// The two lines every guarded fetch writes, in one call: start it if nobody has, do nothing
// if it is in flight or done.
void res_ensure(WRes *r) {
  if (!r || r->started) return;
  res_start(r);
}

WResState res_state(WRes *r) {
  if (!r) return WRES_IDLE;
  if (!r->started) return WRES_IDLE;
  if (!r->ran) return WRES_LOADING;
  return r->result.ok ? WRES_OK : WRES_ERR;
}

str res_err_str(WRes *r) {
  return r && res_failed(r) ? r->err : (str){0};
}

void *res_data(WRes *r) {
  return res_ok(r) ? r->data : nullptr;
}

// ---------------------------------------------------------------- router

#define WROUTE_MAX 64

typedef struct WPageReg {
  char *pattern;
  WPageFn page;
} WPageReg;

static WPageReg g_pages[WROUTE_MAX];
static i32 g_npages;

void front_route_add(const char *pattern, WPageFn page) {
  if (g_npages >= WROUTE_MAX) {
    wc_limit_once(&g_npages, "client route limit reached: this route is not registered",
                  "WROUTE_MAX", "route");
    return;
  }
  if (!pattern || !page) return;
  size_t n = strlen(pattern);
  char *copy = wm_alloc(n + 1);
  memcpy(copy, pattern, n + 1);
  g_pages[g_npages++] = (WPageReg){.pattern = copy, .page = page};
}

i32 front_route_count(void) {
  return g_npages;
}

// One segment at a time, so `/todos/:id` and `/todos/new` can both exist and the
// literal wins — the same rule the server router applies (§6.3).
static bool seg_is_param(const char *p, size_t n, str *name) {
  if (n >= 2 && p[0] == ':') {
    *name = (str){(char *)p + 1, n - 1};
    return true;
  }
  return false;
}

static bool match_pattern(const char *pat, str path, WPageMatch *m) {
  m->nparams = 0;
  size_t pi = 0, si = 0;
  size_t plen = strlen(pat);
  while (pi < plen) {
    if (pat[pi] == '*') { // wildcard: the rest of the path
      str rest = (str){path.p + si, path.n - si};
      if (m->nparams < WROUTE_MAX_PARAMS) {
        m->keys[m->nparams] = (str){(char *)pat + pi + 1, plen - pi - 1};
        m->vals[m->nparams] = rest;
        m->nparams++;
      }
      return true;
    }
    size_t pe = pi;
    while (pe < plen && pat[pe] != '/') pe++;
    str pseg = (str){(char *)pat + pi, pe - pi};
    size_t se = si;
    while (se < path.n && path.p[se] != '/') se++;
    str sseg = (str){path.p + si, se - si};
    str pname = {0};
    if (seg_is_param(pseg.p, pseg.n, &pname)) {
      if (sseg.n == 0) return false;
      if (m->nparams < WROUTE_MAX_PARAMS) {
        m->keys[m->nparams] = pname;
        m->vals[m->nparams] = sseg;
        m->nparams++;
      }
    } else if (!str_eq(pseg, sseg)) {
      return false;
    }
    pi = pe;
    si = se;
    if (pi < plen && pat[pi] == '/') pi++;
    if (si < path.n && path.p[si] == '/') si++;
    else if (pi < plen) return false;
  }
  return si >= path.n;
}

WPageMatch front_route(str path) {
  WPageMatch best = {0};
  if (path.n && path.p[0] == '/') {
    path.p++;
    path.n--;
  }
  // the root is the empty path
  for (i32 pass = 0; pass < 2; pass++) {
    for (i32 i = 0; i < g_npages; i++) {
      WPageMatch m = {0};
      const char *pat = g_pages[i].pattern;
      size_t plen = strlen(pat);
      if (plen && pat[0] == '/') {
        pat++;
        plen--;
      }
      bool needs = strchr(pat, ':') != nullptr || strchr(pat, '*') != nullptr;
      if ((pass == 0) == needs) continue; // literals first, then patterns
      if (match_pattern(pat, path, &m)) {
        m.found = true;
        m.page = g_pages[i].page;
        m.pattern = g_pages[i].pattern;
        return m;
      }
    }
  }
  return best;
}

str front_route_param(WPageMatch *m, const char *name) {
  if (!m || !name) return (str){0};
  for (i32 i = 0; i < m->nparams; i++)
    if (m->keys[i].n == strlen(name) && memcmp(m->keys[i].p, name, m->keys[i].n) == 0)
      return m->vals[i];
  return (str){0};
}

// ---------------------------------------------------------------- forms

void form_init(WForm *f, const WModel *m, void *obj) {
  if (!f) return;
  *f = (WForm){.m = m, .obj = obj};
}

bool form_submit(WForm *f) {
  if (!f || !f->m || !f->obj) return false;
  f->submitted = true;
  memset(&f->errs, 0, sizeof f->errs);
  return model_validate(f->m, f->obj, &f->errs);
}

str form_error(WForm *f, const char *field) {
  if (!f || !field) return (str){0};
  size_t n = strlen(field);
  for (i32 i = 0; i < f->errs.n; i++)
    if (f->errs.items[i].field.n == n && memcmp(f->errs.items[i].field.p, field, n) == 0)
      return f->errs.items[i].msg;
  return (str){0};
}

bool form_field_ok(WForm *f, const char *field) {
  return form_error(f, field).n == 0;
}

i32 form_error_count(WForm *f) {
  return f ? f->errs.n : 0;
}

void form_reset(WForm *f) {
  if (!f) return;
  memset(&f->errs, 0, sizeof f->errs);
  f->submitted = false;
}

// ---------------------------------------------------------------- portals

WDom *wc_portal_open(const char *selector) {
  WDom *n = wc_el_open("#portal");
  if (n && selector) wc_attr_c("to", selector);
  return n;
}

// ---------------------------------------------------------------- stores

bool store_init(WStore *s, const WDoc *d, u32 size) {
  if (!s || !d || !size) return false;
  // The seed arrives BEFORE client_main runs (the boot order), so an init that
  // had already been called from a previous hot swap must not wipe it: an app
  // that calls store_init in client_main every start would throw the seed away
  // and mount an empty room — found as a whole-tree hydration failure.
  if (s->a[0] && s->doc == d && s->size == size) return true;
  if (s->a[0]) store_free(s); // a re-init with a different document is a new store
  s->a[0] = arena_new_labeled(96 * 1024, "store-gen");
  s->a[1] = arena_new_labeled(96 * 1024, "store-gen");
  s->docs[0] = wm_alloc(size);
  s->docs[1] = wm_alloc(size);
  if (!s->a[0] || !s->a[1] || !s->docs[0] || !s->docs[1]) return false;
  s->doc = d;
  s->size = size;
  s->cur = -1;
  return true;
}

void *store_read(WStore *s, str json) {
  if (!s || !s->doc || s->cur < -1) return nullptr;
  i32 next = s->cur < 0 ? 0 : s->cur ^ 1;
  arena_reset(s->a[next]); // the generation two frames ago is dead; this one is fresh
  memset(s->docs[next], 0, s->size);
  if (!doc_read(s->a[next], s->doc, json, s->docs[next])) return nullptr;
  s->cur = next;
  // the rows now live in this generation; ref() reads must find them even though
  // the render's scratch arena is the current one
  wc_ref_epoch(s->a[next]);
  return s->docs[next];
}

void *store_cur(WStore *s) {
  return s && s->cur >= 0 ? s->docs[s->cur] : nullptr;
}

Arena *store_arena(WStore *s) {
  return s && s->cur >= 0 ? s->a[s->cur] : nullptr;
}

void store_free(WStore *s) {
  if (!s) return;
  for (i32 i = 0; i < 2; i++) {
    if (s->a[i]) arena_destroy(s->a[i]);
    if (s->docs[i]) wm_free(s->docs[i]);
    s->a[i] = nullptr;
    s->docs[i] = nullptr;
  }
  s->cur = -1;
}
