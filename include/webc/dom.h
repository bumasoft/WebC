// SPDX-License-Identifier: Apache-2.0
// WebC — the markup builder. One tree, three renderers: SSR string, wasm DOM
// commands, and (later) hydration of an SSR tree in the browser.
//
//   el("div") {
//     el("h1") { text("Hello, %s", name); }
//     el("button") { on("click", increment, nullptr); text("+1"); }
//   }
//
// The braces are real C block scopes; the cleanup attribute pops the builder
// stack when they close (SPEC §5).
#ifndef WEBC_DOM_H
#define WEBC_DOM_H

#include <webc/core.h>
#include <webc/react.h>

typedef struct WDom WDom;
typedef struct WBuild WBuild;
typedef struct WItem WItem;

typedef void (*WEventFn)(void *ctx, const char *type);

typedef enum WDomType : u8 { WD_ELEMENT, WD_TEXT, WD_RAW, WD_FRAGMENT } WDomType;

typedef struct WAttrPair {
  str name;
  str value;
  bool reactive; // changed during the last patch (emitted to the DOM)
} WAttrPair;

typedef struct WListener {
  str event;
  WEventFn fn;
  void *ctx;
  bool delegated;
} WListener;

struct WDom {
  WDomType type;
  const char *tag;
  str text;
  WAttrPair *attrs;
  i32 nattrs, capattrs;
  WListener *listeners;
  i32 nlisteners, caplisteners;
  WDom *parent, *first, *last, *next, *prev;
  WDom *was_parent; // `parent` at the start of this render: reuse matches on (was_parent,
                    // tag), so a branch that appears in the middle of a view does not shift
                    // every node after it onto the wrong element (see take_reuse in dom.c)
  WDom *was_prev;   // `prev` at the start of this render: a reused keyed item keeps its
                    // nodes but changes place in the list, and that is a `move` op
  WDom *was_first;  // `first` at the start of this render: an item fragment's previous
                    // children, so reuse inside a keyed item matches its OWN nodes and
                    // not whatever the global cursor happens to be pointing at
  WDom *was_next;   // `next` at the start of this render: the chain was_first walks
  WScope *scope; // per-item scope, disposed when the item leaves
  i32 id;        // stable id for hydration / bridge calls
  i32 key_i;
  str key_s;
  bool keyed;
  bool dead;
  bool is_new;           // created during this build (emit a create op)
  bool wrap;             // the framework's own wrapper fragment: the several roots of one
                         // view. A keyed item's fragment is NOT a wrapper — it owns a
                         // definite place in the tree, and reuse must not match it to
                         // any other fragment (see same_place in dom.c)
  bool text_dirty;       // text differs from the previous value
  bool attrs_dirty;      // at least one attribute differs
  WAttrPair *prev_attrs; // attributes from the previous run, for diffing
  i32 nprevattrs;            // how many the previous run had
  void *items;    // WItemTable* for keyed lists
  void *inst;     // the per-instance state (webc/react.h) in scope when built:
                  // the bridge pushes it back around an event, so `get()` works in
                  // an action of a mounted component
  void *owner;    // the render position that drew this node (a mounted view or one
                  // for_each row): the bridge pushes it back around an event, so a
                  // `state_own` read in an action is the row's own slot
  void *bind_sig; // two-way binding target (a Wn* signal)
  // Two copies of the value, alternated on every write (see wdom_apply_binding): the signal
  // points into one of them, so a reader that is holding the previous value keeps a valid
  // buffer for a whole render instead of watching it being freed and re-filled underneath —
  // which is how a typed field came back as "\0\0\0\0 duty limits".
  char *bind_val[2];
  i32 bind_len[2];
  i32 bind_parity;
  void *live; // WLive*: the live bindings attached to this node (dom.c, `text_live`)
  i32 seen;       // render number that last touched this node
};

struct WItem {
  i32 key;
  WScope scope;
  WDom *node;
  bool reused;
};

struct WBuild {
  Arena *a;           // scratch: strings, attribute snapshots (reset per render)
  Arena *nodes_arena; // tree nodes: live for the component's lifetime
  WDom *root;
  WDom *cur;
  WScope *scope;
  i32 next_id;
  i32 depth;
  bool ssr;
  WDom **reuse; // nodes from the previous run of the same effect
  i32 nreuse, capreuse, cursor;
  WDom **seq; // every node touched this run, in pre-order
  i32 nseq, capseq;
  bool *id_used;       // ids held by the previous run, so a new node never
  i32 id_cap, id_scan; // takes an id the browser already knows
  i32 nnodes;          // nodes this render created or reused = the size of the tree
  i32 run;             // this render's number: a node is live iff seen == run
  i32 patches;         // emitted DOM operations (patch cost, for tests)
  struct WItemTable *items;
  void *item_ptr;           // the element the current keyed block is rendering
  struct WBuild *enclosing; // the build this one was started inside, if any
  struct WProv *prov;       // the context chain in scope (webc/fe.h provide/use)
  struct WDomBatch *out;    // where a live binding appends its op (the component's batch)
  void *comp_root;          // a render with no position of its own (a bare wbuild): comp()
                            // hangs its instances here and they die with the build
};

// ---------------------------------------------------------------- builder

WBuild *wbuild_begin(Arena *a, WScope *scope);
void wbuild_end(WBuild *b);
WBuild *wbuild_current(void);
// Called by a panic handler: forgets the build a view left half-written.
WBuild *wbuild_abandon(void);
WDom *wbuild_root(WBuild *b);

WDom *wc_el_open(const char *tag);
void wc_el_pop(WDom **slot);
void wc_text_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void wc_text_str(str s);
void wc_raw_static(const char *s);
void wc_attr(const char *name, str value);
void wc_attr_c(const char *name, const char *value);
void wc_attr_i64(const char *name, i64 value);
void wc_attr_bool(const char *name, bool value);
void wc_bool_attr(const char *name, bool on); // present-only attribute
void wc_on(const char *event, WEventFn fn, void *ctx);
void wc_class(str value);
void wc_style(const char *decls);
void wc_bind_str(void *sig, const char *name); // two-way bind for inputs

// ---------------------------------------------------------------- live bindings
//
// A value that is *written into this node* when its signal changes — no view re-run, no
// diff. This is the half of the reactivity model a re-render cannot give you: the view
// runs once, the binding stays, and a 60 Hz pointer move costs one DOM op.
//
//   state(i64, count, 0);   state(bool, open, false);
//   view(Counter) {
//     el("span") { text_live("%lld", count); }            // the text node
//     el("li")   { class_live("open", open); }             // one class, in place
//     el("div")  { style("width:1px"); show_live(open); }  // display:none, in place
//     el("i")    { attr_live("data-n", count); }           // one attribute
//   }
//
// The binding belongs to the node it was declared on (a text node for `text_live`, the
// open element for the rest), so a keyed list that moves or removes the row takes its
// bindings with it. It only ever writes a leaf — text, one attribute — never structure:
// an `if`, a `for_each` or a part still needs the render, which is why a live action
// (`action_live`) may write signals and must not change what the view draws.
//
// The signal is a `state(…)` or a `state_own(…)`: the declaration is what gives the macro the
// value's type (`WC_SIGKIND_`), and an owned declaration is resolved to *this row's* node at
// render time, which is what makes a binding inside a `for_each` mean one value per row. A
// `state_in` is not a binding target (it has no single node to bind to — name the instance
// with `get_in` and re-render instead). Default formatting for the attribute bindings is
// `%lld`/`%d`/`%g`/`true`/the string; `text_live` takes the format `text()` would have taken.
typedef enum WBValKind : u8 {
  WLV_I32,
  WLV_U32,
  WLV_I64,
  WLV_U64,
  WLV_F64,
  WLV_F32,
  WLV_BOOL,
  WLV_TEXT,
  WLV_UNSUPPORTED_ // a type the bindings do not know: reported by name, once
} WBValKind;

// One door behind the four macros: `verb` is "text", "attr", "class" or "show", and
// `param` is the format, the attribute name, the class name, or null. Four spellings for
// the view, one function to keep working.
void wc_live(const char *verb, const char *param, Wn *sig, u8 valkind);

// The declaration is the type: `count_slot` is what `state(i64, count, 0)` declared, so
// `_Generic` picks the reader here and there is no second place to say "this is an i64".
#define WC_SIGKIND_(v_)                                                                          \
  _Generic((v_), i32: WLV_I32, u32: WLV_U32, i64: WLV_I64, u64: WLV_U64, f64: WLV_F64,            \
           f32: WLV_F32, bool: WLV_BOOL, char *: WLV_TEXT, const char *: WLV_TEXT,                \
           default: WLV_UNSUPPORTED_)

#define text_live(fmt_, N_) wc_live("text", (fmt_), &N_##_node, WC_SIGKIND_(N_##_slot))
#define attr_live(name_, N_) wc_live("attr", (name_), &N_##_node, WC_SIGKIND_(N_##_slot))
#define class_live(cls_, N_) wc_live("class", (cls_), &N_##_node, WC_SIGKIND_(N_##_slot))
#define show_live(N_) wc_live("show", nullptr, &N_##_node, WC_SIGKIND_(N_##_slot))

// Did this listener run only write signals? `action_live` says so, and the bridge then
// sends the bindings' ops instead of re-rendering every mounted view. The promise has
// teeth: an action that also changes what the view draws (a vector, a struct field the
// markup reads) leaves the page stale, because no render follows it.
void wc_live_mark(void);
void wc_live_reset(void);
i32 wc_live_marks(void);

// Bridge support: id -> node index, and a bound input's new value.
i32 wdom_index(WDom *root, WDom **out, i32 cap);
i32 wdom_id_space(WDom *root); // ids the tree uses: size `out` with this
bool wdom_apply_binding(WDom *n, str value);

#define el(tag_) WC_EL_(__COUNTER__, tag_)
#define WC_EL_(C_, tag_)                                                                           \
  for (WDom * WC_CAT(_wc_el_, C_) __attribute__((cleanup(wc_el_pop))) = wc_el_open(tag_);          \
       WC_CAT(_wc_el_, C_); WC_CAT(_wc_el_, C_) = nullptr)

// WC_AS_STR (a Text, a `str` or a literal, chosen at compile time) lives in core.h.

// `text(x)` is a value; `text(fmt, ...)` is a format. The argument count decides,
// at compile time, and one argument is always the safe reading:
//
//   text("50% off")        -> 50% off        (a format would be UB)
//   text(story->title)     -> the title      (a format would corrupt it, silently)
//   text("%s/%d", a, 7)    -> a/7            (a format, as before)
//
#define text(...) WC_CAT(WC_TEXT_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_TEXT_1(v_) wc_text_str(WC_AS_STR(v_))
#define WC_TEXT_2(f_, a_) wc_text_fmt(f_, a_)
#define WC_TEXT_3(f_, a_, b_) wc_text_fmt(f_, a_, b_)
#define WC_TEXT_4(f_, a_, b_, c_) wc_text_fmt(f_, a_, b_, c_)
#define WC_TEXT_5(f_, a_, b_, c_, d_) wc_text_fmt(f_, a_, b_, c_, d_)
#define WC_TEXT_6(f_, a_, b_, c_, d_, e_) wc_text_fmt(f_, a_, b_, c_, d_, e_)
#define WC_TEXT_7(f_, a_, b_, c_, d_, e_, g_) wc_text_fmt(f_, a_, b_, c_, d_, e_, g_)
// Authored markup — an icon, a fragment of HTML, a file under partials/ — is `markup()`:
// one call, a name that says what the bytes are. (A file the server owns is
// `partial("site/head")` — http.h — and a fragment both halves render is `#embed`.)
#define markup(s_) wc_raw_static(s_)

// ---------------------------------------------------------------- prose
//
// Long copy is text, not markup: paragraphs separated by a blank line, and four pieces
// of inline punctuation (**bold**, *italic*, `code`, [label](/href)). prose() renders it
// as <p> elements with everything else escaped; prose_inline() renders one run without
// the paragraph wrapper. The vocabulary is closed on purpose — it is not Markdown.
//
//   prose(story->body);                       // one <p> per paragraph
//   prose_with(c->body, .first = "first-p",   // the first paragraph styled differently
//              .after = pullquote, .ctx = st); // called after each paragraph, 1-based
//   prose_inline(st->standfirst);             // a run of text inside your own element
//
// A paragraph hook runs between paragraphs, so an app can put its own furniture there — a
// pull-quote, a promo — without the framework knowing what that is.
typedef struct WProseOpts {
  const char *class;      // class on every <p>
  const char *first;      // class on the first <p> only (wins over `class`)
  void (*after)(i32 paragraph, void *ctx); // after each paragraph, 1-based
  void *ctx;
  const char *link_class; // class on links made from [label](href)
} WProseOpts;

i32 wc_prose(str text);
i32 wc_prose_with(str text, WProseOpts o);
void wc_prose_inline(str text);

#define prose(s_) wc_prose(WC_AS_STR(s_))
#define prose_with(s_, ...) wc_prose_with(WC_AS_STR(s_), (WProseOpts){__VA_ARGS__})
#define prose_inline(s_) wc_prose_inline(WC_AS_STR(s_))

// ---------------------------------------------------------------- attributes
//
// One spelling per concept. Every one of these takes a Text, a `str` or a
// literal, chosen at compile time (WC_AS_STR), so view code never casts:
//
//   attr("href", url)   class_("lead is-open")   style("--acc: #3d4ee0")
//   id_("reader")       href(url)                src("/art/bands.svg")
#define attr(n_, v_) wc_attr((n_), WC_AS_STR(v_))
#define attr_i(n_, v_) wc_attr_i64((n_), (v_))
#define on(ev_, fn_, ctx_) wc_on((ev_), (fn_), (ctx_))
#define class_(v_) wc_class(WC_AS_STR(v_))
// A style is a declaration list, not a format: `%` in `width:50%` is a value.
#define style(s_) wc_style_str(WC_AS_STR(s_))
#define show(cond_) wc_show((cond_))

// The short forms: one attribute, one name, no cast.
#define id_(v_) wc_attr("id", WC_AS_STR(v_))
#define href(v_) wc_attr("href", WC_AS_STR(v_))
#define src(v_) wc_attr("src", WC_AS_STR(v_))
#define alt(v_) wc_attr("alt", WC_AS_STR(v_))
#define title_(v_) wc_attr("title", WC_AS_STR(v_))
#define role(v_) wc_attr("role", WC_AS_STR(v_))
#define rel(v_) wc_attr("rel", WC_AS_STR(v_))
#define lang(v_) wc_attr("lang", WC_AS_STR(v_))
#define target(v_) wc_attr("target", WC_AS_STR(v_))
#define value_(v_) wc_attr("value", WC_AS_STR(v_))

// ---------------------------------------------------------------- the `_fmt` companions
//
// Every helper above takes a Text, a str or a literal (WC_AS_STR), so `class_(fmt(…))` has
// always worked. These exist because a developer writes `class_fmt(…)` first, and that guess
// should compile rather than teach.
#define class_fmt(...) class_(fmt(__VA_ARGS__))
#define style_fmt(...) style(fmt(__VA_ARGS__))
#define attr_fmt(n_, ...) attr((n_), fmt(__VA_ARGS__))
#define post_to_fmt(...) post_to(fmt(__VA_ARGS__))
#define href_fmt(...) href(fmt(__VA_ARGS__))
#define src_fmt(...) src(fmt(__VA_ARGS__))
#define id_fmt(...) id_(fmt(__VA_ARGS__))

// ---------------------------------------------------------------- sb(): a string built
//                                                                 for this render
//
// A polyline's `points`, a path's `d`, a class list built from a loop: the appends a
// render produces do not deserve a file-scope buffer with a lifetime note on it. `sb()`
// hands back a builder that lives in the render's own arena, so the string is valid
// exactly as long as the render needs it — which is exactly as long as an attribute is.
//
//   Buf *b = sb();
//   for (i32 i = 0; i < n; i++) { sb_num(b, x[i], 1); sb_putc(b, ','); sb_num(b, y[i], 1); sb_putc(b, ' '); }
//   el("polyline") { attr("points", sb_str(b)); }
//
// Each call is a fresh builder, so two in one expression (a curve and the fat hit line
// that shares its points) cannot overwrite each other. Outside a render — a test, a tool,
// a log line — the current scope arena is used instead, so the same code still works.
Buf *sb(void);
str  sb_str(Buf *b);                    // the bytes so far, for attr()/text()
const char *sb_cstr(Buf *b);            // NUL-terminated, for fprintf and friends
void sb_puts(Buf *b, const char *s);
void sb_putstr(Buf *b, str s);
void sb_putc(Buf *b, char c);
void sb_i64(Buf *b, i64 v);
void sb_u64(Buf *b, u64 v);
void sb_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// A double with n decimals and no jitter: chart coordinates land on the same text for the
// same value, so a patch that did not change says so.
void sb_num(Buf *b, double v, int decimals);

// ---------------------------------------------------------------- the form kit
//
// The four attributes every form input needs, spelled once each. A form that
// posts and redirects works with JavaScript off, which is what makes one view
// serve both halves:
//
//   el("form") { post_to("/todos/toggle"); hidden_i("id", t->id); ... }
#define post_to(url_)                                                          \
  do {                                                                         \
    wc_attr_c("method", "post");                                              \
    wc_attr("action", WC_AS_STR(url_));                                        \
  } while (0)
// A hidden field is a whole element, so it is one call: an id, not an id plus
// three attributes in the right order.
#define hidden_i(n_, v_)                                                       \
  el("input") {                                                                \
    wc_attr_c("type", "hidden");                                              \
    wc_attr_c("name", (n_));                                                   \
    wc_attr_i64("value", (v_));                                                \
  }
#define hidden_s(n_, v_)                                                       \
  el("input") {                                                                \
    wc_attr_c("type", "hidden");                                              \
    wc_attr_c("name", (n_));                                                   \
    wc_attr_c("value", (v_));                                                  \
  }
#define name(v_) wc_attr("name", WC_AS_STR(v_))
#define placeholder(v_) wc_attr("placeholder", WC_AS_STR(v_))
#define type_(v_) wc_attr("type", WC_AS_STR(v_))
#define required() wc_bool_attr("required", true)
#define checked(on_) wc_bool_attr("checked", (on_))
#define disabled(on_) wc_bool_attr("disabled", (on_))
// ---------------------------------------------------------------- client hooks
//
// The view never names an event; the client half never names a selector. An element
// is *named* in the view and *bound* in the client file:
//
//   shared view:   el("button") { named("tick"); text("✓"); }
//   client file:   bind("tick", "click", toggle);
//
// Inside a keyed list the binding receives the item (`action(toggle, Todo *t)`);
// outside one it receives null. On a server render `bind` is never called, so the
// same element is inert markup — which is what a no-JavaScript page wants.
void wc_style_str(str decls);                      // style("…") without a cast
void wc_hook(const char *name);                    // mark the current element

// ---------------------------------------------------------------- progressive enhancement
//
// The pattern this framework exists to make normal, spelled once:
//
//   // chrome, rendered by the server and by the module alike
//   enhance("reader") { el("form") { post_to("/theme"); … } }
//   // the module mounts the same view over it
//   client_main { mount(Reader, "#reader"); }
//
// `enhance` opens `<div id="…" data-enhance="…">`, renders the no-JavaScript
// fallback inside it, and the module replaces the contents when it mounts. Nothing
// in the app has to know which half is running.
//
// A control that exists *outside* the mount point (a theme switch in the header)
// marks itself so the module can retire it once it is running:
//
//   el("form") { scripted_hide("themeswitch"); … }   // data-scripted-hide="themeswitch"
//
// assets/webc.js sets `<html data-wasm="1">` and hides every marked element when
// the module has started, so the two controls never appear together.
#define enhance(id_) WC_ENHANCE_(__COUNTER__, id_)
#define WC_ENHANCE_(C_, id_)                                                   \
  for (WDom *WC_CAT(wc_en_, C_) __attribute__((cleanup(wc_el_pop))) =           \
           wc_el_open("div");                                                  \
       WC_CAT(wc_en_, C_); WC_CAT(wc_en_, C_) = nullptr)                        \
    for (int WC_CAT(wc_env_, C_) = (wc_attr("id", str_lit(id_)),                  \
                                    wc_attr("data-enhance", str_lit(id_)), 1);   \
         WC_CAT(wc_env_, C_); WC_CAT(wc_env_, C_) = 0)

#define scripted_hide(name_) wc_attr("data-scripted-hide", WC_AS_STR(name_))
void wc_bind_hook(const char *name, const char *event, void *fn);
void wc_bind_hook_soft(const char *name, const char *event, void *fn);
void wc_bind_field(const char *name, void *sig);   // a signal behind an input
void wc_input_hook(const char *name);              // mark the current input

#define named(v_) wc_hook((v_))
// The third argument is the *name* of an action, not its symbol: `bind("tick",
// "click", toggle)` live `action_toggle`, so the view and the client agree on one
// word for the behaviour.
#define bind(hook_, ev_, N_) wc_bind_hook((hook_), (ev_), (void *)WC_CAT(action_, N_))
// A binding whose control lives behind a branch — a panel that is only rendered once a row is
// selected. It is wired, it just is not on the page yet, and the dead-binding report would
// name it on every load. `bind_soft` says "this one is conditional"; the report keeps its
// teeth for the case that matters, a name no view ever renders (a typo).
#define bind_soft(hook_, ev_, N_) wc_bind_hook_soft((hook_), (ev_), (void *)WC_CAT(action_, N_))
#define input_hook(N_) wc_input_hook(#N_)
#define bind_input(N_) wc_bind_field(#N_, &N_##_node)

// One call, in the view, and the field is live — no second line in client_main to forget:
//
//   el("input") { attr("name", "reason"); bind_str(reason); }
//
// It is wc_bind_str() underneath, which is what input_hook().bind_input() sets up in two
// steps. That pair is order-sensitive (the hook resolves its field when the view runs, so a
// bind_input() written after it in the same view registers the field too late) and it fails
// silently when it is got wrong; this form cannot be got wrong. See wc_input_hook() for the
// report that catches the mistake.
#define bind_str(N_) wc_bind_str(&N_##_node, #N_)

// The browser entry point, without an #ifdef in application code: the
// framework compiles `client_main { ... }` to the module's entry on the wasm
// target and to the same-named function everywhere else, so a native test can call
// it and a server build can link the file without a special case.
#ifndef __wasm__
int wc_app_main(void) __attribute__((weak));
#endif
#define client_main int wc_app_main(void)

// ---------------------------------------------------------------- parts
//
// A part is a view's composition unit: a plain C function with TYPED arguments,
// callable from any view or any other part. The signature is written once, as
// tuples, and the compiler checks every call site — which a five-int helper
// (const Card*, i32 lane, i32 at, i32 in_lane) never did.
//
//   parts.h:  PART(Avatar, (name, Text), (hue, i64), (online, bool))
//   parts.c:  void Avatar(Text name, i64 hue, bool online) {
//               el("span") { class_("avatar"); text("%s", name); }
//             }
//
// PART declares; the definition is the plain C function it declared — the body is
// ordinary view code (el/text/attr, for_each, other parts), and callers are
// compile-checked against the declaration. A definition macro that also spelled
// the parameters would say them twice; half of this framework is not saying
// things twice.
#define WC_PART_ARG(g) WC_PART_ARG_I g
#define WC_PART_ARG_I(id, FT, ...) FT id
#define PART(N_, ...) void N_(__VA_OPT__(WC_MAP(WC_PART_ARG, __VA_ARGS__)))

// ---------------------------------------------------------------- component instances
//
// A part is a function; a *component* is that function plus a state of its own. `comp()`
// says which call site is which, so two calls of one part no longer share one slot:
//
//   PART(Stepper, (label, Text), (max, i64));
//   void Stepper(Text label, i64 max) {
//     state_own(i64, qty, 0);              // in the file, once
//     el("div") { text_live("%lld", qty); el("button") { on_click(inc); } }
//   }
//
//   view(Order) {
//     el("main") {
//       comp(Stepper, "Chairs", 9);        // its own qty
//       comp(Stepper, "Tables", 3);        // and this one's
//     }
//   }
//
// What the call site buys, in one list, because every item is a promise the framework keeps:
//
//   * `state_own` and `computed_own` inside the part resolve to *this* call (identity is
//     `(the position that drew it, the call site)`, the site being the counter below);
//   * `effect_own` + `mount_own` start per instance and die with it (`react.h`);
//   * `on_cleanup` in the part, or in one of its effects, runs when the call goes away —
//     a branch stops drawing it, a row leaves, the mount is destroyed;
//   * `text_live`/`attr_live`/`class_live`/`show_live`, `bind_str` and every action already
//     resolve through the node's owner, so they inherit the instance with no extra wiring;
//   * a re-render finds the same instance (the state survives), a row that moves in a keyed
//     list keeps its components, and a component that stops being drawn takes its state and
//     its subscriptions with it.
//
// A part called *without* `comp()` keeps the older, coarser rule: it draws in the position
// that called it, and its `state_own` is shared with every other call in that position. That
// is what makes the change opt-in — `webc check --wiring` names the parts where it matters.
//
// Not a compile-time wrapper: a C function body cannot be spliced into a builder, so the
// `cleanup` + `for` idiom (the one `el` and `dialog` use) is what closes the frame on every
// exit path — return, break, and the panic unwinder.
typedef struct WCompFrame {
  WInst *in;
  bool open;
} WCompFrame;

WCompFrame wc_comp_enter_(i64 site); // pushes the call site's instance as the ambient position
void wc_comp_leave_(WCompFrame *f);  // pops it (cleanup-linked: runs on every exit path)

#define comp(N_, ...) WC_COMP_(__COUNTER__, N_ __VA_OPT__(, ) __VA_ARGS__)
// The loop variable is the *key*, not the frame: the cleanup attribute gets the frame by
// address, and a frame whose `open` was already cleared could not pop the position it pushed.
#define WC_COMP_(C_, N_, ...)                                                             \
  for (WCompFrame WC_CAT(wc_cf_, C_) __attribute__((cleanup(wc_comp_leave_))) =            \
           wc_comp_enter_((i64)(C_)),                                                     \
                  *WC_CAT(wc_keep_, C_) = &WC_CAT(wc_cf_, C_);                            \
       WC_CAT(wc_keep_, C_); WC_CAT(wc_keep_, C_) = nullptr)                               \
    N_(__VA_ARGS__)

// ---------------------------------------------------------------- children
//
// A function cannot take a block, so a component that wraps other markup takes a *callback*
// and a context — the same `(fn, ctx)` pair `wkeyed_list` and `prose_with` already use. The
// builder is ambient, so the children land exactly where the part calls `kids()`:
//
//   PART(Card, (title, Text), (inner, WKids));
//   void Card(Text title, WKids inner) {
//     el("section") { class_("card"); el("h3") { text(title); } kids(inner); }
//   }
//
//   static void body(void *ctx) { el("p") { text("%s", (const char *)ctx); } }
//   comp(Card, "Hello", kids_of(body, msg));
//
// `kids_of` remembers the position that wrote the call, and `kids()` re-enters it while the
// children run: markup the caller wrote reads the *caller's* state, which is what a `state_own`
// in `body` above means (React's rule too — children belong to the parent).
typedef struct WKids {
  void (*fn)(void *);
  void *ctx;
  WInst *pos; // the position the children belong to (captured by kids_of)
} WKids;

void wc_kids_(WKids k);

// `kids_of` is written inside a `comp(...)` argument list, where the ambient position is
// already the component being entered — and the children are the *caller's* markup. So the
// position captured is the component's parent: one level out.
[[maybe_unused]] static inline WInst *wc_kids_pos(void) {
  WInst *cur = winst_own_current();
  return (cur && cur->parent) ? cur->parent : cur;
}

#define kids_of(fn_, ctx_)                                                                    \
  ((WKids){.fn = (void (*)(void *))(fn_), .ctx = (void *)(ctx_), .pos = wc_kids_pos()})
#define kids(k_) wc_kids_(k_)
#define no_kids ((WKids){0})

// ---------------------------------------------------------------- views
//
//   view(Todos, VecTodo *todos) { el("main") { ... } }
//   view(About)                { el("main") { text("About"); } }
//
// The body is the view. The `(WBuild *, void *)` signature the renderer needs is
// generated, the context is cast once, and nothing has to be silenced with
// `(void)ctx;` because there is no unused parameter to silence. The generated
// symbol is `view_<Name>`, external, so `page(Todos, ctx)`, `mount(Todos, "#root")`
// and a test can all name the same function.
#define view(N_, ...)                                                          \
  WC_CAT(WC_VIEW, __VA_OPT__(1))(__COUNTER__, N_ __VA_OPT__(, __VA_ARGS__))

#define WC_VIEW(C_, N_)                                                        \
  void WC_CAT(view_, N_)(WBuild *, void *);                                    \
  static void WC_CAT(wc_view_body_, N_)(void);                                 \
  void WC_CAT(view_, N_)(WBuild *WC_CAT(wc_b_, C_), void *WC_CAT(wc_ctx_, C_)) { \
    (void)WC_CAT(wc_b_, C_);                                                   \
    (void)WC_CAT(wc_ctx_, C_);                                                 \
    WC_CAT(wc_view_body_, N_)();                                               \
  }                                                                            \
  static void WC_CAT(wc_view_body_, N_)(void)

#define WC_VIEW1(C_, N_, ...)                                                  \
  void WC_CAT(view_, N_)(WBuild *, void *);                                    \
  static void WC_CAT(wc_view_body_, N_)(__VA_ARGS__);                          \
  void WC_CAT(view_, N_)(WBuild *WC_CAT(wc_b_, C_), void *WC_CAT(wc_ctx_, C_)) { \
    (void)WC_CAT(wc_b_, C_);                                                   \
    WC_CAT(wc_view_body_, N_)(WC_CAT(wc_ctx_, C_));                            \
  }                                                                            \
  static void WC_CAT(wc_view_body_, N_)(__VA_ARGS__)

// A view whose context has a *name* — the type the route fills by field:
//
//   view(SectionPage, SectionCtx *c) { Site *site = c->site; ... }
//   GET("/s/:slug") {
//     return render(SectionPage, .site = site, .section = sec, .list = list);
//   }
//
// `view(N, T *c)` emits `typedef T N_ctx;` as well as the view, so `render()` — which
// needs the struct to build — works for every context-taking view with no second
// declaration. A misspelt field is a compile error that suggests the right one:
//   error: field designator 'sectoin' does not refer to any field in type
//          'SectionPage_ctx' (aka 'struct SectionCtx'); did you mean 'section'?

// The other half's declaration, and the context type with it. A shared header says
// once what both halves name:
//
//   app.h:  declare_view(StoryPage, StoryCtx);   // view_StoryPage and StoryPage_ctx
//   view.c: view(StoryPage, StoryCtx *c) { … }
//
// The typedef is a redeclaration in the view file, which C allows to the same type.
#define declare_view(N_, ...)                                                  \
  void WC_CAT(view_, N_)(WBuild *, void *);                                    \
  __VA_OPT__(typedef __VA_ARGS__ WC_CAT(N_, _ctx);)
#define ACTION(N_) void WC_CAT(action_, N_)(void *, const char *);

// ---------------------------------------------------------------- actions
//
//   action(toggle, Todo *t) { t->done = !t->done; set(tick, get(tick) + 1); }
//   action(add)             { push(&todos, (Todo){...}); }
//
// An action is an event handler whose context is typed. The `(void *ctx, const
// char *event)` adapter is generated, so an action never takes an argument it does
// not use; `bind("tick", "click", toggle)` in the client file attaches it.
#define action(N_, ...)                                                        \
  WC_CAT(WC_ACTION, __VA_OPT__(1))(__COUNTER__, N_ __VA_OPT__(, __VA_ARGS__))

#define WC_ACTION(C_, N_)                                                      \
  void WC_CAT(action_, N_)(void *, const char *);                              \
  static void WC_CAT(wc_action_body_, N_)(void);                               \
  void WC_CAT(action_, N_)(void *WC_CAT(wc_actx_, C_), const char *WC_CAT(wc_ev_, C_)) { \
    (void)WC_CAT(wc_actx_, C_);                                                \
    (void)WC_CAT(wc_ev_, C_);                                                  \
    WC_CAT(wc_action_body_, N_)();                                             \
  }                                                                            \
  static void WC_CAT(wc_action_body_, N_)(void)

#define WC_ACTION1(C_, N_, ...)                                                \
  void WC_CAT(action_, N_)(void *, const char *);                              \
  static void WC_CAT(wc_action_body_, N_)(__VA_ARGS__);                        \
  void WC_CAT(action_, N_)(void *WC_CAT(wc_actx_, C_), const char *WC_CAT(wc_ev_, C_)) { \
    (void)WC_CAT(wc_ev_, C_);                                                  \
    /* an action that declares an item and gets NULL did nothing at all, silently: the */ \
    /* list it lives in is not keyed, so ev_attr() is how to find the row — say so once */ \
    if (!WC_CAT(wc_actx_, C_))                                                 \
      wc_action_no_item(#N_, (const void *)&WC_CAT(action_, N_));               \
    WC_CAT(wc_action_body_, N_)(WC_CAT(wc_actx_, C_));                         \
  }                                                                            \
  static void WC_CAT(wc_action_body_, N_)(__VA_ARGS__)

// `action_live` — the same handler, declaring that it only writes signals. The bridge then
// sends the live bindings' ops and does not re-render the view, which is what makes a
// pointer move, a keystroke or a stream frame cost one DOM op instead of a whole page.
//
// The promise has teeth: an action that also changes what the view *draws* (a vector, a
// struct field the markup reads, a branch) leaves the page stale, because no render follows
// it. When in doubt, `action` is the safe one: it costs a render and cannot be wrong.
#define action_live(N_, ...)                                                   \
  WC_CAT(WC_ACTION_LIVE, __VA_OPT__(1))(__COUNTER__, N_ __VA_OPT__(, __VA_ARGS__))

#define WC_ACTION_LIVE(C_, N_)                                                 \
  void WC_CAT(action_, N_)(void *, const char *);                              \
  static void WC_CAT(wc_action_body_, N_)(void);                               \
  void WC_CAT(action_, N_)(void *WC_CAT(wc_actx_, C_), const char *WC_CAT(wc_ev_, C_)) { \
    (void)WC_CAT(wc_actx_, C_);                                                \
    (void)WC_CAT(wc_ev_, C_);                                                  \
    wc_live_mark();                                                            \
    WC_CAT(wc_action_body_, N_)();                                             \
  }                                                                            \
  static void WC_CAT(wc_action_body_, N_)(void)

#define WC_ACTION_LIVE1(C_, N_, ...)                                           \
  void WC_CAT(action_, N_)(void *, const char *);                              \
  static void WC_CAT(wc_action_body_, N_)(__VA_ARGS__);                        \
  void WC_CAT(action_, N_)(void *WC_CAT(wc_actx_, C_), const char *WC_CAT(wc_ev_, C_)) { \
    (void)WC_CAT(wc_ev_, C_);                                                  \
    if (!WC_CAT(wc_actx_, C_))                                                 \
      wc_action_no_item(#N_, (const void *)&WC_CAT(action_, N_));              \
    wc_live_mark();                                                            \
    WC_CAT(wc_action_body_, N_)(WC_CAT(wc_actx_, C_));                         \
  }                                                                            \
  static void WC_CAT(wc_action_body_, N_)(__VA_ARGS__)

void wc_show(bool cond); // display:none unless true

// A hook where the control is. `on_click(next_story)` names the element after the action
// and attaches it on click, so the wiring is one line in the view instead of a string in
// client_main that has to match a string in the view:
//
//   view:    el("button") { class_("btn"); on_click(next_story); text("Next"); }
//   client:  action(next_story) { set(at, get(at) + 1); }
//
// The markup always carries `data-hook="next_story"` (a page, a test and the browser can
// all see the wiring); the listener is attached only in the client half, because the
// server's node ids mean nothing in the browser.
//
// The action must be in the same translation unit or declared in a header both halves see.
// A *shared* view is compiled for the server too, where the client's actions do not exist,
// so it wires with `named()` + `bind()` in `client_main` instead (see the fullstack
// scaffold); a client-only app uses this and has no matching line in `client_main` at all.
void wc_on_click(const char *name, WEventFn fn);
#define on_click(fn_) wc_on_click("" #fn_, (WEventFn)WC_CAT(action_, fn_))

// Whether this render is the client half (a mount has happened). A view can ask; almost
// none should.
bool wc_in_client(void);
void wc_client_mode(void);

// ---------------------------------------------------------------- the event's element
//
// Inside a listener or an action, the element the event was delivered to — and the nearest
// value of one of its attributes, walking up the ancestors, because a click lands on a
// <span> inside the <button data-id="7"> the view meant.
//
//   action(pick) { i64 id = ev_attr_i64("data-id"); if (id) set(sel, id); }
//
// This is what a positional list (`for_i`) uses instead of the item an action receives
// inside a keyed one (`for_each`).
WDom *ev_self(void);
// A node by the id the browser knows it by (`__webc` in the page): how a verb that arrives
// from the bridge — a key, a field, a frame — gets back to the element it concerns. Null on
// a server build, where there is no browser and no ids.
WDom *wc_node_by_id(i32 id);
str ev_attr(const char *name);     // (str){0} when nothing at or above the element has it
i64 ev_attr_i64(const char *name); // 0 when absent, empty or not a number
str ev_value(void);                // the payload the bridge sent (input/change)
Text ev_keep(void);                // ev_value(), persisted: safe to store in state

// ---------------------------------------------------------------- where the pointer was
//
// A pointer event carries where it happened, and the bridge sends that with the event, so
// following the pointer is an action and a signal — no script, no getBoundingClientRect in
// a .js file, no second source of truth about what the view drew.
//
//   action(follow) { WPoint p = ev_point(); set(tip_x, p.cx); set(tip_y, p.cy); }
//   el("div") { class_("tip");
//               style("left:%gpx;top:%gpx", get(tip_x) + 14, get(tip_y) - 12); … }
//
// A pointer is a mouse, a pen or a finger: bind "pointermove"/"pointerdown"/"pointerleave"
// and the same code works for all three.
typedef struct WPoint {
  f64 x, y;   // inside the element: 0,0 is its top-left corner
  f64 w, h;   // the element's own box, so a chart can invert its scale
  f64 cx, cy; // on the page (clientX/clientY): what a position: fixed element wants
} WPoint;

// Zeroed when the event carried no pointer data — a keyboard click, a plain field, a test
// that did not say where. Check `p.w || p.h` before using a coordinate as a fraction.
WPoint ev_point(void);
typedef struct WBox { f64 w, h; } WBox;
WBox ev_box(void); // the element's box (the 0,0 corner is the element's own)
void wc_ev_enter(WDom *n, str value);
void wc_ev_leave(void);
void wc_action_no_item(const char *name, const void *id);

// A native test cannot click a button; it can ask for the click by the name the button
// carries. Returns false when nothing is wired to that name (which is itself the thing
// worth asserting).
bool wc_test_dispatch(const char *hook, const char *event);
// Called by the bridge when an event reaches the module (see hooks_report in dom.c):
// a binding that no element carries is reported from the first render after an
// interaction, never before.
void wc_note_interaction(const char *event);

// Keyed lists: reconciliation moves nodes and never re-creates them; the per-item
// scope is disposed when the item leaves (SPEC §7.3).
typedef i64 (*WKeyFn)(void *ctx, i32 index);
typedef void (*WItemFn)(void *ctx, i32 index, WBuild *b);

void wkeyed_begin(void);
// Opens one item's scope and returns **the owner it pushed on the ambient stack** — NULL if it
// opened nothing. `wkeyed_end()` sweeps and pops *that* value; re-reading the top of the stack
// further down is what let a row that left its pushes unbalanced (or an item that returned
// before pushing) hand a released instance to `winst_sweep_children` — heap-use-after-free,
// once per ~36,000 requests under load (W-10, ASan stack in findings/w10-keyed-list-crash.md).
WInst *wkeyed_item(i64 key);
// The element the current keyed block is rendering, so a binding attached by
// `named(...)` inside it receives the item and not the list.
void wkeyed_item_ptr(void *p);
void wkeyed_end(WInst *owner); // `owner` is what wkeyed_item() returned
// The cleanup-carried guard the for_each macros use: it holds the owner so the close is a
// matched pair with the open, whatever the loop body did to the ambient stack.
typedef struct {
  WInst *owner; // what wkeyed_item() pushed, handed back to wkeyed_end()
  int    open;  // a close is owed (set at open, never cleared by the loop)
  int    done;  // the loop's own terminator — NOT the same flag as `open`
} WKeyedGuard;
// `open` and `done` are deliberately separate: the macro's increment clears `done` to end the
// loop, and cleanup runs *after* that, so a single flag read at close time is always 0 and the
// close never happens (measured: the guard fired with live=0 and `wkeyed_end` never ran, which
// left every row's b->cur unrestored and nested one element inside another).
[[maybe_unused]] static inline void wkeyed_guard_close(WKeyedGuard *g) {
  if (g->open) wkeyed_end(g->owner);
}
// Opens one row: the keyed scope, then the element binding — the order the macros have always
// used — and hands back the guard that closes it.
[[maybe_unused]] static inline WKeyedGuard wkeyed_guard_open(i64 key, void *p) {
  WInst *own = wkeyed_item(key);
  wkeyed_item_ptr(p);
  return (WKeyedGuard){.owner = own, .open = 1, .done = 0};
}
void wkeyed_list(i32 count, WKeyFn key_at, WItemFn item, void *ctx);

// ---------------------------------------------------------------- renderers

// SSR: HTML with hydration anchors.
str webc_render(WBuild *b);
str wdom_html(WBuild *b);

// DOM: a command buffer for the JS bridge (runtime/webc.js).
typedef struct WDomBatch {
  Buf b;
  i32 ops;
} WDomBatch;

void wdom_mount_batch(WBuild *b, WDomBatch *out); // full tree for mount()
void wdom_patch_batch(WBuild *b, WDomBatch *out); // diffs against the reuse list

// ------------------------------------------------ tree transplant (dev, hot reload)
// The previous render as text, and back. A hot module swap keeps the document, so the new
// module loads what the page already shows and its first render diffs against it — which
// is what keeps scroll, focus and an open <details> across a save.
void wdom_tree_dump(Buf *out, WDom *root);
i32  wdom_tree_load(str in);                        // -> nodes loaded for the last `view`
i32  wdom_tree_take(i32 view, WDom ***nodes);       // -> that tree, or 0
// Walk the tree this render produced and panic on a cycle, a broken sibling link or
// a child that does not point back at its parent. Called by every render; the
// emitter, the id index and the browser all assume a tree, and this is the one
// place that can say so out loud instead of spinning.
void wc_render_assert(WBuild *b);
i32 wdom_node_count(WBuild *b);

// component(Name) { ... } — the body is the view; `el(...)` uses the current build.
#define component(N_)                                                                              \
  static void N_##_view(WBuild *b_);                                                               \
  [[maybe_unused]] static void N_##_run(WBuild *b_) {                                              \
    N_##_view(b_);                                                                                 \
  }                                                                                                \
  static void N_##_view([[maybe_unused]] WBuild *b_)

WScope *pool_scope(WBuild *b);
void wbuild_set_reuse(WBuild *b, WDom **nodes, i32 n);
// Reserve the ids of `n` live nodes so fresh nodes get unused ones.
void wbuild_reserve_ids(WBuild *b, WDom **nodes, i32 n);
WDom **wbuild_seq(WBuild *b, i32 *n);

// A reusable component instance: remembers the previous node list so the next
// render can reconcile against it and emit only the diffs.
typedef struct WComponent {
  WDom **seq; // previous run's nodes (live, in the node arena)
  i32 nseq;
  // The per-run node list is written every render, so it is double-buffered: with one
  // buffer the list is reallocated in the node arena on every render (128 B per render for
  // a lone element, measured), and with a single buffer the run that writes it would
  // overwrite the run it reads.
  WDom **seqbuf[2];
  i32 seqcap[2];
  Arena *nodes;      // nodes live for the component's lifetime
  Arena *html;       // the last wcomp_html() result: valid until the next one
  bool owns_nodes;   // false when the caller passed its own arena
  Arena *scratch[2]; // double-buffered strings/attrs
  i32 slot;
  WDomBatch cmds; // heap-backed: never reset by the arena swap
  i32 renders;
  i32 id_base; // the first id this component may hand out (0 = 1). Several mounted
               // views share one browser, and one id space: the bridge looks an event
               // up by id alone, so two views numbering from 1 would fight over it.
  WDom *root; // the node tree this component last rendered
  WInst *own; // render-owned state (`state_own`) for this mount: created on the first
              // render that reads one, freed by wcomp_destroy. Every mounted component is
              // an owner, so the same view mounted twice has two independent copies.
} WComponent;

void wcomp_init(WComponent *c, Arena *a);
// Release what the component owns (its node arena unless the caller supplied one,
// both scratch arenas, the command buffer). A component built for one render —
// res_ssr()/res_page() make one per response — must be destroyed, or every
// response leaks ~1.4 KB (measured: 1422 B/op in bench_http the moment the
// benchmark started reporting B/op).
void wcomp_destroy(WComponent *c);
void wcomp_render(WComponent *c, void (*fn)(WBuild *b));
str wcomp_html(WComponent *c, void (*fn)(WBuild *b));
// Views that take their context: `text("%s", user->name)` needs one.
void wcomp_render_ctx(WComponent *c, void (*fn)(WBuild *b, void *ctx), void *ctx);
str wcomp_html_ctx(WComponent *c, void (*fn)(WBuild *b, void *ctx), void *ctx);

// Take a loaded tree as this component's previous render: the next render is a patch.
void wcomp_adopt(WComponent *c, WDom **nodes, i32 n);

// mount(App, "#root") on the wasm target; res_ssr/res_page(App, ctx) on the server.
// Views that need their context use mount_ctx, exactly like wcomp_render_ctx.
void wc_mount(void (*fn)(WBuild *b), const char *selector);
void wc_mount_ctx(void (*fn)(WBuild *b, void *ctx), void *ctx, const char *selector);

// ---------------------------------------------------------------- client tests
//
// Natively, `mount()` has nothing to mount into — there is no document. Rather than
// ignoring it (a client file compiled for a test would link, do nothing, and prove
// nothing), a native build *records* every mount, so `webc test --client` can render
// exactly what the browser half renders and replay it through the real webc.js.
typedef struct WTestMount {
  void (*fn)(WBuild *b, void *ctx);
  void *ctx;
  const char *selector;
} WTestMount;

i32 wc_test_mount_count(void);
const WTestMount *wc_test_mount(i32 i);
void wc_test_mount_clear(void);

// `mount(App, "#root")` — the view named once, mounted into an element.
// mount(App)                 → into #root, no context
// mount(App, "#app")         → into a selector
// mount(App, &state)         → into #root, with a context
// mount(App, &state, "#app") → both
#define mount(V_, ...) WC_CAT(WC_MOUNT, WC_NARG(V_ __VA_OPT__(, __VA_ARGS__)))(V_ __VA_OPT__(, __VA_ARGS__))
#define WC_MOUNT1(V_) wc_mount_ctx(WC_CAT(view_, V_), nullptr, "#root")
// `mount(App, "#app")` is a selector; `mount(App, &card)` is a context. The argument's
// type decides, at compile time, so the two-argument form needs no second name.
[[maybe_unused]] static inline void wc_mount_at(void (*fn)(WBuild *, void *), const char *sel) {
  wc_mount_ctx(fn, nullptr, sel);
}
[[maybe_unused]] static inline void wc_mount_with(void (*fn)(WBuild *, void *), const void *ctx) {
  wc_mount_ctx(fn, (void *)ctx, "#root");
}
#define WC_MOUNT2(V_, A_)                                                      \
  _Generic((A_), char *: wc_mount_at, const char *: wc_mount_at, default: wc_mount_with) \
      (WC_CAT(view_, V_), (A_))
#define WC_MOUNT3(V_, A_, B_) wc_mount_ctx(WC_CAT(view_, V_), (void *)(A_), (B_))

#endif // WEBC_DOM_H