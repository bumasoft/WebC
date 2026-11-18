// SPDX-License-Identifier: Apache-2.0
// webc/test.h — the test surface (SPEC §6.7).
//
//   #include <webc/test.h>
//
//   TEST(json_roundtrip) {
//     Account a = { .id = 1, .name = str_lit("ada") };
//     str out = try(json_encode(&a));       // a failing try() fails this test
//     ASSERT_STR_EQ(out, "{\"id\":1,\"name\":\"ada\"}");
//   }
//
//   TEST(parses_numbers) {
//     static const struct { const char *name, *in; i64 want; } rows[] = {
//       { "zero", "0", 0 }, { "negative", "-7", -7 },
//     };
//     SUBTESTS(rows) {
//       i64 got = 0;
//       ASSERT_TRUE(str_to_i64(str_lit(r->in), &got));
//       ASSERT_EQ(got, r->want);
//     }
//   }
//
//   TEST_MAIN();
//
// `webc test` compiles every tests/*_test.c into its own binary and runs it;
// the output is go test's shape:
//
//   == json_test.c
//     --- PASS: json_roundtrip (0.00s)
//     --- FAIL: parses_numbers (0.00s)
//         --- PASS: parses_numbers/zero
//         --- FAIL: parses_numbers/negative
//             json_test.c:41: got -6, want -7
//     1 passed, 1 failed
//
// A test never takes the suite down: a panic inside a test (an ASSERT does not
// panic, but `try`, `wc_assert()` and any framework panic do) is caught, the
// test is reported as failed with the panic message, and the next test runs.
#ifndef WEBC_TEST_H
#define WEBC_TEST_H

#include <webc/server.h>
#include <webc/web.h>

// ---------------------------------------------------------------- registry
// Implemented in the library, so a test file is just TEST blocks.
typedef void (*WcTestFn)(void);
void wc_test_register(const char *name, WcTestFn fn);
int wc_test_run(void);

void wc_test_expect(bool ok, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void wc_test_failf(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void wc_test_skip(const char *file, int line, const char *why);
const char *wc_test_str(str s);   // printable, truncated, never null

// Assertions about text take a str, a Buf (a Response body) or a C string.
static inline str wc_test_view_str(str v) { return v; }
static inline str wc_test_view_buf(Buf v) { return (str){ .p = v.p, .n = v.n }; }
static inline str wc_test_view_c(const char *v) {
  return v ? (str){ .p = (char *)v, .n = strlen(v) } : (str){ nullptr, 0 };
}
// A whole Response, so ASSERT_STR_CONTAINS(res, "…") reads what a test means,
// without reaching for `res.body` and casting it to Buf.
static inline str wc_test_view_res(Response r) {
  return (str){ .p = r.body.p, .n = r.body.n ? r.body.n : 0 };
}
#define WC_TEST_VIEW(x_)                                                       \
  _Generic((x_), str: wc_test_view_str, Buf: wc_test_view_buf,                  \
           const char *: wc_test_view_c, char *: wc_test_view_c,                \
           Response: wc_test_view_res, default: wc_test_view_str)(x_)

// Header names are case-insensitive (RFC 9110 §5.1) and a handler stores them as it spelled
// them, so `Set-Cookie` and `set-cookie` are the same header. This compared with memcmp, which
// is why every app that asserted on a session cookie wrote its own case-insensitive helper.
static inline bool wc_test_ieq_c(str s, const char *lit) {
  size_t n = strlen(lit);
  if (s.n != n || !s.p) return false;
  for (size_t i = 0; i < n; i++) {
    char a = s.p[i], b = lit[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    if (a != b) return false;
  }
  return true;
}

// One response header by name (empty when absent), and the length of the body.
static inline str wc_test_res_header(Response r, const char *name) {
  for (i32 i = 0; i < r.nheaders; i++)
    if (wc_test_ieq_c(r.headers[i].name, name)) return r.headers[i].value;
  return (str){ nullptr, 0 };
}
#define ASSERT_HEADER(res_, name_, want_)                                      \
  __extension__({                                                              \
    str wc_h_ = wc_test_res_header((res_), (name_));                            \
    wc_test_expect(str_eq_c(wc_h_, (want_)), __FILE__, __LINE__,                \
                   "%s[%s]: got \"%s\", want \"%s\"", #res_, (name_),                \
                   wc_test_str(wc_h_), (want_));                                \
  })
#define ASSERT_HEADER_CONTAINS(res_, name_, lit_)                               \
  __extension__({                                                              \
    str wc_h_ = wc_test_res_header((res_), (name_));                            \
    wc_test_expect(str_contains(wc_h_, str_lit(lit_)), __FILE__, __LINE__,      \
                   "%s[%s]: \"%s\" does not contain \"%s\"", #res_, (name_),      \
                   wc_test_str(wc_h_), lit_);                                   \
  })
// A header must appear exactly once: duplicate Set-Cookie or CSP headers are legal
// HTTP and a real bug, and counting them is the only way to see it from here.
#define ASSERT_HEADER_COUNT(res_, name_, want_)                                \
  __extension__({                                                              \
    i32 wc_n_ = 0;                                                             \
    for (i32 wc_i_ = 0; wc_i_ < (res_).nheaders; wc_i_++)                       \
      if (wc_test_ieq_c((res_).headers[wc_i_].name, (name_))) wc_n_++;          \
    wc_test_expect(wc_n_ == (want_), __FILE__, __LINE__,                        \
                   "%s[%s]: got %d header(s), want %d", #res_, (name_), wc_n_,     \
                   (int)(want_));                                              \
  })
// `webc test --macros` has listed this one for as long as the list has existed, and it was
// never defined: the table named an assertion nobody could write. A status is the first thing
// a route test asserts, so it earns the name — and the failure message names the code, not
// just the number.
#define ASSERT_STATUS(res_, want_)                                                    \
  __extension__({                                                                     \
    i32 wc_st_ = (res_).status;                                                       \
    wc_test_expect(wc_st_ == (i32)(want_), __FILE__, __LINE__,                        \
                   "%s: status %d, want %d", #res_, wc_st_, (int)(want_));            \
  })
#define ASSERT_BODY_LEN(res_, want_) ASSERT_EQ((res_).body.n, (want_))

// ---------------------------------------------------------------- the client half of a test
//
// `webc_test_request` is a request with no memory: no cookies, no form encoder, no session. A
// test of a *mutating* route is therefore a test that rebuilds the browser, and after the third
// one the routes stop being tested — this app's suite is 19 tests for 108 routes.
//
//   TestClient c = test_client();                     // a jar, per test
//   Response r = tc_post_form(&c, Session, &(Session){ .email = str_lit("a@b.c"), ... });
//   ASSERT_REDIRECT(r, "/admin");                     // the status *and* the Location
//   ASSERT_DB_ONE(Post, slug, "the-last-ferry");      // the row it should have written
//
//   TestClient other = test_client();                 // a second browser, in the same test
//   ASSERT_STATUS(tc_get(&other, "/admin"), 302);     // logged out, as it should be
//
// What it does not do: run JavaScript (that is `webc test --client`), follow redirects (a test
// that follows one hides the 302 it came for), or touch the network.
typedef struct TestClient {
  Arena *a;
  Buf jar; // "name=value; name2=value2", rebuilt from Set-Cookie
  i32 hops;
} TestClient;

static inline TestClient test_client(void) {
  static const Buf empty = {0};
  TestClient c = {.a = wc_arena_get(), .jar = empty};
  buf_init(&c.jar, c.a, 64);
  return c;
}

// One `Set-Cookie` header into the jar: the name and its value, not the attributes.
static inline void wc_tc_keep_cookie(TestClient *c, str header) {
  i64 semi = 0;
  for (size_t i = 0; i < header.n; i++)
    if (header.p[i] == ';') {
      semi = (i64)i;
      break;
    }
  str pair = str_trim(semi > 0 ? str_sub(header, 0, (size_t)semi) : header);
  if (pair.n == 0) return;
  // an expired cookie is a delete: `Max-Age=0` or a date in the past is the server saying so
  bool drop = str_contains(header, str_lit("Max-Age=0")) || str_contains(header, str_lit("max-age=0"));
  i64 eq = str_index(pair, '=');
  if (eq < 0) return;
  str name = str_sub(pair, 0, (size_t)eq);
  // replace whatever the jar held under that name
  Buf keep;
  buf_init(&keep, c->a, c->jar.n + pair.n + 8);
  size_t i = 0;
  while (i < c->jar.n) {
    size_t e = i;
    while (e < c->jar.n && c->jar.p[e] != ';') e++;
    str item = str_trim(str_sub(buf_str(&c->jar), i, e));
    i64 ieq = str_index(item, '=');
    str iname = ieq < 0 ? item : str_sub(item, 0, (size_t)ieq);
    if (!(iname.n == name.n && str_eq(iname, name))) {
      if (keep.n) buf_puts(&keep, "; ");
      buf_putstr(&keep, item);
    }
    if (e >= c->jar.n) break;
    i = e + 1;
  }
  if (!drop) {
    if (keep.n) buf_puts(&keep, "; ");
    buf_putstr(&keep, pair);
  }
  c->jar = keep;
}

static inline str wc_tc_jar(TestClient *c) { return buf_str(&c->jar); }

// One request through the real handler chain, with the jar's cookies attached and the
// response's `Set-Cookie` kept. This is the only function that talks to the app.
static inline Response wc_tc_send(TestClient *c, const char *method, const char *path,
                                  const char *ctype, str body) {
  Buf h;
  buf_init(&h, c->a, 256);
  if (ctype && *ctype) buf_printf(&h, "Content-Type: %s\r\n", ctype);
  str jar = wc_tc_jar(c);
  if (jar.n) buf_printf(&h, "Cookie: %s\r\n", str_cstr(c->a, jar));
  c->hops++;
  Response r = webc_test_request_ex(method, path, buf_cstr(&h), body.p ? body.p : "",
                                    body.p ? body.n : 0);
  for (i32 i = 0; i < r.nheaders; i++)
    if (wc_test_ieq_c(r.headers[i].name, "set-cookie")) wc_tc_keep_cookie(c, r.headers[i].value);
  return r;
}

// One form value, percent-encoded the way a browser encodes it and the way
// `model_from_form` decodes it (space is `+`, and everything that is not unreserved is escaped).
static inline void wc_tc_escape(Buf *b, str v) {
  static const char *HEX = "0123456789ABCDEF";
  for (size_t i = 0; i < v.n; i++) {
    unsigned char c = (unsigned char)v.p[i];
    bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.' || c == '~';
    if (plain) {
      buf_putc(b, (char)c);
    } else if (c == ' ') {
      buf_putc(b, '+');
    } else {
      buf_putc(b, '%');
      buf_putc(b, HEX[c >> 4]);
      buf_putc(b, HEX[c & 15]);
    }
  }
}

// The model as the form it would have drawn: one field per input, in declaration order, with
// the name the parser looks for. A Vec of strings is one comma-separated value, which is what
// `model_from_form` splits; a bool is `on` or `off`; an enum is its value's name.
// A `Vec(str)` by its shape, since a macro over the array is what the framework itself uses:
// the descriptor first, then the elements. Spelling the type here keeps the encoder a header
// function instead of a generated one per model.
typedef struct WC_TcVecStr {
  const WVecDesc *desc;
  str *at;
  i32 n;
  i32 cap;
} WC_TcVecStr;

static inline str wc_tc_form_body(TestClient *c, const WModel *m, const void *obj) {
  Buf b;
  buf_init(&b, c->a, 256);
  for (i32 i = 0; i < m->n; i++) {
    const WField *f = &m->fields[i];
    if (f->q.pk) continue; // the browser does not post the id back on a create
    Buf v;
    buf_init(&v, c->a, 32);
    if (f->kind == WK_ENUM && f->en) {
      buf_puts(&v, wc_enum_name(f->en, *(const int *)((const char *)obj + f->off)));
    } else if (f->kind == WK_BOOL) {
      buf_puts(&v, (*(const bool *)((const char *)obj + f->off)) ? "on" : "off");
    } else if (f->vec && f->vec->elem_kind == WK_STR) {
      const WC_TcVecStr *vec = (const WC_TcVecStr *)((const char *)obj + f->off);
      for (i32 k = 0; vec->at && k < vec->n; k++) {
        if (k) buf_puts(&v, ", ");
        buf_putstr(&v, vec->at[k]);
      }
    } else if (WC_KIND_IS_TEXT(f->kind)) {
      str s = f->size == (i32)sizeof(str) ? *(const str *)((const char *)obj + f->off)
                                         : str_lit(*(const char *const *)((const char *)obj + f->off)
                                                       ? *(const char *const *)((const char *)obj + f->off)
                                                       : "");
      buf_putstr(&v, s);
    } else if (f->kind == WK_F32 || f->kind == WK_F64) {
      buf_printf(&v, "%.17g", *(const f64 *)((const char *)obj + f->off));
    } else {
      buf_printf(&v, "%lld", (long long)wfield_get_i64(f, obj));
    }
    if (b.n) buf_putc(&b, '&');
    wc_tc_escape(&b, (str){f->name, strlen(f->name)});
    buf_putc(&b, '=');
    wc_tc_escape(&b, buf_str(&v));
  }
  return buf_str(&b);
}

static inline Response wc_tc_post_model(TestClient *c, const char *path, const WModel *m,
                                        const void *obj, bool as_json) {
  if (!as_json)
    return wc_tc_send(c, "POST", path, "application/x-www-form-urlencoded",
                      wc_tc_form_body(c, m, obj));
  return wc_tc_send(c, "POST", path, "application/json",
                    model_to_json(c->a, m, obj));
}

#define tc_send(c_, method_, path_) wc_tc_send(&(c_), (method_), (path_), nullptr, (str){0})
#define tc_get(c_, path_) tc_send((c_), "GET", (path_))
#define tc_post_form(c_, T_, obj_) \
  wc_tc_post_model(&(c_), "/" #T_, T_##_modelof(), (obj_), false)
#define tc_post_form_at(c_, path_, T_, obj_) \
  wc_tc_post_model(&(c_), (path_), T_##_modelof(), (obj_), false)
#define tc_post_json(c_, path_, T_, obj_) \
  wc_tc_post_model(&(c_), (path_), T_##_modelof(), (obj_), true)
// One form body of your own, which is what a route with fields the model does not have wants.
#define tc_post_body(c_, path_, body_) \
  wc_tc_send(&(c_), "POST", (path_), "application/x-www-form-urlencoded", (body_))
#define tc_post_json_body(c_, path_, body_) \
  wc_tc_send(&(c_), "POST", (path_), "application/json", (body_))

// The assertion every POST test wants, and that every app was writing by hand: the status is
// a redirect *and* it points where the test says.
#define ASSERT_REDIRECT(res_, location_)                                              \
  __extension__({                                                                     \
    i32 wc_st_ = (res_).status;                                                       \
    str wc_loc_ = wc_test_res_header((res_), "location");                             \
    wc_test_expect(wc_st_ == 302 || wc_st_ == 303 || wc_st_ == 307, __FILE__, __LINE__, \
                   "%s: status %d, want a redirect (302/303/307)", #res_, wc_st_);    \
    wc_test_expect(str_eq_c(wc_loc_, (location_)), __FILE__, __LINE__,                \
                   "%s: Location \"%s\", want \"%s\"", #res_, wc_test_str(wc_loc_),   \
                   (location_));                                                      \
  })

// The other half of a mutation test: the row it should have written. `ASSERT_DB_ONE` takes a
// string field, `ASSERT_DB_ONE_I64` a number, because a filter is typed in this framework and
// a helper that guesses the type would compile the wrong guess.
#define ASSERT_DB_COUNT(T_, want_)                                                    \
  __extension__({                                                                     \
    i64 wc_n_ = db_count(T_);                                                          \
    wc_test_expect(wc_n_ == (i64)(want_), __FILE__, __LINE__,                          \
                   #T_ ": got %lld row(s), want %lld", (long long)wc_n_,               \
                   (long long)(want_));                                               \
  })
#define ASSERT_DB_ONE(T_, f_, v_)                                                     \
  __extension__({                                                                     \
    WWhere wc_w_;                                                                     \
    ww_init(&wc_w_, wc_arena_get());                                                  \
    ww_eq_str(&wc_w_, T_, f_, (v_));                                                  \
    void *wc_row_ = db_one(db_default(), T_, &wc_w_);                                 \
    wc_test_expect(wc_row_ != nullptr, __FILE__, __LINE__,                            \
                   #T_ " has no row with ." #f_ " = \"%s\"", (v_));                   \
  })
#define ASSERT_DB_ONE_I64(T_, f_, v_)                                                 \
  __extension__({                                                                     \
    WWhere wc_w_;                                                                     \
    ww_init(&wc_w_, wc_arena_get());                                                  \
    ww_eq_i64(&wc_w_, T_, f_, (v_));                                                  \
    void *wc_row_ = db_one(db_default(), T_, &wc_w_);                                 \
    wc_test_expect(wc_row_ != nullptr, __FILE__, __LINE__,                            \
                   #T_ " has no row with ." #f_ " = %lld", (long long)(v_));          \
  })

// ---------------------------------------------------------------- JSON and HTML
//
// A test says what the response *is*: a number at a path in the JSON, an element
// with that structure in the HTML. Substring checks pass when the markup moved
// somewhere the CSS no longer reaches, which is exactly what a redesign breaks.
//
//   ASSERT_JSON(res, "views", "42");
//   ASSERT_JSON_STR(res, "slug", "the-last-ferry");
//   ASSERT_HTML(res, "article.lead > h2 a");
//   ASSERT_HTML_COUNT(res, "article.card", 6);
//   ASSERT_HTML_TEXT(res, "article.lead > h2 a", "The chair nobody notices");
bool wc_json_path(JVal *v, const char *path, JVal *out);
str wc_json_at_str(str body, const char *path);   // null str when the path is absent
i32 wc_html_count(str html, const char *selector); // -1: the selector did not parse
bool wc_html_has(str html, const char *selector);
str wc_html_text_at(str html, const char *selector);
f64 wc_html_number_at(str html, const char *selector); // NAN when the text has no number
bool wc_html_selector_ok(const char *selector);

#define ASSERT_JSON(res_, path_, want_)                                        \
  __extension__({                                                              \
    str wc_j_ = wc_json_at_str(WC_TEST_VIEW((res_)), (path_));                  \
    wc_test_expect(wc_j_.p && str_eq_c(wc_j_, (want_)), __FILE__, __LINE__,     \
                   "%s[\"%s\"]: got %s, want \"%s\"", #res_, (path_),            \
                   wc_j_.p ? wc_test_str(wc_j_) : "(no such path)", (want_));     \
  })
#define ASSERT_JSON_STR ASSERT_JSON

// The same question about a JSON *document* you built rather than a response — the unit a
// writer, a `ld_*` function or a serializer is tested at, before any route is involved.
#define ASSERT_JSON_DOC(doc_, path_, want_)                                    \
  __extension__({                                                              \
    str wc_jd_ = wc_json_at_str((doc_), (path_));                              \
    wc_test_expect(wc_jd_.p && str_eq_c(wc_jd_, (want_)), __FILE__, __LINE__,   \
                   "doc[\"%s\"]: got %s, want \"%s\"", (path_),               \
                   wc_jd_.p ? wc_test_str(wc_jd_) : "(no such path)", (want_)); \
  })
#define ASSERT_JSON_DOC_ABSENT(doc_, path_)                                    \
  __extension__({                                                              \
    str wc_ja_ = wc_json_at_str((doc_), (path_));                              \
    wc_test_expect(wc_ja_.p == nullptr, __FILE__, __LINE__,                    \
                   "doc[\"%s\"]: present (%s), want it absent", (path_),         \
                   wc_test_str(wc_ja_));                                       \
  })

// Structured data lives inside an HTML body, in a <script type="application/ld+json"> block,
// so it is neither: ASSERT_JSON parses the body and finds HTML, ASSERT_HTML finds a tag and
// not the facts in it. These read the first block out of a page and then ask it questions by
// path, which is the only way "the page states what it is" can be a claim anyone can check.
//
//   ASSERT_LD(res, "headline", "The last ferry");
//   ASSERT_LD(res, "author.name", "Ada Okafor");
//   ASSERT_LD_ABSENT(res, "datePublished");   // a draft has none, and the key is not ""
str wc_ld_block(str html);   // the first ld+json payload, as JSON text; null when there is none

#define ASSERT_LD(res_, path_, want_)                                          \
  __extension__({                                                              \
    str wc_ld_ = wc_ld_block(WC_TEST_VIEW((res_)));                            \
    str wc_lv_ = wc_json_at_str(wc_ld_, (path_));                              \
    wc_test_expect(wc_ld_.p != nullptr, __FILE__, __LINE__,                    \
                   "%s: no <script type=\"application/ld+json\"> block", #res_); \
    wc_test_expect(wc_lv_.p && str_eq_c(wc_lv_, (want_)), __FILE__, __LINE__,   \
                   "%s ld+json[\"%s\"]: got %s, want \"%s\"", #res_, (path_),  \
                   wc_lv_.p ? wc_test_str(wc_lv_) : "(no such path)", (want_)); \
  })

#define ASSERT_LD_ABSENT(res_, path_)                                          \
  __extension__({                                                              \
    str wc_la_ = wc_ld_block(WC_TEST_VIEW((res_)));                            \
    str wc_lb_ = wc_json_at_str(wc_la_, (path_));                              \
    wc_test_expect(wc_la_.p != nullptr, __FILE__, __LINE__,                    \
                   "%s: no <script type=\"application/ld+json\"> block", #res_); \
    wc_test_expect(wc_lb_.p == nullptr, __FILE__, __LINE__,                    \
                   "%s ld+json[\"%s\"]: present (%s), want it absent", #res_,     \
                   (path_), wc_test_str(wc_lb_));                              \
  })

// The selector is a subset of CSS: tag, .class, #id, [attr], [attr="v"],
// descendant and '>'.
#define ASSERT_HTML(res_, sel_)                                                \
  __extension__({                                                              \
    i32 wc_n_ = wc_html_count(WC_TEST_VIEW((res_)), (sel_));                    \
    wc_test_expect(wc_n_ > 0, __FILE__, __LINE__,                              \
                   "%s: no element matches \"%s\" (%s)", #res_, (sel_),          \
                   wc_n_ < 0 ? "selector not understood" : "nothing matched");    \
  })
#define ASSERT_HTML_ABSENT(res_, sel_)                                         \
  __extension__({                                                              \
    i32 wc_n_ = wc_html_count(WC_TEST_VIEW((res_)), (sel_));                    \
    wc_test_expect(wc_n_ == 0, __FILE__, __LINE__,                             \
                   "%s: %d element(s) match \"%s\", want none", #res_, wc_n_,     \
                   (sel_));                                                    \
  })
#define ASSERT_HTML_COUNT(res_, sel_, want_)                                   \
  __extension__({                                                              \
    i32 wc_n_ = wc_html_count(WC_TEST_VIEW((res_)), (sel_));                    \
    wc_test_expect(wc_n_ == (want_), __FILE__, __LINE__,                       \
                   "%s: %d element(s) match \"%s\", want %d", #res_, wc_n_,       \
                   (sel_), (int)(want_));                                      \
  })
#define ASSERT_HTML_TEXT(res_, sel_, want_)                                    \
  __extension__({                                                              \
    str wc_t_ = wc_html_text_at(WC_TEST_VIEW((res_)), (sel_));                  \
    wc_test_expect(wc_t_.p && str_eq_c(wc_t_, (want_)), __FILE__, __LINE__,     \
                   "%s \"%s\" text: got \"%s\", want \"%s\"", #res_, (sel_),       \
                   wc_t_.p ? wc_test_str(wc_t_) : "(no match)", (want_));        \
  })

// The number a card shows, read the way a person reads it: "$10,038" is 10038 and "96.3%"
// is 96.3. The client half renders markup, not a Response, and this is the assertion that
// makes its *values* testable instead of its punctuation — no hand-written scanner in the
// test file, and a redesign that moves the card does not break it.
//
//   str html = wcomp_html_ctx(&comp, view_Stats, nullptr);
//   ASSERT_HTML_NUMBER(html, ".card.ruin b", 96.3);
#define ASSERT_HTML_NUMBER(res_, sel_, want_)                                                  \
  __extension__({                                                                              \
    f64 wc_num_ = wc_html_number_at(WC_TEST_VIEW((res_)), (sel_));                             \
    f64 wc_want_ = (f64)(want_);                                                               \
    bool wc_ok_ = !__builtin_isnan(wc_num_) && !__builtin_isnan(wc_want_) &&                                       \
                  (wc_num_ > wc_want_ ? wc_num_ - wc_want_ < 1e-6 : wc_want_ - wc_num_ < 1e-6);\
    wc_test_expect(wc_ok_, __FILE__, __LINE__, "%s \"%s\" number: got %s, want %g", #res_,      \
                   (sel_), __builtin_isnan(wc_num_) ? "(no number)" : wc_test_str(wc_html_text_at(        \
                               WC_TEST_VIEW((res_)), (sel_))),                                 \
                   wc_want_);                                                                  \
  })

// A value at a position, when the page has a list and the test means row 6, cell 3:
//
//   ASSERT_HTML_NUMBER_AT(html, "tbody tr", 6, "td", 3, 12.4);   // the table cell
//   ASSERT_HTML_ATTR(html, "input[name=title]", "placeholder", "what needs doing?");
//   ASSERT_JSON_COUNT(res, "items", 6);                          // an array's length
//
// The selectors are the same subset the assertions above use; the *nth* selectors
// (`:nth-child(3)`, `:first-child`) do the rest of the work, so a test can say what it
// means instead of walking bytes.
#define ASSERT_HTML_NUMBER_AT(res_, row_sel_, row_, cell_sel_, cell_, want_)                   \
  __extension__({                                                                              \
    char wc_sel_[256];                                                                         \
    snprintf(wc_sel_, sizeof wc_sel_, "%s:nth-child(%d) %s:nth-child(%d)", (row_sel_),          \
             (int)(row_), (cell_sel_), (int)(cell_));                                          \
    f64 wc_num_ = wc_html_number_at(WC_TEST_VIEW((res_)), wc_sel_);                            \
    f64 wc_want_ = (f64)(want_);                                                               \
    bool wc_ok_ = !__builtin_isnan(wc_num_) && (wc_num_ > wc_want_ ? wc_num_ - wc_want_ < 1e-6 \
                                                                   : wc_want_ - wc_num_ < 1e-6);\
    wc_test_expect(wc_ok_, __FILE__, __LINE__, "%s row %d \"%s\": got %g, want %g", #res_,      \
                   (int)(row_), wc_sel_, wc_num_, wc_want_);                                   \
  })
#define ASSERT_HTML_ATTR(res_, sel_, name_, want_)                                             \
  __extension__({                                                                              \
    str wc_v_ = wc_html_attr_at(WC_TEST_VIEW((res_)), (sel_), (name_));                         \
    wc_test_expect(wc_v_.p && str_eq_c(wc_v_, (want_)), __FILE__, __LINE__,                     \
                   "%s \"%s\" [%s]: got \"%s\", want \"%s\"", #res_, (sel_), (name_),           \
                   wc_v_.p ? wc_test_str(wc_v_) : "(no such attribute)", (want_));              \
  })
#define ASSERT_JSON_COUNT(res_, path_, want_)                                                  \
  ASSERT_EQ(wc_json_count_at(WC_TEST_VIEW((res_)), (path_)), (i32)(want_))

str wc_html_attr_at(str html, const char *selector, const char *name);
i32 wc_json_count_at(str body, const char *path);

// A body as a parse tree, in the test's own arena. A test that has to *search* a list
// ("the id of the row whose title says X") is doing something the path assertions cannot
// express — and doing it by hand means an arena of its own and a second JSON parser in
// the test file. The arena is the one the runner resets between tests; the tree dies with
// it. `json_at`/`json_get` read the tree.
JVal *wc_test_json(Response r);

void wc_test_sub_begin(const char *label);
void wc_test_sub_end(void);

// One setup and one teardown for the whole binary (a database, a scheduler, a
// temp directory). A setup that panics fails every test with that message.
void wc_test_setup_fn(void (*fn)(void));
void wc_test_teardown_fn(void (*fn)(void));

// ---------------------------------------------------------------- tests

#if defined(__GNUC__) || defined(__clang__)
// A test is a function plus a constructor that files it with the runner, so the
// order in the file does not matter and nothing has to be listed by hand.
#define TEST(name_)                                                            \
  static void wc_test_fn_##name_(void);                                        \
  __attribute__((constructor)) static void wc_test_reg_##name_(void) {          \
    wc_test_register(#name_, wc_test_fn_##name_);                              \
  }                                                                            \
  static void wc_test_fn_##name_(void)
#else
#error "webc/test.h needs section-attributed constructors (GCC or Clang)"
#endif

// TEST_SETUP(setup_fn); TEST_TEARDOWN(teardown_fn);
#define TEST_SETUP(fn_)                                                        \
  __attribute__((constructor)) static void WC_UNIQ(wc_test_setup_reg_)(void) {   \
    wc_test_setup_fn(fn_);                                                      \
  }
#define TEST_TEARDOWN(fn_)                                                     \
  __attribute__((constructor)) static void WC_UNIQ(wc_test_td_reg_)(void) {      \
    wc_test_teardown_fn(fn_);                                                   \
  }

// The suite's main. It boots the app first — once, against an in-memory database unless
// the suite or the config names another — so a suite does not begin with ten lines of
// setup that every other suite also has, and cannot accidentally boot twice:
//
//   TEST_MAIN()
//
// `wc_test_boot()` calls the app's `webc_boot()` if it has one, sets DATABASE_URL to
// sqlite::memory: when nothing else did, and does nothing at all if a TEST_SETUP() already
// did the work (it is idempotent, because the second call to webc_boot() would seed twice).
void wc_test_boot(void);

#define TEST_MAIN()                                                            \
  int main(void) {                                                             \
    setvbuf(stdout, nullptr, _IONBF, 0);                                        \
    wc_test_boot();                                                             \
    return wc_test_run() == 0 ? 0 : 1;                                         \
  }

// A named group inside a test. Its checks are reported under that name and a
// failure in one subtest does not stop the next.
#define SUBTEST(label_)                                                        \
  for (int wc_sub_ = (wc_test_sub_begin(label_), 1); wc_sub_;                 \
       wc_test_sub_end(), wc_sub_ = 0)

// Run the body once per row; the row is bound to `r`, and its `name` member
// names the subtest.
#define SUBTESTS(rows_)                                                        \
  for (size_t wc_row_ = 0; wc_row_ < sizeof rows_ / sizeof *rows_; wc_row_++)  \
    for (__auto_type r = &(rows_)[wc_row_]; r != nullptr; r = nullptr)          \
      for (int wc_sub_ = (wc_test_sub_begin(r->name), 1); wc_sub_;             \
           wc_test_sub_end(), wc_sub_ = 0)

// ---------------------------------------------------------------- assertions
// Every assertion records a failure and continues, like go's t.Error. Use
// FAIL() when the test cannot sensibly go on, or SKIP() to stop it.

#define ASSERT(cond_)                                                          \
  wc_test_expect(!!(cond_), __FILE__, __LINE__, "want %s", #cond_)
#define ASSERT_TRUE(cond_) ASSERT(cond_)
#define ASSERT_FALSE(cond_)                                                    \
  wc_test_expect(!(cond_), __FILE__, __LINE__, "want !%s", #cond_)

#define ASSERT_EQ(a_, b_)                                                      \
  __extension__({                                                              \
    long long wc_a_ = (long long)(a_), wc_b_ = (long long)(b_);                 \
    wc_test_expect(wc_a_ == wc_b_, __FILE__, __LINE__,                          \
                   "%s == %s: got %lld, want %lld", #a_, #b_, wc_a_, wc_b_);     \
  })
#define ASSERT_NE(a_, b_)                                                      \
  __extension__({                                                              \
    long long wc_a_ = (long long)(a_), wc_b_ = (long long)(b_);                 \
    wc_test_expect(wc_a_ != wc_b_, __FILE__, __LINE__,                          \
                   "%s == %s: both %lld", #a_, #b_, wc_a_);                     \
  })
#define ASSERT_LT(a_, b_) WC_ASSERT_CMP(a_, <, b_)
#define ASSERT_LE(a_, b_) WC_ASSERT_CMP(a_, <=, b_)
#define ASSERT_GT(a_, b_) WC_ASSERT_CMP(a_, >, b_)
#define ASSERT_GE(a_, b_) WC_ASSERT_CMP(a_, >=, b_)
#define WC_ASSERT_CMP(a_, op_, b_)                                             \
  __extension__({                                                              \
    long long wc_a_ = (long long)(a_), wc_b_ = (long long)(b_);                 \
    wc_test_expect(wc_a_ op_ wc_b_, __FILE__, __LINE__,                         \
                   "%s %s %s: %lld vs %lld", #a_, #op_, #b_, wc_a_, wc_b_);     \
  })

#define ASSERT_FEQ(a_, b_)                                                     \
  __extension__({                                                              \
    f64 wc_a_ = (a_), wc_b_ = (b_);                                            \
    wc_test_expect(wc_a_ > wc_b_ ? wc_a_ - wc_b_ < 1e-9 : wc_b_ - wc_a_ < 1e-9, \
                   __FILE__, __LINE__, "%s: got %.9g, want %.9g", #a_, wc_a_,   \
                   wc_b_);                                                     \
  })

// a str against a C string literal
#define ASSERT_STR_EQ(s_, lit_)                                                \
  __extension__({                                                              \
    str wc_s_ = WC_TEST_VIEW(s_);                                               \
    const char *wc_l_ = (lit_);                                                \
    wc_test_expect(str_eq_c(wc_s_, wc_l_), __FILE__, __LINE__,                  \
                   "%s: got \"%s\", want \"%s\"", #s_,                         \
                   wc_test_str(wc_s_), wc_l_);                                 \
  })
// two str values
#define ASSERT_STR(a_, b_)                                                     \
  __extension__({                                                              \
    str wc_a_ = WC_TEST_VIEW(a_), wc_b_ = WC_TEST_VIEW(b_);                      \
    wc_test_expect(str_eq(wc_a_, wc_b_), __FILE__, __LINE__,                    \
                   "%s: got \"%s\", want \"%s\"", #a_, wc_test_str(wc_a_),     \
                   wc_test_str(wc_b_));                                        \
  })
#define ASSERT_STR_CONTAINS(s_, lit_)                                          \
  __extension__({                                                              \
    str wc_s_ = WC_TEST_VIEW(s_);                                               \
    wc_test_expect(str_contains(wc_s_, str_lit(lit_)), __FILE__, __LINE__,       \
                   "%s: \"%s\" does not contain \"%s\"", #s_,                  \
                   wc_test_str(wc_s_), lit_);                                  \
  })

#define ASSERT_MEM_EQ(p_, q_, n_)                                              \
  __extension__({                                                              \
    const void *wc_p_ = (p_), *wc_q_ = (q_);                                   \
    size_t wc_n_ = (size_t)(n_);                                               \
    wc_test_expect(wc_p_ && wc_q_ && memcmp(wc_p_, wc_q_, wc_n_) == 0,          \
                   __FILE__, __LINE__, "%s == %s (%zu bytes)", #p_, #q_, wc_n_);\
  })
#define ASSERT_NULL(p_)                                                        \
  wc_test_expect((p_) == nullptr, __FILE__, __LINE__, "%s is not NULL", #p_)
#define ASSERT_PTR_EQ(a_, b_)                                                  \
  wc_test_expect((const void *)(a_) == (const void *)(b_), __FILE__, __LINE__, \
                 "%s == %s: got %p, want %p", #a_, #b_, (const void *)(a_),    \
                 (const void *)(b_))
// ASSERT_PANICS(body) passes when `body` raises a panic and the panic is caught
// here (the runner has a frame per test, so a panic never leaves the suite).
#define ASSERT_PANICS(body_)                                                   \
  __extension__({                                                              \
    wc_try_frame wc_pf_;                                                        \
    bool wc_panicked_ = false;                                                  \
    if (wc_try_begin(&wc_pf_)) {                                                \
      body_;                                                                    \
      wc_try_end(&wc_pf_);                                                      \
    } else {                                                                    \
      wc_panicked_ = true;                                                      \
    }                                                                           \
    wc_test_expect(wc_panicked_, __FILE__, __LINE__, "%s did not panic",        \
                   #body_);                                                     \
  })
#define ASSERT_NOT_NULL(p_)                                                    \
  wc_test_expect((p_) != nullptr, __FILE__, __LINE__, "%s is NULL", #p_)

// Results: ASSERT_OK(x) takes anything with `.ok`, ASSERT_ERR(x) its failure.
#define ASSERT_OK(x_)                                                          \
  __extension__({                                                              \
    __auto_type wc_r_ = (x_);                                                  \
    wc_test_expect(wc_r_.ok, __FILE__, __LINE__, "%s failed: \"%s\"", #x_,      \
                   wc_test_str(werr_msg(&wc_r_.e)));                                  \
  })
#define ASSERT_ERR(x_)                                                         \
  __extension__({                                                              \
    __auto_type wc_r_ = (x_);                                                  \
    wc_test_expect(!wc_r_.ok, __FILE__, __LINE__, "%s unexpectedly succeeded",  \
                   #x_);                                                       \
  })
#define ASSERT_ERR_KIND(x_, kind_)                                             \
  __extension__({                                                              \
    __auto_type wc_r_ = (x_);                                                  \
    wc_test_expect(!wc_r_.ok && wc_r_.e.kind == (kind_), __FILE__, __LINE__,     \
                   "%s: got %s, want %s", #x_,                                 \
                   wc_r_.ok ? "ok" : werr_kind_name(wc_r_.e.kind),             \
                   werr_kind_name(kind_));                                     \
  })

#define FAIL(...) wc_test_failf(__FILE__, __LINE__, __VA_ARGS__)
#define SKIP(why_)                                                             \
  do {                                                                         \
    wc_test_skip(__FILE__, __LINE__, (why_));                                  \
    return;                                                                    \
  } while (0)

// ---------------------------------------------------------------- try
// Inside a test, `try(expr)` cannot return the failing Result (a TEST body
// returns void), so it panics instead; the runner turns that panic into a
// failed test with the error message, which is what the caller meant anyway.
#undef try
#define try(x)                                                                 \
  __extension__({                                                              \
    __auto_type wc_tr_ = (x);                                                  \
    if (!wc_tr_.ok)                                                            \
      wc_panic(__FILE__, __LINE__, "%.*s", WERR_FMT(wc_tr_.e));                      \
    wc_tr_.v;                                                                  \
  })
#undef tryv
#define tryv(x)                                                                \
  do {                                                                         \
    WErrRes wc_tv_ = (x);                                                      \
    if (!wc_tv_.ok)                                                            \
      wc_panic(__FILE__, __LINE__, "%.*s", WERR_FMT(wc_tv_.e));                      \
  } while (0)

#endif // WEBC_TEST_H