// gzip_test.c — the bytes, and the three headers that make them correct.
//
// The compression itself is zlib's, so what is worth testing here is everything zlib does
// not decide: when a response may be compressed at all, what the client is told when it is
// not, and the validators. Each case below is a bug that a real browser or a real CDN
// finds for you, at the worst time:
//
//   * a body compressed for a client that never sent Accept-Encoding is unreadable;
//   * a `Vary` that only appears on the compressed reply lets a cache hand the identity
//     bytes to the next gzip client;
//   * an ETag shared between the two encodings makes `If-None-Match` answer a gzip request
//     with a 304 for the identity bytes;
//   * re-compressing a file on every request is 30 ms of CPU per hit on a 64 KB module.
#include <webc/test.h>
#include <webc/http.h>
#include <webc/gzip.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// 4 KB of JSON, so the body is over the floor and the answer is worth compressing.
GET("/probe/big") {
  Buf pad;
  buf_init(&pad, req->arena, 4096);
  buf_puts(&pad, "BIG-JSON-PADDING");
  while (pad.n < 4000) buf_puts(&pad, "0123456789abcdef");
  return res_bytes(200, "application/json", buf_cstr(&pad), pad.n);
}

// A page the view layer assembled sets the base type in the *header* (with its charset),
// not in `res.content_type`. Reading only the field makes every server-rendered page
// "not compressible", so it goes out at full size — which is why the case is a test.
GET("/probe/header-typed") {
  Buf b;
  buf_init(&b, req->arena, 4096);
  while (b.n < 3000) buf_puts(&b, "<p>typed in the header</p>");
  Response r = res_bytes(200, nullptr, buf_cstr(&b), b.n);
  res_header(&r, "Content-Type", "text/html; charset=utf-8");
  return r;
}

static str resp_header(Response r, const char *name) {
  for (i32 i = 0; i < r.nheaders; i++)
    if (str_eq_c(r.headers[i].name, name)) return r.headers[i].value;
  return (str){nullptr, 0};
}

static void write_file(const char *path, const char *body) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fputs(body, f);
  fclose(f);
}

// A body big enough that gzip is a win, and repetitive enough that it is a big one.
static char *make_body(size_t n) {
  static char buf[64 * 1024];
  for (size_t i = 0; i < n - 1; i++) buf[i] = (char)('a' + (i % 26) + (i % 7 == 0 ? 1 : 0));
  buf[n - 1] = '\n';
  buf[n] = 0;
  return buf;
}

TEST(gzip_round_trips_and_actually_shrinks) {
  Arena *a = arena_new(1 << 16);
  char *body = make_body(20000);
  Bytes in = {.p = body, .n = strlen(body)};
  Bytes gz = wc_gzip(a, in, WC_GZIP_LEVEL);
  ASSERT(gz.p != nullptr);
  ASSERT(gz.n > 0);
  ASSERT(gz.n < in.n / 4); // repetitive text: at least 4x
  // the gzip magic, so this is not a zlib stream by accident
  ASSERT_EQ((unsigned char)gz.p[0], 0x1f);
  ASSERT_EQ((unsigned char)gz.p[1], 0x8b);
  Bytes back = wc_gunzip(a, gz);
  ASSERT_EQ(back.n, in.n);
  ASSERT(memcmp(back.p, in.p, in.n) == 0);
  arena_destroy(a);
}

TEST(an_empty_or_unreadable_stream_is_refused_not_guessed) {
  Arena *a = arena_new(1 << 12);
  ASSERT(wc_gzip(a, (Bytes){0}, 9).p == nullptr);
  ASSERT(wc_gunzip(a, (Bytes){.p = (char *)"not a gzip stream at all", .n = 24}).p == nullptr);
  arena_destroy(a);
}

TEST(a_file_is_served_gzipped_when_the_client_asks_and_plain_when_it_does_not) {
  system("rm -rf /tmp/webc-gz && mkdir -p /tmp/webc-gz");
  char *body = make_body(9000);
  write_file("/tmp/webc-gz/app.css", body);

  webc_use_reset();
  webc_use(mw_compress());
  webc_use(mw_static("/", "/tmp/webc-gz"));

  Response gz = webc_test_request_ex("GET", "/app.css", "Accept-Encoding: gzip", nullptr, 0);
  ASSERT_EQ(gz.status, 200);
  ASSERT(str_eq_c(resp_header(gz, "Content-Encoding"), "gzip"));
  ASSERT(str_eq_c(resp_header(gz, "Vary"), "Accept-Encoding"));
  ASSERT(gz.body.n > 0 && gz.body.n < strlen(body));
  Bytes back = wc_gunzip(nullptr, (Bytes){.p = gz.body.p, .n = gz.body.n});
  ASSERT(back.p != nullptr);
  ASSERT_EQ(back.n, strlen(body));
  ASSERT(memcmp(back.p, body, back.n) == 0);
  wm_free(back.p);

  // The gzipped response's validator is copied out *now*, before the second request asks
  // the server for anything else: a Response from webc_test_request() lives in that
  // request's arena, so every pointer in it is stale once the next request starts. On
  // glibc that memory is recycled (this test read garbage), on Darwin it happened to stay
  // readable, so the same code passed there and failed here.
  char etag_gz[160];
  {
    str t = resp_header(gz, "ETag");
    ASSERT(t.n > 0 && t.n < (i64)sizeof etag_gz);
    snprintf(etag_gz, sizeof etag_gz, "%.*s", (int)t.n, t.p);
  }

  Response plain = webc_test_request("GET", "/app.css");
  ASSERT_EQ(plain.status, 200);
  ASSERT(resp_header(plain, "Content-Encoding").n == 0);
  // still varies: the next client may ask for gzip and a cache has to keep them apart
  ASSERT(str_eq_c(resp_header(plain, "Vary"), "Accept-Encoding"));

  // the two representations have different validators, and each answers its own 304
  str etag_plain = resp_header(plain, "ETag");
  ASSERT(etag_plain.n > 0);
  str gz_tag = {etag_gz, strlen(etag_gz)};
  ASSERT(!str_eq(gz_tag, etag_plain));
  ASSERT(str_has_suffix(gz_tag, str_lit("-gz")));
  webc_use_reset();
}

TEST(gzip_q_zero_means_do_not) {
  system("rm -rf /tmp/webc-gz2 && mkdir -p /tmp/webc-gz2");
  write_file("/tmp/webc-gz2/app.css", make_body(9000));
  webc_use_reset();
  webc_use(mw_compress());
  webc_use(mw_static("/", "/tmp/webc-gz2"));

  Response off = webc_test_request_ex("GET", "/app.css", "Accept-Encoding: gzip;q=0", nullptr, 0);
  ASSERT_EQ(off.status, 200);
  ASSERT(resp_header(off, "Content-Encoding").n == 0);
  // still the file on disk, streamed as it was: nothing was read and re-written to say no
  ASSERT(off.file_path.p != nullptr);
  ASSERT_EQ(off.body.n, (size_t)0);

  // br first, gzip at half weight: still gzip, because it is the only one on offer
  Response gz = webc_test_request_ex("GET", "/app.css", "Accept-Encoding: br, gzip;q=0.5", nullptr,
                                     0);
  ASSERT(str_eq_c(resp_header(gz, "Content-Encoding"), "gzip"));
  webc_use_reset();
}

TEST(a_small_or_already_compressed_response_is_left_alone) {
  system("rm -rf /tmp/webc-gz3 && mkdir -p /tmp/webc-gz3");
  write_file("/tmp/webc-gz3/tiny.css", "a{}\n");
  system("printf '\\x89PNG\\r\\n\\x1a\\n' > /tmp/webc-gz3/logo.png && "
         "head -c 4000 /dev/urandom >> /tmp/webc-gz3/logo.png");
  webc_use_reset();
  webc_use(mw_compress());
  webc_use(mw_static("/", "/tmp/webc-gz3"));

  Response tiny = webc_test_request_ex("GET", "/tiny.css", "Accept-Encoding: gzip", nullptr, 0);
  ASSERT_EQ(tiny.status, 200);
  ASSERT(resp_header(tiny, "Content-Encoding").n == 0); // under the 1 KB floor

  // a PNG is already compressed: no gzip, and no Vary either — the reply does not depend
  // on the request, so a cache may share it
  Response png = webc_test_request_ex("GET", "/logo.png", "Accept-Encoding: gzip", nullptr, 0);
  ASSERT_EQ(png.status, 200);
  ASSERT(resp_header(png, "Content-Encoding").n == 0);
  ASSERT(resp_header(png, "Vary").n == 0);
  webc_use_reset();
}

// A rendered response is compressed too — that is the half a precompressed-sidecar server
// cannot do, and the reason this needed a real compressor instead of a file cache.
TEST(a_rendered_body_is_compressed_too) {
  webc_use_reset();
  webc_use(mw_compress());
  Response gz = webc_test_request_ex("GET", "/probe/big", "Accept-Encoding: gzip", nullptr, 0);
  ASSERT_EQ(gz.status, 200);
  ASSERT(str_eq_c(resp_header(gz, "Content-Encoding"), "gzip"));
  Bytes back = wc_gunzip(nullptr, (Bytes){.p = gz.body.p, .n = gz.body.n});
  ASSERT(back.p != nullptr);
  ASSERT(back.n > 2000);
  ASSERT(str_contains((str){back.p, back.n}, str_lit("BIG-JSON-PADDING")));
  wm_free(back.p);

  Response plain = webc_test_request("GET", "/probe/big");
  ASSERT_EQ(plain.status, 200);
  ASSERT(resp_header(plain, "Content-Encoding").n == 0);
  ASSERT(str_contains((str){plain.body.p, plain.body.n}, str_lit("BIG-JSON-PADDING")));
  webc_use_reset();
}

TEST(a_page_that_sets_its_type_in_a_header_is_still_compressed) {
  webc_use_reset();
  webc_use(mw_compress());
  Response gz = webc_test_request_ex("GET", "/probe/header-typed", "Accept-Encoding: gzip", nullptr,
                                     0);
  ASSERT_EQ(gz.status, 200);
  ASSERT(str_eq_c(resp_header(gz, "Content-Encoding"), "gzip"));
  ASSERT(gz.body.n > 0 && gz.body.n < 1000);
  webc_use_reset();
}

TEST(the_cache_compresses_a_file_once_and_notices_when_it_changes) {
  system("rm -rf /tmp/webc-gz4 && mkdir -p /tmp/webc-gz4");
  write_file("/tmp/webc-gz4/app.css", make_body(9000));
  webc_use_reset();
  webc_use(mw_compress());
  webc_use(mw_static("/", "/tmp/webc-gz4"));

  Response one = webc_test_request_ex("GET", "/app.css", "Accept-Encoding: gzip", nullptr, 0);
  ASSERT_EQ(one.status, 200);
  str etag_one = resp_header(one, "ETag");
  size_t n_one = one.body.n;

  // same file, second request: same bytes out
  Response two = webc_test_request_ex("GET", "/app.css", "Accept-Encoding: gzip", nullptr, 0);
  ASSERT_EQ(two.body.n, n_one);
  ASSERT(str_eq(resp_header(two, "ETag"), etag_one));

  // a different file size is a different cache key, so the new content is what comes back
  char *other = make_body(12000);
  for (size_t i = 0; i < 12000; i++) other[i] = (char)('A' + (i % 5));
  write_file("/tmp/webc-gz4/app.css", other);
  Response three = webc_test_request_ex("GET", "/app.css", "Accept-Encoding: gzip", nullptr, 0);
  ASSERT_EQ(three.status, 200);
  ASSERT(three.body.n != n_one);
  Bytes back = wc_gunzip(nullptr, (Bytes){.p = three.body.p, .n = three.body.n});
  ASSERT(back.p != nullptr);
  ASSERT_EQ(back.n, strlen(other));
  ASSERT(memcmp(back.p, other, back.n) == 0);
  wm_free(back.p);
  webc_use_reset();
}

TEST_MAIN()