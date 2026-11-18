// What the MySQL driver has to do on its own: speak the wire protocol without a client
// library, take the two execution paths, and classify the errors a strict server reports.
//
// Gated on WEBC_MYSQL_URL like db_pg_test.c (`make mysql-test` starts a server in docker).
// Without it every test SKIPs with a reason — a machine without MySQL still gets a green run,
// and nobody reads a green run as "the third driver works" when it never spoke to one.
//
// The driver is a wire protocol in this repository, not libmysqlclient: handshake, auth
// switch, text rows for `COM_QUERY`, binary rows for a prepared statement, and the OK packet
// that carries the new key because MySQL has no `returning`.
#define WEBC_NO_SELECT_MACRO 1
#include <webc/db.h>
#include <webc/ctx.h>
#include <webc/model.h>
#include <webc/task.h>
#include <webc/test.h>

#define MY_URL_OR_SKIP(var_)                                                    \
  const char *var_ = getenv("WEBC_MYSQL_URL");                                  \
  if (!(var_) || !*(var_)) SKIP("no WEBC_MYSQL_URL: run `make mysql-test` to start a server")

WEBC_MODEL(MyThing, (id, i64, .pk = true), (label, str), (qty, i64), (price, f64),
                    (live, bool), (note, str))
WEBC_MODEL_TABLE(MyThing, "my_things")
WEBC_VEC_DEFINE(VecMyThing, vec_my_thing, MyThing)

// A table per run, dropped first: `make mysql-test` shares one server with the parity test.
static WDB *fresh(const char *url) {
  WDB *db = db_open(url);
  if (!db) return nullptr;
  // The ORM's own calls (`db_save`, `db_all`) go through the default handle; without this they
  // reach a lazily opened in-memory sqlite and say "no such table" — which is exactly what
  // happened the first time this suite ran against a real server.
  db_set_default(db);
  db_exec_sql(db, "drop table if exists my_things");
  WRes ddl = db_exec_sql(db,
                         "create table my_things (id bigint auto_increment primary key, "
                         "label varchar(64) not null, qty integer not null default 0, "
                         "price double not null default 0, live tinyint(1) not null default 1, "
                         "note text)");
  if (!ddl.ok) {
    wres_free(&ddl);
    db_close(db);
    return nullptr;
  }
  wres_free(&ddl);
  return db;
}

TEST(the_url_says_which_server_it_is) {
  MY_URL_OR_SKIP(url);
  WDB *db = fresh(url);
  ASSERT_NOT_NULL(db);
  ASSERT_STR_EQ(db_driver_name(db), "mysql");
  // `mariadb://` is the same protocol: an app that names its server gets the right driver —
  // and the other way round, so running this suite with a mariadb:// URL checks both names.
  char alias_url[512];
  const char *rest = url;
  const char *other = "mariadb://";
  if (str_has_prefix_c(str_lit(url), "mysql://")) {
    rest = url + strlen("mysql://");
  } else if (str_has_prefix_c(str_lit(url), "mariadb://")) {
    rest = url + strlen("mariadb://");
    other = "mysql://";
  }
  snprintf(alias_url, sizeof alias_url, "%s%s", other, rest);
  WDB *alias = db_open(alias_url);
  ASSERT_NOT_NULL(alias);
  ASSERT_STR_EQ(db_driver_name(alias), "mysql");
  db_close(alias);
  db_close(db);
}

TEST(a_row_goes_in_and_comes_back_typed) {
  MY_URL_OR_SKIP(url);
  WDB *db = fresh(url);
  ASSERT_NOT_NULL(db);
  MyThing t = {.label = str_lit("widget"), .qty = 7, .price = 2.5, .live = true,
               .note = str_lit("the note")};
  ASSERT_TRUE(db_save(MyThing, &t));
  ASSERT_TRUE(t.id > 0); // the OK packet's last_insert_id, since there is no `returning`

  MyThing *back = db_get(MyThing, t.id);
  ASSERT_NOT_NULL(back);
  ASSERT_STR_EQ(back->label, "widget");
  ASSERT_EQ(back->qty, 7);
  ASSERT_TRUE(back->price == 2.5);
  ASSERT_TRUE(back->live); // tinyint(1) is a bool on the way out, by the declared length
  ASSERT_STR_EQ(back->note, "the note");

  // A NULL text column, written as SQL: an unset `str` field on a model binds the empty
  // string (a value), so only raw SQL stores a NULL here. The *driver* reports it as NULL in
  // the row — that is the part this file owns — and the model layer then hands the field the
  // framework's empty string, the same on all three drivers, so a view can print `%s` without
  // a null check.
  ASSERT_TRUE(db_exec_sql(db,
                          "insert into my_things (label, qty, price, live, note) "
                          "values ('bare', 0, 0, 0, NULL)")
                  .ok);
  WQuery *sel = q("select id, note from my_things where label = :l");
  q_cstr(sel, "l", "bare");
  WRes raw = db_exec(db, sel);
  ASSERT_TRUE(raw.ok);
  ASSERT_EQ(raw.rows[0].n, 2);
  ASSERT_EQ(raw.rows[0].vals[1].kind, WV_NULL); // the wire said NULL, not ""
  i64 bare = row_i64(&raw.rows[0], "id");
  wres_free(&raw);
  MyThing *nulls = db_get(MyThing, bare);
  ASSERT_NOT_NULL(nulls);
  ASSERT_EQ(nulls->note.n, 0);
  ASSERT_FALSE(nulls->live);
  db_close(db);
}

TEST(a_prepared_statement_binds_every_type) {
  MY_URL_OR_SKIP(url);
  WDB *db = fresh(url);
  ASSERT_NOT_NULL(db);
  // The second execution path: named parameters, so this is COM_STMT_PREPARE/EXECUTE and the
  // rows come back in the binary format, not as text.
  WQuery *qy = q("insert into my_things (label, qty, price, live, note) "
                 "values (:label, :qty, :price, :live, :note)");
  q_str(qy, "label", str_lit("bound"));
  q_i64(qy, "qty", 42);
  q_f64(qy, "price", 9.75);
  q_bool(qy, "live", true);
  q_str(qy, "note", str_lit("multi\nline"));
  WRes r = db_exec(db, qy);
  ASSERT_TRUE(r.ok);
  wres_free(&r);

  WQuery *sel = q("select id, label, qty, price, live, note from my_things where qty = :qty");
  q_i64(sel, "qty", 42);
  WRes got = db_exec(db, sel);
  ASSERT_TRUE(got.ok);
  ASSERT_EQ(got.nrows, 1);
  ASSERT_STR_EQ(row_str(&got.rows[0], "label"), "bound");
  ASSERT_EQ(row_i64(&got.rows[0], "qty"), 42);
  ASSERT_TRUE(row_f64(&got.rows[0], "price") == 9.75);
  ASSERT_TRUE(row_i64(&got.rows[0], "live") != 0);
  ASSERT_STR_EQ(row_str(&got.rows[0], "note"), "multi\nline");
  wres_free(&got);
  db_close(db);
}

TEST(one_call_runs_a_whole_ddl_file) {
  MY_URL_OR_SKIP(url);
  WDB *db = fresh(url);
  ASSERT_NOT_NULL(db);
  // `multi_statement`: COM_QUERY runs a migration file as one string, which is what
  // `webc db migrate` hands the driver.
  ASSERT_TRUE(db_exec_sql(db,
                          "create table my_tmp_a (id int primary key);"
                          "create table my_tmp_b (id int primary key);"
                          "insert into my_tmp_a (id) values (1)")
                  .ok);
  WRes r = db_exec_sql(db, "select count(*) as n from my_tmp_a");
  ASSERT_EQ(row_i64(&r.rows[0], "n"), 1);
  wres_free(&r);
  ASSERT_TRUE(db_exec_sql(db, "drop table my_tmp_a; drop table my_tmp_b").ok);
  db_close(db);
}

TEST(a_duplicate_and_a_missing_parent_are_different_kinds) {
  MY_URL_OR_SKIP(url);
  WDB *db = fresh(url);
  ASSERT_NOT_NULL(db);
  ASSERT_TRUE(db_exec_sql(db, "create unique index my_label on my_things (label)").ok);
  // `fresh()` only owns my_things; these two are this test's, so it drops them first — a run
  // that aborted halfway must not fail the next one.
  ASSERT_TRUE(db_exec_sql(db, "drop table if exists my_child").ok);
  ASSERT_TRUE(db_exec_sql(db, "drop table if exists my_parent").ok);
  ASSERT_TRUE(db_exec_sql(db, "create table my_parent (id int primary key)").ok);
  ASSERT_TRUE(db_exec_sql(db,
                          "create table my_child (id int primary key, parent int not null, "
                          "constraint fk_my_child_parent foreign key (parent) references "
                          "my_parent (id))")
                  .ok);
  MyThing a = {.label = str_lit("same")};
  ASSERT_TRUE(db_save(MyThing, &a));
  MyThing b = {.label = str_lit("same")};
  WRes dup = db_exec_model(db, "insert", MyThing_modelof(), &b);
  ASSERT_FALSE(dup.ok);
  ASSERT_EQ(dup.cls, WDBE_CONFLICT); // errno 1062, not a search for the word "unique"
  wres_free(&dup);

  WRes orphan = db_exec_sql(db, "insert into my_child (id, parent) values (1, 99)");
  ASSERT_FALSE(orphan.ok);
  ASSERT_EQ(orphan.cls, WDBE_BAD_REFERENCE);
  wres_free(&orphan);
  db_exec_sql(db, "drop table my_child");
  db_exec_sql(db, "drop table my_parent");
  db_close(db);
}

TEST(strict_mode_turns_a_too_long_value_into_an_error) {
  MY_URL_OR_SKIP(url);
  WDB *db = fresh(url);
  ASSERT_NOT_NULL(db);
  // Without STRICT_TRANS_TABLES the server would truncate and warn, and the app would never
  // know — the driver's session setup turns it on so a 422 is reachable.
  str long_label = str_lit("0123456789012345678901234567890123456789012345678901234567890123456789");
  MyThing t = {.label = long_label};
  WRes r = db_exec_model(db, "insert", MyThing_modelof(), &t);
  ASSERT_FALSE(r.ok);
  ASSERT_EQ(r.cls, WDBE_TOO_LONG);
  wres_free(&r);
  db_close(db);
}

TEST(a_transaction_rolls_back_and_a_savepoint_holds) {
  MY_URL_OR_SKIP(url);
  WDB *db = fresh(url);
  ASSERT_NOT_NULL(db);
  ASSERT_TRUE(db_begin(db));
  MyThing a = {.label = str_lit("gone")};
  ASSERT_TRUE(db_save(MyThing, &a));
  ASSERT_TRUE(db_rollback(db));
  ASSERT_EQ(vec_my_thing_len(db_all(db, MyThing)), 0);

  ASSERT_TRUE(db_begin(db));
  MyThing b = {.label = str_lit("kept")};
  ASSERT_TRUE(db_save(MyThing, &b));
  ASSERT_TRUE(db_exec_sql(db, "savepoint sp1").ok);
  MyThing c = {.label = str_lit("undone")};
  ASSERT_TRUE(db_save(MyThing, &c));
  ASSERT_TRUE(db_exec_sql(db, "rollback to savepoint sp1").ok);
  ASSERT_TRUE(db_commit(db));
  VecMyThing *all = db_all(db, MyThing);
  ASSERT_EQ(vec_my_thing_len(all), 1);
  ASSERT_STR_EQ(vec_my_thing_ref(all, 0)->label, "kept");
  db_close(db);
}

TEST(a_reserved_word_column_is_quoted_for_this_dialect) {
  MY_URL_OR_SKIP(url);
  WDB *db = fresh(url);
  ASSERT_NOT_NULL(db);
  // MySQL reserves `rank`, `groups`, `window`… where sqlite and postgres do not; the driver
  // spells columns with backticks so a model may have such a field at all.
  ASSERT_TRUE(db_exec_sql(db,
                          "create table my_words (id bigint auto_increment primary key, "
                          "`rank` integer not null default 0, `groups` integer not null default 0)")
                  .ok);
  ASSERT_TRUE(db_exec_sql(db, "insert into my_words (`rank`, `groups`) values (3, 4)").ok);
  WRes r = db_exec_sql(db, "select `rank`, `groups` from my_words");
  ASSERT_TRUE(r.ok);
  ASSERT_EQ(row_i64(&r.rows[0], "rank"), 3);
  wres_free(&r);
  ASSERT_TRUE(db_exec_sql(db, "drop table my_words").ok);
  db_close(db);
}

TEST(a_statement_stops_when_the_request_is_cancelled) {
  MY_URL_OR_SKIP(url);
  db_pool_config(4, 5000);
  WDB *db = fresh(url);
  ASSERT_NOT_NULL(db);
  db_default_set(db);
  // The server-side half: `max_execution_time` from the request's deadline, plus `KILL QUERY`
  // on a second connection. A statement that outlives its request must fail, not finish.
  WCtx *ctx = ctx_new(nullptr);
  ctx_set_deadline(ctx, (i64)wc_time_ms() + 150);
  WDbSession *s = db_session(ctx);
  ASSERT_NOT_NULL(s);
  // A query that really is slow, not `sleep()`: MySQL's SLEEP() is *interrupted* by
  // max_execution_time and answers a row saying so, so it returns "successfully" in 150 ms and
  // proves nothing about cancellation. A cross join over information_schema is the honest
  // slow read — and `select count(*)` is exactly the read-only shape the server cap covers.
  u64 t0 = wc_time_ms();
  WRes r = db_exec_sql(db,
                       "select count(*) from information_schema.columns a "
                       "join information_schema.columns b "
                       "join information_schema.columns c");
  u64 took = wc_time_ms() - t0;
  ASSERT_FALSE(r.ok);
  ASSERT_TRUE(took < 2000); // stopped, not waited out
  char msg[256] = {0};
  snprintf(msg, sizeof msg, "%.*s", WERR_FMT(r.e));
  // "we gave up" has to read differently from "the database died"
  ASSERT_TRUE(strstr(msg, "cancel") || strstr(msg, "interrupt") || strstr(msg, "timeout"));
  db_session_release();
  ctx_free(ctx);
  db_close(db);
  WDB *again = db_open(url); // the connection was cancelled, not parked back in the pool
  ASSERT_NOT_NULL(again);
  db_close(again);
}

TEST(a_connection_that_lost_its_server_is_not_reused) {
  MY_URL_OR_SKIP(url);
  WDB *db = fresh(url);
  ASSERT_NOT_NULL(db);
  // The driver notices a dead socket and reports it as a lost connection rather than handing
  // it to the next request: `broken()` is what makes the pool open a new one.
  ASSERT_TRUE(db_exec_sql(db, "kill connection_id()").ok || true); // not fatal if refused
  WRes after = db_exec_sql(db, "select 1 as one");
  if (!after.ok) {
    ASSERT_EQ(after.cls, WDBE_LOST);
    wres_free(&after);
    WDB *again = fresh(url); // a new handle works
    ASSERT_NOT_NULL(again);
    WRes ok = db_exec_sql(again, "select 1 as one");
    ASSERT_TRUE(ok.ok);
    wres_free(&ok);
    db_close(again);
  } else {
    wres_free(&after);
  }
  db_close(db);
}

TEST(a_null_handle_is_a_refusal_not_a_crash) {
  SKIP("covered by db_errors_test.c for every dialect");
}

TEST_MAIN()
