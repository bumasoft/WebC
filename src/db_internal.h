// WebC — database layer internals shared by the drivers.
#ifndef WEBC_DB_INTERNAL_H
#define WEBC_DB_INTERNAL_H

#include <webc/db.h>

#include "internal.h"

typedef struct WConn {
  const struct WDBDriver *drv;
  void *h;
  // The request's context while a statement runs on this connection: the driver parks on
  // the socket with a bounded wait and stops when the deadline passes, so `mw_timeout_ms()`
  // is true for a query and not only for a handler that computes.
  struct WCtx *ctx;
  // A connection's own arena. NOT the database's: connections are opened by whichever task
  // finds the slot empty, i.e. concurrently, and an arena is a bump allocator with no lock.
  // Sharing one garbled the protocol messages of two connects at once ("expected SASL
  // continue" from a server that had said something else entirely).
  Arena *arena;
} WConn;

typedef struct WDBDriver {
  const char *name;
  const char *placeholder;   // "?" or "$"
  // ---------------------------------------------------------------- dialect facts
  //
  // The SQL generator must not *infer* a dialect from something a driver happens to
  // look like: deciding whether to append `returning` by sniffing the placeholder
  // character (`'$'` means postgres) worked exactly as long as there were two
  // drivers. These four flags are the capabilities the generator actually asks
  // about, so a fourth driver is a table entry and not a hunt through `db.c`.
  bool returning;           // an insert can ask for its key in the statement
  bool offset_needs_limit;  // `offset N` alone is a syntax error (mysql)
  char ident_quote;         // '`' | '"'; 0 leaves generated identifiers bare
  bool multi_statement;     // db_exec_sql may carry several statements
  WConn *(*open)(const char *url, Arena *a, WErrRes *err);
  void   (*close)(WConn *c);
  WRes   (*exec)(WConn *c, WQuery *qy, Arena *a);
  bool   (*begin)(WConn *c);
  bool   (*commit)(WConn *c);
  bool   (*rollback)(WConn *c);
  // Nested scopes are savepoints named by their depth, so "the innermost open scope of this
  // session" is a number and not a search: SAVEPOINT wc_sp_<n> / RELEASE / ROLLBACK TO.
  bool   (*savepoint)(WConn *c, i32 depth);
  bool   (*release_savepoint)(WConn *c, i32 depth);
  bool   (*rollback_to)(WConn *c, i32 depth);
  // True when the connection can no longer be used (a lost socket, a cancelled statement
  // whose server-side work is gone). The pool then recycles the slot instead of handing a
  // dead connection to the next request.
  bool   (*broken)(WConn *c);
} WDBDriver;

extern const WDBDriver wc_driver_sqlite;
extern const WDBDriver wc_driver_pg;
extern const WDBDriver wc_driver_mysql;

// Every driver's name, in the order `webc check` and the migration listing use. A
// `<name>.sql` twin of a migration is derived from this list, so adding a driver does
// not leave a hardcoded pair of `strstr`s behind (SPEC part C3).
extern const char *const WC_DB_DRIVERS[];
extern const i32 WC_DB_DRIVER_N;

// The column name as SQL the generator may emit: quoted with the driver's own quote
// character when it declared one, bare otherwise, and never quoted when the field
// carries a `.sql("...")` override — that override is an expression the app wrote,
// and wrapping it in quotes would turn `coalesce(a, '')` into a column named that.
str db_ident(const WDBDriver *drv, const WField *f);
str db_ident_c(const WDBDriver *drv, Arena *a, const char *name);
str db_table(const WDBDriver *drv, const WModel *m);

// Rewrite :name placeholders into the driver's positional form, filling
// qy->params in the order the placeholders appear.
WQuery *db_bind_named(WQuery *qy, const char *placeholder);
WVal    db_val_from_str(Arena *a, str s);

// ---------------------------------------------------------------- the connection string
//
// One splitter for every driver (db_url.c): a field is empty (`n == 0`) when the URL does
// not carry it, and every slice points into the caller's string.
typedef struct WDbUrl {
  str user, pass, host, port, db, query;
} WDbUrl;

void wc_db_url_split(const char *url, WDbUrl *u);

// A field copied into a driver's fixed buffer, keeping that buffer's default when the URL
// left the field out.
static inline void wc_url_copy(char *dst, size_t cap, str v) {
  if (v.p && v.n) snprintf(dst, cap, "%.*s", (int)v.n, v.p);
}

#endif // WEBC_DB_INTERNAL_H