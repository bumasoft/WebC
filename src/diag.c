// SPDX-License-Identifier: Apache-2.0
// WebC — the things a framework can check for you, and the fixtures that let a
// project start with data instead of an empty table.
//
// Three jobs, one file, because they share the same fact: the models are declared
// in the *application* binary, so anything that wants to reason about them —
// `webc check`, `webc seed`, a schema diff — has to run inside the app, not in a
// tool that never links it.
//
//   model registry     every WEBC_MODEL registers itself at load time
//   webc_schema_report models vs. the tables that actually exist
//   seed_from_json     a JSON array of objects into a model's table
//
// `webc check` and `webc seed` re-execute the built binary with WEBC_SCHEMA_CHECK /
// WEBC_SEED set; webc_boot_diagnostics() answers and exits. The app keeps its own
// main() and its own boot hook, and the framework gets to see the real models.
#include <webc/server.h>
#include <webc/web.h>

// The model registry itself lives in model.c: a wasm build links model.c and not
// this file, and every WEBC_MODEL calls model_register() wherever it runs.

// ---------------------------------------------------------------- schema drift

// `select * from t where 1 = 0` is the one question both drivers answer the same
// way: the column names of a table, without a row. No pragma, no information_schema.
static WRes table_columns(WDB *db, const char *table) {
  char sql[256];
  snprintf(sql, sizeof sql, "select * from \"%s\" where 1 = 0", table);
  return db_exec_sql(db, sql);
}

// A table the model expects, compared with the table that exists. Prints what is
// wrong in the terms of the fix: the column, and the ALTER that would add it. With
// `plan` the same walk writes the statements instead of prose — one diff, two readers.
static bool check_model(WDB *db, const WModel *m, i32 *missing_cols, i32 *missing_tables,
                        Buf *plan) {
  if (m->internal) return true; // a framework row type has no table of its own
  const char *table = m->table ? m->table : "?";
  WRes r = table_columns(db, table);
  if (!r.ok) {
    // A model with no primary key that is also missing its table is almost never a forgotten
    // migration: it is a request body (the shape `body(T)` validates), which owns no table at
    // all. The fix is one macro away, and the message never named it.
    bool no_pk = true;
    for (i32 i = 0; i < m->n; i++)
      if (m->fields[i].q.pk) no_pk = false;
    if (no_pk) {
      printf("  %-16s has no primary key, so it may be a request body, not a row. If it is a\n",
             m->name);
      printf("  %-16s body validated by body(%s), say so and the schema check skips it:\n", "",
             m->name);
      printf("  %-16s   WEBC_MODEL_INTERNAL(%s)\n", "", m->name);
      printf("  %-16s otherwise it is a table that was never migrated:\n", "");
    }
    if (plan) {
      buf_printf(plan, "%s;\n",
                 str_cstr(wc_arena_get(), model_sql_create_table(wc_arena_get(), m, table)));
    } else {
      printf("  %-16s table \"%s\" is missing      → run `webc migrate diff --write`\n", m->name,
             table);
      printf("  %-16s   create table %s (\n", "", table);
      for (i32 i = 0; i < m->n; i++)
        printf("  %-16s     %s %.*s%s\n", "", model_column_name(&m->fields[i]),
               (int)model_sql_column_type(&m->fields[i]).n,
               model_sql_column_type(&m->fields[i]).p,
               m->fields[i].q.pk ? " primary key" : "");
      printf("  %-16s   );\n", "");
    }
    (*missing_tables)++;
    wres_free(&r);
    return false;
  }
  bool ok = true;
  for (i32 i = 0; i < m->n; i++) {
    const char *col = model_column_name(&m->fields[i]);
    bool found = false;
    for (i32 k = 0; k < r.ncols && r.keys; k++)
      if (r.keys[k].p && strcmp(r.keys[k].p, col) == 0) {
        found = true;
        break;
      }
    if (found) continue;
    (*missing_cols)++;
    ok = false;
    const WField *f = &m->fields[i];
    if (plan) {
      // Additive on purpose. A NOT NULL with no default cannot be added to a table that has
      // rows, so it is written with the default it needs; a nullable column needs nothing.
      buf_printf(plan, "alter table %s add column %s %.*s", table, col,
                 (int)model_sql_column_type(f).n, model_sql_column_type(f).p);
      if (f->q.references) buf_printf(plan, " references %s", f->q.references);
      if (f->q.not_null && f->q.def) buf_printf(plan, " not null default %s", f->q.def);
      buf_puts(plan, ";\n");
      if (f->q.not_null && !f->q.def)
        buf_printf(plan, "-- TODO %s.%s is not null and has no default: backfill the table and\n"
                         "--   add the constraint by hand, or give the field `.def`\n", table, col);
      continue;
    }
    printf("  %-16s .%-16s missing from \"%s\"   → alter table %s add column %s %.*s;\n", m->name, col,
           table, table, col, (int)model_sql_column_type(&m->fields[i]).n,
           model_sql_column_type(&m->fields[i]).p);
  }
  // Extra columns are reported once, quietly: a dropped field that is still in the
  // table is not a bug, but it is worth knowing.
  for (i32 k = 0; k < r.ncols && r.keys; k++) {
    if (!r.keys[k].p) continue;
    bool known = false;
    for (i32 i = 0; i < m->n && !known; i++)
      if (strcmp(model_column_name(&m->fields[i]), r.keys[k].p) == 0) known = true;
    if (known) continue;
    if (plan) // a leftover column is not a statement: the tool never drops one on its own
      buf_printf(plan, "-- note: %s.%s is in the table and not in the model (leftover)\n", table,
                 r.keys[k].p);
    else
      printf("  %-16s .%-16s in \"%s\", not in the model (leftover)\n", m->name, r.keys[k].p,
             table);
  }
  wres_free(&r);
  return ok;
}

bool webc_schema_report(WDB *db, FILE *out) {
  (void)out;
  if (!db) db = db_default();
  if (!db) {
    printf("  schema         no database (DATABASE_URL is not set)\n");
    return true;
  }
  i32 n = 0;
  for (i32 i = 0; i < model_registry_count(); i++)
    if (!model_registry_at(i)->internal) n++;
  if (n == 0) {
    printf("  schema         no models declared\n");
    return true;
  }
  i32 missing_cols = 0, missing_tables = 0;
  i32 internal = model_registry_count() - n;
  printf("  schema         %s, %d model%s", db_driver_name(db).p, n, n == 1 ? "" : "s");
  if (internal > 0) printf(" (%d internal, no table)", (int)internal);
  printf("\n");
  for (i32 i = 0; i < n; i++)
    check_model(db, model_registry_at(i), &missing_cols, &missing_tables, nullptr);
  if (missing_tables || missing_cols) {
    printf("  schema         DRIFT: %d table(s), %d column(s) missing\n", missing_tables,
           missing_cols);
    return false;
  }
  printf("  schema         models and tables agree\n");
  return true;
}

// The same comparison, as the migration that would fix it:
//
//   webc migrate diff --write     →  migrations/0004_schema.sql
//
// Additive only. It adds a column, adds a table, and — deliberately — never drops or narrows
// one: sqlite cannot drop a constraint without copying the table, and a tool that guesses at
// a data migration is a tool that loses rows. What it cannot infer it writes as a `-- TODO`,
// which is a line a person has to answer before the file is applied.
i32 webc_schema_plan(WDB *db, Buf *plan) {
  if (!db) db = db_default();
  if (!db || !plan) return 0;
  i32 missing_cols = 0, missing_tables = 0, before = (i32)plan->n;
  for (i32 i = 0; i < model_registry_count(); i++) {
    const WModel *m = model_registry_at(i);
    if (m->internal) continue;
    check_model(db, m, &missing_cols, &missing_tables, plan);
  }
  (void)before;
  return missing_cols + missing_tables;
}

// `WEBC_SCHEMA_DIFF=1`: the statements on stdout and nothing else, because the caller is
// `webc migrate diff` and it is going to write them into a file. Comments and notes are
// written as SQL comments so the file is still readable after it is generated.
bool webc_schema_diff(WDB *db, FILE *out) {
  Arena *a = wc_arena_get();
  Buf plan;
  buf_init(&plan, a, 1024);
  i32 n = webc_schema_plan(db, &plan);
  if (n == 0) {
    printf("-- no drift: the models and the tables agree\n");
    return true;
  }
  (void)out;
  printf("%s", buf_cstr(&plan));
  return false;
}

// `WEBC_MODEL_DUMP=Post`: one model as JSON, for `webc scaffold`. The descriptor is the only
// place the truth lives, and it lives in the app's process — so the command that generates
// code from a model runs the app, the same way the schema check does.
bool webc_model_dump(const char *name, FILE *out) {
  const WModel *m = nullptr;
  for (i32 i = 0; i < model_registry_count(); i++)
    if (strcmp(model_registry_at(i)->name, name) == 0) m = model_registry_at(i);
  if (!m) {
    printf("-- no model named %s\n", name);
    return false;
  }
  printf("{\"name\":\"%s\",\"table\":\"%s\",\"size\":%d,\"fields\":[", m->name,
         m->table ? m->table : wc_snake_plural(m->name), (int)m->n);
  for (i32 i = 0; i < m->n; i++) {
    const WField *f = &m->fields[i];
    char kind[32];
    size_t kn = model_sql_column_type(f).n;
    snprintf(kind, sizeof kind, "%.*s", (int)(kn < sizeof kind - 1 ? kn : sizeof kind - 1),
             model_sql_column_type(f).p);
    printf("%s{\"name\":\"%s\",\"type\":\"%s\",\"kind\":%d,\"text\":%s,\"num\":%s,"
           "\"bool\":%s,\"enum\":%s,\"vec\":%s,\"pk\":%s,\"required\":%s,"
           "\"unique\":%s,\"max\":%d,\"nullable\":%s}",
           i ? "," : "", f->name, kind, (int)f->kind,
           WC_KIND_IS_TEXT(f->kind) ? "true" : "false",
           (f->kind == WK_F32 || f->kind == WK_F64 || WC_KIND_IS_INT(f->kind)) ? "true" : "false",
           f->kind == WK_BOOL ? "true" : "false", f->en ? "true" : "false",
           f->vec ? "true" : "false", f->q.pk ? "true" : "false",
           f->q.required ? "true" : "false", f->q.unique ? "true" : "false", (int)f->q.max,
           f->q.null ? "true" : "false");
  }
  printf("]}\n");
  (void)out;
  return true;
}

// ---------------------------------------------------------------- seeding

static str slurp(Arena *a, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return (str){nullptr, 0};
  str out = {nullptr, 0};
  if (fseek(f, 0, SEEK_END) == 0) {
    long n = ftell(f);
    if (n >= 0) {
      rewind(f);
      char *buf = arena_alloc(a, (size_t)n + 1);
      size_t got = fread(buf, 1, (size_t)n, f);
      buf[got] = 0;
      out = (str){buf, got};
    }
  }
  fclose(f);
  return out;
}

// The rows a model already has, without loading them: a seed that runs on every
// boot must not insert a second copy of the site.
static bool table_has_rows(WDB *db, const WModel *m) {
  char sql[256];
  snprintf(sql, sizeof sql, "select count(*) as n from \"%s\"", m->table ? m->table : "?");
  WRes r = db_exec_sql(db, sql);
  bool any = r.ok && r.nrows > 0 && row_i64(&r.rows[0], "n") > 0;
  wres_free(&r);
  return any;
}

static i32 seed_one(const WModel *m, const char *path, bool reset, bool quiet) {
  if (!m || !path) return 0;
  WDB *db = db_default();
  if (!db) {
    if (!quiet) fprintf(stderr, "webc seed: no database (DATABASE_URL is not set)\n");
    return 0;
  }
  if (reset) {
    char sql[256];
    snprintf(sql, sizeof sql, "delete from \"%s\"", m->table ? m->table : "?");
    WRes d = db_exec_sql(db, sql);
    wres_free(&d);
  } else if (table_has_rows(db, m)) {
    return 0;
  }
  Arena *a = arena_new_labeled(256 * 1024, "seed");
  Arena *prev = wc_arena_swap(a);
  str text = slurp(a, path);
  i32 inserted = 0;
  if (text.n == 0) {
    if (!quiet) fprintf(stderr, "webc seed: cannot read %s\n", path);
    goto done;
  }
  JParse jp = json_parse(a, text);
  if (!jp.ok) {
    if (!quiet) fprintf(stderr, "webc seed: %s is not JSON: %.*s\n", path, WERR_FMT(jp.e));
    goto done;
  }
  JVal *root = jp.v;
  if (root->kind != J_ARR) {
    if (!quiet) fprintf(stderr, "webc seed: %s must hold an array of objects\n", path);
    goto done;
  }
  for (i32 i = 0; i < root->n; i++) {
    JVal *item = root->items[i];
    if (item->kind != J_OBJ) {
      if (!quiet) fprintf(stderr, "webc seed: %s[%d] is not an object\n", path, i);
      continue;
    }
    void *row = arena_zalloc(a, m->size);
    WModelParse p = model_from_json_val(a, m, item, row);
    if (!p.ok) {
      if (!quiet)
        fprintf(stderr, "webc seed: %s[%d] rejected: %.*s\n", path, i, WERR_FMT(p.e));
      continue;
    }
    i64 id = 0;
    if (!db_insert_into(db, m, row, &id)) {
      if (!quiet) fprintf(stderr, "webc seed: %s[%d] not inserted: %s\n", path, i, db_error(db).p);
      continue;
    }
    inserted++;
  }
  if (!quiet && inserted)
    printf("  seed           %-16s %d row%s from %s\n", m->name, inserted,
           inserted == 1 ? "" : "s", path);
done:
  wc_arena_swap(prev);
  arena_destroy(a);
  return inserted;
}

i32 seed_from_json(const WModel *m, const char *path) { return seed_one(m, path, false, false); }

i32 seed_from_json_reset(const WModel *m, const char *path) {
  return seed_one(m, path, true, false);
}

// `<dir>/<table>.json` for every registered model, in registration order. A model
// with no file is skipped: fixtures are per table, and a project usually has one.
i32 seed_all(const char *dir, bool reset) {
  i32 total = 0;
  for (i32 i = 0; i < model_registry_count(); i++) {
    const WModel *m = model_registry_at(i);
    if (m->internal) continue;
    char path[512];
    snprintf(path, sizeof path, "%s/%s.json", dir ? dir : "content", m->table ? m->table : "");
    FILE *f = fopen(path, "rb");
    if (!f) continue;
    fclose(f);
    total += seed_one(m, path, reset, false);
  }
  return total;
}

// ---------------------------------------------------------------- boot hooks
//
// One place where `webc check` and `webc seed` meet the application: both re-run
// the built binary with an environment variable, because that is the only process
// that has the models.
//
//   WEBC_SCHEMA_CHECK=1  compare models with tables, exit 0/1
//   WEBC_SCHEMA_DIFF=1   the same comparison as additive SQL (`webc migrate diff`)
//   WEBC_MODEL_DUMP=T    one model as JSON (`webc scaffold`)
//   WEBC_SEED=1          seed from WEBC_CONTENT (default: content), exit 0
//   WEBC_BOOT_ONLY=1     boot and exit: a fast "does the app start?" check
int webc_boot_diagnostics(void) {
  const char *schema = getenv("WEBC_SCHEMA_CHECK");
  const char *seed = getenv("WEBC_SEED");
  const char *only = getenv("WEBC_BOOT_ONLY");
  if (!schema && !seed && !only && !getenv("WEBC_SCHEMA_DIFF") && !getenv("WEBC_MODEL_DUMP"))
    return -1;
  if (seed) {
    const char *dir = getenv("WEBC_CONTENT");
    bool reset = getenv("WEBC_SEED_RESET") != nullptr;
    i32 n = seed_all(dir ? dir : "content", reset);
    printf("  seed           %d row%s\n", n, n == 1 ? "" : "s");
  }
  if (schema) {
    bool ok = webc_schema_report(db_default(), stdout);
    return ok ? 0 : 1;
  }
  if (getenv("WEBC_SCHEMA_DIFF")) {
    webc_schema_diff(db_default(), stdout);
    return 0;
  }
  if (getenv("WEBC_MODEL_DUMP")) {
    webc_model_dump(getenv("WEBC_MODEL_DUMP"), stdout);
    return 0;
  }
  if (only) printf("  boot           ok\n");
  return 0;
}