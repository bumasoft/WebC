// SPDX-License-Identifier: Apache-2.0
// WebC — the HTTP client (SPEC §8/§12 `net/http`, and the CLI's own transport).
//
//   WRespC r;
//   WErrRes e = http_get(req->arena, "http://127.0.0.1:8080/health", &r);
//   if (e.ok) log_info(...);
//
// Blocking-shaped, green underneath: the socket waits park the calling task, so a
// handler may call a service and another task keeps serving meanwhile. Redirects are
// returned, not followed — a client that follows silently is a client you cannot
// debug, and `webc` (which does follow them) does it in one visible loop.
#ifndef WEBC_CLIENT_H
#define WEBC_CLIENT_H

#include <webc/core.h>

typedef struct WClient {
  const char *method;          // "GET", "POST", ...
  const char *url;             // http:// or https://
  str body;                    // borrowed
  const char *content_type;    // used when body is non-empty and this is set
  const char *const *headers;  // name, value, name, value ...
  i32 nheaders;                // number of strings in `headers` (not pairs)
  i32 timeout_ms;              // 0 = 10 000
  const char *ca_file;         // https: a CA bundle; null = the system store
  bool insecure;               // https: accept any certificate (tests only)
} WClient;

typedef struct WRespC {
  i32 status;
  str body;        // arena-allocated, decompressing nothing (no gzip on the wire)
  str content_type;
  str location;    // for 3xx
  str headers_raw; // the block as received, for a client that needs something odd
} WRespC;

WErrRes http_do(Arena *a, WClient c, WRespC *out);
WErrRes http_get(Arena *a, const char *url, WRespC *out);
WErrRes http_post(Arena *a, const char *url, const char *ctype, str body, WRespC *out);

// Follow up to `max_hops` 3xx redirects, re-sending the same method and body.
WErrRes http_do_follow(Arena *a, WClient c, i32 max_hops, WRespC *out);

#endif // WEBC_CLIENT_H
