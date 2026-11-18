// db: sqlite CRUD through the model layer, named params, transactions, migrations.
#include <unistd.h> // getpid, unlink, rmdir: system headers first — task.h owns 'select'
#include <webc/test.h>

WEBC_MODEL(Account, (id, i64, .pk = true),
           (email, str, .unique = true, .required = true),
           (balance, i64))

WEBC_VEC_DEFINE(VecAccount, vec_account, Account)

// Each test gets its own database: state shared between tests is state nobody
// can debug later.
static WDB *fresh_db(void) {
  WDB *db = db_open("sqlite::memory:");
  if (!db) return nullptr;
  WRes ddl = db_exec_sql(db, "create table accounts (id integer primary key autoincrement,"
                               " email text unique not null, balance integer not null default 0)");
  if (!ddl.ok) {
    db_close(db);
    return nullptr;
  }
  return db;
}

static bool add_account(WDB *db, const char *email, i64 balance) {
  Account a = { .email = str_lit(email), .balance = balance };
  return db_insert_into(db, Account_modelof(), &a, nullptr);
}

// A file database whose directory does not exist yet must not answer "cannot open"
// at the first request — a fresh clone of an example, a data/ wiped by a clean rule.
// The directory is not a decision the app made (the scaffold makes it for new apps);
// the runtime makes it too, and the first request just works.
TEST(a_file_database_creates_its_missing_directory) {
  char dir[256], url[320], path[300];
  snprintf(dir, sizeof dir, "/tmp/.webc_db_dirs_%d/one/two", (int)getpid());
  snprintf(path, sizeof path, "%s/dev.db", dir);
  unlink(path);
  rmdir(dir); // the tail; the rest is the runtime's to make
  snprintf(url, sizeof url, "sqlite:%s", path);
  WDB *db = db_open(url);
  ASSERT_NOT_NULL((void *)db);
  if (!db) return;
  ASSERT_TRUE(db_exec_sql(db, "create table t (id integer primary key)").ok);
  ASSERT_TRUE(db_exec_sql(db, "insert into t (id) values (7)").ok);
  WRes r = db_exec_sql(db, "select id from t");
  ASSERT_TRUE(r.ok && r.nrows == 1 && row_i64(&r.rows[0], "id") == 7);
  wres_free(&r);
  db_close(db);
  unlink(path);
}

TEST(driver_and_ddl) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  ASSERT_STR_EQ(db_driver_name(db), "sqlite");
  db_close(db);
}

TEST(insert_and_read_back) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);

  Account a = { .email = str_lit("ada@example.com"), .balance = 100 };
  i64 id = 0;
  ASSERT(db_insert_into(db, Account_modelof(), &a, &id));
  ASSERT_EQ(id, 1);
  ASSERT(add_account(db, "grace@example.com", 50));

  VecAccount *all = db_all(db, Account);
  ASSERT_EQ(vec_account_len(all), 2);
  ASSERT_STR_EQ(vec_account_ref(all, 0)->email, "ada@example.com");
  ASSERT_EQ(vec_account_ref(all, 1)->balance, 50);
  db_close(db);
}

TEST(update_goes_through_the_pk) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  ASSERT(add_account(db, "ada@example.com", 100));
  ASSERT(add_account(db, "grace@example.com", 50));

  Account a = { .id = 1, .email = str_lit("ada@example.com"), .balance = 175 };
  ASSERT(db_update(db, Account, &a));
  VecAccount *again = db_all(db, Account);
  ASSERT_EQ(vec_account_len(again), 2);
  ASSERT_EQ(vec_account_ref(again, 0)->balance, 175);
  db_close(db);
}

TEST(named_parameters_in_any_order) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  ASSERT(add_account(db, "ada@example.com", 100));
  ASSERT(add_account(db, "grace@example.com", 50));

  WQuery *qy = q("select count(*) as n from accounts where balance >= :min and email != :skip");
  q_str(qy, "skip", str_lit("nobody@example.com"));   // out of order on purpose
  q_i64(qy, "min", 60);
  WRes r = db_exec(db, qy);
  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.nrows, 1);
  ASSERT_EQ(row_i64(&r.rows[0], "n"), 1);
  ASSERT_STR_EQ(row_str(&r.rows[0], "n"), "");
  db_close(db);
}

TEST(a_failed_statement_does_not_poison_the_pool) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  WRes bad = db_exec_sql(db, "select * from nope");
  ASSERT_FALSE(bad.ok);
  ASSERT_GT(db_error(db).n, 0);
  ASSERT(db_exec_sql(db, "select 1 as one").ok);
  db_close(db);
}

TEST(transactions_roll_back) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  ASSERT(add_account(db, "ada@example.com", 100));
  ASSERT(add_account(db, "grace@example.com", 50));

  db_begin(db);
  ASSERT(add_account(db, "temp@example.com", 1));
  db_rollback(db);
  ASSERT_EQ(vec_account_len(db_all(db, Account)), 2);
  db_close(db);
}

TEST(unique_constraints_surface_as_errors) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  ASSERT(add_account(db, "ada@example.com", 100));
  ASSERT_FALSE(add_account(db, "ada@example.com", 5));
  db_close(db);
}

TEST(delete_by_pk) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  ASSERT(add_account(db, "ada@example.com", 100));
  ASSERT(add_account(db, "grace@example.com", 50));
  ASSERT(db_delete(db, Account, 2));
  ASSERT_EQ(vec_account_len(db_all(db, Account)), 1);
  db_close(db);
}

// `:name` is rewritten to `?`/`$n`, but only where a placeholder can live. A colon
// inside a string literal, a quoted identifier or a comment is part of the SQL:
// postgres's `to_char(now(), 'HH24:MI:SS')` became `'HH24$1MI$2SS'` and the driver
// then demanded two parameters it was never given.
TEST(colons_inside_literals_survive_rewriting) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  WQuery *qy = q("select ':not_a_param' as lit, :real as v -- :nor :this\n"
                 "/* :nor :this */");
  q_i64(qy, "real", 7);
  WRes r = db_exec(db, qy);
  ASSERT(r.ok);
  ASSERT_EQ(r.nrows, 1);
  ASSERT_STR_EQ(row_str(&r.rows[0], "lit"), ":not_a_param");   // left alone
  ASSERT_EQ(row_i64(&r.rows[0], "v"), 7);                      // bound
  wres_free(&r);

  // the shape that broke a postgres migration: a time format in a literal
  WRes t = db_exec_sql(db, "select strftime('%H:%M', 'now') as t");
  ASSERT(t.ok);
  ASSERT_STR_CONTAINS(row_str(&t.rows[0], "t"), ":");
  wres_free(&t);
  db_close(db);
}

// A query that fails allocates its error message in the query's arena. From
// str_fmt(nullptr, …) it is a few bytes leaked per failure, which is a slow leak in a
// server whose error path runs whenever someone asks for a missing row.
TEST(a_failing_query_does_not_leak) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  Arena *a = arena_new(16 * 1024);
  WRes first = db_where(db, Account, "nope = :x", nullptr);
  ASSERT_FALSE(first.ok);
  wres_free(&first);
  WmStats before = wm_stats();
  for (i32 i = 0; i < 200; i++) {
    Arena *qa = arena_new(8192);
    Arena *prev = wc_arena_swap(qa);
    WRes r = db_where(db, Account, "nope = :x", nullptr);
    ASSERT_FALSE(r.ok);
    wres_free(&r);
    wc_arena_swap(prev);
    arena_destroy(qa);
  }
  WmStats after = wm_stats();
  arena_destroy(a);
  db_close(db);
  ASSERT_EQ(after.heap_used, before.heap_used);
}

// ---------------------------------------------------------------- typed filters

TEST(typed_filters_build_the_clause_and_bind_the_values) {
  WDB *db = fresh_db();
  ASSERT_NOT_NULL(db);
  add_account(db, "a@x", 10);
  add_account(db, "b@x", 0);
  add_account(db, "c@x", 30);

  // The field name is checked by the compiler, the column name comes from the model,
  // and the value is bound: the three things a hand-written
  // "balance > 0 order by balance desc" string does not give you. The column is quoted the
  // way the database in play expects — `rank` and `group` are reserved words in MySQL, and a
  // filter that spelled them bare was a syntax error on one of the three and fine on the rest.
  WWhere w;
  ww_init(&w, nullptr);
  ww_gt_i64(&w, Account, balance, 0);
  ww_order(&w, Account, balance, true);
  ASSERT_STR_EQ(ww_sql(&w), "\"balance\" > :p0 order by \"balance\" desc");
  WRes r = db_select(db, Account, &w);
  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.nrows, 2);
  ASSERT_STR_EQ(row_str(&r.rows[0], "email"), "c@x");
  ww_free(&w);
  db_close(db);
}

TEST(a_numeric_filter_on_a_text_column_is_a_loud_mistake) {
  WWhere w;
  ww_init(&w, nullptr);
  // an integer bound against a text column matches nothing, so this is a panic rather
  // than a query that quietly answers zero rows
  ASSERT_PANICS(ww_eq_i64(&w, Account, email, 1));
  ww_free(&w);
}

// The one silent wrong answer a list query can produce: a whole `select …` where a clause
// belongs. It compiles, it runs, and the statement it builds matches nothing — so the page
// says "No posts match" over six existing posts, with no error to follow.
TEST(a_statement_where_a_clause_belongs_is_refused) {
  WDB *db = db_open("sqlite::memory:");
  ASSERT_NOT_NULL(db);
  ASSERT_PANICS(db_all_where(db, Account, "select * from account", nullptr));
  ASSERT_PANICS(db_all_where(db, Account, "  SELECT * from account", nullptr));
  // ...and a subquery inside a real clause is not the mistake: the guard reads the first
  // word only
  VecAccount *v = db_all_where(db, Account,
                               "where 1 = 1 and email in (select email from account where email "
                               "like '%@x')",
                               nullptr);
  ASSERT_NOT_NULL(v);
  db_close(db);
}

TEST_MAIN()
// ---------------------------------------------------------------- embedded migrations

// The `#embed` form: the schema is compiled into the binary, so a deploy cannot
// ship without it (SPEC §6.5). This is the smallest possible migration — the point
// is the mechanism, not the table.
static const char MIG_EMBED_1[] = "create table if not exists embed_probe (id integer primary key, note text);";
static const char MIG_EMBED_2[] = "alter table embed_probe add column extra text;";
static const WMig EMBEDDED[] = {
    {"9001_embed_probe.sql", MIG_EMBED_1},
    {"9002_embed_extra.sql", MIG_EMBED_2},
};

TEST(embedded_migrations_apply_once) {
  WDB *db = db_open("sqlite::memory:");
  ASSERT_TRUE(db != nullptr);
  ASSERT_TRUE(migrations_apply_embedded(db, EMBEDDED, (i32)WC_COUNTOF(EMBEDDED)));
  // the schema is there
  ASSERT_OK(db_exec_sql(db, "insert into embed_probe (note, extra) values ('hi', 'x')"));
  // and running it again applies nothing: a second pass must be a no-op
  ASSERT_TRUE(migrations_apply_embedded(db, EMBEDDED, (i32)WC_COUNTOF(EMBEDDED)));
  WRes r = db_exec_sql(db, "select count(*) from embed_probe");
  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.nrows, 1);
  db_close(db);
}

TEST(embedded_migrations_pick_the_driver_twin) {
  static const WMig twinned[] = {
      {"9003_twin.sql", "create table twin_default (a);"},
      {"9003_twin.sqlite.sql", "create table twin_from_twin (a);"},
  };
  const char *sql = migration_for_driver(twinned, (i32)WC_COUNTOF(twinned), "9003_twin.sql",
                                         "sqlite");
  ASSERT_STR_EQ(str_lit(sql), "create table twin_from_twin (a);");
  // no twin for this driver: the plain one is used
  ASSERT(migration_for_driver(twinned, (i32)WC_COUNTOF(twinned), "9003_twin.sql", "postgres") ==
         nullptr);
}
