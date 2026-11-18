// SPDX-License-Identifier: Apache-2.0
// WebC — markup builder, SSR renderer and DOM command emitter.
#include <stdarg.h>
#include <webc/log.h>
#include <webc/dom.h>
#include <webc/react.h>
#include <webc/vec.h>
#include <webc/relations.h> // wc_ref_epoch_swap: the render says where the rows live
#include <webc/http.h>      // webc_req_current: ...which is the request's arena

#include "internal.h"

// ---------------------------------------------------------------- reporting
//
// The renderer's own diagnostics have to reach a human in both halves. Natively that is
// stderr; in the wasm build there is no stderr, but the bridge forwards the module's stdout
// to the browser console — so one channel covers both, and no code has to ask which half it
// is in.
//
// These are *reports*, not panics. A panic in wasm is a blank tab, which is strictly worse
// than the thing being reported: a dropped attribute is a CSS bug, a dead module is nothing
// at all. Each flag makes the message once per process — a render that damages one node
// damages it on every frame, and 60 lines a second is not a diagnostic.
static bool g_attr_name_reported;
[[maybe_unused]] static bool g_attr_scan_reported; // only read where reporting replaces panicking
static bool g_field_missing_reported;
static const char *tag_of(WDom *n);

static void dom_report_once(bool *flag, const char *fmt, ...) {
  if (!flag || *flag) return;
  *flag = true;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
#ifdef __wasm__
  fflush(stderr);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  fflush(stdout);
#endif
}

// The build in progress belongs to the *task*, not the OS thread. A view that
// touches the database parks the green thread, and it is then resumed on another
// worker — whose thread-local build is null. That is not theory: `examples/todo`
// renders `db_all()` inside its view, and under load at 8 connections every
// worker started answering 500 "el() outside a build", then one render
// dereferenced the null and took the process down with SIGSEGV at address 72.
//
// So the pointer lives in task-local storage (wtask_build), with a thread-local
// fallback for the wasm target (no scheduler) and for code that runs with no
// current task (tests, startup).
static _Thread_local WBuild *t_build_outside;

// Weak: src/task.c is not part of the wasm build, where there are no tasks.
extern void *wtask_build(void) __attribute__((weak));
extern void wtask_set_build(void *p) __attribute__((weak));

WBuild *wbuild_current(void) {
  if (wtask_build) return (WBuild *)wtask_build();
  return t_build_outside;
}

static void build_set(WBuild *b) {
  if (wtask_set_build)
    wtask_set_build(b);
  else
    t_build_outside = b;
}

static void *n_alloc(WBuild *b, size_t n) {
  return b ? arena_zalloc(b->nodes_arena, n) : wm_alloc(n);
}

WBuild *wbuild_begin(Arena *a, WScope *scope) {
  WBuild *b = arena_zalloc(a, sizeof *b);
  b->a = a;
  b->nodes_arena = a;
  b->scope = scope ? scope : pool_scope(b);
  b->next_id = 1;
  b->run = 1;
  b->cursor = 0;
  b->root = nullptr;
  // reuse list: nodes from the previous run, for fine-grained patching
  b->reuse = b->scope && b->scope->owned ? nullptr : nullptr;
  b->enclosing = wbuild_current(); // a partial rendered inside a view
  // a partial rendered inside a view inherits that view's provided context
  b->prov = b->enclosing ? b->enclosing->prov : nullptr;
  build_set(b);
  return b;
}

void wbuild_end(WBuild *b) {
  if (b->comp_root) {
    // A build with no position of its own (a bare wbuild, an email body) still let `comp()`
    // draw: its instances hang off a per-build root, and the build's end is their end.
    winst_release((WInst *)b->comp_root);
    b->comp_root = nullptr;
  }
  if (wbuild_current() == b) build_set(b->enclosing);
}

// ---------------------------------------------------------------- component instances
//
// `comp(Name, args)` (dom.h): draw the part in a position of its own. The parent position is
// the nearest owner — the mounted view, the for_each row, or the `comp()` one level out — and
// the key is the call site the macro captured, so the instance is a property of *this call*.
//
// Nothing is allocated here for a component that never asks for anything: the entry is the
// identity and the instance inside it is created on demand (winst_child).
WCompFrame wc_comp_enter_(i64 site) {
  WCompFrame f = {0};
  WInst *parent = winst_own_current();
  if (!parent) {
    WBuild *b = wbuild_current();
    if (!b) return f; // nowhere to draw and nothing to own: the body runs, the frame is inert
    if (!b->comp_root) b->comp_root = winst_alloc();
    parent = (WInst *)b->comp_root;
  }
  WInst *in = winst_child(parent, site);
  if (!in) return f; // out of memory: the markup still draws, it just owns nothing
  winst_own_push(in);
  f.in = in;
  f.open = true;
  return f;
}

void wc_comp_leave_(WCompFrame *f) {
  if (!f || !f->open) return;
  f->open = false;
  // This call's render is over: every component it drew has been claimed (or not). Sweeping
  // here — while this instance is still the ambient position — is what makes a nested
  // component's cleanup able to read the state of the one it was drawn in.
  if (f->in) {
    winst_sweep_children(f->in);
    f->in->rendered = true; // on_cleanup/on_mount were the first render's business
  }
  winst_own_pop();
}

void wc_kids_(WKids k) {
  if (!k.fn) return;
  // The children are the caller's markup: run them in the caller's position, so a `state_own`
  // read inside the block is the caller's (and an action bound in it resolves the same way).
  winst_own_push(k.pos);
  k.fn(k.ctx);
  winst_own_pop();
}

// A panic inside a view unwinds past wbuild_end() without running it, so the
// frame that caught the panic (mw_recover, the server's dispatch guard) has to
// clear the build it left behind. Otherwise the next render on that task sees a
// build pointing at a dead stack.
WBuild *wbuild_abandon(void) {
  WBuild *b = wbuild_current();
  build_set(nullptr);
  return b;
}

WDom *wbuild_root(WBuild *b) {
  return b ? b->root : nullptr;
}

// Scopes for renders are arena-allocated: one instruction frees them with the
// arena, and disposal still runs explicitly for correctness.
WScope *pool_scope(WBuild *b) {
  WScope *s = arena_zalloc(b->a, sizeof *s);
  w_scope_enter(s, b->scope);
  return s;
}

// ---------------------------------------------------------------- node basics

// A DOM id is a slot the browser holds a node for: it must be unique for the
// whole life of the component, not unique within a render. Reused nodes keep
// theirs, new ones take the lowest free one. The table grows with the view, because
// a fixed one hands out ids the wasm bridge never indexed: past its end a page looks
// right and has dead rows (measured: 1 906 of 6 001 nodes unaddressable at 2 000 rows).
#define WC_ID_CAP_INIT 4096

static i32 id_cap_for(i32 need) {
  i32 cap = WC_ID_CAP_INIT;
  while (cap <= need) cap *= 2;
  return cap;
}

// Make ids below `need` addressable. The table lives in the render arena, so growing
// it is a bump allocation and a copy, not a new lifetime to reason about.
static void id_ensure(WBuild *b, i32 need) {
  if (b->id_used && need <= b->id_cap) return;
  i32 cap = id_cap_for(need - 1);
  bool *nu = arena_zalloc(b->a, (size_t)cap * sizeof *nu);
  if (!nu) return;
  if (b->id_used)
    for (i32 i = 0; i < b->id_cap; i++) nu[i] = b->id_used[i];
  b->id_used = nu;
  b->id_cap = cap;
}

static i32 take_id(WBuild *b) {
  if (!b->id_used) return b->next_id++; // SSR: ids are labels, nothing indexes them
  for (;;) {
    for (i32 i = b->id_scan < 1 ? 1 : b->id_scan; i < b->id_cap; i++) {
      if (b->id_used[i]) continue;
      b->id_used[i] = true;
      b->id_scan = i + 1;
      return i;
    }
    // every id below the cap is in use: grow rather than hand out an unreachable one
    id_ensure(b, b->id_cap * 2);
    if (b->id_cap < 2) return b->next_id++; // allocation failed: at least stay unique
  }
}

static WDom *node_new(WBuild *b, WDomType t, const char *tag) {
  WDom *n = n_alloc(b, sizeof *n);
  b->nnodes++;
  n->inst = winst_current();      // the instance this node's view was rendered in, if any
  n->owner = winst_own_current(); // … and the render position that drew it (state_own)
  n->type = t;
  n->tag = tag;
  n->id = t == WD_FRAGMENT ? -1 : take_id(b);
  n->is_new = true;
  n->seen = b->run;
  return n;
}

void wbuild_reserve_ids(WBuild *b, WDom **nodes, i32 n) {
  if (!b || !nodes) return;
  i32 hi = 0;
  for (i32 i = 0; i < n; i++)
    if (nodes[i] && nodes[i]->id > hi) hi = nodes[i]->id;
  b->id_cap = id_cap_for(hi);
  b->id_used = arena_zalloc(b->a, (size_t)b->id_cap * sizeof *b->id_used);
  b->id_scan = 1;
  for (i32 i = 0; i < n; i++) {
    WDom *nd = nodes[i];
    if (nd && nd->id > 0 && nd->id < b->id_cap) b->id_used[nd->id] = true;
  }
}

static void append_child(WDom *parent, WDom *child) {
  // A node cannot be its own parent, and it cannot be appended under itself. Both
  // make a cycle, and a cycle in this tree is not a tree the rest of the framework
  // can walk: emit_new() and wdom_index() spin forever, which in a browser is a tab
  // that stops answering with no error anywhere. Detectable here for one compare.
  if (parent == child)
    panic("dom: appended <%s> to itself", child->tag ? child->tag : "#text");
  // The third cycle shape is one node twice in the same child list: the second
  // append makes the node its own sibling (`last->next` = itself) and every walk
  // of the tree spins — render_ssr first, which is a page that never answers with
  // no error anywhere. One compare names it here; the duplicate-key panic in
  // wkeyed_item() is the sentence that explains the common way in (two rows with
  // the unset pk 0).
  if (parent->last == child)
    panic("dom: <%s> appended to <%s> twice — a node cannot be two children",
          child->tag ? child->tag : "#text", parent->tag ? parent->tag : "#frag");
  for (WDom *up = parent; up; up = up->parent)
    if (up == child)
      panic("dom: appended <%s> under a node it contains (the tree already has a cycle)",
            child->tag ? child->tag : "#text");
  child->parent = parent;
  child->prev = parent->last;
  child->next = nullptr; // the node may carry a stale link from a previous run
  if (parent->last)
    parent->last->next = child;
  else
    parent->first = child;
  parent->last = child;
}

static void seq_push(WBuild *b, WDom *n) {
  if (b->nseq == b->capseq) {
    i32 ncap = b->capseq ? b->capseq * 2 : 16;
    WDom **ns = arena_alloc(b->nodes_arena, (size_t)ncap * sizeof(WDom *));
    if (b->seq && b->nseq) memcpy(ns, b->seq, (size_t)b->nseq * sizeof(WDom *));
    b->seq = ns;
    b->capseq = ncap;
  }
  b->seq[b->nseq++] = n;
}

void wbuild_set_reuse(WBuild *b, WDom **nodes, i32 n) {
  b->reuse = nodes;
  b->nreuse = n;
  b->cursor = 0;
  b->nseq = 0;
  // Every node is re-attached by the render, so the previous child lists are
  // stale the moment we start: clearing them keeps a reused node from being
  // reachable from two parents at once.
  for (i32 i = 0; i < n; i++) {
    WDom *old = nodes[i];
    if (!old) continue;
    // Where this node lived, read now and not during the render: a reused node is
    // re-parented as soon as it is taken, so `->parent` stops meaning "the previous tree"
    // the moment the first match happens.
    old->was_parent = old->parent;
    old->was_prev = old->prev;
    old->was_first = old->first;
    old->was_next = old->next;
    old->first = old->last = nullptr;
  }
}

WDom **wbuild_seq(WBuild *b, i32 *n) {
  if (n) *n = b->nseq;
  return b->seq;
}

// "The same place in the tree" for reuse purposes: the same parent. The framework's
// own *wrapper* fragments — the several roots of one view, re-wrapped fresh every run —
// are interchangeable with each other. A keyed item's fragment is not: it hangs under a
// real element and owns its rows, so matching it to any other fragment let one keyed
// list steal another's nodes — the second loop rendered the first loop's rows, the
// orphaned originals were removed with their parent, and a hydrated page lost the rows
// it started with, one append at a time (tests/keyed_patch_test.c).
static bool same_place(WDom *before, WDom *now) {
  if (before == now) return true;
  bool was_wrap = !before || (before->type == WD_FRAGMENT && before->wrap);
  bool is_wrap = !now || (now->type == WD_FRAGMENT && now->wrap);
  return was_wrap && is_wrap;
}

// Reuse a node from the previous run, so unchanged nodes keep their identity (and their
// DOM id) across runs.
//
// The match is `(parent, tag)`, not a flat cursor through the previous tree: that tree is
// a pre-order walk, so one branch that appears or disappears shifts every node after it,
// and a flat cursor then hands back a node from a different parent. The view overwrites its
// text and attributes, so the content usually still comes out right — but the node the
// render needed is left unclaimed, and the emitter tells the browser to `remove` a node
// the view still wrote. That is a page that silently loses part of itself between renders:
// a chart keeps its frame and loses its bars, and the workaround is to stop writing `if` in
// a view. Scanning forward for the right parent makes a branch free to appear, grow and
// vanish.
// The caller decides the cursor; taking a node never moves it.
static WDom *take_node(WBuild *b, WDom *old) {
  // snapshot attributes for diffing, then start a clean list
  old->prev_attrs = old->attrs;
  old->nprevattrs = old->nattrs;
  old->attrs = nullptr;
  old->nattrs = 0;
  old->capattrs = 0;
  // Listeners are re-registered by this run's view: keeping the old list would call a
  // handler once per render the node has survived. The array has to go with the count.
  // It lives in a scratch arena, and the arena that held it is reset at the end of the
  // *next* render — so a node that kept its pointer across two renders wrote this run's
  // listeners into memory the render had already handed to somebody else. That is the
  // damaged attribute pair (§1) exactly: `data-hook`'s value read "click", the next
  // pair had no name at all, and the batch that carried it dies on the wire.
  old->nlisteners = 0;
  old->listeners = nullptr;
  old->caplisteners = 0;
  old->is_new = false;
  old->text_dirty = false;
  old->attrs_dirty = false;
  old->first = old->last = nullptr;
  // Its place in the tree is this render's decision, not the last one's. A kept
  // parent/prev/next links the node into the previous render's tree: a second root's `el()`
  // appends to a fragment from the previous render whose `last` still points at this node,
  // so append_child sets child->next = child, and the emitter then walks a sibling list that
  // points at itself and the browser tab stops answering.
  old->parent = old->prev = old->next = nullptr;
  old->seen = b->run;
  old->inst = winst_current();
  old->owner = winst_own_current();
  b->nnodes++;
  seq_push(b, old);
  return old;
}

static WDom *take_reuse(WBuild *b, WDomType t, const char *tag) {
  // A node's previous children come back first, whatever the node is: a keyed
  // item's fragment, a reused row element, the wrapper of a multi-root view. Reuse
  // is per parent, so a list that returns in a new order re-matches each item
  // against its own rows instead of whatever the global cursor is pointing at —
  // the monotonic cursor made a reorder re-create every node past the first move
  // (tests/keyed_move_test.c).
  if (b->cur && b->cur->was_first) {
    for (WDom *old = b->cur->was_first; old; old = old->was_next) {
      if (!old || old->dead || old->seen == b->run) continue;
      if (old->was_parent != b->cur) continue; // not this fragment's, after a swap
      if (old->type == t &&
          (t != WD_ELEMENT || old->tag == tag ||
           (old->tag && tag && strcmp(old->tag, tag) == 0)))
        return take_node(b, old);
    }
  }
  for (i32 i = b->cursor; i < b->nreuse; i++) {
    WDom *old = b->reuse[i];
    if (!old || old->dead) continue;
    if (old->seen == b->run) continue; // taken by a fragment's own chain this run
    if (!same_place(old->was_parent, b->cur)) continue;
    // the pointer compare is the fast path (every render inside one module hits it);
    // the content compare is what makes reuse work after a hot swap moved the tree into
    // another module's memory
    if (old->type == t &&
        (t != WD_ELEMENT || old->tag == tag || (old->tag && tag && strcmp(old->tag, tag) == 0))) {
      b->cursor = i + 1;
      return take_node(b, old);
    }
  }
  // No match: the view added a node here. The cursor stays where it is — the old node
  // here is gone from this render, but a later sibling of this parent may still want it.
  return nullptr;
}

// A render may have more than one top-level node: a view that returns two
// siblings, a page plus its own script, a plain fragment. Keeping exactly one — the
// first node becomes b->root, every later top-level node is allocated, appended to
// nothing and dropped — ships a third of a view with no error anywhere. So the second
// root promotes the first into a fragment, which renders as its children do and emits
// each child at the top level.
static void add_root(WBuild *b, WDom *n) {
  if (!b->root) {
    b->root = n;
    return;
  }
  if (b->root->type != WD_FRAGMENT) {
    WDom *frag = node_new(b, WD_FRAGMENT, nullptr);
    frag->wrap = true;
    seq_push(b, frag);
    append_child(frag, b->root);
    b->root = frag;
  }
  append_child(b->root, n);
  // from here the fragment is the top level: a third root closes back into it
  b->cur = b->root;
}

WDom *wc_el_open(const char *tag) {
  WBuild *b = wbuild_current();
  wc_assert(b && "el() outside a build: call wbuild_begin first");
  WDom *n = take_reuse(b, WD_ELEMENT, tag);
  if (!n) {
    n = node_new(b, WD_ELEMENT, tag);
    seq_push(b, n);
  } else {
    n->tag = tag;
  }
  if (b->cur)
    append_child(b->cur, n);
  else
    add_root(b, n);
  b->cur = n;
  b->depth++;
  return n;
}

void wc_el_pop(WDom **slot) {
  WBuild *b = wbuild_current();
  if (b && b->cur) {
    // close: compare attributes with the previous run
    WDom *n = b->cur;
    if (!n->is_new) {
      if (n->nattrs != n->nprevattrs) n->attrs_dirty = true;
      for (i32 i = 0; i < n->nattrs && !n->attrs_dirty; i++) {
        bool found = false;
        for (i32 j = 0; j < n->nprevattrs; j++) {
          if (!str_eq(n->prev_attrs[j].name, n->attrs[i].name)) continue; // names are str_lit
          if (!str_eq(n->prev_attrs[j].value, n->attrs[i].value)) n->attrs_dirty = true;
          found = true;
          break;
        }
        if (!found) n->attrs_dirty = true;
      }
    }
    b->cur = n->parent;
  }
  b->depth--;
  (void)slot;
}

static WDom *add_text_node(WDom *parent, str text, WDomType t) {
  WBuild *b = wbuild_current();
  WDom *n = take_reuse(b, t, nullptr);
  if (!n) {
    n = node_new(b, t, nullptr);
    n->text = str_dup(b->a, text);
    n->text_dirty = true;
    seq_push(b, n);
  } else {
    // Text and attribute values live in the *render's* arena, which a component
    // recycles: the copy made by render N-1 is still readable during render N (that
    // is the window the patch needs) and gone afterwards. So every node rewrites its
    // own copy each run instead of keeping the previous pointer — a kept pointer pins
    // these copies in the component arena forever, 303 B per render for a single changing
    // text node.
    bool changed = !str_eq(n->text, text);
    n->text = str_dup(b->a, text);
    if (changed) n->text_dirty = true;
  }
  if (parent)
    append_child(parent, n);
  else if (b)
    add_root(b, n);
  return n;
}

static void add_text(WDom *parent, str text, WDomType t) { (void)add_text_node(parent, text, t); }

void wc_text_str(str s) {
  WBuild *b = wbuild_current();
  if (!b) return;
  add_text(b->cur, s, WD_TEXT);
}

void wc_text_fmt(const char *fmt, ...) {
  WBuild *b = wbuild_current();
  if (!b) return;
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    return;
  }
  char *p = arena_alloc(b->a, (size_t)n + 1);
  vsnprintf(p, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  add_text(b->cur, (str){p, (size_t)n}, WD_TEXT);
}

void wc_raw_static(const char *s) {
  WBuild *b = wbuild_current();
  if (!b) return;
  add_text(b->cur, str_lit(s), WD_RAW);
}

void wc_attr(const char *name, str value) {
  WBuild *b = wbuild_current();
  if (!b || !b->cur) return;
  WDom *n = b->cur;
  // A nameless attribute is not markup, and it is the one malformed pair that reaches the
  // browser as `setAttribute('')` — an exception that takes the whole batch with it. Refuse
  // it at the writer, where the node and the index are still known, and say
  // so once: the rest of the tree is fine, and a dropped attribute is a CSS bug, not a
  // blank page.
  if (!name || !*name) {
    dom_report_once(&g_attr_name_reported,
                    "dom: <%s id=%d> attribute #%d has an empty name — the pair was dropped "
                    "(value %.40s)",
                    tag_of(n), n->id, (int)n->nattrs, value.p ? value.p : "");
    return;
  }
  if (n->nattrs == n->capattrs) {
    i32 ncap = n->capattrs ? n->capattrs * 2 : 4;
    WAttrPair *na = arena_alloc(b->a, (size_t)ncap * sizeof(WAttrPair));
    if (n->attrs && n->nattrs) memcpy(na, n->attrs, (size_t)n->nattrs * sizeof(WAttrPair));
    n->attrs = na;
    n->capattrs = ncap;
  }
  // The value is copied into this render's arena: `value` may be a scratch string
  // (`fmt()`, `str_from_i64`) that dies with the render, and the previous run's copy
  // is recycled once this run's patch has read it.
  n->attrs[n->nattrs++] =
      (WAttrPair){.name = str_lit(name), .value = value.p ? str_dup(b->a, value) : value};
}

void wc_attr_c(const char *name, const char *value) {
  wc_attr(name, value ? str_lit(value) : (str){"", 0});
}

void wc_attr_i64(const char *name, i64 value) {
  wc_attr(name, str_from_i64(wbuild_current() ? wbuild_current()->a : nullptr, value));
}

void wc_attr_bool(const char *name, bool value) {
  wc_attr(name, str_lit(value ? "true" : "false"));
}

void wc_bool_attr(const char *name, bool on) {
  if (on) wc_attr(name, (str){"", 0});
}

void wc_class(str value) {
  if (value.n) wc_attr("class", value);
}

void wc_style(const char *decls) {
  if (decls && *decls) wc_attr("style", (str){decls, strlen(decls)});
}

void wc_style_str(str decls) {
  if (decls.p && decls.n) wc_attr("style", decls);
}

// ---------------------------------------------------------------- sb()
//
// See dom.h: a builder scoped to the render that asked for it. `buf_init` with an arena
// means growth is arena-allocated and nothing is ever freed by hand; the render's reset
// collects all of it.
Buf *sb(void) {
  WBuild *b = wbuild_current();
  Arena *a = (b && b->a) ? b->a : wc_arena_get();
  Buf *out = arena_zalloc(a, sizeof *out);
  buf_init(out, a, 256);
  return out;
}

str sb_str(Buf *b) { return buf_str(b); }

const char *sb_cstr(Buf *b) {
  buf_reserve(b, 1); // room for the NUL buf_cstr writes
  return buf_cstr(b);
}

void sb_puts(Buf *b, const char *s) { if (s) buf_puts(b, s); }
void sb_putstr(Buf *b, str s) { if (s.p && s.n) buf_putstr(b, s); }
void sb_putc(Buf *b, char c) { buf_putc(b, c); }
void sb_i64(Buf *b, i64 v) { buf_puti64(b, v); }
void sb_u64(Buf *b, u64 v) { buf_putu64(b, v); }

void sb_printf(Buf *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  buf_vprintf(b, fmt, ap);
  va_end(ap);
}

void sb_num(Buf *b, double v, int decimals) {
  if (decimals < 0) decimals = 0;
  if (decimals > 6) decimals = 6;
  buf_reserve(b, 48); // a double with 6 decimals and a 1e308 exponent fits
  size_t room = b->cap - b->n;
  int n = snprintf(b->p + b->n, room, "%.*f", decimals, v);
  if (n > 0) b->n += ((size_t)n < room) ? (size_t)n : room - 1;
}

void wc_show(bool cond) {
  if (!wbuild_current() || !wbuild_current()->cur) return;
  if (!cond) {
    // append to the existing style attribute when there is one
    WDom *n = wbuild_current()->cur;
    for (i32 i = 0; i < n->nattrs; i++) {
      if (str_eq_c(n->attrs[i].name, "style")) {
        n->attrs[i].value = str_fmt(wbuild_current()->a, "%.*s;display:none",
                                    (int)n->attrs[i].value.n, n->attrs[i].value.p);
        return;
      }
    }
    wc_attr("style", str_lit("display:none"));
  }
}

void wc_on(const char *event, WEventFn fn, void *ctx) {
  WBuild *b = wbuild_current();
  if (!b || !b->cur) return;
  WDom *n = b->cur;
  // A view re-runs on every change, and a handler attached to a node it did not
  // recreate must not be attached twice. Replacing by (event, fn) keeps one listener
  // per binding — and refreshes its context, which is how a keyed list hands a click
  // the element's current address after the vector has grown.
  for (i32 i = 0; i < n->nlisteners; i++)
    if (n->listeners[i].fn == fn && str_eq_c(n->listeners[i].event, event)) {
      n->listeners[i].ctx = ctx;
      return;
    }
  if (n->nlisteners == n->caplisteners) {
    i32 ncap = n->caplisteners ? n->caplisteners * 2 : 2;
    WListener *nl = arena_alloc(b->a, (size_t)ncap * sizeof(WListener));
    if (n->listeners && n->nlisteners)
      memcpy(nl, n->listeners, (size_t)n->nlisteners * sizeof(WListener));
    n->listeners = nl;
    n->caplisteners = ncap;
  }
  n->listeners[n->nlisteners++] = (WListener){.event = str_lit(event), .fn = fn, .ctx = ctx};
}

// ---------------------------------------------------------------- client hooks
//
// Two small tables, filled by the client half at startup and read while the tree is
// being built: a named element becomes a listener with the current keyed item as its
// context, and a named input becomes the DOM face of a signal.
//
// 128, not 32: a table-driven tab strip goes live one name per row off its own data, so a
// page of 23 tabs with a named chart group of three events and its own view-wired buttons
// passes 32 easily — and a full table silently drops the registration after it. A
// bind-per-row list is a supported shape: the cap catches a runaway loop, not a long page.
#define WC_HOOK_MAX 128

typedef struct WHook {
  const char *name;
  const char *event;
  WEventFn fn;
  bool seen;       // some element carried this name — a bind nothing honours is dead code
  bool ever_seen;  // and has it ever (a typo never renders; a conditional renders sometimes)
  bool reported;   // and this is how it was said out loud, once
  i32 since;     // the client render this name was bound during
  WDom *node;    // the element that carried it, so a native test can say which one it clicked
  void *ctx;     // the item the element was rendered for (a keyed list), or NULL
  bool soft;     // bound with bind_soft(): its control lives behind a branch, don't report it
} WHook;

static WHook g_hooks[WC_HOOK_MAX];
static i32 g_nhooks;

// The element and the item a control was rendered for, remembered on its hook so that
// wc_test_dispatch() can hand an action the same context the browser would.
static void record_hook_place(WHook *h) {
  WBuild *b = wbuild_current();
  if (!h) return;
  if (b && b->cur) h->node = b->cur;
  h->ctx = (b && b->item_ptr) ? (void *)b->item_ptr : nullptr;
}

// A client render, as opposed to server-side markup. `on_click(...)` attaches a listener
// only here: the server's node ids mean nothing in the browser, so an SSR page that
// carried `data-on-click` would be pointing at numbers that do not exist.
static bool g_in_client;
static bool g_interacted;
static i32 g_client_renders;

bool wc_in_client(void) { return g_in_client; }
void wc_client_mode(void) { g_in_client = true; }

// Called by the first render after the client half has taken over: a name that no element
// in the tree carries is a control that can never fire, and the build said nothing about
// it (that is the class of bug this reports — `bind("next_stroy", …)` compiled, ran, and
// did nothing at all).
// When is a binding dead? Not at startup: the name is bound after the mount render (the
// documented order) and the element it names may be created by a render that is a flush or
// two away — a list row, a control behind a fetch. Counting renders called those dead on
// the very render that gave them a home, which is a warning a developer learns to ignore.
//
// So the report waits for the thing a binding exists for: an interaction. The user clicks,
// a render follows, and if no element in *that* tree carries the name, then the control
// really cannot fire. A page nobody touches says nothing, which is also right — a name that
// no element carries has not cost anybody anything yet.
static void hooks_report(void) {
  g_client_renders++;
  if (!g_in_client || !g_interacted) return;
  for (i32 i = 0; i < g_nhooks; i++) {
    WHook *h = &g_hooks[i];
    if (h->seen || h->reported || h->soft) continue;
    h->reported = true;
    // a name with several bindings is one control: say it once
    for (i32 j = 0; j < g_nhooks; j++)
      if (strcmp(g_hooks[j].name, h->name) == 0) g_hooks[j].reported = true;
    // Two different failures wear this report, and they deserve different words:
    // a hook that has *never* rendered is a typo, and a hook that rendered once and
    // does not now is a conditional — the control lives behind a branch, and the
    // spelling for that is bind_soft. Reporting both as the same error taught
    // developers to ignore the report.
    const char *why = h->ever_seen
        ? "it rendered once and does not now, so its control is behind a branch — "
          "bind_soft() is the spelling for that"
        : "no render has ever carried it, which is usually a typo in bind() or in the "
          "view's named()";
    fprintf(stderr,
            "webc: bind(\"%s\") — no element carries that hook, so a click on it will "
            "never fire (%s).\n",
            h->name, why);
  }
}

// A click (or any event a person aimed at an element) reached the module: from here on a
// dead binding is worth saying out loud. `input` does not arm it — the bridge echoes a
// field's value at startup, without anybody touching the page, and arming on that would
// put us back to reporting on the mount render.
void wc_note_interaction(const char *event) {
  if (event && str_eq_c(str_lit(event), "input")) return;
  g_interacted = true;
}

typedef struct WBoundField {
  const char *name;
  void *sig;
} WBoundField;

static WBoundField g_fields[WC_HOOK_MAX];
static i32 g_nfields;

static void bind_hook(const char *name, const char *event, void *fn, bool soft) {
  if (g_nhooks >= WC_HOOK_MAX) {
    wc_limit_once(&g_nhooks, "client hook limit reached: this hook is not registered",
                  "WC_HOOK_MAX", "hook");
    return;
  }
  if (!name || !event) return;
  g_hooks[g_nhooks++] = (WHook){.name = name,
                                .event = event,
                                .fn = (WEventFn)fn,
                                .since = g_client_renders,
                                .soft = soft};
}

void wc_bind_hook(const char *name, const char *event, void *fn) {
  bind_hook(name, event, fn, false);
}

// A control that is only rendered sometimes: wired, but not on the page yet.
void wc_bind_hook_soft(const char *name, const char *event, void *fn) {
  bind_hook(name, event, fn, true);
}

void wc_bind_field(const char *name, void *sig) {
  if (g_nfields >= WC_HOOK_MAX) {
    wc_limit_once(&g_nfields, "bound field limit reached: this field is not reactive",
                  "WC_HOOK_MAX", "field");
    return;
  }
  if (!name) return;
  g_fields[g_nfields++] = (WBoundField){.name = name, .sig = sig};
}

static WHook *hook_find(const char *name) {
  if (!name) return nullptr;
  for (i32 i = 0; i < g_nhooks; i++)
    if (strcmp(g_hooks[i].name, name) == 0) return &g_hooks[i];
  return nullptr;
}

static void *field_find(const char *name) {
  if (!name) return nullptr;
  for (i32 i = 0; i < g_nfields; i++)
    if (strcmp(g_fields[i].name, name) == 0) return g_fields[i].sig;
  return nullptr;
}

// A named element: the attribute is always emitted (it is how the browser debugs a
// page, and how a test asserts a hook exists), and the listener is attached only
// when the client half bound that name. A name may carry SEVERAL events —
// bind("card", "click", …) and bind("card", "dragstart", …) on one element is the
// shape drag-and-drop wants, and a table keyed only on the name fires once, renders
// everything, and never fires again.
void wc_hook(const char *name) {
  wc_attr_c("data-hook", name);   // not data-wc: that one is the node id
  if (__builtin_expect(g_nhooks == 0, 1)) return;
  WBuild *b = wbuild_current();
  bool any = false;
  for (i32 i = 0; i < g_nhooks; i++) {
    WHook *h = &g_hooks[i];
    if (strcmp(h->name, name) != 0) continue;
    any = true;
    h->seen = true;
    h->ever_seen = true;
    if (b) {
      wc_on(h->event, h->fn, b->item_ptr);
      record_hook_place(h);
    }
  }
  (void)any;
}

// Fire the handler a name is bound to, through the same table the bridge uses by name.
// A native test cannot click; it can ask for the click.
// ---------------------------------------------------------------- the event's element
//
// A listener hears (ctx, event) and nothing else, so a row rendered by a positional list had
// no way to say which row it was: `for_each` hands the action its item, `for_i` hands it
// NULL, and there was no accessor for "the element this event is being delivered from". The
// workaround was a data-id attribute plus a `WebC.call('on_pick', {id})` dispatch from the
// page, in every row of every list.
//
// The dispatch path knows the node, so it keeps it here for the duration of the call.
static WDom *t_ev_node;
static str t_ev_value;
static WPoint t_ev_point;   // where the pointer was, when the event carried it
static bool t_ev_point_set;

void wc_ev_enter(WDom *n, str value) {
  t_ev_node = n;
  t_ev_value = value;
}

void wc_ev_leave(void) {
  t_ev_point_set = false;
  t_ev_point = (WPoint){0};
  t_ev_node = nullptr;
  t_ev_value = (str){0};
}

WDom *ev_self(void) { return t_ev_node; }

str ev_value(void) { return t_ev_value; }

// The one-liner every action that keeps what it was sent wants: the value is
// callback-scoped, and state that outlives the keystroke needs its own bytes.
Text ev_keep(void) {
  str v = t_ev_value;
  return v.n ? persist(text_of(v)) : (Text) "";
}

// ---------------------------------------------------------------- the pointer
//
// The bridge encodes a pointer event as the payload: a leading \x01, then six numbers. A
// string that does not start with it is an ordinary value (an input's text), which is what
// makes one payload field serve both without a union nobody can see.
WPoint ev_point(void) {
  if (t_ev_point_set) return t_ev_point;
  WPoint p = {0};
  str v = t_ev_value;
  if (!v.p || v.n < 2 || (u8)v.p[0] != 1) return p;
  char buf[192];
  size_t n = v.n - 1 < sizeof buf - 1 ? v.n - 1 : sizeof buf - 1;
  memcpy(buf, v.p + 1, n);
  buf[n] = 0;
  const char *q = buf;
  f64 *out[6] = {&p.x, &p.y, &p.w, &p.h, &p.cx, &p.cy};
  for (i32 i = 0; i < 6; i++) {
    char *end = nullptr;
    f64 d = strtod(q, &end);
    if (end == q) return (WPoint){0};
    *out[i] = d;
    q = (*end == ',') ? end + 1 : end;
  }
  return p;
}

WBox ev_box(void) {
  WPoint p = ev_point();
  return (WBox){p.w, p.h};
}

// A native test says where the pointer was the same way the bridge does.
bool wc_test_dispatch_at(const char *hook, const char *event, f64 x, f64 y, f64 w, f64 h) {
  t_ev_point = (WPoint){x, y, w, h, x, y};
  t_ev_point_set = true;
  bool ran = wc_test_dispatch(hook, event);
  t_ev_point_set = false;
  return ran;
}

// The nearest value of `name` at or above the element that was clicked: a click lands on a
// <span> inside the <button data-id="7"> the view meant, and the row is what the app wants.
// Returns an empty str when nothing carries it, which is also what an absent attribute is.
str ev_attr(const char *name) {
  if (!name || !*name) return (str){0};
  for (WDom *p = t_ev_node; p; p = p->parent)
    for (i32 i = 0; i < p->nattrs; i++)
      if (str_eq_c(p->attrs[i].name, name)) return p->attrs[i].value;
  return (str){0};
}

i64 ev_attr_i64(const char *name) {
  str v = ev_attr(name);
  if (!v.p || !v.n) return 0;
  // a leading sign is a number, not a terminator: the pager's Prev carries
  // data-step="-1", and a parser that stopped at the '-' reads it as 0: a Next that
  // works and a Prev that silently does nothing.
  bool neg = v.p[0] == '-';
  size_t i = neg || v.p[0] == '+' ? 1 : 0;
  i64 out = 0;
  bool any = false;
  for (; i < v.n; i++) {
    if (v.p[i] < '0' || v.p[i] > '9') break;
    out = out * 10 + (v.p[i] - '0');
    any = true;
  }
  if (!any) return 0;
  return neg ? -out : out;
}

// An action that declares an item and runs without one is the silent failure this whole
// section exists for: `bind()` passes the item only inside a keyed list, so a positional list
// gave every action NULL and the action did nothing at all.
void wc_action_no_item(const char *name, const void *id) {
  char msg[192];
  snprintf(msg, sizeof msg,
           "webc: action \"%s\" ran with no item — the list it lives in is not keyed "
           "(for_each), so use ev_attr(\"data-id\") instead of the argument",
           name ? name : "?");
  wc_limit_once(id, msg, "item", "action");
}

bool wc_test_dispatch(const char *hook, const char *event) {
  g_interacted = true;
  // a name may carry several events: find this name's binding for THIS event
  WHook *h = nullptr;
  for (i32 i = 0; i < g_nhooks && !h; i++)
    if (strcmp(g_hooks[i].name, hook) == 0 &&
        (!event || str_eq(str_lit(g_hooks[i].event), str_lit(event))))
      h = &g_hooks[i];
  if (!h || !h->fn) return false;
  // A native test has no DOM, so it can say which element it means by the hook's node, and
  // hand the action the item the element was rendered for.
  //
  // It also has to hand the action the two ambients the browser hands it: the instance the
  // node was built in (`with_inst`/`state_in`) and the render position that drew it
  // (`state_own`). Without that, an action that reads per-instance or per-row state worked
  // in a browser and panicked in a test — the worst place for a difference.
  WDom *node = h->node;
  WInst *inst = node ? (WInst *)node->inst : nullptr;
  WInst *owner = node ? (WInst *)node->owner : nullptr;
  if (inst) winst_push(inst);
  if (owner) winst_own_push(owner);
  wc_ev_enter(h->node, (str){0});
  h->fn(h->ctx, h->event);
  wc_ev_leave();
  if (owner) winst_own_pop();
  if (inst) winst_pop();
  return true;
}

// The hook, declared where the control is:
//
//   el("button") { class_("btn"); on_click(next_story); text("Next"); }
//
// `on_click(next_story)` means the element is named `next_story` in the markup (so a page
// and a test can see the wiring) and that the action runs on click — one fact, one line,
// and the function name is checked by the compiler instead of matched by a string in
// another file. `bind("next_story", "click", next_story)` in client_main still works, for a
// control whose name is decided at runtime.
void wc_on_click(const char *name, WEventFn fn) {
  if (!name || !*name) panic("on_click: the action has no name to bind under");
  wc_attr_c("data-hook", name);
  WHook *h = hook_find(name);
  if (!h) {
    wc_bind_hook(name, "click", (void *)fn);
    h = hook_find(name);
  }
  if (!h) return;
  h->seen = true;
  h->ever_seen = true;
  // Where the control is, recorded in *both* halves: a native test dispatches by name and
  // then asks ev_attr() which row it was, which needs the element, not just the listener.
  record_hook_place(h);
  if (!g_in_client) return; // SSR is markup: the listener belongs to the browser render
  WBuild *b = wbuild_current();
  wc_on("click", fn, b ? b->item_ptr : nullptr);
}

void wc_input_hook(const char *name) {
  // No attribute of its own: wc_bind_str already emits `data-bind="<name>"`, which is
  // both the bridge's marker and the debugging handle. Two attributes for one fact
  // would be one too many.
  void *sig = field_find(name);
  if (sig) {
    wc_bind_str(sig, name);
    return;
  }
  // The field is not registered *yet* — or never will be. Either way it looks like a working
  // input: the element renders, the human types, and the signal never moves, with nothing
  // said anywhere. If the render is already the browser's (client_main has returned), no
  // later bind_input() can arrive, so this is the mistake and it gets named.
  if (!wbuild_current() || wbuild_current()->ssr) return;
  dom_report_once(&g_field_missing_reported,
                  "webc: input_hook(\"%s\") has no bind_input(\"%s\") — this field will never "
                  "update. Write bind_str(%s) inside the view instead.",
                  name, name, name);
}

// Defined with the live bindings below; `bind_str` resolves a `state_own` marker the same way.
static Wn *wc_sig_resolve(Wn *sig);

void wc_bind_str(void *sig, const char *name) {
  // Two-way binding: the bridge echoes `input` events back with the field's value,
  // and wdom_apply_binding writes the signal. The attribute keeps SSR honest about
  // which inputs are live.
  wc_attr("data-bind", str_lit(name));
  if (!wbuild_current() || !wbuild_current()->cur) return;
  WDom *n = wbuild_current()->cur;
  sig = wc_sig_resolve((Wn *)sig); // state_own: this row's field, not the declaration
  n->bind_sig = sig;
  // Render the signal's value into the field, so clearing the signal clears the
  // box: reading it here also subscribes this render to the signal.
  if (sig) {
    Text *cur = w_read((Wn *)sig);
    if (cur) wc_attr("value", *cur ? str_lit(*cur) : str_lit(""));
  }
  bool have = false;
  for (i32 i = 0; i < n->nlisteners; i++)
    if (str_eq_c(n->listeners[i].event, "input")) have = true;
  if (!have) wc_on("input", nullptr, nullptr);
}

// The id space this tree needs: highest id in it, plus one. The bridge uses it to size
// the array it hands to wdom_index, so no node is silently left out.
i32 wdom_id_space(WDom *root) {
  i32 hi = 0;
  for (WDom *p = root; p;) {
    if (!p->dead && p->type != WD_FRAGMENT && p->id > hi) hi = p->id;
    if (p->first) {
      p = p->first;
      continue;
    }
    while (p && !p->next)
      p = p->parent;
    if (p) p = p->next;
  }
  return hi + 1;
}

i32 wdom_index(WDom *root, WDom **out, i32 cap) {
  i32 n = 0;
  for (WDom *p = root; p;) {
    if (!p->dead && p->type != WD_FRAGMENT && p->id < cap) out[p->id] = p;
    if (p->first) {
      p = p->first;
      continue;
    }
    while (p && !p->next)
      p = p->parent;
    if (p) p = p->next;
  }
  for (i32 i = 0; i < cap; i++)
    if (out[i]) n = i + 1;
  return n;
}

bool wdom_apply_binding(WDom *n, str value) {
  if (!n || !n->bind_sig) return false;
  Wn *sig = n->bind_sig;
  // The signal holds a str{ptr,len} that outlives this call, so the node keeps its own copy —
  // and it must be the *node's*: with one shared buffer every bound input on the page shows
  // the last value anyone typed.
  //
  // It must also be *two* buffers, used alternately. The signal aliases the node's buffer, so
  // growing it in place (`free` then `alloc`) frees the bytes the signal still points at:
  // the render that is in flight, or a view reading `get(field)`, then sees whatever the
  // allocator put there. In a real app that was four NULs on the front of a typed value, and
  // then `memory access out of bounds` when a length outlived its buffer — one trap, four
  // symptoms that all looked like "the form does nothing".
  //
  // Alternating gives every value one render of grace: the write goes into the other buffer,
  // the previous one stays readable until the next write, exactly like the render arena's
  // double buffer. Memory cost is one short string per bound field.
  i32 cur = n->bind_parity & 1, next = cur ^ 1;
  i32 need = (i32)value.n + 1;
  if (n->bind_len[next] < need) {
    char *grown = wm_alloc((size_t)need);
    if (!grown) return false;
    wm_free(n->bind_val[next]);
    n->bind_val[next] = grown;
    n->bind_len[next] = need;
  }
  if (value.n && value.p) memcpy(n->bind_val[next], value.p, value.n);
  n->bind_val[next][value.n] = 0;
  n->bind_parity = next;
  // The slot holds a Text (a NUL-terminated pointer), not a str{ptr,len}: the node's buffer
  // is already NUL-terminated above, so the pointer is what the state stores and what a view
  // reading `get(field)` gets back.
  char *held = n->bind_val[next];
  w_write(sig, &held, sizeof held);
  return true;
}

// ---------------------------------------------------------------- keyed lists

typedef struct WItemEntry {
  i64 key;
  bool used; // present in the current run
  bool live;
  bool ever; // has been occupied at some point: a tombstone, not the end of a chain
  WDom *node;
  WScope scope;
  WInst *owner; // render-owned state (`state_own`) of this row, or null until it reads one.
                // A pointer, not an embedded struct: item_put() rehashes the table and
                // copies entries by value, and the nodes in the row point back at it.
  i32 order;
} WItemEntry;

typedef struct WItemTable {
  WItemEntry *e;
  i32 cap, n;
} WItemTable;

// Linear probing that stops at the first slot which has never held a key. Sweeping a
// list leaves tombstones, so a dead-but-once-used slot cannot end the probe; a
// never-used slot can, and that is what keeps a render with n new keys O(n) rather
// than the O(n^2) a full-table scan costs (8 000 rows: 108 ms against 0.6 ms).
static WItemEntry *item_find(WItemTable *t, i64 key) {
  if (!t || !t->cap) return nullptr;
  u64 h = wc_hash_i64(key) & (u64)(t->cap - 1);
  for (i32 probe = 0; probe < t->cap; probe++) {
    WItemEntry *e = &t->e[(h + (u64)probe) & (u64)(t->cap - 1)];
    if (!e->live) {
      if (!e->ever) return nullptr;
      continue;
    }
    if (e->key == key) return e;
  }
  return nullptr;
}

static WItemEntry *item_put(WBuild *b, WItemTable *t, i64 key) {
  if (!t->cap || (t->n + 1) * 10 >= t->cap * 7) {
    i32 ncap = t->cap ? t->cap * 2 : 16;
    WItemEntry *ne = arena_zalloc(b->nodes_arena, (size_t)ncap * sizeof(WItemEntry));
    for (i32 i = 0; i < t->cap; i++) {
      if (!t->e[i].live) continue;
      u64 h = wc_hash_i64(t->e[i].key) & (u64)(ncap - 1);
      while (ne[h & (u64)(ncap - 1)].live)
        h++;
      ne[h & (u64)(ncap - 1)] = t->e[i];
    }
    t->e = ne;
    t->cap = ncap;
  }
  u64 h = wc_hash_i64(key) & (u64)(t->cap - 1);
  while (t->e[h].live && t->e[h].key != key)
    h = (h + 1) & (u64)(t->cap - 1);
  if (!t->e[h].live) {
    t->e[h] = (WItemEntry){.key = key, .live = true, .ever = true};
    t->n++;
  }
  return &t->e[h];
}

WInst *wkeyed_item(i64 key) {
  WBuild *b = wbuild_current();
  if (!b || !b->cur) return nullptr; // nothing opened, so wkeyed_end(NULL) pops nothing
  WDom *parent = b->cur;
  if (!parent->items) parent->items = n_alloc(b, sizeof(WItemTable));
  WItemTable *t = parent->items;
  WItemEntry *e = item_find(t, key);
  // A key drawn twice in one run is a row the app cannot tell apart, and before
  // this sentence it was worse than wrong: the second draw re-appended the first
  // draw's fragment and the tree grew a node that was its own sibling — every
  // walk spun, and the page never answered. The key is the model's `.pk`, whose
  // zero is the unset value, which is exactly what a hand-built row forgets.
  if (e && e->used)
    panic("dom: a keyed list drew key %lld twice — the key is the model's .pk, and 0 "
          "is its unset value; two rows share a key (give them distinct ids, or read "
          "them from the table)",
          (long long)key);
  if (!e) {
    e = item_put(b, t, key);
    e->node = node_new(b, WD_FRAGMENT, "g");
    w_scope_enter(&e->scope, b->scope);
    e->owner = winst_alloc(); // a row is a render position: `state_own` lives here
  } else {
    e->node->first = e->node->last = nullptr;
    e->node->seen = b->run; // alive this run: not a remove, and eligible to move
    e->node->is_new = false;
    // The fragment is in this run's tree as much as it was on the render that created it:
    // wc_render_assert() budgets its walk by nnodes + 64, so an uncounted fragment per row
    // puts a keyed list of more than 64 rows on a reported "cycle" on its second render.
    b->nnodes++;
  }
  // An item fragment is a node like any other: it joins this render's sequence so
  // the *next* render can match its rows back to it (per-key reuse) and see when
  // the item changed place in the list (a `move`). A fragment left out of the
  // sequence makes reuse fall back to the global cursor.
  seq_push(b, e->node);
  // the parent starts each run with an empty child list, so every item is
  // re-attached, whether it is new or reused
  append_child(parent, e->node);
  e->used = true;
  e->node->is_new = e->node->is_new && e->node->first == nullptr;
  b->cur = e->node;
  b->depth++;
  // The row draws until wkeyed_end(): while it does, it is the ambient owner, so
  // `state_own` reads in the block (and the nodes it creates) belong to this row.
  winst_own_push(e->owner);
  return e->owner;
}

// Closes one item: back to the element that owns the item table. The unused
// sweep waits for the end of the list (wkeyed_list), because during the loop the
// items after this one have not been marked yet.
void wkeyed_end(WInst *own) {
  // `own` is the owner wkeyed_item() pushed for this row. It is passed in rather than read
  // back off the ambient stack: the row body may have left that stack unbalanced, and then
  // the top is not this row (or is a released instance), which is exactly the use-after-free
  // this signature exists to prevent.
  WBuild *b = wbuild_current();
  if (b && b->cur) {
    WDom *item = b->cur;
    if (item->parent) b->cur = item->parent;
    if (b->depth > 0) b->depth--;
  }
  // A row is a render position, so the components it drew are swept when the row's body
  // ends — before the loop moves on, so a widget in row 3 can never claim row 2's state.
  if (own) {
    winst_sweep_children(own);
    own->rendered = true;
  }
  winst_own_pop_to(own); // matched pop: repairs an unbalanced stack instead of trusting it
}

void wkeyed_begin(void) {}

// Disposes every item of `parent` that this render did not claim.
static void keyed_sweep(WBuild *b, WDom *parent) {
  WItemTable *t = parent ? parent->items : nullptr;
  if (!t) return;
  for (i32 i = 0; i < t->cap; i++) {
    WItemEntry *e = &t->e[i];
    if (!e->live || e->used) continue;
    w_scope_dispose(&e->scope);
    // A row that left takes its render-owned state with it: the slots are killed and the
    // reactive graph shrinks back, which is what lets a list churn rows for hours.
    winst_release(e->owner);
    e->owner = nullptr;
    if (e->node) {
      // not claimed this run: it stays out of the tree, and every node inside it
      // is removed from the browser because the patch removes whatever this run
      // did not touch
      e->node->parent = nullptr;
      e->node->prev = e->node->next = nullptr;
      e->node->dead = true;
    }
    if (e->live) {
      e->live = false;
      e->ever = true; // tombstone: item_find may not stop here
      if (t->n > 0) t->n--;
    }
    e->node = nullptr;
  }
  for (i32 i = 0; i < t->cap; i++)
    if (t->e[i].live) t->e[i].used = false;
  (void)b;
}

// End of a block-shaped keyed list (fe.h `for_each`): dispose the items this
// render did not claim.
void wkeyed_sweep_here(void) {
  WBuild *b = wbuild_current();
  if (b && b->cur) keyed_sweep(b, b->cur);
}

void wkeyed_list(i32 count, WKeyFn key_at, WItemFn item, void *ctx) {
  WBuild *b = wbuild_current();
  if (!b || !b->cur) return;
  WDom *parent = b->cur;
  for (i32 i = 0; i < count; i++) {
    WInst *own = wkeyed_item(key_at(ctx, i));
    item(ctx, i, b);
    wkeyed_end(own);
  }
  keyed_sweep(b, parent);
}

// ---------------------------------------------------------------- SSR

// Void elements are decided by first letter and length, not by a 13-entry strcmp scan
// per element: this runs once per element per response on the SSR path.
static bool tag_is_void(const char *tag) {
  if (!tag) return false;
  switch (tag[0]) {
  case 'a': return strcmp(tag, "area") == 0;
  case 'b': return strcmp(tag, "br") == 0 || strcmp(tag, "base") == 0;
  case 'c': return strcmp(tag, "col") == 0;
  case 'e': return strcmp(tag, "embed") == 0;
  case 'h': return strcmp(tag, "hr") == 0;
  case 'i': return strcmp(tag, "img") == 0 || strcmp(tag, "input") == 0;
  case 'l': return strcmp(tag, "link") == 0;
  case 'm': return strcmp(tag, "meta") == 0;
  case 's': return strcmp(tag, "source") == 0;
  case 't': return strcmp(tag, "track") == 0;
  case 'w': return strcmp(tag, "wbr") == 0;
  default: return false;
  }
}

static void render_ssr(Buf *out, WDom *n) {
  for (; n; n = n->next) {
    if (n->dead) continue;
    switch (n->type) {
    case WD_TEXT:
      // escaped straight into the response: going through a scratch buffer would
      // allocate and copy every text node twice
      wc_html_esc_buf(out, n->text);
      break;
    case WD_RAW:
      buf_putstr(out, n->text);
      break;
    case WD_FRAGMENT:
      render_ssr(out, n->first);
      break;
    case WD_ELEMENT: {
      bool is_void = tag_is_void(n->tag);
      buf_printf(out, "<%s data-wc=\"%d\"", n->tag, n->id);
      for (i32 i = 0; i < n->nattrs; i++) {
        buf_printf(out, " %.*s=\"", (int)n->attrs[i].name.n, n->attrs[i].name.p);
        wc_html_esc_buf(out, n->attrs[i].value);
        buf_putc(out, '"');
      }
      for (i32 i = 0; i < n->nlisteners; i++)
        buf_printf(out, " data-on-%.*s=\"%d\"", (int)n->listeners[i].event.n,
                   n->listeners[i].event.p, n->id);
      if (is_void) {
        buf_puts(out, ">");
        break;
      }
      buf_puts(out, ">");
      render_ssr(out, n->first);
      buf_printf(out, "</%s>", n->tag);
      break;
    }
    }
  }
}

str wdom_html(WBuild *b) {
  if (!b) return (str){"", 0};
  Buf out;
  buf_init(&out, b->a, 1024);
  render_ssr(&out, b->root);
  return buf_str(&out);
}

str webc_render(WBuild *b) {
  return wdom_html(b);
}

// ---------------------------------------------------------------- DOM commands

static void cmd(Buf *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void cmd(Buf *out, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  buf_vprintf(out, fmt, ap);
  va_end(ap);
  buf_putc(out, '\n'); // one op per line; the caller counts them
}

// ---------------------------------------------------------------- live bindings
//
// A binding is an effect whose body writes one DOM op. It owns no structure: the node it
// targets already exists (a text node for `text_live`, the open element for the rest), and
// the op it emits is the line the patcher would have emitted for that node — `text <id> …`
// or `attr <id> <name> …`. So the bridge gains no verb, and the two paths cannot disagree
// about syntax. What the binding removes is the *render*: when the signal is written, no
// view body runs, no tree is walked and no node is matched.
//
// Soundness, in one sentence: a binding only ever writes a leaf (text, one attribute), and
// anything that can add or remove a node still belongs to the render, which the bridge runs
// after every event unless the action declared itself `action_live`.

typedef enum WLiveKind : u8 { WL_TEXT, WL_ATTR, WL_CLASS, WL_SHOW } WLiveKind;

typedef struct WLive {
  struct WLive *next; // the chain on the target node
  Wn *node;           // the effect that owns the dependency edge
  WDom *target;
  WDomBatch *out;  // the component's batch
  Wn *sig;
  const char *attr;  // the DOM attribute written (NULL for text)
  const char *param; // the format (text) or the class name (class)
  u8 kind, valkind;
  i32 run;    // the render that last asked for this binding
  bool first; // record the value, do not emit (attach and every re-render)
  i32 par;    // which value buffer is current
  Buf val[2]; // the last two values, alternated: the node points at one of them
  char *base; // class/show: the view's own value, without our token
  size_t base_cap;
} WLive;

static str wlive_attr_get(WDom *n, const char *attr) {
  for (i32 i = 0; i < n->nattrs; i++)
    if (str_eq_c(n->attrs[i].name, attr)) return n->attrs[i].value;
  return (str){0};
}

// The node's own copy is what a later render diffs against, so a binding that changes the
// DOM has to change this too — otherwise "set, then set back" compares equal to the last
// render and the page keeps the value in the middle.
static void wlive_attr_put(WDom *n, const char *attr, str v) {
  for (i32 i = 0; i < n->nattrs; i++)
    if (str_eq_c(n->attrs[i].name, attr)) {
      n->attrs[i].value = v;
      return;
    }
}

static void wlive_put_typed(Buf *b, u8 k, const void *v) {
  switch (k) {
    case WLV_I32: buf_printf(b, "%d", *(const i32 *)v); break;
    case WLV_U32: buf_printf(b, "%u", *(const u32 *)v); break;
    case WLV_I64: buf_printf(b, "%lld", (long long)*(const i64 *)v); break;
    case WLV_U64: buf_printf(b, "%llu", (unsigned long long)*(const u64 *)v); break;
    case WLV_F64: buf_printf(b, "%g", *(const f64 *)v); break;
    case WLV_F32: buf_printf(b, "%g", (double)*(const f32 *)v); break;
    case WLV_BOOL: buf_puts(b, *(const bool *)v ? "true" : "false"); break;
    case WLV_TEXT: {
      const char *t = v ? *(const char *const *)v : nullptr;
      buf_puts(b, t ? t : "");
      break;
    }
    default: buf_puts(b, "??live-binding type??"); break;
  }
}

static void wlive_put_fmt(Buf *b, const char *fmt, u8 k, const void *v) {
  switch (k) {
    case WLV_I32: buf_printf(b, fmt, *(const i32 *)v); break;
    case WLV_U32: buf_printf(b, fmt, *(const u32 *)v); break;
    case WLV_I64: buf_printf(b, fmt, (long long)*(const i64 *)v); break;
    case WLV_U64: buf_printf(b, fmt, (unsigned long long)*(const u64 *)v); break;
    case WLV_F64: buf_printf(b, fmt, *(const f64 *)v); break;
    case WLV_F32: buf_printf(b, fmt, (double)*(const f32 *)v); break;
    case WLV_BOOL: buf_printf(b, fmt, (int)*(const bool *)v); break;
    case WLV_TEXT: buf_printf(b, fmt, v ? *(const char *const *)v : ""); break;
    default: buf_puts(b, "??live-binding type??"); break;
  }
}

static bool wlive_truth(u8 k, const void *v) {
  if (!v) return false;
  switch (k) {
    case WLV_I32: return *(const i32 *)v != 0;
    case WLV_U32: return *(const u32 *)v != 0;
    case WLV_I64: return *(const i64 *)v != 0;
    case WLV_U64: return *(const u64 *)v != 0;
    case WLV_F64: return *(const f64 *)v != 0;
    case WLV_F32: return *(const f32 *)v != 0;
    case WLV_BOOL: return *(const bool *)v;
    case WLV_TEXT: {
      const char *t = *(const char *const *)v;
      return t && *t;
    }
    default: return false;
  }
}

static void wlive_emit(WLive *bd, str v) {
  if (!bd->out || !bd->target || bd->target->id <= 0) return;
  Buf *out = &bd->out->b;
  if (bd->kind == WL_TEXT)
    buf_printf(out, "text %d ", bd->target->id);
  else
    buf_printf(out, "attr %d %s ", bd->target->id, bd->attr);
  wc_line_esc(out, v);
  buf_putc(out, '\n');
  bd->out->ops++;
}

static void wlive_commit(WLive *bd, str v) {
  if (bd->kind == WL_TEXT)
    bd->target->text = v;
  else
    wlive_attr_put(bd->target, bd->attr, v);
}

static void wlive_run(Wn *self, void *ctx) {
  WLive *bd = ctx;
  (void)self;
  if (!bd->target || bd->target->dead) return;
  const void *v = w_read(bd->sig);
  i32 np = bd->par ^ 1;
  Buf *b = &bd->val[np];
  buf_reset(b);
  switch (bd->kind) {
    case WL_TEXT: wlive_put_fmt(b, bd->param, bd->valkind, v); break;
    case WL_ATTR: wlive_put_typed(b, bd->valkind, v); break;
    case WL_CLASS: {
      bool on = wlive_truth(bd->valkind, v);
      if (bd->base && *bd->base) buf_puts(b, bd->base);
      if (on) {
        if (bd->base && *bd->base) buf_putc(b, ' ');
        buf_puts(b, bd->param);
      }
      break;
    }
    case WL_SHOW: {
      bool on = wlive_truth(bd->valkind, v);
      if (bd->base && *bd->base) buf_puts(b, bd->base);
      if (!on) {
        if (bd->base && *bd->base) buf_putc(b, ';');
        buf_puts(b, "display:none");
      }
      break;
    }
  }
  str nv = buf_str(b);
  str ov = buf_str(&bd->val[bd->par]);
  bool same = str_eq(ov, nv);
  bool first = bd->first;
  bd->first = false;
  bd->par = np;
  if (!first && same) return; // a write that changes nothing costs nothing below this line
  wlive_commit(bd, nv);
  if (!first) wlive_emit(bd, nv);
}

static void wlive_set_base(WLive *bd, str base) {
  size_t need = base.n + 1;
  if (need > bd->base_cap) {
    bd->base = wm_realloc(bd->base, need);
    bd->base_cap = need;
  }
  if (base.p && base.n) memcpy(bd->base, base.p, base.n);
  bd->base[base.n] = 0;
}

// What the view wrote, without the token this binding owns: the class a list row carries
// (`class_("row")`), or the style a bar was drawn with. Re-read every render, so a view that
// changes its own classes/styles keeps working under a live binding.
static void wlive_capture_base(WLive *bd) {
  str cur = wlive_attr_get(bd->target, bd->attr);
  if (!cur.p) cur.n = 0;
  if (bd->kind == WL_CLASS) {
    size_t cl = strlen(bd->param);
    if (cur.n >= cl && memcmp(cur.p + cur.n - cl, bd->param, cl) == 0) {
      size_t n = cur.n - cl;
      if (n > 0 && cur.p[n - 1] == ' ') n--;
      wlive_set_base(bd, (str){cur.p, n});
      return;
    }
    wlive_set_base(bd, cur);
    return;
  }
  const char *suf = "display:none";
  size_t sl = strlen(suf);
  if (cur.n >= sl && memcmp(cur.p + cur.n - sl, suf, sl) == 0) {
    size_t n = cur.n - sl;
    if (n > 0 && cur.p[n - 1] == ';') n--;
    wlive_set_base(bd, (str){cur.p, n});
    return;
  }
  wlive_set_base(bd, cur);
}

static WLive *wlive_find(WDom *n, WLiveKind kind, const char *attr, const char *param) {
  for (WLive *b = n->live; b; b = b->next) {
    if (b->kind != kind) continue;
    if (!b->attr != !attr) continue;
    if (b->attr && strcmp(b->attr, attr) != 0) continue;
    if (!b->param != !param) continue;
    if (b->param && strcmp(b->param, param) != 0) continue;
    return b;
  }
  return nullptr;
}

static void wlive_kill(WLive **link) {
  WLive *bd = *link;
  *link = bd->next;
  if (bd->node) w_kill(bd->node);
  buf_free(&bd->val[0]);
  buf_free(&bd->val[1]);
  if (bd->base) wm_free(bd->base);
  wm_free(bd);
}

static void wlive_kill_all(WDom *n) {
  while (n->live) wlive_kill((WLive **)&n->live);
}

// Every render asks for its bindings again. One that was not asked for this time belongs to
// a branch that is gone (or to an attribute the view stopped writing), so it dies here —
// otherwise a toggle would leave a live node editing an element nobody can see.
static void wlive_sweep(WDom *n, i32 run) {
  WLive **link = (WLive **)&n->live;
  while (*link) {
    if ((*link)->run != run)
      wlive_kill(link);
    else
      link = &(*link)->next;
  }
}

static void wlive_attach(WDom *n, WLiveKind kind, const char *attr, const char *param, Wn *sig,
                         u8 valkind, WDomBatch *out, i32 run) {
  WLive *bd = wlive_find(n, kind, attr, param);
  if (!bd) {
    bd = wm_alloc(sizeof *bd);
    memset(bd, 0, sizeof *bd);
    buf_init(&bd->val[0], nullptr, 64);
    buf_init(&bd->val[1], nullptr, 64);
    bd->kind = (u8)kind;
    bd->attr = attr;
    bd->param = param;
    bd->target = n;
    bd->node = w_effect_new(wlive_run, bd, kind == WL_TEXT ? "text_live" : (attr ? attr : "live"));
    bd->next = n->live;
    n->live = bd;
  }
  bd->sig = sig;
  bd->valkind = valkind;
  bd->out = out;
  bd->run = run;
  if (kind == WL_CLASS || kind == WL_SHOW) wlive_capture_base(bd);
  bd->first = true; // this run records the value; the batch already carries it
  w_effect_run(bd->node);
}

// The signal's value, without registering a dependency: drawing the *initial* markup must
// not make the render an observer (the view is re-rendered by the bridge, not by the graph).
static const void *wc_sig_value(Wn *sig) { return sig ? sig->val : nullptr; }

// A `state_own` declaration is a marker, not a node (react.h `W_MARK_OWN`): the binding
// holds the *declaration*, and this is where it becomes the node of the position that is
// drawing right now. That one line is what makes `text_live` inside a `for_each` mean one
// value per row instead of one value for the list; `panic`s (naming the declaration) when
// there is no position, because a binding to the wrong row is worse than a loud failure.
static Wn *wc_sig_resolve(Wn *sig) {
  if (!sig || sig->kind != W_MARK_OWN) return sig;
  return winst_own_node((const WDecl *)sig->ctx);
}

void wc_live(const char *verb, const char *param, Wn *sig, u8 valkind) {
  WBuild *b = wbuild_current();
  if (!b || !b->cur || !verb || !sig) return;
  sig = wc_sig_resolve(sig); // state_own: this position's node, not the declaration
  if (!sig) return;
  bool is_text = strcmp(verb, "text") == 0;
  bool is_attr = strcmp(verb, "attr") == 0;
  bool is_class = strcmp(verb, "class") == 0;
  bool is_show = strcmp(verb, "show") == 0;
  if (!is_text && !is_attr && !is_class && !is_show) return;
  if ((is_text || is_attr || is_class) && !param) return;

  WDom *target = b->cur;
  const void *v = wc_sig_value(sig);
  Buf tmp;
  buf_init(&tmp, b->a, 64);

  if (is_text) {
    if (v) wlive_put_fmt(&tmp, param, valkind, v);
    target = add_text_node(b->cur, buf_str(&tmp), WD_TEXT); // the text node is the target
    if (!target) return;
  } else if (is_attr) {
    if (v) wlive_put_typed(&tmp, valkind, v);
    wc_attr(param, buf_str(&tmp));
  } else if (is_class) {
    str base = wlive_attr_get(target, "class");
    if (base.n) buf_putstr(&tmp, base);
    if (v && wlive_truth(valkind, v)) {
      if (tmp.n) buf_putc(&tmp, ' ');
      buf_puts(&tmp, param);
    }
    wc_attr("class", buf_str(&tmp));
  } else { // show: the style the view drew, plus display:none when the flag is false
    str base = wlive_attr_get(target, "style");
    if (base.n) buf_putstr(&tmp, base);
    if (!v || !wlive_truth(valkind, v)) {
      if (tmp.n) buf_putc(&tmp, ';');
      buf_puts(&tmp, "display:none");
    }
    wc_attr("style", buf_str(&tmp));
  }

  // The server half and a plain HTML render get the value and nothing else: a live node
  // without a browser is a subscription nobody will ever read.
  if (!wc_in_client() || !b->out) return;
  if (is_text)
    wlive_attach(target, WL_TEXT, nullptr, param, sig, valkind, b->out, b->run);
  else if (is_attr)
    wlive_attach(target, WL_ATTR, param, nullptr, sig, valkind, b->out, b->run);
  else if (is_class)
    wlive_attach(target, WL_CLASS, "class", param, sig, valkind, b->out, b->run);
  else
    wlive_attach(target, WL_SHOW, "style", nullptr, sig, valkind, b->out, b->run);
}

// ---------------------------------------------------------------- live actions
//
// `action_live` marks its run. The bridge counts the listeners that ran and the marks; when
// every one of them was marked, it sends the bindings' ops and skips the re-render. Without
// a mark, every mounted view re-renders.
static i32 g_live_marks;
void wc_live_mark(void) { g_live_marks++; }
void wc_live_reset(void) { g_live_marks = 0; }
i32 wc_live_marks(void) { return g_live_marks; }

static void emit_create(WDomBatch *cmds, WDom *n, i32 parent_id) {
  if (n->type == WD_ELEMENT) {
    cmd(&cmds->b, "create %d %d %s", n->id, parent_id, n->tag);
    for (i32 i = 0; i < n->nattrs; i++) {
      buf_printf(&cmds->b, "attr %d %.*s ", n->id, (int)n->attrs[i].name.n, n->attrs[i].name.p);
      wc_line_esc(&cmds->b, n->attrs[i].value);
      buf_putc(&cmds->b, '\n');
    }
    for (i32 i = 0; i < n->nlisteners; i++)
      cmd(&cmds->b, "listen %d %.*s", n->id, (int)n->listeners[i].event.n, n->listeners[i].event.p);
  } else if (n->type == WD_TEXT) {
    cmd(&cmds->b, "create %d %d #text", n->id, parent_id);
    buf_printf(&cmds->b, "text %d ", n->id);
    wc_line_esc(&cmds->b, n->text);
    buf_putc(&cmds->b, '\n');
  } else if (n->type == WD_RAW) {
    // Markup, not text: `raw()` on the server renders it as HTML, so it needs its own
    // command — sent as a text node it arrives in the browser as its own source, `<svg …>`
    // written out letter by letter. The bridge keeps a comment in the tree to hold the
    // position and puts the parsed markup there.
    buf_printf(&cmds->b, "raw %d %d ", n->id, parent_id);
    wc_line_esc(&cmds->b, n->text);
    buf_putc(&cmds->b, '\n');
  }
}

// A portal is an element carrying where it wants to live; the bridge moves it
// once its children exist. Emitting it after the subtree keeps the order right.
static void emit_portal(WDomBatch *cmds, WDom *n) {
  if (!n->attrs) return;
  for (i32 i = 0; i < n->nattrs; i++)
    if (n->attrs[i].name.n == 2 && memcmp(n->attrs[i].name.p, "to", 2) == 0) {
      buf_printf(&cmds->b, "portal %d ", n->id);
      wc_line_esc(&cmds->b, n->attrs[i].value);
      buf_putc(&cmds->b, '\n');
      cmds->ops++;
      return;
    }
}

static void emit_tree(WDomBatch *cmds, WDom *n, i32 parent_id) {
  for (; n; n = n->next) {
    if (n->dead) continue;
    if (n->type == WD_FRAGMENT) {
      emit_tree(cmds, n->first, parent_id);
      continue;
    }
    emit_create(cmds, n, parent_id);
    emit_tree(cmds, n->first, n->id);
    if (n->tag && strcmp(n->tag, "#portal") == 0) emit_portal(cmds, n);
  }
}

// ---------------------------------------------------------------- invariants
//
// The render tree is walked by three different pieces of code (emit, index, the
// browser's op stream) and every one of them trusts it: a `next` that points back
// at an ancestor is an infinite loop in all three. So the walk that can hear about
// it is bounded by the number of nodes this run produced — a cycle cannot be bigger
// than that — and reports the node instead of hanging.
static const char *tag_of(WDom *n) {
  return n->tag ? n->tag : (n->type == WD_FRAGMENT ? "#fragment" : "#text");
}

// Every attribute a render puts in the tree must have a name. A pair without one reached the
// browser as `setAttribute('')` and cost the whole mount batch, and the damaged pair was only
// ever *visible* on the wire — the tree it came from looked fine from outside. Checking it
// rides along the walk wc_render_assert() already does, so it costs one branch per attribute.
static void check_attrs(WDom *p) {
  for (i32 i = 0; i < p->nattrs; i++) {
    const WAttrPair *a = &p->attrs[i];
    if (a->name.p && a->name.n > 0 && a->name.p[0]) continue;
#ifdef __wasm__
    dom_report_once(&g_attr_scan_reported,
                    "dom: <%s id=%d> attribute #%d has no name — a render wrote a damaged "
                    "pair; the batch will skip it",
                    tag_of(p), p->id, i);
#else
    panic("dom: <%s id=%d> attribute #%d has no name (a render wrote a damaged pair)", tag_of(p),
          p->id, i);
#endif
  }
}

static void dom_check_walk(WDom *root, i64 budget) {
  i64 steps = 0;
  for (WDom *p = root; p;) {
    check_attrs(p);
    if (++steps > budget)
      panic("dom: the render tree has a cycle (%lld nodes walked, no end); a view that "
            "returns two top-level elements, or a reused fragment left in the tree, does this",
            (long long)steps);
    if (p->first) {
      if (p->first->parent != p || p->last->parent != p)
        panic("dom: <%s> owns a child whose parent is not <%s>", p->tag ? p->tag : "#text",
              p->tag ? p->tag : "#text");
      if (p->first->prev)
        panic("dom: <%s>'s first child has a previous sibling", p->tag ? p->tag : "#text");
      p = p->first;
      continue;
    }
    while (p && !p->next) p = p->parent;
    if (p) {
      if (p->next->prev != p)
        panic("dom: <%s id=%d> next is <%s id=%d>, whose prev is <%s id=%d> (stale sibling link: "
              "a node kept a link from the previous render)",
              tag_of(p), p->id, tag_of(p->next), p->next->id,
              p->next->prev ? tag_of(p->next->prev) : "none",
              p->next->prev ? p->next->prev->id : 0);
      p = p->next;
    }
  }
}

void wc_render_assert(WBuild *b) {
  if (!b || !b->root) return;
  // Every node in the tree was created or reused this run, so the count is an exact
  // bound: a walk that passes it is not walking a tree. 64 of slack covers the
  // container and any fragment the build wrapped around the roots.
  dom_check_walk(b->root, (i64)b->nnodes + 64);
}

static i32 batch_lines(Buf *b) {
  i32 n = 0;
  for (size_t i = 0; i < b->n; i++)
    if (b->p[i] == '\n') n++;
  return n;
}

// A batch that is bigger than any real page is a render that did not converge. It is
// reported with the tail of the stream, because "ops=100000" alone tells a developer
// nothing about which node repeated.
static void batch_budget(WDomBatch *out, const char *mode) {
  const char *env = getenv("WEBC_OPS_MAX");
  i64 limit = env && *env ? atoll(env) : 20000;
  if (limit <= 0) return;
  i32 ops = batch_lines(&out->b);
  if ((i64)ops <= limit) return;
  str tail = buf_str(&out->b);
  size_t from = tail.n > 400 ? tail.n - 400 : 0;
  panic("dom: render did not converge: %d ops in one %s batch (limit %lld, WEBC_OPS_MAX).\n"
        "      last ops:\n%.*s",
        (int)ops, mode, (long long)limit, (int)(tail.n - from), tail.p + from);
}

void wdom_mount_batch(WBuild *b, WDomBatch *out) {
  out->ops = 0;
  if (!b || !b->root) return;
  // create parents before children: a pre-order walk is already in that order
  emit_tree(out, b->root, 0);
  out->ops++;
  batch_budget(out, "mount");
}

// Nodes created by this render (new keyed items, a branch that just appeared)
// have no counterpart in the DOM yet; emit them before the diffs.
static void emit_new(WDomBatch *cmds, WDom *n, i32 parent_id) {
  for (; n; n = n->next) {
    if (n->dead) continue;
    if (n->type == WD_FRAGMENT) {
      emit_new(cmds, n->first, parent_id);
      continue;
    }
    if (n->is_new) {
      emit_create(cmds, n, parent_id);
      emit_new(cmds, n->first, n->id);
      if (n->tag && strcmp(n->tag, "#portal") == 0) emit_portal(cmds, n);
    } else {
      emit_new(cmds, n->first, n->id);
    }
  }
}

void wdom_patch_batch(WBuild *b, WDomBatch *out) {
  out->ops = 0;
  if (!b) return;
  emit_new(out, b->root, 0);
  // Nodes that were reused keep their id; only dirty text/attrs and new nodes
  // produce operations. This is the "one text node patched" proof.
  for (i32 i = 0; i < b->nreuse; i++) {
    WDom *n = b->reuse[i];
    if (!n) continue;
    if (n->seen != b->run) {
      // gone from this render: the browser still has it (fragments never made
      // it to the browser, so they have nothing to remove)
      if (n->id > 0) cmd(&out->b, "remove %d", n->id);
      wlive_kill_all(n); // the node is gone; so is anything that would write into it
      continue;
    }
    if (n->is_new) continue;
    wlive_sweep(n, b->run);
    // A reused keyed item keeps its nodes but its *place* in the list is this
    // render's decision: a fragment that moved (or landed under a different
    // fragment neighbour) is a `move` for each of its element children, with
    // the next fragment's first element as the anchor. Until this op existed,
    // reuse was positional — a reorder rewrote every row's text, and DOM state
    // inside a row (focus, selection) was lost (tests/keyed_move_test.c).
    // "Moved" means its place changed: a different sibling before it, or a
    // different *element* for a parent. A wrapper fragment is re-created every
    // render, so a root whose parent went old-wrapper -> new-wrapper did not
    // move (tests/dom_roots_test.c); an item's fragment parent never changes,
    // and the fragment itself carries the order change.
    bool moved_parent = n->parent != n->was_parent &&
                        (!n->parent || n->parent->type != WD_FRAGMENT) &&
                        (!n->was_parent || n->was_parent->type != WD_FRAGMENT);
    if (n->type == WD_ELEMENT && n->id > 0 && (n->was_prev != n->prev || moved_parent)) {
      i32 pid = n->parent && n->parent->id > 0 ? n->parent->id : 0;
      i32 before = 0;
      for (WDom *anchor = n->next; anchor && !before; anchor = anchor->next)
        if (anchor->id > 0 && !anchor->is_new) before = anchor->id;
        else
          for (WDom *f = anchor->first; f && !before; f = f->first)
            if (f->id > 0 && !f->is_new) before = f->id;
      cmd(&out->b, "move %d %d %d", n->id, pid, before);
    }
    if (n->type == WD_FRAGMENT && (n->was_prev != n->prev || n->was_parent != n->parent)) {
      i32 pid = n->parent && n->parent->id > 0 ? n->parent->id : 0;
      // where the browser should stop inserting: the first element after this
      // fragment that it already knows (0 = "at the end of the parent")
      i32 before = 0;
      for (WDom *anchor = n->next; anchor && !before; anchor = anchor->next)
        for (WDom *f = anchor; f && !before; f = f->first)
          if (f->id > 0 && !f->is_new) before = f->id;
      for (WDom *c = n->first; c; c = c->next)
        if (c->id > 0) cmd(&out->b, "move %d %d %d", c->id, pid, before);
    }
    if (n->text_dirty && n->type == WD_RAW) {
      // parent id only matters the first time; 0 means "the container"
      i32 pid = n->parent && n->parent->id > 0 ? n->parent->id : 0;
      buf_printf(&out->b, "raw %d %d ", n->id, pid);
      wc_line_esc(&out->b, n->text);
      buf_putc(&out->b, '\n');
    } else if (n->text_dirty) {
      buf_printf(&out->b, "text %d ", n->id);
      wc_line_esc(&out->b, n->text);
      buf_putc(&out->b, '\n');
    }
    if (n->attrs_dirty) {
#ifdef WC_TRACE_DOM
      fprintf(stderr, "dirty attrs id=%d type=%d tag=%s nattrs=%d prev=%d new=%d\n", n->id, n->type,
              n->tag ? n->tag : "-", n->nattrs, n->nprevattrs, n->is_new);
#endif
      cmd(&out->b, "attrs-clear %d", n->id);
      for (i32 k = 0; k < n->nattrs; k++) {
        buf_printf(&out->b, "attr %d %.*s ", n->id, (int)n->attrs[k].name.n, n->attrs[k].name.p);
        wc_line_esc(&out->b, n->attrs[k].value);
        buf_putc(&out->b, '\n');
      }
    }
  }
}

i32 wdom_node_count(WBuild *b) {
  return b ? b->next_id - 1 : 0;
}

// ---------------------------------------------------------------- components

void wcomp_init(WComponent *c, Arena *a) {
  *c = (WComponent){.nodes = a ? a : arena_new(16 * 1024),
                    .owns_nodes = a == nullptr,
                    .html = arena_new(4 * 1024),
                    .scratch = {arena_new(8 * 1024), arena_new(8 * 1024)}};
  buf_init(&c->cmds.b, nullptr, 512);
}

void wcomp_destroy(WComponent *c) {
  for (i32 i = 0; i < c->nseq; i++)
    if (c->seq[i]) {
      wlive_kill_all(c->seq[i]); // a live binding outlives a render; not the component
      for (i32 k = 0; k < 2; k++)
        if (c->seq[i]->bind_val[k]) {
          wm_free(c->seq[i]->bind_val[k]);
          c->seq[i]->bind_val[k] = nullptr;
        }
    }
  // after the bindings: an owned signal must not outlive the effect that wrote its node
  winst_release(c->own);
  c->own = nullptr;
  if (c->owns_nodes && c->nodes) arena_destroy(c->nodes);
  for (i32 i = 0; i < 2; i++)
    if (c->scratch[i]) arena_destroy(c->scratch[i]);
  if (c->html) arena_destroy(c->html);
  buf_free(&c->cmds.b);
  *c = (WComponent){0};
}

void wcomp_render_ctx(WComponent *c, void (*fn)(WBuild *b, void *ctx), void *ctx) {
  i32 slot = c->slot;
  WBuild *b = wbuild_begin(c->scratch[slot], nullptr);
  b->nodes_arena = c->nodes;
  b->out = &c->cmds; // where a live binding appends the op it earned
  b->seq = c->seqbuf[slot];
  b->capseq = c->seqcap[slot];
  wbuild_set_reuse(b, c->seq, c->nseq);
  // one number per render: a node is live this run iff its `seen` matches
  b->run = c->renders + 1;
  wbuild_reserve_ids(b, c->seq, c->nseq);
  // Several mounted views share one bridge and one id space (an event is looked up
  // by id alone), so each component numbers from its own base.
  if (c->id_base > 1) {
    b->next_id = c->id_base;
    if (b->id_scan < c->id_base) b->id_scan = c->id_base;
  }
  // A render is a scope: `fmt()` inside a view allocates in this render's scratch
  // arena, so its text dies with the render instead of growing the request arena
  // (or the heap, in a browser that re-renders on every click).
  // The mount is a render position: every `state_own` its view reads lives in this
  // component's instance, and every node it draws remembers the position for the events
  // that come back later. Created on the first render that reads one (an unused mount
  // costs a null pointer).
  if (!c->own) c->own = winst_alloc();
  winst_own_push(c->own);
  Arena *prev_arena = wc_arena_swap(c->scratch[slot]);
  if (fn) fn(b, ctx);
  wc_arena_swap(prev_arena);
  // The mount's render is over: the components it drew this run are the ones that live.
  winst_sweep_children(c->own);
  c->own->rendered = true;
  winst_own_pop();
  c->root = b->root;
  wbuild_end(b);
  buf_reset(&c->cmds.b);
  // The tree is the contract between the view and everything downstream of it (the
  // emitter, the id index, the browser). Checking it costs one walk per render and
  // turns "the tab froze" into a line of text naming the node.
  wc_render_assert(b);
  if (c->renders == 0)
    wdom_mount_batch(b, &c->cmds);
  else
    wdom_patch_batch(b, &c->cmds);
  // After the batch, not before it: a dead-binding report has to read the tree this render
  // just built. Reported from the assertion hook it saw the *previous* tree, so a binding
  // whose element only appears once the list is non-empty was called dead on the very
  // render that gave it a home.
  hooks_report();
#ifdef WEBC_TRACE_RENDER
  fprintf(stderr, "[webc] render %s nodes=%d ops=%d bytes=%zu\n",
          c->renders == 0 ? "mount" : "patch", (int)b->nseq, (int)batch_lines(&c->cmds.b),
          (size_t)c->cmds.b.n);
#endif
  c->seq = wbuild_seq(b, &c->nseq);
  c->seqbuf[slot] = c->seq;
  c->seqcap[slot] = b->capseq;
  c->renders++;
  // the scratch arena from two renders ago is no longer referenced
  c->slot = slot ^ 1;
  arena_reset(c->scratch[c->slot]);
}

// The same render, for views that take no context.
typedef struct ViewThunk {
  void (*plain)(WBuild *);
} ViewThunk;

static void render_thunk(WBuild *b, void *ctx) {
  ViewThunk *t = ctx;
  if (t->plain) t->plain(b);
}

// No ctx parameter, on purpose: accepting one and dropping it lets
// `wcomp_render(&c, body, ctx)` compile, run, and render without the context.
// Views that need one call wcomp_render_ctx.
void wcomp_render(WComponent *c, void (*fn)(WBuild *b)) {
  ViewThunk t = {.plain = fn};
  wcomp_render_ctx(c, render_thunk, &t);
}

// An html render is over the moment it is text: nothing will ever claim these rows again, so
// their instances are released now instead of leaking with the tree. Left to the tree, a row
// leaks one `WInst` per row per html render (the item table lives on a node the next render
// throws away), and a component inside a row one more.
static void items_release_all_(WBuild *b, WDom *n) {
  for (WDom *p = n; p; p = p->next) {
    if (p->items) {
      WItemTable *t = p->items;
      for (i32 i = 0; i < t->cap; i++) t->e[i].used = false; // nothing is claimed after the fact
      keyed_sweep(b, p);
    }
    if (p->first) items_release_all_(b, p->first);
  }
}

str wcomp_html_ctx(WComponent *c, void (*fn)(WBuild *b, void *ctx), void *ctx) {
  WBuild *b = wbuild_begin(c->scratch[c->slot], nullptr);
  b->nodes_arena = c->nodes;
  if (!c->own) c->own = winst_alloc();
  winst_own_push(c->own);
  Arena *prev_arena = wc_arena_swap(c->scratch[c->slot]);
  // A `ref()` in a view must find the links `prefetch()` made in the handler, and those live
  // in the *request's* arena — which is not the arena the template runs in: looked up in the
  // wrong arena every `ref()` is null, and the query that made the link is a statement nobody
  // can see. The epoch names where the rows live, so the render names the request.
  Request *cur = webc_req_current();
  Arena *prev_epoch = wc_ref_epoch_swap(cur ? cur->arena : nullptr);
  if (fn) fn(b, ctx);
  wc_ref_epoch_swap(prev_epoch);
  wc_arena_swap(prev_arena);
  // An HTML render is one render: nothing of the position survives it (the same rule the
  // server half applies to a row's state — the browser rebuilds it from the same call sites).
  winst_sweep_children(c->own);
  c->own->rendered = true;
  winst_own_pop();
  str out = wdom_html(b);
  items_release_all_(b, b->root);
  wbuild_end(b);
  // The HTML copy lives in the component's own arena, not on the heap: a copy from
  // str_dup(nullptr, …) has no owner, so every SSR response leaks it (45 B per render in
  // the smallest possible view). Valid until the next call.
  arena_reset(c->html);
  char *p = arena_alloc(c->html, out.n + 1);
  if (p) memcpy(p, out.p, out.n);
  str copy = {.p = p, .n = p ? out.n : 0};
  c->slot ^= 1;
  arena_reset(c->scratch[c->slot]);
  return copy;
}

str wcomp_html(WComponent *c, void (*fn)(WBuild *b)) {
  ViewThunk t = {.plain = fn};
  return wcomp_html_ctx(c, render_thunk, &t);
}

// ---------------------------------------------------------------- mount

// On the wasm target `mount` hands a batch to the bridge. On the server it is a
// no-op: the same component is rendered by webc_render instead.
void wkeyed_item_ptr(void *p) {
  WBuild *b = wbuild_current();
  if (b) b->item_ptr = p;
}

// The wasm glue owns the real one; a native build (a test) links this instead, so a
// client file compiles and links on both targets without a target test.
//
// It records rather than ignores: `webc test --client` renders each of these and replays
// the batch through the real webc.js, which is the only way to test the half of an app
// that a C test cannot reach.
#define WC_TEST_MOUNTS 32
static WTestMount g_test_mounts[WC_TEST_MOUNTS];
static i32 g_ntest_mounts;

i32 wc_test_mount_count(void) {
  return g_ntest_mounts;
}

const WTestMount *wc_test_mount(i32 i) {
  if (i < 0 || i >= g_ntest_mounts) return nullptr;
  return &g_test_mounts[i];
}

void wc_test_mount_clear(void) {
  g_ntest_mounts = 0;
}

__attribute__((weak)) void wc_mount_ctx(void (*fn)(WBuild *b, void *ctx), void *ctx,
                                        const char *selector) {
  g_in_client = true; // a mount is the client half taking over
  if (g_ntest_mounts < WC_TEST_MOUNTS) {
    g_test_mounts[g_ntest_mounts++] = (WTestMount){fn, ctx, selector ? selector : "#root"};
    return;
  }
  wc_limit_once(&g_ntest_mounts, "more mounts than a client test can record",
                "WC_TEST_MOUNTS", "mount");
}

__attribute__((weak)) void wc_mount(void (*fn)(WBuild *b), const char *selector) {
  (void)fn;
  (void)selector;
  fprintf(stderr, "webc: mount() is a wasm-target call; this build renders SSR\n");
}
// ---------------------------------------------------------------- tree transplant (dev)
//
// A hot module swap keeps the document, so the new module needs to know what is already in
// it — not to render it again, but to *diff against it*. The renderer already knows how to
// do that (take_reuse / wdom_patch_batch); all it is missing after a swap is the previous
// tree, which lived in the previous module's memory.
//
// The format is pre-order with an explicit depth, because pre-order alone cannot tell you
// where one subtree ends and the next sibling begins:
//
//   n <depth> <type> <id> <nattrs> <taglen> <textlen>
//   <tag> \n <text> \n            (tag empty for text/raw/fragment)
//   a <namelen> <vallen> \n <name> \n <value> \n      (nattrs of these)
//
// Only what a diff can see crosses: type, tag, id, text, attributes. Not listeners (this
// run re-registers them), not scopes (this run owns them), not keys (a keyed list re-keys
// itself on its next render).
//
// One caveat, written down because it is a real cost: reuse compares tags by pointer, and
// after a swap the old tags live in other memory. take_reuse falls back to strcmp, so the
// first render after a swap pays a string compare per element — and every render after
// that is back to a pointer compare.

static void tree_dump_node(Buf *out, WDom *n, i32 depth) {
  for (WDom *cur = n; cur; cur = cur->next) {
    if (cur->dead) continue;
    const char *tag = cur->type == WD_ELEMENT && cur->tag ? cur->tag : "";
    str text = (cur->type == WD_TEXT || cur->type == WD_RAW) ? cur->text : (str){nullptr, 0};
    i32 nattrs = 0;
    if (cur->type == WD_ELEMENT && cur->attrs)
      for (i32 i = 0; i < cur->nattrs; i++)
        if (cur->attrs[i].name.p) nattrs = i + 1;
    buf_printf(out, "n %d %d %d %d %zu %zu\n", (int)depth, (int)cur->type, (int)cur->id, (int)nattrs,
               strlen(tag), (size_t)text.n);
    buf_puts(out, tag);
    buf_putc(out, '\n');
    if (text.n && text.p) buf_write(out, text.p, (size_t)text.n);
    buf_putc(out, '\n');
    for (i32 i = 0; i < nattrs; i++) {
      WAttrPair *a = &cur->attrs[i];
      buf_printf(out, "a %zu %zu\n", (size_t)a->name.n, (size_t)a->value.n);
      if (a->name.n) buf_write(out, a->name.p, (size_t)a->name.n);
      buf_putc(out, '\n');
      if (a->value.n && a->value.p) buf_write(out, a->value.p, (size_t)a->value.n);
      buf_putc(out, '\n');
    }
    if (cur->first) tree_dump_node(out, cur->first, depth + 1);
  }
}

void wdom_tree_dump(Buf *out, WDom *root) {
  if (!out || !root) return;
  tree_dump_node(out, root, 0);
}

// Where a loaded tree waits for the render that will adopt it. Per view, in the module's
// own static memory: there is exactly one hot swap in flight at a time.
#define WC_LOADED_VIEWS 8
#define WC_LOADED_MAX 8192
static WDom *g_loaded[WC_LOADED_VIEWS][WC_LOADED_MAX];
static i32 g_nloaded[WC_LOADED_VIEWS];
static Arena *g_tree_arena;

i32 wdom_tree_take(i32 view, WDom ***nodes) {
  if (view < 0 || view >= WC_LOADED_VIEWS) return 0;
  i32 n = g_nloaded[view];
  g_nloaded[view] = 0;
  *nodes = g_loaded[view];
  return n;
}

// Rebuild the tree in this module's memory. One arena, reset per swap: a loaded tree is
// replaced wholesale, never patched.
i32 wdom_tree_load(str in) {
  if (!in.p || in.n <= 0) return 0;
  if (!g_tree_arena) g_tree_arena = arena_new(128 * 1024);
  arena_reset(g_tree_arena);
  const char *p = in.p, *end = in.p + in.n;
  WDom *stack[64] = {0};
  // A buffer with no `view` header is one tree, and a tree with one view: the header is
  // what the multi-view dump writes, not a requirement of the format.
  i32 view = 0, nflat = 0;
  g_nloaded[0] = 0;
  while (p < end) {
    if (*p == 'v' && end - p > 4 && memcmp(p, "view", 4) == 0) {
      i32 v = (i32)strtol(p + 4, nullptr, 10);
      if (v >= 0 && v < WC_LOADED_VIEWS) {
        view = v;
        g_nloaded[view] = 0;
      }
      const char *nl = memchr(p, '\n', (size_t)(end - p));
      p = nl ? nl + 1 : end;
      continue;
    }
    if (view < 0 || view >= WC_LOADED_VIEWS) break;
    if (*p == 'n' && p[1] == ' ') {
      i32 depth = 0, type = 0, id = 0, nattrs = 0;
      size_t taglen = 0, textlen = 0;
      if (sscanf(p, "n %d %d %d %d %zu %zu", &depth, &type, &id, &nattrs, &taglen, &textlen) != 6)
        break;
      const char *nl = memchr(p, '\n', (size_t)(end - p));
      if (!nl) break;
      p = nl + 1;
      if (depth < 0 || depth >= 64 || (size_t)(end - p) < taglen + 1) break;
      char *tag = arena_alloc(g_tree_arena, taglen + 1);
      memcpy(tag, p, taglen);
      tag[taglen] = 0;
      p += taglen + 1;
      if ((size_t)(end - p) < textlen + 1) break;
      char *text = arena_alloc(g_tree_arena, textlen + 1);
      memcpy(text, p, textlen);
      text[textlen] = 0;
      p += textlen + 1;
      WDom *n = arena_zalloc(g_tree_arena, sizeof *n);
      n->type = (WDomType)(type >= 0 && type <= WD_FRAGMENT ? type : WD_ELEMENT);
      n->tag = n->type == WD_ELEMENT ? tag : nullptr;
      n->text = (str){text, (i64)textlen};
      n->id = id;
      n->wrap = n->type == WD_FRAGMENT && depth == 0; // the view's own wrapper
      i32 keep = nattrs < 0 ? 0 : nattrs;
      n->attrs = keep ? arena_zalloc(g_tree_arena, (size_t)keep * sizeof(WAttrPair)) : nullptr;
      for (i32 i = 0; i < keep; i++) {
        size_t nl2 = 0, vl = 0;
        if (p >= end || *p != 'a' || sscanf(p, "a %zu %zu", &nl2, &vl) != 2) break;
        const char *l2 = memchr(p, '\n', (size_t)(end - p));
        if (!l2) break;
        p = l2 + 1;
        if ((size_t)(end - p) < nl2 + 1) break;
        char *an = arena_alloc(g_tree_arena, nl2 + 1);
        memcpy(an, p, nl2);
        an[nl2] = 0;
        p += nl2 + 1;
        if ((size_t)(end - p) < vl + 1) break;
        char *av = arena_alloc(g_tree_arena, vl + 1);
        memcpy(av, p, vl);
        av[vl] = 0;
        p += vl + 1;
        n->attrs[n->nattrs].name = (str){an, (i64)nl2};
        n->attrs[n->nattrs].value = (str){av, (i64)vl};
        n->nattrs++;
      }
      if (depth > 0 && stack[depth - 1]) append_child(stack[depth - 1], n);
      stack[depth] = n;
      for (i32 d = depth + 1; d < 64; d++) stack[d] = nullptr;
      if (nflat < WC_LOADED_MAX) g_loaded[view][nflat++] = n;
      continue;
    }
    // an unknown line: skip it rather than losing the rest of the tree
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    p = nl ? nl + 1 : end;
  }
  g_nloaded[view] = nflat;
  return nflat;
}

// Adopt a loaded tree as this component's previous render, so the next render diffs
// against it instead of emitting a fresh tree.
void wcomp_adopt(WComponent *c, WDom **nodes, i32 n) {
  if (!c || !nodes || n <= 0) return;
  WDom **arr = arena_alloc(c->nodes, (size_t)n * sizeof *arr);
  memcpy(arr, nodes, (size_t)n * sizeof *arr);
  c->seq = arr;
  c->nseq = n;
  // renders == 1 makes the next render a *patch*, which is the whole point: mount_batch
  // would emit a create for every node and the browser would rebuild the page it has.
  c->renders = 1;
}
