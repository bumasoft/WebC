// SPDX-License-Identifier: Apache-2.0
// WebC — database layer: one API, two drivers, models first (SPEC §6.5).
//
//   WDB *db = db_open(env("DATABASE_URL"));
//   Account a = { .email = email, .password_hash = hash };
//   Account *saved = db_insert(db, Account, &a);
//
// Drivers: sqlite (embedded, default for dev) and postgres (wire protocol, no libpq).
#ifndef WEBC_DB_H
#define WEBC_DB_H

#include <webc/core.h>
#include <webc/kinds.h>
#include <webc/model.h>
#include <webc/pp.h>

typedef struct WDB WDB;
typedef struct WRow WRow;
typedef struct WQuery WQuery;

typedef enum WValKind : u8 { WV_NULL, WV_I64, WV_F64, WV_TEXT, WV_BLOB, WV_BOOL } WValKind;

// Why a statement failed, classified by the driver that saw the error — never by
// sniffing the message text for a code. sqlite reports an extended result code,
// postgres a SQLSTATE, MySQL an errno; each driver turns its own vocabulary into this
// one, and `db_fail` turns this one into a status code (409 / 400 / 422 / 503 / 500).
// A message is for the log and for the person reading it; this is for the program.
typedef enum WDbErrKind : u8 {
  WDBE_NONE,
  WDBE_CONFLICT,      // unique or referenced-row conflict      -> 409
  WDBE_BAD_REFERENCE, // a foreign key pointed at nothing        -> 400
  WDBE_NOT_NULL,      // a required column was NULL              -> 422
  WDBE_TOO_LONG,      // a value overran its column              -> 422
  WDBE_CHECK,         // a check constraint said no              -> 422
  WDBE_DEADLOCK,      // the server rolled the transaction back  -> 503
  WDBE_TIMEOUT,       // a lock wait or a statement cap fired    -> 503
  WDBE_AUTH,          // the server refused this user            -> 500, at open
  WDBE_LOST,          // the connection is gone; the slot is recycled
} WDbErrKind;

typedef struct WVal {
  WValKind kind;
  i64 i;
  f64 f;
  str s;          // text/blob, arena-owned
  bool b;
} WVal;

struct WRow {
  WVal *vals;
  i32 n;
  str *keys;      // column names, arena-owned
};

typedef struct WRes {
  bool ok;
  WErr e;
  WRow *rows;
  i32 nrows;
  i32 ncols;
  // The shape of the result, whether or not it has rows: `select * from t where
  // 1 = 0` is how a schema check asks a table for its columns without reading it.
  str *keys;
  i64 affected;
  i64 last_id;
  WDbErrKind cls;      // why it failed, in terms the framework can act on
  Arena *own;          // rows live here; free with wres_free()
} WRes;

// Rows belong to the result, not to the database: free them when you are done.
void wres_free(WRes *r);

struct WQuery {
  Arena *arena;
  str sql;
  // parameters, positional in SQL order; named ones are rewritten to `?`/`$n`
  WVal *params;
  i32 nparams, cap;
  str *names;
  bool owns_arena;     // q() made it, db_exec() frees it
  // The handle's own lifetime, held outside `arena` on purpose: the arena dies with the
  // query, so a flag kept in it would be unreadable exactly when a second use needs to be
  // reported. See qh_take() in src/db.c.
  // A copy of the SQL that lives with the handle, not with the arena: the message about a
  // spent query has to quote the statement *after* the arena is gone.
  char sql_head[96];
  bool ring;           // came from q()/q_a(), so it is spent after one db_exec()
  bool in_use;
  bool released;       // db_exec() or q_free() ended it
};

// ---------------------------------------------------------------- query builder

WQuery *q(const char *sql);
void    q_free(WQuery *qy);   // `q()` owns an arena; db_exec() frees it, a
                              // query that never reaches the driver does not
WQuery *q_a(Arena *a, const char *sql);
void    q_i64(WQuery *qy, const char *name, i64 v);
void    q_f64(WQuery *qy, const char *name, f64 v);
void    q_str(WQuery *qy, const char *name, str v);
void    q_cstr(WQuery *qy, const char *name, const char *v);
void    q_bool(WQuery *qy, const char *name, bool v);
void    q_null(WQuery *qy, const char *name);
void    q_bytes(WQuery *qy, const char *name, Bytes v);

// ---------------------------------------------------------------- rows

WVal  row_at(WRow *r, i32 i);
WVal  row_get(WRow *r, const char *name);
i64   row_i64(WRow *r, const char *name);
i32   row_i32(WRow *r, const char *name);
f64   row_f64(WRow *r, const char *name);
str   row_str(WRow *r, const char *name);
bool  row_bool(WRow *r, const char *name);
bool  row_is_null(WRow *r, const char *name);
i64   row_i64_at(WRow *r, i32 i);
str   row_str_at(WRow *r, i32 i);
i32   row_cols(WRow *r);

// ---------------------------------------------------------------- connection

WDB  *db_open(const char *url);          // sqlite:path | postgres://user:pw@host:port/db
void  db_close(WDB *db);
bool  db_ok(WDB *db);
str   db_error(WDB *db);
WDB  *db(void);                          // process-wide default
void  db_set_default(WDB *db);
str   db_driver_name(WDB *db);

// How many connections the pool may open, and how long a task waits for one before the
// request is a 503 instead of a hang. Defaults: two per worker (capped at WC_POOL_MAX) and
// 5 000 ms, overridable with WEBC_DB_POOL / WEBC_DB_ACQUIRE_MS. Two, not one: a request holds
// its connection across every round trip of a page, so one per worker caps throughput at
// workers ÷ page latency (measured: doubling it was +19.5% on the same box).
void  db_pool_config(i32 max_conns, i64 acquire_ms);
i32   db_pool_size(WDB *db);   // connections actually open

// The start of a request: forget every row fetched for the previous one. Called by the server
// loop beside wc_ref_clear(), because a pooled arena returns at the same address and rows from
// the previous request would otherwise look current.
void wc_row_pool_clear(void);

// How many statements this process has sent to a driver (a query-cache hit is not one).
// It exists because "one prefetch per relation, however long the page" is a promise this
// framework makes and a stopwatch cannot check: a test reads the counter on both sides of a
// call, and `WEBC_TRACE_DB=1` shows the same number as a line per statement.
u64   db_statement_count(void);

// ---------------------------------------------------------------- the session
//
// A *session* is one connection that belongs to one task for as long as that task lives:
// every statement the task runs goes through it, a transaction opened in the task is
// opened on it, and only that task can end it. Node calls the same object a `PoolClient`
// — `const client = await pool.connect()` — and it is what makes a transaction mean
// something under concurrency.
//
// The dispatcher opens one at the request boundary and releases it when the handler
// returns, so an app never sees this type unless it asks. A handler that panics is covered
// by the exit hook, so a half-finished transaction is rolled back, never parked in the pool:
//
//   GET("/orders") {
//     tx_scope(db_default()) {        // BEGIN on this request's own connection
//       db_save(Order, &o);
//       tx_commit_here();             // COMMIT on the same connection, or the scope rolls back
//     }
//   }
//
// A task with no session (a CLI command, a migration) borrows a connection per statement:
// it runs, and gives the connection back.
typedef struct WDbSession WDbSession;
typedef struct WCtx WCtx;
WDbSession *db_session(WCtx *ctx);        // this task's session, opened if needed
WDbSession *db_session_current(void);     // the one in use, or null
void        db_session_release(void);     // roll back what is open, return the connection
bool        db_session_owns(WDbSession *s, WDB *db);
bool        db_session_in_tx(WDbSession *s);

// ---------------------------------------------------------------- queries

WRes db_exec(WDB *db, WQuery *qy);
WRes db_exec_sql(WDB *db, const char *sql);
WRes db_ping(WDB *db);
bool db_begin(WDB *db);
bool db_commit(WDB *db);
bool db_rollback(WDB *db);

// Model-shaped helpers: the descriptor table generates the SQL.
typedef struct WOpts {
  const char *where;
  const char *order;
  i64  limit;
  i64  offset;
  bool return_row;
} WOpts;

WRes db_exec_model(WDB *db, const char *verb, const WModel *m, const void *obj);
bool db_insert_into(WDB *db, const WModel *m, const void *obj, i64 *id_out);
WRes db_select_where(WDB *db, const WModel *m, const char *where, WQuery *params);
// Fill a model vector from a select. `db_all(db, Todo)` expects the collection to
// be called VecTodo, the name WEBC_VEC_DEFINE gives it by convention.
void *db_all_vec(WDB *db, const WModel *m, const WVecDesc *vec);
// The same, with a clause appended after the table name — the order a list comes
// back in, or a filter and an order together. `qy` holds the bound values when the
// clause has placeholders (null for none):
//
//   VecStory *latest = db_all_ordered(db, Story, "published desc");
//   VecStory *few    = db_all_where(db, Story, "where views > ? order by views desc", qy);
void *db_all_clause_vec(WDB *db, const WModel *m, const WVecDesc *vec, const char *clause,
                        WQuery *qy);

// Insert/update/select as macros so the model name reaches the descriptor table.
// ---------------------------------------------------------------- the app kit
//
// One line per database operation, on the configured handle, failing the request
// when the driver refuses:
//
//   POST("/todos") { Todo t = body(Todo); db_save(Todo, &t); return redirect("/"); }
//   GET("/t/{id}") { Todo *t = db_get(Todo, path_i64("id")); if (!t) fail(404, "no such todo"); ... }
//
// A unique violation answers 409, a foreign-key violation 400, a NOT NULL
// violation 422 and anything else 500 — with the server's own message and
// SQLSTATE, because an error that says only "ERROR" is an error you cannot fix.
struct WDB;
struct WDB *db(void);                                    // the handle in use, if any
struct WDB *db_default(void);                            // ... opened from DATABASE_URL
                                                         // the first time it is asked for
bool  db_save_(const WModel *m, void *obj);              // insert or update by pk
bool  db_try_save_(const WModel *m, void *obj);          // ... false only if it conflicts
void *db_fetch(const WModel *m, i64 id);                 // arena-allocated, null if gone
bool  db_delete_(const WModel *m, i64 id);
i64   db_count_(const WModel *m);
void  db_fail(const char *what);                         // map the driver error, fail the request

#define db_save(T_, obj_) db_save_(T_##_modelof(), (void *)(obj_))
// `if (!db_try_save(Account, &a)) { /* say it in the page, not in a 500 */ }`
// A unique violation is the client's mistake and a value; everything else is ours
// and still panics.
#define db_try_save(T_, obj_) db_try_save_(T_##_modelof(), (void *)(obj_))
// One row from a condition of your own: `db_find(Account, "email = :em", qy)`.
// Null means "no such row" — not an error; a driver failure panics.
void *db_find_where(const WModel *m, const char *where, WQuery *qy);
#define db_find(T_, where_, qy_) ((T_ *)db_find_where(T_##_modelof(), (where_), (qy_)))
#define db_get(T_, id_) ((T_ *)db_fetch(T_##_modelof(), (i64)(id_)))
// Two arguments: the configured handle, failing the request on error.
// Three: a handle you own, answering a bool — what a test wants.
#define db_delete(...) WC_CAT(WC_DBDEL_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_DBDEL_2(T_, id_) db_delete_(T_##_modelof(), (i64)(id_))
#define db_count(T_) db_count_(T_##_modelof())

#define db_insert(db_, T_, obj_)                                               \
  ({                                                                          \
    i64 _id_ = 0;                                                             \
    bool _ok_ = db_insert_into((db_), T_##_modelof(), (obj_), &_id_);          \
    (void)_ok_;                                                               \
    _ok_;                                                                     \
  })
#define db_update(db_, T_, obj_)   db_exec_model((db_), "update", T_##_modelof(), (obj_)).ok
#define WC_DBDEL_3(db_, T_, id_)                                               \
  db_exec_sql((db_), str_fmt(nullptr, "delete from %s where id = %lld",         \
                             T_##_modelof()->table, (long long)(id_)).p).ok
// `db_all(Todo)` on the configured handle, or `db_all(handle, Todo)` on one you own.
#define db_all(...) WC_CAT(WC_DBALL_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_DBALL_2(db_, T_) ((Vec##T_ *)db_all_vec((db_), T_##_modelof(), WC_VECOF_Vec##T_))
#define WC_DBALL_1(T_) WC_DBALL_2(db_default(), T_)
// `db_all_ordered(db, Story, "published desc")`, and the general form:
// `db_all_where(db, Story, "where section = ? order by published desc", qy)`.
#define WC_DBALLC_4(db_, T_, clause_, qy_) \
  ((Vec##T_ *)db_all_clause_vec((db_), T_##_modelof(), WC_VECOF_Vec##T_, (clause_), (qy_)))
#define db_all_where(db_, T_, clause_, qy_) WC_DBALLC_4(db_, T_, clause_, qy_)
#define db_all_ordered(db_, T_, order_) WC_DBALLC_4(db_, T_, "order by " order_, nullptr)
#define db_where(db_, T_, w_, qy_) db_select_where((db_), T_##_modelof(), (w_), (qy_))

// ---------------------------------------------------------------- relations
//
// `prefetch(all, Story, section)` runs one query for the whole list and links every row to
// its section; the view then reads `ref(Story, st, section)` with no query at all. The
// declaration is WEBC_MODEL_REF(Story, section, Section, slug), next to the models.
//
// N+1 queries are the classic cost of a relation, and they are invisible: the page is
// correct, and it runs 13 queries. One declaration, one prefetch, no query in the view.
void wc_prefetch(WDB *db, const WModel *m, const void *vec, const char *field);
// Two hops or three, left to right: each hop is one query, applied to the rows the
// previous hop linked. `prefetch(all, Section, stories, author)` is two statements — the
// stories of every section on the page, then the authors those stories named. Deeper than
// three is a loop in the app, because a path string is a second query language nobody
// asked for.
void wc_prefetch_path(WDB *db, const WModel *m, const void *vec, const char *const *fields,
                      i32 nfields);

#define WC_PF_FIELDS_1(a_) ((const char *const[]){a_})
#define WC_PF_FIELDS_2(a_, b_) ((const char *const[]){a_, b_})
#define WC_PF_FIELDS_3(a_, b_, c_) ((const char *const[]){a_, b_, c_})

#define prefetch(...) WC_CAT(WC_PF_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_PF_3(vec_, T_, f1_) \
  wc_prefetch_path(db_default(), T_##_modelof(), (const void *)(vec_), WC_PF_FIELDS_1(#f1_), 1)
#define WC_PF_4(vec_, T_, f1_, f2_)                                                 \
  wc_prefetch_path(db_default(), T_##_modelof(), (const void *)(vec_),               \
                   WC_PF_FIELDS_2(#f1_, #f2_), 2)
#define WC_PF_5(vec_, T_, f1_, f2_, f3_)                                            \
  wc_prefetch_path(db_default(), T_##_modelof(), (const void *)(vec_),               \
                   WC_PF_FIELDS_3(#f1_, #f2_, #f3_), 3)
// The same on a handle you own (a test, a second database).
#define prefetch_on(...) WC_CAT(WC_PFON_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_PFON_4(db_, vec_, T_, f1_) \
  wc_prefetch_path((db_), T_##_modelof(), (const void *)(vec_), WC_PF_FIELDS_1(#f1_), 1)
#define WC_PFON_5(db_, vec_, T_, f1_, f2_)                                          \
  wc_prefetch_path((db_), T_##_modelof(), (const void *)(vec_),                      \
                   WC_PF_FIELDS_2(#f1_, #f2_), 2)
#define WC_PFON_6(db_, vec_, T_, f1_, f2_, f3_)                                     \
  wc_prefetch_path((db_), T_##_modelof(), (const void *)(vec_),                      \
                   WC_PF_FIELDS_3(#f1_, #f2_, #f3_), 3)

// ---------------------------------------------------------------- typed filters
//
// `db_where(db, Todo, "done = ? order by created desc", qy)` is the escape hatch, and
// it is also how a filter value ends up spliced into SQL: nothing checks the column
// name, and nothing stops a value from being interpolated instead of bound. These
// build the same clause from the model descriptor: the field name is checked by the
// compiler, the column name comes from the field (so a `.sql` override still works),
// and every value is a bound parameter.
//
//   WWhere w; ww_init(&w, a);
//   ww_eq_bool(&w, Todo, done, false);
//   ww_order(&w, Todo, created, true);
//   WRes r = db_select(db, Todo, &w);
//   ww_free(&w);
typedef struct WWhere {
  Arena *a;
  bool owns_arena;
  Buf sql;
  WQuery qy;
  i32 n;        // conditions so far
  bool order;   // an order-by was added
  bool in_or;   // inside (a like ? or b like ?)
  bool no_sep;  // the next condition opens a group and takes no separator
} WWhere;

void ww_init(WWhere *w, Arena *a);
void ww_free(WWhere *w);
const char *ww_sql(const WWhere *w); // the clause, for db_where / db_find
WQuery *ww_query(WWhere *w);

void ww_eq_i64_(WWhere *w, const WModel *m, const char *field, i64 v);
void ww_ne_i64_(WWhere *w, const WModel *m, const char *field, i64 v);
void ww_gt_i64_(WWhere *w, const WModel *m, const char *field, i64 v);
void ww_gte_i64_(WWhere *w, const WModel *m, const char *field, i64 v);
void ww_lt_i64_(WWhere *w, const WModel *m, const char *field, i64 v);
void ww_lte_i64_(WWhere *w, const WModel *m, const char *field, i64 v);
void ww_eq_bool_(WWhere *w, const WModel *m, const char *field, bool v);
void ww_eq_str_(WWhere *w, const WModel *m, const char *field, str v);
void ww_like_(WWhere *w, const WModel *m, const char *field, str pattern);
void ww_order_(WWhere *w, const WModel *m, const char *field, bool desc);
void ww_limit_(WWhere *w, i64 n, i64 offset);
// One question over several columns:
//   ww_or_begin(&w);
//   ww_contains(&w, Story, title, q);
//   ww_contains(&w, Story, dek, q);
//   ww_or_end(&w);
void ww_or_begin_(WWhere *w);
void ww_or_end_(WWhere *w);
void ww_contains_(WWhere *w, const WModel *m, const char *field, str v);
// `.null` is the only field where "the column is absent" is a question: 0 is the value
// this side spells NULL, and `= 0` would ask the server for a row that is not null.
void ww_is_null_(WWhere *w, const WModel *m, const char *field);
void ww_not_null_(WWhere *w, const WModel *m, const char *field);

// ---------------------------------------------------------------- across a relation
//
//   WWhere sub; ww_init(&sub, a);
//   ww_contains(&sub, Section, name, q);
//   ww_in_ref(&w, Story, section, &sub);
//   // where section in (select slug from sections where lower(name) like lower(:p0))
//
// A subquery, not a join: a join changes the *shape* of the result, and the ORM reads rows
// back into models — `select * from stories where …` is the shape `model_from_row` wants.
// The sub-WWhere's arena must outlive the outer query; `ww_free` the inner last.
void ww_in_ref_(WWhere *w, const WModel *m, const char *field, WWhere *sub, bool negate);
void ww_order_ref_(WWhere *w, const WModel *m, const char *field, const WModel *target,
                   const char *target_key, bool desc);

#define ww_in_ref(w_, T_, f_, sub_) \
  (WC_WW_FIELD(T_, f_), ww_in_ref_((w_), T_##_modelof(), #f_, (sub_), false))
#define ww_not_in_ref(w_, T_, f_, sub_) \
  (WC_WW_FIELD(T_, f_), ww_in_ref_((w_), T_##_modelof(), #f_, (sub_), true))
// `order by (select name from sections where slug = stories.section) asc` — a correlated
// subquery every one of the three servers answers, at the cost of one index lookup per row
// on the referenced key.
#define ww_order_ref(w_, T_, f_, RT_, rkey_, desc_)                                       \
  (WC_WW_FIELD(T_, f_),                                                                   \
   ww_order_ref_((w_), T_##_modelof(), #f_, RT_##_modelof(), #rkey_, (desc_)))

// The compile-time half: a field that is not on the model is a compile error here,
// not an SQL error at runtime.
#define WC_WW_FIELD(T_, f_) ((void)sizeof(((T_ *)0)->f_))
#define ww_eq_i64(w_, T_, f_, v_) (WC_WW_FIELD(T_, f_), ww_eq_i64_((w_), T_##_modelof(), #f_, (i64)(v_)))
#define ww_ne_i64(w_, T_, f_, v_) (WC_WW_FIELD(T_, f_), ww_ne_i64_((w_), T_##_modelof(), #f_, (i64)(v_)))
#define ww_gt_i64(w_, T_, f_, v_) (WC_WW_FIELD(T_, f_), ww_gt_i64_((w_), T_##_modelof(), #f_, (i64)(v_)))
#define ww_gte_i64(w_, T_, f_, v_) (WC_WW_FIELD(T_, f_), ww_gte_i64_((w_), T_##_modelof(), #f_, (i64)(v_)))
#define ww_lt_i64(w_, T_, f_, v_) (WC_WW_FIELD(T_, f_), ww_lt_i64_((w_), T_##_modelof(), #f_, (i64)(v_)))
#define ww_lte_i64(w_, T_, f_, v_) (WC_WW_FIELD(T_, f_), ww_lte_i64_((w_), T_##_modelof(), #f_, (i64)(v_)))
#define ww_eq_bool(w_, T_, f_, v_) (WC_WW_FIELD(T_, f_), ww_eq_bool_((w_), T_##_modelof(), #f_, (v_)))
#define ww_eq_str(w_, T_, f_, v_) (WC_WW_FIELD(T_, f_), ww_eq_str_((w_), T_##_modelof(), #f_, (v_)))
#define ww_like(w_, T_, f_, v_) (WC_WW_FIELD(T_, f_), ww_like_((w_), T_##_modelof(), #f_, (v_)))
#define ww_order(w_, T_, f_, desc_) (WC_WW_FIELD(T_, f_), ww_order_((w_), T_##_modelof(), #f_, (desc_)))
#define ww_contains(w_, T_, f_, v_) \
  (WC_WW_FIELD(T_, f_), ww_contains_((w_), T_##_modelof(), #f_, WC_AS_STR(v_)))
//   ww_is_null(&w, Post, cover_media_asset_id);      // "no cover", as SQL means it
//   ww_not_null(&w, Post, cover_media_asset_id);
#define ww_is_null(w_, T_, f_) (WC_WW_FIELD(T_, f_), ww_is_null_((w_), T_##_modelof(), #f_))
#define ww_not_null(w_, T_, f_) (WC_WW_FIELD(T_, f_), ww_not_null_((w_), T_##_modelof(), #f_))
#define ww_or_begin(w_) ww_or_begin_(w_)
#define ww_or_end(w_) ww_or_end_(w_)
// The limit belongs to the query, not to the caller's loop: `ww_limit(&w, 5, 0)` is
// `limit 5`, and reading thirteen rows to keep two is what this removes.
#define ww_limit(w_, n_, off_) ww_limit_((w_), (i64)(n_), (i64)(off_))
// `db_select(db, Todo, &w)`: the configured handle, failing the request on error.
#define db_select(db_, T_, w_) db_select_where((db_), T_##_modelof(), ww_sql(w_), ww_query(w_))

// The same filter, answered three ways. `db_select` returns a `WRes` you have to read
// row by row; these are the shapes an app actually wants:
//
//   VecStory *list = db_all_w(db, Story, &w);    // rows, ordered and limited by the query
//   Story    *one  = db_one(db, Story, &w);      // the first row, or null
//   i64       n    = db_count_w(db, Story, &w);  // how many there are, without reading them
void *db_all_w_(WDB *db, const WModel *m, const WVecDesc *vec, WWhere *w);
void *db_one_w_(WDB *db, const WModel *m, WWhere *w);
i64   db_count_w_(WDB *db, const WModel *m, WWhere *w);
bool  db_increment_(WDB *db, const WModel *m, const void *obj, const char *field);

// ---------------------------------------------------------------- aggregates
//
// `db_count_w` was the only question the builder could answer with a number. These are the
// rest, on the same filter and with the same field-name check:
//
//   i64 views  = db_sum(db, Story, views, &w);
//   f64 mean   = db_avg(db, Story, views, &w);   // f64 even for an integer column
//   i64 newest = db_max(db, Story, published, &w);
//
// `order by`/`limit` are stripped from the filter exactly as db_count_w strips them — a sum
// of the first page is a different question and has its own answer. No rows, or every row
// NULL, is 0 (the SUM of nothing is not an error, it is nothing).
WVal db_agg_(WDB *db, const WModel *m, const char *field, const char *fn, WWhere *w);
i64  db_sum_(WDB *db, const WModel *m, const char *field, WWhere *w);
i64  db_min_(WDB *db, const WModel *m, const char *field, WWhere *w);
i64  db_max_(WDB *db, const WModel *m, const char *field, WWhere *w);
f64  db_avg_(WDB *db, const WModel *m, const char *field, WWhere *w);

#define db_sum(...) WC_CAT(WC_DBSUM_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_DBSUM_4(db_, T_, f_, w_) \
  (WC_WW_FIELD(T_, f_), db_sum_((db_), T_##_modelof(), #f_, (w_)))
#define db_min(...) WC_CAT(WC_DBMIN_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_DBMIN_4(db_, T_, f_, w_) \
  (WC_WW_FIELD(T_, f_), db_min_((db_), T_##_modelof(), #f_, (w_)))
#define db_max(...) WC_CAT(WC_DBMAX_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_DBMAX_4(db_, T_, f_, w_) \
  (WC_WW_FIELD(T_, f_), db_max_((db_), T_##_modelof(), #f_, (w_)))
#define db_avg(...) WC_CAT(WC_DBAVG_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_DBAVG_4(db_, T_, f_, w_) \
  (WC_WW_FIELD(T_, f_), db_avg_((db_), T_##_modelof(), #f_, (w_)))

// ---------------------------------------------------------------- a page of a list
//
//   WPager pg = {.page = page_number, .limit = 20};
//   VecStory *rows = db_page(db, Story, &w, &pg);   // two statements: count + window
//
// `page` is clamped into 1..pages: an out-of-range page is the same list, not an empty
// render and not an error. Two statements on purpose — the count is what tells the pager how
// many pages there are, and it cannot be derived from a window. Between the two the data may
// change (a row inserted shifts everything by one); this does not open a transaction for it,
// because doubling the cost of every list render to hide a race nobody has hit is the wrong
// trade (docs/reference.md §9a2).
typedef struct WPager {
  i64 page;  // 1-based, in/out
  i64 limit; // per page, 0 = 20
  i64 total; // out: rows matching the conditions, without order/limit
  i64 pages; // out: ceil(total / limit), at least 1
  bool has_prev, has_next; // out
} WPager;

void *db_page_(WDB *db, const WModel *m, const WVecDesc *vec, WWhere *w, WPager *pg);

#define db_page(...) WC_CAT(WC_DBPAGE_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_DBPAGE_4(db_, T_, w_, pg_) \
  ((Vec##T_ *)db_page_((db_), T_##_modelof(), WC_VECOF_Vec##T_, (w_), (pg_)))
#define WC_DBPAGE_3(T_, w_, pg_) WC_DBPAGE_4(db_default(), T_, w_, pg_)

#define db_all_w(db_, T_, w_) \
  ((Vec##T_ *)db_all_w_((db_), T_##_modelof(), WC_VECOF_Vec##T_, (w_)))
#define db_one(db_, T_, w_) ((T_ *)db_one_w_((db_), T_##_modelof(), (w_)))
#define db_count_w(db_, T_, w_) db_count_w_((db_), T_##_modelof(), (w_))
// ---------------------------------------------------------------- one field, one value
//
// The lookup every app writes, and every app wrote the same way:
//
//   WQuery *qy = q("select 1");                 // a query built only to carry a value
//   q_str(qy, "s", slug);
//   const Section *sec = db_find(Section, "slug = :s", qy);
//
// The carrier exists because `db_find` takes SQL and a query, and there was no way to say
// "this field equals this value" in a form the framework could type-check. Now there is:
//
//   const Section *sec = db_by(Section, "slug", slug);
//   VecStory *mine     = db_all_by(Story, "section", "science");    // + ww_* to refine
//
// The field name is checked against the model's fields — a typo is a panic naming the real
// ones, not a query that quietly returns nothing. `db_by` returns the first match; for a
// list, `db_all_by`; for a number, `db_count_by`. Add `&w` (a WWhere) as a 5th argument or
// use db_all_w when the condition is more than one clause.
// A bound value that can be a string or a number: what the macro below hands the three
// lookups. It exists so `db_by(Section, "slug", slug)` takes a str and
// `db_by(Story, "id", 3)` takes an integer without a second macro name.
typedef struct WValue {
  bool is_i64;
  i64 n;
  str s;
} WValue;
WValue wv_of_str(str s);
WValue wv_of_cstr(const char *p);
WValue wv_of_i64(i64 n);
WValue wv_of_u64(u64 n);

void *db_by_(WDB *db, const WModel *m, const char *field, WValue v);
void *db_all_by_(WDB *db, const WModel *m, const WVecDesc *vec, const char *field, WValue v);
i64 db_count_by_(WDB *db, const WModel *m, const char *field, WValue v);

// A string literal is an array, and `_Generic` does not decay it, so a literal goes through
// str_lit() at the call site the way every other `str` parameter in WebC does.
// The 64-bit integer spellings, named once each: on Darwin i64 is `long long` and
// long is a second, distinct type; on LP64 Linux i64 *is* long, so naming both is a
// duplicate association. One list per platform keeps both honest.
#if defined(__APPLE__)
#define WC_VALUE_NUM_(x_)                                                              \
  i64: wv_of_i64, i32: wv_of_i64, long: wv_of_i64,                                     \
  unsigned int: wv_of_u64, unsigned long: wv_of_u64
#else
#define WC_VALUE_NUM_(x_)                                                              \
  i64: wv_of_i64, i32: wv_of_i64, long long: wv_of_i64,                                \
  unsigned int: wv_of_u64, unsigned long long: wv_of_u64
#endif
#define WC_VALUE_OF(x_)                                                                  \
  _Generic((x_), char *: wv_of_cstr, const char *: wv_of_cstr, str: wv_of_str,           \
           WC_VALUE_NUM_(x_), default: wv_of_str)((x_))
#define db_by(...) WC_CAT(WC_DBBY_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_DBBY_3(T_, f_, v_) \
  ((T_ *)db_by_(db_default(), T_##_modelof(), (f_), WC_VALUE_OF((v_))))
#define WC_DBBY_4(db_, T_, f_, v_) \
  ((T_ *)db_by_((db_), T_##_modelof(), (f_), WC_VALUE_OF((v_))))
#define db_all_by(...) WC_CAT(WC_DBALLBY_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_DBALLBY_3(T_, f_, v_)                                                   \
  ((Vec##T_ *)db_all_by_(db_default(), T_##_modelof(), WC_VECOF_Vec##T_, (f_),      \
                         WC_VALUE_OF((v_))))
#define WC_DBALLBY_4(db_, T_, f_, v_)                                              \
  ((Vec##T_ *)db_all_by_((db_), T_##_modelof(), WC_VECOF_Vec##T_, (f_),             \
                         WC_VALUE_OF((v_))))
#define db_count_by(...) WC_CAT(WC_DBCOUNTBY_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_DBCOUNTBY_3(T_, f_, v_) \
  db_count_by_(db_default(), T_##_modelof(), (f_), WC_VALUE_OF((v_)))
#define WC_DBCOUNTBY_4(db_, T_, f_, v_) \
  db_count_by_((db_), T_##_modelof(), (f_), WC_VALUE_OF((v_)))

// One column, plus one, in one statement: `db_increment(Todo, t, views)` on the configured
// handle, or `db_increment(db, Todo, t, views)` on one you own (tests).
#define db_increment(...) WC_CAT(WC_DBINC_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_DBINC_3(T_, obj_, f_) \
  db_increment_(db_default(), T_##_modelof(), (const void *)(obj_), #f_)
#define WC_DBINC_4(db_, T_, obj_, f_) \
  db_increment_((db_), T_##_modelof(), (const void *)(obj_), #f_)

// Read a model back out of a row (columns matched by field sql name).
bool model_from_row(const WModel *m, WRow *r, void *out);
// The same, but string fields are copied into `a`, so the row can go away.
bool model_from_row_dup(const WModel *m, WRow *r, void *out, Arena *a);

// ---------------------------------------------------------------- writing a parent and
//                                                               its children together
//
//   tx_scope(db_default()) {
//     db_save(Order, &o);                             // o.id is set from here on
//     db_save_all(db_default(), Order, &o, items, &n); // each item->order = o.id
//     tx_commit_here();
//   }
//
// The FK column is the `back_field` of the WEBC_MODEL_MANY rule, so it is checked against
// the declaration instead of being a string that might name the wrong column. It joins an
// open transaction if there is one and opens its own otherwise, so a half-written order
// cannot survive either way.
i64  db_save_all_(WDB *db, const WModel *parent, void *obj, void *vec, i64 *n_out);
i64  db_delete_tree_(WDB *db, const WModel *parent, const void *obj, void *vec);

#define db_save_all(...) WC_CAT(WC_DBSAVEALL_, WC_NARG(__VA_ARGS__))(__VA_ARGS__)
#define WC_DBSAVEALL_4(db_, T_, obj_, vec_) \
  db_save_all_((db_), T_##_modelof(), (void *)(obj_), (void *)(vec_), nullptr)
#define WC_DBSAVEALL_5(db_, T_, obj_, vec_, n_out_) \
  db_save_all_((db_), T_##_modelof(), (void *)(obj_), (void *)(vec_), (n_out_))
#define db_delete_tree(db_, T_, obj_, vec_) \
  db_delete_tree_((db_), T_##_modelof(), (const void *)(obj_), (void *)(vec_))

// ---------------------------------------------------------------- transactions
//
// A transaction belongs to a session, and a session belongs to a task, so a concurrent
// request can neither join it nor end it. Nested scopes are savepoints, and a COMMIT or
// ROLLBACK that finds no open transaction is an error the caller can see (`WTx.err`, the
// false return) instead of a silent no-op — a rollback that quietly does nothing is how a
// committed row disappears.
//
// tx_scope(db) opens a scope that rolls back unless it is committed:
//
//   tx_scope(db_default()) { db_save(Order, &o); tx_commit_here(); }
//
// `tx_commit_here()`/`tx_rollback_here()` end the innermost scope of this task's session,
// which is why the scope's handle does not need a name. The handle form still exists for a
// transaction that outlives one block: `WTx t = tx_begin(db); … tx_commit(&t);`.
typedef struct WTx {
  WDB *db;
  bool done;
  bool began;   // the BEGIN/SAVEPOINT this handle issued actually ran
  i32 depth;    // the session depth it belongs to (0 = not in a transaction)
  char err[192]; // why it failed, when it did (empty on success)
} WTx;
// `t` may be null: "the innermost scope of this task's session".
WTx  tx_begin(WDB *db);
bool tx_commit(WTx *t);
bool tx_rollback(WTx *t);
void tx_cleanup(WTx *t);
// This task's session depth: 0 when no transaction is open.
i32  tx_depth(void);
// One iteration, so the braces are a real block and `cleanup` runs at its end — the same
// trick `group(prefix) { … }` uses.
#define tx_scope(db_) WC_TXSCOPE_(__COUNTER__, db_)
#define WC_TXSCOPE_(C_, db_)                                                                    \
  for (WTx WC_CAT(wc_tx_, C_) __attribute__((cleanup(tx_cleanup))) = tx_begin(db_),             \
           *WC_CAT(wc_tx_once_, C_) = &WC_CAT(wc_tx_, C_);                                      \
       WC_CAT(wc_tx_once_, C_); WC_CAT(wc_tx_once_, C_) = nullptr)
#define tx_commit_here()   tx_commit(nullptr)
#define tx_rollback_here() tx_rollback(nullptr)

// ---------------------------------------------------------------- migrations

typedef struct WMigrate {
  WDB *db;
  const char *dir;
  bool dry_run;
  bool quiet;
} WMigrate;
bool migrations_apply(WMigrate *m);
bool migrations_status(WMigrate *m);

// Embedded migrations (SPEC §6.5): the same SQL, compiled into the binary with
// `#embed`, so a deploy is one file and cannot ship without its schema.
//
//   static const char m0001[] = { #embed "migrations/0001_init.sql", 0 };
//   static const WMig MIGS[] = { { "0001_init.sql", m0001 } };
//   migrations_apply_embedded(db, MIGS, WC_COUNTOF(MIGS));
//
// The records go to the same schema_migrations table as the directory form, and a
// named `<name>.<driver>.sql` variant is honoured the same way, so one project can
// use either — or both, as long as names do not collide.
typedef struct WMig {
  const char *name; // recorded in schema_migrations; the .sql suffix is optional
  const char *sql;
} WMig;

bool migrations_apply_embedded(WDB *db, const WMig *migs, i32 n);
// Driver-specific twin of an embedded migration, or null. Same rule as the files:
// `0001_init.sql` has an optional `0001_init.postgres.sql`.
const char *migration_for_driver(const WMig *migs, i32 n, const char *name, const char *driver);

#endif // WEBC_DB_H