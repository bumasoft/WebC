// SPDX-License-Identifier: Apache-2.0
// gzip — see include/webc/gzip.h for what this is and why it is zlib.
//
// The shape of the file is zlib's C API, which is a state machine with four calls and
// three rules that are easy to get wrong:
//
//   * deflateBound() asks how big the output can be *after* deflateInit2, and
//     compressBound() (used here) is the same answer without the stream. The gzip
//     wrapper adds 18 bytes on top, which compressBound already covers, but a cap of
//     `compressBound(n)` alone is one byte short for an incompressible input on some
//     zlib builds — the +64 is the margin that keeps that from being an "unexpected
//     Z_BUF_ERROR" at 3 a.m.
//   * next_in/avail_in are `uInt`, so a body over 4 GB cannot be one call. A static
//     file that big is not what this is for; the caller gets a refusal, not a wrap.
//   * inflateInit2(15 + 32) is "gzip or zlib, whichever it is": 15 is the window, 32
//     is the "auto-detect" bit. 15 + 16 would read gzip only, and a `Content-Encoding:
//     deflate` response someone sends us would be refused.
#include <webc/gzip.h>

#include <string.h>

#include <zlib.h>

static Bytes out_alloc(Arena *a, size_t n) {
  char *p = a ? (char *)arena_alloc(a, n) : (char *)wm_alloc(n);
  return (Bytes){.p = p, .n = n};
}

Bytes wc_gzip(Arena *a, Bytes in, i32 level) {
  if (!in.p || in.n == 0) return (Bytes){0};
  if (in.n > 0xFFFFFFFFu) return (Bytes){0}; // zlib's uInt counter cannot hold it
  if (level < -1 || level > 9) level = WC_GZIP_LEVEL;

  z_stream z = {0};
  // 15 + 16: a 32 KB window and the gzip wrapper (a zlib wrapper is 15 + 0).
  if (deflateInit2(&z, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    return (Bytes){0};

  uLong cap = compressBound((uLong)in.n) + 64;
  Bytes out = out_alloc(a, (size_t)cap);
  z.next_in = (Bytef *)in.p;
  z.avail_in = (uInt)in.n;
  z.next_out = (Bytef *)out.p;
  z.avail_out = (uInt)cap;

  // One call is enough for Z_FINISH on a buffer that is already the bound: deflate
  // keeps going while it has output room, and Z_FINISH means "do not stop at a flush
  // point, finish the stream".
  int rc = deflate(&z, Z_FINISH);
  size_t produced = (size_t)z.total_out;
  deflateEnd(&z);
  if (rc != Z_STREAM_END) {
    if (!a) wm_free(out.p);
    return (Bytes){0};
  }
  out.n = produced;
  return out;
}

Bytes wc_gunzip(Arena *a, Bytes in) {
  if (!in.p || in.n == 0) return (Bytes){0};
  if (in.n > 0xFFFFFFFFu) return (Bytes){0};

  z_stream z = {0};
  if (inflateInit2(&z, 15 + 32) != Z_OK) return (Bytes){0};
  z.next_in = (Bytef *)in.p;
  z.avail_in = (uInt)in.n;

  // The size is not in the stream, so inflate is given a buffer and told to grow when
  // it fills. Starting at 4x the input is right for what this is used on (text, wasm,
  // JSON) and wrong only for a highly compressed bomb — which then grows, one doubling
  // at a time, up to the arena's own limit.
  size_t cap = in.n * 4 + 1024;
  char *buf = nullptr;
  size_t used = 0;
  for (i32 round = 0; round < 12; round++) {
    char *nb = a ? (char *)arena_alloc(a, cap) : (char *)wm_alloc(cap);
    if (used) memcpy(nb, buf, used);
    if (!a && buf) wm_free(buf);
    buf = nb;
    z.next_out = (Bytef *)(buf + used);
    z.avail_out = (uInt)(cap - used);
    int rc = inflate(&z, Z_NO_FLUSH);
    used = (size_t)z.total_out;
    if (rc == Z_STREAM_END) {
      inflateEnd(&z);
      return (Bytes){.p = buf, .n = used};
    }
    if (rc != Z_OK && rc != Z_BUF_ERROR) break; // corrupt, or not a stream at all
    if (z.avail_in == 0) break;                 // out of input before the end: truncated
    cap *= 2;
  }
  inflateEnd(&z);
  if (!a && buf) wm_free(buf);
  return (Bytes){0};
}