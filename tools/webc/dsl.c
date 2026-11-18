// SPDX-License-Identifier: Apache-2.0
// WebC — tokens, DSL regions, and the registry that names them.
//
// This file answers one question for every other tool: "what is at this byte?"
//   plain C, or a DSL construct, and if a construct: which macro, which argument,
//   and where does its block end.
//
// It never parses C. It tokenizes (strings, comments, preprocessor lines, #if 0
// regions), then matches `ident ( … ) [ { … } ]` against a table of macros that are
// known to take a block. That is enough to find every construct a template can be
// wrong about, and it is small enough to be obviously correct.
#include "dsl.h"

// ---------------------------------------------------------------- the vocabulary
//
// The spellings a model tuple accepts. They are `WC_KIND_<token>` in kinds.h, so the
// list is the compiler's own, and a 15th scalar cannot appear here without one there.

typedef struct WKindRow { const char *name; const char *doc; } WKindRow;
static const WKindRow KINDS[] = {
    {"i8", "an 8-bit signed integer"},
    {"i16", "a 16-bit signed integer"},
    {"i32", "a 32-bit signed integer"},
    {"i64", "a 64-bit signed integer; also what a primary key is"},
    {"u8", "an 8-bit unsigned integer"},
    {"u16", "a 16-bit unsigned integer"},
    {"u32", "a 32-bit unsigned integer"},
    {"u64", "a 64-bit unsigned integer"},
    {"f32", "a 32-bit float"},
    {"f64", "a 64-bit float"},
    {"bool", "a boolean: true/false on the wire, 0/1 in SQL"},
    {"str", "a (pointer, length) pair; not NUL-terminated for you"},
    {"Text", "a NUL-terminated string: what a model field holds"},
    {"Bytes", "a blob that may contain a NUL"},
};

// `WQual` in kinds.h, in its declaration order. `.min`/`.max` bound a *string's
// length*; a number uses `.gte`/`.lte` — a rule the compiler enforces and this table
// only repeats where a person is typing.
typedef struct WQualRow { const char *name; const char *doc; bool string_bound; } WQualRow;
static const WQualRow QUALS[] = {
    {"pk", "primary key: the row's id, and what db_save upserts on", false},
    {"not_null", "the column is NOT NULL", false},
    {"required", "must be present and non-empty, or the request fails 422", false},
    {"trim", "strings: trim whitespace before validation", false},
    {"unique", "the column is UNIQUE; a violation is a 409", false},
    {"default_false", "a bool that a form leaves out means false", false},
    {"no_json", "never serialised: secrets stay in", false},
    {"json_array", "Vec fields: an array of scalars on the wire", false},
    {"sensitive", "redacted by the logger", false},
    {"max", "string length ceiling (numbers use .lte)", true},
    {"min", "string length floor (numbers use .gte)", true},
    {"gte", "numeric lower bound", false},
    {"lte", "numeric upper bound", false},
    {"format", "\"email\", \"uuid\", \"url\" or \"date\": a named check", false},
    {"sql", "the column name, when it is not the field name", false},
    {"def", "a SQL default expression literal", false},
    {"doc", "documentation, surfaced by webc doctor", false},
};

// Elements a page is built from. `el()` takes any name (the renderer writes it
// through), so this is a vocabulary for completion, never a legality check.
static const char *TAGS[] = {
    "main", "header", "footer", "nav", "section", "article", "aside", "div", "span", "p",
    "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "dl", "dt", "dd",
    "a", "button", "label", "form", "input", "textarea", "select", "option", "fieldset", "legend",
    "table", "thead", "tbody", "tfoot", "tr", "th", "td", "caption", "colgroup",
    "strong", "em", "small", "code", "pre", "blockquote", "hr", "br", "img", "figure", "figcaption",
    "time", "progress", "meter", "dialog", "details", "summary", "template", "canvas", "video",
    "svg", "g", "path", "circle", "rect", "line", "text", "polyline", "polygon",
};

// The modules on the shelf (§12i), by header name. Five of them have no browser
// half at all, which is the one half-rule `webc lint` already enforces.
static const char *HEADERS[] = {
    "app", "auth", "chart", "client", "client_test", "config", "core", "crypto", "csv", "ctx",
    "db", "dnd", "dom", "fe", "form", "gzip", "host", "http", "i18n", "icons", "jobs", "js",
    "json", "keys", "kinds", "ld", "limits", "live", "log", "mail", "map", "model", "motion",
    "multipart", "overlay", "page", "pp", "pwa", "qcache", "react", "redis", "relations",
    "richtext", "search", "server", "shared", "table", "task", "test", "theme", "time", "tls", "ui",
    "upload", "url", "vec", "web", "webc",
};
static const char *SERVER_ONLY[] = {"upload", "mail", "jobs", "limits", "pwa"};

// ---------------------------------------------------------------- the registry
//
// One row per macro an editor has to reason about. `block` is the load-bearing
// column: it is what makes `view(N, T *c) { … }` a construct rather than a call.
// `server` is the half rule (a server-only macro in a shared source links for the
// server and vanishes for wasm). The doc lines are the headers' own words, shortened.
static const WMacro MACROS[] = {
    // ---- definitions (a type and a body)
    {"WEBC_MODEL", 2, 33, false, false, WK_DECL,
     "The model: the struct, the descriptor table and the SQL column types, from one "
     "declaration (§4). Fields are `(name, Kind, .pk = true)`."},
    {"WEBC_MODEL_TABLE", 2, 2, false, false, WK_DECL, "The table name, when it is not the plural."},
    {"WEBC_MODEL_INTERNAL", 1, 1, false, false, WK_DECL,
     "A row type with no table of its own (a request body): the schema check skips it."},
    {"WEBC_MODEL_ROUTE", 2, 2, false, false, WK_DECL, "`/a/:slug`: what link(Model, &row) is built from."},
    {"WEBC_ENUM", 2, 33, false, false, WK_DECL,
     "A closed set: an int in the struct, its name on the wire (§4)."},
    {"WEBC_MODEL_REF", 4, 4, false, false, WK_DECL, "A relation: one declaration, one query (§9a2)."},
    {"Vec", 1, 1, false, false, WK_DECL, "The collection for a model: `Vec(Todo)` is `VecTodo`."},
    {"WEBC_VEC_DEFINE", 3, 3, false, false, WK_DECL, "The same, when the name is not the convention."},
    {"declare_view", 1, 2, false, false, WK_DECL,
     "The view's prototype in a shared header: `view_<N>`, and `<N>_ctx` when it takes one."},
    {"ACTION", 1, 1, false, false, WK_DECL, "An action's prototype, for a test or another file."},
    {"view", 1, 2, true, false, WK_FN,
     "The body is the view; the `(WBuild *, void *)` signature and `view_<N>` are generated."},
    {"action", 1, 2, true, false, WK_FN,
     "An event handler whose context is typed; `bind(...)` and `on_click(...)` name it."},
    {"TEST", 1, 1, true, false, WK_FN, "One test; it runs inside a panic frame (§14b)."},
    {"computed", 2, 2, true, false, WK_FN, "A derived signal: the body is read in an effect."},
    {"effect", 1, 1, true, false, WK_FN, "A body that re-runs when what it read changes (§11a)."},
    {"computed_in", 2, 2, true, false, WK_FN, "The same, per instance."},
    {"effect_in", 1, 1, true, false, WK_FN, "The same, per instance."},
    {"computed_own", 2, 2, true, false, WK_FN, "Per-instance, owned by the component."},
    {"effect_own", 1, 1, true, false, WK_FN, "Per-instance, owned by the component."},
    {"effect_with", 2, 2, true, false, WK_FN, "An effect with a typed context."},
    {"computed_with", 2, 2, true, false, WK_FN, "A computed with a typed context."},
    {"state", 2, 3, false, false, WK_DECL, "`state(i64, count, 0)`: a signal with its own storage."},
    {"state_in", 2, 3, false, false, WK_DECL, "The same, per instance."},
    {"state_own", 2, 3, false, false, WK_DECL, "The same, owned by the component (§11c)."},

    // ---- routes (the server half)
    {"GET", 1, 3, true, true, WK_STMT, "A route: `GET(\"/:id\") { … }`. No main(), no middleware list (§7)."},
    {"POST", 1, 3, true, true, WK_STMT, "A route (§7)."},
    {"PUT", 1, 3, true, true, WK_STMT, "A route (§7)."},
    {"PATCH", 1, 3, true, true, WK_STMT, "A route (§7)."},
    {"DELETE", 1, 3, true, true, WK_STMT, "A route (§7)."},
    {"HEAD", 1, 3, true, true, WK_STMT, "A route (§7)."},
    {"OPTIONS", 1, 3, true, true, WK_STMT, "A route (§7)."},
    {"ANY", 1, 3, true, true, WK_STMT, "A route that answers every method (§7)."},
    {"GET_MW", 2, 4, true, true, WK_STMT, "A route with its own middleware (§8)."},
    {"POST_MW", 2, 4, true, true, WK_STMT, "A route with its own middleware (§8)."},
    {"group", 1, 1, true, true, WK_STMT, "A prefix for the routes inside the block, as a runtime scope."},
    {"tx_scope", 1, 1, true, true, WK_STMT, "BEGIN on this request's connection; COMMIT or roll back."},
    {"nocache", 0, 0, true, false, WK_STMT, "The block runs with the query cache off (§9d)."},

    // ---- markup
    {"el", 1, 1, true, false, WK_STMT, "Opens an element; the block is its children (§12)."},
    {"comp", 1, 8, true, false, WK_STMT,
     "A component instance: its own state, effects and lifecycle, children via WKids (§11d)."},
    {"for_each", 2, 3, true, false, WK_STMT, "The render loop: `for_each(t, (*v))` or keyed, 3 args."},
    {"for_each_ptr", 2, 3, true, false, WK_STMT, "The same, over pointers."},
    {"with_inst", 1, 1, true, false, WK_STMT, "Runs the block against one component instance."},
    {"mount", 1, 2, false, false, WK_STMT, "Mounts a view onto a selector: `mount(Todos, \"#root\")`."},

    // ---- values the DSL names
    {"text", 1, 8, false, false, WK_EXPR, "A value, or a format: the argument count decides (§12)."},
    {"page", 1, 8, false, true, WK_EXPR, "A rendered view as a response: `page(Todos, db_all(Todo))`."},
    {"render", 1, 8, false, false, WK_EXPR, "A view as a body, with the ctx filled by field."},
    {"json", 2, 2, false, false, WK_EXPR, "`json(200, v)`: the value serialised, the status set."},
    {"json_list", 2, 2, false, false, WK_EXPR,
     "`json_list(200, rows)`: a collection as JSON — the spelling the compiler checks."},
    {"body", 1, 1, false, true, WK_EXPR, "The request body validated into a model: `body(Todo)`."},
    {"body_try", 1, 1, false, true, WK_EXPR,
     "The same parse, the handler answers: `body_try(Todo, &t)` — a page can show the field."},
    {"body_json", 1, 1, false, true, WK_EXPR, "A JSON body, parsed and validated (§7)."},
    {"body_form", 1, 1, false, true, WK_EXPR, "A form body, parsed and validated (§7)."},
    {"db_all", 1, 3, false, true, WK_EXPR, "Every row of a model, into its Vec (§9)."},
    {"db_get", 2, 2, false, true, WK_EXPR, "One row by primary key, or null (§9)."},
    {"db_save", 2, 2, false, true, WK_EXPR, "Insert or update by primary key."},
    {"db_try_save", 2, 2, false, true, WK_EXPR, "The same, answering false on a unique violation."},
    {"db_delete", 2, 2, false, true, WK_EXPR, "Delete by primary key."},
    {"db_find", 3, 3, false, true, WK_EXPR, "One row from a where clause of your own (§9a)."},
    {"db_count", 1, 1, false, true, WK_EXPR, "How many rows the model has."},
    {"db_insert", 3, 3, false, true, WK_EXPR, "Insert through a handle you own."},
    {"db_update", 3, 3, false, true, WK_EXPR, "Update through a handle you own."},
    {"mail_view", 3, 4, false, true, WK_EXPR, "`mail_view(m, VIEW, ctx)`: the compiler checks the view name."},
    {"named", 1, 1, false, false, WK_EXPR, "Names a control so a bind can reach it."},
    {"named_field", 1, 1, false, false, WK_EXPR, "The same, for a field the host knows by name."},
    {"class_", 1, 1, false, false, WK_EXPR, "The element's class attribute: `class_(t->done ? \"done\" : \"\")`."},
    {"on_click", 1, 1, false, false, WK_EXPR, "Wires an action to this element's click."},
    {"bind", 3, 3, false, false, WK_EXPR, "`bind(\"hook\", \"click\", act)`: the client half's wiring."},
    {"text_live", 1, 2, false, false, WK_EXPR, "A node that rewrites itself in place (§12j)."},
    {"attr_live", 2, 3, false, false, WK_EXPR, "The same, for one attribute (§12j)."},
    {"class_live", 1, 2, false, false, WK_EXPR, "The same, for a class list (§12j)."},
    {"show_live", 1, 2, false, false, WK_EXPR, "The same, for visibility (§12j)."},
    {"action_live", 2, 2, false, false, WK_EXPR, "An event handled without re-rendering the view (§12j)."},
    {"path", 1, 1, false, true, WK_EXPR, "A path segment from the route's `{name}` (§7)."},
    {"path_i64", 1, 1, false, true, WK_EXPR, "The same, parsed as an integer."},
    {"query", 1, 1, false, true, WK_EXPR, "A query-string value (§7)."},
    {"js", 1, 8, false, false, WK_EXPR, "JavaScript, with typed arguments and a WJs result (§13c)."},
    {"kids", 1, 1, false, false, WK_EXPR, "The children a component was given (§11e)."},
    {"set", 2, 2, false, false, WK_EXPR, "Writes a signal."},
    {"get", 1, 1, false, false, WK_EXPR, "Reads a signal, and makes an effect depend on it."},
    {"toggle", 1, 1, false, false, WK_EXPR, "Flips a bool signal."},
};

static int macro_cmp(const void *a, const void *b) {
  return strcmp(((const WMacro *)a)->name, ((const WMacro *)b)->name);
}
// bsearch with a `const char *` key: the name being looked up arrives as bytes in a
// buffer, so the comparison has to be key-first.
static int macro_cmp_ptr(const void *key, const void *elem) {
  return strcmp((const char *)key, ((const WMacro *)elem)->name);
}

const WMacro *dsl_macro(const char *name, size_t n) {
  // A linear scan would be 70 strcmps per identifier; the table is sorted once and
  // bsearch'd instead, because the tokenizer calls this for every token.
  static WMacro *sorted;
  static i32 nsorted;
  if (!sorted) {
    nsorted = (i32)WC_COUNTOF(MACROS);
    sorted = (WMacro *)malloc(sizeof(MACROS));
    memcpy(sorted, MACROS, sizeof(MACROS));
    qsort(sorted, (size_t)nsorted, sizeof(WMacro), macro_cmp);
  }
  // A token's bytes are not NUL-terminated (they point into the file), and strcmp
  // would read past them into the next `(`. Copy into a small buffer first: every
  // macro name in the framework is far shorter than this.
  if (n == 0 || n >= 48) return nullptr;
  char key[48];
  memcpy(key, name, n);
  key[n] = 0;
  const WMacro *hit = bsearch(key, sorted, (size_t)nsorted, sizeof(WMacro), macro_cmp_ptr);
  if (!hit || strlen(hit->name) != n) return nullptr;
  return hit;
}

bool dsl_is_head(str name) { return dsl_macro(name.p, name.n) != nullptr; }
i32 dsl_macro_count(void) { return (i32)WC_COUNTOF(MACROS); }

i32 dsl_kind_count(void) { return (i32)WC_COUNTOF(KINDS); }
const char *dsl_kind_at(i32 i, const char **doc) {
  if (i < 0 || i >= (i32)WC_COUNTOF(KINDS)) return nullptr;
  if (doc) *doc = KINDS[i].doc;
  return KINDS[i].name;
}
i32 dsl_qual_count(void) { return (i32)WC_COUNTOF(QUALS); }
const char *dsl_qual_at(i32 i, const char **doc) {
  if (i < 0 || i >= (i32)WC_COUNTOF(QUALS)) return nullptr;
  if (doc) *doc = QUALS[i].doc;
  return QUALS[i].name;
}
bool dsl_qual_is_string_bound(const char *q, size_t n) {
  return (n == 3 && !strncmp(q, "min", 3)) || (n == 3 && !strncmp(q, "max", 3));
}
i32 dsl_tag_count(void) { return (i32)WC_COUNTOF(TAGS); }
const char *dsl_tag_at(i32 i) {
  return (i >= 0 && i < (i32)WC_COUNTOF(TAGS)) ? TAGS[i] : nullptr;
}
i32 dsl_header_count(void) { return (i32)WC_COUNTOF(HEADERS); }
const char *dsl_header_at(i32 i) {
  return (i >= 0 && i < (i32)WC_COUNTOF(HEADERS)) ? HEADERS[i] : nullptr;
}
bool dsl_header_is_server_only(const char *n, size_t len) {
  for (size_t i = 0; i < WC_COUNTOF(SERVER_ONLY); i++)
    if (strlen(SERVER_ONLY[i]) == len && !strncmp(SERVER_ONLY[i], n, len)) return true;
  return false;
}

// ---------------------------------------------------------------- tokenizer

static bool ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}
static bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }
static bool digit(char c) { return c >= '0' && c <= '9'; }

static void tok_add(WSrc *s, WTokKind kind, u32 off, u32 len, i32 line, i32 col, u8 pp) {
  if (s->ntok == s->cap) {
    i32 cap = s->cap ? s->cap * 2 : 256;
    WTok *n = (WTok *)realloc(s->tok, (size_t)cap * sizeof(WTok));
    if (!n) abort();
    s->tok = n;
    s->cap = cap;
  }
  s->tok[s->ntok++] = (WTok){.kind = (u8)kind, .pp = pp, .off = off, .len = len, .line = line,
                             .col = col};
}

// A preprocessor directive is a logical line: it runs until a newline that is not
// escaped. Its condition decides whether the tokens under it are dead (#if 0),
// conditional (any other #if) or live.
static u8 pp_after(u8 cur, const char *line, u32 n) {
  while (n && (*line == ' ' || *line == '\t')) line++, n--;
  if (n < 2 || line[0] != '#') return cur;
  const char *p = line + 1;
  u32 left = n - 1;
  while (left && (*p == ' ' || *p == '\t')) p++, left--;
#define HAS(lit) (left >= sizeof(lit) - 1 && !strncmp(p, lit, sizeof(lit) - 1))
  if (HAS("if 0")) return 1;
  if (HAS("if")) return cur ? cur : 2;
  if (HAS("else") || HAS("elif")) return cur ? cur : 0;
  if (HAS("endif")) return 0;
#undef HAS
  return cur;
}

WSrc *dsl_src(Arena *a, str text) {
  WSrc *s = (WSrc *)arena_zalloc(a, sizeof(WSrc));
  s->text = text.p;
  s->n = (u32)text.n;
  s->tok = (WTok *)realloc(nullptr, 256 * sizeof(WTok));
  s->cap = 256;

  // line index
  s->nline = 1;
  for (u32 i = 0; i < s->n; i++) s->nline += s->text[i] == '\n';
  s->line = (u32 *)arena_alloc(a, (size_t)s->nline * sizeof(u32));
  s->line[0] = 0;
  i32 ln = 1;
  for (u32 i = 0; i < s->n; i++)
    if (s->text[i] == '\n') s->line[ln++] = i + 1;

  const char *p = s->text;
  u32 i = 0;
  i32 line = 0, line_start = 0;
  u8 pp = 0;
  bool bol = true; // at the beginning of a line (only blanks so far)
  while (i < s->n) {
    char c = p[i];
    if (c == '\n') {
      bol = true;
      i++;
      line++;
      line_start = (i32)i;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
      i++;
      continue;
    }
    if (c == '\\' && i + 1 < s->n && p[i + 1] == '\n') {
      // a swallowed newline: the logical line continues
      i += 2;
      line++;
      line_start = (i32)i;
      continue;
    }
    if (c == '/' && i + 1 < s->n && p[i + 1] == '/') {
      while (i < s->n && p[i] != '\n') i++;
      continue; // a comment never becomes a token, so it can never wire anything
    }
    if (c == '/' && i + 1 < s->n && p[i + 1] == '*') {
      i += 2;
      while (i + 1 < s->n && !(p[i] == '*' && p[i + 1] == '/')) {
        if (p[i] == '\n') {
          line++;
          line_start = (i32)i + 1;
        }
        i++;
      }
      i = (i + 1 < s->n) ? i + 2 : s->n;
      continue;
    }
    u32 start = i;
    i32 start_col = (i32)i - line_start;
    if (c == '#' && bol) {
      // one directive, continuations included
      while (i < s->n) {
        if (p[i] == '\\' && i + 1 < s->n && p[i + 1] == '\n') {
          i += 2;
          line++;
          line_start = (i32)i;
          continue;
        }
        if (p[i] == '\n') break;
        if (p[i] == '/' && i + 1 < s->n && p[i + 1] == '*') { // a comment inside a directive
          i += 2;
          while (i + 1 < s->n && !(p[i] == '*' && p[i + 1] == '/')) i++;
          i = (i + 1 < s->n) ? i + 2 : s->n;
          continue;
        }
        i++;
      }
      pp = pp_after(pp, p + start, i - start);
      tok_add(s, WT_PP, start, i - start, line, start_col, pp);
      continue;
    }
    bol = false;
    if (c == '"' || c == '\'') {
      char q = c;
      i++;
      while (i < s->n) {
        if (p[i] == '\\' && i + 1 < s->n) {
          i += 2;
          continue;
        }
        if (p[i] == '\n') break; // unterminated: stop at the newline, do not eat the file
        if (p[i] == q) {
          i++;
          break;
        }
        i++;
      }
      tok_add(s, WT_STR, start, i - start, line, start_col, pp);
      continue;
    }
    if (ident_start(c)) {
      while (i < s->n && ident_char(p[i])) i++;
      tok_add(s, WT_IDENT, start, i - start, line, start_col, pp);
      continue;
    }
    if (digit(c) || (c == '.' && i + 1 < s->n && digit(p[i + 1]))) {
      while (i < s->n && (digit(p[i]) || p[i] == '.' || p[i] == 'x' || p[i] == 'X' ||
                          (p[i] >= 'a' && p[i] <= 'f') || (p[i] >= 'A' && p[i] <= 'F') ||
                          p[i] == '_'))
        i++;
      tok_add(s, WT_NUM, start, i - start, line, start_col, pp);
      continue;
    }
    i++;
    tok_add(s, WT_PUNCT, start, 1, line, start_col, pp);
  }
  return s;
}

void dsl_src_free(WSrc *s) {
  if (!s) return;
  free(s->tok);
  s->tok = nullptr;
  s->ntok = s->cap = 0;
}

void dsl_scan_free(WScan *sc) {
  if (!sc) return;
  free(sc->reg);
  sc->reg = nullptr;
  sc->n = sc->cap = 0;
}

void dsl_line_col(const WSrc *s, u32 off, i32 *line, i32 *col) {
  i32 lo = 0, hi = s->nline - 1;
  while (lo < hi) {
    i32 mid = (lo + hi + 1) / 2;
    if (s->line[mid] <= off) lo = mid;
    else hi = mid - 1;
  }
  if (line) *line = lo;
  if (col) *col = (i32)off - (i32)s->line[lo];
}
u32 dsl_line_off(const WSrc *s, i32 line) {
  if (line < 0) return 0;
  if (line >= s->nline) return s->n;
  return s->line[line];
}
i32 dsl_line_count(const WSrc *s) { return s->nline; }

const char *dsl_tok_text(const WSrc *s, const WTok *t) { return s->text + t->off; }
bool dsl_tok_is(const WSrc *s, const WTok *t, const char *lit) {
  size_t n = strlen(lit);
  return t->len == n && !strncmp(s->text + t->off, lit, n);
}

// ---------------------------------------------------------------- scanning

static bool punct_is(const WSrc *s, const WTok *t, char c) {
  return t->kind == WT_PUNCT && s->text[t->off] == c;
}

// The index of the token that closes `open_tok`'s bracket, or -1.
static i32 match_close(const WSrc *s, i32 open_i, char open_c, char close_c, i32 limit) {
  i32 depth = 0;
  for (i32 i = open_i; i < limit && i < s->ntok; i++) {
    const WTok *t = &s->tok[i];
    if (t->kind != WT_PUNCT) continue;
    char c = s->text[t->off];
    if (c == open_c) depth++;
    else if (c == close_c) {
      depth--;
      if (depth == 0) return i;
    }
  }
  return -1;
}

// The first token at or after a byte offset (tokens are in source order).
static i32 tok_at(const WSrc *s, u32 off) {
  i32 lo = 0, hi = s->ntok - 1, best = s->ntok;
  while (lo <= hi) {
    i32 mid = (lo + hi) / 2;
    if (s->tok[mid].off >= off) {
      best = mid;
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return best;
}

static void region_push(WScan *sc, WRegion r) {
  if (sc->n == sc->cap) {
    i32 cap = sc->cap ? sc->cap * 2 : 32;
    WRegion *n = (WRegion *)realloc(sc->reg, (size_t)cap * sizeof(WRegion));
    if (!n) abort();
    sc->reg = n;
    sc->cap = cap;
  }
  sc->reg[sc->n++] = r;
}

// Top-level comma split inside a byte range, using the token stream. Splitting on
// characters would be wrong the moment an argument holds a string with a comma in
// it or a nested `(a, b)` — which is every model in the framework.
i32 dsl_split(const WSrc *s, u32 begin, u32 end, WArg *out, i32 max) {
  i32 n = 0;
  i32 depth = 0;
  u32 part_begin = begin;
  bool any = false;
  for (i32 i = 0; i < s->ntok; i++) {
    const WTok *t = &s->tok[i];
    if (t->off < begin || t->off + t->len > end) continue;
    if (t->kind == WT_PUNCT) {
      char c = s->text[t->off];
      if (c == '(' || c == '[' || c == '{') depth++;
      else if (c == ')' || c == ']' || c == '}') depth--;
      else if (c == ',' && depth == 0) {
        if (n < max) out[n++] = (WArg){part_begin, t->off, false, false};
        part_begin = t->off + 1;
        any = true;
      }
      continue;
    }
    if (t->kind == WT_IDENT || t->kind == WT_STR || t->kind == WT_NUM) any = true;
  }
  if (any || n) {
    if (n < max) out[n++] = (WArg){part_begin, end, false, false};
  }
  // trim, and classify: a paren group, or a single string literal
  for (i32 k = 0; k < n; k++) {
    u32 b = out[k].begin, e = out[k].end;
    while (b < e && (s->text[b] == ' ' || s->text[b] == '\t' || s->text[b] == '\n' ||
                     s->text[b] == '\r'))
      b++;
    while (e > b && (s->text[e - 1] == ' ' || s->text[e - 1] == '\t' || s->text[e - 1] == '\n' ||
                     s->text[e - 1] == '\r'))
      e--;
    out[k].begin = b;
    out[k].end = e;
    out[k].is_paren = b < e && s->text[b] == '(' && e > b + 1 && s->text[e - 1] == ')';
    out[k].is_str = b < e && (s->text[b] == '"' || s->text[b] == '\'') && e > b + 1 &&
                    s->text[e - 1] == s->text[b];
  }
  return n;
}

static void scan_range(WScan *sc, i32 lo, i32 hi, i8 depth);

// The body of a matched region: recurse so `el("ul") { for_each(t, v) { el("li"){} } }`
// is three regions, not one.
static void scan_block(WScan *sc, i32 bopen_tok, i32 bclose_tok, i8 depth) {
  if (bclose_tok < 0) return;
  scan_range(sc, bopen_tok + 1, bclose_tok, (i8)(depth + 1));
}

static void scan_range(WScan *sc, i32 lo, i32 hi, i8 depth) {
  const WSrc *s = sc->src;
  if (hi > s->ntok) hi = s->ntok;
  for (i32 i = lo; i < hi; i++) {
    const WTok *t = &s->tok[i];
    if (t->kind != WT_IDENT) continue;
    const WMacro *m = dsl_macro(s->text + t->off, t->len);
    if (!m) continue;
    if (m->max_args == 0) {
      // `nocache { … }`: an object-like macro whose body is a for-loop
      if (!m->block) continue;
      i32 j = i + 1;
      if (j >= hi) continue;
      if (punct_is(s, &s->tok[j], '{')) {
        i32 close = match_close(s, j, '{', '}', hi);
        WRegion r = {.macro = m, .head = t->off, .head_len = t->len, .open = 0, .close = 0,
                     .nargs = 0, .has_block = true, .bopen = s->tok[j].off,
                     .bclose = close >= 0 ? s->tok[close].off : 0, .depth = depth,
                     .pp = t->pp, .pp_line = t->line};
        region_push(sc, r);
        scan_block(sc, j, close, depth);
      }
      continue;
    }
    if (i + 1 >= hi || !punct_is(s, &s->tok[i + 1], '(')) continue;
    i32 open_tok = i + 1;
    i32 close_tok = match_close(s, open_tok, '(', ')', hi);
    WRegion r = {.macro = m, .head = t->off, .head_len = t->len,
                 .open = s->tok[open_tok].off,
                 .close = close_tok >= 0 ? s->tok[close_tok].off : 0,
                 .depth = depth, .pp = t->pp, .pp_line = t->line};
    if (close_tok >= 0 && close_tok > open_tok + 1) {
      WArg parts[8];
      i32 n = dsl_split(s, s->tok[open_tok].off + 1, r.close, parts, 8);
      for (i32 k = 0; k < n; k++) r.arg[k] = parts[k];
      r.nargs = (i8)n;
    }
    i32 next = close_tok >= 0 ? close_tok + 1 : open_tok + 1;
    if (m->block && next < hi && punct_is(s, &s->tok[next], '{')) {
      i32 bclose = match_close(s, next, '{', '}', hi);
      r.has_block = true;
      r.bopen = s->tok[next].off;
      r.bclose = bclose >= 0 ? s->tok[bclose].off : 0;
      region_push(sc, r);
      // An argument can hold a construct: `page(Todos, db_all(Todo))` names a view
      // *and* a model, and `db_all` is the one that asks for a `Vec(Todo)`.
      for (i32 k = 0; k < r.nargs; k++)
        if (r.arg[k].is_paren || r.arg[k].begin < r.arg[k].end)
          scan_range(sc, tok_at(s, r.arg[k].begin), tok_at(s, r.arg[k].end), (i8)(depth + 1));
      scan_block(sc, next, bclose, depth);
      if (bclose >= 0) i = bclose;
      else i = next;
      continue;
    }
    region_push(sc, r);
    for (i32 k = 0; k < r.nargs; k++)
      if (r.arg[k].begin < r.arg[k].end)
        scan_range(sc, tok_at(s, r.arg[k].begin), tok_at(s, r.arg[k].end), (i8)(depth + 1));
    if (close_tok >= 0) i = close_tok;
  }
}

WScan *dsl_scan(Arena *a, const WSrc *src) {
  WScan *sc = (WScan *)arena_zalloc(a, sizeof(WScan));
  sc->src = src;
  sc->reg = (WRegion *)realloc(nullptr, 32 * sizeof(WRegion));
  sc->cap = 32;
  scan_range(sc, 0, src->ntok, 0);
  return sc;
}

const WRegion *dsl_at(const WScan *s, u32 off) {
  const WRegion *best = nullptr;
  for (i32 i = 0; i < s->n; i++) {
    const WRegion *r = &s->reg[i];
    u32 end = r->bclose ? r->bclose + 1 : (r->close ? r->close + 1 : r->head + r->head_len);
    if (off < r->head || off > end) continue;
    if (!best || r->depth >= best->depth) best = r;
  }
  return best;
}

const WRegion *dsl_in_block(const WScan *s, u32 off) {
  const WRegion *best = nullptr;
  for (i32 i = 0; i < s->n; i++) {
    const WRegion *r = &s->reg[i];
    if (!r->has_block || !r->bclose) continue;
    if (off <= r->bopen || off >= r->bclose) continue;
    if (!best || r->depth >= best->depth) best = r;
  }
  return best;
}

// ---------------------------------------------------------------- diagnostics list

WDiags *dsl_diags_new(Arena *a) {
  WDiags *l = (WDiags *)arena_zalloc(a, sizeof(WDiags));
  l->a = a;
  l->cap = 32;
  l->d = (WDiag *)arena_alloc(a, sizeof(WDiag) * (size_t)l->cap);
  return l;
}

void dsl_diag_add(WDiags *l, WDiag d) {
  for (i32 i = 0; i < l->n; i++) {
    WDiag *o = &l->d[i];
    if (o->begin == d.begin && o->end == d.end && o->code && d.code &&
        !strcmp(o->code, d.code))
      return; // same span, same rule: say it once
  }
  if (l->n == l->cap) {
    i32 cap = l->cap * 2;
    WDiag *n = (WDiag *)arena_alloc(l->a, sizeof(WDiag) * (size_t)cap);
    memcpy(n, l->d, sizeof(WDiag) * (size_t)l->n);
    l->d = n;
    l->cap = cap;
  }
  if (l->n < l->cap) l->d[l->n++] = d;
}

void dsl_diagf(WDiags *l, Arena *a, u32 begin, u32 end, WSev sev, const char *code,
               const char *fmt_, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt_);
  vsnprintf(buf, sizeof buf, fmt_, ap);
  va_end(ap);
  dsl_diag_add(l, (WDiag){.begin = begin, .end = end, .sev = sev, .code = code,
                          .msg = arena_strdup(a, buf)});
}

void dsl_diags_dedupe(WDiags *l) {
  for (i32 i = 0; i < l->n; i++)
    for (i32 j = l->n - 1; j > i; j--)
      if (l->d[i].begin == l->d[j].begin && l->d[i].end == l->d[j].end &&
          l->d[i].code && l->d[j].code && !strcmp(l->d[i].code, l->d[j].code))
        l->d[j] = l->d[--l->n];
}