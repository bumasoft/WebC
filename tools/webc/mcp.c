// SPDX-License-Identifier: Apache-2.0
// WebC — the Model Context Protocol server (SPEC §14f).
//
//   webc mcp [--root DIR]          MCP over stdio, for an agent
//   webc mcp --selftest [--root]   a scripted session, asserted
//
// Why a subcommand and not a package: the same argument that gave us `webc lsp`.
// A server that reports on a compiler must *be* that compiler's version — a second
// distribution is a second thing to keep in step, and the framework's whole tooling
// story is one static binary with no Node, no download and no build step.
//
// What it is for. An agent with a shell already owns every command (see the table in
// docs/reference.md §14f): what it does not own is a *shape* to read and a *lifecycle*
// to hold. So the tools here are the four things a shell does badly —
//
//   - `webc_check` / `webc_test`: the gates, as data (`--json`), not as prose to parse;
//   - `webc_facts`: the project's own index (models, views, actions, routes, halves)
//     instead of a grep that half-answers;
//   - `webc_dev` + `webc_fetch`: a dev server that stays up across calls, and the page
//     it is serving, so the agent can read what it built;
//   - `webc_scaffold`: the two commands that write files, named explicitly.
//
// Every tool is a thin door onto something the CLI already does. Nothing here parses
// human output: if a tool needs data, the command that owns it learned `--json` first.
// That rule is what keeps this file from becoming a second, disagreeing front end.
#include <webc/client.h>
#include <webc/core.h>
#include <webc/json.h>

#include "api_search.h"
#include "prefix.h"
#include "cli.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "facts.h"
#include "tty.h"

#define MCP_PROTOCOL "2025-06-18"

static const char *g_self;  // this binary, for the child processes
static char g_self_bin[1024]; // resolved beside this tree, when argv[0] was a name
static char g_root[1024];   // the project, absolute
static pid_t g_dev_pid;
static i32 g_dev_port;

// ---------------------------------------------------------------- small helpers


// One `webc <args> --json` in the project, captured. `args` is a shell fragment built
// only from tool arguments this file validates (a filter, a template name).
static char *run_json(Arena *a, const char *args) {
  char cmd[4096];
  snprintf(cmd, sizeof cmd, "cd '%s' && '%s' %s --json 2>/dev/null", g_root, g_self, args);
  FILE *f = popen(cmd, "r");
  if (!f) return nullptr;
  Buf out;
  buf_init(&out, a, 1 << 15);
  char line[4096];
  while (fgets(line, sizeof line, f)) buf_puts(&out, line);
  pclose(f);
  if (out.n == 0) buf_puts(&out, "{}");
  return buf_cstr(&out);
}

// ---------------------------------------------------------------- tools/list

typedef struct Tool {
  const char *name;
  const char *title;
  const char *description;
  const char *schema;   // the inputSchema, as JSON
  bool writes;
} Tool;

static const Tool TOOLS[] = {
    {"webc_check", "Check the project",
     "Build, test, doctor, boot, schema, routes, lint and wiring consistency, in one call. "
     "Returns {ok, steps:[{name, ok, skipped, output}]}. This is the gate to run after every "
     "change; a failed build comes back with the WebC sentence, not the compiler's macro "
     "expansion.",
     "{\"type\":\"object\",\"properties\":{\"verbose\":{\"type\":\"boolean\",\"description\":\"list "
     "every note, not the summary\"}}}",
     false},
    {"webc_facts", "Read the project index",
     "The project as facts, read from the sources: models with their fields and qualifiers, "
     "views with their context type, actions, routes with the view each returns, every file's "
     "half (server/shared/client), the stdlib modules included, and the wiring gaps the "
     "consistency pass would report. Use it instead of grepping: a fact that cannot be "
     "navigated to is a rumour, and every fact here carries its file and line.",
     "{\"type\":\"object\",\"properties\":{}}", false},
    {"webc_api", "Search the framework surface",
     "Search the public surface (1686 names across 56 headers) for a name or a fragment. "
     "Returns the declaration line from the header it lives in, its header, and the doc "
     "sentence above it. Use it to confirm an API exists before writing it: this framework "
     "retires spellings on purpose and `webc lint` will refuse a retired one.",
     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"a name or "
     "fragment, e.g. `db_all` or `pager`\"},\"limit\":{\"type\":\"integer\",\"description\":\"max "
     "matches, default 12\"}},\"required\":[\"query\"]}",
     false},
    {"webc_test", "Run the test suites",
     "Build and run tests/*_test.c, returning {ok, tests:[{suite, passed, failed, skipped, "
     "output}]}. Tests run in-process against a scratch database, so this is cheap enough to "
     "run on every edit.",
     "{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\",\"description\":\"only "
     "suites whose filename or test name matches\"}}}",
     false},
    {"webc_dev", "Run the dev server",
     "Start, stop or inspect a build-and-serve process for this project. `start` builds and "
     "serves on a free port and returns {url, port, ready}; `status` reports the same; `stop` "
     "ends it. Then use webc_fetch to read the page it is serving.",
     "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"stop\","
     "\"status\"],\"description\":\"what to do, default status\"}},\"required\":[\"action\"]}",
     true},
    {"webc_fetch", "Fetch a page from the dev server",
     "GET (or POST) a path from the running dev server and return {status, headers, body}. "
     "This is how an agent reads the page it just built: the HTML the server rendered, "
     "without a browser. Start the server with webc_dev first.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"e.g. `/` or "
     "`/api/notes`\"},\"method\":{\"type\":\"string\",\"description\":\"GET by default\"},"
     "\"body\":{\"type\":\"string\",\"description\":\"POST/PUT body, sent as application/json\"}},"
     "\"required\":[\"path\"]}",
     false},
    {"webc_scaffold", "Create a project or add a module",
     "Create a project from a template (`server`, `web`, `fullstack`, `board`) or install a "
     "standard-library module into this project. The only tool that writes files outside the "
     "app's own sources.",
     "{\"type\":\"object\",\"properties\":{\"template\":{\"type\":\"string\",\"enum\":[\"server\","
     "\"web\",\"fullstack\",\"board\"],\"description\":\"scaffold a new project\"},\"dir\":{\"type\":"
     "\"string\",\"description\":\"where, for a new project\"},\"add_module\":{\"type\":\"string\","
     "\"description\":\"a stdlib module to install into this project, e.g. `table`\"}}}",
     true},
};

// ---------------------------------------------------------------- tool: facts

static void facts_json(Buf *j) {
  Arena *a = arena_new(512 * 1024);
  WFacts *F = facts_new(a, g_root);
  const char *rootp = g_root;
  buf_puts(j, "{\"root\":");
  jputs(j, rootp);
  buf_puts(j, ",\"is_app\":");
  buf_puts(j, F->is_app ? "true" : "false");
  buf_puts(j, ",\"files\":[");
  i32 k = 0;
  for (i32 i = 0; F && i < F->nfile; i++) {
    WFile *f = &F->file[i];
    if (!f->indexed || f->proj < 0) continue;
    if (k++) buf_puts(j, ",");
    const char *p = f->path;
    size_t rn = strlen(g_root);
    if (!strncmp(p, g_root, rn) && p[rn] == '/') p += rn + 1;
    buf_puts(j, "{\"path\":");
    jputs(j, p);
    buf_puts(j, ",\"half\":");
    WHalf h = facts_half_of(f->path, i, F);
    jputs(j, h == WH_SERVER ? "server" : h == WH_CLIENT ? "client" : "shared");
    if (f->nmods) {
      buf_puts(j, ",\"modules\":[");
      for (i32 m = 0; m < f->nmods; m++) {
        if (m) buf_puts(j, ",");
        jputs(j, f->mods[m]);
      }
      buf_puts(j, "]");
    }
    buf_puts(j, "}");
  }
  buf_puts(j, "],\"models\":[");
  k = 0;
  for (i32 i = 0; F && i < F->nmod; i++) {
    WMod *m = &F->mod[i];
    if (m->file < 0 || facts_proj_of(F, m->file) < 0) continue;
    if (k++) buf_puts(j, ",");
    buf_puts(j, "{\"name\":");
    jputs(j, m->name);
    buf_puts(j, ",\"internal\":");
    buf_puts(j, m->internal ? "true" : "false");
    buf_printf(j, ",\"line\":%d", m->line + 1);
    buf_puts(j, ",\"fields\":[");
    for (i32 f = 0; f < m->nf; f++) {
      if (f) buf_puts(j, ",");
      buf_puts(j, "{\"name\":");
      jputs(j, m->f[f].name);
      buf_puts(j, ",\"kind\":");
      jputs(j, m->f[f].kind);
      if (m->f[f].quals && *m->f[f].quals) {
        buf_puts(j, ",\"quals\":");
        jputs(j, m->f[f].quals);
      }
      buf_printf(j, ",\"pk\":%s,\"required\":%s}", m->f[f].pk ? "true" : "false",
                 m->f[f].required ? "true" : "false");
    }
    buf_puts(j, "]}");
  }
  buf_puts(j, "],\"views\":[");
  k = 0;
  for (i32 i = 0; F && i < F->nview; i++) {
    WSym *v = &F->view[i];
    if (v->file < 0 || facts_proj_of(F, v->file) < 0) continue;
    if (k++) buf_puts(j, ",");
    buf_puts(j, "{\"name\":");
    jputs(j, v->name);
    buf_printf(j, ",\"line\":%d,\"context\":", v->line + 1);
    jputs(j, v->detail ? v->detail : "");
    buf_puts(j, "}");
  }
  buf_puts(j, "],\"declared_views\":[");
  k = 0;
  for (i32 i = 0; F && i < F->ndview; i++) {
    WSym *v = &F->dview[i];
    if (v->file < 0 || facts_proj_of(F, v->file) < 0) continue;
    if (k++) buf_puts(j, ",");
    jputs(j, v->name);
  }
  buf_puts(j, "],\"actions\":[");
  k = 0;
  for (i32 i = 0; F && i < F->nact; i++) {
    WSym *v = &F->act[i];
    if (v->file < 0 || facts_proj_of(F, v->file) < 0) continue;
    if (k++) buf_puts(j, ",");
    buf_puts(j, "{\"name\":");
    jputs(j, v->name);
    buf_printf(j, ",\"line\":%d}", v->line + 1);
  }
  buf_puts(j, "],\"routes\":[");
  k = 0;
  for (i32 i = 0; F && i < F->nroute; i++) {
    WRoute *r = &F->route[i];
    if (facts_proj_of(F, r->file) < 0) continue;
    if (k++) buf_puts(j, ",");
    buf_puts(j, "{\"method\":");
    jputs(j, r->method);
    buf_puts(j, ",\"path\":");
    jputs(j, r->path);
    buf_printf(j, ",\"line\":%d", r->line + 1);
    if (r->view) {
      buf_puts(j, ",\"view\":");
      jputs(j, r->view);
    }
    buf_puts(j, "}");
  }
  buf_puts(j, "]}");
  arena_destroy(a);
}

// ---------------------------------------------------------------- tool: api

static bool read_whole(const char *path, char **out, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *p = (char *)malloc((size_t)len + 1);
  if (!p) {
    fclose(f);
    return false;
  }
  size_t got = fread(p, 1, (size_t)len, f);
  fclose(f);
  p[got] = 0;
  *out = p;
  *n = got;
  return true;
}

// Search the headers the framework ships. A name is answered where it is *declared*,
// with the doc sentence above it and the header it lives in — which is what a reader
// with no completion would do, done once.
// ---------------------------------------------------------------- tool: dev

static bool port_open(i32 port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((u16)port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ok = connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0;
  close(fd);
  return ok;
}

static i32 free_port(i32 from) {
  for (i32 p = from; p < from + 40; p++)
    if (!port_open(p)) return p;
  return from;
}

static void dev_stop(void) {
  if (g_dev_pid > 0) {
    kill(g_dev_pid, SIGTERM);
    int st = 0;
    waitpid(g_dev_pid, &st, 0);
    g_dev_pid = 0;
  }
}

static void dev_json(Buf *j, const char *action) {
  char url[128] = "";
  if (!strcmp(action, "start")) {
    dev_stop();
    // build first: a serve that fails to compile is a confusing way to learn that
    WCap cap;
    wcap_begin(&cap);
    char bcmd[4096];
    snprintf(bcmd, sizeof bcmd, "cd '%s' && '%s' build 2>&1", g_root, g_self);
    int built = system(bcmd);
    char *bout = wcap_end(&cap);
    if (built != 0) {
      // a serve over a broken build serves the last good binary: say so instead
      buf_puts(j, "{\"running\":false,\"error\":");
      jputs(j, bout ? bout : "build failed");
      buf_puts(j, "}");
      free(bout);
      return;
    }
    free(bout);
    i32 port = free_port(8180);
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "cd '%s' && exec '%s' serve --no-build --port %d --quiet", g_root,
             g_self, port);
    pid_t pid = fork();
    if (pid == 0) {
      // stdout is the protocol channel: a child that prints its banner there corrupts
      // every message after it. The child's words go to a log in .webc/, where a
      // failure can still be read.
      int log = open(".webc/mcp-dev.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (log >= 0) {
        dup2(log, STDOUT_FILENO);
        dup2(log, STDERR_FILENO);
        close(log);
      } else {
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
          dup2(null, STDOUT_FILENO);
          dup2(null, STDERR_FILENO);
          close(null);
        }
      }
      execl("/bin/sh", "sh", "-c", cmd, (char *)nullptr);
      _exit(127);
    }
    g_dev_pid = pid;
    g_dev_port = port;
    for (i32 i = 0; i < 200; i++) {
      if (port_open(port)) break;
      usleep(50 * 1000);
    }
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
  } else if (!strcmp(action, "stop")) {
    dev_stop();
  } else if (g_dev_port && port_open(g_dev_port)) {
    snprintf(url, sizeof url, "http://127.0.0.1:%d", g_dev_port);
  }
  buf_puts(j, "{\"running\":");
  buf_puts(j, (g_dev_pid > 0 && url[0]) ? "true" : "false");
  buf_printf(j, ",\"pid\":%d,\"port\":%d,\"url\":", (int)g_dev_pid, g_dev_port);
  jputs(j, url);
  buf_puts(j, "}");
}

static void fetch_json(Buf *j, const char *path, const char *method, const char *body) {
  if (!(g_dev_pid > 0 && port_open(g_dev_port))) {
    buf_puts(j, "{\"error\":\"no dev server — call webc_dev with action=start first\"}");
    return;
  }
  Arena *a = arena_new(1 << 20);
  char url[1024];
  if (path && path[0] == 'h') snprintf(url, sizeof url, "%s", path);
  else snprintf(url, sizeof url, "http://127.0.0.1:%d%s", g_dev_port, (path && *path) ? path : "/");
  WRespC r = {0};
  WErrRes e = http_do_follow(a, (WClient){.method = method ? method : "GET",
                                          .url = url,
                                          .body = {(char *)body, body ? strlen(body) : 0},
                                          .content_type = body ? "application/json" : nullptr},
                             5, &r);
  if (!e.ok) {
    buf_puts(j, "{\"error\":");
    jputs(j, e.e.buf);
    buf_puts(j, "}");
    arena_destroy(a);
    return;
  }
  buf_printf(j, "{\"status\":%d,\"content_type\":", r.status);
  jputs(j, r.content_type.p ? r.content_type.p : "");
  buf_puts(j, ",\"body\":");
  char *plain = wc_strip_ansi(r.body.p ? r.body.p : "");
  jputs(j, plain ? plain : "");
  free(plain);
  buf_puts(j, "}");
  arena_destroy(a);
}

// ---------------------------------------------------------------- tool calls

static const Tool *tool_by_name(const char *n) {
  for (size_t i = 0; i < sizeof TOOLS / sizeof *TOOLS; i++)
    if (!strcmp(TOOLS[i].name, n)) return &TOOLS[i];
  return nullptr;
}

// A JSON string argument out of the tool's `arguments` object. MCP arguments are small
// and flat here; a full JSON reader for them would be more machinery than the protocol
// needs, but the escape handling is the json module's.
static char *arg_str(Arena *a, const char *args_json, const char *key) {
  if (!args_json) return nullptr;
  JParse p = json_parse(a, str_lit(args_json));
  if (!p.ok || !p.v || p.v->kind != J_OBJ) return nullptr;
  JVal *v = json_get(p.v, key);
  if (!v || v->kind != J_STR) return nullptr;
  char *out = (char *)arena_alloc(a, v->s.n + 1);
  if (!out) return nullptr;
  memcpy(out, v->s.p, v->s.n);
  out[v->s.n] = 0;
  return out;
}

static char *call_tool(Arena *a, const char *name, const char *args) {
  Buf j;
  buf_init(&j, a, 1 << 16);
  if (!strcmp(name, "webc_check")) {
    bool verbose = arg_str(a, args, "verbose") != nullptr;
    char *out = run_json(a, verbose ? "check --verbose" : "check");
    buf_puts(&j, out ? out : "{}");
  } else if (!strcmp(name, "webc_facts")) {
    facts_json(&j);
  } else if (!strcmp(name, "webc_api")) {
    char *q = arg_str(a, args, "query");
    if (!q || !*q) return (char *)"{\"error\":\"query is required\"}";
    i32 limit = 12;
    JParse p = json_parse(a, str_lit(args ? args : "{}"));
    if (p.ok && p.v) {
      JVal *l = json_get(p.v, "limit");
      if (l && l->kind == J_NUM) limit = (i32)l->num;
    }
    api_search_json(&j, q, limit > 0 && limit <= 100 ? limit : 12);
  } else if (!strcmp(name, "webc_test")) {
    char *filter = arg_str(a, args, "filter");
    char frag[512];
    if (filter && *filter) {
      // a filter is a shell argument: keep it to the alphabet a test name uses
      for (size_t i = 0; filter[i]; i++)
        if (!isalnum((unsigned char)filter[i]) && filter[i] != '_' && filter[i] != '-')
          filter[i] = '_';
      snprintf(frag, sizeof frag, "test --filter=%s", filter);
    } else {
      snprintf(frag, sizeof frag, "test");
    }
    char *out = run_json(a, frag);
    buf_puts(&j, out ? out : "{}");
  } else if (!strcmp(name, "webc_dev")) {
    char *action = arg_str(a, args, "action");
    dev_json(&j, action ? action : "status");
  } else if (!strcmp(name, "webc_fetch")) {
    char *path = arg_str(a, args, "path");
    char *method = arg_str(a, args, "method");
    char *body = arg_str(a, args, "body");
    fetch_json(&j, path ? path : "/", method, body);
  } else if (!strcmp(name, "webc_scaffold")) {
    char *tpl = arg_str(a, args, "template");
    char *dir = arg_str(a, args, "dir");
    char *mod = arg_str(a, args, "add_module");
    if (mod) {
      char cmd[1024];
      for (size_t i = 0; mod[i]; i++)
        if (!isalnum((unsigned char)mod[i]) && mod[i] != '_') mod[i] = 0;
      snprintf(cmd, sizeof cmd, "add %s", mod);
      char *out = run_json(a, cmd);
      buf_puts(&j, out ? out : "{}");
    } else if (tpl && dir) {
      for (size_t i = 0; dir[i]; i++)
        if (dir[i] == '\'' || dir[i] == '"' || dir[i] == ';') dir[i] = '_';
      char cmd[1200];
      snprintf(cmd, sizeof cmd, "new '%s' --template=%s", dir, tpl);
      char sh[1400];
      snprintf(sh, sizeof sh, "cd '%s' && '%s' %s 2>&1", g_root, g_self, cmd);
      FILE *f = popen(sh, "r");
      buf_puts(&j, "{\"created\":true,\"output\":\"");
      if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
          for (char *p = line; *p; p++)
            if (*p == '"' || *p == '\n' || *p == '\\') *p = ' ';
          if (strlen(buf_cstr(&j)) < 3000) buf_puts(&j, line);
        }
        pclose(f);
      }
      buf_puts(&j, "\"}");
    } else {
      buf_puts(&j, "{\"error\":\"give template+dir (a new project) or add_module\"}");
    }
  } else {
    buf_puts(&j, "{\"error\":\"no such tool\"}");
  }
  return buf_cstr(&j);
}

// ---------------------------------------------------------------- the protocol

static void send_msg(const char *json) {
  fputs(json, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

static void reply(const char *id, const char *result_json) {
  Buf b;
  buf_init(&b, nullptr, 1 << 16);
  buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  buf_puts(&b, id ? id : "null");
  buf_puts(&b, ",\"result\":");
  buf_puts(&b, result_json);
  buf_puts(&b, "}");
  send_msg(buf_cstr(&b));
  buf_free(&b);
}

static void reply_error(const char *id, i32 code, const char *msg) {
  Buf b;
  buf_init(&b, nullptr, 2048);
  buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  buf_puts(&b, id ? id : "null");
  buf_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
  jputs(&b, msg);
  buf_puts(&b, "}}");
  send_msg(buf_cstr(&b));
  buf_free(&b);
}

static void handle(Arena *a, const char *line) {
  JParse p = json_parse(a, str_lit(line));
  if (!p.ok || !p.v) {
    reply_error(nullptr, -32700, "parse error");
    return;
  }
  JVal *idv = json_get(p.v, "id");
  char id[64] = "";
  if (idv) {
    if (idv->kind == J_NUM) snprintf(id, sizeof id, "%lld", (long long)idv->num);
    else if (idv->kind == J_STR) snprintf(id, sizeof id, "%.*s", (int)idv->s.n, idv->s.p);
  }
  JVal *m = json_get(p.v, "method");
  if (!m || m->kind != J_STR) {
    reply_error(id, -32600, "no method");
    return;
  }
  char method[128];
  snprintf(method, sizeof method, "%.*s", (int)m->s.n, m->s.p);
  JVal *params = json_get(p.v, "params");

  if (!strcmp(method, "initialize")) {
    Buf r;
    buf_init(&r, a, 4096);
    buf_printf(&r,
               "{\"protocolVersion\":\"" MCP_PROTOCOL "\",\"capabilities\":{\"tools\":{},"
               "\"resources\":{}},\"serverInfo\":{\"name\":\"webc\",\"version\":\"%s\"},"
               "\"instructions\":\"The WebC toolchain. Run webc_check after every change; "
               "webc_facts reads the project (models, views, actions, routes, halves) instead "
               "of grepping; webc_dev + webc_fetch let you read the page you built. Every "
               "error comes back in the framework's words, and `webc lint` refuses the "
               "spellings it retired.\"}",
               WEBC_VERSION);
    reply(id, buf_cstr(&r));
    return;
  }
  if (!strcmp(method, "notifications/initialized") || !strcmp(method, "initialized")) return;
  if (!strcmp(method, "ping")) {
    reply(id, "{}");
    return;
  }
  if (!strcmp(method, "tools/list")) {
    Buf r;
    buf_init(&r, a, 1 << 15);
    buf_puts(&r, "{\"tools\":[");
    for (size_t i = 0; i < sizeof TOOLS / sizeof *TOOLS; i++) {
      if (i) buf_puts(&r, ",");
      buf_puts(&r, "{\"name\":");
      jputs(&r, TOOLS[i].name);
      buf_puts(&r, ",\"title\":");
      jputs(&r, TOOLS[i].title);
      buf_puts(&r, ",\"description\":");
      jputs(&r, TOOLS[i].description);
      buf_puts(&r, ",\"inputSchema\":");
      buf_puts(&r, TOOLS[i].schema);
      if (TOOLS[i].writes) {
        buf_puts(&r, ",\"annotations\":{\"readOnlyHint\":false}");
      } else {
        buf_puts(&r, ",\"annotations\":{\"readOnlyHint\":true}");
      }
      buf_puts(&r, "}");
    }
    buf_puts(&r, "]}");
    reply(id, buf_cstr(&r));
    return;
  }
  if (!strcmp(method, "tools/call")) {
    char name[128] = "";
    char *args_json = nullptr;
    if (params && params->kind == J_OBJ) {
      JVal *nv = json_get(params, "name");
      if (nv && nv->kind == J_STR) snprintf(name, sizeof name, "%.*s", (int)nv->s.n, nv->s.p);
      JVal *av = json_get(params, "arguments");
      if (av) {
        // re-serialise the arguments so the tool sees one object
        Buf ab;
        buf_init(&ab, a, 4096);
        buf_putc(&ab, '{');
        if (av->kind == J_OBJ) {
          for (i32 i = 0; i < av->n; i++) {
            if (i) buf_putc(&ab, ',');
            char kbuf[256];
            snprintf(kbuf, sizeof kbuf, "%.*s", (int)av->keys[i].n, av->keys[i].p);
            jputs(&ab, kbuf);
            buf_putc(&ab, ':');
            JVal *x = av->items[i];
            if (x->kind == J_STR) {
              char tmp[1024];
              snprintf(tmp, sizeof tmp, "%.*s", (int)x->s.n, x->s.p ? x->s.p : "");
              jputs(&ab, tmp);
            }
            else if (x->kind == J_NUM) buf_printf(&ab, "%g", x->num);
            else if (x->kind == J_BOOL) buf_puts(&ab, x->b ? "true" : "false");
            else buf_puts(&ab, "null");
          }
        }
        buf_putc(&ab, '}');
        args_json = buf_cstr(&ab);
      }
    }
    if (!tool_by_name(name)) {
      reply_error(id, -32602, "unknown tool");
      return;
    }
    char *data = call_tool(a, name, args_json);
    Buf r;
    buf_init(&r, a, 1 << 16);
    buf_puts(&r, "{\"content\":[{\"type\":\"text\",\"text\":");
    jputs(&r, data);
    buf_puts(&r, "}]}");
    reply(id, buf_cstr(&r));
    return;
  }
  if (!strcmp(method, "resources/list")) {
    Buf r;
    buf_init(&r, a, 4096);
    buf_puts(&r,
             "{\"resources\":[{\"uri\":\"webc://project\",\"name\":\"project\","
             "\"title\":\"The project index\",\"description\":\"Models, views, actions, routes "
             "and each file's half, read from the sources.\",\"mimeType\":\"application/json\"},"
             "{\"uri\":\"webc://reference\",\"name\":\"reference\","
             "\"title\":\"The reference's section index\",\"description\":\"Every section of "
             "docs/reference.md with its anchor, so a reader can ask for one instead of "
             "grepping 3392 lines.\",\"mimeType\":\"text/markdown\"}]}");
    reply(id, buf_cstr(&r));
    return;
  }
  if (!strcmp(method, "resources/read")) {
    char uri[256] = "";
    if (params && params->kind == J_OBJ) {
      JVal *u = json_get(params, "uri");
      if (u && u->kind == J_STR) snprintf(uri, sizeof uri, "%.*s", (int)u->s.n, u->s.p);
    }
    Buf content;
    buf_init(&content, a, 1 << 15);
    if (!strcmp(uri, "webc://project")) {
      facts_json(&content);
    } else if (!strcmp(uri, "webc://reference")) {
      char path[1200];
      snprintf(path, sizeof path, "%s/docs/reference.md", webc_prefix());
      char *text = nullptr;
      size_t n = 0;
      if (read_whole(path, &text, &n)) {
        // the headings, and nothing else: the index, not the book
        i32 lines = 0;
        for (char *l = text; l && *l && lines < 400;) {
          char *nl = strchr(l, '\n');
          if (nl) *nl = 0;
          if (l[0] == '#') {
            buf_puts(&content, l);
            buf_puts(&content, "\n");
            lines++;
          }
          l = nl ? nl + 1 : nullptr;
        }
        free(text);
      }
    } else {
      reply_error(id, -32602, "unknown resource");
      return;
    }
    Buf r;
    buf_init(&r, a, 1 << 16);
    buf_puts(&r, "{\"contents\":[{\"uri\":");
    jputs(&r, uri);
    buf_puts(&r, ",\"mimeType\":\"application/json\",\"text\":");
    jputs(&r, buf_cstr(&content));
    buf_puts(&r, "}]}");
    reply(id, buf_cstr(&r));
    return;
  }
  reply_error(id, -32601, "method not found");
}

// ---------------------------------------------------------------- selftest

static i32 g_fail;
static void expect(bool ok, const char *what) {
  if (!ok) {
    fprintf(stderr, "  FAIL %s\n", what);
    g_fail++;
  } else {
    fprintf(stderr, "  ok   %s\n", what);
  }
}

int cmd_mcp(i32 argc, char **argv) {
  const char *root = ".";
  bool selftest = false;
  for (i32 i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
    else if (!strncmp(argv[i], "--root=", 7)) root = argv[i] + 7;
    else if (!strcmp(argv[i], "--selftest")) selftest = true;
    else if (!strcmp(argv[i], "--stdio")) continue;
    else {
      fprintf(stderr,
              "webc mcp: unknown option '%s'\n"
              "  webc mcp [--root DIR]        the MCP server, on stdio\n"
              "  webc mcp --selftest          a scripted session, asserted\n",
              argv[i]);
      return 1;
    }
  }
  char *rp = realpath(root, nullptr);
  snprintf(g_root, sizeof g_root, "%s", rp ? rp : root);
  free(rp);
  // A client starts this process in whatever directory the session is in, and an agent
  // may well be sitting in `src/`. The project is the nearest directory at or above it
  // that has a `webc.json`: answering about a modern project that is not the one on
  // screen would be worse than answering nothing.
  {
    char here[1024];
    snprintf(here, sizeof here, "%s", g_root);
    for (i32 depth = 0; depth < 32; depth++) {
      char probe[1200];
      snprintf(probe, sizeof probe, "%s/webc.json", here);
      if (access(probe, R_OK) == 0) {
        snprintf(g_root, sizeof g_root, "%s", here);
        break;
      }
      char *slash = strrchr(here, '/');
      if (!slash || slash == here) break; // no project at or above: keep what we were given
      *slash = 0;
    }
  }
  // argv[0] may be a bare name (spawned without a path); the prefix knows the tree. Two
  // layouts exist and both are supported: a checkout keeps the CLI in build/webc, and an
  // install — `make install`, or the release tarball — puts it in bin/webc. Naming the
  // path that is not there is a `webc_dev` that never starts.
  snprintf(g_self_bin, sizeof g_self_bin, "%s/build/webc", webc_prefix());
  if (access(g_self_bin, X_OK) != 0)
    snprintf(g_self_bin, sizeof g_self_bin, "%s/bin/webc", webc_prefix());
  g_self = g_self_bin;

  if (selftest) {
    char here[1024];
    snprintf(here, sizeof here, "%s/examples/todo", webc_prefix());
    char *tr = realpath(here, nullptr);
    if (tr) {
      snprintf(g_root, sizeof g_root, "%s", tr);
      free(tr);
    }
    // The session a client would send, one message per line, asserted end to end.
    Arena *a = arena_new(1 << 20);
    Buf r;
    buf_init(&r, a, 1 << 16);
    facts_json(&r);
    expect(strstr(buf_cstr(&r), "\"models\":[") != nullptr, "facts name the models");
    expect(strstr(buf_cstr(&r), "Todo") != nullptr, "facts find the todo model");
    Buf b2;
    buf_init(&b2, a, 1 << 16);
    api_search_json(&b2, "db_all", 5);
    expect(strstr(buf_cstr(&b2), "\"count\":") != nullptr, "api search answers");
    expect(strstr(buf_cstr(&b2), "db.h") != nullptr, "api search finds db_all in db.h");
    char *check = call_tool(a, "webc_check", "{}");
    expect(strstr(check, "\"steps\"") != nullptr, "check returns steps");
    char *fetch = call_tool(a, "webc_fetch", "{\"path\":\"/\"}");
    expect(strstr(fetch, "no dev server") != nullptr, "fetch without a server says so");
    arena_destroy(a);
    fprintf(stderr, "webc mcp: %d failure%s\n", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
  }

  Arena *a = arena_new(1 << 20);
  char line[1 << 16];
  while (fgets(line, sizeof line, stdin)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n) continue;
    // a fresh arena per message: a long session must not grow without bound
    arena_reset(a);
    handle(a, line);
  }
  dev_stop();
  arena_destroy(a);
  return 0;
}