// SPDX-License-Identifier: Apache-2.0
// webc — the HTTP client (SPEC §8). One request, one response, no hidden retries and
// no silent redirects; the socket waits go through wfd_wait so the calling green
// task parks instead of blocking a worker.
//
// What it does not do, on purpose: gzip, HTTP/2, cookie jars, connection pools. A
// v1 client needs to read a health endpoint and post a webhook; everything else is
// a library nobody asked for.
#include <ctype.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <webc/client.h>

#define WEBC_NO_SELECT_MACRO 1 // sys/select.h's select() vs the `select` statement
#include <webc/task.h>
#include <webc/tls.h>

static bool ieq(str a, const char *b) {
  size_t n = strlen(b);
  if (a.n != n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)a.p[i]) != tolower((unsigned char)b[i])) return false;
  return true;
}

static bool icontains(str a, const char *b) {
  size_t n = strlen(b);
  if (n == 0 || a.n < n) return false;
  for (size_t i = 0; i + n <= a.n; i++) {
    size_t k = 0;
    while (k < n && tolower((unsigned char)a.p[i + k]) == tolower((unsigned char)b[k])) k++;
    if (k == n) return true;
  }
  return false;
}

// find `needle` in `hay` at or after `from`
static i64 find_at(str hay, size_t from, str needle) {
  if (needle.n == 0 || hay.n < needle.n) return -1;
  for (size_t i = from; i + needle.n <= hay.n; i++)
    if (memcmp(hay.p + i, needle.p, needle.n) == 0) return (i64)i;
  return -1;
}

typedef struct Url {
  bool tls;
  char host[256];
  char port[16];
  str path; // includes the query string
} Url;

static bool parse_url(Arena *a, const char *url, Url *out) {
  memset(out, 0, sizeof *out);
  str s = {url, strlen(url)};
  if (str_has_prefix_c(s, "https://")) {
    out->tls = true;
    s = str_sub(s, 8, s.n);
  } else if (str_has_prefix_c(s, "http://")) {
    s = str_sub(s, 7, s.n);
  } else {
    return false;
  }
  i64 slash = str_index(s, '/');
  str authority = slash < 0 ? s : str_sub(s, 0, (size_t)slash);
  str rest = slash < 0 ? str_lit("/") : str_sub(s, (size_t)slash, s.n);
  i64 colon = str_index(authority, ':');
  str host = colon < 0 ? authority : str_sub(authority, 0, (size_t)colon);
  if (host.n == 0 || host.n >= sizeof out->host) return false;
  memcpy(out->host, host.p, host.n);
  out->host[host.n] = '\0';
  if (colon >= 0) {
    str p = str_sub(authority, (size_t)colon + 1, authority.n);
    if (p.n == 0 || p.n >= sizeof out->port) return false;
    memcpy(out->port, p.p, p.n);
    out->port[p.n] = '\0';
  } else {
    snprintf(out->port, sizeof out->port, "%s", out->tls ? "443" : "80");
  }
  char *path = arena_alloc(a, rest.n + 1);
  memcpy(path, rest.p, rest.n);
  path[rest.n] = '\0';
  out->path = (str){path, rest.n};
  return true;
}

// A TCP connection, with the green-thread wait instead of a blocking connect.
static i32 dial(const char *host, const char *port, i32 timeout_ms, char *err, size_t errcap) {
  struct addrinfo hints = {0}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  i32 rc = getaddrinfo(host, port, &hints, &res);
  if (rc != 0 || !res) {
    snprintf(err, errcap, "cannot resolve %s: %s", host, gai_strerror(rc));
    return -1;
  }
  i32 fd = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    wfd_nonblock(fd);
    i32 one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    i32 c = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (c == 0) break;
    if (!wfd_wait(fd, true, timeout_ms)) {
      wfd_close(fd);
      fd = -1;
      continue;
    }
    i32 soerr = 0;
    socklen_t len = sizeof soerr;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0) break;
    wfd_close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) snprintf(err, errcap, "cannot connect to %s:%s", host, port);
  return fd;
}

WErrRes http_do(Arena *a, WClient c, WRespC *out) {
  memset(out, 0, sizeof *out);
  if (!c.url || !c.method) return wfail(WEBC_EINVAL, str_fmt(a, "%s", "http: method and url are required"));
  i32 timeout = c.timeout_ms > 0 ? c.timeout_ms : 10000;
  Url u;
  if (!parse_url(a, c.url, &u)) {
    return wfail(WEBC_EINVAL,
                 str_fmt(a, "http: '%s' is not an http:// or https:// URL", c.url));
  }
  char err[256] = {0};
  i32 fd = dial(u.host, u.port, timeout, err, sizeof err);
  if (fd < 0) return wfail(WEBC_EIO, str_fmt(a, "%s", err));

  void *tls = nullptr;
  if (u.tls) {
    if (!tls_available()) {
      wfd_close(fd);
      return wfail(WEBC_EIO, str_fmt(a, "%s", "https: this build has no TLS (build with -DWEBC_TLS=openssl)"));
    }
    tls = tls_connect(fd, u.host, c.ca_file, c.insecure, err, sizeof err);
    if (!tls) {
      wfd_close(fd);
      return wfail(WEBC_EIO, str_fmt(a, "https: %s", err[0] ? err : "handshake failed"));
    }
  }

  // ---- request
  Buf req;
  buf_init(&req, a, 256);
  buf_printf(&req, "%s %.*s HTTP/1.1\r\n", c.method, (int)u.path.n, u.path.p);
  buf_printf(&req, "Host: %s%s\r\n", u.host,
             (u.tls && strcmp(u.port, "443") == 0) || (!u.tls && strcmp(u.port, "80") == 0) ? ""
                                                                                         : "");
  if (strcmp(u.port, u.tls ? "443" : "80") != 0) {
    // a non-default port belongs in Host, and the line above already has the host
    buf_printf(&req, "X-WebC-Port: %s\r\n", u.port);
  }
  buf_puts(&req, "Connection: close\r\n");
  buf_puts(&req, "User-Agent: webc/0.1\r\n");
  buf_puts(&req, "Accept: */*\r\n");
  if (c.body.n > 0 || c.method[0] == 'P' || c.method[0] == 'p') {
    buf_printf(&req, "Content-Length: %zu\r\n", c.body.n);
    if (c.content_type) buf_printf(&req, "Content-Type: %s\r\n", c.content_type);
  }
  for (i32 i = 0; i + 1 < c.nheaders; i += 2)
    buf_printf(&req, "%s: %s\r\n", c.headers[i], c.headers[i + 1]);
  buf_puts(&req, "\r\n");
  if (c.body.n) buf_putstr(&req, c.body);

  str head = buf_str(&req);
  size_t sent = 0;
  while (sent < head.n) {
    i64 n = tls ? tls_write(tls, head.p + sent, head.n - sent) : wfd_write(fd, head.p + sent, head.n - sent);
    if (n <= 0) {
      if (tls) tls_close(tls);
      wfd_close(fd);
      return wfail(WEBC_EIO, str_fmt(a, "%s", "http: the connection closed while sending"));
    }
    sent += (size_t)n;
  }

  // ---- response, read to EOF (Connection: close makes that the frame)
  Buf raw;
  buf_init(&raw, a, 4096);
  char chunk[8192];
  for (;;) {
    i64 n = tls ? tls_read(tls, chunk, sizeof chunk) : wfd_read(fd, chunk, sizeof chunk);
    if (n < 0) {
      if (tls) tls_close(tls);
      wfd_close(fd);
      return wfail(WEBC_EIO, str_fmt(a, "%s", "http: read failed"));
    }
    if (n == 0) break;
    buf_putstr(&raw, (str){chunk, (size_t)n});
    if (raw.n > 64u * 1024 * 1024) break; // a client-side ceiling, same reason as the server's
  }
  if (tls) tls_close(tls);
  wfd_close(fd);

  str all = buf_str(&raw);
  i64 head_end = find_at(all, 0, str_lit("\r\n\r\n"));
  if (head_end < 0) return wfail(WEBC_EPARSE, str_fmt(a, "%s", "http: no response head"));
  str headstr = str_sub(all, 0, (size_t)head_end);
  str body = str_sub(all, (size_t)head_end + 4, all.n);
  i64 sp = str_index(headstr, ' ');
  if (sp < 0) return wfail(WEBC_EPARSE, str_fmt(a, "%s", "http: malformed status line"));
  i64 code = 0;
  if (!str_to_i64(str_sub(headstr, (size_t)sp + 1, (size_t)sp + 4), &code))
    return wfail(WEBC_EPARSE, str_fmt(a, "%s", "http: malformed status code"));
  out->status = (i32)code;
  out->headers_raw = headstr;

  // header lookups, case-insensitive, first wins
  size_t line = (size_t)str_index(headstr, '\n') + 1;
  bool chunked = false;
  size_t content_length = 0;
  while (line < headstr.n) {
    // find_at returns an absolute index: a line is [line, nl), and the next one
    // starts after the CRLF. Getting this wrong reads a header name as a value.
    i64 nl = find_at(headstr, line, str_lit("\r\n"));
    str h = nl < 0 ? str_sub(headstr, line, headstr.n) : str_sub(headstr, line, (size_t)nl);
    line = nl < 0 ? headstr.n : (size_t)nl + 2;
    i64 colon = str_index(h, ':');
    if (colon <= 0) continue;
    str k = str_trim(str_sub(h, 0, (size_t)colon));
    str v = str_trim(str_sub(h, (size_t)colon + 1, h.n));
    if (ieq(k, "content-type")) out->content_type = str_dup(a, v);
    if (ieq(k, "location")) out->location = str_dup(a, v);
    if (ieq(k, "transfer-encoding") && icontains(v, "chunked")) chunked = true;
    if (ieq(k, "content-length")) {
      i64 cl = 0;
      if (str_to_i64(v, &cl) && cl > 0) content_length = (size_t)cl;
    }
  }

  if (chunked) {
    Buf decoded;
    buf_init(&decoded, a, body.n);
    size_t at = 0;
    while (at < body.n) {
      i64 nl = find_at(body, at, str_lit("\r\n"));
      if (nl < 0) break;
      str hexline = str_sub(body, at, (size_t)nl);
      i64 size = (i64)strtoull(hexline.p, nullptr, 16);
      if (size <= 0) break;
      at = (size_t)nl + 2;
      if (at + (size_t)size > body.n) break;
      buf_putstr(&decoded, (str){body.p + at, (size_t)size});
      at += (size_t)size + 2;
    }
    body = buf_str(&decoded);
  } else if (content_length > 0 && content_length < body.n) {
    body = str_sub(body, 0, content_length);
  }
  out->body = body;
  return wok();
}

WErrRes http_get(Arena *a, const char *url, WRespC *out) {
  return http_do(a, (WClient){.method = "GET", .url = url}, out);
}

WErrRes http_post(Arena *a, const char *url, const char *ctype, str body, WRespC *out) {
  return http_do(a, (WClient){.method = "POST", .url = url, .body = body, .content_type = ctype},
                 out);
}

WErrRes http_do_follow(Arena *a, WClient c, i32 max_hops, WRespC *out) {
  char url[1024];
  snprintf(url, sizeof url, "%s", c.url);
  for (i32 hop = 0;; hop++) {
    c.url = url;
    WErrRes e = http_do(a, c, out);
    if (!e.ok) return e;
    if (out->status < 300 || out->status >= 400 || out->location.n == 0) return e;
    if (hop >= max_hops) return wfail(WEBC_EIO, str_fmt(a, "%s", "http: too many redirects"));
    // A relative Location is legal and common; resolve it against the current URL.
    if (out->location.p[0] == '/') {
      Url u;
      if (!parse_url(a, url, &u)) return wfail(WEBC_EINVAL, str_fmt(a, "%s", "http: bad URL"));
      snprintf(url, sizeof url, "%s://%s:%s%.*s", u.tls ? "https" : "http", u.host, u.port,
               (int)out->location.n, out->location.p);
    } else if (str_has_prefix_c(out->location, "http")) {
      if (out->location.n >= sizeof url) return wfail(WEBC_EINVAL, str_fmt(a, "%s", "http: redirect too long"));
      snprintf(url, sizeof url, "%.*s", (int)out->location.n, out->location.p);
    } else {
      return wfail(WEBC_EINVAL, str_fmt(a, "%s", "http: cannot resolve relative redirect"));
    }
  }
}
