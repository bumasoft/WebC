// A driver error is a *kind*, not a message.
//
// A status picked by looking for "23505" or "UNIQUE constraint" in the server's text is
// true of sqlite and postgres and false of the third server this framework speaks. Each
// driver classifies its own codes into WDbErrKind instead, and this is the file that says
// the classes are right.
#include <webc/test.h>

WEBC_MODEL(ErrRow, (id, i64, .pk = true), (email, str, .unique = true, .required = true),
                  (name, str, .required = true))
WEBC_VEC_DEFINE(VecErrRow, vec_err_row, ErrRow)

static WDB *fresh(void) {
  WDB *db = db_open("sqlite::memory:");
  ASSERT_NOT_NULL(db);
  db_set_default(db);
  ASSERT_TRUE(db_exec_sql(db,
                          "create table err_rows (id integer primary key autoincrement, "
                          "email text unique not null, name text not null)").ok);
  ErrRow a = {.email = str_lit("a@example.com"), .name = str_lit("A")};
  ASSERT_TRUE(db_insert(db, ErrRow, &a));
  return db;
}

TEST(a_duplicate_is_a_conflict) {
  WDB *db = fresh();
  ErrRow dup = {.email = str_lit("a@example.com"), .name = str_lit("again")};
  WRes r = db_exec_model(db, "insert", ErrRow_modelof(), &dup);
  ASSERT_FALSE(r.ok);
  ASSERT_EQ(r.cls, WDBE_CONFLICT);
  wres_free(&r);
  db_close(db);
}

TEST(a_missing_required_value_is_a_not_null) {
  WDB *db = fresh();
  // A NULL bound for a NOT NULL column. Written as SQL rather than through the model: an
  // unset `str` field binds the empty string, which a NOT NULL column accepts — the empty
  // string is a value, and "required" on a form is the form layer's rule, not the column's.
  WQuery *qy = q("insert into err_rows (email, name) values (:e, :n)");
  q_cstr(qy, "e", "b@example.com");
  q_null(qy, "n");
  WRes r = db_exec(db, qy);
  ASSERT_FALSE(r.ok);
  ASSERT_EQ(r.cls, WDBE_NOT_NULL);
  wres_free(&r);
  db_close(db);
}

TEST(a_reference_to_nothing_is_a_bad_reference) {
  WDB *db = db_open("sqlite::memory:");
  ASSERT_NOT_NULL(db);
  db_set_default(db);
  ASSERT_TRUE(db_exec_sql(db, "pragma foreign_keys = on").ok);
  ASSERT_TRUE(db_exec_sql(db, "create table parents (id integer primary key)").ok);
  ASSERT_TRUE(db_exec_sql(db,
                          "create table kids (id integer primary key, "
                          "parent integer not null references parents(id))").ok);
  WRes r = db_exec_sql(db, "insert into kids (parent) values (7)");
  ASSERT_FALSE(r.ok);
  ASSERT_EQ(r.cls, WDBE_BAD_REFERENCE);
  wres_free(&r);
  db_close(db);
}

TEST(a_check_constraint_is_a_check) {
  WDB *db = db_open("sqlite::memory:");
  ASSERT_NOT_NULL(db);
  db_set_default(db);
  ASSERT_TRUE(db_exec_sql(db,
                          "create table checked (id integer primary key, n integer check (n > 0))")
                  .ok);
  WRes r = db_exec_sql(db, "insert into checked (n) values (0)");
  ASSERT_FALSE(r.ok);
  ASSERT_EQ(r.cls, WDBE_CHECK);
  wres_free(&r);
  db_close(db);
}

TEST(an_unclassifiable_error_is_none_and_stays_a_500) {
  WDB *db = fresh();
  WRes r = db_exec_sql(db, "select * from no_such_table");
  ASSERT_FALSE(r.ok);
  ASSERT_EQ(r.cls, WDBE_NONE);
  wres_free(&r);
  db_close(db);
}

TEST(a_url_with_an_unknown_scheme_names_the_schemes_that_exist) {
  // A `postgre://…` URL opened as a *sqlite file* with that name shows up as an empty table
  // three screens later; the refusal has to name the schemes that exist.
  WDB *db = db_open("postgre://user@host/db");
  ASSERT_NULL(db);
  ASSERT_STR_CONTAINS(db_error(db), "no driver");
}

TEST_MAIN()
