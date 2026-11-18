// SPDX-License-Identifier: Apache-2.0
#if !defined(__APPLE__)
#define st_mtimespec st_mtim // same tv_sec, POSIX spelling
#endif
// WebC — middleware.
#define WEBC_NO_SELECT_MACRO 1
#include <webc/http.h>
#include <webc/log.h>
#include <webc/dom.h> // wc_raw_static: the 404 file's markup goes in the way an app puts markup in

// after http.h: gzip.h pulls core.h, and http.h's `state()`/`select()` macros shadow the
// system headers this file needs by name
#include <webc/gzip.h>
#include <webc/task.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// ---------------------------------------------------------------- logging

static bool mw_logging_fn(Request *req, void *ctx, WNext *next) {
  (void)ctx;
  u64 t0 = wc_time_ms();
  bool ok = wc_next_run(next);
  u64 dt = wc_time_ms() - t0;
  if (log_level() <= LOG_INFO) {
    // The request arena, not the heap: a log line lives exactly as long as the
    // request, and this path runs on every single request.
    log_info("request", {"method", req->method}, {"path", req->path},
             {"status", str_fmt(req->arena, "%d", next->res->status)},
             {"dur_ms", str_fmt(req->arena, "%llu", (unsigned long long)dt)},
             {"req_id", req->request_id});
  }
  metrics_observe_http(req->method, next->res->status, dt);
  return ok;
}

WMiddleware mw_logging(void) {
  return (WMiddleware){.name = "logging", .fn = mw_logging_fn};
}

// ---------------------------------------------------------------- request id

static bool mw_request_id_fn(Request *req, void *ctx, WNext *next) {
  (void)ctx;
  str given = req_header(req, "x-request-id");
  if (given.n > 0 && given.n <= 128) {
    req->request_id = str_dup(req->arena, given);
  } else {
    static _Atomic u64 seq;
    u64 n = atomic_fetch_add(&seq, 1);
    req->request_id = str_fmt(
        req->arena, "%08llx%04llx%04llx", (unsigned long long)(wc_wall_ms() & 0xffffffffull),
        (unsigned long long)(n & 0xffff), (unsigned long long)(rand() & 0xffff));
  }
  bool ok = wc_next_run(next);
  res_header_str(next->res, "X-Request-Id", req->request_id);
  return ok;
}

WMiddleware mw_request_id(void) {
  return (WMiddleware){.name = "request_id", .fn = mw_request_id_fn};
}

// ---------------------------------------------------------------- security headers
//
// The policy is one value built from four parts. A CSP spelled out at each of the three
// places that serve HTML — the rendered page, the hand-built response and the dev server —
// makes a project that needs a CDN in `img-src` splice the header by hand, with `strstr` and
// a fixed buffer. So it is a struct: strings in, one header out, and the same struct serves
// the dev policy with the dev additions appended.
str wc_csp_build(Arena *a, const HeadersCfg *cfg, const char *nonce, bool dev, i32 ws,
                 const char *dev_connect) {
  if (cfg && cfg->no_default) return cfg->own ? str_dup(a, str_lit(cfg->own)) : (str){nullptr, 0};
  Buf b;
  buf_init(&b, a, 256);
  buf_puts(&b, "default-src 'self'; img-src ");
  if (cfg && cfg->img_src && cfg->img_src_n > 0) {
    for (i32 i = 0; i < cfg->img_src_n; i++) {
      if (i) buf_putc(&b, ' ');
      buf_puts(&b, cfg->img_src[i]);
    }
  } else {
    buf_puts(&b, "'self' data:");
  }
  buf_puts(&b, "; style-src 'self' 'unsafe-inline'; script-src ");
  if (dev) {
    buf_puts(&b, "'self' 'unsafe-inline' 'wasm-unsafe-eval'");
  } else if (cfg && cfg->script_src && cfg->script_src_n > 0) {
    for (i32 i = 0; i < cfg->script_src_n; i++) {
      if (i) buf_putc(&b, ' ');
      buf_puts(&b, cfg->script_src[i]);
    }
  } else if (nonce && *nonce) {
    // The page the server rendered carries its own nonce on every script tag, so the
    // inline bootstrap runs and anybody else's inline script does not. `wasm-unsafe-eval`
    // is not optional: the default web-assets middleware serves the app's own module at
    // /app.wasm, and without it the browser refuses to instantiate it — the framework's
    // default policy disabled the framework's own browser half.
    buf_printf(&b, "'self' 'nonce-%s' 'wasm-unsafe-eval'", nonce);
  } else {
    buf_puts(&b, "'self' 'wasm-unsafe-eval'");
  }
  if (dev) {
    // The reload script is inline and opens a websocket, and a client-only app talks to an
    // API the framework cannot know: the *dev* policy (never served by a deployment) allows
    // the local ports and any https origin, or exactly the origins `dev.connect` names.
    buf_printf(&b,
               "; connect-src 'self' ws://localhost:%d ws://127.0.0.1:%d %s", (int)ws, (int)ws,
               dev_connect && *dev_connect ? dev_connect
                                           : "https: http://localhost:* http://127.0.0.1:*");
  }
  if (cfg && cfg->extra && *cfg->extra) {
    buf_puts(&b, "; ");
    buf_puts(&b, cfg->extra);
  }
  return buf_str(&b);
}

static bool mw_headers_fn(Request *req, void *ctx, WNext *next) {
  HeadersCfg *cfg = ctx;
  // Recorded before `next`, not after: `mw_dev` runs *inside* this middleware and its own
  // post-`next` code runs first, so a record written on the way out would be too late for the
  // one reader that needs it. The outermost `mw_headers` is the one that wins, which is the
  // same precedence that decides the header itself.
  if (cfg && !req->hdr_cfg) req->hdr_cfg = cfg;
  bool ok = wc_next_run(next);
  res_header(next->res, "X-Content-Type-Options", "nosniff");
  res_header(next->res, "Referrer-Policy", "strict-origin-when-cross-origin");
  res_header(next->res, "X-Frame-Options", "SAMEORIGIN");
  // A response that already carries a policy chose it: `req->csp` (the dev server's, or the
  // app's own via `res_header`) wins over the default, and a project with `no_default` gets
  // no header at all.
  if (req->csp.p) {
    res_header(next->res, "Content-Security-Policy", str_cstr(next->res->arena, req->csp));
    return ok;
  }
  if (cfg && cfg->no_default) return ok;
  if (res_header_get(next->res, "Content-Security-Policy").n) return ok;
  char nonce[64];
  snprintf(nonce, sizeof nonce, "%.*s", (int)req->nonce.n, req->nonce.p ? req->nonce.p : "");
  str csp = wc_csp_build(next->res->arena, cfg, nonce, false, 0, nullptr);
  if (csp.p) res_header(next->res, "Content-Security-Policy", str_cstr(next->res->arena, csp));
  return ok;
}

// `mw_headers_default()` is `mw_headers((HeadersCfg){0})`: the policy this framework has always
// served. A project that needs a CDN, an analytics origin or its own script list names them:
//
//   webc_use(mw_headers((HeadersCfg){
//       .img_src = (const char *[]){"'self'", "data:", "https://cdn.example.com"},
//       .img_src_n = 3,
//       .extra = "frame-ancestors 'self'"}));
//
// Register it *instead of* nothing else: the dev server widens whatever policy is here with
// what live reload needs, so a strict policy costs nothing in development.
WMiddleware mw_headers(HeadersCfg o) {
  HeadersCfg *cfg = wm_alloc(sizeof *cfg);
  *cfg = o;
  return (WMiddleware){.name = "headers", .fn = mw_headers_fn, .ctx = cfg};
}

WMiddleware mw_headers_default(void) { return mw_headers((HeadersCfg){0}); }

// ---------------------------------------------------------------- cors

typedef struct CorsCfg {
  const char *origin; // "*" or a specific origin
  const char *methods;
  const char *headers;
  i32 max_age;
  bool credentials;
} CorsCfg;

static bool mw_cors_fn(Request *req, void *ctx, WNext *next) {
  CorsCfg *cfg = ctx;
  const char *origin = cfg && cfg->origin ? cfg->origin : "*";
  str req_origin = req_header(req, "origin");
  bool preflight = str_eq_c(req->method, "OPTIONS");
  bool ok = true;
  if (!preflight) ok = wc_next_run(next);
  res_header(next->res, "Access-Control-Allow-Origin", strcmp(origin, "*") == 0 ? "*" : origin);
  res_header(next->res, "Access-Control-Allow-Methods",
             cfg && cfg->methods ? cfg->methods : "GET, POST, PUT, PATCH, DELETE, OPTIONS");
  res_header(next->res, "Access-Control-Allow-Headers",
             cfg && cfg->headers ? cfg->headers : "Content-Type, Authorization");
  if (cfg && cfg->max_age > 0) {
    res_header(next->res, "Access-Control-Max-Age", str_fmt(req->arena, "%d", cfg->max_age).p);
  }
  if (cfg && cfg->credentials) res_header(next->res, "Access-Control-Allow-Credentials", "true");
  if (preflight) {
    next->res->status = 204;
  }
  (void)req_origin;
  return ok;
}

WMiddleware mw_cors(void *cfg) {
  return (WMiddleware){.name = "cors", .fn = mw_cors_fn, .ctx = cfg};
}

// ---------------------------------------------------------------- limits

static bool mw_body_limit_fn(Request *req, void *ctx, WNext *next) {
  size_t limit = ctx ? *(size_t *)ctx : 1024 * 1024;
  str cl = req_header(req, "content-length");
  if (cl.n) {
    i64 n = 0;
    if (str_to_i64(cl, &n) && n > (i64)limit) {
      *next->res = res_error(413, "payload_too_large", "request body too large");
      return false;
    }
  }
  return wc_next_run(next);
}

WMiddleware mw_body_limit(size_t bytes) {
  size_t *p = wm_alloc(sizeof *p);
  *p = bytes;
  return (WMiddleware){.name = "body_limit", .fn = mw_body_limit_fn, .ctx = p};
}

static bool mw_timeout_fn(Request *req, void *ctx, WNext *next) {
  i32 ms = ctx ? *(i32 *)ctx : 30 * 1000;
  i64 deadline = (i64)wc_time_ms() + ms;
  // The handler can see the same deadline through req->ctx, so a task it started
  // stops early instead of finishing work whose answer is already a 504.
  if (req->ctx) ctx_set_deadline(req->ctx, deadline);
  bool ok = wc_next_run(next);
  if (wc_time_ms() > (u64)deadline && next->res->status == 200 && next->res->body.n == 0) {
    if (req->ctx) ctx_cancel(req->ctx);
    *next->res = res_error(504, "timeout", "handler exceeded its deadline");
    return false;
  }
  return ok;
}

WMiddleware mw_timeout_ms(i32 ms) {
  i32 *p = wm_alloc(sizeof *p);
  *p = ms;
  return (WMiddleware){.name = "timeout", .fn = mw_timeout_fn, .ctx = p};
}

// ---------------------------------------------------------------- auth

static bool mw_auth_fn(Request *req, void *ctx, WNext *next) {
  const char *token = ctx;
  str auth = req_header(req, "authorization");
  bool ok = false;
  if (auth.n > 7 && strncasecmp(auth.p, "bearer ", 7) == 0) {
    str got = (str){auth.p + 7, auth.n - 7};
    ok = wc_ct_eq(got, str_lit(token));
  } else if (auth.n) {
    // allow a signed session cookie too
    str c = req_cookie(req, "session");
    if (c.n) ok = wc_ct_eq(c, str_lit(token));
  }
  if (!ok) {
    *next->res = res_error(401, "unauthorized", "missing or invalid credentials");
    res_header(next->res, "WWW-Authenticate", "Bearer");
    return false;
  }
  return wc_next_run(next);
}

WMiddleware mw_auth_bearer(const char *token) {
  return (WMiddleware){.name = "auth", .fn = mw_auth_fn, .ctx = (void *)token};
}

// ---------------------------------------------------------------- recover

static bool mw_recover_fn(Request *req, void *ctx, WNext *next) {
  (void)ctx;
  wc_try_frame frame;
  if (wc_try_begin(&frame)) {
    bool ok = wc_next_run(next);
    wc_try_end(&frame);
    return ok;
  }
  // A panic unwound to here. The view that panicked never reached wbuild_end(),
  // so forget the half-built tree: this task's next render would otherwise see a
  // build pointing into a dead stack frame (measured: SIGSEGV at address 72 in
  // add_text, some requests later).
  wbuild_abandon();
  // `fail(422, …)` is a panic too — it is the one way to stop a handler — but it is a
  // decision, not a bug: it gets a warn and the status it asked for, while a genuine
  // panic gets an error and a 500.
  Response mapped = webc_error_from_panic();
  // A 500 that names its route is worth more than a 500 that names its framework: the person
  // reading this is usually looking at a browser tab or a curl, not a log file — and "internal
  // error" over forty routes is where the search begins, not where it ends. The body stays a
  // flat JSON error (`error`, `message`), with the request appended to the message; the status
  // and the shape are unchanged, so a client that parses it keeps working.
  if (mapped.status >= 500 && req->path.p) {
    str route = req->matched_pattern.p ? req->matched_pattern : str_lit("(unmatched)");
    // `fail(500, …)` is a decision and keeps its sentence; a genuine panic stays opaque
    // ("internal error") so a broken handler cannot leak its internals to a stranger — the
    // route, the method and the id are not internals, they are the part that makes the
    // report usable.
    Text why = req->fail_status ? fmt("%s", wc_panic_message()) : fmt("internal error");
    Text full = fmt("%s — %s %s (route %s, id %s)", why,
                    req->method.p ? req->method.p : "?", req->path.p,
                    str_cstr(req->arena, route),
                    req->request_id.p ? req->request_id.p : "?");
    mapped = res_error(mapped.status, "internal_error", full);
  }
  // The request the panic happened in, on the same line as the message: a log of
  // `error panic message=...` over a server with forty routes names no route, and the
  // route is the thing being fixed. `method`, `path`, the matched pattern and the id
  // are the four fields a report needs to be actionable without a second search.
  str route = req->matched_pattern.p ? req->matched_pattern : str_lit("(unmatched)");
  if (mapped.status >= 500)
    log_error("panic", {"method", req->method}, {"path", req->path}, {"route", route},
              {"req_id", req->request_id}, {"message", str_lit(wc_panic_message())});
  else
    log_warn("fail", {"method", req->method}, {"path", req->path}, {"route", route},
             {"req_id", req->request_id},
             {"status", str_from_i64(nullptr, mapped.status)});
  *next->res = mapped;
  return false;
}

WMiddleware mw_recover(void) {
  return (WMiddleware){.name = "recover", .fn = mw_recover_fn};
}

// ---------------------------------------------------------------- metrics

static bool mw_metrics_fn(Request *req, void *ctx, WNext *next) {
  (void)req;
  (void)ctx;
  return wc_next_run(next);
}

WMiddleware mw_metrics(void) {
  return (WMiddleware){.name = "metrics", .fn = mw_metrics_fn};
}

// ---------------------------------------------------------------- static files

typedef struct StaticCfg {
  const char *prefix;
  const char *dir;
  bool spa;
} StaticCfg;

static bool serve_file(Request *req, Response *res, const char *dir, str rel, bool spa);

static bool mw_static_fn(Request *req, void *ctx, WNext *next) {
  StaticCfg *cfg = ctx;
  size_t plen = strlen(cfg->prefix);
  bool matches = req->path.n >= plen && strncmp(req->path.p, cfg->prefix, plen) == 0;
  if (!matches) return wc_next_run(next);

  str rest = (str){req->path.p + plen, req->path.n - plen};
  while (rest.n && rest.p[0] == '/') {
    rest.p++;
    rest.n--;
  }
  if (rest.n == 0) rest = str_lit("index.html");
  // path traversal guard
  if (str_contains(rest, str_lit(".."))) {
    *next->res = res_error(403, "forbidden", "invalid path");
    return false;
  }
  if (serve_file(req, next->res, cfg->dir, rest, cfg->spa)) return false;
  return wc_next_run(next);
}

// A path made of `..` is refused above, but a symlink inside the static directory
// can still point anywhere on disk. So the *resolved* path has to stay under the
// resolved root: that is the check that makes "no directory traversal" true rather
// than merely likely (SPEC §11).
static bool stays_inside(const char *root, const char *path) {
  char real_root[4096], real_path[4096];
  if (!realpath(root, real_root)) return false;
  if (!realpath(path, real_path)) return false;
  size_t n = strlen(real_root);
  if (n == 0) return false;
  if (strncmp(real_path, real_root, n) != 0) return false;
  // "/srv/www-root" must not admit "/srv/www-rootelsewhere"
  return real_path[n] == '\0' || real_path[n] == '/';
}

// A static file is one decision, and it does not live here any more: `res_file()` owns the
// validators (both encodings' tags, `If-Modified-Since`), the 304 and the byte range, so this
// middleware only has to find the file and name it. Spelling the ETag and the 304 here is how
// the static path came to be the only file response in the framework without a range.
static bool serve_file(Request *req, Response *res, const char *dir, str rel, bool spa) {
  // Room for whichever name is longer: the request's (`dir` + "/" + rel) or the SPA fallback
  // (`dir` + "/index.html"), which is 11 bytes after `dir`. Sizing by `rel` alone truncated
  // the fallback to `/srv/site/index.htm` whenever the request path was shorter than
  // "index.html" — `GET /deep/link` was a 404 for a file that was right there.
  size_t cap = strlen(dir) + (rel.n > 11 ? rel.n : 11) + 2;
  char *path = arena_alloc(req->arena, cap);
  snprintf(path, cap, "%s/%.*s", dir, (int)rel.n, rel.p);
  struct stat st;
  if (stat(path, &st) != 0 || S_ISDIR(st.st_mode) || !stays_inside(dir, path)) {
    if (!spa) return false;
    snprintf(path, cap, "%s/index.html", dir);
    if (stat(path, &st) != 0) return false;
  }
  *res = res_file(200, path);
  return true;
}

WMiddleware mw_static(const char *url_prefix, const char *dir) {
  StaticCfg *cfg = wm_alloc(sizeof *cfg);
  cfg->prefix = url_prefix;
  cfg->dir = dir;
  cfg->spa = false;
  return (WMiddleware){.name = "static", .fn = mw_static_fn, .ctx = cfg};
}

WMiddleware mw_static_spa(const char *url_prefix, const char *dir) {
  StaticCfg *cfg = wm_alloc(sizeof *cfg);
  cfg->prefix = url_prefix;
  cfg->dir = dir;
  cfg->spa = true;
  return (WMiddleware){.name = "static", .fn = mw_static_fn, .ctx = cfg};
}
// ---------------------------------------------------------------- web assets

#include <dev_js.h>
#include <webc_js.h>

// What a wasm build needs beside your HTML: the bridge, and the module itself.
// `mw_web_assets(".webc/app.wasm")` serves /webc.js and /app.wasm, and the path
// `webc build` writes is WC_WASM_PATH — the same string, so the artefact exists where
// the middleware looks for it.
typedef struct WebCfg {
  const char *wasm_path;
} WebCfg;

static bool web_file_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

// A project built before the names agreed says `.webc/web.wasm` in its own copy of
// this call, or has a stale `.webc/web.wasm` from an older `webc build`. Serving the
// sibling that does exist — and saying so once — costs one stat and saves the half
// hour of "the module 404s and it is right there on disk".
static const char *wasm_path_for(const char *configured) {
  if (!configured) return nullptr;
  if (web_file_exists(configured)) return configured;
  static _Atomic int warned;
  static char alt[512];
  const char *dot = strrchr(configured, '/');
  size_t dirlen = dot ? (size_t)(dot - configured) : 0;
  const char *base = dot ? dot + 1 : configured;
  if (!dot) return configured;
  const char *other = strcmp(base, "app.wasm") == 0 ? "web.wasm" : "app.wasm";
  int n = snprintf(alt, sizeof alt, "%.*s/%s", (int)dirlen, configured, other);
  if (n > 0 && (size_t)n < sizeof alt && web_file_exists(alt)) {
    if (atomic_exchange(&warned, 1) == 0)
      log_error("wasm: serving the module from a different name than the one configured",
                {"configured", str_lit(configured)}, {"found", str_lit(alt)});
    return alt;
  }
  return configured;
}

static bool mw_web_assets_fn(Request *req, void *ctx, WNext *next) {
  WebCfg *cfg = ctx;
  if (str_eq_c(req->path, "/webc.js")) {
    // The bridge has no file and no mtime, so its validator is its own bytes: two builds
    // that ship the same bridge revalidate against the same tag, and a new framework
    // version invalidates it without anybody bumping a number by hand.
    str etag = str_fmt(req->arena, "\"js-%016llx\"",
                       (unsigned long long)str_hash((str){webc_js, webc_js_len}));
    if (wc_not_modified(req, next->res, etag, (str){0})) return false;
    Response r = res_bytes(200, "text/javascript; charset=utf-8", webc_js, webc_js_len);
    res_header(&r, "Cache-Control", "no-cache");
    res_header_str(&r, "ETag", etag);
    *next->res = r;
    return false;
  }
  if (str_eq_c(req->path, "/app.wasm") && cfg->wasm_path) {
    const char *path = wasm_path_for(cfg->wasm_path);
    if (!path || !web_file_exists(path)) {
      *next->res = res_error(404, "no_wasm", "build it with `webc build --target wasm`");
      return false;
    }
    // The module is a file like any other now: `res_file()` carries the validators, answers a
    // 304, and serves a byte range — which is what makes a 4 MB debug module resumable and
    // lets a range request for its first kilobyte cost a kilobyte.
    Response r = res_file(200, path);
    r.content_type = str_lit("application/wasm");
    res_header(&r, "Cache-Control", "no-cache");
    *next->res = r;
    return false;
  }
  return wc_next_run(next);
}

WMiddleware mw_web_assets(const char *wasm_path) {
  WebCfg *cfg = wm_alloc(sizeof *cfg);
  cfg->wasm_path = wasm_path;
  return (WMiddleware){.name = "web-assets", .fn = mw_web_assets_fn, .ctx = cfg};
}

// ---------------------------------------------------------------- compression
//
// `webc_use(mw_compress())` is the whole API: every response the client said it accepts
// goes out gzipped, and one that is not worth compressing — a PNG, a 40-byte 404, an
// SSE stream — goes out exactly as it was.
//
// Three things have to be right, and each of them has burned someone:
//
//   * Vary: Accept-Encoding goes on every response that *could* have been compressed,
//     not only on the ones that were. A shared cache that stores the identity copy of a
//     stylesheet and hands it to the next client that asks for gzip is a bug the server
//     cannot see.
//   * The ETag has to differ between the two encodings, or a `If-None-Match` that came
//     back from an identity response is answered with a 304 to a gzip request and the
//     browser renders the earlier, uncompressed bytes — which is right by luck until it
//     is not. The suffix `-gz` is the whole fix.
//   * Compression is not free, so a file is compressed once: the cache is keyed by
//     (path, size, mtime), which is exactly "this file has not changed", and the entry
//     is copied into the request arena on the way out so the cache can evict under a
//     response that is still being written.

typedef struct GzSlot {
  char path[512];
  i64 size, mtime;
  Bytes gz;    // heap, owned by the cache
  u64 used;    // a monotonic tick: the smallest one is evicted first
} GzSlot;

typedef struct CompressCfg {
  CompressOpts o;
  WMutex mu;
  GzSlot *slots;
  i32 n, cap;
  u64 tick;
  i32 hits, misses;
} CompressCfg;

// The response header, read without going through the request: `req_header` reads a
// request and the write path has already forgotten it by the time this runs.
str res_header_get(const Response *res, const char *name) {
  for (i32 i = 0; res && i < res->nheaders; i++)
    if (str_eq_c(res->headers[i].name, name)) return res->headers[i].value;
  return (str){nullptr, 0};
}
#define res_get_header res_header_get

static bool res_has_header_name(const Response *res, const char *name) {
  return res_get_header(res, name).p != nullptr;
}

// "gzip" or "*", with anything but q=0. `gzip;q=0`, `identity` and `br, gzip;q=0.5`
// all appear in the wild; the last one is the interesting case and the answer is yes.
static bool accepts_gzip(Request *req) {
  str ae = req_header(req, "accept-encoding");
  if (ae.n == 0) return false; // no header: identity
  size_t i = 0;
  while (i < ae.n) {
    size_t j = i;
    while (j < ae.n && ae.p[j] != ',') j++;
    str one = str_trim(str_sub(ae, i, j));
    i = j + 1;
    str name = one;
    f64 q = 1.0;
    i64 semi = str_index(one, ';');
    if (semi >= 0) {
      name = str_trim(str_sub(one, 0, (size_t)semi));
      str params = str_sub(one, (size_t)semi + 1, one.n);
      i64 qi = str_index(params, 'q');
      if (qi >= 0) {
        str v = str_trim(str_sub(params, (size_t)qi + 2, params.n));
        if (!str_to_f64(v, &q)) q = 0.0;
      }
    }
    if ((str_eq_c(name, "gzip") || str_eq_c(name, "x-gzip") || str_eq_c(name, "*")) && q > 0)
      return true;
  }
  return false;
}

// What gzip actually helps. A wasm module, a stylesheet, JSON and HTML shrink by 60-80%;
// a JPEG, a woff2 or an already-gzipped file grows by 18 bytes plus the CPU.
static bool compressible_type(str ct) {
  if (ct.n == 0) return false;
  i64 semi = str_index(ct, ';');
  str base = str_trim(str_sub(ct, 0, semi < 0 ? ct.n : (size_t)semi));
  static const char *ok_exact[] = {
      "application/json",  "application/javascript", "application/xml",
      "application/wasm",  "application/manifest+json", "image/svg+xml",
      "application/xhtml+xml", "application/rss+xml", "text/csv",
  };
  for (size_t i = 0; i < WC_COUNTOF(ok_exact); i++)
    if (str_eq_c(base, ok_exact[i])) return true;
  if (str_has_prefix_c(base, "text/")) return !str_eq_c(base, "text/event-stream");
  if (str_has_suffix(base, str_lit("+json")) || str_has_suffix(base, str_lit("+xml")))
    return true;
  return false;
}

// The compressed bytes for one file, from the cache or computed once and kept.
// The returned Bytes belong to the caller's arena (a copy), never to the cache.
static Bytes gz_for_file(CompressCfg *cfg, const char *path, i64 size, i64 mtime, Arena *a) {
  Bytes out = {0};
  wmutex_lock(&cfg->mu);
  cfg->tick++;
  i32 free_slot = -1;
  for (i32 i = 0; i < cfg->n; i++) {
    GzSlot *s = &cfg->slots[i];
    if (s->size == size && s->mtime == mtime && strcmp(s->path, path) == 0) {
      s->used = cfg->tick;
      cfg->hits++;
      out = (Bytes){.p = arena_memdup(a, s->gz.p, s->gz.n), .n = s->gz.n};
      wmutex_unlock(&cfg->mu);
      return out;
    }
    if (free_slot < 0 || s->used < cfg->slots[free_slot].used) free_slot = i;
  }
  cfg->misses++;
  wmutex_unlock(&cfg->mu);

  // Compress outside the lock: another request may want a different file, and one slow
  // 170 KB module must not hold up every other response in the process.
  Bytes raw = {0};
  FILE *f = fopen(path, "rb");
  if (!f) return (Bytes){0};
  raw.n = (size_t)size;
  raw.p = (char *)wm_alloc(raw.n ? raw.n : 1);
  size_t got = fread(raw.p, 1, raw.n, f);
  fclose(f);
  raw.n = got;
  Bytes gz = wc_gzip(nullptr, raw, cfg->o.level);
  wm_free(raw.p);
  if (!gz.p || gz.n == 0) {
    if (gz.p) wm_free(gz.p);
    return (Bytes){0};
  }

  wmutex_lock(&cfg->mu);
  if (cfg->n < cfg->cap) {
    free_slot = cfg->n++;
  }
  GzSlot *s = &cfg->slots[free_slot];
  if (s->gz.p) wm_free(s->gz.p);
  snprintf(s->path, sizeof s->path, "%s", path);
  s->size = size;
  s->mtime = mtime;
  s->gz = gz;
  s->used = ++cfg->tick;
  out = (Bytes){.p = arena_memdup(a, gz.p, gz.n), .n = gz.n};
  wmutex_unlock(&cfg->mu);
  return out;
}

static bool mw_compress_fn(Request *req, void *ctx, WNext *next) {
  CompressCfg *cfg = ctx;
  bool ok = wc_next_run(next);
  Response *res = next->res;
  if (!req || !res) return ok;
  // A stream has no length to compress against, an SSE body is supposed to arrive as it
  // is produced, and a 204/304 has no body at all.
  if (res->streamed || res->upgraded) return ok;
  if (res->status == 204 || res->status == 304) return ok;
  // A byte range is answered from the file on disk as the identity representation: the
  // offsets in `Content-Range` are offsets into the bytes the client asked about, so
  // compressing first would both lose them and read a whole file into memory to serve the
  // first kilobyte of it. `res_file()` resolves the range before this middleware sees the
  // response, which is why the status is all this has to check.
  if (res->status == 206 || res->status == 416) return ok;
  if (res_has_header_name(res, "Content-Encoding")) return ok;
  // The type is on the response *or* in its headers: a handler may set either, and a page
  // assembled by the view layer sets the header (with its charset). Reading only the field
  // meant every server-rendered page was "not compressible" and went out at full size.
  str ct = res->content_type.p ? res->content_type : res_get_header(res, "Content-Type");
  if (!compressible_type(ct)) return ok;

  // eligible, so the answer depends on the request: say so even when it is "no"
  res_header(res, "Vary", "Accept-Encoding");
  if (!accepts_gzip(req)) return ok;

  size_t min = cfg->o.min_bytes ? cfg->o.min_bytes : WC_GZIP_MIN;
  Bytes gz = {0};
  if (res->file_path.p) {
    char path[1024];
    snprintf(path, sizeof path, "%.*s", (int)res->file_path.n, res->file_path.p);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return ok;
    if ((size_t)st.st_size < min) return ok;
    gz = gz_for_file(cfg, path, (i64)st.st_size, (i64)st.st_mtimespec.tv_sec * 1000, res->arena);
    if (!gz.p) return ok;
    res->file_path = (str){0};
  } else {
    if (res->body.n < min) return ok;
    gz = wc_gzip(res->arena, (Bytes){.p = res->body.p, .n = res->body.n}, cfg->o.level);
    if (!gz.p || gz.n >= res->body.n) {
      // "compressed" bigger than the original: the client gets the original, and the
      // Vary header above is still true
      return ok;
    }
  }
  res->body = (Buf){.p = gz.p, .n = gz.n, .cap = gz.n, .a = res->arena};
  res_header_set(res, "Content-Encoding", "gzip");
  if (res_has_header_name(res, "Content-Length")) {
    char n[32];
    snprintf(n, sizeof n, "%zu", gz.n);
    res_header_set(res, "Content-Length", n);
  }
  // a 304 is only reusable for the encoding it was computed for
  str etag = res_get_header(res, "ETag");
  if (etag.n) {
    str tagged = str_fmt(res->arena, "%.*s-gz", (int)etag.n, etag.p);
    res_header_set(res, "ETag", str_cstr(res->arena, tagged));
  }
  return ok;
}

WMiddleware mw_compress_opts(CompressOpts o) {
  CompressCfg *cfg = wm_alloc(sizeof *cfg);
  cfg->o = o;
  if (cfg->o.level < -1 || cfg->o.level > 9) cfg->o.level = WC_GZIP_LEVEL;
  cfg->cap = cfg->o.slots > 0 ? cfg->o.slots : 32;
  cfg->slots = wm_alloc((size_t)cfg->cap * sizeof *cfg->slots);
  return (WMiddleware){.name = "compress", .fn = mw_compress_fn, .ctx = cfg};
}

WMiddleware mw_compress(void) {
  return mw_compress_opts((CompressOpts){.level = WC_GZIP_LEVEL, .min_bytes = WC_GZIP_MIN,
                                         .slots = 32});
}

// ---------------------------------------------------------------- the document, finished
//
// A page's <head> and its `data-theme` are collected in the *request* while the view runs,
// and spliced into the response body on the way out (webc_apply_head). That splice has to
// happen while the body is still HTML: `mw_compress` has already turned it into gzip bytes
// by the time the socket writer sees it, and searching compressed bytes for `</head>` finds
// nothing — so every browser loses its title, its description, its OG tags and its nonce on
// inline scripts while every `curl` smoke test passes.
//
// So the framework installs this as the *innermost* middleware: its post-`next` code runs the
// instant the route returns, before static files, before `mw_dev` appends its reload script
// and before anything compresses. `webc_apply_head` is idempotent (it records
// `req->head_applied`), so the socket writer and `webc_test_request` may still call it for a
// response an app assembled outside the chain.
static bool mw_document_fn(Request *req, void *ctx, WNext *next) {
  (void)ctx;
  bool ok = wc_next_run(next);
  Response *res = next->res;
  if (!res) return ok;
  webc_apply_head(res);
  // The policy `mw_headers_default()` emits names the request's nonce, so a hand-built
  // response needs the same pass a rendered one gets inside `wc_render_page`.
  if (req->nonce.n) webc_inject_nonce(res, req->nonce);
  return ok;
}

WMiddleware mw_document(void) { return (WMiddleware){.name = "document", .fn = mw_document_fn}; }

// ---------------------------------------------------------------- a 404 that is a page
//
// The default answer to a URL nobody routed is a JSON error. For a server-rendered app that is
// the wrong shape of answer: the person is in a browser, and the framework's first extra route
// in every project was `GET("/*rest")` rendering a page. A project names the file once —
//
//   "not_found": "public/404.html"
//
// — and gets a real 404: the file's markup inside the app's own shell (so it carries the title,
// the theme and the nonce), status 404, `text/html`, and the shell's chrome. A file that is a
// whole document already (`<html …`) is served as it is: it was written to be one.
typedef struct NotFoundCfg {
  const char *path;
} NotFoundCfg;

// The file's markup goes in through the same `markup()` an app uses for authored HTML, so the
// document it lands in is the document a `page()` render produces.
static void notfound_view(struct WBuild *b, void *ctx) {
  (void)b;
  str *html = ctx;
  head_title("Not found");
  wc_raw_static(html->p);
}

static bool mw_not_found_fn(Request *req, void *ctx, WNext *next) {
  NotFoundCfg *cfg = ctx;
  bool ok = wc_next_run(next);
  Response *res = next->res;
  if (!cfg || !cfg->path || res->status != 404) return ok;
  // The app's own 404 wins: a handler that answered 404 with HTML chose that page.
  if (res->content_type.p && str_contains(res->content_type, str_lit("text/html"))) return ok;
  // ...and so does a client that asked for JSON.
  str accept = req_header(req, "accept");
  if (accept.n && !str_contains(accept, str_lit("text/html"))) return ok;
  Arena *a = res->arena;
  FILE *f = fopen(cfg->path, "rb");
  if (!f) return ok;
  Buf b;
  buf_init(&b, a, 2048);
  char chunk[4096];
  size_t got;
  while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) buf_write(&b, chunk, got);
  fclose(f);
  if (b.n == 0) return ok;
  str html = buf_str(&b);
  Response page;
  bool whole_document = false;
  for (size_t i = 0; html.p && i + 5 <= html.n; i++)
    if ((html.p[i] == '<') && strncasecmp(html.p + i, "<html", 5) == 0) whole_document = true;
  if (whole_document) {
    page = res_bytes(404, "text/html; charset=utf-8", html.p, html.n);
  } else {
    page = wc_page_auto(404, notfound_view, &html);
  }
  page.status = 404;
  res_header_set(&page, "Content-Type", "text/html; charset=utf-8");
  *res = page;
  return ok;
}

WMiddleware mw_not_found(const char *path) {
  NotFoundCfg *cfg = wm_alloc(sizeof *cfg);
  cfg->path = wm_strdup(path);
  return (WMiddleware){.name = "not_found", .fn = mw_not_found_fn, .ctx = cfg};
}

// ---------------------------------------------------------------- dev reload

typedef struct DevCfg {
  i32 ws_port;
  const char *connect; // extra `connect-src` sources from webc.json `dev.connect`, or null
} DevCfg;

// The dev policy's connect-src. A client-only app in a browser exists to talk to somebody
// else's API, and the framework cannot know which one, so the *dev* policy (never served by
// a real deployment) allows any https origin and the usual local dev ports. A project that
// names origins (`"dev": { "connect": ["https://api.example.com"] }`) gets exactly those
// instead: the default is permissive, the knob is a narrowing one.
// The dev policy is *the policy in force* plus what live reload needs — not a policy of its
// own: building it from `nullptr` throws the project's `img-src` away, so a page whose
// thumbnails come from a CDN works in production and shows empty boxes under `webc dev`.
static str dev_csp(Arena *a, const HeadersCfg *cfg, const char *nonce, i32 ws, const char *extra) {
  HeadersCfg shell = {0};
  if (!cfg) {
    shell = wc_shell_csp();
    cfg = &shell;
  }
  return wc_csp_build(a, cfg, nonce, true, ws, extra);
}

// Reads a whole file into the request arena: dev only, and only for HTML.
static str dev_read(const char *path, Arena *a) {
  FILE *f = fopen(path, "rb");
  if (!f) return (str){nullptr, 0};
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return (str){nullptr, 0};
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return (str){nullptr, 0};
  }
  rewind(f);
  char *buf = arena_alloc(a, (size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = 0;
  return (str){buf, got};
}

static bool mw_dev_fn(Request *req, void *ctx, WNext *next) {
  DevCfg *cfg = ctx;
  if (str_eq_c(req->path, "/__webc/dev.js")) {
    Response r = res_bytes(200, "text/javascript; charset=utf-8", dev_js, dev_js_len);
    res_header(&r, "Cache-Control", "no-store");
    *next->res = r;
    return false;
  }
  bool ok = wc_next_run(next);
  Response *res = next->res;
  // a static file is streamed from disk, so pull it in before rewriting it
  if (res->file_path.p && res->content_type.p &&
      str_contains(res->content_type, str_lit("text/html"))) {
    char path_copy[1024];
    snprintf(path_copy, sizeof path_copy, "%.*s", (int)res->file_path.n, res->file_path.p);
    str file = dev_read(path_copy, res->arena);
    if (file.p) {
      res->body = (Buf){.p = (char *)file.p, .n = file.n, .cap = file.n, .a = res->arena};
      res->file_path = (str){0};
    }
  }
  if (res->content_type.p && str_contains(res->content_type, str_lit("text/html")) &&
      res->body.n > 0) {
    str body = (str){res->body.p, res->body.n};
    i64 at = -1;
    const char *close = "</body>";
    for (size_t i = body.n; i >= 7; i--) {
      if (memcmp(body.p + i - 7, close, 7) == 0) {
        at = (i64)(i - 7);
        break;
      }
    }
    Buf nb;
    buf_init(&nb, res->arena, body.n + 256);
    if (at >= 0) {
      buf_write(&nb, body.p, (size_t)at);
    } else {
      buf_write(&nb, body.p, body.n);
    }
    // The reload script is inline, and the default CSP (which a real page wants)
    // blocks both it and the websocket it opens — so in dev the policy is widened by
    // exactly what live reload needs. Found with a browser and a quiet console: the
    // page worked, the tab just never reloaded.
    i32 ws = cfg ? cfg->ws_port : 9999;
    // `req->nonce` rather than the policy's: dev forces `'unsafe-inline'` into `script-src`
    // anyway, and the nonce is what the framework's own inline script carries.
    res_header_set(res, "Content-Security-Policy",
                   str_cstr(res->arena,
                            dev_csp(res->arena, req->hdr_cfg, nullptr, ws,
                                    cfg ? cfg->connect : nullptr)));
    // The reload script is the framework's own inline script, so it carries the page's nonce
    // like every script the view emitted. Without it the *identity* response was nonced by the
    // writer's own pass and the gzip one was not (that pass runs after compression), so the two
    // encodings of the same page differed by one attribute — and a strict policy blocked the
    // reload script in exactly the case nobody curls.
    char nonce[80] = "";
    if (req->nonce.n)
      snprintf(nonce, sizeof nonce, " nonce=\"%.*s\"", (int)req->nonce.n, req->nonce.p);
    buf_printf(&nb,
               "<script%s>window.__WEBC_DEV_PORT__=%d;</script>"
               "<script src=\"/__webc/dev.js\"%s></script>",
               nonce, (int)ws, nonce);
    if (at >= 0) buf_write(&nb, body.p + at, body.n - (size_t)at);
    res->body = nb;
    res_header(res, "Cache-Control", "no-store");
  }
  return ok;
}

WMiddleware mw_dev(i32 ws_port) {
  return mw_dev_connect(ws_port, (str){nullptr, 0});
}

// The same, with the origins the project's own API lives on: space-separated CSP source
// expressions ("https://api.example.com"). Anything here replaces the permissive default,
// so a app that wants a strict dev policy can have one.
WMiddleware mw_dev_connect(i32 ws_port, str connect_src) {
  DevCfg *cfg = wm_alloc(sizeof *cfg);
  cfg->ws_port = ws_port;
  cfg->connect = nullptr;
  if (connect_src.p && connect_src.n) {
    char *copy = wm_alloc(connect_src.n + 1);
    memcpy(copy, connect_src.p, connect_src.n);
    copy[connect_src.n] = 0;
    cfg->connect = copy;
  }
  return (WMiddleware){.name = "dev", .fn = mw_dev_fn, .ctx = cfg};
}
