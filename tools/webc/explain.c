// SPDX-License-Identifier: Apache-2.0
// WebC — see explain.h.
#include "explain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dsl.h"
#include "facts.h"
#include "lspx.h"
#include "tty.h"

// ---------------------------------------------------------------- the engine
//
// One WLsp, built the way `webc lsp` builds it: the project index from the sources,
// one document per file. Nothing here is editor-shaped — no version numbers, no URIs
// over the wire — but it is the same code, so an agent and an editor cannot be told
// two different stories about the same file.

typedef struct Engine {
  Arena *a;
  WLsp L;
  bool ok;
} Engine;

static bool engine_open(Engine *E, const char *root) {
  memset(E, 0, sizeof *E);
  E->a = arena_new(512 * 1024);
  E->L.a = E->a;
  E->L.req = arena_new(256 * 1024);
  buf_init(&E->L.out, E->a, 4096);
  snprintf(E->L.root, sizeof E->L.root, "%s", root);
  E->L.F = facts_new(E->a, root);
  E->ok = E->L.F && E->L.F->nfile > 0;
  return E->ok;
}

static void engine_close(Engine *E) {
  if (E->a) arena_destroy(E->a);
  E->a = nullptr;
}

static char *slurp(Arena *a, const char *path, size_t *n_out) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    *n_out = 0;
    return nullptr;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *p = (char *)arena_alloc(a, (size_t)n + 1);
  size_t got = 0;
  if (p && n > 0) got = fread(p, 1, (size_t)n, f);
  fclose(f);
  if (!p) {
    *n_out = 0;
    return nullptr;
  }
  p[got] = 0;
  *n_out = got;
  return p;
}

// Run both engines over one file on disk. The diagnostics land in `d->diag`.
static WDiags *diags_of(Engine *E, const char *path) {
  size_t n = 0;
  char *text = slurp(E->a, path, &n);
  if (!text) return nullptr;
  WDoc *d = lsp_doc_open(&E->L, path, nullptr, 0, (str){text, n});
  if (!d) return nullptr;
  Buf scratch;
  buf_init(&scratch, E->a, 2048);
  lspx_diagnostics(&E->L, d, &scratch);
  return d->diag;
}

// The document a path resolved to, so offsets can be turned back into line/col.
static WDoc *doc_of(Engine *E, const char *path) {
  for (i32 i = 0; i < E->L.ndoc; i++)
    if (!strcmp(E->L.doc[i].path, path)) return &E->L.doc[i];
  return nullptr;
}

// The engine compares absolute paths (facts stores realpaths); a caller that says "."
// must not make every file look like an unknown one, which would silently switch the
// half rule off. One realpath, here, for both doors.
static const char *abs_root(char *buf, size_t cap, const char *root) {
  char *rp = realpath(root, nullptr);
  if (rp) {
    snprintf(buf, cap, "%s", rp);
    free(rp);
    return buf;
  }
  snprintf(buf, cap, "%s", root);
  return buf;
}

// ---------------------------------------------------------------- printing

static const char *sev_name(WSev s) {
  switch (s) {
    case WSEV_ERROR: return "error";
    case WSEV_WARN: return "warning";
    case WSEV_INFO: return "info";
    default: return "hint";
  }
}

// A sentence from the engine is one line in the diagnostic's source — 200 columns is how
// it arrives, and unreadable is how it lands. Wrap at a fixed width, at the sentence's own
// indent: the bytes down a pipe are the bytes on a screen, which is what makes a pasted
// transcript agree with what the person saw.
static void say(i32 indent, const char *text) {
  const i32 width = 92;
  i32 col = 0;
  for (i32 i = 0; i < indent; i++, col++) putchar(' ');
  for (const char *w = text; *w;) {
    const char *e = w;
    while (*e && *e != ' ' && *e != '\n') e++;
    i32 len = (i32)(e - w);
    if (col > indent && col + 1 + len > width) {
      putchar('\n');
      for (i32 i = 0; i < indent; i++, col++) putchar(' ');
      col = indent;
    } else if (col > indent) {
      putchar(' ');
      col++;
    }
    for (const char *p = w; p < e; p++, col++) putchar(*p);
    w = *e ? e + 1 : e;
  }
  putchar('\n');
}

static void print_diag(const char *rel, i32 line, i32 col, const char *msg,
                       const char *fix_title, const char *related) {
  printf("  %s%s:%d:%d%s %s\n", g_b, rel, line + 1, col + 1, g_x, msg);
  if (fix_title && *fix_title) printf("      %sfix%s %s\n", g_d, g_x, fix_title);
  if (related && *related) printf("      %scompiler%s %s\n", g_d, g_x, related);
}

static void json_str(Buf *b, const char *s) {
  if (!s) s = "";
  wj_str(b, s);
}

// ---------------------------------------------------------------- the clang side

typedef struct CErr {
  char path[1024];
  char rel[1024];
  i32 line, col; // 1-based, as the compiler prints them
  char msg[1024];
} CErr;

// `./src/view.c:8:22: error: use of undeclared identifier 'db_all'`
static bool parse_clang_line(const char *l, CErr *e) {
  const char *p = strstr(l, ": error:");
  if (!p) p = strstr(l, ": warning:");
  if (!p) return false;
  const char *sev = p;
  // walk back over :line:col
  const char *q = p;
  i32 nfield = 0;
  i32 v[2] = {0, 0};
  while (q > l && nfield < 2) {
    const char *colon = q;
    while (colon > l && colon[-1] != ':') colon--;
    if (colon == q) break;
    bool digits = true;
    for (const char *r = colon; r < q; r++)
      if (*r < '0' || *r > '9') digits = false;
    if (!digits) break;
    v[nfield++] = atoi(colon);
    q = colon - 1;
  }
  if (nfield != 2) return false;
  size_t plen = (size_t)(q - l);
  if (plen == 0 || plen >= sizeof e->path) return false;
  memcpy(e->path, l, plen);
  e->path[plen] = 0;
  e->line = v[1];
  e->col = v[0];
  const char *msg = sev + (sev[2] == 'e' ? 8 : 10);
  while (*msg == ' ') msg++;
  snprintf(e->msg, sizeof e->msg, "%s", msg);
  size_t mn = strlen(e->msg);
  while (mn && (e->msg[mn - 1] == '\n' || e->msg[mn - 1] == '\r')) e->msg[--mn] = 0;
  const char *base = strrchr(e->path, '/');
  snprintf(e->rel, sizeof e->rel, "%s", base ? base + 1 : e->path);
  return true;
}

// A name quoted in a compiler sentence: `use of undeclared identifier 'db_all'`.
static bool quoted(const char *msg, const char *at, char *out, size_t cap) {
  const char *p = strstr(msg, at);
  if (!p) return false;
  p += strlen(at);
  if (*p != '\'' && *p != '`') return false;
  char close = *p == '\'' ? '\'' : '`';
  p++;
  const char *end = strchr(p, close);
  if (!end || (size_t)(end - p) >= cap) return false;
  memcpy(out, p, (size_t)(end - p));
  out[end - p] = 0;
  return true;
}

// The macros that only exist in a server source. The list is the DSL's own (each is
// declared `server` in dsl.c), but the *symptom* is a plain clang error naming an
// identifier no header declared, so the answer here is a small table by prefix.
static bool looks_server_half(const char *name) {
  static const char *prefixes[] = {"db_", "req", "res_", "path", "query", "body",
                                   "mail_", "tls_", "wc_req"};
  for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++)
    if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0) return true;
  const WMacro *m = dsl_macro(name, strlen(name));
  return m && m->server;
}

// ---------------------------------------------------------------- public: annotate

static const char *rel_to(const char *root, const char *path, char *buf, size_t cap) {
  size_t rn = strlen(root);
  if (strncmp(path, root, rn) == 0 && path[rn] == '/') {
    snprintf(buf, cap, "%s", path + rn + 1);
    return buf;
  }
  snprintf(buf, cap, "%s", path);
  return buf;
}

bool explain_compiler_output(const char *root_in, const char *compiler_output) {
  if (!compiler_output || !*compiler_output) return false;
  char rootbuf[1024];
  const char *root = abs_root(rootbuf, sizeof rootbuf, root_in);
  Arena *a = arena_new(256 * 1024);
  CErr *errs = (CErr *)arena_alloc(a, sizeof(CErr) * 64);
  i32 n = 0;
  // the output as lines
  char *copy = (char *)arena_alloc(a, strlen(compiler_output) + 1);
  if (!copy) {
    arena_destroy(a);
    return false;
  }
  strcpy(copy, compiler_output);
  for (char *line = copy; line && *line && n < 64;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (parse_clang_line(line, &errs[n])) n++;
    line = nl ? nl + 1 : nullptr;
  }
  if (n == 0) {
    arena_destroy(a);
    return false;
  }
  Engine E;
  bool used_engine = engine_open(&E, root);

  i32 explained = 0;
  char spoken[64][1024];
  i32 nspoken = 0;
  for (i32 i = 0; i < n; i++) {
    CErr *e = &errs[i];
    char rbuf[1024];
    const char *rel = rel_to(root, e->path, rbuf, sizeof rbuf);
    // One mistake usually arrives as several sentences: the second error on a line
    // that was already explained is the first one's echo, not new information.
    char key[1100];
    snprintf(key, sizeof key, "%s:%d", rel, e->line);
    bool echo = false;
    for (i32 k = 0; k < nspoken; k++)
      if (!strcmp(spoken[k], key)) echo = true;
    if (!echo && nspoken < 64) snprintf(spoken[nspoken++], 1024, "%s", key);
    else if (echo) continue;
    printf("  %s%s%s:%d:%d\n", g_b, rel, g_x, e->line, e->col);
    char name[128];
    const char *sentence = nullptr;
    // 1. what the WebC engine says about that line, when it says anything
    if (used_engine) {
      // the compiler's path may be relative to the project root
      char candidate[1024], full[1024];
      if (e->path[0] == '/') snprintf(candidate, sizeof candidate, "%s", e->path);
      else
        snprintf(candidate, sizeof candidate, "%s/%s", root,
                 e->path[0] == '.' && e->path[1] == '/' ? e->path + 2 : e->path);
      char *real = realpath(candidate, nullptr);
      if (real) {
        snprintf(full, sizeof full, "%s", real);
        free(real);
      } else {
        snprintf(full, sizeof full, "%s", candidate);
      }
      WDiags *dg = diags_of(&E, full);
      WDoc *d = doc_of(&E, full);
      if (dg && d) {
        for (i32 k = 0; k < dg->n; k++) {
          WDiag *g = &dg->d[k];
          i32 gl = 0, gc = 0;
          dsl_line_col(d->src, g->begin, &gl, &gc);
          if (gl + 1 != e->line) continue;
          if (g->sev > WSEV_WARN) continue;
          sentence = g->msg;
          break;
        }
      }
    }
    if (sentence) {
      say(4, sentence);
      explained++;
    } else if (quoted(e->msg, "use of undeclared identifier ", name, sizeof name)) {
      char view[128];
      if (strncmp(name, "view_", 5) == 0) {
        snprintf(view, sizeof view, "%s", name + 5);
        printf("    no view named `%s` in this project — `view(%s, …) { … }` draws it, or\n"
               "    `declare_view(%s)` in the shared header announces it\n",
               view, view, view);
      } else if (looks_server_half(name)) {
        printf("    `%s` is a server-half name: this file compiles for the browser too, so the\n"
               "    server half's headers do not declare it. Move the call into a `server`\n"
               "    source and pass the value in through the view's context\n",
               name);
      } else if (dsl_macro(name, strlen(name))) {
        printf("    `%s()` is a DSL macro: it belongs inside a `view`, `action`, handler or\n"
               "    test body, and the enclosing block may not have closed\n",
               name);
      } else {
        printf("    `%s` is not declared here — check the header it comes from, or the\n"
               "    spelling (the whole surface is `webc api`)\n",
               name);
      }
      explained++;
    } else if (quoted(e->msg, "unknown type name ", name, sizeof name)) {
      if (dsl_macro(name, strlen(name)))
        printf("    `%s` is a declaration macro: it must stand at file scope, not inside a\n"
               "    view, a handler or another macro's block\n",
               name);
      else
        printf("    no type named `%s` — a model is `WEBC_MODEL(%s, …)`, a view's context is\n"
               "    its own struct, and `Vec(%s)` makes `Vec%s`\n",
               name, name, name, name);
      explained++;
    } else if (strstr(e->msg, "to parameter of incompatible type 'str'")) {
      printf("    a body is bytes, not a C string: `json(200, s)` with a `str`, `json_list(200, rows)`\n"
             "    with a collection, or `json(200, Model, &row)` — never a JSON body written by hand\n");
      explained++;
    } else {
      say(4, e->msg);
    }
  }
  engine_close(&E);
  arena_destroy(a);
  if (!explained) return false;
  (void)explained;
  return true;
}

// ---------------------------------------------------------------- public: project

// `skip` is the build's own output: a file the compiler already spoke about must not be
// spoken about twice. A failed build stops at the first file it cannot compile, so the
// rest of the project's errors would otherwise wait for the next attempt — and an agent
// pays a whole turn for each one.
static i32 project_scan(const char *root, bool json, bool quiet, const char *skip) {
  Engine E;
  if (!engine_open(&E, root)) {
    if (!json) fprintf(stderr, "webc: no project under %s (no webc.json, no src/)\n", root);
    engine_close(&E);
    return 0;
  }
  Buf out;
  buf_init(&out, E.a, 8192);
  i32 errors = 0, files_with = 0;
  if (json) buf_puts(&out, "{\"root\":");
  if (json) json_str(&out, root);
  if (json) buf_puts(&out, ",\"files\":[");
  for (i32 i = 0; i < E.L.F->nfile; i++) {
    WFile *f = &E.L.F->file[i];
    if (!f->indexed || f->proj < 0) continue;
    const char *path = f->path;
    size_t nn = strlen(path);
    if (nn < 2 || strcmp(path + nn - 2, ".c")) continue;
    char relp[1024];
    rel_to(root, path, relp, sizeof relp);
    if (skip && *skip) {
      char full[1100];
      snprintf(full, sizeof full, "%s:", relp);
      char dotted[1100];
      snprintf(dotted, sizeof dotted, "./%s:", relp);
      if (strstr(skip, full) || strstr(skip, dotted) || strstr(skip, path)) continue;
    }
    // the tests are their own program: a suite's errors are worth reporting too
    WDiags *dg = diags_of(&E, path);
    WDoc *d = doc_of(&E, path);
    if (!dg || !d || dg->n == 0) continue;
    i32 nerr = 0;
    for (i32 k = 0; k < dg->n; k++)
      if (dg->d[k].sev == WSEV_ERROR) nerr++;
    if (nerr == 0) continue;
    errors += nerr;
    files_with++;
    char rel[1024];
    rel_to(root, path, rel, sizeof rel);
    if (json) {
      if (files_with > 1) buf_puts(&out, ",");
      buf_puts(&out, "{\"path\":");
      json_str(&out, rel);
      buf_puts(&out, ",\"diagnostics\":[");
      i32 emitted = 0;
      for (i32 k = 0; k < dg->n; k++) {
        WDiag *g = &dg->d[k];
        if (g->sev > WSEV_WARN) continue;
        i32 line = 0, col = 0;
        dsl_line_col(d->src, g->begin, &line, &col);
        if (emitted++) buf_puts(&out, ",");
        buf_printf(&out, "{\"line\":%d,\"col\":%d,\"severity\":", line + 1, col + 1);
        json_str(&out, sev_name(g->sev));
        buf_puts(&out, ",\"code\":");
        json_str(&out, g->code);
        buf_puts(&out, ",\"message\":");
        json_str(&out, g->msg);
        if (g->fix_title && *g->fix_title) {
          buf_puts(&out, ",\"fix\":");
          json_str(&out, g->fix_title);
          if (g->fix_text) {
            buf_puts(&out, ",\"fix_text\":");
            json_str(&out, g->fix_text);
          }
          if (g->fix_file && *g->fix_file) {
            buf_puts(&out, ",\"fix_file\":");
            json_str(&out, g->fix_file);
          }
        }
        if (g->related && *g->related) {
          buf_puts(&out, ",\"compiler\":");
          json_str(&out, g->related);
        }
        buf_puts(&out, "}");
      }
      buf_puts(&out, "]}");
    } else if (!quiet) {
      for (i32 k = 0; k < dg->n; k++) {
        WDiag *g = &dg->d[k];
        if (g->sev > WSEV_WARN) continue;
        i32 line = 0, col = 0;
        dsl_line_col(d->src, g->begin, &line, &col);
        print_diag(rel, line, col, g->msg, g->fix_title, g->related);
      }
    }
  }
  if (json) {
    buf_printf(&out, "],\"errors\":%d}", errors);
    printf("%s\n", buf_cstr(&out));
  }
  engine_close(&E);
  return errors;
}

i32 explain_project(const char *root_in, bool json, bool quiet) {
  char rootbuf[1024];
  return project_scan(abs_root(rootbuf, sizeof rootbuf, root_in), json, quiet, nullptr);
}

i32 explain_project_rest(const char *root_in, const char *compiler_output) {
  char rootbuf[1024];
  const char *root = abs_root(rootbuf, sizeof rootbuf, root_in);
  i32 n = project_scan(root, false, true, compiler_output);
  if (n == 0) return 0;
  printf("\n  %sthe WebC engine, on the files the build did not reach%s\n", g_d, g_x);
  project_scan(root, false, false, compiler_output);
  return n;
}

// ---------------------------------------------------------------- public: command

i32 cmd_explain(const char *root, const char *self, i32 argc, char **argv) {
  bool json = false, raw = false, project = false;
  for (i32 i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--json")) json = true;
    else if (!strcmp(argv[i], "--raw")) raw = true;
    else if (!strcmp(argv[i], "--project")) project = true;
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      printf("  webc explain [--json] [--raw] [--project]\n"
             "                            build, then say what each error means in WebC\n"
             "  webc explain --project    no build: diagnose the sources on disk\n"
             "  webc explain --json       the same, as data (one object per file)\n");
      return 0;
    } else {
      fprintf(stderr, "webc explain: unknown option '%s'\n", argv[i]);
      return 1;
    }
  }
  if (project) {
    // The framework checkout is a tree of example apps, not an app: running the engine
    // over `src/` there answers questions about a program that does not exist.
    char probe[1200];
    snprintf(probe, sizeof probe, "%s/webc.json", root);
    bool is_app = access(probe, R_OK) == 0;
    snprintf(probe, sizeof probe, "%s/include/webc", root);
    bool is_framework = access(probe, F_OK) == 0;
    if (!is_app && is_framework) {
      printf("  this is the framework checkout, not an app — `cd` into an app, or run\n"
             "  `webc explain --project` there (apps: %s/examples/*)\n",
             root);
      return 0;
    }
    return explain_project(root, json, false) > 0 ? 1 : 0;
  }

  // The real build, captured, so its output can be annotated rather than repeated.
  // WEBC_NO_EXPLAIN keeps the child from annotating what the parent is about to.
  char cmd[2048];
  snprintf(cmd, sizeof cmd, "WEBC_NO_EXPLAIN=1 %s build 2>&1", self);
  WCap cap;
  wcap_begin(&cap);
  int rc = system(cmd);
  char *text = wcap_end(&cap);
  bool ok = (rc == 0);
  i32 code = 0;
  if (ok) {
    if (text) fputs(text, stdout);
  } else {
    bool annotated = explain_compiler_output(root, text ? text : "");
    if (!annotated || raw) {
      if (text) fputs(text, stderr);
    }
    // and whatever the WebC engine sees that the compiler never got to
    if (!json) explain_project(root, false, false);
    code = 1;
  }
  if (json) explain_project(root, true, false);
  free(text);
  return code;
}