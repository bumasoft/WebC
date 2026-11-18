// WebC — the C engine: what the compiler knows, asked and translated.
//
// The other half of the language server (dsl.c). A WebC file is a C file with DSL
// regions inside it, and there is no world in which this repository grows a second C
// front end to check the parts that are plain C: clang is already the truth about C,
// it is fast, and it is what `webc build` will run. So the server runs it too — on the
// buffer the editor holds, not on what is on disk — and hands back its sentences.
//
// Two rules keep this from being a second, disagreeing opinion:
//   - a diagnostic whose position is inside a DSL region is dropped: those bytes are
//     the WebC engine's to explain, and clang speaks about them in generated names
//     (`view_Nope`, `WC_CAT(...)`).
//   - if the WebC engine already found an error, the compiler is not run at all: the
//     file it would be asked about is exactly the file a macro is still going to
//     rewrite.
#ifndef WEBC_TOOL_CENG_H
#define WEBC_TOOL_CENG_H

#include "lspx.h"

// Append what the compiler says about this buffer to `dg`. Silent when there is no
// compiler to ask, when the project is not a C project, or when the WebC engine has
// already spoken — a missing clang is not the user's problem to be told about twice.
void ceng_check(WLsp *L, WDoc *d, WDiags *dg);

// True when a compiler was found and answered at least once in this process.
bool ceng_available(void);

// Write `.webc/compile_commands.json`: the flags `webc build` uses, for every source
// the index knows. It is for the *other* tools — clangd reads it and stops guessing at
// a WebC file it has no include paths for. The server itself never reads it.
void ceng_write_compile_commands(WLsp *L);

// How long the last check took, for `--bench` and for the adaptive rule that moves a
// slow project from "check on every keystroke" to "check on save".
u32 ceng_last_ms(void);

#endif // WEBC_TOOL_CENG_H