// db_query_own: who owns a query's arena, and what happens when the same query is used
// twice. `q()` allocates, `db_exec()` frees, and getting that wrong aborts the process on
// the *next* request with `malloc: pointer being freed was not allocated` — which reads like
// a database or a threading problem, not like "this query is gone".
#include <webc/test.h>

// db_find() uses the default handle
void db_default_set(WDB *db);

WEBC_MODEL(Note, (id, i64, .pk = true), (text, str))

WEBC_VEC_DEFINE(VecNote, vec_note, Note)

static WDB *fresh_db(void) {
  WDB *db = db_open("sqlite::memory:");
  if (!db) return nullptr;
  WRes ddl = db_exec_sql(db, "create table notes (id integer primary key autoincrement,"
                               " text text not null)");
  if (!ddl.ok) {
    db_close(db);
    return nullptr;
  }
  db_exec_sql(db, "insert into notes (text) values ('one')");
  return db;
}

TEST(a_query_that_ran_is_released_and_says_so) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  WQuery *qy = q("select * from notes");
  WRes r = db_exec(db, qy);
  ASSERT_OK(r);
  wres_free(&r);
  // using it again is a sentence with the SQL in it, not a double free on a later request
  ASSERT_PANICS({ db_exec(db, qy); });
  db_close(db);
}

TEST(q_free_after_db_exec_is_harmless) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  WQuery *qy = q("select * from notes");
  WRes r = db_exec(db, qy);
  ASSERT_OK(r);
  wres_free(&r);
  q_free(qy); // was: SIGABRT on the next request
  db_close(db);
}

TEST(a_query_that_never_ran_is_still_ours_to_free) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  WQuery *qy = q("select * from notes");
  q_free(qy);
  q_free(qy); // twice is a no-op, not a crash
  db_close(db);
}

TEST(db_find_releases_its_query) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  db_default_set(db);
  WQuery *qy = q("select * from notes where id = :id");
  q_i64(qy, "id", 1);
  Note *n = db_find(Note, "id = :id", qy);
  ASSERT_NOT_NULL(n);
  ASSERT_STR_EQ(n->text, "one");
  ASSERT_PANICS({ db_exec(db, qy); }); // db_find released it
  db_close(db);
}

TEST_MAIN()