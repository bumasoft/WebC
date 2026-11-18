// SPDX-License-Identifier: Apache-2.0
// WebC — postgres wire protocol driver (v3, text format, SCRAM-SHA-256 auth).
//
// There is no libpq here: the driver speaks the protocol itself, which is why
// crypto.c exists (SPEC §6.5).
// `select` is a statement macro in <webc/task.h>, and this file includes <unistd.h>.
#define WEBC_NO_SELECT_MACRO 1
#include "db_internal.h"
#include <webc/ctx.h>
#include <webc/crypto.h>
#include <webc/log.h>
#include <webc/task.h>
#include <webc/tls.h>

#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#define PG_PLANS 64 // distinct statements cached per connection (this app uses ~12)

typedef struct PgConn {
  i32 fd;
  void *tls; // a tls_connect() session when sslmode asked for one
  char host[256];
  char port[16];
  char user[128];
  char pass[256];
  char dbname[128];
  char err[512];
  bool broken;
  bool cancelled;      // a CancelRequest was sent: this connection is not reusable
  bool have_key;
  u32 backend_pid, backend_key; // from BackendKeyData ('K'), for the CancelRequest
  WCtx *ctx;           // the request's context while a statement runs
  i64 timeout_ms;
  // Prepared plans this connection has sent. Postgres plans a statement on Parse and keeps the
  // plan under its name, so a page that runs the same eight statements again should send Bind
  // only — pgbench on this server: 0.094 ms of CPU per query unnamed, 0.064 ms prepared, and 5
  // frames instead of 2. Bounded and never evicted: past the cap a statement falls back to the
  // unnamed path, so the server cannot accumulate plans nobody will reuse.
  struct {
    u64 hash;
    u32 name;
  } plans[PG_PLANS];
  i32 nplans;
  u32 plan_seq;
} PgConn;

// ---------------------------------------------------------------- buffer

typedef struct Msg {
  Buf b;
} Msg;

static void msg_begin(Msg *m, Arena *a, char type) {
  buf_init(&m->b, a, 256);
  buf_putc(&m->b, type);
  buf_write(&m->b, "\0\0\0\0", 4); // length patched below
}

static void msg_end(Msg *m) {
  u32 len = (u32)(m->b.n - 1);
  m->b.p[1] = (char)(len >> 24);
  m->b.p[2] = (char)(len >> 16);
  m->b.p[3] = (char)(len >> 8);
  m->b.p[4] = (char)(len);
}

static void msg_cstr(Msg *m, const char *s) {
  buf_write(&m->b, s, strlen(s) + 1);
}

static void msg_i16(Msg *m, u16 v) {
  char b[2] = {(char)(v >> 8), (char)v};
  buf_write(&m->b, b, 2);
}

static void msg_i32(Msg *m, i32 v) {
  char b[4] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
  buf_write(&m->b, b, 4);
}

// ---------------------------------------------------------------- wire
//
// Every byte of this driver is *parked* I/O, not blocking I/O: the socket is non-blocking
// and wfd_read()/wfd_write() hand the worker thread to another task while the database
// thinks. That is the difference between "ten workers, ten queries in flight" and "ten
// workers, a thousand queries in flight" — the event loop a Node backend gets from libuv.
static i64 pg_raw_write(i32 fd, const void *buf, size_t n) { return wfd_write(fd, buf, n); }

// A statement can outlive the request that asked for it: a client that closes its connection,
// an `mw_timeout_ms()` that has already fired, a server that is draining. The way to stop the
// *server* too is the protocol's CancelRequest — a separate connection that names the backend
// (pid + secret from BackendKeyData) and asks it to abort. After one, this connection is
// finished: the cancellation and the in-flight query race on the wire, so it is closed rather
// than reused (db.c recycles the slot).
static void pg_cancel(PgConn *pc) {
  if (!pc || pc->cancelled || !pc->have_key || pc->fd < 0) return;
  pc->cancelled = true;
  snprintf(pc->err, sizeof pc->err, "statement cancelled: %s",
           pc->ctx && ctx_err(pc->ctx).n ? ctx_err(pc->ctx).p : "the request ended");
  i32 fd = wfd_open_connect(pc->host, (u16)atoi(pc->port), 2000);
  if (fd < 0) return;
  char req[16];
  req[0] = 0;
  req[1] = 0;
  req[2] = 0;
  req[3] = 16;
  req[4] = 0x04; // 80877102
  req[5] = (char)0xd2;
  req[6] = 0x16;
  req[7] = 0x2e;
  for (i32 i = 0; i < 4; i++) {
    req[8 + i] = (char)((pc->backend_pid >> (24 - 8 * i)) & 0xff);
    req[12 + i] = (char)((pc->backend_key >> (24 - 8 * i)) & 0xff);
  }
  (void)!pg_raw_write(fd, req, sizeof req);
  wfd_close(fd);
}

static i64 pg_read(PgConn *pc, void *buf, size_t n) {
  if (pc->tls) return tls_read(pc->tls, buf, n);
  for (;;) {
    ssize_t r = read(pc->fd, buf, n);
    if (r >= 0) return r;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    if (pc->ctx && ctx_done(pc->ctx)) {
      pg_cancel(pc);
      return -1;
    }
    // A bounded park, not a 30-second one: this is the granularity at which a cancelled
    // request actually stops waiting on its database (a few hundred milliseconds).
    if (!wfd_wait(pc->fd, false, 200) && pc->ctx && ctx_done(pc->ctx)) {
      pg_cancel(pc);
      return -1;
    }
  }
}

static i64 pg_write(PgConn *pc, const void *buf, size_t n) {
  if (pc->tls) return tls_write(pc->tls, buf, n);
  return pg_raw_write(pc->fd, buf, n);
}

static bool send_all(PgConn *c, const void *p, size_t n) {
  const char *at = p;
  while (n) {
    i64 w = pg_write(c, at, n);
    if (w <= 0) return false;
    at += w;
    n -= (size_t)w;
  }
  return true;
}

static bool msg_send(PgConn *c, Msg *m) {
  msg_end(m);
  return send_all(c, m->b.p, m->b.n);
}

static bool read_exact(PgConn *c, void *p, size_t n) {
  char *at = p;
  while (n) {
    i64 r = pg_read(c, at, n);
    if (r <= 0) return false;
    at += r;
    n -= (size_t)r;
  }
  return true;
}

static bool read_msg(PgConn *c, Arena *a, char *type, str *payload) {
  char t;
  if (!read_exact(c, &t, 1)) return false;
  u8 lenb[4];
  if (!read_exact(c, lenb, 4)) return false;
  u32 len = ((u32)lenb[0] << 24) | ((u32)lenb[1] << 16) | ((u32)lenb[2] << 8) | lenb[3];
  if (len < 4 || len > 64u * 1024 * 1024) return false;
  size_t n = len - 4;
  char *p = arena_alloc(a, n + 1);
  if (n && !read_exact(c, p, n)) return false;
  p[n] = 0;
  *type = t;
  *payload = (str){p, n};
  return true;
}

// ---------------------------------------------------------------- auth

// postgres' SQLSTATE is in the error packet, and it is the only stable reading of a
// failure: `23505` is a unique violation in any language the message happens to be in.
// The class (first two characters) plus the specific code is what an app can act on.
static WDbErrKind pg_class(const char *code) {
  if (!code || strlen(code) < 5) return WDBE_NONE;
  if (strcmp(code, "23505") == 0) return WDBE_CONFLICT;
  if (strcmp(code, "23503") == 0) return WDBE_BAD_REFERENCE;
  if (strcmp(code, "23502") == 0) return WDBE_NOT_NULL;
  if (strcmp(code, "23514") == 0) return WDBE_CHECK;
  if (strcmp(code, "22001") == 0) return WDBE_TOO_LONG;
  if (strcmp(code, "40001") == 0 || strcmp(code, "40P01") == 0) return WDBE_DEADLOCK;
  if (strcmp(code, "55P03") == 0 || strcmp(code, "57014") == 0) return WDBE_TIMEOUT;
  if (strncmp(code, "28", 2) == 0) return WDBE_AUTH;
  if (strncmp(code, "08", 2) == 0 || strncmp(code, "57P", 3) == 0) return WDBE_LOST;
  if (strncmp(code, "23", 2) == 0) return WDBE_CONFLICT; // integrity_constraint_violation
  return WDBE_NONE;
}

static WDbErrKind pg_last_class; // the last ErrorResponse's class, read by pg_exec()

static void pg_error_from(PgConn *c, str payload) {
  // ErrorResponse is a list of (field code byte, cstring) pairs ending in a zero
  // byte. `M` is the human message and `C` the SQLSTATE; `S` is only the severity
  // ("ERROR"), which says nothing a person can act on.
  size_t i = 0;
  c->err[0] = 0;
  char severity[16] = "";
  char code[8] = ""; // SQLSTATE, the part you can switch on
  while (i < payload.n && payload.p[i]) {
    char field = payload.p[i++];
    size_t start = i;
    while (i < payload.n && payload.p[i])
      i++;
    str value = str_sub(payload, start, i);
    i++; // the zero terminator
    if (field == 'M')
      snprintf(c->err, sizeof c->err, "%.*s", (int)value.n, value.p);
    else if (field == 'S')
      snprintf(severity, sizeof severity, "%.*s", (int)value.n, value.p);
    else if (field == 'C')
      snprintf(code, sizeof code, "%.*s", (int)value.n, value.p);
    if (field == 'M') break; // keep scanning only for classification below
  }
  pg_last_class = pg_class(code);
  if (c->err[0] && code[0])
    snprintf(c->err + strlen(c->err), sizeof c->err - strlen(c->err), " (SQLSTATE %s)", code);
  else if (!c->err[0])
    snprintf(c->err, sizeof c->err, "%s", severity[0] ? severity : "postgres error");
}

static bool scram_auth(PgConn *c, Arena *a, str mechanisms) {
  if (!str_contains(mechanisms, str_lit("SCRAM-SHA-256"))) {
    snprintf(c->err, sizeof c->err, "server offers no supported SASL mechanism");
    return false;
  }
  str nonce = crypto_random_hex(a, 18);
  str client_first_bare = str_fmt(a, "n=,r=%.*s", (int)nonce.n, nonce.p);
  str client_first = str_fmt(a, "n,,%.*s", (int)client_first_bare.n, client_first_bare.p);

  Msg m;
  msg_begin(&m, a, 'p');
  msg_cstr(&m, "SCRAM-SHA-256");
  msg_i32(&m, (i32)client_first.n);
  buf_write(&m.b, client_first.p, client_first.n);
  if (!msg_send(c, &m)) return false;

  char type;
  str payload;
  if (!read_msg(c, a, &type, &payload)) return false;
  if (type == 'E') {
    pg_error_from(c, payload);
    return false;
  }
  if (type != 'R') {
    snprintf(c->err, sizeof c->err, "unexpected auth response");
    return false;
  }
  u32 code = ((u8)payload.p[0] << 24) | ((u8)payload.p[1] << 16) | ((u8)payload.p[2] << 8) |
             (u8)payload.p[3];
  if (code != 11) {
    snprintf(c->err, sizeof c->err, "expected SASL continue");
    return false;
  }
  str server_first = str_sub(payload, 4, payload.n);

  // r=<nonce>,s=<salt>,i=<iters>
  str s_nonce = {0}, s_salt = {0}, s_iter = {0};
  size_t i = 0;
  while (i < server_first.n) {
    size_t j = i;
    while (j < server_first.n && server_first.p[j] != ',')
      j++;
    str kv = str_sub(server_first, i, j);
    if (kv.n > 2) {
      str key = str_sub(kv, 0, 1);
      str val = str_sub(kv, 2, kv.n);
      if (str_eq_c(key, "r"))
        s_nonce = val;
      else if (str_eq_c(key, "s"))
        s_salt = val;
      else if (str_eq_c(key, "i"))
        s_iter = val;
    }
    i = j + 1;
  }
  if (!s_nonce.p || !s_salt.p || !s_iter.p) {
    snprintf(c->err, sizeof c->err, "malformed SCRAM challenge");
    return false;
  }
  i64 iters = 4096;
  str_to_i64(s_iter, &iters);
  str salt_b64 = {0};
  base64_decode(a, s_salt, &salt_b64);

  u8 salted[32];
  pbkdf2_sha256(c->pass, strlen(c->pass), salt_b64.p, salt_b64.n, (u32)iters, salted, 32);

  u8 client_key[32], stored_key[32];
  hmac_sha256(salted, 32, "Client Key", 10, client_key);
  sha256(client_key, 32, stored_key);

  str client_final_wo_proof = str_fmt(a, "c=biws,r=%.*s", (int)s_nonce.n, s_nonce.p);
  str auth_message = str_fmt(a, "%.*s,%.*s,%.*s", (int)client_first_bare.n, client_first_bare.p,
                             (int)server_first.n, server_first.p, (int)client_final_wo_proof.n,
                             client_final_wo_proof.p);
  u8 client_sig[32];
  hmac_sha256(stored_key, 32, auth_message.p, auth_message.n, client_sig);
  u8 proof[32];
  for (i32 k = 0; k < 32; k++)
    proof[k] = client_key[k] ^ client_sig[k];
  str proof_b64 = {0};
  base64_encode(a, (str){(char *)proof, 32}, &proof_b64);
  str client_final = str_fmt(a, "%.*s,p=%.*s", (int)client_final_wo_proof.n,
                             client_final_wo_proof.p, (int)proof_b64.n, proof_b64.p);

  Msg m2;
  msg_begin(&m2, a, 'p');
  buf_write(&m2.b, client_final.p, client_final.n);
  if (!msg_send(c, &m2)) return false;

  if (!read_msg(c, a, &type, &payload)) return false;
  if (type == 'E') {
    pg_error_from(c, payload);
    return false;
  }
  if (type != 'R') {
    snprintf(c->err, sizeof c->err, "unexpected SASL final");
    return false;
  }
  u32 code2 = ((u8)payload.p[0] << 24) | ((u8)payload.p[1] << 16) | ((u8)payload.p[2] << 8) |
              (u8)payload.p[3];
  if (code2 != 12) {
    snprintf(c->err, sizeof c->err, "SASL verification failed");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- sslmode
//
// A managed database (RDS, Cloud SQL, Neon, Supabase, Crunchy) refuses a plaintext client,
// so `DATABASE_URL=postgres://…?sslmode=require` has to mean something. The values are
// libpq's, and so are their meanings.
typedef enum PgSsl : u8 { PG_SSL_DISABLE, PG_SSL_PREFER, PG_SSL_REQUIRE, PG_SSL_VERIFY_CA, PG_SSL_VERIFY_FULL } PgSsl;

typedef struct PgTls {
  PgSsl mode;
  char ca_file[512]; // ?sslrootcert=…
} PgTls;

static PgSsl pg_sslmode_parse(str v) {
  if (str_eq_c(v, "disable")) return PG_SSL_DISABLE;
  if (str_eq_c(v, "allow")) return PG_SSL_PREFER;
  if (str_eq_c(v, "prefer")) return PG_SSL_PREFER;
  if (str_eq_c(v, "require")) return PG_SSL_REQUIRE;
  if (str_eq_c(v, "verify-ca")) return PG_SSL_VERIFY_CA;
  if (str_eq_c(v, "verify-full")) return PG_SSL_VERIFY_FULL;
  return PG_SSL_PREFER;
}

// The query string after the database name: `?sslmode=require&sslrootcert=/etc/ca.pem`.
static void pg_read_query(str q, PgTls *tls, PgSsl *mode, bool *mode_set) {
  str key, val;
  for (size_t i = 0; wc_kv_next(q, &i, &key, &val);) {
    if (str_eq_c(key, "sslmode")) {
      *mode = pg_sslmode_parse(val);
      *mode_set = true;
    } else if (str_eq_c(key, "sslrootcert")) {
      snprintf(tls->ca_file, sizeof tls->ca_file, "%.*s", (int)val.n, val.p);
    }
  }
}

// The SSLRequest handshake: an 8-byte packet, one byte back. 'S' means the server is
// willing to speak TLS on this socket, 'N' means it is not and the conversation continues
// in plaintext.
static bool pg_ssl_request(PgConn *pc, bool *accepted) {
  char req[8] = {0, 0, 0, 8, 0x04, (char)0xd2, (char)0x16, 0x2f}; // 80877103
  if (send_all(pc, req, sizeof req) != true) return false;
  char one = 0;
  if (pg_read(pc, &one, 1) != 1) return false;
  *accepted = one == 'S';
  if (!*accepted && one != 'N') return false;
  return true;
}

static WConn *pg_open(const char *url, Arena *a, WErrRes *err) {
  PgConn *pc = arena_zalloc(a, sizeof *pc);
  snprintf(pc->host, sizeof pc->host, "127.0.0.1");
  snprintf(pc->port, sizeof pc->port, "5432");
  snprintf(pc->user, sizeof pc->user, "postgres");
  snprintf(pc->dbname, sizeof pc->dbname, "postgres");

  // user[:pass]@host[:port]/db, split once for every driver (db_url.c).
  WDbUrl u;
  wc_db_url_split(url, &u);
  wc_url_copy(pc->user, sizeof pc->user, u.user);
  wc_url_copy(pc->pass, sizeof pc->pass, u.pass);
  wc_url_copy(pc->host, sizeof pc->host, u.host);
  wc_url_copy(pc->port, sizeof pc->port, u.port);
  wc_url_copy(pc->dbname, sizeof pc->dbname, u.db);

  PgTls tls = {.mode = PG_SSL_PREFER};
  bool mode_set = false;
  pg_read_query(u.query, &tls, &tls.mode, &mode_set);
  // `WEBC_PG_SSLMODE` wins over the URL, so a deployment can turn TLS on without
  // rewriting the connection string it was handed.
  const char *env_ssl = getenv("WEBC_PG_SSLMODE");
  if (env_ssl && *env_ssl) {
    tls.mode = pg_sslmode_parse(str_lit(env_ssl));
    mode_set = true;
  }
  // `prefer` is libpq's default: try TLS, fall back to plaintext. A build without TLS can
  // still honour it (it just cannot try); `require` and the verify-* modes cannot, and say so
  // at startup instead of sending a password in the clear to a server that asked for TLS.
  if (tls.mode == PG_SSL_PREFER && !tls_available()) tls.mode = PG_SSL_DISABLE;
  if (tls.mode != PG_SSL_DISABLE && !tls_available()) {
    *err = wfail(WEBC_EDB,
                 str_fmt(nullptr,
                         "postgres: sslmode=%s needs a TLS build — rebuild with "
                         "-DWEBC_TLS=openssl, or set sslmode=disable explicitly",
                         mode_set ? (tls.mode == PG_SSL_REQUIRE ? "require"
                                     : tls.mode == PG_SSL_VERIFY_CA ? "verify-ca"
                                     : tls.mode == PG_SSL_VERIFY_FULL ? "verify-full"
                                                                     : "prefer")
                                  : "prefer"));
    return nullptr;
  }

  i32 fd = wfd_open_connect(pc->host, (u16)atoi(pc->port), 10000);
  if (fd < 0) {
    *err =
        wfail(WEBC_EDB, str_fmt(nullptr, "postgres: cannot connect to %s:%s", pc->host, pc->port));
    return nullptr;
  }
  pc->fd = fd;
  pc->timeout_ms = 30000;

  if (tls.mode != PG_SSL_DISABLE) {
    bool accepted = false;
    if (!pg_ssl_request(pc, &accepted)) {
      if (getenv("WEBC_DB_DEBUG")) fprintf(stderr, "[pg] no TLS answer\n");
      *err = wfail(WEBC_EDB, str_fmt(nullptr, "postgres: no answer to the TLS request from %s:%s",
                                     pc->host, pc->port));
      close(fd);
      return nullptr;
    }
    if (!accepted) {
      if (tls.mode != PG_SSL_PREFER) {
        *err = wfail(WEBC_EDB,
                     str_fmt(nullptr,
                             "postgres: %s:%s does not accept TLS and sslmode asks for it",
                             pc->host, pc->port));
        close(fd);
        return nullptr;
      }
      log_warn("postgres: server refused TLS, continuing in plaintext (sslmode=prefer)",
               {"host", str_lit(pc->host)});
    } else {
      char tls_err[256] = "";
      // libpq verifies the certificate only in verify-ca and verify-full. `require` and
      // `prefer` encrypt without checking who is on the other end — that is the contract
      // `require` names, and pretending otherwise makes it fail on every managed server
      // whose CA is not in this machine's store.
      bool insecure = tls.mode != PG_SSL_VERIFY_CA && tls.mode != PG_SSL_VERIFY_FULL;
      bool verify_host = tls.mode == PG_SSL_VERIFY_FULL;
      pc->tls = tls_connect_opt(fd, pc->host, tls.ca_file[0] ? tls.ca_file : nullptr,
                                verify_host, insecure, tls_err, sizeof tls_err);
      if (!pc->tls && tls.mode == PG_SSL_PREFER) {
        // `prefer` means "TLS if it works, plaintext if it does not": libpq reconnects
        // rather than give up, and so does this.
        log_warn("postgres: TLS handshake failed, continuing in plaintext (sslmode=prefer)",
                 {"host", str_lit(pc->host)}, {"why", str_lit(tls_err)});
        wfd_close(fd);
        fd = wfd_open_connect(pc->host, (u16)atoi(pc->port), 10000);
        if (fd < 0) {
          *err = wfail(WEBC_EDB,
                       str_fmt(nullptr, "postgres: cannot connect to %s:%s", pc->host, pc->port));
          return nullptr;
        }
        pc->fd = fd;
      } else if (!pc->tls) {
        *err = wfail(WEBC_EDB, str_fmt(nullptr, "postgres: TLS handshake with %s:%s failed: %s",
                                       pc->host, pc->port, tls_err));
        close(fd);
        return nullptr;
      }
    }
  }

  // StartupMessage (no type byte)
  Msg m;
  buf_init(&m.b, a, 256);
  buf_write(&m.b, "\0\0\0\0", 4);
  msg_i32(&m, 196608); // 3.0
  msg_cstr(&m, "user");
  msg_cstr(&m, pc->user);
  msg_cstr(&m, "database");
  msg_cstr(&m, pc->dbname);
  msg_cstr(&m, "application_name");
  msg_cstr(&m, "webc");
  msg_cstr(&m, "client_encoding");
  msg_cstr(&m, "UTF8");
  buf_putc(&m.b, 0);
  u32 len = (u32)m.b.n;
  m.b.p[0] = (char)(len >> 24);
  m.b.p[1] = (char)(len >> 16);
  m.b.p[2] = (char)(len >> 8);
  m.b.p[3] = (char)len;
  // A StartupMessage has no type byte, so its length is patched above and it is written as
  // is — msg_send() would prepend-shift it and the server would see a malformed packet.
  if (!send_all(pc, m.b.p, m.b.n)) {
    *err = wfail(WEBC_EDB, str_lit("postgres: startup write failed"));
    return nullptr;
  }

  for (;;) {
    char type;
    str payload;
    if (!read_msg(pc, a, &type, &payload)) {
      *err = wfail(WEBC_EDB, str_lit("postgres: connection closed during startup"));
      if (getenv("WEBC_DB_DEBUG")) fprintf(stderr, "[pg] closed during startup\n");
      return nullptr;
    }
    if (type == 'E') {
      pg_error_from(pc, payload);
      *err = wfail(WEBC_EDB, str_lit(pc->err));
      return nullptr;
    }
    if (type == 'R') {
      u32 code = ((u8)payload.p[0] << 24) | ((u8)payload.p[1] << 16) | ((u8)payload.p[2] << 8) |
                 (u8)payload.p[3];
      if (code == 0) continue;
      if (code == 3) {
        Msg pw;
        msg_begin(&pw, a, 'p');
        buf_write(&pw.b, pc->pass, strlen(pc->pass) + 1);
        if (!msg_send(pc, &pw)) return nullptr;
        continue;
      }
      if (code == 10) {
        if (!scram_auth(pc, a, str_sub(payload, 4, payload.n))) {
          *err = wfail(WEBC_EDB, str_lit(pc->err));
          return nullptr;
        }
        continue;
      }
      if (code == 5) {
        snprintf(pc->err, sizeof pc->err, "postgres: md5 auth is not supported; use scram-sha-256");
        *err = wfail(WEBC_EDB, str_lit(pc->err));
        return nullptr;
      }
      snprintf(pc->err, sizeof pc->err, "postgres: unsupported auth method %u", code);
      *err = wfail(WEBC_EDB, str_lit(pc->err));
      return nullptr;
    }
    if (type == 'K') { // BackendKeyData: what a CancelRequest has to name
      if (payload.n >= 8) {
        pc->backend_pid = ((u32)(u8)payload.p[0] << 24) | ((u32)(u8)payload.p[1] << 16) |
                          ((u32)(u8)payload.p[2] << 8) | (u32)(u8)payload.p[3];
        pc->backend_key = ((u32)(u8)payload.p[4] << 24) | ((u32)(u8)payload.p[5] << 16) |
                          ((u32)(u8)payload.p[6] << 8) | (u32)(u8)payload.p[7];
        pc->have_key = true;
      }
      continue;
    }
    if (type == 'Z') break; // ReadyForQuery
  }

  WConn *c = arena_zalloc(a, sizeof *c);
  c->drv = &wc_driver_pg;
  c->h = pc;
  return c;
}

static void pg_close(WConn *c) {
  if (!c || !c->h) return;
  PgConn *pc = c->h;
  if (pc->fd > 0) {
    Arena *a = arena_new(256);
    Msg m;
    msg_begin(&m, a, 'X');
    msg_send(pc, &m);
    arena_destroy(a);
    if (pc->tls) {
      tls_close(pc->tls);
      pc->tls = nullptr;
    }
    wfd_close(pc->fd);
    pc->fd = -1;
  }
}

// ---------------------------------------------------------------- exec

// The column's type, from RowDescription. Guessing from the text was wrong in a way that
// mattered: `select 1::text` (and any text column holding "1", "t" or "2.5") came back as an
// integer or a bool, so `row_str()` answered "" for a value the database had sent as text.
#define PG_OID_BOOL 16
#define PG_OID_BYTEA 17
#define PG_OID_INT8 20
#define PG_OID_INT2 21
#define PG_OID_INT4 23
#define PG_OID_TEXT 25
#define PG_OID_OID 26
#define PG_OID_FLOAT4 700
#define PG_OID_FLOAT8 701
#define PG_OID_NUMERIC 1700

static bool pg_oid_numeric(i32 oid) {
  return oid == PG_OID_INT2 || oid == PG_OID_INT4 || oid == PG_OID_INT8 || oid == PG_OID_OID ||
         oid == PG_OID_FLOAT4 || oid == PG_OID_FLOAT8 || oid == PG_OID_NUMERIC;
}

static WVal parse_pg_value(Arena *a, str s, i32 oid) {
  if (oid == PG_OID_BOOL) return (WVal){WV_BOOL, .b = s.n > 0 && s.p[0] == 't'};
  if (oid == PG_OID_BYTEA) {
    char *copy = arena_alloc(a, s.n + 1);
    memcpy(copy, s.p, s.n);
    copy[s.n] = 0;
    return (WVal){WV_BLOB, .s = (str){copy, s.n}};
  }
  if (oid != 0 && !pg_oid_numeric(oid)) { // text, varchar, uuid, json, date, an enum, …
    char *copy = arena_alloc(a, s.n + 1);
    memcpy(copy, s.p, s.n);
    copy[s.n] = 0;
    return (WVal){WV_TEXT, .s = (str){copy, s.n}};
  }
  bool numeric = s.n > 0;
  for (size_t i = 0; i < s.n; i++) {
    char ch = s.p[i];
    if (!((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' || ch == 'e' ||
          ch == 'E')) {
      numeric = false;
      break;
    }
  }
  if (numeric && !str_contains(s, str_lit(".")) && !str_contains(s, str_lit("e"))) {
    i64 v = 0;
    if (str_to_i64(s, &v)) return (WVal){WV_I64, .i = v};
  }
  if (numeric) {
    f64 f = 0;
    if (str_to_f64(s, &f)) return (WVal){WV_F64, .f = f};
  }
  char *copy = arena_alloc(a, s.n + 1);
  memcpy(copy, s.p, s.n);
  copy[s.n] = 0;
  return (WVal){WV_TEXT, .s = (str){copy, s.n}};
}

static u64 pg_hash_sql(str sql) {
  u64 h = 1469598103934665603ull; // FNV-1a
  for (size_t i = 0; i < sql.n; i++) {
    h ^= (u8)sql.p[i];
    h *= 1099511628211ull;
  }
  return h ? h : 1;
}

static i32 pg_plan_find(PgConn *pc, u64 h) {
  for (i32 i = 0; i < pc->nplans; i++)
    if (pc->plans[i].hash == h) return i;
  return -1;
}

static WRes pg_exec(WConn *c, WQuery *qy, Arena *a) {
  WRes r = {.ok = true};
  PgConn *pc = c->h;
  pc->ctx = c->ctx;
  if (pc->broken || pc->cancelled) {
    r.ok = false;
    r.e = werr(WEBC_EDB, str_lit(pc->err));
    return r;
  }

  Msg m;
  // Plan-cache state for this statement, which the response loop below also needs: `pi` is the
  // cached plan that was reused (−1 if none), `newplan` the slot a successful Parse claims.
  u64 h = 0;
  i32 pi = -1;
  i32 newplan = -1;
  if (qy->nparams == 0) {
    // No parameters: the *simple* protocol, which is the one that accepts several statements in
    // one call. A migration file is several statements, and the extended protocol answers
    // "cannot insert multiple commands into a prepared statement" — which is what made a project's
    // hand-written `0001_init.postgres.sql` unapplyable while its sqlite twin ran the whole file.
    // Nothing is lost: the result format is text either way (see the Bind below), and the last
    // result set is the one this driver has always kept.
    msg_begin(&m, a, 'Q');
    buf_write(&m.b, qy->sql.p, qy->sql.n);
    buf_putc(&m.b, 0);
    msg_send(pc, &m);
  } else {
  // Parse / Bind / Describe / Execute / Sync — or, for a statement this connection has already
  // parsed, Bind / Describe / Execute / Sync under its name. Sending every statement unnamed is
  // what made Postgres re-parse and re-plan all eight statements of a page on every request.
  h = pg_hash_sql(qy->sql);
  pi = pg_plan_find(pc, h);
  char sname[24] = "";
  if (pi >= 0) {
    snprintf(sname, sizeof sname, "wc%u", pc->plans[pi].name);
  } else if (pc->nplans < PG_PLANS) {
    // name it: this is the Parse that the next request will not repeat
    newplan = pc->nplans; // provisional; committed only if the statement succeeds
    snprintf(sname, sizeof sname, "wc%u", pc->plan_seq + 1);
    msg_begin(&m, a, 'P');
    msg_cstr(&m, sname);
    buf_write(&m.b, qy->sql.p, qy->sql.n);
    buf_putc(&m.b, 0);
    msg_i16(&m, 0); // no parameter type hints
    msg_send(pc, &m);
  }

  msg_begin(&m, a, 'B');
  msg_cstr(&m, "");     // portal
  msg_cstr(&m, sname);  // the statement: named when cached, unnamed when the cache is full
  msg_i16(&m, 0);   // all params in text format
  msg_i16(&m, (u16)qy->nparams);
  for (i32 i = 0; i < qy->nparams; i++) {
    WVal v = qy->params[i];
    str text = {0};
    switch (v.kind) {
    case WV_NULL:
      msg_i32(&m, -1);
      continue;
    case WV_I64:
      text = str_fmt(a, "%lld", (long long)v.i);
      break;
    case WV_F64:
      text = str_fmt(a, "%g", v.f);
      break;
    case WV_BOOL:
      text = str_lit(v.b ? "true" : "false");
      break;
    case WV_TEXT:
      text = v.s;
      break;
    case WV_BLOB:
      text = v.s;
      break;
    }
    msg_i32(&m, (i32)text.n);
    buf_write(&m.b, text.p, text.n);
  }
  msg_i16(&m, 0); // result format: text
  msg_send(pc, &m);

  msg_begin(&m, a, 'D');
  buf_putc(&m.b, 'P');
  msg_cstr(&m, "");
  msg_send(pc, &m);

  msg_begin(&m, a, 'E');
  msg_cstr(&m, "");
  msg_i32(&m, 0);
  msg_send(pc, &m);

  msg_begin(&m, a, 'S');
  msg_send(pc, &m);
  }

  str *col_names = nullptr;
  i32 *col_oids = nullptr; // type oid per column, from RowDescription
  i32 ncols = 0;
  WRow *rows = nullptr;
  i32 cap_rows = 0;
  const char *tag = nullptr;

  for (;;) {
    char type;
    str payload;
    if (!read_msg(pc, a, &type, &payload)) {
      if (pc->cancelled) {
        // the statement was interrupted on purpose: the message already says why
        r.ok = false;
        r.e = werr(WEBC_ECANCELED, str_lit(pc->err));
        return r;
      }
      pc->broken = true;
      snprintf(pc->err, sizeof pc->err, "postgres: connection lost");
      r.ok = false;
      r.e = werr(WEBC_EDB, str_lit(pc->err));
      return r;
    }
    switch (type) {
    case 'T': { // RowDescription
      // A new result set: the rows before it belong to an earlier statement of the same call, and
      // what a caller gets back is the last set (the sqlite and mysql drivers' documented meaning,
      // and what a migration file depends on).
      r.nrows = 0;
      ncols = ((u8)payload.p[0] << 8) | (u8)payload.p[1];
      col_names = arena_alloc(a, (size_t)(ncols ? ncols : 1) * sizeof(str));
      col_oids = arena_alloc(a, (size_t)(ncols ? ncols : 1) * sizeof(i32));
      size_t i = 2;
      for (i32 k = 0; k < ncols; k++) {
        size_t start = i;
        while (i < payload.n && payload.p[i])
          i++;
        col_names[k] = str_sub(payload, start, i);
        i++;    // name terminator
        i += 4; // table oid
        i += 2; // attnum
        if (i + 4 > payload.n) break;
        col_oids[k] = ((u8)payload.p[i] << 24) | ((u8)payload.p[i + 1] << 16) |
                      ((u8)payload.p[i + 2] << 8) | (u8)payload.p[i + 3];
        i += 4; // type oid
        i += 2; // typlen
        i += 4; // typmod
        i += 2; // format
      }
      r.ncols = ncols;
      r.keys = col_names; // the shape, even with no rows: a schema check reads it
      break;
    }
    case 'D': { // DataRow
      if (rows == nullptr) {
        cap_rows = 16;
        rows = arena_alloc(a, (size_t)cap_rows * sizeof(WRow));
      } else if (r.nrows == cap_rows) {
        i32 ncap = cap_rows * 2;
        WRow *nr = arena_alloc(a, (size_t)ncap * sizeof(WRow));
        memcpy(nr, rows, (size_t)r.nrows * sizeof(WRow));
        rows = nr;
        cap_rows = ncap;
      }
      WRow *row = &rows[r.nrows++];
      i32 n = ((u8)payload.p[0] << 8) | (u8)payload.p[1];
      row->n = n;
      row->keys = col_names;
      row->vals = arena_alloc(a, (size_t)(n ? n : 1) * sizeof(WVal));
      size_t i = 2;
      for (i32 k = 0; k < n; k++) {
        i32 len = ((u8)payload.p[i] << 24) | ((u8)payload.p[i + 1] << 16) |
                  ((u8)payload.p[i + 2] << 8) | (u8)payload.p[i + 3];
        i += 4;
        if (len < 0) {
          row->vals[k] = (WVal){WV_NULL};
          continue;
        }
        row->vals[k] =
            parse_pg_value(a, str_sub(payload, i, i + (size_t)len), col_oids && k < ncols ? col_oids[k] : 0);
        i += (size_t)len;
      }
      break;
    }
    case 'C': { // CommandComplete
      tag = str_cstr(a, str_sub(payload, 0, payload.n ? payload.n - 1 : 0));
      break;
    }
    case 'E': {
      pg_error_from(pc, payload);
      r.ok = false;
      r.e = werr(WEBC_EDB, str_lit(pc->err));
      r.cls = pg_last_class;
      // 26000 invalid_sql_statement_name: the plan we reused is gone (DEALLOCATE, a schema
      // reload, a server restart). Forget it so the next request parses again instead of
      // failing forever on a stale name. Detected from the message's SQLSTATE, because a stale
      // plan is a driver concern, not an app-facing error kind.
      if (pi >= 0 && strstr(pc->err, "(SQLSTATE 26000)")) pc->plans[pi].hash = 0;
      break;
    }
    case 'Z':
      // Reached a ReadyForQuery: the statement (and the Parse that opened it) took.
      if (newplan >= 0 && r.ok) {
        pc->plans[newplan].hash = h;
        pc->plans[newplan].name = pc->plan_seq + 1;
        pc->plan_seq++;
        pc->nplans++;
      }
      r.rows = rows;
      if (tag) {
        // "INSERT 0 1", "UPDATE 3", "SELECT 5"
        const char *sp = strrchr(tag, ' ');
        if (sp) r.affected = atoll(sp + 1);
        if (strncmp(tag, "INSERT", 6) == 0) r.last_id = 0;
      }
      return r;
    default:
      break;
    }
  }
}

static bool pg_simple(WConn *c, const char *sql) {
  PgConn *pc = c->h;
  Arena *a = arena_new(1024);
  Msg m;
  msg_begin(&m, a, 'Q');
  buf_write(&m.b, sql, strlen(sql) + 1);
  if (!msg_send(pc, &m)) {
    arena_destroy(a);
    return false;
  }
  bool ok = true;
  for (;;) {
    char type;
    str payload;
    if (!read_msg(pc, a, &type, &payload)) {
      ok = false;
      break;
    }
    if (type == 'E') {
      pg_error_from(pc, payload);
      ok = false;
    }
    if (type == 'Z') break;
  }
  arena_destroy(a);
  return ok;
}

static bool pg_begin(WConn *c) {
  return pg_simple(c, "begin");
}
static bool pg_commit(WConn *c) {
  return pg_simple(c, "commit");
}
static bool pg_rollback(WConn *c) {
  return pg_simple(c, "rollback");
}

// Nested scopes are savepoints named by depth — the same convention as sqlite, so a
// transaction that nests in a handler behaves the same on both drivers.
static bool pg_savepoint(WConn *c, i32 depth) {
  char buf[64];
  snprintf(buf, sizeof buf, "savepoint wc_sp_%d", (int)depth);
  return pg_simple(c, buf);
}
static bool pg_release_savepoint(WConn *c, i32 depth) {
  char buf[64];
  snprintf(buf, sizeof buf, "release savepoint wc_sp_%d", (int)depth);
  return pg_simple(c, buf);
}
static bool pg_rollback_to(WConn *c, i32 depth) {
  char buf[64];
  snprintf(buf, sizeof buf, "rollback to savepoint wc_sp_%d", (int)depth);
  return pg_simple(c, buf);
}

static bool pg_broken(WConn *c) {
  PgConn *pc = c && c->h ? c->h : nullptr;
  return !pc || pc->broken || pc->cancelled;
}

const WDBDriver wc_driver_pg = {
    .name = "postgres",
    .placeholder = "$",
    .returning = true,            // `insert … returning id` is how a new key comes back
    .offset_needs_limit = false,  // `offset 5` alone is valid
    .ident_quote = '"',           // postgres folds an unquoted name to lower case, and every
                                  // quoted name here is already lower case: same identifier,
                                  // but a column called `order` or `user` now works
    .multi_statement = true,      // no parameters: the simple protocol runs a whole DDL file

    .open = pg_open,
    .close = pg_close,
    .exec = pg_exec,
    .begin = pg_begin,
    .commit = pg_commit,
    .rollback = pg_rollback,
    .savepoint = pg_savepoint,
    .release_savepoint = pg_release_savepoint,
    .rollback_to = pg_rollback_to,
    .broken = pg_broken,
};