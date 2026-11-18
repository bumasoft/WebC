// SPDX-License-Identifier: Apache-2.0
// WebC — MySQL / MariaDB driver: the wire protocol itself, no libmysqlclient.
//
// The promise this file keeps is the same one `db_pg.c` keeps for postgres and `redis.c`
// keeps for Redis: the binary stays static and needs nothing installed. That costs a
// protocol client, and the protocol is bigger than postgres' text format:
//
//   handshake v10 -> capability negotiation -> auth -> session setup -> commands
//
// Two execution paths, forced by the protocol and not by taste:
//
//   nparams == 0   COM_QUERY, text rows, several statements per call (db_exec_sql)
//   nparams  > 0   COM_STMT_PREPARE / COM_STMT_EXECUTE, binary params, binary rows,
//                  statements cached per connection
//
// Prepared statements cannot carry several statements, and `db_exec_sql` has always
// meant "run them all, keep the last result set" (that is how a migration file runs), so
// the split is not a shortcut: it is the only way to keep both promises. Every value
// still reaches the server as a bound parameter — nothing is interpolated.
//
// Values always cross the wire as parameters, never as text spliced into the SQL, which
// is the whole reason this driver speaks COM_STMT_* instead of escaping strings itself.
//
// `select` is a statement macro in <webc/task.h>, and this file includes <unistd.h>.
#define WEBC_NO_SELECT_MACRO 1
#include "db_internal.h"

#include <webc/ctx.h>
#include <webc/crypto.h>
#include <webc/log.h>
#include <webc/task.h>
#include <webc/tls.h>

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

// ---------------------------------------------------------------- capabilities

#define MY_CLIENT_LONG_PASSWORD 0x00000001u
#define MY_CLIENT_LONG_FLAG 0x00000004u
#define MY_CLIENT_CONNECT_WITH_DB 0x00000008u
// FOUND_ROWS is deliberately absent: `affected_rows` has to mean "rows changed". The
// durable job queue claims a job by `update … where state = 'queued'` and reads the count
// to find out whether it won (src/jobs.c); with FOUND_ROWS the server reports *matched*
// rows and every claim wins, silently, forever.
#define MY_CLIENT_PROTOCOL_41 0x00000200u
#define MY_CLIENT_SSL 0x00000800u
#define MY_CLIENT_TRANSACTIONS 0x00002000u
#define MY_CLIENT_SECURE_CONNECTION 0x00008000u
#define MY_CLIENT_MULTI_STATEMENTS 0x00010000u
#define MY_CLIENT_MULTI_RESULTS 0x00020000u
#define MY_CLIENT_PS_MULTI_RESULTS 0x00040000u
#define MY_CLIENT_PLUGIN_AUTH 0x00080000u
#define MY_CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA 0x00200000u

// The collations that mean "utf8mb4" on both servers. 45 (utf8mb4_general_ci) exists on
// MySQL 8 and MariaDB 11 alike; 255 (utf8mb4_0900_ai_ci) is MySQL-8-only and would be a
// driver that works on one server and not the other.
#define MY_CHARSET_UTF8MB4 45

// Sequences in a command packet; a resultset ends with EOF, or with OK when the server
// announced DEPRECATE_EOF. We do not announce it: one packet shape is easier to be sure
// about than two, and the difference is four bytes on the wire.
#define MY_STATUS_MORE_RESULTS 0x0008u
#define MY_STATUS_IN_TRANS 0x0001u
#define MY_STATUS_AUTOCOMMIT 0x0002u

// Commands
#define MY_COM_QUIT 0x01
#define MY_COM_QUERY 0x03
#define MY_COM_PING 0x0e
#define MY_COM_STMT_PREPARE 0x16
#define MY_COM_STMT_EXECUTE 0x17
#define MY_COM_STMT_CLOSE 0x19

// Column types (the parts this driver decodes)
#define MY_TYPE_DECIMAL 0
#define MY_TYPE_TINY 1
#define MY_TYPE_SHORT 2
#define MY_TYPE_LONG 3
#define MY_TYPE_FLOAT 4
#define MY_TYPE_DOUBLE 5
#define MY_TYPE_NULL 6
#define MY_TYPE_TIMESTAMP 7
#define MY_TYPE_LONGLONG 8
#define MY_TYPE_INT24 9
#define MY_TYPE_DATE 10
#define MY_TYPE_TIME 11
#define MY_TYPE_DATETIME 12
#define MY_TYPE_YEAR 13
#define MY_TYPE_NEWDATE 14
#define MY_TYPE_VARCHAR 15
#define MY_TYPE_BIT 16
#define MY_TYPE_TIMESTAMP2 17
#define MY_TYPE_DATETIME2 18
#define MY_TYPE_TIME2 19
#define MY_TYPE_JSON 245
#define MY_TYPE_NEWDECIMAL 246
#define MY_TYPE_ENUM 247
#define MY_TYPE_SET 248
#define MY_TYPE_TINY_BLOB 249
#define MY_TYPE_MEDIUM_BLOB 250
#define MY_TYPE_LONG_BLOB 251
#define MY_TYPE_BLOB 252
#define MY_TYPE_VAR_STRING 253
#define MY_TYPE_STRING 254
#define MY_TYPE_GEOMETRY 255

#define MY_UNSIGNED_FLAG 0x0020u
#define MY_BINARY_CHARSET 63

#define MY_STMT_CACHE 64
#define MY_MAX_PACKET (64u * 1024 * 1024)

// ---------------------------------------------------------------- structures

// The SQL text is the cache key, and it is kept *inside* the entry: a connection lives for
// as long as the process, and copying every distinct statement into its arena would be a
// slow leak that only shows up under load. A statement too long to fit is prepared, run and
// closed — correctness first, and the cache is for the statements an app repeats.
#define MY_STMT_SQL 384

typedef struct MyStmt {
  char sql[MY_STMT_SQL];
  size_t len;
  u32 id;      // the server's statement handle: only valid on this connection
  u16 nparams;
  u16 ncols;
  bool cached;
  u64 used;    // LRU stamp
} MyStmt;

typedef enum MySsl : u8 {
  MY_SSL_DISABLE = 0,
  MY_SSL_PREFER,
  MY_SSL_REQUIRE,
  MY_SSL_VERIFY_CA,
  MY_SSL_VERIFY_FULL
} MySsl;

typedef struct MyConn {
  i32 fd;
  void *tls;
  char host[256];
  char port[16];
  char user[128];
  char pass[256];
  char dbname[128];
  MySsl ssl;
  char ca_file[512];
  char err[512];
  bool broken;
  bool cancelled;
  bool in_tx;
  bool mariadb;
  u32 server_version; // 80004 for 8.0.4, 110100 for MariaDB 11.1.0
  u32 thread_id;      // the connection id `KILL QUERY` names
  u32 status;         // the last OK/EOF status flags
  u8 seq;             // packet sequence number, reset per command
  i64 timeout_ms;     // the socket's bounded park; not the statement's cap
  WCtx *ctx;          // the request's context while a statement runs
  Arena *a;           // the connection's own arena (see WConn::arena)
  MyStmt stmts[MY_STMT_CACHE];
  i32 nstmts;
  u64 stamp;
} MyConn;

// ---------------------------------------------------------------- wire

static void my_kill_query(MyConn *c);

static i64 my_read_raw(MyConn *c, void *buf, size_t n) {
  if (c->tls) return tls_read(c->tls, buf, n);
  for (;;) {
    ssize_t r = read(c->fd, buf, n);
    if (r >= 0) return r;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    // The same bargain the postgres driver makes: a 200 ms park, so a cancelled request
    // stops waiting on its database at a granularity a person can feel, while the worker
    // thread goes and serves somebody else.
    if (c->ctx && ctx_done(c->ctx)) return -2;
    if (!wfd_wait(c->fd, false, 200) && c->ctx && ctx_done(c->ctx)) return -2;
  }
}

static i64 my_write_raw(MyConn *c, const void *buf, size_t n) {
  if (c->tls) return tls_write(c->tls, buf, n);
  return wfd_write(c->fd, buf, n);
}

static bool my_read_exact(MyConn *c, void *p, size_t n) {
  char *at = p;
  while (n) {
    i64 r = my_read_raw(c, at, n);
    if (r == -2) {
      // The client gave up. MySQL has no out-of-band cancel, so the *server* has to be
      // told on another connection — see my_kill_query(). This connection cannot be
      // reused afterwards: the KILL and the statement race on this socket.
      my_kill_query(c);
      if (!c->cancelled) {
        snprintf(c->err, sizeof c->err, "statement cancelled: %s",
                 c->ctx && ctx_err(c->ctx).n ? ctx_err(c->ctx).p : "the request ended");
        c->cancelled = true;
      }
      return false;
    }
    if (r <= 0) return false;
    at += r;
    n -= (size_t)r;
  }
  return true;
}

static bool my_write_all(MyConn *c, const void *p, size_t n) {
  const char *at = p;
  while (n) {
    i64 w = my_write_raw(c, at, n);
    if (w <= 0) return false;
    at += w;
    n -= (size_t)w;
  }
  return true;
}

// One packet: 3-byte little-endian length, 1-byte sequence, payload.
static bool my_send(MyConn *c, const void *payload, size_t n) {
  u8 head[4] = {(u8)(n & 0xff), (u8)((n >> 8) & 0xff), (u8)((n >> 16) & 0xff), c->seq};
  c->seq++;
  if (!my_write_all(c, head, 4)) return false;
  return n == 0 || my_write_all(c, payload, n);
}

static bool my_send_str(MyConn *c, str s) { return my_send(c, s.p, s.n); }

// Read one packet into `a`. A packet that is a multiple of 16 MB is followed by another;
// a 64 MB ceiling on a single logical packet is enough for any statement an app writes
// and stops a corrupt length from asking for a gigabyte.
static bool my_recv(MyConn *c, Arena *a, str *out) {
  size_t total = 0;
  char *buf = nullptr;
  for (;;) {
    u8 head[4];
    if (!my_read_exact(c, head, 4)) return false;
    // The sequence number is the conversation's counter, shared by both sides: the packet we
    // send next must carry *this* one plus one. Not reading it here left the counter at
    // whatever we last wrote, so the answer to an AuthSwitchRequest went out with a sequence
    // the server had already seen — "Got packets out of order" (errno 1156), from a driver
    // that had spoken to nobody yet.
    c->seq = (u8)(head[3] + 1);
    size_t n = (size_t)head[0] | ((size_t)head[1] << 8) | ((size_t)head[2] << 16);
    if (n > MY_MAX_PACKET || total + n > MY_MAX_PACKET) {
      snprintf(c->err, sizeof c->err, "mysql: packet of %zu bytes is too large", n);
      return false;
    }
    if (!buf) {
      buf = arena_alloc(a, n + 1);
      if (!buf) return false;
    } else {
      char *bigger = arena_alloc(a, total + n + 1);
      if (!bigger) return false;
      memcpy(bigger, buf, total);
      buf = bigger;
    }
    if (n && !my_read_exact(c, buf + total, n)) return false;
    total += n;
    buf[total] = 0;
    if (n < 16777215) break; // a short packet ends the logical message
  }
  *out = (str){buf, total};
  return true;
}

// ---------------------------------------------------------------- encoding helpers

static void my_put_u16(Buf *b, u16 v) {
  char t[2] = {(char)(v & 0xff), (char)((v >> 8) & 0xff)};
  buf_write(b, t, 2);
}
static void my_put_u24(Buf *b, u32 v) {
  char t[3] = {(char)(v & 0xff), (char)((v >> 8) & 0xff), (char)((v >> 16) & 0xff)};
  buf_write(b, t, 3);
}
static void my_put_u32(Buf *b, u32 v) {
  char t[4] = {(char)(v & 0xff), (char)((v >> 8) & 0xff), (char)((v >> 16) & 0xff),
               (char)((v >> 24) & 0xff)};
  buf_write(b, t, 4);
}
static void my_put_u64(Buf *b, u64 v) {
  for (i32 i = 0; i < 8; i++) buf_putc(b, (char)((v >> (8 * i)) & 0xff));
}
static void my_put_lenenc(Buf *b, u64 v) {
  if (v < 251) {
    buf_putc(b, (char)v);
  } else if (v < 65536) {
    buf_putc(b, (char)0xfc);
    my_put_u16(b, (u16)v);
  } else if (v < 16777216) {
    buf_putc(b, (char)0xfd);
    my_put_u24(b, (u32)v);
  } else {
    buf_putc(b, (char)0xfe);
    my_put_u64(b, v);
  }
}
static void my_put_lenenc_str(Buf *b, str s) {
  my_put_lenenc(b, s.n);
  buf_write(b, s.p, s.n);
}
static void my_put_cstr(Buf *b, const char *s) { buf_write(b, s, strlen(s) + 1); }

static u16 my_get_u16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 my_get_u24(const u8 *p) { return (u32)(p[0] | (p[1] << 8) | (p[2] << 16)); }
static u32 my_get_u32(const u8 *p) {
  return (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24));
}
static u64 my_get_u64(const u8 *p) {
  u64 v = 0;
  for (i32 i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

typedef struct MySlice {
  const u8 *p;
  size_t n;
  size_t at;
} MySlice;

static bool my_byte(MySlice *s, u8 *out) {
  if (s->at >= s->n) return false;
  *out = s->p[s->at++];
  return true;
}

static bool my_lenenc(MySlice *s, u64 *out) {
  if (s->at >= s->n) return false;
  u8 first = s->p[s->at++];
  if (first < 251) {
    *out = first;
    return true;
  }
  if (first == 251) { // SQL NULL, only legal in a row
    *out = (u64)-1;
    return true;
  }
  if (first == 0xfc) {
    if (s->at + 2 > s->n) return false;
    *out = my_get_u16(s->p + s->at);
    s->at += 2;
    return true;
  }
  if (first == 0xfd) {
    if (s->at + 3 > s->n) return false;
    *out = my_get_u24(s->p + s->at);
    s->at += 3;
    return true;
  }
  if (first == 0xfe) {
    if (s->at + 8 > s->n) return false;
    *out = my_get_u64(s->p + s->at);
    s->at += 8;
    return true;
  }
  return false;
}

static bool my_lenenc_str(MySlice *s, str *out) {
  u64 n = 0;
  if (!my_lenenc(s, &n)) return false;
  if (n == (u64)-1) {
    *out = (str){nullptr, 0};
    return true;
  }
  if (s->at + n > s->n) return false;
  *out = (str){(char *)s->p + s->at, (size_t)n};
  s->at += (size_t)n;
  return true;
}

// ---------------------------------------------------------------- errors

// The ERR packet: 0xFF, a 16-bit errno, then '#', a five-character SQLSTATE and the
// message. Both halves are classified: the SQLSTATE says `23000` and the errno says
// *which* integrity constraint, and it is the errno an app actually needs (1062 is a
// duplicate; 1452 is a parent that does not exist).
static WDbErrKind my_class(u16 code, const char *sqlstate) {
  switch (code) {
  case 1062: // ER_DUP_ENTRY
  case 1586: // ER_DUP_ENTRY_WITH_KEY_NAME
    return WDBE_CONFLICT;
  case 1451: // ER_ROW_IS_REFERENCED_2: deleting a row something points at
    return WDBE_CONFLICT;
  case 1452: // ER_NO_REFERENCED_ROW_2
  case 1216:
  case 1217:
    return WDBE_BAD_REFERENCE;
  case 1048: // ER_BAD_NULL_ERROR
  case 1364: // ER_NO_DEFAULT_FOR_FIELD (strict mode: a required column with no value)
    return WDBE_NOT_NULL;
  case 1406: // ER_DATA_TOO_LONG
  case 1264: // ER_WARN_DATA_OUT_OF_RANGE
  case 1292: // ER_TRUNCATED_WRONG_VALUE
    return WDBE_TOO_LONG;
  case 3819: // ER_CHECK_CONSTRAINT_VIOLATED
  case 4025: // MariaDB's ER_CONSTRAINT_FAILED
    return WDBE_CHECK;
  case 1213: // ER_LOCK_DEADLOCK
    return WDBE_DEADLOCK;
  case 1205: // ER_LOCK_WAIT_TIMEOUT
    return WDBE_TIMEOUT;
  case 3024: // ER_QUERY_TIMEOUT (max_execution_time)
    return WDBE_TIMEOUT;
  case 1045: // ER_ACCESS_DENIED_ERROR
  case 1698: // ER_ACCESS_DENIED_NO_PASSWORD_ERROR
    return WDBE_AUTH;
  case 2006: // server has gone away
  case 2013: // lost connection during query
  case 1053: // server shutdown in progress
    return WDBE_LOST;
  default:
    break;
  }
  if (sqlstate && strlen(sqlstate) >= 2) {
    if (strncmp(sqlstate, "28", 2) == 0) return WDBE_AUTH;
    if (strncmp(sqlstate, "08", 2) == 0) return WDBE_LOST;
    if (strncmp(sqlstate, "40", 2) == 0) return WDBE_DEADLOCK;
    if (strcmp(sqlstate, "23000") == 0) return WDBE_CONFLICT;
    if (strcmp(sqlstate, "23502") == 0) return WDBE_NOT_NULL;
    if (strcmp(sqlstate, "22001") == 0) return WDBE_TOO_LONG;
  }
  return WDBE_NONE;
}

// Returns the class of the packet and fills `c->err` with a sentence that names the
// server's own code, because an error a person cannot look up is an error they cannot fix.
static WDbErrKind my_error_from(MyConn *c, str payload) {
  if (payload.n < 3) {
    snprintf(c->err, sizeof c->err, "mysql: malformed error packet");
    return WDBE_NONE;
  }
  const u8 *p = (const u8 *)payload.p;
  u16 code = my_get_u16(p + 1);
  char sqlstate[6] = "";
  size_t at = 3;
  if (payload.n >= 9 && p[3] == '#') {
    memcpy(sqlstate, payload.p + 4, 5);
    sqlstate[5] = 0;
    at = 9;
  }
  snprintf(c->err, sizeof c->err, "mysql: %.*s (errno %u%s%s)", (int)(payload.n - at),
           payload.p + at, (unsigned)code, sqlstate[0] ? ", SQLSTATE " : "", sqlstate);
  return my_class(code, sqlstate);
}

// ---------------------------------------------------------------- OK / EOF

typedef struct MyOk {
  u64 affected;
  u64 last_id;
  u32 status;
  u16 warnings;
} MyOk;

// An OK packet starts 0x00 (or 0xFE when it terminates a resultset).
static bool my_parse_ok(str payload, MyOk *ok) {
  if (!payload.p || payload.n < 1) return false;
  MySlice s = {(const u8 *)payload.p, payload.n, 1};
  u64 affected = 0, last_id = 0;
  if (!my_lenenc(&s, &affected)) return false;
  if (!my_lenenc(&s, &last_id)) return false;
  u32 status = 0;
  u16 warnings = 0;
  if (s.at + 4 <= s.n) {
    status = my_get_u16(s.p + s.at);
    warnings = my_get_u16(s.p + s.at + 2);
  }
  ok->affected = affected;
  ok->last_id = last_id;
  ok->status = status;
  ok->warnings = warnings;
  return true;
}

static bool my_is_eof(str payload) {
  return payload.n < 9 && payload.n > 0 && (u8)payload.p[0] == 0xfe;
}

// ---------------------------------------------------------------- column definitions

typedef struct MyCol {
  str name;
  u16 charset;
  u8 type;
  u16 flags;
  u32 length; // the declared width: `tinyint(1)` is how every schema spells a bool
} MyCol;

static bool my_parse_col(str payload, MyCol *col) {
  MySlice s = {(const u8 *)payload.p, payload.n, 0};
  str catalog, schema, table, org_table, name, org_name;
  if (!my_lenenc_str(&s, &catalog) || !my_lenenc_str(&s, &schema) ||
      !my_lenenc_str(&s, &table) || !my_lenenc_str(&s, &org_table) ||
      !my_lenenc_str(&s, &name) || !my_lenenc_str(&s, &org_name))
    return false;
  u64 fixed = 0;
  if (!my_lenenc(&s, &fixed)) return false;
  if (s.at + 10 > s.n) return false;
  col->name = name;
  col->charset = my_get_u16(s.p + s.at);
  col->length = my_get_u32(s.p + s.at + 2);
  col->type = s.p[s.at + 6];
  col->flags = my_get_u16(s.p + s.at + 7);
  return true;
}

// ---------------------------------------------------------------- value decoding

// The binary protocol sends typed values; the text protocol sends everything as a string.
// Both land in a WVal, and the *column's* type decides how — which is why the type is
// carried out of the column definition instead of guessed from the bytes. Guessing is what
// turned a text column holding "1" into an integer on postgres once already.
static WVal my_val_text(Arena *a, MyCol *col, str s) {
  if (col->type == MY_TYPE_TINY && col->length == 1)
    return (WVal){WV_BOOL, .b = s.n > 0 && s.p[0] != '0'};
  if (col->type == MY_TYPE_BIT) {
    u64 v = 0;
    for (size_t k = 0; k < s.n; k++) v = (v << 8) | (u8)s.p[k];
    if (s.n == 1 && v <= 1) return (WVal){WV_BOOL, .b = v == 1};
    return (WVal){WV_I64, .i = (i64)v};
  }
  // A FLOAT/DOUBLE column is a real even when its digits read like an integer: `sum(score)`
  // over 1.5, 2.5, 4.0 is "8" on the wire and 8.0 everywhere else, so parsing it as the wider
  // integer would silently change the type between dialects.
  if (col->type == MY_TYPE_FLOAT || col->type == MY_TYPE_DOUBLE) {
    f64 f = 0;
    if (str_to_f64(s, &f)) return (WVal){WV_F64, .f = f};
  }
  bool numeric = false;
  switch (col->type) {
  case MY_TYPE_TINY:
  case MY_TYPE_SHORT:
  case MY_TYPE_LONG:
  case MY_TYPE_LONGLONG:
  case MY_TYPE_INT24:
  case MY_TYPE_YEAR:
  case MY_TYPE_BIT:
  // A DECIMAL on the wire is digits, and the digits say which shape it is: `sum(int)` is
  // DECIMAL(32,0) — an integer, like postgres' bigint — and `avg(int)` is DECIMAL(x,4) — a
  // real. Omitting these two cases made every aggregate come back as a WV_BLOB, because a
  // DECIMAL column reports the *binary* charset and the charset check below owns that.
  case MY_TYPE_NEWDECIMAL:
  case MY_TYPE_DECIMAL:
    numeric = true;
    break;
  default:
    break;
  }
  if (numeric) {
    i64 i = 0;
    if (!str_contains(s, str_lit(".")) && str_to_i64(s, &i)) {
      if (col->flags & MY_UNSIGNED_FLAG) {
        u64 u = 0;
        for (size_t k = 0; k < s.n; k++)
          if (s.p[k] >= '0' && s.p[k] <= '9') u = u * 10 + (u64)(s.p[k] - '0');
        return (WVal){WV_I64, .i = (i64)u};
      }
      return (WVal){WV_I64, .i = i};
    }
    f64 f = 0;
    if (str_to_f64(s, &f)) return (WVal){WV_F64, .f = f};
  }
  if (col->charset == MY_BINARY_CHARSET) {
    char *copy = arena_alloc(a, s.n + 1);
    if (!copy) return (WVal){WV_NULL};
    memcpy(copy, s.p, s.n);
    copy[s.n] = 0;
    return (WVal){WV_BLOB, .s = (str){copy, s.n}};
  }
  char *copy = arena_alloc(a, s.n + 1);
  if (!copy) return (WVal){WV_NULL};
  memcpy(copy, s.p, s.n);
  copy[s.n] = 0;
  return (WVal){WV_TEXT, .s = (str){copy, s.n}};
}

static WVal my_val_bin(Arena *a, MyCol *col, MySlice *s) {
  switch (col->type) {
  case MY_TYPE_TINY: {
    u8 v = 0;
    if (!my_byte(s, &v)) return (WVal){WV_NULL};
    // `tinyint(1)` is how every MySQL schema spells a boolean, and it is what
    // `WField{bool}` is written to. Reading it back as an integer works for `row_bool()`
    // and produces `1` for `json()`, so the declared width decides: 1 is a bool, anything
    // wider is an integer.
    if (col->length == 1) return (WVal){WV_BOOL, .b = v != 0};
    return (WVal){WV_I64, .i = (i64)v};
  }
  case MY_TYPE_SHORT:
  case MY_TYPE_YEAR: {
    if (s->at + 2 > s->n) return (WVal){WV_NULL};
    u16 v = my_get_u16(s->p + s->at);
    s->at += 2;
    return (WVal){WV_I64, .i = (i64)v};
  }
  case MY_TYPE_LONG:
  case MY_TYPE_INT24: {
    if (s->at + 4 > s->n) return (WVal){WV_NULL};
    u32 v = my_get_u32(s->p + s->at);
    s->at += 4;
    return (WVal){WV_I64, .i = (col->flags & MY_UNSIGNED_FLAG) ? (i64)v : (i64)(i32)v};
  }
  case MY_TYPE_LONGLONG: {
    if (s->at + 8 > s->n) return (WVal){WV_NULL};
    u64 v = my_get_u64(s->p + s->at);
    s->at += 8;
    return (WVal){WV_I64, .i = (i64)v};
  }
  case MY_TYPE_FLOAT: {
    if (s->at + 4 > s->n) return (WVal){WV_NULL};
    u32 bits = my_get_u32(s->p + s->at);
    s->at += 4;
    f32 f;
    memcpy(&f, &bits, 4);
    return (WVal){WV_F64, .f = (f64)f};
  }
  case MY_TYPE_DOUBLE: {
    if (s->at + 8 > s->n) return (WVal){WV_NULL};
    u64 bits = my_get_u64(s->p + s->at);
    s->at += 8;
    f64 f;
    memcpy(&f, &bits, 8);
    return (WVal){WV_F64, .f = f};
  }
  case MY_TYPE_DATE:
  case MY_TYPE_NEWDATE:
  case MY_TYPE_DATETIME:
  case MY_TYPE_TIMESTAMP:
  case MY_TYPE_DATETIME2:
  case MY_TYPE_TIMESTAMP2:
  case MY_TYPE_TIME:
  case MY_TYPE_TIME2: {
    // length-prefixed: 0 = zero date, 4 = date, 7 = date+time, 11 = with microseconds
    u8 len = 0;
    if (!my_byte(s, &len)) return (WVal){WV_NULL};
    if (len == 0) return (WVal){WV_TEXT, .s = str_lit("0000-00-00 00:00:00")};
    if (s->at + len > s->n) return (WVal){WV_NULL};
    const u8 *p = s->p + s->at;
    s->at += len;
    if (col->type == MY_TYPE_TIME || col->type == MY_TYPE_TIME2) {
      bool neg = len >= 8 && p[0] != 0;
      u32 days = len >= 8 ? my_get_u32(p + 1) : 0;
      u8 hh = len >= 8 ? p[5] : 0, mm = len >= 8 ? p[6] : 0, ss = len >= 8 ? p[7] : 0;
      u32 us = len >= 12 ? my_get_u32(p + 8) : 0;
      i64 total = (i64)days * 86400 + (i64)hh * 3600 + (i64)mm * 60 + ss;
      char buf[40];
      snprintf(buf, sizeof buf, "%s%02d:%02d:%02d.%06u", neg ? "-" : "", (int)(total / 3600),
               (int)((total / 60) % 60), (int)(total % 60), (unsigned)us);
      return (WVal){WV_TEXT, .s = str_fmt(a, "%s", buf)};
    }
    u16 year = my_get_u16(p);
    u8 month = len >= 4 ? p[2] : 0, day = len >= 4 ? p[3] : 0;
    u8 hh = len >= 7 ? p[4] : 0, mm = len >= 7 ? p[5] : 0, ss = len >= 7 ? p[6] : 0;
    u32 us = len >= 11 ? my_get_u32(p + 7) : 0;
    // Text, not a time struct: the framework's model fields are `i64` epoch or `text`,
    // and a driver that invents a third shape is a driver an app has to special-case.
    if (us)
      return (WVal){WV_TEXT, .s = str_fmt(a, "%04u-%02u-%02u %02u:%02u:%02u.%06u", year, month,
                                          day, hh, mm, ss, (unsigned)us)};
    if (hh || mm || ss)
      return (WVal){WV_TEXT, .s = str_fmt(a, "%04u-%02u-%02u %02u:%02u:%02u", year, month, day,
                                          hh, mm, ss)};
    return (WVal){WV_TEXT, .s = str_fmt(a, "%04u-%02u-%02u", year, month, day)};
  }
  case MY_TYPE_NEWDECIMAL:
  case MY_TYPE_DECIMAL: {
    // Binary protocol DECIMAL is a length-encoded *string* of digits (packed BCD is a
    // storage format, not a wire format). It is decoded to f64 exactly as postgres'
    // numeric is today, which is the one place the three dialects agree on a value and
    // disagree about a type — see docs/reference.md §9a4.
    str text;
    if (!my_lenenc_str(s, &text)) return (WVal){WV_NULL};
    i64 i = 0;
    if (!str_contains(text, str_lit(".")) && str_to_i64(text, &i)) return (WVal){WV_I64, .i = i};
    f64 f = 0;
    if (str_to_f64(text, &f)) return (WVal){WV_F64, .f = f};
    return (WVal){WV_TEXT, .s = text};
  }
  case MY_TYPE_BIT: {
    str raw;
    if (!my_lenenc_str(s, &raw)) return (WVal){WV_NULL};
    u64 v = 0;
    for (size_t k = 0; k < raw.n; k++) v = (v << 8) | (u8)raw.p[k];
    if (raw.n == 1 && (v == 0 || v == 1)) return (WVal){WV_BOOL, .b = v == 1};
    return (WVal){WV_I64, .i = (i64)v};
  }
  default: {
    str raw;
    if (!my_lenenc_str(s, &raw)) return (WVal){WV_NULL};
    if (col->charset == MY_BINARY_CHARSET) {
      char *copy = arena_alloc(a, raw.n + 1);
      if (!copy) return (WVal){WV_NULL};
      if (raw.n) memcpy(copy, raw.p, raw.n);
      copy[raw.n] = 0;
      return (WVal){WV_BLOB, .s = (str){copy, raw.n}};
    }
    char *copy = arena_alloc(a, raw.n + 1);
    if (!copy) return (WVal){WV_NULL};
    if (raw.n) memcpy(copy, raw.p, raw.n);
    copy[raw.n] = 0;
    return (WVal){WV_TEXT, .s = (str){copy, raw.n}};
  }
  }
}

// ---------------------------------------------------------------- resultsets

// One resultset, text or binary. Returns false on a protocol failure (or a cancellation),
// and reports through `*more` whether the server announced another result after this one.
static bool my_resultset(MyConn *c, Arena *a, bool binary, WRes *r, bool *more) {
  Arena *tmp = arena_new(4096);
  str pkt = {0};
  if (!my_recv(c, tmp, &pkt)) {
    arena_destroy(tmp);
    return false;
  }
  if (pkt.n == 0) {
    // an empty packet where a header belongs is the server hanging up
    snprintf(c->err, sizeof c->err, "mysql: empty packet");
    arena_destroy(tmp);
    return false;
  }
  u8 first = (u8)pkt.p[0];
  if (first == 0xff) {
    r->ok = false;
    r->cls = my_error_from(c, pkt);
    r->e = werr(WEBC_EDB, str_lit(c->err));
    arena_destroy(tmp);
    return true;
  }
  if (first == 0x00) { // OK: a statement with no result set
    MyOk ok = {0};
    if (!my_parse_ok(pkt, &ok)) {
      snprintf(c->err, sizeof c->err, "mysql: malformed OK packet");
      arena_destroy(tmp);
      return false;
    }
    c->status = ok.status;
    r->affected += (i64)ok.affected;
    if (ok.last_id) r->last_id = (i64)ok.last_id;
    *more = (ok.status & MY_STATUS_MORE_RESULTS) != 0;
    arena_destroy(tmp);
    return true;
  }
  // a lenenc column count
  MySlice s = {(const u8 *)pkt.p, pkt.n, 0};
  u64 ncols = 0;
  if (!my_lenenc(&s, &ncols) || ncols == 0 || ncols > 4096) {
    snprintf(c->err, sizeof c->err, "mysql: bad column count %llu",
             (unsigned long long)ncols);
    arena_destroy(tmp);
    return false;
  }
  MyCol *cols = arena_alloc(a, (size_t)ncols * sizeof(MyCol));
  str *names = arena_alloc(a, (size_t)ncols * sizeof(str));
  if (!cols || !names) {
    arena_destroy(tmp);
    return false;
  }
  for (u64 i = 0; i < ncols; i++) {
    if (!my_recv(c, tmp, &pkt)) {
      arena_destroy(tmp);
      return false;
    }
    if (!my_parse_col(pkt, &cols[i])) {
      snprintf(c->err, sizeof c->err, "mysql: malformed column definition");
      arena_destroy(tmp);
      return false;
    }
    // the name has to outlive `tmp`
    names[i] = str_dup(a, cols[i].name);
  }
  if (!my_recv(c, tmp, &pkt)) { // EOF after the column definitions
    arena_destroy(tmp);
    return false;
  }
  if ((u8)pkt.p[0] == 0xff) {
    r->ok = false;
    r->cls = my_error_from(c, pkt);
    r->e = werr(WEBC_EDB, str_lit(c->err));
    arena_destroy(tmp);
    return true;
  }
  r->keys = names;
  r->ncols = (i32)ncols;
  i32 cap = 0;
  WRow *rows = nullptr;
  for (;;) {
    if (!my_recv(c, tmp, &pkt)) {
      arena_destroy(tmp);
      return false;
    }
    if (my_is_eof(pkt)) {
      if (pkt.n >= 5) c->status = my_get_u16((const u8 *)pkt.p + 3);
      *more = (c->status & MY_STATUS_MORE_RESULTS) != 0;
      break;
    }
    if ((u8)pkt.p[0] == 0xff) {
      r->ok = false;
      r->cls = my_error_from(c, pkt);
      r->e = werr(WEBC_EDB, str_lit(c->err));
      arena_destroy(tmp);
      return true;
    }
    if (r->nrows == cap) {
      i32 ncap = cap ? cap * 2 : 16;
      WRow *nr = arena_alloc(a, (size_t)ncap * sizeof(WRow));
      if (!nr) {
        arena_destroy(tmp);
        return false;
      }
      if (rows) memcpy(nr, rows, (size_t)r->nrows * sizeof(WRow));
      rows = nr;
      cap = ncap;
    }
    WRow *row = &rows[r->nrows++];
    row->n = (i32)ncols;
    row->keys = names;
    row->vals = arena_alloc(a, (size_t)ncols * sizeof(WVal));
    if (!row->vals) {
      arena_destroy(tmp);
      return false;
    }
    if (!binary) {
      MySlice rs = {(const u8 *)pkt.p, pkt.n, 0};
      for (u64 i = 0; i < ncols; i++) {
        str v;
        if (!my_lenenc_str(&rs, &v)) {
          row->vals[i] = (WVal){WV_NULL};
          continue;
        }
        row->vals[i] = v.p ? my_val_text(a, &cols[i], v) : (WVal){WV_NULL};
      }
    } else {
      // 0x00 header, then a NULL bitmap whose first two bits belong to nothing
      size_t bitmap = (size_t)((ncols + 7 + 2) / 8);
      MySlice rs = {(const u8 *)pkt.p, pkt.n, 1 + bitmap};
      const u8 *bits = (const u8 *)pkt.p + 1;
      for (u64 i = 0; i < ncols; i++) {
        size_t bit = (size_t)i + 2;
        bool is_null = (bits[bit / 8] >> (bit % 8)) & 1;
        row->vals[i] = is_null ? (WVal){WV_NULL} : my_val_bin(a, &cols[i], &rs);
      }
    }
  }
  r->rows = rows;
  arena_destroy(tmp);
  return true;
}

// Run one COM_QUERY, following every resultset it produces (multi-statement SQL keeps the
// last result set, exactly as the sqlite driver does, and sums `affected`).
static WRes my_query_text(MyConn *c, str sql, Arena *a) {
  WRes r = {.ok = true};
  c->seq = 0;
  Buf b;
  buf_init(&b, a, sql.n + 1);
  buf_putc(&b, (char)MY_COM_QUERY);
  buf_write(&b, sql.p, sql.n);
  if (!my_send(c, b.p, b.n)) {
    r.ok = false;
    r.e = werr(c->cancelled ? WEBC_ECANCELED : WEBC_EDB,
               str_lit(c->err[0] ? c->err : "mysql: the connection is gone"));
    if (!c->cancelled) c->broken = true;
    return r;
  }
  bool more = true;
  bool first = true;
  while (more) {
    more = false;
    if (!first) {
      // "run them all, keep the last result set" — the sqlite driver's documented
      // meaning, and what a migration file depends on. Everything here still lands in the
      // caller's arena, so nothing dangles.
      r.rows = nullptr;
      r.nrows = 0;
      r.keys = nullptr;
      r.ncols = 0;
    }
    if (!my_resultset(c, a, false, &r, &more)) {
      r.ok = false;
      if (r.e.len == 0)
        r.e = werr(c->cancelled ? WEBC_ECANCELED : WEBC_EDB,
                   str_lit(c->err[0] ? c->err : "mysql: the connection is lost"));
      if (!c->cancelled) c->broken = true;
      return r;
    }
    first = false;
    if (!r.ok) return r; // an error in any statement ends the batch
  }
  return r;
}

// ---------------------------------------------------------------- prepared statements
//
// A statement id belongs to a connection, so the cache lives here and not in the pool. It
// is small on purpose (64 is more distinct statements than an app runs in one request) and
// it is LRU, because a migration script with 500 statements should not quietly keep 500
// server-side handles open.

static MyStmt *my_stmt_find(MyConn *c, str sql) {
  for (i32 i = 0; i < c->nstmts; i++)
    if (c->stmts[i].len == sql.n && memcmp(c->stmts[i].sql, sql.p, sql.n) == 0)
      return &c->stmts[i];
  return nullptr;
}

static void my_stmt_close(MyConn *c, u32 id) {
  if (c->broken || c->fd < 0) return;
  Buf b;
  buf_init(&b, c->a, 8);
  buf_putc(&b, (char)MY_COM_STMT_CLOSE);
  my_put_u32(&b, id);
  c->seq = 0;
  (void)my_send(c, b.p, b.n); // COM_STMT_CLOSE has no reply
}

static void my_stmt_evict_one(MyConn *c) {
  i32 worst = 0;
  for (i32 i = 1; i < c->nstmts; i++)
    if (c->stmts[i].used < c->stmts[worst].used) worst = i;
  my_stmt_close(c, c->stmts[worst].id);
  c->stmts[worst] = c->stmts[c->nstmts - 1];
  c->nstmts--;
}

static MyStmt *my_stmt_prepare(MyConn *c, str sql, Arena *a) {
  MyStmt *hit = my_stmt_find(c, sql);
  if (hit) {
    hit->used = ++c->stamp;
    return hit;
  }
  bool cacheable = sql.n < MY_STMT_SQL;
  Buf b;
  buf_init(&b, a, sql.n + 1);
  buf_putc(&b, (char)MY_COM_STMT_PREPARE);
  buf_write(&b, sql.p, sql.n);
  c->seq = 0;
  if (!my_send(c, b.p, b.n)) {
    snprintf(c->err, sizeof c->err, "mysql: the connection is gone");
    c->broken = true;
    return nullptr;
  }
  Arena *tmp = arena_new(2048);
  str pkt = {0};
  bool ok = false;
  u32 id = 0;
  u16 nparams = 0, ncols = 0;
  for (;;) {
    if (!my_recv(c, tmp, &pkt)) break;
    u8 first = (u8)pkt.p[0];
    if (first == 0xff) {
      my_error_from(c, pkt);
      break;
    }
    if (first != 0x00 || pkt.n < 12) {
      snprintf(c->err, sizeof c->err, "mysql: malformed prepare response");
      break;
    }
    const u8 *p = (const u8 *)pkt.p;
    id = my_get_u32(p + 1);
    ncols = my_get_u16(p + 5);
    nparams = my_get_u16(p + 7);
    // the parameter and column definitions that follow, and their EOFs — read and
    // discarded: the *execute* response carries fresh column definitions, so nothing here
    // has to be remembered
    for (i32 k = 0; k < nparams; k++)
      if (!my_recv(c, tmp, &pkt)) break;
    if (nparams) my_recv(c, tmp, &pkt); // EOF
    for (i32 k = 0; k < ncols; k++)
      if (!my_recv(c, tmp, &pkt)) break;
    if (ncols) my_recv(c, tmp, &pkt); // EOF
    ok = true;
    break;
  }
  arena_destroy(tmp);
  if (!ok) {
    if (!c->err[0]) snprintf(c->err, sizeof c->err, "mysql: cannot prepare the statement");
    return nullptr;
  }
  MyStmt *st;
  if (cacheable) {
    if (c->nstmts >= MY_STMT_CACHE) my_stmt_evict_one(c);
    st = &c->stmts[c->nstmts++];
    memcpy(st->sql, sql.p, sql.n);
    st->len = sql.n;
    st->cached = true;
    st->used = ++c->stamp;
  } else {
    // too long to be worth caching: prepared, run and closed in my_exec
    st = arena_zalloc(a, sizeof *st);
    st->len = sql.n;
    st->cached = false;
  }
  st->id = id;
  st->nparams = nparams;
  st->ncols = ncols;
  return st;
}

static u16 my_param_type(WVal v) {
  switch (v.kind) {
  case WV_NULL:
    return MY_TYPE_NULL;
  case WV_F64:
    return MY_TYPE_DOUBLE;
  case WV_BOOL:
    return MY_TYPE_TINY;
  case WV_BLOB:
    return MY_TYPE_BLOB;
  case WV_I64:
  case WV_TEXT:
  default:
    return v.kind == WV_I64 ? MY_TYPE_LONGLONG : MY_TYPE_VAR_STRING;
  }
}

static WRes my_stmt_execute(MyConn *c, MyStmt *st, WQuery *qy, Arena *a) {
  WRes r = {.ok = true};
  Buf b;
  buf_init(&b, a, 64 + (size_t)qy->nparams * 16);
  buf_putc(&b, (char)MY_COM_STMT_EXECUTE);
  my_put_u32(&b, st->id);
  buf_putc(&b, 0); // CURSOR_TYPE_NO_CURSOR
  my_put_u32(&b, 1); // iteration count
  if (st->nparams > 0) {
    size_t nbytes = ((size_t)st->nparams + 7) / 8;
    u8 *nullmap = arena_zalloc(a, nbytes ? nbytes : 1);
    for (u16 i = 0; i < st->nparams && i < (u16)qy->nparams; i++)
      if (qy->params[i].kind == WV_NULL) nullmap[i / 8] |= (u8)(1u << (i % 8));
    buf_write(&b, nullmap, nbytes);
    buf_putc(&b, 1); // new-params-bound: the type of every parameter follows
    for (u16 i = 0; i < st->nparams; i++)
      my_put_u16(&b, i < (u16)qy->nparams ? my_param_type(qy->params[i]) : MY_TYPE_NULL);
    // then the values, in binary, in order
    for (u16 i = 0; i < st->nparams && i < (u16)qy->nparams; i++) {
      WVal v = qy->params[i];
      switch (v.kind) {
      case WV_NULL:
        break; // the NULL bitmap already said so, and a NULL has no bytes
      case WV_I64:
        my_put_u64(&b, (u64)v.i);
        break;
      case WV_BOOL:
        buf_putc(&b, (char)(v.b ? 1 : 0));
        break;
      case WV_F64: {
        u64 bits;
        memcpy(&bits, &v.f, 8);
        my_put_u64(&b, bits);
        break;
      }
      case WV_TEXT:
      case WV_BLOB:
        my_put_lenenc_str(&b, v.s);
        break;
      }
    }
  }
  c->seq = 0;
  if (!my_send(c, b.p, b.n)) {
    r.ok = false;
    r.e = werr(c->cancelled ? WEBC_ECANCELED : WEBC_EDB,
               str_lit(c->err[0] ? c->err : "mysql: the connection is gone"));
    if (!c->cancelled) c->broken = true;
    return r;
  }
  bool more = false;
  if (!my_resultset(c, a, true, &r, &more)) {
    r.ok = false;
    if (r.e.len == 0)
      r.e = werr(c->cancelled ? WEBC_ECANCELED : WEBC_EDB,
                 str_lit(c->err[0] ? c->err : "mysql: the connection is lost"));
    if (!c->cancelled) c->broken = true;
  }
  return r;
}

static WRes my_exec(WConn *c, WQuery *qy, Arena *a) {
  WRes r = {.ok = true};
  MyConn *mc = c->h;
  mc->ctx = c->ctx;
  if (mc->broken || mc->cancelled) {
    r.ok = false;
    r.e = werr(WEBC_EDB, str_lit(mc->err[0] ? mc->err : "mysql: the connection is unusable"));
    r.cls = WDBE_LOST;
    return r;
  }
  // The server-side statement cap, when the request named a deadline: even a client that
  // has gone away then stops costing the server a query (see §B5 of the spec). The units
  // differ between the two servers, and asking the wrong one is an error, not a fallback.
  if (c->ctx) {
    i64 deadline = ctx_deadline_ms(c->ctx);
    i64 left = deadline ? deadline - (i64)wc_time_ms() : -1;
    if (deadline && left > 0) {
      char cap[64];
      if (mc->mariadb)
        snprintf(cap, sizeof cap, "%lld", (long long)(left / 1000 + 1));
      else
        snprintf(cap, sizeof cap, "%lld", (long long)left);
      str set = mc->mariadb
                    ? str_fmt(a, "SET SESSION max_statement_time=%s", cap)
                    : str_fmt(a, "SET SESSION max_execution_time=%s", cap);
      (void)my_query_text(mc, set, a);
    }
  }
  if (qy->nparams == 0) return my_query_text(mc, qy->sql, a);
  MyStmt *st = my_stmt_prepare(mc, qy->sql, a);
  if (!st) {
    r.ok = false;
    r.e = werr(WEBC_EDB, str_lit(mc->err[0] ? mc->err : "mysql: cannot prepare the statement"));
    if (mc->broken) r.cls = WDBE_LOST;
    return r;
  }
  r = my_stmt_execute(mc, st, qy, a);
  if (!st->cached) my_stmt_close(mc, st->id);
  return r;
}

// ---------------------------------------------------------------- authentication
//
// Three plugins, three honest answers:
//
//   mysql_native_password   SHA-1 scramble over the nonce. MariaDB's default, and what a
//                           MySQL 8 user configured with it offers.
//   caching_sha2_password   Fast path: SHA-256 scramble over the nonce (no TLS needed).
//                           Full path: the password in clear — *only over TLS*. MySQL
//                           would take it RSA-encrypted over plaintext, and this build has
//                           no RSA on purpose (adding RSA-OAEP to crypto.c to serve a
//                           database handshake is a security primitive in the wrong place).
//   sha256_password         The same, via the TLS-cleartext path.
//
// `ed25519` (a MariaDB plugin) is refused by name, with the two working alternatives in
// the sentence: a driver that supports "some" plugins should say which.
static bool my_auth_native(Arena *a, str pass, str nonce, str *out) {
  if (pass.n == 0) {
    *out = (str){"", 0};
    return true;
  }
  u8 stage1[20], stage2[20], mix[20], token[20];
  sha1(pass.p, pass.n, stage1);
  sha1(stage1, 20, stage2);
  Buf b;
  buf_init(&b, a, 20 + nonce.n);
  buf_write(&b, nonce.p, nonce.n);
  buf_write(&b, stage2, 20);
  sha1(b.p, b.n, mix);
  for (i32 i = 0; i < 20; i++) token[i] = stage1[i] ^ mix[i];
  char *copy = arena_alloc(a, 20);
  memcpy(copy, token, 20);
  *out = (str){copy, 20};
  return true;
}

static bool my_auth_sha2(Arena *a, str pass, str nonce, str *out) {
  if (pass.n == 0) {
    *out = (str){"", 0};
    return true;
  }
  u8 stage1[32], stage2[32];
  sha256(pass.p, pass.n, stage1);
  sha256(stage1, 32, stage2);
  Buf b;
  buf_init(&b, a, 32 + nonce.n);
  buf_write(&b, stage2, 32);
  buf_write(&b, nonce.p, nonce.n);
  u8 mix[32];
  sha256(b.p, b.n, mix);
  u8 token[32];
  for (i32 i = 0; i < 32; i++) token[i] = stage1[i] ^ mix[i];
  char *copy = arena_alloc(a, 32);
  memcpy(copy, token, 32);
  *out = (str){copy, 32};
  return true;
}

// The first 20 bytes of the auth-plugin-data are the nonce; MySQL sends 21 (with a NUL)
// and MariaDB 20, and a scramble computed over the terminator is a scramble the server
// rejects with "Access denied" and no further explanation.
static str my_nonce(str data) {
  size_t n = data.n;
  while (n > 0 && data.p[n - 1] == 0) n--;
  if (n > 20) n = 20;
  return (str){data.p, n};
}

static bool my_scramble(Arena *a, const char *plugin, str pass, str nonce, str *out,
                        char *err, size_t errcap) {
  if (!plugin || strcmp(plugin, "mysql_native_password") == 0)
    return my_auth_native(a, pass, nonce, out);
  if (strcmp(plugin, "caching_sha2_password") == 0) return my_auth_sha2(a, pass, nonce, out);
  if (strcmp(plugin, "sha256_password") == 0) {
    // no scramble exists: this plugin is "the password, protected by TLS or RSA", and
    // only the first of those is in this build
    snprintf(err, errcap,
             "mysql: sha256_password needs TLS (this build has no RSA) — add ssl-mode=require "
             "to the URL and rebuild with -DWEBC_TLS=openssl");
    return false;
  }
  if (strcmp(plugin, "auth_gssapi_client") == 0 || strcmp(plugin, "ed25519") == 0) {
    snprintf(err, errcap,
             "mysql: the server asks for the '%s' auth plugin, which this driver does not "
             "implement — use a user on mysql_native_password or caching_sha2_password",
             plugin);
    return false;
  }
  snprintf(err, errcap, "mysql: unsupported auth plugin '%s'", plugin);
  return false;
}

// ---------------------------------------------------------------- handshake

typedef struct MyHello {
  str server_version;
  u32 thread_id;
  str nonce1;   // 8 bytes
  str nonce2;   // 12 or 13 bytes, including the trailing NUL
  u32 caps;
  u8 charset;
  str plugin;
} MyHello;

static bool my_parse_hello(str p, MyHello *h) {
  memset(h, 0, sizeof *h);
  if (p.n < 1 || (u8)p.p[0] != 10) return false;
  size_t at = 1;
  size_t start = at;
  while (at < p.n && p.p[at]) at++;
  h->server_version = str_sub(p, start, at);
  at++; // NUL
  if (at + 4 > p.n) return false;
  h->thread_id = my_get_u32((const u8 *)p.p + at);
  at += 4;
  if (at + 8 > p.n) return false;
  h->nonce1 = str_sub(p, at, at + 8);
  at += 8;
  at++; // filler
  if (at + 2 > p.n) return false;
  u32 caps = my_get_u16((const u8 *)p.p + at);
  at += 2;
  if (at >= p.n) { // a very old server stops here
    h->caps = caps;
    return true;
  }
  h->charset = (u8)p.p[at];
  at++;
  at += 2; // status flags
  if (at + 2 > p.n) return false;
  caps |= (u32)my_get_u16((const u8 *)p.p + at) << 16;
  at += 2;
  u8 auth_len = (u8)p.p[at];
  at++;
  at += 10; // reserved
  size_t want = (auth_len > 8 ? (size_t)auth_len - 8 : 13);
  if (want < 13) want = 13;
  if (at + want > p.n) want = p.n - at;
  h->nonce2 = str_sub(p, at, at + want);
  at += want;
  if (at < p.n && (caps & MY_CLIENT_PLUGIN_AUTH)) {
    size_t pl = at;
    while (at < p.n && p.p[at]) at++;
    h->plugin = str_sub(p, pl, at);
  }
  h->caps = caps;
  return true;
}

static MySsl my_ssl_parse(str v) {
  if (str_eq_c(v, "disable")) return MY_SSL_DISABLE;
  if (str_eq_c(v, "require")) return MY_SSL_REQUIRE;
  if (str_eq_c(v, "verify-ca")) return MY_SSL_VERIFY_CA;
  if (str_eq_c(v, "verify-full")) return MY_SSL_VERIFY_FULL;
  return MY_SSL_PREFER; // include `prefer` and anything unrecognised: try, then fall back
}

// `?ssl-mode=require&ssl-ca=/etc/ca.pem` — the postgres grammar with the hyphen MySQL's
// own documentation uses, so one deployment teaches one idea.
static void my_read_query(str q, MyConn *c, bool *mode_set) {
  str key, val;
  for (size_t i = 0; wc_kv_next(q, &i, &key, &val);) {
    if (str_eq_c(key, "ssl-mode") || str_eq_c(key, "ssl_mode")) {
      c->ssl = my_ssl_parse(val);
      *mode_set = true;
    } else if (str_eq_c(key, "ssl-ca") || str_eq_c(key, "ssl_ca")) {
      snprintf(c->ca_file, sizeof c->ca_file, "%.*s", (int)val.n, val.p);
    }
  }
}

static bool my_handshake(MyConn *c, Arena *a) {
  str pkt = {0};
  if (!my_recv(c, a, &pkt)) {
    if (!c->err[0]) snprintf(c->err, sizeof c->err, "mysql: no handshake from the server");
    return false;
  }
  if (pkt.n > 0 && (u8)pkt.p[0] == 0xff) {
    my_error_from(c, pkt);
    return false;
  }
  MyHello h = {0};
  if (!my_parse_hello(pkt, &h)) {
    snprintf(c->err, sizeof c->err, "mysql: the server did not greet with protocol 10");
    return false;
  }
  c->thread_id = h.thread_id;
  c->mariadb = h.server_version.p && str_contains(h.server_version, str_lit("MariaDB"));
  // 8.0.4 -> 80004, 11.1.0-MariaDB -> 110100: the digits before the first dot, zero-padded
  {
    int maj = 0, min = 0, patch = 0;
    if (h.server_version.p) sscanf(h.server_version.p, "%d.%d.%d", &maj, &min, &patch);
    c->server_version = (u32)(maj * 10000 + min * 100 + patch);
  }
  u32 caps = MY_CLIENT_PROTOCOL_41 | MY_CLIENT_LONG_PASSWORD | MY_CLIENT_LONG_FLAG |
             MY_CLIENT_TRANSACTIONS | MY_CLIENT_SECURE_CONNECTION |
             MY_CLIENT_PLUGIN_AUTH | MY_CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA |
             MY_CLIENT_MULTI_STATEMENTS | MY_CLIENT_MULTI_RESULTS |
             MY_CLIENT_PS_MULTI_RESULTS;
  if (c->dbname[0] && (h.caps & MY_CLIENT_CONNECT_WITH_DB))
    caps |= MY_CLIENT_CONNECT_WITH_DB;
  caps &= h.caps | MY_CLIENT_SSL; // ask only for what the server offers

  bool want_tls = c->ssl != MY_SSL_DISABLE;
  if (want_tls && !tls_available()) {
    if (c->ssl == MY_SSL_PREFER) {
      want_tls = false;
    } else {
      snprintf(c->err, sizeof c->err,
               "mysql: ssl-mode=%s needs a TLS build — rebuild with -DWEBC_TLS=openssl, or "
               "set ssl-mode=disable explicitly",
               c->ssl == MY_SSL_REQUIRE  ? "require"
               : c->ssl == MY_SSL_VERIFY_CA ? "verify-ca"
                                           : "verify-full");
      return false;
    }
  }
  if (want_tls && !(h.caps & MY_CLIENT_SSL)) {
    if (c->ssl == MY_SSL_PREFER) {
      want_tls = false;
      log_warn("mysql: server does not offer TLS, continuing in plaintext (ssl-mode=prefer)",
               {"host", str_lit(c->host)});
    } else {
      snprintf(c->err, sizeof c->err, "mysql: %s:%s does not accept TLS and ssl-mode asks for it",
               c->host, c->port);
      return false;
    }
  }
  // CLIENT_SSL belongs in *both* packets: the SSLRequest and the handshake response sent
  // inside TLS. A server that sees it only in the first one answers "malformed packet".
  if (want_tls) caps |= MY_CLIENT_SSL;
  if (want_tls) {
    // The SSLRequest is the first half of a handshake response: capabilities, packet size,
    // charset, 23 reserved bytes. TLS then takes over the same socket, and the real
    // handshake response is written inside it.
    Buf ssl;
    buf_init(&ssl, a, 32);
    my_put_u32(&ssl, caps);
    my_put_u32(&ssl, 16u * 1024 * 1024);
    buf_putc(&ssl, (char)MY_CHARSET_UTF8MB4);
    for (i32 i = 0; i < 23; i++) buf_putc(&ssl, 0);
    c->seq = 1;
    if (!my_send(c, ssl.p, ssl.n)) {
      snprintf(c->err, sizeof c->err, "mysql: could not send the TLS request");
      return false;
    }
    char tls_err[256] = "";
    bool insecure = c->ssl == MY_SSL_REQUIRE || c->ssl == MY_SSL_PREFER;
    bool verify_host = c->ssl == MY_SSL_VERIFY_FULL;
    c->tls = tls_connect_opt(c->fd, c->host, c->ca_file[0] ? c->ca_file : nullptr, verify_host,
                             insecure, tls_err, sizeof tls_err);
    if (!c->tls) {
      if (c->ssl == MY_SSL_PREFER) {
        log_warn("mysql: TLS handshake failed, continuing in plaintext (ssl-mode=prefer)",
                 {"host", str_lit(c->host)}, {"why", str_lit(tls_err)});
        // `prefer` reconnects rather than giving up, exactly as the postgres driver does:
        // the half-written handshake cannot be continued in the clear
        c->tls = nullptr;
        close(c->fd);
        c->fd = wfd_open_connect(c->host, (u16)atoi(c->port), 10000);
        if (c->fd < 0) {
          snprintf(c->err, sizeof c->err, "mysql: cannot connect to %s:%s", c->host, c->port);
          return false;
        }
        return my_handshake(c, a);
      }
      snprintf(c->err, sizeof c->err, "mysql: TLS handshake with %s:%s failed: %s", c->host,
               c->port, tls_err);
      return false;
    }
    c->seq = 2; // the SSLRequest consumed sequence 1
  } else {
    c->seq = 1;
  }

  Buf nb;
  buf_init(&nb, a, 32);
  buf_write(&nb, h.nonce1.p, h.nonce1.n);
  if (h.nonce2.p) buf_write(&nb, h.nonce2.p, h.nonce2.n);
  str nonce = my_nonce(buf_str(&nb));
  const char *plugin = h.plugin.p ? h.plugin.p : "mysql_native_password";
  str auth = {0};
  // caching_sha2 and sha256_password are answered with a scramble now and, if the server
  // answers "full authentication", with the password inside TLS below.
  if (strcmp(plugin, "sha256_password") == 0) {
    if (!c->tls) {
      // Two reasons the server switches to this plugin, and the second one is worth naming:
      // it also asks for sha256_password when the *user does not exist*, so "add TLS" is only
      // half the advice — a typo'd user name reads exactly like a TLS problem.
      snprintf(c->err, sizeof c->err,
               "mysql: the server asked for sha256_password — either the user is on that plugin "
               "and needs TLS on the first connect (add ssl-mode=require to the URL and rebuild "
               "with -DWEBC_TLS=openssl), or mysql does not know this user at all");
      return false;
    }
    auth = (str){c->pass, strlen(c->pass) + 1}; // NUL included: that is the plugin's format
  } else if (!my_scramble(a, plugin, str_lit(c->pass), nonce, &auth, c->err, sizeof c->err)) {
    return false;
  }

  Buf resp;
  buf_init(&resp, a, 128);
  my_put_u32(&resp, caps);
  my_put_u32(&resp, 16u * 1024 * 1024); // max packet size we accept
  buf_putc(&resp, (char)MY_CHARSET_UTF8MB4);
  for (i32 i = 0; i < 23; i++) buf_putc(&resp, 0);
  my_put_cstr(&resp, c->user);
  if (caps & MY_CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)
    my_put_lenenc_str(&resp, auth);
  else {
    buf_putc(&resp, (char)(auth.n > 255 ? 255 : auth.n));
    buf_write(&resp, auth.p, auth.n);
  }
  if (caps & MY_CLIENT_CONNECT_WITH_DB) my_put_cstr(&resp, c->dbname);
  if (caps & MY_CLIENT_PLUGIN_AUTH) my_put_cstr(&resp, plugin);
  if (!my_send(c, resp.p, resp.n)) {
    snprintf(c->err, sizeof c->err, "mysql: could not send the handshake response");
    return false;
  }

  for (;;) {
    if (!my_recv(c, a, &pkt)) {
      if (!c->err[0]) snprintf(c->err, sizeof c->err, "mysql: the server closed during auth");
      return false;
    }
    u8 first = (u8)pkt.p[0];
    if (first == 0x00) return true; // OK: authenticated
    if (first == 0xff) {
      my_error_from(c, pkt);
      return false;
    }
    if (first == 0xfe) { // AuthSwitchRequest: the server picks the plugin after all
      if (getenv("WEBC_DB_DEBUG"))
        fprintf(stderr, "[mysql] auth switch, %zu bytes: %.32s\n", pkt.n, pkt.p + 1);
      size_t at = 1;
      while (at < pkt.n && pkt.p[at]) at++;
      str name = str_sub(pkt, 1, at);
      at++;
      str data = at < pkt.n ? str_sub(pkt, at, pkt.n) : (str){"", 0};
      str n2 = my_nonce(data);
      str reply = {0};
      const char *pname = str_cstr(a, name);
      if (strcmp(pname, "sha256_password") == 0) {
        if (!c->tls) {
          snprintf(c->err, sizeof c->err,
                   "mysql: the server asked for sha256_password — either the user is on that "
                   "plugin and needs TLS on the first connect (add ssl-mode=require), or mysql "
                   "does not know this user at all");
          return false;
        }
        reply = (str){(char *)c->pass, strlen(c->pass) + 1};
      } else if (!my_scramble(a, pname, str_lit(c->pass), n2.n ? n2 : nonce, &reply, c->err,
                              sizeof c->err)) {
        return false;
      }
      if (!my_send_str(c, reply)) return false;
      continue;
    }
    if (first == 0x01) { // AuthMoreData
      u8 code = pkt.n > 1 ? (u8)pkt.p[1] : 0;
      if (code == 0x03) continue; // caching_sha2 fast path succeeded; an OK follows
      if (code == 0x04) {         // full authentication: the password, over TLS only
        if (!c->tls) {
          snprintf(c->err, sizeof c->err,
                   "mysql: caching_sha2_password needs one TLS connection to authenticate this "
                   "user (this build has no RSA) — add ssl-mode=require to the URL and rebuild "
                   "with -DWEBC_TLS=openssl, or use a mysql_native_password user");
          return false;
        }
        str plain = {(char *)c->pass, strlen(c->pass) + 1};
        if (!my_send_str(c, plain)) return false;
        continue;
      }
      // sha256_password asking for a public key: this build refuses rather than
      // implementing RSA-OAEP for one handshake
      snprintf(c->err, sizeof c->err,
               "mysql: the server asked for an RSA key exchange, which this build does not "
               "implement — use TLS (ssl-mode=require) or a mysql_native_password user");
      return false;
    }
    snprintf(c->err, sizeof c->err, "mysql: unexpected packet 0x%02x during auth", first);
    return false;
  }
}

// The session every connection starts in. Strict mode is not decoration: without it MySQL
// silently truncates an over-long string, which would make WDBE_TOO_LONG (422) unreachable
// and let a 200-character title into a `varchar(200)` column.
static bool my_session_setup(MyConn *c, Arena *a) {
  static const char *setup =
      "SET NAMES utf8mb4;"
      "SET SESSION sql_mode='STRICT_TRANS_TABLES,NO_ENGINE_SUBSTITUTION,"
      "ERROR_FOR_DIVISION_BY_ZERO';"
      "SET SESSION time_zone='+00:00';"
      "SET SESSION autocommit=1";
  WRes r = my_query_text(c, str_lit(setup), a);
  bool ok = r.ok;
  if (!ok)
    snprintf(c->err, sizeof c->err, "mysql: session setup failed: %.*s", WERR_FMT(r.e));
  wres_free(&r);
  return ok;
}

// ---------------------------------------------------------------- open / close

// "mysql://user:pass@host:3306/db?ssl-mode=require". A unix socket is `mysql://user:pass@
// localhost/db?socket=/tmp/mysql.sock`, because a container's socket is not in a URL's
// authority section.
static void my_parse_url(MyConn *c, const char *url) {
  snprintf(c->host, sizeof c->host, "127.0.0.1");
  snprintf(c->port, sizeof c->port, "3306");
  snprintf(c->user, sizeof c->user, "root");
  c->ssl = MY_SSL_PREFER;

  WDbUrl u;
  wc_db_url_split(url, &u);
  wc_url_copy(c->user, sizeof c->user, u.user);
  wc_url_copy(c->pass, sizeof c->pass, u.pass);
  wc_url_copy(c->host, sizeof c->host, u.host);
  wc_url_copy(c->port, sizeof c->port, u.port);
  wc_url_copy(c->dbname, sizeof c->dbname, u.db);

  bool mode_set = false;
  my_read_query(u.query, c, &mode_set);
  const char *env_mode = getenv("WEBC_MYSQL_SSLMODE");
  if (env_mode && *env_mode) c->ssl = my_ssl_parse(str_lit(env_mode));
}

// Socket, TLS, handshake, session setup. Split from my_open so the cancel path can open a
// second, short-lived connection with the same grammar (that is the only thing that can
// stop a statement the client no longer wants).
static bool my_connect(MyConn *c, Arena *a) {
  c->fd = wfd_open_connect(c->host[0] ? c->host : "127.0.0.1", (u16)atoi(c->port), 10000);
  if (c->fd < 0) {
    snprintf(c->err, sizeof c->err, "mysql: cannot connect to %s:%s",
             c->host[0] ? c->host : "127.0.0.1", c->port);
    return false;
  }
  if (!my_handshake(c, a)) {
    if (c->tls) tls_close(c->tls);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    return false;
  }
  if (!my_session_setup(c, a)) {
    if (c->tls) tls_close(c->tls);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    return false;
  }
  return true;
}

static WConn *my_open(const char *url, Arena *a, WErrRes *err) {
  MyConn *mc = arena_zalloc(a, sizeof *mc);
  mc->a = a;
  mc->timeout_ms = 30000;
  mc->fd = -1;
  if (getenv("WEBC_DB_DEBUG")) fprintf(stderr, "[mysql] opening %s\n", url ? url : "");
  my_parse_url(mc, url ? url : "");
  if (!my_connect(mc, a)) {
    *err = wfail(WEBC_EDB, str_lit(mc->err[0] ? mc->err : "mysql: cannot open a connection"));
    return nullptr;
  }
  WConn *c = arena_zalloc(a, sizeof *c);
  c->drv = &wc_driver_mysql;
  c->h = mc;
  return c;
}

static void my_close(WConn *c) {
  if (!c || !c->h) return;
  MyConn *mc = c->h;
  if (mc->fd >= 0 && !mc->broken) {
    mc->seq = 0;
    u8 quit = MY_COM_QUIT;
    (void)my_send(mc, &quit, 1);
  }
  if (mc->tls) {
    tls_close(mc->tls);
    mc->tls = nullptr;
  }
  if (mc->fd >= 0) {
    close(mc->fd);
    mc->fd = -1;
  }
}

// ---------------------------------------------------------------- cancellation
//
// MySQL has no out-of-band cancel packet: the way to stop a statement is another
// connection issuing `KILL QUERY <thread id>`. That connection is opened outside the pool
// (so a busy pool can never make a cancel wait), used once, and closed.
static void my_kill_query(MyConn *c) {
  if (!c || c->cancelled || c->fd < 0 || c->thread_id == 0) return;
  c->cancelled = true;
  snprintf(c->err, sizeof c->err, "statement cancelled: %s",
           c->ctx && ctx_err(c->ctx).n ? ctx_err(c->ctx).p : "the request ended");
  Arena *a = arena_new(4096);
  MyConn *k = arena_zalloc(a, sizeof *k);
  k->a = a;
  k->fd = -1;
  snprintf(k->host, sizeof k->host, "%s", c->host);
  snprintf(k->port, sizeof k->port, "%s", c->port);
  snprintf(k->user, sizeof k->user, "%s", c->user);
  snprintf(k->pass, sizeof k->pass, "%s", c->pass);
  snprintf(k->dbname, sizeof k->dbname, "%s", c->dbname);
  snprintf(k->ca_file, sizeof k->ca_file, "%s", c->ca_file);
  k->ssl = c->ssl;
  if (k->ssl == MY_SSL_DISABLE) k->ssl = MY_SSL_PREFER; // the kill only needs to get in
  if (my_connect(k, a)) {
    str sql = str_fmt(a, "KILL QUERY %u", (unsigned)c->thread_id);
    WRes r = my_query_text(k, sql, a);
    wres_free(&r);
  } else if (getenv("WEBC_DB_DEBUG")) {
    fprintf(stderr, "[mysql] cancel connection failed: %s\n", k->err);
  }
  WConn kc = {.h = k};
  my_close(&kc);
  arena_destroy(a);
}

// ---------------------------------------------------------------- transactions

static bool my_simple(MyConn *c, const char *sql) {
  Arena *a = arena_new(1024);
  WRes r = my_query_text(c, str_lit(sql), a);
  bool ok = r.ok;
  wres_free(&r);
  arena_destroy(a);
  return ok;
}

static bool my_begin(WConn *c) {
  MyConn *mc = c->h;
  bool ok = my_simple(mc, "START TRANSACTION");
  if (ok) mc->in_tx = true;
  return ok;
}
static bool my_commit(WConn *c) {
  MyConn *mc = c->h;
  bool ok = my_simple(mc, "COMMIT");
  if (ok) mc->in_tx = false;
  return ok;
}
static bool my_rollback(WConn *c) {
  MyConn *mc = c->h;
  bool ok = my_simple(mc, "ROLLBACK");
  if (ok) mc->in_tx = false;
  return ok;
}

// Nested scopes are savepoints named by depth, the same convention sqlite and postgres
// use, so `tx_scope` inside `tx_scope` behaves identically on all three servers.
static bool my_savepoint(WConn *c, i32 depth) {
  char buf[64];
  snprintf(buf, sizeof buf, "SAVEPOINT wc_sp_%d", (int)depth);
  return my_simple(c->h, buf);
}
static bool my_release_savepoint(WConn *c, i32 depth) {
  char buf[64];
  snprintf(buf, sizeof buf, "RELEASE SAVEPOINT wc_sp_%d", (int)depth);
  return my_simple(c->h, buf);
}
static bool my_rollback_to(WConn *c, i32 depth) {
  char buf[64];
  snprintf(buf, sizeof buf, "ROLLBACK TO SAVEPOINT wc_sp_%d", (int)depth);
  return my_simple(c->h, buf);
}

static bool my_broken(WConn *c) {
  MyConn *mc = c && c->h ? c->h : nullptr;
  return !mc || mc->broken || mc->cancelled;
}

const WDBDriver wc_driver_mysql = {
    .name = "mysql",
    .placeholder = "?",
    .returning = false,           // the OK packet's `last_insert_id` is the new key
    .offset_needs_limit = true,   // `offset N` alone is a syntax error here
    .ident_quote = '`',           // and `rank`, `groups`, `window` are reserved words
    .multi_statement = true,      // COM_QUERY runs several, which is how DDL files run
    .open = my_open,
    .close = my_close,
    .exec = my_exec,
    .begin = my_begin,
    .commit = my_commit,
    .rollback = my_rollback,
    .savepoint = my_savepoint,
    .release_savepoint = my_release_savepoint,
    .rollback_to = my_rollback_to,
    .broken = my_broken,
};