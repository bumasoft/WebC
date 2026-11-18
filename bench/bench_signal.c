// How fast is a signal write + one effect, and how fast is a DOM patch?
#include <webc/server.h>
#include <webc/web.h>
#include <time.h>

state(int, n, 0);
static i64 sink;
effect(obs) { sink += get(n); }

view(Row) {
  el("div") { el("span") { text("%lld", (long long)get(n)); } }
}

// Bytes the heap saw grow, per operation: the figure a Go benchmark prints as
// B/op. It is the interesting one for a framework that owns its own allocator.
static double bytes_per_op(size_t before, size_t after, i32 ops) {
  return after > before ? (double)(after - before) / (double)ops : 0.0;
}

int main(void) {
  const i32 N = 200000;
  size_t heap0 = wm_stats().heap_used;
  double t0 = wc_wall_ms();
  for (i32 i = 0; i < N; i++) set(n, i);
  double t1 = wc_wall_ms();
  printf("signal write + 1 effect   %8.2f ns/op  %6.1f B/op  (%d ops)\n",
         (t1 - t0) * 1e6 / N, bytes_per_op(heap0, wm_stats().heap_used, N), N);

  WComponent c;
  wcomp_init(&c, nullptr);
  wcomp_render_ctx(&c, view_Row, nullptr);
  heap0 = wm_stats().heap_used;
  t0 = wc_wall_ms();
  for (i32 i = 0; i < N; i++) {
    set(n, i);
    wcomp_render_ctx(&c, view_Row, nullptr);
  }
  t1 = wc_wall_ms();
  printf("re-render + patch         %8.2f ns/op  %6.1f B/op  (%d bytes in the last patch)\n",
         (t1 - t0) * 1e6 / N, bytes_per_op(heap0, wm_stats().heap_used, N), (int)c.cmds.b.n);
  (void)sink;
  return 0;
}
