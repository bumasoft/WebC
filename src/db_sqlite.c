// SPDX-License-Identifier: Apache-2.0
// WebC — sqlite driver (the development default, and the one `webc test` uses).
#include "db_internal.h"

#include <sqlite3.h>
#include <sys/stat.h>

// sqlite answers "why did this fail" with an extended result code, and those codes are
// the only accurate reading of a constraint violation: the message is prose, the code is
// a number. The five that map to a status an app acts on are classified here; everything
// else is WDBE_NONE and becomes a 500 with the server's own words.
static WDbErrKind sqlite_class(i32 rc) {
  // the extended codes first: they are what says *which* constraint, and they are what
  // sqlite3_extended_errcode() answers
  switch (rc) {
  case SQLITE_CONSTRAINT_UNIQUE:
  case SQLITE_CONSTRAINT_PRIMARYKEY:
  case SQLITE_CONSTRAINT_ROWID:
    return WDBE_CONFLICT;
  case SQLITE_CONSTRAINT_FOREIGNKEY:
    return WDBE_BAD_REFERENCE;
  case SQLITE_CONSTRAINT_NOTNULL:
    return WDBE_NOT_NULL;
  case SQLITE_CONSTRAINT_CHECK:
    return WDBE_CHECK;
  case SQLITE_CONSTRAINT_TRIGGER:
  case SQLITE_CONSTRAINT_DATATYPE:
    return WDBE_CONFLICT;
  default:
    break;
  }
  // then the base codes, for a build where extended codes were never switched on
  switch (rc & 0xff) {
  case SQLITE_CONSTRAINT:
    return WDBE_CONFLICT;
  case SQLITE_BUSY:
  case SQLITE_LOCKED:
    return WDBE_DEADLOCK;
  case SQLITE_TOOBIG:
    return WDBE_TOO_LONG;
  case SQLITE_IOERR:
  case SQLITE_CANTOPEN:
  case SQLITE_CORRUPT:
  case SQLITE_NOTADB:
    return WDBE_LOST;
  default:
    return WDBE_NONE;
  }
}

typedef struct SqliteConn {
  sqlite3 *h;
} SqliteConn;

// A file database lives in a directory somebody was supposed to create: `webc new`
// scaffolds one, but a fresh clone of an example, an app run from another directory,
// or a data/ wiped by a clean rule all present as "cannot open" at the first request —
// a panic with a message that names the wrong suspect. The directory is not a
// decision the app made; the runtime makes it, one mkdir per missing component.
static void ensure_parent_dirs(const char *path) {
  if (!path || !*path || !strchr(path, '/')) return;
  char buf[1024];
  snprintf(buf, sizeof buf, "%s", path);
  for (char *p = buf + 1; *p; p++) {
    if (*p != '/') continue;
    *p = 0;
    mkdir(buf, 0755); // EEXIST is fine: only the missing tail is created
    *p = '/';
  }
}

static WConn *sqlite_open(const char *url, Arena *a, WErrRes *err) {
  const char *path = url;
  if (strncmp(path, "sqlite:", 7) == 0) path += 7;
  if (strncmp(path, "file:", 5) == 0) path += 5;
  if (*path == 0) path = ":memory:";
  if (strcmp(path, ":memory:") != 0) ensure_parent_dirs(path);

  SqliteConn *sc = arena_zalloc(a, sizeof *sc);
  i32 flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(path, &sc->h, flags, nullptr) != SQLITE_OK) {
    *err = wfail(WEBC_EDB, str_fmt(nullptr, "sqlite: cannot open %s: %s", path,
                                   sc->h ? sqlite3_errmsg(sc->h) : "unknown"));
    return nullptr;
  }
  sqlite3_busy_timeout(sc->h, 3000);
  sqlite3_extended_result_codes(sc->h, 1); // see sqlite_class(): the code, not the prose
  sqlite3_exec(sc->h, "pragma journal_mode=wal; pragma foreign_keys=on;", nullptr, nullptr,
               nullptr);
  WConn *c = arena_zalloc(a, sizeof *c);
  c->drv = &wc_driver_sqlite;
  c->h = sc;
  return c;
}

static void sqlite_close(WConn *c) {
  if (!c || !c->h) return;
  SqliteConn *sc = c->h;
  if (sc->h) sqlite3_close(sc->h);
  sc->h = nullptr;
}

static void bind_val(sqlite3_stmt *st, i32 i, WVal v) {
  switch (v.kind) {
  case WV_NULL:
    sqlite3_bind_null(st, i);
    break;
  case WV_I64:
    sqlite3_bind_int64(st, i, v.i);
    break;
  case WV_F64:
    sqlite3_bind_double(st, i, v.f);
    break;
  case WV_BOOL:
    sqlite3_bind_int(st, i, v.b ? 1 : 0);
    break;
  case WV_TEXT:
    sqlite3_bind_text(st, i, v.s.p, (int)v.s.n, SQLITE_TRANSIENT);
    break;
  case WV_BLOB:
    sqlite3_bind_blob(st, i, v.s.p, (int)v.s.n, SQLITE_TRANSIENT);
    break;
  }
}

static WRes sqlite_exec(WConn *c, WQuery *qy, Arena *a) {
  WRes r = {.ok = true};
  SqliteConn *sc = c->h;
  sqlite3_stmt *st = nullptr;
  const char *tail = nullptr;
  i64 affected_total = 0;
  i64 last_id = 0;
  i32 cap_rows = 0;
  WRow *rows = nullptr;

  const char *sql = str_cstr(a, qy->sql);
  // a statement may be several statements; run them all, keep the last result set
  while (sql && *sql) {
    if (sqlite3_prepare_v2(sc->h, sql, -1, &st, &tail) != SQLITE_OK) {
      r.ok = false;
      // the message belongs to the query's arena: a failing query must not leak
      r.e = werr(WEBC_EDB, str_fmt(a, "sqlite: %s", sqlite3_errmsg(sc->h)));
      r.cls = sqlite_class(sqlite3_extended_errcode(sc->h));
      return r;
    }
    if (!st) {
      sql = tail;
      continue;
    }
    for (i32 i = 0; i < qy->nparams; i++)
      bind_val(st, i + 1, qy->params[i]);

    i32 ncols = sqlite3_column_count(st);
    if (!r.keys && ncols > 0) {
      str *cols = arena_alloc(a, (size_t)ncols * sizeof(str));
      for (i32 k = 0; k < ncols; k++) {
        const char *cname = sqlite3_column_name(st, k);
        cols[k] = str_lit(arena_strdup(a, cname ? cname : ""));
      }
      r.keys = cols;
      r.ncols = ncols;
    }
    for (;;) {
      i32 rc = sqlite3_step(st);
      if (rc == SQLITE_ROW) {
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
        row->n = ncols;
        row->vals = arena_alloc(a, (size_t)ncols * sizeof(WVal));
        row->keys = r.keys ? r.keys : arena_alloc(a, (size_t)ncols * sizeof(str));
        for (i32 k = 0; k < ncols; k++) {
          if (!r.keys) {
            // the name lives in the statement; copy before it is finalised
            const char *cname = sqlite3_column_name(st, k);
            row->keys[k] = str_lit(arena_strdup(a, cname ? cname : ""));
          }
          switch (sqlite3_column_type(st, k)) {
          case SQLITE_INTEGER:
            row->vals[k] = (WVal){WV_I64, .i = sqlite3_column_int64(st, k)};
            break;
          case SQLITE_FLOAT:
            row->vals[k] = (WVal){WV_F64, .f = sqlite3_column_double(st, k)};
            break;
          case SQLITE_NULL:
            row->vals[k] = (WVal){WV_NULL};
            break;
          case SQLITE_BLOB: {
            const void *b = sqlite3_column_blob(st, k);
            i32 n = sqlite3_column_bytes(st, k);
            char *copy = arena_alloc(a, (size_t)n + 1);
            memcpy(copy, b, (size_t)n);
            row->vals[k] = (WVal){WV_BLOB, .s = (str){copy, (size_t)n}};
            break;
          }
          default: {
            const char *t = (const char *)sqlite3_column_text(st, k);
            i32 n = sqlite3_column_bytes(st, k);
            char *copy = arena_alloc(a, (size_t)n + 1);
            memcpy(copy, t, (size_t)n);
            copy[n] = 0;
            row->vals[k] = (WVal){WV_TEXT, .s = (str){copy, (size_t)n}};
            break;
          }
          }
        }
        r.ncols = ncols;
      } else if (rc == SQLITE_DONE) {
        break;
      } else {
        r.ok = false;
        // the message belongs to the query's arena: a failing query must not leak
        r.e = werr(WEBC_EDB, str_fmt(a, "sqlite: %s", sqlite3_errmsg(sc->h)));
        r.cls = sqlite_class(sqlite3_extended_errcode(sc->h));
        sqlite3_finalize(st);
        return r;
      }
    }
    // `sqlite3_changes` answers for the statement that just ran, so it is read *after* the
    // step loop, not before it. Reading it before the first step gave the previous statement's
    // count — 0 on a fresh connection — which made every `update … where` that asks "did I win
    // the race" answer no (the durable job queue's claim is the first caller to notice).
    affected_total += sqlite3_changes(sc->h);
    last_id = sqlite3_last_insert_rowid(sc->h);
    sqlite3_finalize(st);
    st = nullptr;
    sql = tail;
  }
  r.rows = rows;
  r.affected = affected_total;
  r.last_id = last_id;
  return r;
}

static bool sqlite_simple(WConn *c, const char *sql) {
  SqliteConn *sc = c->h;
  return sqlite3_exec(sc->h, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

static bool sqlite_begin(WConn *c) {
  return sqlite_simple(c, "begin");
}
static bool sqlite_commit(WConn *c) {
  return sqlite_simple(c, "commit");
}
static bool sqlite_rollback(WConn *c) {
  return sqlite_simple(c, "rollback");
}

// A nested tx_scope is a savepoint named by its depth. sqlite has had them since 3.6.8, and
// naming them by depth means the scope that unwinds first is always the highest.
static char *sp_sql(char *buf, size_t cap, const char *verb, i32 depth) {
  snprintf(buf, cap, "%s wc_sp_%d", verb, (int)depth);
  return buf;
}
static bool sqlite_savepoint(WConn *c, i32 depth) {
  char buf[64];
  return sqlite_simple(c, sp_sql(buf, sizeof buf, "savepoint", depth));
}
static bool sqlite_release_savepoint(WConn *c, i32 depth) {
  char buf[64];
  return sqlite_simple(c, sp_sql(buf, sizeof buf, "release", depth));
}
static bool sqlite_rollback_to(WConn *c, i32 depth) {
  char buf[64];
  snprintf(buf, sizeof buf, "rollback to savepoint wc_sp_%d", (int)depth);
  return sqlite_simple(c, buf);
}

const WDBDriver wc_driver_sqlite = {
    .name = "sqlite",
    .placeholder = "?",
    .returning = false,           // `last_insert_rowid()` is how sqlite reports a new key
    .offset_needs_limit = true,   // sqlite requires LIMIT before OFFSET (`offset 5` alone is
                                  // a syntax error here — postgres is the one that allows it)
    .ident_quote = '"',           // `group`, `order`, `index` are reserved here too, so a
                                  // column named one of them fails at runtime unless it is
                                  // quoted. Quoted lower case is the same identifier.
    .multi_statement = true,      // db_exec_sql runs a whole migration file
    .open = sqlite_open,
    .close = sqlite_close,
    .exec = sqlite_exec,
    .begin = sqlite_begin,
    .commit = sqlite_commit,
    .rollback = sqlite_rollback,
    .savepoint = sqlite_savepoint,
    .release_savepoint = sqlite_release_savepoint,
    .rollback_to = sqlite_rollback_to,
};