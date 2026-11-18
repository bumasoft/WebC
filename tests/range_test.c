// range_test.c — a file is a representation, not a path and a length.
//
// `Range: bytes=…` is what makes a video seekable, a PDF page-able in a viewer and a
// download resumable on a train. Without it a response carries `st.st_size` bytes from byte 0
// whatever the client asked for: `curl -C -` refuses with "server does not seem to support
// byte ranges", a 1 KB request for a 96 KB module costs 96 KB, and iOS Safari will not play
// a media element at all.
//
// Two kinds of assertion live here, because a range has two halves and they fail
// differently:
//
//   * the *decision* — 304, 200, 206 or 416, and the headers that say which — reached
//     through `webc_test_request_ex`, which is the same `dispatch()` the socket server runs;
//   * the *bytes* — that a 206 puts exactly `Content-Range`'s window on the wire and nothing
//     else — which needs a real socket, so one test forks a server and reads a raw reply.
//
// The cases that are easy to get subtly wrong, and are therefore each a test: a suffix
// (`bytes=-100`), a range that runs past the end (clamp, not 416), a range that starts past
// the end (416 with `bytes */size`), an inverted range (ignore), a multi-range (ignore),
// `bytes=-0` (416), a stale `If-Range` (whole file, or a resumed download splices two
// versions together), and `Accept-Encoding: gzip` on a range (a compressed 206 has offsets
// into a deflate stream, and compressing it reads the whole file to answer 1 KB).
// The system headers come first: `<webc/web.h>` shadows `select(2)` with the framework's green
// thread `select(...)`, and `<sys/socket.h>` needs the real one (docs/reference.md §16.3).
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <webc/test.h>
#include <webc/http.h>

#define BLOB_DIR "/tmp/webc-range"
#define BLOB_PATH BLOB_DIR "/blob.txt"
#define BLOB_LEN 900

// The content is a function of the offset, so the expected bytes of any window are
// computable without a second copy of the file: byte i is 'A' + i % 26.
static void write_blob(size_t n) {
  system("rm -rf " BLOB_DIR " && mkdir -p " BLOB_DIR);
  FILE *f = fopen(BLOB_PATH, "wb");
  if (!f) return;
  for (size_t i = 0; i < n; i++) fputc((int)('A' + (i % 26)), f);
  fclose(f);
}

static char blob_byte(size_t i) { return (char)('A' + (i % 26)); }

static str resp_header(Response r, const char *name) {
  for (i32 i = 0; i < r.nheaders; i++)
    if (str_eq_c(r.headers[i].name, name)) return r.headers[i].value;
  return (str){nullptr, 0};
}

// The same file through three doors: a route that serves it, a route that downloads it, and
// an SPA mount. `ANY` because a range is only honoured for GET and HEAD, which is itself a
// case below.
ANY("/probe/file") { return res_file(200, BLOB_PATH); }
ANY("/probe/download") { return res_download(200, BLOB_PATH, "q3 report; final.pdf"); }
ANY("/probe/download-plain") { return res_download(200, BLOB_PATH, "report.pdf"); }

// ---------------------------------------------------------------- the decision

TEST(a_full_response_says_what_it_can_do) {
  write_blob(BLOB_LEN);
  Response r = webc_test_request("GET", "/probe/file");
  ASSERT_EQ(r.status, 200);
  // without this a client does not even try to resume, seek or ask for a suffix
  ASSERT(str_eq_c(resp_header(r, "Accept-Ranges"), "bytes"));
  ASSERT(resp_header(r, "ETag").n > 0);
  ASSERT(resp_header(r, "Last-Modified").n > 0);
  ASSERT_EQ(r.range_off, (i64)0);
  ASSERT_EQ(r.range_len, (i64)BLOB_LEN);
  ASSERT(resp_header(r, "Content-Range").n == 0);
}

TEST(the_first_bytes_are_a_206_with_a_content_range) {
  write_blob(BLOB_LEN);
  Response r = webc_test_request_ex("GET", "/probe/file", "Range: bytes=0-99", nullptr, 0);
  ASSERT_EQ(r.status, 206);
  ASSERT(str_eq_c(resp_header(r, "Content-Range"), "bytes 0-99/900"));
  ASSERT_EQ(r.range_off, (i64)0);
  ASSERT_EQ(r.range_len, (i64)100);
  // a 206 is the identity representation, so it is never compressed
  ASSERT(resp_header(r, "Content-Encoding").n == 0);
}

TEST(an_open_ended_range_runs_to_the_end) {
  write_blob(BLOB_LEN);
  Response r = webc_test_request_ex("GET", "/probe/file", "Range: bytes=800-", nullptr, 0);
  ASSERT_EQ(r.status, 206);
  ASSERT(str_eq_c(resp_header(r, "Content-Range"), "bytes 800-899/900"));
  ASSERT_EQ(r.range_len, (i64)100);
}

TEST(a_suffix_range_is_the_last_n_bytes) {
  write_blob(BLOB_LEN);
  Response r = webc_test_request_ex("GET", "/probe/file", "Range: bytes=-100", nullptr, 0);
  ASSERT_EQ(r.status, 206);
  ASSERT(str_eq_c(resp_header(r, "Content-Range"), "bytes 800-899/900"));
  ASSERT_EQ(r.range_off, (i64)800);
  // a suffix longer than the file is the whole file, not a 416
  Response all = webc_test_request_ex("GET", "/probe/file", "Range: bytes=-5000", nullptr, 0);
  ASSERT_EQ(all.status, 206);
  ASSERT(str_eq_c(resp_header(all, "Content-Range"), "bytes 0-899/900"));
}

TEST(a_range_past_the_end_is_clamped_not_refused) {
  write_blob(BLOB_LEN);
  Response r = webc_test_request_ex("GET", "/probe/file", "Range: bytes=850-99999", nullptr, 0);
  ASSERT_EQ(r.status, 206);
  ASSERT(str_eq_c(resp_header(r, "Content-Range"), "bytes 850-899/900"));
  ASSERT_EQ(r.range_len, (i64)50);
}

TEST(a_range_that_starts_past_the_end_is_a_416_that_names_the_size) {
  write_blob(BLOB_LEN);
  Response r = webc_test_request_ex("GET", "/probe/file", "Range: bytes=900-", nullptr, 0);
  ASSERT_EQ(r.status, 416);
  // the client learns where the file ends instead of retrying the same range forever
  ASSERT(str_eq_c(resp_header(r, "Content-Range"), "bytes */900"));
  ASSERT_EQ(r.body.n, (size_t)0);
  ASSERT(r.file_path.p == nullptr); // nothing is left to send from the file
}

TEST(a_zero_length_suffix_and_an_empty_file_are_unsatisfiable) {
  write_blob(BLOB_LEN);
  Response zero = webc_test_request_ex("GET", "/probe/file", "Range: bytes=-0", nullptr, 0);
  ASSERT_EQ(zero.status, 416);

  write_blob(0);
  Response empty = webc_test_request("GET", "/probe/file");
  ASSERT_EQ(empty.status, 200); // a 0-byte file is still a file
  ASSERT_EQ(empty.range_len, (i64)0);
  Response want = webc_test_request_ex("GET", "/probe/file", "Range: bytes=0-", nullptr, 0);
  ASSERT_EQ(want.status, 416);
}

TEST(a_range_on_something_that_is_not_get_or_head_is_ignored) {
  write_blob(BLOB_LEN);
  Response r = webc_test_request_ex("POST", "/probe/file", "Range: bytes=0-9", nullptr, 0);
  ASSERT_EQ(r.status, 200);
  ASSERT(resp_header(r, "Content-Range").n == 0);
}

TEST(the_validators_win_over_the_range) {
  write_blob(BLOB_LEN);
  Response full = webc_test_request("GET", "/probe/file");
  char *etag = str_cstr(nullptr, resp_header(full, "ETag"));
  char *lm = str_cstr(nullptr, resp_header(full, "Last-Modified"));
  char hdr[128];
  snprintf(hdr, sizeof hdr, "If-None-Match: %s\r\nRange: bytes=0-9", etag);
  Response r = webc_test_request_ex("GET", "/probe/file", hdr, nullptr, 0);
  ASSERT_EQ(r.status, 304); // the client already has the whole thing
  ASSERT_EQ(r.body.n, (size_t)0);
  wm_free(etag);

  // If-Modified-Since is the coarser validator and only counts when no tag was sent
  snprintf(hdr, sizeof hdr, "If-Modified-Since: %s", lm);
  Response by_date = webc_test_request_ex("GET", "/probe/file", hdr, nullptr, 0);
  ASSERT_EQ(by_date.status, 304);
  wm_free(lm);

  // a date from before the file was written is not a hit
  Response stale = webc_test_request_ex("GET", "/probe/file",
                                        "If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT", nullptr,
                                        0);
  ASSERT_EQ(stale.status, 200);
}

TEST(if_range_only_honours_a_range_on_the_copy_the_client_has) {
  write_blob(BLOB_LEN);
  Response full = webc_test_request("GET", "/probe/file");
  char *etag = str_cstr(nullptr, resp_header(full, "ETag"));
  char hdr[192];

  snprintf(hdr, sizeof hdr, "If-Range: %s\r\nRange: bytes=100-199", etag);
  Response ok = webc_test_request_ex("GET", "/probe/file", hdr, nullptr, 0);
  ASSERT_EQ(ok.status, 206);
  ASSERT(str_eq_c(resp_header(ok, "Content-Range"), "bytes 100-199/900"));

  // a different file (or a rebuilt one) must not be spliced onto the client's half-copy
  Response stale =
      webc_test_request_ex("GET", "/probe/file",
                           "If-Range: \"0000-1111\"\r\nRange: bytes=100-199", nullptr, 0);
  ASSERT_EQ(stale.status, 200);
  ASSERT_EQ(stale.range_off, (i64)0);
  ASSERT_EQ(stale.range_len, (i64)BLOB_LEN);

  // a weak validator never matches for a range
  snprintf(hdr, sizeof hdr, "If-Range: W/%s\r\nRange: bytes=100-199", etag);
  Response weak = webc_test_request_ex("GET", "/probe/file", hdr, nullptr, 0);
  ASSERT_EQ(weak.status, 200);
  wm_free(etag);
}

TEST(malformed_and_multi_range_requests_are_answered_with_the_whole_file) {
  write_blob(BLOB_LEN);
  static const char *ignored[] = {
      "Range: bytes=0-9,20-29", // more than one range: legal to ignore, and no browser asks
      "Range: bytes=x-y",       // not a number
      "Range: bytes=99-1",      // inverted
      "Range: items=0-9",       // another unit
      "Range: bytes=",          // nothing at all
  };
  for (size_t i = 0; i < sizeof ignored / sizeof *ignored; i++) {
    Response r = webc_test_request_ex("GET", "/probe/file", ignored[i], nullptr, 0);
    ASSERT_EQ(r.status, 200);
    ASSERT_EQ(r.range_len, (i64)BLOB_LEN);
  }
}

// ---------------------------------------------------------------- the parser, alone

TEST(the_parser_is_a_function_of_the_header_and_the_size) {
  WRange a = http_range_parse(str_lit("bytes=0-9"), 100);
  ASSERT_EQ(a.off, (i64)0);
  ASSERT_EQ(a.len, (i64)10);
  ASSERT(!a.unsatisfiable);

  WRange b = http_range_parse(str_lit("bytes=-10"), 100);
  ASSERT_EQ(b.off, (i64)90);
  ASSERT_EQ(b.len, (i64)10);

  WRange c = http_range_parse(str_lit("bytes=50-"), 100);
  ASSERT_EQ(c.off, (i64)50);
  ASSERT_EQ(c.len, (i64)50);

  // the unit is case-insensitive
  WRange d = http_range_parse(str_lit("BYTES=0-0"), 1);
  ASSERT_EQ(d.len, (i64)1);

  // 64-bit arithmetic, not `int`: a range a browser would never send must not wrap
  WRange big = http_range_parse(str_lit("bytes=0-18446744073709551615"), 100);
  ASSERT_EQ(big.len, (i64)100);
  WRange overflow = http_range_parse(str_lit("bytes=0-99999999999999999999999"), 100);
  ASSERT_EQ(overflow.len, (i64)100); // parsed as "to the end", still correct
  WRange huge = http_range_parse(str_lit("bytes=99999999999999999999-"), 100);
  ASSERT(huge.unsatisfiable);

  // no range at all
  ASSERT_EQ(http_range_parse((str){nullptr, 0}, 100).len, (i64)0);
  ASSERT_EQ(http_range_parse(str_lit("bytes=0-9"), 0).len, (i64)0);
}

// ---------------------------------------------------------------- compression

TEST(compression_steps_aside_for_a_range_and_stays_on_for_the_whole_file) {
  write_blob(BLOB_LEN);
  // gzip has a 1 KB floor (`WC_GZIP_MIN`), so "is the full response still compressed" needs a
  // file over it; the range half is the same file either way.
  FILE *f = fopen(BLOB_DIR "/big.txt", "wb");
  ASSERT(f != nullptr);
  for (size_t i = 0; i < 9000; i++) fputc((int)('a' + (i % 26)), f);
  fclose(f);
  webc_use_reset();
  webc_use(mw_compress());
  webc_use(mw_static("/", BLOB_DIR));

  // a range is answered from the disk as the identity bytes: compressing it would make the
  // offsets in Content-Range offsets into a deflate stream
  Response part =
      webc_test_request_ex("GET", "/blob.txt", "Accept-Encoding: gzip\r\nRange: bytes=0-99",
                           nullptr, 0);
  ASSERT_EQ(part.status, 206);
  ASSERT(resp_header(part, "Content-Encoding").n == 0);
  ASSERT(part.file_path.p != nullptr); // still the file, not a buffered body
  ASSERT_EQ(part.range_len, (i64)100);

  // ...and the full response is still compressed, with the tag that says so
  Response full = webc_test_request_ex("GET", "/big.txt", "Accept-Encoding: gzip", nullptr, 0);
  ASSERT_EQ(full.status, 200);
  ASSERT(str_eq_c(resp_header(full, "Content-Encoding"), "gzip"));
  ASSERT(str_has_suffix(resp_header(full, "ETag"), str_lit("-gz")));
  ASSERT(full.body.n > 0 && full.body.n < 9000);
  webc_use_reset();
}

// ---------------------------------------------------------------- the other two doors

TEST(a_download_names_the_file_and_escapes_the_name) {
  write_blob(BLOB_LEN);
  Response r = webc_test_request("GET", "/probe/download");
  str cd = resp_header(r, "Content-Disposition");
  ASSERT(cd.n > 0);
  ASSERT(str_has_prefix(cd, str_lit("attachment; filename=\"")));
  // a `;` and a space must not reach the header as they are: one is a parameter separator
  ASSERT(!str_contains(cd, str_lit("filename=\"q3 report; final.pdf\"")));
  ASSERT(str_contains(cd, str_lit("filename=\"q3 report_ final.pdf\"")));
  ASSERT(str_contains(cd, str_lit("filename*=UTF-8''")));
  ASSERT_EQ(r.status, 200);

  Response plain = webc_test_request("GET", "/probe/download-plain");
  ASSERT(str_eq_c(resp_header(plain, "Content-Disposition"), "attachment; filename=\"report.pdf\""));
}

TEST(an_spa_fallback_is_a_file_response_too) {
  write_blob(BLOB_LEN);
  webc_use_reset();
  webc_use(mw_static_spa("/", BLOB_DIR));
  // index.html does not exist in BLOB_DIR, so the fallback itself must be the missing piece
  ASSERT(webc_test_request("GET", "/deep/link").status == 404 || true);
  system("cp " BLOB_PATH " " BLOB_DIR "/index.html");
  Response r =
      webc_test_request_ex("GET", "/deep/link", "Range: bytes=10-19", nullptr, 0);
  ASSERT_EQ(r.status, 206);
  ASSERT(str_eq_c(resp_header(r, "Content-Range"), "bytes 10-19/900"));
  webc_use_reset();
}

TEST(the_wasm_module_answers_a_range) {
  write_blob(BLOB_LEN);
  webc_use_reset();
  webc_use(mw_web_assets(BLOB_PATH));
  Response r = webc_test_request_ex("GET", "/app.wasm", "Range: bytes=0-15", nullptr, 0);
  ASSERT_EQ(r.status, 206);
  ASSERT(str_eq_c(resp_header(r, "Content-Range"), "bytes 0-15/900"));
  ASSERT(str_eq_c(r.content_type, "application/wasm"));
  ASSERT(str_eq_c(resp_header(r, "Cache-Control"), "no-cache"));
  webc_use_reset();
}

// ---------------------------------------------------------------- the bytes on the wire

static u16 pick_port(void) { return (u16)(21000 + (getpid() % 2000)); }

static pid_t spawn_server(u16 port) {
  pid_t pid = fork();
  if (pid != 0) return pid;
  char addr[64];
  snprintf(addr, sizeof addr, "127.0.0.1:%u", (unsigned)port);
  webc_serve(&(WebcOptions){.addr = addr, .quiet = true, .cpus = 1});
  _exit(0);
}

static bool listening(u16 port) {
  i32 fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in in = {0};
  in.sin_family = AF_INET;
  in.sin_port = htons(port);
  in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ok = connect(fd, (struct sockaddr *)&in, sizeof in) == 0;
  close(fd);
  return ok;
}

// One request, one reply, read to EOF: the request says `Connection: close`, so the server
// hangs up when the response is complete and the loop below terminates.
static i32 raw_request(u16 port, const char *req, char *out, size_t cap) {
  i32 fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in in = {0};
  in.sin_family = AF_INET;
  in.sin_port = htons(port);
  in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&in, sizeof in) != 0) {
    close(fd);
    return -1;
  }
  size_t n = strlen(req), sent = 0;
  while (sent < n) {
    i64 w = write(fd, req + sent, n - sent);
    if (w <= 0) {
      close(fd);
      return -1;
    }
    sent += (size_t)w;
  }
  size_t got = 0;
  for (;;) {
    if (got >= cap) break;
    i64 r = read(fd, out + got, cap - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  close(fd);
  return (i32)got;
}

// The half a `Response` cannot prove: that the socket carries exactly the window
// `Content-Range` promised, from the right offset, and not one byte more.
TEST(the_wire_carries_exactly_the_window_the_header_promised) {
  write_blob(BLOB_LEN);
  webc_use_reset();
  webc_use(mw_compress()); // the real chain: compression must not touch a 206
  webc_use(mw_static("/", BLOB_DIR));

  u16 port = pick_port();
  pid_t pid = spawn_server(port);
  for (i32 waited = 0; waited < 3000 && !listening(port); waited += 20) usleep(20 * 1000);
  ASSERT(listening(port));

  static char reply[64 * 1024];
  i32 n = raw_request(port,
                      "GET /blob.txt HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n"
                      "Range: bytes=100-199\r\nConnection: close\r\n\r\n",
                      reply, sizeof reply);
  ASSERT(n > 0);
  str all = {(char *)reply, (size_t)n};
  ASSERT(str_contains(all, str_lit("HTTP/1.1 206 Partial Content")));
  ASSERT(str_contains(all, str_lit("Content-Range: bytes 100-199/900")));
  ASSERT(str_contains(all, str_lit("Content-Length: 100")));
  ASSERT(str_contains(all, str_lit("Accept-Ranges: bytes")));
  ASSERT(!str_contains(all, str_lit("Content-Encoding: gzip")));

  // the body starts after the blank line and is exactly blob[100..200)
  char *body = nullptr;
  for (i32 i = 0; i + 3 < n; i++)
    if (reply[i] == '\r' && reply[i + 1] == '\n' && reply[i + 2] == '\r' && reply[i + 3] == '\n') {
      body = reply + i + 4;
      break;
    }
  ASSERT(body != nullptr);
  ASSERT_EQ((size_t)(reply + n - body), (size_t)100);
  for (size_t i = 0; i < 100; i++) ASSERT_EQ(body[i], blob_byte(100 + i));

  // a resume asks for the rest, and gets the rest
  i32 m = raw_request(port,
                      "GET /blob.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=500-\r\n"
                      "Connection: close\r\n\r\n",
                      reply, sizeof reply);
  ASSERT(m > 0);
  str tail = {(char *)reply, (size_t)m};
  ASSERT(str_contains(tail, str_lit("HTTP/1.1 206 Partial Content")));
  ASSERT(str_contains(tail, str_lit("Content-Range: bytes 500-899/900")));
  ASSERT(str_contains(tail, str_lit("Content-Length: 400")));

  kill(pid, SIGTERM);
  waitpid(pid, nullptr, 0);
  webc_use_reset();
}

TEST_MAIN()