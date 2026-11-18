// SPDX-License-Identifier: Apache-2.0
// client_test.c — the native half of `webc test --client`.
//
// It runs the app's real `client_main` (mount(), bind(), every signal it declares), then
// renders each recorded mount the way the wasm glue would, and prints one JSON object
// per mount:
//
//   {"sel":"#root","mount":"…ops…","patch":"…ops…","html":"<div …>"}
//
// `tools/webc/client_test.js` replays `mount`/`patch` through the real assets/webc.js
// against a fake document and compares what the DOM ends up holding with `html`. That is
// the only test of the browser half that does not need a browser: a batch that would
// have frozen the tab, an op webc.js does not know, a view that renders nothing.
//
// The app's own sources are compiled in, so this is the app's client half — not a model
// of it.
#include <webc/dom.h>
#include <webc/react.h>
#include <stdio.h>
#include <string.h>

// client_main expands to this on every non-wasm target (webc/dom.h).
int wc_app_main(void);

static void emit_json_str(Buf *out, str s) {
  const char *hex = "0123456789abcdef";
  for (size_t i = 0; i < s.n; i++) {
    unsigned char c = (unsigned char)s.p[i];
    if (c == '"' || c == '\\') {
      buf_putc(out, '\\');
      buf_putc(out, (char)c);
    } else if (c == '\n') {
      buf_puts(out, "\\n");
    } else if (c == '\r') {
      buf_puts(out, "\\r");
    } else if (c == '\t') {
      buf_puts(out, "\\t");
    } else if (c < 0x20) {
      buf_puts(out, "\\u00");
      buf_putc(out, hex[c >> 4]);
      buf_putc(out, hex[c & 15]);
    } else {
      buf_putc(out, (char)c);
    }
  }
}

int main(void) {
  // The app's client half: registers its mounts, binds its actions.
  int rc = wc_app_main();
  if (rc != 0) fprintf(stderr, "webc: client_main returned %d\n", rc);

  i32 n = wc_test_mount_count();
  Buf out;
  buf_init(&out, nullptr, 8192);
  buf_puts(&out, "{\"mounts\":[");
  for (i32 i = 0; i < n; i++) {
    const WTestMount *m = wc_test_mount(i);
    if (!m || !m->fn) continue;
    // exactly what the wasm glue does per mounted view: an instance of its own (that is
    // where per-instance state lives, and `iget` panics outside one), a render, a patch
    WInst inst;
    winst_init(&inst, nullptr);
    WComponent c;
    wcomp_init(&c, nullptr);
    winst_push(&inst);
    wcomp_render_ctx(&c, m->fn, m->ctx);
    Buf mount;
    buf_init(&mount, nullptr, 1024);
    if (c.cmds.b.p) buf_puts(&mount, c.cmds.b.p);
    // the same view again with nothing changed: the patch this produces is the one a
    // browser gets on a re-render, and a wrong one freezes the tab
    wcomp_render_ctx(&c, m->fn, m->ctx);
    Buf patch;
    buf_init(&patch, nullptr, 1024);
    if (c.cmds.b.p) buf_puts(&patch, c.cmds.b.p);
    // the server-rendered HTML for the same view and context: what the DOM must match
    str html = wcomp_html_ctx(&c, m->fn, m->ctx);
    winst_pop();
    if (i) buf_putc(&out, ',');
    buf_puts(&out, "{\"sel\":\"");
    emit_json_str(&out, str_lit(m->selector ? m->selector : "#root"));
    buf_puts(&out, "\",\"mount\":\"");
    emit_json_str(&out, (str){mount.p, mount.n});
    buf_puts(&out, "\",\"patch\":\"");
    emit_json_str(&out, (str){patch.p, patch.n});
    buf_puts(&out, "\",\"html\":\"");
    emit_json_str(&out, html);
    buf_puts(&out, "\"}");
  }
  buf_puts(&out, "]}");
  fwrite(out.p, 1, out.n, stdout);
  fputc('\n', stdout);
  return 0;
}