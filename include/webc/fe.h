// SPDX-License-Identifier: Apache-2.0
// webc/fe.h — the frontend application layer: context, async resources, a client
// router, typed forms, portals and block-shaped keyed lists (SPEC §7.3, §7.4, §13).
//
// These are the pieces SolidJS gives you around the reactivity core. All of them
// are closure-free: the "argument" a component would close over is a context
// pointer, which is what C hands around anyway.
#ifndef WEBC_FE_H
#define WEBC_FE_H

#include <webc/core.h>
#include <webc/pp.h>
#include <webc/model.h>
#include <webc/react.h>
#include <webc/dom.h>
#include <webc/ctx.h>

typedef struct WBuild WBuild;

// ---------------------------------------------------------------- context
//
//   WPROV_KEY(Theme);
//   WProv *theme = provide(wprov_current(), &Theme, "dark");
//   ...
//   const char *t = use_ctx(&Theme);
//
// The chain is a linked list of (key, value) pairs. `use_ctx` walks upwards, so a
// component three levels deep finds what an ancestor provided without the app
// threading it through every signature.
typedef struct WProvKey {
  const char *name;
} WProvKey;

#define WPROV_KEY(N_) static WProvKey N_ = {#N_}

typedef struct WProv {
  struct WProv *parent;
  const WProvKey *key;
  void *val;
} WProv;

WProv *provide(WProv *parent, const WProvKey *key, void *val);
void  *use_ctx_from(WProv *from, const WProvKey *key);       // panics when absent
void  *use_ctx_or(WProv *from, const WProvKey *key, void *dflt);
// The provider in scope for the build being rendered (null outside a render).
WProv *wprov_current(void);
// Provide a value for the rest of this render (the node is allocated in the
// build's scratch arena, so a per-render provide costs nothing that outlives it).
WProv *wprov_push(const WProvKey *key, void *val);

// ---------------------------------------------------------------- resource
//
//   WErrRes load_todo(WCtx *ctx, void *user, void *out, size_t size);
//   WRes *r = res_new(a, load_todo, &state, sizeof(Todo));
//   ...
//   if (res_loading(r)) { el("p") { text("Loading…"); } }
//   else if (!res_ok(r)) { text_s(res_err_str(r)); }
//   else { Todo *t = res_data(r); ... }
//
// A resource is a tiny state machine with signals behind it, so the view
// re-renders when the state changes. On the server the loader runs on a green
// task; in the browser it runs inline unless the app drives it from the JS bridge
// (`res_complete`), because wasm has no threads.
typedef struct WCtx WCtx;
typedef WErrRes (*WResLoad)(WCtx *ctx, void *user, void *out, size_t size);

typedef struct WRes WRes;

// Arena may be null (heap). `elem` is sizeof(T) for the data the loader fills in.
WRes *res_new(Arena *a, WResLoad load, void *user, size_t elem);
// Start (or restart) the load. Safe to call from a view; while a load is in
// flight a second call is ignored.
void res_start(WRes *r);
// The bridge path: the app completes the load when its fetch resolves.
void res_complete(WRes *r, WErrRes e, const void *data);
// Synchronous path (no scheduler, or a test): run the loader right now.
void res_run_now(WRes *r);
// Four states, because three of them look the same:
//
//   WRES_IDLE    nobody has called res_start() — the view has nothing to show yet
//   WRES_LOADING it was started and has not settled
//   WRES_OK      it settled, with data in res_data()
//   WRES_ERR     it settled with an error, in res_err_str()
typedef enum WResState : u8 { WRES_IDLE, WRES_LOADING, WRES_OK, WRES_ERR } WResState;

bool res_loading(WRes *r);  // started && !settled   (false before the first start)
bool res_idle(WRes *r);     // nobody asked yet
WResState res_state(WRes *r);
void res_ensure(WRes *r);   // start if idle, otherwise do nothing: "fetch once"
// ---------------------------------------------------------------- computed resources
//
// Some work is not a fetch: 21 strategies over 10 000 flips, a Monte Carlo walk, a
// median of medians. Left to the action it blocks the page for a quarter of a second; left
// to a timer callback it walks to 2 GiB (see js.h, the callback arena). So it is a resource:
//
// A computed resource is the same state machine as a fetched one, with the work split into
// chunks that yield to the browser between them:
//
//   static void build_table(WJob *j, void *ctx) {
//     State *s = ctx;
//     for (i32 i = 0; i < NSTRAT && job_tick(j); i++) {
//       simulate(&s->strats[i]);           // the expensive part
//       job_steps(j, i + 1, NSTRAT);       // what the progress bar reads
//     }
//   }
//   WRes *table = res_compute(nullptr, build_table, &g_state, sizeof(Run));
//   ...
//   res_ensure_key(table, key);            // in the view: start, or restart if key moved
//   if (res_loading(table)) { text("simulating %d%%", res_progress(table)); }
//   else { Run *run = res_data(table); … }
//
// The work function returns whenever it likes: `job_tick()` says "you still have time",
// and a function that returns without `job_done()` is resumed where it left off (the cursor
// is the app's own loop variable, kept in its own state — no closures, no coroutines).
// Between chunks the view re-renders, because a chunk ends in a host callback (§11a), so
// the progress bar advances without a signal of its own.
typedef struct WJob WJob;
typedef void (*WJobFn)(WJob *j, void *ctx);

WRes *res_compute(Arena *a, WJobFn fn, void *ctx, size_t elem);
// Start, or restart when `key` differs from the key the last run used. The key is the app's
// hash of everything the work depends on (a seed, a slider, the row count): one number, so
// the framework never has to guess at dependencies or compare a struct it does not know.
void res_ensure_key(WRes *r, u64 key);
void res_invalidate(WRes *r);              // the next res_ensure_key() runs again
bool job_tick(WJob *j);                    // false: out of time (return; you are resumed)
void job_done(WJob *j);                    // the work is complete
void job_steps(WJob *j, i32 done, i32 total); // what res_progress() reports
void *job_out(WJob *j);                    // the elem bytes to fill, already zeroed
bool job_cancelled(WJob *j);               // a newer key arrived: stop and start over
i32 res_progress(WRes *r);                 // 0..100
// The chunk budget, in ms, and a way to make it exact: a test sets 1 step per chunk so the
// chunked path is the path under test rather than a fiction exercised only in a browser.
void wc_job_budget_ms(f64 ms);
bool wc_jobs_alive(void); // a job is still running (a test drives it with wc_jobs_step)

bool res_ok(WRes *r);       // finished, no error
bool res_failed(WRes *r);
str  res_err_str(WRes *r);
void *res_data(WRes *r);    // null unless res_ok()
void res_destroy(WRes *r);

// ---------------------------------------------------------------- router
//
//   route("/todos/:id", TodoPage);
//   ...
//   WPageMatch m = front_route(location);
//   if (!m.found) m = front_route("/");
//
// Same path syntax as the server router (§6.3): `:name` is one segment, `*rest`
// is the tail. Matching is pure (no globals beyond the registration table), so a
// test can call it directly.
typedef void (*WPageFn)(WBuild *b, void *ctx);

#define WROUTE_MAX_PARAMS 8

typedef struct WPageMatch {
  bool found;
  WPageFn page;
  const char *pattern;
  i32 nparams;
  str keys[WROUTE_MAX_PARAMS];
  str vals[WROUTE_MAX_PARAMS];
} WPageMatch;

// Registers a page for a pattern; `route(pat, Fn)` is the readable form.
void front_route_add(const char *pattern, WPageFn page);
WPageMatch front_route(str path);
str front_route_param(WPageMatch *m, const char *name);
i32 front_route_count(void);

#define route(pat_, Page_) WC_FEROUTE_(__COUNTER__, pat_, Page_)
#define WC_FEROUTE_(C_, pat_, Page_) WC_FEROUTE__(C_, pat_, Page_)
#define WC_FEROUTE__(C_, pat_, Page_)                                                                \
  __attribute__((constructor)) static void WC_CAT(wc_route_reg_, C_)(void) {                       \
    front_route_add((pat_), (Page_));                                                              \
  }                                                                                                \
  static WPageFn WC_CAT(wc_route_use_, C_) __attribute__((unused)) = (Page_)

// ---------------------------------------------------------------- forms
//
//   state(Text, email, "");          // the input's live value
//   WForm f; Signup draft;
//   form_init(&f, Signup_modelof(), &draft);
//   el("input") { attr_c("name", "email"); wc_bind_str(&email_node, "email"); }
//   el("button") { on("submit", submit, &f); text("Sign up"); }
//   ...
//   draft.email = get(email);       // submit copies signals into the struct, then
//   form_submit(&f);                // the *server's* validators run over it
//
// `form_submit` copies nothing: it runs the model's validators over the struct the
// inputs are already bound to, which are the same validators the request runs
// (§7.4 — the shared-module promise). The errors come back per field, so the view
// can put each one next to its input.
typedef struct WForm {
  const WModel *m;
  void *obj;
  WErrs errs;
  bool submitted;
  // The id prefix the field kit (webc/form.h) builds `id`/`for`/`aria-describedby` from.
  // Two forms on one page — a create panel and an edit panel with the same model — would
  // otherwise emit the same ids twice, and a label would point at whichever came first.
  const char *prefix; // null: "f"
} WForm;

void form_init(WForm *f, const WModel *m, void *obj);
bool form_submit(WForm *f);                                  // runs the validators
str  form_error(WForm *f, const char *field);                // "" when the field is fine
bool form_field_ok(WForm *f, const char *field);
i32  form_error_count(WForm *f);
void form_reset(WForm *f);

// ---------------------------------------------------------------- portals
//
//   portal("#modal-root") { el("div") { ... } }
//
// Children go to the element the selector names, not to their lexical parent:
// dialogs and toasts escape the stacking context they sit in. Client-only — the
// server renders them in place, because HTML has nowhere else to put them.
WDom *wc_portal_open(const char *selector);
#define portal(sel_) WC_PORTAL_(__COUNTER__, sel_)
#define WC_PORTAL_(C_, sel_)                                                                       \
  for (WDom *WC_CAT(_wc_portal_, C_) __attribute__((cleanup(wc_el_pop))) = wc_portal_open(sel_);    \
       WC_CAT(_wc_portal_, C_); WC_CAT(_wc_portal_, C_) = nullptr)

// ---------------------------------------------------------------- stores
//
// A store owns the frames a live document arrives in: two generations that take
// turns, so a frame's strings and rows die the moment the frame after next
// replaces them. Without it every streaming client hand-rolls the same pair;
// `WFrame` on the server is its twin.
//
//   static WStore room;                       // once, before the first frame
//   store_init(&room, BoardDoc_docof(), sizeof(BoardDoc));
//   BoardDoc *d = store_read(&room, json);    // NULL = a bad frame; state unchanged
//   wc_doc_join(...);                         // relations, if the document has them
//
typedef struct WStore {
  Arena *a[2];     // the two generations
  void *docs[2];   // one decoded document per generation
  const WDoc *doc;
  u32 size;
  i32 cur;         // which generation is live; -1 before the first good frame
} WStore;

bool  store_init(WStore *s, const WDoc *d, u32 size);
void *store_read(WStore *s, str json); // decode into the fresh generation, swap
void *store_cur(WStore *s);            // the live document, or NULL
Arena *store_arena(WStore *s);         // the generation it lives in (for joins)
void  store_free(WStore *s);

// ---------------------------------------------------------------- keyed lists
//
//   for_each(t, todos, t.id) { el("li") { text_s(t.title); } }
//
// The block runs per item, the item pointer is bound to the first argument, and
// reconciliation is keyed by the third: reordering moves nodes instead of
// re-creating them, and a removed item's scope is disposed (§7.3).
void wkeyed_sweep_here(void);
// The block-shaped keyed list. __COUNTER__ is captured once per expansion so the
// loop's internals never collide, even nested.
#define for_each_ptr(P_, v_) WC_FE_FORP(__COUNTER__, P_, v_)
#define WC_FE_FORP(C_, P_, v_) WC_FE_FORP_(C_, P_, v_)
#define WC_FE_FORP_(C_, P_, v_)                                                                    \
  for (typeof_unqual((v_).at) P_ = (v_).at, WC_CAT(wc_end_, C_) = (v_).at + (v_).n;                        \
       P_ < WC_CAT(wc_end_, C_); P_++)

//   for_each(t, todos) { el("li") { text("%s", t->title); } }
//
// The key comes from the collection's descriptor (its `.pk`), so the everyday form
// takes two arguments; the three-argument form stays for a key the model does not
// carry (a hash, a composite, an index in a list of scalars).
//
// **`for_each` is a *render* loop; it is not a way to iterate.** Each pass opens a keyed item
// scope on the node that is currently open, and the loop's last statement sweeps every item of
// that node the pass did not claim (`wkeyed_sweep_here`). That is what makes a removed row's
// scope disposed. It is also why a `for_each` that renders *nothing* — a count, a sum, a
// `find` — is not harmless: it runs the sweep on the open node anyway. Found the hard way,
// writing `cards_in()` in examples/board: the count ran before every `<section>`, swept the
// items of `<main>`, and two of the three lanes vanished from the page.
//
// To count, sum or search, write the loop:
//
//   i32 n = 0; for (i32 i = 0; i < vlen(rows); i++) n += vat(rows, i).lane == lane;
//
// `for_each_ptr` is the same loop with no keyed scopes at all, when the pointer is what you
// want and the items are not being rendered.
#define for_each(...) WC_CAT(WC_FE_SEL_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_FE_SEL_2(P_, v_) WC_FE_FORK(__COUNTER__, P_, v_)
#define WC_FE_SEL_3(P_, v_, KEY_) WC_FE_FOR(__COUNTER__, P_, v_, KEY_)

#define WC_FE_FORK(C_, P_, v_) WC_FE_FORK_(C_, P_, v_)
#define WC_FE_FORK_(C_, P_, v_)                                                                    \
  for (i32 WC_CAT(wc_fl_, C_) = 0; WC_CAT(wc_fl_, C_) == 0;                                         \
       wkeyed_sweep_here(), WC_CAT(wc_fl_, C_) = 1)                                                 \
    for (typeof_unqual((v_).at) P_ = (v_).at, WC_CAT(wc_end_, C_) = (v_).at + (v_).n;                      \
         P_ < WC_CAT(wc_end_, C_); P_++)                                                            \
      for (WKeyedGuard WC_CAT(wc_kd_, C_) __attribute__((cleanup(wkeyed_guard_close))) =            \
               wkeyed_guard_open(wc_vec_key((v_).desc, P_), (void *)P_);                            \
           !WC_CAT(wc_kd_, C_).done; WC_CAT(wc_kd_, C_).done = 1)

#define WC_FE_FOR(C_, P_, v_, KEY_) WC_FE_FOR_(C_, P_, v_, KEY_)
#define WC_FE_FOR_(C_, P_, v_, KEY_)                                                               \
  for (i32 WC_CAT(wc_fl_, C_) = 0; WC_CAT(wc_fl_, C_) == 0;                                         \
       wkeyed_sweep_here(), WC_CAT(wc_fl_, C_) = 1)                                                 \
    for (typeof_unqual((v_).at) P_ = (v_).at, WC_CAT(wc_end_, C_) = (v_).at + (v_).n;                      \
         P_ < WC_CAT(wc_end_, C_); P_++)                                                            \
      for (WKeyedGuard WC_CAT(wc_kd_, C_) __attribute__((cleanup(wkeyed_guard_close))) =            \
               wkeyed_guard_open((i64)(KEY_), (void *)P_);                                          \
           !WC_CAT(wc_kd_, C_).done; WC_CAT(wc_kd_, C_).done = 1)

#endif // WEBC_FE_H