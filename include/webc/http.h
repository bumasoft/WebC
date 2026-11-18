// SPDX-License-Identifier: Apache-2.0
// WebC — HTTP: typed router, request/response, middleware chain, helpers.
//
//   GET("/accounts/:id") {
//     Account *a = store_find(path_i64("id"));
//     if (!a) return res_error(404, "not_found", "no such account");
//     return json(200, a);
//   }
//
// Registration is a file-scope macro (a C function cannot be defined inside a
// function — see docs/reference.md): routes register with a constructor, and
// `webc_serve` starts the kqueue-backed server on the green-thread scheduler.
#ifndef WEBC_HTTP_H
#define WEBC_HTTP_H

#include <webc/core.h>
#include <webc/ctx.h>
#include <webc/json.h>
#include <webc/model.h>
#include <webc/url.h>
#include <webc/page.h>
#include <webc/pp.h>
#include <webc/task.h>

typedef struct Request Request;
typedef struct Response Response;
typedef struct WModel WModel;
typedef struct WNext WNext;
typedef struct WRouter WRouter;
typedef struct WServer WServer;

typedef struct WHeader {
  str name;
  str value;
} WHeader;

typedef struct HeadersCfg {
  const char **img_src;    // null = `'self' data:`
  i32 img_src_n;
  const char **script_src; // null = `'self' <nonce> 'wasm-unsafe-eval'`
  i32 script_src_n;
  const char *extra;       // appended verbatim: "frame-ancestors 'self'; form-action 'self'"
  const char *own;         // with no_default: the policy to emit instead of building one
  bool no_default;         // the app owns the header entirely
} HeadersCfg;

struct Request {
  Arena *arena;
  str method, path, query_string, proto;
  WHeader *headers;
  i32 nheaders;
  str *param_k, *param_v;
  i32 nparams;
  str *query_k, *query_v;
  i32 nquery;
  Bytes body;
  str peer;
  str matched_pattern;
  str request_id;
  str host;
  u64 start_ms;
  bool head_only;
  bool keep_alive;
  bool secure;    // the connection is TLS: a cookie this request sets may be Secure
  bool body_consumed;
  i32 fd;
  WServer *srv;
  void *user;
  void *user2;
  const HeadersCfg *hdr_cfg;  // the policy this request declared (mw_headers), for the dev server
  str csp;        // Content-Security-Policy for this request
  str nonce;      // per-request CSP nonce, for the scripts an SSR page injects
  str body_err;   // the validation message of the last body_try(), if any
  str body_err_field; // ... and which field failed, when the model said so
  u8 body_err_kind;   // ... and its WErrKind: unparseable or invalid
  i32 fail_status; // set by webc_fail(): the status a panicking handler earns
  str fail_code;   // ... and its machine-readable code
  struct WMultipart *multipart; // set when the body was multipart/form-data
  i32 max_parts;
  i32 max_json_depth;                // multipart limits, copied from the server options
  size_t max_part_bytes;
  // The <head> of the page this request is building: what head_title() and friends
  // collected, spliced into the response before it is written. See head_title().
  Buf head;
  str head_title;
  bool head_title_set;
  bool head_applied;
  // Attributes for <html> and <body>, in the order they were added: the theme a
  // reader picked is a server-rendered attribute on <html>, not something the module
  // has to fix up after the first paint. See html_attr().
  Buf doc_html;
  Buf doc_body;
  WCtx *ctx;       // cancellation: done when the client goes away, the timeout
                   // middleware fires, or the server drains
};

struct Response {
  i32 status;
  WHeader *headers;
  i32 nheaders, capheaders;
  Buf body;
  Arena *arena;
  str content_type;
  // streaming
  bool streamed;
  bool sse;
  bool finished;
  void (*stream_fn)(Response *res, void *ctx);
  // Connection takeover: after the head is written the handler owns the socket
  // (websockets, long-lived protocols). Returning closes it.
  void (*upgrade_fn)(Response *res, i32 fd, void *ctx);
  void *upgrade_ctx;
  bool upgraded;
  void *stream_ctx;
  str file_path;
  // The window of `file_path` this response sends. 0/0 means the whole file; `res_file()`
  // resolves both from the request's `Range` header, and the writer sends exactly this and
  // nothing more. A response that carries a range is a 206 and never a compressed one.
  i64 range_off, range_len;
  i64 bytes_written;
  // filled in by the server for streaming responses
  i32 sock;
  bool chunked;
  str extra_headers;
};

typedef Response (*Handler)(Request *req);

typedef struct WMiddleware {
  const char *name;
  bool (*fn)(Request *req, void *ctx, WNext *next);
  void *ctx;
  i32 order;
} WMiddleware;

bool wc_next_run(WNext *nx); // run the rest of the chain (middleware `next`)

struct WNext {
  Request *req;
  Response *res;
  WMiddleware *chain;
  i32 n, i;
  Handler handler;
  bool ran;
  bool aborted;
};

// ---------------------------------------------------------------- registration

// How many middlewares one chain carries: `webc_use()` fills it for the globals, a
// group and a route share the same ceiling, and the request dispatcher runs exactly
// this many. One number, because two numbers is how a phase that is registered and
// never run happens: storing globals up to 32 and running up to 8 drops the extra ones —
// an app's own middleware, added after the seven the framework installs — in silence.
#define WC_MW_MAX 32

// Where `webc build` puts the browser half, and the path the default
// `mw_web_assets()` serves at /app.wasm. One name, so the file that is built is the file
// that is served: two names put a 404 on a file that plainly exists.
#define WC_WASM_STEM ".webc/app"
#define WC_WASM_PATH WC_WASM_STEM ".wasm"

WRouter *webc_router(void);
void webc_router_reset(void);
void webc_route_add(const char *method, const char *pattern, Handler h, WMiddleware *mws, i32 nmws);
void webc_route_add_named(const char *method, const char *pattern, const char *name, Handler h,
                          WMiddleware *mws, i32 nmws);
void webc_use(WMiddleware mw);
void webc_use_reset(void);
i32 webc_route_count(void);
void webc_routes_dump(FILE *out);

// A `use(...)` chain is built from these constructors; each takes its config by
// pointer so the options stay readable at the call site.
WMiddleware mw_logging(void);
WMiddleware mw_request_id(void);
WMiddleware mw_cors(void *cfg);
WMiddleware mw_timeout_ms(i32 ms);
WMiddleware mw_body_limit(size_t bytes);
WMiddleware mw_auth_bearer(const char *token);
WMiddleware mw_recover(void);
WMiddleware mw_headers_default(void);

// The policy `mw_headers_default()` serves, as a value: four parts, one header. Name a CDN in
// `img_src`, a script origin in `script_src`, or append a directive in `extra`; `no_default`
// says the app writes the header itself (`res_header(res, "Content-Security-Policy", …)`) and
// the framework emits nothing. The dev server widens whatever this produces with what live
// reload needs, so a strict policy costs nothing in development.
//
//   webc_use(mw_headers((HeadersCfg){
//       .img_src = (const char *[]){"'self'", "data:", "https://cdn.example.com"},
//       .img_src_n = 3,
//       .extra = "frame-ancestors 'self'"}));
//
// `webc.json` reads the same two lists (`"csp": {"img_src": [...], "script_src": [...]}`), so
// the common case needs no C at all.
WMiddleware mw_headers(HeadersCfg o);
// The policy builder, for a project that emits the header somewhere of its own (a static file
// server, a proxy). `nonce` may be null; `dev` adds what live reload needs.
str wc_csp_build(Arena *a, const HeadersCfg *cfg, const char *nonce, bool dev, i32 ws,
                 const char *dev_connect);
WMiddleware mw_static(const char *url_prefix, const char *dir);
WMiddleware mw_static_spa(const char *url_prefix, const char *dir);
WMiddleware mw_metrics(void);
WMiddleware mw_dev(i32 ws_port); // live reload for `webc dev`
// Live reload plus the CSP origins a client-only app needs to call: `webc dev` serves a
// browser app, and a browser app that cannot reach its API is not a dev loop. Space
// separated CSP source expressions; empty keeps the permissive dev default (any https).
WMiddleware mw_dev_connect(i32 ws_port, str connect_src);
// Serves /webc.js (the browser bridge) and /app.wasm for a wasm build.
WMiddleware mw_web_assets(const char *wasm_path);

// gzip for whatever the client said it accepts. Register it before the middlewares that
// answer (it wraps their response), and it compresses files and rendered bodies alike:
//
//   webc_use(mw_compress());
//   webc_use(mw_static("/public", "public/"));
//
// A file is compressed once and cached by (path, size, mtime); a body is compressed on
// the way out. `Vary: Accept-Encoding` and a `-gz` ETag suffix come with it.
typedef struct CompressOpts {
  i32 level;        // 0 = WC_GZIP_LEVEL (9)
  size_t min_bytes; // 0 = WC_GZIP_MIN (1024): below this, the wrapper costs more
  i32 slots;        // 0 = 32 files cached; the smallest working set of a real app
} CompressOpts;
WMiddleware mw_compress(void);
WMiddleware mw_compress_opts(CompressOpts o);

// The splice of the page's <head> into the response body, installed innermost by the
// framework so it happens while the body is still HTML — before compression, before the dev
// reload script. An app never needs it; it is here because a project that replaces the
// default chain (`webc_serve` with its own middlewares) should keep it:
//
//   webc_use(mw_compress());
//   webc_use(mw_document());      // last, so it wraps the route
//   ...routes...
WMiddleware mw_document(void);

// A 404 that is a page: the file's markup inside the app's own shell, status 404, `text/html`,
// for a client that accepts HTML — and the request's own 404 (an app route that answered with
// HTML) is left alone. `install_default_middleware()` registers this from `webc.json`'s
// `"not_found"` key; with no key nothing changes, and a JSON client still gets a JSON error.
WMiddleware mw_not_found(const char *path);

// File-scope route registration. The body is a function; `req` is available and
// the extractor shorthands (path/query/header) read the bound current request.
//
// A second argument names the route, which is what makes `url_to()` possible:
//
//   GET("/a/:slug", "story") { ... }
//   href(url_to("story", "slug", st->slug))
#define GET(...) WC_ROUTE_SEL_("GET", __COUNTER__, __VA_ARGS__)
#define POST(...) WC_ROUTE_SEL_("POST", __COUNTER__, __VA_ARGS__)
#define PUT(...) WC_ROUTE_SEL_("PUT", __COUNTER__, __VA_ARGS__)
#define PATCH(...) WC_ROUTE_SEL_("PATCH", __COUNTER__, __VA_ARGS__)
#define DELETE(...) WC_ROUTE_SEL_("DELETE", __COUNTER__, __VA_ARGS__)
#define HEAD(...) WC_ROUTE_SEL_("HEAD", __COUNTER__, __VA_ARGS__)
#define OPTIONS(...) WC_ROUTE_SEL_("OPTIONS", __COUNTER__, __VA_ARGS__)
#define ANY(...) WC_ROUTE_SEL_("*", __COUNTER__, __VA_ARGS__)

#define WC_ROUTE_SEL_(m_, C_, ...) WC_CAT(WC_ROUTE_SEL_N, WC_NARG(__VA_ARGS__))(m_, C_, __VA_ARGS__)
#define WC_ROUTE_SEL_N1(m_, C_, p_) WC_ROUTE_(m_, p_, C_)
#define WC_ROUTE_SEL_N2(m_, C_, p_, name_) WC_ROUTE_NAMED_(m_, p_, name_, C_)

#define WC_ROUTE_NAMED_(m_, p_, name_, C_)                                                      \
  static Response WC_CAT(wc_h_, C_)(Request * req);                                              \
  __attribute__((constructor)) static void WC_CAT(wc_reg_, C_)(void) {                           \
    webc_route_add_named(m_, p_, name_, WC_CAT(wc_h_, C_), nullptr, 0);                           \
  }                                                                                              \
  static Response WC_CAT(wc_h_, C_)([[maybe_unused]] Request * req)

#define WC_ROUTE_(m_, p_, C_)                                                                      \
  static Response WC_CAT(wc_h_, C_)(Request * req);                                              \
  __attribute__((constructor)) static void WC_CAT(wc_reg_, C_)(void) {                           \
    webc_route_add(m_, p_, WC_CAT(wc_h_, C_), nullptr, 0);                                         \
  }                                                                                              \
  static Response WC_CAT(wc_h_, C_)([[maybe_unused]] Request * req)

// URL building — `url_to()`, `link_to()`, `url_build()` — lives in <webc/url.h>, which
// both halves can include: a shared view needs links as much as a route does. `GET(…,
// "name")` registers the name there, so the server needs no extra declaration.

// A group: a shared prefix and shared middleware for every route inside it
// (SPEC §6.3). `group("/api/v1") { use(auth_bearer()); GET("/me") { ... } }` — the
// braces are real C, so `use` inside runs between group_open and group_close.
void webc_group_begin(const char *prefix);
void webc_group_mw(WMiddleware mw);
void webc_group_end(void);

// group("/api/v1") { use(auth_bearer()); webc_route_add("GET", "/me", h, nullptr, 0); }
//
// Inside a group, `use()` attaches to the group rather than to the whole app, and
// every route added while the group is open inherits the prefix and those
// middleware. Routes are added with webc_route_add() here, not with the file-scope
// GET()/POST() macros: those register themselves from constructors, which run
// before any block of code — including this one — ever executes.
#define group(prefix_) WC_GROUP_(__COUNTER__, prefix_)
#define WC_GROUP_(C_, prefix_)                                                                     \
  for (int WC_CAT(wc_g_, C_) __attribute__((cleanup(wc_group_cleanup))) =                          \
           (webc_group_begin(prefix_), 1);                                                         \
       WC_CAT(wc_g_, C_); WC_CAT(wc_g_, C_) = 0)
[[maybe_unused]] static inline void wc_group_cleanup(int *flag) {
  (void)flag;
  webc_group_end();
}

// Per-route middleware: GET_MW("/admin", auth_bearer("t")) { ... }
#define GET_MW(p_, ...) WC_ROUTE_MW_("GET", p_, __COUNTER__, __VA_ARGS__)
#define POST_MW(p_, ...) WC_ROUTE_MW_("POST", p_, __COUNTER__, __VA_ARGS__)
#define WC_ROUTE_MW_(m_, p_, C_, ...)                                                              \
  static Response WC_CAT(wc_h_, C_)(Request * req);                                                \
  __attribute__((constructor)) static void WC_CAT(wc_reg_, C_)(void) {                             \
    WMiddleware _wc_mws_[] = {__VA_ARGS__};                                                        \
    webc_route_add(m_, p_, WC_CAT(wc_h_, C_), _wc_mws_,                                            \
                   (i32)(sizeof _wc_mws_ / sizeof _wc_mws_[0]));                                   \
  }                                                                                                \
  static Response WC_CAT(wc_h_, C_)([[maybe_unused]] Request * req)

// ---------------------------------------------------------------- router

WRouter *router_new(Arena *a);
void router_insert(WRouter *r, const char *method, const char *pattern, Handler h, WMiddleware *mws,
                   i32 nmws);
typedef struct WMatch {
  bool found;
  Handler handler;
  WMiddleware *mws;
  i32 nmws;
  str pattern;
  str param_k[16], param_v[16];
  i32 nparams;
  bool method_not_allowed;
} WMatch;
WMatch router_match(WRouter *r, str method, str path);
i32 router_count(WRouter *r);

// ---------------------------------------------------------------- named routes
//
// A route can be named at registration, and then a URL is built from the pattern
// rather than spelled again:
//
//   GET("/a/:slug", "story") { ... }
//   href(url_to("story", "slug", st->slug))     -> /a/the-last-ferry
//
// The name and the pattern live together, so a renamed route is one edit and every
// link follows. A name that is not registered, or a parameter the pattern wants and
// the call did not pass, is a panic that prints the route table instead of a 404 a
// reader finds.
void router_name(WRouter *r, const char *name, const char *method, const char *pattern);
const char *router_pattern_for(WRouter *r, str name);
i32 router_named_count(WRouter *r);
void router_named_dump(WRouter *r, FILE *out);

// ---------------------------------------------------------------- request

str req_header(Request *r, const char *name);
str req_header_i(Request *r, str name);
i64 req_header_i64(Request *r, const char *name, i64 dflt);
str req_param(Request *r, const char *name);
str req_query(Request *r, const char *name);         // the query string, else the form body
str req_body_field(Request *r, const char *name);    // the form body only; null str when absent
str req_query_default(Request *r, const char *name, const char *dflt);
str req_cookie(Request *r, const char *name);
str req_body_str(Request *r);
bool req_wants_json(Request *r);
i64 req_path_i64(const char *name);
i64 req_query_i64(const char *name, i64 dflt);
i32 req_query_i32(const char *name, i32 dflt);
bool req_query_bool(const char *name, bool dflt);

// File-scope handler shorthands: the current request is bound per task.
Request *webc_req_current(void);
WCtx *webc_req_ctx(void); // the current request's cancellation context (or null)
void webc_req_bind(Request *r);
void webc_req_unbind(void);

Text req_param_text(Request *r, const char *name);
// Route parameters and query values are `Text` on the app surface: the one string type an
// app file names. (`path_i64` for the ones that are numbers.)
#define path(name_) req_param_text(webc_req_current(), (name_))
#define query_text(name_) text_of(req_query(webc_req_current(), (name_)))
#define path_i64(name_) req_path_i64((name_))
// `query()` is a str (§2's parser type: a query value can be binary); `query_text()` is
// the same value as the Text an app prints.
#define query(name_) req_query(webc_req_current(), (name_))
#define query_i64(name_, d_) req_query_i64((name_), (d_))
#define query_str(name_) req_query(webc_req_current(), (name_))
#define header(name_) req_header(webc_req_current(), (name_))
#define header_str(name_) req_header(webc_req_current(), (name_))
#define header_i64(name_) req_header_i64(webc_req_current(), (name_), 0)
#define header_i64_or(name_, d_) req_header_i64(webc_req_current(), (name_), (d_))
#define cookie(name_) req_cookie(webc_req_current(), (name_))

// Typed extractors: the model descriptor parses the body straight into the
// struct, so the same declaration serves the server and the frontend form.
WErrRes webc_body_into(void *out, size_t size, const WModel *m, bool as_form);
Response webc_body_error(WErrRes e);
#define TRY_BODY(T_, var_)                                                                         \
  T_ var_;                                                                                         \
  {                                                                                                \
    WErrRes _wc_b_ = webc_body_into(&var_, sizeof var_, T_##_modelof(), false);                    \
    if (!_wc_b_.ok) return webc_body_error(_wc_b_);                                                \
  }
#define TRY_FORM(T_, var_)                                                                         \
  T_ var_;                                                                                         \
  {                                                                                                \
    WErrRes _wc_b_ = webc_body_into(&var_, sizeof var_, T_##_modelof(), true);                     \
    if (!_wc_b_.ok) return webc_body_error(_wc_b_);                                                \
  }
bool wc_body_is_form(void);

// Typed extractors, spelled as in SPEC §6.2. They parse the body with the model
// descriptor and *fail the request* with 400/422 on bad input instead of
// returning an error the caller has to remember to check:
//
//   POST("/accounts/:id/deposit") {
//     Deposit d = body_json(Deposit);
//     return json(202, ack);
//   }
//
// `webc_fail()` is the mechanism: it raises a panic carrying a status, and the
// guards that catch panics build that status instead of a 500.
void webc_fail(i32 status, const char *code, const char *msg);
Response webc_error_from_panic(void); // 4xx if webc_fail() set one, else 500
#define body_json(T_)                                                                              \
  ({                                                                                               \
    T_ _v_ = {0};                                                                                  \
    WErrRes _e_ = webc_body_into(&_v_, sizeof _v_, T_##_modelof(), false);                         \
    if (!_e_.ok) webc_fail(400, "invalid_body", str_cstr(webc_req_current()->arena, werr_msg(&_e_.e)));    \
    _v_;                                                                                           \
  })
#define body_form(T_)                                                                              \
  ({                                                                                               \
    T_ _v_ = {0};                                                                                  \
    WErrRes _e_ = webc_body_into(&_v_, sizeof _v_, T_##_modelof(), true);                          \
    if (!_e_.ok) webc_fail(422, "invalid_form", str_cstr(webc_req_current()->arena, werr_msg(&_e_.e)));   \
    _v_;                                                                                           \
  })

// ---------------------------------------------------------------- response

Response res_empty(i32 status);

// One constructor per body shape. Each takes a `Text`, a literal, a `str` (the parsing
// type) or a number: WC_AS_STR decides, so there is nothing to choose on the call line and
// no second spelling for the same body. (`text()`, `html()` and `json()` below are these
// three, shortened.)
#define res_text(status_, s_) wc_res_text((status_), WC_AS_STR(s_))
#define res_html(status_, s_) wc_res_html((status_), WC_AS_STR(s_))
Response wc_res_text(i32 status, str body);
Response wc_res_html(i32 status, str body);
// One model, or an array of them straight from a Vec<Model>.
Response res_json(i32 status, const WModel *m, const void *obj);
Response res_json_list(i32 status, const WModel *m, const void *first, i64 n, size_t stride);
Response res_error(i32 status, const char *code, const char *msg);
Response res_redirect(i32 status, const char *location);
Response res_not_found(void);
// Server-rendered component → HTML with hydration anchors (SPEC §7.1).
struct WBuild;
// A panicking view never reaches wbuild_end(); the guard that caught the panic
// calls this so the next render on the same task does not start from a build
// pointing into a dead stack. (Declared here because the guards are the callers.)
struct WBuild *wbuild_abandon(void);

// The same render, into the `<div id="root">` of an HTML file on disk: the page the
// browser then hydrates. Use with mw_web_assets() so /webc.js and /app.wasm are served.
// (`page(View, ctx)` is the app-facing macro over this pair; an app never names either.)
Response wc_render_fragment(i32 status, void (*fn)(struct WBuild *, void *), void *ctx);
Response wc_render_page(i32 status, const char *shell_path, void (*fn)(struct WBuild *, void *),
                        void *ctx);

// The generated shell (src/shell.c). A project with a `boot` block in webc.json needs no
// `public/index.html`: `wc_page_auto()` reads the block, and the document — title,
// stylesheet, mount point, `/webc.js` and the `WebC.start()` options — is built to match.
// `wc_shell_auto()` returns the text (or an empty str when there is no boot block), and
// `wc_shell_has_boot()` says whether one was found, so a server-only project falls back to
// bare SSR exactly as before.
str  wc_shell_auto(Arena *a);
str  wc_shell_mount(void); // the id res_page() splices at, from `boot.el`

// With "inline": true in the boot block, the page embeds the document it was rendered
// against instead of sending the module to fetch a fresh one — no tick can land between
// the render and the seed, and one round trip disappears. Implement it in the server
// half, returning the same document the data URL serves; the weak default returns none
// and the fetch stays.
str webc_seed(Arena *a);
bool wc_shell_has_boot(void);
// The `"csp"` block of webc.json as the HeadersCfg it configures, so `install_default_middleware`
// serves the policy the project asked for without the project writing any C.
HeadersCfg wc_shell_csp(void);
// `"not_found": "public/404.html"` from webc.json, or "" — what `mw_not_found()` is registered
// with. A project that wants a real page for a URL nobody routed names the file there.
const char *wc_shell_not_found(void);
void wc_shell_reset(void); // re-read webc.json: tests and `webc dev` both want this
// ---------------------------------------------------------------- the page head
//
// A page's <head> is per page, but res_page() splices the view into a shell file
// whose <head> is fixed: the title of every page was the title of the shell, and a
// canonical URL, an og:image or a meta description could only arrive by hand-
// assembling the whole document. These write into the head of the response being
// built, whatever built it — res_page(), res_html(), or a document you assemble
// yourself, because the splice happens on the way out (webc_apply_head).
//
//   GET("/s/:slug") {
//     Section *s = find_section(path_text("slug"));
//     head_title(fmt("{s} — The Meridian"));
//     head_meta("description", s->blurb);
//     head_link("canonical", fmt("https://example.com/s/{s}"));
//     head_prop("og:title", s->name);
//     return wc_render_page(200, "public/shell.html", SectionView, &ctx);
//   }
//
// A <title> the shell already has is replaced; the rest is appended at the end of
// <head>. Text is escaped on the way in, once.
void webc_head_title(str title);

// Authored HTML as a file: `partials/<name>.html` at the project root, read once, cached for
// the process. The trust is markup()'s (bytes you wrote, no sanitiser) and the content is
// static (no interpolation — a fragment that needs a value is a view). Server-side only: the
// browser half has no filesystem, so a fragment both halves render is `#embed`. A missing
// file is a null str and one warning; `webc check` names the use the compiler cannot.
//
//   head_html(partial("site/head"));               // the chrome every page carries
//   markup(partial("cta/banner"));                 // inside a view
//   return res_html(200, partial("maintenance"));  // a whole page, still a file
str  wc_partial(str name);
#define partial(n_) wc_partial(WC_AS_STR(n_))

void webc_head_meta(str name, str content);
void webc_head_prop(str property, str content); // og:*, twitter:* — <meta property=…>
void webc_head_link(str rel, str href);
void webc_head_html(str markup); // escape hatch: any other tag, verbatim
// Structured data, and the reason this is not just head_html() with a <script> around it: a
// <script> body is JSON *and* markup. `json` is escaped so no value inside it can close the
// element, which is what a block built from a title with `</script>` in it does otherwise.
// Empty `json` writes nothing at all — an empty block is invalid, not neutral.
//
//   head_ld(ld_blog_posting(.headline = p->title, .url = abs_url(path), …));
void webc_head_json_ld(str json);
// A Text, a `str` or a literal at every call site (WC_AS_STR): a page title is a
// `char *` as often as it is a `str`, and neither should need a cast to reach the head.
#define head_title(t_) webc_head_title(WC_AS_STR(t_))
#define head_meta(n_, c_) webc_head_meta(WC_AS_STR(n_), WC_AS_STR(c_))
#define head_prop(n_, c_) webc_head_prop(WC_AS_STR(n_), WC_AS_STR(c_))
#define head_link(r_, h_) webc_head_link(WC_AS_STR(r_), WC_AS_STR(h_))
#define head_html(m_) webc_head_html(WC_AS_STR(m_))
#define head_ld(j_) webc_head_json_ld(WC_AS_STR(j_))
// Splice what those collected into an HTML response. Called for every response the
// server writes; idempotent, and a no-op for a body without </head>.
void webc_apply_head(Response *res);

// ---------------------------------------------------------------- xml
//
// A feed is markup, and every app writes the same one by hand: thirty-five `buf_puts`,
// fifteen lines of escaping, and the same three mistakes (an attribute escaper applied to
// text, a `<pubDate>` that is not RFC 822, a `<guid>` that is not stable). The writer is
// small enough to make the escaping its own job, which is the whole point: one call per
// element, escaped once, on the way in.
//
//   Buf b; xml_init(&b, req->arena);
//   xml_decl(&b);
//   xml_open(&b, "rss", "version=\"2.0\"");
//   xml_open(&b, "channel");
//   xml_el(&b, "title", "The Meridian");
//   for_each(st, (*all)) {
//     xml_open(&b, "item");
//     xml_el(&b, "title", st->title);
//     xml_el(&b, "link", link_to(Story, st));
//     xml_close(&b, "item");
//   }
//   xml_close(&b, "channel", …);
//   return res_xml(200, &b);
void xml_init(Buf *b, Arena *a);
void xml_decl(Buf *b);
void xml_open(Buf *b, const char *tag, const char *attrs); // attrs may be null
void xml_close(Buf *b, const char *tag);
void xml_el(Buf *b, const char *tag, str text);   // <tag>text</tag>, escaped
void xml_esc(Buf *b, str text);                   // just the text, escaped
Response res_xml(i32 status, const Buf *body);

// RFC 822, which is what <pubDate> has always wanted and what an ISO stamp is not:
//   rfc822("2026-10-11T09:40:00Z") -> "Sun, 11 Oct 2026 09:40:00 +0000"
str rfc822(str iso);

// The feed, for a model that has the three fields a feed needs. Field names go in as bare
// identifiers — a C argument cannot be a designator — so the macro stringifies them and the
// model checks them at startup: a typo panics naming the field and listing the real ones,
// instead of shipping an empty <title> that nobody notices.
//
//   return res_feed_rss(200, all, Story, title, dek, published_at,
//                       "The Meridian", "https://themeridian.example/");
Response res_feed_rss_(i32 status, const void *vec, const WModel *m, const char *title_f,
                      const char *summary_f, const char *date_f, const char *feed_title,
                      const char *feed_link, const char *limit);
#define res_feed_rss(status_, vec_, T_, title_f_, summary_f_, date_f_, feed_title_, feed_link_) \
  res_feed_rss_((status_), (const void *)(vec_), T_##_modelof(), #title_f_, #summary_f_, #date_f_, \
                (feed_title_), (feed_link_), nullptr)
#define res_feed_rss_n(status_, vec_, T_, title_f_, summary_f_, date_f_, feed_title_, feed_link_, \
                       limit_)                                                                 \
  res_feed_rss_((status_), (const void *)(vec_), T_##_modelof(), #title_f_, #summary_f_, #date_f_, \
                (feed_title_), (feed_link_), (limit_))

// ---------------------------------------------------------------- a page, as a value
//
// Four calls for a title and five for the rest is a shape you write once and then stop
// reading: seven routes meant 25 `head_*` calls, and nothing said when a new route forgot
// the title. So a page's head is a struct and the reader's settings are another, and both
// arrive through the context:
//
//   GET("/s/:slug") {
//     return render(SectionPage,
//                   .page = head(.title = fmt("%s — The Meridian", sec->name),
//                                .desc = sec->blurb,
//                                .canonical = fmt("https://…/s/%s", sec->slug)),
//                   .section = sec, .list = list);
//   }
//
// The context carries `WPage page;` and the view reads `c->page.env.dark`. Set once, read
// anywhere. The types are in <webc/page.h> because a page's title and the reader's theme
// are the app's own data — a shared header declares the context, and a context that could
// only be declared on the server could not be shared.
// Called by render()/render_with(): fills the environment from the request, sets the
// reader's theme on <html> so the first byte is the right colour, splices the head, and
// says so when a page has no title. Returns ctx, so it reads as a wrapper.
void *webc_page_begin(void *ctx);

// ---------------------------------------------------------------- document attributes
//
// A page's `<html>` and `<body>` are per request too. The one that always matters is
// an attribute on <html>: a site that server-renders the reader's theme needs it in
// the markup before the first byte, or the page flashes the wrong edition. Without
// this the only way to have it was to assemble the whole document by hand.
//
//   html_attr("data-theme", theme);            -> <html lang="en" data-theme="dark">
//   body_attr("class", "is-article");
//   return page(ArticlePage, &ctx);
//
// Same splice as the head: on the way out, for any HTML response, escaped once.
void webc_html_attr(str name, str value);
void webc_body_attr(str name, str value);
#define html_attr(n_, v_) webc_html_attr(WC_AS_STR(n_), WC_AS_STR(v_))
#define body_attr(n_, v_) webc_body_attr(WC_AS_STR(n_), WC_AS_STR(v_))

Response res_file(i32 status, const char *path);
// `res_file()` plus `Content-Disposition: attachment`, so the browser saves it instead of
// rendering it: an export, an invoice, a backup. The name is escaped twice over — an ASCII
// fallback for old clients and `filename*=UTF-8''…` (RFC 6266) for what the fallback cannot
// spell — because a quote or a semicolon reaching a header is response splitting.
Response res_download(i32 status, const char *path, const char *filename);

// ---------------------------------------------------------------- byte ranges
//
// A file response is a *representation*, not a path and a length: the client may ask for
// part of it (`Range: bytes=…`) and the answer is 206 with a `Content-Range`. That is what
// makes a video seekable, a PDF usable in a viewer and a download resumable — the thing
// Go's `http.ServeContent` and Node's `send` give away that a hand-written server has to
// spell out. `res_file()` does all of it against the current request, so an app never calls
// these three itself; they are here because the socket writer and the test client normalise
// a response with the same function, and because a `WRange` is worth testing on its own —
// it is a pure function of a header and a size.
typedef struct WRange {
  i64 off;            // first byte to send
  i64 len;            // 0 = no usable range: the whole representation goes out
  bool unsatisfiable; // a valid range that starts at or past the end of the file → 416
} WRange;

// `bytes=first-last`, `bytes=first-`, `bytes=-suffix` (RFC 9110 §14.1.1). A request for
// more than one range is answered with the whole file, which the RFC allows and which no
// browser asks for.
WRange http_range_parse(str header, i64 size);

// The validators: 304 when the client already has this representation, and the answer is
// written into `res`. `last_modified` may be empty for a representation with no file behind
// it (the browser bridge), and the `-gz` twin of `etag` is answered because `mw_compress()`
// gives the compressed bytes a tag of their own.
bool wc_not_modified(Request *req, Response *res, str etag, str last_modified);

// Resolve a `file_path` response against its request: 304 (validators), 206 (a range), 416
// (a range past the end), or a plain 200. Idempotent, so `res_file()` resolves at
// construction — before `mw_compress()` decides whether to gzip — and the writer resolves
// whatever an app built by hand.
bool wc_file_finalize(Request *req, Response *res);

Response res_stream(i32 status, void (*fn)(Response *, void *), void *ctx);// Write one chunk of a streaming response (chunked transfer-encoding). Inside the
// stream function only: outside a live connection there is nowhere to write.
bool webc_stream_write(Response *r, str bytes);
Response res_upgrade(i32 status, void (*fn)(Response *, i32, void *), void *ctx);
Response res_bytes(i32 status, const char *ctype, const void *p, size_t n);
void res_header(Response *r, const char *name, const char *value);
void res_header_set(Response *r, const char *name, const char *value);  // replace if present
void res_header_str(Response *r, const char *name, str value);
// `attrs` is *extra* attributes ("Max-Age=0", "Secure", "SameSite=Strict"). Path=/,
// HttpOnly and SameSite=Lax are added for you, and an attribute you name yourself is
// not added twice — passing "HttpOnly; SameSite=Lax" twice would put
// `Path=/; HttpOnly; SameSite=Lax; HttpOnly; SameSite=Lax` on the wire.
void res_cookie(Response *r, const char *name, const char *value, const char *attrs);
void res_delete_cookie(Response *r, const char *name);
// Put this request's CSP nonce on every <script>/<style> in an HTML body. The server
// calls it on every response it writes, so a page you assemble yourself gets the
// same treatment as one res_page() splices into a shell (SPEC §11). Idempotent.
void webc_inject_nonce(Response *res, str nonce);

// The one-field form: `body(RowId)` for a row action that carries an id and
// nothing else. A `Todo` would be the obvious choice and the wrong one — a typed
// extractor validates the whole model, so a form holding only `id` would fail the
// `title` rule and answer 422.
WEBC_MODEL(RowId, (id, i64, .pk = true, .required = true))
WEBC_MODEL_INTERNAL(RowId)

// ---------------------------------------------------------------- the app kit
//
// The four calls a handler actually makes. Each one either answers or fails the
// request; none of them returns an error for the handler to re-describe.

// `Todo t = body(Todo);` — a form or a JSON body, chosen by Content-Type and
// validated by the model. An invalid body fails the request with 422 and the
// field message, so no handler ever sees a half-built model.
#define body(T_) WC_BODY_(__COUNTER__, T_)
#define WC_BODY_(C_, T_)                                                                           \
  __extension__({                                                                                  \
    T_ WC_CAT(wc_in_, C_) = {0};                                                                   \
    WErrRes WC_CAT(wc_e_, C_) = webc_body_into(&WC_CAT(wc_in_, C_), sizeof WC_CAT(wc_in_, C_),      \
                                               T_##_modelof(), wc_body_is_form());                 \
    if (!WC_CAT(wc_e_, C_).ok)                                                                     \
      webc_fail(422, "invalid_body",                                                               \
                str_cstr(webc_req_current()->arena, werr_msg(&WC_CAT(wc_e_, C_).e)));                     \
    WC_CAT(wc_in_, C_);                                                                            \
  })

// The same parse, for a handler that answers with a *page*: `body(T)` is right for
// an API (422 and the field message), and wrong for a form, where the answer should
// be the form again with the sentence on it. `body_try` leaves the answer to you.
bool wc_body_try_impl(void *out, size_t size, const WModel *m, bool form);
Text wc_body_error(void);
// The same failure as data: which field, and of what kind. The framework knows both
// (model_validate records the field with the message), so a form does not have to
// pattern-match an English sentence to find out that `email` was required.
Text wc_body_error_field(void);
WErrKind wc_body_error_kind(void);
#define body_try(T_, ptr_)                                                     \
  wc_body_try_impl((ptr_), sizeof *(ptr_), T_##_modelof(), wc_body_is_form())
#define body_error() (wc_body_error())
#define body_error_field() (wc_body_error_field())
#define body_error_kind() (wc_body_error_kind())

// `fail(404, "no todo %lld", (long long)id);` — answer with a status and a
// sentence, from anywhere in the handler.
#define fail(status_, ...) webc_fail((status_), "error", fmt(__VA_ARGS__))

// `return page(Todos, db_all(Todo));` — server-rendered into public/index.html
// when the project has one, into a bare fragment when it does not.
//
// The context goes through `webc_page_begin()` in *every* form, not only in `render()`: it
// is the call that reads the reader's theme cookie onto <html>, fills `c->page.env` and
// applies `head(...)`, and a context that does not begin with a `WPage` is left untouched by
// it (the magic-number check). Without this, `page(Thing, &ctx)` — the form in the README,
// the tutorial and the scaffold — was a page with no title, no description and no theme,
// silently, because the "a page with no title" warning lives inside that call.
Response wc_page_auto(i32 status, void (*fn)(struct WBuild *, void *), void *ctx);
#define page(V_, ...) WC_CAT(WC_PAGE, __VA_OPT__(1))(V_ __VA_OPT__(, __VA_ARGS__))
#define WC_PAGE(V_) wc_page_auto(200, WC_CAT(view_, V_), nullptr)
// The same render with the status the handler means: a form that failed is a 422
// *page*, not a JSON error.
#define page_with(status_, V_, ctx_)                                                               \
  wc_page_auto((status_), WC_CAT(view_, V_), webc_page_begin((void *)(ctx_)))
// The cast is explicit because a view's context is read-only from the framework's
// side: `view(Me, Account *me)` and `view(Signin, Text error)` both pass through the
// same `void *` slot, and neither side ever writes through it.
#define WC_PAGE1(V_, ctx_) wc_page_auto(200, WC_CAT(view_, V_), webc_page_begin((void *)(ctx_)))

// `render(SectionPage, .site = site, .section = sec, .list = list)` — the same page,
// with the context filled in by field name. The view is declared with `view_as(Name,
// CtxType)`, which is what tells this macro which struct the fields belong to, so
// every name is checked by the compiler:
//
//   error: field designator 'sectoin' does not refer to any field in type
//          'SectionPage_ctx' (aka 'struct SectionCtx'); did you mean 'section'?
//
// `page(View, ctx)` stays for the one-value case (`page(Notes, db_all(Note))`).
#define render(V_, ...)                                                       \
  __extension__({                                                             \
    WC_CAT(V_, _ctx) wc_pc_ = {__VA_ARGS__};                                   \
    wc_page_auto(200, WC_CAT(view_, V_), webc_page_begin(&wc_pc_));            \
  })
#define render_with(status_, V_, ...)                                         \
  __extension__({                                                             \
    WC_CAT(V_, _ctx) wc_pc_ = {__VA_ARGS__};                                   \
    wc_page_auto((status_), WC_CAT(view_, V_), webc_page_begin(&wc_pc_));      \
  })

// `Site *site = must(site_load(), "no site");` — a lookup that cannot sensibly be
// absent answers 404 with a sentence instead of rendering a page with a null in it.
// `must_with` when the status is not 404.
#define must(v_, ...)                                                          \
  __extension__({                                                              \
    __auto_type wc_must_v_ = (v_);                                             \
    if (!wc_must_v_) fail(404, __VA_ARGS__);                                    \
    wc_must_v_;                                                                \
  })
#define must_with(status_, v_, ...)                                            \
  __extension__({                                                              \
    __auto_type wc_must_v_ = (v_);                                             \
    if (!wc_must_v_) webc_fail((status_), "error", fmt(__VA_ARGS__));            \
    wc_must_v_;                                                                \
  })

// Handler shorthands. A collection names its own element type (its descriptor is
// inside the struct), so `json(200, &todos)` is enough for a list; one object still names
// its model, because a `Todo*` carries nothing to recover `Todo` from; and bytes that are
// already JSON pass straight through.
#define json(...) WC_CAT(WC_JSON_SEL_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_JSON_SEL_3(status_, T_, ptr_) res_json((status_), T_##_modelof(), (ptr_))
#define WC_JSON_SEL_2(status_, v_) WC_JSON_ONE((status_), (v_))
// `json(200, str)` is bytes that are already JSON — a parse tree rendered elsewhere, or a
// payload from another service. Everything else in the two-argument form is a Vec, which
// carries its own descriptor.
#define WC_JSON_ONE(status_, v_)                                                     \
  _Generic((v_), char *: wc_res_json_bytes, const char *: wc_res_json_bytes,          \
           str: wc_res_json_bytes, default: wc_json_vec)((status_), (v_))
Response wc_res_json_bytes(i32 status, str body);
[[maybe_unused]] static inline Response wc_json_vec(i32 status, const void *vp) {
  const WVecDesc *d = vp ? *(const WVecDesc *const *)vp : nullptr;
  if (!d) return wc_res_json_bytes(status, str_lit("[]"));
  // `&vec` is one `&` too many, and the field it lands on is a pointer whose value happens to
  // be non-null — so the call reads a struct's front as a descriptor and crashes the process
  // inside the writer, with no route in the message. The magic word makes it a 500.
  if (d->magic != WC_VEC_MAGIC)
    webc_fail(500, "json_not_a_vector",
              "json() got something that is not a collection: pass the vector itself "
              "(json(200, &todos)) or use json_list(200, todos), which cannot be written the "
              "wrong way");
  const void *at = *(void *const *)((const char *)vp + d->off_at);
  i64 n = (i64)*(const i32 *)((const char *)vp + d->off_n);
  return res_json_list(status, d->sub, at, n, d->elem_size);
}
// `json_list(200, todos)` — the same array as `json(200, todos)`, spelled so that a wrong
// argument stops the compiler instead of the process. `->desc` and `->at` are member
// accesses on the collection's own type, so `json_list(200, &todos)` is
// "member reference base type 'VecTodo **' is not a structure" — a sentence with a line
// number, in place of the segfault `json()` answered with. Prefer it wherever a collection
// goes out: the failure it prevents is one character, and the cost of that character is a
// dead process with no route in the message.
#define json_list(status_, v_)                                                                    \
  ((v_) && (v_)->desc                                                                              \
       ? res_json_list((status_), (v_)->desc->sub, (v_)->at, (v_)->n, (v_)->desc->elem_size)        \
       : wc_json_list_nodesc((status_), (v_) ? (v_)->n : 0))

// A collection declared as a bare value — `static VecNote g_notes;` — has a null descriptor,
// and every one of those answered `[]` while the list on the page had rows in it. With the
// rows visible, the descriptor is missing, and that is worth a 500 rather than a silent
// empty answer; with no rows, `[]` is the truth.
[[maybe_unused]] static inline Response wc_json_list_nodesc(i32 status, i32 n) {
  if (n <= 0) return wc_res_json_bytes(status, str_lit("[]"));
  webc_fail(500, "json_no_descriptor",
            "this collection has rows but no descriptor: it was declared as a bare value "
            "(`static VecNote notes;`) instead of built with `vec_new(Note)` or "
            "`VecNote_with(arena)`. Reading it as JSON from the type alone is not possible, so "
            "nothing is written and the size on the page is right where the body is empty");
  return wc_res_json_bytes(status, str_lit("[]"));
}

// NOTE: `text(...)` is the markup macro (dom.h). Responses use res_text(), so one
// noun never means two things (docs/reference.md, SPEC §5.1 rule 3).
#define html(status_, s_) wc_res_html((status_), WC_AS_STR(s_))
#define redirect(...) WC_CAT(WC_REDIR_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_REDIR_1(loc_) res_redirect(303, (loc_))
#define WC_REDIR_2(status_, loc_) res_redirect((status_), (loc_))
#define not_found() res_not_found()
#define file(status_, path_) res_file((status_), (path_))
#define stream(status_, fn_, ctx_) res_stream((status_), (fn_), (ctx_))
#define sse(fn_, ctx_) res_sse((fn_), (ctx_))
// The spec spells middleware registration `use(...)`; `webc_use` is the function
// underneath, kept for code that would rather not use a macro.
#define use(mw_) webc_use(mw_)

// ---------------------------------------------------------------- SSE

Response res_sse(void (*fn)(Response *, void *), void *ctx);
bool sse_send_json(Response *r, const char *event, const void *model, const void *obj);

// A stream's frames: two generations that take turns, so a frame can be decoded or
// built in memory that dies the moment the *next* frame replaces it. A response's
// own arena only grows across a stream's life, and "an app never touches arenas"
// needs its one documented exception — this is that exception, spelled as a verb
// (the app-side twin of the browser half's WLive).
typedef struct WFrame {
  Arena *a[2];
  i32 cur; // the generation the last frame_begin() handed out
} WFrame;

void   frame_init(WFrame *f, size_t chunk); // call once before the stream loop
Arena *frame_begin(WFrame *f);              // resets the fresh generation; make it current
void   frame_end(WFrame *f, Arena *prev);   // hand the previous currency back
void   frame_free(WFrame *f);               // both generations, returned
bool sse_send_text(Response *r, const char *event, str data);
void sse_end(Response *r);

// ---------------------------------------------------------------- server

typedef struct WebcOptions {
  const char *addr; // ":8080"
  i32 port;
  // Worker threads. 0 = one per core (capped at 64). A request is a green task,
  // so a handler that waits on the database or a socket parks and gives the core
  // back; the task's arena and request state travel with it if it resumes on
  // another worker.
  i32 cpus;
  i32 tasks; // accepted for API parity (task budget)
  i32 drain_ms;
  size_t arena_per_request;
  size_t max_body;
  i32 max_headers;
  i32 header_timeout_ms;
  i32 idle_timeout_ms;
  i32 max_conns;
  i32 max_multipart_parts;      // §11: a body that claims a thousand parts is refused
  size_t max_multipart_bytes;
  i32 max_json_depth;           // 0 = the default (32)
  bool metrics; // expose /metrics
  bool debug;   // expose /debug
  bool quiet;
  const char *csp; // Content-Security-Policy value
  // TLS is a build feature (§11): set both and a build with -DWEBC_TLS=openssl
  // speaks HTTPS on the same socket; a build without it refuses to start.
  const char *tls_cert;
  const char *tls_key;
  void (*on_start)(void);
  void (*on_stop)(void);
  void (*routes)(void); // runtime registration hook (optional)
} WebcOptions;

i32 webc_serve(WebcOptions *opts);

// One response header by name, "" when the response does not carry it. The counterpart of
// `res_header(res, name, value)`, which is how a response gets one; an app that manages its
// own CSP (or its own cache policy) needs to *read* what is already there before deciding.
str res_header_get(const Response *res, const char *name);

// The framework's own chain: gzip, request id, security headers, logging, panic recovery,
// the body limit, /webc.js + /app.wasm, /public and /assets, and — innermost — the page's
// head spliced into the body while it is still HTML (mw_document). `webc_main()` calls this
// after the app's `webc_boot()`, which is why an app middleware registered there wraps all of
// it; a project that assembles its own chain (a custom `main`) calls it once and adds to it.
void webc_install_default_middleware(void);

// The boot-time answers `webc check` and `webc seed` need, asked of the process that
// has the models: WEBC_SCHEMA_CHECK compares every registered model with its table,
// WEBC_SEED loads content/<table>.json, WEBC_BOOT_ONLY just starts and stops. Returns
// -1 when none of them is set (the normal case: keep going and serve), otherwise the
// exit status. Called by webc_main() right after the app's webc_boot().
int webc_boot_diagnostics(void);
// Compare the models with the tables that exist; prints and returns true when they
// agree. `webc doctor --schema` and `webc check` both land here.
struct WDB;
bool webc_schema_report(struct WDB *db, FILE *out);
// The same comparison as SQL: the additive statements that would bring the tables up to the
// models. `webc migrate diff` runs the app with WEBC_SCHEMA_DIFF and writes what it prints, so
// the migration is generated by the process that has the models, not by a guess about them.
// Returns how many statements it appended; never drops or narrows anything.
struct Buf;
i32 webc_schema_plan(struct WDB *db, struct Buf *plan);
// One registered model as JSON, for `webc scaffold`: the descriptor is the truth a code
// generator needs, and it only exists inside the running app.
bool webc_model_dump(const char *name, FILE *out);
bool webc_schema_diff(struct WDB *db, FILE *out);
// Seed one model from a JSON array of objects. Only when the table is empty, unless
// `reset`. Returns the rows inserted.
i32 seed_from_json(const WModel *m, const char *path);
i32 seed_from_json_reset(const WModel *m, const char *path);
i32 seed_all(const char *dir, bool reset);
void webc_shutdown(void);
bool webc_running(void);
// Test a handler without a socket (SPEC §6.7).
Response webc_test_request(const char *method, const char *path);
// The same, with headers and a body: `headers` is "Name: value\r\n..." with no
// blank line, which is what a test needs for a session cookie or a JSON payload.
Response webc_test_request_ex(const char *method, const char *path, const char *headers,
                              const void *body, size_t body_len);

// ---------------------------------------------------------------- misc

str http_status_text(i32 status);
str http_mime_for(const char *path);
str http_date(Arena *a, u64 unix_ms);
bool http_date_parse(str s, u64 *out_ms);
str http_etag_for(Arena *a, i64 size, u64 mtime_ms);
i32 ws_handshake(Request *req, Response *res); // 0 on success
bool ws_send_frame(i32 fd, u8 opcode, str payload);
i32 webc_global_middleware(WMiddleware *out, i32 max);

#endif // WEBC_HTTP_H