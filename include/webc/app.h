// SPDX-License-Identifier: Apache-2.0
// webc/app.h — the app's boot function.
//
// `webc new` generates `void webc_boot(void)` in src/main.c. It builds everything
// the server needs — database, migrations, secrets, in-memory state — and does
// not serve anything. `main()` calls it and then hands over to `webc_serve()`,
// and a test calls it from `TEST_SETUP()` after pointing the app at a test
// database:
//
//   static void boot(void) {
//     setenv("DATABASE_URL", "sqlite::memory:", 1);
//     webc_boot();
//   }
//   TEST_SETUP(boot);
//
// The point is that the tests construct the world the same way the server does,
// instead of keeping a second copy of that construction that drifts.
#ifndef WEBC_APP_H
#define WEBC_APP_H

struct WDB;
void webc_boot(void);

// The framework's own boot: app arena, webc_boot(), default middleware,
// listener, shutdown. `main()` is weak and calls it, so an app normally writes
// neither. Call it from a custom `main` when you need the arguments.
int webc_main(int argc, char **argv);

// The default database handle: DATABASE_URL, opened once, migrations
// applied. `db_save`, `db_all`, `db_get` and friends use it, so no handler has to
// thread a handle around. `db_default_set` is for a test that boots its own.
struct WDB *db_default(void);
void db_default_set(struct WDB *db);

// App-wide state, the thing `on_start` fills in (SPEC §6.1):
//
//   static void on_start(void) { app_state_set("db", try(db_open(getenv("DATABASE_URL")))); }
//   WDB *db = app_state_get("db");
//
// A named slot, not a global variable per service: "what does this app depend on"
// becomes greppable and `webc doctor` can list it. Thread-safe (a mutex around a
// fixed table); it is set once at startup and read on every request, so the read
// is a lock and a scan of at most 64 names.
void  app_state_set(const char *name, void *p);
void *app_state_get(const char *name);
void  app_state_clear(void); // tests: forget everything (a fresh boot per test)

#endif // WEBC_APP_H