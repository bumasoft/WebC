// dx3: long copy and authored markup.
//
// The ugliness these tests hold shut, from The Meridian:
//
//   * 77 lines of app-side parser (rich() + body_copy()) in every app that shows prose
//   * `text("%.*s", (int)n, p)` — a printf doing the job of a string slice
//   * long copy as escaped C string literals with \n\n for paragraph breaks
//   * markup("<svg …>") with quotes escaped, at ten call sites
//
// The surfaces: prose() / prose_with() / prose_inline(), markup(), and C23 #embed for
// copy that is a file rather than a literal.
#include <unistd.h>

#include <webc/test.h>

// ---------------------------------------------------------------- prose

// The app's own furniture, injected between paragraphs. Declared before its use because
// that is what a view-local helper looks like in a real app, too.
static void pull_after(i32 n, void *ctx);



static void prose_page(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("article") {
    prose("One **bold** word, an *italic* one, `some_code()` and a [link](/about).");
  }
}
GET("/dx3/prose") { return wc_render_fragment(200, prose_page, nullptr); }

TEST(prose_renders_the_inline_vocabulary) {
  Response r = webc_test_request("GET", "/dx3/prose");
  ASSERT_EQ(r.status, 200);
  ASSERT_HTML(r, "article > p > strong");
  ASSERT_HTML_TEXT(r, "article strong", "bold");
  ASSERT_HTML_TEXT(r, "article em", "italic");
  ASSERT_HTML_TEXT(r, "article code", "some_code()");
  ASSERT_HTML(r, "article p a[href=\"/about\"]");
  ASSERT_HTML_TEXT(r, "article a", "link");
}

static void para_page(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("div") {
    prose_with("First.\n\n\n   Second  \n\nThird, and last.",
               .first = "first-p", .class = "para");
  }
}
GET("/dx3/paras") { return wc_render_fragment(200, para_page, nullptr); }

TEST(prose_forces_one_paragraph_per_blank_line) {
  Response r = webc_test_request("GET", "/dx3/paras");
  ASSERT_HTML_COUNT(r, "div > p", 3);
  ASSERT_HTML(r, "div > p.first-p");
  ASSERT_HTML_TEXT(r, "div > p.first-p", "First.");       // the first class does not leak
  ASSERT_STR_CONTAINS(r, "Third, and last.");
  ASSERT_HTML_TEXT(r, "div > p.para", "Second");          // runs of blank lines make no empty paragraph
}

static void pull_after(i32 n, void *ctx) {
  if (n != 2) return;
  el("blockquote") { class_("pull");
    el("p") { text((const char *)ctx); }
  }
}

static void hook_page(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("div") {
    prose_with("One.\n\nTwo.\n\nThree.", .after = pull_after, .ctx = (void *)"A quote.");
  }
}
GET("/dx3/hook") { return wc_render_fragment(200, hook_page, nullptr); }

TEST(prose_runs_a_hook_between_paragraphs) {
  Response r = webc_test_request("GET", "/dx3/hook");
  ASSERT_HTML(r, "div > blockquote.pull");
  ASSERT_HTML_TEXT(r, "blockquote.pull p", "A quote.");
  // ... and it lands where the app asked for it: after the second paragraph, before
  // the third — which is the one thing a pull-quote has to get right.
  const char *quote = strstr(r.body.p, "A quote.");
  const char *three = strstr(r.body.p, "Three.");
  ASSERT_TRUE(quote && three && quote < three);
}

static void safe_page(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("div") {
    prose("**Two** stars, <script>alert(1)</script>, 100% and [x](javascript:alert(1)).");
  }
}
GET("/dx3/safe") { return wc_render_fragment(200, safe_page, nullptr); }

TEST(prose_escapes_text_and_refuses_a_dangerous_link) {
  Response r = webc_test_request("GET", "/dx3/safe");
  ASSERT_HTML(r, "div > p > strong");
  ASSERT_HTML_ABSENT(r, "script");
  ASSERT_STR_CONTAINS(r, "&lt;script&gt;");
  ASSERT_STR_CONTAINS(r, "100%");            // a value, never a format
  ASSERT_HTML_ABSENT(r, "div a");            // javascript: is not a link, it is text
  ASSERT_STR_CONTAINS(r, "javascript:alert(1)");
}

static void unclosed_page(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("div") { prose("A stray **asterisk and a [bracket that never closes."); }
}
GET("/dx3/unclosed") { return wc_render_fragment(200, unclosed_page, nullptr); }

TEST(an_unclosed_marker_stays_text) {
  Response r = webc_test_request("GET", "/dx3/unclosed");
  ASSERT_HTML_ABSENT(r, "div strong");
  ASSERT_STR_CONTAINS(r, "A stray **asterisk");
  ASSERT_STR_CONTAINS(r, "[bracket that never closes");
}

// ---------------------------------------------------------------- inline, and markup

static void inline_page(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("h2") { prose_inline("A **deck** under a headline"); }
}
GET("/dx3/inline") { return wc_render_fragment(200, inline_page, nullptr); }

TEST(prose_inline_does_not_wrap_the_text_in_a_paragraph) {
  Response r = webc_test_request("GET", "/dx3/inline");
  ASSERT_HTML(r, "h2 > strong");
  ASSERT_HTML_ABSENT(r, "h2 p");
  ASSERT_HTML_TEXT(r, "h2", "A deck under a headline");
}

static void markup_page(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("span") { class_("icon"); markup("<svg viewBox=\"0 0 1 1\"><path d=\"M0 0h1\"/></svg>"); }
}
GET("/dx3/markup") { return wc_render_fragment(200, markup_page, nullptr); }

TEST(markup_is_raw_with_a_name_that_states_intent) {
  Response r = webc_test_request("GET", "/dx3/markup");
  ASSERT_HTML(r, "span.icon > svg");
  ASSERT_HTML(r, "span.icon svg path[d]");
}

// ---------------------------------------------------------------- a file, not a literal
//
// The idiom this test is standing on: the copy is a file in the project, the compiler
// splices it in, and no byte of it is escaped by hand. The `, 0` is the trailing NUL —
// #embed's own suffix() parameter is still refused by clang 21, so this is the spelling
// that works on both targets today.

static const char copy_txt[] = {
#embed "fixtures/copy.txt"
    , 0 };

static void file_page(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  el("div") { prose(copy_txt); }
}
GET("/dx3/file") { return wc_render_fragment(200, file_page, nullptr); }

TEST(copy_can_be_a_file_embedded_at_compile_time) {
  Response r = webc_test_request("GET", "/dx3/file");
  ASSERT_EQ(r.status, 200);
  ASSERT_HTML_COUNT(r, "div > p", 2);
  ASSERT_HTML(r, "div > p strong");
  ASSERT_HTML(r, "div > p a[href=\"/about\"]");
  ASSERT_STR_CONTAINS(r, "an ampersand &amp; a &lt;tag&gt;.");
}

TEST_MAIN();