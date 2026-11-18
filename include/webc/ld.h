// SPDX-License-Identifier: Apache-2.0
// webc/ld.h — structured data: the facts a page states, in the shape a crawler reads.
//
// A page already knows what it is about. The <title> says it, the og:* tags say it, the
// article says it in prose — and then the JSON-LD block repeats it in a format string,
// hand-nested, hand-escaped, and wrong in the ways a format string is always wrong:
//
//   head_html(fmt("<script type=\"application/ld+json\">{\"@type\":\"BlogPosting\","
//                 "\"headline\":\"%s\",\"author\":{\"@type\":\"Person\",\"name\":\"%s\"}…",
//                 json_str_esc(p->title), json_str_esc(p->author_name)));
//
// Three things are wrong with that and none of them is the app's fault: the nesting is
// vocabulary knowledge re-typed per page, an absent field becomes `""` (which is not a date
// and not a name), and the string is built next to a `<script>` tag by hand.
//
//   #include <webc/ld.h>
//
//   head_ld(ld_blog_posting(.url = abs_url(path),
//                           .headline = p->title,
//                           .description = desc,
//                           .image = cover_url,
//                           .author = p->author_name,
//                           .publisher = brand(site),
//                           .published = p->published_at,
//                           .modified = p->updated_at,
//                           .word_count = p->word_count));
//
// **What it guarantees.** Each function returns one JSON document for one schema.org type,
// already nested the way the vocabulary nests: `author` is a `Person`, `publisher` is an
// `Organization` with a `logo` `ImageObject`, `mainEntityOfPage` is `{"@id": …}`. A field you
// leave null is *omitted*, never emitted empty — `"datePublished":""` is a claim that the page
// was published at an unparseable moment, and a crawler's answer to it is to drop the whole
// result. `head_ld()` is the door: it escapes the document for a `<script>` body, so a title
// containing `</script>` cannot end the element (see `json_for_script`).
//
// **What it is not.** Not a validator and not a generator: a type that is not here is four
// `jw_*` calls in your own function (that is what these are). Dates are passed through as you
// hold them — ISO-8601 with a zone is what the vocabulary wants, and `time.h` produces it.
// URLs must already be absolute: `abs_url()` on the server, and nothing guesses a host here.
#pragma once

#include <webc/core.h>
#include <webc/json.h>

// ---------------------------------------------------------------- the shapes
//
// Fields are C strings, because they come from a row or from `fmt()`; a value that is already
// a `str` is `text_of(s)`. A null field is omitted from the document.

typedef struct WLdArticle {
  const char *type;            // nullptr = "BlogPosting"; "Article", "NewsArticle", "TechArticle"
  const char *url;             // absolute; becomes `@id` and `mainEntityOfPage.@id`
  const char *headline;        // the article's title, not the page's (`… — Brand`)
  const char *description;     // one sentence, the same one the meta description uses
  const char *image;           // absolute URL of the cover
  const char *author;          // a person's name
  const char *author_url;      // the page that person's other work lives on
  const char *publisher;       // the site's name
  const char *publisher_logo;  // absolute URL; omitted when the site has no logo
  const char *published;       // ISO-8601, e.g. 2026-10-14T20:31:00Z
  const char *modified;        // ISO-8601
  const char *section;         // "Engineering" — the category a reader would name
  const char *language;        // "en"
  const char *const *keywords; // null-terminated, or nullptr
  i64 word_count;              // 0 = omitted
} WLdArticle;

typedef struct WLdWebsite {
  const char *name;
  const char *url;         // absolute, the home page
  const char *description;
  const char *language;
  const char *publisher;   // an organisation name
  const char *search_url;  // absolute URL template with `{query}` where the words go
} WLdWebsite;

typedef struct WLdOrg {
  const char *name;
  const char *url;
  const char *logo;        // absolute
  const char *description;
  const char *email;
  const char *phone;
  const char *const *same_as; // null-terminated profile URLs (a social account, a feed)
} WLdOrg;

typedef struct WLdPerson {
  const char *name;
  const char *url;
  const char *job_title;
  const char *description;
  const char *image;       // absolute
  const char *const *same_as;
} WLdPerson;

typedef struct WLdCrumb {
  const char *name;
  const char *url;         // null on the last one: the trail ends where the reader is
} WLdCrumb;

typedef struct WLdQA {
  const char *question;
  const char *answer;      // plain text; the same words the page shows
} WLdQA;

// ---------------------------------------------------------------- the pieces they share

// A nested object with a `@type` and its string fields, skipped whole when `name` is empty.
static inline void wc_ld_named_in(JsonWriter *w, const char *key, const char *type,
                                  const char *name, const char *url) {
  if (!name || !*name) return;
  jw_obj_in(w, key) {
    jw_key(w, "@type");
    jw_cstr(w, type);
    jw_kv_cstr(w, "name", name);
    jw_kv_cstr(w, "url", url);
  }
}

// `"image": {"@type": "ImageObject", "url": …}` — the shape the vocabulary wants, rather than
// a bare string, so an image can grow a width and a caption without changing the page.
static inline void wc_ld_image_in(JsonWriter *w, const char *key, const char *url) {
  if (!url || !*url) return;
  jw_obj_in(w, key) {
    jw_key(w, "@type");
    jw_cstr(w, "ImageObject");
    jw_kv_cstr(w, "url", url);
  }
}

static inline void wc_ld_org_in(JsonWriter *w, const char *key, const char *name,
                                const char *logo) {
  if (!name || !*name) return;
  jw_obj_in(w, key) {
    jw_key(w, "@type");
    jw_cstr(w, "Organization");
    jw_kv_cstr(w, "name", name);
    wc_ld_image_in(w, "logo", logo);
  }
}

// The document's opening: the context every document carries, and the type of the thing.
static inline JsonWriter wc_ld_open(const char *type) {
  JsonWriter w;
  jw_init(&w, wc_arena_get(), 512);
  jw_obj_begin(&w);
  jw_key(&w, "@context");
  jw_cstr(&w, "https://schema.org");
  jw_key(&w, "@type");
  jw_cstr(&w, type);
  return w;
}

// The closing. A writer that ran out of depth hands back nothing rather than half a document:
// half a document is invalid JSON, and invalid JSON in a <script> is a parse error a reader
// sees in the console and a crawler discards.
static inline str wc_ld_close(JsonWriter *w) {
  jw_obj_end(w);
  if (w->failed) return (str){nullptr, 0};
  return jw_finish(w);
}

// ---------------------------------------------------------------- the types

// An article: the one type almost every site with a page of prose emits.
static inline str wc_ld_article(WLdArticle a) {
  JsonWriter w = wc_ld_open(a.type && *a.type ? a.type : "BlogPosting");
  jw_kv_cstr(&w, "@id", a.url);
  jw_kv_cstr(&w, "headline", a.headline);
  jw_kv_cstr(&w, "description", a.description);
  if (a.url && *a.url)
    jw_obj_in(&w, "mainEntityOfPage") { jw_kv_cstr(&w, "@id", a.url); }
  wc_ld_image_in(&w, "image", a.image);
  wc_ld_named_in(&w, "author", "Person", a.author, a.author_url);
  wc_ld_org_in(&w, "publisher", a.publisher, a.publisher_logo);
  jw_kv_cstr(&w, "datePublished", a.published);
  jw_kv_cstr(&w, "dateModified", a.modified);
  jw_kv_cstr(&w, "articleSection", a.section);
  jw_kv_cstr(&w, "inLanguage", a.language);
  jw_kv_str_list(&w, "keywords", a.keywords);
  if (a.word_count > 0) jw_kv_i64(&w, "wordCount", a.word_count);
  return wc_ld_close(&w);
}

static inline str wc_ld_website(WLdWebsite s) {
  JsonWriter w = wc_ld_open("WebSite");
  jw_kv_cstr(&w, "@id", s.url);
  jw_kv_cstr(&w, "name", s.name);
  jw_kv_cstr(&w, "url", s.url);
  jw_kv_cstr(&w, "description", s.description);
  jw_kv_cstr(&w, "inLanguage", s.language);
  wc_ld_named_in(&w, "publisher", "Organization", s.publisher, nullptr);
  if (s.search_url && *s.search_url) {
    // The one field with a placeholder in it: {query} is where the words go.
    jw_obj_in(&w, "potentialAction") {
      jw_key(&w, "@type");
      jw_cstr(&w, "SearchAction");
      jw_obj_in(&w, "target") {
        jw_key(&w, "@type");
        jw_cstr(&w, "EntryPoint");
        jw_kv_cstr(&w, "urlTemplate", s.search_url);
      }
      jw_kv_cstr(&w, "query-input", "required name=query");
    }
  }
  return wc_ld_close(&w);
}

static inline str wc_ld_organization(WLdOrg o) {
  JsonWriter w = wc_ld_open("Organization");
  jw_kv_cstr(&w, "@id", o.url);
  jw_kv_cstr(&w, "name", o.name);
  jw_kv_cstr(&w, "url", o.url);
  wc_ld_image_in(&w, "logo", o.logo);
  jw_kv_cstr(&w, "description", o.description);
  jw_kv_cstr(&w, "email", o.email);
  jw_kv_cstr(&w, "telephone", o.phone);
  jw_kv_str_list(&w, "sameAs", o.same_as);
  return wc_ld_close(&w);
}

static inline str wc_ld_person(WLdPerson p) {
  JsonWriter w = wc_ld_open("Person");
  jw_kv_cstr(&w, "@id", p.url);
  jw_kv_cstr(&w, "name", p.name);
  jw_kv_cstr(&w, "url", p.url);
  jw_kv_cstr(&w, "jobTitle", p.job_title);
  jw_kv_cstr(&w, "description", p.description);
  wc_ld_image_in(&w, "image", p.image);
  jw_kv_str_list(&w, "sameAs", p.same_as);
  return wc_ld_close(&w);
}

// A trail: Home › Archive › This article. Positions count from 1 and are this function's job,
// because a hand-written `position` is the field that goes wrong first when the trail is edited.
static inline str wc_ld_breadcrumbs(const WLdCrumb *crumbs, i32 n) {
  JsonWriter w = wc_ld_open("BreadcrumbList");
  if (n > 0 && crumbs) {
    jw_key(&w, "itemListElement");
    jw_arr_begin(&w);
    for (i32 i = 0; i < n; i++) {
      if (!crumbs[i].name || !*crumbs[i].name) continue;
      jw_obj_begin(&w);
      jw_key(&w, "@type");
      jw_cstr(&w, "ListItem");
      jw_kv_i64(&w, "position", i + 1);
      jw_kv_cstr(&w, "name", crumbs[i].name);
      jw_kv_cstr(&w, "item", crumbs[i].url);
      jw_obj_end(&w);
    }
    jw_arr_end(&w);
  }
  return wc_ld_close(&w);
}

// Questions the page answers in its own words. An entry with no answer is skipped: a question
// with no answer is what makes a page ineligible for the result it is asking for.
static inline str wc_ld_faq(const WLdQA *qa, i32 n) {
  JsonWriter w = wc_ld_open("FAQPage");
  if (n > 0 && qa) {
    jw_key(&w, "mainEntity");
    jw_arr_begin(&w);
    for (i32 i = 0; i < n; i++) {
      if (!qa[i].question || !*qa[i].question || !qa[i].answer || !*qa[i].answer) continue;
      jw_obj_begin(&w);
      jw_key(&w, "@type");
      jw_cstr(&w, "Question");
      jw_kv_cstr(&w, "name", qa[i].question);
      jw_obj_in(&w, "acceptedAnswer") {
        jw_key(&w, "@type");
        jw_cstr(&w, "Answer");
        jw_kv_cstr(&w, "text", qa[i].answer);
      }
      jw_obj_end(&w);
    }
    jw_arr_end(&w);
  }
  return wc_ld_close(&w);
}

// Several things about one page — the site, the article, the trail — as one graph. A crawler
// reads a single document and links the pieces by `@id`, which is what `@id` is for.
static inline str wc_ld_graph(const str *docs, i32 n) {
  JsonWriter w;
  jw_init(&w, wc_arena_get(), 1024);
  jw_obj_begin(&w);
  jw_key(&w, "@context");
  jw_cstr(&w, "https://schema.org");
  jw_key(&w, "@graph");
  jw_arr_begin(&w);
  for (i32 i = 0; i < n; i++)
    if (docs[i].p && docs[i].n) jw_raw(&w, docs[i]);
  jw_arr_end(&w);
  return wc_ld_close(&w);
}

// ---------------------------------------------------------------- the call sites

#define ld_article(...) wc_ld_article((WLdArticle){__VA_ARGS__})
#define ld_blog_posting(...)                                                                   \
  wc_ld_article((WLdArticle){.type = "BlogPosting" __VA_OPT__(, ) __VA_ARGS__})
#define ld_news_article(...)                                                                   \
  wc_ld_article((WLdArticle){.type = "NewsArticle" __VA_OPT__(, ) __VA_ARGS__})
#define ld_website(...) wc_ld_website((WLdWebsite){__VA_ARGS__})
#define ld_organization(...) wc_ld_organization((WLdOrg){__VA_ARGS__})
#define ld_person(...) wc_ld_person((WLdPerson){__VA_ARGS__})
#define ld_breadcrumbs(crumbs_, n_) wc_ld_breadcrumbs((crumbs_), (n_))
#define ld_faq(qa_, n_) wc_ld_faq((qa_), (n_))
#define ld_graph(docs_, n_) wc_ld_graph((docs_), (n_))