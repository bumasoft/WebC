// SPDX-License-Identifier: Apache-2.0
// WebC — fine-grained reactivity. Signals → memos → effects, no virtual DOM, no
// diffing, no closures (SPEC §7.1, §13).
//
//   * dynamic dependency vectors, not a fixed W_MAX_DEP
//   * topological flush (effects run in graph-height order, not queue order)
//   * per-graph runtimes so two SSR renders can interleave on one thread
//   * scopes own nodes with dynamic capacity and dispose them deterministically
#ifndef WEBC_REACT_H
#define WEBC_REACT_H

#include <webc/core.h>

typedef struct Wn Wn;
typedef struct WScope WScope;
typedef struct Wrt Wrt;
typedef struct WInst WInst;
typedef struct WDecl WDecl;
typedef struct WInstChild WInstChild;
typedef struct WCl WCl;

typedef void (*WnFn)(Wn *self, void *ctx);

// `W_MARK_OWN` is not a node in the graph: it is a *declaration site* for render-owned
// state (`state_own`), the thing a live binding or `bind_str` can name. Resolving it reads
// the WDecl out of `ctx` and looks the real node up in the instance of the render position
// that is reading. Nothing ever reads or writes a marker.
typedef enum WNodeKind : u8 { W_SIGNAL, W_MEMO, W_EFFECT, W_MARK_OWN } WNodeKind;

struct Wn {
  WnFn fn;
  void *ctx;
  void *val;
  void *prev;             // memos: previous value image for equality suppression
  size_t vlen;
  WNodeKind kind;
  bool dirty, alive, queued, disposed;
  i32 height;
  i32 ver;
  i32 runs;               // how many times this node has run (debug view)
  Wn **src;
  i32 nsrc, cap_src;
  Wn **obs;
  i32 nobs, cap_obs;
  i32 *seen;              // per-source version seen by this observer
  WScope *owner;
  Wrt *rt;
  WInst *inst;           // the component instance this node belongs to, if any
  const char *name;
  bool eq_bytes;          // memos: suppress when the byte image is unchanged
  WCl *cl;                // cleanups for the current run of an effect (on_cleanup)
  Wn *next_free;          // heap graphs only: this node's slot on the recycle list
};

struct WScope {
  WScope *parent;
  Wn **owned;
  i32 n, cap;
  bool disposed;
  Wrt *rt;
};

typedef struct Wrt {
  Arena *arena;           // NULL = heap
  Wn *cur;                // node currently computing (dependency tracking)
  Wn **queue;             // dirty effects awaiting flush
  i32 nq, capq;
  i32 batch_depth;
  WScope *scope;          // current owner
  Wn *free_list;          // killed nodes whose memory can be handed out again
  i32 nfree;
  i32 nodes, runs, flushes;
  bool in_flush;
  // The ambient component instance (see winst_push): a stack, because a view can
  // render inside another view's instance.
  WInst **insts;
  i32 ninsts, capinsts;
  // The ambient *owner* (see winst_own_push): the instance of the render position that is
  // drawing — a mounted view, or one `for_each` row. Separate from `insts` on purpose: an
  // app that owns its instances with `with_inst` and a framework-owned render position are
  // two different questions, and `state_own` must never answer the wrong one.
  WInst **owns;
  i32 nowns, capowns;
} Wrt;

// ---------------------------------------------------------------- runtime

Wrt *wrt_new(Arena *a);
void wrt_destroy(Wrt *rt);
Wrt *wrt_current(void);
Wrt *wrt_bind(Wrt *rt);     // returns the previous current runtime
void wrt_unbind(Wrt *prev);
Wrt *wrt_default(void);     // the process-wide graph used by file-scope DSL

Wn *w_new(Wrt *rt, WNodeKind kind, const char *name);
void w_sig_init(Wn *n, void *slot, size_t len, const char *name);
void w_memo_init(Wn *n, WnFn fn, void *ctx, void *out, size_t len, const char *name);
void w_effect_init(Wn *n, WnFn fn, void *ctx, const char *name);
void *w_read(Wn *n);
void w_write(Wn *n, const void *v, size_t len);
void w_memo_pull(Wn *n);
void w_flush(void);

// A DOM binding is an effect that writes one node instead of re-rendering the view
// (`text_live` and friends, dom.h). These two are how it is born: `w_effect_new` makes
// a node that starts clean (nothing is queued, so attaching one never flushes somebody
// else's work), and `w_effect_run` runs it once right there, which is what registers the
// dependency edge before any signal can be written.
Wn *w_effect_new(WnFn fn, void *ctx, const char *name);
void w_effect_run(Wn *e);
void w_batch_begin(void);
void w_batch_end(void);
void w_kill(Wn *n);        // dispose a single node
i32  w_node_count(Wrt *rt);

// ---------------------------------------------------------------- instances
//
// One declaration, many instances. `state` is one slot for the whole page, which is
// right for app-wide state and wrong for a view that appears twice. `state_in` declares the
// same thing without fixing how many copies exist: the state lives in a `WInst` the caller
// owns, so a list of counters is a loop over instances and a test is an instance of its own.
// `state_own` (below) is the same idea with the *framework* owning the lifetime, which is
// what a component wants.
//
//   state_in(int, count, 0);
//   view(Counter) { el("span") { text("%d", iget(count)); } }
//
//   WInst side, page;                       // two cards, two counts
//   winst_init(&side, nullptr);
//   with_inst(&side) { h1 = wcomp_html(&c1, view_Counter); }
//
// Inside a bound instance `iget`/`iset` reach that instance's slot; outside one they
// panic rather than quietly touch a different copy. Where there is no ambient
// instance — an action, a test, a second card — name the instance: `get_in(&page, count)`.
struct WDecl {
  const char *name;
  size_t size;     // value bytes; 0 for an effect
  WNodeKind kind;
  WnFn fn;         // memo/effect body
  void *init;      // the value the declaration started with
};

typedef struct WSlot {
  const WDecl *decl;
  Wn *node;
  void *val; // the instance's own copy of a state's or memo's value
} WSlot;

struct WInst {
  WSlot *slots;
  i32 n, cap;
  Arena *arena; // NULL = heap
  // Component instances drawn by this position (`webc/dom.h` `comp()`), keyed by the call
  // site that drew them (react.h `winst_child`). A component is a render position of its
  // own, so this is what makes two calls of one part two states; the table is swept when
  // the position's render ends, exactly like a keyed list's. `cl` belongs to the position
  // itself (on_cleanup outside an effect).
  WInstChild *kids;
  i32 nkids, capkids;
  WCl *cl;
  WInst *parent; // the position that drew this component (null for a mount, a row or a WInst
                 // the app made): `kids_of` uses it so children belong to the caller
  void *mounts; // queued on_mount hooks: cancelled (not freed) when this position is cleared
  bool live;    // false once cleared: a queued on_mount must not run for a dead position
  bool rendered; // this position's first render has finished: on_cleanup/on_mount registered
                 // once, on the way in, and not again on every re-render
};

void winst_init(WInst *in, Arena *a); // a != NULL: the instance must not outlive it
void winst_free(WInst *in);           // kills the nodes, so the graph shrinks back
void winst_clear(WInst *in);          // the same, but leaves the struct reusable
Wn  *winst_node(WInst *in, const WDecl *d);
void *winst_value(WInst *in, const WDecl *d); // state/memo value, created on first use
void winst_effect(WInst *in, const WDecl *d); // start a declared effect
WInst *winst_push(WInst *in);                 // returns the previous ambient
void  winst_pop(void);
WInst *winst_current(void);                   // NULL outside a bound instance
WInst *winst_need(const char *what);          // panics instead of guessing
i32  winst_count(WInst *in);                  // declarations materialised so far

// with_inst(&card) { ... } — the instance is ambient for the block, and only there.
// Same shape as `batch` above: the flag makes the body run once, and the cleanup that
// runs when the variable leaves scope is what pops — including when the block exits
// through `break`, `return` or a panic.
#define with_inst(in_) WC_WITH_INST(__COUNTER__, in_)
#define WC_WITH_INST(C_, in_)                                                     \
  for (int WC_CAT(wc_inst_, C_) __attribute__((cleanup(wc_inst_leave_))) =       \
           (winst_push(in_), 1);                                                  \
       WC_CAT(wc_inst_, C_); WC_CAT(wc_inst_, C_) = 0)
[[maybe_unused]] static inline void wc_inst_leave_(int *flag) {
  (void)flag;
  winst_pop();
}

// ---------------------------------------------------------------- state transplant (dev)
//
// Per-instance and render-owned state do not cross a hot module swap: an instance belongs to
// the module that made it. A `state_own` value is therefore reset by a save, like any other
// in-memory structure the app does not write a save/load pair for (webc_dev.h).
//
// A hot module swap cannot copy a pointer or walk a heap: only named state crosses, by
// name, in the new module's own shape. `state()` registers itself
// here, which is what lets a save keep the value somebody typed. Anything else the app
// wants to survive opts in with webc_dev_state_save/load (include/webc/webc_dev.h).
void w_state_register(const char *name, void *val, size_t len, bool is_str);
void w_state_dump(Buf *out);
// Back to the world the process started in: every registered state returns to the value
// it was declared with. The test runner calls this before each TEST, so no suite has to
// end with "leave the app as you found it"; a long-running process can call it too.
void w_state_rewind(void);
void w_state_clear(void);
i32  w_state_load(str in, char *warn, size_t warn_cap);
i32  w_state_count(void);

// ---------------------------------------------------------------- scopes

void w_scope_enter(WScope *s, WScope *parent);
void w_own(WScope *s, Wn *n);
void w_scope_dispose(WScope *s);

// ---------------------------------------------------------------- DSL

// state(int, count, 0) → get(count) / set(count, v)
//
// One declaration for process-wide state, whatever the type — including a string:
//
//   state(i64, count, 0);            set(count, get(count) + 1);
//   state(Text, draft, "");          set(draft, fmt("%s!", typed));
//   state(bool, open, false);        toggle(open);
//
// The storage is a *weak* global, not a static: a state declared in a header is then one
// state, whichever file reads it. As a static it was one per translation unit — the view
// set `count` in its own copy and the action read a different one, so the page simply
// never updated, with no warning and nothing to grep for. Weak definitions merge at link
// time into a single object, so the header keeps working as a declaration and both files
// address the same storage. (For state that should exist per component instance,
// `state_in` is what you want.)
//
// A `Text` slot is registered as a string, so `webc dev`'s transplant carries its bytes
// across a reload instead of a pointer into the old module; `_Generic` decides which,
// because `Text` and `int` are the same syntax to the macro and not the same thing.
#define WC_STATE_TEXT_(T_)                                                     \
  _Generic(&(T_){0}, const char **: true, char **: true, default: false)

#define state(T_, N_, ...)                                                     \
  __attribute__((weak)) T_ N_##_slot = __VA_ARGS__;                            \
  __attribute__((weak)) Wn N_##_node = { .kind = W_SIGNAL, .val = &N_##_slot,   \
                          .vlen = sizeof(T_), .alive = true, .name = #N_ };     \
  __attribute__((constructor)) static void N_##_state_reg(void) {               \
    w_state_register(#N_, &N_##_slot, sizeof(T_), WC_STATE_TEXT_(T_)); }        \
  [[maybe_unused]] static inline T_ get_##N_(void) {                            \
    return *(T_ *)w_read(&N_##_node); }                                        \
  [[maybe_unused]] static inline void set_##N_(T_ v_) {                         \
    w_write(&N_##_node, &v_, sizeof v_); }

#define get(N_)      (get_##N_())
#define set(N_, ...) (set_##N_(__VA_ARGS__))

// The one thing everyone does to a flag. Written out by hand it is an inversion the
// reader must decode (`set(x, get(x) ? 0 : 1)`); named, it is what it does — and with a
// `state(bool, …)` it is a real boolean flip, not a convention in a comment.
#define toggle(N_)   (set_##N_(!get_##N_()))

#define computed(T_, N_)                                                       \
  static T_ N_##_impl(void);                                                   \
  static T_ N_##_slot;                                                         \
  static Wn N_##_node;                                                         \
  static void N_##_compute([[maybe_unused]] Wn *self_, [[maybe_unused]] void *ctx_) { \
    N_##_slot = N_##_impl(); }                                                 \
  __attribute__((constructor)) static void N_##_boot(void) {                    \
    w_memo_init(&N_##_node, N_##_compute, nullptr, &N_##_slot, sizeof(T_), #N_); } \
  [[maybe_unused]] static inline T_ get_##N_(void) {                            \
    w_memo_pull(&N_##_node);                                                   \
    return *(T_ *)w_read(&N_##_node); }                                        \
  static T_ N_##_impl(void)

// effect(name) { ... } — re-runs whenever anything it read changed.
#define effect(N_)                                                             \
  static Wn N_##_node;                                                         \
  static void N_##_run([[maybe_unused]] Wn *self_, [[maybe_unused]] void *ctx_); \
  __attribute__((constructor)) static void N_##_boot(void) {                    \
    w_effect_init(&N_##_node, N_##_run, nullptr, #N_); }                        \
  static void N_##_run([[maybe_unused]] Wn *self_, [[maybe_unused]] void *ctx_)

// batch { ... } — every effect runs once, at the end.
#define batch for (int _wc_b_ __attribute__((cleanup(wc_batch_cleanup))) = (w_batch_begin(), 1); \
                   _wc_b_; _wc_b_ = 0)
[[maybe_unused]] static inline void wc_batch_cleanup(int *flag) {
  (void)flag;
  w_batch_end();
}

// ---------------------------------------------------------------- per-instance DSL
//
// The `_in` family: same declarations, per-instance storage. `state` above is
// process-wide state and says so; these say "one copy per instance", and the
// instance is a value the caller owns (`WInst`), so nothing here is a hidden global.
//
//   state_in(int, count, 0);
//   computed_in(int, doubled)  { return iget(count) * 2; }
//   effect_in(log_it)          { cf_append(iget(doubled)); }
//
//   WInst a, b;  winst_init(&a, nullptr);  winst_init(&b, nullptr);
//   set_in(&a, count, 1);                  set_in(&b, count, 2);
//   mount_in(log_it, &a);                  // effects are started by name, per instance
#define state_in(T_, N_, ...)                                                  \
  static T_ N_##_init_ = __VA_ARGS__;                                          \
  static WDecl N_##_decl_ = {.name = #N_, .size = sizeof(T_), .kind = W_SIGNAL, \
                             .init = &N_##_init_};                             \
  [[maybe_unused]] static inline T_ get_##N_##_in(WInst *i_) {                  \
    return *(T_ *)w_read(winst_node(i_, &N_##_decl_)); }                       \
  [[maybe_unused]] static inline void set_##N_##_in(WInst *i_, T_ v_) {         \
    w_write(winst_node(i_, &N_##_decl_), &v_, sizeof v_); }                    \
  /* the same two, for the code that runs inside an instance (a view body) */   \
  [[maybe_unused]] static inline T_ get_##N_(void) {                            \
    return get_##N_##_in(winst_need(#N_)); }                                   \
  [[maybe_unused]] static inline void set_##N_(T_ v_) {                         \
    set_##N_##_in(winst_need(#N_), v_); }

#define computed_in(T_, N_)                                                    \
  static T_ N_##_impl_(void);                                                  \
  static void N_##_compute_([[maybe_unused]] Wn *self_, [[maybe_unused]] void *ctx_) { \
    winst_push(self_->inst);                                                   \
    *(T_ *)self_->val = N_##_impl_();                                          \
    winst_pop();                                                               \
  }                                                                            \
  static WDecl N_##_decl_ = {.name = #N_, .size = sizeof(T_), .kind = W_MEMO,    \
                             .fn = N_##_compute_};                             \
  [[maybe_unused]] static inline T_ get_##N_##_in(WInst *i_) {                  \
    Wn *n_ = winst_node(i_, &N_##_decl_);                                      \
    w_memo_pull(n_);                                                           \
    return *(T_ *)w_read(n_); }                                                \
  [[maybe_unused]] static inline T_ get_##N_(void) {                            \
    return get_##N_##_in(winst_need(#N_)); }                                   \
  static T_ N_##_impl_(void)

#define effect_in(N_)                                                          \
  static void N_##_run_(void);                                                 \
  static void N_##_tick_([[maybe_unused]] Wn *self_, [[maybe_unused]] void *ctx_) { \
    winst_push(self_->inst);                                                   \
    N_##_run_();                                                               \
    winst_pop();                                                               \
  }                                                                            \
  static WDecl N_##_decl_ = {.name = #N_, .kind = W_EFFECT, .fn = N_##_tick_};  \
  [[maybe_unused]] static void N_##_start_in(WInst *i_) {                       \
    winst_effect(i_, &N_##_decl_); }                                           \
  static void N_##_run_(void)

// ---------------------------------------------------------------- render-owned DSL

// `effect_own(name) { … }` — an effect that belongs to the ambient render position, started
// once per instance by `mount_own(name)` and killed with it. The body reads and writes owned
// state (`get`/`set` on a `state_own`), because the tick re-enters the instance it was started
// in; `on_cleanup` inside it runs before each re-run and when the position dies.
//
//   effect_own(save) { webc_later(800, flush_draft); on_cleanup(cancel_pending); }
//   view(Editor) { mount_own(save); … }
//
// Why the extra `mount_own` line: a C function body is not something a view can splice in
// (there are no closures), so the body is declared at file scope like `effect_in`'s — and the
// one thing the framework cannot infer is *which* effects a given view wants. It is the same
// pair `effect_in` + `mount_in` already has, with the framework owning the lifetime instead of
// the app.
#define effect_own(N_)                                                         \
  static void N_##_run_(void);                                                 \
  static void N_##_tick_([[maybe_unused]] Wn *self_, [[maybe_unused]] void *ctx_) { \
    winst_own_push(self_->inst);                                               \
    N_##_run_();                                                               \
    winst_own_pop();                                                           \
  }                                                                            \
  static WDecl N_##_decl_ = {.name = #N_, .kind = W_EFFECT, .fn = N_##_tick_};  \
  [[maybe_unused]] static void N_##_start_(void) {                              \
    winst_effect_once(winst_own_need(#N_), &N_##_decl_); }                      \
  static void N_##_run_(void)

// Start an `effect_own` in the position being drawn. Idempotent: a second call on a later
// render does nothing, so the line is written where the view declares what it needs.
#define mount_own(N_) (N_##_start_())

// ---------------------------------------------------------------- render-owned DSL
//
// `state_own(T, name, init)` — one slot per render position, created on first read and
// disposed with the position (the keyed sweep, `wcomp_destroy`). The three declarations in
// this header are the three answers to "how many copies?":
//
//   state(i64, n, 0);        one for the page          the app's question
//   state_in(i64, n, 0);     one per WInst the app owns  an escape hatch with a lifetime
//   state_own(i64, n, 0);    one per render position     what a component author means
//   comp(Name, …)            one per component call      what a component *gets* (dom.h)
//
// `N_##_node` is not storage: it is the *name* a live binding can hold (`text_live("%lld",
// n)`), resolved to the owning position's node at render time. `N_##_slot` exists only so
// `_Generic` (WC_SIGKIND_) can see the value's type, exactly as `state` does.
#define state_own(T_, N_, ...)                                                 \
  static T_ N_##_init_ = __VA_ARGS__;                                          \
  static WDecl N_##_decl_ = {.name = #N_, .size = sizeof(T_), .kind = W_SIGNAL, \
                             .init = &N_##_init_};                             \
  [[maybe_unused]] static Wn N_##_node = {.kind = W_MARK_OWN, .ctx = &N_##_decl_, \
                                          .name = #N_};                        \
  [[maybe_unused]] static T_ N_##_slot;                                        \
  [[maybe_unused]] static inline T_ get_##N_(void) {                           \
    return *(T_ *)w_read(winst_own_node(&N_##_decl_)); }                       \
  [[maybe_unused]] static inline void set_##N_(T_ v_) {                        \
    w_write(winst_own_node(&N_##_decl_), &v_, sizeof v_); }

// A derived value per render position. The body reads other owned declarations (`get`), so
// the compute re-enters the owner it belongs to — the same reason `computed_in` re-enters
// its instance.
#define computed_own(T_, N_)                                                   \
  static T_ N_##_impl_(void);                                                  \
  static void N_##_compute_(Wn *self_, void *ctx_) {                            \
    (void)ctx_;                                                                \
    winst_own_push(self_->inst);                                               \
    *(T_ *)self_->val = N_##_impl_();                                          \
    winst_own_pop();                                                           \
  }                                                                            \
  static WDecl N_##_decl_ = {.name = #N_, .size = sizeof(T_), .kind = W_MEMO,   \
                             .fn = N_##_compute_};                             \
  [[maybe_unused]] static Wn N_##_node = {.kind = W_MARK_OWN, .ctx = &N_##_decl_, \
                                          .name = #N_};                        \
  [[maybe_unused]] static T_ N_##_slot;                                        \
  [[maybe_unused]] static inline T_ get_##N_(void) {                           \
    Wn *n_ = winst_own_node(&N_##_decl_);                                      \
    w_memo_pull(n_);                                                           \
    return *(T_ *)w_read(n_); }                                                \
  static T_ N_##_impl_(void)

// ---------------------------------------------------------------- render-owned state
//
// The third declaration after `state` (one per page) and `state_in` (one per WInst the app
// owns): **one per render position**. A mounted view is an owner; so is each `for_each`
// row. The slot is created the first time the position reads it and dies with the position,
// so a row that survives a reorder keeps its state and a row that leaves takes it along.
//
//   state_own(i64, qty, 0);
//   view(Line) { el("td") { text_live("%lld", qty); el("button") { on_click(bump); } } }
//   action(bump) { set(qty, get(qty) + 1); }        // the row's own slot
//
// Two differences from `state_in` that are the whole point:
//   * nobody has to create or thread an instance — the framework owns it;
//   * a live binding may name it, because the node it writes is resolved per position
//     (`text_live` holds the declaration, not a file-scope node).
//
// The stack below is what "the position I am drawing" means, and it is separate from the
// `with_inst` stack: `state_in` keeps reading the instance the *app* bound, even inside a
// row, so adding render-owned state cannot change what an existing app reads.
WInst *winst_own_current(void);   // NULL outside a render position
WInst *winst_own_push(WInst *in); // returns the previous ambient owner
void  winst_own_pop(void);
// Pops until `in` itself has been popped: a *matched* pop. If a render position left the
// stack unbalanced, this repairs it instead of handing the next position the wrong owner
// (or a released one). With `in` NULL it pops nothing — the matching push never happened.
void  winst_own_pop_to(WInst *in);
// The declaration's node in the ambient owner's instance; panics (naming the declaration)
// when there is no owner, instead of quietly reading somebody else's slot.
Wn *winst_own_node(const WDecl *d);
// A heap instance for an owner the framework creates (a keyed item, a mounted view).
WInst *winst_alloc(void);
void  winst_release(WInst *in); // kills its nodes and frees the struct
// The panic unwinder's view of the owner stack (declared weak in core.h).
i32  wc_own_depth(void);
void wc_own_restore(i32 depth);

// ---------------------------------------------------------------- component instances
//
// The fourth answer, and the one a component author means: **one per component call**.
// `comp(Name, args)` (webc/dom.h) opens a render position of its own for the call it wraps,
// so a part used twice in one view has two states instead of one — the thing `state_own`
// alone cannot give you when the two copies sit inside the same view or the same row.
//
//   PART(Stepper, (label, Text));
//   void Stepper(Text label) { state_own… reads get(qty), this call's own slot }
//   view(Order) { comp(Stepper, "Chairs"); comp(Stepper, "Tables"); }
//
// Identity is `(the position that drew it, the call site)`: the call site is the counter the
// `comp()` macro captured, so a re-render finds the same instance, a row that moves keeps
// its components, and a branch that stops drawing one takes it (and its effects) with it.
// The table below is the position's, swept when the position's render ends — the same shape
// as a keyed list's item table, keyed by call site instead of by the app's key.
struct WInstChild {
  i64 site;
  bool live, used, ever; // `used` = claimed by this render; `ever` keeps find() honest
  WInst *in;             // heap, so a growing table never moves a node's owner pointer
};

WInst *winst_child(WInst *parent, i64 site); // find-or-create, marks it claimed this render
void   winst_sweep_children(WInst *parent);   // dispose the ones this render did not claim
WInst *winst_own_need(const char *what);      // the ambient position, or a panic naming it
// Start a declared effect in an instance, once: calling it on every render is a no-op,
// which is what lets `mount_own(x)` sit inline in a view body.
Wn *winst_effect_once(WInst *in, const WDecl *d);

// ---------------------------------------------------------------- lifecycle
//
// An instance is worth nothing without a way to clean up after it: the websocket it opened,
// the timer it armed, the observer it installed. `on_cleanup` registers a function that runs
// when the thing it was called in goes away — the effect run it was called in (before each
// re-run, Solid's rule) or, outside one, the component/mount/row instance. `on_mount` runs
// once, after the batch that created the position has gone out, which is when the node it
// wants to focus finally exists.
//
//   state_own(i64, draft, 0);
//   effect_own(save) { webc_later(800, flush_draft); on_cleanup(cancel_pending); }
//   view(Editor) { mount_own(save); el("textarea") { bind_str(draft); } }
//
void wc_on_cleanup(void (*fn)(void *), void *ctx);
void wc_on_mount(void (*fn)(void *), void *ctx);
// Run the queued on_mount hooks. The bridge calls this at the end of a flush (the batch has
// been emitted), and a native test calls it by hand after a render.
void wc_mount_run(void);
void wc_mount_clear(void);

i32 wc_mount_pending(void); // for tests and for the dev warning that says "nobody ran these"

#define on_cleanup(fn_, ctx_) wc_on_cleanup((void (*)(void *))(fn_), (void *)(ctx_))
#define on_mount(fn_, ctx_) wc_on_mount((void (*)(void *))(fn_), (void *)(ctx_))

// `state` reads its slot, an instance state reads the ambient instance (`with_inst`, which is
// what a view body is), an owned state reads the ambient owner — and each panics outside its
// own kind of scope, because a read that cannot know which copy it means is a bug, not a
// default. Where the instance is named rather than ambient (an action, a test, the second
// card): `get_in` / `set_in` / `mount_in`.
#define get_in(i_, N_) (get_##N_##_in(i_))
#define set_in(i_, N_, ...) (set_##N_##_in((i_), (__VA_ARGS__)))
#define mount_in(N_, i_) (N_##_start_in(i_))

// Per-instance state: component-local without closures.
#define CELL(T_) struct { Wn node; T_ slot; bool inited; }

[[maybe_unused]] static inline void wc_cell_init_impl(Wn *n, void *slot, size_t len,
                                                      const char *name, bool *inited) {
  if (*inited) return;
  *inited = true;
  w_sig_init(n, slot, len, name);
}
#define cell_init(c_, v_)                                                      \
  do {                                                                         \
    (c_).slot = (v_);                                                          \
    wc_cell_init_impl(&(c_).node, &(c_).slot, sizeof (c_).slot, #c_, &(c_).inited); \
  } while (0)
#define cget(c_) (*(typeof((c_).slot) *)w_read(&(c_).node))
#define cset(c_, ...)                                                          \
  do {                                                                         \
    typeof((c_).slot) _wc_v_ = (__VA_ARGS__);                                   \
    w_write(&(c_).node, &_wc_v_, sizeof _wc_v_);                                \
  } while (0)

// ctx-taking reactive units: the closure-free answer to `row => ...`. The row is
// the context (SPEC §7.2).
#define effect_with(N_, Ctx_)                                                  \
  static void N_##_impl(Ctx_ *c);                                             \
  static void N_##_run([[maybe_unused]] Wn *self_, void *ctx_) {                \
    N_##_impl((Ctx_ *)ctx_); }                                                  \
  [[maybe_unused]] static inline Wn *N_##_mount(WScope *s_, Ctx_ *c_) {         \
    Wn *e_ = w_new(nullptr, W_EFFECT, #N_);                                    \
    w_effect_init(e_, N_##_run, c_, #N_);                                      \
    w_own(s_, e_);                                                             \
    return e_; }                                                               \
  static void N_##_impl(Ctx_ *c)

#define computed_with(T_, N_, Ctx_)                                            \
  static T_ N_##_impl(Ctx_ *c);                                                \
  static void N_##_compute([[maybe_unused]] Wn *self_, void *ctx_) {            \
    *(T_ *)self_->val = N_##_impl((Ctx_ *)ctx_); }                              \
  [[maybe_unused]] static inline Wn *N_##_init(WScope *s_, Ctx_ *c_) {          \
    Wn *e_ = w_new(nullptr, W_MEMO, #N_);                                      \
    w_memo_init(e_, N_##_compute, c_, nullptr, sizeof(T_), #N_);                \
    w_own(s_, e_);                                                             \
    return e_; }                                                               \
  [[maybe_unused]] static inline T_ N_##_read(Wn *n_) {                         \
    w_memo_pull(n_);                                                           \
    return *(T_ *)w_read(n_); }                                                \
  static T_ N_##_impl(Ctx_ *c)

// ---------------------------------------------------------------- stores

// Nested reactivity without a second declaration: field nodes are created lazily
// from the model descriptor, so only the fields that are actually read are
// tracked and only the fields that are written notify (SPEC §13, createStore).
#define WEBC_STORE(N_, T_)                                                     \
  static T_ N_##__value;                                                       \
  static Wn N_##__fields[WC_FIELD_MAX];                                        \
  static unsigned char N_##__inited[WC_FIELD_MAX];                             \
  static Wn N_##__root = { .kind = W_SIGNAL, .val = &N_##__value,              \
                           .vlen = sizeof(T_), .alive = true, .name = #N_ };   \
  [[maybe_unused]] static inline T_ *N_##__ptr(void) { return &N_##__value; }  \
  [[maybe_unused]] static inline Wn *N_##__node(size_t off_) {                 \
    const WModel *m_ = T_##_modelof();                                         \
    for (i32 i_ = 0; i_ < m_->n; i_++) {                                       \
      if (m_->fields[i_].off != off_) continue;                                \
      if (!N_##__inited[i_]) {                                                 \
        N_##__inited[i_] = 1;                                                  \
        w_sig_init(&N_##__fields[i_], (char *)&N_##__value + off_,             \
                   m_->fields[i_].size, m_->fields[i_].name);                  \
      }                                                                        \
      return &N_##__fields[i_];                                                \
    }                                                                          \
    return &N_##__root;                                                        \
  }

#define sget(N_, field_)                                                       \
  (*(typeof(((typeof(N_##__value) *)0)->field_) *)                             \
        w_read(N_##__node(offsetof(typeof(N_##__value), field_))))
#define sset(N_, field_, ...)                                                  \
  do {                                                                         \
    typeof(((typeof(N_##__value) *)0)->field_) _wc_sv_ = (__VA_ARGS__);        \
    w_write(N_##__node(offsetof(typeof(N_##__value), field_)), &_wc_sv_,        \
            sizeof _wc_sv_);                                                   \
  } while (0)
#define sroot(N_) (*(typeof(N_##__value) *)w_read(&N_##__root))

#endif // WEBC_REACT_H