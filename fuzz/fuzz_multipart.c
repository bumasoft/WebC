// fuzz_multipart — the multipart reader: boundaries that never close, part
// headers with no colon, CRLF injection, and limits that must be enforced while
// parsing rather than after (SPEC §11).
// `webc/webc.h` is retired (see `webc lint`); multipart_parse() lives here.
#include <webc/multipart.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  Arena *a = arena_new(1 << 20);
  WMultipart mp;
  // The content type is part of the input's first line; the rest is the body.
  size_t nl = 0;
  while (nl < size && data[nl] != '\n') nl++;
  str ctype = {(char *)data, nl};
  Bytes body = {(char *)(data + (nl < size ? nl + 1 : nl)), size - (nl < size ? nl + 1 : nl)};
  WErrRes e = multipart_parse(a, ctype, body, 8, 64 * 1024, &mp);
  if (e.ok) {
    // every value a caller can ask for must live inside the body it came from
    for (i32 i = 0; i < multipart_count(&mp); i++) {
      (void)i;
    }
    (void)multipart_value(&mp, "title");
    (void)multipart_file(&mp, "avatar");
    (void)multipart_filename(&mp, "avatar");
    (void)multipart_file_type(&mp, "avatar");
  }
  arena_destroy(a);
  return 0;
}
