// WebC — what the language server says.
//
// Every feature here is a function of one document's region map (dsl.h) and the
// project index (facts.h). Positions arrive as byte offsets and leave as JSON; the
// UTF-16 conversion is the transport's job and lives in exactly one place (lsp.c).
//
// The rule that keeps this honest: a diagnostic is only produced from a fact the
// index proved (a name that was read out of a declaration), never from a guess about
// what a string might mean.
#ifndef WEBC_TOOL_LSPX_H
#define WEBC_TOOL_LSPX_H

#include "facts.h"

typedef struct WDoc {
  char *path;      // as the editor named it, filesystem form
  char *uri;       // file:// form
  i32 version;
  str text;
  bool open;
  u32 mtime;
  Arena *a;        // owns text, tokens, scan; reset on every change
  Arena *da;       // owns the diagnostics last published: reset on every publish
  WSrc *src;
  WScan *scan;
  WDiags *diag;    // the diagnostics last published for this document
  bool slow;       // the compiler took longer than a keystroke's worth of time here
} WDoc;

typedef struct WLsp {
  Arena *a;        // the process's arena: the index and the documents live here
  Arena *req;      // per-request arena, reset at the start of every message
  WFacts *F;
  char root[1024];
  WDoc *doc;
  i32 ndoc, capdoc;
  Buf out;         // responses when capturing (--selftest / --replay)
  bool capture;
  FILE *trace;
  bool saving;     // this publish follows a save or an open, not a keystroke
  i64 t_start;
  // counters the selftest asserts on
  i32 nreq, nresp, ndiag;
} WLsp;

// ---- transport-owned plumbing
u32 lspx_off(WDoc *d, i32 line, i32 u16col);
i32 lspx_u16col(WDoc *d, u32 off);

// ---- features. Each appends one complete JSON value to `out`.
void lspx_diagnostics(WLsp *L, WDoc *d, Buf *out);            // the diagnostics array
void lspx_completion(WLsp *L, WDoc *d, u32 off, Buf *out);    // CompletionItem[]
void lspx_hover(WLsp *L, WDoc *d, u32 off, Buf *out);         // Hover | null
void lspx_definition(WLsp *L, WDoc *d, u32 off, Buf *out);    // Location[]
void lspx_references(WLsp *L, WDoc *d, u32 off, Buf *out);    // Location[]
void lspx_symbols(WLsp *L, WDoc *d, Buf *out);                // DocumentSymbol[]
void lspx_actions(WLsp *L, WDoc *d, u32 begin, u32 end, Buf *out); // CodeAction[]

// ---- document bookkeeping (implemented in lsp.c, used by both)
WDoc *lsp_doc_open(WLsp *L, const char *path, const char *uri, i32 version, str text);
WDoc *lsp_doc_find(WLsp *L, const char *uri);
void  lsp_doc_reindex(WLsp *L, WDoc *d);
// New text for a document: the old text lives in the arena that is about to be
// reset, so the copy happens first. This is the only path that may reset it.
void  lsp_doc_set_text(WDoc *d, str text);
char *lsp_path_of_uri(Arena *a, const char *uri);
char *lsp_uri_of_path(Arena *a, const char *path);

// JSON helpers shared with the transport
void wj_str(Buf *b, const char *s);
void wj_strn(Buf *b, const char *s, size_t n);
void wj_range(Buf *b, WDoc *d, u32 begin, u32 end);

#endif // WEBC_TOOL_LSPX_H