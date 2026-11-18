// SPDX-License-Identifier: Apache-2.0
// webc/multipart.h — multipart/form-data, with the limits SPEC §11 asks for.
//
//   WMultipart mp;
//   WErrRes e = multipart_parse(req->arena, req_header(req, "content-type"),
//                               req->body, 32, 4 * 1024 * 1024, &mp);
//   if (!e.ok) return webc_body_error(e);
//   str title  = multipart_value(&mp, "title");
//   str upload = multipart_file(&mp, "avatar");    // "" when the field is absent
//   str fname  = multipart_filename(&mp, "avatar");
//
// A file upload is a request body that claims to be several bodies: the parser is
// the only thing that decides where one ends. Hence the limits, and hence they are
// arguments here rather than a global: a 200-byte JSON endpoint and a 10 MB upload
// endpoint should not share a number.
#ifndef WEBC_MULTIPART_H
#define WEBC_MULTIPART_H

#include <webc/core.h>

typedef struct WPart {
  str name;
  str filename;     // "" for a text field
  str content_type; // "" when the part did not declare one
  str data;         // borrowed from the request body: no copy
  bool is_file;
} WPart;

typedef struct WMultipart {
  WPart *parts;
  i32 n, cap;
} WMultipart;

// `max_parts` and `max_bytes` are enforced while parsing, not after: a body that
// declares a thousand parts must not be read into a thousand WPart structs first.
WErrRes multipart_parse(Arena *a, str content_type, Bytes body, i32 max_parts, size_t max_bytes,
                        WMultipart *out);
bool multipart_is(const char *content_type); // does this Content-Type say multipart?
str  multipart_value(const WMultipart *mp, const char *name);
str  multipart_file(const WMultipart *mp, const char *name);
str  multipart_filename(const WMultipart *mp, const char *name);
str  multipart_file_type(const WMultipart *mp, const char *name);
i32  multipart_count(const WMultipart *mp);

#endif // WEBC_MULTIPART_H