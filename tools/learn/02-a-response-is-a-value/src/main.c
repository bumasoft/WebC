// SPDX-License-Identifier: Apache-2.0
// learn 02 — a response is a value.
//
// A handler does not write to a socket; it returns a Response, and everything a
// request carries is a function call away. The test greets a reader by name; the
// handler answers the same words to everybody. Make it answer the query string —
// `query_text` reads it, `fmt` builds into the request's scope.
#include <webc/server.h>

GET("/greet") {
  return res_text(200, "hello");
}
