// WebC — what a project says, gathered once.
//
// The index is the answer to the three questions no C compiler will answer: which
// models, views, actions and routes exist; which half of the app each file compiles
// for; and what the wiring names.
//
// It is built from the sources alone — nothing under `.webc/` is read, so a fresh
// clone is a working workspace. Every fact carries the file and position it came
// from, because a fact that cannot be navigated to is a rumour.
#ifndef WEBC_TOOL_FACTS_H
#define WEBC_TOOL_FACTS_H

#include <webc/json.h>

#include "dsl.h"

typedef struct WSym {
  char *name;
  i32 file;    // index into WFacts.file
  u32 off, len;
  i32 line, col; // 0-based, the name token
  // a view's or a model's extra shape, when the DSL declared one
  char *detail;  // "VecTodo *todos" for a view, the field list for a model
} WSym;

typedef struct WFld {
  char *name;
  char *kind;      // the spelling in the tuple: "i64", "Text", or a model's name
  char *quals;     // ".pk = true, .max = 200", as written
  u32 off, len;
  i32 line, col;
  bool pk, required, unique, not_null;
} WFld;

typedef struct WMod {
  char *name;
  bool internal;
  i32 file;
  u32 off, len;
  i32 line, col;
  i32 nf;
  WFld *f;         // nf entries, in declaration order
} WMod;

// One DSL call site that *names* something: `page(V)`, `db_all(T)`, `body(T)`,
// `on_click(act)`, `bind("hook", …)`, `named("hook")`, `Vec(T)`, `declare_view(V)`.
typedef struct WCall {
  char *macro;   // the DSL macro that named it
  char *name;    // what it named
  i32 arg;       // which argument the name came from (0-based)
  i32 status;    // `json(201, …)`: the status literal, 0 when the call names none
  bool list;     // json_list, or json over a collection: the wire shape is an array
  i32 file;
  u32 off, len;      // the name token (the caret's target)
  u32 call_off, call_len; // the whole call, for a diagnostic range
  i32 line, col;
} WCall;

typedef struct WRoute {
  char *method;
  char *path;    // as written, `{id}` placeholders included
  char *view;    // the view its block returns, when exactly one is named
  i32 file;
  u32 off, len;
  i32 line, col;
} WRoute;

// One project inside the workspace. The framework repository is a tree of them
// (`examples/*`), and facts never cross the line: a view named `Probe` in a suite and
// a view named `Probe` in an example are not the same name, and a quick fix that adds
// `declare_view(Probe)` must land in the header of the project that asked.
typedef struct WProj {
  char root[1024];
  char shared_header[1024]; // where declare_view belongs, "" when there is none
  char name[64];
} WProj;

typedef struct WFile {
  char *path;
  WHalf half;
  u32 mtime;         // seconds; 0 when the file has never been read
  char *mods[16];    // `#include <webc/x.h>` modules, in order
  i32 nmods;
  bool indexed;
  i32 proj;          // index into WFacts.proj; -1 when the file belongs to none
} WFile;

typedef struct WFacts {
  Arena *a;
  char root[1024];
  char name[64];
  bool is_app;          // a webc.json or a src/ entry: a project, not the framework
  bool has_sets;        // webc.json named server/shared/client, so halves are known

  WProj proj[32]; // a framework workspace is a tree of projects; 32 is the tree
  i32 nproj;

  WFile *file;
  i32 nfile, capfile;

  WMod *mod;   i32 nmod, capmod;
  WSym *view;  i32 nview, capview;   // view(N, …)
  WSym *dview; i32 ndview, capdview; // declare_view(N, …)
  WSym *enumt; i32 nenum, capenum;   // WEBC_ENUM(N, …)
  WSym *vec;   i32 nvec, capvec;      // Vec(T) / WEBC_VEC_DEFINE(VecT, …, T)
  WSym *act;   i32 nact, capact;      // action(N, …)
  WCall *call; i32 ncall, capcall;
  WRoute *route; i32 nroute, caproute;

  // The framework's own row types (RowId, WJob), read from WEBC_PREFIX/include/webc
  // so a project mentioning `body(RowId)` is not reported as naming nothing.
  char **frame; i32 nframe, capframe;
} WFacts;

// Build the index for the project rooted at `root`. Reads webc.json, walks the file
// sets, indexes every `.c`, `.h`. Returns null only if the arena itself fails.
WFacts *facts_new(Arena *a, const char *root);

// Re-index one file from text that may not be on disk yet (an unsaved editor
// buffer). `path` must be one the index already knows, or a new file that belongs
// to the project; the half is inherited from webc.json's sets.
void facts_refresh(WFacts *F, const char *path, str text);

// Lookups. `n` is the name length, so a token never has to be copied to be asked
// about. All return the *first* declaration, which is the one a caret should go to.
WMod *facts_model(WFacts *F, const char *name, size_t n);
WSym *facts_one(WSym *v, i32 n, const char *name, size_t n_name);
bool  facts_has_view(WFacts *F, const char *name, size_t n);   // view() or declare_view()
bool  facts_has_action(WFacts *F, const char *name, size_t n);
bool  facts_has_vec(WFacts *F, const char *name, size_t n);    // a Vec for this model
bool  facts_is_model(WFacts *F, const char *name, size_t n);   // project or framework

// "Todo  id: i64 pk, title: Text, done: bool" — the one-line shape a hover shows.
char *facts_model_detail(WFacts *F, const WMod *m);

const char *facts_file_path(WFacts *F, i32 fi);
// Which project a file belongs to (-1 when none), and where that project's shared
// header is ("" when it has none).
i32 facts_proj_of(WFacts *F, i32 fi);
const char *facts_shared_header(WFacts *F, i32 fi);
WHalf facts_half_of(const char *path, i32 fi, WFacts *F);

// The route whose block contains this position, for path-parameter completion.
WRoute *facts_route_at(WFacts *F, i32 file, u32 off);

#endif // WEBC_TOOL_FACTS_H