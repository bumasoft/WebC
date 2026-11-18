// SPDX-License-Identifier: Apache-2.0
// WebC — reactive graph runtime: dynamic dependencies, topological flush, scopes.
#include <webc/react.h>

// The runtime in use belongs to the *task*, not the OS thread: a task that parks (a db
// call, a fd wait) resumes on another worker, and the effect it queues must still be
// flushed by the graph it belongs to. A thread-local runtime meant an effect queued in
// graph X could be flushed out of graph Y — i.e. silently skipped, or run on the wrong
// thread. src/task.c keeps the slot beside the markup build and the arena; the
// thread-local below is the fallback for the wasm target (no scheduler), for startup
// and for threads the app made itself.
static _Thread_local Wrt *g_rt;

// Weak: src/task.c is not part of the wasm build, where there are no tasks.
extern void *wtask_rt_get(void) __attribute__((weak));
extern void wtask_rt_set(void *rt) __attribute__((weak));

// The client half, defined in src/dom.c. A mount hook is a document gesture and a server
// render has no document, so `on_mount` queues nothing there.
bool wc_in_client(void);

static Wrt *task_rt(void) { return wtask_rt_get ? (Wrt *)wtask_rt_get() : nullptr; }

Wrt *wrt_new(Arena *a) {
  Wrt *rt = a ? arena_zalloc(a, sizeof *rt) : wm_alloc(sizeof *rt);
  rt->arena = a;
  return rt;
}

void wrt_destroy(Wrt *rt) {
  if (!rt) return;
  wc_mount_clear(); // hooks whose batch never went out die with the runtime
  if (!rt->arena) {
    for (i32 i = 0; i < rt->nq; i++) { /* queue holds borrowed nodes */
    }
    wm_free(rt->queue);
    wm_free(rt->insts);
    wm_free(rt->owns);
    wm_free(rt);
  }
}

Wrt *wrt_current(void) {
  Wrt *t = task_rt();
  return t ? t : g_rt;
}

Wrt *wrt_bind(Wrt *rt) {
  Wrt *prev = wrt_current();
  if (wtask_rt_set)
    wtask_rt_set(rt);
  else
    g_rt = rt;
  return prev;
}

void wrt_unbind(Wrt *prev) {
  if (wtask_rt_set)
    wtask_rt_set(prev);
  else
    g_rt = prev;
}

Wrt *wrt_default(void) {
  Wrt *t = task_rt();
  if (t) return t;
  if (!g_rt) g_rt = wrt_new(nullptr);
  if (wtask_rt_set) wtask_rt_set(g_rt); // the task carries it from here on
  return g_rt;
}

static Wrt *rt_of(Wn *n) {
  return n->rt ? n->rt : wrt_default();
}

// ---------------------------------------------------------------- allocation

static void *rt_alloc(Wrt *rt, size_t n) {
  return rt && rt->arena ? arena_alloc(rt->arena, n) : wm_alloc(n);
}

static Wn **grow_ptr_array(Wrt *rt, Wn **old, i32 old_cap, i32 new_cap) {
  // arena-backed graphs cannot free; they copy forward (bounded lifetime by design)
  Wn **np = rt_alloc(rt, (size_t)new_cap * sizeof(Wn *));
  if (old && old_cap) memcpy(np, old, (size_t)old_cap * sizeof(Wn *));
  if (!rt || !rt->arena) wm_free(old);
  return np;
}

// A killed node is not dead weight: the struct, its dependency array and its seen array
// are recycled rather than left allocated for the life of the runtime, because a list that
// mounts and disposes an effect per row leaks ~200 B per row per render (measured) when
// they are not — see w_recycle() for when reuse is safe.
static void w_recycle(Wn *n);

// ---------------------------------------------------------------- cleanups
//
// `on_cleanup` registers a function with whatever is running: an effect run (the list hangs
// off the node, so it is emptied before the next run and when the node dies) or an instance
// (the component, mount or row it was called in). A singly-linked list in reverse order, so
// teardown is LIFO, which is what nesting expects.
struct WCl {
  void (*fn)(void *);
  void *ctx;
  WCl *next;
};

static void cl_push(WCl **head, void (*fn)(void *), void *ctx) {
  if (!fn) return;
  WCl *c = wm_alloc(sizeof *c);
  if (!c) return;
  c->fn = fn;
  c->ctx = ctx;
  c->next = *head;
  *head = c;
}

static void cl_run(WCl **head) {
  while (*head) {
    WCl *c = *head;
    *head = c->next;
    if (c->fn) c->fn(c->ctx);
    wm_free(c);
  }
}

// Defined with the lifecycle block below; a torn-down instance has to cancel the mount hooks
// it queued, and the teardown path is reached from several places before that block.
static void mnt_cancel(WInst *in);

Wn *w_new(Wrt *rt, WNodeKind kind, const char *name) {
  if (!rt) rt = wrt_default();
  Wn *n = nullptr;
  if (rt->free_list) {
    n = rt->free_list;
    rt->free_list = n->next_free;
    rt->nfree--;
    memset(n, 0, sizeof *n);
  } else {
    n = rt_alloc(rt, sizeof *n);
  }
  n->kind = kind;
  n->name = name;
  n->alive = true;
  n->rt = rt;
  n->inst = nullptr;
  rt->nodes++;
  return n;
}

// ---------------------------------------------------------------- linking

static void obs_push(Wn *src, Wn *o) {
  Wrt *rt = rt_of(src);
  for (i32 i = 0; i < src->nobs; i++)
    if (src->obs[i] == o) return;
  if (src->nobs == src->cap_obs) {
    i32 ncap = src->cap_obs ? src->cap_obs * 2 : 4;
    src->obs = grow_ptr_array(rt, src->obs, src->cap_obs, ncap);
    src->cap_obs = ncap;
  }
  src->obs[src->nobs++] = o;
}

// Drop `src` from o's dependency list. A node that is killed can be recycled, and a
// recycled node that some effect still lists as a source would look alive again — the
// effect would then read the wrong node's version and skip a run it needed.
static void src_del(Wn *o, Wn *src) {
  for (i32 i = 0; i < o->nsrc; i++) {
    if (o->src[i] != src) continue;
    i32 last = --o->nsrc;
    o->src[i] = o->src[last];
    if (o->seen) o->seen[i] = o->seen[last];
    return;
  }
}

static void obs_del(Wn *src, Wn *o) {
  for (i32 i = 0; i < src->nobs; i++) {
    if (src->obs[i] != o) continue;
    src->obs[i] = src->obs[--src->nobs];
    return;
  }
}

static void unlink_all(Wn *n) {
  for (i32 i = 0; i < n->nsrc; i++)
    obs_del(n->src[i], n);
  n->nsrc = 0;
}

static void link_dep(Wn *src) {
  Wrt *rt = wrt_current();
  Wn *cur = rt ? rt->cur : nullptr;
  if (!cur || cur == src || !src->alive) return;
  for (i32 i = 0; i < cur->nsrc; i++)
    if (cur->src[i] == src) return;
  if (cur->nsrc == cur->cap_src) {
    i32 ncap = cur->cap_src ? cur->cap_src * 2 : 4;
    cur->src = grow_ptr_array(rt, cur->src, cur->cap_src, ncap);
    i32 *seen = rt_alloc(rt, (size_t)ncap * sizeof(i32));
    if (cur->seen && cur->cap_src) memcpy(seen, cur->seen, (size_t)cur->cap_src * sizeof(i32));
    if (!rt->arena) wm_free(cur->seen);
    cur->seen = seen;
    cur->cap_src = ncap;
  }
  cur->src[cur->nsrc] = src;
  cur->seen[cur->nsrc] = src->ver;
  cur->nsrc++;
  if (src->height + 1 > cur->height) cur->height = src->height + 1;
  obs_push(src, cur);
}

void *w_read(Wn *n) {
  if (!n) return nullptr;
  if (!n->alive) {
    if (n->disposed) return n->val; // disposed reads are inert, not fatal
    wc_panic(__FILE__, __LINE__, "read of uninitialised reactive node '%s'",
             n->name ? n->name : "?");
  }
  link_dep(n);
  return n->val;
}

// ---------------------------------------------------------------- queue

static void q_push(Wn *e) {
  if (e->queued) return;
  Wrt *rt = rt_of(e);
  e->queued = true;
  e->dirty = true;
  if (rt->nq == rt->capq) {
    i32 ncap = rt->capq ? rt->capq * 2 : 16;
    Wn **nq = rt_alloc(rt, (size_t)ncap * sizeof(Wn *));
    if (rt->queue && rt->nq) memcpy(nq, rt->queue, (size_t)rt->nq * sizeof(Wn *));
    if (!rt->arena) wm_free(rt->queue);
    rt->queue = nq;
    rt->capq = ncap;
  }
  // insert keeping ascending height (small n: insertion sort is the right tool)
  i32 i = rt->nq++;
  while (i > 0 && rt->queue[i - 1]->height > e->height) {
    rt->queue[i] = rt->queue[i - 1];
    i--;
  }
  rt->queue[i] = e;
}

static Wn *q_pop(Wrt *rt) {
  if (!rt || rt->nq == 0) return nullptr;
  Wn *e = rt->queue[0];
  memmove(rt->queue, rt->queue + 1, (size_t)(rt->nq - 1) * sizeof(Wn *));
  rt->nq--;
  e->queued = false;
  return e;
}

static void flush_rt(Wrt *rt);
static void w_recycle(Wn *n);

static void propagate(Wn *n) {
  for (i32 i = 0; i < n->nobs; i++) {
    Wn *o = n->obs[i];
    if (!o->alive) continue;
    if (o->kind == W_EFFECT) {
      q_push(o);
      continue;
    }
    if (o->dirty) continue;
    o->dirty = true;
    propagate(o);
  }
}

// ---------------------------------------------------------------- memo / effect

void w_memo_pull(Wn *n) {
  if (!n) return;
  if (n->kind != W_MEMO) return;
  if (!n->dirty) return;
  Wrt *rt = rt_of(n);
  if (rt->cur == n) return; // self-reference: keep the previous value
  if (!n->val) {
    // a memo without a caller-provided slot owns its storage
    n->val = rt_alloc(rt, n->vlen ? n->vlen : 8);
  }
  if (!n->prev && n->vlen && n->eq_bytes) n->prev = rt_alloc(rt, n->vlen);
  if (n->prev && n->vlen) memcpy(n->prev, n->val, n->vlen);
  Wn *prev_cur = rt->cur;
  rt->cur = n;
  unlink_all(n);
  n->dirty = false;
  n->fn(n, n->ctx);
  rt->cur = prev_cur;
  n->runs++;
  rt->runs++;
  if (n->eq_bytes && n->prev && n->vlen && memcmp(n->prev, n->val, n->vlen) != 0)
    n->ver++;
  else if (!n->eq_bytes)
    n->ver++;
}

static bool effect_should_run(Wn *e) {
  if (e->nsrc == 0) return true; // first run
  bool changed = false;
  for (i32 i = 0; i < e->nsrc; i++) {
    Wn *s = e->src[i];
    if (!s->alive) continue;
    if (s->kind == W_MEMO) w_memo_pull(s);
    if (e->seen[i] != s->ver) changed = true;
  }
  return changed;
}

static void run_effect(Wn *e) {
  Wrt *rt = rt_of(e);
  // A run's cleanups belong to the run that is ending: a subscription re-armed by the body
  // must not be opened twice, and a timer must not be armed twice because a signal moved.
  cl_run(&e->cl);
  Wn *prev_cur = rt->cur;
  rt->cur = e;
  unlink_all(e);
  e->dirty = false;
  e->fn(e, e->ctx);
  rt->cur = prev_cur;
  for (i32 i = 0; i < e->nsrc; i++)
    e->seen[i] = e->src[i]->ver;
  e->runs++;
  rt->runs++;
}

void w_flush(void) { flush_rt(wrt_default()); }

// A binding (dom.h `text_live`) is an effect whose whole job is one DOM op. It is made
// clean and run once on the spot: `w_effect_init` would queue it, and attaching a
// binding in the middle of a render must not push somebody else's effect into a flush.
Wn *w_effect_new(WnFn fn, void *ctx, const char *name) {
  Wn *e = w_new(wrt_default(), W_EFFECT, name ? name : "bind");
  e->fn = fn;
  e->ctx = ctx;
  e->dirty = false;
  return e;
}

void w_effect_run(Wn *e) {
  if (!e || !e->alive || e->disposed) return;
  run_effect(e);
}

static void flush_rt(Wrt *rt) {
  if (!rt) return;
  if (rt->batch_depth > 0) return;
  if (rt->in_flush) return;
  rt->in_flush = true;
  i32 guard = 0;
  for (;;) {
    Wn *e = q_pop(rt);
    if (!e) break;
    if (!e->alive || e->disposed) {
      // a disposed effect that was still queued: nothing points at it any more, so
      // hand its memory back now instead of at the end of the process
      w_recycle(e);
      continue;
    }
    if (!effect_should_run(e)) continue;
    run_effect(e);
    if (++guard > 100000) {
      rt->in_flush = false;
      wc_panic(__FILE__, __LINE__, "effect loop: 100000 runs in one flush");
    }
  }
  rt->flushes++;
  rt->in_flush = false;
}

void w_batch_begin(void) { wrt_default()->batch_depth++; }

void w_batch_end(void) {
  Wrt *rt = wrt_default();
  if (rt->batch_depth > 0) rt->batch_depth--;
  if (rt->batch_depth == 0) flush_rt(rt);
}

// ---------------------------------------------------------------- node init

void w_sig_init(Wn *n, void *slot, size_t len, const char *name) {
  if (!n->rt) n->rt = wrt_default();
  n->kind = W_SIGNAL;
  n->val = slot;
  n->vlen = len;
  n->alive = true;
  n->name = name;
}

void w_memo_init(Wn *n, WnFn fn, void *ctx, void *out, size_t len, const char *name) {
  if (!n->rt) n->rt = wrt_default();
  n->kind = W_MEMO;
  if (!n->alive) {
    n->val = out;
    n->vlen = len;
    n->alive = true;
    n->dirty = true;
    n->eq_bytes = true;
    n->name = name;
  }
  if (out && !n->val) n->val = out;
  if (len && !n->vlen) n->vlen = len;
  n->fn = fn;
  n->ctx = ctx;
}

void w_effect_init(Wn *e, WnFn fn, void *ctx, const char *name) {
  if (!e->rt) e->rt = wrt_default();
  if (!e->alive) {
    e->kind = W_EFFECT;
    e->alive = true;
    e->dirty = true;
    e->name = name;
  }
  e->fn = fn;
  e->ctx = ctx;
  q_push(e); // runs at the next flush point, never mid-construction
}

void w_write(Wn *n, const void *v, size_t len) {
  if (!n || !n->alive) return;
  if (len && n->vlen && len != n->vlen) len = n->vlen;
  if (n->val && len && memcmp(n->val, v, len) == 0) return; // no-op writes never notify
  if (n->val && len) memcpy(n->val, v, len);
  n->ver++;
  propagate(n);
  // flush the graph this node belongs to: the caller's runtime is not necessarily the
  // node's (a signal declared at file scope lives in the default graph; a request runs
  // in its own), and flushing the wrong one drops the effect silently
  flush_rt(rt_of(n));
}

void w_kill(Wn *n) {
  if (!n || !n->alive) return;
  // before anything is unlinked: a cleanup is allowed to touch the graph (and does, when it
  // closes a subscription whose handler wrote a signal), so it runs while the node is intact
  cl_run(&n->cl);
  unlink_all(n);
  n->alive = false;
  n->disposed = true;
  for (i32 i = 0; i < n->nobs; i++) {
    Wn *o = n->obs[i];
    src_del(o, n);
    o->dirty = true;
    if (o->kind == W_EFFECT) q_push(o);
  }
  n->nobs = 0;
  n->owner = nullptr; // whoever owned it may still list it; dispose() checks this
  w_recycle(n);
}

// Only a node nothing points at may be handed out again: no observers, no sources,
// not sitting in the flush queue, and only in a heap-backed graph (an arena graph
// frees everything at once by design).
static void w_recycle(Wn *n) {
  Wrt *rt = n->rt;
  if (!rt || rt->arena) return;
  if (n->nsrc || n->nobs || n->queued || n->owner) return;
  wm_free(n->src);
  wm_free(n->seen);
  wm_free(n->obs);
  // a killed node may still hold a cleanup list that was pushed but never run (a node that
  // died in the same render it was created: on_mount, then the row left)
  cl_run(&n->cl);
  // a memo's previous-value image is as much an allocation as its dependency list, and
  // it was the one array recycling forgot (4 B per recycled memo, measured)
  if (n->prev && n->prev != n->val) wm_free(n->prev);
  n->prev = nullptr;
  n->src = n->obs = nullptr;
  n->seen = nullptr;
  n->cap_src = n->cap_obs = 0;
  n->next_free = rt->free_list;
  rt->free_list = n;
  rt->nfree++;
  if (rt->nodes > 0) rt->nodes--;
}

i32 w_node_count(Wrt *rt) {
  return rt ? rt->nodes : 0;
}

// ---------------------------------------------------------------- instances
//
// A declaration (`signal_in`) is a *key*: `WDecl` is a static, so its address is a
// stable identity that costs one pointer comparison to look up. The storage is in the
// instance, keyed by that address — which is why two instances of the same view share
// nothing, and why a declaration inside a header included by two files still finds its
// own slot in each.

static void *inst_alloc(WInst *in, size_t n) {
  return in->arena ? arena_alloc(in->arena, n) : wm_alloc(n);
}

// A declaration is matched by *name*, not by address. `state_in(...)` written in a
// shared header compiles to one `static WDecl` per translation unit, and matching by
// address gave the view and the action two different slots in the same instance —
// the same "one signal per file" trap `state()` has, which is exactly what the `_in`
// family exists to fix. Two declarations with the same name are the same state.
static WSlot *slot_find(WInst *in, const WDecl *d) {
  for (i32 i = 0; i < in->n; i++) {
    const WDecl *have = in->slots[i].decl;
    if (have == d) return &in->slots[i];
    if (have && d && have->name && d->name && strcmp(have->name, d->name) == 0 &&
        have->kind == d->kind)
      return &in->slots[i];
  }
  return nullptr;
}

void winst_init(WInst *in, Arena *a) {
  if (!in) return;
  memset(in, 0, sizeof *in);
  in->arena = a;
  in->live = true;
}

Wn *winst_node(WInst *in, const WDecl *d) {
  if (!in || !d) return nullptr;
  WSlot *s = slot_find(in, d);
  if (s) return s->node;
  if (in->n == in->cap) {
    i32 ncap = in->cap ? in->cap * 2 : 4;
    WSlot *ns = inst_alloc(in, (size_t)ncap * sizeof *ns);
    if (in->slots) memcpy(ns, in->slots, (size_t)in->n * sizeof *ns);
    if (!in->arena) wm_free(in->slots);
    in->slots = ns;
    in->cap = ncap;
  }
  s = &in->slots[in->n++];
  *s = (WSlot){.decl = d};
  Wn *n = w_new(wrt_current(), d->kind, d->name);
  n->inst = in;
  s->node = n;
  if (d->kind == W_SIGNAL) {
    void *val = inst_alloc(in, d->size ? d->size : 1);
    if (d->init && d->size) memcpy(val, d->init, d->size);
    s->val = val;
    w_sig_init(n, val, d->size, d->name);
  } else if (d->kind == W_MEMO) {
    void *val = inst_alloc(in, d->size ? d->size : 1);
    s->val = val;
    w_memo_init(n, d->fn, in, val, d->size, d->name);
    // w_memo_init() only marks a *fresh* (alive == false) node dirty, and w_new() has
    // just made this one alive: without this the memo would answer with zeroes forever.
    n->dirty = true;
    n->eq_bytes = true;
    n->val = val;
    n->vlen = d->size;
  } else {
    w_effect_init(n, d->fn, in, d->name);
  }
  return n;
}

void *winst_value(WInst *in, const WDecl *d) {
  Wn *n = winst_node(in, d);
  return n ? n->val : nullptr;
}

void winst_effect(WInst *in, const WDecl *d) { (void)winst_node(in, d); }

// Killing a node can queue an *effect* that observes it (propagate says "this source
// changed"), and a queued node cannot be recycled. Draining the dead entries here is
// what makes an instance's nodes come back immediately rather than at the next flush —
// which may never come, because the instance is gone.
static void drain_dead_queue(Wrt *rt) {
  if (!rt) return;
  for (i32 i = 0; i < rt->nq;) {
    Wn *e = rt->queue[i];
    if (e->alive) {
      i++;
      continue;
    }
    rt->queue[i] = rt->queue[--rt->nq];
    e->queued = false;
    w_recycle(e);
  }
}

void winst_clear(WInst *in) {
  if (!in) return;
  // A queued on_mount belongs to a batch that has not gone out yet; this position will not be
  // on the page by then, so the hook is cancelled rather than left to touch a dead node.
  mnt_cancel(in);
  // Children first: a nested component's cleanups may still read the outer one's state, so
  // the inner instance must be torn down while the outer is still intact.
  if (in->kids) {
    for (i32 i = 0; i < in->capkids; i++) {
      WInstChild *e = &in->kids[i];
      if (!e->live || !e->in) continue;
      winst_release(e->in); // recursive: clear, then free the struct
      e->in = nullptr;
    }
    wm_free(in->kids);
    in->kids = nullptr;
    in->nkids = in->capkids = 0;
  }
  cl_run(&in->cl);
  in->live = false;
  Wrt *rt = nullptr;
  for (i32 i = 0; i < in->n; i++) {
    Wn *n = in->slots[i].node;
    if (n) {
      rt = rt_of(n);
      n->inst = nullptr; // a killed node must not keep pointing at a dead instance
      w_kill(n);
    }
    // the value buffer is the instance's, not the node's: w_kill() would not free it
    if (!in->arena) wm_free(in->slots[i].val);
  }
  drain_dead_queue(rt);
  if (!in->arena) wm_free(in->slots);
  memset(in, 0, sizeof *in);
}

void winst_free(WInst *in) { winst_clear(in); }

i32 winst_count(WInst *in) { return in ? in->n : 0; }

WInst *winst_push(WInst *in) {
  Wrt *rt = wrt_default();
  if (rt->ninsts == rt->capinsts) {
    i32 ncap = rt->capinsts ? rt->capinsts * 2 : 8;
    WInst **ns = wm_alloc((size_t)ncap * sizeof *ns);
    if (rt->ninsts) memcpy(ns, rt->insts, (size_t)rt->ninsts * sizeof *ns);
    wm_free(rt->insts);
    rt->insts = ns;
    rt->capinsts = ncap;
  }
  WInst *prev = rt->ninsts ? rt->insts[rt->ninsts - 1] : nullptr;
  rt->insts[rt->ninsts++] = in;
  return prev;
}

void winst_pop(void) {
  Wrt *rt = wrt_current();
  if (rt && rt->ninsts > 0) rt->ninsts--;
}

WInst *winst_current(void) {
  Wrt *rt = wrt_current();
  return rt && rt->ninsts > 0 ? rt->insts[rt->ninsts - 1] : nullptr;
}

// The unwinder's view of the ambient stack (declared weak in core.h): a panic longjmps
// past the `cleanup` attribute that would have popped the instance.
i32 wc_inst_depth(void) {
  Wrt *rt = wrt_current();
  return rt ? rt->ninsts : 0;
}

void wc_inst_restore(i32 depth) {
  Wrt *rt = wrt_current();
  if (!rt) return;
  if (depth < 0) depth = 0;
  if (depth < rt->ninsts) rt->ninsts = depth;
}

WInst *winst_need(const char *what) {
  WInst *in = winst_current();
  if (!in)
    wc_panic(__FILE__, __LINE__,
             "%s is a per-instance signal read outside any instance: bind one with "
             "with_inst(&inst) { ... } or name it with get_in(&inst, %s)",
             what, what);
  return in;
}

// ---------------------------------------------------------------- render-owned state
//
// The owner stack is where "the render position I am drawing" lives: pushed by
// wcomp_render_ctx/wcomp_html_ctx for a mounted view and by wkeyed_item for one row of a
// keyed list, popped by the matching end. It is deliberately NOT the `winst` stack above:
// that one answers "which instance did the app bind with with_inst?", and an app that binds
// its own instance inside a row must keep getting its own answer after this exists.

static void own_room(Wrt *rt) {
  if (rt->nowns < rt->capowns) return;
  i32 ncap = rt->capowns ? rt->capowns * 2 : 8;
  WInst **ns = wm_alloc((size_t)ncap * sizeof *ns);
  if (!ns) return;
  if (rt->nowns) memcpy(ns, rt->owns, (size_t)rt->nowns * sizeof *ns);
  wm_free(rt->owns);
  rt->owns = ns;
  rt->capowns = ncap;
}

// A null owner never shadows a real one further out: a node that is not inside a render
// position reads what an enclosing position provides.
WInst *winst_own_push(WInst *in) {
  Wrt *rt = wrt_default();
  if (!in) return winst_own_current();
  own_room(rt);
  WInst *prev = rt->nowns ? rt->owns[rt->nowns - 1] : nullptr;
  rt->owns[rt->nowns++] = in;
  return prev;
}

void winst_own_pop(void) {
  Wrt *rt = wrt_current();
  if (rt && rt->nowns > 0) rt->nowns--;
}

void winst_own_pop_to(WInst *in) {
  Wrt *rt = wrt_current();
  if (!rt || !in) return; // the matching push never happened: nothing to pop
  while (rt->nowns > 0 && rt->owns[rt->nowns - 1] != in) rt->nowns--;
  if (rt->nowns > 0) rt->nowns--;
}

WInst *winst_own_current(void) {
  Wrt *rt = wrt_current();
  return rt && rt->nowns > 0 ? rt->owns[rt->nowns - 1] : nullptr;
}

Wn *winst_own_node(const WDecl *d) {
  WInst *in = winst_own_current();
  if (!in)
    wc_panic(__FILE__, __LINE__,
             "%s is render-owned state read outside the position that owns it: it is drawn "
             "by a mounted view or a for_each row, and read from that view or an action on "
             "it. Declare it state(...) for one value per page, or state_in(...) if the app "
             "owns the lifetime",
             d && d->name ? d->name : "state_own");
  return winst_node(in, d);
}

WInst *winst_alloc(void) {
  WInst *in = wm_alloc(sizeof *in);
  if (!in) return nullptr;
  winst_init(in, nullptr);
  return in;
}

void winst_release(WInst *in) {
  if (!in) return;
  if (getenv("WEBC_QUARANTINE") && !wc_heap_alive(in))
    wc_panic(__FILE__, __LINE__, "winst_release on a FREED instance (%p)", (void *)in);
  winst_free(in); // kills every node it holds, so the graph shrinks back
  wm_free(in);
}

// ---------------------------------------------------------------- component instances
//
// A component call is a render position: its own state, its own effects, its own teardown.
// The identity is `(the position that drew it, the call site the macro captured)`, so the
// same call on the next render finds the same instance and two calls never share one. The
// table is the position's, swept when the position's render ends — the same shape as a keyed
// list's, with the call site as the key instead of the app's.

// The instance is heap and the table only holds a pointer to it: the table *grows* (and
// rehashes), and every node the instance drew remembers its owner by address.
WInst *winst_child(WInst *parent, i64 site) {
  if (!parent) return nullptr;
  if (!parent->kids || (parent->nkids + 1) * 10 >= parent->capkids * 7) {
    i32 ncap = parent->capkids ? parent->capkids * 2 : 8;
    WInstChild *ne = wm_alloc((size_t)ncap * sizeof *ne);
    if (!ne) return nullptr;
    if (parent->kids) {
      for (i32 i = 0; i < parent->capkids; i++) {
        WInstChild old = parent->kids[i];
        if (!old.live) continue; // a tombstone does not survive the rehash
        u64 h = wc_hash_i64(old.site) & (u64)(ncap - 1);
        while (ne[h & (u64)(ncap - 1)].live) h++;
        ne[h & (u64)(ncap - 1)] = old;
      }
      wm_free(parent->kids);
    }
    parent->kids = ne;
    parent->capkids = ncap;
  }
  u64 mask = (u64)(parent->capkids - 1);
  u64 h = wc_hash_i64(site) & mask;
  WInstChild *slot = nullptr;
  for (i32 probe = 0; probe <= parent->capkids; probe++) {
    WInstChild *e = &parent->kids[(h + (u64)probe) & mask];
    if (e->live && e->site == site) {
      slot = e;
      break;
    }
    if (!e->live && !e->ever) {
      slot = e; // empty: the probe ends here
      break;
    }
    if (!e->live && !slot) slot = e; // tombstone: a candidate, keep looking for a live match
  }
  if (!slot) return nullptr;
  if (!slot->live) {
    *slot = (WInstChild){.site = site, .live = true, .ever = true};
    parent->nkids++;
  }
  if (!slot->in) slot->in = winst_alloc();
  if (slot->in) slot->in->parent = parent;
  if (!slot->in) {
    slot->live = false;
    slot->ever = true;
    return nullptr;
  }
  slot->used = true;
  return slot->in;
}

void winst_sweep_children(WInst *parent) {
  if (!parent) return;
  // DIAGNOSTIC: a parent that has already been released is the fault we are hunting
  // (winst_sweep_children+0x60 under wkeyed_end). Say so instead of segfaulting.
  if (getenv("WEBC_QUARANTINE") && !wc_heap_alive(parent))
  {
    char d[160];
    wc_heap_describe(parent, d, sizeof d);
    wc_panic(__FILE__, __LINE__, "winst_sweep_children on a DEAD instance (%p): %s; freed at %p; "
             "nowns=%d", (void *)parent, d, wc_heap_freed_by(parent),
             wrt_current() ? (int)wrt_current()->nowns : -1);
  }
  if (!parent->kids) return;
  for (i32 i = 0; i < parent->capkids; i++) {
    WInstChild *e = &parent->kids[i];
    if (e->live && !e->used) {
      // the branch stopped drawing this component: its state, effects, cleanups and every
      // nested instance go with it
      if (e->in && getenv("WEBC_QUARANTINE") && !wc_heap_alive(e->in))
        wc_panic(__FILE__, __LINE__, "sweep child %d holds a FREED instance (%p)", (int)i,
                 (void *)e->in);
      if (e->in) winst_release(e->in);
      e->in = nullptr;
      e->live = false;
      e->ever = true; // tombstone: find() may not stop here
      if (parent->nkids > 0) parent->nkids--;
    }
  }
  for (i32 i = 0; i < parent->capkids; i++)
    if (parent->kids[i].live) parent->kids[i].used = false;
}

WInst *winst_own_need(const char *what) {
  WInst *in = winst_own_current();
  if (!in)
    wc_panic(__FILE__, __LINE__,
             "%s belongs to a render position and there is none in scope: call it from a view "
             "body (a mounted view, a for_each row, or a comp() call)",
             what ? what : "this declaration");
  return in;
}

Wn *winst_effect_once(WInst *in, const WDecl *d) {
  if (!in || !d) return nullptr;
  WSlot *s = slot_find(in, d);
  if (s) return s->node; // already started in this instance: a no-op, not a re-run
  return winst_node(in, d);
}

// ---------------------------------------------------------------- lifecycle

void wc_on_cleanup(void (*fn)(void *), void *ctx) {
  if (!fn) return;
  Wrt *rt = wrt_current();
  Wn *cur = rt ? rt->cur : nullptr;
  if (cur && cur->kind == W_EFFECT) {
    cl_push(&cur->cl, fn, ctx);
    return;
  }
  WInst *in = winst_own_current();
  if (!in) in = winst_current();
  if (!in)
    wc_panic(__FILE__, __LINE__,
             "on_cleanup outside an effect and outside any instance: it tears down what it is "
             "called in, so call it from an effect body, a comp() call, a row or a mount");
  // Outside an effect the cleanup belongs to the *position*, which outlives this render: it is
  // registered on the way in (the first render), not once per re-render, or a view that runs
  // sixty times a second would register sixty teardowns for one subscription.
  if (in->rendered) return;
  cl_push(&in->cl, fn, ctx);
}

typedef struct WMnt {
  void (*fn)(void *);
  void *ctx;
  WInst *in;
  struct WMnt *next;      // the queue, in registration order
  struct WMnt *next_same; // this instance's pending hooks, for cancellation
  bool dead;              // the position died before the flush: do not run it
} WMnt;

static WMnt *g_mnt_head, *g_mnt_tail;
static i32 g_mnt_n;

void wc_on_mount(void (*fn)(void *), void *ctx) {
  if (!fn) return;
  // A mount hook talks to the document, and a server render has none: queueing it there would
  // be a leak that nobody drains. The client half is the only half that runs them.
  if (!wc_in_client()) return;
  WInst *in = winst_own_current();
  if (!in)
    wc_panic(__FILE__, __LINE__,
             "on_mount outside a render position: it runs once, after the batch that drew the "
             "node it wants to touch");
  if (in->rendered) return; // mounted once: a re-render is not a mount
  WMnt *m = wm_alloc(sizeof *m);
  if (!m) return;
  *m = (WMnt){.fn = fn, .ctx = ctx, .in = in, .next_same = (WMnt *)in->mounts};
  in->mounts = m;
  if (g_mnt_tail) g_mnt_tail->next = m;
  else g_mnt_head = m;
  g_mnt_tail = m;
  g_mnt_n++;
}

// Called when an instance is torn down: its queued hooks must not touch a position that no
// longer has a node on the page (the pointer itself stays valid until the queue is drained).
static void mnt_cancel(WInst *in) {
  for (WMnt *m = (WMnt *)in->mounts; m; m = m->next_same) {
    m->dead = true;
    // The queue outlives the instance: `in` is about to be freed, so the drain must
    // never touch it again. Leaving `m->in` set made wc_mount_run() dereference a
    // freed WInst (and write `m->in->mounts = nullptr` into whatever reused it) —
    // the intermittent SIGSEGV seen at high concurrency.
    m->in = nullptr;
  }
  in->mounts = nullptr;
}

// Run them in the order they were registered, after the batch has gone out. A hook whose
// position died before the drain is skipped: the node it wanted is not on the page.
void wc_mount_run(void) {
  while (g_mnt_head) {
    WMnt *m = g_mnt_head;
    g_mnt_head = m->next;
    if (!g_mnt_head) g_mnt_tail = nullptr;
    g_mnt_n--;
    if (!m->dead && (!m->in || m->in->live)) {
      winst_own_push(m->in);
      m->fn(m->ctx);
      winst_own_pop();
    }
    // The instance's list head is the hook registered last, so it is freed last too: when
    // this runs with the head, the list is empty. A pointer left behind makes the next
    // teardown of that instance walk freed memory.
    if (m->in && m->in->mounts == (void *)m) m->in->mounts = nullptr;
    wm_free(m);
  }
}

void wc_mount_clear(void) {
  while (g_mnt_head) {
    WMnt *m = g_mnt_head;
    g_mnt_head = m->next;
    // The queue owns what it frees: an instance still pointing at a hook that is gone would
    // walk freed memory the next time it is torn down.
    if (m->in) m->in->mounts = nullptr;
    wm_free(m);
  }
  g_mnt_tail = nullptr;
  g_mnt_n = 0;
}

i32 wc_mount_pending(void) { return g_mnt_n; }

// The unwinder's view of the owner stack: a panic longjmps past the cleanup that would have
// popped it, and a leaked owner is the same bug the instance stack had (a view that panicked
// inside a row left that row bound for the next request on the task).
i32 wc_own_depth(void) {
  Wrt *rt = wrt_current();
  return rt ? rt->nowns : 0;
}

void wc_own_restore(i32 depth) {
  Wrt *rt = wrt_current();
  if (!rt) return;
  if (depth < 0) depth = 0;
  if (depth < rt->nowns) rt->nowns = depth;
}

// ---------------------------------------------------------------- scopes

void w_scope_enter(WScope *s, WScope *parent) {
  *s = (WScope){.parent = parent, .rt = g_rt ? g_rt : wrt_default()};
}

void w_own(WScope *s, Wn *n) {
  if (!s || !n) return;
  if (s->n == s->cap) {
    i32 ncap = s->cap ? s->cap * 2 : 8;
    Wn **no = rt_alloc(s->rt, (size_t)ncap * sizeof(Wn *));
    if (s->owned && s->n) memcpy(no, s->owned, (size_t)s->n * sizeof(Wn *));
    if (!s->rt->arena) wm_free(s->owned);
    s->owned = no;
    s->cap = ncap;
  }
  s->owned[s->n++] = n;
  n->owner = s;
}

void w_scope_dispose(WScope *s) {
  if (!s || s->disposed) return;
  s->disposed = true;
  for (i32 i = s->n - 1; i >= 0; i--) {
    Wn *n = s->owned[i];
    // a node killed here and already recycled could be owned by someone else now,
    // so the ownership tag is what decides, not the (possibly stale) list entry
    if (!n || n->owner != s) continue;
    w_kill(n);
  }
  s->n = 0;
  if (s->parent && s->parent->owned == s->owned) s->parent->owned = nullptr;
  // the ownership list was the scope's own memory; leaving it behind is 64 B per
  // mount+dispose cycle in a heap graph (measured with effect_with in a list)
  if (s->owned && (!s->rt || !s->rt->arena)) {
    wm_free(s->owned);
    s->owned = nullptr;
    s->cap = 0;
  }
}
// ---------------------------------------------------------------- state transplant (dev)
//
// A hot module swap runs new code against old memory — except that in wasm "old memory"
// is a different linear memory, so the values have to cross the boundary explicitly. The
// rule that makes that safe: only *named* state crosses, and only byte-for-byte, by name,
// with the new module's own declaration deciding the shape. Nothing walks the heap and
// nothing copies a pointer.
//
// `state()` and `signal_str()` register themselves; anything else the app wants to keep
// across a swap opts in with `webc_dev_state_save/load` (see webc_dev.h). The registry
// costs a pointer array and one constructor call per signal, and it is what makes a save
// stop losing the value somebody just typed into a search box.

typedef struct WState {
  const char *name;
  void *val;
  size_t len;
  bool is_str;  // the slot holds a `const char *` (a Text), not a value to copy byte-wise
  void *init;   // the value the declaration started with, held by the registry
} WState;

static WState g_state[192];
static i32 g_nstate;

void w_state_register(const char *name, void *val, size_t len, bool is_str) {
  if (!name || !val || !len) return;
  for (i32 i = 0; i < g_nstate; i++)
    if (g_state[i].name == name) return; // the same declaration, seen from another TU
  for (i32 i = 0; i < g_nstate; i++)
    if (g_state[i].name && strcmp(g_state[i].name, name) == 0) return;
  if (g_nstate >= (i32)WC_COUNTOF(g_state)) return;
  // The initial value, held here rather than read back from the slot: a test that writes a
  // state has no other way home, and "leave the app as you found it" is a line every test
  // would otherwise have to remember.
  void *init = nullptr;
  if (is_str) {
    const char *v = *(const char **)val;
    size_t n = v ? strlen(v) : 0;
    char *copy = (char *)malloc(n + 1);
    if (copy) {
      if (n) memcpy(copy, v, n);
      copy[n] = 0;
      init = copy;
    }
  } else {
    init = malloc(len);
    if (init) memcpy(init, val, len);
  }
  g_state[g_nstate++] = (WState){name, val, len, is_str, init};
}

i32 w_state_count(void) { return g_nstate; }

// Forget the registry and the initial values with it: a wasm module is about to be
// replaced, and the new one registers its own.
void w_state_clear(void) {
  for (i32 i = 0; i < g_nstate; i++) free(g_state[i].init);
  g_nstate = 0;
}

// Every state back to what it was declared with. Cheap enough to run before every test:
// 192 slots at most, and a memcpy each.
void w_state_rewind(void) {
  for (i32 i = 0; i < g_nstate; i++) {
    WState *s = &g_state[i];
    if (!s->init) continue;
    if (s->is_str) *(const char **)s->val = (const char *)s->init;
    else memcpy(s->val, s->init, s->len);
  }
}

// "name\tlen\tkind\n" then the payload then "\n". The length is explicit so the reader
// never has to guess where a value ends — a `str` signal's bytes are arbitrary.
void w_state_dump(Buf *out) {
  for (i32 i = 0; i < g_nstate; i++) {
    WState *s = &g_state[i];
    buf_printf(out, "%s\t%zu\t%d\n", s->name, s->len, s->is_str ? 1 : 0);
    if (s->is_str) {
      const char *v = *(const char **)s->val;
      size_t n = v ? strlen(v) : 0;
      buf_printf(out, "%zu\n", n);
      if (n) buf_write(out, v, n);
      buf_putc(out, '\n');
    } else {
      buf_write(out, s->val, s->len);
      buf_putc(out, '\n');
    }
  }
}

static WState *state_find(const char *name) {
  for (i32 i = 0; i < g_nstate; i++)
    if (g_state[i].name && strcmp(g_state[i].name, name) == 0) return &g_state[i];
  return nullptr;
}

// Returns how many values were taken from the old module. A name the new module does not
// declare is skipped silently (a signal was deleted, which is a normal edit); a length or
// kind mismatch is reported through `warn` and skipped, because guessing there would be a
// corrupted value on a live page.
i32 w_state_load(str in, char *warn, size_t warn_cap) {
  if (!in.p || in.n <= 0) return 0;
  const char *p = in.p, *end = in.p + in.n;
  i32 applied = 0;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) break;
    size_t hl = (size_t)(nl - p);
    char head[192];
    if (hl >= sizeof head) {
      p = nl + 1;
      continue;
    }
    memcpy(head, p, hl);
    head[hl] = 0;
    char *t1 = strchr(head, '\t');
    if (!t1) break;
    *t1++ = 0;
    char *t2 = strchr(t1, '\t');
    if (!t2) break;
    *t2++ = 0;
    size_t len = (size_t)strtoul(t1, nullptr, 10);
    bool is_str = strtoul(t2, nullptr, 10) != 0;
    p = nl + 1;
    WState *s = state_find(head);
    if (is_str) {
      const char *ln = memchr(p, '\n', (size_t)(end - p));
      if (!ln) break;
      size_t n = (size_t)strtoul(p, nullptr, 10);
      p = ln + 1;
      if ((size_t)(end - p) < n) break;
      if (s && s->is_str) {
        // one byte more than the value: a str signal's bytes are also read as a C string
        // by every caller that prints it, so a value that stops exactly at its last byte is
        // a read past the end waiting to happen
        char *copy = (char *)malloc(n + 1);
        if (copy) {
          if (n) memcpy(copy, p, n);
          copy[n] = 0;
        }
        *(const char **)s->val = copy;
        applied++;
      }
      p += n;
      if (p < end && *p == '\n') p++;
      continue;
    }
    if ((size_t)(end - p) < len) break;
    if (s && !s->is_str && s->len == len) {
      memcpy(s->val, p, len);
      applied++;
    } else if (s && warn && !warn[0]) {
      snprintf(warn, warn_cap, "signal '%s' changed shape (%zu bytes -> %zu); hard reload",
               head, len, s->len);
    }
    p += len;
    if (p < end && *p == '\n') p++;
  }
  return applied;
}
