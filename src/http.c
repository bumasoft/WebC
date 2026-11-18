// SPDX-License-Identifier: Apache-2.0
#if !defined(__APPLE__)
#define st_mtimespec st_mtim // same tv_sec, POSIX spelling
#endif
// WebC — request accessors, response helpers, middleware, HTTP utilities.
#define WEBC_NO_SELECT_MACRO 1
#include <webc/dom.h>
#include <webc/http.h>
#include <webc/multipart.h>
#include <webc/log.h>

#include "internal.h"

#include <pthread.h>
#include <stdlib.h>

#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- global state

static WRouter *g_router;
static Arena *g_router_arena;
// Capacity is WC_MW_MAX (http.h) — the same ceiling the dispatcher runs, so nothing
// can be registered into a slot that never runs.
static WMiddleware g_global_mws[WC_MW_MAX];
static i32 g_global_n;

WRouter *webc_router(void) {
  if (!g_router) {
    g_router_arena = arena_new(64 * 1024);
    g_router = router_new(g_router_arena);
  }
  return g_router;
}

void webc_router_reset(void) {
  if (g_router_arena) arena_destroy(g_router_arena);
  g_router_arena = nullptr;
  g_router = nullptr;
  g_global_n = 0;
}

// A group is a prefix plus a middleware list that every route inside it inherits.
// It is a *statement* at runtime rather than a macro block, because the routes in a
// group are registered by constructors that run before main: `group()` opens a
// scope, `ungroup()` closes it, and the macros inside the block see the open group.
// (A block macro cannot work here: `GET(...) { }` bodies register themselves at
// startup, so the grouping has to be a value, not a lexical block.)
typedef struct GroupScope {
  char prefix[512]; // cumulative: a nested group's prefix already contains its parent's
  WMiddleware mws[8];
  i32 nmws;
  struct GroupScope *outer;
} GroupScope;

static GroupScope *g_group;
static Arena *g_group_arena;

void webc_group_begin(const char *prefix) {
  if (!g_group_arena) g_group_arena = arena_new(4096);
  GroupScope *g = arena_alloc(g_group_arena, sizeof *g);
  *g = (GroupScope){.nmws = 0, .outer = g_group};
  if (g_group)
    snprintf(g->prefix, sizeof g->prefix, "%s%s", g_group->prefix, prefix);
  else
    snprintf(g->prefix, sizeof g->prefix, "%s", prefix);
  g_group = g;
}

void webc_group_end(void) {
  if (g_group) g_group = g_group->outer;
}

void webc_group_mw(WMiddleware mw) {
  if (!g_group || g_group->nmws >= (i32)WC_COUNTOF(g_group->mws)) {
    panic("group middleware: no group open (or more than 8 per group)");
  }
  g_group->mws[g_group->nmws++] = mw;
}

void webc_route_add(const char *method, const char *pattern, Handler h, WMiddleware *mws,
                    i32 nmws) {
  webc_route_add_named(method, pattern, nullptr, h, mws, nmws);
}

void webc_route_add_named(const char *method, const char *pattern, const char *name, Handler h,
                          WMiddleware *mws, i32 nmws) {
  WRouter *r = webc_router();
  // The name (and the pattern it stands for) also goes into the shared URL table, so
  // `url_to("story", …)` works in a view, in a shared header, and in the browser half
  // without the app declaring the pattern twice.
  if (name && *name) wc_url_register(name, pattern);
  // A route inside a group carries the group's prefix (nested groups are
  // cumulative) and inherits the middleware of every group around it, outermost
  // first, then the route's own.
  if (g_group) {
    char joined[512];
    if (strcmp(pattern, "/") == 0)
      snprintf(joined, sizeof joined, "%s", g_group->prefix);
    else
      snprintf(joined, sizeof joined, "%s%s", g_group->prefix, pattern);
    pattern = arena_strdup(g_router_arena, joined);
    // outermost -> innermost
    GroupScope *chain[16];
    i32 depth = 0;
    for (GroupScope *g = g_group; g && depth < 16; g = g->outer) chain[depth++] = g;
    i32 total = nmws;
    for (i32 i = 0; i < depth; i++) total += chain[i]->nmws;
    WMiddleware *all = nullptr;
    if (total > 0) {
      all = arena_alloc(g_router_arena, (size_t)total * sizeof(WMiddleware));
      i32 at = 0;
      for (i32 i = depth - 1; i >= 0; i--)
        for (i32 k = 0; k < chain[i]->nmws; k++) all[at++] = chain[i]->mws[k];
      for (i32 i = 0; i < nmws; i++) all[at++] = mws[i];
    }
    mws = all;
    nmws = total;
  }
  WMiddleware *copy = nullptr;
  if (nmws > 0 && mws) {
    copy = arena_alloc(g_router_arena, (size_t)nmws * sizeof(WMiddleware));
    memcpy(copy, mws, (size_t)nmws * sizeof(WMiddleware));
  }
  router_insert(r, method, pattern, h, copy, nmws);
  if (name) router_name(r, name, method, pattern);
}

// ---------------------------------------------------------------- URLs
//
// A URL is built from the pattern that matches it, in <webc/url.h> — shared with the
// browser half, because a mounted view needs links too.


// Fill `pattern`'s :params from (name, value) pairs. `kv` is flat: k, v, k, v …
void webc_use(WMiddleware mw) {
  if (g_group) { // inside a group: the middleware belongs to those routes
    webc_group_mw(mw);
    return;
  }
  if (g_global_n >= (i32)WC_COUNTOF(g_global_mws)) {
    // Reported, not swallowed: a middleware that never runs is a bug you find by
    // reading the whole request path, and the message names the number to raise.
    wc_limit_once(&g_global_mws, "global middleware limit reached: this middleware never runs",
                  "WC_MW_MAX", "middleware");
    return;
  }
  g_global_mws[g_global_n++] = mw;
}

void webc_use_reset(void) {
  g_global_n = 0;
}

i32 webc_global_middleware(WMiddleware *out, i32 max) {
  i32 n = g_global_n < max ? g_global_n : max;
  for (i32 i = 0; i < n; i++)
    out[i] = g_global_mws[i];
  return n;
}

i32 webc_route_count(void) {
  return router_count(webc_router());
}

void webc_routes_dump(FILE *out) {
  fprintf(out, "routes: %d\n", webc_route_count());
  router_named_dump(webc_router(), out);
}

// ---------------------------------------------------------------- current request

// Per task, not per thread: several tasks interleave on one worker thread.
Request *webc_req_current(void) {
  return wtask_local();
}
void webc_req_bind(Request *r) {
  wtask_set_local(r);
}
void webc_req_unbind(void) {
  wtask_set_local(nullptr);
}

// ---------------------------------------------------------------- request accessors

str req_header_i(Request *r, str name) {
  if (!r) return (str){"", 0};
  for (i32 i = 0; i < r->nheaders; i++)
    if (str_eq(r->headers[i].name, name)) return r->headers[i].value;
  return (str){"", 0};
}

str req_header(Request *r, const char *name) {
  if (!r) return (str){"", 0};
  size_t n = strlen(name);
  for (i32 i = 0; i < r->nheaders; i++) {
    if (r->headers[i].name.n != n) continue;
    bool same = true;
    for (size_t k = 0; k < n; k++) {
      char a = r->headers[i].name.p[k], b = name[k];
      if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
      if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
      if (a != b) {
        same = false;
        break;
      }
    }
    if (same) return r->headers[i].value;
  }
  return (str){"", 0};
}

str req_param(Request *r, const char *name) {
  if (!r) return (str){"", 0};
  str n = str_lit(name);
  for (i32 i = 0; i < r->nparams; i++)
    if (str_eq(r->param_k[i], n)) return r->param_v[i];
  return (str){"", 0};
}

Text req_param_text(Request *r, const char *name) { return text_of(req_param(r, name)); }

// One field of an `application/x-www-form-urlencoded` body, decoded, or a null str when this
// request has no such field. This is the other half of `query()`: a form with JavaScript off
// posts its values in the body, and the documented progressive-enhancement control is exactly
// that form — so `query("to")` has to look where the value actually is instead of finding an
// empty query string and returning "".
str req_body_field(Request *r, const char *name) {
  if (!r || !name || !*name || !r->body.p || !r->body.n) return (str){0};
  if (!(str_eq_c(r->method, "POST") || str_eq_c(r->method, "PUT") ||
        str_eq_c(r->method, "PATCH")))
    return (str){0};
  str ct = req_header(r, "content-type");
  // no Content-Type at all is a browser form, and the multipart parser owns the other one
  if (ct.n && !str_has_prefix_c(ct, "application/x-www-form-urlencoded")) return (str){0};
  str body = {r->body.p, r->body.n};
  str raw_k, raw_v;
  for (size_t i = 0; wc_kv_next(body, &i, &raw_k, &raw_v);)
    if (str_eq(url_decode(r->arena, raw_k, true), str_lit(name)))
      return url_decode(r->arena, raw_v, true);
  return (str){0};
}

str req_query(Request *r, const char *name) {
  if (!r) return (str){"", 0};
  str n = str_lit(name);
  for (i32 i = 0; i < r->nquery; i++)
    if (str_eq(r->query_k[i], n)) return r->query_v[i];
  // then the body: a no-JavaScript form is a POST with its fields in the body, and one view
  // has to serve both halves (see post_to()). The query string wins, so a link that carries
  // the same key explicitly still decides.
  str from_body = req_body_field(r, name);
  if (from_body.p) return from_body;
  return (str){"", 0};
}

str req_query_default(Request *r, const char *name, const char *dflt) {
  str v = req_query(r, name);
  return v.p ? v : str_lit(dflt);
}

str req_cookie(Request *r, const char *name) {
  str all = req_header(r, "cookie");
  if (!all.p) return (str){"", 0};
  size_t i = 0;
  while (i < all.n) {
    size_t j = i;
    while (j < all.n && all.p[j] != ';')
      j++;
    str pair = str_trim(str_sub(all, i, j));
    i64 eq = str_index(pair, '=');
    if (eq > 0) {
      str k = str_trim(str_sub(pair, 0, (size_t)eq));
      if (str_eq_c(k, name)) return str_trim(str_sub(pair, (size_t)eq + 1, pair.n));
    }
    i = j + 1;
  }
  return (str){"", 0};
}

str req_body_str(Request *r) {
  if (!r || !r->body.p) return (str){"", 0};
  return (str){r->body.p, r->body.n};
}

bool req_wants_json(Request *r) {
  str a = req_header(r, "accept");
  return str_contains(a, str_lit("application/json"));
}

i64 req_path_i64(const char *name) {
  str v = req_param(webc_req_current(), name);
  i64 out = 0;
  if (!str_to_i64(v, &out)) return 0;
  return out;
}

i64 req_query_i64(const char *name, i64 dflt) {
  str v = req_query(webc_req_current(), name);
  i64 out;
  return str_to_i64(v, &out) ? out : dflt;
}

i32 req_query_i32(const char *name, i32 dflt) {
  i64 v = req_query_i64(name, dflt);
  return (i32)v;
}

bool req_query_bool(const char *name, bool dflt) {
  str v = req_query(webc_req_current(), name);
  if (!v.p) return dflt;
  return str_eq_c(v, "1") || str_eq_c(v, "true") || str_eq_c(v, "yes") || str_eq_c(v, "on");
}

WCtx *webc_req_ctx(void) {
  Request *r = webc_req_current();
  return r ? r->ctx : nullptr;
}

i64 req_header_i64(Request *r, const char *name, i64 dflt) {
  str v = req_header(r, name);
  if (v.n == 0) return dflt;
  char buf[32];
  size_t n = v.n < sizeof buf - 1 ? v.n : sizeof buf - 1;
  memcpy(buf, v.p, n);
  buf[n] = 0;
  char *end = nullptr;
  long long x = strtoll(buf, &end, 10);
  return end == buf ? dflt : (i64)x;
}

// A typed extractor that fails deep inside a handler should answer 4xx, not 500:
// `webc_fail()` records the status on the request (task-local, so concurrent
// requests never see each other's) and then panics, and the guards that catch
// panics call webc_error_from_panic() to build the response.
void webc_fail(i32 status, const char *code, const char *msg) {
  Request *r = webc_req_current();
  if (r && r->fail_status == 0) {
    r->fail_status = status;
    r->fail_code = str_lit(code ? code : "invalid");
  }
  wc_panic(__FILE__, __LINE__, "%s", msg ? msg : "failed");
}

Response webc_error_from_panic(void) {
  Request *r = webc_req_current();
  if (r && r->fail_status) {
    const char *msg = wc_panic_message();
    return res_error(r->fail_status, str_cstr(r->arena, r->fail_code),
                     msg && *msg ? msg : "invalid request");
  }
  return res_error(500, "internal_error", "internal error");
}

bool wc_body_try_impl(void *out, size_t size, const WModel *m, bool form) {
  Request *r = webc_req_current();
  WErrRes e = webc_body_into(out, size, m, form);
  if (r) // the message belongs to the error, which dies here: keep a copy the request owns
    r->body_err = e.ok ? (str){"", 0} : str_dup(r->arena, werr_msg(&e.e));
  return e.ok;
}

Text wc_body_error(void) {
  Request *r = webc_req_current();
  if (!r || !r->body_err.n) return "";
  return str_cstr(r->arena, r->body_err);
}

// Which field failed, and whether the body was unparseable or merely invalid: the
// two facts a form needs to put a sentence next to the input and the right status on
// the page. Both were known when the error was made (model_validate records the
// field name with the message) and thrown away in the flattened sentence.
Text wc_body_error_field(void) {
  Request *r = webc_req_current();
  if (!r || !r->body_err_field.n) return "";
  return str_cstr(r->arena, r->body_err_field);
}

WErrKind wc_body_error_kind(void) {
  Request *r = webc_req_current();
  return r ? (WErrKind)r->body_err_kind : WEBC_EINVAL;
}

// A form post, a multipart upload or a JSON body: the one decision `body(T)` makes.
bool wc_body_is_form(void) {
  Request *r = webc_req_current();
  if (!r) return false;
  str ct = req_header(r, "content-type");
  if (str_has_prefix_c(ct, "application/json")) return false;
  if (str_has_prefix_c(ct, "multipart/form-data")) return true;
  if (str_has_prefix_c(ct, "application/x-www-form-urlencoded")) return true;
  // No Content-Type at all: a browser form is the common case.
  return ct.n == 0;
}

// The page shell a project ships, or null when it has none. Checked once: a file
// that exists at boot does not stop existing mid-request, and a stat per page
// render is a syscall nobody budgets for.
static const char *page_shell(void) {
  static _Atomic int state; // 0 = unknown, 1 = yes, 2 = no
  int s = atomic_load(&state);
  if (s == 0) {
    FILE *f = fopen("public/index.html", "rb");
    if (f) fclose(f);
    s = f ? 1 : 2;
    atomic_store(&state, s);
  }
  return s == 1 ? "public/index.html" : nullptr;
}

Response wc_page_auto(i32 status, void (*fn)(struct WBuild *, void *), void *ctx) {
  const char *shell = page_shell();
  if (shell) return wc_render_page(status, shell, fn, ctx);
  // No file, but a `boot` block: the document is generated, so the project does not ship an
  // HTML file at all. With neither, this is a server-only app and bare SSR is right.
  if (wc_shell_has_boot()) return wc_render_page(status, nullptr, fn, ctx);
  return wc_render_fragment(status, fn, ctx);
}

// ---------------------------------------------------------------- response

static Response res_new(i32 status, Arena *a) {
  Response r = {0};
  r.status = status;
  r.arena = a ? a : wc_arena_get();
  buf_init(&r.body, r.arena, 128);
  return r;
}

WErrRes webc_body_into(void *out, size_t size, const WModel *m, bool as_form) {
  Request *r = webc_req_current();
  if (!r) return wfail(WEBC_EINVAL, str_lit("no current request"));
  r->body_err_field = (str){"", 0};
  r->body_err_kind = 0;
  memset(out, 0, size);
  str body = req_body_str(r);
  WModelParse p = {.ok = true};
  if (body.n > 0) {
    str ctype = req_header(r, "content-type");
    if (as_form && ctype.n > 19 && strncasecmp(ctype.p, "multipart/form-data", 19) == 0) {
      // A browser posting a form with a file input sends multipart even when the
      // handler only wants the text fields (SPEC §11).
      // The parsed parts belong to the *request*, not to this stack frame: `r->multipart` is
      // what `upload_take()` reads, long after this function returns, so a pointer to a local
      // either reads a dead frame or — once the arena has been reset — answers "the form was
      // not sent as a file upload" for a perfectly good upload.
      struct WMultipart *mp = arena_zalloc(r->arena, sizeof *mp);
      i32 max_parts = r->max_parts > 0 ? r->max_parts : 32;
      size_t max_bytes = r->max_part_bytes ? r->max_part_bytes : 4u << 20;
      WErrRes e = multipart_parse(r->arena, ctype, r->body, max_parts, max_bytes, mp);
      if (!e.ok) return e;
      p = model_from_multipart(r->arena, m, mp, out);
      r->multipart = mp;
    } else {
      p = as_form ? model_from_form(r->arena, m, body, out)
                  : model_from_json_depth(r->arena, m, body, out, r->max_json_depth);
    }
  }
  if (!p.ok) {
    r->body_err_kind = (u8)p.e.kind;
    return (WErrRes){.ok = false, .e = p.e};
  }
  // the same validators the frontend runs (§7.4): one implementation, both sides
  WErrs errs = {0};
  if (!model_validate(m, out, &errs) && errs.n > 0) {
    str msg = str_fmt(r->arena, "%.*s: %.*s", (int)errs.items[0].field.n, errs.items[0].field.p,
                      (int)errs.items[0].msg.n, errs.items[0].msg.p);
    r->body_err_field = str_dup(r->arena, errs.items[0].field);
    r->body_err_kind = (u8)WEBC_EVALIDATION;
    return wfail(WEBC_EVALIDATION, msg);
  }
  return wok();
}

// A body the model refused becomes a JSON error: 422 when a field failed its
// validators, 400 when the JSON itself was wrong.
Response webc_body_error(WErrRes e) {
  i32 status = e.e.kind == WEBC_EVALIDATION ? 422 : 400;
  Response r = res_new(status, nullptr);
  r.content_type = str_lit("application/json; charset=utf-8");
  JsonWriter w;
  jw_init(&w, r.arena, 64);
  jw_obj_begin(&w);
  jw_key(&w, "error");
  jw_cstr(&w, status == 422 ? "invalid" : "bad_request");
  jw_key(&w, "message");
  jw_cstr(&w, str_cstr(r.arena, werr_msg(&e.e)));
  jw_key(&w, "status");
  jw_i64(&w, status);
  jw_obj_end(&w);
  buf_putstr(&r.body, jw_finish(&w));
  return r;
}

Response res_empty(i32 status) {
  return res_new(status, nullptr);
}

Response wc_res_text(i32 status, str body) {
  Response r = res_new(status, nullptr);
  r.content_type = str_lit("text/plain; charset=utf-8");
  buf_putstr(&r.body, body);
  return r;
}

Response wc_res_html(i32 status, str body) {
  Response r = res_new(status, nullptr);
  r.content_type = str_lit("text/html; charset=utf-8");
  buf_putstr(&r.body, body);
  return r;
}

Response wc_res_json_bytes(i32 status, str body) {
  Response r = res_new(status, nullptr);
  r.content_type = str_lit("application/json; charset=utf-8");
  buf_putstr(&r.body, body);
  return r;
}

Response res_json_list(i32 status, const WModel *m, const void *first, i64 n, size_t stride) {
  Response r = res_new(status, nullptr);
  r.content_type = str_lit("application/json; charset=utf-8");
  JsonWriter w;
  jw_init(&w, r.arena, 256);
  jw_arr_begin(&w);
  for (i64 i = 0; i < n; i++)
    model_json_write(&w, m, (const char *)first + (size_t)i * stride);
  jw_arr_end(&w);
  str body = jw_finish(&w);
  buf_putstr(&r.body, body);
  return r;
}

Response res_json(i32 status, const WModel *m, const void *obj) {
  Response r = res_new(status, nullptr);
  r.content_type = str_lit("application/json; charset=utf-8");
  JsonWriter w;
  jw_init(&w, r.arena, 128);
  model_json_write(&w, m, obj);
  buf_putstr(&r.body, jw_finish(&w));
  return r;
}

Response res_error(i32 status, const char *code, const char *msg) {
  Response r = res_new(status, nullptr);
  r.content_type = str_lit("application/json; charset=utf-8");
  JsonWriter w;
  jw_init(&w, r.arena, 64);
  jw_obj_begin(&w);
  jw_key(&w, "error");
  jw_cstr(&w, code ? code : "error");
  jw_key(&w, "message");
  jw_cstr(&w, msg ? msg : http_status_text(status).p);
  jw_key(&w, "status");
  jw_i64(&w, status);
  jw_obj_end(&w);
  buf_putstr(&r.body, jw_finish(&w));
  return r;
}

Response res_redirect(i32 status, const char *location) {
  Response r = res_new(status, nullptr);
  res_header(&r, "Location", location);
  return r;
}

Response res_not_found(void) {
  return res_error(404, "not_found", "no such route");
}

Response wc_render_fragment(i32 status, void (*fn)(struct WBuild *, void *), void *ctx) {
  Response r = res_new(status, nullptr);
  r.content_type = str_lit("text/html; charset=utf-8");
  WComponent comp;
  wcomp_init(&comp, nullptr);
  str html = wcomp_html_ctx(&comp, fn, ctx);
  buf_putstr(&r.body, html);
  wcomp_destroy(&comp);
  return r;
}

// Every <script>/<style> a page carries gets this request's nonce (SPEC §11). The
// shell's bootstrap script is inline — without a nonce the default CSP
// ("script-src 'self'") blocks it, and the page silently never hydrates. Tags that
// already say nonce= are left alone, so running this twice is free.
//
// It lives here, on the response, rather than inside res_page(): a page built by hand (a
// custom `document()`, res_html_s, anything that writes its own <html>) hits the same CSP as
// the shell, so a nonce injected by res_page() alone leaves that page with a nonce in the
// policy and none in the tag — the script blocked, the reason invisible. write_response()
// calls this for every HTML response, whatever built it.
static void inject_nonce(Buf *b, str nonce) {
  if (!b->p || nonce.n == 0) return;
  // The rewritten body REPLACES the one it was handed, so it must be owned by the
  // same arena as that body — otherwise every HTML response leaks a page-sized
  // heap block (measured: 16,400 B per home page, 32,784 B per article).
  Buf out;
  buf_init(&out, b->a, b->n + 64);
  size_t i = 0;
  while (i < b->n) {
    if (b->p[i] == '<' && i + 7 <= b->n &&
        (memcmp(b->p + i, "<script", 7) == 0 || memcmp(b->p + i, "<style", 6) == 0)) {
      size_t tag_end = i;
      while (tag_end < b->n && b->p[tag_end] != '>') tag_end++;
      str tag = (str){b->p + i, tag_end - i};
      // str_index returns i64 (-1 when absent); keep the signed type until it is
      // clamped, or -1 becomes SIZE_MAX and the copy overflows.
      i64 ati = str_index(tag, '>');
      size_t at = ati < 0 ? tag.n : (size_t)ati;
      buf_write(&out, b->p + i, at);
      if (str_index(tag, 'n') < 0 || !str_contains(tag, str_lit("nonce="))) {
        buf_putstr(&out, str_lit(" nonce=\""));
        buf_putstr(&out, nonce);
        buf_putstr(&out, str_lit("\""));
      }
      i += (size_t)at;
      continue;
    }
    buf_write(&out, b->p + i, 1);
    i++;
  }
  Buf prev = *b;
  *b = out;
  if (prev.p && !prev.a) wm_free(prev.p);
}

// ---------------------------------------------------------------- the page head

static Buf *head_slot(void) {
  Request *req = webc_req_current();
  if (!req) return nullptr;
  if (!req->head.p) buf_init(&req->head, req->arena, 128);
  return &req->head;
}

static void head_attr(Buf *b, Arena *a, const char *name, str value) {
  str esc = html_escape(a, value);
  buf_printf(b, " %s=\"", name);
  buf_putstr(b, esc);
  buf_putc(b, '"');
}

void webc_head_title(str title) {
  Request *req = webc_req_current();
  if (!req) return;
  req->head_title = str_dup(req->arena, title);
  req->head_title_set = true;
}

void webc_head_meta(str name, str content) {
  Buf *b = head_slot();
  if (!b) return;
  Arena *a = b->a;
  buf_puts(b, "<meta");
  head_attr(b, a, "name", name);
  head_attr(b, a, "content", content);
  buf_puts(b, ">\n");
}

void webc_head_prop(str property, str content) {
  Buf *b = head_slot();
  if (!b) return;
  Arena *a = b->a;
  buf_puts(b, "<meta");
  head_attr(b, a, "property", property);
  head_attr(b, a, "content", content);
  buf_puts(b, ">\n");
}

void webc_head_link(str rel, str href) {
  Buf *b = head_slot();
  if (!b) return;
  Arena *a = b->a;
  buf_puts(b, "<link");
  head_attr(b, a, "rel", rel);
  head_attr(b, a, "href", href);
  buf_puts(b, ">\n");
}

void webc_head_html(str markup) {
  Buf *b = head_slot();
  if (!b) return;
  buf_putstr(b, markup);
  buf_putc(b, '\n');
}

// A <script> body is JSON and markup at the same time, so it gets its own door: the caller
// hands over a document, and the one escaping that makes it safe in HTML happens here rather
// than in every app that ever emitted structured data.
void webc_head_json_ld(str json) {
  Buf *b = head_slot();
  if (!b || !json.p || json.n == 0) return;
  Request *req = webc_req_current();
  buf_puts(b, "<script type=\"application/ld+json\">");
  buf_putstr(b, json_for_script(req->arena, json));
  buf_puts(b, "</script>\n");
}

// ---------------------------------------------------------------- partials
//
// Authored HTML as files. `markup()` is for bytes typed into the source; a whole head's
// chrome — favicons, theme colours, a preloaded font, a stylesheet bundle — is dozens of
// lines of it, and in a C string every one of those lines is an escape and a line-continuation.
// `partials/<name>.html` at the project root is the same bytes with neither: a file the
// server includes by name, read once and cached for the process.
//
//   head_html(partial("site/head"));        // the chrome every page carries
//   markup(partial("cta/banner"));          // inside a view, like markup() itself
//   return res_html(200, partial("maintenance"));   // a whole page, still a file
//
// A partial is *authored* — the trust of `markup()`, no sanitiser — and *static*: no
// interpolation, because a fragment that needs a value is what a view is. And it is
// server-side: the browser half ships bytes, not a filesystem, so a fragment both halves
// render is `#embed` beside the code that renders it. `webc check` verifies every name has
// its file, which is the one thing the compiler cannot.

#define WC_PARTIALS_MAX 64 // names, not bytes; a project's head chrome is not unbounded

typedef struct Partial {
  const char *name; // heap, process lifetime — the cache is never evicted
  str body;
  bool tried; // a miss is cached too: one warning, not one per page render
} Partial;

static Partial g_partials[WC_PARTIALS_MAX];
static i32 g_partials_n;
static pthread_mutex_t g_partials_mu = PTHREAD_MUTEX_INITIALIZER;

// A name is a path under partials/, and it is spelled by a string in app code, so it gets the
// judgement a path built from a string always gets: no `..`, no absolute path, no climbing.
static bool partial_name_ok(str name) {
  if (!name.p || name.n == 0 || name.n > 128) return false;
  if (name.p[0] == '.' || name.p[0] == '/') return false;
  for (size_t i = 0; i < name.n; i++) {
    char c = name.p[i];
    // no '.' at all: the extension is the loader's (".html"), so a dot can only be
    // climbing (`..`) or a doubled extension someone will debug for an hour
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '/';
    if (!ok) return false;
  }
  return true;
}

str wc_partial(str name) {
  if (!partial_name_ok(name)) {
    char pretty[144];
    snprintf(pretty, sizeof pretty, "%.*s", (int)(name.n < 128 ? name.n : 128), name.p);
    log_warn("partial: name is not a name under partials/ — refused", {"name", str_lit(pretty)});
    return (str){nullptr, 0};
  }
  pthread_mutex_lock(&g_partials_mu);
  for (i32 i = 0; i < g_partials_n; i++)
    if (strlen(g_partials[i].name) == name.n &&
        memcmp(g_partials[i].name, name.p, name.n) == 0) {
      str body = g_partials[i].body;
      pthread_mutex_unlock(&g_partials_mu);
      return body;
    }
  char path[256];
  snprintf(path, sizeof path, "partials/%.*s.html", (int)name.n, name.p);
  str body = {nullptr, 0};
  FILE *f = fopen(path, "rb");
  if (f) {
    if (fseek(f, 0, SEEK_END) == 0) {
      long n = ftell(f);
      if (n >= 0) {
        rewind(f);
        char *buf = malloc((size_t)n + 1);
        if (buf) {
          size_t got = fread(buf, 1, (size_t)n, f);
          buf[got] = 0;
          body = (str){buf, got};
        }
      }
    }
    fclose(f);
  }
  if (!body.p) {
    char pretty[144];
    snprintf(pretty, sizeof pretty, "%.*s", (int)(name.n < 128 ? name.n : 128), name.p);
    log_warn("partial: no file partials/<name>.html — it renders as nothing",
             {"name", str_lit(pretty)});
  }
  if (g_partials_n < WC_PARTIALS_MAX) {
    char *heap = malloc((size_t)name.n + 1);
    if (heap) {
      memcpy(heap, name.p, name.n);
      heap[name.n] = 0;
      g_partials[g_partials_n++] = (Partial){.name = heap, .body = body, .tried = true};
    }
  }
  pthread_mutex_unlock(&g_partials_mu);
  return body;
}

static size_t find_at(str hay, const char *needle, size_t from) {
  size_t nl = strlen(needle);
  if (!hay.p || hay.n < nl) return SIZE_MAX;
  for (size_t i = from; i + nl <= hay.n; i++)
    if (memcmp(hay.p + i, needle, nl) == 0) return i;
  return SIZE_MAX;
}

// Put `attrs` just before the `>` of the first `<tag …` in the body. Returns true
// when the tag was found and the body replaced.
static bool splice_into_tag(Arena *a, Buf *body, const char *tag, Buf *attrs) {
  if (!attrs || !attrs->p || attrs->n == 0 || !body->p) return false;
  size_t tlen = strlen(tag);
  size_t at = SIZE_MAX;
  for (size_t i = 0; i + tlen < body->n; i++) {
    if (body->p[i] != '<') continue;
    if (memcmp(body->p + i + 1, tag, tlen) != 0) continue;
    char next = body->p[i + 1 + tlen];
    if (next == ' ' || next == '>' || next == '\t' || next == '\n' || next == '\r' || next == '/')
      at = i;
    if (at != SIZE_MAX) break;
  }
  if (at == SIZE_MAX) return false;
  size_t gt = at;
  while (gt < body->n && body->p[gt] != '>') gt++;
  if (gt >= body->n) return false;
  Buf out;
  buf_init(&out, a, body->n + attrs->n + 8);
  buf_write(&out, body->p, gt);
  buf_putstr(&out, (str){attrs->p, attrs->n});
  buf_write(&out, body->p + gt, body->n - gt);
  *body = out;
  return true;
}

// ---------------------------------------------------------------- xml

void xml_init(Buf *b, Arena *a) { buf_init(b, a, 1024); }

void xml_decl(Buf *b) { buf_puts(b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"); }

void xml_esc(Buf *b, str text) {
  if (!text.p) return;
  for (size_t i = 0; i < text.n; i++) {
    switch (text.p[i]) {
    case '&': buf_puts(b, "&amp;"); break;
    case '<': buf_puts(b, "&lt;"); break;
    case '>': buf_puts(b, "&gt;"); break;
    case '"': buf_puts(b, "&quot;"); break;
    case '\'': buf_puts(b, "&apos;"); break;
    default: buf_putc(b, text.p[i]);
    }
  }
}

void xml_open(Buf *b, const char *tag, const char *attrs) {
  buf_putc(b, '<');
  buf_puts(b, tag);
  if (attrs && *attrs) {
    buf_putc(b, ' ');
    buf_puts(b, attrs); // written by the app, as markup, like an href in a view
  }
  buf_putc(b, '>');
}

void xml_close(Buf *b, const char *tag) {
  buf_puts(b, "</");
  buf_puts(b, tag);
  buf_putc(b, '>');
}

void xml_el(Buf *b, const char *tag, str text) {
  if (!text.p) return;
  xml_open(b, tag, nullptr);
  xml_esc(b, text);
  xml_close(b, tag);
}

Response res_xml(i32 status, const Buf *body) {
  return res_bytes(status, "application/xml; charset=utf-8", body ? body->p : "", body ? body->n : 0);
}

// <pubDate> wants RFC 822 and an ISO stamp is not one: readers showed an empty date, or
// the whole feed failed a validator. One call, in the same file as the date formatter, so
// the feed and the page cannot disagree about what day it is.
str rfc822(str iso) { return wc_fmt_date_iso(iso, "%a, %d %b %Y %H:%M:%S +0000"); }

// ---------------------------------------------------------------- a feed for a model

static const WField *field_named(const WModel *m, const char *name, const char *what) {
  for (i32 i = 0; m && i < m->n; i++)
    if (strcmp(m->fields[i].name, name) == 0) return &m->fields[i];
  Buf b;
  buf_init(&b, wc_arena_get(), 256);
  buf_printf(&b, "res_feed_rss: %s has no field \"%s\" (the %s of an item). Its fields are:",
             m ? m->name : "the model", name, what);
  for (i32 i = 0; m && i < m->n; i++) buf_printf(&b, "%s%s", i ? ", " : " ", m->fields[i].name);
  wc_panic(__FILE__, __LINE__, "%s", buf_cstr(&b));
  return nullptr;
}

Response res_feed_rss_(i32 status, const void *vec, const WModel *m, const char *title_f,
                       const char *summary_f, const char *date_f, const char *feed_title,
                       const char *feed_link, const char *limit) {
  const WField *ft = field_named(m, title_f, "title");
  const WField *fs = field_named(m, summary_f, "summary");
  const WField *fd = field_named(m, date_f, "date");
  // The key the model's own route asks for: `/a/:slug` makes the item's field `slug`, and a
  // feed whose links disagree with its pages is the bug this avoids.
  const WField *key = nullptr;
  const char *route = m ? m->route : nullptr;
  const char *colon = route ? strchr(route, ':') : nullptr;
  if (colon) {
    char name[64];
    size_t k = 0;
    for (const char *p = colon + 1; *p && *p != '/' && k + 1 < sizeof name; p++) name[k++] = *p;
    name[k] = 0;
    key = field_named(m, name, "url key");
  }
  if (!key)
    for (i32 i = 0; m && i < m->n; i++)
      if (m->fields[i].q.pk) key = &m->fields[i];

  // Every collection begins with its descriptor, so a feed can be written for any model
  // without the caller naming the vector type twice.
  const WVecDesc *vd = vec ? *(const WVecDesc *const *)vec : nullptr;
  i32 n = 0;
  // elements are stored by value, `elem_size` apart — a collection is a flat array, not an
  // array of pointers (which is what makes it one allocation per list).
  const char *elems = nullptr;
  if (vd) {
    n = *(const i32 *)((const char *)vec + vd->off_n);
    elems = *(const char *const *)((const char *)vec + vd->off_at);
  }
  i64 max = limit ? strtoll(limit, nullptr, 10) : (i64)n;

  Arena *a = wc_arena_get();
  Buf b;
  xml_init(&b, a);
  xml_decl(&b);
  xml_open(&b, "rss", "version=\"2.0\"");
  xml_open(&b, "channel", nullptr);
  xml_el(&b, "title", str_lit(feed_title));
  xml_el(&b, "link", str_lit(feed_link));
  if (m && route && key) {
    for (i32 i = 0; i < n && i < max; i++) {
      const char *elem = elems + (size_t)i * vd->elem_size;
      str url = wc_url_from(a, route,
                            (str[]){str_lit(key->name), wfield_get_str(key, elem)}, 1,
                            "res_feed_rss");
      xml_open(&b, "item", nullptr);
      xml_el(&b, "title", wfield_get_str(ft, elem));
      xml_el(&b, "link", url);
      xml_el(&b, "guid", url);
      xml_el(&b, "description", wfield_get_str(fs, elem));
      xml_el(&b, "pubDate", rfc822(wfield_get_str(fd, elem)));
      xml_close(&b, "item");
    }
  }
  xml_close(&b, "channel");
  xml_close(&b, "rss");
  buf_putc(&b, '\n');
  return res_xml(status, &b);
}

// ---------------------------------------------------------------- a page, as a value

// Once per render of a page, before the view runs: what this request knows about the
// reader, the reader's theme on <html>, and the head the route declared. The order of the
// tags here is the order they appear in the document, which is why it is written down
// once instead of at seven call sites.
void *webc_page_begin(void *ctx) {
  if (!ctx) return ctx;
  WPage *p = ctx;
  if (p->tag != WC_PAGE_MAGIC) return ctx; // a context that predates the page, or a partial
  Request *req = webc_req_current();
  PageEnv env = {0};
  if (req) {
    str theme = req_cookie(req, "theme");
    if (str_eq_c(theme, "dark") || str_eq_c(theme, "light"))
      env.theme = str_cstr(req->arena, theme);
    else
      env.theme = "";
    env.path = req->path.p ? str_cstr(req->arena, req->path) : "";
    str q = req_query(req, "q");
    env.query = q.p ? str_cstr(req->arena, q) : "";
    env.lang = "en";
  }
  env.dark = env.theme && strcmp(env.theme, "dark") == 0;
  if (p->env.theme == nullptr || !*p->env.theme) p->env = env; // a route may override
  if (env.theme && *env.theme) webc_html_attr(str_lit("data-theme"), str_lit(env.theme));

  const Head *h = &p->head;
  if (h->title && *h->title) webc_head_title(str_lit(h->title));
  if (h->desc && *h->desc) webc_head_meta(str_lit("description"), str_lit(h->desc));
  const char *og_title = h->og_title ? h->og_title : h->title;
  if (og_title && *og_title) webc_head_prop(str_lit("og:title"), str_lit(og_title));
  if (h->og_type && *h->og_type) webc_head_prop(str_lit("og:type"), str_lit(h->og_type));
  if (h->og_image && *h->og_image) webc_head_prop(str_lit("og:image"), str_lit(h->og_image));
  if (h->og_site && *h->og_site) webc_head_prop(str_lit("og:site_name"), str_lit(h->og_site));
  if (h->section_name && *h->section_name)
    webc_head_meta(str_lit("article:section"), str_lit(h->section_name));
  if (h->canonical && *h->canonical) webc_head_link(str_lit("canonical"), str_lit(h->canonical));
  if (h->alternate && *h->alternate) webc_head_link(str_lit("alternate"), str_lit(h->alternate));

  // A page with no title is a document that is wrong in the tab, and silently wrong: the
  // shell's generic title just stays. Said once per route *per worker*, so a loop of 404s
  // does not become a wall of text and two workers rendering at once cannot race on the list
  // (`_Thread_local`: every other mutable static in src/ is `_Atomic` or thread-local — this
  // one, in the diagnostic a broken app needs most, was neither).
  if (!h->title || !*h->title) {
    static _Thread_local char warned[16][64];
    static _Thread_local i32 nwarned;
    const char *where = req && req->matched_pattern.p ? req->matched_pattern.p : "a page";
    bool seen = false;
    for (i32 i = 0; i < nwarned; i++)
      if (strcmp(warned[i], where) == 0) seen = true;
    if (!seen && nwarned < 16) {
      snprintf(warned[nwarned++], sizeof warned[0], "%s", where);
      log_warn("a page rendered without a title: set .page = head(.title = …)",
               {"route", str_lit(where)});
    }
  }
  return ctx;
}

void webc_html_attr(str name, str value) {
  Request *req = webc_req_current();
  if (!req || name.n == 0) return;
  if (!req->doc_html.p) buf_init(&req->doc_html, req->arena, 64);
  head_attr(&req->doc_html, req->arena, str_cstr(req->arena, name), value);
}

void webc_body_attr(str name, str value) {
  Request *req = webc_req_current();
  if (!req || name.n == 0) return;
  if (!req->doc_body.p) buf_init(&req->doc_body, req->arena, 64);
  head_attr(&req->doc_body, req->arena, str_cstr(req->arena, name), value);
}

void webc_apply_head(Response *res) {
  Request *req = webc_req_current();
  if (!res || !req || req->head_applied) return;
  req->head_applied = true;
  bool have_doc = req->doc_html.n > 0 || req->doc_body.n > 0;
  if (!req->head_title_set && req->head.n == 0 && !have_doc) return;
  if (res->streamed || res->file_path.p || res->upgraded || res->body.n == 0) return;
  // Only a real document: a <head> is what makes a title meaningful, and a JSON
  // body must not be re-encoded on its way out.
  if (!res->content_type.p || !str_contains(res->content_type, str_lit("html"))) return;
  Arena *a = res->arena;
  // <html> and <body> attributes first: they are found by tag name, and the head
  // splice below works on offsets into the body it is given.
  if (have_doc) {
    Buf staged = res->body;
    splice_into_tag(a, &staged, "html", &req->doc_html);
    splice_into_tag(a, &staged, "body", &req->doc_body);
    if (res->body.p && !res->body.a) wm_free(res->body.p);
    res->body = staged;
  }
  if (!req->head_title_set && req->head.n == 0) return;
  str body = (str){res->body.p, res->body.n};
  i64 close_head = -1;
  if (body.n >= 7) {
    for (size_t i = body.n - 7 + 1; i > 0; i--) {
      if (memcmp(body.p + (i - 1), "</head>", 7) == 0) {
        close_head = (i64)(i - 1);
        break;
      }
    }
  }
  if (close_head < 0) return; // no <head>: nothing to say

  str title = {nullptr, 0};     // the whole element, for a shell with no <title>
  str title_text = {nullptr, 0}; // just the text, for one that has it already
  if (req->head_title_set) {
    str esc = html_escape(a, req->head_title);
    title_text = esc;
    title = str_fmt(a, "<title>%.*s</title>\n", (int)esc.n, esc.p);
  }
  // A <title> the shell already carries is replaced where it stands, so the page's
  // own title is first in the head and the shell keeps its shape.
  size_t t_open = SIZE_MAX, t_body = 0, t_close = SIZE_MAX, t_end = 0;
  if (title.p) {
    t_open = find_at(body, "<title", 0);
    if (t_open != SIZE_MAX) {
      size_t gt = t_open;
      while (gt < body.n && body.p[gt] != '>') gt++;
      if (gt < body.n) {
        t_body = gt + 1;
        t_close = find_at(body, "</title>", t_body);
        if (t_close != SIZE_MAX) t_end = t_close + 8;
      }
    }
  }
  Buf ins;
  buf_init(&ins, a, (size_t)close_head + req->head.n + (title.p ? title.n : 0) + 32);
  bool replaced = t_close != SIZE_MAX;
  if (title.p && !replaced) buf_putstr(&ins, title);
  if (req->head.n) buf_putstr(&ins, (str){req->head.p, req->head.n});

  Buf out;
  buf_init(&out, a, body.n + ins.n + 16);
  if (replaced) {
    // keep the shell's own <title> … </title> tags and swap only the text between
    buf_write(&out, body.p, t_body);
    buf_putstr(&out, title_text);
    buf_write(&out, body.p + t_close, (size_t)close_head - t_close);
    (void)t_end;
  } else {
    buf_write(&out, body.p, (size_t)close_head);
  }
  buf_putstr(&out, buf_str(&ins));
  buf_write(&out, body.p + (size_t)close_head, body.n - (size_t)close_head);
  if (res->body.p && !res->body.a) wm_free(res->body.p);
  res->body = out;
}

void webc_inject_nonce(Response *res, str nonce) {  if (!res || nonce.n == 0) return;
  if (res->streamed || res->file_path.p || res->upgraded) return;
  // Only HTML: a JSON body or a stylesheet has no script tags, and rewriting it
  // would be a copy per response for nothing.
  if (!res->content_type.p || !str_contains(res->content_type, str_lit("html"))) return;
  inject_nonce(&res->body, nonce);
}

// The page an app actually serves: an HTML shell from disk with the component
// rendered into `<div id="root">`. The browser then hydrates the very same nodes,
// so the first paint costs no JavaScript and the hand-over costs no DOM writes.
Response wc_render_page(i32 status, const char *shell_path, void (*fn)(struct WBuild *, void *),
                  void *ctx) {
  // DIAGNOSTIC: the render-position stack (Wrt.owns/nowns) must be empty when a render
  // starts. A non-zero value here means a previous render left entries behind and the
  // next wkeyed_end() can be handed an instance that no longer exists.
  // TEST BUILD: a render position stack is per-render state, so a render must
  // start with it empty. It does not: measured nowns=1..2 left over from earlier
  // requests on the same task, which is what hands wkeyed_end() a dead instance.
  Wrt *diag_rt = wrt_current();
  if (diag_rt && diag_rt->nowns != 0) diag_rt->nowns = 0;
  Response r = res_new(status, nullptr);
  r.content_type = str_lit("text/html; charset=utf-8");
  Arena *a = r.arena;
  str shell = (str){nullptr, 0};
  FILE *f = shell_path ? fopen(shell_path, "rb") : nullptr;
  if (!shell_path) shell = wc_shell_auto(a);
  if (f) {
    if (fseek(f, 0, SEEK_END) == 0) {
      long n = ftell(f);
      if (n >= 0) {
        rewind(f);
        char *buf = arena_alloc(a, (size_t)n + 1);
        size_t got = fread(buf, 1, (size_t)n, f);
        buf[got] = 0;
        shell = (str){buf, got};
      }
    }
    fclose(f);
  }
  WComponent comp;
  wcomp_init(&comp, nullptr);
  str html = wcomp_html_ctx(&comp, fn, ctx);
  // find the mount point: `<div id="root"></div>`, attributes and all
  size_t at = 0, open_len = 0, close_at = 0;
  bool found = false;
  // Where the view goes: `<div id="root">` in a project's own shell, and whatever id the
  // `boot` block named when the shell is generated. A generated shell with `"el": "#board"`
  // that res_page() could not find was a page with no markup and no scripts on it.
  str mount = shell_path ? str_lit("root") : wc_shell_mount();
  char idattr[128];
  int idlen = snprintf(idattr, sizeof idattr, "id=\"%.*s\"", (int)mount.n, mount.p);
  if (idlen < 0) idlen = 0;
  for (size_t i = 0; i + 5 < shell.n && !found; i++) {
    if (memcmp(shell.p + i, "<div", 4) != 0) continue;
    size_t j = i + 4;
    bool is_root = false;
    while (j < shell.n && shell.p[j] != '>') {
      if (shell.p[j] == 'i' && j + (size_t)idlen < shell.n &&
          memcmp(shell.p + j, idattr, (size_t)idlen) == 0)
        is_root = true;
      j++;
    }
    if (!is_root || j >= shell.n) continue;
    size_t k = j + 1;
    while (k < shell.n && (shell.p[k] == ' ' || shell.p[k] == '\n' || shell.p[k] == '\r'))
      k++;
    if (k + 6 <= shell.n && memcmp(shell.p + k, "</div>", 6) == 0) {
      at = j + 1;
      open_len = at;
      close_at = k;
      found = true;
    }
  }
  if (found) {
    buf_write(&r.body, shell.p, at);
    buf_write(&r.body, html.p, html.n);
    buf_write(&r.body, shell.p + close_at, shell.n - close_at);
  } else {
    // no shell, or no #root in it: render the component on its own
    buf_putstr(&r.body, html);
  }
  (void)open_len;
  wcomp_destroy(&comp);
  Request *cur = webc_req_current();
  if (cur && cur->nonce.n) inject_nonce(&r.body, cur->nonce);
  return r;
}

Response res_bytes(i32 status, const char *ctype, const void *p, size_t n) {
  Response r = res_new(status, nullptr);
  r.content_type = str_lit(ctype);
  buf_write(&r.body, p, n);
  return r;
}

// Set-or-replace: a header that is already there is overwritten, so a middleware can
// change a policy a later one set without duplicating it on the wire.
void res_header_set(Response *r, const char *name, const char *value) {
  if (!r) return;
  for (i32 i = 0; i < r->nheaders; i++)
    if (str_eq_c(r->headers[i].name, name)) {
      r->headers[i].value = str_lit(value);
      return;
    }
  res_header(r, name, value);
}

// A header name or value reaches the wire verbatim (`write_head` prints it between a
// name and a CRLF), so a CR, an LF or a NUL inside one is response splitting, not
// data. A query value is url_decode()d before anybody sees it, which made
// `?next=%0d%0aSet-Cookie:…` a complete header injection. One check here covers every
// caller: %s res_header, res_header_str, res_cookie and the framework's own headers.
static bool header_ok(str s, bool is_name) {
  for (size_t i = 0; i < s.n; i++) {
    unsigned char ch = (unsigned char)s.p[i];
    if (ch == '\r' || ch == '\n' || ch == 0) return false;
    if (is_name && (ch == ':' || ch == ' ' || ch == '\t')) return false;
  }
  return true;
}

void res_header(Response *r, const char *name, const char *value) {
  if (!r) return;
  str n = str_lit(name), v = str_lit(value);
  if (!header_ok(n, true) || !header_ok(v, false)) {
    // Dropped, not escaped: a header that needed escaping was never a header.
    // Counted once so the app finds out, and never silently.
    static _Atomic int warned;
    if (atomic_exchange(&warned, 1) == 0)
      log_error("response header refused: CR/LF/NUL in a header name or value",
                {"name", n}, {"value", v});
    metrics_counter_inc("webc_response_header_refused_total");
    return;
  }
  if (r->nheaders == r->capheaders) {
    i32 ncap = r->capheaders ? r->capheaders * 2 : 4;
    WHeader *nh = arena_alloc(r->arena, (size_t)ncap * sizeof(WHeader));
    if (r->headers) memcpy(nh, r->headers, (size_t)r->nheaders * sizeof(WHeader));
    r->headers = nh;
    r->capheaders = ncap;
  }
  r->headers[r->nheaders++] = (WHeader){.name = n, .value = v};
}

void res_header_str(Response *r, const char *name, str value) {
  res_header(r, name, str_cstr(r->arena, value));
}

// `attrs` is a `;`-separated list, compared case-insensitively: a default attribute
// is skipped when the caller already spells it, so a cookie never carries Path=/
// twice.
static bool attrs_has(const char *attrs, const char *needle, bool assignment) {
  if (!attrs || !*attrs || !needle || !*needle) return false;
  size_t nl = strlen(needle);
  for (const char *p = attrs; *p; p++) {
    while (*p == ' ' || *p == ';' || *p == '\t') p++;
    if (!*p) break;
    const char *t = p;
    while (*p && *p != ';') p++;
    size_t tl = (size_t)(p - t);
    while (tl && (t[tl - 1] == ' ' || t[tl - 1] == '\t')) tl--;
    if (tl < nl) continue;
    if (strncasecmp(t, needle, nl) != 0) continue;
    // "Path" matches "Path=/x" (an assignment); "HttpOnly" matches "HttpOnly" (a flag).
    // A longer token that is not the same attribute — "SecureX" for "Secure" — does not.
    if (tl > nl && t[nl] != '=' && !assignment) continue;
    if (assignment && tl > nl && t[nl] != '=') continue;
    return true;
  }
  return false;
}

void res_cookie(Response *r, const char *name, const char *value, const char *attrs) {
  Buf b;
  buf_init(&b, r->arena, 128);
  buf_printf(&b, "%s=%s", name, value);
  // One attribute each, and none of them twice: the caller's `attrs` are the part
  // they care about (Max-Age, SameSite=Strict, Secure), so an attribute they named
  // themselves replaces the default instead of following it.
  bool has_path = attrs_has(attrs, "Path", true);
  bool has_httponly = attrs_has(attrs, "HttpOnly", false);
  bool has_samesite = attrs_has(attrs, "SameSite", true);
  if (!has_path) buf_puts(&b, "; Path=/");
  if (!has_httponly) buf_puts(&b, "; HttpOnly");
  if (!has_samesite) buf_puts(&b, "; SameSite=Lax");
  // A cookie set over TLS is Secure unless the app says otherwise: without this
  // the session cookie came back on a plaintext request to the same host, which
  // is the exact case SameSite does not cover.
  Request *req = webc_req_current();
  if (req && req->secure && !attrs_has(attrs, "Secure", false)) buf_puts(&b, "; Secure");
  if (attrs && *attrs) buf_printf(&b, "; %s", attrs);
  res_header(r, "Set-Cookie", str_cstr(r->arena, buf_str(&b)));
}

void res_delete_cookie(Response *r, const char *name) {
  res_cookie(r, name, "", "Max-Age=0");
}

// ---------------------------------------------------------------- validators
//
// Two representations of one file have two tags: `"abc"` for the bytes on disk and
// `"abc-gz"` for what `mw_compress()` makes of them. Without the second one a browser that
// cached the compressed module sends `If-None-Match: "abc-gz"`, the server compares it with
// `"abc"`, decides they are different, and re-sends 64 KB every reload — the cache works but
// never hits. So both tags are answered here, in the one function that decides 304.
bool wc_not_modified(Request *req, Response *res, str etag, str last_modified) {
  if (!req || !res) return false;
  str gz_tag = str_fmt(req->arena, "%.*s-gz", (int)etag.n, etag.p);
  str inm = req_header(req, "if-none-match");
  bool plain = inm.n > 0 && str_eq(inm, etag);
  bool gz = !plain && inm.n > 0 && str_eq(inm, gz_tag);
  // `If-Modified-Since` is the coarser validator and it is only consulted when the client
  // sent no tag (RFC 9110 §13.2.2) — without this a static file answered a date-only client
  // with the whole file on every request, which is the half of conditional GET that was
  // silently missing. Dates are compared at the one-second resolution HTTP dates have, or a
  // file re-touched within the same second would answer "not modified" off a stale date.
  if (!plain && !gz && inm.n == 0 && last_modified.n) {
    str ims = req_header(req, "if-modified-since");
    u64 since = 0, lm = 0;
    if (ims.n && http_date_parse(ims, &since) && http_date_parse(last_modified, &lm) &&
        lm / 1000 <= since / 1000)
      plain = true;
  }
  if (!plain && !gz) return false;
  *res = res_new(304, req->arena);
  res_header_str(res, "ETag", gz ? gz_tag : etag);
  if (last_modified.n) res_header_str(res, "Last-Modified", last_modified);
  // the answer still depends on Accept-Encoding: a cache must not reuse this 304 for
  // the other encoding
  res_header(res, "Vary", "Accept-Encoding");
  if (gz) res_header(res, "Content-Encoding", "gzip");
  return true;
}

// ---------------------------------------------------------------- byte ranges

// One run of digits: 0 = no digits, 1 = a number, 2 = past what a u64 holds. The three cases
// are different answers, not one failure: a first position that overflows is a range past the
// end of any file (416), a *last* position that overflows is "to the end" (clamp), and
// something that is not a number at all means the header is malformed and is ignored.
static i32 range_u64(const char **pp, const char *end, u64 *out) {
  const char *p = *pp;
  u64 v = 0;
  bool any = false, overflow = false;
  while (p < end && *p >= '0' && *p <= '9') {
    any = true;
    u64 d = (u64)(*p - '0');
    if (v > (UINT64_MAX - d) / 10) overflow = true;
    else v = v * 10 + d;
    p++;
  }
  if (!any) return 0;
  *pp = p;
  *out = v;
  return overflow ? 2 : 1;
}

WRange http_range_parse(str header, i64 size) {
  WRange out = {0};
  if (!header.p || header.n < 7) return out;
  // `bytes` is the only unit RFC 9110 defines, and the unit is case-insensitive.
  if (strncasecmp(header.p, "bytes=", 6) != 0) return out;
  const char *p = header.p + 6, *end = header.p + header.n;
  while (p < end && (*p == ' ' || *p == '\t')) p++;
  // More than one range in one request: answer with the whole file. Legal — a server MAY
  // ignore `Range` — and `multipart/byteranges` is a body no browser ever asks for.
  if (p >= end || memchr(p, ',', (size_t)(end - p))) return out;

  if (*p == '-') {
    // a suffix: the last N bytes. `bytes=-0` is unsatisfiable, not "the whole file".
    p++;
    u64 n = 0;
    i32 got = range_u64(&p, end, &n);
    if (got == 0) return out;
    if (size <= 0 || n == 0) {
      out.unsatisfiable = true;
      return out;
    }
    if (got == 2 || (i64)n >= size) {
      out.off = 0;
      out.len = size;
    } else {
      out.off = size - (i64)n;
      out.len = (i64)n;
    }
    return out;
  }

  u64 first = 0;
  i32 got_first = range_u64(&p, end, &first);
  if (got_first == 0) return out;
  if (p >= end || *p != '-') return out;
  p++;
  u64 last = 0;
  i32 got_last = range_u64(&p, end, &last);
  if (size <= 0 || got_first == 2 || first >= (u64)size) {
    out.unsatisfiable = true;
    return out;
  }
  // `bytes=0-` and `bytes=0-99999` both mean "to the end": the last byte is clamped, not
  // an error. An inverted range (`bytes=99-1`) is malformed, so it is ignored instead.
  if (got_last != 1 || last >= (u64)size) last = (u64)size - 1;
  if (last < first) return out;
  out.off = (i64)first;
  out.len = (i64)(last - first + 1);
  return out;
}

// Everything a file response needs to be correct, in one place: the validators, the 304, the
// range and its two failure modes. A file as nothing but a path and a length — `write_head`
// printing `Content-Length: st.st_size`, the writer reading to EOF — cannot be seeked,
// resumed or streamed by a media element.
//
// Idempotent on purpose: `res_file()` resolves at construction, which happens *inside* the
// middleware chain and therefore before `mw_compress()` looks at the response, and the
// writer resolves again for a response an app assembled by hand.
bool wc_file_finalize(Request *req, Response *res) {
  if (!res || !res->file_path.p) return false;
  if (res->status == 206 || res->status == 416) return true; // already resolved
  struct stat st;
  if (stat(str_cstr(res->arena, res->file_path), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  i64 size = (i64)st.st_size;
  u64 mtime = (u64)st.st_mtimespec.tv_sec * 1000ull;
  if (res->status != 200) return true; // a file served with another status still streams

  str etag = http_etag_for(res->arena, size, mtime);
  str modified = http_date(res->arena, mtime);
  if (req && wc_not_modified(req, res, etag, modified)) return true; // 304: no file left to send
  res_header_str(res, "ETag", etag);
  res_header_str(res, "Last-Modified", modified);
  res->range_off = 0;
  res->range_len = size;
  res_header(res, "Accept-Ranges", "bytes");
  if (!req) return true;
  if (!str_eq_c(req->method, "GET") && !str_eq_c(req->method, "HEAD")) return true;
  str h = req_header(req, "range");
  if (!h.n) return true;

  // `If-Range`: the client is resuming a copy it already has. Honour the range only when
  // that copy is still this one — a weak tag never is — and answer with the whole (new)
  // representation otherwise. That is the whole point of the header: it is what makes a
  // resumed download safe when the file changed underneath it.
  str ifr = req_header(req, "if-range");
  if (ifr.n) {
    bool same;
    if (ifr.n > 2 && (ifr.p[0] == 'W' || ifr.p[0] == 'w') && ifr.p[1] == '/') {
      same = false;
    } else if (ifr.p[0] == '"') {
      same = str_eq(ifr, etag);
    } else {
      u64 at = 0;
      same = http_date_parse(ifr, &at) && at / 1000 == mtime / 1000;
    }
    if (!same) return true;
  }

  WRange r = http_range_parse(h, size);
  if (r.unsatisfiable) {
    // 416 answers with where the file ends, so a client that asked beyond it learns the size
    // it should have asked about instead of retrying the same range forever.
    *res = res_new(416, req->arena);
    res_header_str(res, "Content-Range", str_fmt(req->arena, "bytes */%lld", (long long)size));
    res_header(res, "Accept-Ranges", "bytes");
    return true;
  }
  if (r.len <= 0) return true;
  res->status = 206;
  res->range_off = r.off;
  res->range_len = r.len;
  res_header_str(res, "Content-Range",
                 str_fmt(req->arena, "bytes %lld-%lld/%lld", (long long)r.off,
                         (long long)(r.off + r.len - 1), (long long)size));
  return true;
}

Response res_file(i32 status, const char *path) {
  Response r = res_new(status, nullptr);
  r.file_path = str_lit(path);
  r.content_type = http_mime_for(path);
  // `Accept-Ranges` is a property of the response, not of the request: a client that sees it
  // knows it may resume, seek or ask for a suffix. It goes on in `wc_file_finalize()` — one
  // place, and only once the file is known to exist — and then the answer to *this* request,
  // resolved here so that `mw_compress()` meets a 206 before it decides to gzip, and so that
  // the bytes on the wire and the bytes a test asserts on come from one decision.
  wc_file_finalize(webc_req_current(), &r);
  return r;
}

// `Content-Disposition: attachment; filename="…"`, plus `filename*=UTF-8''…` (RFC 6266) for
// a name the ASCII fallback cannot spell. The fallback replaces everything unprintable, a
// quote, a backslash, a semicolon and a `%` with `_`: a name that reaches a header verbatim
// is how an upload called `"; rm -rf ~` becomes something other than a file name.
Response res_download(i32 status, const char *path, const char *filename) {
  Response r = res_file(status, path);
  const char *name = filename && *filename ? filename : (path ? path : "download");
  const char *slash = strrchr(name, '/');
  if (slash) name = slash + 1;
  str raw = str_lit(name);
  Buf b;
  buf_init(&b, r.arena, raw.n + 64);
  buf_puts(&b, "attachment; filename=\"");
  bool encoded = false;
  for (size_t i = 0; i < raw.n; i++) {
    unsigned char c = (unsigned char)raw.p[i];
    if (c < 0x20 || c >= 0x7f || c == '"' || c == '\\' || c == ';' || c == '%') {
      encoded = true;
      buf_putc(&b, '_');
    } else {
      buf_putc(&b, (char)c);
    }
  }
  buf_putc(&b, '"');
  if (encoded) {
    buf_puts(&b, "; filename*=UTF-8''");
    buf_putstr(&b, url_encode(r.arena, raw));
  }
  res_header(&r, "Content-Disposition", buf_cstr(&b));
  return r;
}

Response res_stream(i32 status, void (*fn)(Response *, void *), void *ctx) {
  Response r = res_new(status, nullptr);
  r.streamed = true;
  r.stream_fn = fn;
  r.stream_ctx = ctx;
  return r;
}

// ---------------------------------------------------------------- SSE

Response res_upgrade(i32 status, void (*fn)(Response *, i32, void *), void *ctx) {
  Response r = res_new(status, nullptr);
  r.upgraded = true;
  r.upgrade_fn = fn;
  r.upgrade_ctx = ctx;
  return r;
}

Response res_sse(void (*fn)(Response *, void *), void *ctx) {
  Response r = res_stream(200, fn, ctx);
  r.sse = true;
  r.content_type = str_lit("text/event-stream");
  res_header(&r, "Cache-Control", "no-cache");
  res_header(&r, "Connection", "keep-alive");
  res_header(&r, "X-Accel-Buffering", "no");
  return r;
}

// The server gives a streaming response a writer; SSE helpers push frames into it.
static bool sse_put(Response *r, const char *event, str data) {
  if (!r || !r->sse) return false;
  Buf frame;
  buf_init(&frame, r->arena, 64);
  if (event && *event) buf_printf(&frame, "event: %s\n", event);
  // data lines must be split on newlines
  size_t start = 0;
  for (size_t i = 0; i <= data.n; i++) {
    if (i == data.n || data.p[i] == '\n') {
      buf_puts(&frame, "data: ");
      buf_write(&frame, data.p + start, i - start);
      buf_putc(&frame, '\n');
      start = i + 1;
    }
  }
  buf_putc(&frame, '\n');
  // a streaming response owns a socket; the writer is installed by the server
  extern bool webc_stream_write(Response * r, str bytes);
  return webc_stream_write(r, buf_str(&frame));
}

bool sse_send_json(Response *r, const char *event, const void *model, const void *obj) {
  const WModel *m = model;
  JsonWriter w;
  jw_init(&w, r->arena, 64);
  model_json_write(&w, m, obj);
  return sse_put(r, event, jw_finish(&w));
}

bool sse_send_text(Response *r, const char *event, str data) {
  return sse_put(r, event, data);
}

void sse_end(Response *r) {
  if (r) r->finished = true;
}

// ---------------------------------------------------------------- stream frames

void frame_init(WFrame *f, size_t chunk) {
  if (!f) return;
  f->cur = 0;
  f->a[0] = arena_new_labeled(chunk ? chunk : 96 * 1024, "stream-frame");
  f->a[1] = arena_new_labeled(chunk ? chunk : 96 * 1024, "stream-frame");
}

Arena *frame_begin(WFrame *f) {
  if (!f || !f->a[0] || !f->a[1]) return nullptr;
  i32 next = f->cur ^ 1;
  arena_reset(f->a[next]); // the generation two frames ago is dead; this one is fresh
  f->cur = next;
  return wc_arena_swap(f->a[next]);
}

void frame_end(WFrame *f, Arena *prev) {
  if (!f) return;
  wc_arena_swap(prev);
}

void frame_free(WFrame *f) {
  if (!f) return;
  for (i32 i = 0; i < 2; i++) {
    if (f->a[i]) arena_destroy(f->a[i]);
    f->a[i] = nullptr;
  }
}

// ---------------------------------------------------------------- utilities

str http_status_text(i32 status) {
  switch (status) {
  case 200:
    return str_lit("OK");
  case 201:
    return str_lit("Created");
  case 202:
    return str_lit("Accepted");
  case 204:
    return str_lit("No Content");
  case 206:
    return str_lit("Partial Content");
  case 301:
    return str_lit("Moved Permanently");
  case 302:
    return str_lit("Found");
  case 304:
    return str_lit("Not Modified");
  case 307:
    return str_lit("Temporary Redirect");
  case 308:
    return str_lit("Permanent Redirect");
  case 400:
    return str_lit("Bad Request");
  case 401:
    return str_lit("Unauthorized");
  case 403:
    return str_lit("Forbidden");
  case 404:
    return str_lit("Not Found");
  case 405:
    return str_lit("Method Not Allowed");
  case 409:
    return str_lit("Conflict");
  case 413:
    return str_lit("Payload Too Large");
  case 415:
    return str_lit("Unsupported Media Type");
  case 416:
    // the answer to a range past the end of the file; without it the status line reads
    // "HTTP/1.1 416 Unknown", which a strict client may refuse
    return str_lit("Range Not Satisfiable");
  case 422:
    return str_lit("Unprocessable Entity");
  case 429:
    return str_lit("Too Many Requests");
  case 500:
    return str_lit("Internal Server Error");
  case 501:
    return str_lit("Not Implemented");
  case 503:
    return str_lit("Service Unavailable");
  default:
    return str_lit("Unknown");
  }
}

str http_mime_for(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot) return str_lit("application/octet-stream");
  if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm"))
    return str_lit("text/html; charset=utf-8");
  if (!strcasecmp(dot, ".css")) return str_lit("text/css; charset=utf-8");
  if (!strcasecmp(dot, ".js") || !strcasecmp(dot, ".mjs"))
    return str_lit("text/javascript; charset=utf-8");
  if (!strcasecmp(dot, ".wasm")) return str_lit("application/wasm");
  if (!strcasecmp(dot, ".json")) return str_lit("application/json; charset=utf-8");
  if (!strcasecmp(dot, ".svg")) return str_lit("image/svg+xml");
  if (!strcasecmp(dot, ".png")) return str_lit("image/png");
  if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return str_lit("image/jpeg");
  if (!strcasecmp(dot, ".gif")) return str_lit("image/gif");
  if (!strcasecmp(dot, ".webp")) return str_lit("image/webp");
  if (!strcasecmp(dot, ".ico")) return str_lit("image/x-icon");
  if (!strcasecmp(dot, ".woff2")) return str_lit("font/woff2");
  if (!strcasecmp(dot, ".txt")) return str_lit("text/plain; charset=utf-8");
  if (!strcasecmp(dot, ".md")) return str_lit("text/markdown; charset=utf-8");
  if (!strcasecmp(dot, ".pdf")) return str_lit("application/pdf");
  return str_lit("application/octet-stream");
}

str http_date(Arena *a, u64 unix_ms) {
  static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *mons[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  time_t t = (time_t)(unix_ms / 1000);
  struct tm tm;
  gmtime_r(&t, &tm);
  return str_fmt(a, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[tm.tm_wday], tm.tm_mday,
                 mons[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

bool http_date_parse(str s, u64 *out_ms) {
  struct tm tm = {0};
  if (!strptime(str_cstr(nullptr, s), "%a, %d %b %Y %H:%M:%S GMT", &tm)) return false;
  *out_ms = (u64)timegm(&tm) * 1000;
  return true;
}

str http_etag_for(Arena *a, i64 size, u64 mtime_ms) {
  return str_fmt(a, "\"%llx-%llx\"", (unsigned long long)size, (unsigned long long)mtime_ms);
}

// ---------------------------------------------------------------- chain

bool wc_next_run(WNext *nx) {
  if (!nx) return false;
  if (nx->i < nx->n) {
    WMiddleware mw = nx->chain[nx->i++];
    if (mw.fn) return mw.fn(nx->req, mw.ctx, nx);
    return wc_next_run(nx);
  }
  if (!nx->ran) {
    nx->ran = true;
    if (nx->handler) {
      Response r = nx->handler(nx->req);
      if (nx->res) *nx->res = r;
    }
  }
  return true;
}
