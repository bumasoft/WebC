// SPDX-License-Identifier: Apache-2.0
// WebC — HTTP/1.1 server on the green-thread scheduler (kqueue-backed).
#define WEBC_NO_SELECT_MACRO 1
#include <webc/crypto.h>
#include <webc/db.h>
#include <webc/relations.h>
#include <webc/http.h>
#include <webc/tls.h>
#include <webc/log.h>
#include <webc/task.h>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define WC_MAX_HEADER_BYTES (32 * 1024)
#define WC_CONN_BUF (16 * 1024)

struct WServer {
  WebcOptions o;
  i32 fd;
  Arena *arena;
  ArenaPool *pool; // request and connection arenas, reused between requests
  _Atomic i32 conns;
  _Atomic i32 inflight;  // requests being handled right now
  _Atomic i64 req_newest_ms; // when the newest in-flight request started (0: none)
  _Atomic u64 served;    // requests finished: the stall monitor watches this
  _Atomic u64 last_seen; // ms when `served` last moved
  _Atomic bool running;
  u64 started_ms;
  str csp;
  bool tls; // TLS configured and initialised
  WRouter *router;
  WCtx *ctx; // the server's own context: shutdown cancels every request below it
};

static WServer *g_srv;
static _Atomic bool g_running;

// ---------------------------------------------------------------- helpers

bool webc_running(void) {
  return atomic_load(&g_running);
}

// ---------------------------------------------------------------- connection

typedef struct Conn {
  i32 fd;
  void *tls; // SSL* when this connection speaks TLS (SPEC §11 build feature)
  WServer *srv;
  Arena *arena;
  Buf in;
  size_t off;
  str peer;
  str buf_peer;
} Conn;

// >0 bytes read, 0 peer closed, -1 read failed (the two are different: a
// failed read is a server-side problem and must not look like a closed client).
static i64 conn_fill(Conn *c) {
  size_t before = c->in.n;
  char tmp[WC_CONN_BUF];
  i64 r = c->tls ? tls_read(c->tls, tmp, sizeof tmp) : wfd_read(c->fd, tmp, sizeof tmp);
  if (r < 0) return -1;
  if (r == 0) return 0;
  buf_write(&c->in, tmp, (size_t)r);
  return (i64)(c->in.n - before);
}

static i64 conn_send(Conn *c, const void *p, size_t n) {
  return c->tls ? tls_write(c->tls, p, n) : wfd_write(c->fd, p, n);
}

static bool conn_write_str(Conn *c, str s) {
  return conn_send(c, s.p, s.n) == (i64)s.n;
}

// ---------------------------------------------------------------- request parsing

static void split_query(Conn *c, Request *req) {
  if (req->query_string.n == 0) return;
  i32 cap = 32, n = 0;
  req->query_k = arena_alloc(req->arena, (size_t)cap * sizeof(str));
  req->query_v = arena_alloc(req->arena, (size_t)cap * sizeof(str));
  size_t i = 0;
  str q = req->query_string;
  while (i <= q.n) {
    size_t j = i;
    while (j < q.n && q.p[j] != '&')
      j++;
    str pair = str_sub(q, i, j);
    if (pair.n) {
      i64 eq = str_index(pair, '=');
      str k = eq >= 0 ? str_sub(pair, 0, (size_t)eq) : pair;
      str v = eq >= 0 ? str_sub(pair, (size_t)eq + 1, pair.n) : (str){"", 0};
      if (n == cap) {
        i32 ncap = cap * 2;
        str *nk = arena_alloc(req->arena, (size_t)ncap * sizeof(str));
        str *nv = arena_alloc(req->arena, (size_t)ncap * sizeof(str));
        memcpy(nk, req->query_k, (size_t)n * sizeof(str));
        memcpy(nv, req->query_v, (size_t)n * sizeof(str));
        req->query_k = nk;
        req->query_v = nv;
        cap = ncap;
      }
      req->query_k[n] = url_decode(req->arena, k, false);
      req->query_v[n] = url_decode(req->arena, v, false);
      n++;
    }
    i = j + 1;
  }
  req->nquery = n;
}

// `reject` is set when the request broke a limit: the caller then answers with that
// status instead of just closing the socket, because a client that sent 10 MB of
// body deserves a 413 rather than a mystery EOF (SPEC §11).
static bool read_request(Conn *c, Request *req, i32 *reject) {
  // find the end of the header block
  size_t hdr_end = (size_t)-1;
  while (hdr_end == (size_t)-1) {
    const char *p = c->in.p + c->off;
    size_t n = c->in.n - c->off;
    if (n >= 4) {
      for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') {
          hdr_end = c->off + i;
          break;
        }
      }
    }
    if (hdr_end != (size_t)-1) break;
    if (n > WC_MAX_HEADER_BYTES) {
      *reject = 431; // request header fields too large
      return false;
    }
    i64 got = conn_fill(c);
    if (got == 0) {
      metrics_counter_inc("webc_conn_closed_eof_total");
      return false;
    }
    if (got < 0) {
      metrics_counter_inc("webc_conn_read_error_total");
      return false;
    }
    if (c->off > 64 * 1024) {
      memmove(c->in.p, c->in.p + c->off, c->in.n - c->off);
      c->in.n -= c->off;
      c->off = 0;
    }
  }

  str block = (str){c->in.p + c->off, hdr_end - c->off};
  size_t head_len = block.n;

  // request line
  i64 eol = str_index(block, '\n');
  if (eol < 0) return false;
  str line = str_trim(str_sub(block, 0, (size_t)eol));
  i64 sp1 = str_index(line, ' ');
  if (sp1 <= 0) return false;
  i64 sp2 = str_index(str_sub(line, (size_t)sp1 + 1, line.n), ' ');
  req->method = str_sub(line, 0, (size_t)sp1);
  str target = sp2 < 0 ? str_sub(line, (size_t)sp1 + 1, line.n)
                       : str_sub(line, (size_t)sp1 + 1, (size_t)sp1 + 1 + (size_t)sp2);
  req->proto = sp2 < 0 ? str_lit("HTTP/1.1") : str_sub(line, (size_t)sp1 + 2 + (size_t)sp2, line.n);

  // target: absolute-form ("http://h/p") or origin-form
  str t = target;
  if (t.n > 7 && strncmp(t.p, "http://", 7) == 0) {
    const char *slash = memchr(t.p + 7, '/', t.n - 7);
    t = slash ? (str){slash, (size_t)(t.p + t.n - slash)} : str_lit("/");
  }
  i64 qmark = str_index(t, '?');
  str raw_path = qmark < 0 ? t : str_sub(t, 0, (size_t)qmark);
  req->query_string = qmark < 0 ? (str){"", 0} : str_sub(t, (size_t)qmark + 1, t.n);
  req->path = url_decode(req->arena, raw_path, false);
  req->head_only = str_eq_c(req->method, "HEAD");
  // The test helper split the query string from the start; the socket path did not,
  // so `query("q")` worked under test and returned empty over the wire. Split here,
  // where the request line is parsed, and let both doors share one behaviour.
  split_query(c, req);

  // headers
  i32 cap = 16, n = 0;
  req->headers = arena_alloc(req->arena, (size_t)cap * sizeof(WHeader));
  size_t pos = (size_t)eol + 1;
  i64 content_length = -1;
  bool chunked = false;
  while (pos < block.n) {
    i64 next = str_index(str_sub(block, pos, block.n), '\n');
    if (next < 0) next = (i64)(block.n - pos);
    str hl = str_trim(str_sub(block, pos, pos + (size_t)next));
    pos += (size_t)next + 1;
    if (hl.n == 0) break;
    i64 colon = str_index(hl, ':');
    if (colon <= 0) continue;
    str name = str_trim(str_sub(hl, 0, (size_t)colon));
    str value = str_trim(str_sub(hl, (size_t)colon + 1, hl.n));
    if (n == cap) {
      i32 ncap = cap * 2;
      WHeader *nh = arena_alloc(req->arena, (size_t)ncap * sizeof(WHeader));
      memcpy(nh, req->headers, (size_t)n * sizeof(WHeader));
      req->headers = nh;
      cap = ncap;
    }
    if (c->srv && c->srv->o.max_headers > 0 && n >= c->srv->o.max_headers) {
      *reject = 431;
      return false;
    }
    req->headers[n++] = (WHeader){name, value};
    if (strncasecmp(name.p, "content-length", name.n) == 0) {
      i64 v = 0;
      if (str_to_i64(value, &v)) content_length = v;
    } else if (strncasecmp(name.p, "transfer-encoding", name.n) == 0 &&
               str_contains(value, str_lit("chunked"))) {
      chunked = true;
    }
  }
  req->nheaders = n;

  str conn_h = req_header(req, "connection");
  req->keep_alive = !str_contains(conn_h, str_lit("close"));
  if (str_eq_c(req->proto, "HTTP/1.0"))
    req->keep_alive = str_contains(conn_h, str_lit("keep-alive"));
  req->host = req_header(req, "host");

  // body
  size_t body_start = hdr_end + 4;
  if (content_length > 0) {
    size_t want = (size_t)content_length;
    size_t cap_body = c->srv && c->srv->o.max_body ? c->srv->o.max_body : 8u * 1024 * 1024;
    if (want > cap_body) {
      *reject = 413; // payload too large
      return false;
    }
    while (c->in.n - body_start < want) {
      i64 got = conn_fill(c);
      if (got == 0) {
        metrics_counter_inc("webc_conn_closed_eof_total");
        return false;
      }
      if (got < 0) {
        metrics_counter_inc("webc_conn_read_error_total");
        return false;
      }
      // the buffer may have moved
      body_start = hdr_end + 4;
    }
    req->body = (Bytes){arena_alloc(req->arena, want + 1), want};
    memcpy(req->body.p, c->in.p + body_start, want);
    c->off = body_start + want;
  } else if (chunked) {
    // A chunked body has no Content-Length, so `max_body` has to be enforced by
    // counting: without this a client could stream until the process died, and a
    // chunk length near SIZE_MAX made `p + chunk + 2` wrap past the only check
    // there was.
    Buf body;
    buf_init(&body, req->arena, 1024);
    size_t cap_body = c->srv && c->srv->o.max_body ? c->srv->o.max_body : 8u * 1024 * 1024;
    size_t p = body_start;
    for (;;) {
      // the chunk-size line: refill until a CRLF arrives. A line longer than a
      // size and an extension can be is a broken client, not a slow one.
      const char *crlf = nullptr;
      for (;;) {
        for (size_t i = p; i + 1 < c->in.n; i++)
          if (c->in.p[i] == '\r' && c->in.p[i + 1] == '\n') {
            crlf = c->in.p + i;
            break;
          }
        if (crlf) break;
        if (c->in.n - p > 64) {
          *reject = 400;
          return false;
        }
        i64 got = conn_fill(c);
        if (got == 0) {
          metrics_counter_inc("webc_conn_closed_eof_total");
          return false;
        }
        if (got < 0) {
          metrics_counter_inc("webc_conn_read_error_total");
          return false;
        }
      }
      size_t line_len = (size_t)(crlf - (c->in.p + p));
      char sizebuf[80];
      size_t copy = line_len < sizeof sizebuf - 1 ? line_len : sizeof sizebuf - 1;
      memcpy(sizebuf, c->in.p + p, copy);
      sizebuf[copy] = 0;
      // the size is the hex prefix; a ";chunk-ext" after it is ignored
      char *endp = nullptr;
      errno = 0;
      unsigned long long parsed = strtoull(sizebuf, &endp, 16);
      if (errno == ERANGE || endp == sizebuf) {
        *reject = 400;
        return false;
      }
      if (parsed > (unsigned long long)cap_body) {
        *reject = 413;
        return false;
      }
      size_t chunk = (size_t)parsed;
      p += line_len + 2;
      if (chunk == 0) {
        // trailers until the final blank line. EOF here is a client that went away, not
        // a reason to keep reading: a loop that does not test for it spins forever, and
        // one such connection per core takes the whole server down.
        size_t tb = 0;
        for (;;) {
          const char *tcrlf = nullptr;
          for (size_t i = p; i + 1 < c->in.n; i++)
            if (c->in.p[i] == '\r' && c->in.p[i + 1] == '\n') {
              tcrlf = c->in.p + i;
              break;
            }
          if (!tcrlf) {
            if (tb > WC_MAX_HEADER_BYTES) {
              *reject = 431;
              return false;
            }
            i64 got = conn_fill(c);
            if (got == 0) {
              metrics_counter_inc("webc_conn_closed_eof_total");
              return false;
            }
            if (got < 0) {
              metrics_counter_inc("webc_conn_read_error_total");
              return false;
            }
            continue;
          }
          size_t tl = (size_t)(tcrlf - (c->in.p + p));
          p += tl + 2;
          tb += tl + 2;
          if (tb > WC_MAX_HEADER_BYTES) {
            *reject = 431;
            return false;
          }
          if (tl == 0) break; // the blank line that ends the trailer block
        }
        break;
      }
      // chunk data and its CRLF. chunk <= cap_body, so this sum cannot wrap.
      if (chunk > cap_body - body.n) {
        *reject = 413;
        return false;
      }
      while (c->in.n < p + chunk + 2) {
        i64 got = conn_fill(c);
        if (got == 0) {
          metrics_counter_inc("webc_conn_closed_eof_total");
          return false;
        }
        if (got < 0) {
          metrics_counter_inc("webc_conn_read_error_total");
          return false;
        }
      }
      buf_write(&body, c->in.p + p, chunk);
      p += chunk + 2;
    }
    req->body = (Bytes){body.p, body.n};
    c->off = p;
  } else {
    req->body = (Bytes){nullptr, 0};
    c->off = body_start;
  }
  (void)head_len;
  return true;
}

// ---------------------------------------------------------------- response writing

static bool res_has_header(const Response *res, const char *name) {
  for (i32 i = 0; res && i < res->nheaders; i++)
    if (str_eq_c(res->headers[i].name, name)) return true;
  return false;
}

static bool write_head(Conn *c, WServer *srv, Request *req, Response *res, size_t body_len,
                       bool chunked_out, i64 file_len) {
  Buf h;
  buf_init(&h, req->arena, 512);
  str stext = http_status_text(res->status);
  buf_printf(&h, "HTTP/1.1 %d %.*s\r\n", res->status, (int)stext.n, stext.p);
  buf_printf(&h, "Date: %.*s\r\n", (int)http_date(req->arena, wc_wall_ms()).n,
             http_date(req->arena, wc_wall_ms()).p);
  if (res->content_type.p) {
    // A handler may set the content type either way round; never emit it twice
    // (a duplicate Content-Type makes browsers reject a wasm module).
    bool has_ct = false;
    for (i32 i = 0; i < res->nheaders; i++)
      if (str_eq_c(res->headers[i].name, "Content-Type")) has_ct = true;
    if (!has_ct)
      buf_printf(&h, "Content-Type: %.*s\r\n", (int)res->content_type.n, res->content_type.p);
  }
  bool have_cl = false;
  for (i32 i = 0; i < res->nheaders; i++) {
    if (strncasecmp(res->headers[i].name.p, "content-length", 14) == 0) have_cl = true;
    // an upgrade answers with its own Connection header; everything else gets ours
    if (strncasecmp(res->headers[i].name.p, "connection", 10) == 0) continue;
    buf_printf(&h, "%.*s: %.*s\r\n", (int)res->headers[i].name.n, res->headers[i].name.p,
               (int)res->headers[i].value.n, res->headers[i].value.p);
  }
  if (res->status != 101 && res->status != 204 && res->status != 304 && !have_cl) {
    if (chunked_out)
      buf_puts(&h, "Transfer-Encoding: chunked\r\n");
    else if (file_len >= 0)
      buf_printf(&h, "Content-Length: %lld\r\n", (long long)file_len);
    else
      buf_printf(&h, "Content-Length: %zu\r\n", body_len);
  }
  if (res->upgraded)
    buf_puts(&h, "Connection: Upgrade\r\n");
  else
    buf_printf(&h, "Connection: %s\r\n", req->keep_alive ? "keep-alive" : "close");
  // Only what no middleware already said: mw_request_id() sets this header on the
  // response, and mw_headers_default() sets the policy, so writing them here as well
  // put the same header on the wire twice (legal HTTP, and a header a load balancer
  // oder a test may read as two different values).
  if (req->request_id.n && !res_has_header(res, "X-Request-Id"))
    buf_printf(&h, "X-Request-Id: %.*s\r\n", (int)req->request_id.n, req->request_id.p);
  if (srv && srv->csp.p && !res_has_header(res, "Content-Security-Policy"))
    buf_printf(&h, "Content-Security-Policy: %.*s\r\n", (int)srv->csp.n, srv->csp.p);
  buf_puts(&h, "\r\n");
  return conn_write_str(c, buf_str(&h));
}

bool webc_stream_write(Response *r, str bytes) {
  if (!r || r->sock <= 0) return false;
  if (bytes.n == 0) return true;
  char hdr[32];
  int n = snprintf(hdr, sizeof hdr, "%zx\r\n", bytes.n);
  Conn c = {.fd = r->sock};
  if (!conn_write_str(&c, (str){hdr, (size_t)n})) return false;
  if (!conn_write_str(&c, bytes)) return false;
  return conn_write_str(&c, str_lit("\r\n"));
}

// A websocket or an SSE stream is *supposed* to stay open: it is the client holding
// the line, not a stuck handler. Those two branches would leave `inflight` raised for as
// long as they run, so the stall detector would call every quiet dev server stalled — the
// reload socket alone is enough to trip it. They bow out of the count before the long
// part, and bow back in after it.
static void long_lived_enter(WServer *srv) { atomic_fetch_sub(&srv->inflight, 1); }
static void long_lived_leave(WServer *srv) { atomic_fetch_add(&srv->inflight, 1); }

static void write_response(Conn *c, Request *req, Response *res) {
  if (res->upgraded) {
    write_head(c, c->srv, req, res, 0, false, -1);
    long_lived_enter(c->srv);
    if (res->upgrade_fn) res->upgrade_fn(res, c->fd, res->upgrade_ctx);
    long_lived_leave(c->srv);
    req->keep_alive = false;
    return;
  }
  if (res->streamed) {
    res->sock = c->fd;
    res->chunked = true;
    write_head(c, c->srv, req, res, 0, true, -1);
    long_lived_enter(c->srv);
    if (res->stream_fn) res->stream_fn(res, res->stream_ctx);
    long_lived_leave(c->srv);
    // terminator
    conn_write_str(c, str_lit("0\r\n\r\n"));
    res->sock = 0;
    return;
  }
  // A `file_path` response is a name until it is resolved: 304, 206 and 416 are decisions
  // about *this* request, and an app that assembled a Response by hand never went through
  // `res_file()`. Resolving here as well as there is what keeps the socket path and the test
  // path one path; the call is a no-op on a response that is already resolved.
  if (res->file_path.p) wc_file_finalize(req, res);
  if (res->file_path.p) {
    i32 fd = open(str_cstr(req->arena, res->file_path), O_RDONLY);
    if (fd < 0) {
      Response e = res_error(404, "not_found", "file not found");
      write_response(c, req, &e);
      return;
    }
    struct stat st;
    fstat(fd, &st);
    // The window, not the file: 0/0 is the whole representation, and a 206 sends exactly
    // `range_len` bytes from `range_off`. This is what makes a video seekable and a download
    // resumable; a reply of `st.st_size` bytes from byte 0 makes neither possible.
    i64 off = res->range_off > 0 ? res->range_off : 0;
    i64 left = res->range_len > 0 ? res->range_len : (i64)st.st_size;
    write_head(c, c->srv, req, res, 0, false, left);
    if (!req->head_only && left > 0) {
      if (off > 0 && lseek(fd, (off_t)off, SEEK_SET) < 0) {
        close(fd);
        return;
      }
      char buf[64 * 1024];
      while (left > 0) {
        size_t want = left < (i64)sizeof buf ? (size_t)left : sizeof buf;
        i64 r = read(fd, buf, want);
        if (r <= 0) break;
        if (conn_send(c, buf, (size_t)r) != r) break;
        left -= r;
      }
    }
    close(fd);
    return;
  }
  size_t body_len = res->body.n;
  // A page that was assembled by hand still gets the request's nonce on its script
  // tags: the policy the headers middleware emits names that nonce, so a tag without
  // it is a script the browser blocks and nobody hears about.
  webc_apply_head(res);
  if (req->nonce.n) webc_inject_nonce(res, req->nonce);
  body_len = res->body.n;
  write_head(c, c->srv, req, res, body_len, false, -1);
  if (!req->head_only && body_len) conn_send(c, res->body.p, body_len);
}

// ---------------------------------------------------------------- dispatch

static Response not_found_handler(Request *req) {
  (void)req;
  return res_error(404, "not_found", "no such route");
}

static Response method_not_allowed_handler(Request *req) {
  (void)req;
  return res_error(405, "method_not_allowed", "method not allowed");
}

static Response dispatch(WServer *srv, Request *req) {
  WMatch m = router_match(srv->arena ? srv->router : webc_router(), req->method, req->path);
  // Even an unmatched path runs the global chain: that is how static files, CORS
  // preflights and the dev-reload script exist without being routes.
  if (!m.found) {
    Handler terminal = m.method_not_allowed ? method_not_allowed_handler : not_found_handler;
    // Globals only — and all of them: the same room as a matched route, so which
    // middleware runs never depends on whether a route existed.
    WMiddleware chain[WC_MW_MAX];
    i32 n = webc_global_middleware(chain, WC_MW_MAX);
    WNext nx = {.req = req, .chain = chain, .n = n, .handler = terminal};
    Response res = res_empty(200);
    nx.res = &res;
    wc_next_run(&nx);
    if (res.status == 0) res.status = 404;
    return res;
  }
  req->matched_pattern = m.pattern;
  if (m.nparams > 16)
    wc_limit_once(&m, "route parameter limit reached: extra parameters are not bound",
                  "16 per route", "parameter");
  for (i32 i = 0; i < m.nparams && i < 16; i++) {
    req->param_k = req->param_k ? req->param_k : arena_alloc(req->arena, 16 * sizeof(str));
    req->param_v = req->param_v ? req->param_v : arena_alloc(req->arena, 16 * sizeof(str));
    req->param_k[req->nparams] = m.param_k[i];
    req->param_v[req->nparams] = m.param_v[i];
    req->nparams++;
  }

  // chain = per-route middleware, then globals. WC_MW_MAX for each, so a chain that is
  // full of framework middleware still has room for the app's own: a stock server installs
  // seven, and a hard 8 here would silently drop the *first* one an app registered.
  WMiddleware chain[WC_MW_MAX * 2];
  i32 n = 0;
  for (i32 i = 0; i < m.nmws && n < WC_MW_MAX; i++)
    chain[n++] = m.mws[i];
  if (m.nmws > WC_MW_MAX)
    wc_limit_once(&chain[0], "route middleware limit reached: extra middleware skipped",
                  "WC_MW_MAX per route", "middleware");
  i32 g_n = webc_global_middleware(chain + n, WC_MW_MAX);
  n += g_n;

  WNext nx = {.req = req, .chain = chain, .n = n, .handler = m.handler};
  Response res = res_empty(200);
  nx.res = &res;
  bool ok = n > 0 ? wc_next_run(&nx) : (res = m.handler(req), true);
  (void)ok;
  if (res.status == 0) {
    // A handler that falls off the end without `return` compiles cleanly in C and would
    // answer an empty 200: the browser shows a blank page, the log shows a 200, and nothing
    // at all points at the route. Name it instead — the mistake is always the same one, and
    // a missing `return` is invisible in review.
    panic("the handler for %.*s %.*s returned no response: a handler must `return` one "
          "(res_html(...), res_text(...), res_json(...), resp_redirect(...)). A missing "
          "`return` is legal C and answers an empty 200.",
          (int)req->method.n, req->method.p, (int)req->path.n, req->path.p);
  }
  return res;
}

// Closing a socket that still has unread data makes the kernel send a RST, and a RST can
// throw away a response the client has not read yet: an over-limit request would answer
// 413 and the browser would still say "connection reset". So the reject path lingers,
// reading and discarding what the client is still sending, for a bounded time.
static void conn_linger(Conn *c, i32 ms) {
  i64 deadline = wc_time_ms() + ms;
  char tmp[4096];
  for (;;) {
    i64 left = deadline - wc_time_ms();
    if (left <= 0) break;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(c->fd, &rf);
    struct timeval tv = {.tv_sec = 0, .tv_usec = (suseconds_t)(left > 1000 ? 1000 * 1000 : left * 1000)};
    i32 r = select(c->fd + 1, &rf, nullptr, nullptr, &tv);
    if (r <= 0) break;
    ssize_t n = read(c->fd, tmp, sizeof tmp);
    if (n <= 0) break;
  }
}

static void conn_main(void *arg) {
  Conn *c = arg;
  WServer *srv = c->srv;
  webc_req_bind(nullptr);
  i32 nreq = 0;
  for (;;) {
    Request req = {0};
    req.arena = arena_pool_get(srv->pool);
    // a pooled arena comes back at the same address: links and fetched rows are per request
    wc_ref_clear();
    wc_row_pool_clear();
    req.fd = c->fd;
    req.secure = c->tls != nullptr;
    req.peer = c->peer;
    req.srv = srv;
    req.start_ms = wc_time_ms();
    req.csp = srv->csp;
    req.max_parts = srv->o.max_multipart_parts > 0 ? srv->o.max_multipart_parts : 32;
    req.max_part_bytes = srv->o.max_multipart_bytes > 0 ? srv->o.max_multipart_bytes : 4u << 20;
    req.max_json_depth = srv->o.max_json_depth;
    req.ctx = ctx_new(srv->ctx);
    // A script tag an SSR page emits carries this nonce (CSP §11): no inline
    // script survives without one, and it is different for every response.
    req.nonce = crypto_random_hex(req.arena, 16);
    webc_req_bind(&req);

    // Declared here, built below: `res_empty()` takes its body's arena from
    // wc_arena_get(), and the request arena is not installed until a few lines down.
    // Built at the top, every response body fell back to the heap and leaked
    // 145 B per request (LeakSanitizer; measured 152 B/request over 38 913 requests).
    Response res;
    i32 reject = 0;
    bool ok = false;
    bool panicked = false;
    // arena-scoped helpers (db_all, str_fmt in handlers, res_new's default arena)
    // use the request arena, so it is installed before anything can allocate —
    // including the parser. wc_arena_swap(), not a direct write: the handler may
    // park and resume on another worker, and a thread-local write after that would
    // land in the wrong thread's slot (measured: a wild arena pointer).
    // The request is counted once its bytes have arrived: a keep-alive connection parked in
    // read_request() is a browser reading the page, not a handler stuck — counting that
    // window prints "server stalled" every few seconds for every quiet tab.
    Arena *prev_arena = wc_arena_swap(req.arena);
    res = res_empty(200); // now the request arena owns the body
    // A panic is a 500, not a dead worker — and that has to include the *parser*: a
    // frame that starts only around dispatch() lets an allocation inside read_request()
    // (a body over the arena's head, a multipart part, a repeated header block) reach
    // abort().
    wc_try_frame frame;
    if (wc_try_begin(&frame)) {
      ok = read_request(c, &req, &reject);
      if (ok) {
        atomic_fetch_add(&srv->inflight, 1);
        atomic_store(&srv->req_newest_ms, (i64)wc_time_ms());
        res = dispatch(srv, &req);
      }
      wc_try_end(&frame);
    } else {
      wbuild_abandon();
      log_error("request panic", {"path", req.path},
                {"message", str_lit(wc_panic_message())});
      res = webc_error_from_panic();
      req.keep_alive = false;
      panicked = true;
    }
    // The handler is done, so its transaction is too: whatever was left open is rolled back
    // and the connection goes back to the pool before the bytes are written.
    db_session_release();

    if (!ok && !panicked) {
      if (reject) {
        // a limit was broken: say which, and close (the rest of the body is not read)
        metrics_counter_inc(reject == 413   ? "webc_request_too_large_total"
                            : reject == 431 ? "webc_request_headers_too_large_total"
                                            : "webc_request_malformed_total");
        req.method = req.method.p ? req.method : str_lit("GET");
        req.proto = str_lit("HTTP/1.1");
        req.keep_alive = false;
        res = res_error(reject,
                        reject == 413   ? "payload_too_large"
                        : reject == 431 ? "headers_too_large"
                                        : "bad_request",
                        reject == 413   ? "request body exceeds the configured limit"
                        : reject == 431 ? "too many request headers"
                                        : "malformed request framing");
        write_response(c, &req, &res);
      } else {
        metrics_counter_inc(c->in.n > c->off ? "webc_conn_close_partial_total"
                                             : "webc_conn_close_idle_total");
        if (log_level() <= LOG_DEBUG)
          log_debug("connection closed", {"fd", str_fmt(c->arena, "%d", (int)c->fd)},
                    {"requests", str_fmt(c->arena, "%d", nreq)},
                    {"leftover", str_fmt(c->arena, "%d", (int)(c->in.n - c->off))});
        if (c->in.n > c->off) {
          res = res_error(400, "bad_request", "malformed request");
          req.proto = str_lit("HTTP/1.1");
          req.method = req.method.p ? req.method : str_lit("GET");
          req.keep_alive = false;
          write_response(c, &req, &res);
          conn_linger(c, 500);
        }
      }
      ctx_cancel(req.ctx); // the client is gone: stop whatever it started
      ctx_free(req.ctx);
      webc_req_unbind();
      wc_arena_swap(prev_arena);
      arena_pool_put(srv->pool, req.arena);
      break;
    }

    // arena-scoped helpers (db_all, str_fmt in handlers) use the request arena.
    // Install it through wc_arena_swap(): the handler may park and resume on
    // another worker, and a direct thread-local write after that would land in
    // the wrong thread's slot (measured: a wild arena pointer in arena_alloc).
    if (res.status == 0) res.status = 200;

    write_response(c, &req, &res);
    if (atomic_fetch_sub(&srv->inflight, 1) == 1)
      atomic_store(&srv->req_newest_ms, 0);
    atomic_fetch_add(&srv->served, 1);
    nreq++;
    webc_req_unbind();
    ctx_free(req.ctx);
    wc_arena_swap(prev_arena);
    arena_pool_put(srv->pool, req.arena);
    if (!req.keep_alive) break;
    if (c->in.n == c->off) {
      c->in.n = 0;
      c->off = 0;
    }
  }
  if (c->tls) tls_close(c->tls);
  wfd_close(c->fd);
  arena_pool_put(srv->pool, c->arena);
  wm_free(c); // it was wm_alloc'd by the accept loop: 105 B per connection, forever
  atomic_fetch_sub(&srv->conns, 1);
}

// ---------------------------------------------------------------- accept loop

static void accept_loop(void *arg) {
  WServer *srv = arg;
  while (atomic_load(&srv->running)) {
    i32 fd = -1;
    if (!wfd_accept(srv->fd, &fd)) {
      if (!atomic_load(&srv->running)) break;
      wc_sleep_ms(1);
      continue;
    }
    if (atomic_load(&srv->conns) >= srv->o.max_conns) {
      const char *busy =
          "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      (void)!write(fd, busy, strlen(busy));
      close(fd);
      continue;
    }
    i32 one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    Conn *c = wm_alloc(sizeof *c);
    c->fd = fd;
    c->srv = srv;
    c->arena = arena_pool_get(srv->pool);
    buf_init(&c->in, c->arena, WC_CONN_BUF);
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    char host[64] = "?";
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) == 0) {
      if (ss.ss_family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)&ss;
        uint8_t *b = (uint8_t *)&in->sin_addr;
        snprintf(host, sizeof host, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
      } else if (ss.ss_family == AF_INET6) {
        snprintf(host, sizeof host, "[ipv6]");
      }
    }
    c->peer = str_lit(arena_strdup(c->arena, host));
    if (srv->tls) {
      c->tls = tls_accept(fd);
      if (!c->tls) {
        // a failed handshake is a client problem, not a server one: count it, close
        metrics_counter_inc("webc_tls_handshake_failures_total");
        // the connection arena was already allocated above; returning it here is
        // the difference between "junk on the TLS port is free for the client"
        // and one leaked 32 KiB chunk per handshake attempt
        arena_pool_put(srv->pool, c->arena);
        wm_free(c);
        close(fd);
        continue;
      }
    }
    atomic_fetch_add(&srv->conns, 1);
    if (log_level() <= LOG_DEBUG)
      log_debug("accepted", {"fd", str_fmt(c->arena, "%d", (int)fd)}, {"peer", str_lit(host)});
    task_async(conn_main, c);
  }
}

// ---------------------------------------------------------------- built-in routes

static Response debug_index(Request *req) {
  Buf b;
  buf_init(&b, req->arena, 512);
  WmStats st = wm_stats();
  buf_printf(&b,
             "{\"heap_used\":%zu,\"heap_peak\":%zu,\"allocs\":%zu,\"frees\":%zu,"
             "\"arena_live\":%zu,\"tasks\":%d,\"routes\":%d,\"requests\":%llu,"
             "\"db_statements\":%llu}",
             st.heap_used, st.heap_peak, st.heap_allocs, st.heap_frees, st.arena_live, wtask_live(),
             webc_route_count(), (unsigned long long)metrics_request_count(),
             (unsigned long long)db_statement_count());
  return wc_res_json_bytes(200, buf_str(&b));
}

// GET /debug/cpu?ms=250 — sample the process for a moment, then report where it
// was. The window is capped at 5 s; this is a debugging aid, not a job.
static Response debug_cpu(Request *req) {
  str ms_s = req_query(req, "ms");
  i64 ms = 250;
  if (ms_s.n) {
    i64 v = 0;
    if (str_to_i64(ms_s, &v) && v > 0) ms = v;
  }
  i32 err = 0;
  if (!webc_profile_start(ms, &err))
    return res_error(409, "profiler_busy", "a profile is already running");
  wc_sleep_ms(ms + 20);
  webc_profile_stop();
  str report = webc_profile_report(req->arena);
  return res_bytes(200, "text/plain; charset=utf-8", report.p, report.n);
}


#ifdef WC_LOCK_HIST
// GET /debug/locks — top g_lock() call sites (build with -DWC_LOCK_HIST). The number that
// matters is locks per request: a scheduler lock that leaves the request path cannot be
// found by reading the code, only by counting where it was taken.
static Response debug_locks(Request *req) {
  long lines[20], counts[20];
  i32 n = 0;
  lock_hist_top(lines, counts, 20, &n);
  Buf b;
  buf_init(&b, req->arena, 1024);
  long total = 0;
  for (i32 i = 0; i < n; i++) total += counts[i];
  buf_printf(&b, "total %ld\n", total);
  for (i32 i = 0; i < n; i++)
    buf_printf(&b, "task.c:%-5ld %8ld  %5.1f%%\n", lines[i], counts[i],
               total ? 100.0 * (double)counts[i] / (double)total : 0.0);
  // Count is not cost: wait time is contention, hold time is what causes it.
  buf_printf(&b, "\nsite      count   wait_avg_us  wait_max_us  hold_avg_us  hold_max_us\n");
  i32 tl[16];
  long tc[16], tw[16], twm[16], th[16], thm[16];
  i32 nt = 0;
  lock_time_top(tl, tc, tw, twm, th, thm, 16, &nt);
  for (i32 i = 0; i < nt; i++)
    buf_printf(&b, "task.c:%-5d %7ld %12.3f %12.3f %12.3f %12.3f\n", tl[i], tc[i],
               tc[i] ? (double)tw[i] / tc[i] / 1000.0 : 0.0, twm[i] / 1000.0,
               tc[i] ? (double)th[i] / tc[i] / 1000.0 : 0.0, thm[i] / 1000.0);
  buf_printf(&b, "\nwmutex callers (offset from wtask_init):\n");
  uintptr_t ras[12];
  long rn[12];
  i32 nr = 0;
  ra_hist_top(ras, rn, 12, &nr);
  for (i32 i = 0; i < nr; i++) buf_printf(&b, "  +0x%lx %8ld\n", (unsigned long)ras[i], rn[i]);
  return res_bytes(200, "text/plain; charset=utf-8", buf_str(&b).p, buf_str(&b).n);
}
#endif

// GET /debug/sched — what the scheduler did: turns, idle entries, polls, wakeups,
// steals and mutex operations. A worker count that raises "polls per request" instead of
// "requests per second" is the signature of contention, not of work.
static Response debug_sched(Request *req) {
  WTaskStats st;
  wtask_stats(&st);
  Buf b;
  buf_init(&b, req->arena, 512);
  buf_printf(&b,
             "{\"procs\":%d,\"iter\":%ld,\"idle\":%ld,\"poll\":%ld,\"wake\":%ld,"
             "\"steal\":%ld,\"lock\":%ld,\"unlock\":%ld,\"ready\":%ld,",
             st.procs, st.iter, st.idle, st.poll, st.wake, st.steal, st.lock, st.unlock, st.ready);
  long fdr = 0, fdw = 0, gf = 0, gs = 0;
  fd_park_counts(&fdr, &fdw);
  gmu_contention(&gf, &gs);
  buf_printf(&b, "\"fd_park_read\":%ld,\"fd_park_write\":%ld,\"gmu_fast\":%ld,"
                "\"gmu_contended\":%ld}",
             fdr, fdw, gf, gs);
  return wc_res_json_bytes(200, buf_str(&b));
}

static Response metrics_index(Request *req) {
  Buf b;
  buf_init(&b, req->arena, 2048);
  if (req->srv) {
    // The two numbers that tell a healthy server from a leaking one.
    buf_printf(&b,
             "# TYPE webc_conns gauge\nwebc_conns %d\n"
             "# TYPE webc_inflight gauge\nwebc_inflight %d\n",
             atomic_load(&req->srv->conns), atomic_load(&req->srv->inflight));
    buf_printf(&b, "# TYPE webc_tasks gauge\nwebc_tasks %d\n", wtask_live());
    buf_printf(&b, "# TYPE webc_requests_total counter\nwebc_requests_total %llu\n",
               (unsigned long long)atomic_load(&req->srv->served));
  }
  metrics_render(req->arena, &b);
  return res_bytes(200, "text/plain; version=0.0.4; charset=utf-8", b.p, b.n);
}

static bool g_builtins;
// A wedged server must say so instead of answering husks forever. Every second,
// compare finished requests with the previous sample; if nothing moved while a
// request is actually being handled, write a task dump to the log once per stall.
//
// "Connections are open" is not the same thing: a browser holds a keep-alive socket open
// while a person reads the page, which would print "server stalled served=10 conns=2" every
// five quiet seconds — a false alarm that teaches the reader to ignore the one message that
// should be believed.
static void stall_monitor(void *arg) {
  WServer *srv = arg;
  u64 last = 0;
  i32 stalled = 0;
  while (atomic_load(&srv->running)) {
    wc_sleep_ms(1000);
    u64 now_served = atomic_load(&srv->served);
    i32 inflight = atomic_load(&srv->inflight);
    if (now_served != last || inflight == 0) {
      last = now_served;
      stalled = 0;
      continue;
    }
    if (++stalled == 5) {
      i64 newest = atomic_load(&srv->req_newest_ms);
      i64 age_ms = newest ? (i64)wc_time_ms() - newest : 0;
      log_error("server stalled",
                {"served", str_fmt(srv->arena, "%llu", (unsigned long long)now_served)},
                {"inflight", str_fmt(srv->arena, "%d", inflight)},
                {"newest_req_age_ms", str_fmt(srv->arena, "%lld", (long long)age_ms)},
                {"conns", str_fmt(srv->arena, "%d", atomic_load(&srv->conns))},
                {"tasks", str_fmt(srv->arena, "%d", wtask_live())});
      // to the log file, not the terminal: this is a diagnosis, not a page
      wtask_dump_safe(stderr);
    }
  }
}

static void register_builtins(WServer *srv) {
  if (g_builtins) return;
  g_builtins = true;
  if (srv->o.metrics) webc_route_add("GET", "/metrics", metrics_index, nullptr, 0);
  if (srv->o.debug) {
    webc_route_add("GET", "/debug", debug_index, nullptr, 0);
    webc_route_add("GET", "/debug/cpu", debug_cpu, nullptr, 0);
    webc_route_add("GET", "/debug/sched", debug_sched, nullptr, 0);
#ifdef WC_LOCK_HIST
    webc_route_add("GET", "/debug/locks", debug_locks, nullptr, 0);
#endif
  }
}

// ---------------------------------------------------------------- serve

// Close the listener and wake whoever is parked in accept(). Closing an fd removes its
// kqueue registration without telling the task waiting on it, so the task would stay
// parked forever — and `wtask_shutdown()` waits for it, which is what turned every
// SIGTERM'd server into an unkillable process. Order matters: shut the socket down (so the
// next accept() fails instead of re-parking), wake the waiter, then close.
static void close_listener(WServer *srv) {
  if (!srv) return;
  atomic_store(&srv->running, false);
  if (srv->ctx) ctx_cancel(srv->ctx); // drain: handlers may finish early
  i32 fd = srv->fd;
  srv->fd = -1;
  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);
    wtask_wake_fd(fd);
    close(fd);
  }
}

// The signal handler writes one byte to a pipe and nothing else: a mutex, a malloc or a
// broadcast in a handler is a deadlock waiting for the one signal that lands while a
// worker holds the scheduler lock. The serve loop is the reader, so it does the shutting
// down at once — no polling tick, and the ordering above is preserved.
static i32 g_sigpipe[2] = {-1, -1};

static void on_signal(int sig) {
  i32 saved = errno;
  char c = (char)sig;
  if (g_sigpipe[1] >= 0) (void)!write(g_sigpipe[1], &c, 1);
  atomic_store(&g_running, false); // belt: a serve loop that is not in wfd_wait yet
  errno = saved;
}

void webc_shutdown(void) {
  atomic_store(&g_running, false);
  if (g_sigpipe[1] >= 0) {
    char c = 'x';
    (void)!write(g_sigpipe[1], &c, 1); // let the serve loop do it, in order
  }
  close_listener(g_srv);
}

// ---------------------------------------------------------------- the listener

// `webc dev` owns the listen socket and hands it down: the new process adopts a dup of it
// and never calls bind() again, so the kernel's accept queue survives a restart and no
// connection is refused. A dup, not the fd itself: the supervisor keeps its copy, so the
// socket stays open across the whole handover and a third process can inherit later.
static i32 adopt_listener(const char *addr, u16 *port_out) {
  const char *inherit = getenv("WEBC_LISTEN_FD");
  if (inherit && *inherit) {
    i32 src = atoi(inherit);
    i32 fd = dup(src);
    if (fd < 0) return -1;
    wfd_nonblock(fd);
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0) {
      if (ss.ss_family == AF_INET)
        *port_out = ntohs(((struct sockaddr_in *)&ss)->sin_port);
      else if (ss.ss_family == AF_INET6)
        *port_out = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    }
    return fd;
  }
  return wfd_open_listen(addr, port_out);
}

// One line on WEBC_READY_FD, then close it. Nobody listening on it is the normal case
// for `webc run` and for every test, so failure here is not an error.
static void ready_fd_report(const char *fmt, ...) {
  const char *env = getenv("WEBC_READY_FD");
  if (!env || !*env) return;
  i32 fd = atoi(env);
  if (fd <= 0) return;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(fd, buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
  close(fd);
}

i32 webc_serve(WebcOptions *opts) {
  WebcOptions o = {0};
  if (opts) o = *opts;
  if (!o.addr) o.addr = getenv("WEBC_ADDR") ? getenv("WEBC_ADDR") : ":8080";
  if (o.max_body == 0) o.max_body = 8 * 1024 * 1024;
  if (o.max_conns == 0) o.max_conns = 10000;
  if (o.max_headers == 0) o.max_headers = 100;
  if (o.drain_ms == 0) o.drain_ms = 3000;
  if (o.arena_per_request == 0) o.arena_per_request = 64 * 1024;

  WServer *srv = wm_alloc(sizeof *srv);
  srv->o = o;
  srv->arena = arena_new(256 * 1024);
  // Request arenas are the hottest allocation in the server: one per request, freed
  // and re-made. `arena_pool_*` existed for exactly this and nothing called it.
  srv->pool = arena_pool_new(srv->o.arena_per_request ? srv->o.arena_per_request : 64 * 1024);
  srv->router = webc_router();
  srv->started_ms = wc_time_ms();
  srv->csp = o.csp ? str_lit(o.csp) : (str){nullptr, 0};
  g_srv = srv;

  if (o.routes) o.routes();
  register_builtins(srv);

  u16 port = 0;
  i32 fd = adopt_listener(o.addr, &port);
  if (fd < 0) {
    log_error("cannot listen", {"addr", str_lit(o.addr)});
    ready_fd_report("error: cannot listen on %s", o.addr);
    return 1;
  }
  srv->fd = fd;
  srv->o.port = port;

  signal(SIGPIPE, SIG_IGN);
  if (g_sigpipe[0] < 0 && pipe(g_sigpipe) == 0) {
    wfd_nonblock(g_sigpipe[0]);
    wfd_nonblock(g_sigpipe[1]);
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  srv->ctx = ctx_new(nullptr);
  if (o.tls_cert || o.tls_key) {
    char terr[256] = {0};
    if (!tls_init(o.tls_cert, o.tls_key, terr, sizeof terr)) {
      log_error("TLS configured but unusable", {"error", str_lit(terr)});
      close(fd);
      return 1;
    }
    srv->tls = true;
  }
  // One worker per core by default (0 = decide for the caller). Requests are
  // green tasks, so a handler that waits on the database parks instead of
  // blocking a core; the arena and the request state travel with the task.
  i32 workers = o.cpus > 0 ? o.cpus : (i32)sysconf(_SC_NPROCESSORS_ONLN);
  if (workers < 1) workers = 1;
  if (workers > 64) workers = 64;
  wtask_init(workers);
  atomic_store(&srv->running, true);
  atomic_store(&g_running, true);
  if (o.on_start) o.on_start();
  log_info("listening", {"addr", str_lit(o.addr)},
           {"routes", str_fmt(nullptr, "%d", webc_route_count())},
           {"tasks", str_fmt(nullptr, "%d", wtask_live())});
  // The supervisor (`webc dev`) holds this pipe open: one line when the new process is
  // actually answering, so the old one is only asked to go away once the replacement can
  // take traffic. Without it a restart has a window with nothing listening, and today
  // that window was permanent.
  ready_fd_report("ready %d\n", (int)port);

  task_async(accept_loop, srv);
  task_async(stall_monitor, srv);
  // Wait on the signal pipe: a SIGTERM is acted on within a millisecond instead of up to
  // one 20 ms tick later, and an idle server consumes no wakeups at all. The timeout is
  // only for `webc_shutdown()` from another thread on a build where the pipe is missing.
  while (atomic_load(&g_running)) {
    if (g_sigpipe[0] >= 0 && wfd_wait(g_sigpipe[0], false, 250)) break;
    else if (g_sigpipe[0] < 0) wc_sleep_ms(20);
  }
  close_listener(srv);

  // drain
  u64 deadline = wc_time_ms() + (u64)o.drain_ms;
  while (atomic_load(&srv->conns) > 0 && wc_time_ms() < deadline)
    wc_sleep_ms(10);
  if (o.on_stop) o.on_stop();
  log_info("stopped", {"conns", str_fmt(nullptr, "%d", atomic_load(&srv->conns))});
  wtask_shutdown();
  tls_shutdown_all();
  return 0;
}

// ---------------------------------------------------------------- test harness

// A request's state has to outlive the call (the response points into it), so it
// lives in a per-thread arena that is *reset* by the next test request instead of
// a fresh arena per call. Allocating one per call leaked 3 KB of heap per request:
// visible as a flat 3200 B/op in bench_http the moment the benchmark started
// reporting B/op.
static _Thread_local Arena *t_test_arena;

static Arena *test_request_arena(void) {
  if (!t_test_arena) {
    t_test_arena = arena_new(64 * 1024);
  } else {
    arena_reset(t_test_arena); // keeps the first chunk, drops the rest
  }
  return t_test_arena;
}

Response webc_test_request(const char *method, const char *path) {
  return webc_test_request_ex(method, path, nullptr, nullptr, 0);
}

Response webc_test_request_ex(const char *method, const char *path, const char *headers,
                              const void *body, size_t body_len) {
  Arena *a = test_request_arena();
  Request req = {0};
  req.arena = a;
  req.method = str_lit(method);
  req.path = str_lit(path);
  req.proto = str_lit("HTTP/1.1");
  req.keep_alive = false;
  req.start_ms = wc_time_ms();
  req.ctx = ctx_new(nullptr);
  req.max_parts = 32;
  req.max_part_bytes = 4u << 20;
  req.nonce = crypto_random_hex(a, 16);
  if (body && body_len) req.body = (Bytes){(char *)body, body_len};
  if (headers && *headers) {
    // "Name: value\r\nName: value" — one header per line, no blank line at the end
    i32 cap = 8, n = 0;
    WHeader *hdrs = arena_alloc(a, (size_t)cap * sizeof(WHeader));
    str block = str_lit(headers);
    size_t i = 0;
    while (i < block.n) {
      size_t j = i;
      while (j < block.n && !(block.p[j] == '\r' && j + 1 < block.n && block.p[j + 1] == '\n'))
        j++;
      str line = str_sub(block, i, j);
      i64 colon = str_index(line, ':');
      if (colon > 0) {
        if (n == cap) {
          i32 ncap = cap * 2;
          WHeader *nh = arena_alloc(a, (size_t)ncap * sizeof(WHeader));
          memcpy(nh, hdrs, (size_t)n * sizeof(WHeader));
          hdrs = nh;
          cap = ncap;
        }
        hdrs[n++] = (WHeader){.name = str_trim(str_sub(line, 0, (size_t)colon)),
                              .value = str_trim(str_sub(line, (size_t)colon + 1, line.n))};
      }
      i = j + 2;
    }
    req.headers = hdrs;
    req.nheaders = n;
  }
  i64 q = str_index(req.path, '?');
  if (q >= 0) {
    req.query_string = str_sub(req.path, (size_t)q + 1, req.path.n);
    req.path = str_sub(req.path, 0, (size_t)q);
    split_query(nullptr, &req);
  }
  webc_req_bind(&req);
  WServer fake = {0};
  fake.router = webc_router();
  // Install the request arena exactly as conn_main does: helpers that allocate
  // "in the current arena" (json, res_new, db rows) must not fall back to the
  // process heap, or the test path would allocate differently from production.
  Arena *prev_arena = wc_arena_swap(a);
  // A handler panic is a 500 here too, exactly like the socket server: a test
  // that a route blows up in must not take the test binary with it.
  wc_try_frame frame;
  Response res;
  if (wc_try_begin(&frame)) {
    res = dispatch(&fake, &req);
    wc_try_end(&frame);
  } else {
    req.keep_alive = false;
    wbuild_abandon();
    res = webc_error_from_panic();
  }
  // A session is pinned on the request's first statement (not on every request), and it
  // ends here: whatever was left open is rolled back and the connection goes back to the
  // pool before another request can be handed it.
  db_session_release();
  wc_arena_swap(prev_arena);
  // The same normalisation the socket writer does, for the same reason: a file response is
  // resolved against this request (304/206/416), so a test asserts the status, the validators
  // and the `Content-Range` a browser would receive instead of the path it was built from.
  if (res.file_path.p) wc_file_finalize(&req, &res);
  // The same nonce pass the socket server does, so a test sees the bytes a browser
  // sees (and so a hand-built page cannot pass a test it fails in production).
  webc_apply_head(&res);
  if (req.nonce.n) webc_inject_nonce(&res, req.nonce);
  ctx_free(req.ctx);
  webc_req_unbind();
  return res;
}

// ---------------------------------------------------------------- websocket

static void sha1_b64(Arena *a, const char *key, str *out) {
  // Sec-WebSocket-Accept = base64(sha1(key + GUID))
  static const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  size_t n = strlen(key) + strlen(guid);
  char *buf = arena_alloc(a, n);
  memcpy(buf, key, strlen(key));
  memcpy(buf + strlen(key), guid, strlen(guid));
  u8 digest[20];
  sha1((const u8 *)buf, n, digest);
  *out = (str){nullptr, 0};
  base64_encode(a, (str){(char *)digest, 20}, out);
}

i32 ws_handshake(Request *req, Response *res) {
  str key = req_header(req, "sec-websocket-key");
  if (key.n == 0) return -1;
  str accept;
  sha1_b64(req->arena, str_cstr(req->arena, key), &accept);
  *res = res_empty(101);
  res_header_str(res, "Upgrade", str_lit("websocket"));
  res_header_str(res, "Connection", str_lit("Upgrade"));
  res_header_str(res, "Sec-WebSocket-Accept", accept);
  return 0;
}

bool ws_send_frame(i32 fd, u8 opcode, str payload) {
  u8 head[10];
  size_t hn = 0;
  head[hn++] = (u8)(0x80 | (opcode & 0x0f));
  if (payload.n < 126) {
    head[hn++] = (u8)payload.n;
  } else if (payload.n < 65536) {
    head[hn++] = 126;
    head[hn++] = (u8)(payload.n >> 8);
    head[hn++] = (u8)(payload.n & 0xff);
  } else {
    head[hn++] = 127;
    for (i32 i = 7; i >= 0; i--)
      head[hn++] = (u8)((u64)payload.n >> (8 * i));
  }
  if (write(fd, head, hn) != (ssize_t)hn) return false;
  return write(fd, payload.p, payload.n) == (ssize_t)payload.n;
}