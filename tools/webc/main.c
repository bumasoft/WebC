// SPDX-License-Identifier: Apache-2.0
// webc — the WebC command line (SPEC §8).
//
//   webc new myapp          scaffold a project
//   webc build              compile src/*.c against libwebc.a
//   webc dev                build, run, rebuild on save, reload the browser
//   webc test               build and run tests/*_test.c plus the JS bridge test
//   webc migrate up|status|new NAME|dry|diff
//   webc doctor             check the toolchain and the project
//   webc routes             list the routes declared in src/
//   webc fmt [--check]
//
// It has no dependencies beyond a C compiler and, for `dev`, nothing at all: the
// reload server is the framework's own HTTP server.
#define WEBC_NO_SELECT_MACRO 1
#include <webc/client.h>
#include <webc/core.h>
#include <webc/crypto.h>
#include <webc/db.h>
#include <webc/http.h>
#include <webc/json.h>
#include <webc/log.h>
#include <webc/model.h>
#include <webc/task.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "explain.h"
#include "api_search.h"
#include "facts.h"
#include "tty.h"
#include "prefix.h"
#include "cli.h"

// Darwin spells the nanosecond field of struct stat st_mtimespec; glibc and POSIX spell
// it st_mtim. Same two members, so one #define covers the four places the watcher reads
// it — the same line src/http.c and src/middleware.c already carry.
#if !defined(__APPLE__)
#define st_mtimespec st_mtim
#endif

// Every write in this file goes through the terminal layer (tty.h): it keeps the
// escape sequences away from a pipe, a log and an agent, and it is the one place that
// decides. The macros are here, once, so no call site has to remember.
#define printf(...) wc_printf(__VA_ARGS__)
#define fprintf(...) wc_fprintf(__VA_ARGS__)
#define puts(s) wc_puts(s)

// The CLI's own version is the framework's: it comes from VERSION through -DWEBC_VERSION,
// with the fallback in <webc/core.h> for a hand-written compile. The literal that used to
// live here was the eleventh copy of the same number in the tree.
#define CLI_VERSION WEBC_VERSION

// Not the framework version: the number `webc new` writes into a fresh app's webc.json.
// Those are different questions — an app made today is 0.1.0 whatever the framework is —
// and collapsing them would have made every generated project claim to be the toolchain.
#define APP_VERSION_DEFAULT "0.1.0"

static const char *g_self = "webc"; // argv[0], for the commands that run themselves
static char g_pack[128];            // `webc deploy --pack`: which ops files to write beside the artifact

// ---------------------------------------------------------------- helpers

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "webc: ");
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

// `g_quiet` is for commands whose whole output is a table: `webc size --modules` builds
// twenty probe apps and the "built …" lines would bury the twenty numbers it exists for.
static bool g_quiet = false;

static void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void info(const char *fmt, ...) {
  if (g_quiet) return;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stdout, "\033[36mwebc\033[0m ");
  vfprintf(stdout, fmt, ap);
  va_end(ap);
  fputc('\n', stdout);
  fflush(stdout);
}

static bool path_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

static u64 mtime_of(const char *p) {
  struct stat st;
  return stat(p, &st) == 0
             ? (u64)st.st_mtimespec.tv_sec * 1000ull + (u64)st.st_mtimespec.tv_nsec / 1000000ull
             : 0;
}

static void mkdirs(const char *p) {
  char tmp[1024];
  snprintf(tmp, sizeof tmp, "%s", p);
  for (char *q = tmp + 1; *q; q++) {
    if (*q != '/') continue;
    *q = 0;
    mkdir(tmp, 0755);
    *q = '/';
  }
  mkdir(tmp, 0755);
}

// The object cache is the one directory the CLI may delete: it exists to be thrown away,
// and a build that cannot tell a stale object from a bad source needs to be able to.
static void rm_rf(const char *path) {
  DIR *d = opendir(path);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      char p[1200];
      snprintf(p, sizeof p, "%s/%s", path, e->d_name);
      struct stat st;
      if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
        rm_rf(p);
      else
        unlink(p);
    }
    closedir(d);
  }
  rmdir(path);
}

static void write_file(const char *path, const char *content) {
  char dir[1024];
  snprintf(dir, sizeof dir, "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = 0;
    mkdirs(dir);
  }
  FILE *f = fopen(path, "wb");
  if (!f) die("cannot write %s", path);
  fwrite(content, 1, strlen(content), f);
  fclose(f);
}

static str read_file(Arena *a, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return (str){"", 0};
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = arena_alloc(a, (size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = 0;
  return (str){buf, got};
}

// ---------------------------------------------------------------- webc.json

typedef struct FileSet {
  char paths[64][1024];
  i32 n;
} FileSet;

typedef struct Config {
  char name[64];
  char version[32];
  char root[512];
  char src[512];
  char entry[512];
  char public_dir[512];
  char web[512]; // src/web.c: the wasm half, when the app has one
  char database[256];
  char migrations[256];
  int port;
  int dev_port;
  char dev_connect[512]; // `dev.connect`: CSP origins the app calls (space separated)
  i64 wasm_budget;       // `budget.wasm`: gzipped browser half, 0 = this project declares none
  bool dev;
  char cflags[512];
  char ldflags[512];
  // The three file sets: a project that names them is split into a shared
  // middle, a server half and a client half, and `webc build` links each target
  // from its own set. Old projects name none of them and keep the src/ scan.
  FileSet server, shared, client;
  bool has_sets;
  // `lint.exempt`: docs that quote a retired spelling *in order to forbid it*. An app's own
  // history page is that kind of paper too, and without a way to say so `webc lint --all`
  // cannot pass in a repo whose docs are honest about an older spelling.
  char lint_exempt[16][512];
  i32 lint_exempt_n;
} Config;

static str jstr(JVal *v, const char *key, const char *dflt) {
  JVal *f = json_get(v, key);
  if (f && f->kind == J_STR) return f->s;
  return str_lit(dflt);
}

static i64 jint(JVal *v, const char *key, i64 dflt) {
  JVal *f = json_get(v, key);
  return f && (f->kind == J_NUM) ? (i64)f->num : dflt;
}

static void config_settle(Config *c);

// One array of paths from webc.json, relative to the project root.
static void config_read_set(JVal *v, const char *key, const char *root, FileSet *out) {
  JVal *arr = json_get(v, key);
  if (!arr || arr->kind != J_ARR) return;
  for (i32 i = 0; i < arr->n && out->n < 64; i++) {
    if (arr->items[i]->kind != J_STR) continue;
    str p = arr->items[i]->s;
    snprintf(out->paths[out->n++], 1024, "%s/%.*s", root, (int)p.n, p.p);
  }
}

// ---------------------------------------------------------------- webc.json, checked
//
// Every key this file reads is in the table below, and every key that is not is a mistake:
// silence ("clinet": [...] builds nothing and says nothing) is the worst possible answer to
// a spelling error. The table is the schema — one place, next to
// the readers, so a key added without a line here fails the framework's own gate.
typedef enum { CK_STR, CK_INT, CK_BOOL, CK_ARR, CK_OBJ } CfgKind;

typedef struct CfgKey {
  const char *name;
  CfgKind kind;
  const char *what;
} CfgKey;

static const CfgKey CFG_TOP[] = {
    {"name", CK_STR, "the app's name, in logs and the page title"},
    {"version", CK_STR, "what `webc version` and the deploy manifest print"},
    {"description", CK_STR, "one line for the manifest"},
    {"mode", CK_STR, "\"web\" for a browser app (the CLI reads the file sets, not this)"},
    {"target", CK_STR, "\"wasm\" for a browser app (the CLI reads the file sets, not this)"},
    {"entry", CK_STR, "the server half's main source, when it is not src/main.c"},
    {"web", CK_STR, "the browser half's entry, for a project with no server binary"},
    {"database", CK_STR, "a URL, e.g. sqlite:./data/dev.db or postgres://…"},
  {"migrations", CK_STR, "the migrations directory, when it is not ./migrations"},
    {"port", CK_INT, "the port `webc dev` serves on"},
    {"cflags", CK_STR, "extra compiler flags, space separated"},
    {"ldflags", CK_STR, "extra linker flags, space separated"},
    {"server", CK_ARR, "the sources only the server half compiles"},
    {"shared", CK_ARR, "the sources both halves compile"},
    {"client", CK_ARR, "the sources only the browser half compiles"},
    {"dev", CK_OBJ, "the dev server's block: port, connect"},
    {"boot", CK_OBJ, "the page shell: title, lang, css, icon, el, portal, data, hydrate"},
    {"budget", CK_OBJ, "what the app promises to weigh: \"wasm\", gzipped bytes"},
    {"csp", CK_OBJ, "the Content-Security-Policy: img_src, script_src, no_default"},
    {"not_found", CK_STR, "an HTML file a browser gets for a URL nobody routed, e.g. \"public/404.html\""},
    {"lint", CK_OBJ, "the lint gate: which docs may name a retired spelling on purpose"},
};

static const CfgKey CFG_LINT[] = {
    {"exempt", CK_ARR, "markdown files that quote a retired spelling to forbid it"},
};

static const CfgKey CFG_BUDGET[] = {
    {"wasm", CK_INT, "the browser half's gzip limit in bytes; 0 or absent = no promise"},
};

static const CfgKey CFG_DEV[] = {
    {"port", CK_INT, "the reload socket's port"},
    {"connect", CK_ARR, "origins the dev server may proxy to (one string or a list)"},
};

static const CfgKey CFG_CSP[] = {
    {"img_src", CK_ARR, "image sources, default \"'self' data:\" — a CDN goes here"},
    {"script_src", CK_ARR, "script sources, default \"'self' + the page's nonce"},
    {"no_default", CK_BOOL, "the app writes the header itself; the framework emits none"},
};

static const CfgKey CFG_BOOT[] = {
    {"title", CK_STR, "the <title>, default \"webc app\""},
    {"lang", CK_STR, "the <html lang>, default \"en\""},
    {"icon", CK_STR, "the favicon href, a data: URI included"},
    {"css", CK_ARR, "stylesheet hrefs, in order"},
    {"el", CK_STR, "the mount point, default \"#root\""},
    {"portal", CK_STR, "where modals and tooltips are appended"},
    {"data", CK_STR, "the URL the module fetches its seed from"},
    {"wasm", CK_STR, "the module's URL, default \"/app.wasm\""},
    {"hydrate", CK_BOOL, "adopt the server's markup instead of rebuilding it"},
    {"inline", CK_BOOL, "embed the seed in the page (needs webc_seed())"},
    {"html", CK_OBJ, "extra <html> attributes, name → value"},
};

static const CfgKey *cfg_find(const CfgKey *t, size_t n, str key) {
  for (size_t i = 0; i < n; i++)
    if (key.n == strlen(t[i].name) && memcmp(key.p, t[i].name, key.n) == 0) return &t[i];
  return nullptr;
}

// "clinet" -> "client": one transposition away, which is the shape of most typos. Cheap
// Levenshtein, capped at two edits — a suggestion is a kindness, not a guess.
static const char *cfg_suggest(const CfgKey *t, size_t n, str key) {
  const char *best = nullptr;
  i32 best_d = 3;
  for (size_t i = 0; i < n; i++) {
    str cand = {(char *)t[i].name, strlen(t[i].name)};
    if (key.n > 32 || cand.n > 32) continue;
    i32 d[33][33];
    for (i32 r = 0; r <= (i32)key.n; r++) d[r][0] = r;
    for (i32 c2 = 0; c2 <= (i32)cand.n; c2++) d[0][c2] = c2;
    for (i32 r = 1; r <= (i32)key.n; r++)
      for (i32 c2 = 1; c2 <= (i32)cand.n; c2++) {
        i32 sub = d[r - 1][c2 - 1] + (key.p[r - 1] != cand.p[c2 - 1]);
        i32 del = d[r - 1][c2] + 1, ins = d[r][c2 - 1] + 1;
        d[r][c2] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
      }
    if (d[key.n][cand.n] < best_d) {
      best_d = d[key.n][cand.n];
      best = t[i].name;
    }
  }
  return best_d <= 2 ? best : nullptr;
}

static bool cfg_kind_ok(JVal *v, CfgKind k) {
  if (!v) return true;
  switch (k) {
  case CK_STR: return v->kind == J_STR;
  case CK_INT: return v->kind == J_NUM;
  case CK_BOOL: return v->kind == J_BOOL;
  case CK_ARR: return v->kind == J_ARR;
  case CK_OBJ: return v->kind == J_OBJ;
  }
  return true;
}

static const char *cfg_kind_name(CfgKind k) {
  switch (k) {
  case CK_STR: return "a string";
  case CK_INT: return "a number";
  case CK_BOOL: return "true or false";
  case CK_ARR: return "a list";
  case CK_OBJ: return "an object";
  }
  return "a value";
}

// Returns the number of problems, having printed each one with the line's own words.
static i32 config_reject(const CfgKey *t, size_t n, JVal *obj, const char *path, const char *where) {
  i32 bad = 0;
  for (i32 i = 0; i < obj->n; i++) {
    str key = obj->keys[i];
    JVal *val = obj->items[i];
    const CfgKey *k = cfg_find(t, n, key);
    if (!k) {
      const char *hint = cfg_suggest(t, n, key);
      fprintf(stderr, "%s: unknown key \"%.*s\"%s", path, (int)key.n, key.p, where);
      if (hint)
        fprintf(stderr, " — did you mean \"%s\"?\n", hint);
      else
        fprintf(stderr, " — this version of webc has no such setting\n");
      bad++;
    } else if (!cfg_kind_ok(val, k->kind)) {
      fprintf(stderr, "%s: \"%.*s\"%s wants %s — %s\n", path, (int)key.n, key.p, where,
              cfg_kind_name(k->kind), k->what);
      bad++;
    }
  }
  return bad;
}

static void config_validate(JVal *v, const char *path) {
  if (!v || v->kind != J_OBJ) {
    die("%s: the top level must be an object, not a %s", path,
        v && v->kind == J_ARR ? "list" : "value");
  }
  i32 bad = config_reject(CFG_TOP, WC_COUNTOF(CFG_TOP), v, path, "");
  JVal *dev = json_get(v, "dev");
  if (dev && dev->kind == J_OBJ)
    bad += config_reject(CFG_DEV, WC_COUNTOF(CFG_DEV), dev, path, " in \"dev\"");
  JVal *boot = json_get(v, "boot");
  if (boot && boot->kind == J_OBJ)
    bad += config_reject(CFG_BOOT, WC_COUNTOF(CFG_BOOT), boot, path, " in \"boot\"");
  JVal *csp = json_get(v, "csp");
  if (csp && csp->kind == J_OBJ)
    bad += config_reject(CFG_CSP, WC_COUNTOF(CFG_CSP), csp, path, " in \"csp\"");
  JVal *budget = json_get(v, "budget");
  if (budget && budget->kind == J_OBJ)
    bad += config_reject(CFG_BUDGET, WC_COUNTOF(CFG_BUDGET), budget, path, " in \"budget\"");
  JVal *lint = json_get(v, "lint");
  if (lint && lint->kind == J_OBJ)
    bad += config_reject(CFG_LINT, WC_COUNTOF(CFG_LINT), lint, path, " in \"lint\"");
  // one file, one file set: the same path in server and client compiles it twice, with
  // different defines, and links whichever object the linker sees first
  static const char *sets[] = {"server", "shared", "client"};
  for (i32 a = 0; a < 3; a++)
    for (i32 b = a + 1; b < 3; b++) {
      JVal *va = json_get(v, sets[a]), *vb = json_get(v, sets[b]);
      if (!va || !vb || va->kind != J_ARR || vb->kind != J_ARR) continue;
      for (i32 i = 0; i < va->n; i++)
        for (i32 j = 0; j < vb->n; j++) {
          str x = json_as_str(json_at(va, i)), y = json_as_str(json_at(vb, j));
          if (x.n && str_eq(x, y)) {
            fprintf(stderr, "%s: \"%.*s\" is in both %s and %s — pick one set\n", path,
                    (int)x.n, x.p, sets[a], sets[b]);
            bad++;
          }
        }
    }
  if (bad) die("%s: %d problem(s); the settings it accepts are in docs/reference.md §13e", path, bad);
}

static void config_load(Config *c, const char *root) {
  memset(c, 0, sizeof *c);
  snprintf(c->root, sizeof c->root, "%s", root);
  snprintf(c->name, sizeof c->name, "app");
  snprintf(c->version, sizeof c->version, APP_VERSION_DEFAULT);
  snprintf(c->src, sizeof c->src, "%s/src", root);
  snprintf(c->entry, sizeof c->entry, "%s/src/main.c", root);
  snprintf(c->public_dir, sizeof c->public_dir, "%s/public", root);
  snprintf(c->migrations, sizeof c->migrations, "%s/migrations", root);
  snprintf(c->web, sizeof c->web, "%s/src/web.c", root);
  snprintf(c->database, sizeof c->database, "sqlite:%s/data/dev.db", root);
  c->port = 8080;
  c->dev_port = 9999;
  c->dev_connect[0] = 0;
  c->dev = true;

  char path[1024];
  snprintf(path, sizeof path, "%s/webc.json", root);
  if (!path_exists(path)) {
    config_settle(c);
    return;
  }
  Arena *a = arena_new(16 * 1024);
  str text = read_file(a, path);
  JParse jp = json_parse(a, text);
  if (!jp.ok) die("%s is not valid JSON", path);
  JVal *v = jp.v;
  config_validate(v, path);
  str name = jstr(v, "name", "app");
  str version = jstr(v, "version", APP_VERSION_DEFAULT);
  str entry = jstr(v, "entry", "src/main.c");
  str db = jstr(v, "database", "");
  snprintf(c->name, sizeof c->name, "%.*s", (int)name.n, name.p);
  snprintf(c->version, sizeof c->version, "%.*s", (int)version.n, version.p);
  snprintf(c->entry, sizeof c->entry, "%s/%.*s", root, (int)entry.n, entry.p);
  if (db.n) snprintf(c->database, sizeof c->database, "%.*s", (int)db.n, db.p);
  c->port = (int)jint(v, "port", 8080);
  JVal *dev = json_get(v, "dev");
  if (dev) c->dev_port = (int)jint(dev, "port", 9999);
  // "connect": one origin or a list. `webc dev` is a browser app's only dev server, so an
  // app that talks to a public API has to be able to name it.
  if (dev) {
    JVal *con = json_get(dev, "connect");
    size_t at = 0;
    if (con && con->kind == J_STR && con->s.n) {
      snprintf(c->dev_connect, sizeof c->dev_connect, "%.*s", (int)con->s.n, con->s.p);
    } else if (con && con->kind == J_ARR) {
      for (i32 i = 0; i < con->n && at + 1 < sizeof c->dev_connect; i++) {
        JVal *it = json_at(con, i);
        if (!it || it->kind != J_STR || it->s.n == 0) continue;
        int w = snprintf(c->dev_connect + at, sizeof c->dev_connect - at, "%s%.*s",
                         at ? " " : "", (int)it->s.n, it->s.p);
        if (w > 0) at += (size_t)w;
      }
    }
  }
  {
    JVal *budget = json_get(v, "budget");
    if (budget && budget->kind == J_OBJ) c->wasm_budget = jint(budget, "wasm", 0);
  }
  {
    JVal *lint = json_get(v, "lint");
    JVal *exempt = lint && lint->kind == J_OBJ ? json_get(lint, "exempt") : nullptr;
    if (exempt && exempt->kind == J_ARR)
      for (i32 i = 0; i < exempt->n && c->lint_exempt_n < 16; i++) {
        str one = json_as_str(json_at(exempt, i));
        if (one.n == 0) continue;
        snprintf(c->lint_exempt[c->lint_exempt_n++], sizeof c->lint_exempt[0], "%.*s",
                 (int)one.n, one.p);
      }
  }
  str web = jstr(v, "web", "");
  if (web.n) snprintf(c->web, sizeof c->web, "%s/%.*s", root, (int)web.n, web.p);
  // The library looks for migrations through WEBC_MIGRATIONS (default ./migrations), so
  // the key is the CLI's to hand over: a project that keeps them elsewhere says so here
  // once instead of in every shell.
  str mig = jstr(v, "migrations", "");
  if (mig.n) snprintf(c->migrations, sizeof c->migrations, "%s/%.*s", root, (int)mig.n, mig.p);
  // "server": [...], "shared": [...], "client": [...]
  config_read_set(v, "server", root, &c->server);
  config_read_set(v, "shared", root, &c->shared);
  config_read_set(v, "client", root, &c->client);
  c->has_sets = c->server.n || c->shared.n || c->client.n;
  // A project that names a web entry and no server entry is a browser-only app:
  // `webc dev` then serves public/ + the module instead of trying to link a server
  // binary that has no main(). Without this the default `src/main.c` made a
  // web-only project look like a server one and fail with "undefined _main".
  if (web.n && !json_get(v, "entry")) c->entry[0] = '\0';
  str cflags = jstr(v, "cflags", "");
  str ldflags = jstr(v, "ldflags", "");
  snprintf(c->cflags, sizeof c->cflags, "%.*s", (int)cflags.n, cflags.p);
  snprintf(c->ldflags, sizeof c->ldflags, "%.*s", (int)ldflags.n, ldflags.p);
  arena_destroy(a);
  config_settle(c);
}

// A layout is a convention, not a rule: an app may keep its one source file at
// the root, or name a browser half that does not exist. Settle that once, here,
// so build/dev/test all look at the same paths.
static void config_settle(Config *c) {
  char p[1024];
  // A web entry that isn't there is not a web entry: settle it first, or the
  // browser-only check below would hide a plain `main.c` project from itself.
  if (c->web[0] && !path_exists(c->web)) c->web[0] = 0;
  if (!path_exists(c->entry)) {
    if (c->web[0]) {
      // browser-only: an app with a web entry and no server entry has no main()
      c->entry[0] = '\0';
    } else {
      // the root main.c convention
      snprintf(p, sizeof p, "%s/main.c", c->root);
      if (path_exists(p)) snprintf(c->entry, sizeof c->entry, "%s", p);
    }
  }
}

// ---------------------------------------------------------------- sources

typedef struct Sources {
  char paths[256][1024];
  i32 n;
} Sources;

static void collect_c(Sources *s, const char *dir, bool recursive) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) && s->n < 256) {
    if (e->d_name[0] == '.') continue;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (recursive) collect_c(s, p, recursive);
      continue;
    }
    size_t len = strlen(e->d_name);
    if (len > 2 && strcmp(e->d_name + len - 2, ".c") == 0)
      snprintf(s->paths[s->n++], 1024, "%s", p);
  }
  closedir(d);
}

typedef enum Target { TARGET_SERVER, TARGET_CLIENT } Target;

static void sources_add(Sources *s, const char *p) {
  if (!p || !p[0] || !path_exists(p)) return;
  for (i32 i = 0; i < s->n; i++)
    if (strcmp(s->paths[i], p) == 0) return;
  if (s->n < 256) snprintf(s->paths[s->n++], 1024, "%s", p);
}

// Declaration order matters to nobody and to the linker: shared first, then the
// half being built, so a symbol is always defined in the file a reader expects.
static void collect_set(Config *c, Sources *s, Target t) {
  memset(s, 0, sizeof *s);
  for (i32 i = 0; i < c->shared.n; i++) sources_add(s, c->shared.paths[i]);
  const FileSet *own = t == TARGET_SERVER ? &c->server : &c->client;
  for (i32 i = 0; i < own->n; i++) sources_add(s, own->paths[i]);
  if (s->n == 0 && c->has_sets && t == TARGET_CLIENT) {
    // a project may keep its client half in one `web` file and declare no set
    sources_add(s, c->web);
    for (i32 i = 0; i < c->shared.n; i++) sources_add(s, c->shared.paths[i]);
  }
}

// Every .c file the app needs, each exactly once: the entry (and the browser half,
// which may be the same file) plus everything else under the source directory —
// unless the project declares file sets, in which case the set *is* the answer and
// a stray file is not silently compiled into a target that cannot run it.
static void collect_sources_for(Config *c, Sources *s, Target t);

static void collect_sources(Config *c, Sources *s) {
  collect_sources_for(c, s, TARGET_SERVER);
}

static void collect_sources_for(Config *c, Sources *s, Target t) {
  if (c->has_sets) {
    collect_set(c, s, t);
    return;
  }
  memset(s, 0, sizeof *s);
  Sources more = {0};
  collect_c(&more, c->src, true);
  for (i32 i = 0; i < more.n; i++)
    if (s->n < 256) snprintf(s->paths[s->n++], 1024, "%s", more.paths[i]);
  const char *extra[2] = {c->entry, c->web};
  for (i32 e = 0; e < 2; e++) {
    if (!extra[e][0] || !path_exists(extra[e])) continue;
    bool dup = false;
    for (i32 i = 0; i < s->n; i++)
      if (strcmp(s->paths[i], extra[e]) == 0) dup = true;
    if (!dup && s->n < 256) snprintf(s->paths[s->n++], 1024, "%s", extra[e]);
  }
}

// ---------------------------------------------------------------- build

static char g_cc[128] = "cc";

static void pick_cc(void) {
  const char *env = getenv("CC");
  if (env) {
    snprintf(g_cc, sizeof g_cc, "%s", env);
    return;
  }
  const char *candidates[] = {"clang", "cc", "gcc"};
  for (size_t i = 0; i < WC_COUNTOF(candidates); i++) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", candidates[i]);
    if (system(cmd) == 0) {
      snprintf(g_cc, sizeof g_cc, "%s", candidates[i]);
      return;
    }
  }
}

// Does this compiler speak C23? A machine with only Ubuntu's default `cc` gets gcc 13, which
// rejects `-std=c23` outright — and the first thing the reader saw used to be
// "cc: error: unrecognized command-line option '-std=c23'", then a cache-clearing retry of a
// build that could never succeed. Ask once, cache the answer, and say what to install.
static int g_c23 = -1; // -1 unasked, 0 no, 1 yes

static bool cc_does_c23(void) {
  if (g_c23 >= 0) return g_c23 == 1;
  char cmd[512], outbin[256];
  // `printf … | cc -x c -` reads the source on stdin, so the probe leaves no .c file behind and
  // needs no temporary directory. The binary lands in /tmp and is removed either way.
  snprintf(outbin, sizeof outbin, "/tmp/.webc_c23_probe_%d", (int)getpid());
  snprintf(cmd, sizeof cmd,
           "printf 'int main(void){ return 0; }\\n' | %s -std=c23 -x c -o %s - >/dev/null 2>&1",
           g_cc, outbin);
  g_c23 = system(cmd) == 0 ? 1 : 0;
  remove(outbin);
  return g_c23 == 1;
}

// The message for a compiler that cannot build this project, in the words of the fix.
static void say_c23_missing(void) {
  char cmd[256], ver[256] = "";
  snprintf(cmd, sizeof cmd, "%s --version 2>/dev/null | head -1", g_cc);
  FILE *p = popen(cmd, "r");
  if (p) {
    if (fgets(ver, sizeof ver, p)) {
      size_t n = strlen(ver);
      while (n && (ver[n - 1] == '\n' || ver[n - 1] == '\r')) ver[--n] = 0;
    }
    pclose(p);
  }
  fprintf(stderr,
          "\033[1mwebc\033[0m %s cannot build C23%s%s\n"
          "      WebC needs clang 21 or newer: the framework and its suite use \033[1m-std=c23\033[0m\n"
          "      and \033[1m#embed\033[0m, and a compiler without them fails on the first file.\n"
          "\n"
          "      macOS:    brew install llvm && export CC=$(brew --prefix llvm)/bin/clang\n"
          "      Debian:   apt install clang-21 && export CC=clang-21\n"
          "\n"
          "      Then \033[1mwebc doctor\033[0m says what it found.\n",
          g_cc, ver[0] ? " — " : "", ver);
}

// The same, without the echo: `webc size` asks llvm-nm a question, and a tool's whole
// symbol table is not a message to the person running it.
static bool run_cmd_quiet(const char *cmd, Buf *out) {
  FILE *f = popen(cmd, "r");
  if (!f) return false;
  char line[4096];
  while (fgets(line, sizeof line, f))
    if (out) buf_puts(out, line);
  return pclose(f) == 0;
}

// `webc dev` wants the compiler's words twice: in the terminal, where a developer already
// looks, and in the browser, where the overlay is. These two globals are how a build says
// "keep what the tools printed" without every call site growing a parameter.
static char *g_build_log;
static size_t g_build_log_cap;

static void build_log_append(const char *s, size_t n) {
  if (!g_build_log || !g_build_log_cap) return;
  size_t used = strlen(g_build_log);
  if (used + n + 1 >= g_build_log_cap) return;
  memcpy(g_build_log + used, s, n);
  g_build_log[used + n] = 0;
}

// Same as run_cmd(), but keeps what the tool said. The wasm link is the one place
// where the compiler's message is about a *symbol*, not about the app's own mistake:
// `undefined symbol: wc_prose` names something no app ever typed, in an object file
// with a temp name. Catching it here is what lets `webc check` say which of the app's
// files called it and what to do instead.
static bool run_cmd_capture(const char *cmd, Buf *out) {
  if (getenv("WEBC_VERBOSE")) fprintf(stderr, "  $ %s\n", cmd);
  // 2>&1: wasm-ld says everything interesting on stderr, and this is the one run whose
  // stderr is worth reading back.
  char shell[8192];
  snprintf(shell, sizeof shell, "%s 2>&1", cmd);
  FILE *f = popen(shell, "r");
  if (!f) return false;
  char line[4096];
  while (fgets(line, sizeof line, f)) {
    if (out) buf_puts(out, line);
    build_log_append(line, strlen(line));
    fputs(line, stderr);
  }
  int rc = pclose(f);
  return rc == 0;
}

// `webc dev` wants the compiler's words twice: in the terminal, where a developer already
// looks, and in the browser, where the overlay is. These two globals are how a build says
// "keep what the tools printed" without every call site growing a parameter.
static bool run_cmd_logged(const char *cmd) {
  char shell[16384];
  snprintf(shell, sizeof shell, "%s 2>&1", cmd);
  FILE *f = popen(shell, "r");
  if (!f) return false;
  char line[4096];
  while (fgets(line, sizeof line, f)) {
    fputs(line, stderr);
    build_log_append(line, strlen(line));
  }
  return pclose(f) == 0;
}

static bool run_cmd(const char *cmd, bool quiet_ok) {
  if (getenv("WEBC_VERBOSE")) fprintf(stderr, "  $ %s\n", cmd);
  if (g_build_log) return run_cmd_logged(cmd);
  int rc = system(cmd);
  if (rc != 0) {
    // The tool already said what went wrong, in its own words, on the line above.
    // Repeating its command line under it is noise a person learns to skip and an
    // agent pays tokens for; WEBC_VERBOSE is where it belongs.
    if (!quiet_ok && getenv("WEBC_VERBOSE")) fprintf(stderr, "webc: command failed: %s\n", cmd);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- incremental build
//
// A save must cost one compile and one link, not a rebuild of everything. Objects live in
// .webc/obj/ and are recompiled only when their source changed, when one of the headers
// clang says they included changed (the .d file next to the object is the only honest
// answer to that question), or when the flags changed. The wasm half is cached the same
// way: clang -c per file, then one link through wasm-ld.
//
// Measured on a 40-file app: a no-op build 1.9 s → 0.16 s, one edited file in the server
// half 2.2 s → 0.4 s, and one edited shared file (both halves) 0.6 s.
//
// WEBC_NO_CACHE=1 turns it off, because a cache that is wrong is worse than no cache: it
// turns "my change did nothing" into the most confusing bug in the tool.

static char *read_all(const char *path, i64 *n_out);
static int docs_emit(const char *dir);

typedef struct Stamp {
  i64 mtime; // nanoseconds
  i64 size;
} Stamp;

static Stamp stamp_of(const char *p) {
  Stamp s = {0, 0};
  struct stat st;
  if (stat(p, &st) != 0) return s;
  s.mtime = (i64)st.st_mtime * 1000000000LL + (i64)st.st_mtimespec.tv_nsec;
  s.size = (i64)st.st_size;
  return s;
}

// The framework library's own stamp: a rebuilt libwebc must invalidate every object.
static Stamp stamp_of_fw_lib(void) {
  char lib[1024];
  snprintf(lib, sizeof lib, "%s/build/libwebc.a", webc_prefix());
  return stamp_of(lib);
}

static bool cache_off(void) {
  const char *e = getenv("WEBC_NO_CACHE");
  return e && *e && strcmp(e, "0") != 0;
}

// `webc build --explain` says why a file got compiled: the same lines WEBC_TRACE_WATCH has
// always printed, but as a flag, because the moment somebody needs them is the moment they
// are staring at a build that did the wrong thing, not the moment they read the docs.
// WEBC_TRACE_WATCH still works — it is also the dev server's watch log.
static bool g_explain;
static bool trace_on(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("WEBC_TRACE_WATCH");
    v = (g_explain || (e && *e && strcmp(e, "0") != 0)) ? 1 : 0;
  }
  return v == 1;
}

// A path turned into a file name: "src/view.c" -> "src_view_c". Collisions are possible
// only between two files whose paths differ by punctuation alone, and the framework's own
// sources are prefixed separately, so this is enough — and it keeps the object directory
// readable, which matters the first time somebody has to look inside it.
static void obj_slug(char *out, size_t cap, const char *prefix, const char *path) {
  size_t o = 0;
  o += (size_t)snprintf(out, cap, "%s", prefix);
  // An absolute path (the framework's own sources) becomes its base name plus a hash:
  // "fw_core_c_9a1f2b3c", not 90 characters of directory. A project-relative one keeps its
  // shape ("src_view_c"), because that is the name a person reads in the object directory.
  if (path[0] == '/') {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    u64 h = 1469598103934665603ull;
    for (const char *q = path; *q; q++) {
      h ^= (u8)*q;
      h *= 1099511628211ull;
    }
    for (; *b && o + 10 < cap; b++)
      out[o++] = ((*b >= 'a' && *b <= 'z') || (*b >= 'A' && *b <= 'Z') ||
                  (*b >= '0' && *b <= '9'))
                     ? *b
                     : '_';
    o += (size_t)snprintf(out + o, cap - o, "_%08x", (unsigned)(h >> 32));
    out[o] = 0;
    return;
  }
  while (path[0] == '.' && path[1] == '/') path += 2;
  for (; *path && o + 1 < cap; path++) {
    char ch = *path;
    out[o++] = ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) ? ch
                                                                                                : '_';
  }
  out[o] = 0;
}

// Is `obj` up to date for `src` compiled with `flags`?
static bool obj_fresh(const char *obj, const char *flags) {
  bool dbg = trace_on();
  Stamp os = stamp_of(obj);
  if (!os.mtime) return false;
  char side[1200];
  snprintf(side, sizeof side, "%s.flags", obj);
  i64 n = 0;
  char *prev = read_all(side, &n);
  if (!prev) {
    if (dbg) fprintf(stderr, "  [stale] %s: no .flags\n", obj);
    return false;
  }
  bool same = strcmp(prev, flags) == 0;
  if (!same && dbg) fprintf(stderr, "  [stale] %s: flags differ\n  was: %s\n  now: %s\n", obj, prev, flags);
  free(prev);
  if (!same) return false;
  // the .d lives next to the *stem*, not next to the object ("view.o" -> "view.d")
  char dep[1200];
  snprintf(dep, sizeof dep, "%s", obj);
  size_t dl = strlen(dep);
  if (dl > 2) dep[dl - 2] = 0;
  size_t dl2 = strlen(dep);
  snprintf(dep + dl2, sizeof dep - dl2, ".d");
  char *deps = read_all(dep, &n);
  if (!deps) {
    if (dbg) fprintf(stderr, "  [stale] %s: no .d\n", obj);
    return false;
  }
  char *p = strchr(deps, ':');
  p = p ? p + 1 : deps;
  bool fresh = true;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!*p) break;
    // clang writes one dependency per line with a trailing backslash, and escapes a
    // space inside a path as "\ ". Both have to be understood: a path with a space in it
    // (any home directory in a project under "Application Support") otherwise parses into
    // two names that do not exist, and every build recompiles everything forever.
    char *tok = p, *w = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
      if (*p == '\\') {
        if (p[1] == '\n' || p[1] == '\r') { // continuation: end of this token
          p += (p[1] == '\r' && p[2] == '\n') ? 3 : 2;
          break;
        }
        if (p[1]) {
          *w++ = p[1];
          p += 2;
          continue;
        }
      }
      *w++ = *p++;
    }
    // `w` is at the token's end and `p` is at the delimiter that ended it — which may be
    // the same byte. Terminating the token in place would then zero the character the scan
    // continues from, and the loop stopped after the FIRST dependency of every file. (That
    // is how an edited source could be declared fresh: only the SDK path was ever checked.)
    // `w` ends at the delimiter that stopped the token (they differ only when an escape
    // shortened the text), so step *past* the delimiter before writing the terminator over
    // it. Writing the NUL first zeroed the byte the scan resumes from, and the dependency
    // loop stopped after the first name in every .d file — which made an edited source look
    // fresh to the object cache.
    char *delim = p;
    if (*delim) p++;
    *w = 0;
    if (!*tok) continue;
    Stamp ds = stamp_of(tok);
    if (!ds.mtime || ds.mtime > os.mtime) {
      if (dbg)
        fprintf(stderr, "  [stale] %s: %s mtime=%lld obj=%lld\n", obj, tok, (long long)ds.mtime,
                (long long)os.mtime);
      fresh = false;
      break;
    }
  }
  free(deps);
  return fresh;
}

// Compile every source of `s` into `objdir` (reusing what is fresh) and append the object
// paths to `objs`. `key` is what freshness is decided on and `flags` is what the compiler
// is given: they differ by the framework library's stamp, which must invalidate objects
// without ever reaching clang's command line. `blame` receives the first file that failed.
static i32 compile_set(const char *cc, const char *flags, const char *key, const char *objdir,
                       const char *prefix, Sources *s, Buf *objs, char *blame, size_t blame_cap) {
  mkdirs(objdir);
  i32 done = 0;
  for (i32 i = 0; i < s->n; i++) {
    char slug[600], obj[1200];
    obj_slug(slug, sizeof slug, prefix, s->paths[i]);
    snprintf(obj, sizeof obj, "%s/%s.o", objdir, slug);
    if (!obj_fresh(obj, key)) {
      char side[1200], stem[1200];
      snprintf(stem, sizeof stem, "%s", obj);
      size_t l = strlen(stem);
      if (l > 2) stem[l - 2] = 0;
      Buf cmd;
      buf_init(&cmd, nullptr, 2048);
      buf_printf(&cmd, "%s -c %s %s -o %s -MD -MF %s.d", cc, flags, s->paths[i], obj, stem);
      bool ok = run_cmd(buf_cstr(&cmd), false);
      buf_free(&cmd);
      if (!ok) {
        // a failed compile must not leave a fresh-looking object or sidecar behind
        unlink(obj);
        unlink(side);
        if (blame && !blame[0]) snprintf(blame, blame_cap, "%s", s->paths[i]);
        return -1;
      }
      snprintf(side, sizeof side, "%s.flags", obj);
      FILE *f = fopen(side, "w");
      if (f) {
        fputs(key, f); // the cache key, not the command line: they differ by design
        fclose(f);
      }
      done++;
      if (trace_on()) fprintf(stderr, "  [cc] %s\n", s->paths[i]);
    }
    buf_printf(objs, " %s", obj);
  }
  return done;
}
static bool build_wasm(Config *c, bool release, const char *outpath);
static char *read_all(const char *path, i64 *n_out);

// Does this project ship a browser half at all?
static bool has_client_half(Config *c) { return c->client.n > 0 || (c->web[0] && path_exists(c->web)); }

// The compiler flags every object of the native half is built with. It is also the value
// the object cache compares: anything that would change the output (and nothing that
// would not) belongs in this string, and the framework library's own timestamp is part of
// it because its headers are macros as much as they are declarations.
static void native_flags(Config *c, bool release, char *out, size_t cap, char *key, size_t key_cap,
                         const char *inc, char *link_flags, size_t link_cap) {
  static Stamp lib; // the framework library's stamp, read once per process
  if (!lib.mtime) lib = stamp_of_fw_lib();
  snprintf(out, cap,
           "-std=c23 -g %s -Werror=return-type -Wno-unused-parameter "
           "-Wno-missing-field-initializers -I %s -I %s -D_DARWIN_C_SOURCE=1 -D_GNU_SOURCE=1",
           release ? "-O2" : "-O0", inc, c->src);
  if (c->cflags[0]) {
    size_t l = strlen(out);
    snprintf(out + l, cap - l, " %s", c->cflags);
  }
  // the cache key: the same flags plus what a compiler cannot see
  snprintf(key, key_cap, "%s fw=%lld.%lld", out, (long long)lib.mtime, (long long)lib.size);
  if (link_flags) snprintf(link_flags, link_cap, "%s", c->ldflags[0] ? c->ldflags : "");
}

// The native half: objects (cached) then one link. `build()` keeps its old shape for
// every caller; this is the part that knows about files.
static bool build_native_once(Config *c, bool release, const char *outpath) {
  Sources s;
  collect_sources(c, &s);
  char lib[1024], inc[1024];
  snprintf(lib, sizeof lib, "%s/build/libwebc.a", webc_prefix());
  snprintf(inc, sizeof inc, "%s/include", webc_prefix());
  char out_dir[1024];
  snprintf(out_dir, sizeof out_dir, "%s", outpath);
  char *slash = strrchr(out_dir, '/');
  if (slash) {
    *slash = 0;
    mkdirs(out_dir);
    *slash = '/';
  }
  char flags[4096], key[4096], ldflags[1024];
  native_flags(c, release, flags, sizeof flags, key, sizeof key, inc, ldflags, sizeof ldflags);

  i64 t0 = wc_time_ms();
  Buf objs;
  buf_init(&objs, nullptr, 4096);
  char blame[1024] = {0};
  bool ok = true;
  if (cache_off()) {
    // the one-shot command, unchanged: the escape hatch has to be the code path that was
    // already well tested, not a second implementation of it
    Buf cmd;
    buf_init(&cmd, nullptr, 4096);
    buf_printf(&cmd, "%s %s", g_cc, flags);
    for (i32 i = 0; i < s.n; i++) buf_printf(&cmd, " %s", s.paths[i]);
    buf_printf(&cmd, " %s -o %s -lpthread -lz -lsqlite3", lib, outpath);
    if (ldflags[0]) buf_printf(&cmd, " %s", ldflags);
    ok = run_cmd(buf_cstr(&cmd), false);
    buf_free(&cmd);
  } else {
    char objdir[1024];
    snprintf(objdir, sizeof objdir, "%s/.webc/obj", c->root);
    i32 recompiled = compile_set(g_cc, flags, key, objdir, "", &s, &objs, blame, sizeof blame);
    ok = recompiled >= 0;
    if (recompiled > 0)
      info("compiling %d of %d source%s -> %s", recompiled, s.n, s.n == 1 ? "" : "s", outpath);
    if (ok) {
      Buf cmd;
      buf_init(&cmd, nullptr, 4096);
      buf_printf(&cmd, "%s -o %s %s %s -lpthread -lz -lsqlite3", g_cc, outpath, buf_cstr(&objs), lib);
      if (ldflags[0]) buf_printf(&cmd, " %s", ldflags);
      // the link line must not start with a stray space inside the shell word
      char *c0 = buf_cstr(&cmd);
      while (*c0 == ' ') c0++;
      ok = run_cmd(c0, false);
      buf_free(&cmd);
    }
  }
  buf_free(&objs);
  i64 took = wc_time_ms() - t0;
  if (ok && trace_on())
    info("built %s (%d source%s, %.2f s)", outpath, s.n, s.n == 1 ? "" : "s", (double)took / 1000.0);
  else if (ok)
    info("built %s (%.1f s)", outpath, (double)took / 1000.0);
  return ok;
}

// A cache that is stale in a way mtimes cannot see — a restored checkout, a header touched
// back in time, a killed build's half-written object — presents as a compile error in a
// file that compiles, and the cure is `rm -rf .webc`. A build that fails is allowed to
// clear its own cache and try once more, which is the same cure, taken automatically and
// said out loud.
//
// The first attempt is captured rather than printed: if the retry succeeds, the errors
// were about a cache nobody can see, and showing them teaches the reader to distrust the
// second run. If it fails twice the errors are real, and the second attempt's output is
// printed exactly once — one mistake, one message.
static bool build_native(Config *c, bool release, const char *outpath) {
  // Before the first compile, and before the cache-clearing retry: a compiler that rejects
  // `-std=c23` is not a stale object somebody can clear away.
  if (!cc_does_c23()) {
    say_c23_missing();
    return false;
  }
  if (cache_off()) return build_native_once(c, release, outpath);
  WCap cap;
  wcap_begin(&cap);
  bool ok = build_native_once(c, release, outpath);
  char *first = wcap_end(&cap);
  if (ok) {
    if (first) fputs(first, stdout);
    free(first);
    return true;
  }
  char objdir[1024];
  snprintf(objdir, sizeof objdir, "%s/.webc/obj", c->root);
  info("build failed — clearing %s and building once more, in case the cache was stale", objdir);
  rm_rf(objdir);
  wcap_begin(&cap);
  ok = build_native_once(c, release, outpath);
  char *second = wcap_end(&cap);
  if (ok) {
    // the first failure was the cache's fault and nothing the reader needs to see
    info("a clean rebuild compiles — the object cache was stale, not the code");
  } else if (second) {
    // One mistake, one message: the raw compiler output goes through the translator
    // first, and only reaches the terminal unannotated when there is nothing better
    // to say (WEBC_NO_EXPLAIN keeps a nested `webc explain` from saying it twice).
    bool spoken = false;
    if (!getenv("WEBC_NO_EXPLAIN")) {
      spoken = explain_compiler_output(c->root, second);
      if (spoken) explain_project_rest(c->root, second);
    }
    if (!spoken) fputs(second, stderr);
  }
  free(first);
  free(second);
  return ok;
}

static bool build(Config *c, const char *target, bool release, const char *out) {
  pick_cc();
  Sources s;
  collect_sources(c, &s);
  char lib[1024], inc[1024];
  snprintf(lib, sizeof lib, "%s/build/libwebc.a", webc_prefix());
  snprintf(inc, sizeof inc, "%s/include", webc_prefix());
  if (!path_exists(lib)) {
    fprintf(stderr,
            "webc: framework library missing at %s\n"
            "      build it once with: make -C %s lib\n",
            lib, webc_prefix());
    return false;
  }
  char outpath[1024];
  snprintf(outpath, sizeof outpath, "%s", out);
  mkdirs(c->root);
  char *slash = strrchr(outpath, '/');
  if (slash) {
    *slash = 0;
    mkdirs(outpath);
    *slash = '/';
  }
  if (target && strcmp(target, "wasm") == 0) {
    // One name for the module, whichever door built it: `--wasm`, a `webc build` of an app
    // with a browser half, and `webc dev` all write .webc/app.wasm — the file the asset
    // middleware serves at /app.wasm and the one a deployment copies. Naming it after the
    // app here left two modules on disk, one of them stale.
    char wout[1024];
    snprintf(wout, sizeof wout, "%s/" WC_WASM_STEM, c->root);
    return build_wasm(c, release, wout);
  }
  // A browser-only project has no server half to link: `webc build` produces the
  // module and says so, instead of failing on a missing main(). With file sets the
  // question is whether a `server` set exists, not whether `entry` does.
  bool server_half = c->has_sets ? c->server.n > 0 : (c->entry[0] && path_exists(c->entry));
  if (!server_half) {
    if (!c->client.n && (!c->web[0] || !path_exists(c->web))) {
      fprintf(stderr,
              "webc: no entry point — webc.json names none of `server`, `client`, "
              "`entry` or `web`\n");
      return false;
    }
    char wout[1024];
    snprintf(wout, sizeof wout, "%s/" WC_WASM_STEM, c->root);
    return build_wasm(c, release, wout);
  }
  bool ok = build_native(c, release, outpath);
  if (ok && target == NULL && has_client_half(c)) {
    // an app that ships a browser half gets both halves from one command
    char wasm[1024];
    snprintf(wasm, sizeof wasm, "%s/" WC_WASM_STEM, c->root);
    mkdirs(".webc");
    build_wasm(c, release, wasm);
  }
  return ok;
}

// ---------------------------------------------------------------- wasm

// The wasm toolchain is a separate download: wasi-sdk carries clang, wasm-ld and
// a sysroot in one tarball. Look where people put it, then on PATH.
static bool find_wasi(char *out, size_t cap) {
  const char *env = getenv("WASI_SDK_PATH");
  if (env && *env) {
    snprintf(out, cap, "%s", env);
    return true;
  }
  const char *home_env = getenv("HOME");
  const char *home = home_env ? home_env : ".";
  struct {
    const char *base, *rel;
  } candidates[] = {
      {home, "/.local/share/wasi-sdk"},
      {"/opt/homebrew/opt/wasi-sdk", ""},
      {"/usr/local/opt/wasi-sdk", ""},
      {"/opt/wasi-sdk", ""},
  };
  for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
    char probe[1024];
    snprintf(probe, sizeof probe, "%s%s/bin/clang", candidates[i].base, candidates[i].rel);
    if (path_exists(probe)) {
      snprintf(out, cap, "%s%s", candidates[i].base, candidates[i].rel);
      return true;
    }
  }
  // a versioned tarball unpacked by hand: ~/.local/share/wasi-sdk-34.0-arm64-macos
  char cmd[512];
  snprintf(cmd, sizeof cmd, "ls -d %s/.local/share/wasi-sdk-*/ 2>/dev/null | tail -1", home);
  FILE *f = popen(cmd, "r");
  if (f) {
    if (fgets(out, (int)cap, f)) {
      size_t n = strlen(out);
      while (n && (out[n - 1] == '\n' || out[n - 1] == '/'))
        out[--n] = 0;
      pclose(f);
      if (n && path_exists(out)) return true;
    } else {
      pclose(f);
    }
  }
  // last resort: a `clang` that already targets wasm
  if (system("command -v wasm-ld >/dev/null 2>&1") == 0) {
    snprintf(out, cap, "%s", "");
    return true;
  }
  return false;
}

static const char *WASM_EXPORTS =
    "-Wl,--export=webc_start -Wl,--export=webc_dispatch -Wl,--export=webc_take_cmds "
    "-Wl,--export=webc_cmds_len -Wl,--export=webc_alloc -Wl,--export=webc_free -Wl,--export=webc_probe -Wl,--export=webc_arena "
    "-Wl,--export=webc_action -Wl,--export=webc_js_settle "
    "-Wl,--export-memory";

// The browser half of the framework: no tasks, no server, no SQL.
// The public API behind a server-only symbol, and what to do about it. The wasm half
// links core, react, dom, json, model, fe, js, prose and rows; everything else in this
// framework needs a server to mean anything.
typedef struct LinkHint {
  const char *prefix;
  const char *api;
  const char *fix;
} LinkHint;

static const LinkHint kLinkHints[] = {
    {"wc_prose", "prose() / prose_with()",
     "render that text in a `server` source or a page view and pass the markup in"},
    {"wc_sort_vec_", "sort_by() / sort_desc()", "sort in the server half, or sort the vec before you pass it"},
    {"wc_sort_rows_", "sort_by() / sort_desc()", "sort in the server half, or sort the vec before you pass it"},
    {"wc_filter", "filter_str() / filter_i64() / filter_bool()", "filter in the server half, or filter before you pass it"},
    {"wc_htmlcheck", "the HTML checker", "it runs in tests and on the server only"},
    {"db_", "the database", "a mounted component has no database: fetch in the server half (js_then) and pass it down"},
    {"wc_body", "body_try() and request bodies", "request bodies belong to the server half"},
    {"form_", "forms and validators", "the form helpers run in the server half"},
    {"auth_", "sessions and auth", "auth needs the server half"},
    {"res_", "Response builders", "responses belong to the server half"},
    {"mw_", "middleware", "middleware belongs to the server half"},
    {"router_", "the router", "the router belongs to the server half"},
    {"wc_route", "routes", "routes belong to the server half"},
    {"webc_fail", "fail()", "a mounted component has no request to fail"},
    {"wc_tls", "TLS", "the browser does its own TLS"},
    {"wtask", "tasks and green threads", "the wasm target is single-threaded (WEBC_NO_TASKS)"},
    {nullptr, nullptr, nullptr},
};

static void obj_slug(char *out, size_t cap, const char *prefix, const char *path);

static const char *src_for_object(const char *obj, const Sources *app, char *buf, size_t n) {
  // .../T/app_src_views_admin_posts_c.o → the app source whose *path* spells that slug.
  // The path, not the basename: an app that keeps its halves in folders has a posts.c in
  // each of them, and matching on the basename would blame a file it never compiled.
  const char *base = strrchr(obj, '/');
  base = base ? base + 1 : obj;
  size_t stem = 0;
  while (base[stem] && base[stem] != '.') stem++;
  static const char *prefixes[] = {"app_", "fw_", ""};
  for (i32 i = 0; app && i < app->n; i++) {
    char slug[600];
    for (size_t k = 0; k < WC_COUNTOF(prefixes); k++) {
      obj_slug(slug, sizeof slug, prefixes[k], app->paths[i]);
      if (strlen(slug) == stem && memcmp(base, slug, stem) == 0) {
        snprintf(buf, n, "%s", app->paths[i]);
        return buf;
      }
    }
  }
  // An object the slug rule cannot account for (clang's own `posts-545cd8.o` in the temp
  // directory, from the one-shot link) falls back to the basename — and says *all* the
  // files that answer to it, because blaming the wrong one is worse than naming two.
  stem = 0;
  while (base[stem] && base[stem] != '-' && base[stem] != '.') stem++;
  size_t at = 0;
  i32 matches = 0;
  for (i32 i = 0; app && i < app->n; i++) {
    const char *p = app->paths[i];
    const char *b = strrchr(p, '/');
    b = b ? b + 1 : p;
    if (strncmp(b, base, stem) != 0 || (b[stem] != '.' && b[stem] != 0)) continue;
    matches++;
    at += (size_t)snprintf(buf + at, n - at, "%s%s", matches == 1 ? "" : " or ", p);
    if (at >= n - 8) break;
  }
  return matches ? buf : nullptr;
}

// Turn the linker's leftovers into sentences. Returns the number of symbols explained.
static int explain_link(Buf *log, const Sources *app) {
  if (!log || !log->n) return 0;
  str text = buf_str(log);
  int explained = 0;
  size_t i = 0;
  while (i < text.n) {
    size_t eol = i;
    while (eol < text.n && text.p[eol] != '\n') eol++;
    str line = {text.p + i, eol - i};
    i = eol + 1;
    // wasm-ld says: <obj>: undefined symbol: <sym>
    i64 at = str_index(line, ':');
    const char marker[] = "undefined symbol: ";
    i64 m = -1;
    for (size_t k = 0; k + sizeof marker - 1 <= line.n; k++)
      if (memcmp(line.p + k, marker, sizeof marker - 1) == 0) { m = (i64)k; break; }
    if (m < 0) continue;
    (void)at;
    str sym = str_sub(line, (size_t)(m + (i64)sizeof marker - 1), line.n);
    sym = str_trim(sym);
    // `wasm-ld: error: /tmp/….o: undefined symbol: x` — the object names the app file
    // the call came from, and the object's stem is the source's basename.
    char obj[1024] = {0}, file[1024] = {0};
    for (size_t k = (size_t)m; k > 3; k--)
      if (line.p[k] == 'o' && line.p[k - 1] == '.' && line.p[k - 2] == ' ' +
          line.p[k - 2] - ' ') {
        size_t from = k;
        while (from > 0 && line.p[from - 1] != ' ' && line.p[from - 1] != ':') from--;
        size_t n = k + 1 - from < sizeof obj - 1 ? k + 1 - from : sizeof obj - 1;
        memcpy(obj, line.p + from, n);
        break;
      }
    const char *where = src_for_object(obj, app, file, sizeof file);
    char symc[256] = {0};
    snprintf(symc, sizeof symc, "%.*s", (int)(sym.n < sizeof symc - 1 ? sym.n : sizeof symc - 1), sym.p);
    const LinkHint *h = nullptr;
    for (const LinkHint *q = kLinkHints; q->prefix; q++)
      if (strncmp(symc, q->prefix, strlen(q->prefix)) == 0) { h = q; break; }
    if (h)
      fprintf(stderr, "\nwebc: %s calls %s, which is not part of the browser half.\n"
                      "      fix: %s\n",
              where ? where : "a shared source", h->api, h->fix);
    else
      fprintf(stderr, "\nwebc: %s needs `%s`, which the browser half does not link.\n"
                      "      fix: move that call into a `server` source, or stop calling it "
                      "from the client half.\n",
              where ? where : "a shared source", symc);
    explained++;
  }
  return explained;
}

static const char *FRONTEND_SRC =
    // prose.c and rows.c are pure text/memory work with no server in them, and a
    // shared view that calls prose() or sort_by() links here: excluding them meant
    // `wasm-ld: error: undefined symbol: wc_prose` for code the same headers advertise.
    // host.c is the other half of that rule: the host verbs are declared in a header the
    // browser half includes, and `webc_get` undefined was the same error, one layer up.
    // timekit.c and i18n.c are here for the same reason: `fmt_date()` and `t()` are documented
    // for views, a view compiles into both halves, and a client half without them fails the link
    // with `undefined symbol: wc_fmt_date`.
    // timekit.c's one task dependency (time_sleep_ms) is dropped by --gc-sections when no view
    // calls it, which is why the wasm side of the framework has no scheduler.
    "core.c react.c dom.c json.c model.c fe.c js.c host.c wasm_glue.c prose.c rows.c urlkit.c refs.c"
    // live.c for the same reason: the module's client half is `#if defined(__wasm__)` in the
    // same file as the server's, so a `live_connect` in an app's client file is `undefined
    // symbol: wc_live_connect`. Its server half costs the browser nothing — --gc-sections
    // drops it when no view calls it.
    " timekit.c i18n.c live.c crypto.c";
    // crypto.c for the same reason: `model_json_write`'s WK_BYTES case calls
    // `base64_encode`, so an app whose client half encodes a model (a save that posts
    // JSON) fails to link with `undefined symbol: base64_encode`. The hashing and
    // entropy halves are dropped by --gc-sections when no view calls them.

static bool build_wasm(Config *c, bool release, const char *outpath) {
  char wasi[1024];
  if (!find_wasi(wasi, sizeof wasi)) {
    // The tarball is per host: telling a Linux user to fetch the arm64-macos one was
    // a macOS-only hint in a cross-platform message.
#if defined(__APPLE__) && defined(__aarch64__)
    const char *wasi_tar = "wasi-sdk-34.0-arm64-macos.tar.gz";
#elif defined(__APPLE__)
    const char *wasi_tar = "wasi-sdk-34.0-x86_64-macos.tar.gz";
#elif defined(__aarch64__)
    const char *wasi_tar = "wasi-sdk-34.0-arm64-linux.tar.gz";
#else
    const char *wasi_tar = "wasi-sdk-34.0-x86_64-linux.tar.gz";
#endif
    fprintf(stderr,
            "webc: the wasm target needs wasi-sdk (clang + wasm-ld + a sysroot).\n"
            "      curl -L -o /tmp/wasi-sdk.tar.gz \\\n"
            "        https://github.com/WebAssembly/wasi-sdk/releases/download/"
            "wasi-sdk-34/%s\n"
            "      tar xzf /tmp/wasi-sdk.tar.gz -C ~ --strip-components=1 --one-top-level=wasi-sdk\n"
            "      # then: export PATH=$HOME/wasi-sdk/bin:$PATH, or unpack into ~/.local/share\n",
            wasi_tar);
    return false;
  }
  char clang[1200], sysroot[1200];
  if (wasi[0]) {
    snprintf(clang, sizeof clang, "%s/bin/clang", wasi);
    snprintf(sysroot, sizeof sysroot, "%s/share/wasi-sysroot", wasi);
  } else {
    snprintf(clang, sizeof clang, "clang");
    snprintf(sysroot, sizeof sysroot, "");
  }
  char inc[1024];
  snprintf(inc, sizeof inc, "%s/include", webc_prefix());
  // One flag string for every wasm object. `-mexec-model=reactor` is passed at link time
  // too: it selects the crt that exports _initialize, and a module without it can never run
  // the app's constructors — which is where mount() happens.
  Buf base;
  buf_init(&base, nullptr, 4096);
  // -mexec-model is a link-time option (it picks the crt), not a compile-time one: clang
  // rejects it inside a -c invocation, so it belongs on the link command and nowhere else.
  buf_printf(&base, "--target=wasm32-wasip1 -std=c23 %s -ffunction-sections -fdata-sections "
                    "-DWEBC_NO_TASKS=1 -I %s",
             release ? "-Oz" : "-O1", inc);
  if (sysroot[0]) buf_printf(&base, " --sysroot=%s", sysroot);
  if (getenv("WEBC_TRACE_EVENTS")) buf_puts(&base, " -DWEBC_TRACE_EVENTS=1");
  if (getenv("WEBC_TRACE_RENDER")) buf_puts(&base, " -DWEBC_TRACE_RENDER=1");
  if (c->cflags[0]) buf_printf(&base, " %s", c->cflags);

  // The app's client half: the declared `client` set plus `shared`, or, in a project that
  // declares no sets, the single `web` entry.
  Sources appset = {0};
  if (c->has_sets) collect_sources_for(c, &appset, TARGET_CLIENT);
  else if (c->web[0]) sources_add(&appset, c->web);

  Sources fw = {0};
  char list[512];
  snprintf(list, sizeof list, "%s", FRONTEND_SRC);
  for (char *tok = strtok(list, " "); tok; tok = strtok(nullptr, " ")) {
    char one[512];
    snprintf(one, sizeof one, "%s/src/%s", webc_prefix(), tok);
    sources_add(&fw, one);
  }

  char stem[1024];
  snprintf(stem, sizeof stem, "%s", outpath);
  size_t sl = strlen(stem);
  if (sl > 5 && strcmp(stem + sl - 5, ".wasm") == 0) stem[sl - 5] = 0;

  bool ok = false;
  if (!cache_off()) {
    // Framework objects never change between saves and the app's half is one or two files,
    // so per-object compilation turns a 1.7 s wasm build into 0.2 s. Same flags, same
    // linker, so the module is the one the one-shot command would have produced.
    char objdir[1024];
    snprintf(objdir, sizeof objdir, "%s/.webc/obj/wasm", c->root);
    Buf objs;
    buf_init(&objs, nullptr, 4096);
    char blame[1024] = {0};
    i32 nfw = compile_set(clang, buf_cstr(&base), buf_cstr(&base), objdir, "fw_", &fw, &objs, blame,
                          sizeof blame);
    i32 nap = nfw < 0 ? -1
                      : compile_set(clang, buf_cstr(&base), buf_cstr(&base), objdir, "app_", &appset,
                                    &objs, blame, sizeof blame);
    if (nfw >= 0 && nap >= 0) {
      Buf cmd;
      buf_init(&cmd, nullptr, 4096);
      buf_printf(&cmd,
                 "%s --target=wasm32-wasip1 -mexec-model=reactor %s-o %s.wasm %s "
                 "-Wl,--gc-sections -Wl,--strip-debug %s",
                 clang, sysroot[0] ? "" : "", stem, buf_cstr(&objs), WASM_EXPORTS);
      if (sysroot[0]) buf_printf(&cmd, " --sysroot=%s", sysroot);
      Buf log;
      buf_init(&log, nullptr, 4096);
      ok = run_cmd_capture(buf_cstr(&cmd), &log);
      if (!ok && explain_link(&log, &appset) == 0)
        info("the browser half did not link; run with WEBC_VERBOSE=1 for the command");
      buf_free(&log);
      buf_free(&cmd);
      if (ok && trace_on())
        info("wasm: %d framework + %d app object(s) compiled", nfw, nap);
    }
    buf_free(&objs);
  }
  if (!ok) {
    // The cache path failed. Fall back to the one-shot command: a green build in 1.7 s
    // beats a fast, confusing failure, and the diagnostics are the same. The object
    // directory goes with it — a stale object that survives makes the *next* build fail
    // the same way. Cleared here, the slow path also repairs the fast one.
    char objdir[1024];
    snprintf(objdir, sizeof objdir, "%s/.webc/obj/wasm", c->root);
    rm_rf(objdir);
    Buf cmd;
    buf_init(&cmd, nullptr, 4096);
    buf_printf(&cmd, "%s %s", clang, buf_cstr(&base));
    for (i32 i = 0; i < fw.n; i++) buf_printf(&cmd, " %s", fw.paths[i]);
    for (i32 i = 0; i < appset.n; i++) buf_printf(&cmd, " %s", appset.paths[i]);
    buf_printf(&cmd, " -o %s.wasm -Wl,--gc-sections -Wl,--strip-debug %s", stem, WASM_EXPORTS);
    Buf log;
    buf_init(&log, nullptr, 4096);
    ok = run_cmd_capture(buf_cstr(&cmd), &log);
    if (!ok && explain_link(&log, &appset) == 0)
      info("the browser half did not link; run with WEBC_VERBOSE=1 for the command");
    buf_free(&log);
    buf_free(&cmd);
  }
  buf_free(&base);
  if (ok) info("built %s.wasm", outpath);
  return ok;
}

// ---------------------------------------------------------------- new
//
// The three shapes a WebC app comes in, as source. Each one is a live app, not a sketch:
// no arena, no target test, no `(void)ctx`, no hand-built JSON. `make dx` greps these
// strings, so a scaffold can never drift back.

/* ---------------------------------------------- TEMPLATE_SERVER_MAIN */
static const char *TEMPLATE_SERVER_MAIN =
    // ---- src/main.c
    "// %s — a WebC server.\n"
    "//\n"
    "//   webc dev        build, run, reload on save\n"
    "//   webc test       the suite in tests/\n"
    "//   webc build      a single binary in .webc/\n"
    "//\n"
    "// The shape is the same in every WebC app: models, a view, routes, done. No main(),\n"
    "// no arena, no middleware list — the framework boots, answers /health and /metrics,\n"
    "// opens WEBC_DATABASE (or DATABASE_URL) and applies migrations/ for you.\n"
    "#include <webc/server.h>\n"
    "\n"
    "WEBC_MODEL(Todo, (id, i64, .pk = true),\n"
    "                 (title, Text, .required = true, .max = 200),\n"
    "                 (done, bool, .default_false = true))\n"
    "\n"
    "Vec(Todo)\n"
    "\n"
    "// ---------------------------------------------------------------- the view\n"
    "//\n"
    "// A view is markup and nothing else: no handler, no event, no selector. The form\n"
    "// posts to a route, which is what a browser without JavaScript needs anyway.\n"
    "\n"
    "view(Home, VecTodo *todos) {\n"
    "  el(\"main\") {\n"
    "    el(\"h1\") { text(\"Todos\"); }\n"
    "    el(\"form\") { post_to(\"/todos\");\n"
    "      el(\"input\") { name(\"title\"); placeholder(\"what needs doing?\"); required(); }\n"
    "      el(\"button\") { text(\"Add\"); }\n"
    "    }\n"
    "    el(\"ul\") {\n"
    "      for_each(t, *todos) {\n"
    "        el(\"li\") { class_(t->done ? \"done\" : \"\");\n"
    "          el(\"form\") { post_to(\"/todos/toggle\");\n"
    "            hidden_i(\"id\", t->id);\n"
    "            el(\"button\") { class_(\"tick\"); text(\"%%s\", t->done ? \"✓\" : \"○\"); }\n"
    "          }\n"
    "          el(\"span\") { text(\"%%s\", t->title); }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "}\n"
    "\n"
    "// ---------------------------------------------------------------- the routes\n"
    "\n"
    "GET(\"/\") { return page(Home, db_all(Todo)); }\n"
    "\n"
    "GET(\"/health\") { return res_text(200, \"ok\"); }\n"
    "\n"
    "POST(\"/todos\") {\n"
    "  Todo t = body(Todo);\n"
    "  db_save(Todo, &t);\n"
    "  return redirect(\"/\");\n"
    "}\n"
    "\n"
    "POST(\"/todos/toggle\") {\n"
    "  RowId in = body(RowId);\n"
    "  Todo *t = db_get(Todo, in.id);\n"
    "  if (!t) fail(404, \"no todo %%lld\", (long long)in.id);\n"
    "  t->done = !t->done;\n"
    "  db_save(Todo, t);\n"
    "  return redirect(\"/\");\n"
    "}\n";

/* ---------------------------------------------- TEMPLATE_SERVER_TEST */
static const char *TEMPLATE_SERVER_TEST =
    // ---- tests/app_test.c
    "// `webc test` builds and runs every tests/*_test.c with the app linked in.\n"
    "//\n"
    "// Handlers are reachable without a socket: webc_test_request() runs the router, the\n"
    "// middleware and the handler, and the database is an in-memory sqlite for the run.\n"
    "#include <webc/test.h>\n"
    "\n"
    "static void boot(void) {\n"
    "  setenv(\"DATABASE_URL\", \"sqlite::memory:\", 1);\n"
    "  webc_boot();\n"
    "  db_default();\n"
    "}\n"
    "TEST_SETUP(boot);\n"
    "\n"
    "TEST(the_page_renders) {\n"
    "  Response r = webc_test_request(\"GET\", \"/\");\n"
    "  ASSERT_EQ(r.status, 200);\n"
    "  ASSERT_HTML(r, \"h1\");\n"
    "  ASSERT_HTML_TEXT(r, \"h1\", \"Todos\");\n"
    "}\n"
    "\n"
    "TEST(a_form_post_creates_a_row) {\n"
    "  const char *form = \"title=write+the+model\";\n"
    "  Response r = webc_test_request_ex(\"POST\", \"/todos\",\n"
    "                                    \"Content-Type: application/x-www-form-urlencoded\", form,\n"
    "                                    strlen(form));\n"
    "  ASSERT_EQ(r.status, 303);\n"
    "\n"
    "  Response page = webc_test_request(\"GET\", \"/\");\n"
    "  ASSERT_HTML(page, \"ul > li\");\n"
    "  ASSERT_HTML_TEXT(page, \"ul > li > span\", \"write the model\");\n"
    "}\n"
    "\n"
    "TEST(an_empty_title_is_422) {\n"
    "  Response r = webc_test_request(\"POST\", \"/todos\");\n"
    "  ASSERT_EQ(r.status, 422);\n"
    "}\n"
    "\n"
    "TEST(a_missing_route_is_404) {\n"
    "  ASSERT_EQ(webc_test_request(\"GET\", \"/nope\").status, 404);\n"
    "}\n"
    "\n"
    "TEST_MAIN()\n";

/* ---------------------------------------------- TEMPLATE_WEB_MAIN */
static const char *TEMPLATE_WEB_MAIN =
    // ---- src/main.c
    "// %s — a WebC browser app.\n"
    "//\n"
    "//   webc dev        build, serve, reload on save\n"
    "//   webc build      produce .webc/app.wasm\n"
    "//\n"
    "// There is no server half: the module is the app. A view reads signals, an action\n"
    "// changes them, and `client_main` binds the two by name — the view never mentions an\n"
    "// event, and this file never mentions a selector.\n"
    "#include <webc/web.h>\n"
    "\n"
    "state(i64, count, 0);\n"
    "\n"
    "view(Counter) {\n"
    "  el(\"main\") {\n"
    "    el(\"h1\") { text(\"%s\"); }\n"
    "    // `text_live` binds this node to the signal: a write updates this text and\n"
    "    // nothing else, because no view body runs. `text(\"count: %%lld\", get(count))`\n"
    "    // is correct too, and re-runs the whole view after every click.\n"
    "    el(\"p\") { text_live(\"count: %%lld\", count); }\n"
    "    el(\"button\") { named(\"bump\"); text(\"+1\"); }\n"
    "  }\n"
    "}\n"
    "\n"
    "// `action_live` is the matching promise: this handler writes a signal and changes\n"
    "// nothing the view draws, so the bridge sends the binding's one op and skips the\n"
    "// render. A handler that adds or removes a node — or edits a vector the markup\n"
    "// reads — is a plain `action`: it always re-renders, and cannot go stale.\n"
    "// (reference §12j; watch it in the console: WebC.diagnostics().ops rises by one\n"
    "// per click, not by the size of the view)\n"
    "action_live(bump) { set(count, get(count) + 1); }\n"
    "\n"
    "client_main {\n"
    "  mount(Counter, \"#root\");\n"
    "  bind(\"bump\", \"click\", bump);\n"
    "  return 0;\n"
    "}\n";

/* ---------------------------------------------- TEMPLATE_WEB_TEST */
static const char *TEMPLATE_WEB_TEST =
    // ---- tests/app_test.c
    "// A browser app is testable in-process: render the component with the same builder\n"
    "// the wasm module runs, then drive the action directly.\n"
    "#include <webc/test.h>\n"
    "\n"
    "declare_view(Counter);\n"
    "ACTION(bump)\n"
    "\n"
    "static WComponent g_comp;\n"
    // not `render`: <webc/http.h> owns that word for a server response (`render(View)`,
    // `render(view, model)`), and a macro that greedy makes the scaffold's own test fail to
    // compile with an error that points at the framework, not at the name.
    "static str render_page(void) {\n"
    "  static bool inited;\n"
    "  if (!inited) {\n"
    "    wcomp_init(&g_comp, nullptr);\n"
    "    inited = true;\n"
    "  }\n"
    "  return wcomp_html_ctx(&g_comp, view_Counter, nullptr);\n"
    "}\n"
    "\n"
    "TEST(renders_the_component) {\n"
    "  str html = render_page();\n"
    "  ASSERT_HTML(html, \"main > button\");\n"
    "  ASSERT_HTML_TEXT(html, \"main > p\", \"count: 0\");\n"
    "  ASSERT_HTML_ATTR(html, \"main > button\", \"data-hook\", \"bump\");\n"
    "}\n"
    "\n"
    "TEST(clicking_increments) {\n"
    "  action_bump(nullptr, \"click\");\n"
    "  ASSERT_HTML_TEXT(render_page(), \"main > p\", \"count: 1\");\n"
    "}\n"
    "\n"
    "TEST_MAIN()\n";

/* ---------------------------------------------- TEMPLATE_FULL_APP_H */
static const char *TEMPLATE_FULL_APP_H =
    // ---- src/app.h
    "// The half both sides share: the model, the context, the view name, and the markup.\n"
    "// Nothing in here can reach a database or a request — that is what <webc/shared.h>\n"
    "// is for, and the compiler enforces it.\n"
    "#pragma once\n"
    "\n"
    "#include <webc/shared.h>\n"
    "#include <webc/form.h> // the form is drawn from the model it validates with\n"
    "\n"
    "WEBC_MODEL(Note, (id, i64, .pk = true),\n"
    "                 (body, Text, .required = true, .max = 500))\n"
    "\n"
    "Vec(Note)\n"
    "\n"
    "// The view's context: the server fills it per request, the client keeps one as its\n"
    "// whole application state — the same view renders from either.\n"
    "typedef struct NotesCtx {\n"
    "  VecNote *notes;\n"
    "  // The model, drawn: `form_fields()` writes the label, the input, its bounds and the\n"
    "  // sentence when a value is wrong — so a field added to WEBC_MODEL appears here, with\n"
    "  // its validation, without this file changing.\n"
    "  WForm *form;\n"
    "} NotesCtx;\n"
    "\n"
    "declare_view(Notes, NotesCtx);\n";

/* ---------------------------------------------- TEMPLATE_FULL_VIEW */
static const char *TEMPLATE_FULL_VIEW =
    // ---- src/view.c
    "// The component. Server-rendered for the first paint, hydrated in the browser, and\n"
    "// patched in place afterwards — one view, both targets, one context type.\n"
    "#include \"app.h\"\n"
    "\n"
    "view(Notes, NotesCtx *c) {\n"
    "  el(\"main\") {\n"
    "    el(\"h1\") { text(\"Notes\"); }\n"
    "\n"
    "    el(\"form\") { post_to(\"/api/notes\");\n"
    "      // one line instead of a label, an input, `required`, `maxlength` and the error\n"
    "      // paragraph: the model already said all of it\n"
    "      if (c->form) form_fields(c->form);\n"
    "      el(\"button\") { text(\"Add\"); }\n"
    "    }\n"
    "\n"
    "    el(\"ul\") {\n"
    "      if (c->notes) for_each(n, (*c->notes)) {\n"
    "        el(\"li\") {\n"
    "          el(\"span\") { text(n->body); }\n"
    "          // `named` + `bind` rather than on_click: this view is compiled for the\n"
    "          // server too, where the client's actions do not exist (dom.h, on_click)\n"
    "          el(\"button\") { class_(\"x\"); named(\"remove\"); text(\"×\"); }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "\n"
    "    el(\"p\") { class_(\"count\"); text(\"%lld notes\",\n"
    "                                (long long)(c->notes ? len(*c->notes) : 0)); }\n"
    "  }\n"
    "}\n";

/* ---------------------------------------------- TEMPLATE_FULL_SERVER */
static const char *TEMPLATE_FULL_SERVER =
    // ---- src/server.c
    "// The server half: the same view, rendered with a context, plus the routes that\n"
    "// change it. Every answer is a redirect or the model — no JSON is written by hand.\n"
    "#include <webc/server.h>\n"
    "#include \"app.h\"\n"
    "\n"
    "// the context the shared view renders from: filled by field name, one line\n"
    "GET(\"/\") {\n"
    "  Note draft = {0};\n"
    "  WForm f;\n"
    "  form_init(&f, Note_modelof(), &draft);\n"
    "  NotesCtx ctx = {.notes = db_all(Note), .form = &f};\n"
    "  return page(Notes, &ctx);\n"
    "}\n"
    "\n"
    "GET(\"/api/notes\") { return json(200, db_all(Note)); }\n"
    "\n"
    "POST(\"/api/notes\") {\n"
    "  Note n = {0};\n"
    "  WForm f;\n"
    "  form_init(&f, Note_modelof(), &n);\n"
    "  // `body_try` leaves the answer to us, which is the whole point for a form: a bad\n"
    "  // value gets the page back at 422 with the sentence under the field, not a JSON\n"
    "  // error nobody sees. `form_submit` runs the model's validators — the same ones\n"
    "  // the client half runs — and fills `f` with one message per field.\n"
    "  body_try(Note, &n);\n"
    "  if (!form_submit(&f)) {\n"
    "    NotesCtx back = {.notes = db_all(Note), .form = &f};\n"
    "    return page_with(422, Notes, &back);\n"
    "  }\n"
    "  db_save(Note, &n);\n"
    "  if (wc_body_is_form()) return redirect(\"/\");\n"
    "  return json(201, Note, &n);\n"
    "}\n"
    "\n"
    "POST(\"/api/notes/:id/delete\") {\n"
    "  Note *n = db_get(Note, path_i64(\"id\"));\n"
    "  if (!n) fail(404, \"no note %lld\", (long long)path_i64(\"id\"));\n"
    "  db_delete(Note, n->id);\n"
    "  if (wc_body_is_form()) return redirect(\"/\");\n"
    "  return res_empty(204);\n"
    "}\n"
    "\n"
    "GET(\"/health\") { return res_text(200, \"ok\"); }\n";

/* ---------------------------------------------- TEMPLATE_FULL_CLIENT */
static const char *TEMPLATE_FULL_CLIENT =
    // ---- src/client.c
    "// The client half: one context as the whole application state, the SAME shared\n"
    "// view the server rendered, mounted over the page's markup. A view of its own\n"
    "// here forfeits hydration — the module rebuilds what the server already painted,\n"
    "// and WebC.diagnostics() says so.\n"
    "#include <webc/web.h>\n"
    "#include <webc/host.h> // webc_post(): the action's other half, on the network\n"
    "#include \"app.h\"\n"
    "\n"
    "static VecNote g_notes = {.desc = &WC_VECDESC_VecNote};\n"
    "static Note g_draft;\n"
    "static WForm g_form;\n"
    "static NotesCtx C = {.notes = &g_notes, .form = &g_form};\n"
    "\n"
    "// The seed the boot block carries (data: \"/api/notes\"): same rows the server\n"
    "// rendered, decoded into the context before the first client render — which is\n"
    "// what makes hydration adopt the server's nodes instead of building them.\n"
    "void wc_app_data(str json) {\n"
    "  JParse p = json_parse(wc_arena_get(), json);\n"
    "  JVal *arr = p.ok && p.v && p.v->kind == J_ARR ? p.v : json_get(p.v, \"data\");\n"
    "  if (arr && arr->kind == J_ARR)\n"
    "    for (i32 i = 0; i < arr->n; i++) {\n"
    "      Note n;\n"
    "      if (model_read_json_val(wc_arena_get(), Note_modelof(), arr->items[i], &n))\n"
    "        push(g_notes, n);\n"
    "    }\n"
    "}\n"
    "\n"
    "action(remove, Note *n) {\n"
    "  for (i32 i = 0; i < len(g_notes); i++)\n"
    "    if (at(g_notes, i)->id == n->id) {\n"
    "      remove_at(g_notes, i);\n"
    "      break;\n"
    "    }\n"
    "  // No \"bump the view\" line: the bridge re-renders after every action returns.\n"
    "  webc_post(fmt_s(\"/api/notes/%lld/delete\", (long long)n->id),\n"
    "            str_lit(\"{}\"), nullptr);\n"
    "}\n"
    "\n"
    "client_main {\n"
    "  // the same form the server rendered, over the same model: a field added to\n"
    "  // WEBC_MODEL arrives here too, and its validation is the server's\n"
    "  form_init(&g_form, Note_modelof(), &g_draft);\n"
    "  mount(Notes, &C, \"#root\");\n"
    "  return 0;                 // the view wires its own buttons (on_click)\n"
    "}\n";

/* ---------------------------------------------- TEMPLATE_FULL_TEST */
static const char *TEMPLATE_FULL_TEST =
    // ---- tests/app_test.c
    "// Both halves are C, so both are testable in one binary: the page the server\n"
    "// renders, the API it answers, and the component in-process.\n"
    "#include <webc/test.h>\n"
    "#include \"app.h\"\n"
    "\n"
    "static void boot(void) {\n"
    "  setenv(\"DATABASE_URL\", \"sqlite::memory:\", 1);\n"
    "  webc_boot();\n"
    "  db_default();\n"
    "}\n"
    "TEST_SETUP(boot);\n"
    "\n"
    "TEST(the_page_is_server_rendered) {\n"
    "  Response r = webc_test_request(\"GET\", \"/\");\n"
    "  ASSERT_EQ(r.status, 200);\n"
    "  // the page read as a document: a selector, not a substring scan\n"
    "  ASSERT_HTML(r, \"main > h1\");\n"
    "  ASSERT_HTML_TEXT(r, \"main > h1\", \"Notes\");\n"
    "}\n"
    "\n"
    "TEST(the_api_validates) {\n"
    "  Response bad = webc_test_request_ex(\"POST\", \"/api/notes\",\n"
    "                                      \"Content-Type: application/json\", \"{}\", 2);\n"
    "  ASSERT_EQ(bad.status, 422);\n"
    "\n"
    "  const char *json = \"{\\\"body\\\":\\\"hi\\\"}\";\n"
    "  Response ok = webc_test_request_ex(\"POST\", \"/api/notes\",\n"
    "                                     \"Content-Type: application/json\", json, (i32)strlen(json));\n"
    "  ASSERT_EQ(ok.status, 201);\n"
    "  ASSERT_JSON(ok, \"body\", \"hi\");\n"
    "}\n"
    "\n"
    "TEST(the_view_is_the_same_on_both_sides) {\n"
    "  WComponent comp;\n"
    "  wcomp_init(&comp, nullptr);\n"
    "  Note draft = {0};\n"
    "  WForm f;\n"
    "  form_init(&f, Note_modelof(), &draft);\n"
    "  NotesCtx c = {.notes = db_all(Note), .form = &f};\n"
    "  str html = wcomp_html_ctx(&comp, view_Notes, &c);\n"
    "  ASSERT_HTML(html, \"main ul\");\n"
    "  // the field is the model's, not the view's: label, input and bounds in one call\n"
    "  ASSERT_HTML(html, \"main form input[name=body]\");\n"
    "  ASSERT_HTML_ATTR(html, \"main form label\", \"for\", \"f-body\");\n"
    "  // the primary key is a hidden input, never an editable one\n"
    "  ASSERT_HTML(html, \"main form input[type=hidden]\");\n"
    "}\n"
    "\n"
    "TEST(a_bad_value_is_the_page_again_with_the_sentence_on_it) {\n"
    "  const char *body = \"body=\";\n"
    "  Response r = webc_test_request_ex(\"POST\", \"/api/notes\",\n"
    "                                   \"Content-Type: application/x-www-form-urlencoded\", body,\n"
    "                                   (i32)strlen(body));\n"
    "  ASSERT_EQ(r.status, 422);                          // a page, not a JSON error\n"
    "  ASSERT_HTML_TEXT(r, \"main form .field-err\", \"required\");\n"
    "}\n"
    "\n"
    "TEST_MAIN()\n";

typedef enum Template { TPL_SERVER, TPL_WEB, TPL_FULLSTACK, TPL_BOARD } Template;

// `webc new --template=board` ships the example, not a second copy of it. A template written
// twice is a template that drifts from the app it claims to be — so the board template is
// examples/board, copied, with the project's own name written into webc.json.
static void copy_tree(const char *from, const char *to, const char *name) {
  mkdirs(to);
  DIR *d = opendir(from);
  if (!d) die("cannot read the board template at %s (set WEBC_HOME)", from);
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue; // .webc, .git, .DS_Store: build output and noise
    char src[1200], dst[1200];
    snprintf(src, sizeof src, "%s/%s", from, e->d_name);
    snprintf(dst, sizeof dst, "%s/%s", to, e->d_name);
    struct stat st;
    if (stat(src, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      copy_tree(src, dst, name);
      continue;
    }
    i64 n = 0;
    char *text = read_all(src, &n);
    if (!text) continue;
    // one substitution, and only where it is a lie otherwise: the project's name
    if (strcmp(e->d_name, "webc.json") == 0 && name && *name) {
      Buf out;
      buf_init(&out, nullptr, (size_t)n + 64);
      const char *p = text;
      while (*p) {
        const char *hit = strstr(p, "\"name\": \"board\"");
        if (!hit) {
          buf_puts(&out, p);
          break;
        }
        buf_write(&out, p, (size_t)(hit - p)); // everything before the name, verbatim
        buf_puts(&out, "\"name\": \"");
        buf_puts(&out, name);
        buf_puts(&out, "\"");
        p = hit + strlen("\"name\": \"board\"");
      }
      write_file(dst, buf_cstr(&out));
      buf_free(&out);
    } else {
      write_file(dst, text);
    }
    free(text);
  }
  closedir(d);
}

static int cmd_new_tpl(const char *dir, Template tpl) {
  char root[1024];
  snprintf(root, sizeof root, "%s", dir && *dir ? dir : ".");
  const char *name = strrchr(root, '/');
  name = name ? name + 1 : root;
  if (path_exists(root) && strcmp(root, ".") != 0) {
    DIR *d = opendir(root);
    struct dirent *e;
    while (d && (e = readdir(d)))
      if (e->d_name[0] != '.' && strcmp(e->d_name, "..") != 0)
        die("%s already exists and is not empty", root);
    if (d) closedir(d);
  }
  if (tpl == TPL_BOARD) {
    char from[1024];
    snprintf(from, sizeof from, "%s/examples/board", webc_prefix());
    copy_tree(from, root, name);
    printf("\033[36mwebc\033[0m created %s — the board from the standard-library proof\n"
           "      cd %s && webc dev      (a kanban in %d lines of app code)\n",
           root, root, 385);
    return 0;
  }
  mkdirs(root);
  char p[1200];
  snprintf(p, sizeof p, "%s/src", root);
  mkdirs(p);
  snprintf(p, sizeof p, "%s/public", root);
  mkdirs(p);
  if (tpl != TPL_WEB) {
    // a browser-only app has no server half, so no database and no migrations
    snprintf(p, sizeof p, "%s/data", root);
    mkdirs(p);
    snprintf(p, sizeof p, "%s/migrations", root);
    mkdirs(p);
    snprintf(p, sizeof p, "%s/public/assets", root);
    mkdirs(p);
  }

  Buf src;
  buf_init(&src, nullptr, 8192);

  // The files each shape starts with. A project's own history is what it grows from,
  // so the first commit is the app, not a skeleton to delete.
  if (tpl == TPL_WEB) {
    snprintf(p, sizeof p, "%s/src/main.c", root);
    buf_printf(&src, TEMPLATE_WEB_MAIN, name, name); // the header line and the <h1>
    write_file(p, buf_cstr(&src));
    buf_reset(&src);
    snprintf(p, sizeof p, "%s/tests/app_test.c", root);
    buf_printf(&src, TEMPLATE_WEB_TEST, name);
    write_file(p, buf_cstr(&src));
    buf_reset(&src);
    snprintf(p, sizeof p, "%s/webc.json", root);
    buf_printf(&src,
               "{\n  \"name\": \"%s\",\n  \"version\": \"" APP_VERSION_DEFAULT "\",\n  \"mode\": \"web\",\n"
               "  \"target\": \"wasm\",\n  \"client\": [\"src/main.c\"],\n"
               "  \"dev\": { \"port\": 9999 }\n}\n",
               name);
    write_file(p, buf_cstr(&src));
    buf_reset(&src);
  } else if (tpl == TPL_FULLSTACK) {
    snprintf(p, sizeof p, "%s/src/app.h", root);
    write_file(p, TEMPLATE_FULL_APP_H);
    snprintf(p, sizeof p, "%s/src/view.c", root);
    write_file(p, TEMPLATE_FULL_VIEW);
    snprintf(p, sizeof p, "%s/src/server.c", root);
    write_file(p, TEMPLATE_FULL_SERVER);
    snprintf(p, sizeof p, "%s/src/client.c", root);
    write_file(p, TEMPLATE_FULL_CLIENT);
    snprintf(p, sizeof p, "%s/tests/app_test.c", root);
    buf_printf(&src, TEMPLATE_FULL_TEST, name);
    write_file(p, buf_cstr(&src));
    buf_reset(&src);
    snprintf(p, sizeof p, "%s/webc.json", root);
    // `boot` is the document: title, stylesheet, mount point and the WebC.start()
    // options. A fullstack project ships no HTML file at all — the shell is built
    // from this block, so `hydrate` and the seed-data URL cannot drift from the
    // routes the server actually serves.
    buf_printf(&src,
               "{\n  \"name\": \"%s\",\n  \"version\": \"" APP_VERSION_DEFAULT "\",\n"
               "  \"database\": \"sqlite:data/dev.db\",\n  \"port\": 8080,\n"
               "  \"dev\": { \"port\": 9999 },\n"
               "  \"server\": [\"src/server.c\"],\n"
               "  \"shared\": [\"src/view.c\"],\n"
               "  \"client\": [\"src/client.c\"],\n"
               "  \"boot\": {\n"
               "    \"title\": \"%s\",\n"
               "    \"lang\": \"en\",\n"
               "    \"css\": [\"/public/style.css\"],\n"
               "    \"el\": \"#root\",\n"
               "    \"data\": \"/api/notes\",\n"
               "    \"hydrate\": true\n"
               "  }\n}\n",
               name, name);
    write_file(p, buf_cstr(&src));
    buf_reset(&src);
  } else {
    snprintf(p, sizeof p, "%s/src/main.c", root);
    buf_printf(&src, TEMPLATE_SERVER_MAIN, name);
    write_file(p, buf_cstr(&src));
    buf_reset(&src);
    snprintf(p, sizeof p, "%s/tests/app_test.c", root);
    buf_printf(&src, TEMPLATE_SERVER_TEST, name);
    write_file(p, buf_cstr(&src));
    buf_reset(&src);
    snprintf(p, sizeof p, "%s/webc.json", root);
    buf_printf(&src,
               "{\n  \"name\": \"%s\",\n  \"version\": \"" APP_VERSION_DEFAULT "\",\n"
               "  \"database\": \"sqlite:data/dev.db\",\n  \"port\": 8080,\n"
               "  \"dev\": { \"port\": 9999 },\n"
               "  \"server\": [\"src/main.c\"]\n}\n",
               name);
    write_file(p, buf_cstr(&src));
    buf_reset(&src);
  }
  buf_free(&src);

  // Both shapes with a server half get a migration and a shell page; a browser-only
  // app gets the shell its module mounts into and nothing else.
  if (tpl != TPL_WEB) {
    snprintf(p, sizeof p, "%s/migrations/0001_init.sql", root);
    write_file(p, "-- the tables the models in src/ declare\n"
                  "create table if not exists todos (\n"
                  "  id integer primary key autoincrement,\n"
                  "  title text not null,\n"
                  "  done integer not null default 0\n"
                  ");\n"
                  "create table if not exists notes (\n"
                  "  id integer primary key autoincrement,\n"
                  "  body text not null\n"
                  ");\n");
    // A dialect twin per driver that spells the identity column differently. The migration
    // runner picks the file matching the URL's driver and falls back to the plain one, so an
    // app scaffolded today runs on all three without editing anything.
    snprintf(p, sizeof p, "%s/migrations/0001_init.postgres.sql", root);
    write_file(p, "-- the postgres spelling of 0001_init.sql\n"
                  "create table if not exists todos (\n"
                  "  id bigserial primary key,\n"
                  "  title text not null,\n"
                  "  done boolean not null default false\n"
                  ");\n"
                  "create table if not exists notes (\n"
                  "  id bigserial primary key,\n"
                  "  body text not null\n"
                  ");\n");
    snprintf(p, sizeof p, "%s/migrations/0001_init.mysql.sql", root);
    write_file(p, "-- the mysql/mariadb spelling of 0001_init.sql\n"
                  "create table if not exists todos (\n"
                  "  id bigint auto_increment primary key,\n"
                  "  title varchar(255) not null,\n"
                  "  done tinyint(1) not null default 0\n"
                  ");\n"
                  "create table if not exists notes (\n"
                  "  id bigint auto_increment primary key,\n"
                  "  body text not null\n"
                  ");\n");
    // A fullstack project's document is generated from the `boot` block above: no
    // public/index.html, and no second copy of the title and stylesheet to keep in
    // step with it. A project that wants its own document can still ship one and it
    // wins — res_page() splices into it exactly as before.
  } else {
    snprintf(p, sizeof p, "%s/public/index.html", root);
    write_file(p, "<!doctype html>\n<html lang=\"en\">\n<head>\n"
                  "  <meta charset=\"utf-8\">\n"
                  "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                  "  <title>WebC</title>\n"
                  "  <link rel=\"stylesheet\" href=\"/style.css\">\n"
                  "</head>\n<body>\n  <div id=\"root\"></div>\n"
                  "  <script src=\"/webc.js\"></script>\n"
                  "  <script>\n"
                  "    WebC.start({ el: '#root', wasm: '/app.wasm' });\n"
                  "  </script>\n</body>\n</html>\n");
  }

  char tdir[1024];
  snprintf(tdir, sizeof tdir, "%s/tests", root);
  mkdirs(tdir);

  snprintf(p, sizeof p, "%s/public/style.css", root);
  write_file(
      p, "body { font: 16px/1.5 system-ui, sans-serif; margin: 2rem auto; max-width: 40rem; }\n");

  // `.webc/` is this app's build output; `compile_commands.json` is the language
  // server's, for clangd to read so it agrees with `webc build` instead of guessing
  snprintf(p, sizeof p, "%s/.gitignore", root);
  write_file(p, ".webc/\ndata/*.db\ndata/*.db-*\ncompile_commands.json\n");

  snprintf(p, sizeof p, "%s/README.md", root);
  Buf rd;
  buf_init(&rd, nullptr, 512);
  buf_printf(&rd,
             "# %s\n\nA WebC app.\n\n```sh\nwebc dev      # build + run + reload\n"
             "webc build    # produce .webc/app.wasm\nwebc test     # run the tests\n```\n"
             "\nA browser half that calls a public API should name its origin in webc.json,\n"
             "so `webc dev` allows it through the Content-Security-Policy:\n\n"
             "```json\n\"dev\": { \"port\": 9999, \"connect\": [\"https://api.example.com\"] }\n```\n",
             name);
  write_file(p, buf_cstr(&rd));
  // Every coding agent looks for this file before it writes anything, in every project,
  // whether or not its author installed a skill. Eight lines here are worth more than a
  // page of documentation nobody opens: the four mistakes below are the ones an agent
  // makes by default, and each of them costs a build to discover.
  snprintf(p, sizeof p, "%s/AGENTS.md", root);
  write_file(p,
             "# Working in a WebC app\n\n"
             "C23, one model declaration for the server and the browser. Verify with\n"
             "`webc check` (build + tests + schema + lint); it exits non-zero. Run it after\n"
             "every change — a failed build comes back in WebC's words, not the compiler's.\n\n"
             "- A `shared` source (see webc.json) reaches neither the database nor the\n"
             "  request: `db_*`, `req`, `path*`, `query`, `body` are server-half names. Put\n"
             "  them in a `server` source and pass the values in through the view's context.\n"
             "- One `WEBC_MODEL(...)` gives the struct, the validation, the JSON and the SQL.\n"
             "  Never write a JSON body, a form parser or a SQL statement by hand.\n"
             "- No arena, no allocator in app code: `fmt()` builds into the current scope,\n"
             "  `persist()` is the one escape hatch for a value that outlives it.\n"
             "- Add a column as a file in `migrations/`; the server applies it at boot.\n"
             "- Authored HTML that is not interpolated is a file under `partials/`,\n"
             "  included by `partial(\"name\")` — head chrome is a file edit, not a C\n"
             "  string of escaped quotes. `webc check` verifies every name has its file.\n"
             "- Structured data is `head_ld(ld_blog_posting(...))` from `<webc/ld.h>`;\n"
             "  `webc lint` refuses the hand-built version, and a field you leave out is\n"
             "  omitted, never written empty.\n"
             "- `webc lint` names each retired spelling and what to write instead. Believe\n"
             "  it: the framework refuses the C you are about to write on purpose.\n"
             "- Code you own, generated from the app's own models: `webc scaffold Post`\n"
             "  writes the admin CRUD, `webc scaffold live Post` the SSE room and its\n"
             "  broadcasting mutations, with tests. Edit what they wrote; they never\n"
             "  rewrite the model.\n"
             "- The API boundary is generated too: `webc openapi` writes OpenAPI 3.1 and\n"
             "  `webc client --ts` a typed fetch client, from the routes and models. If\n"
             "  the app ships `openapi.json` or `webc.client.ts`, keep them current —\n"
             "  `--check` (and `webc ci`) fails when they drift.\n"
             "- Deploying: `webc deploy --pack docker,fly,systemd,caddy,nginx` writes the\n"
             "  ops files (SSE-aware) beside the binary; `webc ci` writes the workflow\n"
             "  that runs this file's gate.\n"
             "- The framework's documentation is here: `.webc/docs/SKILL.md` routes you to one\n"
             "  file per topic (models, http, views, auth, concurrency, stdlib, traps…). Read\n"
             "  that before guessing, and never search the framework sources for an answer —\n"
             "  `webc docs` rewrites this directory if it is missing.\n"
             "- The full surface is `webc api <name>`; `webc explain` translates a build failure.\n");
  // The documentation goes into the app, because an agent working here should not have to
  // find the framework's sources to answer "what does this name do". It is under `.webc/`,
  // which is gitignored: the toolchain owns it, `webc docs` rewrites it, and a clone gets it
  // with one command the AGENTS.md names.
  char docdir[1200];
  snprintf(docdir, sizeof docdir, "%s/.webc/docs", root);
  docs_emit(docdir);
  info("created %s", root);
  return 0;
}

// ---------------------------------------------------------------- test

// `webc test [--filter=suite] [--test=name] [--list] [--macros]`: a suite is a tests/*_test.c
// file. `--filter` narrows *which suites are built* and then runs them whole; `--test` narrows
// by test name through WEBC_TEST_FILTER, which the runner applies. `--list` prints the suites
// without building them.
typedef struct TestArgs {
  bool json; // `--json`: one object, no prose
  const char *filter; // `--filter=`: which *suite* (a tests/*_test.c file); the whole file runs
  const char *only;   // `--test=`: which test *name*, in every suite (slow: everything builds)
  bool list;
  bool macros;
  bool client;
  bool watch; // `--watch`: run, then run again whenever src/ or tests/ changes
} TestArgs;

// The newest modification time under one directory, recursively, and how many files there
// are — the count is what notices a file that was *added*, since an addition does not move
// the maximum. Two numbers, one comparison, no inotify (it is Linux-only, and a test suite
// is not a build system).
static void watch_mark_dir(const char *dir, u64 *newest, i64 *count) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      watch_mark_dir(path, newest, count);
      continue;
    }
    (*count)++;
    u64 m = (u64)st.st_mtime * 1000000000ull + (u64)st.st_mtimespec.tv_nsec;
    if (m > *newest) *newest = m;
  }
  closedir(d);
}

static u64 watch_stamp(void) {
  u64 newest = 0;
  i64 count = 0;
  watch_mark_dir("src", &newest, &count);
  watch_mark_dir("tests", &newest, &count);
  watch_mark_dir("shared", &newest, &count);
  watch_mark_dir("content", &newest, &count);
  watch_mark_dir("partials", &newest, &count);
  struct stat st;
  if (stat("webc.json", &st) == 0) {
    u64 m = (u64)st.st_mtime * 1000000000ull + (u64)st.st_mtimespec.tv_nsec;
    if (m > newest) newest = m;
    count++;
  }
  return newest ^ ((u64)count << 48);
}

// `webc test --client`: the half of an app a C test cannot reach. It compiles the app's
// client sources, runs the real client_main, renders each mount to a batch of ops, and
// replays those batches through assets/webc.js against a fake document. What it catches
// is what actually goes wrong in a browser: an op stream that does not apply, a view that
// renders nothing, a patch that re-creates the page, a batch that never comes back.
static int cmd_client_test(void) {
  pick_cc();
  char lib[1024], inc[1024], srcdir[1200];
  snprintf(lib, sizeof lib, "%s/build/libwebc.a", webc_prefix());
  snprintf(inc, sizeof inc, "%s/include", webc_prefix());
  snprintf(srcdir, sizeof srcdir, "%s/tools/client", webc_prefix());
  Config cfg;
  config_load(&cfg, ".");
  Sources cs = {0};
  if (cfg.has_sets)
    collect_sources_for(&cfg, &cs, TARGET_CLIENT);
  else if (cfg.web[0] && path_exists(cfg.web))
    sources_add(&cs, cfg.web);
  if (cs.n == 0) {
    info("no client half to test (webc.json names no `client` or `web` sources)");
    return 0;
  }
  mkdirs(".webc/tests");
  char harness[1024], outbin[1024], batchfile[1024];
  snprintf(harness, sizeof harness, "%s/client_test.c", srcdir);
  snprintf(outbin, sizeof outbin, ".webc/tests/client_test");
  snprintf(batchfile, sizeof batchfile, ".webc/tests/client_batches.json");
  Buf cmd;
  buf_init(&cmd, nullptr, 4096);
  buf_printf(&cmd,
             "%s -std=c23 -g -O0 -Werror=return-type -I %s -I . -I %s "
             "-D_DARWIN_C_SOURCE=1 -D_GNU_SOURCE=1 %s %s %s -o %s -lpthread -lz",
             g_cc, inc, cfg.src, harness, buf_cstr(&(Buf){0}), "", outbin);
  // (built below, once the app's sources are in the command)
  buf_free(&cmd);
  buf_init(&cmd, nullptr, 4096);
  buf_printf(&cmd,
             "%s -std=c23 -g -O0 -Werror=return-type -I %s -I . -I %s "
             "-D_DARWIN_C_SOURCE=1 -D_GNU_SOURCE=1 %s",
             g_cc, inc, cfg.src, harness);
  for (i32 i = 0; i < cs.n; i++) buf_printf(&cmd, " %s", cs.paths[i]);
  buf_printf(&cmd, " %s -o %s -lpthread -lz -lsqlite3", lib, outbin);
  if (!run_cmd(buf_cstr(&cmd), false)) {
    fprintf(stderr, "webc: could not build the client harness for %d source%s\n", cs.n,
            cs.n == 1 ? "" : "s");
    return 1;
  }
  Buf run;
  buf_init(&run, nullptr, 1024);
  buf_printf(&run, "%s > %s", outbin, batchfile);
  if (!run_cmd(buf_cstr(&run), false)) {
    fprintf(stderr, "webc: the client half did not run (see above)\n");
    return 1;
  }
  Buf js;
  buf_init(&js, nullptr, 1024);
  buf_printf(&js, "node %s/client_test.js %s", srcdir, batchfile);
  int rc = system(buf_cstr(&js));
  if (rc != 0) return 1;
  return 0;
}

// A test binary prints one line per test: `--- PASS: name (0.00s)`. That line is the
// result; everything else it says is about the failure. Rather than teach the runner a
// second output format (it is linked into every app's suite, and its format is a
// contract), `webc test --json` reads the contract it already has.
typedef struct TestTally {
  i32 passed, failed, skipped;
} TestTally;

static void tally_suite(const char *output, TestTally *t, Buf *names, bool *first) {
  for (const char *l = output; l && *l;) {
    const char *nl = strchr(l, '\n');
    while (*l == ' ' || *l == '\t') l++; // the runner indents its result lines
    if (!strncmp(l, "--- PASS:", 9)) {
      t->passed++;
      if (names) {
        if (!*first) buf_puts(names, ",");
        *first = false;
        const char *st = l + 10, *en = strstr(st, " (");
        if (!en) en = st + strlen(st);
        buf_puts(names, "{\"name\":");
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%.*s", (int)(en - st), st);
        jputs(names, tmp);
        buf_puts(names, ",\"ok\":true}");
      }
    } else if (!strncmp(l, "--- FAIL:", 9)) {
      t->failed++;
      if (names) {
        if (!*first) buf_puts(names, ",");
        *first = false;
        const char *st = l + 10, *en = strstr(st, " (");
        if (!en) en = st + strlen(st);
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%.*s", (int)(en - st), st);
        buf_puts(names, "{\"name\":");
        jputs(names, tmp);
        buf_puts(names, ",\"ok\":false}");
      }
    } else if (!strncmp(l, "--- SKIP:", 9)) {
      t->skipped++;
    }
    l = nl ? nl + 1 : nullptr;
  }
}

static int cmd_macros(void);
static int cmd_lint(bool all, bool verbose, bool json);
int cmd_lsp(i32 argc, char **argv); // tools/webc/lsp.c
int cmd_mcp(i32 argc, char **argv); // tools/webc/mcp.c

static int cmd_test(TestArgs ta) {
  pick_cc();
#if defined(__linux__)
  // Linux's AddressSanitizer carries LeakSanitizer; the macOS one does not. Without this
  // the same suite passes on Darwin and fails here for a reason that has nothing to do
  // with the port — the framework does drop result arenas on some paths, and LSan finds
  // every one of them. Leaks stay a named gate instead of a silent difference:
  //
  //   make test-leaks      (WEBC_TEST_LEAKS=1, detect_leaks=1)
  //
  // An ASAN_OPTIONS the caller set is left alone.
  if (!getenv("ASAN_OPTIONS"))
    setenv("ASAN_OPTIONS", getenv("WEBC_TEST_LEAKS") ? "detect_leaks=1" : "detect_leaks=0", 0);
#endif
  char lib[1024], inc[1024];
  snprintf(lib, sizeof lib, "%s/build/libwebc.a", webc_prefix());
  snprintf(inc, sizeof inc, "%s/include", webc_prefix());
  // `--filter` names a suite and runs all of it; `--test` narrows by name across suites. One
  // flag for both would make `--filter=nullable` run only the tests whose *name* contained
  // "nullable" — a filter that answers "no tests" for the file you named.
  if (ta.only) setenv("WEBC_TEST_FILTER", ta.only, 1);
  else unsetenv("WEBC_TEST_FILTER");
  DIR *d = opendir("tests");
  if (!d) {
    info("no tests/ directory");
    return ta.macros ? cmd_macros() : 0;
  }

  // An app's tests must link the app. Its `main` is renamed rather than removed,
  // so the app does not have to know it is under test.
  char objs[64][1024];
  Buf objlist;
  buf_init(&objlist, nullptr, 1024);
  // The app's own sources: whatever `webc build` would compile, found the same
  // way (webc.json's entry/src, or the root main.c convention). No config file
  // is needed for an example or a scratch app.
  Config cfg;
  config_load(&cfg, ".");
  Sources app;
  collect_sources(&cfg, &app);
  // A split project's view tests live next to the client half as often as next to
  // the server one, so both halves are linked into the test binary.
  if (cfg.has_sets && cfg.client.n) {
    Sources cs;
    collect_sources_for(&cfg, &cs, TARGET_CLIENT);
    for (i32 i = 0; i < cs.n && app.n < 240; i++) sources_add(&app, cs.paths[i]);
  }
  // An app is a project with a webc.json, a server entry or a browser entry. The
  // framework checkout has src/ but none of those, so `webc test` there tests the
  // framework itself rather than a phantom app. A browser-only app counts: its tests
  // still need its component linked in.
  bool is_app = path_exists("webc.json") || cfg.has_sets ||
                (cfg.entry[0] && path_exists(cfg.entry)) ||
                (cfg.web[0] && path_exists(cfg.web));
  if (is_app) {
    mkdirs(".webc/tests/obj");
    for (i32 i = 0; i < app.n && i < 64; i++) {
      const char *base = strrchr(app.paths[i], '/');
      snprintf(objs[i], 1024, ".webc/tests/obj/%d_%s.o", (int)i, base ? base + 1 : app.paths[i]);
      Buf cmd;
      buf_init(&cmd, nullptr, 2048);
      buf_printf(&cmd,
                 "%s -std=c23 -g -O0 -Werror=return-type -I %s -I . -I %s "
                 "-D_DARWIN_C_SOURCE=1 -D_GNU_SOURCE=1 "
                 "-Dmain=webc_app_main -c %s -o %s",
                 g_cc, inc, cfg.src, app.paths[i], objs[i]);
      if (!run_cmd(buf_cstr(&cmd), true)) {
        fprintf(stderr, "webc: could not compile %s for the tests\n", app.paths[i]);
        closedir(d);
        if (ta.json) {
          Buf e;
          buf_init(&e, nullptr, 512);
          buf_puts(&e, "{\"tests\":[],\"suites\":0,\"failing\":1,\"ok\":false,\"error\":");
          char why[1100];
          snprintf(why, sizeof why, "could not compile %s for the tests", app.paths[i]);
          jputs(&e, why);
          buf_puts(&e, "}\n");
          printf("%s", buf_cstr(&e));
          buf_free(&e);
        }
        return 1;
      }
      buf_printf(&objlist, " %s", objs[i]);
    }
  }

  int failures = 0, ran = 0;
  struct dirent *e;
  mkdirs(".webc/tests");
  // the accumulated JSON, when `--json` asked for it
  Buf suites_json;
  buf_init(&suites_json, nullptr, 16384);
  bool json_first = true;
  Buf *json_suites = &suites_json;
  while ((e = readdir(d))) {
    size_t len = strlen(e->d_name);
    if (len < 7 || strcmp(e->d_name + len - 7, "_test.c") != 0) continue;
    // a file filter narrows which suites are even built: `webc test --filter=json`
    // must not spend a second compiling the other fifteen
    if (ta.filter && !strstr(e->d_name, ta.filter)) continue;
    if (ta.list) {
      printf("%s\n", e->d_name);
      continue;
    }
    char out[1024];
    snprintf(out, sizeof out, ".webc/tests/%.*s", (int)(len - 2), e->d_name);
    // A suite with no TEST_MAIN() still compiles: the framework's own main() is linked
    // in its place, so the binary boots the app's server and the suite hangs until the
    // timeout, with `listening addr=:8080` and not one word about tests. Say it at the
    // point of the mistake.
    {
      char suite[1024];
      snprintf(suite, sizeof suite, "tests/%s", e->d_name);
      char *text = read_all(suite, nullptr);
      if (text && !strstr(text, "TEST_MAIN")) {
        fprintf(stderr,
                "webc: %s has no TEST_MAIN(). A suite ends with it:\n"
                "        TEST_MAIN()\n"
                "      Without it the framework's main() is linked instead and the binary "
                "starts the app's server, so the suite hangs with no output.\n",
                suite);
        free(text);
        failures++;
        continue;
      }
      free(text);
    }
    Buf cmd;
    buf_init(&cmd, nullptr, 2048);
    buf_printf(&cmd,
               "%s -std=c23 -g -I %s -I tests -I %s -I . -D_DARWIN_C_SOURCE=1 -D_GNU_SOURCE=1 "
               "tests/%s%s %s -o %s -lpthread -lz -lsqlite3 -fsanitize=address,undefined",
               g_cc, inc, cfg.src, e->d_name, buf_cstr(&objlist), lib, out);
    if (!run_cmd(buf_cstr(&cmd), true)) {
      fprintf(stderr, "webc: could not build tests/%s\n", e->d_name);
      failures++;
      continue;
    }
    ran++;
    if (ta.json) {
      WCap cap;
      wcap_begin(&cap);
      int rc = system(out);
      char *text = wcap_end(&cap);
      TestTally t = {0};
      if (!json_first) buf_puts(json_suites, ",");
      json_first = false;
      buf_puts(json_suites, "{\"suite\":");
      jputs(json_suites, e->d_name);
      {
        // the tests in this suite go in their own array, so the object is a suite with
        // results in it rather than a suite with fields bolted on
        Buf items;
        buf_init(&items, nullptr, 4096);
        bool first_item = true;
        tally_suite(text ? text : "", &t, &items, &first_item);
        buf_printf(json_suites, ",\"passed\":%d,\"failed\":%d,\"skipped\":%d,\"tests\":[%s],", t.passed,
                   t.failed, t.skipped, buf_cstr(&items));
        buf_free(&items);
      }
      buf_puts(json_suites, "\"output\":");
      jputs(json_suites, text ? text : "");
      buf_puts(json_suites, "}");
      free(text);
      if (rc != 0 || t.failed) failures++;
      continue;
    }
    printf("\033[1m== %s\033[0m\n", e->d_name);
    if (system(out) != 0) failures++;
  }
  closedir(d);

  // a support program (not a suite): bridge_test.js runs the real command stream
  // through the real bridge, and the stream comes from tests/dom_batch_emit.c.
  if (path_exists("tests/dom_batch_emit.c")) {
    char out[1024];
    snprintf(out, sizeof out, ".webc/tests/dom_batch_emit");
    Buf cmd;
    buf_init(&cmd, nullptr, 2048);
    buf_printf(&cmd,
               "%s -std=c23 -g -I %s -I tests -D_DARWIN_C_SOURCE=1 -D_GNU_SOURCE=1 "
               "tests/dom_batch_emit.c %s %s -o %s -lpthread -lz -lsqlite3",
               g_cc, inc, buf_cstr(&objlist), lib, out);
    // The suite links libwebc.a whole, and the database layer it always pulls in
    // (every request can hold a session) needs sqlite at link time even when the test
    // never opens a database — on every platform, not just Darwin.
    if (!run_cmd(buf_cstr(&cmd), true)) {
      fprintf(stderr, "webc: could not build tests/dom_batch_emit.c\n");
      failures++;
    }
    buf_free(&cmd);
  }

  // the JS suites run the real command stream (and, for start_batch, a real wasm module)
  // through the real bridge
  const char *js_suites[] = {"bridge_test.js", "start_batch_test.js", "check_wiring_test.js",
                             "scaffold_check_test.js", "deploy_pack_test.js", "openapi_test.js",
                             "client_test.js", "learn_test.js", "fromdb_test.js", "scaffold_live_test.js", nullptr};
  for (const char **js = js_suites; *js; js++) {
    char path[128];
    snprintf(path, sizeof path, "tests/%s", *js);
    if (!path_exists(path)) continue;
    printf("\033[1m== %s\033[0m\n", *js);
    ran++;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "node tests/%s", *js);
    if (system(cmd) != 0) failures++;
  }
  if (ta.json) {
    printf("{\"tests\":[%s],\"suites\":%d,\"failing\":%d,\"ok\":%s}\n", buf_cstr(&suites_json),
           ran, failures, failures ? "false" : "true");
    buf_free(&suites_json);
    return failures ? 1 : 0;
  }
  info("%d suite%s, %d failing", ran, ran == 1 ? "" : "s", failures);
  if (ta.macros && cmd_macros() != 0) failures++;
  return failures ? 1 : 0;
}


// ---------------------------------------------------------------- deploy
//
// `webc deploy` (SPEC §9.1, the optional row): produce the one artifact this
// framework is built to produce — a stripped static-ish binary plus a manifest that
// says how to run it. No container image is invented here: a Dockerfile-less image
// is a promise about someone else's base image, and `dist/` with a `run` script
// works in every environment including the ones without a daemon.
//
// Cross-compilation is real when the toolchain is: `--target linux-x86_64` needs a
// musl sysroot, and when it is missing this says so instead of emitting a binary
// for the wrong machine.
static void deploy_packs(const Config *c, const char *out);
static int cmd_deploy(int argc, char **argv) {
  Config c;
  config_load(&c, ".");
  const char *target = nullptr;
  const char *out = "dist";
  bool tar = false;
  for (i32 i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--target") == 0 && i + 1 < argc)
      target = argv[++i];
    else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
      out = argv[++i];
    else if (strcmp(argv[i], "--tar") == 0)
      tar = true;
    else if (strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
      snprintf(g_pack, sizeof g_pack, "%s", argv[++i]);
      for (char *s = g_pack; *s; s++)
        if (*s == ' ') *s = ','; // "--pack docker fly" reads as naturally as commas
    } else
      die("unknown deploy flag %s (--target, --out, --tar, --pack)", argv[i]);
  }
  pick_cc();
  mkdirs(out);
  char bin[1200];
  snprintf(bin, sizeof bin, "%s/.webc/%s", c.root, c.name);
  if (!build(&c, target, true, bin)) {
    fprintf(stderr, "webc: build failed, nothing deployed\n");
    return 1;
  }
  // strip: a deploy artifact has no business carrying symbols
  char strip[1400];
  snprintf(strip, sizeof strip, "strip -x %s 2>/dev/null", bin);
  int _ = system(strip);
  (void)_;
  char dest[1300];
  snprintf(dest, sizeof dest, "%s/%s", out, c.name);
  char copy[1500];
  snprintf(copy, sizeof copy, "cp %s %s && chmod 755 %s", bin, dest, dest);
  if (system(copy) != 0) {
    fprintf(stderr, "webc: could not write %s\n", dest);
    return 1;
  }

  // the manifest: what to run, and which environment variables it reads
  char man[1400];
  snprintf(man, sizeof man, "%s/%s.manifest.json", out, c.name);
  Buf m;
  buf_init(&m, nullptr, 2048);
  buf_printf(&m, "{\n  \"name\": \"%s\",\n  \"version\": \"%s\",\n", c.name, c.version);
  buf_printf(&m, "  \"binary\": \"%s\",\n", c.name);
  buf_printf(&m, "  \"port\": %d,\n", c.port > 0 ? c.port : 8080);
  if (c.database[0]) buf_printf(&m, "  \"database\": \"%s\",\n", c.database);
  buf_puts(&m, "  \"env\": [\"WEBC_ADDR\", \"DATABASE_URL\", \"WEBC_ENV\", \"WEBC_DEV\"],\n");
  buf_puts(&m, "  \"healthcheck\": \"GET /health\"\n}\n");
  write_file(man, buf_cstr(&m));
  buf_free(&m);

  char run[1400];
  snprintf(run, sizeof run, "%s/run", out);
  Buf r;
  buf_init(&r, nullptr, 512);
  buf_printf(&r, "#!/bin/sh\n# start %s; every setting is an environment variable\n",
             c.name);
  buf_printf(&r, "exec ./%s \"$@\"\n", c.name);
  write_file(run, buf_cstr(&r));
  char chmod_cmd[1500];
  snprintf(chmod_cmd, sizeof chmod_cmd, "chmod 755 %s", run);
  int _c = system(chmod_cmd);
  (void)_c;
  buf_free(&r);

  if (tar) {
    char tarcmd[1600];
    snprintf(tarcmd, sizeof tarcmd, "tar czf %s/%s-%s.tar.gz -C %s %s %s.manifest.json run",
             out, c.name, c.version, out, c.name, c.name);
    if (system(tarcmd) != 0) fprintf(stderr, "webc: tar failed (the artifacts are still in %s)\n", out);
  }
  info("deployed %s (%s)", dest, target ? target : "this machine");
  if (g_pack[0]) deploy_packs(&c, out);
  return 0;
}

// ---------------------------------------------------------------- deploy packs
//
// `--pack`: the ops files that *run* the artifact. The binary is still the whole
// deploy — a pack writes the ritual beside it (the Dockerfile, the unit, the proxy
// lines), stamped from the manifest the deploy just wrote, because those files are
// the same in every app except for the name and the port — and the lines that are
// not the same (an SSE stream must not be buffered, an idle stream must not be cut)
// are exactly the ones a copy-paste from a blog gets wrong.
//
// Everything lands in dist/ as files the app owns: edit them, commit them, diff
// them on the next deploy. No pack invents a base image — the Dockerfile's base is
// an ARG, and no healthcheck is promised a route the framework does not serve.
typedef struct PackCtx {
  const char *name, *version, *out;
  int port;
  bool sqlite_file; // the database is a file under ./data: a sandbox must allow writing it
} PackCtx;

static void pack_write(const PackCtx *p, const char *file, const char *body) {
  char path[1300];
  snprintf(path, sizeof path, "%s/%s", p->out, file);
  write_file(path, body);
  info("  pack: %s", file);
}

static void pack_docker(const PackCtx *p) {
  Buf b;
  buf_init(&b, nullptr, 2048);
  buf_printf(&b, "# Written by `webc deploy --pack docker`. Edit freely: this file is the app's.\n");
  buf_puts(&b,
           "# The base is an ARG, not a choice: a base image is a promise about someone\n"
           "# else's machine, and this file runs the binary this repo built. A musl build\n"
           "# (`--target linux-x86_64`) is at home on alpine; the default matches glibc.\n"
           "ARG BASE=debian:bookworm-slim\n"
           "FROM ${BASE}\n"
           "WORKDIR /app\n");
  buf_printf(&b, "COPY %s run %s.manifest.json ./\n", p->name, p->name);
  buf_puts(&b,
           "RUN mkdir -p data            # a sqlite file lives here; nothing else is written\n");
  buf_printf(&b, "ENV WEBC_ADDR=:%d\nEXPOSE %d\n", p->port, p->port);
  buf_printf(&b,
           "# HEALTHCHECK CMD wget -qO- http://127.0.0.1:%d/health || exit 1\n"
           "#   (uncomment on a base with a shell; `webc new` ships the /health route)\n",
           p->port);
  buf_puts(&b, "ENTRYPOINT [\"./run\"]\n");
  pack_write(p, "Dockerfile", buf_cstr(&b));
  buf_free(&b);
}

static void pack_fly(const PackCtx *p) {
  Buf b;
  buf_init(&b, nullptr, 1024);
  buf_printf(&b, "# Written by `webc deploy --pack fly` (run from this directory: `fly deploy`).\n"
                 "# The region and the scale are the app's to change; the port is the app's own.\n");
  buf_printf(&b, "app = \"%s\"\nprimary_region = \"ams\"            # change me\n\n", p->name);
  buf_printf(&b, "[env]\nWEBC_ADDR = \":%d\"\n\n", p->port);
  buf_puts(&b, "[build]\ndockerfile = \"Dockerfile\"\n\n");
  buf_puts(&b,
           "# A WebC stream (SSE) is a long answer, not a stuck one: min_machines_running\n"
           "# keeps a room's frames flowing and `suspend` still saves the idle hour. A page\n"
           "# with no streams can stop machines entirely.\n");
  buf_printf(&b,
           "[[http_service]]\ninternal_port = %d\nforce_https = true\n"
           "processes = [\"app\"]\nmin_machines_running = 1\n\n",
           p->port);
  buf_puts(&b,
           "# Point the probe at a route the app answers (`/health` is the route every\n"
           "# `webc new` scaffold ships — the framework itself serves none).\n"
           "# [[http_service.checks]]\n"
           "#   interval = \"10s\"\n"
           "#   timeout = \"2s\"\n"
           "#   grace_period = \"30s\"\n"
           "#   method = \"GET\"\n"
           "#   path = \"/health\"\n");
  pack_write(p, "fly.toml", buf_cstr(&b));
  buf_free(&b);
}

static void pack_systemd(const PackCtx *p) {
  Buf b;
  buf_init(&b, nullptr, 1024);
  buf_printf(&b,
           "# Written by `webc deploy --pack systemd`. The unit assumes the deploy's\n"
           "# files live in /opt/webc/%s (mkdir it, cp dist/* into it), then:\n"
           "#   systemctl enable --now %s.service\n\n",
           p->name, p->name);
  buf_printf(&b,
           "[Unit]\nDescription=%s — a WebC app\n"
           "Wants=network-online.target\nAfter=network-online.target\n\n",
           p->name);
  buf_printf(&b,
           "[Service]\nType=simple\nWorkingDirectory=/opt/webc/%s\n"
           "ExecStart=/opt/webc/%s/%s\n",
           p->name, p->name, p->name);
  buf_printf(&b, "Environment=WEBC_ADDR=:%d\n", p->port);
  buf_puts(&b, "Restart=on-failure\nRestartSec=2\n\n");
  if (p->sqlite_file)
    buf_printf(&b,
               "# a sqlite database is a file in ./data: the sandbox must allow writing it\n"
               "ReadWritePaths=/opt/webc/%s/data\n\n",
               p->name);
  buf_puts(&b,
           "# the hardening that costs a static binary nothing\n"
           "DynamicUser=yes\n"
           "NoNewPrivileges=yes\n"
           "ProtectSystem=strict\n"
           "ProtectHome=yes\n"
           "PrivateTmp=yes\n");
  if (p->port > 0 && p->port < 1024)
    buf_puts(&b, "AmbientCapabilities=CAP_NET_BIND_SERVICE\n"
                 "CapabilityBoundingSet=CAP_NET_BIND_SERVICE\n");
  buf_puts(&b, "\n[Install]\nWantedBy=multi-user.target\n");
  char file[80];
  snprintf(file, sizeof file, "%s.service", p->name);
  pack_write(p, file, buf_cstr(&b));
  buf_free(&b);
}

static void pack_caddy(const PackCtx *p) {
  Buf b;
  buf_init(&b, nullptr, 512);
  buf_puts(&b,
           "# Written by `webc deploy --pack caddy`. One line is the reason this file\n"
           "# exists: `flush_interval -1` hands each SSE frame to the socket as it is\n"
           "# written — Caddy buffers by default, and a live page behind that looks\n"
           "# dead. Put your domain on the first line.\n");
  buf_printf(&b,
           "your-domain.example.com {\n\tencode zstd gzip\n"
           "\treverse_proxy 127.0.0.1:%d {\n\t\tflush_interval -1\n\t}\n}\n",
           p->port);
  pack_write(p, "Caddyfile.webc", buf_cstr(&b));
  buf_free(&b);
}

static void pack_nginx(const PackCtx *p) {
  Buf b;
  buf_init(&b, nullptr, 1024);
  buf_puts(&b,
           "# Written by `webc deploy --pack nginx`. The three lines that make a WebC\n"
           "# app happy behind nginx: HTTP/1.1 to the upstream (a stream needs it),\n"
           "# buffering off (SSE frames leave as they are written), and a read timeout\n"
           "# in hours, because an open stream is a working page, not a stuck one.\n");
  buf_printf(&b,
           "server {\n    listen 80;\n    server_name your-domain.example.com;\n\n"
           "    location / {\n"
           "        proxy_pass http://127.0.0.1:%d;\n"
           "        proxy_http_version 1.1;\n"
           "        proxy_set_header Host $host;\n"
           "        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\n"
           "        proxy_set_header X-Forwarded-Proto $scheme;\n\n"
           "        proxy_buffering off;\n"
           "        proxy_cache off;\n"
           "        proxy_read_timeout 24h;\n"
           "        proxy_send_timeout 24h;\n\n"
           "        # websockets (res_upgrade): the hop-by-hop headers\n"
           "        proxy_set_header Upgrade $http_upgrade;\n"
           "        proxy_set_header Connection $connection_upgrade;\n"
           "    }\n}\n\n"
           "# The map the Upgrade lines above need — it belongs in the http {} block,\n"
           "# not inside a server. Uncomment it there:\n"
           "# map $http_upgrade $connection_upgrade {\n"
           "#     default upgrade;\n"
           "#     ''      close;\n"
           "# }\n",
           p->port);
  pack_write(p, "nginx-webc.conf", buf_cstr(&b));
  buf_free(&b);
}

static void deploy_packs(const Config *c, const char *out) {
  PackCtx p;
  p.name = c->name;
  p.version = c->version;
  p.out = out;
  p.port = c->port > 0 ? c->port : 8080;
  p.sqlite_file = strncmp(c->database, "sqlite:", 7) == 0 &&
                  strstr(c->database, ":memory:") == nullptr &&
                  strchr(c->database, '/') != nullptr;

  bool docker = false;
  char it[256];
  snprintf(it, sizeof it, "%s", g_pack);
  for (char *tok = strtok(it, ","); tok; tok = strtok(nullptr, ",")) {
    if (strcmp(tok, "all") == 0) {
      pack_docker(&p);
      pack_fly(&p);
      pack_systemd(&p);
      pack_caddy(&p);
      pack_nginx(&p);
      docker = true;
    } else if (strcmp(tok, "docker") == 0) {
      pack_docker(&p);
      docker = true;
    } else if (strcmp(tok, "fly") == 0) {
      pack_docker(&p); // fly builds the image; the toml names the file
      pack_fly(&p);
      docker = true;
    } else if (strcmp(tok, "systemd") == 0) {
      pack_systemd(&p);
    } else if (strcmp(tok, "caddy") == 0) {
      pack_caddy(&p);
    } else if (strcmp(tok, "nginx") == 0) {
      pack_nginx(&p);
    } else {
      die("unknown pack '%s' — docker, fly, systemd, caddy, nginx, all (comma-separated)", tok);
    }
  }

  Buf d;
  buf_init(&d, nullptr, 1024);
  buf_printf(&d, "# Deploying %s\n\n", c->name);
  buf_printf(&d, "One binary, one directory: `%s` + `run` + `%s.manifest.json`.\n", c->name,
             c->name);
  buf_puts(&d, "Every setting is an environment variable (`WEBC_ADDR`, `DATABASE_URL`,\n"
               "`WEBC_ENV`, `WEBC_DEV` — the manifest lists them).\n\n");
  buf_printf(&d, "This deploy also wrote ops files (edit them; they are the app's):\n\n");
  if (docker)
    buf_printf(&d, "- `Dockerfile` — the image around the binary. `docker build -t %s . &&\n"
                      "  docker run -p %d:%d %s`\n",
               c->name, p.port, p.port, c->name);
  buf_puts(&d, "- `fly.toml` — fly.io; `fly deploy` from this directory.\n"
                "- `Caddyfile.webc` / `nginx-webc.conf` — a proxy in front; the SSE lines\n"
                "  are already the ones a live page needs.\n");
  buf_printf(&d, "- `%s.service` — systemd; see the comment at the top of the file.\n", c->name);
  buf_puts(&d, "\nThe healthcheck route is the one every `webc new` scaffold ships (`GET(\"/health\")`\n"
                "in the app's own sources — the framework serves none), or a route you add.\n");
  pack_write(&p, "DEPLOY.md", buf_cstr(&d));
  buf_free(&d);
}

// ---------------------------------------------------------------- ci
//
// `webc ci` writes the workflow the docs promise (`webc check` + `webc test`, a
// dist artifact on a tag), as a file the app owns. The recipe pins the framework
// build it installs the same way an app pins any dependency: one ref at the top,
// bumped deliberately. GitHub Actions by default, `--gitlab` for the other shape.
// Refuses to run outside an app: a workflow about `webc check` in a tree with no
// webc.json is a lie that fails on the first push.
static const char CI_GITHUB[] =
    "# Written by `webc ci` — the gate the docs promise, as a workflow. This file is\n"
    "# the app's: edit the triggers, add jobs, keep `webc check` green.\n"
    "name: webc\n"
    "on:\n"
    "  push:\n"
    "  pull_request:\n\n"
    "env:\n"
    "  # the framework build to pin — bump it the way you bump any dependency\n"
    "  WEBC_REF: v%s\n\n"
    "jobs:\n"
    "  check:\n"
    "    runs-on: ubuntu-latest\n"
    "    steps:\n"
    "      - uses: actions/checkout@v4\n"
    "      - name: install webc\n"
    "        run: |\n"
    "          curl -fsSL https://raw.githubusercontent.com/bumasoft/WebC/$WEBC_REF/scripts/install.sh \\\n"
    "            | sh -s -- --ref $WEBC_REF\n"
    "          echo \"$HOME/.local/bin\" >> \"$GITHUB_PATH\"\n"
    "%s"
    "      - run: webc check\n"
    "      - run: webc test\n"
    "      - name: the API boundary, if the app ships one\n"
    "        run: |\n"
    "          test ! -f openapi.json || webc openapi --check\n"
    "          test ! -f webc.client.ts || webc client --ts --check\n"
    "      - name: dist (tags only)\n"
    "        if: startsWith(github.ref, 'refs/tags/')\n"
    "        run: webc deploy --tar\n"
    "      - uses: actions/upload-artifact@v4\n"
    "        if: startsWith(github.ref, 'refs/tags/')\n"
    "        with:\n"
    "          name: dist\n"
    "          path: dist/\n";

static const char CI_GITLAB[] =
    "# Written by `webc ci --gitlab`. This file is the app's: edit the triggers,\n"
    "# keep `webc check` green.\n"
    "stages: [check]\n\n"
    "variables:\n"
    "  # the framework build to pin — bump it the way you bump any dependency\n"
    "  WEBC_REF: v%s\n\n"
    "check:\n"
    "  stage: check\n"
    "  image: debian:bookworm-slim\n"
    "  before_script:\n"
    "    - apt-get update -qq && apt-get install -y -qq curl ca-certificates build-essential pkg-config libsqlite3-dev nodejs\n"
    "    - curl -fsSL https://raw.githubusercontent.com/bumasoft/WebC/$WEBC_REF/scripts/install.sh | sh -s -- --ref $WEBC_REF\n"
    "    - export PATH=\"$HOME/.local/bin:$PATH\"\n"
    "%s"
    "  script:\n"
    "    - webc check\n"
    "    - webc test\n"
    "    - sh -c 'test ! -f openapi.json || webc openapi --check'\n"
    "    - sh -c 'test ! -f webc.client.ts || webc client --ts --check'\n";

static int cmd_ci(i32 argc, char **argv) {
  Config c;
  config_load(&c, ".");
  if (!path_exists("webc.json"))
    die("no webc.json here — `webc ci` writes CI for an app, and this is not one");
  bool gitlab = false;
  for (i32 i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--gitlab") == 0)
      gitlab = true;
    else
      die("unknown ci flag %s (--gitlab)", argv[i]);
  }
  // a browser half means the gate needs wasi-sdk: install it beside the CLI
  bool wants_wasm = (c.has_sets && c.client.n > 0) || c.web[0];
  const char *wasi_gh =
      wants_wasm
          ? "      - name: install wasi-sdk (the app has a browser half)\n"
            "        run: curl -fsSL https://raw.githubusercontent.com/bumasoft/WebC/$WEBC_REF/scripts/install.sh \\\n"
            "            | sh -s -- --wasi\n"
          : "";
  const char *wasi_gl = wants_wasm ? "    - curl -fsSL https://raw.githubusercontent.com/bumasoft/WebC/$WEBC_REF/scripts/install.sh | sh -s -- --wasi\n" : "";

  char body[4096];
  snprintf(body, sizeof body, gitlab ? CI_GITLAB : CI_GITHUB, CLI_VERSION,
           gitlab ? wasi_gl : wasi_gh);
  const char *rel = gitlab ? ".gitlab-ci.yml" : ".github/workflows/webc.yml";
  char path[1200];
  snprintf(path, sizeof path, "%s/%s", c.root, rel);
  if (path_exists(path)) die("%s already exists — delete it to regenerate", rel);
  write_file(path, body);
  info("wrote %s%s", rel, wants_wasm ? " (with wasi-sdk: the app has a client half)" : "");
  return 0;
}

// ---------------------------------------------------------------- openapi
//
// `webc openapi` — the API boundary, generated from what the sources already say.
// The route table is typed, the models are typed, and every `json()` names its
// shape where it is built (facts.c records the call, its status literal and the
// model it names); this writes the one document the outside world reads: paths
// from the routes, request bodies from `body(T)`, responses from `json`/
// `json_list`, schemas from the model descriptors with their qualifiers as
// constraints — `.max` is `maxLength`, `.gte` is `minimum`, `.format` is `format`.
//
// What it refuses to guess: a response whose shape is not a model (a `str` of
// already-built JSON) is an `{}` with a description, no query parameter is
// invented, and a `:param` is a string unless the handler's own `path_i64`
// says otherwise. The document is a file the app owns — commit it, or let
// `--check` fail CI when it drifts from the sources that generated it.

// `.max = 200` in the quals, as written — the descriptor's own sentence, parsed
// for the three numbers and the one format a schema can carry.
static bool oa_qual_has(const char *quals, const char *name) {
  if (!quals) return false;
  size_t nl = strlen(name);
  const char *p = quals;
  while ((p = strstr(p, name))) {
    bool word_before = p == quals || (p[-1] == '.' || p[-1] == ' ' || p[-1] == ',');
    if (word_before && (p[nl] == '=' || p[nl] == ' ')) return true;
    p += nl;
  }
  return false;
}

static i64 oa_qual_int(const char *quals, const char *name) {
  if (!quals) return 0;
  char pat[64];
  snprintf(pat, sizeof pat, "%s = ", name);
  const char *p = strstr(quals, pat);
  if (!p) return 0;
  return strtoll(p + strlen(pat), nullptr, 10);
}

static void oa_qual_str(const char *quals, const char *name, char *out, size_t cap) {
  out[0] = 0;
  if (!quals) return;
  char pat[64];
  snprintf(pat, sizeof pat, "%s = \"", name);
  const char *p = strstr(quals, pat);
  if (!p) return;
  p += strlen(pat);
  size_t n = 0;
  while (*p && *p != '"' && n < cap - 1) out[n++] = *p++;
  out[n] = 0;
}

static const char *oa_scalar(const char *kind) {
  if (!kind) return nullptr;
  if (!strcmp(kind, "i8") || !strcmp(kind, "i16") || !strcmp(kind, "i32") ||
      !strcmp(kind, "i64") || !strcmp(kind, "u8") || !strcmp(kind, "u16") ||
      !strcmp(kind, "u32") || !strcmp(kind, "u64"))
    return "integer";
  if (!strcmp(kind, "f32") || !strcmp(kind, "f64")) return "number";
  if (!strcmp(kind, "bool")) return "boolean";
  if (!strcmp(kind, "Text") || !strcmp(kind, "str") || !strcmp(kind, "Bytes")) return "string";
  return nullptr;
}

static bool oa_is_model(WFacts *F, const char *kind) {
  return kind && facts_model(F, kind, strlen(kind)) != nullptr;
}

static bool oa_is_enum(WFacts *F, const char *kind) {
  return kind && facts_one(F->enumt, F->nenum, kind, strlen(kind)) != nullptr;
}

// one property's schema: assumes the caller wrote the key (or is inside properties)
static void oa_prop(WFacts *F, JsonWriter *w, const WFld *f) {
  const char *qu = f->quals;
  bool nullable = oa_qual_has(qu, ".null");
  jw_obj_begin(w);
  const char *scalar = oa_scalar(f->kind);
  if (scalar) {
    if (nullable) {
      jw_key(w, "type");
      jw_arr_begin(w);
      jw_cstr(w, scalar);
      jw_null(w);
      jw_arr_end(w);
    } else {
      jw_kv_cstr(w, "type", scalar);
    }
    if (!strcmp(scalar, "string")) {
      i64 mx = oa_qual_int(qu, ".max"), mn = oa_qual_int(qu, ".min");
      if (mx > 0) jw_kv_i64(w, "maxLength", mx);
      if (mn > 0) jw_kv_i64(w, "minLength", mn);
      char fmt[32];
      oa_qual_str(qu, ".format", fmt, sizeof fmt);
      if (fmt[0]) jw_kv_cstr(w, "format", fmt);
    } else {
      i64 lo = oa_qual_int(qu, ".gte"), hi = oa_qual_int(qu, ".lte");
      if (lo || hi) {
        if (lo) jw_kv_i64(w, "minimum", lo);
        if (hi) jw_kv_i64(w, "maximum", hi);
      }
    }
  } else if (oa_is_enum(F, f->kind)) {
    // a closed set exchanges its members' names; the member list is not in the
    // descriptor's quals, so the schema says the type and names the set
    jw_kv_cstr(w, "type", "string");
    jw_key(w, "description");
    jw_cstr(w, fmt_s("one of the %s members (WEBC_ENUM)", f->kind).p);
  } else if (!strncmp(f->kind, "Vec", 3) && f->kind[3]) {
    const char *el = f->kind + 3;
    jw_kv_cstr(w, "type", "array");
    jw_obj_in(w, "items") {
      const char *esc = nullptr;
      if (!strcmp(el, "Str")) esc = "string";
      else if (!strcmp(el, "I64") || !strcmp(el, "I32")) esc = "integer";
      else if (!strcmp(el, "F64")) esc = "number";
      else if (!strcmp(el, "Bytes")) esc = "string";
      if (esc) {
        jw_kv_cstr(w, "type", esc);
      } else if (oa_is_model(F, el)) {
        jw_kv_cstr(w, "$ref", fmt_s("#/components/schemas/%s", el).p);
      } else {
        jw_kv_cstr(w, "type", "object");
      }
    }
  } else if (oa_is_model(F, f->kind)) {
    jw_kv_cstr(w, "$ref", fmt_s("#/components/schemas/%s", f->kind).p);
  } else {
    jw_kv_cstr(w, "type", "object");
  }
  if (f->pk) jw_kv_bool(w, "readOnly", true);
  jw_obj_end(w);
}

static void oa_model(WFacts *F, JsonWriter *w, const WMod *m) {
  jw_obj_in(w, m->name) {
    jw_kv_cstr(w, "type", "object");
    jw_key(w, "properties");
    jw_obj_begin(w);
    for (i32 i = 0; i < m->nf; i++) {
      if (oa_qual_has(m->f[i].quals, ".no_json")) continue;
      jw_key(w, m->f[i].name);
      oa_prop(F, w, &m->f[i]);
    }
    jw_obj_end(w);
    // required: present-and-nonempty is the model's own rule; `.null` opts a field out
    const char *req[64];
    i32 nreq = 0;
    for (i32 i = 0; i < m->nf && nreq < 64; i++) {
      if (oa_qual_has(m->f[i].quals, ".no_json")) continue;
      if ((m->f[i].required || m->f[i].not_null) && !oa_qual_has(m->f[i].quals, ".null"))
        req[nreq++] = m->f[i].name;
    }
    if (nreq) {
      jw_key(w, "required");
      jw_arr_begin(w);
      for (i32 i = 0; i < nreq; i++) jw_cstr(w, req[i]);
      jw_arr_end(w);
    }
  }
}

// `:id` -> `{id}`, and the placeholder names, one per call
static void oa_path(const char *in, char *out, size_t cap, char names[][32], i32 *nnames,
                    i32 maxnames) {
  size_t o = 0;
  *nnames = 0;
  for (const char *p = in; *p && o < cap - 1; p++) {
    if (*p == ':' && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
      p++;
      if (*nnames < maxnames) {
        size_t n = 0;
        while ((isalnum((unsigned char)*p) || *p == '_') && n < 31) names[*nnames][n++] = *p++;
        names[*nnames][n] = 0;
        (*nnames)++;
        if (o + n + 3 >= cap - 1) break;
        out[o++] = '{';
        memcpy(out + o, names[*nnames - 1], n);
        o += n;
        out[o++] = '}';
        p--; // the loop's own ++ lands on the separator
      }
    } else {
      out[o++] = *p;
    }
  }
  out[o] = 0;
}

typedef struct OaRoute {
  const WRoute *r;
  u32 end; // the next route's head in the same file: calls before it are this one's
  char opath[256];
  char params[8][32];
  i32 nparams;
} OaRoute;

static int cmd_openapi(i32 argc, char **argv) {
  Config c;
  config_load(&c, ".");
  if (!path_exists("webc.json"))
    die("no webc.json here — `webc openapi` documents an app's routes, and this is not one");
  const char *out = "openapi.json";
  bool check = false;
  for (i32 i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--out") && i + 1 < argc)
      out = argv[++i];
    else if (!strcmp(argv[i], "--check"))
      check = true;
    else
      die("unknown openapi flag %s (--out, --check)", argv[i]);
  }
  Arena *a = arena_new(512 * 1024);
  WFacts *F = facts_new(a, ".");
  if (!F) die("could not index the project");

  // a framework checkout is a tree of projects; the document answers for the one
  // the developer is standing in, exactly as `webc routes` does
  char here[1024];
  char *rp = realpath(".", nullptr);
  snprintf(here, sizeof here, "%s", rp ? rp : ".");
  free(rp);
  i32 host = -1;
  for (i32 i = 0; i < F->nproj; i++)
    if (!strcmp(F->proj[i].root, here)) host = i;

  OaRoute rs[256];
  i32 nrs = 0;
  for (i32 i = 0; i < F->nroute && nrs < 256; i++) {
    if (host >= 0 && facts_proj_of(F, F->route[i].file) != host) continue;
    rs[nrs].r = &F->route[i];
    rs[nrs].end = UINT32_MAX;
    for (i32 k = i + 1; k < F->nroute; k++)
      if (F->route[k].file == F->route[i].file) {
        rs[nrs].end = F->route[k].off;
        break;
      }
    oa_path(F->route[i].path, rs[nrs].opath, sizeof rs[nrs].opath, rs[nrs].params,
            &rs[nrs].nparams, 8);
    nrs++;
  }

  JsonWriter w;
  jw_init(&w, a, 32 * 1024);
  jw_obj_begin(&w);
  jw_kv_cstr(&w, "openapi", "3.1.0");
  jw_obj_in(&w, "info") {
    jw_kv_cstr(&w, "title", c.name);
    jw_kv_cstr(&w, "version", c.version);
  }

  i32 nops = 0;
  jw_key(&w, "paths");
  jw_obj_begin(&w);
  // one entry per distinct path, methods inside — the shape a client generator reads
  for (i32 i = 0; i < nrs; i++) {
    bool seen = false;
    for (i32 k = 0; k < i; k++)
      if (!strcmp(rs[k].opath, rs[i].opath)) seen = true;
    if (seen) continue;
    jw_obj_in(&w, rs[i].opath) {
      for (i32 m = i; m < nrs; m++) {
        if (strcmp(rs[m].opath, rs[i].opath)) continue;
        const WRoute *r = rs[m].r;
        char method[16];
        snprintf(method, sizeof method, "%s", r->method);
        for (char *s = method; *s; s++) *s = (char)tolower((unsigned char)*s);
        if (strcmp(method, "get") && strcmp(method, "post") && strcmp(method, "put") &&
            strcmp(method, "patch") && strcmp(method, "delete"))
          continue; // ANY/HEAD/OPTIONS have no OpenAPI spelling
        nops++;
        jw_obj_in(&w, method) {
          // the placeholders first: a param is a string unless this handler's own
          // `path_i64` says it is a number
          if (rs[m].nparams) {
            jw_key(&w, "parameters");
            jw_arr_begin(&w);
            for (i32 pi = 0; pi < rs[m].nparams; pi++) {
              jw_obj_begin(&w);
              jw_kv_cstr(&w, "name", rs[m].params[pi]);
              jw_kv_cstr(&w, "in", "path");
              jw_kv_bool(&w, "required", true);
              const char *ptype = "string";
              for (i32 ci = 0; ci < F->ncall; ci++) {
                const WCall *cl = &F->call[ci];
                if (cl->file != r->file || cl->call_off < r->off || cl->call_off >= rs[m].end)
                  continue;
                if (!strcmp(cl->macro, "path-i64") && cl->name &&
                    !strcmp(cl->name, rs[m].params[pi]))
                  ptype = "integer";
              }
              jw_obj_in(&w, "schema") { jw_kv_cstr(&w, "type", ptype); }
              jw_obj_end(&w);
            }
            jw_arr_end(&w);
          }
          // the calls this handler's block makes decide the body and the responses
          const char *body_model = nullptr;
          i32 body_kind = 0; // 0 = json and form (body, body_try), 1 = json, 2 = form
          const WCall *resp[16];
          i32 nresp = 0;
          bool html = r->view != nullptr;
          for (i32 ci = 0; ci < F->ncall; ci++) {
            const WCall *cl = &F->call[ci];
            if (cl->file != r->file || cl->call_off < r->off || cl->call_off >= rs[m].end)
              continue;
            if ((!strcmp(cl->macro, "body") || !strcmp(cl->macro, "body_try")) && cl->name[0]) {
              body_model = cl->name;
              body_kind = 0;
            } else if (!strcmp(cl->macro, "body_json") && cl->name[0]) {
              body_model = cl->name;
              body_kind = 1;
            } else if (!strcmp(cl->macro, "body_form") && cl->name[0]) {
              body_model = cl->name;
              body_kind = 2;
            }
            if ((!strcmp(cl->macro, "json") || !strcmp(cl->macro, "json_list")) &&
                nresp < 16 && cl->status)
              resp[nresp++] = cl;
            if (!strcmp(cl->macro, "page") || !strcmp(cl->macro, "render")) html = true;
          }
          if (body_model) {
            jw_obj_in(&w, "requestBody") {
              jw_kv_cstr(&w, "description",
                        "parsed and validated — a 422 names the field that failed");
              jw_obj_in(&w, "content") {
                if (body_kind != 2)
                  jw_obj_in(&w, "application/json") {
                    jw_obj_in(&w, "schema") {
                      if (oa_is_model(F, body_model))
                        jw_kv_cstr(&w, "$ref", fmt_s("#/components/schemas/%s", body_model).p);
                      else
                        jw_kv_cstr(&w, "type", "object");
                    }
                  }
                if (body_kind != 1)
                  jw_obj_in(&w, "application/x-www-form-urlencoded") {
                    jw_obj_in(&w, "schema") {
                      if (oa_is_model(F, body_model))
                        jw_kv_cstr(&w, "$ref", fmt_s("#/components/schemas/%s", body_model).p);
                      else
                        jw_kv_cstr(&w, "type", "object");
                    }
                  }
              }
            }
          }
          jw_key(&w, "responses");
          jw_obj_begin(&w);
          if (!nresp && !html && !body_model) {
            // a handler the index cannot read a shape from (res_text, a stream) still
            // needs one entry: an empty responses object is not a document a
            // generator will accept
            jw_obj_in(&w, "default") { jw_kv_cstr(&w, "description", ""); }
          }
          for (i32 ri = 0; ri < nresp; ri++) {
            const WCall *cl = resp[ri];
            char sk[8];
            snprintf(sk, sizeof sk, "%d", cl->status);
            jw_obj_in(&w, sk) {
              jw_kv_cstr(&w, "description", "");
              jw_obj_in(&w, "content") {
                jw_obj_in(&w, "application/json") {
                  jw_obj_in(&w, "schema") {
                    if (cl->list) {
                      jw_kv_cstr(&w, "type", "array");
                      jw_obj_in(&w, "items") {
                        if (cl->name[0] && oa_is_model(F, cl->name))
                          jw_kv_cstr(&w, "$ref",
                                     fmt_s("#/components/schemas/%s", cl->name).p);
                        else
                          jw_kv_cstr(&w, "type", "object");
                      }
                    } else if (cl->name[0] && oa_is_model(F, cl->name)) {
                      jw_kv_cstr(&w, "$ref", fmt_s("#/components/schemas/%s", cl->name).p);
                    } else {
                      jw_kv_cstr(&w, "type", "object");
                      jw_kv_cstr(&w, "description",
                                "already-built JSON — the shape lives in the handler");
                    }
                  }
                }
              }
            }
          }
          if (html) {
            jw_obj_in(&w, "200") {
              jw_kv_cstr(&w, "description", "the page, server-rendered");
              jw_obj_in(&w, "content") {
                jw_obj_in(&w, "text/html") { jw_obj_in(&w, "schema") { jw_kv_cstr(&w, "type", "string"); } }
              }
            }
          }
          if (body_model) {
            jw_obj_in(&w, "422") {
              jw_kv_cstr(&w, "description",
                      "invalid input — the field that failed, by name");
              jw_obj_in(&w, "content") {
                jw_obj_in(&w, "application/json") {
                  jw_obj_in(&w, "schema") { jw_kv_cstr(&w, "$ref", "#/components/schemas/WebcError"); }
                }
              }
            }
          }
          jw_obj_end(&w); // responses
        }
      }
    }
  }
  jw_obj_end(&w); // paths

  jw_key(&w, "components");
  jw_obj_begin(&w);
  jw_key(&w, "schemas");
  jw_obj_begin(&w);
  i32 nschemas = 0;
  for (i32 i = 0; i < F->nmod; i++) {
    const WMod *m = &F->mod[i];
    if (m->internal) continue; // WEBC_MODEL_INTERNAL: a request body, not a table — still
                              // worth a schema, but it never reaches the wire alone
    if (host >= 0 && facts_proj_of(F, m->file) != host) continue;
    oa_model(F, &w, m);
    nschemas++;
  }
  // the flat error every failure answers with
  jw_obj_in(&w, "WebcError") {
    jw_kv_cstr(&w, "type", "object");
    jw_key(&w, "properties");
    jw_obj_begin(&w);
    jw_key(&w, "error");
    jw_obj_begin(&w);
    jw_kv_cstr(&w, "type", "string");
    jw_obj_end(&w);
    jw_key(&w, "message");
    jw_obj_begin(&w);
    jw_kv_cstr(&w, "type", "string");
    jw_obj_end(&w);
    jw_key(&w, "status");
    jw_obj_begin(&w);
    jw_kv_cstr(&w, "type", "integer");
    jw_obj_end(&w);
    jw_obj_end(&w);
  }
  nschemas++;
  jw_obj_end(&w);   // schemas
  jw_obj_end(&w);   // components
  jw_obj_end(&w);   // document
  str doc = jw_finish(&w);

  if (check) {
    str on_disk = read_file(a, out);
    // the file carries one trailing newline (git likes it); the document does not
    if (on_disk.n && on_disk.p[on_disk.n - 1] == '\n') on_disk.n--;
    if (!on_disk.p || !str_eq(on_disk, doc)) {
      fprintf(stderr, "webc: %s is stale — run `webc openapi --out %s` and commit it\n", out, out);
      arena_destroy(a);
      return 1;
    }
    info("%s: current (%d paths, %d operations, %d schemas)", out, nrs, nops, nschemas);
    arena_destroy(a);
    return 0;
  }
  char body[64 * 1024];
  snprintf(body, sizeof body, "%.*s\n", (int)doc.n, doc.p);
  write_file(out, body);
  info("%s: %d paths, %d operations, %d schemas", out, nrs, nops, nschemas);
  arena_destroy(a);
  return 0;
}

// ---------------------------------------------------------------- client
//
// `webc client --ts` — the beachhead's other half: a typed fetch client for the
// same facts `webc openapi` documents, for the React app that is not moving today.
// Wire types per model (and an Input twin without the pk), one function per API
// operation (a route with a json response, a body, or a mutating verb), path
// placeholders as arguments typed by the handler's own `path_i64`.
//
// The file is the app's the moment it lands — edit the names, commit it, or let
// `--check` fail when the sources drifted. Field names keep their wire spelling
// (snake_case stays): the client describes what the server sends, not what a
// style guide wishes it sent.

static const char *ts_type(WFacts *F, const char *kind) {
  const char *s = oa_scalar(kind);
  if (s) return !strcmp(s, "integer") || !strcmp(s, "number") ? "number" : s;
  if (!kind) return "unknown";
  if (!strncmp(kind, "Vec", 3) && kind[3]) {
    const char *el = kind + 3;
    if (!strcmp(el, "Str") || !strcmp(el, "Bytes")) return "string[]";
    if (!strcmp(el, "I64") || !strcmp(el, "I32")) return "number[]";
    if (!strcmp(el, "F64")) return "number[]";
    if (oa_is_model(F, el)) return nullptr; // an array of a model: named below
  }
  if (oa_is_model(F, kind) || oa_is_enum(F, kind)) return nullptr; // named types
  return "unknown";
}

static void ts_model_types(WFacts *F, Buf *b, const WMod *m) {
  buf_printf(&*b, "export interface %s {\n", m->name);
  for (i32 i = 0; i < m->nf; i++) {
    if (oa_qual_has(m->f[i].quals, ".no_json")) continue;
    const char *t = ts_type(F, m->f[i].kind);
    if (!strncmp(m->f[i].kind, "Vec", 3) && m->f[i].kind[3] &&
        oa_is_model(F, m->f[i].kind + 3))
      buf_printf(&*b, "  %s: %s[];\n", m->f[i].name, m->f[i].kind + 3);
    else if (!t)
      buf_printf(&*b, "  %s: %s;\n", m->f[i].name, m->f[i].kind);
    else
      buf_printf(&*b, "  %s: %s;\n", m->f[i].name, t);
  }
  buf_printf(&*b, "}\n\n");
  // the Input twin: what a create/update sends — no pk (the database assigns it),
  // no field the model refuses to serialise, required recomputed without the pk
  buf_printf(&*b, "export interface %sInput {\n", m->name);
  for (i32 i = 0; i < m->nf; i++) {
    if (m->f[i].pk || oa_qual_has(m->f[i].quals, ".no_json")) continue;
    bool req = (m->f[i].required || m->f[i].not_null) &&
               !oa_qual_has(m->f[i].quals, ".null") && !m->f[i].pk;
    const char *t = ts_type(F, m->f[i].kind);
    if (!strncmp(m->f[i].kind, "Vec", 3) && m->f[i].kind[3] &&
        oa_is_model(F, m->f[i].kind + 3))
      buf_printf(&*b, "  %s%s: %s[];\n", m->f[i].name, req ? "" : "?", m->f[i].kind + 3);
    else if (!t)
      buf_printf(&*b, "  %s%s: %s;\n", m->f[i].name, req ? "" : "?", m->f[i].kind);
    else
      buf_printf(&*b, "  %s%s: %s;\n", m->f[i].name, req ? "" : "?", t);
  }
  buf_printf(&*b, "}\n\n");
}

// `/api/notes/:id/delete` -> "Notes" (the model-named segment, singularised only
// when the model says so) — the readable half of a generated name, mechanical
// about it: no segment is dropped or rewritten unless a model backs the rewrite
static void ts_path_name(WFacts *F, const char *path, char *out, size_t cap,
                         char params[][32], i32 nparams) {
  size_t o = 0;
  out[0] = 0;
  const char *p = path;
  bool first = true, dropped_api = false;
  while (*p) {
    while (*p == '/') p++;
    if (!*p) break;
    const char *e = p;
    while (*e && *e != '/') e++;
    size_t n = (size_t)(e - p);
    if (*p == ':') {
      p = e;
      continue;
    }
    // the mount is the URL's, not the type's: `api` names a prefix, not a model
    if (n == 3 && !strncmp(p, "api", 3) && first) {
      dropped_api = true;
      p = e;
      first = false;
      continue;
    }
    (void)dropped_api;
    char seg[64];
    if (n >= sizeof seg) n = sizeof seg - 1;
    memcpy(seg, p, n);
    seg[n] = 0;
    // a trailing 's' names the model's table, not the type: strip it when the
    // singular is a model and the plural is not (todos -> Todo, addresses stays —
    // a URL spells the model lowercase; the model's own case is the type's)
    if (seg[n - 1] == 's' && n > 1 && strcmp(&seg[n - 2], "ss")) {
      char singular[64];
      snprintf(singular, sizeof singular, "%.*s", (int)n - 1, seg);
      singular[0] = (char)toupper((unsigned char)singular[0]);
      if (oa_is_model(F, singular) && !oa_is_model(F, seg))
        snprintf(seg, sizeof seg, "%s", singular);
    }
    seg[0] = (char)toupper((unsigned char)seg[0]);
    if (o + strlen(seg) + 1 < cap) {
      memcpy(out + o, seg, strlen(seg));
      o += strlen(seg);
    }
    p = e;
    first = false;
  }
  (void)params;
  (void)nparams;
  out[o] = 0;
}

static int cmd_client(i32 argc, char **argv) {
  Config c;
  config_load(&c, ".");
  if (!path_exists("webc.json"))
    die("no webc.json here — `webc client` writes a client for an app's API, and this "
        "is not one");
  const char *out = "webc.client.ts";
  bool check = false, ts = false;
  for (i32 i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--out") && i + 1 < argc)
      out = argv[++i];
    else if (!strcmp(argv[i], "--check"))
      check = true;
    else if (!strcmp(argv[i], "--ts"))
      ts = true; // the only dialect today; the flag is the door the next one uses
    else
      die("unknown client flag %s (--ts, --out, --check)", argv[i]);
  }
  (void)ts;
  Arena *a = arena_new(512 * 1024);
  WFacts *F = facts_new(a, ".");
  if (!F) die("could not index the project");
  char here[1024];
  char *rp = realpath(".", nullptr);
  snprintf(here, sizeof here, "%s", rp ? rp : ".");
  free(rp);
  i32 host = -1;
  for (i32 i = 0; i < F->nproj; i++)
    if (!strcmp(F->proj[i].root, here)) host = i;

  // routes and their blocks, exactly as cmd_openapi sees them
  typedef struct ClRoute {
    const WRoute *r;
    u32 end;
    char opath[256];
    char params[8][32];
    i32 nparams;
  } ClRoute;
  ClRoute rs[256];
  i32 nrs = 0;
  for (i32 i = 0; i < F->nroute && nrs < 256; i++) {
    if (host >= 0 && facts_proj_of(F, F->route[i].file) != host) continue;
    rs[nrs].r = &F->route[i];
    rs[nrs].end = UINT32_MAX;
    for (i32 k = i + 1; k < F->nroute; k++)
      if (F->route[k].file == F->route[i].file) {
        rs[nrs].end = F->route[k].off;
        break;
      }
    // the :param -> {param} conversion openapi uses; params collected for the args
    const char *p = F->route[i].path;
    size_t o = 0;
    rs[nrs].opath[0] = 0;
    rs[nrs].nparams = 0;
    while (*p && o < sizeof rs[nrs].opath - 1) {
      if (*p == ':' && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
        p++;
        size_t n = 0;
        char name[32] = {0};
        while ((isalnum((unsigned char)*p) || *p == '_') && n < 31) name[n++] = *p++;
        if (rs[nrs].nparams < 8) snprintf(rs[nrs].params[rs[nrs].nparams++], 32, "%s", name);
        o += (size_t)snprintf(rs[nrs].opath + o, sizeof rs[nrs].opath - o, "${%s}", name);
      } else {
        rs[nrs].opath[o++] = *p++;
      }
    }
    rs[nrs].opath[o] = 0;
    nrs++;
  }

  Buf b;
  buf_init(&b, a, 32 * 1024);
  buf_printf(&b,
             "// %s — generated by `webc client --ts`, from the routes and models the\n"
             "// sources declare. It is yours: edit the names, commit it; regenerate with\n"
             "// the command, or let `webc client --ts --check` fail when it drifted.\n"
             "// Field names keep their wire spelling — the client describes what the\n"
             "// server sends, not what a style guide wishes it sent.\n\n",
             out);
  buf_printf(&b, "const BASE = (globalThis.WEBC_API_BASE as string | undefined) ?? \"\";\n\n");
  buf_printf(&b,
             "async function req<T>(method: string, path: string, body?: unknown): Promise<T> {\n"
             "  const res = await fetch(BASE + path, {\n"
             "    method,\n"
             "    headers: body === undefined ? undefined : { \"content-type\": \"application/json\" },\n"
             "    body: body === undefined ? undefined : JSON.stringify(body),\n"
             "  });\n"
             "  if (!res.ok) {\n"
             "    const e = await res.json().catch(() => ({ message: res.statusText }));\n"
             "    throw new Error(\x60${method} ${path}: ${res.status} ${e.message ?? \"\"}`);\n"
             "  }\n"
             "  if (res.status === 204) return undefined as T;\n"
             "  return (await res.json()) as T;\n"
             "}\n\n");

  i32 nschemas = 0;
  for (i32 i = 0; i < F->nmod; i++) {
    const WMod *m = &F->mod[i];
    if (m->internal) continue;
    if (host >= 0 && facts_proj_of(F, m->file) != host) continue;
    ts_model_types(F, &b, m);
    nschemas++;
  }

  buf_printf(&b, "export const api = {\n");
  i32 nops = 0;
  char names[256][128];
  for (i32 m = 0; m < nrs; m++) {
    const WRoute *r = rs[m].r;
    char method[16];
    snprintf(method, sizeof method, "%s", r->method);
    for (char *s = method; *s; s++) *s = (char)tolower((unsigned char)*s);
    if (strcmp(method, "get") && strcmp(method, "post") && strcmp(method, "put") &&
        strcmp(method, "patch") && strcmp(method, "delete"))
      continue;

    const char *body_model = nullptr;
    const WCall *best = nullptr; // the first 2xx json response, openapi's pick too
    for (i32 ci = 0; ci < F->ncall; ci++) {
      const WCall *cl = &F->call[ci];
      if (cl->file != r->file || cl->call_off < r->off || cl->call_off >= rs[m].end)
        continue;
      if ((!strcmp(cl->macro, "body") || !strcmp(cl->macro, "body_try") ||
           !strcmp(cl->macro, "body_json") || !strcmp(cl->macro, "body_form")) &&
          cl->name[0])
        body_model = cl->name;
      if ((!strcmp(cl->macro, "json") || !strcmp(cl->macro, "json_list")) && cl->status &&
          cl->status >= 200 && cl->status < 300 && !best)
        best = cl;
    }
    // pages are not the API: a GET whose only answer is html stays out of the client.
    // Everything else is callable — a form action returns void, a json route its shape.
    if (!best && !body_model && !strcmp(method, "get")) continue;

    // the verb: from the method (a GET that answers a list reads as `list`), unless
    // the path itself names the act (`/delete`)
    const char *verb = "get";
    if (!strcmp(method, "post")) verb = "create";
    if (!strcmp(method, "put") || !strcmp(method, "patch")) verb = "update";
    if (!strcmp(method, "delete")) verb = "delete";
    if (!strcmp(method, "get") && best && best->list) verb = "list";
    bool action = false; // a POST that is not a create: the path says what it is
    {
      const char *pp = r->path;
      while ((pp = strstr(pp, "/"))) {
        pp++;
        if (!strncmp(pp, "delete", 6) || !strncmp(pp, "remove", 6) ||
            !strncmp(pp, "toggle", 6) || !strncmp(pp, "save", 4)) {
          action = true;
          break;
        }
      }
    }
    char pname[128];
    ts_path_name(F, r->path, pname, sizeof pname, rs[m].params, rs[m].nparams);
    // drop the act's own segment from the name: deleteApiNotesDelete -> deleteNote
    if (action) {
      struct {
        const char *seg, *verb;
      } acts[] = {{"delete", "delete"}, {"remove", "delete"}, {"toggle", "toggle"},
                  {"save", "save"}};
      for (size_t k = 0; k < WC_COUNTOF(acts); k++)
        if (!strcmp(verb, acts[k].verb) || action) {
          char tail[64];
          snprintf(tail, sizeof tail, "%s", acts[k].seg);
          tail[0] = (char)toupper((unsigned char)tail[0]);
          size_t tl = strlen(tail);
          size_t pl = strlen(pname);
          if (pl > tl && !strcmp(pname + pl - tl, tail)) pname[pl - tl] = 0;
          if (!strcmp(verb, "create")) verb = acts[k].verb;
        }
    }
    if (!pname[0]) snprintf(pname, sizeof pname, "Root");
    char fname[160];
    snprintf(fname, sizeof fname, "%s%s", verb, pname);
    for (i32 k = 0; k < nops; k++)
      if (!strcmp(names[k], fname)) {
        snprintf(fname, sizeof fname, "%s%s_%d", verb, pname, nops);
        break;
      }
    snprintf(names[nops], 128, "%s", fname);

    // args: the path's placeholders, then the body
    char args[512] = "";
    for (i32 pi = 0; pi < rs[m].nparams; pi++) {
      const char *ptype = "string";
      for (i32 ci = 0; ci < F->ncall; ci++) {
        const WCall *cl = &F->call[ci];
        if (cl->file != r->file || cl->call_off < r->off || cl->call_off >= rs[m].end)
          continue;
        if (!strcmp(cl->macro, "path-i64") && cl->name &&
            !strcmp(cl->name, rs[m].params[pi]))
          ptype = "number";
      }
      char one[80];
      snprintf(one, sizeof one, "%s%s: %s, ", rs[m].params[pi], "", ptype);
      strncat(args, one, sizeof args - strlen(args) - 1);
    }
    if (body_model && oa_is_model(F, body_model)) {
      char one[128];
      snprintf(one, sizeof one, "body: %sInput, ", body_model);
      strncat(args, one, sizeof args - strlen(args) - 1);
    }
    // trailing ", " -> ""
    size_t al = strlen(args);
    if (al >= 2 && args[al - 2] == ',' && args[al - 1] == ' ') args[al - 2] = 0;

    char ret[128] = "void";
    if (best) {
      if (best->list)
        snprintf(ret, sizeof ret, "%s[]", (best->name[0] && oa_is_model(F, best->name))
                                                ? best->name : "unknown");
      else if (best->name[0] && oa_is_model(F, best->name))
        snprintf(ret, sizeof ret, "%s", best->name);
      else
        snprintf(ret, sizeof ret, "unknown");
    }
    buf_printf(&b, "  // %s %s\n", r->method, r->path);
    buf_printf(&b, "  %s: (%s) => req<%s>(\"%s\", \x60%s\x60%s),\n", fname, args, ret,
               r->method, rs[m].opath, body_model && oa_is_model(F, body_model) ? ", body" : "");
    nops++;
  }
  buf_printf(&b, "};\n");

  str doc = {.p = buf_cstr(&b), .n = b.n};
  if (check) {
    str on_disk = read_file(a, out);
    if (on_disk.n && on_disk.p[on_disk.n - 1] == '\n') on_disk.n--;
    if (!on_disk.p || !str_eq(on_disk, doc)) {
      fprintf(stderr, "webc: %s is stale — run `webc client --ts --out %s` and commit it\n",
              out, out);
      arena_destroy(a);
      return 1;
    }
    info("%s: current (%d operations, %d models)", out, nops, nschemas);
    arena_destroy(a);
    return 0;
  }
  char *body = arena_alloc(a, doc.n + 2);
  memcpy(body, doc.p, (size_t)doc.n);
  body[doc.n] = '\n';
  body[doc.n + 1] = 0;
  write_file(out, body);
  info("%s: %d operations, %d models", out, nops, nschemas);
  arena_destroy(a);
  return 0;
}

// ---------------------------------------------------------------- learn
//
// `webc learn` — five lessons, one idea each, verified by the framework's own
// gates. A lesson is a real app in `.webc/learn/<slug>/` whose test (or, for the
// wiring one, `webc check`) fails until the learner makes the one edit the brief
// names — and the referee is never a second checker the CLI owns: it is
// `webc test` / `webc check`, the same commands a real app answers to.
//
// The lessons live in `tools/learn/` in the framework tree, as apps: they build,
// they fail for the reason the brief says, and they pass after the edit — which
// is what `tests/learn_test.js` asserts, lesson by lesson.
typedef struct Lesson {
  const char *slug, *title, *brief, *gate; // gate: "test" or "check"
} Lesson;

static const Lesson LESSONS[] = {
    {"01-the-model-is-the-schema", "the model is the schema",
     "the test names a field the model does not have. Add it where the compiler\n"
     "says — and the column that follows it, in migrations/.",
     "test"},
    {"02-a-response-is-a-value", "a response is a value",
     "the handler answers the same words to everybody. Read the query string\n"
     "(`query_text`) and build the answer with `fmt` — a handler returns, it\n"
     "never writes to a socket.",
     "test"},
    {"03-one-view-both-halves", "one view, both halves",
     "the route fills the view's context by field name; the view draws the list\n"
     "but not the note. Draw the note: one element, one class, one text.",
     "test"},
    {"04-views-name-clients-bind", "views name, clients bind",
     "the view names an element; the client half binds a behaviour to a name.\n"
     "One of the two names is wrong and `webc check` knows which — make them\n"
     "agree. (Needs wasi-sdk: `webc doctor` says whether it is there.)",
     "check"},
    {"05-validation-is-the-declaration", "validation is the declaration",
     "the signup accepts an empty email and the test knows it should not. There\n"
     "is no validation layer to write: say the rule once, on the field.",
     "test"},
};

static int cmd_learn(i32 argc, char **argv) {
  i32 nlessons = (i32)(sizeof LESSONS / sizeof LESSONS[0]);
  if (argc >= 3) {
    i32 n = atoi(argv[2]);
    if (n < 1 || n > nlessons) die("lessons are 1..%d — `webc learn` lists them", nlessons);
    const Lesson *l = &LESSONS[n - 1];
    char from[1024], to[1024];
    snprintf(from, sizeof from, "%s/tools/learn/%s", webc_prefix(), l->slug);
    snprintf(to, sizeof to, ".webc/learn/%s", l->slug);
    if (path_exists(to))
      die("%s already exists — delete it to start over (the fix you made goes with "
          "it)", to);
    copy_tree(from, to, "learn");
    info("wrote %s", to);
    printf("\n  lesson %d — %s\n\n  %s\n\n", n, l->title, l->brief);
    printf("  cd %s\n  webc %s      the referee; webc dev to see it\n", to, l->gate);
    return 0;
  }
  if (argc != 2) die("usage: webc learn [1..%d]", nlessons);

  printf("webc learn — five lessons, one idea each\n\n");
  i32 started = 0, passed = 0;
  for (i32 i = 0; i < nlessons; i++) {
    const Lesson *l = &LESSONS[i];
    char to[1024];
    snprintf(to, sizeof to, ".webc/learn/%s", l->slug);
    if (!path_exists(to)) {
      printf("  %d  %-34s · not started\n", i + 1, l->title);
      continue;
    }
    started++;
    // the referee is the gate the lesson names — the same commands a real app
    // answers to, run quietly: the number that matters is its exit code
    char cmd[2200];
    snprintf(cmd, sizeof cmd, "cd '%s' && '%s' %s >/dev/null 2>&1", to, g_self, l->gate);
    bool ok = system(cmd) == 0;
    if (ok) passed++;
    printf("  %d  %-34s %s\n", i + 1, l->title, ok ? "\u2713 passed" : "\u2717 failing");
  }
  printf("\n  %d of %d passed; %d started\n", passed, nlessons, started);
  i32 next = -1;
  for (i32 i = 0; i < nlessons && next < 0; i++) {
    char to2[1024];
    snprintf(to2, sizeof to2, ".webc/learn/%s", LESSONS[i].slug);
    if (!path_exists(to2)) next = i + 1;
  }
  if (next > 0)
    printf("  start the next: webc learn %d   (writes .webc/learn/%s)\n", next,
           LESSONS[next - 1].slug);
  else if (passed < nlessons)
    printf("  a failing lesson names its mistake — cd into it and run its gate\n");
  else
    printf("  all five — the mental model is yours. `webc new` and build something.\n");
  return 0;
}

static bool scaffold_register_source(const char *path_in);

// ---------------------------------------------------------------- model from-db
//
// `webc model from-db` — the reverse of `webc migrate diff`: point it at a
// database that already exists and it writes the `WEBC_MODEL` declarations the
// tables describe, plus (with `--write`) a baseline migration and the two files
// that link them. Nobody greenfields; the evaluation that ends in "rewrite our
// admin by hand" is the one this deletes — the models a team gets on minute one,
// from the database they already have.
//
// The output is a file the app owns, not a connection: generated once, edited
// freely, diffed in review. sqlite's baseline carries the database's own DDL
// verbatim (sqlite_master has it); postgres and MySQL cannot hand back the
// source DDL, so `--write` says the one command that finishes the job — the
// models are linked by then, and `webc migrate diff --write` writes what they
// say the baseline should.

typedef struct DbCol {
  char name[80], type[80];
  bool notnull, pk;
  char dflt[80];
} DbCol;

typedef struct DbTable {
  char name[80];
  DbCol col[64];
  i32 ncol;
  char ddl[4096]; // sqlite: the database's own words; empty when the driver cannot
} DbTable;

// `todos` -> `Todo`, `media_assets` -> `MediaAsset`: title-case the segments,
// strip the plural from the last one when it is a plain 's'. A guess the header
// says to edit — table names lie sometimes, and the model is the app's now.
static void fromdb_model_name(const char *table, char *out, size_t cap) {
  size_t o = 0;
  const char *p = table;
  while (*p && o < cap - 1) {
    while (*p == '_') p++;
    if (!*p) break;
    out[o++] = (char)toupper((unsigned char)*p);
    p++;
    while (*p && *p != '_' && o < cap - 1) out[o++] = *p++;
  }
  if (o > 1 && out[o - 1] == 's' && out[o - 2] != 's') out[--o] = 0;
  out[o] = 0;
}

// one column -> one field tuple, in the model's own vocabulary
static void fromdb_field(const DbCol *c, char *out, size_t cap) {
  char low[80];
  snprintf(low, sizeof low, "%s", c->type);
  for (char *s = low; *s; s++) *s = (char)tolower((unsigned char)*s);
  const char *kind = "Text";
  const char *extra = "";
  char maxq[40] = "";
  char note[120] = "";
  i64 vlen = 0;
  char *paren = strchr(low, '(');
  if (paren) {
    vlen = strtoll(paren + 1, nullptr, 10);
    *paren = 0;
  }
  if (c->pk) {
    kind = "i64";
    extra = ", .pk = true";
  } else if (strstr(low, "int")) {
    kind = "i64";
    extra = c->notnull ? "" : ", .null = true";
  } else if (strstr(low, "bool")) {
    kind = "bool";
  } else if (strstr(low, "real") || strstr(low, "double") || strstr(low, "float") ||
             strstr(low, "numeric")) {
    kind = "f64";
  } else if (strstr(low, "blob")) {
    kind = "Bytes";
  } else if (strstr(low, "timestamp") || strstr(low, "datetime") || strstr(low, "date")) {
    kind = "Text";
    snprintf(note, sizeof note, "   // a date, kept as ISO text");
  } else {
    kind = "Text";
    if (vlen > 0) snprintf(maxq, sizeof maxq, ", .max = %lld", (long long)vlen);
    if (c->notnull && !c->dflt[0]) extra = ", .required = true";
    if (note[0] == 0 && !strstr(low, "text") && !strstr(low, "char") && low[0])
      snprintf(note, sizeof note, "   // type '%s', mapped as text — edit freely", c->type);
  }
  snprintf(out, cap, "(%s, %s%s%s)%s", c->name, kind, maxq, extra, note);
}

static void fromdb_emit(WDB *db, bool is_sqlite, const char *only, bool write,
                        const char *out_path) {
  Arena *a = arena_new(1 << 20);
  DbTable tabs[64];
  i32 ntab = 0;

  if (is_sqlite) {
    WRes r = db_exec_sql(db, "select name, sql from sqlite_master where type='table' "
                             "and name not like 'sqlite_%' and name not like '_wc%' "
                             "order by name");
    if (!r.ok) {
      fprintf(stderr, "webc: could not read the schema: %s\n", r.e.buf);
      return;
    }
    for (i32 i = 0; i < r.nrows && ntab < 64; i++) {
      const char *nm = row_get(&r.rows[i], "name").s.p ? "name" : nullptr;
      (void)nm;
      str name = row_get(&r.rows[i], "name").s;
      if (!name.p || !name.n) continue;
      if (only && strcmp(only, name.p) != 0) continue;
      DbTable *t = &tabs[ntab++];
      snprintf(t->name, sizeof t->name, "%.*s", (int)name.n, name.p);
      str sql = row_get(&r.rows[i], "sql").s;
      if (sql.p) snprintf(t->ddl, sizeof t->ddl, "%.*s", (int)sql.n, sql.p);
      t->ncol = 0;
      char pragma[256];
      snprintf(pragma, sizeof pragma, "pragma table_info('%s')", t->name);
      WRes ti = db_exec_sql(db, pragma);
      for (i32 k = 0; k < ti.nrows && t->ncol < 64; k++) {
        DbCol *c = &t->col[t->ncol++];
        snprintf(c->name, sizeof c->name, "%s", row_get(&ti.rows[k], "name").s.p);
        snprintf(c->type, sizeof c->type, "%s", row_get(&ti.rows[k], "type").s.p);
        c->notnull = row_i64(&ti.rows[k], "notnull") != 0;
        c->pk = row_i64(&ti.rows[k], "pk") > 0;
        str d = row_get(&ti.rows[k], "dflt_value").s;
        snprintf(c->dflt, sizeof c->dflt, "%s", d.p ? d.p : "");
      }
      wres_free(&ti);
    }
    wres_free(&r);
  } else {
    // postgres and MySQL: information_schema carries the columns and the nullability;
    // the primary key is the auto-increment column (MySQL) or a constraint join away
    // (postgres) — best effort, marked in the header, because the output is a file
    // the app edits anyway
    const char *q =
        "select table_name, column_name, data_type,"
        "       coalesce(character_maximum_length, 0) as maxlen, is_nullable"
        "  from information_schema.columns"
        " where table_schema = 'public' or table_schema = database()"
        " order by table_name, ordinal_position";
    WRes r = db_exec_sql(db, q);
    if (r.ok) {
      for (i32 i = 0; i < r.nrows; i++) {
        str tn = row_get(&r.rows[i], "table_name").s;
        if (!tn.p || !tn.n) continue;
        if (only && strcmp(only, tn.p) != 0) continue;
        DbTable *t = nullptr;
        for (i32 k = 0; k < ntab; k++)
          if (!strcmp(tabs[k].name, tn.p)) t = &tabs[k];
        if (!t) {
          if (ntab >= 64) break;
          t = &tabs[ntab++];
          snprintf(t->name, sizeof t->name, "%s", tn.p);
          t->ncol = 0;
          t->ddl[0] = 0;
        }
        if (t->ncol >= 64) continue;
        DbCol *c = &t->col[t->ncol++];
        snprintf(c->name, sizeof c->name, "%s", row_get(&r.rows[i], "column_name").s.p);
        char dt[80];
        snprintf(dt, sizeof dt, "%s", row_get(&r.rows[i], "data_type").s.p);
        i64 maxlen = row_i64(&r.rows[i], "maxlen");
        if (maxlen > 0) snprintf(c->type, sizeof c->type, "%s(%lld)", dt, (long long)maxlen);
        else
          snprintf(c->type, sizeof c->type, "%s", dt);
        c->notnull = row_str(&r.rows[i], "is_nullable").n == 2 &&
                     row_str(&r.rows[i], "is_nullable").p[0] == 'N';
        c->pk = false;
        c->dflt[0] = 0;
      }
      wres_free(&r);
    }
  }
  if (ntab == 0) die("the database has no tables to describe (or the name is wrong)");

  Buf h;
  buf_init(&h, a, 16 * 1024);
  buf_puts(&h,
           "// generated by `webc model from-db` — the database's own description of\n"
           "// its tables, as WebC models. This file is the app's: rename a model, fix\n"
           "// a field, add a qualifier; regenerate only on purpose.\n");
  if (!is_sqlite)
    buf_puts(&h,
             "//\n// postgres/MySQL note: the primary key is the auto-increment column where\n"
             "// the driver could see it — check each model's first field before trusting it.\n");
  buf_puts(&h, "#include <webc/shared.h>\n\n");
  for (i32 i = 0; i < ntab; i++) {
    DbTable *t = &tabs[i];
    char model[80];
    fromdb_model_name(t->name, model, sizeof model);
    buf_printf(&h, "WEBC_MODEL(%s,\n", model);
    for (i32 k = 0; k < t->ncol; k++) {
      char field[256];
      fromdb_field(&t->col[k], field, sizeof field);
      buf_printf(&h, "           %s%s\n", field, k + 1 < t->ncol ? "," : ")");
    }
    buf_printf(&h, "\nVec(%s)\n\n", model);
  }

  if (!write) {
    printf("%s", buf_cstr(&h));
    arena_destroy(a);
    return;
  }
  char path[1024];
  snprintf(path, sizeof path, "%s", out_path);
  write_file(path, buf_cstr(&h));
  info("wrote %s (%d models)", path, ntab);

  // models.c: one line that links the models' constructors into the app
  char cpath[1024];
  snprintf(cpath, sizeof cpath, "%s", "src/models.c");
  write_file(cpath, "// generated by `webc model from-db` — including the header links the\n"
                    "// models' constructors, which is what `webc check`'s schema step reads\n"
                    "#include \"models.h\"\n");
  scaffold_register_source("src/models.c");

  // the baseline: sqlite speaks its own DDL; the others get the one command that
  // finishes the job, now that the models are linked
  if (is_sqlite && tabs[0].ddl[0]) {
    Buf b;
    buf_init(&b, a, 16 * 1024);
    buf_puts(&b, "-- baseline: the schema as the database has it (webc model from-db)\n");
    for (i32 i = 0; i < ntab; i++) {
      if (!tabs[i].ddl[0]) continue;
      // the baseline may be applied to the very database it came from: sqlite's own
      // DDL does not carry IF NOT EXISTS, and a re-run against the source db is the
      // first thing a migration-by-migration copy does
      const char *ddl = tabs[i].ddl;
      char patched[4200];
      const char *ct = strstr(ddl, "CREATE TABLE ");
      if (ct && !strstr(ddl, "IF NOT EXISTS")) {
        size_t pre = (size_t)(ct - ddl);
        snprintf(patched, sizeof patched, "%.*sCREATE TABLE IF NOT EXISTS %s",
                 (int)pre, ddl, ct + 13);
      } else {
        snprintf(patched, sizeof patched, "%s", ddl);
      }
      buf_printf(&b, "%s;\n", patched);
    }
    char mpath[1024];
    snprintf(mpath, sizeof mpath, "migrations/0001_baseline.sql");
    if (!path_exists("migrations/0001_baseline.sql")) {
      write_file(mpath, buf_cstr(&b));
      info("wrote %s (the database's own DDL)", mpath);
    }
  } else {
    printf("  next: webc migrate diff --write baseline\n"
           "        (the models are linked now; the diff writes what they say the "
           "baseline is)\n");
  }
  arena_destroy(a);
}

static int cmd_model(i32 argc, char **argv) {
  if (argc < 3 || strcmp(argv[2], "from-db") != 0)
    die("usage: webc model from-db [--database URL] [--table NAME] [--write] [--out F]");
  Config c;
  config_load(&c, ".");
  const char *url = nullptr;
  const char *only = nullptr;
  const char *out = "src/models.h";
  bool write = false;
  for (i32 i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--database") && i + 1 < argc)
      url = argv[++i];
    else if (!strcmp(argv[i], "--table") && i + 1 < argc)
      only = argv[++i];
    else if (!strcmp(argv[i], "--out") && i + 1 < argc)
      out = argv[++i];
    else if (!strcmp(argv[i], "--write"))
      write = true;
    else
      die("unknown model flag %s (--database, --table, --out, --write)", argv[i]);
  }
  if (!url) {
    if (!path_exists("webc.json"))
      die("which database? --database URL, or run inside a project whose webc.json names one");
    url = c.database;
  }
  bool is_sqlite = strncmp(url, "sqlite:", 7) == 0;
  // the drivers speak the wire protocols themselves (no libpq, no libmysqlclient)
  WDB *db = db_open(url);
  if (!db) die("could not open %s", url);
  fromdb_emit(db, is_sqlite, only, write, out);
  return 0;
}

// ---------------------------------------------------------------- get
//
// `webc get URL` — the client in the framework, used by the toolchain that ships it
// (SPEC §9.1: the CLI dogfoods its own HTTP client). Curl-shaped, no flags beyond
// the method and a body, because its job is to check a health endpoint from CI.
static int cmd_get(int argc, char **argv) {
  const char *method = "GET";
  const char *url = nullptr;
  const char *body = nullptr;
  for (i32 i = 2; i < argc; i++) {
    if (strcmp(argv[i], "-X") == 0 && i + 1 < argc)
      method = argv[++i];
    else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
      body = argv[++i];
    else if (!url)
      url = argv[i];
  }
  if (!url) die("usage: webc get [-X METHOD] [-d BODY] URL");
  Arena *a = arena_new(1 << 16);
  WRespC r = {0};
  WErrRes e = http_do_follow(a, (WClient){.method = method,
                                          .url = url,
                                          .body = {(char *)body, body ? strlen(body) : 0},
                                          .content_type = body ? "application/json" : nullptr},
                             5, &r);
  if (!e.ok) {
    fprintf(stderr, "webc: %s\n", e.e.buf);
    arena_destroy(a);
    return 1;
  }
  printf("%d\n", r.status);
  if (r.body.n) fwrite(r.body.p, 1, r.body.n, stdout);
  if (r.body.n && r.body.p[r.body.n - 1] != '\n') printf("\n");
  arena_destroy(a);
  return r.status >= 400 ? 2 : 0;
}

// ---------------------------------------------------------------- macros

static char g_wasi_dir[1024];
// The wasm compiler, or null when the toolchain is missing (then `--macros` reports
// the target as skipped rather than as a failure: a missing SDK is `webc doctor`'s
// complaint, not a broken macro).
static const char *wasi_cc(void) {
  static char path[1200];
  if (!g_wasi_dir[0] && !find_wasi(g_wasi_dir, sizeof g_wasi_dir)) return nullptr;
  snprintf(path, sizeof path, "%s/bin/clang", g_wasi_dir);
  return path_exists(path) ? path : nullptr;
}

static const char *wasi_root(void) { return g_wasi_dir; }

// read a whole file onto the heap (the macro log): the caller frees it
static char *read_all(const char *path, i64 *n_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) {
    fclose(f);
    return nullptr;
  }
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return nullptr;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = '\0';
  fclose(f);
  if (n_out) *n_out = (i64)got;
  return buf;
}
//
// `webc test --macros` (SPEC §5.1 rule 7): every public DSL construct, once, in a
// file the toolchain generates and compiles for both targets. A macro is a promise
// about expansion, and this is the cheapest way to keep that promise honest: if a
// construct stops expanding, the compiler's own line/column points at the marker
// comment above it, which names the construct.

typedef struct MacroLine {
  i32 line;
  const char *name;
} MacroLine;

static const char *SMOKE_SRC =
    "// generated by `webc test --macros` — do not edit\n"
    "#include <webc/server.h>\n"
    "#include <webc/web.h>\n"
    "\n"
    "//#model\n"
    "WEBC_MODEL(Smoke, (id, i64, .pk = true), (name, str, .required = true, .max = 8),\n"
    "           (ok, bool, .not_null = false), (score, f64))\n"
    "//#vec\n"
    "WEBC_VEC_DEFINE(VecSmoke, vec_smoke, Smoke)\n"
    "//#signal\n"
    "state(i64, count, 0);\n"
    "state(Text, draft, \"\");\n"
    "//#computed\n"
    "computed(i64, doubled) { return get(count) * 2; }\n"
    "//#effect\n"
    "effect(sync) { (void)get(doubled); }\n"
    "//#provide\n"
    "WPROV_KEY(SmokeTheme);\n"
    "\n"
    "static VecSmoke smoke_rows;\n"
    "static void smoke_click(void *ctx, const char *ev);\n"
    "\n"
    "static void smoke_view(WBuild *b, void *ctx) {\n"
    "  (void)b;\n"
    "  (void)ctx;\n"
    "  //#el\n"
    "  el(\"main\") {\n"
    "    //#class\n"
    "    class_(\"page\");\n"
    "    //#attr\n"
    "    attr(\"role\", \"main\");\n"
    "    //#text\n"
    "    text(\"count: %lld\", (long long)get(count));\n"
    "    //#text_s\n"
    "    text(get(draft));\n"
    "    //#on\n"
    "    el(\"button\") { on(\"click\", smoke_click, nullptr); text(\"go\"); }\n"
    "    //#for_each\n"
    "    el(\"ul\") {\n"
    "      for_each(row, smoke_rows, row->id) {\n"
    "        el(\"li\") { text(\"%lld\", (long long)row->id); }\n"
    "      }\n"
    "    }\n"
    "    //#portal\n"
    "    portal(\"#toast\") { el(\"p\") { text(\"hi\"); } }\n"
    "  }\n"
    "}\n"
    "\n"
    "static void smoke_click(void *ctx, const char *ev) {\n"
    "  (void)ctx;\n"
    "  (void)ev;\n"
    "  //#batch\n"
    "  batch { set(count, get(count) + 1); }\n"
    "  //#provide_use\n"
    "  WProv *p = provide(wprov_current(), &SmokeTheme, (void *)1);\n"
    "  (void)use_ctx_or(p, &SmokeTheme, nullptr);\n"
    "}\n"
    "\n"
    "//#route\n"
    "GET(\"/smoke\") { return res_text(200, str_lit(\"ok\")); }\n"
    "POST(\"/smoke\") {\n"
    "  //#try_body\n"
    "  TRY_BODY(Smoke, in);\n"
    "  return json(200, Smoke, &in);\n"
    "}\n"
    "PUT(\"/smoke/:id\") { return res_empty(204); }\n"
    "DELETE(\"/smoke/:id\") { return res_empty(204); }\n"
    "//#route_mw\n"
    "GET_MW(\"/smoke/guarded\", mw_headers_default()) { return res_text(200, str_lit(\"ok\")); }\n"
    "//#body_json\n"
    "POST(\"/smoke/json\") {\n"
    "  Smoke s = body_json(Smoke);\n"
    "  return res_text(200, str_fmt(req->arena, \"%lld\", (long long)s.id));\n"
    "}\n"
    "//#body_form\n"
    "POST(\"/smoke/form\") {\n"
    "  Smoke s = body_form(Smoke);\n"
    "  return res_text(200, str_fmt(req->arena, \"%lld\", (long long)s.id));\n"
    "}\n"
    "//#use\n"
    "static void smoke_wire(void) { use(mw_recover()); use(mw_request_id()); }\n"
    "\n"
    "//#sync_types\n"
    "static void smoke_sync(void) {\n"
    "  Mutex m = WEBC_MUTEX_INIT;\n"
    "  mutex_lock(&m);\n"
    "  mutex_unlock(&m);\n"
    "  WaitGroup wg;\n"
    "  waitgroup_init(&wg);\n"
    "  waitgroup_done(&wg);\n"
    "}\n"
    "//#chan\n"
    "static void smoke_chan(void) {\n"
    "  Chan(i64) c = chan_new(i64, 4);\n"
    "  chan_send(c, 1);\n"
    "  i64 v = 0;\n"
    "  if (!chan_recv(c, &v)) v = -1;\n"
    "  chan_close(c);\n"
    "  (void)v;\n"
    "}\n"
    "\n"
    "int smoke_all(void) {\n"
    "  (void)smoke_wire;\n"
    "  (void)smoke_sync;\n"
    "  (void)smoke_chan;\n"
    "  (void)smoke_view;\n"
    "  return 0;\n"
    "}\n";

// Marker names, in file order. `--macros` reports one verdict per construct, so a
// broken macro is a named line rather than "somewhere in the DSL".
[[maybe_unused]] static const char *SMOKE_NAMES[] = {
    "WEBC_MODEL",   "WEBC_VEC_DEFINE", "signal/signal_str", "computed",
    "effect",       "WPROV_KEY",       "el",                "class_",
    "attr_c",       "text",            "text_s",            "on",
    "for_each",     "portal",          "batch",             "provide/use_ctx_or",
    "GET/POST",     "TRY_BODY",        "PUT/DELETE",        "GET_MW",
    "body_json",    "body_form",       "use(mw)",           "Mutex/WaitGroup",
    "Chan(T)",
};

// Which construct a compiler error line falls in: the nearest marker above it.
static const char *smoke_name_at(const char *src, i32 line) {
  const char *best = "header";
  i32 best_line = 0;
  i32 cur = 1;
  const char *p = src;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t n = eol ? (size_t)(eol - p) : strlen(p);
    if (cur > line) break;
    if (n > 3 && strncmp(p, "//#", 3) == 0) {
      static char names[64][48];
      static i32 used;
      size_t k = n - 3 < 47 ? n - 3 : 47;
      if (used < 64) {
        memcpy(names[used], p + 3, k);
        names[used][k] = '\0';
        // map the file's own marker text to the friendly name when we can
        best = names[used];
        best_line = cur;
        used++;
      }
    }
    (void)best_line;
    if (!eol) break;
    p = eol + 1;
    cur++;
  }
  return best;
}

static int cmd_macros(void) {
  char dir[1024];
  snprintf(dir, sizeof dir, ".webc");
  mkdirs(dir);
  const char *src_path = ".webc/macros.c";
  write_file(src_path, SMOKE_SRC);
  char inc[1024];
  snprintf(inc, sizeof inc, "%s/include", webc_prefix());

  printf("\033[1m== macros\033[0m\n");
  // Both targets: the DSL has to expand the same way for the server and for wasm,
  // and a construct that only compiles on one of them is exactly the bug this finds.
  struct {
    const char *target;
    const char *cc;
    const char *extra;
  } builds[2] = {
      {"server", g_cc, "-D_DARWIN_C_SOURCE=1 -D_GNU_SOURCE=1"},
      {"wasm", wasi_cc(), "--target=wasm32-wasip1"},
  };
  i32 failures = 0;
  for (size_t i = 0; i < WC_COUNTOF(builds); i++) {
    if (!builds[i].cc) {
      printf("  --   %-22s (no compiler)\n", builds[i].target);
      continue;
    }
    Buf cmd;
    buf_init(&cmd, nullptr, 2048);
    if (strcmp(builds[i].target, "wasm") == 0)
      buf_printf(&cmd, "%s --target=wasm32-wasip1 --sysroot=%s/share/wasi-sysroot "
                       "-std=c23 -fsyntax-only -DWEBC_NO_TASKS=1 -I %s %s",
                 builds[i].cc, wasi_root(), inc, src_path);
    else
      buf_printf(&cmd, "%s -std=c23 -fsyntax-only %s -I %s %s", builds[i].cc, builds[i].extra, inc,
                 src_path);
    // capture the compiler's own words: they carry the line number
    Buf log;
    buf_init(&log, nullptr, 4096);
    char logpath[1024];
    snprintf(logpath, sizeof logpath, ".webc/macros-%s.log", builds[i].target);
    buf_printf(&cmd, " > %s 2>&1", logpath);
    bool ok = run_cmd(buf_cstr(&cmd), false);
    buf_free(&cmd);
    if (ok) {
      printf("  ok   %-22s %d constructs\n", builds[i].target,
             (i32)WC_COUNTOF(SMOKE_NAMES));
      buf_free(&log);
      continue;
    }
    failures++;
    // read the log back and name the construct for every error
    i64 n = 0;
    char *text = read_all(logpath, &n);
    printf("  FAIL %-22s %s\n", builds[i].target, logpath);
    if (text) {
      const char *p = text;
      while ((p = strstr(p, "macros.c:"))) {
        p += 9;
        i32 line = atoi(p);
        const char *name = smoke_name_at(SMOKE_SRC, line);
        printf("       %s (macros.c:%d)\n", name, line);
        const char *eol = strchr(p, '\n');
        if (!eol) break;
        p = eol + 1;
      }
      free(text);
    }
    buf_free(&log);
  }
  if (failures == 0) info("macros: all constructs expand on both targets");
  // The assertions are half of what a test file can say, and nothing listed them: nobody
  // used ASSERT_JSON for a month because nobody knew it existed.
  printf("\n\033[1m== assertions\033[0m (the other half of the DSL)\n");
  static const char *asserts[][2] = {
      {"ASSERT(cond)", "a boolean, with the expression printed when it fails"},
      {"ASSERT_EQ(a, b)", "two integers"},
      {"ASSERT_STR_EQ(a, b)", "two strings — a str, a Text or a literal"},
      {"ASSERT_STR_CONTAINS(hay, needle)", "a substring"},
      {"ASSERT_TRUE / ASSERT_FALSE(c)", "readability, same check"},
      {"ASSERT_NULL / ASSERT_NOT_NULL(p)", "a pointer"},
      {"ASSERT_PANICS(expr)", "a failure that must happen, and must not take the suite down"},
      {"ASSERT_JSON(res, path, want)", "a JSON body by path: \"0.slug\", \"items.1.id\""},
      {"ASSERT_JSON_COUNT(res, path)", "how many elements a JSON array has"},
      {"ASSERT_HTML(res, selector)", "an element the page must have"},
      {"ASSERT_HTML_ABSENT(res, selector)", "an element the page must not have"},
      {"ASSERT_HTML_TEXT(res, selector, text)", "what an element says"},
      {"ASSERT_HEADER(res, name, value)", "a response header"},
      {"ASSERT_STATUS(res, code)", "the status, named"},
      {"ASSERT_BODY_LEN(res, min)", "a body of at least this many bytes"},
  };
  for (size_t i = 0; i < WC_COUNTOF(asserts); i++)
    printf("  %-38s %s\n", asserts[i][0], asserts[i][1]);
  printf("\n  webc test --client replays the client half's batches through the real bridge.\n");
  return failures ? 1 : 0;
}

// ---------------------------------------------------------------- migrate

static int run_boot_hook_capture(const char *env_name, const char *value, Buf *out);

static int cmd_migrate(int argc, char **argv) {
  Config c;
  config_load(&c, ".");
  const char *sub = argc > 0 ? argv[0] : "up";
  if (strcmp(sub, "new") == 0) {
    if (argc < 2) die("usage: webc migrate new <name>");
    DIR *d = opendir(c.migrations);
    i32 n = 1;
    struct dirent *e;
    while (d && (e = readdir(d))) {
      if (e->d_name[0] >= '0' && e->d_name[0] <= '9') n++;
    }
    if (d) closedir(d);
    char p[1024];
    snprintf(p, sizeof p, "%s/%04d_%s.sql", c.migrations, n, argv[1]);
    write_file(p, "-- migration: write your SQL here\n");
    info("created %s", p);
    return 0;
  }
  if (strcmp(sub, "diff") == 0) {
    bool write = false;
    const char *name = "schema";
    for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--write") == 0) {
        write = true;
      } else if (argv[i][0] != '-') {
        name = argv[i];
      } else {
        die("usage: webc migrate diff [--write] [name]");
      }
    }
    Buf plan;
    buf_init(&plan, nullptr, 4096);
    if (run_boot_hook_capture("WEBC_SCHEMA_DIFF", nullptr, &plan) != 0) {
      buf_free(&plan);
      return 1;
    }
    // The app's own boot runs first — it applies the migrations it has, which is what makes
    // this comparison meaningful — and it is free to print. Only the statements are ours, so
    // only the statements are kept: the file this writes must be SQL, not a transcript.
    Buf stmts;
    buf_init(&stmts, nullptr, 4096);
    i32 n_stmts = 0;
    for (const char *line = buf_cstr(&plan); line && *line;) {
      const char *eol = strchr(line, '\n');
      size_t len = eol ? (size_t)(eol - line) : strlen(line);
      char low[32];
      size_t ln = len < sizeof low - 1 ? len : sizeof low - 1;
      for (size_t k = 0; k < ln; k++)
        low[k] = (char)(line[k] >= 'A' && line[k] <= 'Z' ? line[k] + 32 : line[k]);
      low[ln] = 0;
      bool keep = len == 0 || (len >= 2 && line[0] == '-' && line[1] == '-') ||
                  strncmp(low, "alter table", 11) == 0 || strncmp(low, "create table", 12) == 0 ||
                  strncmp(low, "create index", 12) == 0 ||
                  strncmp(low, "create unique index", 19) == 0;
      if (keep) {
        if (len && !(line[0] == '-' && len > 1 && line[1] == '-')) n_stmts++;
        buf_write(&stmts, line, len);
        buf_write(&stmts, "\n", 1);
      }
      line = eol ? eol + 1 : nullptr;
    }
    const char *sql = buf_cstr(&stmts);
    if (n_stmts == 0) {
      printf("%s", sql); // "-- no drift: …"
      info("nothing to write: the models and the tables agree");
      buf_free(&stmts);
      buf_free(&plan);
      return 0;
    }
    if (!write) {
      printf("%s", sql);
      info("this is what `webc migrate diff --write` would add to %s/", c.migrations);
      buf_free(&stmts);
      buf_free(&plan);
      return 0;
    }
    // the next number in the directory, the same way `webc migrate new` counts them
    DIR *d = opendir(c.migrations);
    i32 n = 1;
    struct dirent *e;
    while (d && (e = readdir(d)))
      if (e->d_name[0] >= '0' && e->d_name[0] <= '9') n++;
    if (d) closedir(d);
    char path[1024];
    snprintf(path, sizeof path, "%s/%04d_%s.sql", c.migrations, n, name);
    if (access(path, F_OK) == 0) {
      info("%s exists; pick another name with `webc migrate diff --write <name>`", path);
      buf_free(&stmts);
      buf_free(&plan);
      return 1;
    }
    Buf file;
    buf_init(&file, nullptr, 4096);
    buf_printf(&file, "-- generated by `webc migrate diff` from the models; yours to edit.\n");
    buf_printf(&file, "-- additive only: it never drops or narrows a column. Run `webc migrate` to apply.\n");
    buf_printf(&file, "%s", sql);
    write_file(path, buf_cstr(&file));
    buf_free(&file);
    buf_free(&stmts);
    buf_free(&plan);
    info("wrote %s (%s)", path, "not applied: run `webc migrate`");
    return 0;
  }
  WDB *db = db_open(c.database);
  if (!db) die("cannot open %s", c.database);
  WMigrate m = {.db = db, .dir = c.migrations, .dry_run = false};
  if (strcmp(sub, "status") == 0) {
    migrations_status(&m);
  } else if (strcmp(sub, "dry") == 0) {
    m.dry_run = true;
    migrations_apply(&m);
  } else {
    migrations_apply(&m);
  }
  db_close(db);
  return 0;
}

// ---------------------------------------------------------------- doctor

static int run_boot_hook(const char *env_name, bool seed_reset);

// A doctor line: one check, its verdict, and the sentence a reader needs when it is
// wrong. The same record is printed and, with `--json`, emitted — so "is my toolchain
// ready" has one answer for a person and one answer for a tool.
typedef struct DocRow {
  const char *name;
  bool ok;
  bool fatal;    // a missing sqlite3 or wasi-sdk is not a problem for every app
  char detail[512];
} DocRow;

static void doc_row(DocRow *rows, i32 *n, const char *name, bool ok, bool fatal,
                    const char *fmt, ...) __attribute__((format(printf, 6, 7)));
static void doc_row(DocRow *rows, i32 *n, const char *name, bool ok, bool fatal,
                    const char *fmt, ...) {
  DocRow *r = &rows[(*n)++];
  r->name = name;
  r->ok = ok;
  r->fatal = fatal;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r->detail, sizeof r->detail, fmt, ap);
  va_end(ap);
}

static int cmd_doctor(bool schema_only, bool json) {
  if (schema_only) return run_boot_hook("WEBC_SCHEMA_CHECK", false);
  DocRow rows[24];
  i32 n = 0, problems = 0;
  pick_cc();
  char cmd[256];
  // the version banner is worth one line, not five: `clang --version` prints four, and
  // `webc doctor --json` may not have a banner in front of its data at all
  snprintf(cmd, sizeof cmd, "%s --version 2>/dev/null | head -1", g_cc);
  char cver[256] = "";
  bool have_cc = false;
  FILE *vp = popen(cmd, "r");
  if (vp) {
    have_cc = fgets(cver, sizeof cver, vp) != nullptr;
    pclose(vp);
  }
  size_t vn = strlen(cver);
  while (vn && (cver[vn - 1] == '\n' || cver[vn - 1] == '\r')) cver[--vn] = 0;
  doc_row(rows, &n, "compiler", have_cc, true, "%s", have_cc ? cver : "MISSING");
  FILE *f = fopen("/tmp/.webc_c23.c", "w");
  fputs("int main(void){ return 0; }\n", f);
  fclose(f);
  snprintf(cmd, sizeof cmd, "%s -std=c23 -o /tmp/.webc_c23 /tmp/.webc_c23.c 2>/dev/null", g_cc);
  bool c23 = system(cmd) == 0;
  doc_row(rows, &n, "libc23", c23, true, c23 ? "ok" : "MISSING (-std=c23)");
  f = fopen("/tmp/.webc_sqlite.c", "w");
  fputs("#include <sqlite3.h>\nint main(void){ return sqlite3_libversion_number() > 0 ? 0 : 1; }\n",
        f);
  fclose(f);
  snprintf(cmd, sizeof cmd, "%s -o /tmp/.webc_sqlite /tmp/.webc_sqlite.c -lsqlite3 2>/dev/null",
           g_cc);
  bool sqlite3 = system(cmd) == 0;
  doc_row(rows, &n, "sqlite3", sqlite3, false,
          sqlite3 ? "ok" : "MISSING (only needed for sqlite databases)");
  char wasi[1024];
  bool have_wasi = find_wasi(wasi, sizeof wasi);
  doc_row(rows, &n, "wasi-sdk", have_wasi, false, "%s",
          have_wasi ? (wasi[0] ? wasi : "clang on PATH")
                    : "MISSING (webc build --target wasm; native, SSR and the server still build)");
  bool have_node = system("command -v node >/dev/null 2>&1") == 0;
  doc_row(rows, &n, "node", have_node, false, "%s",
          have_node ? "ok" : "MISSING (the JS bridge tests are skipped)");
  char lib[1024];
  snprintf(lib, sizeof lib, "%s/build/libwebc.a", webc_prefix());
  bool have_lib = path_exists(lib);
  doc_row(rows, &n, "library", have_lib, true, "%s",
          have_lib ? lib
                   : "MISSING — make in the checkout, or install a release:\n"
                     "  curl -fsSL https://raw.githubusercontent.com/bumasoft/WebC/main/scripts/install.sh | sh");

  Config c;
  config_load(&c, ".");
  bool has_cfg = path_exists("webc.json");
  doc_row(rows, &n, "project", true, false, "%s", has_cfg ? "" : "no webc.json here");
  // A project that names its file sets has no single entry file to miss: its routes live
  // in `server` and its views in `shared`, and `webc build` links each half from its own
  // set. Complaining about a missing src/main.c in one of those was a doctor that cried
  // wolf on every app the framework's own docs describe. A `client` set and no `server` set
  // is the browser-only app (`mode: "web"` + a client half): it has no main() on purpose, so
  // there is nothing to miss and the binary it would be is one it never links.
  bool entry_ok = c.has_sets ? (c.server.n > 0 || c.client.n > 0) : path_exists(c.entry);
  doc_row(rows, &n, "entry", entry_ok || !has_cfg, has_cfg,
          "%s%s", c.entry[0] ? c.entry : "(none)",
          !entry_ok ? " (missing)"
                    : (c.has_sets ? fmt_s(" (file sets: %d server, %d shared, %d client)",
                                          (int)c.server.n, (int)c.shared.n, (int)c.client.n)
                                        .p
                                  : ""));
  doc_row(rows, &n, "database", true, false, "%s", c.database);
  doc_row(rows, &n, "migrations", true, false, "%s", path_exists(c.migrations) ? "present" : "none");
  doc_row(rows, &n, "port", true, false, "%d (dev reload on %d)", c.port, c.dev_port);

  for (i32 i = 0; i < n; i++)
    if (!rows[i].ok && rows[i].fatal) problems++;

  if (json) {
    Buf j;
    buf_init(&j, nullptr, 8192);
    buf_puts(&j, "{\"webc\":\"" CLI_VERSION "\",\"framework\":");
    jputs(&j, webc_prefix());
    buf_printf(&j, ",\"problems\":%d,\"ok\":%s,\"checks\":[", problems,
               problems ? "false" : "true");
    for (i32 i = 0; i < n; i++) {
      if (i) buf_puts(&j, ",");
      buf_puts(&j, "{\"name\":");
      jputs(&j, rows[i].name);
      buf_printf(&j, ",\"ok\":%s,\"required\":%s,\"detail\":", rows[i].ok ? "true" : "false",
                 rows[i].fatal ? "true" : "false");
      jputs(&j, rows[i].detail);
      buf_puts(&j, "}");
    }
    buf_puts(&j, "]}\n");
    printf("%s", buf_cstr(&j));
    buf_free(&j);
    return problems ? 1 : 0;
  }

  // info() already prints the "webc" prefix: saying it again gave "webc webc 0.1.0"
  // as the first line of `webc doctor`, which is the line a bug report quotes.
  info("%s (framework at %s)", CLI_VERSION, webc_prefix());
  for (i32 i = 0; i < n; i++) {
    // The project row's detail is empty when there is a webc.json: the name and the
    // version are the answer, and the row is the only place they are printed.
    if (!strcmp(rows[i].name, "project") && rows[i].detail[0] == 0) {
      printf("  %-10s %s %s\n", rows[i].name, c.name, c.version);
      continue;
    }
    if (!rows[i].ok && rows[i].fatal) printf("  %-10s %s%s%s\n", rows[i].name, g_r, rows[i].detail, g_x);
    else if (!rows[i].ok) printf("  %-10s %s\n", rows[i].name, rows[i].detail);
    else printf("  %-10s %s\n", rows[i].name, rows[i].detail);
  }
  info(problems ? "%d problem(s)" : "no problems found", problems);
  return problems ? 1 : 0;
}


// ---------------------------------------------------------------- size
//
// `webc size` answers the question a browser app actually has — what does this page cost,
// and where did it go — without opening devtools. Three numbers matter: the module raw, the
// module gzipped (what the network sees), and the bridge the page loads beside it. The
// section table is the free part of the answer: a module that is mostly `custom` is
// shipping function names, and a module whose `data` outgrows its `code` has a table in it.
static i64 file_bytes(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (i64)st.st_size;
}

// gzip -9, measured, not estimated: the browser's decompressor sees exactly this.
static i64 gzip_bytes(const char *path) {
  char tmp[1024];
  snprintf(tmp, sizeof tmp, "%s", "/tmp/.webc-size.gz");
  char cmd[1400];
  snprintf(cmd, sizeof cmd, "gzip -9nc '%s' > '%s' 2>/dev/null", path, tmp);
  if (system(cmd) != 0) return -1;
  i64 n = file_bytes(tmp);
  unlink(tmp);
  return n;
}

static void human_size(char *out, size_t cap, i64 n) {
  if (n < 0)
    snprintf(out, cap, "      —");
  else if (n < 1024)
    snprintf(out, cap, "%6lld B", (long long)n);
  else if (n < 1024 * 1024)
    snprintf(out, cap, "%5.1f KB", (double)n / 1024.0);
  else
    snprintf(out, cap, "%5.2f MB", (double)n / (1024.0 * 1024.0));
}

static const char *wasm_section_name(u8 id) {
  switch (id) {
  case 0: return "custom (names, debug, producers)";
  case 1: return "type";
  case 2: return "import";
  case 3: return "function";
  case 4: return "table";
  case 5: return "memory";
  case 6: return "global";
  case 7: return "export";
  case 8: return "start";
  case 9: return "element";
  case 10: return "code";
  case 11: return "data";
  case 12: return "data count";
  default: return "other";
  }
}

typedef struct {
  u8 id;
  i64 size;
} WasmSection;

// The section table is the one part of a wasm module that is trivial to read: a byte id and
// a LEB128 length. Everything else needs a parser, and a `size` command is not worth one.
static i32 wasm_sections(const char *path, WasmSection *out, i32 cap) {
  i64 n = 0;
  char *buf = read_all(path, &n);
  if (!buf || n < 8 || memcmp(buf, "\0asm", 4) != 0) {
    free(buf);
    return -1;
  }
  i64 p = 8;
  i32 k = 0;
  while (p < n && k < cap) {
    u8 id = (u8)buf[p++];
    u64 len = 0;
    i32 shift = 0;
    while (p < n && shift < 35) {
      u8 b = (u8)buf[p++];
      len |= (u64)(b & 0x7f) << shift;
      shift += 7;
      if (!(b & 0x80)) break;
    }
    if (p + (i64)len > n + 1) break;
    out[k].id = id;
    out[k].size = (i64)len;
    k++;
    p += (i64)len;
  }
  free(buf);
  return k;
}


static int cmp_names(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// A byte-for-byte copy, for the docs: they are generated text, and putting them through a
// Buf would only be a chance to change them.
static void copy_file(const char *from, const char *to) {
  i64 n = 0;
  char *text = read_all(from, &n);
  if (!text) die("cannot read %s", from);
  char dir[1200];
  snprintf(dir, sizeof dir, "%s", to);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = 0;
    mkdirs(dir);
  }
  FILE *f = fopen(to, "wb");
  if (!f) die("cannot write %s", to);
  fwrite(text, 1, (size_t)n, f);
  fclose(f);
  free(text);
}

// `webc docs [--emit DIR] [--list] [TOPIC]`: the agent documentation, as files.
//
// The skill ships with the toolchain and `webc new` writes it into the app, because an agent
// working in an app should never have to find the framework's sources to answer "what does
// this name do" — the failure this command exists to remove is a session spent grepping a
// checkout for the auth header. The leaves are generated (`make skill`) from the reference,
// so copying them cannot go stale while the toolchain that copies them did not.
static int docs_emit(const char *dir) {
  char src[1024];
  snprintf(src, sizeof src, "%s/skills/webc", webc_prefix());
  char router[1100];
  snprintf(router, sizeof router, "%s/SKILL.md", src);
  FILE *f = fopen(router, "rb");
  if (!f)
    die("no documentation at %s — this webc was built from a checkout without `skills/webc` "
        "(run `make skill` there)",
        src);
  fclose(f);

  char dstdir[1200];
  snprintf(dstdir, sizeof dstdir, "%s/references", dir);
  mkdirs(dstdir);
  char dst[1200];
  snprintf(dst, sizeof dst, "%s/SKILL.md", dir);
  copy_file(router, dst);

  i32 leaves = 0;
  char srcref[1100];
  snprintf(srcref, sizeof srcref, "%s/references", src);
  DIR *d = opendir(srcref);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      size_t n = strlen(e->d_name);
      if (n < 4 || strcmp(e->d_name + n - 3, ".md")) continue;
      char from[1400], to[1400];
      snprintf(from, sizeof from, "%s/%s", srcref, e->d_name);
      snprintf(to, sizeof to, "%s/%s", dstdir, e->d_name);
      copy_file(from, to);
      leaves++;
    }
    closedir(d);
  }
  printf("  %s\n", dir);
  printf("    SKILL.md %d references — an agent reads SKILL.md first and follows it to one\n", leaves);
  printf("    leaf per topic; `webc docs <topic>` prints one, `webc docs --list` names them\n");
  return leaves > 0 ? 0 : 1;
}

static int cmd_docs(i32 argc, char **argv) {
  const char *dir = ".webc/docs";
  bool list = false;
  const char *topic = nullptr;
  for (i32 i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--emit") && i + 1 < argc) dir = argv[++i];
    else if (str_has_prefix_c(str_lit(argv[i]), "--emit=")) dir = argv[i] + 7;
    else if (!strcmp(argv[i], "--list")) list = true;
    else if (argv[i][0] != '-') topic = argv[i];
  }
  if (topic) {
    char path[1400];
    snprintf(path, sizeof path, "%s/references/%s.md", dir, topic);
    FILE *f = fopen(path, "rb");
    if (!f) {
      // the topic may be a prefix: `webc docs models` for `models-and-orm.md`
      char refdir[1200];
      snprintf(refdir, sizeof refdir, "%s/references", dir);
      DIR *d = opendir(refdir);
      struct dirent *e;
      bool found = false;
      while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 3, ".md")) continue;
        if (strncmp(e->d_name, topic, strlen(topic))) continue;
        char full[1400];
        snprintf(full, sizeof full, "%s/%s", refdir, e->d_name);
        if (found) printf("  (also: %s)\n", e->d_name);
        else {
          f = fopen(full, "rb");
          found = true;
        }
      }
      if (d) closedir(d);
      if (!f) {
        printf("webc docs: no topic '%s' in %s — `webc docs --list`\n", topic, dir);
        return 1;
      }
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, stdout);
    fclose(f);
    return 0;
  }
  if (list) {
    char refdir[1200];
    snprintf(refdir, sizeof refdir, "%s/references", dir);
    DIR *d = opendir(refdir);
    if (!d) {
      printf("webc docs: %s is empty — `webc docs` writes it\n", dir);
      return 1;
    }
    struct dirent *e;
    char *names[64];
    i32 nn = 0;
    while ((e = readdir(d)) && nn < 64) {
      size_t n = strlen(e->d_name);
      if (n < 4 || strcmp(e->d_name + n - 3, ".md")) continue;
      char *copy = strdup(e->d_name);
      if (copy) copy[n - 3] = 0;
      names[nn++] = copy;
    }
    closedir(d);
    qsort(names, (size_t)nn, sizeof *names, cmp_names);
    for (i32 i = 0; i < nn; i++) {
      printf("  %s\n", names[i]);
      free(names[i]);
    }
    return 0;
  }
  return docs_emit(dir);
}

static int cmd_size_modules(Config *c, const char *check_file);

static int cmd_size(i32 argc, char **argv) {
  Config c;
  config_load(&c, ".");
  for (i32 i = 0; i < argc; i++)
    if (strcmp(argv[i], "--modules") == 0) {
      const char *check = nullptr;
      for (i32 j = i + 1; j < argc; j++) {
        if (strcmp(argv[j], "--check") == 0 && j + 1 < argc) check = argv[++j];
        else if (str_has_prefix_c(str_lit(argv[j]), "--check=")) check = argv[j] + 8;
      }
      return cmd_size_modules(&c, check);
    }
  bool json = false;
  for (i32 i = 0; i < argc; i++)
    if (strcmp(argv[i], "--json") == 0) json = true;
  char wasm[1024];
  snprintf(wasm, sizeof wasm, "%s/%s.wasm", c.root, WC_WASM_STEM);
  char bridge[1024];
  snprintf(bridge, sizeof bridge, "%s/assets/webc.js", webc_prefix());
  i64 wr = file_bytes(wasm), wg = wr > 0 ? gzip_bytes(wasm) : -1;
  i64 br = file_bytes(bridge), bg = br > 0 ? gzip_bytes(bridge) : -1;

  if (json && wr <= 0) {
    printf("{\"built\":false}\n");
    return 0;
  }
  if (!json) printf("\n  \033[1mwebc size\033[0m  %s\n\n", c.name[0] ? c.name : "this project");
  if (wr <= 0) {
    printf("  no browser half built yet — `webc build` first\n\n");
    return 0;
  }
  char a[32], b[32], d[32], e[32];
  human_size(a, sizeof a, wr);
  human_size(b, sizeof b, wg);
  human_size(d, sizeof d, br);
  human_size(e, sizeof e, bg);
  if (json) {
    printf("{\"built\":true,\"wasm\":{\"raw\":%lld,\"gz\":%lld},\"bridge\":{\"raw\":%lld,\"gz\":%lld},"
           "\"wire_gz\":%lld,\"budget\":",
           (long long)wr, (long long)wg, (long long)br, (long long)bg,
           (long long)((wg > 0 && bg > 0) ? wg + bg : wg));
    if (c.wasm_budget > 0 && wg > 0)
      printf("{\"limit\":%lld,\"over\":%lld,\"ok\":%s}", (long long)c.wasm_budget,
             (long long)(wg > c.wasm_budget ? wg - c.wasm_budget : c.wasm_budget - wg),
             wg <= c.wasm_budget ? "true" : "false");
    else
      printf("null");
  } else {
    printf("    %-34s %s   %s gz\n", "module  " WC_WASM_STEM, a, b);
    printf("    %-34s %s   %s gz\n", "bridge  assets/webc.js", d, e);
  }
  if (!json && wg > 0 && bg > 0) {
    char t[32];
    human_size(t, sizeof t, wg + bg);
    printf("    sent over the wire (both, gzipped): %s\n", t);
  }
  // A promise the app declares is a promise the tool reports on. Without this, a budget is a
  // number in a document that nobody runs; with it, `webc size` is the whole conversation.
  if (!json && c.wasm_budget > 0 && wg > 0) {
    char lim[32];
    human_size(lim, sizeof lim, c.wasm_budget);
    if (wg <= c.wasm_budget) {
      char over[32];
      human_size(over, sizeof over, c.wasm_budget - wg);
      printf("    budget: %s gz — %s under\n", lim, over);
    } else {
      char over[32];
      human_size(over, sizeof over, wg - c.wasm_budget);
      printf("    budget: %s gz — \033[31mOVER by %s\033[0m\n", lim, over);
    }
  }
  if (!json) printf("\n");

  WasmSection sec[64];
  i32 k = wasm_sections(wasm, sec, 64);
  if (k > 0 && !json) printf("    where the module goes:\n");
  if (json) printf(",\"sections\":[");
  bool first_sec = true;
  for (i32 i = 0; i < k; i++) {
    if (sec[i].size < 1024) continue; // noise: a section nobody has to think about
    char one[32];
    human_size(one, sizeof one, sec[i].size);
    if (json) {
      Buf nb;
      buf_init(&nb, nullptr, 64);
      jputs(&nb, wasm_section_name(sec[i].id));
      printf("%s{\"name\":%s,\"bytes\":%lld,\"pct\":%.1f}", first_sec ? "" : ",", buf_cstr(&nb),
             (long long)sec[i].size, 100.0 * (double)sec[i].size / (double)wr);
      buf_free(&nb);
      first_sec = false;
    } else {
      printf("      %-34s %s  %4.1f%%\n", wasm_section_name(sec[i].id), one,
             100.0 * (double)sec[i].size / (double)wr);
    }
  }
  if (k > 0 && !json) printf("\n");
  if (json) {
    // the symbol list below is the prose half of this command: the object is the numbers
    printf("]}\n");
    return 0;
  }
  // "Where does it go" is a question about symbols, and the objects are right there. A C
  // array with an initialiser lives in .data — and a wasm data segment is bytes *shipped*,
  // zeros included — while the same array declared without one is zero-fill and costs
  // nothing. That distinction is invisible in a `.c` file and 1.6 MB in a module, so the
  // command that reports the size also names the statics that make it up.
  char objdir[1024];
  snprintf(objdir, sizeof objdir, "%s/.webc/obj/wasm", c.root);
  char wasi[1024];
  if (path_exists(objdir) && find_wasi(wasi, sizeof wasi) && wasi[0]) {
    Buf cmd;
    buf_init(&cmd, nullptr, 4096);
    buf_printf(&cmd, "%s/bin/llvm-nm -S --size-sort %s/*.o 2>/dev/null", wasi, objdir);
    Buf out;
    buf_init(&out, nullptr, 8192);
    bool got = run_cmd_quiet(buf_cstr(&cmd), &out);
    if (getenv("WEBC_VERBOSE"))
      fprintf(stderr, "  [size] nm=%s bytes=%zu\n", got ? "ok" : "failed", (size_t)out.n);
    if (out.n > 0) {
      // the tail is the biggest: `00019a7c0 d g_strats`
      char *line[6] = {0};
      i64 sz[6] = {0};
      i32 n = 0;
      char *p2 = buf_cstr(&out);
      while (p2 && *p2) {
        char *nl = strchr(p2, '\n');
        char *next = nl ? nl + 1 : nullptr;
        if (nl) *nl = 0;
        char nam[128] = {0}, typ = 0;
        unsigned long long v = 0, addr = 0;
        // llvm-nm -S prints address, size, type, name — four columns, the size second.
        if (sscanf(p2, "%llx %llx %c %127s", &addr, &v, &typ, nam) == 4 &&
            (typ == 'd' || typ == 'D' || typ == 'b' || typ == 'B') && v > 4096) {
          // keep the six biggest, smallest at 0
          i32 at = -1;
          for (i32 i = 0; i < 6; i++)
            if (sz[i] < (i64)v) {
              at = i;
              break;
            }
          if (at >= 0) {
            for (i32 i = 5; i > at; i--) {
              sz[i] = sz[i - 1];
              line[i] = line[i - 1];
            }
            sz[at] = (i64)v;
            line[at] = strdup(nam);
          }
        }
        p2 = next;
      }
      n = 0;
      for (i32 i = 0; i < 6; i++)
        if (line[i]) n++;
      if (n) {
        printf("    the biggest statics (from the object files):\n");
        for (i32 i = 0; i < 6; i++) {
          if (!line[i]) continue;
          char one[32];
          human_size(one, sizeof one, sz[i]);
          printf("      %-34s %s\n", line[i], one);
          free(line[i]);
        }
        printf("\n    A static declared without an initialiser is zero-fill and costs no\n"
               "    bytes; one with an initialiser ships its zeros. Compare the list with\n"
               "    the data section above: the static about that size is the one being\n"
               "    paid for, and filling it in boot() instead moves it to zero-fill.\n\n");
      }
    }
    buf_free(&out);
    buf_free(&cmd);
  }

  // The floor is the SDK's: a hello-world module is ~33 KB gzipped because wasi-libc and the
  // reactor frame come along. What sits above it is the app plus every framework door it
  // opens — which is the number worth watching in a review.
  if (wg > 0 && wg > 120 * 1024)
    printf("    the SDK's floor is ~33 KB gzipped; %s above it is this app and the parts of\n"
           "    the framework it links. `--release` and keeping js()/JSON out of a hot path\n"
           "    are the two levers that move it.\n\n",
           b);
  return 0;
}

// ---------------------------------------------------------------- routes

static int count_routes_in(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      n += count_routes_in(p);
      continue;
    }
    size_t len = strlen(e->d_name);
    if (len < 2 || strcmp(e->d_name + len - 2, ".c") != 0) continue;
    Arena *a = arena_new(8 * 1024);
    str text = read_file(a, p);
    const char *verbs[] = {"GET(", "POST(", "PUT(", "PATCH(", "DELETE(", "ANY("};
    for (size_t k = 0; k < WC_COUNTOF(verbs); k++) {
      const char *q = text.p;
      while ((q = strstr(q, verbs[k]))) {
        // a route macro is at the start of a line (GET(, not .GET( or myGET()
        if (q == text.p || q[-1] == '\n' || q[-1] == ' ' || q[-1] == '\t') n++;
        q += strlen(verbs[k]);
      }
    }
    arena_destroy(a);
  }
  closedir(d);
  return n;
}

static int cmd_routes(bool json) {
  Arena *a = arena_new(256 * 1024);
  WFacts *F = facts_new(a, ".");
  // A framework checkout is a tree of projects (`examples/*`), and each one is its own
  // program: listing all of them together answers a question nobody asked. The command
  // answers for the project you are standing in, and says so when there is none.
  char here[1024];
  char *rp = realpath(".", nullptr);
  snprintf(here, sizeof here, "%s", rp ? rp : ".");
  free(rp);
  i32 host = -1;
  for (i32 i = 0; F && i < F->nproj; i++)
    if (!strcmp(F->proj[i].root, here)) host = i;
  Buf j;
  buf_init(&j, nullptr, 8192);
  i32 n = 0;
  if (json) buf_puts(&j, "{\"routes\":[");
  else info("routes declared in %s", "src");
  for (i32 i = 0; F && i < F->nroute; i++) {
    WRoute *r = &F->route[i];
    i32 pi = facts_proj_of(F, r->file);
    if (pi < 0) continue;
    if (host >= 0 && pi != host) continue;
    const char *fp = facts_file_path(F, r->file);
    if (fp && fp[0] == '.' && fp[1] == '/') fp += 2;
    if (json) {
      if (n) buf_puts(&j, ",");
      buf_puts(&j, "{\"method\":");
      jputs(&j, r->method);
      buf_puts(&j, ",\"path\":");
      jputs(&j, r->path);
      buf_puts(&j, ",\"file\":");
      jputs(&j, fp);
      buf_printf(&j, ",\"line\":%d", r->line + 1);
      if (r->view) {
        buf_puts(&j, ",\"view\":");
        jputs(&j, r->view);
      }
      buf_puts(&j, "}");
    } else if (r->view) {
      printf("  %-7s %-38s %s:%d  %s\n", r->method, r->path, fp, r->line + 1, r->view);
    } else {
      printf("  %-7s %-38s %s:%d\n", r->method, r->path, fp, r->line + 1);
    }
    n++;
  }
  if (json) {
    buf_printf(&j, "],\"count\":%d}\n", n);
    printf("%s", buf_cstr(&j));
    buf_free(&j);
  } else if (!n) {
    if (host < 0 && F && F->nproj > 0)
      printf("  (this is not an app: %d project%s under it — `cd` into one, or read the "
             "whole tree with `webc routes --json`)\n",
             F->nproj, F->nproj == 1 ? "" : "s");
    else
      printf("  (none — a route is `GET(\"/\") { … }` in a `server` source)\n");
  }
  arena_destroy(a);
  return 0;
}

// ---------------------------------------------------------------- fmt

// ---------------------------------------------------------------- the boot hooks
//
// The models, the migrations and the seed live in the application binary, so the
// only process that can answer "do my models match my tables?" or "load the
// fixtures" is the application itself. `webc check` and `webc seed` build it, run
// it with one environment variable set, and relay its exit code: webc_boot() runs
// (the app gets to open what it needs), webc_boot_diagnostics() answers, and the
// server never listens.
static int run_boot_hook(const char *env_name, bool seed_reset) {
  Config c;
  config_load(&c, ".");
  char out[1024];
  snprintf(out, sizeof out, "%s/.webc/%s", c.root, c.name);
  if (!build(&c, nullptr, false, out)) {
    fprintf(stderr, "webc: build failed\n");
    return 1;
  }
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    setenv(env_name, "1", 1);
    if (seed_reset) setenv("WEBC_SEED_RESET", "1", 1);
    if (c.database[0]) setenv("DATABASE_URL", c.database, 1);
    if (c.migrations[0]) setenv("WEBC_MIGRATIONS", c.migrations, 1);
    setenv("WEBC_NO_METRICS", "1", 1);
    execl(out, out, (char *)nullptr);
    fprintf(stderr, "webc: cannot run %s\n", out);
    _exit(127);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return 1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

// The same hook, with the child's stdout in a buffer instead of on the terminal: some hooks
// *are* their output (`webc migrate diff` writes what the app prints into a file).
static int run_boot_hook_capture(const char *env_name, const char *value, Buf *out) {
  Config c;
  config_load(&c, ".");
  char exe[1024];
  snprintf(exe, sizeof exe, "%s/.webc/%s", c.root, c.name);
  if (!build(&c, nullptr, false, exe)) {
    fprintf(stderr, "webc: build failed\n");
    return 1;
  }
  fflush(stdout);
  int fds[2];
  if (pipe(fds) != 0) return 1;
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], 1);
    close(fds[1]);
    setenv(env_name, value && *value ? value : "1", 1);
    if (c.database[0]) setenv("DATABASE_URL", c.database, 1);
    if (c.migrations[0]) setenv("WEBC_MIGRATIONS", c.migrations, 1);
    setenv("WEBC_NO_METRICS", "1", 1);
    execl(exe, exe, (char *)nullptr);
    fprintf(stderr, "webc: cannot run %s\n", exe);
    _exit(127);
  }
  close(fds[1]);
  char chunk[4096];
  for (;;) {
    ssize_t got = read(fds[0], chunk, sizeof chunk);
    if (got <= 0) break;
    buf_write(out, chunk, (size_t)got);
  }
  close(fds[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static int cmd_seed(int argc, char **argv) {
  bool reset = false;
  for (int i = 0; i < argc; i++)
    if (strcmp(argv[i], "--reset") == 0) reset = true;
  int rc = run_boot_hook("WEBC_SEED", reset);
  if (rc == 0) info("seeded");
  return rc;
}

// One command that answers "is this project OK?" in the order the answers matter:
// does it build, does it pass its tests, is the toolchain there, do the models match
// the schema, how many routes are there, and can it boot at all.
// ---------------------------------------------------------------- consistency
//
// The mechanical questions are `build`/`tests`/`doctor`/`boot`/`schema`. These are the ones
// a reviewer asks, and every one of them is a grep: a bind nothing carries, a path spelled
// by hand that a model already names, a query built only to hold a value, a calendar
// written out again. None of them fail a build, and all of them are the sort of thing that
// sits in an app for a year.
//
// Rules are deliberately narrow: a rule that guesses is a rule people turn off.

typedef struct Finding {
  char where[160];
  char what[220];
  char fix[220];
  bool error;
} Finding;

#define WC_CHECK_MAX 64
typedef struct Findings {
  Finding f[WC_CHECK_MAX];
  i32 n;
  i32 errors;
} Findings;

static void finding(Findings *fs, bool error, const char *file, i32 line, const char *what,
                    const char *fix);
static void findingf(Findings *fs, bool error, const char *file, i32 line, const char *what,
                     const char *fix_fmt, const char *arg) {
  char fix[256];
  snprintf(fix, sizeof fix, fix_fmt, arg ? arg : "?");
  finding(fs, error, file, line, what, fix);
}

static void finding(Findings *fs, bool error, const char *file, i32 line, const char *what,
                    const char *fix) {
  if (fs->n >= WC_CHECK_MAX) return;
  Finding *f = &fs->f[fs->n++];
  snprintf(f->where, sizeof f->where, "%s:%d", file, line);
  snprintf(f->what, sizeof f->what, "%s", what);
  snprintf(f->fix, sizeof f->fix, "%s", fix);
  f->error = error;
  if (error) fs->errors++;
}

// every `NAME(` occurrence with a quoted first argument: "named", "bind", "q", "text"...
static bool quoted_arg(const char *line, const char *call, char *out, size_t cap) {
  const char *p = strstr(line, call);
  if (!p) return false;
  p += strlen(call);
  if (*p != '"') return false;
  p++;
  size_t k = 0;
  while (*p && *p != '"' && k + 1 < cap) out[k++] = *p++;
  out[k] = 0;
  return k > 0;
}

// A name argument that is either spelling: `named_field("draft")` or `named_field(draft)`.
// The bare identifier is what the macro stringify accepts without quotes reaching the
// attribute, so the wiring pass has to see both.
static bool name_arg(const char *line, const char *call, char *out, size_t cap) {
  if (quoted_arg(line, call, out, cap)) return true;
  const char *p = strstr(line, call);
  if (!p) return false;
  p += strlen(call);
  size_t k = 0;
  while (p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' ||
               (*p >= '0' && *p <= '9')) &&
         k + 1 < cap)
    out[k++] = *p++;
  out[k] = 0;
  return k > 0;
}

// 256, not 64: a real stylesheet carries more classes than a route table carries names,
// and a list that fills silently turns "clean" into a lie for every class past the cut.
typedef struct StrList {
  char v[256][64];
  i32 n;
} StrList;

static bool list_has(const StrList *l, const char *s) {
  for (i32 i = 0; i < l->n; i++)
    if (strcmp(l->v[i], s) == 0) return true;
  return false;
}

static void list_add(StrList *l, const char *s) {
  if (l->n >= 256 || !*s) return;
  if (list_has(l, s)) return; // a name twice is one name; the counts are of *things*
  snprintf(l->v[l->n++], sizeof l->v[0], "%s", s);
}

// Model route prefixes (`/a/` from `/a/:slug`) and the names they belong to.
typedef struct RoutePrefix {
  char model[48];
  char prefix[64];
} RoutePrefix;

// P2's tables: the wiring is strings the compiler cannot check, so the builder does.
// names the views carry, names the client binds, the actions defined and the actions
// wired, the key specs, the field names — and the classes the views emit, for P10.
typedef struct Wiring {
  StrList named, bound;         // named("x") … bind("x", …)
  StrList fields, fields_bound; // named_field("x") … bind_field("x", …)
  StrList actions, wired;       // action(x) … the x in bind(…, x) / on_key(…, x)
  StrList keys, hosthooks;      // on_key specs; focus_hook/reveal_hook names
  StrList view_classes;         // class_("literal") and classes("literal", …)
} Wiring;

// One bind("name", "event", action) as written — for `webc wires`, which emits the
// contract as a header the client half and the tests can both include.
typedef struct BindTriple {
  char name[64], event[24], action[64];
} BindTriple;
#define WC_MAX_BINDS 128
static BindTriple g_binds[WC_MAX_BINDS];
static i32 g_nbinds;

static void acts_scan_line(const char *l, size_t n, StrList *into);

static void capture_bind(char *buf) {
  const char *kw[] = {"bind(", "bind_soft("};
  for (size_t ki = 0; ki < sizeof kw / sizeof kw[0]; ki++) {
    const char *p = strstr(buf, kw[ki]);
    if (!p) continue;
    BindTriple t = {0};
    if (!quoted_arg(buf, kw[ki], t.name, sizeof t.name)) continue;
    const char *q1 = strchr(p + strlen(kw[ki]), '"');
    const char *q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
    const char *q3 = q2 ? strchr(q2 + 1, '"') : nullptr;
    const char *q4 = q3 ? strchr(q3 + 1, '"') : nullptr;
    if (q3 && q4 && (size_t)(q4 - q3 - 1) < sizeof t.event)
      memcpy(t.event, q3 + 1, (size_t)(q4 - q3 - 1));
    const char *a = q4 ? strchr(q4 + 1, ',') : nullptr;
    if (a) {
      a++;
      while (*a == ' ') a++;
      size_t k = 0;
      while (a[k] && (isalnum((unsigned char)a[k]) || a[k] == '_') && k + 1 < sizeof t.action)
        k++;
      memcpy(t.action, a, k);
    }
    if (t.name[0] && t.action[0] && g_nbinds < WC_MAX_BINDS) g_binds[g_nbinds++] = t;
  }
}

// What is left of a line once the prose is gone: the check reads text, so a comment must not
// count as a wiring. Scanned as code, prose cuts both ways — `// on_click(x)` in an
// explanatory line registers x as reachable (masking "an action nothing wires"), and
// `// see on_click(tab_flat)` emits a prototype for an action that does not exist. Cut at the
// first `//` outside a string; a `/* … */` on the same line is skipped, not cut.
static void strip_comment(char *buf) {
  char *out = buf;
  bool inq = false;
  for (const char *q = buf; *q;) {
    if (*q == '"' && (q == buf || q[-1] != '\\')) {
      inq = !inq;
      *out++ = *q++;
      continue;
    }
    if (!inq && q[0] == '/' && q[1] == '/') break;                 // the rest is prose
    if (!inq && q[0] == '/' && q[1] == '*') {
      const char *end = strstr(q + 2, "*/");
      q = end ? end + 2 : q + strlen(q);                           // skip it, keep the code after
      continue;
    }
    *out++ = *q++;
  }
  *out = 0;
}

static void wiring_line(Wiring *w, const char *p, i32 line, char *buf) {
  char name[64];
  strip_comment(buf);
  acts_scan_line(buf, strlen(buf), &w->wired);   // on_click(x) / on(x, …) in a view
  if (quoted_arg(buf, "named(", name, sizeof name)) list_add(&w->named, name);
  // the components (webc/ui.h) carry the name for you: their first argument is the hook
  if (quoted_arg(buf, "seg(", name, sizeof name)) list_add(&w->named, name);
  if (quoted_arg(buf, "tab(", name, sizeof name)) list_add(&w->named, name);
  if (quoted_arg(buf, "switch_(", name, sizeof name)) list_add(&w->named, name);
  if (name_arg(buf, "named_field(", name, sizeof name)) list_add(&w->fields, name);
  if (quoted_arg(buf, "bind(", name, sizeof name)) list_add(&w->bound, name);
  if (quoted_arg(buf, "bind_soft(", name, sizeof name)) list_add(&w->bound, name);
  capture_bind(buf);
  if (quoted_arg(buf, "bind_input(", name, sizeof name)) list_add(&w->bound, name);
  if (quoted_arg(buf, "bind_field(", name, sizeof name)) list_add(&w->fields_bound, name);
  // bind_ctx("x", T, f): the newest field binding — the wiring pass must see it
  // or every context-bound input reports "a field no action reads"
  {
    const char *p = strstr(buf, "bind_ctx");
    if (p) {
      const char *q = strchr(p, '"');
      const char *q2 = q ? strchr(q + 1, '"') : nullptr;
      if (q && q2 && (size_t)(q2 - q - 1) < sizeof name) {
        memcpy(name, q + 1, (size_t)(q2 - q - 1));
        name[q2 - q - 1] = 0;
        list_add(&w->fields_bound, name);
      }
    }
  }
  if (quoted_arg(buf, "focus_hook(", name, sizeof name)) list_add(&w->hosthooks, name);
  // a field marked the plain way — attr("data-field", "name") — is a field all the same;
  // named_field() is the spelling with a name, and both should agree with bind_field
  {
    const char *df = strstr(buf, "\"data-field\", \"");
    if (df) {
      const char *q = df + strlen("\"data-field\", \"");
      const char *q2 = strchr(q, '"');
      if (q2 && (size_t)(q2 - q) < sizeof name) {
        memcpy(name, q, (size_t)(q2 - q));
        name[q2 - q] = 0;
        list_add(&w->fields, name);
      }
    }
  }
  // the document verbs are a wiring too: on_ready(x) / on_nav(x) name actions
  for (const char *fn = buf; (fn = strstr(fn, "on_")) != nullptr; fn++) {
    if (strncmp(fn, "on_ready(", 9) && strncmp(fn, "on_nav(", 7)) continue;
    const char *a = strchr(fn, '(') + 1;
    char act[64];
    size_t k = 0;
    for (; *a && (isalnum((unsigned char)*a) || *a == '_') && k + 1 < sizeof act; a++)
      act[k++] = *a;
    act[k] = 0;
    if (k) list_add(&w->wired, act);
    break;
  }
  if (quoted_arg(buf, "reveal_hook(", name, sizeof name)) list_add(&w->hosthooks, name);
  if (quoted_arg(buf, "class_(", name, sizeof name)) list_add(&w->view_classes, name);
  if (quoted_arg(buf, "classes(", name, sizeof name)) list_add(&w->view_classes, name);

  // action(name) — a definition at the start of a line
  {
    const char *t = buf;
    while (*t == ' ' || *t == '\t') t++;
    if (strncmp(t, "action(", 7) == 0) {
      char act[64];
      size_t k = 0;
      for (const char *a = t + 7; *a && (isalnum((unsigned char)*a) || *a == '_') &&
                                  k + 1 < sizeof act; a++)
        act[k++] = *a;
      act[k] = 0;
      if (k) list_add(&w->actions, act);
    }
  }

  // the action a binding wires: bind*( "hook", "ev", ACT) / bind_field("f", ACT) /
  // on_key("spec", ACT) — the *last* bare identifier before the closing paren of the call
  for (const char *call = buf; (call = strstr(call, "bind")) != nullptr; call++) {
    if (strncmp(call, "bind(", 5) && strncmp(call, "bind_soft(", 10) &&
        strncmp(call, "bind_field(", 11))
      continue;
    // the call's own closing paren, not an argument's: bind(row_hook(i), "click", pick) put
    // the paren of row_hook(i) first, and the pass then read "i" as the action and pick as
    // dead — a hook name computed at runtime (a table of tabs) is a supported shape.
    const char *open_p = strchr(call, '(');
    if (!open_p) continue;
    const char *close = nullptr;
    i32 depth = 0;
    for (const char *q = open_p; *q; q++) {
      if (*q == '"') {   // a quoted argument may hold a paren
        q++;
        while (*q && *q != '"') q++;
        if (!*q) break;
        continue;
      }
      if (*q == '(') depth++;
      else if (*q == ')' && --depth == 0) {
        close = q;
        break;
      }
    }
    if (!close) continue;
    // walk back over the last argument: [a-zA-Z0-9_] before ) or before a trailing )
    const char *e = close;
    while (e > call && *(e - 1) == ' ') e--;
    const char *b2 = e;
    while (b2 > call && (isalnum((unsigned char)b2[-1]) || b2[-1] == '_')) b2--;
    if (e > b2 && (size_t)(e - b2) < sizeof name) {
      memcpy(name, b2, (size_t)(e - b2));
      name[e - b2] = 0;
      list_add(&w->wired, name);
    }
    break; // one binding per line is plenty; a second is a style question
  }
  {
    const char *k = strstr(buf, "on_key(");
    if (k) {
      // on_key("spec", act)
      const char *q1 = strchr(k + 7, '"');
      if (q1) {
        const char *q2 = strchr(q1 + 1, '"');
        if (q2) {
          size_t kn = (size_t)(q2 - q1 - 1);
          if (kn < sizeof name) {
            memcpy(name, q1 + 1, kn);
            name[kn] = 0;
            list_add(&w->keys, name);
          }
          const char *a = q2 + 1;
          while (*a == ' ' || *a == ',') a++;
          char act[64];
          size_t c2 = 0;
          for (; *a && (isalnum((unsigned char)*a) || *a == '_') && c2 + 1 < sizeof act; a++)
            act[c2++] = *a;
          act[c2] = 0;
          if (c2) list_add(&w->wired, act);
        }
      }
    }
  }
  (void)p;
  (void)line;
}

// The one modifier plus one key grammar on_key() promises (the bridge matches, the names
// live in C — so a typo binds nothing, silently; this is where it stops being silent).
static bool key_spec_ok(const char *spec) {
  static const char *known[] = {"escape", "enter",   "tab",     "backspace", "space",
                                "arrowup", "arrowdown", "arrowleft", "arrowright",
                                "home",    "end",     "pageup",  "pagedown",  "delete",
                                "insert",  nullptr};
  char buf[64];
  if (strlen(spec) >= sizeof buf) return false;
  snprintf(buf, sizeof buf, "%s", spec);
  char *plus = strchr(buf, '+');
  const char *key = buf;
  if (plus) {
    *plus = 0;
    key = plus + 1;
    const char *mods[] = {"meta", "ctrl", "alt", "shift", "mod"};
    bool ok = false;
    for (size_t i = 0; i < WC_COUNTOF(mods); i++)
      if (strcmp(buf, mods[i]) == 0) ok = true;
    if (!ok) return false;
  }
  if (!*key) return false;
  if (strlen(key) == 1) return true; // a printing character keys by itself
  if (key[0] == 'f' && strlen(key) <= 3) {
    int fn = atoi(key + 1);
    if (fn >= 1 && fn <= 12) return true;
  }
  for (size_t i = 0; known[i]; i++)
    if (strcmp(key, known[i]) == 0) return true;
  return false;
}

static Wiring g_wire; // one walk, one set of tables: consistency() reconciles them

static void wiring_app(const char *dir, Wiring *w) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      wiring_app(p, w);
      continue;
    }
    // .c *and* .h: the documented multi-file pattern puts the wiring in a shared header
    // (`declare_view(Todos);` once, a named hook beside its view), and a check that reads only the .c
    // files calls the header's half of the contract missing — the bind then reads as a name
    // no view declares, which is an error about the app when the check is what is blind.
    size_t len = strlen(e->d_name);
    bool is_c = len > 2 && strcmp(e->d_name + len - 2, ".c") == 0;
    bool is_h = len > 2 && strcmp(e->d_name + len - 2, ".h") == 0;
    if (!is_c && !is_h) continue;
    if (strstr(p, ".webc/") || strstr(p, "build/")) continue;
    Arena *a = arena_new(64 * 1024);
    str text = read_file(a, p);
    if (!text.p) {
      arena_destroy(a);
      continue;
    }
    i32 line = 0;
    for (const char *l = text.p; *l;) {
      const char *nl = strchr(l, '\n');
      size_t n = nl ? (size_t)(nl - l) : strlen(l);
      char buf[512];
      size_t cn = n < sizeof buf - 1 ? n : sizeof buf - 1;
      memcpy(buf, l, cn);
      buf[cn] = 0;
      line++;
      const char *t = buf;
      while (*t == ' ' || *t == '\t') t++;
      if (strncmp(t, "//", 2) == 0 || strncmp(t, "/*", 2) == 0 || *t == '*') {
        l = nl ? nl + 1 : l + n;
        continue;
      }
      wiring_line(w, p, line, buf);
      l = nl ? nl + 1 : l + n;
    }
    arena_destroy(a);
  }
  closedir(d);
}

// ---------------------------------------------------------------- the wiring pass
//
// The wiring is a string contract, and the check only *reads* it.

// `on_click(name)` and `on(name, …)`: the identifier right after the paren, into the wiring
// pass's "wired" list — because a handler declared on the element *is* the wiring, and a pass
// that knows only bind*/on_key reads every button action as dead code: 30 of 32 buttons on a
// single page were exactly that, which is how a note stops being read at all.
static void acts_scan_line(const char *l, size_t n, StrList *into) {
  static const char *verbs[] = {"on_click(", "on("};
  for (size_t v = 0; v < WC_COUNTOF(verbs); v++) {
    size_t vl = strlen(verbs[v]);
    for (size_t i = 0; i + vl < n; i++) {
      if (memcmp(l + i, verbs[v], vl) != 0) continue;
      char prev = i ? l[i - 1] : ' ';
      if (prev == '_' || (prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
          (prev >= '0' && prev <= '9') || prev == '.')
        continue; // wc_on( is not on(
      size_t k = i + vl, o = 0;
      while (k + o < n) {
        char c = l[k + o];
        if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))
          o++;
        else
          break;
      }
      char tmp[64];
      size_t c = o < sizeof tmp - 1 ? o : sizeof tmp - 1;
      memcpy(tmp, l + k, c);
      tmp[c] = 0;
      if (into) list_add(into, tmp);
    }
  }
  // The shelf passes the action as an argument rather than in a hook position, because the
  // control is the module's and the behaviour is the app's:
  //
  //   table_head(&t, sort_by);      K("Help", "shift+?", help_open);
  //   table_pager(&t, page_flip, "Rows");
  //
  // Without this the wiring pass would call `sort_by` dead code in an app that sorts. `arg`
  // is the 1-based argument the action sits in.
  static const struct {
    const char *call;
    i32 arg;
  } CALLS[] = {{"table_head(", 2}, {"table_pager(", 2}, {"K(", 3}};
  for (size_t c = 0; c < WC_COUNTOF(CALLS); c++) {
    size_t vl = strlen(CALLS[c].call);
    for (size_t i = 0; i + vl < n; i++) {
      if (memcmp(l + i, CALLS[c].call, vl) != 0) continue;
      char prev = i ? l[i - 1] : ' ';
      if (prev == '_' || isalnum((unsigned char)prev) || prev == '.') continue;
      size_t k = i + vl;
      i32 want = CALLS[c].arg;
      for (i32 a = 1; a < want; a++) {
        // skip one argument: to the next comma outside a string, or to the close paren
        bool inq = false;
        while (k < n) {
          char ch = l[k++];
          if (ch == '"') inq = !inq;
          else if (!inq && (ch == ',' || ch == ')')) break;
        }
      }
      while (k < n && l[k] == ' ') k++;
      size_t o = 0;
      while (k + o < n) {
        char ch = l[k + o];
        if (isalnum((unsigned char)ch) || ch == '_') o++;
        else break;
      }
      char tmp[64];
      size_t cc = o < sizeof tmp - 1 ? o : sizeof tmp - 1;
      memcpy(tmp, l + k, cc);
      tmp[cc] = 0;
      if (cc && (isalpha((unsigned char)tmp[0]) || tmp[0] == '_') && into) list_add(into, tmp);
    }
  }
}


// A partial is a file the compiler never sees, so the one failure a build cannot catch — a
// name with no file behind it — is this walk's job. The route renders, the head splices, and
// the chrome quietly is not there: that silence is what the finding replaces.
static void partial_check(Findings *fs, const char *file, i32 line, const char *name) {
  char path[256];
  snprintf(path, sizeof path, "partials/%s.html", name);
  struct stat st;
  if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) return;
  findingf(fs, true, file, line, "a partial with no file", "create %s", path);
}

static void scan_app(const char *dir, Findings *fs, StrList *named, StrList *bound,
                     RoutePrefix *routes, i32 *nroutes) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      scan_app(p, fs, named, bound, routes, nroutes);
      wiring_app(p, &g_wire);
      continue;
    }
    size_t len = strlen(e->d_name);
    if (len < 2 || strcmp(e->d_name + len - 2, ".c") != 0) continue;
    Arena *a = arena_new(64 * 1024);
    str text = read_file(a, p);
    if (!text.p) {
      arena_destroy(a);
      continue;
    }
    // model routes first: a path spelled by hand is only a finding if a model names it
    for (const char *q = text.p; (q = strstr(q, "WEBC_MODEL_ROUTE(")) != nullptr; q++) {
      const char *m = q + strlen("WEBC_MODEL_ROUTE(");
      char model[48];
      size_t k = 0;
      while (*m && *m != ',' && k + 1 < sizeof model) model[k++] = *m++;
      model[k] = 0;
      while (*m && *m != '"') m++;
      if (*m != '"') continue;
      m++;
      char pat[64];
      k = 0;
      while (*m && *m != '"' && k + 1 < sizeof pat) pat[k++] = *m++;
      pat[k] = 0;
      char *colon = strchr(pat, ':');
      if (!colon) continue;
      *colon = 0; // /a/:slug -> /a/
      if (*nroutes < 16) {
        snprintf(routes[*nroutes].model, sizeof routes[0].model, "%s", model);
        snprintf(routes[*nroutes].prefix, sizeof routes[0].prefix, "%s", pat);
        (*nroutes)++;
      }
    }
    i32 line = 0;
    bool renders = false, sets_title = false;
    for (const char *l = text.p; *l;) {
      const char *nl = strchr(l, '\n');
      size_t n = nl ? (size_t)(nl - l) : strlen(l);
      char buf[512];
      size_t cn = n < sizeof buf - 1 ? n : sizeof buf - 1;
      memcpy(buf, l, cn);
      buf[cn] = 0;
      line++;
      // A rule that fires on a comment is a rule people turn off. Prose that mentions an
      // idiom is documentation, not a use of it.
      const char *t = buf;
      while (*t == ' ' || *t == '\t') t++;
      bool comment = strncmp(t, "//", 2) == 0 || strncmp(t, "/*", 2) == 0 || *t == '*';
      if (comment) {
        l = nl ? nl + 1 : l + n;
        continue;
      }
      char name[64];
      char partial_name[96];
      if (quoted_arg(buf, "partial(", partial_name, sizeof partial_name))
        partial_check(fs, p, line, partial_name);
      if (quoted_arg(buf, "named(", name, sizeof name)) list_add(named, name);
      if (quoted_arg(buf, "bind(", name, sizeof name)) list_add(bound, name);
      if (quoted_arg(buf, "bind_input(", name, sizeof name)) list_add(bound, name);
      if (strstr(buf, "render(") || strstr(buf, "res_page")) renders = true;
      if (strstr(buf, "head_title(") || strstr(buf, "head(.title") || strstr(buf, ".page = head("))
        sets_title = true;
      if (strstr(buf, "q(\"select 1\")"))
        finding(fs, false, p, line, "a query built only to carry a bound value",
                "db_by(Model, \"field\", value) — one field, one value, no carrier");
      if (strstr(buf, "\"January\"") || strstr(buf, "\"Monday\""))
        finding(fs, false, p, line, "a calendar written out by hand",
                "fmt_date() / fmt_date_iso(): %B %A %e are the long names");
      if (strstr(buf, "/ 220") || strstr(buf, "* 220"))
        finding(fs, false, p, line, "a reading time computed here",
                "read_minutes(body) / read_time(body)");
      l = nl ? nl + 1 : l + n;
    }
    if (renders && !sets_title)
      finding(fs, false, p, 1, "this file renders pages and never sets a title",
              ".page = head(.title = …) — a page with no title is wrong in the tab");
    // the hand-written-path rule needs the routes collected from *all* files
    for (i32 r = 0; r < *nroutes; r++) {
      char needle[96];
      snprintf(needle, sizeof needle, "fmt(\"%s%%s\"", routes[r].prefix);
      for (const char *hit = text.p; (hit = strstr(hit, needle)) != nullptr; hit++) {
        i32 at = 1;
        for (const char *c = text.p; c < hit; c++)
          if (*c == '\n') at++;
        finding(fs, false, p, at, "a path spelled by hand that a model already names",
                "url_to() or link_to(Model, row) — the route is the one source of truth");
        break; // one per route per file is enough to point at it
      }
    }
    arena_destroy(a);
  }
  closedir(d);
}

static char src_root_for_styles[512];

static bool source_mentions(const char *dir, const char *needle) {
  DIR *d = opendir(dir);
  if (!d) return false;
  struct dirent *e;
  bool found = false;
  while ((e = readdir(d)) && !found) {
    if (e->d_name[0] == '.') continue;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      found = source_mentions(p, needle);
      continue;
    }
    // .c *and* .h: the documented multi-file pattern puts the wiring in a shared header
    // (`declare_view(Todos);` once, a named hook beside its view), and a check that reads only the .c
    // files calls the header's half of the contract missing — the bind then reads as a name
    // no view declares, which is an error about the app when the check is what is blind.
    size_t len = strlen(e->d_name);
    bool is_c = len > 2 && strcmp(e->d_name + len - 2, ".c") == 0;
    bool is_h = len > 2 && strcmp(e->d_name + len - 2, ".h") == 0;
    if (!is_c && !is_h) continue;
    if (strstr(p, ".webc/") || strstr(p, "build/")) continue;
    Arena *a = arena_new(64 * 1024);
    str text = read_file(a, p);
    if (text.p && strstr(text.p, needle)) found = true; // text is NUL-ended by read_file
    arena_destroy(a);
  }
  closedir(d);
  return found;
}

static bool class_styled(const StrList *css, const char *cls, const char **ignore) {
  for (size_t i = 0; ignore[i]; i++)
    if (strcmp(cls, ignore[i]) == 0) return true;
  return list_has(css, cls);
}

// ---- the styles check (P10): the class contract is the last stringly-typed edge
//
// A view emits class_("side-head") and a stylesheet answers — or doesn't. A typo on
// either side fails silently forever, in both directions: a class nothing styles, and a
// selector no markup ever carries. Notes only: state classes (on, sel, …) are the app's
// own vocabulary, and dynamic names are a design choice.
static void styles_check(const Config *c, const Wiring *w, Findings *fs) {
  // state classes are the app's own vocabulary; the component classes (webc/ui.h) are
  // the framework's — emitted from headers the app's sources never mention by name
  static const char *ignore[] = {"on",   "sel",  "open", "late", "err",  "error", "ok",
                                 "hide", "show", "js-only", "even", "odd", "active",
                                 "seg", "seg-label", "seg-count", "switch", "track",
                                 "tab", "meter", "meter-top", "meter-value", "meter-track",
                                 nullptr};
  // the classes the stylesheets know: .name tokens in selector position
  StrList css = {0};
  DIR *d = opendir(c->public_dir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      size_t len = strlen(e->d_name);
      if (len < 4 || strcmp(e->d_name + len - 4, ".css") != 0) continue;
      char p[1024];
      snprintf(p, sizeof p, "%s/%s", c->public_dir, e->d_name);
      Arena *a = arena_new(128 * 1024);
      str text = read_file(a, p);
      if (text.p) {
        // selector position only: a declaration block's { … } is not names — but an
        // at-rule's { (@media …) holds *selectors*, so a block stack, not a flag: one
        // flag swallows every selector inside a media query, which is most of a real
        // stylesheet's second half.
        bool decl[32]; // true: a declaration block; false: an at-rule group
        i32 depth = 0;
        char tok[64];
        size_t tn = 0;
        for (size_t i = 0; i <= text.n; i++) {
          char ch = i < text.n ? text.p[i] : 0;
          if (ch == '{') {
            // an at-rule group if an '@' appears since the last ; { } — @media and friends
            bool at = false;
            for (size_t j = i; j > 0; j--) {
              char back = text.p[j - 1];
              if (back == ';' || back == '{' || back == '}') break;
              if (back == '@') at = true;
            }
            if (depth < (i32)WC_COUNTOF(decl)) decl[depth] = !at;
            depth++;
          } else if (ch == '}' && depth > 0) {
            depth--;
          }
          bool in_block = depth > 0 && decl[depth > 0 ? depth - 1 : 0];
          if (!in_block && ch == '.' && tn == 0) {
            tn = 1; // start collecting, keep the marker out of the name
            continue;
          }
          if (tn > 0) {
            if (isalnum((unsigned char)ch) || ch == '-' || ch == '_') {
              if (tn < sizeof tok) tok[tn++ - 1] = ch;
            } else {
              if (tn > 1) {
                tok[tn - 1] = 0;
                list_add(&css, tok);
              }
              tn = 0;
              // a compound selector's second name starts here: `.segs.tight` is two
              // tokens, and swallowing the dot lost every one of them
              if (ch == '.' && !in_block) tn = 1;
            }
          }
        }
      }
      arena_destroy(a);
    }
    closedir(d);
  }
  // a class the views emit, token by token: class_("seg on") is seg and on
  for (i32 i = 0; i < w->view_classes.n; i++) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s", w->view_classes.v[i]);
    for (char *tok = strtok(buf, " "); tok; tok = strtok(nullptr, " ")) {
      if (!class_styled(&css, tok, ignore))
        findingf(fs, false, "public", 0, "a class no stylesheet answers — it styles nothing",
                 "add .%s to a stylesheet, or fix the spelling on one side", tok);
    }
  }
  // a selector nothing emits: dead css, or a typo the markup side never made
  for (i32 i = 0; i < css.n; i++) {
    const char *cls = css.v[i];
    bool ignored = false;
    for (size_t k = 0; ignore[k]; k++)
      if (strcmp(cls, ignore[k]) == 0) ignored = true;
    if (ignored) continue;
    bool emitted = false;
    for (i32 j = 0; j < w->view_classes.n && !emitted; j++) {
      char buf[128];
      snprintf(buf, sizeof buf, "%s", w->view_classes.v[j]);
      for (char *tok = strtok(buf, " "); tok; tok = strtok(nullptr, " "))
        if (strcmp(tok, cls) == 0) emitted = true;
    }
    // markup() and class_(fmt(…)) carry the literal in the C text even where the
    // scanner cannot see it as a literal: a name the sources never mention at all is
    // the only dead selector worth saying out loud. A dash-prefix family ("k-%s" built
    // from data) is mentioned by its prefix, and the prefix speaks for the family.
    if (emitted) continue;
    if (source_mentions(src_root_for_styles, cls)) continue;
    const char *dash = strchr(cls, '-');
    if (dash) {
      char prefix[64];
      size_t pl = (size_t)(dash - cls) + 1; // through the dash: "k-" is the family's mark
      if (pl < sizeof prefix) {
        snprintf(prefix, sizeof prefix, "%.*s", (int)pl, cls);
        if (source_mentions(src_root_for_styles, prefix)) continue;
      }
    }
    findingf(fs, false, "public", 0, "a selector no markup ever carries",
             ".%s is dead, or the view's class_() spelling drifted", cls);
  }
}

// ---------------------------------------------------------------- signals written, never read
//
// `set(tick, get(tick) + 1)` was the framework's own teaching for two releases, and every
// project it made carries the line. It is a write nobody reads: the bridge re-renders each
// mounted view when control returns to it (reference §11a), so the bump was never what made
// the page update. The note is a note, not an error — a signal can be a debug counter — but
// it names the spelling, because "nothing reads this" is invisible in a 1,600-line file.

typedef struct SigDecl {
  char name[64];
  char where[192];
  i32 line;
  i32 reads, writes;
  i32 kind;         // 0 state, 1 state_in, 2 state_own: one per page, per app instance,
                    // or per render position — the three answers, and the note differs
  i32 live_in_loop; // reads by a live binding *inside* a list: every row, one value
} SigDecl;

#define WC_MAX_SIGS 64
static SigDecl g_sigs[WC_MAX_SIGS];
static i32 g_nsigs;

// Every .c and .h under `dir`, one callback at a time. The wiring scan next door walks
// `.c` only because it is looking for bindings; a signal may be declared in a header.
static void source_walk(const char *dir, void (*fn)(const char *path, str text, void *ctx),
                        void *ctx) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      source_walk(p, fn, ctx);
      continue;
    }
    size_t len = strlen(e->d_name);
    bool is_c = len > 2 && strcmp(e->d_name + len - 2, ".c") == 0;
    bool is_h = len > 2 && strcmp(e->d_name + len - 2, ".h") == 0;
    if (!is_c && !is_h) continue;
    if (strstr(p, ".webc/") || strstr(p, "build/")) continue;
    Arena *a = arena_new(64 * 1024);
    str text = read_file(a, p);
    if (text.p) fn(p, text, ctx);
    arena_destroy(a);
  }
  closedir(d);
}

// The identifier after the macro's first comma: state(T, name, init) / state(Text, name, lit).
static bool sig_name_after(const char *at, char *out, size_t cap) {
  const char *c = strchr(at, ',');
  if (!c) return false;
  c++;
  while (*c == ' ' || *c == '\t') c++;
  size_t k = 0;
  while ((isalnum((unsigned char)*c) || *c == '_') && k + 1 < cap) out[k++] = *c++;
  out[k] = 0;
  return k > 0;
}

static void sig_collect(const char *path, str text, void *ctx) {
  (void)ctx;
  i32 line_no = 0;
  for (const char *l = text.p; l && *l;) {
    const char *nl = strchr(l, '\n');
    size_t n = nl ? (size_t)(nl - l) : strlen(l);
    line_no++;
    char line[512];
    size_t cn = n < sizeof line - 1 ? n : sizeof line - 1;
    memcpy(line, l, cn);
    line[cn] = 0;
    const char *t = line;
    while (*t == ' ' || *t == '\t') t++;
    const char *kw = nullptr;
    i32 kind = 0;
    if (strncmp(t, "state_own(", 10) == 0) kw = t + 10, kind = 2;
    else if (strncmp(t, "state_in(", 9) == 0) kw = t + 9, kind = 1;
    else if (strncmp(t, "state(", 6) == 0) kw = t + 6, kind = 0;
    if (kw) {
      char nm[64];
      if (sig_name_after(kw, nm, sizeof nm)) {
        bool dup = false;
        for (i32 i = 0; i < g_nsigs && !dup; i++)
          if (strcmp(g_sigs[i].name, nm) == 0) dup = true;
        if (!dup && g_nsigs < WC_MAX_SIGS) {
          SigDecl *d = &g_sigs[g_nsigs++];
          snprintf(d->name, sizeof d->name, "%s", nm);
          snprintf(d->where, sizeof d->where, "%s", path);
          d->line = line_no;
          d->kind = kind;
        }
      }
    }
    if (!nl) break;
    l = nl + 1;
  }
}

// Bounded: a `str` slice of a bigger buffer must not be searched past its end, or every
// line before the one that contains the write would count it too.
static i32 count_needle(str hay, const char *needle) {
  size_t nl = strlen(needle);
  if (!nl || hay.n < nl) return 0;
  i32 n = 0;
  for (size_t i = 0; i + nl <= hay.n; i++)
    if (memcmp(hay.p + i, needle, nl) == 0) {
      n++;
      i += nl - 1;
    }
  return n;
}

// A live binding reads its signal without ever saying `get(name)`: the node the view
// declared *is* the reader (`text_live("%lld", count)`), so a write reaches the page through
// it. The signal is the last argument of the `*_live(` call, matched as a whole identifier so
// `count_extra` is not counted for `count`.
static bool live_arg_is(str seg, const char *name) {
  while (seg.n && (*seg.p == ' ' || *seg.p == '\t')) seg.p++, seg.n--;
  while (seg.n && (seg.p[seg.n - 1] == ' ' || seg.p[seg.n - 1] == '\t')) seg.n--;
  size_t nl = strlen(name);
  return seg.n == nl && memcmp(seg.p, name, nl) == 0;
}

static i32 live_reads(str line, const char *name) {
  static const char *const verbs[] = {"text_live(", "attr_live(", "class_live(", "show_live("};
  i32 hits = 0;
  for (size_t v = 0; v < sizeof verbs / sizeof *verbs; v++) {
    size_t vl = strlen(verbs[v]);
    for (size_t i = 0; i + vl <= line.n; i++) {
      if (memcmp(line.p + i, verbs[v], vl) != 0) continue;
      size_t start = i + vl, end = line.n, comma = 0;
      i32 depth = 1;
      bool have = false;
      size_t j = start;
      for (; j < line.n; j++) {
        char c = line.p[j];
        if (c == '(') depth++;
        else if (c == ')' && --depth == 0) { end = j; break; }
        else if (c == ',' && depth == 1) { comma = j; have = true; }
      }
      if (have) start = comma + 1;
      if (start < end && live_arg_is((str){line.p + start, end - start}, name)) hits++;
      i = j; // past this call
    }
  }
  return hits;
}

// Braces outside strings and character literals, over one line whose comment is already
// gone: `text("}")` is not a block ending, and a pass that thought it was would place every
// row after it outside the list.
static void brace_counts(const char *s, i32 *opens, i32 *closes) {
  for (const char *q = s; *q; q++) {
    if (*q == '"' || *q == '\'') {
      char close = *q;
      for (q++; *q && *q != close; q++)
        if (*q == '\\' && q[1]) q++;
      if (!*q) break; // an unterminated literal stops here; the rest of the line is code
      continue;
    }
    if (*q == '{') (*opens)++;
    else if (*q == '}') (*closes)++;
  }
}

// The three macros that render one row per element. A live binding written inside one is
// declared once per row and resolved once per page, so every row shows the same value —
// which is what `state_own` exists to fix, and exactly what the compiler cannot see.
static bool opens_rows(const char *code) {
  static const char *const loops[] = {"for_each(", "for_each_ptr(", "for_i("};
  for (size_t i = 0; i < WC_COUNTOF(loops); i++) {
    const char *p = strstr(code, loops[i]);
    if (!p) continue;
    if (p > code && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue; // my_for_i(
    return true;
  }
  return false;
}

// Reads are counted per line, and a line that also writes the signal only feeds its own
// write. That is the whole point: `set(tick, get(tick) + 1)` reads `tick`, which is why a
// plain "never read" rule misses the idiom the framework teaches.
static void sig_count(const char *path, str text, void *ctx) {
  (void)path;
  (void)ctx;
  // Where the reader sits: a live binding inside a list is the bug this steers around.
  i32 depth = 0, loops[8] = {0}, nloops = 0;
  for (const char *l = text.p; l && *l;) {
    const char *nl = strchr(l, '\n');
    size_t n = nl ? (size_t)(nl - l) : strlen(l);
    str line = {l, n};
    char code[512];
    size_t cn = n < sizeof code - 1 ? n : sizeof code - 1;
    memcpy(code, l, cn);
    code[cn] = 0;
    strip_comment(code);
    bool in_loop = nloops > 0 || opens_rows(code);
    i32 opens = 0, closes = 0;
    brace_counts(code, &opens, &closes);
    if (nl) {
      l = nl + 1;
    }
    for (i32 i = 0; i < g_nsigs; i++) {
      SigDecl *d = &g_sigs[i];
      char q[128];
      i32 w = 0;
      snprintf(q, sizeof q, "set(%s,", d->name);      w += count_needle(line, q);
      snprintf(q, sizeof q, "set(%s)", d->name);      w += count_needle(line, q);
      snprintf(q, sizeof q, "set_%s(", d->name);      w += count_needle(line, q);
      snprintf(q, sizeof q, "set(%s,", d->name);     w += count_needle(line, q);
      snprintf(q, sizeof q, "toggle(%s)", d->name);   w += count_needle(line, q);
      d->writes += w;
      if (w) continue; // a write's own get() is not a reader
      i32 r = 0;
      snprintf(q, sizeof q, "get(%s)", d->name);      r += count_needle(line, q);
      snprintf(q, sizeof q, "get_%s(", d->name);      r += count_needle(line, q);
      snprintf(q, sizeof q, "get(%s)", d->name);     r += count_needle(line, q);
      i32 lr = live_reads(line, d->name);
      d->reads += r + lr;
      if (in_loop) d->live_in_loop += lr;
    }
    depth += opens - closes;
    if (opens_rows(code) && opens > closes) {
      if (nloops < 8) loops[nloops++] = depth;
    }
    while (nloops > 0 && depth < loops[nloops - 1]) nloops--;
    if (!nl) break;
  }
}

// Shared by both passes; `pass` 0 collects declarations, 1 counts uses.
static int g_sig_pass;
static void sig_visit(const char *path, str text, void *ctx) {
  if (g_sig_pass == 0) sig_collect(path, text, ctx);
  else sig_count(path, text, ctx);
}

static void signals_check(const char *src, Findings *fs) {
  g_nsigs = 0;
  g_sig_pass = 0;
  source_walk(src, sig_visit, nullptr);
  if (!g_nsigs) return;
  g_sig_pass = 1;
  source_walk(src, sig_visit, nullptr);
  for (i32 i = 0; i < g_nsigs; i++) {
    SigDecl *d = &g_sigs[i];
    // A list is the one place the three declarations differ, so it is where the pass has
    // something to say that no type can: `text_live(count)` inside a for_each gives every
    // row the page's one `count`. The app meant one per row and has one per page.
    if (d->kind != 2 && d->writes > 0 && d->live_in_loop > 0) {
      char fix[320];
      snprintf(fix, sizeof fix,
             "%s(%s, …) is read by %d live binding(s) inside a list — the row is the position, "
             "and the declaration is not: every row shows the same value. "
             "state_own(i64, %s, …) declares it once per row, and get(%s)/set(%s) then "
             "address the row the action belongs to (reference §11a)",
             d->kind == 0 ? "state" : "state_in", d->name, d->live_in_loop, d->name, d->name,
             d->name);
      finding(fs, false, d->where, d->line,
              "a live binding on a page-wide signal inside a list, so every row shows one "
              "value",
              fix);
      continue;
    }
    if (d->writes == 0 || d->reads > 0) continue;
    char fix[256];
    snprintf(fix, sizeof fix,
             "set(%s, …) is written %d time(s) and only its own write reads it — a view "
             "re-renders when an action returns (reference §11a), so this write is not "
             "what updates the page",
             d->name, d->writes);
    finding(fs, false, d->where, d->line,
            "a signal written and never read, so the view never sees it", fix);
  }
}

// ---- components: a part called twice in one function shares one position
//
// `comp(Name, …)` makes a call its own render position, which is the only thing that gives
// the state declared inside a part one copy per call (reference §11d). Without it, two calls
// in one view draw into the same position and read the same slot — so a part that declares
// render-owned state and is called more than once in one function is the shape that surprises
// people, and the call sites are the only place it shows. A note, never an error: a part may
// be called twice on purpose, and the fix is one word at each call.
#define WC_PARTS_MAX 64
typedef struct {
  char name[64];
  bool stateful;      // its definition sits in a file that declares state_own
  bool defined_here;  // this walk saw `void Name(`
} PartDecl;
static PartDecl g_parts[WC_PARTS_MAX];
static i32 g_nparts;

static PartDecl *part_find(const char *name) {
  for (i32 i = 0; i < g_nparts; i++)
    if (strcmp(g_parts[i].name, name) == 0) return &g_parts[i];
  return nullptr;
}

// `PART(Name, …)` — the declaration tuple, wherever it lives.
static void part_collect(const char *path, str text, void *ctx) {
  (void)path;
  (void)ctx;
  for (const char *p = text.p; p && (p = strstr(p, "PART(")); p += 5) {
    if (p > text.p && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue; // MY_PART(
    const char *q = p + 5;
    char name[64];
    size_t k = 0;
    while (*q && *q != ',' && *q != ')' && k + 1 < sizeof name)
      if (!isspace((unsigned char)*q)) name[k++] = *q++;
      else q++;
    name[k] = 0;
    if (!k || part_find(name)) continue;
    if (g_nparts < WC_PARTS_MAX) snprintf(g_parts[g_nparts++].name, sizeof g_parts[0].name, "%s", name);
  }
}

// Which of them keep state: the definition is in a file that declares `state_own`. The pairing
// is per file, which is enough — a part and the state it declares live together (parts.c).
static void part_stateful(const char *path, str text, void *ctx) {
  (void)path;
  (void)ctx;
  if (!strstr(text.p ? text.p : "", "state_own(")) return;
  for (i32 i = 0; i < g_nparts; i++) {
    char sig[96];
    snprintf(sig, sizeof sig, "void %s(", g_parts[i].name);
    if (strstr(text.p, sig)) g_parts[i].stateful = true;
  }
}

typedef struct {
  char file[256];
  char fn[64];
  char part[64];
  i32 line;
  i32 count;
} PartCall;

// The name of the function body this line opens, if it opens one. Views are written three
// ways in this codebase — `view(Name) {`, `component(Name) {`, and a plain `void name(WBuild
// *b) {` — and a component called from any of them draws into that body's one position.
static bool body_name(const char *code, i32 opens, i32 closes, char *out, size_t cap) {
  if (opens <= closes) return false;
  const char *paren = strchr(code, '(');
  if (!paren) return false;
  const char *q = paren;
  while (q > code && (isalnum((unsigned char)q[-1]) || q[-1] == '_')) q--;
  size_t k = (size_t)(paren - q);
  if (!k || k + 1 >= cap) return false;
  char head[64];
  if (k >= sizeof head) return false;
  memcpy(head, q, k);
  head[k] = 0;
  static const char *const views[] = {"view", "component", "page", "layout", "layout_of"};
  for (size_t i = 0; i < WC_COUNTOF(views); i++)
    if (strcmp(head, views[i]) == 0) {
      // the name is the macro's first argument: view(Order) → "view Order"
      const char *a = paren + 1;
      while (*a == ' ') a++;
      size_t n = 0;
      while (a[n] && a[n] != ',' && a[n] != ')' && n + 1 < sizeof head) n++;
      if (!n) return false;
      snprintf(out, cap, "%s %.*s", head, (int)n, a);
      return true;
    }
  if (strcmp(head, "void") == 0) return false; // `void` alone is not a name
  // a plain function: the word before `(` is its name, but only when the line says `void`
  if (!strstr(code, "void ")) return false;
  snprintf(out, cap, "%s", head);
  return true;
}

// How many times a stateful part is called inside one function, per file. The enclosing body
// is tracked by brace depth with strings skipped — the same reading `opens_rows` needs,
// because a `text("}")` must not end a view early.
static void part_calls(const char *path, str text, void *ctx) {
  Findings *fs = ctx;
  static PartCall seen[128];
  i32 nseen = 0;
  i32 depth = 0, fn_depth = -1, line_no = 0;
  char fn[64] = "";
  for (const char *l = text.p; l && *l; line_no++) {
    const char *nl = strchr(l, '\n');
    size_t n = nl ? (size_t)(nl - l) : strlen(l);
    char code[512];
    size_t cn = n < sizeof code - 1 ? n : sizeof code - 1;
    memcpy(code, l, cn);
    code[cn] = 0;
    strip_comment(code);
    i32 opens = 0, closes = 0;
    brace_counts(code, &opens, &closes);

    char body[64];
    if (body_name(code, opens, closes, body, sizeof body)) {
      snprintf(fn, sizeof fn, "%s", body);
      fn_depth = depth;
    }

    bool is_decl = strstr(code, "PART(") != nullptr || strstr(code, "void ") != nullptr;
    bool comp_line = strstr(code, "comp(") != nullptr;
    if (!is_decl && !comp_line && fn_depth >= 0 && depth >= fn_depth) {
      for (i32 i = 0; i < g_nparts; i++) {
        if (!g_parts[i].stateful || strcmp(g_parts[i].name, fn) == 0) continue;
        char sig[96];
        snprintf(sig, sizeof sig, "%s(", g_parts[i].name);
        i32 calls = count_needle((str){l, n}, sig);
        if (!calls) continue;
        i32 j = 0;
        for (; j < nseen; j++)
          if (strcmp(seen[j].part, g_parts[i].name) == 0 && strcmp(seen[j].fn, fn) == 0 &&
              strcmp(seen[j].file, path) == 0)
            break;
        if (j == nseen) {
          if (nseen >= (i32)WC_COUNTOF(seen)) continue;
          snprintf(seen[j].file, sizeof seen[j].file, "%s", path);
          snprintf(seen[j].fn, sizeof seen[j].fn, "%s", fn);
          snprintf(seen[j].part, sizeof seen[j].part, "%s", g_parts[i].name);
          seen[j].line = line_no;
          seen[j].count = 0;
          nseen++;
        }
        seen[j].count += calls;
      }
    }

    depth += opens - closes;
    if (fn_depth >= 0 && depth <= fn_depth) fn_depth = -1;
    if (!nl) break;
    l = nl + 1;
  }
  for (i32 i = 0; i < nseen; i++) {
    if (seen[i].count < 2) continue;
    char what[224], fix[288];
    snprintf(what, sizeof what,
             "%s is called %d times in %s() and declares state_own — every call draws into "
             "the same position, so they share the state",
             seen[i].part, seen[i].count, seen[i].fn);
    snprintf(fix, sizeof fix,
             "comp(%s, …) at the calls that each need their own, or state(…) if one value "
             "for the page is what you meant (reference §11d)",
             seen[i].part);
    finding(fs, false, seen[i].file, seen[i].line, what, fix);
  }
}

static void parts_check(const char *src, Findings *fs) {
  g_nparts = 0;
  source_walk(src, part_collect, nullptr);
  if (!g_nparts) return;
  source_walk(src, part_stateful, nullptr);
  for (i32 i = 0; i < g_nparts; i++)
    if (g_parts[i].stateful) {
      source_walk(src, part_calls, fs);
      return;
    }
}

static int consistency(const char *src, bool verbose, const Config *cfg, bool styles) {
  static Findings fs_zero;
  Findings fs = fs_zero;
  static StrList named, bound; // off the stack: these grew with the wiring check
  memset(&named, 0, sizeof named);
  memset(&bound, 0, sizeof bound);
  RoutePrefix routes[16];
  i32 nroutes = 0;
  scan_app(src, &fs, &named, &bound, routes, &nroutes);
  memset(&g_wire, 0, sizeof g_wire);
  wiring_app(src, &g_wire);
  // (the bind-without-a-name error is the wiring check's first rule below — one set of
  // tables, so a component's hook counts as a name the same as named()'s does)

  // ---- the wiring check (P2): names, binds, keys, fields, actions — reconciled
  // These are notes, not errors: a control driven only from the palette, or a name the
  // document verbs use (focus, reveal), is a design choice — the check's job is to make
  // the choice visible, not to forbid it.
  for (i32 i = 0; i < g_wire.bound.n; i++)
    if (!list_has(&g_wire.named, g_wire.bound.v[i]))
      findingf(&fs, true, "src", 0, "bind to a name no view declares — the control cannot fire",
               "named(\"%s\") in a view, or the spelling drifted", g_wire.bound.v[i]);
  for (i32 i = 0; i < g_wire.named.n; i++) {
    const char *n = g_wire.named.v[i];
    if (list_has(&g_wire.bound, n)) continue;
    if (list_has(&g_wire.hosthooks, n)) continue; // focus_hook/reveal_hook is a wiring too
    findingf(&fs, false, "src", 0, "a name no binding answers — clicking it does nothing",
             "bind(\"%s\", \"click\", …) in the client half, or remove the name", n);
  }
  for (i32 i = 0; i < g_wire.fields.n; i++)
    if (!list_has(&g_wire.fields_bound, g_wire.fields.v[i]))
      findingf(&fs, false, "src", 0, "a field no action reads — typing into it does nothing",
               "bind_field(\"%s\", …) in the client half", g_wire.fields.v[i]);
  for (i32 i = 0; i < g_wire.fields_bound.n; i++)
    if (!list_has(&g_wire.fields, g_wire.fields_bound.v[i]))
      findingf(&fs, true, "src", 0, "bind_field to a name no input carries",
               "named_field(\"%s\") in the view", g_wire.fields_bound.v[i]);
  for (i32 i = 0; i < g_wire.actions.n; i++)
    if (!list_has(&g_wire.wired, g_wire.actions.v[i]))
      findingf(&fs, false, "src", 0, "an action nothing wires — defined, never reachable",
               "bind/bind_field/on_key should name \"%s\", or it is dead code", g_wire.actions.v[i]);
  snprintf(src_root_for_styles, sizeof src_root_for_styles, "%s", src);
  if (styles) styles_check(cfg, &g_wire, &fs);
  signals_check(src, &fs);
  parts_check(src, &fs);
  for (i32 i = 0; i < g_wire.keys.n; i++) {
    if (!key_spec_ok(g_wire.keys.v[i]))
      findingf(&fs, true, "src", 0, "an on_key spec the bridge would never match",
               "one modifier (meta/ctrl/alt/shift/mod) plus one key — \"%s\" is not", g_wire.keys.v[i]);
    for (i32 j = i + 1; j < g_wire.keys.n; j++)
      if (strcmp(g_wire.keys.v[i], g_wire.keys.v[j]) == 0)
        findingf(&fs, false, "src", 0, "the same key bound twice — the second wins",
                 "on_key(\"%s\", …) appears more than once", g_wire.keys.v[i]);
  }
  if (fs.n == 0) {
    printf("clean\n");
    return 0;
  }
  printf("%d note%s (%d error%s)%s\n", fs.n, fs.n == 1 ? "" : "s", fs.errors,
         fs.errors == 1 ? "" : "s", verbose ? ":" : " — webc check --verbose lists them");
  if (verbose) {
    for (i32 i = 0; i < fs.n; i++) {
      printf("\n    %s %s\n      %s\n      fix: %s\n", fs.f[i].error ? "error" : "note",
             fs.f[i].where, fs.f[i].what, fs.f[i].fix);
    }
    printf("\n");
  }
  return fs.errors ? 1 : 0;
}

// `webc check` is the gate an agent runs after every change, so its answer has to be
// both human and machine. Each step is captured: a person sees the one line and, when a
// step fails, everything it said; a tool sees the same thing as `{steps:[{name, ok,
// output}]}`. One implementation, two renderings — the alternative is a JSON mode that
// drifts from what the command actually did.
typedef struct CkStep {
  const char *name;
  bool ok;
  bool skipped;
  char *output;
} CkStep;

static void ck_add(CkStep *steps, i32 *n, const char *name, bool ok, bool skipped, char *output) {
  CkStep *st = &steps[(*n)++];
  st->name = name;
  st->ok = ok;
  st->skipped = skipped;
  st->output = output;
}

static int cmd_check(bool verbose, bool wiring_only, bool json) {
  Config c;
  config_load(&c, ".");
  // the fast path: the wiring and style notes only, no build, no tests — the pass an
  // editor runs on save. (`webc check` proper still runs everything.)
  if (wiring_only) {
    if (!json) printf("  %-14s ", "wiring");
    WCap cap;
    wcap_begin(&cap);
    int w = consistency(c.src, verbose, &c, true);
    char *out = wcap_end(&cap);
    if (json) {
      Buf j;
      buf_init(&j, nullptr, 4096);
      buf_printf(&j, "{\"ok\":%s,\"steps\":[{\"name\":\"wiring\",\"ok\":%s,\"output\":",
                 w ? "false" : "true", w ? "false" : "true");
      jputs(&j, out ? out : "");
      buf_puts(&j, "}]}\n");
      printf("%s", buf_cstr(&j));
      buf_free(&j);
      free(out);
      return w;
    }
    printf("%s\n", w ? "notes" : "clean");
    if (out && *out) printf("%s", out);
    free(out);
    return w;
  }

  CkStep steps[10];
  i32 n = 0, problems = 0;
  if (!json) printf("\n%s  webc check%s\n\n", g_b, g_x);

  char bin[1024];
  snprintf(bin, sizeof bin, "%s/.webc/%s", c.root, c.name);

  WCap cap;
  wcap_begin(&cap);
  bool built = build(&c, nullptr, false, bin);
  char *build_out = wcap_end(&cap);
  ck_add(steps, &n, "build", built, false, build_out);
  if (!built) {
    // nothing downstream can answer: report what we have and stop
    if (json) {
      Buf j;
      buf_init(&j, nullptr, 16384);
      buf_puts(&j, "{\"ok\":false,\"steps\":[");
      buf_puts(&j, "{\"name\":\"build\",\"ok\":false,\"output\":");
      jputs(&j, build_out);
      buf_puts(&j, "}]}\n");
      printf("%s", buf_cstr(&j));
      buf_free(&j);
      return 1;
    }
    printf("  %-14s FAILED\n", "build");
    if (build_out && *build_out) printf("%s", build_out);
    return 1;
  }

  bool have_bin = path_exists(bin), have_wasm = path_exists(".webc/app.wasm");
  char bin_note[256] = "";
  if (have_bin && have_wasm) snprintf(bin_note, sizeof bin_note, "(%s, .webc/app.wasm)", bin);
  else if (have_bin) snprintf(bin_note, sizeof bin_note, "(%s)", bin);
  else if (have_wasm) snprintf(bin_note, sizeof bin_note, "(.webc/app.wasm)");

  bool server_half = c.has_sets ? c.server.n > 0 : (c.entry[0] && path_exists(c.entry));

  wcap_begin(&cap);
  int t = cmd_test((TestArgs){0});
  ck_add(steps, &n, "tests", t == 0, false, wcap_end(&cap));
  if (t != 0) problems++;

  wcap_begin(&cap);
  int d = cmd_doctor(false, false);
  ck_add(steps, &n, "doctor", d == 0, false, wcap_end(&cap));
  if (d != 0) problems++;

  if (!server_half) {
    ck_add(steps, &n, "boot", true, true, nullptr);
    ck_add(steps, &n, "schema", true, true, nullptr);
  } else {
    wcap_begin(&cap);
    int b = run_boot_hook("WEBC_BOOT_ONLY", false);
    ck_add(steps, &n, "boot", b == 0, false, wcap_end(&cap));
    if (b != 0) problems++;
    wcap_begin(&cap);
    int sc = run_boot_hook("WEBC_SCHEMA_CHECK", false);
    ck_add(steps, &n, "schema", sc == 0, false, wcap_end(&cap));
    if (sc != 0) problems++;
  }

  wcap_begin(&cap);
  int nroutes = count_routes_in(c.src);
  char *rout = wcap_end(&cap);
  free(rout);
  char rnote[64];
  snprintf(rnote, sizeof rnote, "%d declared", nroutes);
  ck_add(steps, &n, "routes", nroutes > 0, false, nullptr);

  wcap_begin(&cap);
  int l = cmd_lint(false, false, false);
  ck_add(steps, &n, "lint", l == 0, false, wcap_end(&cap));
  if (l != 0) problems++;

  wcap_begin(&cap);
  int cs = consistency(c.src, verbose, &c, true);
  ck_add(steps, &n, "consistency", cs == 0, false, wcap_end(&cap));
  if (cs != 0) problems++;

  if (json) {
    Buf j;
    buf_init(&j, nullptr, 32768);
    buf_printf(&j, "{\"ok\":%s,\"name\":", problems ? "false" : "true");
    jputs(&j, c.name);
    buf_puts(&j, ",\"steps\":[");
    for (i32 i = 0; i < n; i++) {
      if (i) buf_puts(&j, ",");
      buf_puts(&j, "{\"name\":");
      jputs(&j, steps[i].name);
      buf_printf(&j, ",\"ok\":%s,\"skipped\":%s", steps[i].ok ? "true" : "false",
                 steps[i].skipped ? "true" : "false");
      if (!strcmp(steps[i].name, "build") && bin_note[0]) {
        buf_puts(&j, ",\"artifact\":");
        jputs(&j, bin_note);
      }
      if (!strcmp(steps[i].name, "routes")) {
        buf_printf(&j, ",\"routes\":%d", nroutes);
      }
      buf_puts(&j, ",\"output\":");
      jputs(&j, steps[i].output ? steps[i].output : "");
      buf_puts(&j, "}");
    }
    buf_puts(&j, "]}\n");
    printf("%s", buf_cstr(&j));
    buf_free(&j);
    for (i32 i = 0; i < n; i++) free(steps[i].output);
    return problems ? 1 : 0;
  }

  for (i32 i = 0; i < n; i++) {
    CkStep *st = &steps[i];
    printf("  %-14s ", st->name);
    if (st->skipped) {
      printf("skipped (%s)\n", !strcmp(st->name, "boot")
                                    ? "browser-only: no server half to run"
                                    : "no database in the browser half");
    } else if (!strcmp(st->name, "build")) {
      printf("ok %s\n", bin_note);
    } else if (!strcmp(st->name, "routes")) {
      printf("%s\n", rnote);
    } else if (st->ok) {
      printf("ok\n");
    } else {
      printf("%sFAILED%s\n", g_r, g_x);
      if (st->output && *st->output) printf("%s", st->output);
    }
  }
  for (i32 i = 0; i < n; i++) free(steps[i].output);
  if (problems) {
    printf("\n  %s%d problem(s)%s\n\n", g_b, problems, g_x);
    return 1;
  }
  printf("\n  %sall clear%s — next: webc dev\n\n", g_g, g_x);
  return 0;
}

static int cmd_fmt(bool check) {
  Config c;
  config_load(&c, ".");
  // A project's style is its own file. Formatting to a style nobody wrote down would be a
  // 900-line diff in a stranger's house, so say what is missing and where to get one.
  if (!path_exists(".clang-format")) {
    fprintf(stderr, "webc: no .clang-format here — `webc fmt` formats to the project's style "
                    "file and will not pick one for you.\n"
                    "      copy the framework's and it works: cp %s/.clang-format .\n",
            webc_prefix());
    return 1;
  }
  char cmd[2048];
  // Xcode ships clang-format without putting it on PATH. The file list comes from `find`
  // rather than a glob: a project that keeps its sources in folders is deeper than `*/*.c`,
  // and a shell without globstar would silently format the top level and nothing else.
  snprintf(cmd, sizeof cmd,
           "CF=$(command -v clang-format || xcrun --find clang-format 2>/dev/null); "
           "[ -n \"$CF\" ] || { echo 'webc: clang-format not found (brew install clang-format)' "
           ">&2; exit 0; }; "
           "FILES=$(find %s \\( -name '*.c' -o -name '*.h' \\)) ; "
           "[ -n \"$FILES\" ] || exit 0; "
           "\"$CF\" %s --style=file:.clang-format -i $FILES 2>/dev/null",
           check ? "--dry-run --Werror" : "", c.src);
  return system(cmd) == 0 ? 0 : 1;
}

// ---------------------------------------------------------------- dev
//
// `webc dev` is one process with three jobs: watch the project, build only what changed,
// and tell the browser what to do about it. What is on the wire is line-JSON over the
// websocket at /ws:
//
//   {"t":"status","state":"building"|"ok"|"error","ms":N}
//   {"t":"error","text":"…","items":[{"file":"src/view.c","line":82,"msg":"…"}]}
//   {"t":"css","href":"/style.css"}            re-fetch one stylesheet, no reload
//   {"t":"wasm","url":"/app.wasm?v=…","keep":"dom"}   stage the new module, then swap
//   {"t":"reload","why":"config"}              last resort: the tab reloads itself
//
// A CSS edit never compiles anything and never reloads the page. A source edit compiles
// one half of the app, and for the browser half the page keeps its DOM and its signals
// (see assets/dev.js and the swap path in assets/webc.js). A build error touches nothing
// that is running: the tab shows the error over the last good build.
//
// dev.js understands the bare `reload` and `css:<href>` messages too, so a tab running a
// copy of the script from before a CLI upgrade keeps working.

typedef struct DevState {
  i32 dev_port;
  i32 app_port;
  bool web; // no server half: the output is a module and there is nothing to restart
  char out[1024];
  char public_dir[512];
  pid_t child;
  i32 listen_fd; // the supervisor owns it, so a restart never closes the socket
  char status[32];
} DevState;

static DevState D = {.listen_fd = -1, .child = -1};

static i32 g_ws_fds[64];
static i32 g_nws;
static WMutex g_ws_mu = WEBC_MUTEX_INIT;

static void ws_broadcast(const char *msg) {
  wmutex_lock(&g_ws_mu);
  if (trace_on()) info("ws -> %s", msg);
  for (i32 i = 0; i < g_nws; i++)
    if (g_ws_fds[i] > 0) ws_send_frame(g_ws_fds[i], 0x1, str_lit(msg));
  wmutex_unlock(&g_ws_mu);
}

static void json_esc(Buf *b, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') buf_printf(b, "\\%c", c);
    else if (c == '\n') buf_puts(b, "\\n");
    else if (c == '\r') buf_puts(b, "\\r");
    else if (c == '\t') buf_puts(b, "\\t");
    else if (c < 0x20) buf_printf(b, "\\u%04x", c);
    else buf_putc(b, (char)c);
  }
}

static void dev_status(const char *state, i64 ms) {
  snprintf(D.status, sizeof D.status, "%s", state);
  Buf b;
  buf_init(&b, nullptr, 128);
  if (ms >= 0)
    buf_printf(&b, "{\"t\":\"status\",\"state\":\"%s\",\"ms\":%lld}", state, (long long)ms);
  else
    buf_printf(&b, "{\"t\":\"status\",\"state\":\"%s\"}", state);
  ws_broadcast(buf_cstr(&b));
  buf_free(&b);
}

static void dev_css(const char *href) {
  Buf b;
  buf_init(&b, nullptr, 256);
  buf_puts(&b, "{\"t\":\"css\",\"href\":\"");
  json_esc(&b, href, strlen(href));
  buf_puts(&b, "\"}");
  ws_broadcast(buf_cstr(&b));
  buf_free(&b);
}

static void dev_wasm(const char *url) {
  Buf b;
  buf_init(&b, nullptr, 256);
  // `keep: state` — the module is swapped, the signals and the app's own saved state cross,
  // and the view re-renders into the container it already owns. `keep: dom` would also carry
  // the rendered tree across so the first render is a diff; it is opt-in (WEBC_DOM_KEEP=1)
  // because the node-for-node alignment still needs work on trees with empty fragments.
  const char *keep = getenv("WEBC_DOM_KEEP") ? "dom" : "state";
  buf_printf(&b, "{\"t\":\"wasm\",\"url\":\"%s\",\"keep\":\"%s\"}", url, keep);
  ws_broadcast(buf_cstr(&b));
  buf_free(&b);
}

static void dev_reload(const char *why) {
  Buf b;
  buf_init(&b, nullptr, 128);
  buf_printf(&b, "{\"t\":\"reload\",\"why\":\"%s\"}", why);
  ws_broadcast(buf_cstr(&b));
  buf_free(&b);
}

// A compile failed. The browser gets the compiler's own words, split into
// file/line/message where clang used the usual "path:line:col: error: text" shape, so the
// overlay can be a list instead of a wall of text.
static void dev_error(const char *log);
static i32 dev_explain_rows(str text, char rows[][600], i32 cap);
static i64 dev_line_of(const char *where);

static void dev_error(const char *log) {
  Buf b;
  buf_init(&b, nullptr, 1024);
  buf_puts(&b, "{\"t\":\"error\",\"text\":\"");
  json_esc(&b, log, strlen(log));
  buf_puts(&b, "\",\"items\":[");
  i32 items = 0;

  // The tab gets the translator's words before it gets the compiler's: `webc explain`
  // is the promise that a failure arrives in WebC's sentences, and the overlay is
  // where a developer reads it. The engine prints prose (ANSI-coloured, indented),
  // so its stdout is captured to a file and read back — one diagnostic engine, no
  // second implementation, and the raw parse below stays as the fallback for the
  // sentences the engine does not have.
  char rows[12][600]; // file:line + the sentence, "|"-joined
  i32 nrows = 0;
  {
    char tmpl[] = "/tmp/.webc_dev_explXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
      int saved = dup(1);
      fflush(stdout);
      dup2(fd, 1);
      bool better = explain_compiler_output(".", log);
      fflush(stdout);
      dup2(saved, 1);
      close(saved);
      close(fd);
      if (better) {
        Arena *ea = arena_new(64 * 1024);
        str text = read_file(ea, tmpl);
        nrows = dev_explain_rows(text, rows, 12);
        arena_destroy(ea);
      }
      unlink(tmpl);
    }
  }

  for (i32 i = 0; i < nrows && items < 20; i++) {
    char *bar = strchr(rows[i], '|');
    if (!bar) continue;
    *bar = 0;
    if (items) buf_putc(&b, ',');
    buf_puts(&b, "{\"file\":\"");
    json_esc(&b, rows[i], strlen(rows[i]));
    buf_printf(&b, "\",\"line\":%lld,\"msg\":\"", (long long)dev_line_of(rows[i]));
    json_esc(&b, bar + 1, strlen(bar + 1));
    buf_puts(&b, "\"}");
    items++;
  }

  if (nrows == 0) {
  // one entry per distinct diagnostic: clang prints the same line twice for a syntax
  // error (once as `path:line:col: error:` and once as the summary), and two identical
  // rows in the overlay look like two mistakes.
  char seen[16][320];
  i32 nseen = 0;
  const char *p = log;
  while (*p && items < 20) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    // `path:line:col: error|warning|fatal error: message` — three fields, and the second
    // must be a number or this line is prose from the compiler, not a diagnostic.
    const char *c1 = nullptr, *c2 = nullptr;
    for (const char *q = p; q < p + len; q++) {
      if (*q != ':') continue;
      if (!c1) c1 = q;
      else if (!c2) c2 = q;
      else break;
    }
    long line = 0;
    const char *msg = nullptr;
    if (c1 && c2) {
      const char *d = c1 + 1;
      line = (*d >= '0' && *d <= '9') ? strtol(d, nullptr, 10) : 0;
      const char *sev = c2 + 1;
      while (*sev == ' ') sev++;
      if (strncmp(sev, "error:", 6) == 0 || strncmp(sev, "fatal error:", 12) == 0 ||
          strncmp(sev, "warning:", 8) == 0) {
        const char *mc = strchr(sev, ':');
        if (mc) {
          msg = mc + 1;
          while (*msg == ' ') msg++;
        }
      }
    }
    if (msg && line > 0) {
      size_t flen = (size_t)(c1 - p);
      // one row per site: clang reports a syntax error once with the caret drawing and
      // once as a summary line, and two rows for one mistake is noise in the overlay
      char key[320];
      snprintf(key, sizeof key, "%.*s:%ld", (int)flen, p, line);
      bool dup = false;
      for (i32 k = 0; k < nseen; k++)
        if (strcmp(seen[k], key) == 0) dup = true;
      if (!dup && nseen < 16) snprintf(seen[nseen++], sizeof seen[0], "%s", key);
      if (!dup) {
        if (items) buf_putc(&b, ',');
        buf_puts(&b, "{\"file\":\"");
        json_esc(&b, p, flen);
        buf_printf(&b, "\",\"line\":%ld,\"msg\":\"", line);
        json_esc(&b, msg, strcspn(msg, "\n"));
        buf_puts(&b, "\"}");
        items++;
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  } // (the raw parse above is the fallback when the translator had nothing)
  buf_puts(&b, "]}");
  ws_broadcast(buf_cstr(&b));
  buf_free(&b);
  dev_status("error", -1);
}

// The translator prints `  path:line:col` and then indented sentences. Turn that into
// overlay rows ("file:line|sentence"), stripping the ANSI colour codes the terminal
// path puts in — the capture file has them, the wire should not.
static i32 dev_explain_rows(str text, char rows[][600], i32 cap) {
  i32 n = 0;
  i32 cur = -1; // the row a sentence belongs to; -1 = none open
  size_t i = 0;
  while (i < text.n && n < cap) {
    size_t e = i;
    while (e < text.n && text.p[e] != '\n') e++;
    // strip ANSI (ESC [ … letter) and leading space in one pass
    char clean[512];
    size_t cn = 0;
    for (size_t k = i; k < e && cn < sizeof clean - 1; k++) {
      if (text.p[k] == 0x1b && k + 1 < e && text.p[k + 1] == '[') {
        while (k < e && !isalpha((unsigned char)text.p[k])) k++;
        continue;
      }
      if (text.p[k] == '\r') continue;
      clean[cn++] = text.p[k];
    }
    clean[cn] = 0;
    // trim
    size_t s = 0, t = cn;
    while (s < t && clean[s] == ' ') s++;
    while (t > s && clean[t - 1] == ' ') t--;
    clean[t] = 0;
    const char *line = clean + s;
    // `path:line:col` — a where-line: the last colon field a small number
    if (line[0] && line[0] != ' ') {
      const char *c1 = strchr(line, ':');
      bool is_where = false;
      char where[300] = "";
      if (c1) {
        const char *d = c1 + 1;
        long ln = (*d >= '0' && *d <= '9') ? strtol(d, nullptr, 10) : 0;
        if (ln > 0) {
          snprintf(where, sizeof where, "%.*s:%ld", (int)(c1 - line), line, ln);
          is_where = true;
        }
      }
      if (is_where) {
        snprintf(rows[n], 600, "%s|", where);
        cur = n;
        n++;
        i = e + 1;
        continue;
      }
    }
    // anything else is a sentence for the open row ("fix …" / "compiler …" included)
    if (cur >= 0 && line[0]) {
      size_t used = strlen(rows[cur]);
      snprintf(rows[cur] + used, 600 - used, "%s%s", used > 1 ? " " : "", line);
    }
    i = e + 1;
  }
  return n;
}

static i64 dev_line_of(const char *where) {
  const char *c = strrchr(where, ':'); // `file:line` — the line is the tail
  return c ? strtoll(c + 1, nullptr, 10) : 0;
}

static bool project_is_web_only(const Config *c) {
  bool server_half = c->has_sets ? c->server.n > 0 : (c->entry[0] && path_exists(c->entry));
  bool client_half = c->client.n > 0 || (c->web[0] && path_exists(c->web));
  return client_half && !server_half;
}

// ---------------------------------------------------------------- watching
//
// One list of files, each tagged with what a change to it means. Building that list once
// per build (rather than keeping a diff of timestamps) is what lets a save be classified
// exactly: a `.css` under public/ is a reskin, a shared source is both halves, a
// `webc.json` is a restart.

enum { WTCH_SERVER = 1, WTCH_CLIENT = 2, WTCH_STATIC = 4, WTCH_CONFIG = 8 };

typedef struct WatchFile {
  char path[1024];
  u64 mtime;
  u16 kinds;
} WatchFile;

typedef struct Watch {
  i32 n;
  WatchFile f[900];
} Watch;

static void watch_add(Watch *w, const char *path, u16 kinds) {
  if (!path || !*path) return;
  u64 m = mtime_of(path);
  if (m == 0) return; // gone: not watched, and a delete shows up as a build error
  for (i32 i = 0; i < w->n; i++) {
    if (strcmp(w->f[i].path, path) == 0) {
      w->f[i].kinds |= kinds;
      return;
    }
  }
  if (w->n >= (i32)(sizeof w->f / sizeof *w->f)) return;
  snprintf(w->f[w->n].path, sizeof w->f[0].path, "%s", path);
  w->f[w->n].mtime = m;
  w->f[w->n].kinds = kinds;
  w->n++;
}

static void watch_walk(Watch *w, const char *dir, u16 kinds) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      watch_walk(w, p, kinds);
      continue;
    }
    watch_add(w, p, kinds);
  }
  closedir(d);
}

static void watch_build(Config *c, Watch *w) {
  memset(w, 0, sizeof *w);
  Sources cs = {0}, ss = {0};
  // A project with no server half has no "other" half: every source is the browser's, and
  // reading the server set anyway would classify an ordinary edit as "both halves changed"
  // — which is a full reload, the one thing the swap exists to avoid.
  if (!project_is_web_only(c)) {
    collect_sources_for(c, &ss, TARGET_SERVER);
    for (i32 i = 0; i < ss.n; i++) watch_add(w, ss.paths[i], WTCH_SERVER);
  }
  collect_sources_for(c, &cs, TARGET_CLIENT);
  for (i32 i = 0; i < cs.n; i++) watch_add(w, cs.paths[i], WTCH_CLIENT);
  // a source in both sets means "both halves changed"
  for (i32 i = 0; i < w->n; i++)
    if ((w->f[i].kinds & WTCH_CLIENT) && (w->f[i].kinds & WTCH_SERVER)) w->f[i].kinds |= WTCH_CONFIG;
  if (c->public_dir[0]) watch_walk(w, c->public_dir, WTCH_STATIC);
  char p[1200];
  // The framework library is the biggest dependency of all and the one nobody thinks
  // about: `make lib` in the WebC checkout replaces libwebc.a, and a `webc dev` that does
  // not watch it keeps serving the old binary — and the old *embedded* webc.js with it, so
  // a bridge fix looks like it changed nothing.
  snprintf(p, sizeof p, "%s/build/libwebc.a", webc_prefix());
  watch_add(w, p, WTCH_CONFIG);
  snprintf(p, sizeof p, "%s/webc.json", c->root);
  watch_add(w, p, WTCH_CONFIG);
  snprintf(p, sizeof p, "%s/migrations", c->root);
  DIR *d = opendir(p);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      char q[1400];
      snprintf(q, sizeof q, "%s/%s", p, e->d_name);
      watch_add(w, q, WTCH_CONFIG);
    }
    closedir(d);
  }
}

// What changed since the snapshot was taken? Returns the OR of the kinds, and records the
// first changed static file so the caller can turn it into an href.
static u16 watch_scan(Watch *w, char *static_path, size_t static_cap) {
  u16 seen = 0;
  if (static_path && static_cap) static_path[0] = 0;
  for (i32 i = 0; i < w->n; i++) {
    u64 m = mtime_of(w->f[i].path);
    if (m == 0 || m == w->f[i].mtime) continue;
    w->f[i].mtime = m;
    if (trace_on()) info("changed: %s", w->f[i].path);
    seen |= w->f[i].kinds;
    if ((w->f[i].kinds & WTCH_STATIC) && static_path && !static_path[0])
      snprintf(static_path, static_cap, "%s", w->f[i].path);
  }
  return seen;
}

// ---------------------------------------------------------------- the app process

// Wait for the child to say it is answering. Returns 0 = ready, 1 = it died, 2 = timeout.
static i32 wait_ready(i32 fd, i32 ms, char *why, size_t why_cap) {
  for (i32 waited = 0; waited < ms; waited += 5) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, 5) > 0) {
      char buf[256];
      i64 n = (i64)read(fd, buf, sizeof buf - 1);
      if (n <= 0) {
        snprintf(why, why_cap, "the app exited during start-up");
        return 1;
      }
      buf[n] = 0;
      if (strncmp(buf, "ready", 5) == 0) return 0;
      snprintf(why, why_cap, "%s", buf);
      return 1;
    }
  }
  snprintf(why, why_cap, "the app did not answer within %d ms", ms);
  return 2;
}

static pid_t dev_spawn(Config *c, char *why, size_t why_cap) {
  i32 rp[2];
  if (pipe(rp) != 0) {
    snprintf(why, why_cap, "pipe() failed");
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(rp[0]);
    close(rp[1]);
    snprintf(why, why_cap, "fork() failed");
    return -1;
  }
  if (pid == 0) {
    close(rp[0]);
    setenv("WEBC_DEV", "1", 1);
    char num[32];
    snprintf(num, sizeof num, "%d", D.dev_port);
    setenv("WEBC_DEV_PORT", num, 1);
    snprintf(num, sizeof num, ":%d", D.app_port);
    setenv("WEBC_ADDR", num, 1);
    setenv("DATABASE_URL", c->database, 1);
    if (c->migrations[0]) setenv("WEBC_MIGRATIONS", c->migrations, 1);
    if (D.listen_fd >= 0) {
      // the inherited listener must survive exec, and the child adopts a dup of it
      fcntl(D.listen_fd, F_SETFD, 0);
      snprintf(num, sizeof num, "%d", D.listen_fd);
      setenv("WEBC_LISTEN_FD", num, 1);
    }
    snprintf(num, sizeof num, "%d", rp[1]);
    setenv("WEBC_READY_FD", num, 1);
    execl(D.out, D.out, (char *)nullptr);
    _exit(127);
  }
  close(rp[1]);
  char msg[256] = {0};
  i32 r = wait_ready(rp[0], 2500, msg, sizeof msg);
  close(rp[0]);
  if (r != 0) {
    kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, 0);
    snprintf(why, why_cap, "%s", msg);
    return -1;
  }
  return pid;
}

// Start the new process before the old one is gone, then retire the old one. During the
// overlap both accept on the same socket, which the kernel handles correctly (each
// connection goes to exactly one of them), so not one request is refused.
static bool dev_restart(Config *c) {
  char why[256] = {0};
  pid_t next = dev_spawn(c, why, sizeof why);
  if (next < 0) {
    info("the new process did not start: %s", why);
    return false;
  }
  if (D.child > 0) {
    kill(D.child, SIGTERM);
    i32 waited = 0;
    for (; waited < 2500; waited += 10) {
      int st;
      if (waitpid(D.child, &st, WNOHANG) == D.child) break;
      usleep(10 * 1000);
    }
    if (waited >= 2500) {
      // A shutdown that does not finish is a bug we want to see, not to hide — but the
      // developer's next save must not queue behind it.
      info("the previous process did not exit in 2.5 s; killing it");
      kill(D.child, SIGKILL);
      waitpid(D.child, nullptr, 0);
    }
  }
  D.child = next;
  return true;
}

static void dev_kill_child(void) {
  if (D.child <= 0) return;
  kill(D.child, SIGTERM);
  for (i32 waited = 0; waited < 1500; waited += 10) {
    int st;
    if (waitpid(D.child, &st, WNOHANG) == D.child) {
      D.child = -1;
      return;
    }
    usleep(10 * 1000);
  }
  kill(D.child, SIGKILL);
  waitpid(D.child, nullptr, 0);
  D.child = -1;
}

// ---------------------------------------------------------------- dev server

static void ws_hold(Response *res, i32 fd, void *ctx) {
  (void)res;
  (void)ctx;
  wmutex_lock(&g_ws_mu);
  i32 slot = -1;
  for (i32 i = 0; i < g_nws; i++)
    if (g_ws_fds[i] <= 0) {
      slot = i;
      break;
    }
  if (slot < 0 && g_nws < 64) slot = g_nws++;
  if (slot >= 0) g_ws_fds[slot] = fd;
  wmutex_unlock(&g_ws_mu);
  if (trace_on()) info("ws client fd=%d slot=%d", fd, slot);
  // A tab that connects is told what the last build did, so it never shows a stale state.
  if (D.status[0]) dev_status(D.status, -1);
  // The socket is non-blocking, so park in the scheduler between reads: holding a
  // worker thread here would starve the dev server. A read of 0 means goodbye.
  char buf[512];
  for (;;) {
    if (!wfd_wait(fd, false, 30000)) continue; // timeout: just wait again
    i64 r = wfd_read(fd, buf, sizeof buf);
    if (r > 0) continue;
    if (r == 0) break;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
    break;
  }
  wmutex_lock(&g_ws_mu);
  if (slot >= 0) g_ws_fds[slot] = -1;
  wmutex_unlock(&g_ws_mu);
  close(fd);
}

static Response ws_reload_handler_impl(Request *req);

typedef struct DevSrv {
  i32 port;
  bool web; // a wasm app: also serve public/ and the module
  char public_dir[512];
  char wasm[512];
  char connect[512]; // dev.connect: CSP origins the app may call
} DevSrv;

static void *dev_server_thread(void *arg) {
  DevSrv *d = arg;
  webc_route_add("GET", "/ws", ws_reload_handler_impl, nullptr, 0);
  if (d->web) {
    // gzip in dev too: the point of a dev server is that the page you test is the page
    // that ships, and "it is 155 KB here and 64 KB in production" is exactly the kind of
    // difference that hides a real problem. Registered first so it wraps the reload
    // script dev.js injects.
    webc_use(mw_compress());
    // dev.js is injected into the HTML, so a rebuild reloads the tab
    webc_use(mw_dev_connect(d->port, (str){d->connect, strlen(d->connect)}));
    webc_use(mw_web_assets(d->wasm));
    webc_use(mw_static("/", d->public_dir));
  }
  char addr[32];
  snprintf(addr, sizeof addr, ":%d", d->port);
  WebcOptions o = {.addr = addr, .quiet = true};
  webc_serve(&o);
  return nullptr;
}


// ---------------------------------------------------------------- the loop

// One save, classified. Returns true if anything was rebuilt.
static void dev_handle(Config *c, Watch *w, u16 changed, const char *static_path, bool web_app) {
  i64 t0 = wc_time_ms();
  bool need_wasm = false, need_native = false, need_reload = false;

  if (changed & WTCH_CONFIG) {
    need_native = !web_app;
    need_wasm = true;
    need_reload = true;
  } else if (changed & (WTCH_SERVER | WTCH_CLIENT)) {
    need_native = (changed & WTCH_SERVER) && !web_app;
    need_wasm = true;
    // A server change rewrites the HTML the page came from, so the document has to come
    // again; a browser-half change is exactly the case the swap exists for.
    need_reload = (changed & WTCH_SERVER) && !web_app;
  } else if (changed & WTCH_STATIC) {
    if (static_path && strstr(static_path, ".css")) {
      // The cheapest win in the whole loop: no compile, no reload, no lost state.
      //
      // The href is the URL the browser already asked for, not the path on disk: the
      // framework mounts public/ at /public and public/assets/ at /assets (boot.c), so a
      // stylesheet at public/style.css is /public/style.css and a bare /style.css is a 404
      // that swaps nothing.
      const char *rel = static_path;
      size_t pl = strlen(c->public_dir);
      if (strncmp(rel, c->public_dir, pl) == 0) {
        rel += pl;
        while (*rel == '/') rel++;
      }
      char href[1200];
      if (strncmp(rel, "assets/", 7) == 0) snprintf(href, sizeof href, "/assets/%s", rel + 7);
      else snprintf(href, sizeof href, "/public/%s", rel);
      dev_css(href);
      info("css %s", href);
      dev_status("ok", 0);
      return;
    }
    // Anything else in public/ may be embedded at compile time (a view can read a file
    // with a generated header), so it is a rebuild; the object cache makes it cheap.
    need_native = !web_app;
    need_reload = true;
    if (web_app) need_wasm = true;
  }
  if (!need_wasm && !need_native && !need_reload) return;

  dev_status("building", -1);
  char log[8192] = {0};
  g_build_log = log;
  g_build_log_cap = sizeof log;
  bool ok = true;
  if (need_native) {
    if (!build(c, nullptr, false, D.out)) ok = false;
    // the wasm half is rebuilt only when the browser half could have changed
    if (ok && changed & (WTCH_SERVER | WTCH_CLIENT | WTCH_CONFIG)) {
      char wasm[1024];
      snprintf(wasm, sizeof wasm, "%s/" WC_WASM_STEM, c->root);
      if (!build_wasm(c, false, wasm)) ok = false;
    }
  } else if (need_wasm) {
    char wasm[1024];
    snprintf(wasm, sizeof wasm, "%s/" WC_WASM_STEM, c->root);
    if (!build_wasm(c, false, wasm)) ok = false;
  }
  g_build_log = nullptr;
  i64 took = wc_time_ms() - t0;

  watch_build(c, w); // pick up new and deleted files before the next scan

  if (!ok) {
    fprintf(stderr, "%s", log);
    info("build failed (%lld ms) — the running page is untouched", (long long)took);
    dev_error(log[0] ? log : "build failed");
    return;
  }
  if (log[0]) fprintf(stderr, "%s", log); // warnings still belong in the terminal
  info("built in %lld ms", (long long)took);
  dev_status("ok", took);

  if (need_native && !dev_restart(c)) {
    dev_error("the new process did not start");
    return;
  }
  if (need_reload) {
    dev_reload((changed & WTCH_CONFIG) ? "config" : "server");
  } else if (need_wasm) {
    // the module is served at /app.wasm (mw_web_assets), not at its path on disk: a
    // cache-buster on the wrong path is a 404 and the tab goes looking for a swap it can
    // never do — the failure looks exactly like "hot reload does nothing"
    char url[128];
    snprintf(url, sizeof url, "/app.wasm?v=%lld", (long long)wc_time_ms());
    dev_wasm(url);
  }
}

static _Atomic bool g_dev_stop;

static void dev_on_signal(int sig) {
  (void)sig;
  atomic_store(&g_dev_stop, true);
}

static int cmd_dev(void) {
  Config c;
  config_load(&c, ".");
  D.web = project_is_web_only(&c);
  D.dev_port = c.dev_port;
  D.app_port = c.port;
  D.listen_fd = -1;
  snprintf(D.out, sizeof D.out, "%s/.webc/%s", c.root, c.name);
  snprintf(D.public_dir, sizeof D.public_dir, "%s", c.public_dir);
  mkdirs(".webc");
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, dev_on_signal);
  signal(SIGINT, dev_on_signal);
  if (path_exists(c.src)) info("watching %s — ctrl-c to stop", c.src);
  else info("watching %s — ctrl-c to stop", path_exists(c.entry) ? c.entry : c.web);

  char log[8192] = {0};
  g_build_log = log;
  g_build_log_cap = sizeof log;
  bool ok;
  if (D.web) {
    char wasm[1024];
    snprintf(wasm, sizeof wasm, "%s/" WC_WASM_STEM, c.root);
    ok = build_wasm(&c, false, wasm);
  } else {
    ok = build(&c, nullptr, false, D.out);
  }
  g_build_log = nullptr;
  if (!ok) {
    fprintf(stderr, "%s", log);
    fprintf(stderr, "webc: the first build failed; fix the error and save\n");
    return 1;
  }

  if (!D.web) {
    // The supervisor owns the listener. The app adopts a dup of it and never binds, so
    // every restart reuses the same socket and the accept queue survives.
    char addr[64];
    snprintf(addr, sizeof addr, ":%d", D.app_port);
    u16 got = 0;
    D.listen_fd = wfd_open_listen(addr, &got);
    if (D.listen_fd < 0) {
      fprintf(stderr, "webc: cannot listen on %s — is another server already using it?\n", addr);
      return 1;
    }
    char why[256] = {0};
    D.child = dev_spawn(&c, why, sizeof why);
    if (D.child < 0) {
      fprintf(stderr, "webc: the app did not start: %s\n", why);
      return 1;
    }
  }

  DevSrv srv = {.port = c.dev_port, .web = D.web};
  if (D.web) {
    snprintf(srv.public_dir, sizeof srv.public_dir, "%s", c.public_dir);
    snprintf(srv.wasm, sizeof srv.wasm, "%s/" WC_WASM_PATH, c.root);
    snprintf(srv.connect, sizeof srv.connect, "%s", c.dev_connect);
  }
  pthread_t th;
  pthread_create(&th, nullptr, dev_server_thread, &srv);
  usleep(80 * 1000);
  // the dev server installed its own handlers: ours must win, or ctrl-c leaves the app behind
  signal(SIGTERM, dev_on_signal);
  signal(SIGINT, dev_on_signal);

  char url[128];
  snprintf(url, sizeof url, "http://localhost:%d", D.web ? c.dev_port : c.port);
  if (D.web)
    printf("\n  \033[1m▸ %s\033[0m  (wasm app, reload server :%d)\n\n", url, c.dev_port);
  else
    printf("\n  \033[1m▸ %s\033[0m  (reload server :%d)\n\n", url, c.dev_port);

  Watch w;
  watch_build(&c, &w);
  bool first = true;
  while (!atomic_load(&g_dev_stop)) {
    struct timespec ts = {0, 120 * 1000 * 1000};
    nanosleep(&ts, nullptr);
    if (atomic_load(&g_dev_stop)) break;
    char sp[1200];
    u16 changed = watch_scan(&w, sp, sizeof sp);
    if (!changed) continue;
    // A save that lands while we are still building the previous one is common (a formatter
    // writing three files): wait half a beat so one edit is one build.
    if (!first) usleep(60 * 1000);
    first = false;
    dev_handle(&c, &w, changed, sp, D.web);
  }
  info("stopping");
  dev_kill_child();
  if (D.listen_fd >= 0) close(D.listen_fd);
  fflush(stdout);
  _exit(0);
}

// ---------------------------------------------------------------- serve
//
// `webc serve` is the app the way a deployment runs it: a release build, no watcher, no
// reload socket — and gzip, because on a real connection the browser's first visit is
// dominated by the bytes of the module and the bridge, and neither one is served
// compressed by any web server that does not do it on purpose.
//
// Two shapes, because a WebC project comes in two shapes:
//
//   web-only (no server half)  this process *is* the server: the framework's own
//                              middleware chain over public/ and .webc/app.wasm
//   with a server half         `webc build --release`, then exec it — the app owns its
//                              chain, and an app that wants gzip adds mw_compress()
//
// The CSP a client-only app needs is not the one a rendered page needs: the document is
// the author's own HTML, with whatever bootstrap script it has in it, so 'unsafe-inline'
// is on for scripts (the same policy `webc dev` uses, without the dev socket). A project
// that calls an API elsewhere names it in `dev.connect`, which this reuses.
static const char *serve_csp(const Config *c, char *buf, size_t cap) {
  const char *more = c->dev_connect[0] ? c->dev_connect : "'self'";
  snprintf(buf, cap,
           "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; "
           "script-src 'self' 'unsafe-inline' 'wasm-unsafe-eval'; connect-src %s",
           more);
  return buf;
}

static int cmd_serve(i32 argc, char **argv) {
  Config c;
  config_load(&c, ".");
  i32 port = c.port > 0 ? c.port : 8080;
  const char *host = nullptr;
  bool build_first = true, quiet = false;
  for (i32 i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
    else if (str_has_prefix_c(str_lit(argv[i]), "--port=")) port = atoi(argv[i] + 7);
    else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
    else if (str_has_prefix_c(str_lit(argv[i]), "--host=")) host = argv[i] + 7;
    else if (strcmp(argv[i], "--no-build") == 0) build_first = false;
    else if (strcmp(argv[i], "--quiet") == 0) quiet = true;
    else {
      fprintf(stderr, "webc: unknown serve flag '%s'\n", argv[i]);
      return 1;
    }
  }
  char addr[128];
  snprintf(addr, sizeof addr, "%s:%d", host && *host ? host : "", port);

  // With a server half, the app is the server: build it as a release binary and hand the
  // process over, so signals, stdout and the exit status are the app's own.
  if (!project_is_web_only(&c)) {
    char bin[1024];
    snprintf(bin, sizeof bin, "%s/.webc/%s", c.root, c.name);
    if (!build_first && !path_exists(bin)) {
      fprintf(stderr, "webc: no build at %s — drop --no-build or run `webc build`\n", bin);
      return 1;
    }
    if (build_first && !build(&c, nullptr, true, bin)) {
      fprintf(stderr, "webc: build failed, nothing served\n");
      return 1;
    }
    // the same environment `webc dev` gives it, minus the dev bits
    setenv("WEBC_ADDR", addr, 1);
    if (c.database[0]) setenv("DATABASE_URL", c.database, 1);
    if (c.migrations[0]) setenv("WEBC_MIGRATIONS", c.migrations, 1);
    unsetenv("WEBC_DEV");
    printf("\n  \033[1m▸ http://%s%s:%d\033[0m   (%s, release)\n\n",
           host && *host ? host : "localhost", host && *host ? "" : "", port, bin);
    fflush(stdout);
    execl(bin, bin, (char *)nullptr);
    fprintf(stderr, "webc: could not run %s\n", bin);
    return 1;
  }

  char wasm[1024];
  snprintf(wasm, sizeof wasm, "%s/%s.wasm", c.root, WC_WASM_STEM);
  if (build_first) {
    char out[1200];
    snprintf(out, sizeof out, "%s/%s", c.root, WC_WASM_STEM);
    if (!build_wasm(&c, true, out)) {
      fprintf(stderr, "webc: build failed, nothing served\n");
      return 1;
    }
  } else if (!path_exists(wasm)) {
    fprintf(stderr, "webc: no module at %s — drop --no-build or run `webc build`\n", wasm);
    return 1;
  }

  // The whole chain, in the order it has to be in: compress wraps everything below it,
  // the security headers describe every response, and the two servers of actual files
  // answer last. `mw_compress` is first so its post-processing is the final word on the
  // body it was handed.
  webc_use_reset();
  webc_use(mw_compress());
  webc_use(mw_headers_default());
  webc_use(mw_web_assets(WC_WASM_PATH));
  webc_use(mw_static("/", c.public_dir));

  if (!quiet) {
    char bridge[1024];
    snprintf(bridge, sizeof bridge, "%s/assets/webc.js", webc_prefix());
    i64 wr = file_bytes(wasm), wg = wr > 0 ? gzip_bytes(wasm) : -1;
    i64 br = file_bytes(bridge), bg = br > 0 ? gzip_bytes(bridge) : -1;
    char a[32], b[32], d[32], e[32], t[32];
    human_size(a, sizeof a, wr);
    human_size(b, sizeof b, wg);
    human_size(d, sizeof d, br);
    human_size(e, sizeof e, bg);
    human_size(t, sizeof t, (wg > 0 ? wg : 0) + (bg > 0 ? bg : 0));
    printf("\n  \033[1m▸ http://localhost:%d\033[0m   (release, gzip on)\n\n", port);
    printf("    %-22s %s   %s gz\n", "module  app.wasm", a, b);
    printf("    %-22s %s   %s gz\n", "bridge  webc.js", d, e);
    printf("    %-22s %s\n", "first load (gz)", t);
    printf("    %-22s %s\n", "static", c.public_dir);
    printf("\n    ctrl-c to stop\n\n");
  }
  char csp_buf[512];
  WebcOptions o = {.addr = addr,
                   .port = port,
                   .quiet = quiet,
                   .csp = serve_csp(&c, csp_buf, sizeof csp_buf)};
  return webc_serve(&o);
}

static Response ws_reload_handler_impl(Request *req) {
  if (str_contains(req_header(req, "upgrade"), str_lit("websocket"))) {
    Response r = res_empty(101);
    str key = req_header(req, "sec-websocket-key");
    str accept = {0};
    u8 digest[20];
    char *buf = arena_alloc(req->arena, key.n + 40);
    snprintf(buf, key.n + 40, "%.*s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", (int)key.n, key.p);
    sha1(buf, strlen(buf), digest);
    base64_encode(req->arena, (str){(char *)digest, 20}, &accept);
    res_header_str(&r, "Upgrade", str_lit("websocket"));
    res_header_str(&r, "Connection", str_lit("Upgrade"));
    res_header_str(&r, "Sec-WebSocket-Accept", accept);
    // keep the handshake headers: a bare res_upgrade() would answer 101 without them
    r.upgraded = true;
    r.upgrade_fn = ws_hold;
    return r;
  }
  return res_text(400, "websocket only");
}

// ---------------------------------------------------------------- api
//
// `webc api` — the public surface, counted.
//
// Every `#define NAME` and every top-level prototype in include/webc/*.h is a name an
// app author may type. That number is what the surface is worth: one way to do something
// means the count falls, and a count that falls without a gate climbs back the first time
// someone adds a convenience. So: print it, write the sorted names to `api.txt`, and let
// `webc api` fail the build when the total grows.

typedef struct {
  char *name;
  bool macro;
} ApiName;

static i32 api_cmp(const void *a, const void *b) {
  return strcmp(((const ApiName *)a)->name, ((const ApiName *)b)->name);
}

typedef struct {
  ApiName *names; // owned
  i32 n, cap;
  i32 macros, protos;
} ApiSet;

static bool api_add(ApiSet *s, const char *name, size_t len, bool is_macro) {
  for (i32 i = 0; i < s->n; i++)
    if (strlen(s->names[i].name) == len && memcmp(s->names[i].name, name, len) == 0) return false;
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 256;
    s->names = realloc(s->names, (size_t)s->cap * sizeof *s->names);
    if (!s->names) die("out of memory");
  }
  char *copy = malloc(len + 1);
  if (!copy) die("out of memory");
  memcpy(copy, name, len);
  copy[len] = 0;
  s->names[s->n].name = copy;
  s->names[s->n].macro = is_macro;
  s->n++;
  if (is_macro) s->macros++;
  else s->protos++;
  return true;
}

static bool api_ident_start(char c) {
  return isalpha((unsigned char)c) || c == '_';
}
static bool api_ident(char c) {
  return isalnum((unsigned char)c) || c == '_';
}

// One header. Two shapes are names: `#define NAME`, and a prototype — a line that
// starts in column 0, carries a `(`, and ends in `;`. A wrapped prototype is counted
// once, at its first line, which is stable across reformatting.
static void api_scan(const char *path, const char *file, ApiSet *s) {
  i64 n = 0;
  char *text = read_all(path, &n);
  if (!text) return;
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    const char *q = p;
    if (len > 7 && memcmp(q, "#define", 7) == 0) {
      const char *id = q + 7;
      while (id < p + len && (*id == ' ' || *id == '\t')) id++;
      if (id < p + len && api_ident_start(*id)) {
        const char *stop = id;
        while (stop < p + len && api_ident(*stop)) stop++;
        if (api_add(s, id, (size_t)(stop - id), true) && getenv("WEBC_API_LIST"))
          printf("    %-40.40s %s\n", file, "");
      }
    } else if (len > 2 && api_ident_start(*q) && q[len - 1] == ';' && memchr(q, '(', len)) {
      const char *paren = memchr(q, '(', len);
      const char *id = paren;
      while (id > q && api_ident(id[-1])) id--;
      if (id < paren && api_ident_start(*id)) api_add(s, id, (size_t)(paren - id), false);
    }
    if (!eol) break;
    p = eol + 1;
  }
  free(text);
}

static bool api_prefixed(const char *nm) {
  return !strncmp(nm, "WC_", 3) || !strncmp(nm, "WEBC_", 5) || nm[0] == '_';
}

// ---------------------------------------------------------------- the standard library
//
// The shelf, as the tool knows it. `webc stdlib` is the index an app author asks for and
// the only place a module is *listed*: a header that is not in this table is core, and a
// header that is here is opt-in, which is the whole distinction the shelf exists to make.
//
// The table is also the budget's seam: `webc api` counts the names these headers spend
// separately from the core surface, so growing the shelf cannot quietly grow the floor.

typedef struct StdlibMod {
  const char *name;  // include/webc/<name>.h
  const char *tier;  // markup | interaction | data | platform
  const char *what;
  bool client;       // the module costs bytes in the browser half (a probe is built for it)
} StdlibMod;

static const StdlibMod STDLIB[] = {
    {"ui", "markup", "switch, segmented control, tab, meter, kbd · badge, stat, empty state, skeleton, icon button", true},
    {"icons", "markup", "stroke icons as data: the ones you draw are the ones that link", true},
    {"table", "markup", "sortable, paginated table: order without owning the rows", true},
    {"overlay", "markup", "dialog, drawer, menu, toast, tip — with the ARIA and the focus", true},
    {"form", "markup", "a form drawn from the model that validates it", true},
    {"theme", "markup", "design tokens and a dark edition that does not flash", true},
    {"keys", "interaction", "one table of shortcuts, bound and documented from the same rows", true},
    {"motion", "interaction", "enter animation and stagger, no script, reduced-motion honoured", true},
    {"dnd", "interaction", "pick a row up and put it down: the drop index, not the maths", true},
    {"live", "interaction", "rooms over SSE: whole documents, coalesced, a dead tab costs nothing", false},
    {"csv", "data", "in and out, quoted correctly, by header name", true},
    {"richtext", "data", "HTML a user wrote, rendered through a policy: parsed, allowed, escaped", true},
    {"ld", "data", "structured data: the article, the site, the trail, the FAQ — one expression", true},
    {"search", "data", "a search box that ANDs its words, escapes a typed %, and ranks the title", false},
    {"i18n", "data", "one app, two languages: catalogs, plurals, direction", true},
    {"upload", "data", "a file arrives and is checked: magic bytes, a name this side makes up", false},
    {"time", "data", "dates, ages and two-unit durations, UTC only and proud of it", true},
    {"mail", "platform", "a real email from one view: SMTP, or a file in the outbox", false},
    {"jobs", "platform", "work that outlives the request: a table, a lease, a retry", false},
    {"limits", "platform", "a rate limit that is one line, and a table that cannot grow", false},
    {"pwa", "platform", "installable and offline: manifest, worker, cache keyed by the build", false},
};
#define STDLIB_N ((i32)(sizeof STDLIB / sizeof STDLIB[0]))

static bool stdlib_is(const char *name) {
  for (i32 i = 0; i < STDLIB_N; i++)
    if (strcmp(STDLIB[i].name, name) == 0) return true;
  return false;
}

static i32 api_stdlib_names(ApiSet *core) {
  ApiSet st = {0};
  i32 hits = 0;
  for (i32 i = 0; i < STDLIB_N; i++) {
    char path[1024];
    snprintf(path, sizeof path, "%s/include/webc/%s.h", webc_prefix(), STDLIB[i].name);
    api_scan(path, STDLIB[i].name, &st);
  }
  for (i32 i = 0; i < st.n; i++)
    for (i32 j = 0; j < core->n; j++)
      if (strcmp(st.names[i].name, core->names[j].name) == 0) {
        hits++;
        break;
      }
  for (i32 i = 0; i < st.n; i++) free(st.names[i].name);
  free(st.names);
  return hits;
}

// A total is a sum, and a sum can stay flat while one module doubles and another is cut: the
// number that would have caught it is the module's own. One count per shelf header, recorded
// beside the total and checked the same way, so `webc api` names the module that grew rather
// than asking you to diff two manifests.
static void api_module_counts(i32 *out) {
  for (i32 i = 0; i < STDLIB_N; i++) {
    ApiSet one = {0};
    char path[1200];
    snprintf(path, sizeof path, "%s/include/webc/%s.h", webc_prefix(), STDLIB[i].name);
    api_scan(path, STDLIB[i].name, &one);
    out[i] = one.n;
    for (i32 j = 0; j < one.n; j++) free(one.names[j].name);
    free(one.names);
  }
}

// Which modules this app includes, by walking the sources it has. Cheap and honest: the
// `#include` line is the manifest, so reading the app's own files is reading its manifest.
static i32 g_used_hits[32];

static void stdlib_used_walk(const char *dir, i32 depth) {
  if (depth > 6) return;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      stdlib_used_walk(p, depth + 1);
      continue;
    }
    i64 n = 0;
    char *text = read_all(p, &n);
    if (!text) continue;
    for (i32 i = 0; i < STDLIB_N && i < 32; i++) {
      char needle[128];
      snprintf(needle, sizeof needle, "<webc/%s.h>", STDLIB[i].name);
      if (memmem(text, (size_t)n, needle, strlen(needle))) g_used_hits[i]++;
    }
    free(text);
  }
  closedir(d);
}

// Fill `g_used_hits` from the app's own sources. Returns how many modules it took.
static i32 stdlib_scan_used(void) {
  for (i32 i = 0; i < 32; i++) g_used_hits[i] = 0;
  if (path_exists("src")) stdlib_used_walk("src", 0);
  if (path_exists("app")) stdlib_used_walk("app", 0);
  if (path_exists("tests")) stdlib_used_walk("tests", 0);
  i32 found = 0;
  for (i32 i = 0; i < STDLIB_N; i++) found += g_used_hits[i] ? 1 : 0;
  return found;
}

// A header's line count, which is the size of the thing an app did not have to write.
static i32 header_lines(const char *mod) {
  char path[1024];
  snprintf(path, sizeof path, "%s/include/webc/%s.h", webc_prefix(), mod);
  i64 n = 0;
  char *text = read_all(path, &n);
  if (!text) return 0;
  i32 lines = 0;
  for (i64 k = 0; k < n; k++) lines += text[k] == '\n';
  free(text);
  return lines;
}

static int stdlib_used(void) {
  stdlib_scan_used();
  printf("\n  \033[1mstandard library in this app\033[0m\n\n");
  i32 found = 0;
  for (i32 i = 0; i < STDLIB_N; i++) {
    if (!g_used_hits[i]) continue;
    printf("    %-10s %-12s included by %d file%s\n", STDLIB[i].name, STDLIB[i].tier,
           g_used_hits[i], g_used_hits[i] == 1 ? "" : "s");
    found++;
  }
  if (!found) printf("    none — this app is core only, and the shelf costs it nothing\n");
  printf("\n");
  return 0;
}

// ---------------------------------------------------------------- stdlib size
//
// "What does this module cost me?" is the question the whole shelf is sold on, and an estimate
// is not an answer. A probe is built per module: one client file that includes `webc/fe.h` and
// the module, calls one of its functions so the linker cannot drop it, and links against the
// same frontend objects a real app uses. The number reported is the delta in gzipped bytes
// against a probe that includes the same headers *minus* the module — which is what an app
// actually pays for adding that line.
// mkdir -p, because a probe directory is one level deep and the alternative is a dependency
static void mkdir_p(const char *dir) {
  char path[1024];
  size_t n = strlen(dir);
  if (n >= sizeof path) return;
  memcpy(path, dir, n + 1);
  for (size_t i = 1; i <= n; i++) {
    if (path[i] != '/' && path[i] != 0) continue;
    char keep = path[i];
    path[i] = 0;
    mkdir(path, 0755);
    path[i] = keep;
  }
}

static i64 probe_bytes(Arena *a, const char *dir, const char *src_text, const char *tag) {
  char src[1024], out[1024];
  snprintf(src, sizeof src, "%s/%s.c", dir, tag);
  snprintf(out, sizeof out, "%s/%s.wasm", dir, tag);
  FILE *f = fopen(src, "w");
  if (!f) return -1;
  fputs(src_text, f);
  fclose(f);
  Config c = {0};
  snprintf(c.root, sizeof c.root, "%s", dir);
  snprintf(c.web, sizeof c.web, "%s", src);
  remove(out);
  if (!build_wasm(&c, true, out)) return -1;
  (void)a;
  return gzip_bytes(out);
}

// A page that *uses* the module: four or five of its verbs, the way an app would use them,
// because one call is folded away by the optimiser and measures nothing. Calling everything
// would measure a tutorial instead of a page, so this is deliberately a small page.
static const char *stdlib_probe_call(const char *mod) {
  if (strcmp(mod, "ui") == 0)
    return "view(Probe) { el(\"div\") { badge(\"3\", \"info\"); kbd(\"K\");"
           " meter(\"used\", 3, 10, \"3 of 10\", nullptr); skeleton(\"line\", 2); } }";
  if (strcmp(mod, "icons") == 0)
    return "view(Probe) { el(\"div\") { icon(WC_ICON_CHECK, 20); icon(WC_ICON_X, 16);"
           " icon(WC_ICON_SEARCH, 0); } }";
  if (strcmp(mod, "table") == 0)
    return "static void action_probe_sort(void *c, const char *e) { (void)c; (void)e; }\n"
           "static WTable probe_t;\n"
           "static i32 probe_order[4];\n"
           "view(Probe) { table_init(&probe_t, nullptr, 0, 4, probe_order); el(\"div\") {"
           " table_head(&probe_t, probe_sort); table_pager(&probe_t, probe_sort, \"row\");"
           " text(\"%lld\", (i64)table_pages(&probe_t)); } }";
  if (strcmp(mod, "overlay") == 0)
    return "static void action_probe_close(void *c, const char *e) { (void)c; (void)e; }\n"
           "view(Probe) { el(\"div\") { toast(\"saved\", \"info\");"
           " overlay_esc(probe_close); tip(\"because\"); } }";
  if (strcmp(mod, "form") == 0)
    return "view(Probe) { el(\"div\") { form_label(\"first_name\"); form_label(\"id\"); } }";
  if (strcmp(mod, "theme") == 0)
    return "static const WToken probe_tokens[] = {{\"bg\", \"#0f1115\"}, {\"fg\", \"#f7f7f8\"}};\n"
           "static const WTheme probe_theme = {\"probe\", probe_tokens, WC_ARRLEN(probe_tokens),"
           " \"system-ui\", \"ui-monospace\"};\n"
           "view(Probe) { theme_paint(true); el(\"div\") {"
           " text(\"%lld\", (i64)wc_theme_is_dark()); } }";
  if (strcmp(mod, "keys") == 0)
    return "static void action_probe_help(void *c, const char *e) { (void)c; (void)e; }\n"
           "static const WCmd probe_cmds[] = {K(\"Help\", \"shift+?\", probe_help),"
           " K(\"Search\", \"meta+k\", probe_help)};\n"
           "view(Probe) { el(\"div\") { keys_bind(probe_cmds); keycap(\"meta+k\"); } }";
  if (strcmp(mod, "motion") == 0)
    return "view(Probe) { el(\"div\") { enter(\"fade\"); stagger(2);"
           " el(\"div\") { class_(\"x\"); } } }";
  if (strcmp(mod, "dnd") == 0)
    return "static WDrag probe_drag = {.n = 3};\n"
           "view(Probe) { wc_dnd_list(&probe_drag, 3); el(\"ul\") {"
           " dnd_row(&probe_drag, nullptr, 0, -1, -1); dnd_row(&probe_drag, nullptr, 1, 0, 1); }"
           " text(\"%lld\", (i64)dnd_over(&probe_drag)); }";
  if (strcmp(mod, "csv") == 0)
    return "view(Probe) { Buf b; buf_init(&b, wc_arena_get(), 32);"
           " csv_cell(&b, str_lit(\"a,b\")); csv_cell(&b, str_lit(\"he said \\\"hi\\\"\"));"
           " el(\"div\") { text(\"%s\", buf_cstr(&b)); } }";
  if (strcmp(mod, "richtext") == 0)
    return "view(Probe) { el(\"div\") {"
           " rich(str_lit(\"<p>a <b>b</b> <a href=\\\"https://x.test\\\">c</a></p>\"));"
           " rich_with(str_lit(\"<ul><li>d</li></ul>\"), .allow = RICH_TIGHT); } }";
  if (strcmp(mod, "ld") == 0)
    return "static const char *const probe_kw[] = {\"go\", \"postgres\", nullptr};\n"
           "view(Probe) { el(\"div\") {"
           " str d = ld_blog_posting(.url = \"https://x.test/a\", .headline = \"A\","
           " .author = \"Ada\", .publisher = \"P\", .keywords = probe_kw, .word_count = 9);"
           " text(\"%s\", text_of(d)); } }";
  if (strcmp(mod, "i18n") == 0)
    return "static const WMsg probe_msgs[] = {{\"save\", \"Save\"}};\n"
           "view(Probe) { i18n_init(CATS(en_catalog(probe_msgs))); el(\"div\") {"
           " text(t(\"save\")); text(lang_dir()); } }";
  if (strcmp(mod, "time") == 0)
    return "view(Probe) { el(\"div\") { text(fmt_date(0, \"%Y-%m-%d\"));"
           " text(ago(0)); text(dur_ms(90000)); } }";
  return "view(Probe) { el(\"div\") { text(\"%lld\", get(n)); } }";
}

// Build the floor once: the browser half with no module at all. Everything else is a delta
// against it, so a catalogue of twenty does not build the floor twenty times.
static i64 stdlib_floor(const char *dir) {
  static const char *floor_src =
      "#include <webc/fe.h>\n"
      "state(i64, n, 0);\n"
      "view(Probe) { el(\"div\") { text(\"%lld\", get(n)); } }\n"
      "client_main { mount(Probe, \"#root\"); return 0; }\n";
  bool was = g_quiet;
  g_quiet = true;
  i64 b = probe_bytes(nullptr, dir, floor_src, "probe_floor");
  g_quiet = was;
  return b;
}

// One module's cost: the same page, plus the module's header and a small page that uses it.
static i64 stdlib_weight(const char *dir, i64 floor_bytes, const char *mod) {
  if (floor_bytes <= 0) return -1;
  for (i32 i = 0; i < STDLIB_N; i++)
    if (strcmp(STDLIB[i].name, mod) == 0 && !STDLIB[i].client) return 0;
  Buf src;
  buf_init(&src, nullptr, 4096);
  buf_printf(&src, "#include <webc/fe.h>\n#include <webc/%s.h>\n", mod);
  buf_puts(&src, "state(i64, n, 0);\n");
  // one small page per module: enough for the linker to keep what the module is *for*, and
  // nothing more. A probe that called everything would measure a tutorial, not a weight.
  buf_printf(&src, "%s\n", stdlib_probe_call(mod));
  buf_puts(&src, "client_main { mount(Probe, \"#root\"); return 0; }\n");
  bool was = g_quiet;
  g_quiet = true;
  i64 b = probe_bytes(nullptr, dir, buf_cstr(&src), mod);
  g_quiet = was;
  // `src` is a heap Buf: buf_free owns the pointer it allocated, free() would not
  buf_free(&src);
  if (b < 0) return -1;
  i64 delta = b - floor_bytes;
  return delta < 0 ? 0 : delta;
}

// `--check <file>`: the module table against recorded numbers, so `make budget` has a line per
// module instead of a paragraph about them. The file is one `name bytes` per line, `#` for
// comments; a module with no line is simply not gated yet.
static int stdlib_check(const char *path, i32 *const deltas) {
  i64 n = 0;
  char *text = read_all(path, &n);
  if (!text) {
    fprintf(stderr, "webc: cannot read %s\n", path);
    return 1;
  }
  i32 bad = 0, checked = 0;
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    if (!eol) eol = p + strlen(p);
    if (*p != '#') {
      char name[64];
      long limit = 0;
      if (sscanf(p, "%63s %ld", name, &limit) == 2) {
        for (i32 i = 0; i < STDLIB_N; i++)
          if (strcmp(STDLIB[i].name, name) == 0) {
            checked++;
            char label[80];
            snprintf(label, sizeof label, "stdlib %s", name);
            if (deltas[i] < 0) {
              printf("%-34s %10s  budget <= %6ld B   \033[31mno build\033[0m\n", label, "n/a",
                     limit);
              bad++;
            } else if (deltas[i] > limit) {
              printf("%-34s %7lld B  budget <= %6ld B   \033[31mOVER BUDGET\033[0m\n", label,
                     (long long)deltas[i], limit);
              bad++;
            } else {
              printf("%-34s %7lld B  budget <= %6ld B   ok\n", label, (long long)deltas[i],
                     limit);
            }
          }
      }
    }
    if (!*eol) break;
    p = eol + 1;
  }
  free(text);
  if (!checked) {
    fprintf(stderr, "webc: %s gates no module — every line needs `name bytes`\n", path);
    return 1;
  }
  return bad ? 1 : 0;
}

static int cmd_size_modules(Config *c, const char *check_file) {
  char dir[1024];
  snprintf(dir, sizeof dir, "%s/.webc/stdlib-size", c->root);
  mkdir_p(dir);
  i32 deltas[64];
  for (i32 i = 0; i < 64; i++) deltas[i] = -1;
  g_quiet = true; // twenty probe apps: the table is the output, not the build log
  // the floor: the client half with no module at all, which is what every app already pays
  i64 floor_bytes = stdlib_floor(dir);
  if (floor_bytes <= 0) {
    fprintf(stderr, "webc: could not build the size probe (is wasi-sdk installed?)\n");
    return 1;
  }
  printf("\n  \033[1mwebc size --modules\033[0m — what each standard-library module costs\n\n");
  char floor_s[32];
  human_size(floor_s, sizeof floor_s, floor_bytes);
  printf("    floor: a client half with no module at all   %s gz\n\n", floor_s);
  printf("    %-10s %-12s %10s  %s\n", "module", "tier", "gzip", "what");
  for (i32 i = 0; i < STDLIB_N; i++) {
    if (!STDLIB[i].client) {
      printf("    %-10s %-12s %10s  %s\n", STDLIB[i].name, STDLIB[i].tier, "0 B",
             "server-side: no bytes in the browser");
      continue;
    }
    i64 delta = stdlib_weight(dir, floor_bytes, STDLIB[i].name);
    deltas[i] = delta;
    char one[32];
    if (delta < 0) {
      printf("    %-10s %-12s %10s  %s\n", STDLIB[i].name, STDLIB[i].tier, "n/a", STDLIB[i].what);
      continue;
    }
    human_size(one, sizeof one, delta);
    printf("    %-10s %-12s %10s  %s\n", STDLIB[i].name, STDLIB[i].tier, one, STDLIB[i].what);
  }
  printf("\n    Measured, not estimated: each line is a real build of a probe, gzipped.\n"
         "    A module you do not include costs nothing at all.\n\n");
  if (check_file) {
    printf("\n");
    return stdlib_check(check_file, deltas);
  }
  return 0;
}


// `webc add <module>` — the other half of `webc stdlib <module>`. That one tells you what a
// module is; this one puts the two files it needs in your app: the stylesheet in `public/`, and
// the two lines to paste. It copies the CSS because a sheet in the framework's `assets/` is not
// served by your app, and a `<link>` to a path that 404s is a module that looks broken.
static int cmd_add(i32 argc, char **argv) {
  const char *mod = nullptr;
  bool force = false, want_css = true;
  for (i32 i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--force") == 0) force = true;
    else if (strcmp(argv[i], "--css") == 0) want_css = true;
    else if (strcmp(argv[i], "--no-css") == 0) want_css = false;
    else if (argv[i][0] != '-') mod = argv[i];
  }
  if (!mod) {
    fprintf(stderr, "webc: add what?\n  webc add table   one of:");
    for (i32 i = 0; i < STDLIB_N; i++) fprintf(stderr, " %s", STDLIB[i].name);
    fprintf(stderr, "\n");
    return 1;
  }
  if (!stdlib_is(mod)) {
    fprintf(stderr, "webc: '%s' is not a standard-library module\n  try: ", mod);
    for (i32 i = 0; i < STDLIB_N; i++) fprintf(stderr, "%s%s", i ? ", " : "", STDLIB[i].name);
    fprintf(stderr, "\n");
    return 1;
  }
  Config c;
  config_load(&c, ".");
  const char *what = "";
  const char *tier = "";
  for (i32 i = 0; i < STDLIB_N; i++)
    if (strcmp(STDLIB[i].name, mod) == 0) {
      what = STDLIB[i].what;
      tier = STDLIB[i].tier;
    }

  char src_path[1024], dst_dir[1024], dst_path[1024];
  snprintf(src_path, sizeof src_path, "%s/assets/stdlib/%s.css", webc_prefix(), mod);
  const char *pub = c.public_dir[0] ? c.public_dir : "public";
  snprintf(dst_dir, sizeof dst_dir, "%s/stdlib", pub);
  snprintf(dst_path, sizeof dst_path, "%s/%s.css", dst_dir, mod);

  printf("\n  \033[1m%s\033[0m  %s · %s\n\n", mod, tier, what);

  // The ledger row first, because it is the question a budget asks: what does this add? The
  // probe is the same one `webc size --modules` builds, so the number here and the number
  // there cannot drift. Then the budget: a project that declares one gets *refused* when the
  // module would put it over, because a budget that only warns is a budget nobody obeys.
  {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/.webc/stdlib-size", c.root);
    mkdir_p(dir);
    i64 floor_bytes = stdlib_floor(dir);
    if (floor_bytes > 0) {
      i64 w = stdlib_weight(dir, floor_bytes, mod);
      if (w > 0) {
        char one[32];
        human_size(one, sizeof one, w);
        printf("    cost  %s gz in the browser half\n", one);
      } else if (w == 0) {
        printf("    cost  nothing in the browser half — this module is server-side\n");
      }
      // the app's own number, if it has been built
      char wasm[1024];
      snprintf(wasm, sizeof wasm, "%s/%s.wasm", c.root, WC_WASM_STEM);
      i64 now = file_bytes(wasm) > 0 ? gzip_bytes(wasm) : -1;
      if (c.wasm_budget > 0 && now > 0 && w > 0) {
        char lim[32], now_s[32], proj[32], over[32];
        human_size(lim, sizeof lim, c.wasm_budget);
        human_size(now_s, sizeof now_s, now);
        human_size(proj, sizeof proj, now + w);
        printf("    now   %s gz, %s gz with %s — budget %s gz\n", now_s, proj, mod, lim);
        if (now + w > c.wasm_budget && !force) {
          human_size(over, sizeof over, now + w - c.wasm_budget);
          printf("\n    \033[31mrefused\033[0m — that is %s over the budget this project declares\n"
                 "    (webc.json \"budget\": {\"wasm\": %lld}). Take a module out, raise the\n"
                 "    budget on purpose, or pass --force.\n\n",
                 over, (long long)c.wasm_budget);
          return 1;
        }
      }
    }
  }

  // 1. the stylesheet, if the module has one
  i64 n = 0;
  char *css = want_css ? read_all(src_path, &n) : nullptr;
  bool have_css = css != nullptr;
  if (have_css) {
    if (path_exists(dst_path) && !force) {
      printf("    \033[2mcss  %s already there\033[0m (--force to overwrite)\n", dst_path);
    } else {
      mkdir_p(dst_dir);
      write_file(dst_path, css);
      printf("    css   %s  \033[2m(%lld bytes)\033[0m\n", dst_path, (long long)n);
    }
    free(css);
  }
  // 2. the two lines that are the actual manifest
  printf("    use   #include <webc/%s.h>\n", mod);
  // the sheet lands in public/stdlib/, and the framework mounts public/ at /public — so the
  // href is /public/stdlib/<module>.css; a bare /stdlib/<module>.css is a 404 from the static
  // handler, as JSON, which a browser refuses as a stylesheet.
  if (have_css)
    printf("    head  <link rel=\"stylesheet\" href=\"/public/stdlib/%s.css\">\n", mod);
  printf("\n    that is the whole install: no build step, no registry, no version pin.\n");
  if (have_css)
    printf("    the CSS is yours now — edit %s freely, it is not regenerated.\n\n", dst_path);
  else
    printf("\n");
  (void)force;
  return 0;
}

// ---------------------------------------------------------------- scaffold
//
// `webc scaffold Post` — the CRUD nobody wants to type, written once and handed over.
//
// The framework's posture everywhere else decides this command's shape: `webc new` scaffolds
// *code*, the shelf adds *headers*, nothing hides a runtime. So this writes two files the app
// owns — a route/view file and a test — and prints what it did. It never touches a file that
// exists (`--force` overwrites), and it never edits the app's model: a scaffold that rewrites
// your struct is a generator, and generators are the thing this framework does not have.
//
// What it reads: the model, from the app's own descriptor (`WEBC_MODEL_DUMP`), because the
// descriptor is the only place the truth lives — the same reason `webc migrate diff` runs the
// app instead of guessing at its models.
static bool scaffold_write(const char *path, Buf *body, bool force) {
  if (!force && path_exists(path)) {
    info("%s exists — left alone (--force overwrites)", path);
    return false;
  }
  char dir[1200];
  snprintf(dir, sizeof dir, "%s", path);
  for (i32 i = (i32)strlen(dir) - 1; i > 0; i--)
    if (dir[i] == '/') {
      dir[i] = 0;
      mkdirs(dir);
      break;
    }
  write_file(path, buf_cstr(body));
  return true;
}

// The lower-case, plural-free stem a file and a route use: "LedgerEntry" -> "ledger_entry".
static void scaffold_snake(const char *name, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; name[i] && o + 2 < cap; i++) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z') {
      if (i > 0) out[o++] = '_';
      out[o++] = (char)(c + 32);
    } else {
      out[o++] = c;
    }
  }
  out[o] = 0;
}

// The path segment for a list of them: the model's own table name is what the framework
// already derived for the SQL, and one name for one thing is the whole point.
static const char *jbool(JVal *v, const char *key) {
  JVal *f = json_get(v, key);
  return f && f->kind == J_BOOL && f->b ? "true" : "false";
}

// Add one path to a set in webc.json, textually: this file is the developer's, and a
// re-serialised copy would lose their key order, their comments and their formatting.
static bool scaffold_register_source(const char *path_in) {
  i64 n = 0;
  char *text = read_all("webc.json", &n);
  if (!text) return false;
  if (strstr(text, path_in)) {
    free(text);
    return true; // already named
  }
  // find "server": [ … ] and insert before the closing bracket
  const char *key = strstr(text, "\"server\"");
  Buf out;
  buf_init(&out, nullptr, (size_t)n + 256);
  if (!key) {
    // no set at all: this project compiles src/ wholesale, which already includes the file
    buf_putstr(&out, (str){text, (size_t)n});
    write_file("webc.json", buf_cstr(&out));
    buf_free(&out);
    free(text);
    return true;
  }
  const char *open = strchr(key, '[');
  const char *close = open ? strchr(open, ']') : nullptr;
  if (!open || !close) {
    buf_free(&out);
    free(text);
    return false;
  }
  // an empty set needs no comma
  const char *last = close;
  while (last > open + 1 && (last[-1] == ' ' || last[-1] == '\n' || last[-1] == '\r')) last--;
  bool empty = last == open + 1;
  buf_putstr(&out, (str){text, (size_t)(close - text)});
  buf_printf(&out, "%s\n    \"%s\"\n  ", empty ? "" : ",", path_in);
  buf_puts(&out, close);
  write_file("webc.json", buf_cstr(&out));
  buf_free(&out);
  free(text);
  return true;
}

// `webc scaffold live <Model>` — the live page in one command: the SSE route that
// seeds every tab with the whole table, the mutations that broadcast after they
// write, and the test that proves a broadcast carries the newest document. The
// client half is three lines the app already owns a place for, so they are
// printed, not injected — `webc scaffold` never edits a file it did not write.
static int scaffold_live(const char *mn, const char *tb, const char *snake,
                          const char *label, const char *header, bool force, Arena *a) {
  char routes_path[256], test_path[256];
  snprintf(routes_path, sizeof routes_path, "src/live_%s.c", snake);
  snprintf(test_path, sizeof test_path, "tests/live_%s_test.c", snake);
  // the include is written from inside src/ (every compile line carries `-I src`),
  // so the header's basename is the spelling that resolves
  const char *slash = header && header[0] ? strrchr(header, '/') : nullptr;
  const char *inc = slash ? slash + 1 : (header && header[0] ? header : "app.h");

  Buf f;
  buf_init(&f, nullptr, 8192);
  buf_printf(&f,
             "// generated by `webc scaffold live %s`; yours to edit.\n"
             "//\n"
             "// The live page in three routes: one SSE stream that seeds every tab with\n"
             "// the whole table and follows every change (a frame is a complete document,\n"
             "// a burst coalesces to one frame — webc/live.h), and the mutations that\n"
             "// broadcast after they write. The room is the table; the state of record\n"
             "// is the database, which is why a room nobody watches keeps nothing.\n"
             "#include <webc/server.h>\n"
             "#include <webc/live.h>\n"
             "#include \"%s\"\n"
             "\n"
             "GET(\"/sse/%s\") {\n"
             "  return live_join(\"%s\", \"all\", live_json(db_all(%s)));\n"
             "}\n"
             "\n"
             "POST(\"/api/%s/save\") {\n"
             "  %s c = body(%s);\n"
             "  db_save(%s, &c);\n"
             "  live_broadcast(\"%s\", \"all\", live_json(db_all(%s)));\n"
             "  return json(200, %s, &c);\n"
             "}\n"
             "\n"
             "POST(\"/api/%s/delete\") {\n"
             "  RowId in = body(RowId);\n"
             "  db_delete(%s, in.id);\n"
             "  live_broadcast(\"%s\", \"all\", live_json(db_all(%s)));\n"
             "  return res_empty(204);\n"
             "}\n",
             mn, inc, tb, tb, mn, tb, mn, mn, mn, tb, mn, mn, tb, mn, tb, mn);
  bool wrote_routes = scaffold_write(routes_path, &f, force);
  buf_free(&f);

  Buf t;
  buf_init(&t, nullptr, 4096);
  buf_printf(&t,
             "// generated by `webc scaffold live %s`; run with `webc test`.\n"
             "#include <webc/test.h>\n"
             "#include <webc/live.h>\n"
             "#include \"%s\"\n"
             "\n"
             "static void boot(void) {\n"
             "  setenv(\"DATABASE_URL\", \"sqlite::memory:\", 1);\n"
             "  webc_boot();\n"
             "  db_default();\n"
             "}\n"
             "TEST_SETUP(boot);\n"
             "\n"
             "TEST(a_save_broadcasts_the_newest_whole_document) {\n"
             "  live_reset();\n"
             "  TestClient c = test_client();\n"
             "  %s row = {0};\n",
             mn, inc, mn);
  if (label)
    buf_printf(&t, "  row.%s = \"live scaffold\";\n", label);
  buf_printf(&t,
             "  ASSERT_EQ(tc_post_json(c, \"/api/%s/save\", %s, &row).status, 200);\n"
             "  // the room holds the newest complete document — the bytes every tab gets\n"
             "  ASSERT_STR_CONTAINS(live_doc(\"%s\", \"all\"), \"live scaffold\");\n"
             "}\n"
             "\n"
             "TEST(a_delete_broadcasts_the_table_without_the_row) {\n"
             "  live_reset();\n"
             "  TestClient c = test_client();\n"
             "  // suites share one database, so the test is a delta, not a census\n"
             "  i64 before = db_count(%s);\n"
             "  %s row = {0};\n",
             tb, mn, tb, mn, mn);
  if (label)
    buf_printf(&t, "  row.%s = \"gone soon\";\n", label);
  // (a generator writing a generator: every specifier the *generated* file needs is
  // %% here — a bare %lld below was consumed by this buf_printf as an i64 and ate
  // the table-name argument with it, which is why this template once generated
  // `snprintf(body, sizeof body, "id=<pointer bits>", ...)` and swapped every
  // argument after it)
  buf_printf(&t,
             "  ASSERT_EQ(tc_post_json(c, \"/api/%s/save\", %s, &row).status, 200);\n"
             "  ASSERT_EQ(db_count(%s), before + 1);\n"
             "  // the row just saved is the newest id: the delete posts it back\n"
             "  i64 id = 0;\n"
             "  Vec%s *rows = db_all(%s);\n"
             "  for (i32 k = 0; k < vlen(rows); k++)\n"
             "    if (vat(rows, k).id > id) id = vat(rows, k).id;\n"
             "  char body[32];\n"
             "  snprintf(body, sizeof body, \"id=%%lld\", (long long)id);\n"
             "  ASSERT_EQ(tc_post_body(c, \"/api/%s/delete\", WC_AS_STR(body)).status, 204);\n"
             "  ASSERT_EQ(db_count(%s), before);\n"
             "  ASSERT_FALSE(str_contains(live_doc(\"%s\", \"all\"), str_lit(\"gone soon\")));\n"
             "}\n"
             "\n"
             "TEST_MAIN()\n",
             tb, mn, mn, mn, mn, tb, mn, tb);
  bool wrote_test = scaffold_write(test_path, &t, force);
  buf_free(&t);

  if (wrote_routes) scaffold_register_source(routes_path);
  if (!wrote_routes && !wrote_test) return 1;
  info("scaffolded live %s: %s%s%s", mn, wrote_routes ? routes_path : "",
       wrote_test ? " and " : "", wrote_test ? test_path : "");
  printf("\n  the client half — three lines in the app's own client file:\n"
         "\n"
         "    static void on_%s_doc(str json) { /* decode into the page's store */ }\n"
         "    action(%s_ready) { live_open(); }        // plus on_ready(%s_ready)\n"
         "    // and where the page mounts: live_connect(\"/sse/%s\", \"doc\", on_%s_doc);\n"
         "\n"
         "  next: webc check   — then open /sse/%s in a second tab and save\n\n",
         snake, snake, snake, tb, snake, tb);
  (void)a;
  return 0;
}

static int cmd_scaffold(i32 argc, char **argv) {
  bool live = false;
  if (argc > 0 && strcmp(argv[0], "live") == 0) {
    live = true;
    argc--;
    argv++;
  }
  const char *model = nullptr;
  bool force = false;
  for (i32 i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--force") == 0) force = true;
    else if (argv[i][0] != '-') model = argv[i];
    else die("usage: webc scaffold <Model> [--force]");
  }
  if (!model) die("usage: webc scaffold <Model> [--force]");
  // The model, from the process that has it.
  Buf dump;
  buf_init(&dump, nullptr, 4096);
  if (run_boot_hook_capture("WEBC_MODEL_DUMP", model, &dump) != 0) {
    buf_free(&dump);
    return 1;
  }
  // The app may have printed a line or two before the object; the object is what is wanted.
  const char *json_start = strchr(buf_cstr(&dump), '{');
  if (!json_start) {
    fprintf(stderr, "webc: `%s` did not answer with a model — is it declared in the app?\n", model);
    buf_free(&dump);
    return 1;
  }
  Arena *a = arena_new(256 * 1024);
  JParse jp = json_parse(a, str_lit(json_start));
  if (!jp.ok) {
    fprintf(stderr, "webc: could not read the model description\n");
    buf_free(&dump);
    return 1;
  }
  JVal *mv = jp.v;
  const char *mn = str_cstr(a, json_as_str(json_get(mv, "name")));
  const char *tb = str_cstr(a, json_as_str(json_get(mv, "table")));
  JVal *fields = json_get(mv, "fields");
  char snake[96];
  scaffold_snake(mn, snake, sizeof snake);
  char routes_path[256], test_path[256];
  snprintf(routes_path, sizeof routes_path, "src/admin_%s.c", snake);
  snprintf(test_path, sizeof test_path, "tests/admin_%s_test.c", snake);

  // The label column: the first text field, and the one a search box filters on.
  const char *label = nullptr, *search = nullptr;
  for (i32 i = 0; fields && i < fields->n; i++) {
    JVal *fl = json_at(fields, i);
    if (strcmp(jbool(fl, "text"), "true") != 0) continue;
    const char *fn = str_cstr(a, json_as_str(json_get(fl, "name")));
    if (!label) label = fn;
    JVal *mx = json_get(fl, "max");
    if (!search && mx && (i64)mx->num > 0) search = fn;
  }

  if (live) {
    // the header WEBC_MODEL(Name) lives in, found the way the admin scaffold finds it
    char lheader[512] = "";
    {
      DIR *d = opendir("src");
      struct dirent *e;
      while (d && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 3 || strcmp(e->d_name + len - 2, ".h") != 0) continue;
        char hp[1024];
        snprintf(hp, sizeof hp, "src/%s", e->d_name);
        i64 n = 0;
        char *text = read_all(hp, &n);
        if (!text) continue;
        char needle[128];
        snprintf(needle, sizeof needle, "WEBC_MODEL(%s", mn);
        if (memmem(text, (size_t)n, needle, strlen(needle)))
          snprintf(lheader, sizeof lheader, "%s", hp);
        free(text);
      }
      if (d) closedir(d);
    }
    i32 rc = scaffold_live(mn, tb, snake, label, lheader, force, a);
    buf_free(&dump);
    arena_destroy(a);
    return rc;
  }

  // ---- the file the app owns
  Buf f;
  buf_init(&f, nullptr, 8192);
  buf_printf(&f,
             "// generated by `webc scaffold %s` from WEBC_MODEL(%s); yours to edit.\n"
             "//\n"
             "// A list, a form, a save and a delete — the four routes every table needs, in the\n"
             "// framework's own vocabulary: the form is drawn from the model (webc/form.h), the\n"
             "// list is a window with a count (WPager), and the body is parsed and validated by\n"
             "// the same descriptor the form came from, so a rule lives in one place.\n"
             "//\n"
             "// Nothing here is hidden: delete a route, change a column, rename a heading.\n",
             mn, mn);
  char header[512] = "";
  {
    DIR *d = opendir("src");
    struct dirent *e;
    while (d && (e = readdir(d))) {
      size_t len = strlen(e->d_name);
      if (len < 3 || strcmp(e->d_name + len - 2, ".h") != 0) continue;
      char hp[1024];
      snprintf(hp, sizeof hp, "src/%s", e->d_name);
      i64 n = 0;
      char *text = read_all(hp, &n);
      if (!text) continue;
      char needle[128];
      snprintf(needle, sizeof needle, "WEBC_MODEL(%s", mn);
      if (memmem(text, (size_t)n, needle, strlen(needle))) snprintf(header, sizeof header, "%s", hp);
      free(text);
      if (header[0]) break;
    }
    if (d) closedir(d);
  }
  buf_puts(&f, "\n#include <webc/server.h>\n#include <webc/form.h>\n\n");
  if (header[0]) {
    buf_printf(&f, "#include \"%s\"\n", header + 4); // "src/app.h" -> "app.h"
  } else {
    // No header declares it, so this file carries a copy of the declaration, taken from the
    // file that owns it. Copying is safe: the registry keys on the model's *name*, so a second
    // descriptor with the same shape registers nothing and queries through it are identical.
    // The tidier answer is a header, and the comment says so.
    char decl[4096] = "";
    char where[512] = "";
    DIR *d2 = opendir("src");
    struct dirent *e2;
    while (d2 && (e2 = readdir(d2))) {
      size_t len = strlen(e2->d_name);
      if (len < 3 || strcmp(e2->d_name + len - 2, ".c") != 0) continue;
      char cp[1024];
      snprintf(cp, sizeof cp, "src/%s", e2->d_name);
      i64 n = 0;
      char *text = read_all(cp, &n);
      if (!text) continue;
      char needle[128];
      snprintf(needle, sizeof needle, "WEBC_MODEL(%s", mn);
      char *at = memmem(text, (size_t)n, needle, strlen(needle));
      if (at) {
        // to the line where the field list closes: the matching `)` at depth zero
        i32 depth = 0;
        char *q = at + strlen("WEBC_MODEL");
        for (; *q; q++) {
          if (*q == '(') depth++;
          else if (*q == ')' && --depth == 0) {
            q++;
            break;
          }
        }
        size_t dl = (size_t)(q - at);
        if (dl < sizeof decl) {
          memcpy(decl, at, dl);
          decl[dl] = 0;
          snprintf(where, sizeof where, "%s", cp);
        }
      }
      free(text);
      if (decl[0]) break;
    }
    if (d2) closedir(d2);
    if (decl[0]) {
      buf_printf(&f,
                 "// Copied from %s by `webc scaffold`, so this file stands alone. Two descriptors\n"
                 "// with one name are one model to the registry; keep them in step, or move the\n"
                 "// declaration into a header (src/models.h) and include it here instead.\n\n%s\n",
                 where, decl);
    } else {
      buf_printf(&f,
                 "// %s is not declared in any src/ file this command could read. Declare it here\n"
                 "// (copy the WEBC_MODEL(%s, …) block) or move it into a header and include it.\n",
                 mn, mn);
    }
  }
  buf_printf(&f, "\nVec(%s)\n", mn);

  // the list
  buf_printf(&f,
             "\nview(v_%s_list, Vec%s *rows) {\n"
             "  el(\"main\") {\n"
             "    class_(\"admin\");\n"
             "    el(\"h1\") { text(\"%s\"); }\n"
             "    el(\"form\") { attr(\"method\", \"get\"); attr(\"action\", \"/admin/%s\");\n"
             "      el(\"input\") { type_(\"search\"); name(\"q\"); placeholder(\"Search\"); }\n"
             "      el(\"button\") { text(\"Search\"); }\n"
             "    }\n"
             "    el(\"table\") {\n"
             "      el(\"thead\") { el(\"tr\") {\n",
             snake, mn, mn, snake);
  for (i32 i = 0; fields && i < fields->n; i++) {
    JVal *fl = json_at(fields, i);
    if (strcmp(jbool(fl, "pk"), "true") == 0) continue;
    const char *fn = str_cstr(a, json_as_str(json_get(fl, "name")));
    buf_printf(&f, "        el(\"th\") { text(\"%s\"); }\n", fn);
  }
  buf_puts(&f, "        el(\"th\") { text(\"\"); }\n      } }\n      el(\"tbody\") {\n");
  buf_printf(&f, "        for_each(r, *rows) {\n          el(\"tr\") {\n");
  for (i32 i = 0; fields && i < fields->n; i++) {
    JVal *fl = json_at(fields, i);
    if (strcmp(jbool(fl, "pk"), "true") == 0) continue;
    const char *fn = str_cstr(a, json_as_str(json_get(fl, "name")));
    if (strcmp(jbool(fl, "text"), "true") == 0)
      buf_printf(&f, "            el(\"td\") { text(\"%%s\", r->%s ? r->%s : \"\"); }\n", fn, fn);
    else if (strcmp(jbool(fl, "bool"), "true") == 0)
      buf_printf(&f, "            el(\"td\") { text(\"%%s\", r->%s ? \"yes\" : \"no\"); }\n", fn);
    else if (strcmp(jbool(fl, "num"), "true") == 0)
      buf_printf(&f, "            el(\"td\") { text(\"%%lld\", (long long)r->%s); }\n", fn);
    else
      buf_puts(&f, "            el(\"td\") { }\n");
  }
  buf_printf(&f,
             "            el(\"td\") {\n"
             "              el(\"a\") { href(fmt(\"/admin/%s/%%lld\", (long long)r->id));"
             " text(\"Edit\"); }\n"
             "              el(\"form\") { post_to(\"/admin/%s/delete\");\n"
             "                hidden_i(\"id\", r->id);\n"
             "                el(\"button\") { type_(\"submit\"); text(\"Delete\"); }\n"
             "              }\n"
             "            }\n"
             "          }\n"
             "        }\n"
             "      }\n"
             "    }\n"
             "  }\n"
             "}\n",
             snake, snake);

  // the form
  buf_printf(&f,
             "\nview(v_%s_form, WForm *fs) {\n"
             "  el(\"main\") {\n"
             "    class_(\"admin\");\n"
             "    el(\"h1\") { text(\"%s\"); }\n"
             "    el(\"form\") { post_to(\"/admin/%s/save\");\n"
             "      form_fields(fs);\n"
             "      form_errors(fs);\n"
             "      el(\"button\") { type_(\"submit\"); text(\"Save\"); }\n"
             "    }\n"
             "    el(\"a\") { href(\"/admin/%s\"); text(\"Back\"); }\n"
             "  }\n"
             "}\n",
             snake, mn, snake, snake);

  // the routes
  buf_printf(&f,
             "\n// GET /admin/%s — the list, with a search box and a window.\n"
             "GET(\"/admin/%s\") {\n"
             "  WWhere w;\n"
             "  ww_init(&w, nullptr); // nullptr: this request's arena, the one every build uses\n",
             tb, snake);
  if (search)
    buf_printf(&f,
               "  str q = query(\"q\");\n"
               "  if (q.n) ww_like(&w, %s, %s, q);\n",
               mn, search);
  else
    buf_puts(&f, "  // no text field to search: add `ww_like(&w, " "…" ")` when there is one\n");
  buf_printf(&f,
             "  WPager pg = {.page = query_i64(\"page\", 1), .limit = 50};\n"
             "  Vec%s *rows = db_page(%s, &w, &pg);\n"
             "  return page(v_%s_list, rows);\n"
             "}\n",
             mn, mn, snake);
  buf_printf(&f,
             "\n// GET /admin/%s/new — an empty form.\n"
             "GET(\"/admin/%s/new\") {\n"
             "  %s draft = {0};\n"
             "  WForm fs;\n"
             "  form_init(&fs, %s_modelof(), &draft);\n"
             "  return page(v_%s_form, &fs);\n"
             "}\n",
             mn, snake, mn, mn, snake);
  buf_printf(&f,
             "\n// GET /admin/%s/{id} — the same form, filled from the row.\n"
             "GET(\"/admin/%s/:id\") {\n"
             "  i64 id = path_i64(\"id\");\n"
             "  %s *row = db_get(%s, id);\n"
             "  if (!row) fail(404, \"no %s %%lld\", (long long)id);\n"
             "  WForm fs;\n"
             "  form_init(&fs, %s_modelof(), row);\n"
             "  return page(v_%s_form, &fs);\n"
             "}\n",
             mn, snake, mn, mn, snake, mn, snake);
  buf_printf(&f,
             "\n// POST /admin/%s/save — parsed and validated by the model, then saved.\n"
             "POST(\"/admin/%s/save\") {\n"
             "  %s row = {0};\n"
             "  if (!body_try(%s, &row)) {\n"
             "    WForm fs;\n"
             "    form_init(&fs, %s_modelof(), &row);\n"
             "    werrs_add(&fs.errs, WC_AS_STR(wc_body_error_field()), WC_AS_STR(wc_body_error()));\n"
             "    fs.submitted = true;\n"
             "    return page_with(422, v_%s_form, &fs);\n"
             "  }\n"
             "  if (!db_try_save(%s, &row)) fail(422, \"a %s with that value already exists\");\n"
             "  return redirect(\"/admin/%s\");\n"
             "}\n",
             mn, snake, mn, mn, mn, snake, mn, snake, snake);
  buf_printf(&f,
             "\n// POST /admin/%s/delete\n"
             "POST(\"/admin/%s/delete\") {\n"
             "  RowId in = body(RowId);\n"
             "  db_delete(%s, in.id);\n"
             "  return redirect(\"/admin/%s\");\n"
             "}\n",
             mn, snake, mn, snake);
  bool wrote_routes = scaffold_write(routes_path, &f, force);
  buf_free(&f);

  // ---- the test
  //
  // The app's own suite boots the app (TEST_SETUP) against an in-memory database, and these
  // tests do the same: a route is reachable without a socket, and a POST is one call. The
  // body is written by name rather than from a model value, so the test compiles in a file
  // that does not have to know how the app declares its models.
  Buf t;
  buf_init(&t, nullptr, 4096);
  buf_printf(&t,
             "// generated by `webc scaffold %s`; yours to edit.\n"
             "//\n"
             "// One test per route the scaffold wrote, with the test client: a cookie jar, a form\n"
             "// body, and assertions about what the response *is*. The database is the in-memory\n"
             "// one the boot below opens, so a run leaves nothing behind.\n"
             "#include <webc/test.h>\n"
             "\n"
             "static void boot(void) {\n"
             "  setenv(\"DATABASE_URL\", \"sqlite::memory:\", 1);\n"
             "  webc_boot();\n"
             "  db_default();\n"
             "}\n"
             "TEST_SETUP(boot);\n"
             "\n"
             "TEST(the_list_renders) {\n"
             "  Response r = webc_test_request(\"GET\", \"/admin/%s\");\n"
             "  ASSERT_STATUS(r, 200);\n"
             "  ASSERT_HTML(r, \"table\");\n"
             "}\n"
             "\n"
             "TEST(the_form_renders) {\n"
             "  ASSERT_STATUS(webc_test_request(\"GET\", \"/admin/%s/new\"), 200);\n"
             "}\n"
             "\n"
             "TEST(a_save_writes_the_row) {\n"
             "  TestClient c = test_client();\n"
             "  Response r = tc_post_body(c, \"/admin/%s/save\", str_lit(\"",
             mn, snake, snake, snake);
  {
    bool first = true;
    for (i32 i = 0; fields && i < fields->n; i++) {
      JVal *fl = json_at(fields, i);
      if (strcmp(jbool(fl, "pk"), "true") == 0 || strcmp(jbool(fl, "text"), "true") != 0) continue;
      const char *fn = str_cstr(a, json_as_str(json_get(fl, "name")));
      buf_printf(&t, "%s%s=scaffold+%s", first ? "" : "&", fn, fn);
      first = false;
    }
    if (first) buf_puts(&t, "id=1");
  }
  buf_printf(&t,
             "\"));\n"
             "  ASSERT_REDIRECT(r, \"/admin/%s\");\n"
             "}\n"
             "\n"
             "TEST(a_delete_removes_nothing_that_is_not_there) {\n"
             "  TestClient c = test_client();\n"
             "  Response r = tc_post_body(c, \"/admin/%s/delete\", str_lit(\"id=999999\"));\n"
             "  ASSERT_REDIRECT(r, \"/admin/%s\");\n"
             "}\n"
             "\n"
             "TEST_MAIN()\n",
             snake, snake, snake);
  bool wrote_test = scaffold_write(test_path, &t, force);
  buf_free(&t);

  if (wrote_routes) scaffold_register_source(routes_path);
  if (!wrote_routes && !wrote_test) {
    arena_destroy(a);
    buf_free(&dump);
    return 1;
  }
  info("scaffolded %s: %s%s%s", mn, wrote_routes ? routes_path : "", wrote_test ? " and " : "",
       wrote_test ? test_path : "");
  printf("\n  next:\n"
         "    webc migrate diff --write    if the table does not exist yet\n"
         "    webc check                   build, test, schema\n"
         "    open /admin/%s\n\n",
         mn);
  arena_destroy(a);
  buf_free(&dump);
  return 0;
}

static int cmd_stdlib(i32 argc, char **argv) {
  const char *mod = nullptr, *css = nullptr;
  bool used = false, weigh = false, all = false, json = false;
  for (i32 i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0) json = true;
    else if (strcmp(argv[i], "--used") == 0) used = true;
    else if (strcmp(argv[i], "--weigh") == 0) weigh = true;
    else if (strcmp(argv[i], "--all") == 0) all = true;
    else if (strcmp(argv[i], "--css") == 0) css = "1";
    else if (argv[i][0] != '-') mod = argv[i];
  }

  if (used && json) {
    // which of the shelf this app already includes, as data
    i32 n = stdlib_scan_used();
    Buf j;
    buf_init(&j, nullptr, 8192);
    buf_puts(&j, "{\"used\":[");
    i32 k = 0;
    for (i32 i = 0; i < STDLIB_N; i++)
      if (g_used_hits[i]) {
        if (k++) buf_puts(&j, ",");
        buf_puts(&j, "{\"name\":");
        jputs(&j, STDLIB[i].name);
        buf_printf(&j, ",\"lines\":%d,\"tier\":", header_lines(STDLIB[i].name));
        jputs(&j, STDLIB[i].tier);
        buf_puts(&j, ",\"what\":");
        jputs(&j, STDLIB[i].what);
        buf_puts(&j, "}");
      }
    buf_printf(&j, "],\"count\":%d}\n", n);
    printf("%s", buf_cstr(&j));
    buf_free(&j);
    return 0;
  }
  if (used) {
    return stdlib_used();
  }
  if (json) {
    Buf j;
    buf_init(&j, nullptr, 1 << 15);
    stdlib_scan_used();
    buf_puts(&j, "{\"modules\":[");
    for (i32 i = 0; i < STDLIB_N; i++) {
      if (i) buf_puts(&j, ",");
      buf_puts(&j, "{\"name\":");
      jputs(&j, STDLIB[i].name);
      buf_printf(&j, ",\"tier\":\"%s\",\"lines\":%d,\"used\":%s,\"browser_bytes\":%s,"
                     "\"what\":",
                 STDLIB[i].tier, header_lines(STDLIB[i].name),
                 g_used_hits[i] ? "true" : "false", STDLIB[i].client ? "true" : "false");
      jputs(&j, STDLIB[i].what);
      buf_puts(&j, "}");
    }
    buf_puts(&j, "]}\n");
    printf("%s", buf_cstr(&j));
    buf_free(&j);
    return 0;
  }
  // `--weigh` is the catalogue with the only number that matters added: what the module costs
  // in the browser half, built rather than estimated. It builds probes, so it is opt-in — and
  // on a project it weighs *what the app took* unless `--all` asks for the whole shelf, which
  // is the number an author actually wants ("you use three; they cost you 3.2 KB").
  i32 used_n = stdlib_scan_used();
  if (!mod) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/.webc/stdlib-size", ".");
    if (weigh) mkdir_p(dir);
    i64 floor_bytes = weigh ? stdlib_floor(dir) : -1;
    if (weigh && floor_bytes <= 0) {
      fprintf(stderr, "webc: could not build the size probe (is wasi-sdk installed?)\n");
      return 1;
    }
    printf("\n  \033[1mthe standard library\033[0m — opt-in headers, none of them in the floor\n\n");
    if (weigh)
      printf("    %-10s %-12s %-6s %10s  %s\n", "module", "tier", "lines", "gzip", "what");
    else
      printf("    %-10s %-12s %-6s %s\n", "module", "tier", "lines", "what");
    i64 taken = 0;
    i64 used_lines = 0;
    i32 weighed = 0;
    for (i32 i = 0; i < STDLIB_N; i++) {
      i32 lines = header_lines(STDLIB[i].name);
      if (g_used_hits[i]) used_lines += lines;
      if (!weigh) {
        printf("    %-10s %-12s %-6d %s\n", STDLIB[i].name, STDLIB[i].tier, lines,
               STDLIB[i].what);
        continue;
      }
      bool want = all || g_used_hits[i];
      char one[32] = "—";
      if (want) {
        i64 w = stdlib_weight(dir, floor_bytes, STDLIB[i].name);
        if (w < 0) snprintf(one, sizeof one, "n/a");
        else {
          human_size(one, sizeof one, w);
          taken += w;
          weighed++;
        }
      }
      printf("    %-10s %-12s %-6d %10s  %s\n", STDLIB[i].name, STDLIB[i].tier, lines, one,
             STDLIB[i].what);
    }
    if (weigh) {
      char total[32];
      human_size(total, sizeof total, taken);
      if (all)
        printf("\n    the whole shelf, in the browser half: %s gz on a floor of ", total);
      else
        printf("\n    what this app takes (%d module%s): %s gz on a floor of ", weighed,
               weighed == 1 ? "" : "s", total);
      char f[32];
      human_size(f, sizeof f, floor_bytes);
      printf("%s gz\n    A line with \"—\" is a module this page does not include, and an\n"
             "    included one that costs nothing is a server-side module: no browser bytes.\n",
             f);
    }
    printf("\n  include <webc/MODULE.h> — that is the whole manifest, and the whole cost.\n"
           "  webc stdlib <module>         what it is, and what it adds to your app\n"
           "  webc stdlib <module> --css   the stylesheet it expects (copy it, or @import)\n"
           "  webc stdlib --weigh          the catalogue with what each one costs, built\n"
           "  webc stdlib --weigh --all    the same for the whole shelf, taken or not\n"
           "  webc stdlib --used           which modules this app already includes\n"
           "  webc add <module>            install one: its sheet into public/, and the lines\n");
    if (used_n)
      printf("\n  this app takes %d of %d — %lld lines of header it did not write\n", used_n,
             STDLIB_N, (long long)used_lines);
    printf("\n");
    return 0;
  }

  if (!stdlib_is(mod)) {
    fprintf(stderr, "webc: '%s' is not a standard-library module\n  try: ", mod);
    for (i32 i = 0; i < STDLIB_N; i++) fprintf(stderr, "%s%s", i ? ", " : "", STDLIB[i].name);
    fprintf(stderr, "\n");
    return 1;
  }

  if (css) {
    char path[1024];
    snprintf(path, sizeof path, "%s/assets/stdlib/%s.css", webc_prefix(), mod);
    i64 n = 0;
    char *text = read_all(path, &n);
    if (!text) {
      printf("  %s ships no stylesheet of its own\n", mod);
      return 0;
    }
    fwrite(text, 1, (size_t)n, stdout);
    free(text);
    return 0;
  }

  char path[1024];
  snprintf(path, sizeof path, "%s/include/webc/%s.h", webc_prefix(), mod);
  i64 n = 0;
  char *text = read_all(path, &n);
  if (!text) {
    fprintf(stderr, "webc: cannot read %s (set WEBC_HOME)\n", path);
    return 1;
  }
  const char *what = "";
  for (i32 i = 0; i < STDLIB_N; i++)
    if (strcmp(STDLIB[i].name, mod) == 0) what = STDLIB[i].what;
  printf("\n  \033[1mwebc/%s.h\033[0m — %s\n\n", mod, what);
  // the header's own banner, verbatim: the doc comment is the documentation
  for (const char *p = text; *p;) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    if (!(len >= 2 && p[0] == '/' && p[1] == '/')) break;
    printf("  %.*s\n", (int)len, p);
    if (!eol) break;
    p = eol + 1;
  }
  printf("\n  what it adds:\n");
  for (const char *p = text; *p;) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    bool is_fn = len > 19 && memcmp(p, "static inline void wc_", 21) == 0;
    bool is_ret = len > 18 && memcmp(p, "static inline ", 14) == 0 && memchr(p, '(', len) &&
                  memmem(p, len, " wc_", 4);
    bool is_def = len > 7 && memcmp(p, "#define ", 8) == 0;
    if (is_fn || is_ret || is_def) {
      const char *s0 = p;
      const char *paren = memchr(p, '(', len);
      const char *end = is_def ? p + 8 : (paren ? paren : p + len);
      while (end > s0 && (end[-1] == ' ' || end[-1] == '*')) end--;
      printf("    %.*s\n", (int)(end - s0), s0);
    }
    if (!eol) break;
    p = eol + 1;
  }
  printf("\n");
  free(text);
  return 0;
}

void api_search_json(Buf *j, const char *query, i32 limit) {
  char dir[1200];
  snprintf(dir, sizeof dir, "%s/include/webc", webc_prefix());
  DIR *d = opendir(dir);
  buf_puts(j, "{\"query\":");
  jputs(j, query);
  buf_puts(j, ",\"matches\":[");
  i32 found = 0;
  struct dirent *e;
  if (d) {
    while ((e = readdir(d)) && found < limit) {
      size_t ln = strlen(e->d_name);
      if (ln < 4 || strcmp(e->d_name + ln - 2, ".h")) continue;
      char path[1400];
      snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
      i64 n = 0;
      char *text = read_all(path, &n);
      if (!text) continue;
      i32 line_no = 0;
      char doc[512] = "";
      bool in_comment = false;
      for (char *l = text; l && *l && found < limit;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        line_no++;
        char *p = l;
        while (*p == ' ') p++;
        // The doc sentence is the first line of the `//` block above the declaration: one
        // sentence per subject is the house style, and a block that runs to three lines is
        // the framework explaining itself, which is far better than the last line of it.
        bool comment = p[0] == '/' && p[1] == '/' && p[2] != '/';
        if (comment) {
          if (!in_comment) {
            snprintf(doc, sizeof doc, "%s", p + 2);
            while (doc[0] == ' ') memmove(doc, doc + 1, strlen(doc));
            in_comment = true;
          }
        } else if (*p) {
          in_comment = false;
        }
        // A declaration or a macro line, not a mention in a comment.
        if (!comment && *p && strstr(p, query) && (strchr(p, '(') || strstr(p, "#define"))) {
          if (found++) buf_puts(j, ",");
          buf_puts(j, "{\"header\":");
          jputs(j, e->d_name);
          buf_printf(j, ",\"line\":%d,\"declaration\":", line_no);
          jputs(j, p);
          buf_puts(j, ",\"doc\":");
          jputs(j, doc);
          buf_puts(j, "}");
        }
        l = nl ? nl + 1 : nullptr;
      }
      free(text);
    }
    closedir(d);
  }
  buf_printf(j, "],\"count\":%d}", found);
}

i32 api_search_text(const char *query, i32 limit) {
  Buf j;
  buf_init(&j, nullptr, 1 << 14);
  api_search_json(&j, query, limit);
  // The same records, printed the way a person reads them. Re-parsing my own JSON would be
  // silly, so the fields are read back out of the Buf with the two calls that built them:
  // the object is flat and the keys are known, which is all this needs.
  printf("\n  \033[1mwebc api\033[0m  %s\n\n", query);
  i32 shown = 0;
  const char *p = buf_cstr(&j);
  while ((p = strstr(p, "{\"header\":\""))) {
    p += 11;
    const char *he = strchr(p, '"');
    char header[128] = "";
    if (he && (size_t)(he - p) < sizeof header) snprintf(header, sizeof header, "%.*s", (int)(he - p), p);
    const char *ln = strstr(he ? he : p, "\"line\":");
    i32 line = ln ? atoi(ln + 7) : 0;
    const char *ds = strstr(ln ? ln : p, "\"declaration\":\"");
    char decl[512] = "";
    if (ds) {
      ds += 15;
      const char *de = ds;
      while (*de && !(*de == '"' && de[-1] != '\\')) de++;
      snprintf(decl, sizeof decl, "%.*s", (int)(de - ds), ds);
    }
    const char *ocs = strstr(ds ? ds : p, "\"doc\":\"");
    char doc[512] = "";
    if (ocs) {
      ocs += 7;
      const char *oe = ocs;
      while (*oe && !(*oe == '"' && oe[-1] != '\\')) oe++;
      snprintf(doc, sizeof doc, "%.*s", (int)(oe - ocs), ocs);
    }
    printf("  %s%s:%d%s\n", g_d, header, line, g_x);
    printf("    %s\n", decl);
    if (doc[0]) printf("    %s%s%s\n", g_y, doc, g_x);
    shown++;
    if (shown >= limit) break;
  }
  if (!shown) {
    printf("  nothing in 56 headers matches that — the whole surface is `webc api`\n\n");
    buf_free(&j);
    return 1;
  }
  printf("\n");
  buf_free(&j);
  return 0;
}

static int cmd_api(bool write_manifest, bool verbose, bool json, const char *query, i32 limit) {
  char dir[1024];
  snprintf(dir, sizeof dir, "%s/include/webc", webc_prefix());
  DIR *d = opendir(dir);
  if (!d) die("cannot read %s (set WEBC_HOME)", dir);
  ApiSet s = {0};
  struct dirent *e;
  i32 headers = 0;
  while ((e = readdir(d))) {
    size_t len = strlen(e->d_name);
    if (len < 3 || strcmp(e->d_name + len - 2, ".h") != 0) continue;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    api_scan(path, e->d_name, &s);
    headers++;
  }
  closedir(d);
  qsort(s.names, (size_t)s.n, sizeof *s.names, api_cmp);

  i32 unprefixed_macros = 0;
  for (i32 i = 0; i < s.n; i++)
    if (s.names[i].macro && !api_prefixed(s.names[i].name)) unprefixed_macros++;
  // The shelf is counted, not hidden: a name in webc/<module>.h is opt-in, so it enlarges
  // what an app *may* use without enlarging what every app pays for. The two numbers move
  // together in the manifest line so the difference stays visible.
  i32 shelf = api_stdlib_names(&s);
  i32 core = s.n - shelf;

  if (query && *query) {
    if (json) {
      Buf j;
      buf_init(&j, nullptr, 1 << 14);
      api_search_json(&j, query, limit);
      printf("%s\n", buf_cstr(&j));
      buf_free(&j);
      return 0;
    }
    return api_search_text(query, limit);
  }
  if (json) {
    // The surface as data: the same names `api.txt` holds, with the count a reviewer
    // argues about. `webc api --json | jq` is what a tool (and an agent) asks instead
    // of parsing a line of prose.
    Buf j;
    buf_init(&j, nullptr, 1 << 16);
    buf_printf(&j,
               "{\"count\":%d,\"macros\":%d,\"prototypes\":%d,\"headers\":%d,"
               "\"core\":%d,\"shelf\":%d,\"unprefixed_macros\":%d,\"names\":[",
               s.n, s.macros, s.protos, headers, core, shelf, unprefixed_macros);
    for (i32 i = 0; i < s.n; i++) {
      if (i) buf_puts(&j, ",");
      buf_puts(&j, "{\"name\":");
      jputs(&j, s.names[i].name);
      buf_printf(&j, ",\"kind\":\"%s\"}", s.names[i].macro ? "macro" : "prototype");
    }
    buf_puts(&j, "]}\n");
    printf("%s", buf_cstr(&j));
    buf_free(&j);
    for (i32 i = 0; i < s.n; i++) free(s.names[i].name);
    free(s.names);
    return 0;
  }

  Buf man;
  buf_init(&man, nullptr, 1 << 16);
  buf_printf(&man,
             "# webc api — %d names, %d macros, %d prototypes, %d headers\n"
             "# of those, %d are standard-library names (opt-in) and %d are core\n"
             "# written by `webc api --write`, checked by `webc api` (P10's surface ratchet).\n"
             "# A name that leaves this file was deleted on purpose. A name that appears was\n"
             "# added — and adding one is the change the budget exists to make you think about.\n",
             s.n, s.macros, s.protos, headers, shelf, core);
  // `--write` used to drop every hand-written line, so recording a new name also destroyed the
  // reasons the last one was added ("1841: WEBC_VERSION, the framework's own number…") — the part
  // that makes a surface ratchet reviewable rather than a list of words. The reasons are carried
  // across instead: every comment line that is not the generated preamble and not a generated
  // `# module` count survives, in order, right after the preamble.
  {
    i64 oldn = 0;
    char *prev = read_all("api.txt", &oldn);
    if (prev) {
      for (char *line = prev; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        bool generated = !strncmp(line, "# webc api ", 11) || !strncmp(line, "# of those, ", 12) ||
                         !strncmp(line, "# written by ", 13) || !strncmp(line, "# A name that ", 14) ||
                         !strncmp(line, "# added ", 8) || !strncmp(line, "# module ", 9);
        if (!generated && (line[0] == 0 || (line[0] == '#' && line[1] == ' ')))
          buf_printf(&man, "%s\n", line);
        line = nl ? nl + 1 : nullptr;
      }
      free(prev);
    }
  }
  // the per-module lines next, as comments, so the name list stays a plain one-name-per-line
  // file that `diff` and `grep` still understand
  {
    i32 counts[64];
    api_module_counts(counts);
    for (i32 i = 0; i < STDLIB_N && i < 64; i++)
      buf_printf(&man, "# module %s %d\n", STDLIB[i].name, counts[i]);
  }
  for (i32 i = 0; i < s.n; i++) buf_printf(&man, "%s\n", s.names[i].name);

  for (i32 i = 0; i < s.n; i++) free(s.names[i].name);
  free(s.names);

  // `api.txt` at the project root, not inside `.webc/`: the manifest is a *policy* the app
  // commits, and `.webc/` is build output that every `.gitignore` in the world drops. A budget
  // file that is not in the repository is a budget nobody else can be held to.
  const char *path = "api.txt";
  if (write_manifest) {
    mkdirs(".webc");
    write_file(path, buf_cstr(&man));
    printf("\033[1mapi\033[0m %d names (%d macros, %d prototypes, %d headers, %d unprefixed macros)\n"
           "      wrote %s  (%d core names, %d shelf names)\n",
           s.n, s.macros, s.protos, headers, unprefixed_macros, path, core, shelf);
    buf_free(&man);
    return 0;
  }

  i64 old_n = 0;
  char *old = read_all(path, &old_n);
  if (!old) {
    printf("\033[1mapi\033[0m %d names (%d macros, %d prototypes, %d headers, %d unprefixed macros)\n"
           "      %d core + %d shelf\n"
           "      no manifest — run `webc api --write` to record this surface\n",
           s.n, s.macros, s.protos, headers, unprefixed_macros, core, shelf);
    buf_free(&man);
    return 0;
  }
  i32 budget = -1;
  if (sscanf(old, "# webc api — %d names", &budget) != 1) budget = -1;
  // the names this build has that the manifest does not, and the reverse
  i32 added = 0, removed = 0;
  {
    const char *p = strchr(man.p, '\n'); // skip the manifest's own comment block
    while (p && p[1] == '#') p = strchr(p + 1, '\n');
    while (p && *p) {
      p++;
      const char *e2 = strchr(p, '\n');
      if (!e2) break;
      char name[160];
      size_t len = (size_t)(e2 - p);
      if (len && len < sizeof name) {
        memcpy(name, p, len);
        name[len] = 0;
        char probe[176];
        snprintf(probe, sizeof probe, "\n%s\n", name);
        if (!strstr(old, probe)) {
          added++;
          if (verbose) printf("  \033[32m+\033[0m %s\n", name);
        }
      }
      p = e2;
    }
    p = strchr(old, '\n');
    while (p && p[1] == '#') p = strchr(p + 1, '\n');
    while (p && *p) {
      p++;
      const char *e2 = strchr(p, '\n');
      if (!e2) break;
      char name[160];
      size_t len = (size_t)(e2 - p);
      if (len && len < sizeof name) {
        memcpy(name, p, len);
        name[len] = 0;
        char probe[176];
        snprintf(probe, sizeof probe, "\n%s\n", name);
        if (!strstr(man.p, probe)) {
          removed++;
          if (verbose) printf("  \033[31m-\033[0m %s\n", name);
        }
      }
      p = e2;
    }
  }
  // each module against its own line
  i32 grew = 0, shrunk = 0;
  char grew_name[64] = {0};
  i32 grew_by = 0;
  {
    i32 counts[64];
    api_module_counts(counts);
    for (i32 i = 0; i < STDLIB_N && i < 64; i++) {
      char tag[96];
      snprintf(tag, sizeof tag, "# module %s ", STDLIB[i].name);
      const char *at = strstr(old, tag);
      if (!at) continue;
      i32 was = atoi(at + strlen(tag));
      if (counts[i] > was) {
        grew++;
        if (!grew_name[0]) {
          snprintf(grew_name, sizeof grew_name, "%s", STDLIB[i].name);
          grew_by = counts[i] - was;
        }
      } else if (counts[i] < was) {
        shrunk += was - counts[i]; // smaller is not a failure: the ratchet only locks growth
      }
    }
  }

  bool ok = budget < 0 || s.n <= budget;
  printf("\033[1mapi\033[0m %d names (%d macros, %d prototypes, %d headers, %d unprefixed macros)"
         "  budget %d, %+d, %d removed\n"
         "      %d core + %d standard-library (opt-in)\n",
         s.n, s.macros, s.protos, headers, unprefixed_macros, budget, added, removed, core, shelf);
  if (!ok)
    printf("      \033[31mthe surface grew by %d\033[0m — delete a name, or raise the budget in "
           "api.txt and say why\n",
           s.n - budget);
  if (grew)
    printf("      \033[31m%s grew by %d\033[0m — a module's own line is what keeps the shelf "
           "pick-and-choose; raise its line in api.txt on purpose\n",
           grew_name, grew_by);
  if (shrunk)
    printf("      \033[2m%d shelf names gone — rewrite the manifest to lock the smaller shelf\033[0m\n",
           shrunk);
  free(old);
  buf_free(&man);
  return ok && grew == 0 ? 0 : 1;
}


// ---------------------------------------------------------------- lint
//
// `webc lint` — the no-leak gate.
//
// It is a table, not a paragraph. Every retired spelling is one row, the rows accumulate, and
// `webc check` runs the table over app code, the scaffolds and the docs. A token list written
// out in prose cannot see itself grow, cannot see a rule name a file that moved, and cannot
// see a rule whose surface is gone.

typedef struct {
  const char *token;
  const char *what;    // what it is
  const char *instead; // what to write
  u32 scopes;          // LINT_CODE, LINT_DOC, LINT_TEST
} LintRule;

#define LINT_CODE 1u
#define LINT_TEST 2u
#define LINT_DOC 4u
#define LINT_ALL 7u

static const LintRule LINT_RULES[] = {
    // ---- P1: the second way
    {"text_s(", "the str flavour of text", "text(\"%s\", s)", LINT_ALL},
    {"wc_res_text(", "the body builder behind res_text", "res_text(200, \"...\")", LINT_CODE},
    {"wc_res_html(", "the body builder behind res_html", "res_html(200, \"...\")", LINT_CODE},
    {"res_json_raw(", "a JSON body from bytes", "json(200, \"{...}\")", LINT_ALL},
    {"res_json_str(", "a JSON body from a str", "json(200, \"{...}\")", LINT_ALL},
    {"json_str(", "a JSON body from a str", "json(200, s)", LINT_ALL},
    {"json_vec(", "a JSON array that needed its type spelled", "json_list(200, rows)", LINT_ALL},
    {"json_error(", "an error body", "res_error(404, \"not_found\", \"...\")", LINT_ALL},
    {"head_html(fmt(\"<script", "a <script> body built by hand", "head_ld(ld_blog_posting(...))", LINT_ALL},
    {"res_ssr(", "the long render call", "page(View, ctx)", LINT_ALL},
    {"res_page(", "the long render call", "page(View, ctx)", LINT_ALL},
    {"res_page_auto(", "the long render call", "page(View, ctx)", LINT_ALL},
    {"view_as(", "a second way to name a view", "declare_view(Name, Ctx); + view(Name, Ctx *c)", LINT_ALL},
    {"VIEW_CTX(", "a second way to declare a view", "declare_view(Name, Ctx)", LINT_ALL},
    {"mount_ctx(", "a second way to mount", "mount(View, ctx, \"#root\")", LINT_ALL},
    {"raw(", "a second name for authored markup", "markup(\"<svg ...>\")", LINT_ALL},
    {"respond_model(", "the four-way response macro", "(wc_body_is_form() ? redirect(\"/\") : json(200, T, p))", LINT_ALL},
    {"WEBC_VEC_DEFINE", "a hand-named vector", "Vec(T)", LINT_ALL},
    {"WEBC_VEC_DECLARE", "a hand-named vector", "Vec(T)", LINT_ALL},
    {"webc/webc.h", "the do-everything include", "<webc/server.h> or <webc/frontend.h>", LINT_ALL},
    // ---- P2: the namespace
    {"(void)(ctx);", "an ignored parameter", "write the signature the view needs", LINT_ALL},
    // ---- P4: wiring by string
    {"wc_bind_hook(", "wiring an action by name", "on_click(action_name)", LINT_ALL},
    {"wc_bind_str(", "wiring an input by name", "bind(sig)", LINT_ALL},
    {"wc_hook(", "registering a name for wires.h", "on_click(action_name)", LINT_ALL},
    {"wc_input_hook(", "registering an input name", "bind(sig)", LINT_ALL},
    {"webc wires", "the wiring codegen step", "nothing: wiring is a symbol now", LINT_DOC},
    // ---- P3/P5: one state, one string
    {"signal_str(", "the str flavour of a signal", "state(Text, name, \"\")", LINT_ALL},
    {"w_state_register(", "the state registry by hand", "state(...)", LINT_ALL},
    {"str_fmt(", "formatting into a str", "fmt()", LINT_ALL},
    {"str_cstr(", "turning a str into a char*", "Text is already char*", LINT_ALL},
    {"str_dup(", "copying into an arena", "text_copy(), persist()", LINT_ALL},
    {"arena_alloc(", "allocating by hand", "fmt(), text_copy(), persist()", LINT_ALL},
    {"arena_new", "an arena in app code", "nothing: the scope arena is installed", LINT_ALL},
    {"wc_arena_get", "asking which arena is current", "fmt(), text_copy(), persist()", LINT_ALL},
    // ---- the target and the protocol
    {"__wasm__", "a target test in app code", "list the file under \"client\"", LINT_ALL},
    {"export_name", "the wasm export protocol", "nothing: the framework exports", LINT_ALL},
    {"wc_app_main", "the wasm entry point", "client_main { ... }", LINT_ALL},
    {"webc_data", "the hydration protocol", "page(View, ctx)", LINT_ALL},
    {"webc_body_into", "unsafe body parsing", "body(T)", LINT_ALL},
    {"->arena", "reaching into a request", "nothing", LINT_ALL},
    {"db_error(", "surfacing a driver message", "nothing: db_* already does", LINT_ALL},
    {"res_error(500", "a hand-built driver error", "fail(409, \"...\")", LINT_ALL},
    {"_ref(", "indexing a vector by hand", "for_each(item, vec)", LINT_ALL},
    {"%.*s", "str internals in a format string", "a Text field prints with %s", LINT_ALL},
    // ---- a unit that lived only in a comment
    {"parse_iso(", "a stamp in an unnamed unit (it is milliseconds)", "parse_iso_ms()", LINT_ALL},
};

static i32 g_lint_fail;
static bool g_lint_verbose;
static i32 g_lint_hits;
// When a caller asked for `--json`, every hit is also recorded here instead of being
// printed. A hit is a *fact* — the file, the line, the retired token and the sentence
// to write instead — so the same table serves a person reading a terminal and an agent
// reading data.
static Buf *g_lint_hits_json;

static bool lint_skip_path(const char *path) {
  return strstr(path, "/tests/") || strstr(path, "/.git/") || strstr(path, "/.webc/") ||
         strstr(path, "/build/") || strstr(path, "/node_modules/");
}

static void lint_file(const char *path, u32 scope) {
  i64 n = 0;
  char *text = read_all(path, &n);
  if (!text) return;
  // a rule that names a *deleted* token is only worth reporting where the token can appear:
  // app sources and docs. Docs get the doc-scope rows only.
  // A file generated from other prose is not a second author's opinion: the reference and
  // the guides are gated where they live, and they name these words inline on purpose
  // (§13 discusses the wasm protocol in its own terms). Re-reporting the copy as the
  // skill's mistake would make the gate noise instead of a ratchet.
  if (strstr(text, "generated by `make skill`")) {
    free(text);
    return;
  }
  i32 hits_per_rule[WC_COUNTOF(LINT_RULES)] = {0};
  i32 line = 1;
  for (const char *p = text; *p; p++) {
    if (*p == '\n') line++;
    if (p > text && p[-1] != '\n' && p[-1] != ' ' && p[-1] != '\t') continue;
    for (size_t r = 0; r < WC_COUNTOF(LINT_RULES); r++) {
      const LintRule *rule = &LINT_RULES[r];
      if (!(rule->scopes & scope)) continue;
      if (hits_per_rule[r] >= 3) continue;
      size_t len = strlen(rule->token);
      if (strncmp(p, rule->token, len) != 0) continue;
      // words, not substrings: `attr(` must not fire inside `xattr_c(`
      hits_per_rule[r]++;
      g_lint_hits++;
      if (g_lint_hits_json) {
        if (g_lint_hits_json->n > 1) buf_puts(g_lint_hits_json, ",");
        buf_puts(g_lint_hits_json, "{\"file\":");
        jputs(g_lint_hits_json, path);
        buf_printf(g_lint_hits_json, ",\"line\":%d,\"token\":", line);
        jputs(g_lint_hits_json, rule->token);
        buf_puts(g_lint_hits_json, ",\"what\":");
        jputs(g_lint_hits_json, rule->what);
        buf_puts(g_lint_hits_json, ",\"instead\":");
        jputs(g_lint_hits_json, rule->instead);
        buf_puts(g_lint_hits_json, "}");
      }
      if (!g_lint_verbose && hits_per_rule[r] > 1) continue;
      if (!g_lint_hits_json)
        printf("  %s:%d: %s — %s\n      write instead: %s\n", path, line, rule->token, rule->what,
               rule->instead);
      g_lint_fail = 1;
      if (hits_per_rule[r] == 3) printf("  %s:%d: (further hits of %s not shown)\n", path, line,
                                        rule->token);
    }
  }
  free(text);
}

static const char *scope_name(u32 scopes) {
  if (scopes == LINT_ALL) return "all";
  if (scopes == LINT_CODE) return "code";
  if (scopes == LINT_TEST) return "tests";
  if (scopes == LINT_DOC) return "docs";
  return "code";
}

// `webc lint --rules` — the retired spellings as a table, generated from the same array
// the gate runs, so a page that teaches them cannot drift from the check that forbids
// them. `--json` is for the tools that build documentation out of it.
static int cmd_lint_rules(bool json) {
  if (json) {
    Buf j;
    buf_init(&j, nullptr, 8192);
    buf_puts(&j, "{\"rules\":[");
    for (size_t r = 0; r < WC_COUNTOF(LINT_RULES); r++) {
      if (r) buf_puts(&j, ",");
      buf_puts(&j, "{\"token\":");
      jputs(&j, LINT_RULES[r].token);
      buf_puts(&j, ",\"what\":");
      jputs(&j, LINT_RULES[r].what);
      buf_puts(&j, ",\"instead\":");
      jputs(&j, LINT_RULES[r].instead);
      buf_puts(&j, ",\"scopes\":");
      jputs(&j, scope_name(LINT_RULES[r].scopes));
      buf_puts(&j, "}");
    }
    buf_printf(&j, "],\"count\":%d}\n", (int)WC_COUNTOF(LINT_RULES));
    printf("%s", buf_cstr(&j));
    buf_free(&j);
    return 0;
  }
  printf("# The retired spellings\n\n");
  printf("Generated by `webc lint --rules` from the table `webc check` runs. A line in\n"
         "the left column is a word an app may not name; the right column is what to write\n"
         "instead. The gate reads code, tests and docs; a page exempted in `lint.exempt`\n"
         "may name a spelling on purpose, because it is quoting it in order to forbid it.\n\n");
  printf("| never write | what it is | write instead | scanned |\n"
         "|---|---|---|---|\n");
  for (size_t r = 0; r < WC_COUNTOF(LINT_RULES); r++) {
    printf("| `%s` | %s | %s | %s |\n", LINT_RULES[r].token, LINT_RULES[r].what,
           LINT_RULES[r].instead, scope_name(LINT_RULES[r].scopes));
  }
  printf("\n%d rules.\n", (int)WC_COUNTOF(LINT_RULES));
  return 0;
}

// Paths (relative to the project root, or absolute) that may name a retired spelling on
// purpose. Set from `lint.exempt` in webc.json before any walk starts.
static const char *g_lint_exempt[16];
static i32 g_lint_exempt_n;

static bool lint_exempted(const char *path) {
  for (i32 i = 0; i < g_lint_exempt_n; i++) {
    const char *want = g_lint_exempt[i];
    size_t wn = strlen(want), pn = strlen(path);
    if (wn && pn >= wn && strcmp(path + pn - wn, want) == 0) return true;
  }
  return false;
}

static void lint_walk(const char *dir, u32 scope, int depth) {
  if (depth > 6 || lint_skip_path(dir)) return;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      lint_walk(p, scope, depth + 1);
      continue;
    }
    size_t len = strlen(e->d_name);
    bool is_c = (len > 2 && !strcmp(e->d_name + len - 2, ".c")) ||
                (len > 2 && !strcmp(e->d_name + len - 2, ".h"));
    // `traps.md` is generated by `webc lint --rules` and exists to list exactly the
    // forbidden words, so it is never scanned. Every other page is written for someone
    // learning the surface as it is.
    bool is_md = len > 3 && !strcmp(e->d_name + len - 3, ".md") && strcmp(e->d_name, "traps.md");
    if (is_md && lint_exempted(p)) is_md = false; // a page that quotes a spelling to forbid it
    if (is_c && (scope & LINT_CODE)) lint_file(p, strstr(p, "/tests/") ? LINT_TEST : LINT_CODE);
    else if (is_md && (scope & LINT_DOC)) lint_file(p, LINT_DOC);
  }
  closedir(d);
}

// Generate one scaffold with this very binary and lint what it emitted. A template is
// the first five minutes of the framework; it may not name anything an app file may not.
static void lint_scaffold(const char *tmp, const char *tpl) {
  char dir[1200], cmd[1600];
  snprintf(dir, sizeof dir, "%s/%s", tmp, tpl);
  snprintf(cmd, sizeof cmd, "'%s' new '%s' --template=%s >/dev/null 2>&1", g_self, dir, tpl);
  if (!run_cmd(cmd, true)) return;
  char src[1300];
  snprintf(src, sizeof src, "%s/src", dir);
  if (path_exists(src)) lint_walk(src, LINT_CODE, 0);
}

// `webc lint --stdlib` — the wrong half. A shelf module with no browser half (`upload`, `mail`,
// `jobs`, `limits`, `pwa`) included by a file that compiles into the browser half is a link
// error with a good story: the compiler will say `undefined symbol: smtp_open` in a language
// nobody asked for. This says the sentence instead, and names the fix.
//
// It cannot be an error in `webc lint`'s other passes, because the compiler is the authority on
// what links; this is the *nice* version of a message the author is going to see anyway.
static const char *STDLIB_SERVER_ONLY[] = {"upload", "mail", "jobs", "limits", "pwa"};
#define STDLIB_SERVER_ONLY_N ((i32)(sizeof STDLIB_SERVER_ONLY / sizeof STDLIB_SERVER_ONLY[0]))

static i32 g_stdlib_hits;

static void lint_wrong_half(const char *path, bool client_half) {
  if (!client_half) return;
  i64 n = 0;
  char *text = read_all(path, &n);
  if (!text) return;
  // a file that names `client_main` is the browser half whatever webc.json says: that is the
  // macro the wasm build defines an entry point from
  bool client = memmem(text, (size_t)n, "client_main", 11) != nullptr;
  for (i32 i = 0; i < STDLIB_SERVER_ONLY_N; i++) {
    char needle[96];
    snprintf(needle, sizeof needle, "<webc/%s.h>", STDLIB_SERVER_ONLY[i]);
    const char *at = memmem(text, (size_t)n, needle, strlen(needle));
    if (!at) continue;
    // the line number, because "somewhere in this file" is not a finding
    i32 line = 1;
    for (const char *q = text; q < at; q++) line += *q == '\n';
    printf("  \033[31m%s:%d\033[0m includes <webc/%s.h>, which has no browser half\n"
           "      %s\n",
           path, line, STDLIB_SERVER_ONLY[i],
           client ? "this file has a client_main: move the call into a route, or a `server` source"
                  : "it is in the client set: move the call into a `server` source");
    g_stdlib_hits++;
    break;
  }
  free(text);
}

static void lint_wrong_half_walk(const char *dir, i32 depth) {
  if (depth > 6) return;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      lint_wrong_half_walk(p, depth + 1);
      continue;
    }
    size_t len = strlen(e->d_name);
    if (len > 2 && strcmp(e->d_name + len - 2, ".c") == 0) lint_wrong_half(p, true);
  }
  closedir(d);
}

static int cmd_lint(bool all, bool verbose, bool json) {
  g_lint_fail = 0;
  g_lint_exempt_n = 0;
  g_stdlib_hits = 0;
  g_lint_verbose = verbose;
  g_lint_hits = 0;
  Buf hits;
  buf_init(&hits, nullptr, 4096);
  buf_puts(&hits, "{\"hits\":[");
  g_lint_hits_json = json ? &hits : nullptr;
  Config c;
  config_load(&c, ".");
  for (i32 i = 0; i < c.lint_exempt_n && i < 16; i++) g_lint_exempt[g_lint_exempt_n++] = c.lint_exempt[i];
  // In the framework's own repository, src/ and include/ are exactly where these names
  // belong: the gate is about what an *app author* reads. There is no webc.json at
  // the root of the framework, and there is one at the root of every app.
  bool framework = !path_exists("webc.json") && path_exists("include/webc");
  if (!json) printf("\n%s  webc lint%s\n\n", g_b, g_x);
  if (framework) {
    all = true;
    if (!json) printf("  framework repository: linting the examples, the scaffolds and the docs\n");
  } else {
    // the app's own sources, wherever webc.json says they are
    if (c.has_sets) {
      for (i32 i = 0; i < c.shared.n; i++) lint_file(c.shared.paths[i], LINT_CODE);
      for (i32 i = 0; i < c.server.n; i++) lint_file(c.server.paths[i], LINT_CODE);
      for (i32 i = 0; i < c.client.n; i++) lint_file(c.client.paths[i], LINT_CODE);
    }
    if (path_exists("src")) lint_walk("src", LINT_CODE, 0);
    if (path_exists("tests")) lint_walk("tests", LINT_TEST, 0);
    if (path_exists("public")) lint_walk("public", LINT_CODE, 0);
    // The wrong-half pass, always on for an app: it is a compile error with a good story, and
    // `webc lint` is where a story belongs. `--stdlib` is accepted for the shape the proposal
    // promised; it does not change what is scanned.
    lint_wrong_half_walk("src", 0);
  }
  if (all) {
    lint_walk("examples", LINT_CODE, 0);
    lint_walk("docs", LINT_DOC, 0);
    // the scaffolds, generated fresh: a template that leaks is worse than an example that
    // does, because nobody sees it until they run it.
    char tmp[] = "/tmp/webc-lint-XXXXXX";
    if (mkdtemp(tmp)) {
      lint_scaffold(tmp, "server");
      lint_scaffold(tmp, "web");
      lint_scaffold(tmp, "fullstack");
      char rm[1200];
      snprintf(rm, sizeof rm, "rm -rf '%s'", tmp);
      run_cmd(rm, true);
    }
  }
  if (g_stdlib_hits) {
    if (!json)
      printf("\n  \033[31ma standard-library module is in the wrong half\033[0m (%d)\n"
             "  the browser half links `webc/upload`, `mail`, `jobs`, `limits` and `pwa` as nothing:\n"
             "  they are server modules, whatever the include line hopes\n\n",
             g_stdlib_hits);
    g_lint_fail++;
  }
  if (json) {
    buf_printf(&hits, "],\"total\":%d,\"wrong_half\":%d,\"fail\":%s,\"all\":%s}\n",
               g_lint_hits, g_stdlib_hits, g_lint_fail ? "true" : "false",
               all ? "true" : "false");
    g_lint_hits_json = nullptr;
    printf("%s", buf_cstr(&hits));
    buf_free(&hits);
    return g_lint_fail ? 1 : 0;
  }
  if (g_lint_fail) {
    printf("\n  \033[31mthe developer surface leaks an internal\033[0m (\033[1m%d\033[0m hit%s)\n"
           "  one line per rule is shown; webc lint --verbose shows every occurrence,\n"
           "  and --all adds the examples and the docs\n\n",
           g_lint_hits, g_lint_hits == 1 ? "" : "s");
    return 1;
  }
  printf("  clean — %s names nothing a developer should not\n\n",
         all ? "the app, the examples, the scaffolds and the docs" : "the app");
  return 0;
}

// `webc lint --skill DIR` — the same table, doc scope, over a skill's prose. A skill
// that teaches a retired spelling is worse than no skill: it is a language nobody can
// compile, delivered with confidence. `traps.md` is exempt because it names the words in
// order to forbid them.
static int cmd_lint_skill(const char *dir, bool json) {
  g_lint_fail = 0;
  g_stdlib_hits = 0;
  g_lint_hits = 0;
  g_lint_verbose = false;
  Buf hits;
  buf_init(&hits, nullptr, 4096);
  buf_puts(&hits, "{\"hits\":[");
  g_lint_hits_json = json ? &hits : nullptr;
  lint_walk(dir, LINT_DOC, 0);
  if (json) {
    buf_printf(&hits, "],\"total\":%d,\"fail\":%s}\n", g_lint_hits,
               g_lint_fail ? "true" : "false");
    g_lint_hits_json = nullptr;
    printf("%s", buf_cstr(&hits));
    buf_free(&hits);
  } else if (g_lint_fail) {
    printf("\n  %s%d retired spelling%s in %s%s\n\n", g_r, g_lint_hits,
           g_lint_hits == 1 ? "" : "s", dir, g_x);
  } else {
    printf("  clean — %s teaches nothing the framework retired\n\n", dir);
  }
  return g_lint_fail ? 1 : 0;
}

// ---------------------------------------------------------------- main

static void usage(void) {
  printf("webc %s — the WebC toolchain\n\n", CLI_VERSION);
  printf("  webc new <dir> [--template=server|web|fullstack|board]\n"
         "                            scaffold a project\n");
  printf("  webc build [--release]    compile the app\n");
  printf("  webc dev                  build, run, reload on save\n");
  printf("  webc serve [--port N] [--host H] [--no-build] [--quiet]\n"
         "                            a release build, served with gzip, no reload\n");
  printf("  webc test [--filter=suite] [--test=name] [--list] [--macros] [--json] [--watch]\n"
         "                            run the suites; --watch reruns on change\n");
  printf("  webc check [--json]         build, test, doctor, boot, schema, routes, lint\n"
         "  webc explain [--project]     what each build error means in WebC\n");
  printf("  webc seed [--reset]         load content/<table>.json through the app's boot\n");
  printf("  webc migrate <up|status|new NAME|dry|diff [--write]>\n");
  printf("  webc scaffold <Model>       list, form, save and delete for one model\n"
         "  webc scaffold live <Model>  + the SSE room, the broadcasting mutations\n"
         "                              and the test that proves the room holds it\n");
  printf("  webc routes [--json]      list the declared routes\n");
  printf("  webc api [--write]          the public surface, counted\n"
         "  webc api <query>            where a name is declared, and the line above it\n");
  printf("  webc lint [--all] [-v]      the no-leak gate over the app, examples and docs\n"
         "  webc lint --rules           the retired spellings, and what to write instead\n");
  printf("  webc docs [--emit DIR] [--list] [topic]\n"
         "                            the framework documentation, as files an agent reads\n");
  printf("  webc stdlib [module] [--css] [--used]\n"
         "                            the opt-in modules, and what one adds to the app\n");
  printf("  webc add <module>           install one: its sheet into public/, and the two lines\n");
  printf("  webc size                   what the page costs, raw and gzipped\n");
  printf("  webc size --modules         what each module costs: a probe build per module\n");
  printf("  webc doctor               check the toolchain\n");
  printf("  webc get [-X M] [-d B] URL  fetch a URL with the framework's own client\n");
  printf("  webc deploy [--target T] [--out DIR] [--tar] [--pack LIST]\n"
         "                            release binary + manifest + run script\n"
         "  webc deploy --pack docker,fly,systemd,caddy,nginx\n"
         "                            + the ops files that run it (a Dockerfile, a unit,\n"
         "                            the proxy lines — stamped from the manifest)\n");
  printf("  webc ci [--gitlab]         the workflow that runs the gate: webc check + test,\n"
         "                            a dist artifact on a tag. A file the app owns\n");
  printf("  webc openapi [--out F] [--check]\n"
         "                            the API boundary as OpenAPI 3.1, from the routes\n"
         "                            and models the sources already declare; --check\n"
         "                            fails when the file drifts from them\n");
  printf("  webc client --ts [--out F] [--check]\n"
         "                            a typed fetch client for the same facts — for the\n"
         "                            frontend that is not moving today. A file the app owns\n");
  printf("  webc model from-db [--database URL] [--table NAME] [--write]\n"
         "                            the reverse of migrate diff: a database that\n"
         "                            exists describes its models. --write adds the\n"
         "                            baseline and links them into the app\n");
  printf("  webc learn [1..5]           five lessons, one idea each: real apps whose\n"
         "                            own gates fail until the one edit lands\n");
  printf("  webc fmt [--check]        format the sources\n");
  printf("  webc lsp [--root DIR]     the language server, on stdio\n"
         "  webc lsp --selftest       a scripted session, asserted\n");
  printf("  webc mcp [--root DIR]     the MCP server, on stdio (for an agent)\n"
         "  webc mcp --selftest       a scripted session, asserted\n");
  printf("  webc version\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 0;
  }
  const char *cmd = argv[1];
  g_self = argv[0];
  webc_prefix_init(argv[0]); // a release tree moves; WEBC_HOME and argv[0] both name it
  tty_init();
  if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
    printf("webc %s\n", CLI_VERSION);
    return 0;
  }
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0) {
    usage();
    return 0;
  }
  if (strcmp(cmd, "new") == 0) {
    Template tpl = TPL_SERVER;
    const char *dir = nullptr;
    for (i32 i = 2; i < argc; i++) {
      // both spellings, because the help text promised `--template=web` and the first
      // version only accepted `--template web` — and silently scaffolded `server`
      const char *t = nullptr;
      if (str_eq_c(str_lit(argv[i]), "--template") && i + 1 < argc)
        t = argv[++i];
      else if (str_has_prefix_c(str_lit(argv[i]), "--template="))
        t = argv[i] + strlen("--template=");
      if (t) {
        if (strcmp(t, "web") == 0)
          tpl = TPL_WEB;
        else if (strcmp(t, "fullstack") == 0)
          tpl = TPL_FULLSTACK;
        else if (strcmp(t, "server") == 0)
          tpl = TPL_SERVER;
        else if (strcmp(t, "board") == 0)
          tpl = TPL_BOARD;
        else
          die("unknown template '%s' (server, web, fullstack, board)", t);
      } else if (argv[i][0] == '-') {
        die("unknown option '%s' for webc new", argv[i]);
      } else if (!dir) {
        dir = argv[i];
      } else {
        die("webc new takes one directory, not '%s' and '%s'", dir, argv[i]);
      }
    }
    return cmd_new_tpl(dir ? dir : ".", tpl);
  }
  if (strcmp(cmd, "build") == 0) {
    Config c;
    config_load(&c, ".");
    bool release = false;
    const char *target = nullptr;
    for (i32 i = 2; i < argc; i++) {
      if (strcmp(argv[i], "--release") == 0)
        release = true;
      else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc)
        target = argv[++i];
      else if (strcmp(argv[i], "--wasm") == 0)
        target = "wasm";
      else if (strcmp(argv[i], "--explain") == 0)
        g_explain = true;
    }
    char out[1024];
    snprintf(out, sizeof out, "%s/.webc/%s", c.root, c.name);
    return build(&c, target, release, out) ? 0 : 1;
  }
  if (strcmp(cmd, "dev") == 0) return cmd_dev();
  if (strcmp(cmd, "serve") == 0) return cmd_serve(argc - 2, argv + 2);
  if (strcmp(cmd, "test") == 0) {
    TestArgs ta = {0};
    for (i32 i = 2; i < argc; i++) {
      if (strcmp(argv[i], "--list") == 0)
        ta.list = true;
      else if (strcmp(argv[i], "--macros") == 0)
        ta.macros = true;
      else if (strcmp(argv[i], "--client") == 0)
        ta.client = true;
      else if (strcmp(argv[i], "--json") == 0)
        ta.json = true;
      else if (strcmp(argv[i], "--watch") == 0)
        ta.watch = true;
      else if (strncmp(argv[i], "--filter=", 9) == 0)
        ta.filter = argv[i] + 9;
      else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc)
        ta.filter = argv[++i];
      else if (strncmp(argv[i], "--test=", 7) == 0)
        ta.only = argv[i] + 7;
      else if (strcmp(argv[i], "--test") == 0 && i + 1 < argc)
        ta.only = argv[++i];
      else
        die("unknown test flag '%s'", argv[i]);
    }
    if (ta.client) return cmd_client_test();
    if (ta.watch) {
      // A suite worth having is a suite that runs while you type. Every pass clears the
      // screen, so what is on it is the last run, not the history of them.
      for (;;) {
        printf("\033[2J\033[H");
        fflush(stdout);
        cmd_test(ta);
        printf("\n\033[2m— watching src/, tests/, shared/, content/ and webc.json; "
               "ctrl-c to stop\033[0m\n");
        fflush(stdout);
        u64 stamp = watch_stamp();
        while (watch_stamp() == stamp) usleep(200000);
      }
    }
    return cmd_test(ta);
  }
  if (strcmp(cmd, "migrate") == 0) return cmd_migrate(argc - 2, argv + 2);
  if (strcmp(cmd, "scaffold") == 0) return cmd_scaffold(argc - 2, argv + 2);
  if (strcmp(cmd, "doctor") == 0) {
    bool schema = false, json = false;
    for (i32 i = 2; i < argc; i++) {
      if (strcmp(argv[i], "--schema") == 0) schema = true;
      else if (strcmp(argv[i], "--json") == 0) json = true;
    }
    return cmd_doctor(schema, json);
  }
  if (strcmp(cmd, "get") == 0) return cmd_get(argc, argv);
  if (strcmp(cmd, "deploy") == 0) return cmd_deploy(argc, argv);
  if (strcmp(cmd, "ci") == 0) return cmd_ci(argc, argv);
  if (strcmp(cmd, "openapi") == 0) return cmd_openapi(argc, argv);
  if (strcmp(cmd, "client") == 0) return cmd_client(argc, argv);
  if (strcmp(cmd, "learn") == 0) return cmd_learn(argc, argv);
  if (strcmp(cmd, "model") == 0) return cmd_model(argc, argv);
  if (strcmp(cmd, "explain") == 0) {
    Config c;
    config_load(&c, ".");
    return cmd_explain(c.root, g_self, argc - 2, argv + 2);
  }
  if (strcmp(cmd, "routes") == 0)
    return cmd_routes(argc > 2 && strcmp(argv[2], "--json") == 0);
  if (strcmp(cmd, "lsp") == 0) return cmd_lsp(argc - 2, argv + 2);
  if (strcmp(cmd, "mcp") == 0) return cmd_mcp(argc - 2, argv + 2);
  if (strcmp(cmd, "lint") == 0) {
    bool all = false, verbose = false, json = false, rules = false;
    const char *skill = nullptr;
    for (i32 i = 2; i < argc; i++) {
      if (strcmp(argv[i], "--all") == 0) all = true;
      else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) verbose = true;
      else if (strcmp(argv[i], "--json") == 0) json = true;
      else if (strcmp(argv[i], "--rules") == 0) rules = true;
      else if (strcmp(argv[i], "--skill") == 0 && i + 1 < argc) skill = argv[++i];
      else if (strncmp(argv[i], "--skill=", 8) == 0) skill = argv[i] + 8;
      else if (strcmp(argv[i], "--stdlib") == 0) { /* always on for an app */ }
    }
    if (rules) return cmd_lint_rules(json);
    if (skill) return cmd_lint_skill(skill, json);
    return cmd_lint(all, verbose, json);
  }
  if (strcmp(cmd, "api") == 0) {
    bool write = false, verbose = false, json = false;
    const char *query = nullptr;
    i32 limit = 12;
    for (i32 i = 2; i < argc; i++) {
      if (strcmp(argv[i], "--write") == 0) write = true;
      else if (strcmp(argv[i], "--json") == 0) json = true;
      else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) verbose = true;
      else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = atoi(argv[++i]);
      else if (argv[i][0] != '-') query = argv[i];
    }
    return cmd_api(write, verbose, json, query, limit);
  }
  if (strcmp(cmd, "stdlib") == 0) return cmd_stdlib(argc - 2, argv + 2);
  if (strcmp(cmd, "add") == 0) return cmd_add(argc - 2, argv + 2);
  if (strcmp(cmd, "size") == 0) return cmd_size(argc, argv);
  if (strcmp(cmd, "docs") == 0) return cmd_docs(argc - 2, argv + 2);
  if (strcmp(cmd, "check") == 0) {
    bool verbose = false, wiring = false, json = false;
    for (int i = 2; i < argc; i++) {
      if (strcmp(argv[i], "--verbose") == 0) verbose = true;
      else if (strcmp(argv[i], "--wiring") == 0) wiring = true;
      else if (strcmp(argv[i], "--json") == 0) json = true;
      else {
        fprintf(stderr, "webc check: unknown flag '%s'\n", argv[i]);
        return 1;
      }
    }
    return cmd_check(verbose, wiring, json);
  }
  if (strcmp(cmd, "seed") == 0) return cmd_seed(argc - 2, argv + 2);
  if (strcmp(cmd, "fmt") == 0) return cmd_fmt(argc > 2 && strcmp(argv[2], "--check") == 0);
  fprintf(stderr, "webc: unknown command '%s'\n\n", cmd);
  usage();
  return 1;
}