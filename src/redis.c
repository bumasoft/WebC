// SPDX-License-Identifier: Apache-2.0
// WebC — Redis: RESP3 by hand, over the parked socket layer (SPEC §9c).
//
// There is no hiredis here for the same reason there is no libpq in src/db_pg.c: a client
// library is a dependency, a version floor and a build flag, and RESP is a protocol the size of
// a reply parser. `select` is a statement macro in <webc/task.h> and this file includes
// <unistd.h>, so it opts out exactly as db.c, tls.c and bench_load do.
#define WEBC_NO_SELECT_MACRO 1
#include <webc/redis.h>

#include "internal.h"

#include <webc/log.h>
#include <webc/tls.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WC_REDIS_DEFAULT_TIMEOUT_MS 500
#define WC_REDIS_DEFAULT_ACQUIRE_MS 2000

static i32 g_pool_default;          // 0 = one per worker
static i64 g_acquire_default = -1;  // -1 = env, else the constant
static WMutex g_default_mu = WEBC_MUTEX_INIT;
static WRedis *g_default;
static _Atomic int g_sub_count;

// ---------------------------------------------------------------- connections

typedef struct RConn {
  i32 slot;       // the pool slot this connection was opened in
  i32 fd;
  void *tls;
  char *buf;      // input buffer, heap: a connection outlives every request that uses it
  size_t cap, len, at;
  i64 deadline;   // absolute ms for the command in flight
  i64 timeout_ms;
  bool broken;
  char err[256];
} RConn;

struct WRedis {
  char url[512];
  char host[256];
  char user[128];
  char pass[256];   // never printed: see the AUTH failure message
  char name[64];
  char ca[256];
  char addr[300];   // "host:port/db", no credentials
  u16 port;
  i32 db;
  bool tls, insecure, configured, closed;
  i64 timeout_ms, acquire_ms;
  i32 cap;
  WMutex mu;
  RConn *conns[WC_REDIS_POOL_MAX];
  WChan *slots;
  // Tokens of the slots used most recently, kept as a LIFO. The channel alone is FIFO, so a
  // single-task app walked through every slot once and opened one connection per slot before it
  // ever reused one — ten connections to run one query at a time. A token lives in exactly one
  // place: this stack or the channel.
  i32 hot[8];
  i32 nhot;
  char err[512];
  // The breaker belongs to the handle because the cache asks it: after three consecutive
  // transport failures a caller may stop trying for a few seconds instead of paying the
  // timeout on every request (§5.8).
  i32 consec_errors;
  i32 breaker_opens;
  i64 breaker_until_ms;
};

// ---------------------------------------------------------------- small helpers

static i64 now_ms(void) { return (i64)wc_time_ms(); }

static void set_err(char *dst, size_t cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(dst, cap, fmt, ap);
  va_end(ap);
}

static void sleep_ms(i64 ms) {
  if (ms <= 0) return;
  struct timespec ts = {.tv_sec = (time_t)(ms / 1000),
                        .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, nullptr);
}

static bool parse_i64(str s, i64 *out) {
  if (!s.p || s.n == 0) return false;
  i64 v = 0, sign = 1;
  size_t i = 0;
  if (s.p[0] == '-') {
    sign = -1;
    i = 1;
  }
  if (i >= s.n) return false;
  for (; i < s.n; i++) {
    if (s.p[i] < '0' || s.p[i] > '9') return false;
    v = v * 10 + (s.p[i] - '0');
  }
  *out = v * sign;
  return true;
}

// ---------------------------------------------------------------- URL

static void decoded_into(Arena *a, const char *p, size_t n, char *out, size_t cap) {
  str dec = url_decode(a, (str){.p = (char *)p, .n = n}, false);
  size_t k = dec.n < cap - 1 ? dec.n : cap - 1;
  if (k) memcpy(out, dec.p, k);
  out[k] = 0;
}

static void parse_query_params(WRedis *r, char *qs) {
  char *save = nullptr;
  for (char *kv = strtok_r(qs, "&", &save); kv; kv = strtok_r(nullptr, "&", &save)) {
    char *eq = strchr(kv, '=');
    if (!eq) continue;
    *eq = 0;
    const char *k = kv, *v = eq + 1;
    if (!strcmp(k, "timeout_ms")) r->timeout_ms = atoll(v);
    else if (!strcmp(k, "acquire_ms")) r->acquire_ms = atoll(v);
    else if (!strcmp(k, "pool")) r->cap = atoi(v);
    else if (!strcmp(k, "db")) r->db = atoi(v);
    else if (!strcmp(k, "tls")) r->insecure = !strcmp(v, "insecure");
    else if (!strcmp(k, "ca")) snprintf(r->ca, sizeof r->ca, "%s", v);
    else if (!strcmp(k, "name")) snprintf(r->name, sizeof r->name, "%s", v);
  }
}

// redis://[user[:pass]@]host[:port][/db][?k=v&…]. The split on '@' takes the LAST one before the
// path, so a password that contains '@' works unencoded — which is how a generated password looks
// the day it is pasted into a dashboard.
static bool redis_configure(WRedis *r, const char *url) {
  Arena *a = wc_app_arena();
  r->port = 6379;
  r->db = 0;
  r->cap = g_pool_default;
  r->acquire_ms = g_acquire_default >= 0
                      ? g_acquire_default
                      : (getenv("WEBC_REDIS_ACQUIRE_MS") ? atoll(getenv("WEBC_REDIS_ACQUIRE_MS"))
                                                         : WC_REDIS_DEFAULT_ACQUIRE_MS);
  r->timeout_ms = getenv("WEBC_REDIS_TIMEOUT_MS") ? atoll(getenv("WEBC_REDIS_TIMEOUT_MS"))
                                                  : WC_REDIS_DEFAULT_TIMEOUT_MS;
  if (getenv("WEBC_REDIS_TLS") && !strcmp(getenv("WEBC_REDIS_TLS"), "insecure")) r->insecure = true;
  if (getenv("WEBC_REDIS_CA")) snprintf(r->ca, sizeof r->ca, "%s", getenv("WEBC_REDIS_CA"));
  if (getenv("WEBC_REDIS_POOL")) r->cap = atoi(getenv("WEBC_REDIS_POOL"));
  if (getenv("WEBC_REDIS_NAME")) snprintf(r->name, sizeof r->name, "%s", getenv("WEBC_REDIS_NAME"));

  if (!url || !*url) {
    set_err(r->err, sizeof r->err, "redis: no server configured (set REDIS_URL)");
    return false;
  }
  if (strlen(url) >= sizeof r->url) {
    set_err(r->err, sizeof r->err, "redis: URL too long (max %zu bytes)", sizeof r->url - 1);
    return false;
  }
  snprintf(r->url, sizeof r->url, "%s", url);

  const char *p = url;
  bool secure = false;
  if (!strncmp(p, "rediss://", 9)) {
    secure = true;
    p += 9;
  } else if (!strncmp(p, "redis://", 8)) {
    p += 8;
  } else {
    set_err(r->err, sizeof r->err, "redis: URL must start with redis:// or rediss:// (%s)", url);
    return false;
  }
  r->tls = secure;
#ifndef WEBC_TLS_OPENSSL
  if (secure) {
    set_err(r->err, sizeof r->err, "redis: rediss:// needs a TLS build (-DWEBC_TLS=openssl)");
    return false;
  }
#endif

  char *rest = (char *)p;
  char *slash = strchr(rest, '/');
  char *auth_end = slash ? slash : rest + strlen(rest);
  char *at = nullptr;
  for (char *q = rest; q < auth_end; q++)
    if (*q == '@') at = q;
  if (at) {
    char *colon = memchr(rest, ':', (size_t)(at - rest));
    if (colon) {
      decoded_into(a, rest, (size_t)(colon - rest), r->user, sizeof r->user);
      decoded_into(a, colon + 1, (size_t)(at - colon - 1), r->pass, sizeof r->pass);
    } else {
      decoded_into(a, rest, (size_t)(at - rest), r->pass, sizeof r->pass);
    }
    rest = at + 1;
  }

  char *path = strchr(rest, '/');
  char *qmark = strchr(rest, '?');
  char *host_end = rest + strlen(rest);
  if (path && path < host_end) host_end = path;
  if (qmark && qmark < host_end) host_end = qmark;

  char hostport[300];
  size_t hplen = (size_t)(host_end - rest);
  if (hplen >= sizeof hostport) hplen = sizeof hostport - 1;
  memcpy(hostport, rest, hplen);
  hostport[hplen] = 0;

  char *port_colon = strrchr(hostport, ':');
  if (port_colon && strchr(port_colon, ':') == port_colon) { // not an IPv6 literal
    *port_colon = 0;
    // A port that is not a number is a typo, and a typo must be a named error at boot rather than
    // a silent 6379: the app would connect to the wrong Redis and nobody would know.
    for (const char *d = port_colon + 1; *d; d++)
      if (*d < '0' || *d > '9') {
        set_err(r->err, sizeof r->err, "redis: bad port \"%.20s\" in %s", port_colon + 1, url);
        return false;
      }
    i32 port = atoi(port_colon + 1);
    if (port <= 0 || port > 65535) {
      set_err(r->err, sizeof r->err, "redis: port %d out of range in %s", port, url);
      return false;
    }
    r->port = (u16)port;
  }
  snprintf(r->host, sizeof r->host, "%s", hostport[0] ? hostport : "127.0.0.1");

  char *qs = qmark ? qmark + 1 : nullptr;
  if (path) {
    const char *dg = path + 1;
    size_t dn = 0;
    while (dg[dn] && dg[dn] != '/' && dg[dn] != '?') dn++;
    if (dn) {
      char buf[16];
      size_t k = dn < sizeof buf - 1 ? dn : sizeof buf - 1;
      memcpy(buf, dg, k);
      buf[k] = 0;
      for (size_t d = 0; d < k; d++)
        if (buf[d] < '0' || buf[d] > '9') {
          set_err(r->err, sizeof r->err, "redis: bad db \"%.16s\" in %s — the path is /0 or /1", buf,
                  url);
          return false;
        }
      r->db = atoi(buf);
      if (r->db < 0 || r->db > 15) {
        set_err(r->err, sizeof r->err, "redis: db %d out of range (0-15) in %s", r->db, url);
        return false;
      }
    }
  }
  if (qs && *qs) {
    char *copy = strdup(qs);
    if (copy) {
      char *amp = strchr(copy, '&');
      (void)amp;
      parse_query_params(r, copy);
      free(copy);
    }
  }

  if (!r->name[0]) snprintf(r->name, sizeof r->name, "webc");
  snprintf(r->addr, sizeof r->addr, "%s:%u/%d", r->host, (unsigned)r->port, r->db);

  if (r->cap <= 0) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    r->cap = (i32)(cpus > 0 ? cpus : 1);
  }
  if (r->cap > WC_REDIS_POOL_MAX) r->cap = WC_REDIS_POOL_MAX;
  if (r->acquire_ms <= 0) r->acquire_ms = WC_REDIS_DEFAULT_ACQUIRE_MS;
  if (r->timeout_ms <= 0) r->timeout_ms = WC_REDIS_DEFAULT_TIMEOUT_MS;
  r->configured = true;
  return true;
}

// ---------------------------------------------------------------- the wire
//
// Every byte is *parked* I/O: the socket is non-blocking and wfd_wait() hands the worker to
// another task while Redis thinks. A park is bounded (200 ms) so a shutdown — or a caller that
// gave up — is noticed instead of hiding behind a deadline.

static i64 rd_some(RConn *c, void *buf, size_t n) {
  for (;;) {
    if (c->tls) return tls_read(c->tls, buf, n);
    ssize_t r = read(c->fd, buf, n);
    if (r >= 0) return (i64)r;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      set_err(c->err, sizeof c->err, "read: %s", strerror(errno));
      return -1;
    }
    i64 left = c->deadline - now_ms();
    if (left <= 0) {
      set_err(c->err, sizeof c->err, "no reply within %lld ms (WEBC_REDIS_TIMEOUT_MS)",
              (long long)c->timeout_ms);
      return -2;
    }
    (void)wfd_wait(c->fd, false, left > 200 ? 200 : left);
  }
}

static bool rd_need(RConn *c, size_t n) {
  while (c->len - c->at < n) {
    if (c->at > 0) {
      memmove(c->buf, c->buf + c->at, c->len - c->at);
      c->len -= c->at;
      c->at = 0;
    }
    if (c->cap < c->len + n + 4096) {
      size_t want = c->len + n + 4096;
      size_t nc = c->cap ? c->cap : 8192;
      while (nc < want) nc *= 2;
      c->buf = wm_realloc(c->buf, nc);
      c->cap = nc;
    }
    i64 got = rd_some(c, c->buf + c->len, c->cap - c->len);
    if (got == -2) return false;
    if (got <= 0) {
      if (got == 0) set_err(c->err, sizeof c->err, "connection closed by the server");
      return false;
    }
    c->len += (size_t)got;
  }
  return true;
}

static bool rd_line(RConn *c, char **p, size_t *n) {
  for (;;) {
    size_t avail = c->len - c->at;
    for (size_t i = 0; i + 1 < avail; i++) {
      if (c->buf[c->at + i] == '\r' && c->buf[c->at + i + 1] == '\n') {
        *p = c->buf + c->at;
        *n = i;
        c->at += i + 2;
        return true;
      }
    }
    if (!rd_need(c, avail + 1)) return false;
  }
}

static WReply *parse_reply(Arena *a, RConn *c, i32 depth);

static WReply *reply_new(Arena *a, WReplyKind k) {
  WReply *v = arena_zalloc(a, sizeof *v);
  v->kind = k;
  return v;
}

static WReply *parse_aggregate(Arena *a, RConn *c, i32 depth, bool set) {
  char *line;
  size_t ln, n = 0;
  if (!rd_line(c, &line, &ln)) return nullptr;
  i64 count = 0;
  if (!parse_i64((str){.p = line, .n = ln}, &count)) {
    set_err(c->err, sizeof c->err, "malformed reply: bad aggregate length");
    return nullptr;
  }
  WReply *v = reply_new(a, WR_ARRAY);
  v->set = set;
  if (count < 0) {
    v->kind = WR_NIL;
    return v;
  }
  if (count > 1000000) {
    set_err(c->err, sizeof c->err, "malformed reply: %lld elements", (long long)count);
    return nullptr;
  }
  n = (size_t)count;
  v->n = (i32)n;
  v->items = arena_zalloc(a, sizeof(WReply) * (n ? n : 1));
  for (size_t i = 0; i < n; i++) {
    WReply *item = parse_reply(a, c, depth + 1);
    if (!item) return nullptr;
    v->items[i] = *item;
  }
  return v;
}

static WReply *parse_reply(Arena *a, RConn *c, i32 depth) {
  if (depth > WC_REDIS_MAX_DEPTH) {
    set_err(c->err, sizeof c->err, "malformed reply: nested deeper than %d", WC_REDIS_MAX_DEPTH);
    return nullptr;
  }
  if (!rd_need(c, 1)) return nullptr;
  char t = c->buf[c->at++];
  char *line;
  size_t ln;

  switch (t) {
  case '+':
  case '-': {
    if (!rd_line(c, &line, &ln)) return nullptr;
    WReply *v = reply_new(a, t == '+' ? WR_STATUS : WR_ERROR);
    v->s = (str){.p = arena_memdup(a, line, ln), .n = ln};
    return v;
  }
  case ':': {
    if (!rd_line(c, &line, &ln)) return nullptr;
    WReply *v = reply_new(a, WR_INT);
    if (!parse_i64((str){.p = line, .n = ln}, &v->i)) {
      set_err(c->err, sizeof c->err, "malformed reply: bad integer");
      return nullptr;
    }
    return v;
  }
  case ',': {
    if (!rd_line(c, &line, &ln)) return nullptr;
    char tmp[64];
    size_t k = ln < sizeof tmp - 1 ? ln : sizeof tmp - 1;
    memcpy(tmp, line, k);
    tmp[k] = 0;
    WReply *v = reply_new(a, WR_DOUBLE);
    if (!strcmp(tmp, "inf")) v->d = __builtin_inf();
    else if (!strcmp(tmp, "-inf")) v->d = -__builtin_inf();
    else if (!strcmp(tmp, "nan")) v->d = __builtin_nan("");
    else v->d = strtod(tmp, nullptr);
    return v;
  }
  case '#': {
    if (!rd_line(c, &line, &ln)) return nullptr;
    WReply *v = reply_new(a, WR_BOOL);
    v->b = ln > 0 && (line[0] == 't' || line[0] == 'T');
    return v;
  }
  case '(': {
    // A 128-bit integer is not an i64: it is kept as text, which is the only lossless answer for
    // a value this API has no noun for.
    if (!rd_line(c, &line, &ln)) return nullptr;
    WReply *v = reply_new(a, WR_STR);
    v->s = (str){.p = arena_memdup(a, line, ln), .n = ln};
    return v;
  }
  case '=': {
    // A verbatim string is length-prefixed like a bulk, and its body starts with "txt:" (or
    // "mkd:") — three bytes plus the colon that mean nothing to a caller.
    if (!rd_line(c, &line, &ln)) return nullptr;
    i64 n = 0;
    if (!parse_i64((str){.p = line, .n = ln}, &n) || n < 0 || (u64)n > WC_REDIS_MAX_BULK) {
      set_err(c->err, sizeof c->err, "malformed reply: bad verbatim length");
      return nullptr;
    }
    if (!rd_need(c, (size_t)n + 2)) return nullptr;
    const char *body = c->buf + c->at;
    c->at += (size_t)n + 2;
    WReply *v = reply_new(a, WR_STR);
    if (n > 4 && body[3] == ':') {
      v->s = (str){.p = arena_memdup(a, body + 4, (size_t)n - 4), .n = (size_t)n - 4};
    } else {
      v->s = (str){.p = arena_memdup(a, body, (size_t)n), .n = (size_t)n};
    }
    return v;
  }
  case '!': {
    // A blob error: an error whose text is a bulk, so it may contain a newline.
    if (!rd_line(c, &line, &ln)) return nullptr;
    i64 n = 0;
    if (!parse_i64((str){.p = line, .n = ln}, &n) || n < 0 || (u64)n > WC_REDIS_MAX_BULK) {
      set_err(c->err, sizeof c->err, "malformed reply: bad error length");
      return nullptr;
    }
    if (!rd_need(c, (size_t)n + 2)) return nullptr;
    const char *body = c->buf + c->at;
    c->at += (size_t)n + 2;
    WReply *v = reply_new(a, WR_ERROR);
    v->s = (str){.p = arena_memdup(a, body, (size_t)n), .n = (size_t)n};
    return v;
  }
  case '_':
    if (!rd_line(c, &line, &ln)) return nullptr;
    return reply_new(a, WR_NIL);
  case '$': {
    if (!rd_line(c, &line, &ln)) return nullptr;
    i64 n = 0;
    if (!parse_i64((str){.p = line, .n = ln}, &n)) {
      set_err(c->err, sizeof c->err, "malformed reply: bad bulk length");
      return nullptr;
    }
    if (n < 0) return reply_new(a, WR_NIL);
    if ((u64)n > WC_REDIS_MAX_BULK) {
      set_err(c->err, sizeof c->err, "reply too large: %lld bytes", (long long)n);
      return nullptr;
    }
    if (!rd_need(c, (size_t)n + 2)) return nullptr;
    char *body = c->buf + c->at;
    c->at += (size_t)n + 2;
    WReply *v = reply_new(a, WR_STR);
    v->s = (str){.p = arena_memdup(a, body, (size_t)n), .n = (size_t)n};
    return v;
  }
  case '*':
    return parse_aggregate(a, c, depth, false);
  case '~':
    return parse_aggregate(a, c, depth, true);
  case '>':
    return parse_aggregate(a, c, depth, true); // a push: not an answer; callers ignore it
  case '%': {
    char *l2;
    size_t l2n;
    if (!rd_line(c, &l2, &l2n)) return nullptr;
    i64 pairs = 0;
    if (!parse_i64((str){.p = l2, .n = l2n}, &pairs) || pairs < 0) {
      set_err(c->err, sizeof c->err, "malformed reply: bad map length");
      return nullptr;
    }
    WReply *v = reply_new(a, WR_MAP);
    v->n = (i32)pairs;
    v->items = arena_zalloc(a, sizeof(WReply) * (size_t)(pairs > 0 ? pairs * 2 : 1));
    for (i32 i = 0; i < v->n * 2; i++) {
      WReply *item = parse_reply(a, c, depth + 1);
      if (!item) return nullptr;
      v->items[i] = *item;
    }
    return v;
  }
  case '|': {
    // An attribute decorates the value that follows it, and is shaped like a map (`|<n>` then n
    // key/value pairs, with no `%` marker of its own). Parse it and drop it: a caller has no use
    // for a TTL hint on a GET, and leaving it in the stream would desynchronise the next read.
    if (!rd_line(c, &line, &ln)) return nullptr;
    i64 pairs = 0;
    if (!parse_i64((str){.p = line, .n = ln}, &pairs) || pairs < 0) {
      set_err(c->err, sizeof c->err, "malformed reply: bad attribute length");
      return nullptr;
    }
    for (i64 i = 0; i < pairs * 2; i++)
      if (!parse_reply(a, c, depth + 1)) return nullptr;
    return parse_reply(a, c, depth + 1);
  }
  default:
    set_err(c->err, sizeof c->err, "malformed reply: unexpected byte 0x%02x",
            (unsigned char)t);
    return nullptr;
  }
}

static bool wr_all(RConn *c, const char *p, size_t n) {
  while (n) {
    if (c->tls) {
      i64 w = tls_write(c->tls, p, n);
      if (w <= 0) {
        set_err(c->err, sizeof c->err, "TLS write failed");
        return false;
      }
      p += w;
      n -= (size_t)w;
      continue;
    }
    ssize_t w = write(c->fd, p, n);
    if (w > 0) {
      p += w;
      n -= (size_t)w;
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      i64 left = c->deadline - now_ms();
      if (left <= 0 || !wfd_wait(c->fd, true, left > 200 ? 200 : left)) {
        set_err(c->err, sizeof c->err, "no room to write within %lld ms",
                (long long)c->timeout_ms);
        return false;
      }
      continue;
    }
    set_err(c->err, sizeof c->err, "write: %s", strerror(errno));
    return false;
  }
  return true;
}

static void frame_arg(Buf *b, const char *p, size_t n) {
  buf_putc(b, '$');
  buf_puti64(b, (i64)n);
  buf_write(b, "\r\n", 2);
  if (n) buf_write(b, p, n);
  buf_write(b, "\r\n", 2);
}

static void frame_cmd(Buf *b, i32 argc, const str *argv) {
  buf_putc(b, '*');
  buf_puti64(b, argc);
  buf_write(b, "\r\n", 2);
  for (i32 i = 0; i < argc; i++) frame_arg(b, argv[i].p ? argv[i].p : "", argv[i].n);
}

// ---------------------------------------------------------------- connect + handshake

static void conn_free(RConn *c) {
  if (!c) return;
  if (c->tls) tls_close(c->tls);
  if (c->fd >= 0) wfd_close(c->fd);
  if (c->buf) wm_free(c->buf);
  wm_free(c);
}

static bool authed_error(WRedis *r, RConn *c, str msg) {
  set_err(r->err, sizeof r->err, "redis: AUTH refused (%s) — check REDIS_URL credentials",
          r->addr);
  set_err(c->err, sizeof c->err, "%s", r->err);
  (void)msg;
  return false;
}

static bool handshake(WRedis *r, RConn *c) {
  Arena *a = arena_new(2048);
  bool ok = true;
  Buf b;
  buf_init(&b, nullptr, 256);

  str argv[8];
  i32 n = 0;
  argv[n++] = str_lit("HELLO");
  argv[n++] = str_lit("3");
  if (r->pass[0]) {
    argv[n++] = str_lit("AUTH");
    argv[n++] = r->user[0] ? str_lit(r->user) : str_lit("default");
    argv[n++] = str_lit(r->pass);
  }
  if (r->name[0]) {
    argv[n++] = str_lit("SETNAME");
    argv[n++] = str_lit(r->name);
  }
  frame_cmd(&b, n, argv);
  c->deadline = now_ms() + r->timeout_ms;
  ok = wr_all(c, b.p, b.n);
  WReply *rep = ok ? parse_reply(a, c, 0) : nullptr;
  buf_free(&b);

  bool fallback = false;
  if (!rep) {
    ok = false;
  } else if (rep->kind == WR_ERROR) {
    if (str_ci_contains(rep->s, "unknown command") || str_ci_contains(rep->s, "wrong number") ||
        str_ci_contains(rep->s, "noproto"))
      fallback = true; // Redis < 6, or a RESP2-only server
    else if (str_ci_contains(rep->s, "noauth") || str_ci_contains(rep->s, "wrongpass") ||
             str_ci_contains(rep->s, "invalid password") || str_ci_contains(rep->s, "invalid username"))
      ok = authed_error(r, c, rep->s);
    else {
      set_err(r->err, sizeof r->err, "redis: HELLO refused (%s): %.*s", r->addr, (int)rep->s.n,
              rep->s.p);
      set_err(c->err, sizeof c->err, "%s", r->err);
      ok = false;
    }
  }

  if (fallback && ok) {
    // AUTH, then SELECT, then CLIENT SETNAME — each one checked, because a connection that is
    // silently unauthenticated or on the wrong database is the failure that takes an evening.
    const char *verbs[3];
    str args[6];
    i32 counts[3];
    i32 ns = 0;
    if (r->pass[0]) {
      verbs[ns] = "AUTH";
      if (r->user[0]) {
        args[ns * 2] = str_lit(r->user);
        args[ns * 2 + 1] = str_lit(r->pass);
        counts[ns] = 2;
      } else {
        args[ns * 2] = str_lit(r->pass);
        counts[ns] = 1;
      }
      ns++;
    }
    if (r->db != 0) {
      verbs[ns] = "SELECT";
      args[ns * 2] = str_fmt(a, "%d", r->db);
      counts[ns] = 1;
      ns++;
    }
    if (r->name[0]) {
      verbs[ns] = "CLIENT";
      args[ns * 2] = str_lit("SETNAME");
      args[ns * 2 + 1] = str_lit(r->name);
      counts[ns] = 2;
      ns++;
    }
    for (i32 i = 0; i < ns && ok; i++) {
      str cmd_argv[3];
      cmd_argv[0] = str_lit(verbs[i]);
      for (i32 k = 0; k < counts[i]; k++) cmd_argv[k + 1] = args[i * 2 + k];
      Buf fb;
      buf_init(&fb, nullptr, 128);
      frame_cmd(&fb, counts[i] + 1, cmd_argv);
      c->deadline = now_ms() + r->timeout_ms;
      ok = wr_all(c, fb.p, fb.n);
      buf_free(&fb);
      WReply *sr = ok ? parse_reply(a, c, 0) : nullptr;
      if (!sr) {
        ok = false;
      } else if (sr->kind == WR_ERROR) {
        if (!strcmp(verbs[i], "AUTH"))
          ok = authed_error(r, c, sr->s);
        else if (!strcmp(verbs[i], "SELECT"))
          set_err(r->err, sizeof r->err, "redis: SELECT %d refused (%s): %.*s", r->db, r->addr,
                  (int)sr->s.n, sr->s.p);
        else
          set_err(r->err, sizeof r->err, "redis: %s refused (%s): %.*s", verbs[i], r->addr,
                  (int)sr->s.n, sr->s.p);
        if (!ok && sr->kind == WR_ERROR && strcmp(verbs[i], "AUTH"))
          set_err(c->err, sizeof c->err, "%s", r->err);
        ok = false;
      }
    }
  }

  arena_destroy(a);
  return ok;
}

static RConn *conn_open(WRedis *r) {
  RConn *c = wm_alloc_zero(sizeof *c);
  c->slot = -1;
  c->fd = -1;
  c->timeout_ms = r->timeout_ms;
  c->fd = wfd_open_connect(r->host, r->port, r->acquire_ms > 0 ? r->acquire_ms : r->timeout_ms);
  if (c->fd < 0) {
    set_err(r->err, sizeof r->err, "redis: connect %s:%u: %s", r->host, (unsigned)r->port,
            strerror(errno));
    conn_free(c);
    return nullptr;
  }
#ifdef WEBC_TLS_OPENSSL
  if (r->tls) {
    char terr[256] = {0};
    c->tls = tls_connect(c->fd, r->host, r->ca[0] ? r->ca : nullptr, r->insecure, terr,
                         sizeof terr);
    if (!c->tls) {
      set_err(r->err, sizeof r->err, "redis: TLS handshake with %s:%u failed: %s", r->host,
              (unsigned)r->port, terr[0] ? terr : "unknown");
      conn_free(c);
      return nullptr;
    }
  }
#endif
  if (!handshake(r, c)) {
    if (r->err[0] == 0)
      set_err(r->err, sizeof r->err, "redis: handshake failed (%s): %s", r->addr, c->err);
    conn_free(c);
    return nullptr;
  }
  return c;
}

// ---------------------------------------------------------------- the pool

static void pool_init(WRedis *r) {
  if (r->slots) return;
  r->slots = wchan_new(sizeof(i32), r->cap > 0 ? r->cap : 1);
  for (i32 i = 0; i < r->cap; i++) wchan_send(r->slots, &i);
}

static RConn *pool_get(WRedis *r, char *why, size_t whycap) {
  if (!r || !r->configured) {
    snprintf(why, whycap, "%s", (r && r->err[0]) ? r->err : "redis: no server configured (set REDIS_URL)");
    if (r) snprintf(r->err, sizeof r->err, "%s", why);
    return nullptr;
  }
  wmutex_lock(&r->mu);
  pool_init(r);
  wmutex_unlock(&r->mu);

  i32 slot = -1;
  wmutex_lock(&r->mu);
  if (r->nhot > 0) slot = r->hot[--r->nhot]; // the connection that just finished, first
  wmutex_unlock(&r->mu);

  i64 deadline = now_ms() + r->acquire_ms;
  bool waited = false;
  for (;;) {
    if (slot >= 0) break;
    if (wchan_try_recv(r->slots, &slot)) break;
    if (!waited) {
      metrics_counter_inc("webc_redis_pool_waits_total");
      waited = true;
    }
    i64 left = deadline - now_ms();
    if (left <= 0) {
      snprintf(why, whycap,
               "redis: no connection free after %lld ms (pool %d; raise WEBC_REDIS_POOL)",
               (long long)r->acquire_ms, r->cap);
      snprintf(r->err, sizeof r->err, "%s", why);
      return nullptr;
    }
    sleep_ms(left > 2 ? 2 : left); // try_recv + a short sleep: never a worker-blocking spin
  }
  if (slot < 0 || slot >= WC_REDIS_POOL_MAX) slot = 0;

  // The slot's connection stays in conns[] while it is borrowed — the token in hand is what says
  // it is in use — so `redis_pool_size()` is the number of connections that exist, not the number
  // idle at this instant.
  wmutex_lock(&r->mu);
  RConn *c = r->conns[slot];
  wmutex_unlock(&r->mu);

  if (!c) {
    c = conn_open(r);
    if (!c) {
      wchan_send(r->slots, &slot);
      snprintf(why, whycap, "%s", r->err);
      return nullptr;
    }
    c->slot = slot;
    wmutex_lock(&r->mu);
    r->conns[slot] = c;
    wmutex_unlock(&r->mu);
  }
  c->deadline = now_ms() + r->timeout_ms;
  return c;
}

static void pool_put(WRedis *r, RConn *c, bool broken) {
  if (!c) return;
  i32 slot = c->slot;
  if (slot < 0 || slot >= WC_REDIS_POOL_MAX || !r) {
    conn_free(c);
    return;
  }
  if (broken || c->broken) {
    // A connection that failed mid-command is freed, not handed back: whatever came after the
    // failure on that socket is not a reply to anything this pool asked for.
    wmutex_lock(&r->mu);
    if (r->conns[slot] == c) r->conns[slot] = nullptr;
    wmutex_unlock(&r->mu);
    conn_free(c);
  }
  wmutex_lock(&r->mu);
  bool stacked = r->nhot < (i32)WC_COUNTOF(r->hot);
  if (stacked) r->hot[r->nhot++] = slot;
  wmutex_unlock(&r->mu);
  if (!stacked) wchan_send(r->slots, &slot); // the stack is full: the token goes back in the queue
}

// ---------------------------------------------------------------- run one command

static bool run_argv(WRedis *r, i32 argc, const str *argv, Arena *a, WReply *out, Arena *own) {
  memset(out, 0, sizeof *out);
  out->kind = WR_NIL;
  char why[512] = {0};
  RConn *c = pool_get(r, why, sizeof why);
  if (!c) {
    // Not a nil reply: the caller has to be able to tell "Redis says the key is absent" from
    // "Redis was not reachable", and only an error reply can carry the difference.
    out->kind = WR_ERROR;
    size_t n = strlen(why);
    out->s = (str){.p = arena_memdup(a, why, n), .n = n};
    return false;
  }

  Buf b;
  buf_init(&b, nullptr, 256);
  frame_cmd(&b, argc, argv);
  c->deadline = now_ms() + r->timeout_ms;
  bool ok = wr_all(c, b.p, b.n);
  buf_free(&b);
  if (!ok) {
    set_err(r->err, sizeof r->err, "redis: %.200s (%s)", c->err, r->addr);
    metrics_counter_inc("webc_redis_errors_total");
    r->consec_errors++;
    pool_put(r, c, true);
    out->kind = WR_ERROR;
    out->s = (str){.p = arena_memdup(a, r->err, strlen(r->err)), .n = strlen(r->err)};
    return false;
  }

  u64 t0 = wc_time_ms();
  WReply *v = parse_reply(a, c, 0);
  metrics_observe("webc_redis_command_ms", (f64)(wc_time_ms() - t0), "redis round trip in ms");
  metrics_counter_inc("webc_redis_commands_total");
  if (!v) {
    set_err(r->err, sizeof r->err, "redis: %.200s (%s)", c->err, r->addr);
    metrics_counter_inc("webc_redis_errors_total");
    r->consec_errors++;
    pool_put(r, c, true);
    out->kind = WR_ERROR;
    out->s = (str){.p = arena_memdup(a, r->err, strlen(r->err)), .n = strlen(r->err)};
    return false;
  }
  *out = *v;
  out->own = own;
  if (v->kind != WR_ERROR) r->consec_errors = 0;
  pool_put(r, c, false);
  return true;
}

static Arena *res_arena(void) {
  Arena *a = wc_arena_get();
  return a ? a : wc_app_arena();
}

// ---------------------------------------------------------------- handles

WRedis *redis_open(const char *url) {
  WRedis *r = wm_alloc_zero(sizeof *r);
  if (!redis_configure(r, url)) {
    // A malformed URL is the caller's mistake and it is caught here, at boot, instead of
    // surfacing as a mysterious "connection refused" on the first request. An unreachable server
    // is the opposite case: that handle exists, so the app can run without Redis (§5.8).
    wm_free(r);
    return nullptr;
  }
  return r;
}

void redis_close(WRedis *r) {
  if (!r || r == g_default) return;
  wmutex_lock(&r->mu);
  for (i32 i = 0; i < WC_REDIS_POOL_MAX; i++) {
    if (r->conns[i]) conn_free(r->conns[i]);
    r->conns[i] = nullptr;
  }
  r->closed = true;
  wmutex_unlock(&r->mu);
  wm_free(r);
}

// The process default: opened from REDIS_URL the first time it is asked for. A failure does not
// panic — the handle exists and every call fails with redis_error(), because a cache must be able
// to be down and an app that cannot live without Redis says so itself.
WRedis *redis(void) {
  WRedis *r = g_default;
  if (r) return r;
  wmutex_lock(&g_default_mu);
  if (!g_default) g_default = redis_open(getenv("REDIS_URL"));
  r = g_default;
  wmutex_unlock(&g_default_mu);
  return r;
}

void redis_set_default(WRedis *r) {
  wmutex_lock(&g_default_mu);
  g_default = r;
  wmutex_unlock(&g_default_mu);
}

bool redis_ok(WRedis *r) { return r && r->configured && r->err[0] == 0; }

// "Configured" is not "healthy": a call that failed a minute ago leaves `redis_ok()` false, which
// is what an app asking "can I live without Redis?" wants to know. A cache asks a different
// question — "should I try?" — and the breaker, not the last error, is the right answer to that.
bool redis_ready(WRedis *r) { return r && r->configured; }

str redis_error(WRedis *r) {
  if (!r) return str_lit("redis: no handle");
  if (r->err[0]) return str_lit(r->err);
  return r->configured ? str_lit("") : str_lit("redis: no server configured (set REDIS_URL)");
}

str redis_addr(WRedis *r) { return r ? str_lit(r->addr) : str_lit(""); }

void redis_pool_config(i32 max_conns, i64 acquire_ms) {
  g_pool_default = max_conns;
  g_acquire_default = acquire_ms;
}

i32 redis_pool_size(WRedis *r) {
  if (!r) return 0;
  i32 n = 0;
  wmutex_lock(&r->mu);
  for (i32 i = 0; i < WC_REDIS_POOL_MAX; i++)
    if (r->conns[i]) n++;
  wmutex_unlock(&r->mu);
  metrics_gauge_set("webc_redis_conns", (f64)n, "open redis connections");
  return n;
}

// ---------------------------------------------------------------- replies

WReplyKind reply_kind(const WReply *r) { return r ? r->kind : WR_NIL; }
bool reply_is_nil(const WReply *r) { return !r || r->kind == WR_NIL; }
bool reply_is_err(const WReply *r) { return r && r->kind == WR_ERROR; }

str reply_str(const WReply *r) {
  if (!r) return (str){0};
  if (r->kind == WR_STR || r->kind == WR_STATUS || r->kind == WR_ERROR) return r->s;
  return (str){0};
}

i64 reply_int(const WReply *r) {
  if (!r) return 0;
  switch (r->kind) {
  case WR_INT: return r->i;
  case WR_BOOL: return r->b ? 1 : 0;
  case WR_DOUBLE: return (i64)r->d;
  case WR_STR: {
    i64 v = 0;
    return parse_i64(r->s, &v) ? v : 0;
  }
  default: return 0;
  }
}

f64 reply_double(const WReply *r) {
  if (!r) return 0;
  if (r->kind == WR_DOUBLE) return r->d;
  return (f64)reply_int(r);
}

bool reply_bool(const WReply *r) {
  if (!r) return false;
  if (r->kind == WR_BOOL) return r->b;
  if (r->kind == WR_NIL) return false;
  if (r->kind == WR_STATUS) return str_eq_c(r->s, "OK");
  return reply_int(r) != 0;
}

i32 reply_len(const WReply *r) { return r ? r->n : 0; }

const WReply *reply_at(const WReply *r, i32 i) {
  if (!r || i < 0 || i >= r->n || !r->items) return nullptr;
  return &r->items[i];
}

const WReply *reply_val(const WReply *r, const char *key) {
  if (!r || r->kind != WR_MAP || !r->items) return nullptr;
  for (i32 i = 0; i < r->n; i++) {
    const WReply *k = &r->items[i * 2];
    if (k->kind == WR_STR && str_eq_c(k->s, key)) return &r->items[i * 2 + 1];
  }
  return nullptr;
}

str reply_err(const WReply *r) { return (r && r->kind == WR_ERROR) ? r->s : (str){0}; }

void reply_free(WReply *r) {
  if (!r) return;
  if (r->own) {
    arena_destroy(r->own);
    r->own = nullptr;
  }
  r->kind = WR_NIL;
  r->items = nullptr;
  r->n = 0;
}

// ---------------------------------------------------------------- the command builder
//
// A handle is spent after it runs, exactly like a WQuery: the ring is never freed, so a stale use
// is a sentence naming the command instead of a use-after-free two requests later (src/db.c
// makes the same argument for q()).

struct WCmd {
  WRedis *r;
  char verb[24];
  str argv[WC_REDIS_MAX_ARGS];
  i32 argc;
  bool spent;
};

#define WC_CMDS 64
static WCmd g_cmds[WC_CMDS];
static i32 g_cmd_at;
static _Atomic int g_cmd_lock;

static void cmd_lock(void) {
  while (atomic_exchange_explicit(&g_cmd_lock, 1, memory_order_acquire)) {
  }
}
static void cmd_unlock(void) { atomic_store_explicit(&g_cmd_lock, 0, memory_order_release); }

WCmd *cmd(WRedis *r, const char *verb) {
  cmd_lock();
  WCmd *c = &g_cmds[g_cmd_at];
  g_cmd_at = (g_cmd_at + 1) % WC_CMDS;
  cmd_unlock();
  memset(c, 0, sizeof *c);
  c->r = r;
  snprintf(c->verb, sizeof c->verb, "%s", verb ? verb : "");
  c->argv[0] = str_lit(c->verb);
  c->argc = 1;
  return c;
}

static void cmd_arg(WCmd *c, str a) {
  if (!c || c->spent || c->argc >= WC_REDIS_MAX_ARGS) return;
  c->argv[c->argc++] = a;
}

void cmd_str(WCmd *c, str a) { cmd_arg(c, a); }
void cmd_cstr(WCmd *c, const char *a) { cmd_arg(c, str_lit(a)); }
void cmd_i64(WCmd *c, i64 a) { cmd_arg(c, str_from_i64(res_arena(), a)); }
void cmd_f64(WCmd *c, f64 a) { cmd_arg(c, str_from_f64(res_arena(), a)); }
void cmd_raw(WCmd *c, const void *p, size_t n) { cmd_arg(c, (str){.p = (char *)p, .n = n}); }

WReply cmd_run(WCmd *c) {
  WReply rep = {.kind = WR_NIL};
  if (!c) return rep;
  if (c->spent) {
    // A spent handle is a bug in the caller, and a bug that reads a freed frame is worse than one
    // that says so: the reply is an error naming the command, and nothing is sent.
    log_error("redis: a command handle was used twice", {"command", str_lit(c->verb)});
    Arena *a = arena_new(256);
    rep.kind = WR_ERROR;
    rep.s = str_fmt(a, "redis: this command handle was already run (%s) — build a new one",
                    c->verb[0] ? c->verb : "?");
    rep.own = a;
    return rep;
  }
  c->spent = true;
  Arena *own = arena_new(4096);
  if (!run_argv(c->r, c->argc, c->argv, own, &rep, own)) {
    // The reply carries why — "connection refused", "no connection free after 5000 ms" — in the
    // arena reply_free() destroys.
    rep.own = own;
  }
  return rep;
}

// ---------------------------------------------------------------- verbs

static bool is_ok(WReply *rep) {
  return rep->kind == WR_STATUS || rep->kind == WR_INT || rep->kind == WR_STR ||
         rep->kind == WR_NIL || rep->kind == WR_ARRAY || rep->kind == WR_MAP;
}

static bool ok_status(WRedis *r, i32 argc, const str *argv) {
  Arena *a = arena_new(1024);
  WReply rep;
  bool ran = run_argv(r, argc, argv, a, &rep, nullptr);
  bool good = ran && is_ok(&rep);
  if (ran && rep.kind == WR_ERROR && r)
    set_err(r->err, sizeof r->err, "redis: %.*s refused: %.*s", (int)argv[0].n, argv[0].p,
            (int)rep.s.n, rep.s.p ? rep.s.p : "");
  arena_destroy(a);
  return good;
}

static WReply one(WRedis *r, i32 argc, const str *argv) {
  WReply rep = {.kind = WR_NIL};
  run_argv(r, argc, argv, res_arena(), &rep, nullptr);
  return rep;
}

str redis_get_(WRedis *r, str k) {
  str argv[2] = {str_lit("GET"), k};
  WReply rep = one(r, 2, argv);
  return rep.kind == WR_STR ? rep.s : (str){0};
}

bool redis_set_(WRedis *r, str k, str v) {
  str argv[3] = {str_lit("SET"), k, v};
  return ok_status(r, 3, argv);
}

bool redis_set_ex_(WRedis *r, str k, str v, i64 ms) {
  str argv[5] = {str_lit("SET"), k, v, str_lit("PX"), str_from_i64(res_arena(), ms)};
  return ok_status(r, 5, argv);
}

bool redis_set_nx_(WRedis *r, str k, str v, i64 ms) {
  str argv[6] = {str_lit("SET"), k, v, str_lit("NX"), str_lit("PX"), str_from_i64(res_arena(), ms)};
  WReply rep = one(r, 6, argv);
  return rep.kind == WR_STATUS || rep.kind == WR_STR;
}

str redis_get_del_(WRedis *r, str k) {
  str argv[2] = {str_lit("GETDEL"), k};
  WReply rep = one(r, 2, argv);
  return rep.kind == WR_STR ? rep.s : (str){0};
}

i64 redis_del_(WRedis *r, str k) {
  str argv[2] = {str_lit("DEL"), k};
  WReply rep = one(r, 2, argv);
  return rep.kind == WR_INT ? rep.i : 0;
}

bool redis_exists_(WRedis *r, str k) {
  str argv[2] = {str_lit("EXISTS"), k};
  WReply rep = one(r, 2, argv);
  return reply_int(&rep) > 0;
}

i64 redis_incr_(WRedis *r, str k) {
  str argv[2] = {str_lit("INCR"), k};
  WReply rep = one(r, 2, argv);
  return rep.kind == WR_INT ? rep.i : 0;
}

i64 redis_incr_by_(WRedis *r, str k, i64 by) {
  str argv[3] = {str_lit("INCRBY"), k, str_from_i64(res_arena(), by)};
  WReply rep = one(r, 3, argv);
  return rep.kind == WR_INT ? rep.i : 0;
}

bool redis_expire_(WRedis *r, str k, i64 ms) {
  str argv[3] = {str_lit("PEXPIRE"), k, str_from_i64(res_arena(), ms)};
  WReply rep = one(r, 3, argv);
  return reply_int(&rep) == 1;
}

i64 redis_ttl_(WRedis *r, str k) {
  str argv[2] = {str_lit("PTTL"), k};
  WReply rep = one(r, 2, argv);
  return rep.kind == WR_INT ? rep.i : -2;
}

str redis_hget_(WRedis *r, str map, str field) {
  str argv[3] = {str_lit("HGET"), map, field};
  WReply rep = one(r, 3, argv);
  return rep.kind == WR_STR ? rep.s : (str){0};
}

bool redis_hset_(WRedis *r, str map, str field, str v) {
  str argv[4] = {str_lit("HSET"), map, field, v};
  return ok_status(r, 4, argv);
}

bool redis_hdel_(WRedis *r, str map, str field) {
  str argv[3] = {str_lit("HDEL"), map, field};
  WReply rep = one(r, 3, argv);
  return reply_int(&rep) > 0;
}

bool redis_zadd_(WRedis *r, str key, f64 score, str member) {
  Arena *a = res_arena();
  str argv[4] = {str_lit("ZADD"), key, str_from_f64(a, score), member};
  return ok_status(r, 4, argv);
}

f64 redis_zscore_(WRedis *r, str key, str member) {
  str argv[3] = {str_lit("ZSCORE"), key, member};
  WReply rep = one(r, 3, argv);
  if (rep.kind == WR_DOUBLE) return rep.d;
  if (rep.kind == WR_STR && rep.s.n > 0) {
    // RESP2 answers a score as a bulk string, and a slice is not a C string: copy first.
    char tmp[64];
    size_t n = rep.s.n < sizeof tmp - 1 ? rep.s.n : sizeof tmp - 1;
    memcpy(tmp, rep.s.p, n);
    tmp[n] = 0;
    return strtod(tmp, nullptr);
  }
  return __builtin_nan(""); // absent: NaN rather than 0, which a real score can be
}

WReply redis_zrange_(WRedis *r, str key, i64 start, i64 stop) {
  Arena *a = res_arena();
  str argv[4] = {str_lit("ZRANGE"), key, str_from_i64(a, start), str_from_i64(a, stop)};
  return one(r, 4, argv);
}

str redis_lpop_(WRedis *r, str list) {
  str argv[2] = {str_lit("LPOP"), list};
  WReply rep = one(r, 2, argv);
  return rep.kind == WR_STR ? rep.s : (str){0};
}

bool redis_lpush_(WRedis *r, str list, str v) {
  str argv[3] = {str_lit("LPUSH"), list, v};
  return ok_status(r, 3, argv);
}

bool redis_rpush_(WRedis *r, str list, str v) {
  str argv[3] = {str_lit("RPUSH"), list, v};
  return ok_status(r, 3, argv);
}

bool redis_sadd_(WRedis *r, str set, str v) {
  str argv[3] = {str_lit("SADD"), set, v};
  return ok_status(r, 3, argv);
}

bool redis_srem_(WRedis *r, str set, str v) {
  str argv[3] = {str_lit("SREM"), set, v};
  return ok_status(r, 3, argv);
}

bool redis_sismember_(WRedis *r, str set, str v) {
  str argv[3] = {str_lit("SISMEMBER"), set, v};
  WReply rep = one(r, 3, argv);
  return reply_int(&rep) == 1;
}

i64 redis_publish_(WRedis *r, str channel, str payload) {
  str argv[3] = {str_lit("PUBLISH"), channel, payload};
  WReply rep = one(r, 3, argv);
  return reply_int(&rep);
}

bool redis_mset_(WRedis *r, i32 n, const str *kv) {
  if (n <= 0 || n * 2 + 1 > WC_REDIS_MAX_ARGS) return false;
  str argv[WC_REDIS_MAX_ARGS];
  argv[0] = str_lit("MSET");
  for (i32 i = 0; i < n * 2; i++) argv[i + 1] = kv[i];
  return ok_status(r, n * 2 + 1, argv);
}

WReply redis_mget_(WRedis *r, i32 n, const str *keys) {
  WReply rep = {.kind = WR_NIL};
  if (n <= 0 || n + 1 > WC_REDIS_MAX_ARGS) return rep;
  str argv[WC_REDIS_MAX_ARGS];
  argv[0] = str_lit("MGET");
  for (i32 i = 0; i < n; i++) argv[i + 1] = keys[i];
  run_argv(r, n + 1, argv, res_arena(), &rep, nullptr);
  return rep;
}

// One command with its own deadline: the caller's arena receives the reply, and `out` is a value
// the caller owns (no arena of its own, so nothing to free).
bool redis_call(WRedis *r, i32 argc, const str *argv, i64 timeout_ms, Arena *a, WReply *out) {
  memset(out, 0, sizeof *out);
  out->kind = WR_NIL;
  char why[512] = {0};
  RConn *c = pool_get(r, why, sizeof why);
  if (!c) {
    // Not a nil reply: the caller has to be able to tell "Redis says the key is absent" from
    // "Redis was not reachable", and only an error reply can carry the difference.
    out->kind = WR_ERROR;
    size_t n = strlen(why);
    out->s = (str){.p = arena_memdup(a, why, n), .n = n};
    return false;
  }
  Buf b;
  buf_init(&b, nullptr, 256);
  frame_cmd(&b, argc, argv);
  c->deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 1000);
  bool ok = wr_all(c, b.p, b.n);
  buf_free(&b);
  if (!ok) {
    set_err(r->err, sizeof r->err, "redis: %.200s (%s)", c->err, r->addr);
    metrics_counter_inc("webc_redis_errors_total");
    r->consec_errors++;
    pool_put(r, c, true);
    return false;
  }
  WReply *v = parse_reply(a, c, 0);
  metrics_counter_inc("webc_redis_commands_total");
  if (!v) {
    set_err(r->err, sizeof r->err, "redis: %.200s (%s)", c->err, r->addr);
    metrics_counter_inc("webc_redis_errors_total");
    r->consec_errors++;
    pool_put(r, c, true);
    return false;
  }
  *out = *v;
  if (v->kind != WR_ERROR) r->consec_errors = 0;
  pool_put(r, c, false);
  return true;
}

// ---------------------------------------------------------------- pipelines

struct WPipe {
  WRedis *r;
  RConn *conn;
  Buf b;
  i32 n;
  char why[256];
};

WPipe *pipe_new(WRedis *r) {
  WPipe *p = wm_alloc_zero(sizeof *p);
  p->r = r;
  p->conn = pool_get(r, p->why, sizeof p->why);
  buf_init(&p->b, nullptr, 512);
  return p;
}

void pipe_cmd(WPipe *p, WCmd *c) {
  if (!p || !c || c->spent) return;
  c->spent = true;
  frame_cmd(&p->b, c->argc, c->argv);
  p->n++;
}

void pipe_add_(WPipe *p, const char *verb, i32 n, const str *args) {
  if (!p || n + 1 > WC_REDIS_MAX_ARGS) return;
  str argv[WC_REDIS_MAX_ARGS];
  argv[0] = str_lit(verb);
  for (i32 i = 0; i < n; i++) argv[i + 1] = args[i];
  frame_cmd(&p->b, n + 1, argv);
  p->n++;
}

WReply *pipe_run(WPipe *p, i32 *n_out) {
  if (n_out) *n_out = 0;
  if (!p) return nullptr;
  if (!p->conn) {
    if (p->r) set_err(p->r->err, sizeof p->r->err, "%s", p->why);
    return nullptr;
  }
  Arena *a = arena_new(8192);
  p->conn->deadline = now_ms() + (p->r ? p->r->timeout_ms : WC_REDIS_DEFAULT_TIMEOUT_MS);
  if (!wr_all(p->conn, p->b.p, p->b.n)) {
    if (p->r) set_err(p->r->err, sizeof p->r->err, "redis: %.200s", p->conn->err);
    pool_put(p->r, p->conn, true);
    p->conn = nullptr;
    arena_destroy(a);
    return nullptr;
  }
  WReply *reps = arena_zalloc(a, sizeof(WReply) * (size_t)(p->n > 0 ? p->n : 1));
  for (i32 i = 0; i < p->n; i++) {
    WReply *v = parse_reply(a, p->conn, 0);
    if (!v) {
      if (p->r) set_err(p->r->err, sizeof p->r->err, "redis: %.200s", p->conn->err);
      pool_put(p->r, p->conn, true);
      p->conn = nullptr;
      arena_destroy(a);
      return nullptr;
    }
    reps[i] = *v;
  }
  if (p->n > 0) reps[0].own = a; // one arena for the batch: reply_free(&reps[0]) releases it
  pool_put(p->r, p->conn, false);
  p->conn = nullptr;
  if (n_out) *n_out = p->n;
  return reps;
}

void pipe_free(WPipe *p) {
  if (!p) return;
  if (p->conn) pool_put(p->r, p->conn, false);
  buf_free(&p->b);
  wm_free(p);
}

// ---------------------------------------------------------------- pub/sub
//
// A subscription owns a connection and a task. The connection is opened directly rather than
// borrowed from the pool: a subscribed connection cannot run ordinary commands, so keeping a pool
// slot parked on a channel would be a slot the rest of the app can never use. The task turns
// frames into callbacks; the loop reconnects with a doubling backoff and resubscribes.

struct WSub {
  WRedis *r;
  WTaskHandle *task;
  char chan[160];
  bool pattern;
  bool stop;
  WRedisMsgFn fn;
  void *arg;
  char err[256];
};

static void sub_loop(void *arg) {
  WSub *s = arg;
  i64 backoff = 100;
  while (!s->stop) {
    RConn *c = conn_open(s->r);
    if (!c) {
      if (s->stop) break;
      log_warn("redis: subscribe connect failed", {"why", str_lit(s->r->err)});
      // The loop must notice `stop` promptly even in backoff: sleep in small pieces.
      for (i64 slept = 0; slept < backoff && !s->stop; slept += 50) sleep_ms(50);
      backoff = backoff * 2 > 30000 ? 30000 : backoff * 2;
      continue;
    }
    backoff = 100;
    Buf b;
    buf_init(&b, nullptr, 128);
    str sub_argv[2] = {str_lit(s->pattern ? "PSUBSCRIBE" : "SUBSCRIBE"), str_lit(s->chan)};
    frame_cmd(&b, 2, sub_argv);
    c->deadline = now_ms() + 86400000; // a subscription waits for as long as it takes
    bool ok = wr_all(c, b.p, b.n);
    buf_free(&b);

    while (ok && !s->stop) {
      Arena *tmp = arena_new(2048);
      // A subscription is idle nearly all the time, and an idle wait has to be interruptible:
      // the wait for a *first byte* is short (so `stop` is noticed within 100 ms), but once a
      // frame has started arriving the ordinary command timeout applies, because a half frame is
      // not idleness — it is a server that went away mid-sentence.
      c->deadline = now_ms() + 100;
      if (!rd_need(c, 1)) {
        bool idle = c->len == c->at && !s->stop && strstr(c->err, "no reply within") != nullptr;
        arena_destroy(tmp);
        if (idle) continue;
        ok = false;
        break;
      }
      c->deadline = now_ms() + c->timeout_ms;
      WReply *v = parse_reply(tmp, c, 0);
      if (!v) {
        ok = false;
      } else if (v->kind == WR_ARRAY && v->n >= 3 && v->items[0].kind == WR_STR) {
        bool pm = str_eq_c(v->items[0].s, "pmessage");
        bool m = str_eq_c(v->items[0].s, "message");
        if ((m && v->n >= 3) || (pm && v->n >= 4)) {
          str chan = pm ? v->items[2].s : v->items[1].s;
          str payload = pm ? v->items[3].s : v->items[2].s;
          if (s->fn) s->fn(chan, payload, s->arg);
        }
      }
      arena_destroy(tmp);
    }
    conn_free(c);
    if (!s->stop) {
      metrics_counter_inc("webc_redis_sub_reconnects_total");
      for (i64 slept = 0; slept < backoff && !s->stop; slept += 50) sleep_ms(50);
      backoff = backoff * 2 > 30000 ? 30000 : backoff * 2;
    }
  }
}

static WSub *sub_new(WRedis *r, str chan, bool pattern, WRedisMsgFn fn, void *arg) {
  WSub *s = wm_alloc_zero(sizeof *s);
  s->r = r ? r : redis();
  s->pattern = pattern;
  s->fn = fn;
  s->arg = arg;
  size_t n = chan.n < sizeof s->chan - 1 ? chan.n : sizeof s->chan - 1;
  if (n) memcpy(s->chan, chan.p, n);
  s->chan[n] = 0;
  atomic_fetch_add(&g_sub_count, 1);
  s->task = task_result(sub_loop, s);
  return s;
}

WSub *redis_subscribe(WRedis *r, str channel, WRedisMsgFn fn, void *arg) {
  return sub_new(r, channel, false, fn, arg);
}

WSub *redis_psubscribe(WRedis *r, str pattern, WRedisMsgFn fn, void *arg) {
  return sub_new(r, pattern, true, fn, arg);
}

void redis_unsubscribe(WSub *s) {
  if (!s) return;
  s->stop = true;
  if (s->task) {
    (void)task_join(s->task); // the loop notices `stop` within one 50 ms sleep
    task_handle_free(s->task);
    s->task = nullptr;
  }
  atomic_fetch_sub(&g_sub_count, 1);
  wm_free(s);
}

i32 redis_sub_count(void) { return atomic_load(&g_sub_count); }
