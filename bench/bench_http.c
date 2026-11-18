// In-process request throughput: routing + handler + JSON/SSR rendering.
// No sockets, so this measures what WebC controls; the examples measure the rest.
#include <webc/server.h>

WEBC_MODEL(Item, (id, i64, .pk = true), (name, Text))
Vec(Item)

static VecItem g_items;
state(i64, unused, 0);

view(List, VecItem *items) {
  el("ul") {
    for_each(it, *items) {
      el("li") { text("%lld: %s", (long long)it->id, it->name); }
    }
  }
}

static Response h_json(Request *req) {
  (void)req;
  return json(200, &g_items);   // the vector knows its model
}
static Response h_ssr(Request *req) {
  (void)req;
  return page(List, &g_items);
}

static void bench(const char *label, const char *path, i32 n) {
  size_t heap0 = wm_stats().heap_used;
  double t0 = wc_wall_ms();
  size_t bytes = 0;
  i32 ok = 0;
  for (i32 i = 0; i < n; i++) {
    Response r = webc_test_request("GET", path);
    if (r.status == 200) ok++;
    bytes += r.body.n;
  }
  double ms = wc_wall_ms() - t0;
  size_t heap1 = wm_stats().heap_used;
  // B/op: the heap the request path itself grows. Requests are arena-scoped, so
  // this should stay flat — and if it does not, that is the headline.
  double bop = heap1 > heap0 ? (double)(heap1 - heap0) / (double)n : 0.0;
  printf("%-22s %8.2f us/req  %8.0f req/s  %6.1f B/op  (%d x %zu bytes)\n", label,
         ms * 1000 / n, n / (ms / 1000.0), bop, ok, bytes / (size_t)(n ? n : 1));
}

int main(void) {
  for (i32 i = 0; i < 10; i++) push(g_items, ((Item){.id = i, .name = fmt("item number %d", i)}));

  webc_router_reset();
  webc_route_add("GET", "/json", h_json, nullptr, 0);
  webc_route_add("GET", "/ssr", h_ssr, nullptr, 0);

  bench("route + json (10)", "/json", 20000);
  bench("route + ssr (10 rows)", "/ssr", 5000);
  return 0;
}
