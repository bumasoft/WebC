// SPDX-License-Identifier: Apache-2.0
// WebC — the project index.
//
// One pass over the files the project compiles, one list of facts, and nothing
// guessed: every entry carries the file and the position it was read from.
#include "facts.h"
#include "prefix.h"

#include <dirent.h>
#include <sys/stat.h>

// ---------------------------------------------------------------- small helpers

static bool path_is_file(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static bool path_is_dir(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static str slurp(Arena *a, const char *path, u32 *mtime) {
  if (mtime) *mtime = 0;
  FILE *f = fopen(path, "rb");
  if (!f) return (str){"", 0};
  struct stat st;
  if (mtime && stat(path, &st) == 0) *mtime = (u32)st.st_mtime;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  if (n < 0) n = 0;
  fseek(f, 0, SEEK_SET);
  char *buf = arena_alloc(a, (size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = 0;
  return (str){buf, got};
}

static void *arr_grow(Arena *a, void *p, i32 *n, i32 *cap, size_t elem) {
  if (*n < *cap) return p;
  i32 ncap = *cap ? *cap * 2 : 8;
  void *q = arena_alloc(a, elem * (size_t)ncap);
  if (p) memcpy(q, p, elem * (size_t)*n);
  *cap = ncap;
  return q;
}

#define PUSH(F, arr, n_arr, cap_arr, val)                                                      \
  do {                                                                                         \
    (F)->arr = arr_grow((F)->a, (F)->arr, &(F)->n_arr, &(F)->cap_arr, sizeof(*(F)->arr));      \
    (F)->arr[(F)->n_arr++] = (val);                                                             \
  } while (0)

static char *dup(Arena *a, const char *p, size_t n) {
  char *s = arena_alloc(a, n + 1);
  memcpy(s, p, n);
  s[n] = 0;
  return s;
}

// ---------------------------------------------------------------- files and halves

static i32 file_find(WFacts *F, const char *path) {
  for (i32 i = 0; i < F->nfile; i++)
    if (!strcmp(F->file[i].path, path)) return i;
  return -1;
}

static i32 file_add(WFacts *F, const char *path, WHalf half, i32 proj) {
  i32 i = file_find(F, path);
  if (i >= 0) return i;
  F->file = arr_grow(F->a, F->file, &F->nfile, &F->capfile, sizeof(WFile));
  WFile *f = &F->file[F->nfile];
  memset(f, 0, sizeof *f);
  f->path = dup(F->a, path, strlen(path));
  f->half = half;
  f->proj = proj;
  return F->nfile++;
}

const char *facts_file_path(WFacts *F, i32 fi) {
  if (fi < 0 || fi >= F->nfile) return "";
  return F->file[fi].path;
}

i32 facts_proj_of(WFacts *F, i32 fi) {
  if (fi < 0 || fi >= F->nfile) return -1;
  return F->file[fi].proj;
}

const char *facts_shared_header(WFacts *F, i32 fi) {
  i32 p = facts_proj_of(F, fi);
  if (p < 0 || p >= F->nproj) return "";
  return F->proj[p].shared_header;
}

// A file the project does not name has no half we can prove. Treating it as the
// server half is the permissive answer on purpose: an editor buffer that is not in
// `webc.json` yet must not be told its route macro is in the wrong place. The half
// rule fires where it is true — on a file a set *does* name.
WHalf facts_half_of(const char *path, i32 fi, WFacts *F) {
  (void)F;
  const char *dot = strrchr(path, '.');
  if (dot && !strcmp(dot, ".h")) return WH_SHARED; // a header is read by both halves
  return (fi >= 0 && fi < F->nfile) ? F->file[fi].half : WH_SERVER;
}

// A directory walk that keeps only the extensions a project compiles.
typedef void (*path_fn)(void *ctx, const char *path);

static void walk(const char *dir, i32 depth, path_fn fn, void *ctx) {
  if (depth > 8) return;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    if (path_is_dir(p)) {
      walk(p, depth + 1, fn, ctx);
      continue;
    }
    size_t len = strlen(e->d_name);
    if (len > 2) {
      const char *ext = e->d_name + len;
      ext -= 2;
      if (!strcmp(ext, ".c") || !strcmp(ext, ".h")) fn(ctx, p);
    }
  }
  closedir(d);
}

// ---------------------------------------------------------------- extraction

static const char *model_arg_macros[] = {"db_all", "db_all_ordered", "db_all_where", "db_where",
                                         "db_insert", "db_update", "db_delete", "db_get",
                                         "db_save", "db_try_save", "db_find", "db_count", "body",
                                         "body_try", "body_json", "body_form"};

static i32 model_arg_index(const char *macro, i32 nargs) {
  if (!strcmp(macro, "db_all")) return nargs - 1; // db_all(T) and db_all(db, T)
  if (!strcmp(macro, "db_all_ordered") || !strcmp(macro, "db_all_where") ||
      !strcmp(macro, "db_where") || !strcmp(macro, "db_insert") || !strcmp(macro, "db_update"))
    return nargs > 1 ? 1 : 0;
  if (!strcmp(macro, "db_delete")) return nargs >= 3 ? 1 : 0;
  return 0;
}

static void call_add(WFacts *F, i32 fi, const char *macro, const char *name, size_t nname, i32 arg,
                     u32 off, u32 len, u32 call_off, u32 call_len, i32 line, i32 col) {
  // A name that is not an identifier is not a fact (a computed hook name, a `str`
  // variable). The index records only what the token stream already proves.
  if (nname == 0) return;
  WCall c = {.macro = dup(F->a, macro, strlen(macro)),
             .name = dup(F->a, name, nname),
             .arg = arg,
             .file = fi,
             .off = off,
             .len = len,
             .call_off = call_off,
             .call_len = call_len,
             .line = line,
             .col = col};
  PUSH(F, call, ncall, capcall, c);
}

static void sym_push(Arena *a, WSym **v, i32 *n, i32 *cap, char *name, i32 fi, u32 off, u32 len,
                     i32 line, i32 col, char *detail) {
  if (*n == *cap) {
    i32 ncap = *cap ? *cap * 2 : 8;
    WSym *q = arena_alloc(a, sizeof(WSym) * (size_t)ncap);
    if (*v) memcpy(q, *v, sizeof(WSym) * (size_t)*n);
    *v = q;
    *cap = ncap;
  }
  (*v)[*n] = (WSym){.name = name, .file = fi, .off = off, .len = len, .line = line, .col = col,
                    .detail = detail};
  (*n)++;
}

// An identifier inside a byte range, or null. `which` picks the nth ident.
static const WTok *ident_in(const WSrc *s, u32 begin, u32 end, i32 which) {
  i32 seen = 0;
  for (i32 i = 0; i < s->ntok; i++) {
    const WTok *t = &s->tok[i];
    if (t->off < begin || t->off >= end) continue;
    if (t->kind != WT_IDENT) continue;
    if (seen++ == which) return t;
  }
  return nullptr;
}

// The text of a string literal, quotes removed. Returns the inner byte range.
static bool str_inner(const WSrc *s, const WArg *a, u32 *b, u32 *e) {
  if (!a->is_str || a->end < a->begin + 2) return false;
  char q = s->text[a->begin];
  if (s->text[a->end - 1] != q) return false;
  *b = a->begin + 1;
  *e = a->end - 1;
  return true;
}

static void model_add(WFacts *F, i32 fi, const WSrc *s, const WRegion *r, bool internal) {
  const WTok *t = ident_in(s, r->arg[0].begin, r->arg[0].end, 0);
  if (!t) return;
  WMod m = {0};
  m.name = dup(F->a, s->text + t->off, t->len);
  m.internal = internal;
  m.file = fi;
  m.off = t->off;
  m.len = t->len;
  m.line = t->line;
  m.col = t->col;
  m.nf = 0;
  i32 capf = 0;
  for (i32 k = 1; k < r->nargs; k++) {
    const WArg *fa = &r->arg[k];
    if (!fa->is_paren) continue;
    WArg parts[4];
    i32 np = dsl_split(s, fa->begin + 1, fa->end - 1, parts, 4);
    if (np < 1) continue;
    const WTok *fn = ident_in(s, parts[0].begin, parts[0].end, 0);
    if (!fn) continue;
    WFld f = {0};
    f.name = dup(F->a, s->text + fn->off, fn->len);
    f.off = fn->off;
    f.len = fn->len;
    f.line = fn->line;
    f.col = fn->col;
    if (np >= 2) {
      const WTok *fk = ident_in(s, parts[1].begin, parts[1].end, 0);
      if (fk) f.kind = dup(F->a, s->text + fk->off, fk->len);
    }
    if (np >= 3) {
      // `.pk = true, .max = 200` — the span from the first qualifier to the last,
      // because dsl_split cuts on commas and a tuple's qualifiers are comma-separated
      // too: reading parts[2] alone kept the first and dropped the rest, which is why
      // `.max` never reached the facts (and `webc openapi` had nothing to generate
      // a `maxLength` from).
      f.quals = dup(F->a, s->text + parts[2].begin, parts[np - 1].end - parts[2].begin);
      // `.pk = true` and friends, read as text: the qualifier vocabulary is closed
      // and a substring search over three characters is exact enough for a flag.
      const char *q = f.quals;
      f.pk = strstr(q, ".pk") != nullptr;
      f.required = strstr(q, ".required") != nullptr;
      f.unique = strstr(q, ".unique") != nullptr;
      f.not_null = strstr(q, ".not_null") != nullptr;
    }
    if (!f.kind) f.kind = dup(F->a, "?", 1);
    if (m.nf == capf) {
      i32 ncap = capf ? capf * 2 : 8;
      WFld *q = arena_alloc(F->a, sizeof(WFld) * (size_t)ncap);
      if (m.f) memcpy(q, m.f, sizeof(WFld) * (size_t)m.nf);
      m.f = q;
      capf = ncap;
    }
    m.f[m.nf++] = f;
  }
  if (F->nmod == F->capmod) {
    i32 ncap = F->capmod ? F->capmod * 2 : 8;
    WMod *q = arena_alloc(F->a, sizeof(WMod) * (size_t)ncap);
    if (F->mod) memcpy(q, F->mod, sizeof(WMod) * (size_t)F->nmod);
    F->mod = q;
    F->capmod = ncap;
  }
  F->mod[F->nmod++] = m;
}

char *facts_model_detail(WFacts *F, const WMod *m) {
  Buf b;
  buf_init(&b, F->a, 256);
  buf_printf(&b, "%s", m->name);
  for (i32 i = 0; i < m->nf; i++) {
    buf_printf(&b, "%s%s: %s", i ? ", " : "  ", m->f[i].name, m->f[i].kind);
    if (m->f[i].pk) buf_puts(&b, " pk");
  }
  return buf_cstr(&b);
}

static void extract(WFacts *F, i32 fi, const WSrc *s, const WScan *sc) {
  for (i32 i = 0; i < s->ntok; i++) {
    const WTok *t = &s->tok[i];
    if (t->kind != WT_PP) continue;
    // `#include <webc/x.h>`, the module's own manifest: the include is what
    // says which modules this app uses, and in which half it may use them.
    const char *p = s->text + t->off, *end = p + t->len;
    const char *inc = strstr(p, "include");
    if (!inc || inc > end) continue;
    const char *lt = memchr(inc, '<', (size_t)(end - inc));
    if (!lt) continue;
    const char *gt = memchr(lt, '>', (size_t)(end - lt));
    if (!gt) continue;
    if (gt - lt < 8 || strncmp(lt, "<webc/", 6) != 0) continue;
    const char *nm = lt + 6;
    size_t nlen = (size_t)(gt - nm);
    if (nlen > 4 && !strncmp(nm + nlen - 2, ".h", 2)) nlen -= 2;
    if (F->file[fi].nmods >= 16) continue;
    F->file[fi].mods[F->file[fi].nmods++] = dup(F->a, nm, nlen);
  }

  for (i32 i = 0; i < sc->n; i++) {
    const WRegion *r = &sc->reg[i];
    const char *mn = r->macro->name;
    if (!strcmp(mn, "WEBC_MODEL") || !strcmp(mn, "WEBC_MODEL_TABLE") ||
        !strcmp(mn, "WEBC_MODEL_ROUTE")) {
      model_add(F, fi, s, r, false);
      continue;
    }
    if (!strcmp(mn, "WEBC_MODEL_INTERNAL")) {
      model_add(F, fi, s, r, true);
      continue;
    }
    if (!strcmp(mn, "WEBC_ENUM")) {
      const WTok *t = ident_in(s, r->arg[0].begin, r->arg[0].end, 0);
      if (t)
        sym_push(F->a, &F->enumt, &F->nenum, &F->capenum,
                 dup(F->a, s->text + t->off, t->len), fi, t->off, t->len, t->line, t->col, nullptr);
      continue;
    }
    if (!strcmp(mn, "Vec") || !strcmp(mn, "WEBC_VEC_DEFINE")) {
      i32 arg = !strcmp(mn, "Vec") ? 0 : 2;
      if (arg >= r->nargs) continue;
      const WTok *t = ident_in(s, r->arg[arg].begin, r->arg[arg].end, 0);
      if (t) {
        // `Vec(Todo)` declares the collection for Todo; `WEBC_VEC_DEFINE(VecX, x, T)`
        // declares it for T. The fact is the *model*, because that is what db_all asks for.
        sym_push(F->a, &F->vec, &F->nvec, &F->capvec, dup(F->a, s->text + t->off, t->len), fi,
                 t->off, t->len, t->line, t->col, nullptr);
      }
      continue;
    }
    if (!strcmp(mn, "view")) {
      const WTok *t = ident_in(s, r->arg[0].begin, r->arg[0].end, 0);
      if (!t) continue;
      char *detail = nullptr;
      if (r->nargs >= 2)
        detail = dup(F->a, s->text + r->arg[1].begin, r->arg[1].end - r->arg[1].begin);
      sym_push(F->a, &F->view, &F->nview, &F->capview, dup(F->a, s->text + t->off, t->len), fi,
               t->off, t->len, t->line, t->col, detail);
      continue;
    }
    if (!strcmp(mn, "declare_view")) {
      const WTok *t = ident_in(s, r->arg[0].begin, r->arg[0].end, 0);
      if (!t) continue;
      char *detail = nullptr;
      if (r->nargs >= 2)
        detail = dup(F->a, s->text + r->arg[1].begin, r->arg[1].end - r->arg[1].begin);
      sym_push(F->a, &F->dview, &F->ndview, &F->capdview, dup(F->a, s->text + t->off, t->len), fi,
               t->off, t->len, t->line, t->col, detail);
      continue;
    }
    if (!strcmp(mn, "action") || !strcmp(mn, "action_live")) {
      const WTok *t = ident_in(s, r->arg[0].begin, r->arg[0].end, 0);
      if (!t) continue;
      char *detail = nullptr;
      if (r->nargs >= 2)
        detail = dup(F->a, s->text + r->arg[1].begin, r->arg[1].end - r->arg[1].begin);
      sym_push(F->a, &F->act, &F->nact, &F->capact, dup(F->a, s->text + t->off, t->len), fi,
               t->off, t->len, t->line, t->col, detail);
      continue;
    }
    if (!strcmp(mn, "GET") || !strcmp(mn, "POST") || !strcmp(mn, "PUT") || !strcmp(mn, "PATCH") ||
        !strcmp(mn, "DELETE") || !strcmp(mn, "HEAD") || !strcmp(mn, "OPTIONS") ||
        !strcmp(mn, "ANY") || !strcmp(mn, "GET_MW") || !strcmp(mn, "POST_MW")) {
      i32 parg = strstr(mn, "_MW") ? 1 : 0;
      if (parg >= r->nargs) continue;
      u32 b, e;
      if (!str_inner(s, &r->arg[parg], &b, &e)) continue;
      WRoute rt = {.method = dup(F->a, mn, strcspn(mn, "_")), .path = dup(F->a, s->text + b, e - b),
                   .view = nullptr, .file = fi, .off = r->head, .len = r->head_len,
                   .line = r->pp_line, .col = 0};
      // the single view the block returns becomes the route's answer (§6.3)
      if (r->has_block && r->bclose) {
        for (i32 k = i + 1; k < sc->n; k++) {
          const WRegion *q = &sc->reg[k];
          if (q->head >= r->bclose) break;
          const char *qn = q->macro->name;
          if (strcmp(qn, "page") && strcmp(qn, "render")) continue;
          const WTok *vt = ident_in(s, q->arg[0].begin, q->arg[0].end, 0);
          if (vt) {
            if (rt.view && strcmp(rt.view, "") != 0) { // two views: not a single answer
              rt.view = nullptr;
              break;
            }
            rt.view = dup(F->a, s->text + vt->off, vt->len);
          }
        }
      }
      PUSH(F, route, nroute, caproute, rt);
      continue;
    }
    if (!strcmp(mn, "page") || !strcmp(mn, "render") || !strcmp(mn, "mail_view")) {
      i32 ai = !strcmp(mn, "mail_view") ? 1 : 0;
      if (ai >= r->nargs) continue;
      const WTok *t = ident_in(s, r->arg[ai].begin, r->arg[ai].end, 0);
      if (t)
        call_add(F, fi, mn, s->text + t->off, t->len, ai, t->off, t->len, r->head,
                 r->has_block && r->bclose ? r->bclose + 1 - r->head : r->head + r->head_len,
                 t->line, t->col);
      continue;
    }
    if (!strcmp(mn, "json") || !strcmp(mn, "json_list")) {
      // A response's shape, named where it is built: `json(201, Note, &n)` is one row,
      // `json_list(200, db_all(Note))` is a list — and the identifier that names the
      // shape inside a `db_all(...)` is the last one in the argument, not the first.
      // `json(200, db_all(Note))` is the same list in the other spelling, so the
      // first identifier decides which reading the argument gets. The first argument
      // is the status, when it is a number literal. These facts are what
      // `webc openapi` generates an API surface from.
      if (r->nargs < 2) continue;
      bool list = !strcmp(mn, "json_list");
      const WTok *first = ident_in(s, r->arg[1].begin, r->arg[1].end, 0);
      if (first && !list) {
        const char *coll[] = {"db_all",      "db_all_ordered", "db_all_where",
                              "db_all_w",    "db_where",       "db_page"};
        for (size_t k = 0; k < WC_COUNTOF(coll); k++)
          if ((u32)first->len == strlen(coll[k]) &&
              !strncmp(s->text + first->off, coll[k], first->len))
            list = true;
      }
      const WTok *t = nullptr;
      if (!list) {
        t = first;
      } else {
        i32 n_ident = 0;
        for (i32 k = 0; k < s->ntok; k++) {
          const WTok *q = &s->tok[k];
          if (q->off >= r->arg[1].begin && q->off < r->arg[1].end && q->kind == WT_IDENT)
            n_ident++;
        }
        t = n_ident ? ident_in(s, r->arg[1].begin, r->arg[1].end, n_ident - 1) : nullptr;
      }
      i32 status = 0;
      for (u32 k = r->arg[0].begin; k + 1 < r->arg[0].end; k++) {
        char ch = s->text[k];
        if (ch >= '0' && ch <= '9') {
          status = (i32)strtol(s->text + k, nullptr, 10);
          break;
        }
      }
      if (t || status) {
        call_add(F, fi, mn, t ? s->text + t->off : "", t ? t->len : 0, 1,
                 t ? t->off : r->arg[1].begin, t ? t->len : 0, r->head, r->head + r->head_len,
                 t ? t->line : r->pp_line, 0);
        F->call[F->ncall - 1].status = status;
        F->call[F->ncall - 1].list = list;
      }
      continue;
    }
    if (!strcmp(mn, "path_i64")) {
      // `path_i64("id")`: the placeholder in this handler's own path is a number on
      // the handler's say-so, and a generated client wants the parameter typed.
      u32 b, e;
      if (!str_inner(s, &r->arg[0], &b, &e)) continue;
      call_add(F, fi, "path-i64", s->text + b, e - b, 0, b, e - b, r->head,
               r->head + r->head_len, r->pp_line, 0);
      continue;
    }
    bool is_model_macro = false;
    for (size_t k = 0; k < WC_COUNTOF(model_arg_macros); k++)
      if (!strcmp(mn, model_arg_macros[k])) is_model_macro = true;
    if (is_model_macro) {
      i32 ai = model_arg_index(mn, r->nargs);
      if (ai < 0 || ai >= r->nargs) continue;
      const WTok *t = ident_in(s, r->arg[ai].begin, r->arg[ai].end, 0);
      if (t)
        call_add(F, fi, mn, s->text + t->off, t->len, ai, t->off, t->len, r->head,
                 r->head + r->head_len, t->line, t->col);
      continue;
    }
    if (!strcmp(mn, "on_click")) {
      const WTok *t = ident_in(s, r->arg[0].begin, r->arg[0].end, 0);
      if (t)
        call_add(F, fi, mn, s->text + t->off, t->len, 0, t->off, t->len, r->head,
                 r->head + r->head_len, t->line, t->col);
      continue;
    }
    if (!strcmp(mn, "bind")) {
      u32 b, e;
      if (str_inner(s, &r->arg[0], &b, &e))
        call_add(F, fi, "bind-hook", s->text + b, e - b, 0, b, e - b, r->head,
                 r->head + r->head_len, r->pp_line, 0);
      if (r->nargs >= 3) {
        const WTok *t = ident_in(s, r->arg[2].begin, r->arg[2].end, 0);
        if (t)
          call_add(F, fi, "bind-action", s->text + t->off, t->len, 2, t->off, t->len, r->head,
                   r->head + r->head_len, t->line, t->col);
      }
      continue;
    }
    if (!strcmp(mn, "named")) {
      u32 b, e;
      if (!str_inner(s, &r->arg[0], &b, &e)) continue;
      call_add(F, fi, "named", s->text + b, e - b, 0, b, e - b, r->head, r->head + r->head_len,
               r->pp_line, 0);
      continue;
    }
  }
}

static void index_file(WFacts *F, i32 fi, str text) {
  if (fi < 0) return;
  Arena *a = arena_new(128 * 1024);
  WSrc *s = dsl_src(a, text);
  WScan *sc = dsl_scan(a, s);
  extract(F, fi, s, sc);
  // The facts were copied into F->a; the scan's own memory goes back in one step.
  dsl_scan_free(sc);
  dsl_src_free(s);
  arena_destroy(a);
  F->file[fi].indexed = true;
}

// ---------------------------------------------------------------- the project

typedef struct WalkCtx {
  WFacts *F;
  WHalf half;
  i32 proj;
} WalkCtx;

static void walk_add(void *ctx, const char *path) {
  WalkCtx *w = (WalkCtx *)ctx;
  file_add(w->F, path, w->half, w->proj);
}

static void add_set(WFacts *F, JVal *v, const char *key, WHalf half, const char *root, i32 proj) {
  JVal *arr = json_get(v, key);
  if (!arr || arr->kind != J_ARR) return;
  for (i32 i = 0; i < arr->n; i++) {
    if (arr->items[i]->kind != J_STR) continue;
    str rel = arr->items[i]->s;
    char p[1024];
    snprintf(p, sizeof p, "%s/%.*s", root, (int)rel.n, rel.p);
    if (path_is_dir(p)) {
      WalkCtx w = {F, half, proj};
      walk(p, 0, walk_add, &w);
    } else if (path_is_file(p)) {
      file_add(F, p, half, proj);
    }
  }
}

// The framework declares two row types of its own (RowId, WJob). A project names
// them — `body(RowId)` is in every lookup route — so the index has to know them, or
// "this is not a model" becomes a lie in the most common handler there is.
static void index_framework(WFacts *F) {
  char dir[1024];
  snprintf(dir, sizeof dir, "%s/include/webc", webc_prefix());
  typedef struct { WFacts *F; } Ctx;
  Ctx c = {F};
  (void)c;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    size_t len = strlen(e->d_name);
    if (len < 3 || strcmp(e->d_name + len - 2, ".h")) continue;
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    str text = slurp(F->a, p, nullptr);
    Arena *a = arena_new(64 * 1024);
    WSrc *s = dsl_src(a, text);
    WScan *sc = dsl_scan(a, s);
    for (i32 i = 0; i < sc->n; i++) {
      const WRegion *r = &sc->reg[i];
      if (strcmp(r->macro->name, "WEBC_MODEL") && strcmp(r->macro->name, "WEBC_MODEL_INTERNAL") &&
          strcmp(r->macro->name, "WEBC_MODEL_TABLE"))
        continue;
      const WTok *t = ident_in(s, r->arg[0].begin, r->arg[0].end, 0);
      if (!t) continue;
      if (F->nframe == F->capframe) {
        i32 ncap = F->capframe ? F->capframe * 2 : 8;
        char **q = arena_alloc(F->a, sizeof(char *) * (size_t)ncap);
        if (F->frame) memcpy(q, F->frame, sizeof(char *) * (size_t)F->nframe);
        F->frame = q;
        F->capframe = ncap;
      }
      F->frame[F->nframe++] = dup(F->a, s->text + t->off, t->len);
    }
    dsl_scan_free(sc);
    dsl_src_free(s);
    arena_destroy(a);
  }
  closedir(d);
}

// One project: its `webc.json`, its file sets, and the headers beside them. Called
// once for the root, and once per example when the root is the framework itself —
// this repository is a tree of projects, not a project.
static i32 proj_add(WFacts *F, const char *root) {
  for (i32 i = 0; i < F->nproj; i++)
    if (!strcmp(F->proj[i].root, root)) return i;
  if (F->nproj >= (i32)(sizeof F->proj / sizeof F->proj[0])) return -1;
  WProj *p = &F->proj[F->nproj];
  memset(p, 0, sizeof *p);
  snprintf(p->root, sizeof p->root, "%s", root);
  snprintf(p->name, sizeof p->name, "%s", "app");
  return F->nproj++;
}

static void load_project(WFacts *F, const char *root) {
  char cfg[1200];
  snprintf(cfg, sizeof cfg, "%s/webc.json", root);
  if (!path_is_file(cfg)) return;
  i32 pi = proj_add(F, root);
  if (pi < 0) return;
  Arena *a = arena_new(32 * 1024);
  str text = slurp(a, cfg, nullptr);
  JParse jp = json_parse(a, text);
  if (jp.ok && jp.v && jp.v->kind == J_OBJ) {
    JVal *n = json_get(jp.v, "name");
    if (n && n->kind == J_STR) {
      snprintf(F->proj[pi].name, sizeof F->proj[pi].name, "%.*s", (int)n->s.n, n->s.p);
      if (!strcmp(F->name, "app")) snprintf(F->name, sizeof F->name, "%.*s", (int)n->s.n, n->s.p);
    }
    i32 before = F->nfile;
    add_set(F, jp.v, "server", WH_SERVER, root, pi);
    add_set(F, jp.v, "shared", WH_SHARED, root, pi);
    add_set(F, jp.v, "client", WH_CLIENT, root, pi);
    if (F->nfile > before) F->has_sets = true;
    JVal *sh = json_get(jp.v, "shared");
    if (sh && sh->kind == J_ARR)
      for (i32 i = 0; i < sh->n; i++) {
        if (sh->items[i]->kind != J_STR) continue;
        str rel = sh->items[i]->s;
        if (rel.n > 2 && rel.p[rel.n - 2] == '.' && rel.p[rel.n - 1] == 'h') {
          snprintf(F->proj[pi].shared_header, sizeof F->proj[pi].shared_header, "%s/%.*s", root,
                   (int)rel.n, rel.p);
          break;
        }
      }
  }
  // Headers are never in a file set — a `webc.json` names sources — and they are
  // where `WEBC_MODEL`, `Vec(T)` and `declare_view` live.
  // Any `.h` beside a source the project compiles is part of it, for both halves.
  i32 upto = F->nfile;
  for (i32 i = 0; i < upto; i++) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", F->file[i].path);
    char *slash = strrchr(dir, '/');
    if (!slash) continue;
    *slash = 0;
    WalkCtx w = {F, WH_SHARED, pi};
    walk(dir, 0, walk_add, &w);
  }
  // A header folder with no sources of its own — `src/models/`, `src/schema/` — is still the
  // project's: every `.h` under `src/` belongs to it. Without this, the models in a folder the
  // `webc.json` sets never name were invisible to the engine, and a correct `db_get(SiteSetting)`
  // was reported as "not a model".
  {
    char srcroot[1024];
    snprintf(srcroot, sizeof srcroot, "%s/src", root);
    WalkCtx w = {F, WH_SHARED, pi};
    walk(srcroot, 0, walk_add, &w);
  }
  // Where a `declare_view` belongs when `webc.json` names no header: the project's
  // own header, which is where every app in this repository keeps it (`src/todo.h`).
  if (!F->proj[pi].shared_header[0] && pi < F->nproj) {
    // Prefer the file that reads like the app's own front door (`src/app.h`, `src/shared.h`) over
    // whichever header a directory listing happened to produce first.
    for (i32 i = 0; i < F->nfile; i++) {
      if (F->file[i].proj != pi) continue;
      const char *p = F->file[i].path;
      const char *b = strrchr(p, '/');
      b = b ? b + 1 : p;
      if (!strcmp(b, "app.h") || !strcmp(b, "shared.h")) {
        snprintf(F->proj[pi].shared_header, sizeof F->proj[pi].shared_header, "%s", p);
        break;
      }
    }
  }
  if (!F->proj[pi].shared_header[0] && pi < F->nproj)
    for (i32 i = 0; i < F->nfile; i++) {
      if (F->file[i].proj != pi) continue;
      const char *p = F->file[i].path;
      size_t n = strlen(p);
      if (n < 3 || strcmp(p + n - 2, ".h")) continue;
      if (strstr(p, "/include/webc/")) continue;
      snprintf(F->proj[pi].shared_header, sizeof F->proj[pi].shared_header, "%s", p);
      break;
    }
  arena_destroy(a);
}

WFacts *facts_new(Arena *a, const char *root) {
  WFacts *F = (WFacts *)arena_zalloc(a, sizeof(WFacts));
  F->a = a;
  snprintf(F->root, sizeof F->root, "%s", root && root[0] ? root : ".");
  snprintf(F->name, sizeof F->name, "%s", "app");

  char cfg[1200], src[1200], fwinc[1200];
  snprintf(cfg, sizeof cfg, "%s/webc.json", F->root);
  snprintf(src, sizeof src, "%s/src", F->root);
  snprintf(fwinc, sizeof fwinc, "%s/include/webc", F->root);
  bool has_cfg = path_is_file(cfg);
  F->is_app = has_cfg || path_is_dir(src);

  index_framework(F);
  if (has_cfg) {
    load_project(F, F->root);
  } else if (path_is_dir(src)) {
    // No file sets: one tree, compiled both ways. A half rule cannot be proved for
    // such a project, so none is enforced — silence beats a wrong error.
    i32 pi = proj_add(F, F->root);
    WalkCtx w = {F, WH_SERVER, pi};
    walk(src, 0, walk_add, &w);
  }
  // The framework repository is a tree of projects: each example has its own
  // `webc.json` and its own halves, and a server macro in a shared example file is
  // exactly the mistake this server exists to find. Index them as the projects they
  // are, plus the suites (compiled natively, so nothing there is half-illegal).
  if (!has_cfg && path_is_dir(fwinc)) {
    char ex[1200];
    snprintf(ex, sizeof ex, "%s/examples", F->root);
    DIR *d = opendir(ex);
    if (d) {
      struct dirent *e;
      while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", ex, e->d_name);
        if (path_is_dir(p)) load_project(F, p);
      }
      closedir(d);
    }
    char tests[1200];
    snprintf(tests, sizeof tests, "%s/tests", F->root);
    i32 pi = proj_add(F, tests);
    WalkCtx t = {F, WH_SERVER, pi};
    walk(tests, 0, walk_add, &t);
  }

  // Index every file once, now that the set is complete: a `.h` that declares a view
  // and a `.c` that draws it have to both be in before either resolves.
  for (i32 i = 0; i < F->nfile; i++) {
    u32 mtime = 0;
    Arena *tmp = arena_new(64 * 1024);
    str text = slurp(tmp, F->file[i].path, &mtime);
    F->file[i].mtime = mtime;
    if (text.n) index_file(F, i, text);
    arena_destroy(tmp);
  }
  return F;
}

void facts_refresh(WFacts *F, const char *path, str text) {
  i32 fi = file_find(F, path);
  if (fi < 0) {
    // a buffer the project has never seen: it belongs to the project whose
    // directory it sits in, so its names resolve against that project's facts
    i32 guess = -1;
    for (i32 i = 0; i < F->nfile && guess < 0; i++) {
      char dir[1024];
      snprintf(dir, sizeof dir, "%s", F->file[i].path);
      char *slash = strrchr(dir, '/');
      if (!slash) continue;
      *slash = 0;
      if (!strncmp(path, dir, strlen(dir))) guess = F->file[i].proj;
    }
    fi = file_add(F, path, facts_half_of(path, -1, F), guess);
  }
  // Drop every fact this file owned, then re-read it. A per-file filter is enough:
  // the arrays are small and a stale fact is worse than a millisecond.
  i32 w = 0;
  for (i32 i = 0; i < F->nmod; i++)
    if (F->mod[i].file != fi) F->mod[w++] = F->mod[i];
  F->nmod = w;
  w = 0;
  for (i32 i = 0; i < F->ncall; i++)
    if (F->call[i].file != fi) F->call[w++] = F->call[i];
  F->ncall = w;
  w = 0;
  for (i32 i = 0; i < F->nroute; i++)
    if (F->route[i].file != fi) F->route[w++] = F->route[i];
  F->nroute = w;
  w = 0;
  for (i32 i = 0; i < F->nvec; i++)
    if (F->vec[i].file != fi) F->vec[w++] = F->vec[i];
  F->nvec = w;
  w = 0;
  for (i32 i = 0; i < F->nview; i++)
    if (F->view[i].file != fi) F->view[w++] = F->view[i];
  F->nview = w;
  w = 0;
  for (i32 i = 0; i < F->ndview; i++)
    if (F->dview[i].file != fi) F->dview[w++] = F->dview[i];
  F->ndview = w;
  w = 0;
  for (i32 i = 0; i < F->nact; i++)
    if (F->act[i].file != fi) F->act[w++] = F->act[i];
  F->nact = w;
  w = 0;
  for (i32 i = 0; i < F->nenum; i++)
    if (F->enumt[i].file != fi) F->enumt[w++] = F->enumt[i];
  F->nenum = w;
  F->file[fi].nmods = 0;
  index_file(F, fi, text);
}

// ---------------------------------------------------------------- lookups

WMod *facts_model(WFacts *F, const char *name, size_t n) {
  for (i32 i = 0; i < F->nmod; i++)
    if (strlen(F->mod[i].name) == n && !strncmp(F->mod[i].name, name, n)) return &F->mod[i];
  return nullptr;
}

WSym *facts_one(WSym *v, i32 n, const char *name, size_t n_name) {
  for (i32 i = 0; i < n; i++)
    if (strlen(v[i].name) == n_name && !strncmp(v[i].name, name, n_name)) return &v[i];
  return nullptr;
}

bool facts_has_view(WFacts *F, const char *name, size_t n) {
  return facts_one(F->view, F->nview, name, n) || facts_one(F->dview, F->ndview, name, n);
}
bool facts_has_action(WFacts *F, const char *name, size_t n) {
  return facts_one(F->act, F->nact, name, n) != nullptr;
}
bool facts_has_vec(WFacts *F, const char *name, size_t n) {
  return facts_one(F->vec, F->nvec, name, n) != nullptr;
}

bool facts_is_model(WFacts *F, const char *name, size_t n) {
  if (facts_model(F, name, n)) return true;
  for (i32 i = 0; i < F->nframe; i++)
    if (strlen(F->frame[i]) == n && !strncmp(F->frame[i], name, n)) return true;
  return false;
}

WRoute *facts_route_at(WFacts *F, i32 file, u32 off) {
  for (i32 i = 0; i < F->nroute; i++) {
    WRoute *r = &F->route[i];
    if (r->file == file && off >= r->off && off <= r->off + 4096) return r;
  }
  return nullptr;
}