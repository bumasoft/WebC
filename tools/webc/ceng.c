// SPDX-License-Identifier: Apache-2.0
// WebC — see ceng.h. One compiler invocation, parsed.
#include "ceng.h"
#include "prefix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

// The compiler we ask, and whether asking was worth it. Both are process-wide: an
// editor session has one.
static char g_cc[128];
static bool g_checked, g_ok, g_disabled;
static u32 g_last_ms;

bool ceng_available(void) { return g_ok && !g_disabled; }
u32 ceng_last_ms(void) { return g_last_ms; }

// A path as /bin/sh wants it: the project root can have a space in it.
static void shq(Buf *b, const char *s) {
  buf_putc(b, '\'');
  for (const char *p = s; *p; p++) {
    if (*p == '\'') {
      buf_puts(b, "'\\''");
      continue;
    }
    buf_putc(b, *p);
  }
  buf_putc(b, '\'');
}

static bool dir_exists(const char *p) {
  DIR *d = opendir(p);
  if (!d) return false;
  closedir(d);
  return true;
}

static bool path_file(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

// `webc.json`'s `cflags`, which the app's own build already honours. Read here rather
// than through the CLI's Config so the engine has no opinion about anything else.
static const char *project_cflags(Arena *a, const char *root) {
  static char cfg[1200];
  snprintf(cfg, sizeof cfg, "%s/webc.json", root);
  if (!path_file(cfg)) return "";
  Arena *tmp = arena_new(64 * 1024);
  str text = {(char *)"", 0};
  FILE *f = fopen(cfg, "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = (char *)arena_alloc(tmp, (size_t)n + 1);
    if (p && fread(p, 1, (size_t)n, f) == (size_t)n) {
      p[n] = 0;
      text = (str){p, (size_t)n};
    }
    fclose(f);
  }
  const char *out = "";
  JParse jp = json_parse(tmp, text);
  if (jp.ok && jp.v && jp.v->kind == J_OBJ) {
    JVal *v = json_get(jp.v, "cflags");
    if (v && v->kind == J_STR) {
      char *copy = (char *)arena_alloc(a, v->s.n + 1);
      if (copy) {
        memcpy(copy, v->s.p, v->s.n);
        copy[v->s.n] = 0;
        out = copy;
      }
    }
  }
  arena_destroy(tmp);
  return out;
}

// Which compiler: the one the project builds with, so the server and `webc build`
// cannot disagree about what an error is.
static void find_cc(void) {
  if (g_checked) return;
  g_checked = true;
  const char *env = getenv("WEBC_CC");
  const char *cands[4] = {env && *env ? env : nullptr, "clang", "cc", "gcc"};
  for (i32 i = 0; i < 4; i++) {
    if (!cands[i]) continue;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", cands[i]);
    if (system(cmd) == 0) {
      snprintf(g_cc, sizeof g_cc, "%s", cands[i]);
      return;
    }
  }
  g_disabled = true;
}

// The flags a WebC translation unit is compiled with, in one place so the check and
// the compile database cannot disagree about what that is. They are handed out one at
// a time because the two callers need them in different shapes: a shell command and a
// JSON array.
typedef void (*flag_fn)(void *ctx, const char *arg);

static void each_flag(WLsp *L, const char *dir, flag_fn fn, void *ctx) {
  fn(ctx, "-std=c23");
  char inc[1024];
  snprintf(inc, sizeof inc, "%s/include", webc_prefix());
  fn(ctx, "-I");
  fn(ctx, inc);
  char gen[1024];
  snprintf(gen, sizeof gen, "%s/build/gen", webc_prefix());
  if (dir_exists(gen)) {
    fn(ctx, "-I");
    fn(ctx, gen);
  }
  // the file's own directory: `#include "todo.h"` is how a WebC source sees its header
  if (dir && dir[0]) {
    fn(ctx, "-I");
    fn(ctx, dir);
  }
  char srcd[1024];
  snprintf(srcd, sizeof srcd, "%s/src", L->F->root);
  if (dir_exists(srcd)) {
    fn(ctx, "-I");
    fn(ctx, srcd);
  }
  fn(ctx, "-D_DARWIN_C_SOURCE=1");
  fn(ctx, "-D_GNU_SOURCE=1");
  // `webc.json`'s own cflags, split on spaces: the app's build honours them, so the
  // compiler the server asks must too (a quoted flag in `cflags` is not supported by
  // `webc build` either).
  char *cf = (char *)project_cflags(L->req, L->F->root);
  while (cf && *cf) {
    while (*cf == ' ') cf++;
    if (!*cf) break;
    char *space = strchr(cf, ' ');
    if (space) *space = 0;
    fn(ctx, cf);
    if (!space) break;
    cf = space + 1;
  }
}

typedef struct ShCtx {
  Buf *b;
} ShCtx;

static void sh_flag(void *ctx, const char *arg) {
  ShCtx *c = (ShCtx *)ctx;
  buf_putc(c->b, ' ');
  shq(c->b, arg);
}

// A JSON string on a FILE*, escaped. Small enough to keep here rather than grow the
// json module a stream API it does not otherwise want.
static void fstr(FILE *f, const char *s) {
  fputc('"', f);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '"': fputs("\\\"", f); break;
      case '\\': fputs("\\\\", f); break;
      case '\n': fputs("\\n", f); break;
      case '\r': fputs("\\r", f); break;
      case '\t': fputs("\\t", f); break;
      default:
        if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
  }
  fputc('"', f);
}

typedef struct JsCtx {
  FILE *f;
  i32 n;
} JsCtx;

static void js_flag(void *ctx, const char *arg) {
  JsCtx *c = (JsCtx *)ctx;
  if (c->n++) fputs(", ", c->f);
  fstr(c->f, arg);
}

// `compile_commands.json` at the project root is for the *other* tools. clangd looks
// there (and in a `build/` beside it) without being told, and with it clangd stops
// guessing at a WebC file it has no include paths for — measured in this repository,
// twenty-one invented errors became none, and the breadcrumb started showing the real
// expanded symbols. The server itself never reads this file.
void ceng_write_compile_commands(WLsp *L) {
  if (!L->F || !L->F->nfile) return;
  if (getenv("WEBC_LSP_NO_COMPILE_COMMANDS")) return;
  find_cc();
  char out[1300];
  snprintf(out, sizeof out, "%s/compile_commands.json", L->F->root);
  FILE *f = fopen(out, "wb");
  if (!f) return; // no .webc/ and no permission: nothing to say, nothing broken
  fprintf(f, "[");
  i32 n = 0;
  for (i32 i = 0; i < L->F->nfile; i++) {
    const char *path = L->F->file[i].path;
    size_t len = strlen(path);
    if (len < 3 || (strcmp(path + len - 2, ".c") && strcmp(path + len - 2, ".h"))) continue;
    char own[1024];
    snprintf(own, sizeof own, "%s", path);
    char *slash = strrchr(own, '/');
    if (slash) *slash = 0;
    if (n++) fputs(",\n", f);
    fputs("{\n  \"directory\": ", f);
    fstr(f, L->F->root);
    fputs(",\n  \"file\": ", f);
    fstr(f, path);
    fputs(",\n  \"arguments\": [", f);
    JsCtx c = {f, 0};
    js_flag(&c, g_cc[0] ? g_cc : "clang");
    each_flag(L, own, js_flag, &c);
    fputs("]\n}", f);
  }
  fputs("\n]\n", f);
  fclose(f);
}

// Byte offset of a 1-based (line, column) as the compiler reports them. The compiler
// counts bytes in a column, which is what this file's offsets are too.
static u32 off_of(const WSrc *s, i32 line, i32 col) {
  i32 l = 1;
  u32 i = 0;
  while (i < s->n && l < line) {
    if (s->text[i] == '\n') l++;
    i++;
  }
  for (i32 c = 1; c < col && i < s->n && s->text[i] != '\n'; c++) i++;
  return i;
}

// ---------------------------------------------------------------- the check

typedef struct CLine {
  i32 line, col;
  WSev sev;
  str msg;
} CLine;

// A macro's own sentence — its name and its parentheses — is a symbol the macro
// invented (`view_Nope`, `WC_CAT(wc_route_sel_, GET)`), so nothing the compiler says
// about those bytes is worth showing. A macro's *block* is different: inside
// `view(Todos, VecTodo *t) { … }` the statement is C again, and a wrong field name
// there is exactly the error a person wants on the right line. So only the head and
// the arguments are out of the compiler's reach.
static bool in_macro_head(const WScan *sc, u32 off) {
  for (i32 i = 0; i < sc->n; i++) {
    const WRegion *r = &sc->reg[i];
    u32 end = r->close ? r->close + 1 : r->head + r->head_len;
    if (off >= r->head && off < end) return true;
  }
  return false;
}

void ceng_check(WLsp *L, WDoc *d, WDiags *dg) {
  if (!d->src || !d->text.n) return;
  if (!d->path || (strstr(d->path, ".c") == nullptr && strstr(d->path, ".h") == nullptr))
    return;
  find_cc();
  if (g_disabled) return;
  // A project whose check does not fit in a keystroke is checked when the buffer is
  // saved instead. The budget is the rule, not a paragraph: past 120 ms the
  // editor would feel this one, so it stops happening on every character.
  if (d->slow && !L->saving) return;

  // A block that never closed, or a call that never closed, means the rest of the file
  // is not C any more: every sentence the compiler would write is about the
  // unbalanced brace. The WebC engine already said where it is.
  for (i32 i = 0; i < dg->n; i++) {
    if (!dg->d[i].code) continue;
    if (!strcmp(dg->d[i].code, "unclosed") || !strcmp(dg->d[i].code, "unclosed-block")) return;
  }

  Arena *a = arena_new(64 * 1024);
  // the document's own directory, for `#include "todo.h"`
  char dir[1024];
  snprintf(dir, sizeof dir, "%s", d->path);
  char *slash = strrchr(dir, '/');
  if (slash) *slash = 0;
  else snprintf(dir, sizeof dir, ".");

  // The buffer the editor holds, in a file the compiler can read. Written outside the
  // project on purpose: a language server must not litter the tree it is reading.
  char tmp[] = "/tmp/webc-lsp-XXXXXX.c";
  i32 fd = mkstemps(tmp, 2);
  if (fd < 0) {
    arena_destroy(a);
    return;
  }
  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(tmp);
    arena_destroy(a);
    return;
  }
  fwrite(d->text.p, 1, d->text.n, f);
  fclose(f);

  Buf cmd;
  buf_init(&cmd, a, 4096);
  buf_puts(&cmd, g_cc);
  buf_puts(&cmd, " -fsyntax-only -ferror-limit=0 -fno-caret-diagnostics -fno-color-diagnostics");
  ShCtx sc = {&cmd};
  each_flag(L, dir, sh_flag, &sc);
  buf_puts(&cmd, " -x c ");
  shq(&cmd, tmp);
  buf_puts(&cmd, " 2>&1");

  u64 t0 = wc_time_ms();
  Buf out;
  buf_init(&out, a, 4096);
  FILE *p = popen(buf_cstr(&cmd), "r");
  if (!p) {
    unlink(tmp);
    arena_destroy(a);
    return;
  }
  char line[4096];
  size_t tlen = strlen(tmp);
  i32 added = 0;
  while (fgets(line, sizeof line, p)) {
    // `<path>:<line>:<col>: error: message` — and nothing else counts. A note is a
    // sentence about a sentence, and the file we asked about is the only one whose
    // offsets mean anything to this document.
    if (strncmp(line, tmp, tlen) != 0) continue;
    i32 ln = 0, col = 0, used = 0;
    if (sscanf(line + tlen, ":%d:%d: %n", &ln, &col, &used) < 2 || !used) continue;
    const char *rest = line + tlen + used;
    WSev sev;
    if (!strncmp(rest, "error:", 6)) {
      sev = WSEV_ERROR;
      rest += 6;
    } else if (!strncmp(rest, "warning:", 8)) {
      sev = WSEV_WARN;
      rest += 8;
    } else {
      continue; // note:, remark:
    }
    while (*rest == ' ') rest++;
    size_t rn = strlen(rest);
    while (rn && (rest[rn - 1] == '\n' || rest[rn - 1] == '\r')) rn--;
    u32 off = off_of(d->src, ln, col);
    if (off >= d->text.n) continue;
    // Inside a DSL region the compiler is talking about generated symbols — the WebC
    // engine owns those bytes (it either already said the thing, or there is nothing
    // to say).
    if (in_macro_head(d->scan, off)) continue;
    // The rest of the filter: a sentence whose subject is a name the DSL invented is
    // a cascade, not an error about the program.
    if (strstr(rest, "WC_CAT") || strstr(rest, "WC_ROUTE") || strstr(rest, "view_"))
      continue;
    dsl_diagf(dg, dg->a, off, off + 1, sev, sev == WSEV_ERROR ? "c" : "c-warning", "%.*s",
              (int)rn, rest);
    added++;
  }
  pclose(p);
  g_last_ms = (u32)(wc_time_ms() - t0);
  g_ok = true;
  unlink(tmp);
  buf_free(&out);
  buf_free(&cmd);
  arena_destroy(a);
  (void)added;
}