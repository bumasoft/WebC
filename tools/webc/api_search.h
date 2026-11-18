// The public-surface search, in one place.
//
// `webc api <query>` and the MCP tool `webc_api` answer the same question — "does this name
// exist, where is it declared, and what does the comment above it say" — so they call the same
// function. Two scanners would eventually disagree about what the framework contains, and the
// one that is wrong would be the one an agent trusts.
#pragma once

#include <webc/core.h>

// Matches for `query` across include/webc/*.h, as `{query, matches:[{header, line,
// declaration, doc}]}`. A declaration line (or `#define`) that contains the fragment; `doc`
// is the last `//` comment above it, which is where this framework writes its one sentence.
void api_search_json(Buf *out, const char *query, i32 limit);

// The same matches, printed for a person: `header:line`, the declaration, and the doc line.
i32 api_search_text(const char *query, i32 limit);
