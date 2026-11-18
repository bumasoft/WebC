// feed: an RSS document written by a writer that escapes as it goes, and the same feed for
// a model without a loop. The alternative is thirty-five `buf_puts` and fifteen lines of
// hand-written escaping per app — with the same three mistakes every time: text escaped
// like an attribute, a `<pubDate>` that is not RFC 822, and a `<guid>` that is not stable.
#include <webc/test.h>

WEBC_MODEL(Post, (id, i64, .pk = true),
                 (slug, Text, .required = true),
                 (title, Text, .required = true),
                 (dek, Text),
                 (published_at, Text))
WEBC_MODEL_ROUTE(Post, "/p/:slug")
WEBC_VEC_DEFINE(VecPost, vec_post, Post)

TEST(the_writer_escapes_once_on_the_way_in) {
  Arena *a = arena_new(4096);
  Buf b;
  xml_init(&b, a);
  xml_decl(&b);
  xml_open(&b, "rss", "version=\"2.0\"");
  xml_open(&b, "channel", nullptr);
  xml_el(&b, "title", str_lit("Salt & <wind>"));
  xml_el(&b, "description", str_lit("a \"quoted\" 'word'"));
  xml_close(&b, "channel");
  xml_close(&b, "rss");
  str doc = buf_str(&b);
  ASSERT_STR_CONTAINS(doc, "<?xml version=\"1.0\" encoding=\"utf-8\"?>");
  ASSERT_STR_CONTAINS(doc, "<title>Salt &amp; &lt;wind&gt;</title>");
  ASSERT_STR_CONTAINS(doc, "<description>a &quot;quoted&quot; &apos;word&apos;</description>");
  ASSERT_STR_CONTAINS(doc, "</channel></rss>");
  arena_destroy(a);
}

TEST(an_empty_field_is_left_out_rather_than_left_empty) {
  Arena *a = arena_new(4096);
  Buf b;
  xml_init(&b, a);
  xml_el(&b, "description", (str){nullptr, 0});
  ASSERT_STR_EQ(buf_str(&b), "");
  arena_destroy(a);
}

TEST(rfc822_is_what_a_pubdate_wants) {
  ASSERT_STR_EQ(rfc822(str_lit("2026-10-11T09:40:00Z")), "Sun, 11 Oct 2026 09:40:00 +0000");
  ASSERT_STR_EQ(rfc822(str_lit("2026-01-01")), "Thu, 01 Jan 2026 00:00:00 +0000");
  ASSERT_STR_EQ(rfc822(str_lit("")), "");
}

TEST(a_feed_for_a_model_needs_no_loop) {
  VecPost all = vec_post_new();
  for (i32 i = 0; i < 3; i++) {
    Post p = {.slug = str_cstr(nullptr, str_fmt(nullptr, "post-%d", (int)i)),
              .title = "T & T", .dek = "the deck", .published_at = "2026-10-11T09:40:00Z"};
    vec_post_push(&all, p);
  }
  Response r =
      res_feed_rss(200, &all, Post, title, dek, published_at, "Posts", "https://x.example/");
  str doc = (str){r.body.p, r.body.n};
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_CONTAINS(doc, "<title>Posts</title>");
  ASSERT_STR_CONTAINS(doc, "<link>https://x.example/</link>");
  ASSERT_STR_CONTAINS(doc, "<link>/p/post-0</link>");
  ASSERT_STR_CONTAINS(doc, "<guid>/p/post-0</guid>");
  ASSERT_STR_CONTAINS(doc, "<title>T &amp; T</title>");
  ASSERT_STR_CONTAINS(doc, "<pubDate>Sun, 11 Oct 2026 09:40:00 +0000</pubDate>");
}

TEST(a_misspelt_field_names_itself_and_the_real_ones) {
  VecPost all = vec_post_new();
  // `dek` is the field, `deck` is not: the feed says which, and what it could have said,
  // instead of shipping an empty <description> that nobody notices for a month
  ASSERT_PANICS(res_feed_rss(200, &all, Post, title, deck, published_at, "Posts", "https://x/"));
}

TEST_MAIN()