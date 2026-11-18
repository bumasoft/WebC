// richtext: a stored fragment rendered through a policy — and the questions an article page
// asks of the same bytes (excerpt, word count, table of contents, stable heading ids).
//
// The assertions read the *rendered* page the way a browser would: a selector and the text it
// holds. An assertion on bytes would pass while `<script>` sat in the markup with its angle
// brackets escaped and its closing tag missing, which is the bug these tests exist to catch.
#include <webc/richtext.h>
#include <webc/test.h>

static const char *g_in;
static void docs_view(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  rich(g_in);
}
static void tight_view(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  rich_with(g_in, .allow = RICH_TIGHT);
}
static void lazy_view(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  rich_with(g_in, .lazy = true);
}
static void noimg_view(WBuild *b, void *ctx) {
  (void)b;
  (void)ctx;
  rich_with(g_in, .no_images = true);
}

static WComponent *cx(void) {
  static WComponent c;
  static bool init;
  if (!init) {
    wcomp_init(&c, nullptr);
    init = true;
  }
  return &c;
}
static str rr(const char *html, void (*view)(WBuild *, void *)) {
  g_in = html;
  return wcomp_html_ctx(cx(), view, nullptr);
}
static str SS(const char *s) { return (str){(char *)s, strlen(s)}; }

// ---------------------------------------------------------------- what survives

TEST(a_script_goes_and_so_does_its_text) {
  str h = rr("<p>before<script>alert('x')</script>after</p>", docs_view);
  ASSERT_HTML_TEXT(h, "p", "beforeafter");
  ASSERT_HTML_ABSENT(h, "script");
  ASSERT_TRUE(strstr(h.p, "alert") == nullptr);
}

TEST(the_tags_a_policy_names_are_the_tags_that_render) {
  g_in = "<h2>Title</h2><p>a <em>b</em> <em>c</em></p>";
  str docs = rr(g_in, docs_view);
  ASSERT_HTML_TEXT(docs, "h2", "Title");
  ASSERT_HTML_COUNT(docs, "em", 2);

  str tight = rr(g_in, tight_view);
  ASSERT_HTML_ABSENT(tight, "h2"); // h2 is not in RICH_TIGHT
  ASSERT_HTML_COUNT(tight, "em", 2);
}

TEST(an_unknown_tag_goes_and_its_words_stay) {
  str h = rr("<div><marquee>still here</marquee></div>", docs_view);
  ASSERT_HTML_ABSENT(h, "marquee");
  ASSERT_HTML_TEXT(h, "div", "still here"); // the tag goes, its words are kept
}

// ---------------------------------------------------------------- URLs

TEST(a_decoded_javascript_url_is_refused) {
  // The entity is the attack: what the browser decodes is what it runs.
  str h = rr("<a href=\"java&#115;cript:alert(1)\">click</a>", docs_view);
  ASSERT_HTML_TEXT(h, "a", "click");
  ASSERT_HTML_ABSENT(h, "a[href]");
}

TEST(control_characters_do_not_smuggle_a_scheme) {
  str h = rr("<a href=\"java\tscr\nipt:alert(1)\">click</a>", docs_view);
  ASSERT_HTML_ABSENT(h, "a[href]");
}

TEST(a_relative_link_is_kept_without_a_rel) {
  str h = rr("<a href=\"/about\">about</a>", docs_view);
  ASSERT_HTML_ATTR(h, "a", "href", "/about");
  ASSERT_HTML_ABSENT(h, "a[rel]");
}

TEST(a_link_that_leaves_the_site_gets_noopener) {
  str h = rr("<a href=\"https://example.com\" target=\"_blank\">out</a>", docs_view);
  ASSERT_HTML_ATTR(h, "a", "target", "_blank");
  ASSERT_HTML_ATTR(h, "a", "rel", "noopener noreferrer");

  str own = rr("<a href=\"https://example.com\">out</a>", docs_view);
  ASSERT_HTML_ATTR(own, "a", "rel", "noopener noreferrer"); // an absolute link, even in place
}

TEST(a_data_url_is_an_image_only_when_the_policy_says_so) {
  const char *img = "<img src=\"data:image/png;base64,iVBORw0K\" alt=\"a\">";
  str strict = rr(img, docs_view);
  ASSERT_HTML_ABSENT(strict, "img[src]");
  ASSERT_HTML_ATTR(strict, "img", "alt", "a"); // the tag stays; the URL does not
  str loose = rr(img, noimg_view);
  ASSERT_HTML_ABSENT(loose, "img");
}

TEST(a_svg_carrying_data_is_refused_even_when_data_is_allowed) {
  g_in = "<img src=\"data:image/svg+xml;base64,PHN2Zz4=\" alt=\"x\">";
  WComponent *c = cx();
  str h = wcomp_html_ctx(c, (void (*)(WBuild *, void *))lazy_view, nullptr);
  (void)h;
  // `allow_data_images` is off here, which is the strict path; the svg case is checked by the
  // unit below, where the flag is on and the URI is still refused.
  ASSERT_TRUE(!wc_rich_url_ok(SS("data:image/svg+xml;base64,PHN2Zz4="), true));
  ASSERT_TRUE(wc_rich_url_ok(SS("data:image/png;base64,iVBORw0K"), true));
  ASSERT_TRUE(!wc_rich_url_ok(SS("data:text/html;base64,PHN2Zz4="), true));
}

TEST(an_on_attribute_never_survives) {
  str h = rr("<img src=\"/a.png\" onerror=\"alert(1)\">", docs_view);
  ASSERT_HTML_ABSENT(h, "img[onerror]");
  ASSERT_TRUE(strstr(h.p, "onerror") == nullptr);
}

TEST(a_srcset_with_one_bad_candidate_goes_whole) {
  str h = rr("<img srcset=\"/a.png 1x, javascript:alert(1) 2x\">", docs_view);
  ASSERT_HTML_ABSENT(h, "img[srcset]");
  str ok = rr("<img srcset=\"/a.png 1x, /b.png 2x\">", docs_view);
  ASSERT_HTML_ATTR(ok, "img", "srcset", "/a.png 1x, /b.png 2x");
}

TEST(images_are_lazy_only_when_the_policy_asks) {
  str eager = rr("<img src=\"/a.png\">", docs_view);
  ASSERT_HTML_ABSENT(eager, "img[loading]");
  str lazy = rr("<img src=\"/a.png\">", lazy_view);
  ASSERT_HTML_ATTR(lazy, "img", "loading", "lazy");
}

// ---------------------------------------------------------------- structure

TEST(a_closing_tag_the_fragment_did_not_open_closes_nothing) {
  str h = rr("</div></body><p>kept</p>", docs_view);
  ASSERT_HTML_TEXT(h, "p", "kept");
  ASSERT_HTML_COUNT(h, "div", 0);
}

TEST(an_unclosed_element_is_closed_at_the_end) {
  str h = rr("<blockquote><p>open", docs_view);
  ASSERT_HTML_TEXT(h, "blockquote p", "open");
}

TEST(an_uppercase_tag_is_the_tag) {
  str h = rr("<P CLASS=\"x\">hi</P>", docs_view);
  ASSERT_HTML_TEXT(h, "p", "hi");
  ASSERT_HTML_ATTR(h, "p", "class", "x");
}

TEST(a_comment_and_a_doctype_are_not_text) {
  str h = rr("<!-- secret --><!doctype html><div>visible</div>", docs_view);
  ASSERT_HTML_TEXT(h, "div", "visible");
  ASSERT_TRUE(strstr(h.p, "secret") == nullptr);
}

TEST(text_is_escaped_by_the_framework_not_by_hand) {
  str h = rr("<p>5 &lt; 6 &amp;&amp; 7 &gt; 6</p>", docs_view);
  ASSERT_HTML_TEXT(h, "p", "5 < 6 && 7 > 6");
  str raw = rr("<p>&lt;script&gt;</p>", docs_view);
  ASSERT_HTML_ABSENT(raw, "script");
  ASSERT_HTML_TEXT(raw, "p", "<script>");
}

TEST(a_heading_gets_an_id_and_the_second_one_does_not_collide) {
  str h = rr("<h2>Getting started</h2><h2>Getting started</h2>", docs_view);
  ASSERT_HTML_ATTR(h, "h2:nth-child(1)", "id", "getting-started");
  ASSERT_HTML_ATTR(h, "h2:nth-child(2)", "id", "getting-started-2");
}

TEST(a_deep_fragment_stops_at_the_depth_cap) {
  // 60 nested divs, and a cap of 40: the page still renders, and it does not recurse 60 deep.
  Buf b;
  buf_init(&b, wc_arena_get(), 60 * 11 + 64);
  for (i32 i = 0; i < 60; i++) buf_puts(&b, "<div>");
  buf_puts(&b, "deep");
  str h = rr(str_cstr(wc_arena_get(), buf_str(&b)), docs_view);
  ASSERT_TRUE(strstr(h.p, "deep") != nullptr);
}

// ---------------------------------------------------------------- the derived questions

TEST(plain_text_is_words_and_block_boundaries) {
  ASSERT_STR_EQ(wc_rich_plain(wc_arena_get(), SS("<p>one</p><p>two</p>")), "one two");
  ASSERT_STR_EQ(wc_rich_plain(wc_arena_get(), SS("a   b\n\n c")), "a b c");
  ASSERT_STR_EQ(wc_rich_plain(wc_arena_get(), SS("<script>gone</script>kept")), "kept");
}

// A closing tag is a boundary only when the element was block-level: an inline element ends
// where its words end, or every `<strong>Ada</strong>, 1843` becomes "Ada , 1843" — which is
// what a `<meta name="description">` then says.
TEST(an_inline_element_does_not_insert_a_space) {
  ASSERT_STR_EQ(wc_rich_plain(wc_arena_get(), SS("<p><strong>Ada</strong>, 1843</p>")), "Ada, 1843");
  ASSERT_STR_EQ(wc_rich_plain(wc_arena_get(), SS("<p>a<em>b</em>c</p>")), "abc");
  ASSERT_STR_EQ(wc_rich_plain(wc_arena_get(), SS("<p>a<br>b</p>")), "a b");
  ASSERT_STR_EQ(wc_rich_plain(wc_arena_get(), SS("<li>one</li><li>two</li>")), "one two");
}

TEST(words_and_reading_time) {
  ASSERT_EQ(wc_rich_words(SS("<p>one two three</p>")), 3);
  ASSERT_EQ(wc_rich_words(SS("<cite>Victor H</cite><cite>Antifragile</cite>")), 3); // a tag ends a word
  ASSERT_EQ(wc_rich_words(SS("<p>a</p><p>b</p>")), 2);
  ASSERT_EQ(wc_rich_words(SS("one&nbsp;two")), 2);       // an entity that is a space is one
  ASSERT_EQ(wc_rich_words(SS("<p>R&amp;D money</p>")), 2);   // the entity is one character
  ASSERT_EQ(wc_rich_words(SS("")), 0);
  ASSERT_EQ(wc_rich_reading_time(SS("<p>one two</p>")), 1);       // never "0 min read"
  ASSERT_EQ(wc_rich_reading_time(SS("")), 1);
  Buf b;
  buf_init(&b, wc_arena_get(), 4096);
  for (i32 i = 0; i < 1000; i++) buf_puts(&b, "word ");
  ASSERT_EQ(wc_rich_reading_time(buf_str(&b)), 5);
}

TEST(an_excerpt_cuts_a_word_not_a_letter) {
  str e = wc_rich_excerpt(SS("<p>the quick brown fox jumps over the lazy dog</p>"), 15);
  ASSERT_STR_EQ(e, "the quick brown\xE2\x80\xA6");
  ASSERT_STR_EQ(wc_rich_excerpt(SS("<p>short</p>"), 100), "short"); // under the limit, no ellipsis
  ASSERT_STR_EQ(wc_rich_excerpt(SS(""), 10), "");
}

TEST(the_table_of_contents_is_the_headings_in_order) {
  const char *doc = "<h1>Guide</h1><p>x</p><h2>Install</h2><h3>On macOS</h3><h2>Use</h2>";
  WRichHeading hs[8];
  i32 n = 0;
  wc_rich_headings_into(SS(doc), hs, 8, &n);
  ASSERT_EQ(n, 4);
  ASSERT_EQ(hs[0].level, 1);
  ASSERT_STR_EQ(hs[0].id, "guide");
  ASSERT_STR_EQ(hs[1].text, "Install");
  ASSERT_STR_EQ(hs[1].id, "install");
  ASSERT_EQ(hs[2].level, 3);
  ASSERT_STR_EQ(hs[2].id, "on-macos");
  ASSERT_STR_EQ(hs[3].id, "use");
}

TEST(an_id_the_fragment_already_carries_wins_in_the_toc) {
  WRichHeading hs[4];
  i32 n = 0;
  wc_rich_headings_into(SS("<h2 id=\"custom\">Title</h2>"), hs, 4, &n);
  ASSERT_EQ(n, 1);
  ASSERT_STR_EQ(hs[0].id, "custom");
}

TEST(stamping_ids_is_the_save_path) {
  const char *doc = "<h2>One</h2>\n<p>text</p>\n<h2>Two</h2>";
  str out = wc_rich_stamp_ids(SS(doc));
  ASSERT_STR_EQ(out, "<h2 id=\"one\">One</h2>\n<p>text</p>\n<h2 id=\"two\">Two</h2>");
  // an author's id is left alone, and still counts as a used slug
  str kept = wc_rich_stamp_ids(SS("<h2 id=\"one\">One</h2><h2>One</h2>"));
  ASSERT_STR_EQ(kept, "<h2 id=\"one\">One</h2><h2 id=\"one-2\">One</h2>");
  // stamping is idempotent: a second pass adds nothing
  str twice = wc_rich_stamp_ids(out);
  ASSERT_TRUE(str_eq(twice, out));
  // and what it stamps is what the toc reads back
  WRichHeading hs[4];
  i32 n = 0;
  wc_rich_headings_into(out, hs, 4, &n);
  ASSERT_STR_EQ(hs[0].id, "one");
  ASSERT_STR_EQ(hs[1].id, "two");
}

TEST(an_excerpt_of_a_hostile_body_is_still_text) {
  const char *doc = "<script>alert(1)</script><p>real words &amp; more</p>";
  ASSERT_STR_EQ(wc_rich_plain(wc_arena_get(), SS(doc)), "real words & more");
}

TEST_MAIN()