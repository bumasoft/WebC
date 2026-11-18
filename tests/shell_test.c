// shell: the document a project does not write by hand (src/shell.c).
//
// Every fullstack project carries the same `public/index.html`, and the interesting line in
// it — `WebC.start({ hydrate: true, data: '/api/board' })` — is the one that can be wrong:
// with `hydrate` off and a data URL nobody serves, the server's markup is thrown away and
// rebuilt. The shell is generated from `webc.json`, which is one place to be wrong instead
// of one per project.
#include <unistd.h>   // before webc/test.h: the SDK's sys/select.h trips over a WebC macro

#include <webc/test.h>
#include <webc/gzip.h>

static char g_old[512];
// A template, not a directory: mkdtemp() rewrites the six X's in place, so reusing
// one buffer makes every call after the first fail on glibc (Darwin happens to tolerate
// it). Each in_project() gets a fresh template.
static char g_tmp[64];

static void in_project(const char *json) {
  if (getcwd(g_old, sizeof g_old) == nullptr) g_old[0] = 0;
  snprintf(g_tmp, sizeof g_tmp, "/tmp/webc-shell-XXXXXX");
  if (mkdtemp(g_tmp) == nullptr) {
    ASSERT_TRUE(false);
    return;
  }
  if (chdir(g_tmp) != 0) ASSERT_TRUE(false);
  FILE *f = fopen("webc.json", "wb");
  if (!f) return;
  fwrite(json, 1, strlen(json), f);
  fclose(f);
  wc_shell_reset();
}

static void out_of_project(void) {
  wc_shell_reset();
  if (g_old[0]) (void)chdir(g_old);
  g_old[0] = 0;
  char cmd[512];
  snprintf(cmd, sizeof cmd, "rm -rf %s", g_tmp);
  (void)system(cmd);
}

static void view_of(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("p") { text("hello"); }
}

TEST(a_project_with_a_boot_block_gets_a_whole_document) {
  in_project("{\"name\":\"contrail\",\"boot\":{\"title\":\"Vela Air\",\"lang\":\"en\","
             "\"html\":{\"data-theme\":\"dark\"},\"css\":[\"/public/style.css\"],"
             "\"data\":\"/api/board\",\"hydrate\":true}}");
  Arena *a = arena_new(16384);
  ASSERT_TRUE(wc_shell_has_boot());
  str s = wc_shell_auto(a);
  ASSERT_TRUE(str_has_prefix_c(s, "<!doctype html>"));
  ASSERT_TRUE(str_contains(s, str_lit("<html lang=\"en\" data-theme=\"dark\">")));
  ASSERT_TRUE(str_contains(s, str_lit("<title>Vela Air</title>")));
  ASSERT_TRUE(str_contains(s, str_lit("href=\"/public/style.css\"")));
  ASSERT_TRUE(str_contains(s, str_lit("<div id=\"root\"></div>")));
  ASSERT_TRUE(str_contains(s, str_lit("<script src=\"/webc.js\"></script>")));
  ASSERT_TRUE(str_contains(s, str_lit("hydrate: true")));
  ASSERT_TRUE(str_contains(s, str_lit("data: \"/api/board\"")));
  out_of_project();
}

TEST(no_boot_block_means_bare_ssr_not_a_guessed_document) {
  // A server-only project must keep the behaviour it had: no shell, no <html>.
  in_project("{\"name\":\"plain\"}");
  Arena *a = arena_new(4096);
  ASSERT_TRUE(!wc_shell_has_boot());
  str s = wc_shell_auto(a);
  ASSERT_EQ((int)s.n, 0);
  out_of_project();
}

TEST(the_defaults_are_a_working_page) {
  in_project("{\"boot\":{\"title\":\"t\"}}");
  Arena *a = arena_new(8192);
  str s = wc_shell_auto(a);
  ASSERT_TRUE(str_contains(s, str_lit("el: \"#root\"")));
  ASSERT_TRUE(str_contains(s, str_lit("wasm: \"/app.wasm\"")));
  ASSERT_TRUE(str_contains(s, str_lit("hydrate: false")));
  ASSERT_TRUE(str_contains(s, str_lit("<html lang=\"en\">")));
  out_of_project();
}

TEST(the_mount_selector_becomes_an_id_and_the_bridge_still_gets_the_selector) {
  // `"el": "#root"` is a selector; the document carries an id and wc_render_page() splices at it.
  // A non-NUL-terminated slice from the JSON buffer turned that id into the rest of the file
  // once, which put the view *outside* the mount point and made hydration adopt nothing.
  in_project("{\"boot\":{\"title\":\"t\",\"el\":\"#board\",\"portal\":\"#more\"}}");
  Arena *a = arena_new(8192);
  str s = wc_shell_auto(a);
  ASSERT_TRUE(str_contains(s, str_lit("<div id=\"board\"></div>")));
  ASSERT_TRUE(str_contains(s, str_lit("<div id=\"more\"></div>")));
  ASSERT_TRUE(str_contains(s, str_lit("el: \"#board\"")));
  out_of_project();
}

TEST(a_title_with_quotes_or_a_script_tag_cannot_break_out) {
  // The title is the one value a project is likely to write by hand, and the boot script is
  // the one place JSON turns into JavaScript.
  in_project("{\"boot\":{\"title\":\"</script><b>x</b> & \\\"y\\\"\",\"data\":\"</script>\"}}");
  Arena *a = arena_new(8192);
  str s = wc_shell_auto(a);
  ASSERT_TRUE(str_contains(s, str_lit("&lt;/script&gt;&lt;b&gt;x&lt;/b&gt; &amp; \"y\"")));
  ASSERT_TRUE(!str_contains(s, str_lit("data: \"</script>\"")));
  ASSERT_TRUE(str_contains(s, str_lit("\\u003c/script>")));
  out_of_project();
}

TEST(the_shell_survives_the_request_arena_being_reset) {
  // The boot block's strings are slices of the file it was read from. Held in the request
  // arena they were freed after the first request, and the second one served a document with
  // no mount point, no /webc.js and no boot script — a page that looked fine and did nothing.
  in_project("{\"boot\":{\"title\":\"Ops\",\"el\":\"#board\",\"data\":\"/api/board\"}}");
  Arena *req = arena_new(8192);
  Arena *prev = wc_arena_swap(req);
  Response first = wc_render_page(200, nullptr, view_of, nullptr);
  ASSERT_TRUE(str_contains((str){first.body.p, first.body.n}, str_lit("<script src=\"/webc.js\">")));
  arena_reset(req); // what the server does between requests
  Response second = wc_render_page(200, nullptr, view_of, nullptr);
  ASSERT_TRUE(str_contains((str){second.body.p, second.body.n}, str_lit("<div id=\"board\">")));
  ASSERT_TRUE(str_contains((str){second.body.p, second.body.n}, str_lit("data: \"/api/board\"")));
  wc_arena_swap(prev);
  out_of_project();
}

TEST(res_page_splices_into_the_generated_shell) {
  in_project("{\"boot\":{\"title\":\"Ops\",\"data\":\"/api/board\",\"hydrate\":true}}");
  Arena *a = arena_new(32768);
  Response r = wc_render_page(200, nullptr, view_of, nullptr);
  str body = r.body.p ? (str){r.body.p, r.body.n} : (str){0};
  ASSERT_TRUE(str_contains(body, str_lit("<div id=\"root\"><p")));
  ASSERT_TRUE(str_contains(body, str_lit("hello</p></div>")));
  ASSERT_TRUE(str_contains(body, str_lit("WebC.start({")));
  (void)a;
  out_of_project();
}

// an app that renders its own seed: the page carries the document it was rendered
// against. A strong definition replaces the weak default for the whole binary, so the
// fallback test switches it off and asserts the same opt-in degrades to the fetch.
static bool g_seed_on;

str webc_seed(Arena *a) {
  if (!g_seed_on) return (str){0};
  return str_fmt(a, "{\"title\": \"the one on screen\", \"note\": \"%s/script> alert(1)\"", "<");
}

TEST(an_inline_seed_is_embedded_and_cannot_break_out_of_the_page) {
  // No tick can land between the render and the seed when they are the same bytes, and
  // the seed round trip disappears. The `</script>` inside the document is the one
  // sequence that could end the tag early — every `<` becomes \\u003c, which is the
  // same JSON and inert markup.
  in_project("{\"boot\":{\"title\":\"t\",\"data\":\"/api/board\",\"inline\":true}}");
  wc_shell_reset();
  g_seed_on = true;
  Arena *a = arena_new(8192);
  str s = wc_shell_auto(a);
  ASSERT_TRUE(str_contains(s, str_lit("id=\"webc-seed\"")));
  ASSERT_TRUE(str_contains(s, str_lit("the one on screen")));
  ASSERT_TRUE(!str_contains(s, str_lit("</script> alert")));
  ASSERT_TRUE(str_contains(s, str_lit("\\u003c/script> alert")));
  // the boot script reads the embedded document, with the URL as its fallback
  ASSERT_TRUE(str_contains(s, str_lit("document.getElementById('webc-seed')")));
  ASSERT_TRUE(str_contains(s, str_lit("/api/board")));
  out_of_project();
}

TEST(inline_without_a_seed_falls_back_to_the_fetch) {
  // webc_seed()'s weak default says nothing: the page keeps the plain data URL, and the
  // module fetches as before. An opt-in that a project half-implements must not serve a
  // page whose boot script reads a missing element.
  in_project("{\"boot\":{\"title\":\"t\",\"data\":\"/api/board\",\"inline\":true}}");
  g_seed_on = false;
  Arena *a = arena_new(8192);
  str s = wc_shell_auto(a);
  ASSERT_TRUE(!str_contains(s, str_lit("id=\"webc-seed\"")));
  ASSERT_TRUE(str_contains(s, str_lit("data: \"/api/board\"")));
  out_of_project();
}

// Two responses to the same page differ only in the per-request nonce (the CSP names one and
// the shell's inline boot script carries it), so "the same document" is compared with every
// nonce token masked. Anything else that differs — a head applied twice, a compression pass
// that rewrote more than the encoding — shows up as a difference here.
static str mask_nonce(Arena *a, str in) {
  Buf b;
  buf_init(&b, a, in.n + 1);
  for (size_t i = 0; i < in.n;) {
    bool after = i >= 7 && memcmp(in.p + i - 7, "nonce=\"", 7) == 0;
    if (after) {
      size_t j = i;
      while (j < in.n && ((in.p[j] >= '0' && in.p[j] <= '9') || (in.p[j] >= 'a' && in.p[j] <= 'f')))
        j++;
      if (j > i) {
        buf_putc(&b, '*');
        i = j;
        continue;
      }
    }
    buf_putc(&b, in.p[i]);
    i++;
  }
  return buf_str(&b);
}

// The page's head is spliced on the way out to the socket, and it has to happen *before*
// `mw_compress` turns the body into gzip bytes. A splice that runs after it searches those
// bytes for `</head>`, finds nothing, and does nothing: every browser gets the shell's generic
// title, no description, no OG tags, no `<html data-theme>` — while `curl` without
// `Accept-Encoding` gets the head, so every smoke test passes. That is why `mw_document` runs
// innermost, and this test is the one that fails if anyone moves the splice later.
typedef struct TitledCtx {
  WPage page;
} TitledCtx;

declare_view(Titled, TitledCtx)
view(Titled, TitledCtx *c) {
  (void)c;
  el("main") {
    for (i32 i = 0; i < 200; i++)
      el("p") { text("paragraph %d of a page long enough that gzip is worth it", (int)i); }
  }
}

GET("/probe/titled") {
  TitledCtx c = {.page = head(.title = "A titled page", .desc = "its description")};
  return page(Titled, &c);
}

static str header_of(Response r, const char *name) {
  for (i32 i = 0; i < r.nheaders; i++)
    if (str_eq_c(r.headers[i].name, name)) return r.headers[i].value;
  return (str){nullptr, 0};
}

TEST(the_head_is_there_for_a_client_that_accepts_gzip) {
  in_project("{\"boot\":{\"title\":\"Generic title\",\"lang\":\"en\"}}");
  // The framework's own chain, in the framework's own order — not a hand-made one, or this
  // asserts what the test built instead of what an app gets.
  webc_use_reset();
  webc_install_default_middleware();

  Response plain = webc_test_request("GET", "/probe/titled");
  ASSERT_EQ(plain.status, 200);
  ASSERT_TRUE(str_contains((str){plain.body.p, plain.body.n}, str_lit("<title>A titled page</title>")));
  ASSERT_TRUE(str_contains((str){plain.body.p, plain.body.n}, str_lit("its description")));
  // copy it now: the body lives in the request arena, which the next request resets
  Arena *a = arena_new(1 << 18);
  str plain_doc = mask_nonce(a, (str){plain.body.p, plain.body.n});

  Response gz = webc_test_request_ex("GET", "/probe/titled", "Accept-Encoding: gzip", nullptr, 0);
  ASSERT_EQ(gz.status, 200);
  ASSERT_TRUE(str_eq_c(header_of(gz, "Content-Encoding"), "gzip"));
  Bytes back = wc_gunzip(nullptr, (Bytes){.p = gz.body.p, .n = gz.body.n});
  ASSERT_TRUE(back.p != nullptr);
  str html = {back.p, back.n};
  ASSERT_TRUE(str_contains(html, str_lit("<title>A titled page</title>")));
  ASSERT_TRUE(str_contains(html, str_lit("its description")));
  // and the same document, so the two encodings cannot drift apart either
  str gz_doc = mask_nonce(a, html);
  ASSERT_TRUE(str_eq(gz_doc, plain_doc));
  arena_destroy(a);
  wm_free(back.p);
  webc_use_reset();
  out_of_project();
}

TEST_MAIN()