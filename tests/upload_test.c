// upload: the client's filename is never trusted, the magic bytes outrank the Content-Type, and
// the saved name is one this module made up.
#include <webc/test.h>
#include <webc/upload.h>

static Request req_with(WMultipart *mp, size_t cap) {
  Request r = {0};
  r.multipart = mp;
  r.max_part_bytes = cap;
  return r;
}

static WPart file_part(const char *field, const char *fname, const char *type, str data) {
  WPart p = {0};
  p.name = str_lit(field);
  p.filename = str_lit(fname);
  p.content_type = str_lit(type);
  p.data = data;
  p.is_file = true;
  return p;
}

// The handler that only wants the file has no reason to parse the body into a model first —
// and if `body(T)` were the only way to get the multipart parsed, a good upload would answer
// "the form was not sent as a file upload". So the parse happens in `upload_take`, once, with
// the request's own limits.
static const char *MULTIPART_BODY =
    "--XyZ\r\n"
    "Content-Disposition: form-data; name=\"title\"\r\n\r\n"
    "a letter\r\n"
    "--XyZ\r\n"
    "Content-Disposition: form-data; name=\"cv\"; filename=\"cv.pdf\"\r\n"
    "Content-Type: application/pdf\r\n\r\n"
    "%PDF-1.7 body bytes\r\n"
    "--XyZ--\r\n";

POST("/probe/upload") {
  UploadPolicy pol = {.dir = "/tmp/webc-upload-test", .max_bytes = 1 << 20};
  Upload up = upload_take(req, "cv", &pol);
  if (!up.ok) return res_error(422, "bad_upload", up.why);
  return res_text(200, fmt("saved %s (%zu bytes)", up.name, up.bytes));
}

TEST(an_upload_without_a_body_call_first_still_works) {
  webc_use_reset();
  Response r = webc_test_request_ex(
      "POST", "/probe/upload",
      "Content-Type: multipart/form-data; boundary=XyZ\r\n", MULTIPART_BODY, strlen(MULTIPART_BODY));
  ASSERT_EQ(r.status, 200);
  ASSERT_TRUE(str_contains((str){r.body.p, r.body.n}, str_lit("saved ")));
  webc_use_reset();
}

TEST(a_body_that_is_not_an_upload_says_what_the_form_is_missing) {
  webc_use_reset();
  Response r = webc_test_request_ex("POST", "/probe/upload", "Content-Type: application/json\r\n",
                                    "{\"a\":1}", 7);
  ASSERT_EQ(r.status, 422);
  ASSERT_TRUE(str_contains((str){r.body.p, r.body.n}, str_lit("enctype")));
  webc_use_reset();
}

TEST(the_declared_type_is_checked_against_the_first_bytes) {
  str png = str_lit("\x89PNG\r\n\x1a\n0000");
  ASSERT_STR_EQ(upload_sniff(png), "image/png");
}

TEST(a_type_that_lies_is_refused) {
  // a shell script wearing a .png, and a JPEG that claims to be a PNG
  str script = str_lit("#!/bin/sh\nrm -rf /\n");
  ASSERT_TRUE(upload_bytes_agree(str_lit("image/png"), script)); // no magic: unproven, allowed
  str jpeg = str_lit("\xFF\xD8\xFF\xE0");
  ASSERT_FALSE(upload_bytes_agree(str_lit("image/png"), jpeg));
  ASSERT_TRUE(upload_bytes_agree(str_lit("image/jpeg"), jpeg));
}

TEST(a_wildcard_in_the_allow_list_means_the_whole_family) {
  ASSERT_TRUE(upload_type_matches(str_lit("image/png"), "image/*"));
  ASSERT_TRUE(upload_type_matches(str_lit("image/png; charset=binary"), "image/png"));
  ASSERT_FALSE(upload_type_matches(str_lit("image/svg+xml"), "image/png"));
  ASSERT_TRUE(upload_type_matches(str_lit("anything/at-all"), "*"));
}

TEST(the_saved_name_is_made_here_not_taken_from_the_client) {
  UploadPolicy pol = upload_policy(nullptr, 0);
  WPart p = file_part("cv", "../../.ssh/authorized_keys", "text/plain", str_lit("hello"));
  WMultipart mp = {.parts = &p, .n = 1, .cap = 1};
  Request r = req_with(&mp, 1024);
  Upload up = upload_take(&r, "cv", &pol);
  ASSERT_TRUE(up.ok);
  ASSERT_FALSE(str_contains(up.name, str_lit("..")));
  ASSERT_FALSE(str_contains(up.name, str_lit("/")));
  ASSERT_STR_EQ(up.type, "text/plain");
  ASSERT_EQ(up.bytes, 5);
}

TEST(a_file_that_is_too_big_is_refused_with_a_sentence) {
  UploadPolicy pol = upload_policy(nullptr, 4);
  WPart p = file_part("cv", "cv.txt", "text/plain", str_lit("hello"));
  WMultipart mp = {.parts = &p, .n = 1, .cap = 1};
  Request r = req_with(&mp, 1024);
  Upload up = upload_take(&r, "cv", &pol);
  ASSERT_FALSE(up.ok);
  ASSERT_STR_EQ(up.why, "the file is larger than this upload allows");
}

TEST(an_empty_file_and_a_missing_field_both_say_so) {
  UploadPolicy pol = upload_policy(nullptr, 0);
  WPart p = file_part("cv", "cv.txt", "text/plain", (str){0});
  WMultipart mp = {.parts = &p, .n = 1, .cap = 1};
  Request r = req_with(&mp, 1024);
  Upload up = upload_take(&r, "cv", &pol);
  ASSERT_FALSE(up.ok);
  ASSERT_STR_EQ(up.why, "the file is empty");
  up = upload_take(&r, "photo", &pol);
  ASSERT_FALSE(up.ok);
  ASSERT_STR_EQ(up.why, "the file did not arrive");
}

TEST(a_type_outside_the_allow_list_is_refused) {
  UploadPolicy pol = upload_policy(nullptr, 0);
  pol.allow = (const char *[]){"image/png"};
  pol.allow_n = 1;
  WPart p = file_part("cv", "cv.txt", "text/csv", str_lit("a,b\n"));
  WMultipart mp = {.parts = &p, .n = 1, .cap = 1};
  Request r = req_with(&mp, 1024);
  Upload up = upload_take(&r, "cv", &pol);
  ASSERT_FALSE(up.ok);
  ASSERT_STR_EQ(up.why, "that kind of file is not accepted here");
}

TEST(a_size_reads_like_a_person_says_it) {
  ASSERT_STR_EQ(bytes_text(512), "512 B");
  ASSERT_STR_EQ(bytes_text(1536), "1.5 KB");
  ASSERT_STR_EQ(bytes_text(3 * 1024 * 1024), "3.0 MB");
}

TEST_MAIN()
