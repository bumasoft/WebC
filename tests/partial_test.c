// partials: authored HTML as a file, included by name.
//
// The three claims: the bytes come back verbatim (they are markup()'s trust, from a file
// instead of a string literal); a missing file is a *loud* nothing (one warning, a null str,
// never a crash and never an empty tag pair); and a name is a name, not a path — the string
// came from app code, so `..` and `/etc/passwd` are refused rather than honoured.
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include <webc/test.h>

static str doc_with_head(void) {
  return str_lit("<!doctype html><html><head><title>t</title></head>"
                 "<body><div id=\"root\"></div></body></html>");
}

GET("/p/hello") {
  (void)req;
  head_html(partial("probe/hello"));
  return res_html(200, doc_with_head());
}

GET("/p/missing") {
  (void)req;
  head_html(partial("probe/never-written"));
  return res_html(200, doc_with_head());
}

GET("/p/whole") {
  (void)req;
  return res_html(200, partial("probe/maintenance"));
}

// ---------------------------------------------------------------- the bytes

TEST(a_partial_is_the_files_bytes_verbatim) {
  Response r = webc_test_request("GET", "/p/hello");
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_CONTAINS(r, "<link rel=\"probe\" href=\"/probe.css\">");
  ASSERT_STR_CONTAINS(r, "an &ampersand stays as it was written"); // no second escaping
  // and it landed inside <head>, before </head>, like every head_html
  str body = WC_TEST_VIEW(r);
  i64 head_end = str_find(body, str_lit("</head>"), 0);
  i64 at = str_find(body, str_lit("<link rel=\"probe\""), 0);
  ASSERT_TRUE(head_end > at && at >= 0);
}

TEST(a_partial_serves_a_whole_page) {
  Response r = webc_test_request("GET", "/p/whole");
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_CONTAINS(r, "<!doctype html>");
  ASSERT_STR_CONTAINS(r, "back at 09:00");
}

TEST(the_second_read_is_the_first_read) {
  str a = partial("probe/hello");
  str b = partial("probe/hello");
  ASSERT_TRUE(a.p == b.p); // cached: one file read for the process, not one per render
}

// ---------------------------------------------------------------- the failures

TEST(a_missing_partial_is_a_null_str_and_renders_nothing) {
  Response r = webc_test_request("GET", "/p/missing");
  ASSERT_EQ(r.status, 200);
  ASSERT_STR_CONTAINS(r, "<title>t</title>"); // the page is fine
  ASSERT_FALSE(str_contains(WC_TEST_VIEW(r), str_lit("probe/never-written")));
  ASSERT_NULL(partial("probe/never-written").p); // null, not ""
}

TEST(a_name_is_a_name_not_a_path) {
  ASSERT_NULL(partial("../webc.json").p);      // no climbing
  ASSERT_NULL(partial("a/../b").p);
  ASSERT_NULL(partial("/etc/passwd").p);       // no absolute paths
  ASSERT_NULL(partial("probe/hello.html").p);  // the extension is the loader's
  ASSERT_NULL(partial("").p);                  // and no empty name
}

// ---------------------------------------------------------------- the harness

// The files this suite reads, written under partials/ before the first test and removed after
// the last one — the same directory `webc check` will look in, so the test exercises the
// convention rather than a copy of it.
static void write_probe(const char *name, const char *body) {
  mkdir("partials", 0755);
  mkdir("partials/probe", 0755);
  char path[128];
  snprintf(path, sizeof path, "partials/%s.html", name);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fwrite(body, 1, strlen(body), f);
  fclose(f);
}

static void probes_write(void) {
  write_probe("probe/hello",
              "<link rel=\"probe\" href=\"/probe.css\">\n"
              "<!-- an &ampersand stays as it was written -->\n");
  write_probe("probe/maintenance",
              "<!doctype html><html><head><title>maintenance</title></head>"
              "<body><h1>Down briefly</h1><p>back at 09:00</p></body></html>");
}

static void rm_rf(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[256];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(p);
    else remove(p);
  }
  closedir(d);
  rmdir(dir);
}

static void rm_probes(void);
TEST_SETUP(probes_write);
TEST_TEARDOWN(rm_probes);

static void rm_probes(void) { rm_rf("partials"); }

TEST_MAIN();