// WebC — the tool layer's shared vocabulary: tokens, DSL regions, diagnostics.
//
// The language is C23 plus a macro DSL, and the macros expand to real C, so a C
// compiler already type-checks a WebC file (measured: 60–135 ms for a whole view).
// What it cannot do is *talk* about it: `page(Nope, …)` comes back as
// "use of undeclared identifier 'view_Nope'" behind five frames of WC_CAT.
//
// This header is the other half of that sentence — a tokenizer that does not care
// about the C grammar, the registry of macros that take a following `{ }`, and the
// scan that turns a file into a list of DSL regions. It is shared by `webc lsp`,
// `webc lint`, `webc routes` and `webc check --wiring`, so there is one answer to
// "what does this file say", not four scans that disagree.
//
// Nothing here allocates outside the arena it is handed, and nothing here ever
// writes to the text it is given.
#ifndef WEBC_TOOL_DSL_H
#define WEBC_TOOL_DSL_H

#include <webc/core.h>

// ---------------------------------------------------------------- aspects

// Which half of a project a file compiles for. `shared` compiles for both, so a
// shared file is a client file as far as legality goes: `GET()` in a shared source
// links for the server and vanishes for wasm, which is why the three sets are a
// compiler-enforced thing.
typedef enum WHalf {
  WH_SERVER = 0, // only the server half: everything is legal
  WH_SHARED = 1, // both halves: no server-only macro, no server-only module
  WH_CLIENT = 2, // the browser half: same, without a server binary to fall back on
} WHalf;

typedef enum WMacroKind {
  WK_STMT, // a statement-shaped macro: `el("x") { … }`, `GET("/") { … }`
  WK_FN,   // a definition: `view(N, …) { … }`, `action(N, …) { … }`
  WK_EXPR, // a value: `text("x")`, `db_all(db, T)`
  WK_DECL, // a declaration: `declare_view(N)`, `Vec(T)`
} WMacroKind;

typedef struct WMacro {
  const char *name;
  u8 min_args, max_args;
  bool block;      // the call may be followed by a `{ … }` body
  bool server;     // server half only (an include of it in a shared file is an error)
  WMacroKind kind;
  const char *doc; // one line, from the header comment; "" when the header has none
} WMacro;

// The registry. `dsl_macro(name)` is an exact, case-sensitive lookup over a static
// table, which is also what makes "is this identifier a DSL head?" a cheap question.
const WMacro *dsl_macro(const char *name, size_t n);
bool dsl_is_head(str name);
i32 dsl_macro_count(void);

// The two vocabularies a model tuple is written in (kinds.h): what a field holds,
// and what `.pk = true` may say. Both are ordered as the header declares them.
i32 dsl_kind_count(void);
const char *dsl_kind_at(i32 i, const char **doc);   // "i64", "the field holds…"
i32 dsl_qual_count(void);
const char *dsl_qual_at(i32 i, const char **doc);   // "pk", "primary key"
bool dsl_qual_is_string_bound(const char *q, size_t n); // .min/.max: strings only

// Tag names `el()` accepts, and the HTML-ish vocabulary docs/§12 documents.
i32 dsl_tag_count(void);
const char *dsl_tag_at(i32 i);

// The stdlib headers under include/webc, and the five with no browser half.
i32 dsl_header_count(void);
const char *dsl_header_at(i32 i);              // "dom", "table", …
bool dsl_header_is_server_only(const char *n, size_t len);

// ---------------------------------------------------------------- tokens

typedef enum WTokKind : u8 {
  WT_IDENT,
  WT_STR,   // "…" or '…', quotes included in the span
  WT_NUM,
  WT_PUNCT, // one byte; multi-byte operators are separate tokens on purpose
  WT_PP,    // a whole preprocessor logical line, `\` continuations included
} WTokKind;

typedef struct WTok {
  u8 kind;
  u8 pp;      // 0 live, 1 dead (#if 0), 2 conditional (any other #if)
  u32 off, len;
  i32 line, col; // 0-based
} WTok;

typedef struct WSrc {
  const char *text;
  u32 n;
  WTok *tok;
  i32 ntok, cap;
  u32 *line;   // byte offset of each line start
  i32 nline;
} WSrc;

// Tokenize. Tolerant by construction: an unterminated string or comment ends at
// the end of the file and is a token, not a panic. `#if 0` regions are marked dead
// and `#if defined(__wasm__)` regions conditional, so a client file never screams
// about a server macro that is not there.
WSrc *dsl_src(Arena *a, str text);

// A scanner owns two heap arrays (tokens, regions). Free them before the arena that
// holds the text goes back, or a keystroke leaks a few kilobytes (measured: 7 KB for
// a 450-token file, once per edit).
void dsl_src_free(WSrc *s);

// Offset ↔ position. `col` is a byte column; the LSP's UTF-16 conversion is the
// caller's business and lives in exactly one function there (see lsp.c:to_pos).
void dsl_line_col(const WSrc *s, u32 off, i32 *line, i32 *col);
u32  dsl_line_off(const WSrc *s, i32 line);
i32  dsl_line_count(const WSrc *s);
const char *dsl_tok_text(const WSrc *s, const WTok *t); // not NUL-terminated
bool dsl_tok_is(const WSrc *s, const WTok *t, const char *lit);

// ---------------------------------------------------------------- regions

typedef struct WArg {
  u32 begin, end; // trimmed byte range, inside the parens
  bool is_paren;  // the argument was itself a `( … )` group
  bool is_str;    // the argument was one string literal
} WArg;

typedef struct WRegion {
  const WMacro *macro;
  u32 head, head_len;   // the macro name
  u32 open, close;      // the parens (close == 0 when unclosed)
  i8 nargs;
  WArg arg[8];
  bool has_block;
  u32 bopen, bclose;    // the braces (bclose == 0 when unclosed)
  i8 depth;             // nesting: 0 = top level of the file
  u8 pp;                // the pp state where the head sits
  i32 pp_line;          // line of the head, for diagnostics
} WRegion;

typedef struct WScan {
  const WSrc *src;
  WRegion *reg;         // in source order
  i32 n, cap;
} WScan;

WScan *dsl_scan(Arena *a, const WSrc *src);
void dsl_scan_free(WScan *sc);

// The innermost region containing `off` (head, arguments or block), or null.
const WRegion *dsl_at(const WScan *s, u32 off);
// The innermost region whose *block* contains `off` (so `el("x") { | }` answers the
// el), or null. `dsl_at` includes the head; this does not.
const WRegion *dsl_in_block(const WScan *s, u32 off);

// Split a `( … )` group into its top-level comma-separated parts. Used for model
// fields, which are `(name, Kind, .pk = true)` inside a macro argument list.
// Returns the number of parts written, and always writes at most `max`.
i32 dsl_split(const WSrc *s, u32 begin, u32 end, WArg *out, i32 max);

// ---------------------------------------------------------------- diagnostics

typedef enum WSev { WSEV_ERROR = 1, WSEV_WARN = 2, WSEV_INFO = 3, WSEV_HINT = 4 } WSev;

typedef struct WDiag {
  u32 begin, end;      // byte range in the *file the diag belongs to*
  WSev sev;
  const char *code;    // "half", "no-view", "unclosed", … — stable, testable
  const char *msg;     // one sentence about WebC, not about C macros
  // An optional edit the editor may apply. Empty when there is no honest fix.
  const char *fix_title;
  const char *fix_file;   // null: this document; else another file's path (a header)
  u32 fix_begin, fix_end;
  const char *fix_text;
  // The C compiler's own sentence, when this diag supersedes one (related
  // information in the LSP, never a second diagnostic on the same span).
  const char *related;
} WDiag;

typedef struct WDiags {
  WDiag *d;
  i32 n, cap;
  Arena *a;
} WDiags;

WDiags *dsl_diags_new(Arena *a);
void dsl_diag_add(WDiags *l, WDiag d);
void dsl_diagf(WDiags *l, Arena *a, u32 begin, u32 end, WSev sev, const char *code,
               const char *fmt, ...) __attribute__((format(printf, 7, 8)));
// Drop diagnostics that cover the same span with the same code, keeping the first.
void dsl_diags_dedupe(WDiags *l);

static inline void dsl_diags_free(WDiags *l) { (void)l; } // arena-owned

#endif // WEBC_TOOL_DSL_H