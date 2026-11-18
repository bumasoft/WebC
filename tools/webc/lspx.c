// SPDX-License-Identifier: Apache-2.0
// WebC — the features: diagnostics, completion, hover, navigation, symbols.
//
// Everything here reads two things and nothing else: this document's region map, and
// the project index. No build, no compiler, no database, no socket — a keystroke is
// answered by code that already ran once.
#include "lspx.h"
#include "prefix.h"
#include "ceng.h"

#include <sys/stat.h>

// ---------------------------------------------------------------- JSON plumbing

void wj_strn(Buf *b, const char *s, size_t n) { json_escape_into(b, str_mem(s, n)); }
void wj_str(Buf *b, const char *s) { wj_strn(b, s ? s : "", s ? strlen(s) : 0); }

// LSP positions are UTF-16 code units by default, and this repository's own examples
// draw `✓ ○ ×` inside strings — so the conversion is not optional decoration. One
// function, used by every range this file writes.
i32 lspx_u16col(WDoc *d, u32 off) {
  u32 ls = dsl_line_off(d->src, 0);
  i32 line, col;
  dsl_line_col(d->src, off, &line, &col);
  (void)ls;
  ls = dsl_line_off(d->src, line);
  i32 u16 = 0;
  for (u32 i = ls; i < off && i < d->src->n; i++) {
    unsigned char c = (unsigned char)d->src->text[i];
    if ((c & 0xC0) != 0x80) u16 += (c >= 0xF0) ? 2 : 1; // a 4-byte codepoint is a surrogate pair
  }
  return u16;
}

u32 lspx_off(WDoc *d, i32 line, i32 u16col) {
  u32 start = dsl_line_off(d->src, line);
  u32 i = start;
  i32 u16 = 0;
  while (i < d->src->n && u16 < u16col) {
    unsigned char c = (unsigned char)d->src->text[i];
    if (c == '\n') break;
    i32 w = ((c & 0xC0) != 0x80) ? ((c >= 0xF0) ? 2 : 1) : 0;
    if (u16 + w > u16col) break;
    u16 += w;
    i++;
  }
  return i;
}

void wj_range(Buf *b, WDoc *d, u32 begin, u32 end) {
  i32 bl, bc, el, ec;
  dsl_line_col(d->src, begin, &bl, &bc);
  dsl_line_col(d->src, end, &el, &ec);
  buf_printf(b, "{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}",
             bl, lspx_u16col(d, begin), el, lspx_u16col(d, end));
}

static void wj_location(Buf *b, WLsp *L, WDoc *d, const char *path, i32 line, i32 col) {
  // A fact's position is (line, byte-column). For another file the byte column is
  // turned into UTF-16 by reading that line's bytes; for this document the same.
  char *uri = lsp_uri_of_path(L->a, path);
  buf_puts(b, "{\"uri\":");
  wj_str(b, uri);
  buf_puts(b, ",\"range\":{\"start\":{\"line\":");
  buf_printf(b, "%d,\"character\":%d},", line, col);
  buf_puts(b, "\"end\":{\"line\":");
  buf_printf(b, "%d,\"character\":%d}}}", line, col + 1);
  (void)d;
}

// ---------------------------------------------------------------- tokens

static const WTok *tok_at(const WSrc *s, u32 off) {
  i32 lo = 0, hi = s->ntok - 1;
  while (lo <= hi) {
    i32 mid = (lo + hi) / 2;
    const WTok *t = &s->tok[mid];
    if (off < t->off) hi = mid - 1;
    else if (off >= t->off + t->len) lo = mid + 1;
    else return t;
  }
  return nullptr;
}

// The span an item should replace when it is accepted: the identifier or string
// literal the caret is in, so `el("di` completes to `el("div"` and not `el("di"div`.
static void word_span(const WSrc *s, u32 off, u32 *begin, u32 *end) {
  const WTok *t = tok_at(s, off);
  if (t && (t->kind == WT_IDENT || t->kind == WT_STR)) {
    *begin = t->off;
    *end = t->off + t->len;
    return;
  }
  // the caret is between tokens: extend over the identifier characters around it
  u32 b = off, e = off;
  while (b > 0) {
    char c = s->text[b - 1];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
      break;
    b--;
  }
  while (e < s->n) {
    char c = s->text[e];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
      break;
    e++;
  }
  *begin = b;
  *end = e;
}

static i32 arg_at(const WRegion *r, u32 off) {
  if (r->nargs == 0) return 0;
  for (i32 k = 0; k < r->nargs; k++)
    if (off >= r->arg[k].begin && off <= r->arg[k].end) return k;
  if (off > r->open && off < r->arg[0].begin) return 0;
  for (i32 k = 1; k < r->nargs; k++)
    if (off > r->arg[k - 1].end && off < r->arg[k].begin) return k;
  if (off > r->arg[r->nargs - 1].end && off < (r->close ? r->close : r->arg[r->nargs - 1].end + 2))
    return r->nargs; // a new argument position
  return -1;
}

// ---------------------------------------------------------------- item emission

typedef struct WItem {
  const char *label;
  const char *detail;   // right-hand side in the editor's list
  const char *doc;      // the one-line documentation pane
  i32 kind;             // LSP CompletionItemKind
  const char *insert;   // null: the label
} WItem;

static void items_json(Buf *b, WDoc *d, WItem *items, i32 n, u32 rb, u32 re) {
  buf_putc(b, '[');
  for (i32 i = 0; i < n; i++) {
    if (i) buf_putc(b, ',');
    buf_puts(b, "{\"label\":");
    wj_str(b, items[i].label);
    buf_printf(b, ",\"kind\":%d", items[i].kind);
    if (items[i].detail && items[i].detail[0]) {
      buf_puts(b, ",\"detail\":");
      wj_str(b, items[i].detail);
    }
    if (items[i].doc && items[i].doc[0]) {
      buf_puts(b, ",\"documentation\":{\"kind\":\"markdown\",\"value\":");
      wj_str(b, items[i].doc);
      buf_putc(b, '}');
    }
    buf_puts(b, ",\"textEdit\":{\"range\":");
    wj_range(b, d, rb, re);
    buf_puts(b, ",\"newText\":");
    wj_str(b, items[i].insert ? items[i].insert : items[i].label);
    buf_puts(b, "}}");
  }
  buf_putc(b, ']');
}

// ---------------------------------------------------------------- completion

static i32 route_param_names(const WSrc *s, const WRegion *route, char names[16][48]) {
  if (!route || route->nargs == 0) return 0;
  u32 b, e;
  const WArg *a = &route->arg[0];
  if (!a->is_str) return 0;
  b = a->begin + 1;
  e = a->end - 1;
  i32 n = 0;
  for (u32 i = b; i < e && n < 16; i++) {
    char c = s->text[i];
    if (c != ':' && c != '{') continue;
    u32 j = i + 1, k = 0;
    while (j < e && k < 47) {
      char cc = s->text[j];
      if (!((cc >= 'a' && cc <= 'z') || (cc >= 'A' && cc <= 'Z') || (cc >= '0' && cc <= '9') ||
            cc == '_'))
        break;
      names[n][k++] = cc;
      j++;
    }
    names[n][k] = 0;
    if (k) n++;
    i = j;
  }
  return n;
}

static const WRegion *enclosing_route(const WScan *sc, u32 off) {
  const WRegion *best = nullptr;
  static const char *verbs[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS", "ANY",
                                "GET_MW", "POST_MW"};
  for (i32 i = 0; i < sc->n; i++) {
    const WRegion *r = &sc->reg[i];
    if (!r->has_block || !r->bclose) continue;
    if (off <= r->bopen || off >= r->bclose) continue;
    bool verb = false;
    for (size_t k = 0; k < WC_COUNTOF(verbs); k++)
      if (!strcmp(r->macro->name, verbs[k])) verb = true;
    if (verb && (!best || r->depth >= best->depth)) best = r;
  }
  return best;
}

void lspx_completion(WLsp *L, WDoc *d, u32 off, Buf *b) {
  const WSrc *s = d->src;
  u32 rb, re;
  word_span(s, off, &rb, &re);
  WItem items[96];
  i32 n = 0;
  Arena *a = L->req;

  // `#include <webc/…>`: the shelf, filtered by the half this file compiles for.
  const WTok *tk = tok_at(s, off);
  if (tk && tk->kind == WT_PP) {
    const char *p = s->text + tk->off;
    const char *lt = strstr(p, "<webc/");
    if (lt && lt < p + tk->len) {
      u32 inner_b = (u32)(lt - s->text) + 6;
      const char *gt = memchr(lt, '>', (size_t)(tk->off + tk->len - (u32)(lt - s->text)));
      u32 inner_e = gt ? (u32)(gt - s->text) : tk->off + tk->len;
      WHalf h = facts_half_of(d->path, -1, L->F);
      for (i32 i = 0; i < dsl_header_count() && n < 95; i++) {
        const char *mod = dsl_header_at(i);
        if (h != WH_SERVER && dsl_header_is_server_only(mod, strlen(mod))) continue;
        items[n++] = (WItem){.label = mod, .detail = "webc header", .kind = 9,
                             .doc = "a module on the shelf: unused costs nothing (§12i)"};
      }
      items_json(b, d, items, n, inner_b, inner_e);
      return;
    }
  }

  const WRegion *r = dsl_at(d->scan, off);
  if (!r) {
    buf_puts(b, "[]");
    return;
  }
  i32 arg = arg_at(r, off);
  const char *mn = r->macro->name;

  if (!strcmp(mn, "WEBC_MODEL") || !strcmp(mn, "WEBC_MODEL_TABLE")) {
    // Inside a field tuple `(name, Kind, .pk = true)`: which part is the caret in?
    for (i32 k = 1; k < r->nargs; k++) {
      const WArg *fa = &r->arg[k];
      if (!fa->is_paren) continue;
      if (off < fa->begin || off > fa->end) continue;
      WArg parts[4];
      i32 np = dsl_split(s, fa->begin + 1, fa->end - 1, parts, 4);
      i32 pi = -1;
      for (i32 q = 0; q < np; q++)
        if (off >= parts[q].begin - 1 && off <= parts[q].end + 1) pi = q;
      if (pi < 0) pi = np; // a new part: a qualifier next
      u32 pb, pe;
      word_span(s, off, &pb, &pe);
      if (pi == 1) {
        for (i32 i = 0; i < dsl_kind_count() && n < 95; i++) {
          const char *doc = "";
          const char *kname = dsl_kind_at(i, &doc);
          items[n++] = (WItem){.label = kname, .detail = "model field kind", .doc = doc, .kind = 14};
        }
        for (i32 i = 0; i < L->F->nmod && n < 95; i++)
          items[n++] = (WItem){.label = L->F->mod[i].name, .detail = "model", .kind = 7,
                               .doc = facts_model_detail(L->F, &L->F->mod[i])};
        for (i32 i = 0; i < L->F->nenum && n < 95; i++)
          items[n++] = (WItem){.label = L->F->enumt[i].name, .detail = "enum", .kind = 13};
      } else if (pi >= 2) {
        for (i32 i = 0; i < dsl_qual_count() && n < 95; i++) {
          const char *doc = "";
          const char *qn = dsl_qual_at(i, &doc);
          const char *ins = fmt_s(".%s = ", qn).p;
          items[n++] = (WItem){.label = qn, .detail = "field qualifier", .doc = doc, .kind = 14,
                               .insert = str_cstr(a, str_lit(ins))};
        }
      }
      items_json(b, d, items, n, pb, pe);
      return;
    }
    buf_puts(b, "[]");
    return;
  }

  if (!strcmp(mn, "el")) {
    for (i32 i = 0; i < dsl_tag_count() && n < 95; i++) {
      const char *tag = dsl_tag_at(i);
      const char *ins = fmt_s("\"%s\"", tag).p;
      items[n++] = (WItem){.label = tag, .detail = "element", .kind = 14,
                           .insert = str_cstr(a, str_lit(ins))};
    }
    items_json(b, d, items, n, rb, re);
    return;
  }

  if (!strcmp(mn, "page") || !strcmp(mn, "render") || !strcmp(mn, "mail_view") ||
      !strcmp(mn, "declare_view") || !strcmp(mn, "mount")) {
    if (arg == (!strcmp(mn, "mail_view") ? 1 : 0)) {
      for (i32 i = 0; i < L->F->nview && n < 95; i++)
        items[n++] = (WItem){.label = L->F->view[i].name, .detail = L->F->view[i].detail
                                                                     ? L->F->view[i].detail
                                                                     : "view",
                             .kind = 12,
                             .doc = facts_file_path(L->F, L->F->view[i].file)};
      items_json(b, d, items, n, rb, re);
      return;
    }
    buf_puts(b, "[]");
    return;
  }

  bool model_ctx = !strcmp(mn, "db_all") || !strcmp(mn, "db_all_ordered") ||
                   !strcmp(mn, "db_all_where") || !strcmp(mn, "db_where") ||
                   !strcmp(mn, "db_insert") || !strcmp(mn, "db_update") ||
                   !strcmp(mn, "db_delete") || !strcmp(mn, "db_get") || !strcmp(mn, "db_save") ||
                   !strcmp(mn, "db_try_save") || !strcmp(mn, "db_find") || !strcmp(mn, "db_count") ||
                   !strcmp(mn, "body") || !strcmp(mn, "Vec") || !strcmp(mn, "WEBC_MODEL_INTERNAL");
  if (model_ctx) {
    for (i32 i = 0; i < L->F->nmod && n < 95; i++)
      items[n++] = (WItem){.label = L->F->mod[i].name, .detail = "model", .kind = 7,
                           .doc = facts_model_detail(L->F, &L->F->mod[i])};
    for (i32 i = 0; i < L->F->nframe && n < 95; i++)
      items[n++] = (WItem){.label = L->F->frame[i], .detail = "framework row type", .kind = 7};
    items_json(b, d, items, n, rb, re);
    return;
  }

  if (!strcmp(mn, "on_click") || (!strcmp(mn, "bind") && arg == 2)) {
    for (i32 i = 0; i < L->F->nact && n < 95; i++)
      items[n++] = (WItem){.label = L->F->act[i].name, .detail = "action", .kind = 12,
                           .doc = facts_file_path(L->F, L->F->act[i].file)};
    items_json(b, d, items, n, rb, re);
    return;
  }

  if ((!strcmp(mn, "bind") && arg == 0) || !strcmp(mn, "named")) {
    // hook names: what a `named(...)` declares, and what a `bind(...)` reaches
    for (i32 i = 0; i < L->F->ncall && n < 95; i++)
      if (!strcmp(L->F->call[i].macro, "named"))
        items[n++] = (WItem){.label = L->F->call[i].name, .detail = "hook",
                             .kind = 12, .doc = "a control this view names"};
    items_json(b, d, items, n, rb, re);
    return;
  }

  if (!strcmp(mn, "path") || !strcmp(mn, "path_i64") || !strcmp(mn, "query")) {
    char names[16][48];
    i32 np = route_param_names(s, enclosing_route(d->scan, off), names);
    for (i32 i = 0; i < np; i++) {
      const char *ins = fmt_s("\"%s\"", names[i]).p;
      items[n++] = (WItem){.label = names[i], .detail = "route parameter", .kind = 12,
                           .insert = str_cstr(a, str_lit(ins))};
    }
    items_json(b, d, items, n, rb, re);
    return;
  }

  buf_puts(b, "[]");
}

// ---------------------------------------------------------------- hover

static void hover_md(Buf *b, const char *md) {
  buf_puts(b, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
  wj_str(b, md);
  buf_puts(b, "}}");
}

// The name a caret is on, plus the region it is in, if any.
typedef struct WHit {
  const WTok *tok;
  const WRegion *reg;
  bool is_head;   // the caret is on the macro name itself
  bool in_str;    // the token is a string literal
} WHit;

static WHit hit(WDoc *d, u32 off) {
  WHit h = {0};
  h.tok = tok_at(d->src, off);
  h.reg = dsl_at(d->scan, off);
  if (h.tok && h.reg && h.reg->head == h.tok->off && h.reg->head_len == h.tok->len) h.is_head = true;
  if (h.tok && h.tok->kind == WT_STR) h.in_str = true;
  return h;
}

void lspx_hover(WLsp *L, WDoc *d, u32 off, Buf *b) {
  WHit h = hit(d, off);
  Buf md;
  buf_init(&md, L->req, 512);

  if (h.is_head) {
    const WMacro *m = h.reg->macro;
    buf_printf(&md, "**`%s`** — %s\n\n", m->name, m->doc[0] ? m->doc : "(no doc line in the header)");
    buf_printf(&md, "`%s%s%s` · %s", m->min_args == m->max_args ? "" : "",
               m->block ? "… { }" : "…", "", m->server ? "server half" : "both halves");
    hover_md(b, buf_cstr(&md));
    buf_free(&md);
    return;
  }

  if (h.tok && h.tok->kind == WT_IDENT) {
    const char *name = d->src->text + h.tok->off;
    size_t n = h.tok->len;
    WMod *m = facts_model(L->F, name, n);
    if (m) {
      buf_printf(&md, "**model `%s`**\n\n```\n%s\n```\n\n", m->name, facts_model_detail(L->F, m));
      if (m->nf) {
        buf_puts(&md, "fields: ");
        for (i32 i = 0; i < m->nf; i++) {
          buf_printf(&md, "`%s`%s", m->f[i].name, i + 1 < m->nf ? ", " : "");
        }
        buf_putc(&md, '\n');
      }
      buf_printf(&md, "\ndeclared in `%s:%d`", facts_file_path(L->F, m->file), m->line + 1);
      hover_md(b, buf_cstr(&md));
      buf_free(&md);
      return;
    }
    WSym *v = facts_one(L->F->view, L->F->nview, name, n);
    if (v) {
      buf_printf(&md, "**view `%s`**%s%s\n\n`%s:%d`\n", v->name,
                 v->detail ? " — " : "", v->detail ? v->detail : "",
                 facts_file_path(L->F, v->file), v->line + 1);
      i32 cnt = 0;
      for (i32 i = 0; i < L->F->ncall; i++)
        if (!strcmp(L->F->call[i].macro, "page") || !strcmp(L->F->call[i].macro, "render")) {
          if (!strcmp(L->F->call[i].name, v->name)) cnt++;
        }
      buf_printf(&md, "\nrendered from %d route%s", cnt, cnt == 1 ? "" : "s");
      hover_md(b, buf_cstr(&md));
      buf_free(&md);
      return;
    }
    WSym *act = facts_one(L->F->act, L->F->nact, name, n);
    if (act) {
      buf_printf(&md, "**action `%s`**%s%s\n\n`%s:%d`\n", act->name,
                 act->detail ? " — " : "", act->detail ? act->detail : "",
                 facts_file_path(L->F, act->file), act->line + 1);
      hover_md(b, buf_cstr(&md));
      buf_free(&md);
      return;
    }
    WSym *en = facts_one(L->F->enumt, L->F->nenum, name, n);
    if (en) {
      buf_printf(&md, "**enum `%s`** — its name on the wire, an int in the struct\n\n`%s:%d`\n",
                 en->name, facts_file_path(L->F, en->file), en->line + 1);
      hover_md(b, buf_cstr(&md));
      buf_free(&md);
      return;
    }
  }

  if (h.in_str && h.reg) {
    const char *mn = h.reg->macro->name;
    if (!strcmp(mn, "named")) {
      u32 b2 = h.tok->off + 1, e2 = h.tok->off + h.tok->len - 1;
      buf_printf(&md, "**hook `%.*s`** — named here; `bind(\"…\")` in the client half reaches it",
                 (int)(e2 - b2), d->src->text + b2);
      hover_md(b, buf_cstr(&md));
      buf_free(&md);
      return;
    }
    if (!strcmp(mn, "GET") || !strcmp(mn, "POST") || !strcmp(mn, "PUT") || !strcmp(mn, "PATCH") ||
        !strcmp(mn, "DELETE")) {
      buf_printf(&md, "**route** `%s %.*s`\n\nhandler at this file, line %d",
                 mn, (int)(h.tok->len - 2), d->src->text + h.tok->off + 1, h.tok->line + 1);
      hover_md(b, buf_cstr(&md));
      buf_free(&md);
      return;
    }
  }
  buf_free(&md);
  buf_puts(b, "null");
}

// ---------------------------------------------------------------- definition

// Resolve the name under the caret to a declaration. Returns false when the caret is
// not on something that has one (a route path, a plain expression).
static bool resolve(WLsp *L, WDoc *d, u32 off, const char **path, i32 *line, i32 *col,
                    const char **kind) {
  WHit h = hit(d, off);
  if (!h.tok) return false;
  if (h.is_head) return false;
  if (h.tok->kind == WT_STR && h.reg && !strcmp(h.reg->macro->name, "named")) {
    u32 b2 = h.tok->off + 1, e2 = h.tok->off + h.tok->len - 1;
    for (i32 i = 0; i < L->F->ncall; i++) {
      WCall *c = &L->F->call[i];
      if (strcmp(c->macro, "bind-hook")) continue;
      if (strlen(c->name) != e2 - b2 || strncmp(c->name, d->src->text + b2, e2 - b2)) continue;
      *path = facts_file_path(L->F, c->file);
      *line = c->line;
      *col = c->col;
      *kind = "hook";
      return true;
    }
    return false;
  }
  if (h.tok->kind == WT_STR && h.reg && !strcmp(h.reg->macro->name, "bind")) {
    u32 b2 = h.tok->off + 1, e2 = h.tok->off + h.tok->len - 1;
    for (i32 i = 0; i < L->F->ncall; i++) {
      WCall *c = &L->F->call[i];
      if (strcmp(c->macro, "named")) continue;
      if (strlen(c->name) != e2 - b2 || strncmp(c->name, d->src->text + b2, e2 - b2)) continue;
      *path = facts_file_path(L->F, c->file);
      *line = c->line;
      *col = c->col;
      *kind = "hook";
      return true;
    }
    return false;
  }
  if (h.tok->kind != WT_IDENT) return false;
  const char *name = d->src->text + h.tok->off;
  size_t n = h.tok->len;
  WMod *m = facts_model(L->F, name, n);
  if (m) {
    *path = facts_file_path(L->F, m->file);
    *line = m->line;
    *col = m->col;
    *kind = "model";
    return true;
  }
  WSym *v = facts_one(L->F->view, L->F->nview, name, n);
  if (v) {
    *path = facts_file_path(L->F, v->file);
    *line = v->line;
    *col = v->col;
    *kind = "view";
    return true;
  }
  WSym *dv = facts_one(L->F->dview, L->F->ndview, name, n);
  if (dv) {
    *path = facts_file_path(L->F, dv->file);
    *line = dv->line;
    *col = dv->col;
    *kind = "declare_view";
    return true;
  }
  WSym *a = facts_one(L->F->act, L->F->nact, name, n);
  if (a) {
    *path = facts_file_path(L->F, a->file);
    *line = a->line;
    *col = a->col;
    *kind = "action";
    return true;
  }
  WSym *e = facts_one(L->F->enumt, L->F->nenum, name, n);
  if (e) {
    *path = facts_file_path(L->F, e->file);
    *line = e->line;
    *col = e->col;
    *kind = "enum";
    return true;
  }
  return false;
}

void lspx_definition(WLsp *L, WDoc *d, u32 off, Buf *b) {
  const char *path = nullptr;
  i32 line = 0, col = 0;
  const char *kind = nullptr;
  if (!resolve(L, d, off, &path, &line, &col, &kind)) {
    buf_puts(b, "[]");
    return;
  }
  buf_putc(b, '[');
  wj_location(b, L, d, path, line, col);
  buf_putc(b, ']');
}

void lspx_references(WLsp *L, WDoc *d, u32 off, Buf *b) {
  WHit h = hit(d, off);
  i32 n = 0;
  buf_putc(b, '[');
  if (!h.tok || h.tok->kind != WT_IDENT) {
    buf_putc(b, ']');
    return;
  }
  const char *name = d->src->text + h.tok->off;
  size_t nlen = h.tok->len;
#define EMIT(path_, line_, col_)                                                               \
  do {                                                                                         \
    if (n++) buf_putc(b, ',');                                                                 \
    wj_location(b, L, d, path_, line_, col_);                                                   \
  } while (0)
  // every DSL call site that named it, plus its own declaration
  for (i32 i = 0; i < L->F->ncall; i++) {
    WCall *c = &L->F->call[i];
    if (strlen(c->name) != nlen || strncmp(c->name, name, nlen)) continue;
    EMIT(facts_file_path(L->F, c->file), c->line, c->col);
  }
  for (i32 i = 0; i < L->F->nview; i++)
    if (strlen(L->F->view[i].name) == nlen && !strncmp(L->F->view[i].name, name, nlen))
      EMIT(facts_file_path(L->F, L->F->view[i].file), L->F->view[i].line, L->F->view[i].col);
  // a `declare_view` is a mention of the same name — it is the sentence the shared
  // header uses to promise the view exists, so it belongs in the list
  for (i32 i = 0; i < L->F->ndview; i++)
    if (strlen(L->F->dview[i].name) == nlen && !strncmp(L->F->dview[i].name, name, nlen))
      EMIT(facts_file_path(L->F, L->F->dview[i].file), L->F->dview[i].line, L->F->dview[i].col);
  for (i32 i = 0; i < L->F->nact; i++)
    if (strlen(L->F->act[i].name) == nlen && !strncmp(L->F->act[i].name, name, nlen))
      EMIT(facts_file_path(L->F, L->F->act[i].file), L->F->act[i].line, L->F->act[i].col);
  for (i32 i = 0; i < L->F->nmod; i++)
    if (strlen(L->F->mod[i].name) == nlen && !strncmp(L->F->mod[i].name, name, nlen))
      EMIT(facts_file_path(L->F, L->F->mod[i].file), L->F->mod[i].line, L->F->mod[i].col);
#undef EMIT
  buf_putc(b, ']');
}

// ---------------------------------------------------------------- symbols

static void sym_begin(Buf *b, WDoc *d, const char *name, i32 kind, u32 begin, u32 end) {
  buf_puts(b, "{\"name\":");
  wj_str(b, name);
  buf_printf(b, ",\"kind\":%d,\"range\":", kind);
  wj_range(b, d, begin, end);
  buf_puts(b, ",\"selectionRange\":");
  wj_range(b, d, begin, end);
  buf_puts(b, ",\"children\":[");
}
static void sym_end(Buf *b) { buf_puts(b, "]}"); }
static void sym_sep(Buf *b, bool *first) {
  if (!*first) buf_putc(b, ',');
  *first = false;
}

void lspx_symbols(WLsp *L, WDoc *d, Buf *b) {
  const WSrc *s = d->src;
  bool first = true;
  buf_putc(b, '[');
  for (i32 i = 0; i < L->F->nmod; i++) {
    WMod *m = &L->F->mod[i];
    // only the models declared in this document: an outline is about this file
    if (strcmp(facts_file_path(L->F, m->file), d->path)) continue;
    sym_sep(b, &first);
    sym_begin(b, d, m->name, 23, m->off, m->off + m->len);
    for (i32 k = 0; k < m->nf; k++) {
      if (k) buf_putc(b, ',');
      buf_puts(b, "{\"name\":");
      wj_str(b, m->f[k].name);
      buf_puts(b, ",\"detail\":");
      wj_str(b, m->f[k].quals ? m->f[k].quals : "");
      buf_printf(b, ",\"kind\":8,\"range\":");
      wj_range(b, d, m->f[k].off, m->f[k].off + m->f[k].len);
      buf_puts(b, ",\"selectionRange\":");
      wj_range(b, d, m->f[k].off, m->f[k].off + m->f[k].len);
      buf_puts(b, ",\"children\":[]}");
    }
    sym_end(b);
  }
  struct { WSym *v; i32 n; i32 kind; } groups[] = {{L->F->view, L->F->nview, 12},
                                                   {L->F->act, L->F->nact, 12}};
  for (size_t g = 0; g < 2; g++)
    for (i32 i = 0; i < groups[g].n; i++) {
      WSym *v = &groups[g].v[i];
      if (strcmp(facts_file_path(L->F, v->file), d->path)) continue;
      sym_sep(b, &first);
      sym_begin(b, d, v->name, groups[g].kind, v->off, v->off + v->len);
      sym_end(b);
    }
  for (i32 i = 0; i < L->F->nroute; i++) {
    WRoute *r = &L->F->route[i];
    if (strcmp(facts_file_path(L->F, r->file), d->path)) continue;
    char label[320];
    snprintf(label, sizeof label, "%s %s%s%s", r->method, r->path, r->view ? "  →  " : "",
             r->view ? r->view : "");
    sym_sep(b, &first);
    sym_begin(b, d, label, 24, r->off, r->off + r->len);
    sym_end(b);
  }
  // Plain C functions: a `{` whose `(` is preceded by an identifier that is not a DSL
  // head and not a control keyword. Conservative on purpose: a missing outline entry
  // is a shrug, a wrong one is a lie.
  for (i32 i = 1; i < s->ntok; i++) {
    const WTok *t = &s->tok[i];
    if (t->kind != WT_PUNCT || s->text[t->off] != '{') continue;
    i32 j = i - 1;
    if (j < 0 || s->tok[j].kind != WT_PUNCT || s->text[s->tok[j].off] != ')') continue;
    if (dsl_at(d->scan, t->off)) continue;
    i32 depth = 0, open_tok = -1;
    for (i32 k = j; k >= 0; k--) {
      const WTok *q = &s->tok[k];
      if (q->kind != WT_PUNCT) continue;
      if (s->text[q->off] == ')') depth++;
      else if (s->text[q->off] == '(') {
        depth--;
        if (depth == 0) {
          open_tok = k;
          break;
        }
      }
    }
    if (open_tok < 1) continue;
    const WTok *nm = &s->tok[open_tok - 1];
    if (nm->kind != WT_IDENT) continue;
    if (dsl_macro(s->text + nm->off, nm->len)) continue;
    if (dsl_tok_is(s, nm, "if") || dsl_tok_is(s, nm, "for") || dsl_tok_is(s, nm, "while") ||
        dsl_tok_is(s, nm, "switch") || dsl_tok_is(s, nm, "sizeof"))
      continue;
    if (dsl_at(d->scan, nm->off)) continue;
    sym_sep(b, &first);
    char label[128];
    snprintf(label, sizeof label, "%.*s()", (int)nm->len, s->text + nm->off);
    sym_begin(b, d, label, 12, nm->off, nm->off + nm->len);
    sym_end(b);
  }
  buf_putc(b, ']');
}

// A path a person reads: relative to the project root when it is inside it.
static const char *rel_path(WLsp *L, const char *path) {
  size_t r = strlen(L->F->root);
  if (r && !strncmp(path, L->F->root, r) && path[r] == '/') return path + r + 1;
  return path;
}

// ---------------------------------------------------------------- diagnostics

// A model in the same project as this file, or `Model` when the project declares none:
// the sentence an agent reads should name a type it can actually reach for.
static const char *diag_nearby_model(WLsp *L, WDoc *d) {
  i32 fi = -1;
  for (i32 i = 0; i < L->F->nfile; i++)
    if (!strcmp(L->F->file[i].path, d->path)) fi = i;
  i32 proj = fi >= 0 ? L->F->file[fi].proj : -1;
  for (i32 m = 0; m < L->F->nmod; m++) {
    i32 mf = L->F->mod[m].file;
    if (proj < 0 || (mf >= 0 && mf < L->F->nfile && L->F->file[mf].proj == proj))
      return L->F->mod[m].name;
  }
  return "Model";
}

static void diag_local(WLsp *L, WDoc *d, WDiags *out) {
  Arena *a = L->req;
  const WSrc *s = d->src;
  for (i32 i = 0; i < d->scan->n; i++) {
    const WRegion *r = &d->scan->reg[i];
    const char *mn = r->macro->name;
    if (r->pp != 0) continue; // dead (#if 0) or conditional: not this half's business
    if (r->close == 0) {
      dsl_diagf(out, a, r->head, r->head + r->head_len, WSEV_ERROR, "unclosed",
                "`%s(` is never closed — a macro call needs its `)` before the block", mn);
      continue;
    }
    if (r->has_block && !r->bclose) {
      u32 line_end = dsl_line_off(s, r->pp_line + 1);
      char *fix = arena_strdup(a, "}");
      dsl_diag_add(out, (WDiag){.begin = r->head,
                                .end = r->head + r->head_len,
                                .sev = WSEV_ERROR,
                                .code = "unclosed-block",
                                .msg = arena_strdup(
                                    a, fmt_s("`%s(…) {` has no closing `}` — the block runs to "
                                             "the end of the file", mn).p),
                                .fix_title = arena_strdup(a, "Close the block"),
                                .fix_begin = line_end,
                                .fix_end = line_end,
                                .fix_text = fix});
      continue;
    }
    if (!strcmp(mn, "el") && r->nargs > 0 && !r->arg[0].is_str) {
      dsl_diagf(out, a, r->arg[0].begin, r->arg[0].end, WSEV_ERROR, "el-tag",
                "`el()` takes the tag as a string literal: `el(\"div\") { … }`");
      continue;
    }
    if ((!strcmp(mn, "WEBC_MODEL") || !strcmp(mn, "WEBC_MODEL_TABLE"))) {
      for (i32 k = 1; k < r->nargs; k++) {
        const WArg *fa = &r->arg[k];
        if (!fa->is_paren) {
          if (fa->end > fa->begin)
            dsl_diagf(out, a, fa->begin, fa->end, WSEV_ERROR, "model-field",
                      "a field is a tuple: `(%.*s, i64, .pk = true)`",
                      (int)(fa->end - fa->begin), s->text + fa->begin);
          continue;
        }
        WArg parts[4];
        i32 np = dsl_split(s, fa->begin + 1, fa->end - 1, parts, 4);
        if (np < 2) {
          dsl_diagf(out, a, fa->begin, fa->end, WSEV_ERROR, "model-field",
                    "a field needs a name and a kind: `(title, Text)`");
          continue;
        }
        const WTok *kt = nullptr, *nt = nullptr;
        for (i32 q = 0; q < np; q++) {
          const WTok *t1 = nullptr;
          for (i32 t = 0; t < s->ntok; t++) {
            const WTok *tk = &s->tok[t];
            if (tk->off < parts[q].begin) continue;
            if (tk->off >= parts[q].end) break;
            if (tk->kind == WT_IDENT) {
              t1 = tk;
              break;
            }
          }
          if (q == 0) nt = t1;
          if (q == 1) kt = t1;
        }
        if (!kt) continue;
        bool ok = false;
        for (i32 kk = 0; kk < dsl_kind_count() && !ok; kk++) {
          const char *kn = dsl_kind_at(kk, nullptr);
          ok = strlen(kn) == kt->len && !strncmp(kn, s->text + kt->off, kt->len);
        }
        if (!ok && facts_is_model(L->F, s->text + kt->off, kt->len)) ok = true;
        if (!ok)
          for (i32 e = 0; e < L->F->nenum && !ok; e++)
            ok = strlen(L->F->enumt[e].name) == kt->len &&
                 !strncmp(L->F->enumt[e].name, s->text + kt->off, kt->len);
        if (!ok) {
          Buf names;
          buf_init(&names, a, 256);
          for (i32 kk = 0; kk < dsl_kind_count(); kk++)
            buf_printf(&names, "%s%s", kk ? ", " : "", dsl_kind_at(kk, nullptr));
          dsl_diagf(out, a, kt->off, kt->off + kt->len, WSEV_ERROR, "field-kind",
                    "`%.*s` is not a field kind — a field holds one of %s, or a model or enum "
                    "declared in this project",
                    (int)kt->len, s->text + kt->off, buf_cstr(&names));
        }
        (void)nt;
      }
    }
  }
}

void lspx_diagnostics(WLsp *L, WDoc *d, Buf *b) {
  // The list outlives this message: `textDocument/codeAction` reads it later, and a
  // use-after-free there would be a crash in the editor, not a wrong answer.
  if (!d->da) d->da = arena_new(16 * 1024);
  arena_reset(d->da);
  d->diag = dsl_diags_new(d->da);
  WDiags *out = d->diag;
  Arena *a = d->da;
  const WSrc *s = d->src;
  diag_local(L, d, out);

  i32 fi = -1;
  for (i32 i = 0; i < L->F->nfile; i++)
    if (!strcmp(L->F->file[i].path, d->path)) fi = i;
  WHalf half = facts_half_of(d->path, fi, L->F);

  // `#include <webc/mail.h>` in a shared or client source: a module with no browser
  // half, which as a wasm link failure names the app and not the include.
  if (fi >= 0)
    for (i32 m = 0; m < L->F->file[fi].nmods; m++) {
      const char *mod = L->F->file[fi].mods[m];
      if (half == WH_SERVER || !dsl_header_is_server_only(mod, strlen(mod))) continue;
      for (i32 t = 0; t < s->ntok; t++) {
        const WTok *tk = &s->tok[t];
        if (tk->kind != WT_PP) continue;
        const char *p = s->text + tk->off;
        const char *hitp = strstr(p, mod);
        if (!hitp || hitp >= p + tk->len) continue;
        dsl_diagf(out, a, tk->off, tk->off + tk->len, WSEV_ERROR, "half-module",
                  "`webc/%s` has no browser half — this file is in the %s set, which compiles "
                  "for wasm too; move the call into a `server` source",
                  mod, half == WH_CLIENT ? "client" : "shared");
        break;
      }
    }

  for (i32 i = 0; i < d->scan->n; i++) {
    const WRegion *r = &d->scan->reg[i];
    const char *mn = r->macro->name;
    if (r->pp != 0) continue;
    if (r->macro->server && half != WH_SERVER) {
      // A route moves; everything else needs its data carried across the seam, which is
      // the distinction the reader has to act on and the reason the second sentence is
      // not the same sentence for both.
      dsl_diagf(out, a, r->head, r->head + r->head_len, WSEV_ERROR, "half",
                "`%s()` is a server-half macro: this file is in the %s set, which compiles for "
                "the browser too. %s",
                mn, half == WH_CLIENT ? "client" : "shared",
                r->macro->block
                    ? "Routes belong in a `server` source"
                    : "Move the call into a `server` source and pass the data in through the "
                      "view's context");
      continue;
    }
    // `json(200, "{\"a\":1}")`: a JSON body written by hand, as a C string. clang
    // answers it with `passing 'char[9]' to parameter of incompatible type 'str'`
    // inside three frames of WC_CAT, and no app author has ever wanted that sentence.
    // The mistake is one mistake and its fix is one fix, so it is named here, where the
    // region map already knows the call is `json` and the argument is a literal.
    if (!strcmp(mn, "json")) {
      for (i8 ai = 1; ai < r->nargs; ai++) {
        if (!r->arg[ai].is_str) continue;
        u32 b = r->arg[ai].begin;
        while (b < r->arg[ai].end && s->text[b] != '"') b++;
        u32 c = b + 1;
        if (c < r->arg[ai].end && (s->text[c] == '{' || s->text[c] == '[')) {
          // The project's own type, not an example type: a sentence that says `Todo` in
          // an app about notes sends the reader looking for a model that is not there.
          const char *model = diag_nearby_model(L, d);
          dsl_diagf(out, a, r->arg[ai].begin, r->arg[ai].end, WSEV_ERROR, "json-literal",
                    "a JSON body written by hand — `json(200, s)` takes a `str`: pass the "
                    "model (`json(200, %s, &t)`), the collection (`json_list(200, rows)`), or "
                    "the bytes as `str_lit(\"...\")`. A JSON string literal is never what "
                    "this framework asks an app to write",
                    model);
          break;
        }
      }
    }
    if (!strcmp(mn, "page") || !strcmp(mn, "render") || !strcmp(mn, "mail_view")) {
      i32 ai = !strcmp(mn, "mail_view") ? 1 : 0;
      if (ai >= r->nargs) continue;
      for (i32 t = 0; t < s->ntok; t++) {
        const WTok *tk = &s->tok[t];
        if (tk->off < r->arg[ai].begin || tk->off >= r->arg[ai].end) continue;
        if (tk->kind != WT_IDENT) continue;
        if (facts_has_view(L->F, s->text + tk->off, tk->len)) break;
        char *name = arena_strdup(a, fmt_s("%.*s", (int)tk->len, s->text + tk->off).p);
        WDiag dg = {.begin = tk->off,
                    .end = tk->off + tk->len,
                    .sev = WSEV_ERROR,
                    .code = "no-view",
                    .msg = arena_strdup(
                        a, fmt_s("no view named `%s` in this project — `view(%s)` draws it, or "
                                 "`declare_view(%s)` in the shared header announces it",
                                 name, name, name).p)};
        const char *hdr = facts_shared_header(L->F, fi);
        if (hdr[0]) {
          // the fix a person would make: one line in the header both halves include
          u32 end = 0;
          FILE *f = fopen(hdr, "rb");
          if (f) {
            fseek(f, 0, SEEK_END);
            end = (u32)ftell(f);
            fclose(f);
          }
          dg.fix_title = arena_strdup(a, fmt_s("Add `declare_view(%s)` to the shared header", name).p);
          dg.fix_file = hdr;
          dg.fix_begin = end;
          dg.fix_end = end;
          dg.fix_text = arena_strdup(a, fmt_s("\ndeclare_view(%s);\n", name).p);
        }
        dsl_diag_add(out, dg);
        break;
      }
      continue;
    }
    if (!strcmp(mn, "db_all") || !strcmp(mn, "db_all_ordered") || !strcmp(mn, "db_all_where") ||
        !strcmp(mn, "db_where") || !strcmp(mn, "db_insert") || !strcmp(mn, "db_update") ||
        !strcmp(mn, "db_delete") || !strcmp(mn, "db_get") || !strcmp(mn, "db_save") ||
        !strcmp(mn, "db_try_save") || !strcmp(mn, "db_find") || !strcmp(mn, "db_count") ||
        !strcmp(mn, "body")) {
      i32 ai = !strcmp(mn, "db_all")                                  ? r->nargs - 1
               : (!strcmp(mn, "db_all_ordered") || !strcmp(mn, "db_all_where") ||
                  !strcmp(mn, "db_where") || !strcmp(mn, "db_insert") || !strcmp(mn, "db_update"))
                   ? (r->nargs > 1 ? 1 : 0)
               : (!strcmp(mn, "db_delete") && r->nargs >= 3) ? 1
                                                             : 0;
      if (ai < 0 || ai >= r->nargs) continue;
      const WTok *tk = nullptr;
      for (i32 t = 0; t < s->ntok; t++) {
        const WTok *q = &s->tok[t];
        if (q->off < r->arg[ai].begin) continue;
        if (q->off >= r->arg[ai].end) break;
        if (q->kind == WT_IDENT) {
          tk = q;
          break;
        }
      }
      if (!tk) continue;
      if (!facts_is_model(L->F, s->text + tk->off, tk->len)) {
        dsl_diagf(out, a, tk->off, tk->off + tk->len, WSEV_ERROR, "not-a-model",
                  "`%.*s` is not a model: `%s` needs the type a `WEBC_MODEL(…)` declares",
                  (int)tk->len, s->text + tk->off, mn);
        continue;
      }
      if (!strcmp(mn, "db_all") && !facts_has_vec(L->F, s->text + tk->off, tk->len)) {
        dsl_diagf(out, a, tk->off, tk->off + tk->len, WSEV_ERROR, "no-vec",
                  "`db_all(%.*s)` needs the collection: add `Vec(%.*s)` where the model is "
                  "declared, and `db_all` will hand back a `Vec%.*s`",
                  (int)tk->len, s->text + tk->off, (int)tk->len, s->text + tk->off,
                  (int)tk->len, s->text + tk->off);
      }
      continue;
    }
    if (!strcmp(mn, "on_click") || (!strcmp(mn, "bind") && r->nargs >= 3)) {
      i32 ai = !strcmp(mn, "bind") ? 2 : 0;
      for (i32 t = 0; t < s->ntok; t++) {
        const WTok *tk = &s->tok[t];
        if (tk->off < r->arg[ai].begin || tk->off >= r->arg[ai].end) continue;
        if (tk->kind != WT_IDENT) continue;
        if (!facts_has_action(L->F, s->text + tk->off, tk->len))
          dsl_diagf(out, a, tk->off, tk->off + tk->len, WSEV_ERROR, "no-action",
                    "no `action(%.*s, …)` in this project — a wiring to a name nothing "
                    "declares cannot fire",
                    (int)tk->len, s->text + tk->off);
        break;
      }
      continue;
    }
  }

  // A view drawn twice is a link error with two good names in it; a route declared
  // twice is a silent shadow.
  for (i32 i = 0; i < L->F->nview; i++) {
    WSym *v = &L->F->view[i];
    for (i32 k = i + 1; k < L->F->nview; k++) {
      if (strcmp(v->name, L->F->view[k].name)) continue;
      if (strcmp(facts_file_path(L->F, L->F->view[k].file), d->path)) continue;
      // only one project's names can collide: two examples are two programs, and a
      // view in a suite and a view in an example never meet in a link
      i32 pa = facts_proj_of(L->F, v->file), pb = facts_proj_of(L->F, L->F->view[k].file);
      if (pa < 0 || pa != pb) continue;
      dsl_diagf(out, a, L->F->view[k].off, L->F->view[k].off + L->F->view[k].len, WSEV_ERROR,
                "dup-view", "`%s` is already drawn in `%s:%d` — one name, one view", v->name,
                rel_path(L, facts_file_path(L->F, v->file)), v->line + 1);
    }
  }
  for (i32 i = 0; i < L->F->nroute; i++) {
    WRoute *r = &L->F->route[i];
    for (i32 k = i + 1; k < L->F->nroute; k++) {
      WRoute *q = &L->F->route[k];
      if (q->file != r->file || strcmp(r->method, q->method) || strcmp(r->path, q->path)) continue;
      if (strcmp(facts_file_path(L->F, q->file), d->path)) continue;
      if (facts_proj_of(L->F, r->file) != facts_proj_of(L->F, q->file)) continue;
      dsl_diagf(out, a, q->off, q->off + q->len, WSEV_WARN, "dup-route",
                "`%s %s` is declared twice — the second registration never answers", q->method,
                q->path);
    }
  }

  // The second engine: plain C is clang's to judge, and it judges the buffer the
  // editor is holding. It sorts itself out last because it must see the WebC engine's
  // verdict first — a file whose DSL is already wrong is a file a macro is going to
  // rewrite, and the compiler's sentences about it would be about the rewrite.
  ceng_check(L, d, out);
  d->slow = ceng_last_ms() > 120;
  dsl_diags_dedupe(out);
  // Publish, and keep the list for code actions.
  L->ndiag += out->n;
  buf_putc(b, '[');
  for (i32 i = 0; i < out->n; i++) {
    WDiag *g = &out->d[i];
    if (i) buf_putc(b, ',');
    buf_puts(b, "{\"range\":");
    wj_range(b, d, g->begin, g->end);
    buf_printf(b, ",\"severity\":%d,\"code\":", (i32)g->sev);
    wj_str(b, g->code);
    buf_puts(b, ",\"source\":\"webc\",\"message\":");
    wj_str(b, g->msg);
    buf_putc(b, '}');
  }
  buf_putc(b, ']');
}

// ---------------------------------------------------------------- code actions

void lspx_actions(WLsp *L, WDoc *d, u32 begin, u32 end, Buf *b) {
  buf_putc(b, '[');
  i32 n = 0;
  if (d->diag)
    for (i32 i = 0; i < d->diag->n; i++) {
      WDiag *g = &d->diag->d[i];
      if (!g->fix_title || g->end < begin || g->begin > end) continue;
      char *uri = g->fix_file ? lsp_uri_of_path(L->a, g->fix_file) : d->uri;
      if (n++) buf_putc(b, ',');
      buf_puts(b, "{\"title\":");
      wj_str(b, g->fix_title);
      buf_puts(b, ",\"kind\":\"quickfix\",\"diagnostics\":[{\"range\":");
      wj_range(b, d, g->begin, g->end);
      buf_printf(b, ",\"severity\":%d,\"code\":", (i32)g->sev);
      wj_str(b, g->code);
      buf_puts(b, ",\"source\":\"webc\",\"message\":");
      wj_str(b, g->msg);
      buf_puts(b, "}],\"edit\":{\"changes\":{");
      wj_str(b, uri);
      buf_puts(b, ":[{\"range\":");
      if (g->fix_file) {
        // a position in another file: line/col of the end of the header
        buf_printf(b, "{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,"
                      "\"character\":0}}");
      } else {
        wj_range(b, d, g->fix_begin, g->fix_end);
      }
      buf_puts(b, ",\"newText\":");
      wj_str(b, g->fix_text ? g->fix_text : "");
      buf_puts(b, "}]}}}}");
    }
  buf_putc(b, ']');
}
