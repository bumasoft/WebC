// fuzz_cookie — the Cookie header reader: quoting, `=` inside values, empty
// names, junk pairs (SPEC §11).
//
// req_cookie() works on a request's parsed headers, so the harness builds the
// header table by hand rather than dragging in a socket: that is exactly the shape
// the server hands the handler.
// `webc/webc.h` is retired (see `webc lint`); req_cookie() lives here.
#include <webc/http.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  Arena *a = arena_new(1 << 16);
  Request r = {0};
  r.arena = a;
  WHeader hdrs[2];
  hdrs[0] = (WHeader){.name = str_lit("cookie"),
                      .value = {(char *)data, size < (1u << 20) ? size : 0}};
  r.headers = hdrs;
  r.nheaders = 1;
  str got = req_cookie(&r, "session");
  // a value that came back must be inside the header we handed in
  if (got.n && (got.p < hdrs[0].value.p || got.p + got.n > hdrs[0].value.p + hdrs[0].value.n))
    __builtin_trap();
  (void)req_cookie(&r, "");
  (void)req_cookie(&r, "a=b; c");
  arena_destroy(a);
  return 0;
}
