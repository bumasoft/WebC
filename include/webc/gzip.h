// SPDX-License-Identifier: Apache-2.0
// gzip, in the framework, so a response can leave smaller than it was built.
//
// Two functions and a middleware, and the middleware is the one an app calls:
//
//   wc_gzip(a, in, level)     RFC 1952 bytes — a gzip stream, what `Content-Encoding:
//                             gzip` promises, what `gzip -d` and every browser read
//   wc_gunzip(a, in)          the inverse, for the code that has to read one back
//   mw_compress()             `webc_use(mw_compress())`: gzip every response the client
//                             said it accepts, and serve it smaller
//
// The compressor is zlib's deflate, not a hand-rolled one. A specific gzip stream is
// the point of the verb — it is what a browser, a CDN and `gzip -d` all verify — and
// zlib is the one implementation on the planet that everybody's already got. It is a
// link flag (`-lz`) and the only library the framework links besides sqlite3.
//
// Level 9 by default, because the fallback that matters here is a wasm module or a
// stylesheet: compressed once, served ten thousand times, and 6% smaller for it.
//
// Both take an arena and both hand the bytes back in it: a compressed body belongs to
// the response that is being written, and freeing it is the arena's job, not the
// caller's. `a == NULL` allocates on the heap instead (freed with `wm_free`), which is
// what the middleware's own cache does.
#pragma once
#include <webc/core.h>

#define WC_GZIP_LEVEL 9         // zlib's slowest and smallest; raise or lower per call
#define WC_GZIP_MIN   1024      // below this the 18-byte wrapper costs more than it saves

// The gzip stream of `in`. `level` is 0..9, -1 for zlib's default.
// Returns {nullptr, 0} when zlib refuses (only possible on a broken build or OOM).
Bytes wc_gzip(Arena *a, Bytes in, i32 level);

// What `in` decompresses to, for a gzip *or* zlib stream (the wrapper is detected).
// Returns {nullptr, 0} when the bytes are not a stream zlib can read.
Bytes wc_gunzip(Arena *a, Bytes in);