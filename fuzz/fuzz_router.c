// fuzz_router — pattern matching: `:param`, `*rest`, trailing slashes, and paths
// that are nothing but separators (SPEC §11).
// `webc/webc.h` is retired (see `webc lint`); the router lives here.
#include <webc/http.h>

static WRouter *g_r;

static WRouter *router(void) {
  if (g_r) return g_r;
  g_r = router_new(nullptr);
  router_insert(g_r, "GET", "/", nullptr, nullptr, 0);
  router_insert(g_r, "GET", "/users/:id", nullptr, nullptr, 0);
  router_insert(g_r, "GET", "/users/:id/posts/:post", nullptr, nullptr, 0);
  router_insert(g_r, "POST", "/users", nullptr, nullptr, 0);
  router_insert(g_r, "GET", "/files/*rest", nullptr, nullptr, 0);
  return g_r;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  Arena *a = arena_new(1 << 16);
  str path = {(char *)data, size};
  WMatch m = router_match(router(), str_lit("GET"), path);
  // a match must name a pattern and every captured param must live inside the path
  if (m.found) {
    for (i32 i = 0; i < m.nparams; i++) {
      str v = m.param_v[i];
      if (v.n && (v.p < path.p || v.p + v.n > path.p + path.n)) __builtin_trap();
    }
  }
  arena_destroy(a);
  return 0;
}
