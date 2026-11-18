// SPDX-License-Identifier: Apache-2.0
// WebC — the boot the framework owns.
//
// An application writes routes and, when it needs to, a boot hook. It does not
// write `main()`: the middleware a server needs (a request id, default headers,
// logging, panic recovery, static files, metrics), the listener, the shutdown and
// the app-lifetime arena are the same in every app, so the framework writes them
// once and reads the few settings from the environment.
//
//   void webc_boot(void)   the app's half: open what it needs. Weak default: none.
//   int  webc_main(int, char **)  the whole sequence. Call it from a custom main.
//   int  main(int, char **)       weak: links only when the app defines none.
#include <webc/server.h>
#include <webc/web.h>

#include <sys/stat.h>

// The app's half. Weak, so an app that defines it wins, and a wasm/test build that
// defines nothing still links.
__attribute__((weak)) void webc_boot(void) {}

// The default database handle: opened once from DATABASE_URL when a handler
// first asks for it, so `db_save(Todo, &t)` needs no handle threaded anywhere. It
// *is* the framework's one default (db()/db_set_default), so an app that opens its
// own handle and calls db_set_default() wins.
static bool g_db_tried;

WDB *db_default(void) {
  WDB *h = db();
  if (h) return h;
  if (g_db_tried) return nullptr;
  g_db_tried = true;
  const char *url = getenv("DATABASE_URL");
  if (!url) url = getenv("WEBC_DB");
  if (!url) return nullptr;
  h = db_open(url);
  if (!h) panic("cannot open the database at %s", url);
  db_set_default(h);
  const char *dir = getenv("WEBC_MIGRATIONS") ? getenv("WEBC_MIGRATIONS") : "migrations";
  struct stat st;
  if (stat(dir, &st) == 0) migrations_apply(&(WMigrate){.db = h, .dir = dir});
  return h;
}

void db_default_set(WDB *h) {
  g_db_tried = true;
  db_set_default(h);
}

// A middleware with one job per app — the policy, the log line, recovery, the dev reload script,
// the document pass — is a *singleton*: an app that installs its own replaces the framework's
// default instead of stacking beside it. That is not cosmetic. `webc_main()` calls this after
// `webc_boot()`, so everything here runs *inside* the app's chain and, on the way out, before it:
// a second `mw_headers` would write the framework's policy after the app's, and the app's
// `HeadersCfg` would be the one that never reached the browser.
//
// `static` is deliberately not one of these: `/public` and `/uploads` are different files, not
// the same promise twice, and both belong in the chain.
static bool chain_has(const char *name) {
  WMiddleware mws[WC_MW_MAX];
  i32 n = webc_global_middleware(mws, WC_MW_MAX);
  for (i32 i = 0; i < n; i++)
    if (name && mws[i].name && strcmp(mws[i].name, name) == 0) return true;
  return false;
}

// `name == nullptr` means "this one is additive, install it either way".
static void use_default(WMiddleware mw, const char *name) {
  if (name && chain_has(name)) return;
  webc_use(mw);
}

// Everything a server needs, in the order that matters: recovery is installed
// before anything that can panic, and static files come last so a route wins over
// a file of the same name.
void webc_install_default_middleware(void) {
  // gzip first, so it is the last word on the body: everything registered below — the dev
  // reload script's injection included — writes into a response this then compresses.
  // `WEBC_NO_GZIP=1` is the way out, for an app that pre-compresses its own bytes or is
  // fronted by something that does.
  if (!getenv("WEBC_NO_GZIP")) use_default(mw_compress(), "compress");
  if (getenv("WEBC_DEV"))
    use_default(mw_dev(atoi(getenv("WEBC_DEV_PORT") ? getenv("WEBC_DEV_PORT") : "9999")), "dev");
  use_default(mw_request_id(), "request_id");
  // The policy from webc.json's "csp" block, which is the default policy when the project said
  // nothing (a CDN in `img-src` is a config decision, not twenty lines of C).
  use_default(mw_headers(wc_shell_csp()), "headers");
  use_default(mw_logging(), "logging");
  use_default(mw_recover(), "recover");
  const char *body_limit = getenv("WEBC_BODY_LIMIT");
  if (body_limit) webc_use(mw_body_limit(atoi(body_limit)));
  // /webc.js and /app.wasm for a project with a client half; a server-only app
  // simply has no such files and the middleware is a no-op for them.
  use_default(mw_web_assets(WC_WASM_PATH), "web-assets");
  webc_use(mw_static("/public", "public"));
  webc_use(mw_static("/assets", "public/assets"));
  // Innermost, and therefore the first post-`next` code to run: the page's <head> and the
  // request's nonce are spliced into the body while it is still HTML. Everything above this
  // line may rewrite the body afterwards (the dev reload script) or compress it (gzip), and
  // a splice that ran after them searched gzip bytes for `</head>` and found nothing.
  use_default(mw_document(), "document");
  // Inside the document pass: a 404 page's markup is spliced into the shell *before* the head
  // is applied, so the page a stranger gets for a wrong URL carries the app's title, theme and
  // nonce like every other page. Nothing is registered when webc.json names no file.
  if (*wc_shell_not_found()) use_default(mw_not_found(wc_shell_not_found()), "not_found");
}

// How many workers to start. sysconf(_SC_NPROCESSORS_ONLN) reports the HOST's cores
// inside a container, so a 2-CPU container started 10 workers and oversubscribed the
// box (measured: throughput peaked at 4 VUs and fell 35% by 10 VUs). WEBC_CPUS wins,
// then the cgroup v2/v1 quota, then the core count (0 = let webc_serve decide).
static i32 cgroup_cpu_quota(void) {
  FILE *f = fopen("/sys/fs/cgroup/cpu.max", "r"); // cgroup v2
  if (f) {
    char quota[32] = {0}, period[32] = {0};
    i32 n = fscanf(f, "%31s %31s", quota, period);
    fclose(f);
    if (n == 2 && strcmp(quota, "max") != 0) {
      long long q = atoll(quota), p = atoll(period);
      if (q > 0 && p > 0) return (i32)((q + p - 1) / p);
    }
    return 0; // "max": no quota
  }
  f = fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", "r"); // cgroup v1
  if (!f) return 0;
  long long q = -1;
  if (fscanf(f, "%lld", &q) != 1) q = -1;
  fclose(f);
  f = fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "r");
  if (!f) return 0;
  long long p = 0;
  if (fscanf(f, "%lld", &p) != 1) p = 0;
  fclose(f);
  if (q > 0 && p > 0) return (i32)((q + p - 1) / p);
  return 0;
}

static i32 worker_count(void) {
  const char *env = getenv("WEBC_CPUS");
  if (env && *env) {
    i32 v = atoi(env);
    if (v > 0) return v;
  }
  i32 q = cgroup_cpu_quota();
  if (q > 0) return q;
  return 0;
}

int webc_main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  // One arena for the life of the process: what the app keeps between requests
  // lives here, and `persist()` is the only door into it.
  Arena *app = arena_new_labeled(64 * 1024, "app");
  wc_app_arena_set(app);
  Arena *prev = wc_arena_swap(app);

  webc_boot();

  // `webc check` and `webc seed` re-run this binary with one variable set: the app's
  // boot has happened, the models are loaded, and the answer is one function away.
  int diag = webc_boot_diagnostics();
  if (diag >= 0) {
    fflush(stdout);
    return diag;
  }

  webc_install_default_middleware();

  const char *addr = getenv("WEBC_ADDR");
  char buf[64];
  if (!addr) {
    const char *port = getenv("WEBC_PORT");
    if (!port) port = getenv("PORT");
    snprintf(buf, sizeof buf, ":%s", port ? port : "8080");
    addr = buf;
  }
  int rc = webc_serve(&(WebcOptions){
      .cpus = worker_count(),
      .addr = addr,
      .metrics = getenv("WEBC_NO_METRICS") ? false : true,
      // diagnostic build only: `WEBC_DEBUG=1` mounts /debug and /debug/cpu,
      // which the deploy guide documents but boot.c never wired up.
      .debug = getenv("WEBC_DEBUG") ? true : false,
  });

  wc_arena_swap(prev);
  WDB *h = db();
  if (h) db_close(h);
  arena_destroy(app);
  return rc;
}

// Weak, and that is the point: an app with its own `main()` (a CLI beside the
// server, a test binary) defines one and the framework's is ignored.
__attribute__((weak)) int main(int argc, char **argv) { return webc_main(argc, argv); }