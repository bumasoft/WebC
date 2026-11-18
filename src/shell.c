// SPDX-License-Identifier: Apache-2.0
// webc — the document a project does not have to write by hand.
//
// Handed to a person, a fullstack project's `public/index.html` is nine lines of boilerplate
// — a doctype, a title, a stylesheet link, `<div id="root">`, `<script src="/webc.js">`, and
// a boot script that tells the bridge where the wasm is, where the seed data is and whether
// to hydrate — and a tenth that is easy to get wrong: a missing `hydrate: true`, a data URL
// that does not match the route the app serves, a self-heal reload that hides a real
// mismatch.
//
// So the shell is generated from `webc.json`:
//
//   "boot": {
//     "title":  "Contrail — Vela Air global ops",
//     "lang":   "en",
//     "html":   { "data-theme": "dark" },      // attributes on <html>
//     "css":    ["/public/style.css"],
//     "icon":   "data:image/svg+xml,…",
//     "el":     "#root",                        // mount point, default #root
//     "portal": "#portal",                      // optional second mount, default none
//     "data":   "/api/board",                   // seed document handed to the module
//     "hydrate": true                           // adopt the server's nodes
//   }
//
// `public/index.html` still wins when it exists: a project that wants a different document
// keeps it, and nothing here is a replacement for `res_page()`'s splice.
#include <stdio.h>
#include <string.h>

#include <webc/http.h>
#include <webc/json.h>

typedef struct WBoot {
  bool present;
  str title, lang, icon, el, portal, data, wasm;
  bool hydrate;
  bool inline_seed; // embed the document itself in the page (webc_seed)
  str css;      // the array's items, joined and escaped, ready to paste
  str htmlattr; // ` data-theme="dark" data-x="1"`, leading space, ready to paste
  // The CSP lists from the `"csp"` block, as the strings the policy builder wants. A CDN in
  // `img-src` is a project decision, and it belongs in the project's config next to the rest of
  // them rather than in twenty lines of C that splice a header with `strstr`.
  const char *img_src[16];
  i32 img_src_n;
  const char *script_src[16];
  i32 script_src_n;
  bool no_default;
  str not_found; // "public/404.html": the page a browser gets for a URL nobody routed
} WBoot;

static WBoot g_boot;
static bool g_boot_read;

// Attributes and text go into HTML: an unescaped `"` in a title is a broken document, and a
// project that puts an ampersand in its name should not have to think about it.
static void esc(Buf *b, str s, bool attr) {
  for (size_t i = 0; i < s.n; i++) {
    char c = s.p[i];
    switch (c) {
    case '&': buf_putstr(b, str_lit("&amp;")); break;
    case '<': buf_putstr(b, str_lit("&lt;")); break;
    case '>': buf_putstr(b, str_lit("&gt;")); break;
    case '"':
      if (attr) buf_putstr(b, str_lit("&quot;"));
      else buf_putc(b, c);
      break;
    default: buf_putc(b, c);
    }
  }
}

// "#root" -> "root": the selector is what the bridge mounts into, the id is what the
// document can carry. A leading '#' is the only form res_page() understands.
static str selector_id(str sel) { return sel.n && sel.p[0] == '#' ? str_sub(sel, 1, sel.n) : sel; }

// A JavaScript string literal, inside the double quotes of an attribute-free tag. The
// boot script is the one place a value crosses from JSON into JavaScript, so it is
// quoted here rather than trusted: `<\/` closes the tag early if a value ever carries it.
static void js_str(Buf *b, str s) {
  buf_putc(b, '"');
  for (size_t i = 0; i < s.n; i++) {
    unsigned char c = (unsigned char)s.p[i];
    if (c == '"' || c == '\\') {
      buf_putc(b, '\\');
      buf_putc(b, (char)c);
    } else if (c == '<') {
      buf_putstr(b, str_lit("\\u003c"));
    } else if (c == '\n') {
      buf_putstr(b, str_lit("\\n"));
    } else if (c < 0x20) {
      char tmp[8];
      snprintf(tmp, sizeof tmp, "\\u%04x", c);
      buf_putstr(b, str_lit(tmp));
    } else {
      buf_putc(b, (char)c);
    }
  }
  buf_putc(b, '"');
}

// Its own arena, for the life of the process. The boot block is read once, and every str in
// it is a slice of the file — keeping those slices in the *request* arena meant the second
// request read a freed buffer, found no `#root`, and served a page with no scripts on it.
// That is the sort of bug that passes a smoke test and fails a reload.
static void read_csp(JVal *root, Arena *a);

static void read_boot(void) {
  if (g_boot_read) return;
  g_boot_read = true;
  static Arena *boot_arena;
  if (!boot_arena) boot_arena = arena_new_labeled(16 * 1024, "shell");
  Arena *a = boot_arena;
  FILE *f = fopen("webc.json", "rb");
  if (!f) return;
  Buf src = {0};
  buf_init(&src, a, 1024);
  char chunk[4096];
  size_t got;
  while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) buf_write(&src, chunk, got);
  fclose(f);
  JParse p = json_parse(a, buf_str(&src));
  if (!p.ok) return; // a broken webc.json is reported by the CLI, not once per page

  // Two top-level keys read here because this is the one place that reads webc.json at boot, in
  // its own arena, once per process — and both are things the *server* needs, not the shell:
  // the 404 page and the CSP lists. They are read before the `boot` check, so a server-only
  // project (no boot block) can have them too.
  str nf = json_as_str(json_get(p.v, "not_found"));
  if (nf.n) g_boot.not_found = nf;
  read_csp(p.v, a);

  JVal *boot = json_get(p.v, "boot");
  if (!boot || boot->kind != J_OBJ) return;
  g_boot.present = true;

  str title = json_as_str(json_get(boot, "title"));
  g_boot.title = title.n ? title : str_lit("webc app");
  str lang = json_as_str(json_get(boot, "lang"));
  g_boot.lang = lang.n ? lang : str_lit("en");
  g_boot.icon = json_as_str(json_get(boot, "icon"));
  str el = json_as_str(json_get(boot, "el"));
  g_boot.el = el.n ? el : str_lit("#root");
  g_boot.portal = json_as_str(json_get(boot, "portal"));
  g_boot.data = json_as_str(json_get(boot, "data"));
  str wasm = json_as_str(json_get(boot, "wasm"));
  g_boot.wasm = wasm.n ? wasm : str_lit("/app.wasm");
  bool hydrate = false;
  if (json_as_bool(json_get(boot, "hydrate"), &hydrate)) g_boot.hydrate = hydrate;
  bool inl = false;
  if (json_as_bool(json_get(boot, "inline"), &inl)) g_boot.inline_seed = inl;

  Buf css = {0};
  buf_init(&css, a, 128);
  JVal *arr = json_get(boot, "css");
  if (arr && arr->kind == J_ARR) {
    for (i32 i = 0; i < arr->n; i++) {
      str href = json_as_str(json_at(arr, i));
      if (!href.n) continue;
      buf_putstr(&css, str_lit("\n    <link rel=\"stylesheet\" href=\""));
      esc(&css, href, true);
      buf_putstr(&css, str_lit("\">"));
    }
  }
  g_boot.css = buf_str(&css);

  Buf ha = {0};
  buf_init(&ha, a, 64);
  JVal *html = json_get(boot, "html");
  if (html && html->kind == J_OBJ) {
    for (i32 i = 0; i < html->n; i++) {
      if (!html->keys[i].n) continue;
      buf_putc(&ha, ' ');
      esc(&ha, html->keys[i], true);
      buf_putstr(&ha, str_lit("=\""));
      esc(&ha, json_as_str(html->items[i]), true);
      buf_putc(&ha, '"');
    }
  }
  g_boot.htmlattr = buf_str(&ha);

}

// "csp": {"img_src": ["'self'", "data:", "https://cdn.example.com"], "script_src": [...],
//         "no_default": true}: the project's policy, as the lists the builder wants.
static void read_csp(JVal *root, Arena *a) {
  JVal *csp = json_get(root, "csp");
  if (csp && csp->kind == J_OBJ) {
    struct {
      const char *key;
      const char **out;
      i32 *n;
    } lists[] = {{"img_src", g_boot.img_src, &g_boot.img_src_n},
                 {"script_src", g_boot.script_src, &g_boot.script_src_n}};
    for (size_t l = 0; l < sizeof lists / sizeof lists[0]; l++) {
      JVal *arr = json_get(csp, lists[l].key);
      if (!arr || arr->kind != J_ARR) continue;
      for (i32 i = 0; i < arr->n && *lists[l].n < 16; i++) {
        str v = json_as_str(json_at(arr, i));
        if (!v.n) continue;
        lists[l].out[(*lists[l].n)++] = str_cstr(a, v);
      }
    }
    bool nd = false;
    if (json_as_bool(json_get(csp, "no_default"), &nd)) g_boot.no_default = nd;
  }
}

// `"not_found": "public/404.html"`, or "" — the page a browser gets for a URL nobody routed.
const char *wc_shell_not_found(void) {
  read_boot();
  return g_boot.not_found.p ? str_cstr(nullptr, g_boot.not_found) : "";
}

// The `"csp"` block, as the middleware struct it configures. Empty when the project said
// nothing, which is what makes the default policy the default.
HeadersCfg wc_shell_csp(void) {
  read_boot();
  HeadersCfg c = {0};
  c.img_src = g_boot.img_src_n ? g_boot.img_src : nullptr;
  c.img_src_n = g_boot.img_src_n;
  c.script_src = g_boot.script_src_n ? g_boot.script_src : nullptr;
  c.script_src_n = g_boot.script_src_n;
  c.no_default = g_boot.no_default;
  return c;
}

// The app's half of an inline seed: the document this page is being rendered against,
// the same bytes the data URL would serve. Weak, so a project that does not implement it
// falls back to the fetch — and a test can define it too.
__attribute__((weak)) str webc_seed(Arena *a) {
  (void)a;
  return (str){0};
}

bool wc_shell_has_boot(void) {
  read_boot();
  return g_boot.present;
}

// `<div id="root"></div>` from `#root`: res_page() splices at the same marker, so the two
// agree about where the server's markup goes without either one parsing the other.
// The mount point's id, without the selector's '#'. res_page() splices at this id, so the
// generated document and the splice cannot disagree about where the view goes.
str wc_shell_mount(void) {
  read_boot();
  if (!g_boot.present) return str_lit("root");
  return selector_id(g_boot.el);
}

str wc_shell_auto(Arena *a) {
  read_boot();
  if (!g_boot.present) return (str){0};
  Buf b = {0};
  buf_init(&b, a, 1024);
  buf_putstr(&b, str_lit("<!doctype html>\n<html lang=\""));
  esc(&b, g_boot.lang, true);
  buf_putc(&b, '"');
  buf_putstr(&b, g_boot.htmlattr);
  buf_putstr(&b, str_lit(">\n  <head>\n    <meta charset=\"utf-8\">\n"
                         "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                         "    <title>"));
  esc(&b, g_boot.title, false);
  buf_putstr(&b, str_lit("</title>"));
  if (g_boot.icon.n) {
    buf_putstr(&b, str_lit("\n    <link rel=\"icon\" href=\""));
    esc(&b, g_boot.icon, true);
    buf_putstr(&b, str_lit("\">"));
  }
  buf_putstr(&b, g_boot.css);
  buf_putstr(&b, str_lit("\n  </head>\n  <body>\n    <div id=\""));
  // `el` is a selector ("#root") and res_page() splices at an id, so the id is what is
  // written. A str, not a char*: both come from the JSON buffer and neither is NUL-ended.
  esc(&b, selector_id(g_boot.el), true);
  buf_putstr(&b, str_lit("\">"));
  // the mount point stays empty here: res_page() splices the rendered view into it
  buf_putstr(&b, str_lit("</div>"));
  if (g_boot.portal.n) {
    buf_putstr(&b, str_lit("\n    <div id=\""));
    esc(&b, selector_id(g_boot.portal), true);
    buf_putstr(&b, str_lit("\"></div>"));
  }
  // The inline seed: the *same document this page was rendered against*, embedded in the
  // page by the app's webc_seed(), so the module's first render cannot disagree with the
  // server's by a tick that landed in between — and the seed round trip disappears. Apps
  // that prefer the fetch keep boot.data as a URL and simply do not implement webc_seed.
  str seed_doc = g_boot.inline_seed ? webc_seed(a) : (str){0};
  if (seed_doc.n) {
    buf_putstr(&b, str_lit("\n    <script type=\"application/json\" id=\"webc-seed\">"));
    // JSON strings may carry a `</script>`; `<` cannot appear in structural JSON, so
    // escaping every `<` as \\u003c keeps the document valid JSON and the page intact
    for (size_t i = 0; i < seed_doc.n; i++) {
      if (seed_doc.p[i] == '<') buf_putstr(&b, str_lit("\\u003c"));
      else buf_putc(&b, seed_doc.p[i]);
    }
    buf_putstr(&b, str_lit("</script>"));
  }

  buf_putstr(&b, str_lit("\n\n    <script src=\"/webc.js\"></script>\n    <script>\n"
                         "      // The module takes over the markup the server rendered and says so on\n"
                         "      // the server rendered and says so on <html>: no app JavaScript, and no second\n"
                         "      // chance to get the options wrong.\n"
                         "      WebC.start({ el: "));
  js_str(&b, g_boot.el);
  buf_putstr(&b, str_lit(", wasm: "));
  js_str(&b, g_boot.wasm);
  buf_putstr(&b, g_boot.hydrate ? str_lit(", hydrate: true") : str_lit(", hydrate: false"));
  if (seed_doc.n && g_boot.data.n) {
    // the document is in the page; the URL stays as the fallback if webc_seed() said
    // nothing — the fetch this replaces is one round trip before the first render
    buf_putstr(&b, str_lit(", data: (document.getElementById('webc-seed')||{}).textContent || "));
    js_str(&b, g_boot.data);
  } else if (g_boot.data.n) {
    buf_putstr(&b, str_lit(", data: "));
    js_str(&b, g_boot.data);
  }
  buf_putstr(&b, str_lit(" }).then(function () {\n"
                         "        document.documentElement.setAttribute('data-ready', '1');\n"
                         "      }).catch(function (e) {\n"
                         "        console.error('[webc] the module did not start', e);\n"
                         "      });\n"
                         "    </script>\n  </body>\n</html>\n"));
  return buf_str(&b);
}

void wc_shell_reset(void) {
  g_boot_read = false;
  g_boot = (WBoot){0};
}