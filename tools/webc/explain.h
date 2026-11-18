// WebC — the compiler's sentences, translated.
//
// A WebC source is C plus a macro DSL, and clang is the truth about both: it
// type-checks a whole view in about a tenth of a second. What it cannot do is *talk*
// about it. `page(Nope, …)` comes back as "use of undeclared identifier 'view_Nope'"
// behind five frames of `WC_CAT`, and `db_all()` in a shared file comes back as "use
// of undeclared identifier 'db_all'" with no mention of the half rule that makes it
// illegal.
//
// The language server already answers both of those sentences — it is the product
// described in docs/reference.md §14d. This file points that engine at a *build*
// instead of at a caret: it runs the same project index (facts.c), the same region map
// (dsl.c) and the same diagnostics (lspx.c) over what is on disk, and it turns the
// compiler's output into `file:line: why, and what to write instead`.
//
// Two doors into it:
//   - `webc explain` runs the build and annotates what it printed.
//   - a failed `webc build` calls explain_compiler_output() itself, so the sentence
//     arrives where the mistake was made, without a second command.
#ifndef WEBC_TOOL_EXPLAIN_H
#define WEBC_TOOL_EXPLAIN_H

#include <stdbool.h>

#include <webc/core.h>

// Print the diagnosis for a project, from the sources on disk. `json` emits one object
// per file with its diagnostics instead of prose. Returns the number of *errors*; 0
// means the project is clean as far as the WebC engine is concerned.
i32 explain_project(const char *root, bool json, bool leave_quiet);

// The same, but only for the files the compiler did *not* name (a build stops at the
// first file it cannot compile). Prints a short header when it has something to say.
i32 explain_project_rest(const char *root, const char *compiler_output);

// Read compiler output (`path:line:col: error: message`) and print the WebC diagnosis
// for each line, in the same order. Returns true when it had something better to say
// than the compiler did; false when the caller should show the raw output instead.
bool explain_compiler_output(const char *root, const char *compiler_output);

// `webc explain [--json] [--raw]` — build, then explain.
i32 cmd_explain(const char *root, const char *self, i32 argc, char **argv);

#endif // WEBC_TOOL_EXPLAIN_H