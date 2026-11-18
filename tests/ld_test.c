// Structured data: the vocabulary as one expression, and the one place a <script> body is
// escaped for being a <script> body.
//
// The defect these tests exist for is the third one: a block built with `fmt()` and a
// hand-written string escaper, in which a title containing `</script>` ends the element and
// the rest of the document becomes markup. The first two — nesting by hand, and an absent
// field emitted as `""` — are the same bug's quieter siblings.
#include <webc/ld.h>
#include <webc/test.h>

// ---------------------------------------------------------------- pages under test

static const char *const KEYWORDS[] = {"go", "postgres", nullptr};

static str full_article(void) {
  return ld_blog_posting(
      .url = "https://example.test/article/go-cutover",
      .headline = "Go cutover walkthrough",
      .description = "One binary, one deploy.",
      .image = "https://example.test/media/cover.jpg",
      .author = "Victor H",
      .author_url = "https://example.test/author/victor-h",
      .publisher = "VictorTrades",
      .publisher_logo = "https://example.test/icons/logo.png",
      .published = "2026-10-14T20:31:00Z",
      .modified = "2026-10-15T13:54:19Z",
      .section = "Engineering",
      .language = "en",
      .keywords = KEYWORDS,
      .word_count = 29);
}

static str draft_article(void) {
  return ld_blog_posting(.url = "https://example.test/article/draft",
                         .headline = "A draft has no date",
                         .publisher = "VictorTrades");
}

static const char *PAGE =
    "<!doctype html><html><head><title>t</title></head><body><h1>hi</h1></body></html>";

GET("/ld/full") {
  (void)req;
  head_ld(full_article());
  return res_html(200, str_lit(PAGE));
}

GET("/ld/draft") {
  (void)req;
  head_ld(draft_article());
  return res_html(200, str_lit(PAGE));
}

GET("/ld/hostile") {
  (void)req;
  head_ld(ld_blog_posting(.headline = "A </script><script>alert(1)</script> & title",
                          .publisher = "VictorTrades"));
  return res_html(200, str_lit(PAGE));
}

GET("/ld/nothing") {
  (void)req;
  head_ld(str_lit(""));
  return res_html(200, str_lit(PAGE));
}

// ---------------------------------------------------------------- the vocabulary

TEST(an_article_carries_the_nesting_the_vocabulary_asks_for) {
  str doc = full_article();
  ASSERT_JSON_DOC(doc, "@type", "BlogPosting");
  ASSERT_JSON_DOC(doc, "@context", "https://schema.org");
  ASSERT_JSON_DOC(doc, "headline", "Go cutover walkthrough");
  // author is a Person, and the page it links to is its url
  ASSERT_JSON_DOC(doc, "author.@type", "Person");
  ASSERT_JSON_DOC(doc, "author.name", "Victor H");
  ASSERT_JSON_DOC(doc, "author.url", "https://example.test/author/victor-h");
  // publisher is an Organization with a logo ImageObject — the two fields a rich result wants
  ASSERT_JSON_DOC(doc, "publisher.@type", "Organization");
  ASSERT_JSON_DOC(doc, "publisher.logo.@type", "ImageObject");
  ASSERT_JSON_DOC(doc, "publisher.logo.url", "https://example.test/icons/logo.png");
  // image is an ImageObject, and mainEntityOfPage is an @id, not a bare string
  ASSERT_JSON_DOC(doc, "image.url", "https://example.test/media/cover.jpg");
  ASSERT_JSON_DOC(doc, "mainEntityOfPage.@id", "https://example.test/article/go-cutover");
  ASSERT_JSON_DOC(doc, "keywords.0", "go");
  ASSERT_JSON_DOC(doc, "keywords.1", "postgres");
  ASSERT_JSON_DOC(doc, "wordCount", "29");
  ASSERT_JSON_DOC(doc, "articleSection", "Engineering");
}

TEST(a_field_left_null_is_omitted_not_written_empty) {
  str doc = draft_article();
  ASSERT_JSON_DOC(doc, "headline", "A draft has no date");
  ASSERT_JSON_DOC_ABSENT(doc, "datePublished"); // not "", which is not a date
  ASSERT_JSON_DOC_ABSENT(doc, "author");        // not {"@type":"Person","name":""}
  ASSERT_JSON_DOC_ABSENT(doc, "image");
  ASSERT_JSON_DOC_ABSENT(doc, "keywords");
  ASSERT_JSON_DOC_ABSENT(doc, "wordCount");     // 0 words is not a fact about an article
  ASSERT_JSON_DOC(doc, "publisher.name", "VictorTrades");
  ASSERT_JSON_DOC_ABSENT(doc, "publisher.logo");
}

TEST(the_last_breadcrumb_is_where_the_reader_is) {
  WLdCrumb trail[] = {{"Home", "https://example.test/"},
                      {"Archive", "https://example.test/archive"},
                      {"Go cutover", nullptr}};
  str doc = ld_breadcrumbs(trail, 3);
  ASSERT_JSON_DOC(doc, "@type", "BreadcrumbList");
  ASSERT_JSON_DOC(doc, "itemListElement.0.position", "1");
  ASSERT_JSON_DOC(doc, "itemListElement.1.position", "2");
  ASSERT_JSON_DOC(doc, "itemListElement.2.position", "3");
  ASSERT_JSON_DOC(doc, "itemListElement.2.name", "Go cutover");
  ASSERT_JSON_DOC_ABSENT(doc, "itemListElement.2.item");
}

TEST(a_question_without_an_answer_is_not_a_question) {
  WLdQA qa[] = {{"Where is the archive?", "Under /archive."},
                {"And the answer nobody wrote?", ""}};
  str doc = ld_faq(qa, 2);
  ASSERT_JSON_DOC(doc, "@type", "FAQPage");
  ASSERT_JSON_DOC(doc, "mainEntity.0.name", "Where is the archive?");
  ASSERT_JSON_DOC(doc, "mainEntity.0.acceptedAnswer.text", "Under /archive.");
  ASSERT_JSON_DOC_ABSENT(doc, "mainEntity.1");
}

TEST(a_graph_is_one_document_with_many_things) {
  str site = ld_website(.name = "VictorTrades", .url = "https://example.test/",
                        .publisher = "VictorTrades", .language = "en",
                        .search_url = "https://example.test/search?q={query}");
  str page = full_article();
  str docs[] = {site, page};
  str doc = ld_graph(docs, 2);
  ASSERT_JSON_DOC(doc, "@context", "https://schema.org");
  ASSERT_JSON_DOC(doc, "@graph.0.@type", "WebSite");
  ASSERT_JSON_DOC(doc, "@graph.0.potentialAction.@type", "SearchAction");
  ASSERT_JSON_DOC(doc, "@graph.1.@type", "BlogPosting");
  ASSERT_JSON_DOC(doc, "@graph.1.author.name", "Victor H");
}

TEST(a_person_and_an_organisation_say_what_they_are) {
  const char *const same[] = {"https://example.test/@victor", nullptr};
  str org = ld_organization(.name = "VictorTrades", .url = "https://example.test/",
                            .email = "hello@example.test", .same_as = same);
  ASSERT_JSON_DOC(org, "@type", "Organization");
  ASSERT_JSON_DOC(org, "email", "hello@example.test");
  ASSERT_JSON_DOC(org, "sameAs.0", "https://example.test/@victor");
  ASSERT_JSON_DOC_ABSENT(org, "logo");
  str person = ld_person(.name = "Victor H", .url = "https://example.test/author/victor-h",
                         .job_title = "Writer");
  ASSERT_JSON_DOC(person, "@type", "Person");
  ASSERT_JSON_DOC(person, "jobTitle", "Writer");
}

// ---------------------------------------------------------------- the <script> body

// JSON's own escaping does not cover the context. This is the defect, as a test: what a title
// containing a closing script tag must not do.
TEST(a_title_cannot_close_the_script_block) {
  Response r = webc_test_request("GET", "/ld/hostile");
  ASSERT_EQ(r.status, 200);
  str body = WC_TEST_VIEW(r);
  ASSERT_STR_CONTAINS(r, "application/ld+json");
  ASSERT_FALSE(str_contains(body, str_lit("</script><script>")));
  ASSERT_STR_CONTAINS(r, "\\u003c/script\\u003e");
  ASSERT_STR_CONTAINS(r, "\\u0026 title");
}

// …and the escaped document is the same document: a JSON reader sees the characters the page
// meant, which is what makes the escaping a transport detail rather than a change of meaning.
TEST(escaping_for_the_script_context_keeps_the_string) {
  Arena *a = wc_arena_get();
  str raw = ld_blog_posting(.headline = "A </script> & <b>bold</b> title\u2028twice");
  str esc = json_for_script(a, raw);
  ASSERT_TRUE(str_contains(esc, str_lit("\\u2028")));
  ASSERT_FALSE(str_contains(esc, str_lit("</script")));
  JParse jp = json_parse(a, esc);
  ASSERT_TRUE(jp.ok);
  str headline = json_as_str(json_get(jp.v, "headline"));
  ASSERT_STR_CONTAINS(headline, "A </script> & <b>bold</b> title");
}

TEST(an_empty_document_writes_no_block_at_all) {
  Response r = webc_test_request("GET", "/ld/nothing");
  ASSERT_EQ(r.status, 200);
  ASSERT_FALSE(str_contains(WC_TEST_VIEW(r), str_lit("ld+json"))); // empty is invalid, not neutral
}

TEST(assert_ld_reads_a_page_the_way_a_crawler_would) {
  Response r = webc_test_request("GET", "/ld/full");
  ASSERT_LD(r, "headline", "Go cutover walkthrough");
  ASSERT_LD(r, "author.name", "Victor H");
  ASSERT_LD(r, "publisher.name", "VictorTrades");
  ASSERT_LD(r, "wordCount", "29");
  Response d = webc_test_request("GET", "/ld/draft");
  ASSERT_LD(d, "headline", "A draft has no date");
  ASSERT_LD_ABSENT(d, "datePublished");
}

// ---------------------------------------------------------------- the writer

TEST(an_object_block_closes_itself_even_when_the_body_breaks) {
  JsonWriter w = jw_doc();
  jw_obj_begin(&w);
  jw_obj_in(&w, "author") {
    jw_kv_cstr(&w, "name", "Ada");
    jw_arr_in(&w, "roles") {
      jw_str(&w, str_lit("writer"));
      break; // the cleanup still closes the array, and the object around it
    }
  }
  jw_kv_cstr(&w, "title", "");
  jw_obj_end(&w);
  str doc = jw_finish(&w);
  JParse jp = json_parse(wc_arena_get(), doc);
  ASSERT_TRUE(jp.ok);
  ASSERT_JSON_DOC(doc, "author.name", "Ada");
  ASSERT_JSON_DOC(doc, "author.roles.0", "writer");
  ASSERT_JSON_DOC_ABSENT(doc, "title"); // a pair with no value writes neither
}
TEST_MAIN();
