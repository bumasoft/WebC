// Which database a suite is about, and the three DDL spellings that actually differ.
//
// `make test` must run offline, so the default is an in-memory sqlite. `WEBC_TEST_DB=mysql://…`
// (or postgres://…) runs the very same file — same model, same filter builder, same promises —
// against a server, which is what `make mysql-test` does for the ORM suites. Without this, the
// SQL the query builder emits for aggregates, `in (select …)` and windows had only ever been
// parsed by sqlite.
#ifndef WEBC_TESTS_TEST_DB_H
#define WEBC_TESTS_TEST_DB_H

#include <webc/test.h>

static inline const char *wc_test_url(void) {
  const char *u = getenv("WEBC_TEST_DB");
  return u && *u ? u : "sqlite::memory:";
}

// Open the database this run is about and make it the default, since `db_save`/`db_all` go
// through the default handle.
static inline WDB *wc_test_open(void) {
  WDB *db = db_open(wc_test_url());
  if (db) db_set_default(db);
  return db;
}

static inline bool wc_test_is_mysql(WDB *db) {
  return str_eq_c(db_driver_name(db), "mysql");
}

// The three spellings that differ. A `str` field needs a length on mysql, because the driver
// indexes it — `text` cannot be a key there without a prefix length.
static inline const char *wc_test_pk(WDB *db) {
  return wc_test_is_mysql(db) ? "bigint auto_increment primary key"
                              : "integer primary key autoincrement";
}
static inline const char *wc_test_text(WDB *db) {
  return wc_test_is_mysql(db) ? "varchar(190)" : "text";
}
// A column may be called `group`/`order`/`rank`; the quote belongs to the dialect.
static inline const char *wc_test_q(WDB *db) {
  return wc_test_is_mysql(db) ? "`" : "\"";
}
static inline const char *wc_test_flag(WDB *db) {
  return wc_test_is_mysql(db) ? "tinyint(1)" : "integer";
}

// A server keeps its tables between tests, an in-memory sqlite does not: every suite drops
// what it is about to create, so a run that aborted halfway cannot fail the next one.
static inline void wc_test_drop(WDB *db, const char *table) {
  char sql[256];
  snprintf(sql, sizeof sql, "drop table if exists %s", table);
  WRes r = db_exec_sql(db, sql);
  wres_free(&r);
}

#endif // WEBC_TESTS_TEST_DB_H