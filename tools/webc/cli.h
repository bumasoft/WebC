// WebC's CLI — the helpers its own translation units share.
//
// `tools/webc/main.c` and `tools/webc/mcp.c` are two faces of one program: a command the
// CLI can run, MCP can call. What they print has to be the same bytes, so the shapes they
// both print live here, once, rather than as a copy per file that a change can part.
#ifndef WEBC_CLI_H
#define WEBC_CLI_H

#include <webc/core.h>

// A JSON string on a Buf, quotes included, for the `--json` face of a command. The
// framework's json.h writes through a JsonWriter over an arena; a command that already
// owns a Buf wants the value in place, and this is the whole of that difference.
void jputs(Buf *b, const char *s);

#endif // WEBC_CLI_H